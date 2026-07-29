#include "ui/Editor.h"
#include "ui/Equip.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Lang.h"
#include "ui/LootBarter.h"
#include "ui/Sfx.h"
#include "game/GoldCoins.h"
#include "ui/Loadout.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// B-4: tetris grids (main + bag windows). Placement is a 1:1 port of the JS
// implementation in view/GridInventory/index.html (maskOf / placeItems:
// saved-spot pass with grid seniority, then first-fit; bag overflow falls
// back to main), layout ini format unchanged (PLAN_B §2-J).

namespace FUI::Grid
{
    namespace
    {
        constexpr const char* kLayoutPath = "Data/SKSE/Plugins/GridInventory_layout.ini";

        struct Mask
        {
            std::vector<std::vector<bool>> rows;
            int w = 1;
            int h = 1;
        };

        struct Item
        {
            std::string         key;
            RE::TESBoundObject* obj = nullptr;
            int                 count = 1;
            GridDef             def;
            Mask                mask;
            std::string         inBag;      // "" = main grid
            int                 col = -1;   // -1 = no saved position
            int                 row = -1;
            bool                fixed = false;
            bool                overflow = false;
            bool                fav = false;   // vanilla favorite flag (Q menu)
            std::uint8_t        glow = 0;   // bit1 enchanted, bit2 unique (DESC)
            int                 coinValue = -1;   // G2/G4: gold value of a coin tile (-1 = not a coin)
            // GI1: which engine sub-stack this tile shows.
            //   uid   = ExtraUniqueID, 0 when the list has none (or none at all)
            //   xlIdx = position in entry->extraLists, -1 = plain unit (no list)
            // uid is authoritative and survives a save; xlIdx is the fallback for
            // the many lists the engine never assigns a uniqueID to (tempered /
            // poisoned / renamed). NEVER cache the ExtraDataList* itself -- the
            // engine reallocates and frees them (freeing one under us was a real
            // CTD; see the note above extraOf in DrawItemTooltip).
            std::uint16_t       uid = 0;
            std::uint16_t       sig = 0;   // GI25: content signature (uid-less units)
            int                 xlIdx = -1;
        };

        struct LayoutEntry
        {
            int         col = 0;
            int         row = 0;
            std::string bag;
            int         count = 0;   // G4: tile's owned quantity (Mabinogi
                                     // split/merge). 0 = unspecified -> the
                                     // reconciler fills it like a fresh pickup
                                     // (legacy saves & never-split forms).
        };

        // one drawable grid: main ("") or an open bag (bag item key)
        struct View
        {
            std::string      bagKey;   // "" = main
            std::string      bagName;  // window title for bags
            int              cols = kCols;
            int              minRows = kMinRows;
            int              maxRows = 4096;
            std::vector<int> items;    // indices into g_items
            int              rows = kMinRows;
        };

        struct Held
        {
            std::string         key;
            RE::TESBoundObject* obj = nullptr;
            Mask                mask;
            int                 count = 1;
            bool                isBag = false;
            float               defScale = 1.0f;
            float               offX = 0.0f;   // grab offset px within the footprint
            float               offY = 0.0f;
            bool                justPicked = true;
            bool                preSplit = false;   // came from a shift+lclick split slider
            int                 coinValue = -1;     // G2/G4: carried coin's gold value
            bool                fromPartner = false;   // carried FROM the merchant/container
            int                 partnerValue = 0;      // its base value (buy pricing)
            // GI1: the sub-stack this carry came from. For a GRID pickup the uid
            // also lives in `key`; for a PARTNER carry the key is "##partner"
            // (not a real tile) so it has to be carried explicitly.
            // Appended LAST on purpose -- the six aggregate initialisers below
            // stay valid and default both.
            int                 xlIdx = -1;
            std::uint16_t       uid = 0;
            int                 hand = 0;   // 1 right, 2 left -- doll carries only
            // Lifted off the DOLL (still worn until the unequip lands) rather
            // than off the board. Only THAT carry may be excluded without
            // actually coming out of the board set -- a board carry is a board
            // unit and must be removed, or a spare of the same form stays
            // counted and the drop lands as a fresh arrival.
            bool                fromDoll = false;
            // GI19: two PLAIN units of the same form share uid 0 and xlIdx -1 --
            // they are indistinguishable by content, and that is the whole point
            // of them being plain. Only their cell ordinal tells them apart, so
            // the carry has to remember which one was lifted or picking up one
            // of two identical daggers looked like picking up both.
            int                 partnerOrd = 0;
            std::uint16_t       sig = 0;   // GI25: survives the queue delay
            // Displaced by a drop onto an OCCUPIED slot. The engine unequips this
            // unit as part of the very equip we just queued, so it stops being
            // worn exactly when that equip lands -- and after that point it must
            // NOT keep claiming a worn list, because the only one left belongs to
            // the unit that replaced it. Identity cannot tell the two apart (same
            // form, same signature, same hand), so the pending equip's lifetime is
            // what draws the line: while it exists the swap is still in flight.
            bool                swappedOut = false;
            // GI36: this carry left the board wearing a star. The exit sinks
            // cannot recompute it -- once the unit is mid-flight there is no
            // tile left to ask -- so it travels WITH the carry.
            bool                fav = false;
        };

        // drop candidate under the cursor this frame (set by DrawGridView)
        struct DropTarget
        {
            bool                     has = false;
            int                      view = -1;
            int                      col = 0;
            int                      row = 0;
            std::vector<int>         blockers;   // g_items indices
            bool                     valid = false;
        };

        bool g_overloaded = false;      // W2: hard board can't hold everything
        bool g_capacityDirty = true;    // recompute on next CapacityTick
        int  g_spaceUsed = 0;           // S2: cells occupied on the MAIN board

        bool g_pouchOpen = false;       // G2: coin-pouch withdraw window
        int  g_pouchSlider = 0;

        // B: gold paid by a barter purchase this frame. The spill pass adds the
        // coin tiles this payment dissolved back into the main-board sim so the
        // bought item can't claim the freed cells (it spills to a bag instead).
        int  g_paidGold = 0;

        // Phase 7: sold/stored units whose ENGINE removal is still queued on
        // the transfer Tick (mirror of the coins' pending-drop pattern). The
        // rebuild subtracts these immediately so the interim frame doesn't
        // re-seat the outgoing units as a fresh pickup. WHICH tile they leave
        // is not tracked here: NotePendingRemove decrements that tile's
        // remembered quantity in g_layout at confirm time (in-place removal).
        std::unordered_map<RE::FormID, int> g_pendingRemoveForm;
        // B3: pending entries must not outlive a FAILED engine removal — a
        // transfer that never lands would keep the form under-counted and the
        // tile invisible until the menu closed. Every entry carries a stamp;
        // Rebuild expires stale ones (safety net, not the normal path).
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point>
                                            g_pendingRemoveWhen;
        constexpr std::chrono::seconds      kPendingRemoveTTL{ 3 };

        // GI22: ...and WHICH pool they are leaving from. The form-level counter
        // above only says "one iron dagger is on its way out"; the walk then
        // dropped a unit from whichever pool it reached first (plain, always).
        // So storing the TEMPERED dagger deducted a PLAIN one instead: the plain
        // pool lost a slot it still needed, and the tempered unit -- whose own
        // slot NotePendingRemove had already erased -- came back as a fresh
        // arrival and first-fit into the front gap on its way out the door.
        std::map<std::string, int>          g_pendingRemovePool;

        // Units whose EQUIP is queued for the next Tick. Deliberately separate
        // from the removal counters above: an equipped item stays in the stock
        // at full count, so a removal entry for it can never resolve. This one
        // is cleared by Equip::ProcessPending the moment it has applied the
        // queue, with a TTL only as a safety net for a rejected equip.
        // B-model: units that are NOT ON THE BOARD, named by identity rather than
        // by a pool string. The board's input set is built with these already
        // removed -- the old scheme left them in and subtracted them later, in
        // several places with different timing, which is what produced ghost
        // tiles, spares jumping to the front and cumulative disappearances.
        struct OffBoardUnit
        {
            std::string   base;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            const char*   why = "?";   // diagnostics only
            // True for units that are MID-TRANSITION between the board and the
            // body, where "is it worn yet?" decides whether it still needs to be
            // removed from the set:
            //   held (from the doll) -- unequip queued, still worn for now
            //   equipping            -- equip queued, not worn yet
            // False for reasons that never involve the body -- reserved, removing,
            // trash -- because for those, consuming a worn entry would let the
            // removal be absorbed by an unrelated equipped copy.
            bool          mayBeWorn = false;
            // The engine has run the equip. Identity matching cannot decide this:
            // the worn list's signature does not always equal the one recorded at
            // request time, so "is a worn list matching me?" answered NO forever
            // and the entry kept subtracting a spare on every rebuild. The queue
            // itself knows when it applied, so let it say so.
            bool          applied = false;
            // Two units of ONE form worn at once (a dagger in each hand) have the
            // same uid AND the same signature -- content cannot tell them apart,
            // and the hand is the only thing that can. Without it, lifting the
            // left one excluded the RIGHT one and left a ghost on the board.
            // 0 = not worn / either, 1 = right, 2 = left.
            int           hand = 0;
            // The board cell this unit left FROM, if it left from one. A unit in
            // transit still owns that cell until the transition completes: the
            // pool must hold the slot open rather than re-pack the survivors into
            // the front slots, which empties WHOSE-EVER cell happens to sort last
            // instead of the one the player acted on. Empty for units that never
            // had a cell (lifted off the doll, held by a preset).
            std::string   srcKey;
            // Heading ONTO the body rather than already on it. Such a unit cannot
            // be backed by a worn list until its equip has actually run -- with a
            // copy in each hand a strict identity match still found the OTHER
            // hand's list, and the accounting wrote the unit off as already worn.
            // (Appended last so the aggregate initialisers above stay valid.)
            bool          arriving = false;
        };
        std::vector<OffBoardUnit>             g_pendingEquip;
        std::chrono::steady_clock::time_point g_pendingEquipWhen{};
        constexpr std::chrono::seconds      kPendingEquipTTL{ 2 };

        // GI1 tile-key grammar:  formKey [ '@' uid-hex ] [ '#' ordinal ]
        //
        //   "Skyrim.esm|0x012EB7"         plain stack, tile 0
        //   "Skyrim.esm|0x012EB7#2"       plain stack, tile 2   (interchangeable units)
        //   "Skyrim.esm|0x012EB7@A31F"    the ONE unit carrying ExtraUniqueID 0xA31F
        //   "Skyrim.esm|0x012EB7@A31F#1"  that list's 2nd unit (GetCount() > 1)
        //
        // Before GI1 a gear tile was identified by its ORDINAL, which the
        // collection loop assigns in inventory-walk order — so equipping one of
        // three identical swords shifted every later tile onto its neighbour's
        // saved spot (and onto its neighbour's per-instance data). The uid binds
        // a tile to the engine's own sub-stack instead.
        //
        // BaseKey strips BOTH suffixes: every one of its ~16 call sites compares
        // the result against a FormKey, so "the form this tile shows" is exactly
        // what they all want.
        // Suffixes are only ever appended AFTER the "|0xID" part, so scan from
        // there: a PLUGIN FILENAME may legitimately contain '#' or '@', and
        // scanning from the start would cut the key inside the mod's name.
        // (The pre-GI1 code used rfind('#'), which was accidentally safe for
        // '#' and would not have been for '@'.)
        std::size_t KeySuffixPos(const std::string& a_key)
        {
            const auto bar = a_key.find('|');
            return a_key.find_first_of("@~#", bar == std::string::npos ? 0 : bar + 1);
        }

        // GI14: a CONTENT signature for a sub-stack the engine never gave a
        // uniqueID -- which is most of them (tempering, poison, charge, a
        // rename all create a list, none of them a uid).
        //
        // Without it those units fell back to the shared ordinal pool, so
        // storing a tempered dagger next to a plain one RENUMBERED both: the
        // tempered list is enumerated first, took ordinal 0, and inherited the
        // plain dagger's remembered cell. The two visibly swapped places.
        //
        // Two units with identical contents hash the same, and that is correct:
        // they ARE interchangeable. Retempering changes the hash, so the tile
        // moves -- the item genuinely changed state.
        std::uint16_t InstanceSig(const RE::ExtraDataList* a_xl)
        {
            if (!a_xl) return 0;
            std::uint32_t h = 2166136261u;                    // FNV-1a
            auto mix = [&h](std::uint32_t v) {
                for (int i = 0; i < 4; ++i) {
                    h ^= (v >> (i * 8)) & 0xFF;
                    h *= 16777619u;
                }
            };
            auto* xl = const_cast<RE::ExtraDataList*>(a_xl);
            // Did this list actually carry anything that distinguishes the unit?
            // A list can exist for reasons that say NOTHING about the item --
            // ExtraWorn on an equipped plain sword, a favourite hotkey.
            // Hashing none of the seven below still produced a
            // non-zero constant (0x1CD9), which invented a pool that no longer
            // existed once the list was dropped: lifting an equipped plain
            // weapon off the doll keyed the carry to "base~1CD9", the unequipped
            // unit came back as a PLAIN unit, the two no longer matched, and the
            // grid drew a tile for the item it was already carrying.
            bool mixed = false;
            // OWNERSHIP is part of the identity. A stolen dagger and a clean one
            // are not interchangeable -- an ordinary merchant refuses the first
            // and buys the second -- so they must not share a tile. Leaving it
            // out meant stolen-ness could only be tracked per FORM, and putting
            // one dagger into an owned chest and taking it back branded every
            // dagger the player owned, tempered ones included.
            if (auto* owner = xl->GetOwner()) {
                // ...but ONLY foreign ownership. An item stamped as the player's
                // own is not distinguishable from a listless copy in any way that
                // matters, and hashing it would split ordinary tiles for nothing.
                auto* pc = RE::PlayerCharacter::GetSingleton();
                if (!pc || owner != pc->GetActorBase()) {
                    mixed = true;
                    mix(0x4F574E52u);
                    mix(owner->GetFormID());
                }
            }
            // A quest unit cannot be dropped, sold or stored while an ordinary
            // copy of the same form can be. Same argument as ownership: not
            // interchangeable, therefore not the same pool. Without this the
            // quest lock could only be tracked per FORM, and one quest-flagged
            // potion locked every potion of that kind in the pack.
            if (xl->HasQuestObjectAlias()) {
                mixed = true;
                mix(0x51554553u);
            }
            if (const auto* x = xl->GetByType<RE::ExtraHealth>()) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &x->health, sizeof(bits));
                mixed = true;
                mix(0x48454C54u); mix(bits);
            }
            if (const auto* x = xl->GetByType<RE::ExtraEnchantment>()) {
                mixed = true;
                mix(0x454E4348u);
                mix(x->enchantment ? x->enchantment->GetFormID() : 0u);
                mix(x->charge);
            }
            if (const auto* x = xl->GetByType<RE::ExtraCharge>()) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &x->charge, sizeof(bits));
                mixed = true;
                mix(0x43485247u); mix(bits);
            }
            if (const auto* x = xl->GetByType<RE::ExtraPoison>()) {
                mixed = true;
                mix(0x50534E4Eu);
                mix(x->poison ? x->poison->GetFormID() : 0u);
                mix(x->count);
            }
            if (const auto* x = xl->GetByType<RE::ExtraSoul>()) {
                mixed = true;
                mix(0x534F554Cu);
                mix(static_cast<std::uint32_t>(x->GetContainedSoul()));
            }
            // ExtraTextDisplayData is deliberately NOT hashed.
            //
            // The engine splits a unit out of its stack when it is equipped and
            // copies ExtraHealth to the new list but NOT the display name, so the
            // SAME dagger answered one signature in the pack and another on the
            // body. Every match across that boundary then failed: lifting off the
            // doll left a duplicate on the board, and equipping subtracted a
            // spare because "is it worn yet?" could never say yes.
            //
            // A name the engine can drop underneath us is not identity. Renamed
            // items therefore share a pool with their unrenamed twins -- the
            // tooltip still shows the real name, since that reads the unit's own
            // list rather than the signature.
            // Nothing distinguishing -> this unit belongs to the PLAIN pool,
            // exactly like a unit with no list at all. Both must answer 0.
            if (!mixed) return 0;
            const std::uint16_t sig = static_cast<std::uint16_t>((h ^ (h >> 16)) & 0xFFFF);
            return sig == 0 ? 1 : sig;   // 0 is reserved for "no signature"
        }

        std::string BaseKey(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            return cut == std::string::npos ? a_key : a_key.substr(0, cut);
        }

        // The engine applies an equip a frame or two after we ask; IsWorn() turning
        // true is the only signal actually in step with it.
        void ReleasePendingEquip(const std::string& a_baseKey)
        {
            std::erase_if(g_pendingEquip,
                [&](const OffBoardUnit& u) { return u.base == a_baseKey; });
        }

        // Per-UNIT release: a queued equip is done when a WORN list matching that
        // unit's identity exists. Defined after g_held (it has to ask what the
        // cursor is holding); declared here because the pool walker calls it.
        void ReleaseWornPendingEquips(const std::string& a_baseKey,
                                      RE::InventoryEntryData* a_entry);

        // ---- GI20: pools ----------------------------------------------------
        //
        // A POOL is a set of units that are interchangeable WITH EACH OTHER:
        //
        //   "form@A31F"  exactly one unit (the engine gave it a uniqueID)
        //   "form~7C2E"  every unit whose extras hash the same (both +10% ones)
        //   "form"       every unit with no extras at all
        //
        // Inside a pool "which one" is a question with no answer -- and asking it
        // was the mistake. What matters is only that the pool has as many slots
        // as units, and that removing a unit frees the slot the player acted on.
        //
        // So: a pool's units are assigned to that pool's remembered slots IN
        // POSITION ORDER, exactly like the stackable branch has always done.
        // Nothing is ever renumbered, so nothing can jump.
        std::string PoolOfKey(const std::string& a_key)
        {
            const auto bar = a_key.find('|');
            const auto h = a_key.find('#', bar == std::string::npos ? 0 : bar + 1);
            return h == std::string::npos ? a_key : a_key.substr(0, h);
        }

        std::string PoolPrefix(const std::string& a_base, std::uint16_t a_uid,
                               std::uint16_t a_sig)
        {
            char buf[8];
            if (a_uid != 0) { std::snprintf(buf, sizeof(buf), "@%04X", a_uid); return a_base + buf; }
            if (a_sig != 0) { std::snprintf(buf, sizeof(buf), "~%04X", a_sig); return a_base + buf; }
            return a_base;
        }

        // The signature encoded in a key ('~XXXX'), 0 when there is none.
        std::uint16_t SigOf(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            if (cut == std::string::npos || a_key[cut] != '~') return 0;
            return static_cast<std::uint16_t>(
                std::strtoul(a_key.c_str() + cut + 1, nullptr, 16));
        }

        // Is this key bound to a specific sub-stack rather than to a position
        // in a sequence? BOTH marks count.
        //
        // GI14 added '~sig' but left the old `UidOf(key) != 0` test in the two
        // places that decide "ordinal tile or not" -- and UidOf returns 0 for a
        // '~sig' key. So a tempered dagger was swept into the dense ORDINAL
        // re-key: storing the plain dagger next to it renamed "form~7C2E" to
        // "form" and handed it the plain tile's slot. The wrong tile emptied.
        bool IsInstanceKey(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            return cut != std::string::npos && (a_key[cut] == '@' || a_key[cut] == '~');
        }

        // 0 = no instance (a unit of the plain stack).
        std::uint16_t UidOf(const std::string& a_key)
        {
            const auto cut = KeySuffixPos(a_key);
            if (cut == std::string::npos || a_key[cut] != '@') return 0;
            return static_cast<std::uint16_t>(
                std::strtoul(a_key.c_str() + cut + 1, nullptr, 16));
        }

        // '@' = engine uniqueID (authoritative, survives a save)
        // '~' = content signature (GI14 fallback for the uid-less majority)
        std::string TileKey(const std::string& a_base, std::uint16_t a_uid,
                            std::uint16_t a_sig, int a_ord)
        {
            std::string k = a_base;
            char buf[8];
            if (a_uid != 0) {
                std::snprintf(buf, sizeof(buf), "@%04X", a_uid);
                k += buf;
            } else if (a_sig != 0) {
                std::snprintf(buf, sizeof(buf), "~%04X", a_sig);
                k += buf;
            }
            if (a_ord > 0) k += "#" + std::to_string(a_ord);
            return k;
        }

        // A carry's uid lives in its tile key for grid pickups and in the
        // explicit field for partner carries (whose key is "##partner").
        struct Held;
        [[nodiscard]] std::uint16_t HeldUidOf(const std::string& a_key, std::uint16_t a_field)
        {
            const auto k = UidOf(a_key);
            return k != 0 ? k : a_field;
        }

        // GI1: resolve the ExtraDataList a TILE stands for -- never "the first
        // one", which is what GlowBits / extraOf / ToggleFavorite all did (a
        // stack of three swords with one enchanted showed three glowing tiles,
        // leaked the enchanted one's tooltip onto the plain ones, and put the
        // favourite star on the wrong sword).
        //
        // uid wins when the engine assigned one. Otherwise fall back to the
        // position recorded at collection time, revalidated against the CURRENT
        // list -- entry->extraLists is a linked list the engine mutates, so the
        // index is a hint, never a promise. Returns nullptr for a plain unit,
        // which is the correct answer: it genuinely has no list.
        RE::ExtraDataList* ExtraForTile(RE::InventoryEntryData* a_entry,
                                        std::uint16_t a_uid, int a_xlIdx)
        {
            if (!a_entry || !a_entry->extraLists) return nullptr;
            if (a_uid != 0) {
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) continue;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                        xu && xu->uniqueID == a_uid) {
                        return xl;
                    }
                }
                return nullptr;   // that instance left the inventory
            }
            if (a_xlIdx < 0) return nullptr;   // plain unit
            int i = 0;
            for (auto* xl : *a_entry->extraLists) {
                if (i++ == a_xlIdx) return xl;
            }
            return nullptr;
        }

        // GI40: is ANY unit of this pool favourited?
        //
        // Worn lists count. Equipping splits a unit off and the engine carries
        // the ExtraHotkey over to the worn list, so excluding it would make the
        // spares left in the bag go dark the instant one was put on -- which is
        // exactly the bug this replaced. A worn plain dagger hashes to the same
        // signature as its spares (ExtraWorn is not part of InstanceSig), so it
        // lands in the same pool, which is the point.
        bool PoolHasStar(RE::InventoryEntryData* a_entry,
                         std::uint16_t a_uid, std::uint16_t a_sig)
        {
            if (!a_entry || !a_entry->extraLists) return false;
            for (auto* xl : *a_entry->extraLists) {
                if (!xl || !xl->HasType<RE::ExtraHotkey>()) continue;
                if (a_uid != 0) {
                    const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                    if (xu && xu->uniqueID == a_uid) return true;
                    continue;
                }
                if (InstanceSig(xl) == a_sig) return true;
            }
            return false;
        }

        // GI25: resolve by POOL -- uid first, then content signature. Unlike
        // ExtraForTile this never falls back to a list POSITION, so it stays
        // correct across the frames between queueing a transfer and the engine
        // actually running it.
        // a_allowWorn: ONLY for a non-player source. A corpse's or a mark's worn
        // gear is shown on the partner board on purpose, so a take has to be able
        // to name it -- excluding it there returned nullptr, the engine picked for
        // itself, and looting an NPC's equipped sword could move a spare from the
        // same inventory instead. Never true for the player's own side (see below).
        RE::ExtraDataList* ExtraForPoolImpl(RE::InventoryEntryData* a_entry,
                                            std::uint16_t a_uid, std::uint16_t a_sig,
                                            bool a_allowWorn = false)
        {
            if (!a_entry || !a_entry->extraLists) return nullptr;
            // The WORN list must never be a candidate here. Tile enumeration
            // already skips it, so every unit this resolver is asked about is a
            // SPARE -- but the resolver matched on signature alone, and an
            // equipped plain item (a list holding only ExtraWorn) hashes to the
            // same value as any other list carrying none of the six extras
            // InstanceSig looks at (a favourited or stolen spare). Selling or
            // trashing the spare could then hand the engine the equipped list
            // instead: the item in the player's hand gets sold, or destroyed.
            // Everything that genuinely wants the worn list goes through
            // WornExtraOf() instead, so excluding it here costs nothing.
            const auto worn = [a_allowWorn](const RE::ExtraDataList* a_xl) {
                if (a_allowWorn) return false;
                auto* xl = const_cast<RE::ExtraDataList*>(a_xl);
                return xl->HasType<RE::ExtraWorn>() || xl->HasType<RE::ExtraWornLeft>();
            };
            if (a_uid != 0) {
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl || worn(xl)) continue;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                        xu && xu->uniqueID == a_uid) return xl;
                }
                return nullptr;
            }
            // GI39: a PLAIN unit (sig 0) can still live in a list. The engine
            // groups identical units into ONE ExtraDataList carrying a count, so
            // "no signature" never meant "no list" -- this used to bail out at
            // sig 0 and hand RemoveItem a nullptr. The engine then picked for
            // itself, and with a tempered spare of the same form in the bag it
            // could walk THAT out: the plain dagger you clicked stayed put and
            // the tempered one left. Worn lists are excluded just above, which
            // is the only reason sig 0 was unsafe to match on before.
            for (auto* xl : *a_entry->extraLists) {
                if (!xl || worn(xl) || InstanceSig(xl) != a_sig) continue;
                // GI42: a uid unit is the sole member of its own pool ("@uid"),
                // so it can never be the answer to a sig- or plain-pool request.
                // The star-clearing loop in ResolveExitUnit already had this
                // filter -- same concept, one implementation now.
                if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
                    xu && xu->uniqueID != 0) continue;
                return xl;
            }
            return nullptr;   // genuinely listless (or every candidate is worn)
        }

        // Same, starting from the player's live InventoryChanges (the engine's
        // OWN entry -- GetInventory hands out copies, so mutations there are
        // silently discarded).
        RE::InventoryEntryData* LiveEntry(RE::TESObjectREFR* a_owner,
                                          RE::TESBoundObject* a_obj)
        {
            if (!a_owner || !a_obj) return nullptr;
            auto* changes = a_owner->GetInventoryChanges();
            if (!changes || !changes->entryList) return nullptr;
            for (auto* e : *changes->entryList) {
                if (e && e->object == a_obj) return e;
            }
            return nullptr;
        }

        // GI43: a TILE's engine-true value (temper folded in like vanilla).
        // GI44: resolved by POOL (uid, sig), never by recorded position -- the
        // tile was collected at rebuild time and list positions drift, and the
        // sale itself already resolves by pool, so the price must match it.
        int TileValue(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig)
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            return UnitValueWith(a_obj,
                ExtraForPoolImpl(LiveEntry(p, a_obj), a_uid, a_sig));
        }


        // forward decl: g_layout defined below; NextTileKey returns the lowest
        // unused "#k" ordinal for a form (k==0 is the bare baseKey). Used when a
        // split fragment lands on an empty cell and needs its own persistent tile.
        std::map<std::string, LayoutEntry>& Layout();
        std::string PlacePin(int a_value, int a_col, int a_row, const std::string& a_bag);  // G4 (defined later)
        std::string NextTileKey(const std::string& a_baseKey)
        {
            auto& layout = Layout();
            // a key is taken if it's placed OR reserved by a pin not yet placed
            auto taken = [&](const std::string& k) {
                return layout.contains(k) || GoldCoins::PinnedValue(k) >= 0;
            };
            if (!taken(a_baseKey)) return a_baseKey;
            for (int k = 1;; ++k) {
                const std::string cand = a_baseKey + "#" + std::to_string(k);
                if (!taken(cand)) return cand;
            }
        }

        // "unique" = has a DESC description (artifacts). The lookup walks the
        // string tables, so cache the verdict per form for the session.
        bool HasDescCached(RE::TESBoundObject* a_obj)
        {
            static std::unordered_map<RE::FormID, bool> s_cache;
            const auto [itc, fresh] = s_cache.try_emplace(a_obj->GetFormID(), false);
            if (fresh) {
                if (auto* d = a_obj->As<RE::TESDescription>()) {
                    RE::BSString out;
                    d->GetDescription(out, a_obj->As<RE::TESForm>());
                    itc->second = out.size() > 0 && out.c_str() && *out.c_str();
                }
            }
            return itc->second;
        }

        // G3: Mabinogi stacking — units per TILE. 1 = never stacks (equipment
        // incl. rings, weapons, gold coins); consumables/materials stack up to
        // a per-category cap and spill into extra tiles beyond it.
        // F-key favorite toggle. GetInventory returns entry COPIES —
        // SetFavorite must mutate the engine's OWN entry, so walk
        // InventoryChanges::entryList for the real one.
        // D3: this took extraLists->front(), and AddExtraList pushes FRONT --
        // so front() is "whatever was given a personality most recently".
        // Pressing F on a plain sword put the star on the enchanted one sitting
        // next to it. The tile now names its own sub-stack; nullptr means a
        // plain unit, which is exactly what the no-lists case always passed.
        // Defined after g_layout / FormKey (it edits our own per-tile flag).
        void ToggleFavorite(const std::string& a_key, RE::TESBoundObject* a_obj,
                            std::uint16_t a_uid, int a_xlIdx);

        int StackCapOf(RE::TESBoundObject* a_obj)
        {
            const RE::FormID fid = a_obj->GetFormID();
            if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) return 1;
            if (a_obj->Is(RE::FormType::Armor) ||
                a_obj->Is(RE::FormType::Weapon)) return 1;
            if (a_obj->Is(RE::FormType::Ammo)) return 100;          // arrows/bolts
            if (a_obj->Is(RE::FormType::AlchemyItem)) return 10;    // potions/food
            if (a_obj->Is(RE::FormType::Ingredient)) return 10;
            if (a_obj->Is(RE::FormType::Scroll)) return 10;
            if (a_obj->Is(RE::FormType::SoulGem)) return 10;
            if (a_obj->Is(RE::FormType::Book)) return 10;
            return 20;   // misc: ores, ingots, leather, gems, keys...
        }

        int MaskCells(const std::vector<std::vector<bool>>& a_rows)
        {
            int n = 0;
            for (const auto& r : a_rows) {
                for (bool c : r) {
                    if (c) ++n;
                }
            }
            return n;
        }

        DefResolver                                    g_resolver;

        // ONE tile holds at most this many units (Phase 2: the former 8-site
        // `(baseCap>1 && stack>0) ? stack : baseCap` copies converge here):
        // gear/coins = 1, else the editor per-item override (stack:N) if any,
        // else the category cap.
        int EffectiveCap(RE::TESBoundObject* a_obj, const GridDef& a_def)
        {
            const int baseCap = StackCapOf(a_obj);
            return (std::max)(1, (baseCap > 1 && a_def.stack > 0) ? a_def.stack : baseCap);
        }
        int EffectiveCap(RE::TESBoundObject* a_obj)
        {
            return EffectiveCap(a_obj, g_resolver ? g_resolver(a_obj) : GridDef{});
        }

        std::vector<Item>                              g_items;
        std::vector<View>                              g_views;    // [0] = main
        std::map<std::string, LayoutEntry>             g_layout;
        std::map<std::string, LayoutEntry>& Layout() { return g_layout; }
        std::set<std::string>                          g_openBags; // remembered (E2)
        std::unordered_set<std::string>                g_prevKeys;
        bool                                           g_poolTrace = false;   // debug switch: [POOL]/[FAV]/[XL] diagnostics


        // GI32: favourite syncs waiting for the game thread (see ToggleFavorite)
        struct FavSync
        {
            RE::TESBoundObject* obj = nullptr;
            std::uint16_t       uid = 0;
            int                 xlIdx = -1;
        };
        std::vector<FavSync> g_favSync;
        // TEMPORARY: last drawn set per gear pool, so a rebuild that changes what
        // is on the board can say so. A flicker is a change that comes back --
        // invisible to the conservation check, which only sees one frame at a
        // time, but obvious as "2 -> 1 -> 2" in a transition log.
        std::unordered_map<std::string, std::string>   g_flickPrev;
        std::unordered_map<RE::FormID, int>            g_values;   // Phase 4: form -> GetValue (barter)
        // Phase 6 / GI26: NOT owned by the player, keyed by POOL (form + uid/sig).
        // Form-keyed, one stolen dagger made every dagger in the pack read as
        // stolen -- dimmed in barter, marked with the crimson dot, refused by
        // ordinary merchants. Ownership is in the signature now, so a stolen unit
        // has a pool of its own to be flagged in.
        std::unordered_map<std::string, bool>          g_stolen;
        // Phase 7 / GI26: quest object, keyed by POOL like g_stolen. Form-keyed,
        // a single quest-flagged copy locked every copy of that form -- the
        // player could not drop, sell, store or trash any of them.
        std::unordered_map<std::string, bool>          g_questItem;
        int                                            g_gold = 0;
        std::optional<Held>                            g_held;
        DropTarget                                     g_target;
        // One-shot: the STACK TILE a unit was just taken from. Stackables have no
        // per-unit identity, so rule 2-B decides it -- the cell the player acted
        // on is the cell that gives one up. Without this the drain took from
        // whichever tile sorted last, so equipping the single torch you had just
        // set down in front emptied the stack behind it instead.
        struct DrainHint { std::string baseKey; std::string key; };
        DrainHint g_drainHint;

        // B2: one-shot placement hint for the next ACQUIRE that creates a new
        // tile of this form (partner-drop lands at the drop cell without a
        // premature layout entry). col<0 = no hint.
        struct DropHint { std::string baseKey; int col = -1; int row = -1; std::string bag; };
        DropHint                                       g_dropHint;
        std::string                                    g_slotTarget;   // hovered equip slot (C6)
        bool                                           g_needRebuild = false;

        // ---- F2: trash window (parked-for-deletion buffer) ----
        // Parked tiles are ordinary layout entries with bag == kTrashKey; the
        // engine inventory is untouched until deletion is CONFIRMED (window /
        // menu close, or FIFO eviction when the 6x4 board needs room).
        constexpr const char* kTrashKey  = "__trash";
        constexpr int         kTrashCols = 6;
        constexpr int         kTrashRows = 4;
        bool                                     g_trashOpen = false;
        std::map<std::string, LayoutEntry>       g_trashReturn;   // key -> pre-park spot
        std::deque<std::string>                  g_trashOrder;    // FIFO, oldest first
        // GI25: a queued deletion names its POOL. Form + count alone let the
        // engine bin whichever copy it liked -- so emptying the trash could
        // destroy the tempered sword instead of the plain one parked there.
        struct TrashDelete
        {
            RE::FormID    form = 0;
            int           count = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            bool          fav = false;   // GI36: carried to the deletion sink
        };
        std::vector<TrashDelete>                g_trashDeleteQ;  // engine removals (Tick)
        struct TrashAsk   // favorite-intake confirm popup
        {
            bool                active = false;
            RE::TESBoundObject* obj = nullptr;
            std::string         key;
            int                 count = 0;
            int                 col = -1;
            int                 row = -1;
        };
        TrashAsk                                 g_trashAsk;

        // units of this form parked in the trash (they occupy no board space)
        int TrashedUnits(const std::string& a_baseKey)
        {
            int n = 0;
            for (const auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey && BaseKey(k) == a_baseKey) {
                    n += (std::max)(1, le.count);
                }
            }
            return n;
        }
        std::function<void(RE::TESBoundObject*, bool)> g_sound;
std::function<void(RE::TESBoundObject*, int, RE::ExtraDataList*)> g_dropWorld;

        // Stable item key across load orders: "Plugin.esp|0xLocalID"
        //
        // A runtime-created form (player-brewed potion, player-enchanted weapon)
        // has NO source file, and GetLocalFormID() dereferences that file without
        // checking it -- so it must never be called for one. Such a form has no
        // local id anyway; its whole FormID is the identity.
        std::string FormKey(RE::TESForm* a_form)
        {
            auto* file = a_form->GetFile(0);
            std::string key = file ? std::string(file->GetFilename()) : "Dynamic";
            char buf[16];
            std::snprintf(buf, sizeof(buf), "|0x%06X",
                file ? a_form->GetLocalFormID() : a_form->GetFormID());
            return key + buf;
        }

        // P1: EVERY unit that is not on the board, in one list, named by identity.
        //
        // These used to be six separate corrections: worn was dropped by the walker
        // while the caller ALSO subtracted it from a scalar count, and reservations,
        // queued removals and trash parking were subtracted from that same scalar.
        // The walk then reconstructed "who to hide" from the DIFFERENCE between the
        // two -- so a scalar subtraction and a set removal for the same unit
        // CANCELLED, and the unit reappeared. That is why two identical items broke
        // everything and one identical item looked fine.
        //
        // Worn units are still handled by the walker's own skipWorn (they are never
        // in the set to begin with); everything else is named here.
        std::vector<OffBoardUnit> OffBoardUnitsFor(RE::TESBoundObject* a_obj,
                                                   const std::string& a_base)
        {
            std::vector<OffBoardUnit> out;
            if (!a_obj) return out;

            // fromPartner: the cursor is holding the CONTAINER's item, not one of
            // ours. Subtracting it from the player's board hid a spare of the same
            // form for as long as the carry lasted -- take a dagger off a merchant
            // while you own three, and one of yours blinked out until you put it
            // down. The carry is the partner board's business, not this one's.
            if (g_held && g_held->obj && !g_held->fromPartner &&
                FormKey(g_held->obj) == a_base) {
                // A SWAPPED-OUT carry stops being worn the moment the equip that
                // displaced it lands -- and the worn list left behind belongs to
                // the unit that replaced it, which is identical in every respect.
                // Keeping mayBeWorn on made the carry match THAT list, so nothing
                // came out of the pack and the displaced dagger sat on the cursor
                // AND on the board. The in-flight equip is the only honest clock:
                // while it exists the swap has not completed.
                bool stillWorn = g_held->fromDoll;
                if (stillWorn && g_held->swappedOut) {
                    stillWorn = std::any_of(g_pendingEquip.begin(), g_pendingEquip.end(),
                        [&](const OffBoardUnit& u) {
                            return u.base == a_base &&
                                   (u.hand == 0 || g_held->hand == 0 ||
                                    u.hand == g_held->hand);
                        });
                }
                out.push_back({ a_base, g_held->uid, g_held->sig, "held", stillWorn,
                                false, g_held->hand, g_held->key });
            }
            for (const auto& u : g_pendingEquip) {                     // equip queued
                if (u.base == a_base) out.push_back(u);
            }
            // held back by an INACTIVE preset: the preset recorded which unit
            for (const std::uint16_t sg : Loadout::ReservedSigs(a_obj->GetFormID())) {
                out.push_back({ a_base, 0, sg, "reserved" });
            }
            // engine removal queued (sold / stored / dropped), per pool
            for (const auto& [pool, n] : g_pendingRemovePool) {
                if (BaseKey(pool) != a_base) continue;
                for (int i = 0; i < n; ++i) {
                    out.push_back({ a_base, UidOf(pool), SigOf(pool), "removing" });
                }
            }
            // parked in the trash: still owned, but occupies no board space
            for (const auto& [k, le] : g_layout) {
                if (le.bag != kTrashKey || BaseKey(k) != a_base) continue;
                for (int i = 0, n = (std::max)(1, le.count); i < n; ++i) {
                    out.push_back({ a_base, UidOf(k), SigOf(k), "trash" });
                }
            }
            return out;
        }

        // A queued equip is done when a worn list matching that unit exists --
        // but "matching" has to be strict about BOTH things identity is made of:
        //
        //   HAND   Equipping to the left while an identical item is worn on the
        //          right read as "already landed" the instant it was queued.
        //
        //   COUNT  In a same-form swap the DISPLACED occupant is still worn and
        //          is identical in every respect to the unit coming in. Its list
        //          proved the new equip had landed before the engine ran it, the
        //          entry vanished, and the incoming unit fell straight back onto
        //          the board -- so the player saw it on the cursor AND in the
        //          grid. The carried unit already owns one of those worn lists,
        //          so a release needs one MORE than it accounts for.
        //
        // Both only misfire when the two units share a pool, which is exactly why
        // this survived every test that used two DIFFERENT items.
        void ReleaseWornPendingEquips(const std::string& a_baseKey,
                                      RE::InventoryEntryData* a_entry)
        {
            if (g_pendingEquip.empty() || !a_entry || !a_entry->extraLists) return;
            std::erase_if(g_pendingEquip, [&](const OffBoardUnit& u) {
                if (u.base != a_baseKey) return false;
                int matching = 0;
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) continue;
                    const bool L = xl->HasType<RE::ExtraWornLeft>();
                    const bool R = xl->HasType<RE::ExtraWorn>();
                    if (!L && !R) continue;
                    if (u.hand == 1 && !R) continue;
                    if (u.hand == 2 && !L) continue;
                    std::uint16_t uid = 0;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
                    if (uid == u.uid && InstanceSig(xl) == u.sig) {
                        matching += (std::max)(1, xl->GetCount());
                    }
                }
                const bool carriedOwnsOne =
                    g_held && g_held->fromDoll && g_held->obj &&
                    FormKey(g_held->obj) == a_baseKey &&
                    g_held->uid == u.uid && g_held->sig == u.sig &&
                    (u.hand == 0 || g_held->hand == 0 || g_held->hand == u.hand);
                return matching > (carriedOwnsOne ? 1 : 0);
            });
        }


        // The pool a CARRIED unit belongs to. Its tile key is unreliable here --
        // a doll lift and a partner lift both use synthetic keys -- but the carry
        // always records the identity it was taken with.
        std::string HeldPoolKey(const Held& a_held)
        {
            return a_held.obj ? PoolPrefix(FormKey(a_held.obj), a_held.uid, a_held.sig)
                              : std::string{};
        }

        // GI33: favourites are VANILLA's. We only draw the star.
        //
        // Owning them per tile could not work: the engine keeps ONE
        // ExtraDataList per set of identical units, so "this dagger, not that
        // one" has nowhere to live. Every attempt to hold that intent ourselves
        // ended up fighting the engine over the Q menu. F now marks the tile's
        // own sub-stack and the star is read straight back out of it, so
        // identical units share the mark while tempered and plain stay apart.
        // (GI34 attaches the ExtraHotkey directly for that second part -- the
        // engine's own call is entry-scoped and refuses a second variant.)
        //
        // Queued for the game thread like every other inventory mutation here.
        void ToggleFavorite(const std::string& a_key, RE::TESBoundObject* a_obj,
                            std::uint16_t a_uid, int a_xlIdx)
        {
            if (!a_obj || a_key.empty()) return;
            g_favSync.push_back({ a_obj, a_uid, a_xlIdx });
        }

        // GI28: the cell an action was AIMED at, flashed as it empties.
        //
        // Every "the wrong cell went blank" bug this project has had was invisible
        // until someone stared at two frames of log. Marking the cell we intended
        // to vacate turns the whole class into something you SEE: the flash and
        // the gap are the same cell when it works, and different cells when it
        // does not. It is also just good feedback -- an action should say where
        // it landed.
        struct Vacated
        {
            int         col = 0, row = 0, w = 1, h = 1;
            std::string bag;
            float       born = 0.0f;
        };
        std::vector<Vacated>  g_vacated;

        constexpr float       kVacatedFade = 0.40f;   // seconds

        // Writes a tile's PLACEMENT and nothing else.
        //
        // Every drop path used to assign a whole LayoutEntry (`g_layout[k] = {col,
        // row, bag, count}`), which silently reset every OTHER field on the tile.
        // The favourite flag lives there now, so moving a starred tile wiped its
        // star -- and the reconcile then re-adopted the star onto whichever key of
        // that pool sorted first. That is why moving the middle tempered dagger
        // put the star on a different one, why the star then "followed" that tile
        // (it was already the first key, so re-adoption landed on itself), and why
        // a plain star vanished for good once a tempered one held the pool.
        // The tile gives up its CELL but survives (it is riding the cursor).
        // Erasing the entry instead threw its flags away with the position: a
        // swap wiped the displaced tile's star, and the reconcile then handed
        // the mark to whichever sibling sorted first -- "A를 B에 스왑했더니
        // 별이 C로 갔다". Use this wherever the item is not actually leaving.
        void ParkTile(const std::string& a_key)
        {
            const auto li = g_layout.find(a_key);
            if (li == g_layout.end()) return;
            li->second.col = -1;
            li->second.row = -1;
            li->second.bag.clear();
        }

        void PlaceTile(const std::string& a_key, int a_col, int a_row,
                       const std::string& a_bag, int a_count)
        {
            auto& le = g_layout[a_key];
            le.col = a_col;
            le.row = a_row;
            le.bag = a_bag;
            le.count = a_count;
        }

        bool g_layoutLoaded = false;   // capacity checks run with the menu closed

        // LEGACY MIGRATION ONLY: the ini is read ONCE per process into a frozen
        // snapshot; record-less (pre-cosave) saves migrate from that snapshot.
        // It is never written any more — the live per-change write leaked the
        // current session's unsaved arrangement into any old save loaded later
        // (the ini was global, not per-save).
        void LoadLayout()
        {
            static bool s_read = false;
            static std::map<std::string, LayoutEntry> s_snapLayout;
            static std::set<std::string> s_snapBags;

            g_layoutLoaded = true;
            if (s_read) {
                g_layout = s_snapLayout;
                g_openBags = s_snapBags;
                return;
            }
            s_read = true;
            g_layout.clear();
            g_openBags.clear();
            std::ifstream in(kLayoutPath);
            if (!in) return;
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == ';' || line[0] == '[') continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                auto trim = [](std::string s) {
                    const auto b = s.find_first_not_of(" \t\r");
                    const auto e = s.find_last_not_of(" \t\r");
                    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
                };
                const std::string key = trim(line.substr(0, eq));
                std::string rest = trim(line.substr(eq + 1));
                if (key.empty()) continue;

                if (key == "!openbags") {   // remembered open-bag list (E2)
                    std::istringstream ss(rest);
                    std::string tok;
                    while (std::getline(ss, tok, ';')) {
                        if (!tok.empty()) g_openBags.insert(tok);
                    }
                    continue;
                }

                LayoutEntry le;
                std::replace(rest.begin(), rest.end(), ',', ' ');
                std::istringstream ss(rest);
                if (!(ss >> le.col >> le.row)) continue;
                ss >> le.bag;   // optional third token
                g_layout[key] = le;
            }
            s_snapLayout = g_layout;
            s_snapBags = g_openBags;
        }

        // ---- cosave (per-save layout; the global ini is legacy fallback) ----

        // v2: per-tile count (G4)
        // v3: GI1 tile keys may carry an "@uid" instance suffix. The BINARY
        //     format is unchanged -- keys were already length-prefixed strings,
        //     so v2 records load field-for-field and simply arrive with no
        //     instance keys (every unit reads as plain, uid 0). A v2 save's
        //     gear therefore keeps its saved cells; only units the engine had
        //     uid'd get re-seated once, on the first rebuild.
        constexpr std::uint32_t kCosaveVersion = 5;   // v5: v4's per-tile fav byte retired (GI33)
        constexpr std::uint32_t kMaxStr = 512;
        constexpr std::uint32_t kMaxEntries = 65536;

        bool WriteStr(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const auto len = static_cast<std::uint32_t>(a_s.size());
            if (!a_intfc->WriteRecordData(len)) return false;
            return len == 0 || a_intfc->WriteRecordData(a_s.data(), len);
        }

        bool ReadStr(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            std::uint32_t len = 0;
            if (!a_intfc->ReadRecordData(len) || len > kMaxStr) return false;
            a_out.assign(len, '\0');
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len);
        }

        // ---- placement (JS maskOf / placeItems 1:1) ----

        Mask MaskOf(const GridDef& a_def)
        {
            Mask m;
            if (!a_def.shape.empty()) {
                std::istringstream ss(a_def.shape);
                std::string tok;
                int w = 1;
                while (std::getline(ss, tok, '|')) {
                    std::vector<bool> row;
                    for (char c : tok) row.push_back(c == '1');
                    w = (std::max)(w, static_cast<int>(row.size()));
                    m.rows.push_back(std::move(row));
                }
                if (m.rows.empty()) m.rows.push_back({ true });
                for (auto& r : m.rows) r.resize(w, false);
                m.w = (std::min)(w, kCols);
                m.h = static_cast<int>(m.rows.size());
                return m;
            }
            m.w = (std::min)(kCols, (std::max)(1, a_def.w));
            m.h = (std::max)(1, a_def.h);
            m.rows.assign(m.h, std::vector<bool>(m.w, true));
            return m;
        }

        // Reads the layout BEFORE the caller changes it, so it must run first.
        void NoteVacated(const std::string& a_key, RE::TESBoundObject* a_obj)
        {
            const auto li = g_layout.find(a_key);
            if (li == g_layout.end() || li->second.col < 0) return;
            if (li->second.bag == kTrashKey) return;   // not a board cell
            const Mask m = MaskOf(a_obj && g_resolver ? g_resolver(a_obj) : GridDef{});
            g_vacated.push_back({ li->second.col, li->second.row, m.w, m.h,
                                  li->second.bag, static_cast<float>(ImGui::GetTime()) });
        }


        int PlaceItems(std::vector<Item*>& a_list, int a_cols, int a_minRows, int a_maxRows)
        {
            std::vector<std::vector<bool>> occ;
            auto ensureRow = [&](int r) {
                while (static_cast<int>(occ.size()) <= r) occ.emplace_back(a_cols, false);
            };
            auto fits = [&](int c, int r, const Mask& m) {
                if (r + m.h > a_maxRows) return false;
                for (int y = 0; y < m.h; ++y) {
                    ensureRow(r + y);
                    for (int x = 0; x < m.w; ++x) {
                        if (m.rows[y][x] && occ[r + y][c + x]) return false;
                    }
                }
                return true;
            };
            auto mark = [&](int c, int r, const Mask& m) {
                for (int y = 0; y < m.h; ++y)
                    for (int x = 0; x < m.w; ++x)
                        if (m.rows[y][x]) occ[r + y][c + x] = true;
            };

            // pass 1: saved spots — items ALREADY in the grid last render first
            auto pass1 = [&](Item& it) {
                it.fixed = false;
                it.overflow = false;
                if (it.col >= 0 && it.row >= 0 && it.col + it.mask.w <= a_cols &&
                    fits(it.col, it.row, it.mask)) {
                    mark(it.col, it.row, it.mask);
                    it.fixed = true;
                }
            };
            for (auto* it : a_list) if (g_prevKeys.contains(it->key)) pass1(*it);
            for (auto* it : a_list) if (!g_prevKeys.contains(it->key)) pass1(*it);

            // pass 2: first-fit the rest (row -> col)
            for (auto* it : a_list) {
                if (it->fixed) continue;
                if (it->mask.w > a_cols) { it->overflow = true; continue; }
                bool placed = false;
                for (int r = 0; r + it->mask.h <= a_maxRows && !placed; ++r) {
                    for (int c = 0; c <= a_cols - it->mask.w; ++c) {
                        if (fits(c, r, it->mask)) {
                            mark(c, r, it->mask);
                            it->col = c;
                            it->row = r;
                            placed = true;
                            break;
                        }
                    }
                }
                if (!placed) it->overflow = true;
            }
            return (std::max)(a_minRows, static_cast<int>(occ.size()));
        }

        // ---- shared grid renderer (JS makeTile / gridShades / linesFor) ----

        // draws one grid at the current cursor pos; returns via g_target when
        // the carried item hovers this grid
        // ---- Phase 3: DrawGridView passes (bodies moved verbatim) ----

        // pass 1: hairline cell grid + outer border + overflow-zone marking
        void DrawGridChrome(View& a_view, int a_viewIdx, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const float gridW = a_view.cols * CellPx();
            const float gridH = a_view.rows * CellPx();
            // v9: hairline cell grid (acc 13%) inside an acc 30% outer border
            const ImU32 lineCol = Theme::Acc(0.13f);
            for (int c = 1; c < a_view.cols; ++c) {
                dl->AddLine(ImVec2(base.x + c * CellPx(), base.y),
                    ImVec2(base.x + c * CellPx(), base.y + gridH), lineCol);
            }
            for (int r = 1; r < a_view.rows; ++r) {
                dl->AddLine(ImVec2(base.x, base.y + r * CellPx()),
                    ImVec2(base.x + gridW, base.y + r * CellPx()), lineCol);
            }
            dl->AddRect(base, ImVec2(base.x + gridW, base.y + gridH), Theme::Acc(0.30f));

            // GI28: the cell the last action aimed to empty, fading out. If the
            // flash and the gap are not the same cell, the bug is on screen.
            const float now = static_cast<float>(ImGui::GetTime());
            std::erase_if(g_vacated,
                [now](const Vacated& v) { return now - v.born > kVacatedFade; });
            for (const auto& v : g_vacated) {
                if (v.bag != a_view.bagKey) continue;
                if (v.col < 0 || v.row < 0) continue;
                const float k = 1.0f - (now - v.born) / kVacatedFade;   // 1 -> 0
                const ImVec2 q0(base.x + v.col * CellPx(), base.y + v.row * CellPx());
                const ImVec2 q1(q0.x + v.w * CellPx(), q0.y + v.h * CellPx());
                dl->AddRectFilled(q0, q1, Theme::Acc(0.22f * k), 2.0f);
                dl->AddRect(q0, q1, Theme::Acc(0.85f * k), 2.0f, 0, 2.0f);
            }

            // design pass F: overflow-zone marking — rows past the hard board
            // are TEMPORARY (they collapse the moment space frees up). A
            // crimson boundary line + faint tint says "this shelf is borrowed".
            if (a_viewIdx == 0 && a_view.rows > kMinRows) {
                const float oy = base.y + kMinRows * CellPx();
                dl->AddRectFilled(ImVec2(base.x, oy),
                    ImVec2(base.x + gridW, base.y + gridH), IM_COL32(204, 81, 72, 14));
                dl->AddLine(ImVec2(base.x, oy), ImVec2(base.x + gridW, oy),
                    IM_COL32(204, 81, 72, 200), 2.0f);
            }
        }

        // pass 2: occupied-cell shading. GI50: the footprint hover border
        // (corner-fade / edge outline) is gone on user request -- hover
        // feedback is pass 4's accent tint + hover note; the corner-fade
        // look now belongs to the STATUS rings (temper/poison) instead.
        void DrawOccupancyPass(View& a_view, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const auto& sk = Theme::S();
            const ImU32 shadeCol = Theme::Col(sk.shade);
            for (int idx : a_view.items) {
                const auto& it = g_items[idx];
                if (it.overflow || it.col < 0) continue;
                for (int y = 0; y < it.mask.h; ++y) {
                    for (int x = 0; x < it.mask.w; ++x) {
                        if (!it.mask.rows[y][x]) continue;
                        const ImVec2 p0(base.x + (it.col + x) * CellPx(),
                                        base.y + (it.row + y) * CellPx());
                        const ImVec2 p1(p0.x + CellPx(), p0.y + CellPx());
                        // 1px inset: the fill must not cover the grid hairlines
                        dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                            ImVec2(p1.x - 1.0f, p1.y - 1.0f), shadeCol);
                    }
                }
            }
        }

        // pass 3: rarity glow UNDER every sprite
        // GI46/GI49: status rings (poison green / temper gold) on a tile border.
        // ONE implementation shared by the player board's glow pass and the
        // partner/doll DrawGlow -- the first cut lived only in DrawGlow, so
        // the player board showed no rings and its unmasked halo switch read
        // the new bits as "both rarities" red.
        // GI50: EXACTLY the retired hover border's recipe (single-layer
        // Theme::CornerFade, default 0.32 fade) so the rings inherit that
        // sleek look -- only the colors differ: temper white, poison green.
        void DrawStatusRings(ImDrawList* a_dl, std::uint8_t a_bits,
                             const ImVec2& a_min, const ImVec2& a_max)
        {
            const bool poison = (a_bits & 0x4) != 0;
            const bool temper = (a_bits & 0x8) != 0;
            if ((!poison && !temper) || !a_dl) return;
            const ImU32 cTemper = IM_COL32(236, 239, 244, 235);   // white
            const ImU32 cPoison = IM_COL32(96, 205, 110, 235);
            // user spec (both set): left+top = temper, right+bottom = poison
            const ImU32 tl = temper ? cTemper : cPoison;
            const ImU32 br = poison ? cPoison : cTemper;
            Theme::CornerFadeEdges(a_dl, a_min, a_max, tl, tl, br, br, 0.32f);
        }

        void DrawGlowPass(View& a_view, const ImVec2& base)
        {
            auto* cache = IconCache::GetSingleton();
            auto* dl = ImGui::GetWindowDrawList();
            // rarity glow pass — a SEPARATE pass before the icons: silhouette
            // halos bleed past their own footprint, so they must all land
            // UNDER every sprite ([tile bg -> all glows -> all items]).
            // enchanted = blue, unique (DESC) = gold, both = red; alphas
            // luminance-matched (mockup: blue 32% / gold 22% / red 29%).
            for (int idx : a_view.items) {
                const auto& it = g_items[idx];
                if (!it.glow || it.overflow || it.col < 0) continue;
                const ImVec2 p0(base.x + it.col * CellPx(), base.y + it.row * CellPx());
                const float  w = it.mask.w * CellPx();
                const float  h = it.mask.h * CellPx();
                // GI46: rings first -- bits 4|8 are STATUS, not rarity, and the
                // halo switch below must never see them (poison-only used to
                // fall into the "both rarities" red default here).
                DrawStatusRings(dl, it.glow, p0, ImVec2(p0.x + w, p0.y + h));
                const std::uint8_t haloBits = it.glow & 0x3;
                if (!haloBits) continue;
                // brightness slider (settings) scales the base alphas
                const auto ga = [](int a_a) {
                    return static_cast<std::uint32_t>((std::min)(255.0f,
                        a_a * Theme::GlowGain()));
                };
                ImU32 tint;
                switch (haloBits) {
                case 1:  tint = IM_COL32(90, 160, 255, ga(82)); break;   // 32%
                case 2:  tint = IM_COL32(255, 190, 70, ga(56)); break;   // 22%
                default: tint = IM_COL32(255, 80, 60, ga(74));  break;   // 29%
                }
                const auto*  icon = cache->Get(it.obj);

                if (Theme::GlowStyle() == 1 && icon && icon->glowSrv) {
                    // A: silhouette halo — the icon's own blurred alpha, so
                    // the glow hugs the sprite outline even when the item
                    // fills its tile (gloves) or is a thin blade (1x4).
                    // The glow canvas maps its core onto the icon draw rect;
                    // the baked gpad margin extends past it (the halo).
                    const float target = (std::max)(w, h) * 0.95f * it.def.scale;
                    const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                    const float dw = icon->w / ms * target;
                    const float dh = icon->h / ms * target;
                    const ImVec2 c(p0.x + w * 0.5f, p0.y + h * 0.5f);
                    const float sx = dw / static_cast<float>(icon->gw - icon->gpad * 2);
                    const float sy = dh / static_cast<float>(icon->gh - icon->gpad * 2);
                    dl->AddImage(reinterpret_cast<ImTextureID>(icon->glowSrv),
                        ImVec2(c.x - dw * 0.5f - icon->gpad * sx,
                               c.y - dh * 0.5f - icon->gpad * sy),
                        ImVec2(c.x + dw * 0.5f + icon->gpad * sx,
                               c.y + dh * 0.5f + icon->gpad * sy),
                        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
                } else if (void* gtex = UIRoot::GlowTexture()) {
                    // B (!glowstyle=0 revert / glow sprite unavailable):
                    // stretched radial across the footprint. GI49 option A:
                    // 22% overscan -- on big footprints the sprite swallowed
                    // the bright core, leaving only the dim tail visible, so
                    // the halo bleeds slightly into the neighbouring cells
                    // (same philosophy as the silhouette style's gpad margin).
                    const float outX = w * 0.11f;
                    const float outY = h * 0.11f;
                    dl->AddImage(reinterpret_cast<ImTextureID>(gtex),
                        ImVec2(p0.x - outX, p0.y - outY),
                        ImVec2(p0.x + w + outX, p0.y + h + outY),
                        ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
                }
            }
        }

        // pass 4: per-tile sprite + badges/markers + hover/click input
        void DrawItemsPass(View& a_view, const ImVec2& base)
        {
            auto* cache = IconCache::GetSingleton();
            auto* dl = ImGui::GetWindowDrawList();
            const auto& io = ImGui::GetIO();
            const auto& sk = Theme::S();
            // items
            for (int idx : a_view.items) {
                const auto& it = g_items[idx];
                if (it.overflow || it.col < 0) continue;
                const ImVec2 p0(base.x + it.col * CellPx(), base.y + it.row * CellPx());
                const float  w = it.mask.w * CellPx();
                const float  h = it.mask.h * CellPx();

                // pouch tile: the ICON follows the stored amount (N/S/M/F
                // variants) while the item itself stays the pouch form
                RE::TESBoundObject* iconObj = it.obj;
                if (GoldCoins::IsPouch(it.obj->GetFormID())) {
                    if (auto* v = GoldCoins::PouchIconObject()) iconObj = v;
                }
                const IconCache::Icon* iconPtr = cache->Get(iconObj);
                if (iconObj != it.obj) {
                    // ALWAYS queue the variant (no-op when its current key is
                    // cached): during live EDIT the pin fallback keeps Get()
                    // returning the last completed capture, so a miss-only
                    // queue would never capture the new rotation (the icon
                    // froze after the first change until EDIT closed).
                    cache->QueueCapture(iconObj);
                    if (!iconPtr) {
                        // not captured yet: show the base pouch icon meanwhile
                        // (the pouch must NEVER go blank)
                        iconPtr = cache->Get(it.obj);
                    }
                }
                if (const auto* icon = iconPtr) {
                    // alpha-trimmed sprite: aspect-preserving fit — long axis
                    // = footprint long axis * def.scale (draw-time zoom:
                    // linear, instant, structurally clip-free)
                    const float target = (std::max)(w, h) * 0.95f * it.def.scale;
                    const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                    const float dw = icon->w / ms * target;
                    const float dh = icon->h / ms * target;
                    const ImVec2 c(p0.x + w * 0.5f, p0.y + h * 0.5f);
                    UIRoot::DrawItemIcon(dl, icon->srv,
                        ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                        ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f));
                    // Phase 5/6: in barter, dim what can't be sold — coins
                    // (mirror) and items the merchant won't buy (category /
                    // stolen). g_stolen holds only the stolen forms.
                    if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter &&
                        (it.coinValue >= 0 ||
                         !LootBarter::MerchantBuys(it.obj, g_stolen.contains(PoolOfKey(it.key))))) {
                        dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                            IM_COL32(8, 8, 8, 150));
                    }
                } else {
                    // Not captured yet -> queue and show the placeholder frame.
                    // Without this the grid was the ONLY panel that could not
                    // heal a cache miss on its own (Equip and LootBarter both
                    // re-queue here), so switching the icon STYLE emptied every
                    // tile until the menu was closed and reopened: a style flip
                    // changes which sprite Get() looks for, and captures were
                    // only ever queued from Rebuild.
                    cache->QueueCapture(iconObj);
                    dl->AddRect(ImVec2(p0.x + 4, p0.y + 4), ImVec2(p0.x + w - 4, p0.y + h - 4),
                        Theme::Acc(0.35f), 3.0f);
                }

                // Mabinogi-style TOP-LEFT badge: stack count — or, on a coin
                // tile, the gold value it represents (G2)
                {
                    char buf[16];
                    buf[0] = '\0';
                    const RE::FormID bfid = it.obj->GetFormID();
                    if (GoldCoins::IsCoinForm(bfid) && !GoldCoins::IsPouch(bfid)) {
                        std::snprintf(buf, sizeof(buf), "%d", it.coinValue);   // G4: stored on the tile
                    } else if (it.count > 1) {
                        std::snprintf(buf, sizeof(buf), "%d", it.count);
                    }
                    if (buf[0]) DrawCountBadge(dl, p0, buf);
                }

                // marker tray (BOTTOM-RIGHT, user-picked system 3): one row of
                // shape+colour coded state markers, anchored right, fixed order
                // favorite ◆(hi) · quest ▲(ivory) · stolen ●(crimson). The
                // top-left stays reserved for the count/coin badge.
                {
                    const RE::FormID mfid = it.obj->GetFormID();
                    const bool isQuest = g_questItem.contains(PoolOfKey(it.key));
                    const bool isStolen = g_stolen.contains(PoolOfKey(it.key));
                    if (it.fav || isQuest || isStolen) {
                        const float S = Theme::Scale();
                        const float r = 4.0f * S;          // marker half-size
                        const float gapM = 4.0f * S;
                        const ImU32 oc = IM_COL32(0, 0, 0, 255);
                        float cx = p0.x + w - r - 4.0f;    // rightmost marker centre
                        const float cy = p0.y + h - r - 4.0f;
                        // draw right-to-left so the visual order stays fav-quest-stolen
                        if (isStolen) {
                            dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(206, 64, 52, 255));
                            dl->AddCircle(ImVec2(cx, cy), r, oc, 0, 1.0f);
                            cx -= 2.0f * r + gapM;
                        }
                        if (isQuest) {
                            const ImVec2 a(cx, cy - r - 1.0f);
                            const ImVec2 b(cx - r, cy + r * 0.8f);
                            const ImVec2 c(cx + r, cy + r * 0.8f);
                            // ivory reads on the (all-dark) skins
                            dl->AddTriangleFilled(a, b, c,
                                IM_COL32(236, 232, 220, 255));
                            dl->AddTriangle(a, b, c, oc, 1.0f);
                            cx -= 2.0f * r + gapM;
                        }
                        if (it.fav) {
                            const float d = r * 1.25f;   // diamond half-diagonal
                            const ImVec2 q0(cx, cy - d), q1(cx + d, cy);
                            const ImVec2 q2(cx, cy + d), q3(cx - d, cy);
                            dl->AddQuadFilled(q0, q1, q2, q3, Theme::Col(sk.hi, 1.0f));
                            dl->AddQuad(q0, q1, q2, q3, oc, 1.0f);
                        }
                    }
                }

                // H1: edit-mode selection highlight (skin sel colour)
                if (Editor::IsSelected(BaseKey(it.key))) {
                    if (sk.cornerFade) {
                        // v10.4: crimson corner fade, doubled 1px-inset for the
                        // same visual weight as the old 2px outline
                        const ImU32 selCol = Theme::Col(sk.sel, 1.0f);
                        Theme::CornerFade(dl, p0, ImVec2(p0.x + w, p0.y + h), selCol);
                        Theme::CornerFade(dl, ImVec2(p0.x + 1, p0.y + 1),
                            ImVec2(p0.x + w - 1, p0.y + h - 1), selCol);
                    } else {
                        dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h),
                            Theme::Col(sk.sel, 1.0f), 0.0f, 0, 2.0f);
                    }
                }

                // hover / pickup / bag-toggle surface
                ImGui::SetCursorScreenPos(p0);
                ImGui::InvisibleButton(("##it_" + it.key).c_str(), ImVec2(w, h));
                // overlay chrome (popups/settings/EDIT panel) extends beyond
                // the ImGui window rect — block hover/interaction underneath
                const bool ovl = UIRoot::MouseInOverlay();
                // C: vanilla "Item Zoom" (controlmap Item Zoom = 0x2E) — the
                // rotatable 3D view, the ONLY way to read detail that lives on
                // the model (dragon-claw glyphs = door puzzle solution).
                // Works in EDIT mode too, and targets the same object/key the
                // editor would select so an adopted angle lands on that def.
                auto inspectHere = [&]() {
                    if (iconObj != it.obj) UIRoot::OpenInspect(iconObj, FormKey(iconObj));
                    else                   UIRoot::OpenInspect(it.obj, BaseKey(it.key));
                };
                if (!g_held && !ovl && ImGui::IsItemHovered() &&
                    ImGui::IsKeyPressed(ImGuiKey_C, false) &&
                    !ImGui::GetIO().WantTextInput) {
                    inspectHere();
                }
                if (!g_held && !ovl && ImGui::IsItemHovered()) {   // v9 hover tint
                    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), Theme::Acc(0.10f));
                    Sfx::HoverNote(ImGui::GetItemID());   // cursor entered this tile
                }
                if (Editor::IsEditMode()) {
                    // H1: clicks select for editing (no carry, no equip).
                    // Split tiles ("#k") edit their BASE key — the def store
                    // is per-form, so overrides apply to every copy.
                    // Pouch: edit the CURRENTLY DISPLAYED variant (N/S/M/F)
                    // — set the stored amount to a band to tune that band's
                    // icon; the tile always shows what is being edited.
                    if (!ovl && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        if (iconObj != it.obj) {
                            Editor::Select(iconObj, FormKey(iconObj));
                        } else {
                            Editor::Select(it.obj, BaseKey(it.key));
                        }
                    }
                    if (!ovl && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", it.obj->GetName());
                    }
                } else if (!g_held && !ovl) {
                    if (ImGui::IsItemHovered()) {   // tooltip suppressed while carrying
                        const RE::FormID fid = it.obj->GetFormID();
                        const bool isCoin = GoldCoins::IsCoinForm(fid) &&
                                            !GoldCoins::IsPouch(fid);
                        if (!isCoin) {   // coins: value badge only, no tooltip
                            // Phase 4/5: in barter, the player side shows the SELL
                            // price — including the pouch (a real sellable item).
                            int price = -1;
                            if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter) {
                                const int val = TileValue(it.obj, it.uid, it.sig);   // GI43
                                if (val > 0) price = LootBarter::SellPrice(it.obj, val);
                            }
                            // D1: a player TILE is exactly one unit — read its
                            // own sub-stack, not "any list on the entry"
                            DrawItemTooltip(it.obj, it.count,
                                GoldCoins::IsPouch(fid) ? GoldCoins::PouchStored() : -1,
                                price, false, nullptr, ExtraScope::kUnit,
                                it.uid, it.xlIdx);
                        }
                        // D1: vanilla-style discard — hover + R drops ONE unit
                        // (spam R for more; carry-outside stays the full-stack drop).
                        // G2: a coin drops its VALUE as real world gold; the
                        // pouch drops as an item — its stored gold travels
                        // with it (container sink handles the ledger).
                        if (ImGui::IsKeyPressed(ImGuiKey_R, false) &&
                            !ImGui::GetIO().WantTextInput) {
                            if (g_questItem.contains(PoolOfKey(it.key))) {   // Phase 7: can't drop
                                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                            } else if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) {
                                // G4: a pinned purse releases its fixed value; an
                                // auto coin drops its ordinal value. Either way
                                // DropAsGold debits the (now-walking) gold.
                                if (GoldCoins::PinnedValue(it.key) >= 0)
                                    GoldCoins::UnpinTile(it.key);
                                GoldCoins::DropAsGold(it.coinValue);
                                g_layout.erase(it.key);   // free THIS slot; rebuild re-maps survivors by position
                                g_needRebuild = true;
                            } else {
                                if (it.count <= 1) {   // last unit: tile disappears
                                    g_layout.erase(it.key);
                                    if (it.def.bag != 0) {   // E4: contents back to main
                                        g_openBags.erase(it.key);
                                        for (auto& [k, le] : g_layout) {
                                            if (le.bag == it.key) le.bag.clear();
                                        }
                                    }
                                }
                                if (g_dropWorld) {
                                    g_dropWorld(it.obj, 1,   // GI36: star dies with it
                                        ResolveExitUnit(it.obj, it.uid, it.sig, 1,
                                                        it.fav ? 1 : 0));
                                }
                                g_needRebuild = true;
                            }
                        }
                        // F: vanilla favorite toggle (feeds the Q menu);
                        // coins/pouch are mirror artefacts — not favoritable
                        if (ImGui::IsKeyPressed(ImGuiKey_F, false) &&
                            !ImGui::GetIO().WantTextInput &&
                            !GoldCoins::IsCoinForm(fid)) {
                            ToggleFavorite(it.key, it.obj, it.uid, it.xlIdx);
                            Sfx::Favorite();
                            g_needRebuild = true;
                        }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {   // C1: pickup
                        const RE::FormID lfid = it.obj->GetFormID();
                        const bool coin = GoldCoins::IsCoinForm(lfid) && !GoldCoins::IsPouch(lfid);
                        if (io.KeyShift && coin && it.coinValue > 1) {
                            // G4: shift+left-click on a gold tile = VALUE split
                            // slider (1..coinValue, starts at half). The chosen
                            // amount lands on the cursor as a pinned purse.
                            LootBarter::OpenSlider(it.obj, it.coinValue, LootBarter::XferDir::kPickup, it.key);
                        } else if (io.KeyShift && GoldCoins::IsPouch(lfid)) {
                            // G2: shift+left-click on the pouch = withdraw
                            // window, same as right-click (user shortcut)
                            g_pouchOpen = true;
                            g_pouchSlider = (std::max)(1, GoldCoins::PouchStored() / 2);
                            Sfx::SelectOn();
                        } else if (io.KeyShift && it.count > 1 && !coin) {
                            // shift+left-click on a stack = split slider (ALWAYS
                            // — plain inventory too). The chosen amount lands on
                            // the cursor; drop it on the container to store, or
                            // outside every window to discard just that many.
                            LootBarter::OpenSlider(it.obj, it.count, LootBarter::XferDir::kPickup, it.key);
                        } else {
                            g_held = Held{ it.key, it.obj, it.mask, it.count, it.def.bag != 0,
                                           it.def.scale,
                                           io.MousePos.x - p0.x, io.MousePos.y - p0.y, true };
                            g_held->coinValue = it.coinValue;   // G4: -1 for non-coins
                            g_held->xlIdx = it.xlIdx;           // GI1
                            g_held->uid = it.uid;
                            g_held->sig = it.sig;               // GI25
                            g_held->fav = it.fav;               // GI36
                            if (g_poolTrace) {
                                const auto le = g_layout.count(it.key) ? g_layout[it.key]
                                                                      : LayoutEntry{};
                                SKSE::log::info("[ACT] lift-from-grid '{}' key '{}' at [{},{}]",
                                    it.obj->GetName(), it.key, le.col, le.row);
                            }
                            if (g_sound) g_sound(it.obj, true);
                            g_needRebuild = true;   // cells free next frame (C1)
                        }
                    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        // bag / pouch right-click is ALWAYS manage (toggle /
                        // withdraw), mode-independent — trade & storage happen
                        // by drag only (confirmed spec). Everything else
                        // branches on the UI mode so loot/barter never fires
                        // the equip action.
                        if (it.inBag == kTrashKey) {
                            // F2: right-click on a PARKED tile = restore. Its
                            // pre-park spot is tried first; taken/gone -> the
                            // placer first-fits (col -1).
                            LayoutEntry back;   // default: first-fit to main
                            back.col = -1;
                            back.row = -1;
                            back.count = it.count;
                            if (const auto ri = g_trashReturn.find(it.key);
                                ri != g_trashReturn.end()) {
                                back = ri->second;
                                back.count = it.count;
                                g_trashReturn.erase(ri);
                            }
                            g_layout[it.key] = back;
                            if (g_sound) g_sound(it.obj, true);
                        } else if (it.def.bag != 0) {   // E2: bag right-click = window toggle
                            if (g_openBags.contains(it.key)) {
                                g_openBags.erase(it.key);
                                Sfx::BagClose();
                            } else {
                                g_openBags.insert(it.key);
                                Sfx::BagOpen();
                            }
                        } else if (GoldCoins::IsPouch(it.obj->GetFormID())) {
                            g_pouchOpen = true;   // G2: withdraw window
                            // start at half the stored amount (split-friendly default)
                            g_pouchSlider = (std::max)(1, GoldCoins::PouchStored() / 2);
                            Sfx::SelectOn();
                        } else if (LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                            // loot: right-click stores this tile into the
                            // container — a stack (>1) opens the quantity
                            // slider first. Coins are mirror artefacts
                            // (excluded); the pouch stores fine (gold travels
                            // via the container sink).
                            const RE::FormID fid = it.obj->GetFormID();
                            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                                if (g_questItem.contains(PoolOfKey(it.key))) {   // Phase 7: locked
                                    Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                                } else if (it.count > 1) {
                                    LootBarter::OpenSlider(it.obj, it.count,
                                        LootBarter::XferDir::kStore, it.key, 0, it.uid, it.sig,
                                        false, it.fav);
                                } else {
                                    LootBarter::RequestStore(it.obj, it.count,
                                                             it.uid, it.sig, it.fav);
                                    NotePendingRemove(it.obj, it.key, it.count);
                                }
                            }
                        } else if (LootBarter::CurrentMode() ==
                                   LootBarter::Mode::kPickpocket) {
                            // F6b: right-click = reverse-pickpocket this tile
                            // onto the mark (engine roll on the Tick)
                            const RE::FormID fid = it.obj->GetFormID();
                            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                                if (g_questItem.contains(PoolOfKey(it.key))) {
                                    Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                                } else if (it.count > 1) {
                                    LootBarter::OpenSlider(it.obj, it.count,
                                        LootBarter::XferDir::kPickStore, it.key, 0, it.uid, it.sig,
                                        false, it.fav);
                                } else {
                                    LootBarter::RequestPickStore(it.obj, 1, it.uid, it.sig, it.key,
                                                                 it.fav);
                                }
                            }
                        } else if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter) {
                            // Phase 5: SELL this tile to the merchant. A stack
                            // opens the quantity slider; a single item sells at
                            // once. Coins excluded (mirror); the pouch sells via
                            // the gold-travel path (§2-C). Blocked when the
                            // merchant can't afford it.
                            const RE::FormID fid = it.obj->GetFormID();
                            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                                const int val = TileValue(it.obj, it.uid, it.sig);   // GI43
                                // Phase 7: quest items can't be sold
                                if (g_questItem.contains(PoolOfKey(it.key))) {
                                    Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                                // Phase 6: merchant category / stolen restriction
                                } else if (!LootBarter::MerchantBuys(it.obj, g_stolen.contains(PoolOfKey(it.key)))) {
                                    Sfx::FailNote(Lang::T(Lang::Str::MerchantWontBuy));
                                } else if (it.count > 1) {
                                    LootBarter::OpenSlider(it.obj, it.count,
                                        LootBarter::XferDir::kSell, it.key, val, it.uid, it.sig,
                                        false, it.fav);
                                } else {
                                    const int total = (val > 0
                                        ? LootBarter::SellPrice(it.obj, val) : 0);
                                    if (total > 0 && LootBarter::MerchantGold() < total) {
                                        Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                                    } else if (it.fav) {
                                        LootBarter::AskSellConfirm(it.obj, 1, total, val, it.key,
                                                                   it.uid, it.sig,
                                                                   it.fav);   // favorite: confirm
                                    } else {
                                        LootBarter::RequestSell(it.obj, 1, total, val,
                                                                it.uid, it.sig, it.fav);
                                        NotePendingRemove(it.obj, it.key, 1);
                                    }
                                }
                            }
                        } else {   // D3: right-click = equip (refresh even on reject)
                            if (g_poolTrace) {
                                const auto le = g_layout.count(it.key) ? g_layout[it.key]
                                                                      : LayoutEntry{};
                                SKSE::log::info("[ACT] rclick-equip '{}' key '{}' at [{},{}]",
                                    it.obj->GetName(), it.key, le.col, le.row);
                            }
                            // The engine equips on the next Tick, so the unit
                            // spends a frame "in the pack, not carried" and
                            // flickers back into its old cell. Suppress it for
                            // those frames -- but NOT via NotePendingRemove:
                            // that mechanism resolves when the engine's stock
                            // COUNT drops, and equipping never drops it, so the
                            // entry never cleared and repeated equips hid one
                            // more spare each time until the pool went blank.
                            if (Equip::EquipItem(it.obj, "", it.uid, it.xlIdx,
                                                 it.sig, it.key)) {
                                // right-click: weapons go to the right hand,
                                // armour has only one place to go -- and a LIGHT
                                // (torch) goes to the LEFT. Recording it as right
                                // meant the "has it landed?" test looked for a
                                // worn list in the wrong hand, never found one,
                                // and the tile flickered until the entry expired.
                                // [DOLL] shieldL='Torch'(h2) is the proof.
                                int hand = 0;
                                if (it.obj->Is(RE::FormType::Weapon)) hand = 1;
                                else if (it.obj->Is(RE::FormType::Light)) hand = 2;
                                NotePendingEquip(it.obj, it.uid, it.sig, hand, it.key);
                                // stackables: name the tile that gives one up
                                g_drainHint = { FormKey(it.obj), it.key };
                            }
                        }
                        g_needRebuild = true;
                    }
                }
            }
        }

        // pass 5: carried item -> drop candidate + placement ghost (C2)
        void ComputeDropCandidate(View& a_view, int a_viewIdx, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const auto& io = ImGui::GetIO();
            const float gridW = a_view.cols * CellPx();
            const float gridH = a_view.rows * CellPx();
            // carried item: this grid is the drop candidate while hovered (C2)
            if (g_held &&
                io.MousePos.x >= base.x && io.MousePos.x < base.x + gridW &&
                io.MousePos.y >= base.y && io.MousePos.y < base.y + gridH) {
                auto& held = *g_held;
                const float px = io.MousePos.x - base.x - held.offX;
                const float py = io.MousePos.y - base.y - held.offY;
                int col = static_cast<int>(std::lround(px / CellPx()));
                int row = static_cast<int>(std::lround(py / CellPx()));
                col = (std::max)(0, (std::min)(a_view.cols - held.mask.w, col));
                row = (std::max)(0, (std::min)(a_view.rows - held.mask.h, row));

                g_target = {};
                g_target.has = true;
                g_target.view = a_viewIdx;
                g_target.col = col;
                g_target.row = row;

                const bool bagInBag = held.isBag && !a_view.bagKey.empty();   // E4
                const bool sizeOk = held.mask.w <= a_view.cols && held.mask.h <= a_view.rows;

                for (int idx : a_view.items) {
                    const auto& other = g_items[idx];
                    if (other.overflow || other.col < 0) continue;
                    bool hit = false;
                    for (int y = 0; y < held.mask.h && !hit; ++y) {
                        for (int x = 0; x < held.mask.w && !hit; ++x) {
                            if (!held.mask.rows[y][x]) continue;
                            const int lc = col + x - other.col, lr = row + y - other.row;
                            if (lc >= 0 && lr >= 0 && lc < other.mask.w && lr < other.mask.h &&
                                other.mask.rows[lr][lc]) {
                                hit = true;
                            }
                        }
                    }
                    if (hit) g_target.blockers.push_back(idx);
                }
                g_target.valid = !bagInBag && sizeOk && g_target.blockers.empty();

                // ghost: green = ok, red = badspot
                const ImU32 ghost = g_target.valid ? IM_COL32(90, 170, 90, 90)
                                                   : IM_COL32(190, 60, 60, 110);
                for (int y = 0; y < held.mask.h; ++y) {
                    for (int x = 0; x < held.mask.w; ++x) {
                        if (!held.mask.rows[y][x]) continue;
                        const ImVec2 p0(base.x + (col + x) * CellPx(), base.y + (row + y) * CellPx());
                        dl->AddRectFilled(p0, ImVec2(p0.x + CellPx(), p0.y + CellPx()), ghost);
                    }
                }
            }
        }

        void DrawGridView(View& a_view, int a_viewIdx)
        {
            const float gridW = a_view.cols * CellPx();
            const float gridH = a_view.rows * CellPx();
            const ImVec2 base = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(gridW, gridH));

            DrawGridChrome(a_view, a_viewIdx, base);      // pass 1
            DrawOccupancyPass(a_view, base);              // pass 2
            DrawGlowPass(a_view, base);                   // pass 3
            DrawItemsPass(a_view, base);                  // pass 4
            ComputeDropCandidate(a_view, a_viewIdx, base);// pass 5
        }
    }

    void SetDefResolver(DefResolver a_resolver)
    {
        g_resolver = std::move(a_resolver);
    }

    GridDef ResolveDef(RE::TESBoundObject* a_obj)
    {
        return (g_resolver && a_obj) ? g_resolver(a_obj) : GridDef{};
    }

    void SetGameCallbacks(std::function<void(RE::TESBoundObject*, bool)> a_sound,
                          std::function<void(RE::TESBoundObject*, int,
                                             RE::ExtraDataList*)> a_dropToWorld)
    {
        g_sound = std::move(a_sound);
        g_dropWorld = std::move(a_dropToWorld);
    }

    bool IsHolding()
    {
        return g_held.has_value();
    }

    // GI17: "is THIS unit the one on the cursor". The form-level question was
    // never enough once a partner stack became several cells: lifting one of
    // three identical daggers hid every cell of that form, which reads exactly
    // like picking up all three at once.
    // GI18: the content signature of the carried unit, resolved from whichever
    // side actually owns it right now. 0 = nothing held, or a plain unit.
    std::uint16_t HeldInstanceSig()
    {
        if (!g_held || !g_held->obj) return 0;
        RE::TESObjectREFR* owner = RE::PlayerCharacter::GetSingleton();
        if (g_held->fromPartner) {
            if (auto* p = LootBarter::Partner()) owner = p;
        }
        auto* xl = ExtraForTile(LiveEntry(owner, g_held->obj),
                                HeldUidOf(g_held->key, g_held->uid), g_held->xlIdx);
        return InstanceSig(xl);
    }

    bool IsHeldPartnerUnit(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                           int a_xlIdx, int a_ord)
    {
        if (!g_held || !g_held->fromPartner || g_held->obj != a_obj) return false;
        return g_held->uid == a_uid && g_held->xlIdx == a_xlIdx &&
               g_held->partnerOrd == a_ord;
    }

    RE::TESBoundObject* HeldPartnerObject()
    {
        return (g_held && g_held->fromPartner) ? g_held->obj : nullptr;
    }

    bool HeldFootprint(int& a_w, int& a_h, float& a_offX, float& a_offY)
    {
        if (!g_held) return false;
        a_w = g_held->mask.w;
        a_h = g_held->mask.h;
        a_offX = g_held->offX;
        a_offY = g_held->offY;
        return true;
    }

    void CancelHold()
    {
        if (!g_held) return;
        g_held.reset();
        Rebuild();   // the item resumes its saved spot (pickup never erased it)
    }

    void BeginCarry(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                    int a_hand, bool a_swappedOut)
    {
        if (!a_obj || g_held) return;
        const GridDef def = g_resolver ? g_resolver(a_obj) : GridDef{};
        Mask m = MaskOf(def);
        const float ox = m.w * CellPx() * 0.5f;
        const float oy = m.h * CellPx() * 0.5f;
        // The key must name the unit's POOL, not just its form. Keyed to the
        // bare FormKey (the PLAIN pool), a tempered weapon picked off the doll
        // made the enumeration hold back a PLAIN unit instead: the tempered one
        // dropped back onto its old cell and the plain one rode the cursor.
        // With the pool prefix a doll carry behaves exactly like a board carry --
        // the unit's own slot is reserved for it and cancel restores it there.
        // ...but it must not COLLIDE with a sibling that is already on the board.
        // The worn unit had no tile (worn units are not enumerated), so it needs
        // a key of its own; taking the bare pool prefix stole the key of a spare
        // sitting in that same pool, whose remembered cell was then excluded as
        // "the carried tile" and lost -- the spare jumped to the first gap and a
        // cancel could not put it back. Take the lowest ordinal nobody holds.
        // GI31: if this pool has a PARKED star -- the entry left behind when the
        // favourite was equipped -- the unit coming off the doll IS that one, so
        // carry its key. Taking a fresh key instead left the parked entry stranded
        // (cell-less, starred, nothing worn) and the tile that landed had no star.
        std::string carryKey;
        {
        }
        {
            carryKey = PoolPrefix(FormKey(a_obj), a_uid, a_sig);
            for (int n = 1; g_layout.contains(carryKey) ||
                            GoldCoins::PinnedValue(carryKey) >= 0; ++n) {
                carryKey = PoolPrefix(FormKey(a_obj), a_uid, a_sig) + "#" + std::to_string(n);
            }
        }
        g_held = Held{ std::move(carryKey), a_obj, std::move(m),
                       1, def.bag != 0, def.scale, ox, oy, true };
        g_held->fromDoll = true;   // still worn until the unequip lands
        g_held->hand = a_hand;
        g_held->uid = a_uid;   // GI25: the doll hands over the WORN sub-stack,
        g_held->sig = a_sig;   // which is usually the tempered/enchanted copy
        g_held->swappedOut = a_swappedOut;
        if (g_poolTrace) {
            SKSE::log::info("[ACT] lift-from-doll '{}' hand={} uid {:04X} sig {:04X} key '{}'",
                a_obj->GetName(), a_hand, a_uid, a_sig, g_held->key);
        }
        if (g_sound) g_sound(a_obj, true);
        g_needRebuild = true;
    }

    void BeginPartnerCarry(RE::TESBoundObject* a_obj, int a_count, int a_value,
                           float a_offX, float a_offY,
                           std::uint16_t a_uid, int a_xlIdx, int a_ord)
    {
        if (!a_obj || g_held) return;
        const GridDef def = g_resolver ? g_resolver(a_obj) : GridDef{};
        Mask m = MaskOf(def);
        Held h;
        h.key = "##partner";   // not a real grid tile
        h.obj = a_obj;
        h.mask = std::move(m);
        h.count = a_count;
        h.isBag = def.bag != 0;
        h.defScale = def.scale;
        // F7: pick up where clicked (player-tile parity); centre fallback
        // for swap pickups (no meaningful click point)
        h.offX = a_offX >= 0.0f ? a_offX : h.mask.w * CellPx() * 0.5f;
        h.offY = a_offY >= 0.0f ? a_offY : h.mask.h * CellPx() * 0.5f;
        h.justPicked = true;
        h.fromPartner = true;
        h.partnerValue = a_value;
        h.uid = a_uid;       // D4: which sub-stack was picked up
        h.xlIdx = a_xlIdx;
        h.partnerOrd = a_ord;   // GI19: and which cell, for plain look-alikes
        if (auto* p = LootBarter::Partner()) {   // GI25: signature for the transfer
            h.sig = InstanceSig(ExtraForTile(LiveEntry(p, a_obj), a_uid, a_xlIdx));
        }
        g_held = std::move(h);
        if (g_sound) g_sound(a_obj, true);
    }

    void RequestRebuild()
    {
        g_needRebuild = true;
    }

    void RefreshDefs()
    {
        auto* cache = IconCache::GetSingleton();
        for (auto& it : g_items) {
            it.def = g_resolver ? g_resolver(it.obj) : GridDef{};
            // mask untouched: this path is for orientation-only edits
            cache->QueueCapture(it.obj);   // no-op when the key is cached
        }
    }

    // GI30: is this pool's favourite currently ON THE BODY? A parked star -- a
    // starred layout entry with no cell -- is exactly that: the unit it belongs
    // to left the board to be worn. The doll draws the mark while it is away, so
    // the star never appears to vanish.
    bool IsPoolStarWorn(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                        std::uint16_t a_sig)
    {
        // GI40: ask the POOL, exactly like the grid tiles do. The worn list is
        // one member of it -- whether the engine happens to be keeping the
        // hotkey there or on a spare's list is an implementation detail that
        // must not decide whether the doll shows a star.
        auto* p = RE::PlayerCharacter::GetSingleton();
        return PoolHasStar(LiveEntryOf(p, a_obj), a_uid, a_sig);
    }

    void ForgetTile(const std::string& a_key)
    {
        // Rule 13: leaving the board forgets the cell, and EQUIPPING is leaving
        // the board -- same as selling, storing or dropping. Instance tiles used
        // to be kept so an unequip landed back in its own cell, but that made
        // equipping the one exception to an otherwise uniform rule, and with two
        // units of one form the returning copy could claim the other's cell.
        //
        // The key names the tile the player actually acted on, which is what the
        // pool model requires: the slot that empties is the one they clicked, not
        // an arbitrary sibling. In-memory; the cosave persists on game save.
        if (a_key.empty()) return;
        g_layout.erase(a_key);
    }

    void NotifySlotDropTarget(const std::string& a_slotId)
    {
        g_slotTarget = a_slotId;
    }

    // ---- Phase 2: ONE inventory walk for display + capacity ----
    namespace
    {
        // skip rules shared by EVERY inventory walk. The four hand-copied
        // filters drifted apart once already (a missed GetPlayable made
        // hidden scripting copies overload the board) — one predicate now.
        bool SkipInventoryEntry(RE::TESBoundObject* a_obj, int a_count)
        {
            if (!a_obj || a_count <= 0) return true;
            // L1: hold back only the RESERVED UNITS, not the whole entry —
            // the surplus (looted, bought, crafted) stays on the board.
            if (a_count <= Loadout::ReservedCount(a_obj->GetFormID())) return true;
            // coins may be deliberately UNNAMED (ESP) so loot-HUD widgets skip
            // their mirror adds — the grid shows them regardless (value badge)
            const char* name = a_obj->GetName();
            if ((!name || !*name) && !GoldCoins::IsCoinForm(a_obj->GetFormID())) return true;
            // vanilla parity: NON-PLAYABLE forms never show in the vanilla UI
            // (mods keep hidden scripting copies in the inventory)
            if (!a_obj->GetPlayable()) return true;
            return false;
        }

        // ---- GI1/GI2: the ONE enumeration of a cap-1 entry into tile identities ----
        // The display collector and the capacity sims MUST agree about which
        // keys exist, or the sim looks up saved spots under keys the board never
        // creates and models a different board than the one on screen.
        //
        // An entry's units are not interchangeable: some carry an ExtraDataList
        // (enchanted / tempered / poisoned / renamed / worn), some don't, and one
        // list can stand for several units (GetCount). Two traps:
        //   * enumerate only the lists -> plain units vanish
        //     (3 swords, 1 enchanted -> 1 list -> 2 tiles lost)
        //   * enumerate only the count -> tiles are bare ordinals again, so
        //     equipping one shifts every later tile onto its neighbour's saved
        //     spot AND its neighbour's per-instance data
        // Hence: plain = count - SUM(list.GetCount()).
        //
        // a_units is the caller's already-adjusted board total (worn copy and
        // pending removals subtracted); the surplus is drained from the plain
        // pool first so a sold unit never orphans an instance-keyed tile.
        struct UnitTile
        {
            std::string key;
            int         xlIdx = -1;   // index in entry->extraLists, -1 = plain
        };

        // a_skipWorn=false keeps the body-worn unit in the walk: corpses and
        // pickpocket targets show what the NPC is wearing.
        // a_base non-empty: also drop the units that are NOT ON THE BOARD -- the one
        // riding the cursor and any whose equip the engine has not applied yet. Doing
        // it HERE, once, is the whole point: the board's input set is correct by
        // construction and every downstream pass just renders what it is handed.
        // Removal is best-effort by design: while a lifted unit is still WORN it is
        // already absent (skipWorn), and asking to remove it again must be a no-op,
        // not an over-subtraction.
        void EnumerateUnitRefs(int a_count, int a_units, RE::InventoryEntryData* a_entry,
                               std::vector<UnitRef>& a_out, bool a_skipWorn = true,
                               const std::string& a_base = {})
        {
            struct Inst { std::uint16_t uid; std::uint16_t sig; int units; int xlIdx;
                          int hand = 0; bool worn = false; };
            std::vector<Inst> insts;
            std::vector<Inst> wornUnits;   // skipped above; never part of the board
            int listed = 0;
            if (a_entry && a_entry->extraLists) {
                int xi = 0;
                for (auto* xl : *a_entry->extraLists) {
                    const int idx = xi++;
                    if (!xl) continue;
                    const int n = (std::max)(1, xl->GetCount());
                    listed += n;
                    if (a_skipWorn && (xl->HasType<RE::ExtraWorn>() ||
                                       xl->HasType<RE::ExtraWornLeft>())) {
                        // remember WHICH units these were: the off-board pass below
                        // has to tell "this unit never entered the set" from "this
                        // unit is on the board and must come out".
                        std::uint16_t wuid = 0;
                        if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                            wuid = xu->uniqueID;
                        }
                        wornUnits.push_back({ wuid, InstanceSig(xl), n, idx,
                                              xl->HasType<RE::ExtraWornLeft>() ? 2 : 1 });
                        continue;
                    }
                    std::uint16_t uid = 0;
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
                    // GI41: only reachable with a_skipWorn=false (partner boards
                    // show what the NPC wears). Record it HERE, where it is a
                    // fact, instead of leaving the consumer to guess by position.
                    const bool wornHere = xl->HasType<RE::ExtraWorn>() ||
                                          xl->HasType<RE::ExtraWornLeft>();
                    insts.push_back({ uid, InstanceSig(xl), n, idx,
                                      wornHere ? (xl->HasType<RE::ExtraWornLeft>() ? 2 : 1) : 0,
                                      wornHere });
                }
            }
            int plain = a_count - listed;
            if (plain < 0) {
                SKSE::log::warn("[GRID] GI2: count {} < listed {} — clamped", a_count, listed);
                plain = 0;
            }
            int have = plain;
            for (const auto& in : insts) have += in.units;
            for (int drop = have - a_units; drop > 0;) {
                if (plain > 0) { const int t = (std::min)(plain, drop); plain -= t; drop -= t; continue; }
                if (insts.empty()) break;
                const int t = (std::min)(insts.back().units, drop);
                insts.back().units -= t;
                drop -= t;
                if (insts.back().units <= 0) insts.pop_back();
            }
            // Which of the hashed extras each list actually carries. The signature
            // was observed to CHANGE when a unit is equipped, which breaks every
            // worn-boundary match; this names the extra responsible.
            if (g_poolTrace && a_entry && a_entry->extraLists && !a_base.empty()) {
                int xi2 = 0;
                for (auto* xl : *a_entry->extraLists) {
                    if (!xl) { ++xi2; continue; }
                    std::string f;
                    if (xl->HasType<RE::ExtraHealth>())          f += "HEALTH ";
                    if (xl->HasType<RE::ExtraEnchantment>())     f += "ENCH ";
                    if (xl->HasType<RE::ExtraCharge>())          f += "CHARGE ";
                    if (xl->HasType<RE::ExtraPoison>())          f += "POISON ";
                    if (xl->HasType<RE::ExtraSoul>())            f += "SOUL ";
                    if (xl->HasType<RE::ExtraTextDisplayData>()) f += "NAME ";
                    if (xl->HasType<RE::ExtraWorn>())            f += "|worn ";
                    if (xl->HasType<RE::ExtraWornLeft>())        f += "|wornL ";
                    SKSE::log::info("[XL] {} [{}] sig {:04X} count {} : {}",
                        a_base, xi2, InstanceSig(xl), xl->GetCount(),
                        f.empty() ? "(none)" : f);
                    ++xi2;
                }
            }

            if (!a_base.empty()) {
                auto takeOne = [&](std::uint16_t uid, std::uint16_t sig, bool mayBeWorn,
                                   const char* why, int hand) {
                    const auto trace = [&](const char* from) {
                        if (g_poolTrace) {
                            SKSE::log::info("[TAKE] {} uid {:04X} sig {:04X} -> {} "
                                            "(worn {}, insts {}, plain {})",
                                why, uid, sig, from, wornUnits.size(), insts.size(), plain);
                        }
                    };
                    // A unit that is STILL WORN was never in the set: `listed`
                    // already accounted for it, so `plain` excludes it too. Asking
                    // to remove it again used to fall through to `--plain` and take
                    // an innocent SPARE instead -- lifting a plain equipped sword
                    // off the doll made every plain copy vanish until the unequip
                    // landed. Consume the worn entry instead and do nothing.
                    if (mayBeWorn) {
                        for (auto it = wornUnits.begin(); it != wornUnits.end(); ++it) {
                            if (it->uid != uid || it->sig != sig) continue;
                            if (hand != 0 && it->hand != hand) continue;   // other hand
                            if (--it->units <= 0) wornUnits.erase(it);
                            trace("worn (already off the board)");
                            return;
                        }
                    }
                    for (auto it = insts.begin(); it != insts.end(); ++it) {
                        if (it->uid != uid || it->sig != sig) continue;
                        if (--it->units <= 0) insts.erase(it);
                        trace("REMOVED from insts");
                        return;
                    }
                    if (uid == 0 && sig == 0 && plain > 0) {
                        --plain;
                        trace("REMOVED from plain");
                        return;
                    }
                    trace("no match (no-op)");
                };
                for (const auto& u : OffBoardUnitsFor(a_entry ? a_entry->object : nullptr,
                                                     a_base)) {
                    takeOne(u.uid, u.sig, u.mayBeWorn, u.why, u.hand);
                }
            }

            // ORDER MATTERS: plain units come FIRST so their ordinals never
            // shift when a list appears or disappears. Emitting lists first was
            // what let a freshly tempered dagger take ordinal 0 and steal the
            // plain dagger's cell.
            for (int k = 0; k < plain; ++k) a_out.push_back({ 0, 0, -1 });
            for (const auto& in : insts) {
                for (int k = 0; k < in.units; ++k) {
                    a_out.push_back({ in.uid, in.sig, in.xlIdx, in.worn, in.hand });
                }
            }
        }

        // a_instanceKeys=false forces the historical ordinal keys. BAGS take that
        // path: a bag's tile key is referenced by every entry it holds
        // (LayoutEntry::bag), so re-keying one would orphan its contents. They
        // would recover -- E4 spills them back to main -- but silently moving a
        // player's bag contents to buy an instance binding no bag needs is a bad
        // trade.
        // a_mutate: only the display collector may drop a pool's surplus slots.
        // The capacity sims run the same walk read-only and must not touch
        // g_layout -- they only need the SAME keys the board will use.
        void EnumerateUnitTiles(const std::string& a_base, int a_count, int a_units,
                                RE::InventoryEntryData* a_entry,
                                std::vector<UnitTile>& a_out,
                                bool a_instanceKeys = true, bool a_mutate = false)
        {
            // Walk EVERY non-worn unit first: which ones to hide is a per-pool
            // question, and the walk cannot answer it (it has no keys).
            std::vector<UnitRef> refs;
            EnumerateUnitRefs(a_count, (std::numeric_limits<int>::max)(), a_entry, refs,
                              true, a_base);

            // group into pools, preserving order within each
            std::map<std::string, std::vector<UnitRef>> pools;
            for (const auto& r : refs) {
                pools[a_instanceKeys ? PoolPrefix(a_base, r.uid, r.sig) : a_base]
                    .push_back(r);
            }

            // GI22: hide the units that are on their way out -- from the pools
            // they actually left. Whatever the caller could not account for
            // (console removals) comes off the plain pool first, then the rest.
            int surplus = static_cast<int>(refs.size()) - a_units;
            std::map<std::string, int> hide;
            for (auto& [prefix, members] : pools) {
                if (surplus <= 0) break;
                const auto pi = g_pendingRemovePool.find(prefix);
                if (pi == g_pendingRemovePool.end()) continue;
                const int t = (std::min)({ surplus, pi->second,
                                           static_cast<int>(members.size()) });
                hide[prefix] += t;
                surplus -= t;
            }
            // L1/D4-b: an inactive preset holds ONE SPECIFIC unit. Take it from
            // the pool it actually belongs to, before the generic rule below --
            // which prefers the plain pool and so hid the plain dagger while the
            // preset was wearing the tempered one, swapping them on screen.
            if (surplus > 0 && a_instanceKeys && a_entry && a_entry->object) {
                for (const std::uint16_t rs :
                     Loadout::ReservedSigs(a_entry->object->GetFormID())) {
                    if (surplus <= 0) break;
                    const std::string prefix = PoolPrefix(a_base, 0, rs);
                    const auto pit = pools.find(prefix);
                    if (pit == pools.end()) continue;
                    if (static_cast<int>(pit->second.size()) - hide[prefix] <= 0) continue;
                    ++hide[prefix];
                    --surplus;
                }
            }

            if (surplus > 0) {   // leftovers: plain pool first
                for (int pass = 0; pass < 2 && surplus > 0; ++pass) {
                    for (auto& [prefix, members] : pools) {
                        if (surplus <= 0) break;
                        const bool plain = (prefix == a_base);
                        if ((pass == 0) != plain) continue;
                        const int room = static_cast<int>(members.size()) - hide[prefix];
                        const int t = (std::min)(surplus, (std::max)(0, room));
                        hide[prefix] += t;
                        surplus -= t;
                    }
                }
            }

            // Cells that belong to a unit IN TRANSIT. A unit that left the board
            // keeps its cell until the transition completes -- otherwise `want`
            // drops while the cell is still remembered, the survivors take
            // slots[0..want-1] in sort order, and the cell that ends up unused is
            // whichever sorted LAST rather than the one the player acted on. The
            // real cell then empties a frame or two later, when the applied equip
            // forgets it, so a sibling blinks off and back on. Holding the cell
            // makes the board show the right thing on the FIRST frame.
            std::set<std::string> inTransit;
            for (const auto& u : OffBoardUnitsFor(a_entry ? a_entry->object : nullptr,
                                                  a_base)) {
                if (!u.srcKey.empty()) inTransit.insert(u.srcKey);
            }

            for (auto& [prefix, members] : pools) {
                // the pool's remembered slots, ordered by where they ARE
                std::vector<std::pair<std::string, LayoutEntry>> slots;
                for (const auto& [k, le] : g_layout) {
                    if (PoolOfKey(k) != prefix) continue;
                    // a trash-parked tile is not a board cell: its unit is already
                    // in the off-board list, so counting its slot here would leave
                    // slots > units and hand a survivor somebody else's cell
                    if (le.bag == kTrashKey) continue;
                    // a carried tile takes its slot with it: excluding both the
                    // unit and the slot keeps the remaining ones where they are
                    // (and restores it on cancel). Same for a queued equip --
                    // both are named by inTransit above.
                    if (inTransit.contains(k)) continue;
                    slots.push_back({ k, le });
                }
                // Slots the pool ALREADY occupies come first, then the rest by
                // position. When there are more slots than units -- an inactive
                // preset holds one back, so its cell stays remembered while no
                // tile uses it -- plain position order handed the survivors the
                // TOPMOST cells and everything shifted up the moment the player
                // emptied a cell in the middle. A tile must only move when its
                // own unit left, so keep the cells that are in use and hold the
                // spare ones for the units that are off the board.
                std::sort(slots.begin(), slots.end(), [](const auto& x, const auto& y) {
                    const bool xLive = g_prevKeys.contains(x.first);
                    const bool yLive = g_prevKeys.contains(y.first);
                    if (xLive != yLive) return xLive;
                    // GI28: a STARRED slot sorts last, so it is the one that goes
                    // unused when the pool shrinks. The star is the player saying
                    // "this one" -- when the favourites menu equips a dagger, the
                    // cell that empties should be the one wearing the star, not
                    // whichever happened to sort last. Routed removals never reach
                    // this (they erase or withhold their own key first), so this
                    // only decides shrinks the engine did behind our back --
                    // exactly the Q-menu / hotkey case.
                    // GI30: a parked star has no cell yet -- it must not sort to
                    // the front and take a placed sibling's position
                    const bool xP = x.second.col < 0, yP = y.second.col < 0;
                    if (xP != yP) return !xP;
                    if (x.second.bag != y.second.bag) return x.second.bag < y.second.bag;
                    if (x.second.row != y.second.row) return x.second.row < y.second.row;
                    return x.second.col < y.second.col;
                });

                std::size_t want = members.size();
                if (const int h = hide[prefix]; h > 0) {
                    want -= (std::min)(want, static_cast<std::size_t>(h));
                }
                // NO carried/pending correction here any more: those units never
                // entered `members` in the first place (EnumerateUnitRefs). The
                // carried unit's SLOT is still held for it by the exclusion above,
                // so a cancel puts it back exactly where it was.

                if (a_mutate && g_poolTrace &&
                    (members.size() > 1 || !OffBoardUnitsFor(
                         a_entry ? a_entry->object : nullptr, a_base).empty())) {
                    std::string off;
                    for (const auto& u : OffBoardUnitsFor(
                             a_entry ? a_entry->object : nullptr, a_base)) {
                        off += std::format("{}(uid {:04X} sig {:04X}) ", u.why, u.uid, u.sig);
                    }
                    std::string cells;
                    for (const auto& sl : slots) {
                        cells += std::format("[{},{}{}{}] ", sl.second.col, sl.second.row,
                            sl.second.bag.empty() ? "" : "/",
                            sl.second.bag.empty() ? "" : sl.second.bag.c_str());
                    }
                    SKSE::log::info("[POOL] {} members={} slots={} want={} | off: {}| slots: {}",
                        prefix, members.size(), slots.size(), want,
                        off.empty() ? "- " : off, cells.empty() ? "-" : cells);
                }

                std::size_t made = 0;
                for (std::size_t i = 0; i < want; ++i) {
                    std::string key;
                    if (i < slots.size()) {
                        key = slots[i].first;
                    } else {
                        // fresh unit: lowest free ordinal inside THIS pool
                        for (int n = 0;; ++n) {
                            key = n == 0 ? prefix : prefix + "#" + std::to_string(n);
                            if (!g_layout.contains(key) && GoldCoins::PinnedValue(key) < 0 &&
                                !(g_held && key == g_held->key)) {
                                break;
                            }
                        }
                        if (a_mutate) g_layout[key] = LayoutEntry{ -1, -1, {}, 1 };
                    }
                    if (a_mutate && g_poolTrace) {
                        const auto le = g_layout.count(key) ? g_layout[key] : LayoutEntry{};
                        SKSE::log::info("[POOL]   -> '{}' at [{},{}]{}", key, le.col, le.row,
                            i < slots.size() ? "" : "  (NEW, first-fit)");
                    }
                    a_out.push_back({ key, members[made++].xlIdx });
                }
                // the pool shrank behind our back (console / script removal):
                // drop the trailing positions. A removal WE routed already
                // erased its own key, so this normally does nothing.
                //
                // NOT while this pool has something on the cursor. Lifting a worn
                // unit off the doll starts the carry a frame or more BEFORE the
                // unequip lands, so for those frames the unit is still worn and
                // absent from `members` while `want` has already been decremented
                // for it -- want(0) < slots(1). That made this cleanup erase the
                // remembered cell of an innocent SPARE in the same pool, which
                // then first-fit into the front gap on the next rebuild and could
                // not be put back by cancelling. The pool reconciles by itself
                // once the engine catches up; nothing needs erasing meanwhile.
                // ...but only for shrinkage WE did not cause. A unit we moved
                // off the board (cursor, queued equip) still owns its slot until
                // the transition completes, and pruning here erased whichever
                // slot happened to sort last -- an innocent sibling half the time.
                const bool ours = a_entry &&
                    !OffBoardUnitsFor(a_entry->object, a_base).empty();
                if (a_mutate && !ours) {
                    for (std::size_t j = want; j < slots.size(); ++j) {
                        // GI30: a starred slot is being held for a unit that is
                        // on the body. Pruning it is exactly the deletion this
                        // whole change exists to prevent.
                        g_layout.erase(slots[j].first);
                    }
                }
            }
        }

        struct CapTiles
        {
            std::vector<Item>     tiles;
            std::set<std::string> bagKeys;   // tile keys of PRESENT bag items
        };

        // Headless tile collection for the capacity sims (MaxAcceptUnits /
        // WouldOverflow / ComputeOverloaded — formerly three drifting copies).
        // Ordinal #k tiles of up-to-cap units (G4 per-tile counts stay a
        // display concern); saved spots from g_layout; overflow-zone spots are
        // TEMPORARY and never honoured; a bag FORM still owned anchors its
        // contents even while its tile is transiently absent, and dangling bag
        // refs are cleared here — the post-load false-overload fix, now shared
        // by all three sims instead of only ComputeOverloaded.
        void CollectCapacityTiles(CapTiles& a_out)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            std::set<std::string> bagForms;   // base keys of bag FORMS owned
            auto inv = player->GetInventory();
            for (auto& [obj, data] : inv) {
                const int count = data.first;
                auto* entry = data.second.get();
                if (!obj || count <= 0) continue;
                if (obj->IsGold()) continue;
                // record BEFORE any skip: worn/reserved bag forms still anchor
                if (g_resolver) {
                    if (const GridDef bd = g_resolver(obj); bd.bag != 0) {
                        bagForms.insert(FormKey(obj));
                    }
                }
                if (SkipInventoryEntry(obj, count)) continue;

                const GridDef gdef = g_resolver ? g_resolver(obj) : GridDef{};
                const int cap = EffectiveCap(obj, gdef);
                const bool worn = entry && entry->IsWorn();
                // The suppression exists only for the gap between requesting an
                // equip and the engine applying it. IsWorn() turning true IS the
                // engine applying it -- releasing on our own queue instead was a
                // frame or two early, and the pool stayed double-hidden (the last
                // spare blinked out) until the next rebuild.
                // Release only the units the engine has ACTUALLY put on. IsWorn()
                // is entry-level: with a copy already equipped it was true from the
                // start, so the suppression died in the same rebuild that armed it
                // and the queued unit showed a ghost tile until the engine caught up.
                ReleaseWornPendingEquips(FormKey(obj), entry);
                // Same worn-unit count as Rebuild -- the capacity gate has to
                // model the same board the player is looking at, or a stack with
                // one unit equipped reports free space that is not free.
                int wornUnits = 0;
                if (worn && entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                   xl->HasType<RE::ExtraWornLeft>())) {
                            wornUnits += (std::max)(1, xl->GetCount());
                        }
                    }
                }
                if (worn && wornUnits <= 0) wornUnits = 1;
                int units = count - wornUnits -
                            Loadout::ReservedCount(obj->GetFormID());
                // pending-drop pattern (mirrors Rebuild): units whose engine
                // removal is still queued leave the sim NOW — else the gates
                // model a fuller board than the display for a frame (pickup
                // wrongly bounced / buy clamp too small / overload flicker)
                if (const auto pit = g_pendingRemoveForm.find(obj->GetFormID());
                    pit != g_pendingRemoveForm.end()) {
                    units -= pit->second;
                }
                const std::string baseKey = FormKey(obj);
                // F2: units parked in the trash occupy no board space — the
                // deletion buffer must not count against capacity/overload
                units -= TrashedUnits(baseKey);
                if (units <= 0) continue;        // the single worn copy: doll only
                // GI1: cap-1 forms enumerate per UNIT through the shared
                // instance walker so the sim's keys match the board's exactly;
                // stackables keep the plain ordinal split (units ARE fungible).
                std::vector<UnitTile> unitKeys;
                if (cap <= 1) {
                    // P1: INT_MAX, not `units`. Every unit that should not be
                    // drawn is already absent from the walker's set, so there is
                    // no surplus left to reconstruct -- and the scalar `units`
                    // subtracted several of them a SECOND time, which cancelled
                    // the set removal and put the unit back on the board.
                    EnumerateUnitTiles(baseKey, count, (std::numeric_limits<int>::max)(),
                                       entry, unitKeys, gdef.bag == 0);
                } else {
                    const int tiles = (units + cap - 1) / cap;
                    for (int k = 0; k < tiles; ++k) unitKeys.push_back({ TileKey(baseKey, 0, 0, k), -1 });
                }
                for (const auto& uk : unitKeys) {
                    Item it;
                    it.key = uk.key;
                    it.uid = UidOf(uk.key);
                    it.sig = SigOf(uk.key);   // GI25
                    it.xlIdx = uk.xlIdx;
                    it.obj = obj;
                    it.def = gdef;
                    it.mask = MaskOf(it.def);
                    if (it.def.bag != 0) a_out.bagKeys.insert(it.key);
                    if (const auto li = g_layout.find(it.key); li != g_layout.end()) {
                        it.col = li->second.col;
                        it.row = li->second.row;
                        it.inBag = li->second.bag;
                        // F2: an ordinal key colliding with a PARKED tile's key
                        // must not import its trash assignment into the sim
                        if (it.inBag == kTrashKey) { it.inBag.clear(); it.col = -1; it.row = -1; }
                        if (it.inBag.empty() && it.row >= kMinRows) {   // overflow zone
                            it.col = -1;
                            it.row = -1;
                        }
                    }
                    if (it.def.bag != 0) it.inBag.clear();   // E3: bags live in main
                    a_out.tiles.push_back(std::move(it));
                }
            }
            // E4 mirror: a DANGLING bag ref (no tile AND no owned form) means
            // the contents reflow to main and stay eligible for the bag spill
            for (auto& it : a_out.tiles) {
                if (!it.inBag.empty() && !a_out.bagKeys.contains(it.inBag) &&
                    !bagForms.contains(BaseKey(it.inBag))) {
                    it.inBag.clear();
                }
            }
        }
    }

    // ---- Phase 3: Rebuild stages (bodies moved verbatim) ----
    namespace
    {
        // stage 1+2: walk the live inventory into display tiles — coin
        // mirror partition + G4 per-tile reconcile. Fills g_items and the
        // per-form caches (g_gold / g_values / g_stolen / g_questItem).
        void CollectDisplayTiles(RE::PlayerCharacter* player)
        {
            // ---- collect ----
            for (auto& [obj, pair] : player->GetInventory()) {
                const int count = pair.first;
                auto* entry = pair.second.get();
                if (!obj || count <= 0) continue;
                if (obj->IsGold()) { g_gold = count; continue; }
                if (SkipInventoryEntry(obj, count)) continue;   // shared filter (Phase 2)

                // Phase 4/5: cache the barter base value for this form. Coins are
                // excluded (mirror, not sellable) but the POUCH is a real sellable
                // item.
                // GI44: the FORM base, never entry->GetValue(). The entry-level
                // native folds in whichever list it fancies, so ONE tempered
                // dagger silently raised the cached base of every plain sibling
                // to 11 -- which is also where the old "Sell 4 vs vanilla 3"
                // mystery came from (the display showed the honest 10 while the
                // sale priced the polluted 11). Temper is folded in per UNIT by
                // UnitValueWith now; the base must stay variant-blind.
                if (entry && !(GoldCoins::IsCoinForm(obj->GetFormID()) &&
                               !GoldCoins::IsPouch(obj->GetFormID()))) {
                    g_values[obj->GetFormID()] = obj->GetGoldValue();
                    // Phase 6: stolen = not owned by the player (defaultTo=true so an
                    // unowned item counts as the player's = not stolen). Merchants
                    // that don't buy stolen refuse it (unless a fence).
                    //
                    // Per SUB-STACK: the engine stamps ownership on the list it
                    // moved, so only that unit is stolen. Asking the entry (which
                    // answers "does ANY list have a foreign owner") branded the
                    // whole form.
                    if (entry->extraLists) {
                        for (auto* xl : *entry->extraLists) {
                            if (!xl) continue;
                            auto* owner = xl->GetOwner();
                            if (!owner || entry->IsOwnedBy(player, owner, true)) continue;
                            std::uint16_t xuid = 0;
                            if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                                xuid = xu->uniqueID;
                            }
                            g_stolen[PoolPrefix(FormKey(obj), xuid, InstanceSig(xl))] = true;
                        }
                    }
                    // Phase 7: quest objects can't leave the inventory (drop/sell/
                    // store) — the engine API doesn't block it, so we must.
                    // per SUB-STACK, for the same reason as ownership above
                    if (entry->extraLists) {
                        for (auto* xl : *entry->extraLists) {
                            if (!xl || !xl->HasQuestObjectAlias()) continue;
                            std::uint16_t xuid = 0;
                            if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                                xuid = xu->uniqueID;
                            }
                            g_questItem[PoolPrefix(FormKey(obj), xuid, InstanceSig(xl))] = true;
                        }
                    }
                }

                // G3: Mabinogi stacking — a tile holds at most StackCapOf units;
                // overflow spills into extra tiles keyed "#k" (each keeps its own
                // saved spot). Cap-1 forms (gear/coins) get one tile per unit; a
                // worn copy sits on the doll while its spares stay on the board.
                const GridDef gdef = g_resolver ? g_resolver(obj) : GridDef{};
                const int cap = EffectiveCap(obj, gdef);
                const bool worn = entry && entry->IsWorn();
                // The suppression exists only for the gap between requesting an
                // equip and the engine applying it. IsWorn() turning true IS the
                // engine applying it -- releasing on our own queue instead was a
                // frame or two early, and the pool stayed double-hidden (the last
                // spare blinked out) until the next rebuild.
                // Release only the units the engine has ACTUALLY put on. IsWorn()
                // is entry-level: with a copy already equipped it was true from the
                // start, so the suppression died in the same rebuild that armed it
                // and the queued unit showed a ghost tile until the engine caught up.
                ReleaseWornPendingEquips(FormKey(obj), entry);
                // How many units the body is ACTUALLY wearing. IsWorn() is
                // entry-level -- one torch in hand made the whole FORM "worn", so
                // all three left the board and looked equipped together. Lifting
                // the one real torch off the doll then "released" the other two,
                // which is what "2 come back and only 1 is on the cursor" was.
                // Only ever one torch is lit; the other two were never equipped.
                int wornUnits = 0;
                if (worn && entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                   xl->HasType<RE::ExtraWornLeft>())) {
                            wornUnits += (std::max)(1, xl->GetCount());
                        }
                    }
                }
                if (worn && wornUnits <= 0) wornUnits = 1;   // worn but unlisted
                int units = count - wornUnits -
                            Loadout::ReservedCount(obj->GetFormID());
                // Phase 7: units sold/stored whose engine removal is still queued
                // leave the board NOW (pending-drop pattern) — else the interim
                // rebuild re-seats them as a fresh pickup at the front.
                if (const auto pit = g_pendingRemoveForm.find(obj->GetFormID());
                    pit != g_pendingRemoveForm.end()) {
                    units -= pit->second;
                }
                // ---- units in transit (stackables) -----------------------------
                // This branch knew only about `carried`, `reserved` and queued
                // removals -- none of the mid-transition machinery the gear branch
                // has. Two blinks came straight out of that:
                //
                //   equip   the unit went back onto the board for the frame or two
                //           between asking and the engine doing it
                //   unlift  a stackable lifted off the doll was subtracted TWICE,
                //           once as worn and once as carried, until the unequip
                //           landed -- so the stack dipped by one and came back
                //
                // Both are the same rule: an off-board unit is subtracted unless a
                // worn list is already accounting for it.
                int wornFree = wornUnits;
                const std::string baseKey = FormKey(obj);
                int carried = 0;
                // (fromPartner excluded for the same reason as the gear branch:
                // the container's item on the cursor is not one of ours)
                if (g_held && g_held->obj && !g_held->fromPartner &&
                    FormKey(g_held->obj) == baseKey) {
                    carried = g_held->count;
                    if (g_held->fromDoll && wornFree > 0 && carried > 0) {
                        --wornFree;      // the carry IS one of the worn units
                        --carried;       // ...so it is already out of `units`
                    }
                }
                for (const auto& u : g_pendingEquip) {
                    if (u.base != baseKey) continue;
                    if (wornFree > 0) --wornFree;   // the engine already took it
                    else --units;                   // still in the pack: hide it now
                }
                if (units <= 0) continue;        // the single worn copy: doll only
                const int tiles = (units + cap - 1) / cap;

                // rarity glow: enchanted (EITM / player-crafted) and unique (DESC).
                // GI1/D2: the base-form part is per FORM; the crafted-enchant bit
                // is per SUB-STACK and is added in makeTile from that tile's own
                // list. Folding it in here is what made every copy of a form glow
                // because one of them was enchanted.
                std::uint8_t glow = 0;
                if (const auto* ef = obj->As<RE::TESEnchantableForm>();
                    ef && ef->formEnchanting) {
                    glow |= 1;
                }
                if (!obj->Is(RE::FormType::Book) && HasDescCached(obj)) glow |= 2;

                // COIN tiles: a coin's VALUE is bound to its ordinal (InstanceValue:
                // low index = 1000, top index = remainder). If the value stayed
                // keyed to the tile index while the position is free-placed, a drop
                // would remove the wrong-LOOKING tile (the reconciler re-lays value
                // by index, not by where the user put it). Fix: sort THIS form's
                // saved slots by grid position and re-key #0,#1,... in that order,
                // so ordinal == visual order (front cell = 1000, last cell =
                // remainder). A drop erases exactly its slot (below), and the next
                // rebuild re-maps the survivors by position — the emptied cell is
                // the one the user dropped, never a shuffled neighbour.
                // coins ONLY — the pouch also passes IsCoinForm but is a normal
                // 2x2 bag tile (no tier), so it must take the generic path.
                if (GoldCoins::IsCoinForm(obj->GetFormID()) &&
                    !GoldCoins::IsPouch(obj->GetFormID())) {
                    const RE::FormID cfid = obj->GetFormID();
                    // Partition this form's saved slots: PINNED purses (G4, fixed
                    // key/value/position) vs AUTO tiles (walking gold, re-keyed by
                    // position with an ordinal-bound value).
                    std::vector<std::pair<std::string, LayoutEntry>> pinnedSlots;
                    std::vector<LayoutEntry> autoSlots;
                    for (auto& [k, v] : g_layout) {
                        if (BaseKey(k) != baseKey) continue;
                        if (v.bag.empty() && v.row >= kMinRows) continue;   // overflow = temporary
                        if (GoldCoins::PinnedValue(k) >= 0) pinnedSlots.push_back({ k, v });
                        else                                 autoSlots.push_back(v);
                    }
                    auto byPos = [](const LayoutEntry& a, const LayoutEntry& b) {
                        if (a.bag != b.bag) return a.bag < b.bag;
                        if (a.row != b.row) return a.row < b.row;
                        return a.col < b.col;
                    };
                    std::sort(autoSlots.begin(), autoSlots.end(), byPos);

                    // wipe only this form's AUTO keys (pinned keys keep their spot)
                    for (auto li = g_layout.begin(); li != g_layout.end();) {
                        if (BaseKey(li->first) == baseKey && GoldCoins::PinnedValue(li->first) < 0)
                            li = g_layout.erase(li);
                        else ++li;
                    }

                    auto emitCoin = [&](const std::string& key, int value,
                                        const LayoutEntry* pos) {
                        if (g_held && key == g_held->key) return;   // carried: cell stays free
                        Item it;
                        it.key = key;
                        it.obj = obj;
                        it.glow = glow;
                        it.count = 1;   // coins: one unit per tile (never favoritable)
                        it.def = gdef;
                        it.mask = MaskOf(it.def);
                        it.coinValue = value;
                        if (pos) {
                            it.col = pos->col;
                            it.row = pos->row;
                            it.inBag = pos->bag;
                            g_layout[key] = *pos;
                        }
                        g_items.push_back(std::move(it));
                    };

                    // 1) pinned purses — fixed value & position
                    for (auto& [k, le] : pinnedSlots) {
                        emitCoin(k, GoldCoins::PinnedValue(k), &le);
                    }

                    // 2) auto tiles from WALKING gold (pending drops subtracted).
                    // Re-key #0.. by position, skipping keys owned by a pin, so the
                    // ordinal (= value index) stays dense while pins keep their key.
                    const int coinTiles = GoldCoins::CoinTileCount(cfid);
                    int probe = 0;
                    for (int rank = 0; rank < coinTiles; ++rank) {
                        std::string key;
                        for (;;) {   // next free key not owned by a pin
                            key = probe == 0 ? baseKey : baseKey + "#" + std::to_string(probe);
                            ++probe;
                            if (GoldCoins::PinnedValue(key) < 0) break;
                        }
                        const int value = GoldCoins::InstanceValue(cfid, rank);
                        emitCoin(key, value, rank < static_cast<int>(autoSlots.size())
                                                 ? &autoSlots[rank] : nullptr);
                    }
                    continue;   // coins handled — skip the generic tile loop
                }

                // emit one tile Item at a saved (or fresh, col<0) spot
                auto makeTile = [&](const std::string& key, int cnt, int col, int row,
                                    const std::string& bag, int xlIdx = -1) {
                    Item it;
                    it.key = key;
                    it.obj = obj;
                    it.glow = glow;
                    it.count = cnt;
                    it.def = gdef;
                    it.mask = MaskOf(it.def);
                    it.col = col;
                    it.row = row;
                    it.inBag = bag;
                    it.uid = UidOf(key);   // GI1: derived, never passed separately
                    it.sig = SigOf(key);   // GI25
                    it.xlIdx = xlIdx;
                    // D2: the crafted-enchant glow belongs to THIS unit
                    if (const auto* xl = ExtraForTile(entry, it.uid, xlIdx)) {
                        if (const auto* xe = xl->GetByType<RE::ExtraEnchantment>();
                            xe && xe->enchantment) {
                            it.glow |= 1;
                        }
                        // GI46: unit status rings (poison green / temper steel)
                        if (const auto* xp = xl->GetByType<RE::ExtraPoison>();
                            xp && xp->poison) {
                            it.glow |= 4;
                        }
                        if (const auto* xh = xl->GetByType<RE::ExtraHealth>();
                            xh && xh->health > 1.0f) {
                            it.glow |= 8;
                        }
                    }
                    // GI40: the star belongs to the POOL, not to one list.
                    //
                    // Asking a single ExtraDataList was wrong in both directions.
                    // Entry-level (vanilla's test) starred every copy of the form,
                    // tempered ones included. List-level lost the star the moment
                    // anything split the pool -- equipping one dagger moves the
                    // ExtraHotkey onto the worn list, and all the spares standing
                    // in the bag went dark even though nothing was unfavourited.
                    //
                    // The pool is the right unit: it is exactly "the identical
                    // things", which is what the engine can mark and what rule 53
                    // promises. Tempered and plain stay separate (rule 54) because
                    // they are separate pools.
                    it.fav = PoolHasStar(entry, it.uid, it.sig);
                    // overflow-zone spots (rows past the hard board) are TEMPORARY
                    // — never honour them, so the item first-fits back INTO the
                    // board the moment space frees up and the extra rows collapse.
                    if (it.inBag.empty() && it.row >= kMinRows) { it.col = -1; it.row = -1; }
                    // E3: bags live in main — except a parked (empty) bag in
                    // the trash (F2 allows trashing an empty bag)
                    if (it.def.bag != 0 && it.inBag != kTrashKey) it.inBag.clear();
                    g_items.push_back(std::move(it));
                };

                if (cap <= 1) {
                    // GI1/GI2: gear / non-stackable — one tile per UNIT, each
                    // bound to the engine sub-stack it actually shows.
                    std::vector<UnitTile> units_v;
                    EnumerateUnitTiles(baseKey, count,   // P1: see the sim's note
                                       (std::numeric_limits<int>::max)(), entry, units_v,
                                       gdef.bag == 0, /*mutate=*/true);
                    // ---- self-check: CONSERVATION ----------------------------
                    // Every unit the engine says the player owns has to be in
                    // exactly one place: drawn on the board, worn, or named in the
                    // off-board set. If the three do not add up to the engine's
                    // count, something was removed twice or not at all -- which is
                    // what "a spare vanished", "it is on the cursor AND the board"
                    // and "it flickers" all look like from the inside. Reporting
                    // the mismatch here means the log identifies the fault without
                    // anyone having to describe the symptom.
                    if (g_poolTrace) {
                        int wornN = 0;
                        if (entry && entry->extraLists) {
                            for (auto* xl : *entry->extraLists) {
                                if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                           xl->HasType<RE::ExtraWornLeft>())) {
                                    wornN += (std::max)(1, xl->GetCount());
                                }
                            }
                        }
                        const auto offv = OffBoardUnitsFor(obj, baseKey);
                        int drawn = 0;
                        for (const auto& u : units_v) {
                            drawn += (std::max)(1, g_layout.count(u.key)
                                                       ? g_layout[u.key].count : 1);
                        }
                        // An off-board unit that is STILL WORN in the engine is
                        // already inside wornN -- lifting a worn item off the doll
                        // starts the carry before the unequip lands, and a queued
                        // equip is worn before the entry clears. Counting it in
                        // both places reported a phantom -1 on every carry.
                        // STRICT match only. WornExtraMatching falls back to "any
                        // worn list of this form" so the doll never shows a blank,
                        // which is right for display and wrong for counting: a
                        // carried TEMPERED dagger matched the PLAIN one on the
                        // body, was written off as already-worn, and the check
                        // reported a unit missing that was never missing.
                        const auto wornBacked = [&](const OffBoardUnit& o) {
                            if (!entry || !entry->extraLists) return false;
                            for (auto* xl : *entry->extraLists) {
                                if (!xl) continue;
                                const bool L = xl->HasType<RE::ExtraWornLeft>();
                                const bool R = xl->HasType<RE::ExtraWorn>();
                                if (!L && !R) continue;
                                if (o.hand == 1 && !R) continue;
                                if (o.hand == 2 && !L) continue;
                                std::uint16_t u = 0;
                                if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                                    u = xu->uniqueID;
                                }
                                if (u == o.uid && InstanceSig(xl) == o.sig) return true;
                            }
                            return false;
                        };
                        std::string why;
                        int offLoose = 0, wornTaken = 0;
                        for (const auto& o : offv) {
                            why += std::string(o.why) + " ";
                            // an ARRIVING unit only counts as worn once its equip
                            // has actually run -- the queue says so, identity cannot
                            if (o.mayBeWorn && !(o.arriving && !o.applied) &&
                                wornTaken < wornN && wornBacked(o)) {
                                ++wornTaken;
                                continue;
                            }
                            ++offLoose;
                        }
                        if (why.empty()) why = "-";
                        const int total = drawn + wornN + offLoose;
                        if (total != count) {
                            SKSE::log::warn("[CHECK] {} MISMATCH engine={} drawn={} worn={} "
                                            "offboard={} ({}) -> {} units unaccounted",
                                baseKey, count, drawn, wornN, offv.size(),
                                why, count - total);
                        }
                        // ---- self-check: FLICKER ---------------------------------
                        // A settled board rebuilds to the SAME drawn set every frame.
                        // Logging only the transitions keeps this quiet: a clean
                        // right-click equip is one line ("3 -> 2"), while a flicker
                        // is a pair that comes straight back ("3 -> 2" then "2 -> 3"
                        // with no user action between them). The conservation check
                        // above cannot see this -- it only ever looks at one frame.
                        {
                            std::vector<std::string> ks;
                            ks.reserve(units_v.size());
                            for (const auto& u : units_v) ks.push_back(u.key);
                            std::sort(ks.begin(), ks.end());
                            std::string sig = std::to_string(drawn);
                            for (const auto& k : ks) { sig += ' '; sig += k; }
                            auto& prev = g_flickPrev[baseKey];
                            if (prev != sig) {
                                SKSE::log::info("[FLICK] {} drawn {} -> {} (engine={} "
                                                "worn={} off={} [{}])  {}",
                                    baseKey,
                                    prev.empty() ? std::string("-")
                                                 : prev.substr(0, prev.find(' ')),
                                    drawn, count, wornN, offv.size(), why, sig);
                                prev = sig;
                            }
                        }
                        // two tiles on one cell is always wrong
                        std::set<std::pair<int, int>> cells;
                        for (const auto& u : units_v) {
                            const auto li = g_layout.find(u.key);
                            if (li == g_layout.end() || li->second.col < 0) continue;
                            if (!cells.insert({ li->second.col, li->second.row }).second) {
                                SKSE::log::warn("[CHECK] {} OVERLAP at [{},{}] key '{}'",
                                    baseKey, li->second.col, li->second.row, u.key);
                            }
                        }
                    }

                    // F2: a trash-parked GEAR tile is deliberately not a board
                    // cell -- the pool skips its slot and its unit is already in
                    // the off-board set -- but it still has to be DRAWN, in the
                    // trash view. Nothing emitted one, so binning a dagger made it
                    // vanish outright: absent from the grid, absent from the bin,
                    // and "restored" only because closing the trash cleared the
                    // bag off a layout entry that had never been shown. (The
                    // stackable branch never had this hole: its slot loop keeps
                    // trash entries and only skips them when choosing what to
                    // fill or drain.)
                    std::vector<std::string> parked;
                    for (const auto& [k, le] : g_layout) {
                        if (le.bag == kTrashKey && BaseKey(k) == baseKey) parked.push_back(k);
                    }
                    for (const auto& k : parked) {
                        const auto& le = g_layout[k];
                        makeTile(k, (std::max)(1, le.count), le.col, le.row, kTrashKey, -1);
                    }

                    for (const auto& u : units_v) {
                        LayoutEntry le;
                        if (const auto li = g_layout.find(u.key); li != g_layout.end()) le = li->second;

                        // B2: partner-drop hint — a NEW gear tile lands at the
                        // drop cell too (the stackable branch below already
                        // consumes it; without this, looted gear ignored the
                        // drop position and first-fitted into any free cell).
                        //
                        // GI21: the test is "has no position yet", NOT "has no
                        // entry". Pool assignment now creates a placeholder entry
                        // (col -1) for a fresh unit so two arrivals in one rebuild
                        // cannot claim the same key -- which made the old
                        // entry-missing test permanently false, and every dragged
                        // loot silently first-fit into the front gap instead of
                        // landing where it was dropped.
                        if (le.col < 0 && g_dropHint.col >= 0 &&
                            g_dropHint.baseKey == baseKey) {
                            le.col = g_dropHint.col;
                            le.row = g_dropHint.row;
                            le.bag = g_dropHint.bag;
                            g_layout[u.key] = le;   // persist the placement
                            g_dropHint = {};
                        }
                        makeTile(u.key, 1, le.col, le.row, le.bag, u.xlIdx);
                    }
                    continue;   // form handled
                }

                // ---- G4: explicit per-tile counts (Mabinogi split/merge) ----
                // A tile OWNS its quantity; the reconciler only closes the gap
                // between the saved sum and the engine's live count. The carried
                // tile/fragment of THIS form is excluded from placement and its
                // units are represented by g_held->count instead — so splitting a
                // stack never looks like a shrink that gets re-absorbed.
                struct Slot { std::string key; LayoutEntry le; };
                std::vector<Slot> slots;
                for (auto& [k, v] : g_layout) {
                    if (BaseKey(k) != baseKey) continue;
                    if (g_held && k == g_held->key) continue;   // carried whole tile
                    slots.push_back({ k, v });
                }
                std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) {
                    if (a.le.bag != b.le.bag) return a.le.bag < b.le.bag;
                    if (a.le.row != b.le.row) return a.le.row < b.le.row;   // top-left order
                    return a.le.col < b.le.col;
                });

                // `carried` was already reduced above by whatever the worn count
                // is still accounting for -- subtracting the raw carry here took
                // a doll-lifted unit out twice.
                int placeUnits = (std::max)(0, units - carried);

                int owned = 0;
                for (auto& s : slots) owned += (std::max)(0, s.le.count);

                int diff = placeUnits - owned;
                if (diff > 0) {
                    // ACQUIRE: fill partial tiles top-left, then spill into new tiles
                    for (auto& s : slots) {
                        if (diff <= 0) break;
                        // F2: never absorb fresh pickups into a tile parked in
                        // the trash (it's queued for deletion, not storage)
                        if (s.le.bag == kTrashKey) continue;
                        const int room = cap - (std::max)(0, s.le.count);
                        if (room <= 0) continue;
                        const int add = (std::min)(room, diff);
                        s.le.count = (std::max)(0, s.le.count) + add;
                        diff -= add;
                    }
                    while (diff > 0) {
                        const int cnt = (std::min)(cap, diff);
                        Slot ns;
                        ns.key = NextTileKey(baseKey);
                        ns.le.col = -1; ns.le.row = -1; ns.le.count = cnt;
                        // B2: partner-drop hint — the first NEW tile of this form
                        // lands at the drop cell (one-shot, then back to first-fit)
                        if (g_dropHint.col >= 0 && g_dropHint.baseKey == baseKey) {
                            ns.le.col = g_dropHint.col;
                            ns.le.row = g_dropHint.row;
                            ns.le.bag = g_dropHint.bag;
                            g_dropHint = {};
                        }
                        g_layout[ns.key] = ns.le;   // reserve so NextTileKey advances
                        slots.push_back(std::move(ns));
                        diff -= cnt;
                    }
                } else if (diff < 0) {
                    // CONSUME: drain partial tiles first (bottom-right), then full
                    // ones; tiles PARKED in the trash (F2) drain dead last — an
                    // outside consumption must not eat the deletion buffer.
                    int deficit = -diff;
                    // The tile the player acted on gives up its unit FIRST (rule
                    // 2-B). Only then do the positional passes below run.
                    if (deficit > 0 && g_drainHint.baseKey == baseKey) {
                        for (auto& s : slots) {
                            if (s.key != g_drainHint.key || s.le.count <= 0) continue;
                            const int take = (std::min)(s.le.count, deficit);
                            s.le.count -= take;
                            deficit -= take;
                            break;
                        }
                        g_drainHint = {};
                    }
                    // (explicit sells/stores never reach this drain: their tile's
                    // remembered quantity is decremented at confirm time in
                    // NotePendingRemove, so the reconciler sees no gap)
                    auto drain = [&](auto&& a_pick) {
                        for (auto it2 = slots.rbegin(); it2 != slots.rend() && deficit > 0; ++it2) {
                            if (!a_pick(*it2)) continue;
                            const int take = (std::min)(it2->le.count, deficit);
                            it2->le.count -= take;
                            deficit -= take;
                        }
                    };
                    drain([&](const Slot& s) {
                        return s.le.bag != kTrashKey && s.le.count > 0 && s.le.count < cap;
                    });
                    drain([&](const Slot& s) { return s.le.bag != kTrashKey && s.le.count > 0; });
                    drain([&](const Slot& s) { return s.le.count > 0; });   // trash: last resort
                }

                // emit survivors; purge emptied tiles from the layout
                for (auto& s : slots) {
                    if (s.le.count <= 0) { g_layout.erase(s.key); continue; }
                    g_layout[s.key].count = s.le.count;   // persist owned count now
                    makeTile(s.key, s.le.count, s.le.col, s.le.row, s.le.bag);
                }

                // ---- self-check: STACKABLES ------------------------------------
                // This branch had no instrumentation at all, so a torch -- which
                // stacks, and therefore never reached the gear checks -- produced
                // no evidence whatsoever and had to be judged by eye.
                if (g_poolTrace) {
                    int drawn = 0;
                    std::string cells;
                    for (const auto& s : slots) {
                        if (s.le.count <= 0) continue;
                        drawn += s.le.count;
                        cells += std::format("{}x[{},{}] ", s.le.count, s.le.col, s.le.row);
                    }
                    const int wornN = wornUnits;
                    const int rsv = Loadout::ReservedCount(obj->GetFormID());
                    int rm = 0;
                    if (const auto p = g_pendingRemoveForm.find(obj->GetFormID());
                        p != g_pendingRemoveForm.end()) {
                        rm = p->second;
                    }
                    const int total = drawn + wornN + rsv + rm + carried;
                    if (total != count) {
                        SKSE::log::warn("[CHECK] {} STACK MISMATCH engine={} drawn={} "
                                        "worn={} reserved={} removing={} carried={} "
                                        "-> {} unaccounted",
                            baseKey, count, drawn, wornN, rsv, rm, carried,
                            count - total);
                    }
                    std::string sig = std::to_string(drawn) + " " + cells;
                    auto& prev = g_flickPrev[baseKey];
                    if (prev != sig) {
                        SKSE::log::info("[FLICK] {} stack {} -> {} (engine={} worn={} "
                                        "carried={})  {}",
                            baseKey, prev.empty() ? std::string("-")
                                                  : prev.substr(0, prev.find(' ')),
                            drawn, count, wornN, carried, sig);
                        prev = sig;
                    }
                }
            }
        }

        // stage 3: bag existence bookkeeping (E3/E4) — returns the map of
        // present bag tiles (key -> g_items index) for the view builder
        std::map<std::string, int> ReconcileBagBookkeeping()
        {
            // ---- bag bookkeeping ----
            // present bags (key -> index); stale open entries and orphaned inBag
            // assignments fall back gracefully (E4)
            std::map<std::string, int> bags;
            for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
                if (g_items[i].def.bag != 0) bags[g_items[i].key] = i;
            }
            for (auto it = g_openBags.begin(); it != g_openBags.end();) {
                if (!bags.contains(*it)) it = g_openBags.erase(it);
                else ++it;
            }
            for (auto& it : g_items) {
                if (it.inBag == kTrashKey) continue;   // F2: virtual bag, no tile
                if (!it.inBag.empty() && !bags.contains(it.inBag)) {
                    // A CARRIED bag isn't "gone" — keep its contents hidden inside
                    // it (inBag stays set -> excluded from main + no bag window),
                    // so they reflow to main only on a real drop/sell, not while
                    // the bag rides the cursor.
                    if (!(g_held && g_held->key == it.inBag)) {
                        it.inBag.clear();   // bag truly gone: contents reflow (E4)
                    }
                }
            }
            return bags;
        }

        // B: purchase-payment spill accounting — 1x1 dummies re-fill the
        // coin cells the payment dissolved this frame
        std::vector<Item> MakePaidGoldDummies()
        {
            // ---- B: consume this frame's purchase payment (spill accounting) ----
            // A barter payment already left the ledger, so the coin mirror shows
            // fewer tiles now. Re-fill the dissolved coin cells with 1x1 dummies so
            // a bought item is judged against the PRE-payment board (else it lands
            // in the very cells the gold just vacated instead of spilling to a bag).
            const int paidGold = g_paidGold;
            g_paidGold = 0;
            std::vector<Item> dummies;
            if (paidGold > 0 && !g_openBags.empty()) {
                const int walking = GoldCoins::WalkingGoldValue();
                const int n = (std::max)(0,
                    GoldCoins::CoinTilesFor(walking + paidGold) - GoldCoins::CoinTilesFor(walking));
                dummies.reserve(n);
                for (int i = 0; i < n; ++i) {
                    Item d;
                    d.key = "##paid" + std::to_string(i);
                    d.def = GridDef{};   // 1x1
                    d.mask = MaskOf(d.def);
                    dummies.push_back(std::move(d));
                }
            }
            return dummies;
        }

        // stage 4: bag views -> main list -> overflow spill into open bags
        // -> main view (placement is FINAL here)
        void BuildViewsAndSpill(std::map<std::string, int>& bags,
                                std::vector<Item>& dummies)
        {
            // ---- views: open bags first (their overflow falls back to main) ----
            std::vector<Item*> mainList;
            for (const auto& bagKey : g_openBags) {
                const auto& bagItem = g_items[bags[bagKey]];
                View v;
                v.bagKey = bagKey;
                v.bagName = bagItem.obj->GetName();
                v.cols = (std::max)(1, bagItem.def.bw);
                v.minRows = (std::max)(1, bagItem.def.bh);
                v.maxRows = v.minRows;   // fixed-height grid (B1/E5)
                std::vector<Item*> list;
                for (int i = 0; i < static_cast<int>(g_items.size()); ++i) {
                    if (g_items[i].inBag == bagKey) list.push_back(&g_items[i]);
                }
                v.rows = PlaceItems(list, v.cols, v.minRows, v.maxRows);
                for (auto* it : list) {
                    if (it->overflow) {   // bag full/shrunk: falls back to main (E4)
                        it->col = -1;
                        it->row = -1;
                        it->inBag.clear();
                    } else {
                        v.items.push_back(static_cast<int>(it - g_items.data()));
                    }
                }
                g_views.push_back(std::move(v));
            }

            // F2: the trash is one more grid view — a fixed 6x4 virtual bag.
            // Parked tiles were assigned bag == kTrashKey at drop time.
            if (g_trashOpen) {
                View v;
                v.bagKey = kTrashKey;
                v.bagName = Lang::T(Lang::Str::TrashTitle);
                v.cols = kTrashCols;
                v.minRows = kTrashRows;
                v.maxRows = kTrashRows;
                std::vector<Item*> list;
                for (auto& it : g_items) {
                    if (it.inBag == kTrashKey) list.push_back(&it);
                }
                v.rows = PlaceItems(list, v.cols, v.minRows, v.maxRows);
                for (auto* it : list) {
                    if (it->overflow) {   // shouldn't happen (intake evicts) — reflow
                        it->col = -1;
                        it->row = -1;
                        it->inBag.clear();
                    } else {
                        v.items.push_back(static_cast<int>(it - g_items.data()));
                    }
                }
                g_views.push_back(std::move(v));
            }

            // main list: unassigned items + bag-overflow fallbacks. Contents of
            // CLOSED bags are fully hidden (E3) — they keep their entries.
            for (auto& it : g_items) {
                if (it.inBag.empty()) mainList.push_back(&it);
            }

            // ---- B: spill main-overflow items into the open bag views ----
            // The placement here is FINAL — committed straight into a bag view — so
            // the "does it fit a bag?" verdict can't disagree with a later
            // re-placement (the old split pre-assigned inBag, then the view loop
            // re-placed and bounced it back to main). Fresh buys/loot drain into
            // bag space; coins (gold ledger) and bag items (no nesting) never spill.
            if (!g_openBags.empty()) {
                std::vector<Item*> probe;
                probe.reserve(dummies.size() + mainList.size());
                for (auto& d : dummies) probe.push_back(&d);   // freed coin cells first
                for (auto* it : mainList) probe.push_back(it);
                PlaceItems(probe, kCols, kMinRows, kMinRows);   // hard board, no growth
                for (auto* cand : probe) {
                    // real items only (dummies have no obj); coins keep the ledger
                    if (!(cand->obj && cand->overflow && cand->coinValue < 0)) continue;
                    for (auto& v : g_views) {   // g_views holds only bag views here
                        if (v.bagKey.empty()) continue;
                        if (v.bagKey == kTrashKey) continue;   // F2: never spill INTO the trash
                        std::vector<Item*> test;
                        test.reserve(v.items.size() + 1);
                        for (int idx : v.items) test.push_back(&g_items[idx]);
                        cand->col = -1;
                        cand->row = -1;
                        test.push_back(cand);
                        const int rows = PlaceItems(test, v.cols, v.minRows, v.maxRows);
                        if (!cand->overflow) {
                            cand->inBag = v.bagKey;
                            v.items.push_back(static_cast<int>(cand - g_items.data()));
                            v.rows = rows;
                            break;   // committed into this bag
                        }
                    }
                }
                mainList.erase(std::remove_if(mainList.begin(), mainList.end(),
                    [](Item* it) { return !it->inBag.empty(); }), mainList.end());
            }

            View main;
            main.bagKey.clear();
            std::vector<Item*> mainPtrs = mainList;
            main.rows = PlaceItems(mainPtrs, kCols, kMinRows, 4096);
            for (auto* it : mainPtrs) {
                if (it->overflow || it->col < 0) continue;
                main.items.push_back(static_cast<int>(it - g_items.data()));
            }
            g_views.insert(g_views.begin(), std::move(main));
        }

        // GI1: an instance tile's key IS the engine's uniqueID, and the engine is
        // free to reassign that when an item changes container. If it does, every
        // move strands the old placement and g_layout -- which is serialised into
        // every single save -- grows for the rest of the playthrough.
        //
        // Keep a small stale budget per FORM, so the ordinary round trip (put the
        // sword in a chest, take it back, land on its own cell again) still works
        // and only genuine churn is collected. Equipped items are "stale" by this
        // test too, which is correct: one entry each, well inside the budget.
        //
        // The log line here is also the only DIRECT measurement of whether the
        // engine churns uniqueIDs at all (Phase 0 gate E7) -- if it never fires
        // across a playthrough, it doesn't.
        constexpr std::size_t kStaleInstanceBudget = 8;

        void PruneStaleInstanceLayouts()
        {
            std::unordered_map<std::string, std::vector<std::string>> staleByForm;
            for (const auto& [k, le] : g_layout) {
                if (!IsInstanceKey(k)) continue;        // ordinal tiles: untouched
                if (g_prevKeys.contains(k)) continue;   // on the board right now
                if (le.bag == kTrashKey) continue;      // parked for deletion, still ours
                staleByForm[BaseKey(k)].push_back(k);
            }
            int pruned = 0;
            for (auto& [form, keys] : staleByForm) {
                if (keys.size() <= kStaleInstanceBudget) continue;
                std::sort(keys.begin(), keys.end());   // deterministic: by uid hex
                for (std::size_t i = 0; i + kStaleInstanceBudget < keys.size(); ++i) {
                    g_layout.erase(keys[i]);
                    ++pruned;
                }
            }
            if (pruned > 0) {
                SKSE::log::info("[GRID] GI1: pruned {} stale instance placements "
                                "(uniqueID churn — see gate E7)", pruned);
            }
        }

        // stage 5: persist placements (cosave source of truth), prevKeys,
        // lazy icon captures, occupancy stat
        void FinalizeRebuild()
        {

            // ---- remember fresh placements (B4) — in-memory; cosave persists ----
            for (const auto& v : g_views) {
                for (int idx : v.items) {
                    const auto& it = g_items[idx];
                    auto& le = g_layout[it.key];
                    le.col = it.col;
                    le.row = it.row;
                    le.bag = it.inBag;
                    le.count = it.count;   // G4: keep owned count in sync with placement
                }
            }

            // An applied entry has covered the one rebuild it existed for.
            std::erase_if(g_pendingEquip, [](const OffBoardUnit& u) { return u.applied; });
            g_prevKeys.clear();
            for (const auto& v : g_views) {
                for (int idx : v.items) g_prevKeys.insert(g_items[idx].key);
            }

            PruneStaleInstanceLayouts();   // GI1

            for (const auto& it : g_items) {
                IconCache::GetSingleton()->QueueCapture(it.obj);
            }

            // S2: cells occupied on the main board (growth rows included, so the
            // used value can exceed the total while overloaded — e.g. 147 / 140)
            g_spaceUsed = 0;
            if (!g_views.empty()) {
                for (int idx : g_views.front().items) {
                    g_spaceUsed += MaskCells(g_items[idx].mask.rows);
                }
            }

            SKSE::log::info("[GRID] rebuilt: {} items, {} views, gold {}",
                g_items.size(), g_views.size(), g_gold);
            g_capacityDirty = true;   // occupancy changed (W2)
        }
    }

    void Rebuild()
    {
        // F2 safety net: with the trash CLOSED no layout entry may stay
        // assigned to it (a crash / mid-menu save could persist one via the
        // cosave; the item would render nowhere). Reflow them to first-fit.
        if (!g_trashOpen) {
            for (auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey) { le.bag.clear(); le.col = -1; le.row = -1; }
            }
            g_trashOrder.clear();
            g_trashReturn.clear();
        } else {
            // FIFO bookkeeping stays in sync with the layout: drop keys that
            // left the trash (restored / deleted), append ones that appeared
            // (a carried tile returning after a cancelled re-drop).
            for (auto it = g_trashOrder.begin(); it != g_trashOrder.end();) {
                const auto li = g_layout.find(*it);
                if (li == g_layout.end() || li->second.bag != kTrashKey) {
                    g_trashReturn.erase(*it);
                    it = g_trashOrder.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto& [k, le] : g_layout) {
                if (le.bag == kTrashKey &&
                    std::find(g_trashOrder.begin(), g_trashOrder.end(), k) ==
                        g_trashOrder.end()) {
                    g_trashOrder.push_back(k);
                }
            }
        }

        g_items.clear();
        g_views.clear();
        g_gold = 0;
        g_values.clear();   // Phase 4: rebuilt below from live inventory entries
        g_stolen.clear();   // Phase 6: form -> stolen (not owned by player)
        g_questItem.clear();// Phase 7: form -> quest object

        // B3: expire pending removals whose engine transfer never landed —
        // without this a failed RemoveItem left the form permanently
        // under-counted (tile invisible until the menu closed)
        if (!g_pendingEquip.empty() &&
            std::chrono::steady_clock::now() - g_pendingEquipWhen > kPendingEquipTTL) {
            SKSE::log::warn("[GRID] pending equip expired ({} units) — releasing",
                            g_pendingEquip.size());
            g_pendingEquip.clear();
        }
        {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = g_pendingRemoveWhen.begin(); it != g_pendingRemoveWhen.end();) {
                if (now - it->second > kPendingRemoveTTL) {
                    if (auto pf = g_pendingRemoveForm.find(it->first);
                        pf != g_pendingRemoveForm.end()) {
                        SKSE::log::warn("[GRID] pending remove expired: form {:08X} x{}",
                            it->first, pf->second);
                        g_pendingRemoveForm.erase(pf);
                        if (auto* f = RE::TESForm::LookupByID(it->first)) {   // GI22
                            const std::string base = FormKey(f->As<RE::TESBoundObject>());
                            for (auto pi = g_pendingRemovePool.begin();
                                 pi != g_pendingRemovePool.end();) {
                                pi = BaseKey(pi->first) == base
                                         ? g_pendingRemovePool.erase(pi) : std::next(pi);
                            }
                        }
                    }
                    it = g_pendingRemoveWhen.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Lazy, not unconditional: the cosave is the layout authority now — an
        // every-rebuild ini re-read was the cross-save contamination (and would
        // clobber the freshly loaded record). Ini = legacy fallback only.
        if (!g_layoutLoaded) LoadLayout();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        CollectDisplayTiles(player);            // stage 1+2 (collect + coin partition)
        auto bags = ReconcileBagBookkeeping();  // stage 3
        auto dummies = MakePaidGoldDummies();   // B: payment spill accounting
        BuildViewsAndSpill(bags, dummies);      // stage 4 (views + spill)
        FinalizeRebuild();                      // stage 5
    }

    int GoldAmount()
    {
        return g_gold;
    }

    void NotePaidGold(int a_price)
    {
        if (a_price > 0) g_paidGold += a_price;
    }

    void NotePendingEquip(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                          std::uint16_t a_sig, int a_hand, const std::string& a_srcKey)
    {
        if (!a_obj) return;
        // mayBeWorn = TRUE: this unit is mid-transition. Before the engine
        // applies the equip it is still on the board and must come out of the
        // set; once applied it is already out via skipWorn, and removing it a
        // second time ate an innocent SPARE of the same pool. Matching a worn
        // list first is exactly the "has it landed yet?" test.
        // srcKey: hold the cell it left open for these frames. Without it the
        // pool re-packed the survivors into the front slots, so the tile that
        // vanished was whichever sorted last -- not the one the player clicked.
        // It came back one frame later, when the applied equip finally forgot
        // the real cell. That round trip is the "spare dagger blinks" report.
        NoteVacated(a_srcKey, a_obj);   // GI28
        g_pendingEquip.push_back({ FormKey(a_obj), a_uid, a_sig, "equipping",
                                   true, false, a_hand, a_srcKey, /*arriving=*/true });
        g_pendingEquipWhen = std::chrono::steady_clock::now();
    }

    void ClearPendingEquips()
    {
        g_pendingEquip.clear();
    }

    void MarkEquipsApplied()
    {
        for (auto& u : g_pendingEquip) u.applied = true;
    }


    // GI32: run queued favourite syncs on the GAME thread (UIRoot::Tick).
    //
    // GI34: and do the attach OURSELVES. The engine's SetFavorite works at ENTRY
    // scope, not unit scope: FavoritesMenu::Entry is { TESForm*,
    // InventoryEntryData* } with nowhere to record WHICH variant, so the engine
    // keeps at most one hotkey per entry and silently refuses a second. That is
    // also why vanilla cannot separate a plain dagger from a tempered one --
    // IsFavorited() is true when ANY list in the entry carries a hotkey, so
    // vanilla just draws the star on every row of that form. It looks like it
    // distinguishes them; it does not.
    //
    // A tile here resolves to its exact ExtraDataList, so we can be right where
    // vanilla is not: attach the ExtraHotkey to that one list. ExtraHotkey is a
    // stock, serialised extra with a public ctor and a game-heap new/delete, so
    // saves, the Q menu and other mods keep reading it exactly as before.
    //
    // The one thing only the engine can do is favourite a unit that owns NO
    // list, because that unit has to be split off the stack first -- and it
    // refuses to split while the entry already carries a hotkey. So lift the
    // other hotkeys for the duration of that one call and put them straight back.
    void ProcessFavorites()
    {
        if (g_favSync.empty()) return;
        auto q = std::move(g_favSync);
        g_favSync.clear();
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* changes = p ? p->GetInventoryChanges() : nullptr;
        if (!changes || !changes->entryList) return;
        for (const auto& f : q) {
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != f.obj) continue;
                const std::string base = FormKey(f.obj);
                // The pool a list belongs to, by CONTENT -- never by pointer.
                // RemoveFavorite/SetFavorite create and destroy lists, so a
                // pointer captured before a call may be freed by it (this is
                // what crashed: RemoveByType deleting an ExtraHotkey the engine
                // had already reclaimed). Pool keys survive that.
                auto poolOf = [&base](RE::ExtraDataList* a_xl) {
                    std::uint16_t u = 0;
                    if (const auto* xu = a_xl->GetByType<RE::ExtraUniqueID>()) u = xu->uniqueID;
                    return PoolPrefix(base, u, InstanceSig(a_xl));
                };
                // Every hotkey removal goes through the engine. We only ever ADD
                // an ExtraHotkey ourselves -- deleting one we did not create is
                // how the crash happened.
                auto clearPools = [&](const std::vector<std::string>& a_pools) {
                    for (bool again = true; again; ) {
                        again = false;
                        if (!entry->extraLists) break;
                        for (auto* x : *entry->extraLists) {
                            if (!x || !x->HasType<RE::ExtraHotkey>()) continue;
                            if (std::find(a_pools.begin(), a_pools.end(), poolOf(x)) ==
                                a_pools.end()) continue;
                            changes->RemoveFavorite(entry, x);
                            again = true;   // the walk is invalid now, restart
                            break;
                        }
                    }
                };

                auto* xl = ExtraForTile(entry, f.uid, f.xlIdx);
                // GI40: read the POOL, the same unit the star is drawn from.
                // Asking this one list said "off" whenever the pool's hotkey was
                // sitting on a SIBLING list -- most often the worn one, after
                // equipping split the pool -- so F added a second hotkey and
                // nothing changed on screen.
                const std::uint16_t tsig = xl ? InstanceSig(xl) : 0;
                const bool on = PoolHasStar(entry, f.uid, tsig);
                if (on) {
                    clearPools({ PoolPrefix(base, f.uid, tsig) });
                } else if (xl) {
                    xl->Add(new RE::ExtraHotkey(RE::ExtraHotkey::Hotkey::kUnbound));
                } else {
                    // No list of its own, and the pool has no star anywhere else.
                    // Only the engine can split the unit off the stack, and it
                    // refuses while the entry already carries a hotkey -- so lift
                    // the others across the call.
                    std::vector<std::string> lifted;
                    if (entry->extraLists) {
                        for (auto* x : *entry->extraLists) {
                            if (x && x->HasType<RE::ExtraHotkey>()) lifted.push_back(poolOf(x));
                        }
                    }
                    clearPools(lifted);
                    changes->SetFavorite(entry, nullptr);
                    // Re-walk the CURRENT list and restore by POOL, so a list
                    // the split rebuilt is matched by what it holds, not by an
                    // address that may no longer mean anything.
                    if (entry->extraLists && !lifted.empty()) {
                        for (auto* x : *entry->extraLists) {
                            if (!x || x->HasType<RE::ExtraHotkey>()) continue;
                            if (std::find(lifted.begin(), lifted.end(), poolOf(x)) ==
                                lifted.end()) continue;
                            x->Add(new RE::ExtraHotkey(RE::ExtraHotkey::Hotkey::kUnbound));
                        }
                    }
                }
                if (g_poolTrace) {
                    std::string ls;
                    if (entry->extraLists) {
                        for (auto* x2 : *entry->extraLists) {
                            if (!x2) continue;
                            std::uint16_t u = 0;
                            if (const auto* xu = x2->GetByType<RE::ExtraUniqueID>()) {
                                u = xu->uniqueID;
                            }
                            ls += std::format("[{}{}] ",
                                PoolPrefix(FormKey(f.obj), u, InstanceSig(x2)),
                                x2->HasType<RE::ExtraHotkey>() ? " HOT" : "");
                        }
                    }
                    SKSE::log::info("[FAV] toggle uid {:04X} xl {} was={} via={} | {}",
                        f.uid, f.xlIdx, on ? "on" : "off",
                        xl ? "self" : "engine", ls.empty() ? "-" : ls);
                }
                break;
            }
        }
        // GI33: the star is read back OUT of the engine, so the board has to be
        // rebuilt once the change has actually landed. Without this the toggle
        // applied a frame later than the draw that was supposed to show it and F
        // looked like it did nothing.
        g_needRebuild = true;
    }

    // GI36: resolve the sub-stack that is ACTUALLY leaving the bag, and drop its
    // star on the way out. Every outbound sink calls this instead of
    // ExtraForPool and hands the result straight to RemoveItem / the world drop.
    //
    // Rule 58: sold, stored, dropped, binned and planted all kill the star.
    // Equipping does not -- it comes back, so the star waits on the doll (55).
    //
    // Resolution and removal are ONE call on purpose. The previous attempt
    // cleared by POOL NAME from NotePendingRemove, and a pool name is a crowd:
    // three separately starred daggers all hash to the same pool, so storing one
    // stripped all three. A crowd cannot give up one member's star. Only the
    // list we are about to hand the engine can.
    //
    // NotePendingRemove is also the wrong MOMENT: it fires at request time, so a
    // failed pickpocket roll rolled the item back but not the star.
    //
    //   a_starred = how many of the a_count outgoing units wore a star. The
    //               caller always knows; the sink never can.
    // Returns nullptr for "let the engine pick" -- only ever within a pool whose
    // members are genuinely interchangeable.
    RE::ExtraDataList* ResolveExitUnit(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                       std::uint16_t a_sig, int a_count, int a_starred)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !a_obj) return nullptr;
        auto* changes = p->GetInventoryChanges();
        auto* entry   = LiveEntry(p, a_obj);
        if (!entry) return nullptr;
        auto* xl = ExtraForPoolImpl(entry, a_uid, a_sig);   // unchanged rules (worn excluded)
        if (a_starred <= 0 || !changes) return xl;          // no star leaving: identical to before

        // GI37: pin a list open before stripping its star.
        //
        // A plain unit's list holds NOTHING but the ExtraHotkey, so removing the
        // star retires the list and takes our only handle with it. RemoveItem
        // then gets nullptr, picks for itself, and can walk a TEMPERED spare out
        // instead of the plain dagger that was clicked -- the two swap places on
        // the board. Targeting must survive the policy.
        //
        // ExtraCount is the anchor: InstanceSig does not hash it, so the pool key
        // (and the remembered cell with it) is unchanged, and the value we write
        // is the count the list already reports.
        auto anchor = [](RE::ExtraDataList* a_xl) {
            if (a_xl && !a_xl->HasType<RE::ExtraCount>()) {
                a_xl->Add(new RE::ExtraCount(
                    static_cast<std::int16_t>((std::max)(1, a_xl->GetCount()))));
            }
        };

        // (1) The pool NAMES one list => that is the unit going out, and its
        //     star is the only one we are entitled to touch.
        if (xl) {
            // Only part of a stack leaving? The survivors own that list, so the
            // star stays with them.
            const int listCount = (std::max)(1, xl->GetCount());
            if (!xl->HasType<RE::ExtraHotkey>() || a_count < listCount) return xl;
            anchor(xl);                        // GI37: keep the handle alive
            changes->RemoveFavorite(entry, xl);
            // RemoveFavorite can retire a list that just went empty: re-fetch the
            // entry and match by ADDRESS ONLY -- never dereference a dead pointer.
            entry = LiveEntry(p, a_obj);
            if (entry && entry->extraLists) {
                for (auto* x : *entry->extraLists) {
                    if (x == xl) return xl;
                }
            }
            return nullptr;   // merged back into the plain stack: any unit will do
        }

        // (2) PLAIN pool (uid 0, sig 0). Members are interchangeable by content
        //     and the star is the ONLY difference, so the only thing that has to
        //     come out right is how many stars remain. Clear at most a_starred,
        //     then hand the engine the list we just stripped so the unit that
        //     leaves is one that has already lost its star.
        int budget = a_starred;
        RE::ExtraDataList* freed = nullptr;
        RE::ExtraDataList* home = nullptr;   // starred list we may not strip
        while (budget > 0) {
            entry = LiveEntry(p, a_obj);
            if (!entry || !entry->extraLists) break;
            RE::ExtraDataList* hit = nullptr;
            for (auto* x : *entry->extraLists) {
                if (!x || !x->HasType<RE::ExtraHotkey>()) continue;
                if (x->HasType<RE::ExtraWorn>() ||
                    x->HasType<RE::ExtraWornLeft>()) continue;       // rule 55
                if (InstanceSig(x) != 0) continue;                   // a different pool
                if (const auto* xu = x->GetByType<RE::ExtraUniqueID>();
                    xu && xu->uniqueID != 0) continue;               // a uid pool
                if (!home) home = x;   // GI38: the unit lives HERE either way
                if ((std::max)(1, x->GetCount()) > budget) continue;  // survivors own it
                hit = x;
                break;
            }
            if (!hit) break;
            budget -= (std::max)(1, hit->GetCount());
            anchor(hit);                       // GI37: keep the handle alive
            changes->RemoveFavorite(entry, hit);
            freed = hit;
        }
        // Re-validate by ADDRESS against the current list before handing anything
        // back -- the calls above can retire a list.
        auto alive = [&](RE::ExtraDataList* a_x) -> RE::ExtraDataList* {
            if (!a_x) return nullptr;
            entry = LiveEntry(p, a_obj);
            if (!entry || !entry->extraLists) return nullptr;
            for (auto* x : *entry->extraLists) {
                if (x == a_x) return a_x;
            }
            return nullptr;
        };
        RE::ExtraDataList* out = alive(freed);
        // GI38: we may not be ALLOWED to strip the star -- several units share one
        // list, so taking its hotkey would rob the ones staying behind. That is a
        // reason to keep the star, NOT a reason to forget where the unit lives.
        // Returning nullptr here handed the choice to the engine, and it walked a
        // TEMPERED dagger out instead of the plain one that was clicked.
        // Correct targeting outranks the star policy: the star rides along.
        if (!out) out = alive(home);
        if (g_poolTrace) {
            std::string ls;
            if (entry && entry->extraLists) {
                for (auto* x : *entry->extraLists) {
                    if (!x) continue;
                    ls += std::format("[sig {:04X} n{}{}] ", InstanceSig(x),
                        (std::max)(1, x->GetCount()),
                        x->HasType<RE::ExtraHotkey>() ? " HOT" : "");
                }
            }
            SKSE::log::info(
                "[FAV] exit uid {:04X} sig {:04X} n={} starred={} cleared={} handed={} | {}",
                a_uid, a_sig, a_count, a_starred, a_starred - budget,
                out ? (out == freed ? "stripped" : "kept-star") : "engine",
                ls.empty() ? "-" : ls);
        }
        return out;
    }

    void NotePendingRemove(RE::TESBoundObject* a_obj, const std::string& a_key, int a_count)
    {
        if (!a_obj || a_count <= 0) return;
        NoteVacated(a_key, a_obj);   // GI28: flash the cell we aimed at
        const RE::FormID fid = a_obj->GetFormID();
        if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) return;   // coins: own system
        g_pendingRemoveForm[fid] += a_count;
        g_pendingRemoveWhen[fid] = std::chrono::steady_clock::now();   // B3
        if (!a_key.empty()) g_pendingRemovePool[PoolOfKey(a_key)] += a_count;   // GI22

        const GridDef gdef = g_resolver ? g_resolver(a_obj) : GridDef{};
        const int cap = EffectiveCap(a_obj, gdef);
        if (a_key.empty()) return;   // carried fragment: form-level only

        if (cap > 1) {
            // stackable: the sold/stored units leave THIS tile's remembered
            // quantity immediately — the reconciler then sees no gap, so no
            // other stack of the form is ever touched. Cancel-safe: this runs
            // on CONFIRM only. (The old key-mark scheme was cleared by
            // ProcessTransfers before the reconciler could consume it, which
            // let the generic drain order eat a different tile.)
            if (auto lt = g_layout.find(a_key); lt != g_layout.end()) {
                lt->second.count -= a_count;
                // 0 must ERASE, not persist — count 0 reads as "unspecified"
                // (legacy) and the tile would resurrect as a fresh pickup
                if (lt->second.count <= 0) g_layout.erase(lt);
            }
            return;
        }

        // GI20: cap<=1 (gear/bags) -- just drop THIS slot.
        //
        // This used to erase the key and then RE-KEY every surviving tile of the
        // form densely (#0..#n-1). That is what made the wrong cell empty: the
        // survivors were renumbered by their old ordinal while the next
        // enumeration handed out ordinals in walk order, and the two orders are
        // not the same thing. Pool assignment (see EnumerateUnitTiles) now maps
        // units to slots by POSITION, so removing a slot is the whole operation
        // -- the rest stay exactly where they are, and no key ever changes name.
        //
        // Losing the re-key also removes its bag hazard: a renamed bag key had to
        // drag g_openBags and every contents' inBag pointer along with it.
        g_layout.erase(a_key);
    }

    void ClearPendingRemove(RE::TESBoundObject* a_obj, int a_count)
    {
        if (!a_obj || a_count <= 0) return;
        const RE::FormID fid = a_obj->GetFormID();
        if (auto it = g_pendingRemoveForm.find(fid); it != g_pendingRemoveForm.end()) {
            it->second -= a_count;
            if (it->second <= 0) {
                g_pendingRemoveForm.erase(it);
                g_pendingRemoveWhen.erase(fid);   // B3
            } else {
                g_pendingRemoveWhen[fid] = std::chrono::steady_clock::now();   // B3: still draining
            }
        }
        // GI22: drain the same amount from this form's pools. Which pool no
        // longer matters -- the engine count already reflects the move, so the
        // only job left is to stop hiding units.
        const std::string base = FormKey(a_obj);
        int left = a_count;
        for (auto pi = g_pendingRemovePool.begin();
             pi != g_pendingRemovePool.end() && left > 0;) {
            if (BaseKey(pi->first) != base) { ++pi; continue; }
            const int t = (std::min)(left, pi->second);
            pi->second -= t;
            left -= t;
            pi = pi->second <= 0 ? g_pendingRemovePool.erase(pi) : std::next(pi);
        }
    }

    void ClearAllPendingRemoves()
    {
        g_pendingRemoveForm.clear();
        g_pendingRemoveWhen.clear();   // B3
        g_pendingRemovePool.clear();   // GI22
    }

    void ClearDropHint()
    {
        g_dropHint = {};
    }

    int MaxAcceptUnits(RE::TESBoundObject* a_obj, int a_want)
    {
        // How many units (<= a_want) the inventory can ACCEPT right now:
        // partial-stack room first, then new tiles on the hard board, then new
        // tiles in open bags (same spill rules as Rebuild). Phase 7: stack
        // buy/take sliders clamp to this so a bulk purchase can't overflow.
        if (a_want <= 0) return 0;
        if (!a_obj || a_obj->IsGold()) return a_want;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return a_want;

        if (!g_layoutLoaded) LoadLayout();

        // stack cap of the incoming form (Mabinogi tiles of up-to-cap units)
        const GridDef aDef = g_resolver ? g_resolver(a_obj) : GridDef{};
        const int aCap = EffectiveCap(a_obj, aDef);
        const std::string aKey = FormKey(a_obj);

        // units that merge into existing PARTIAL tiles (no new cells needed)
        int room = 0;
        if (aCap > 1) {
            for (auto& [k, v] : g_layout) {
                if (BaseKey(k) == aKey && v.count > 0 && v.count < aCap) {
                    room += aCap - v.count;
                }
            }
        }
        if (room >= a_want) return a_want;
        const int tilesNeeded = (a_want - room + aCap - 1) / aCap;

        // shared headless collection (Phase 2) — one rule set for all sims
        CapTiles ct;
        CollectCapacityTiles(ct);
        auto& tmp = ct.tiles;

        // main occupants only: items inside a present (or form-anchored) bag
        // consume that bag's cells, not the board's
        std::vector<Item*> list;
        list.reserve(tmp.size() + tilesNeeded);
        for (auto& it : tmp) {
            if (it.inBag.empty()) list.push_back(&it);
        }

        // probes LAST: they only get what is left over on the hard board
        std::vector<Item> probes(tilesNeeded);
        for (int i = 0; i < tilesNeeded; ++i) {
            probes[i].key = "##probe" + std::to_string(i);
            probes[i].def = aDef;
            probes[i].mask = MaskOf(aDef);
            list.push_back(&probes[i]);
        }
        PlaceItems(list, kCols, kMinRows, kMinRows);   // HARD board, no growth

        int fitTiles = 0;
        std::vector<Item*> leftover;
        for (auto& p : probes) {
            if (!p.overflow) ++fitTiles;
            else leftover.push_back(&p);
        }

        // B: spill leftover probes into OPEN bags with room (a bag item never
        // nests inside another bag) — mirrors the Rebuild spill pass.
        if (aDef.bag == 0) {
            for (const auto& bagKey : g_openBags) {
                if (leftover.empty()) break;
                auto bagIt = std::find_if(tmp.begin(), tmp.end(),
                    [&](const Item& it) { return it.key == bagKey && it.def.bag != 0; });
                if (bagIt == tmp.end()) continue;
                std::vector<Item*> blist;
                for (auto& it : tmp) {
                    if (it.inBag == bagKey) blist.push_back(&it);
                }
                for (auto* p : leftover) {   // reset from the main-board sim
                    p->col = -1;
                    p->row = -1;
                    blist.push_back(p);
                }
                PlaceItems(blist, (std::max)(1, bagIt->def.bw),
                           (std::max)(1, bagIt->def.bh), (std::max)(1, bagIt->def.bh));
                std::vector<Item*> still;
                for (auto* p : leftover) {
                    if (!p->overflow) ++fitTiles;
                    else still.push_back(p);
                }
                leftover = std::move(still);
            }
        }

        const long long units = static_cast<long long>(room) +
                                static_cast<long long>(fitTiles) * aCap;
        return static_cast<int>((std::min)(static_cast<long long>(a_want), units));
    }

    bool CanFitNewItem(RE::TESBoundObject* a_obj)
    {
        // one-unit capacity gate: stacks onto a partial tile, or one new tile
        // first-fits on the hard board / an open bag (delegates to the
        // generalized counter so both gates share identical rules).
        return MaxAcceptUnits(a_obj, 1) >= 1;
    }

    bool WouldOverflow(RE::TESBoundObject* a_obj)
    {
        if (!a_obj || a_obj->IsGold()) return false;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return false;

        if (!g_layoutLoaded) LoadLayout();

        const std::string targetKey = FormKey(a_obj);
        // shared headless collection (Phase 2) — one rule set for all sims
        CapTiles ct;
        CollectCapacityTiles(ct);

        std::vector<Item*> list;
        list.reserve(ct.tiles.size());
        for (auto& it : ct.tiles) {
            if (it.inBag.empty()) list.push_back(&it);
        }

        PlaceItems(list, kCols, kMinRows, kMinRows);   // hard board

        for (const auto& it : ct.tiles) {
            if (it.key == targetKey) return it.overflow;
        }
        return false;   // worn/absent: nothing to bounce
    }

    // W2: does the CURRENT inventory overflow the hard board? Same headless
    // placement as WouldOverflow, but asks about everything at once.
    namespace
    {
        bool ComputeOverloaded()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;

            if (!g_layoutLoaded) LoadLayout();

            // shared headless collection (Phase 2). NOTE two deliberate
            // unifications vs the old copy: ①unnamed COIN mirror tiles now
            // occupy capacity here too (they always did in the display and in
            // MaxAcceptUnits) ②saved overflow-zone spots reset to first-fit
            // (they are temporary everywhere else).
            CapTiles ct;
            CollectCapacityTiles(ct);
            auto& tmp = ct.tiles;

            std::vector<Item*> list;
            list.reserve(tmp.size());
            for (auto& it : tmp) {
                // main occupant only when NOT inside a bag that still exists —
                // by TILE (normal) or by FORM (tile transiently absent at load)
                if (it.inBag.empty()) {
                    list.push_back(&it);
                }
            }

            PlaceItems(list, kCols, kMinRows, kMinRows);   // hard board

            // B: hard-board overflow drains into OPEN bags (mirrors Rebuild's
            // spill) — an item that a bag can hold is NOT overloaded. Coins and
            // bag items can't spill: their overflow is a genuine overload.
            std::vector<Item*> spill;
            bool hardOverflow = false;
            for (auto& it : tmp) {
                if (!it.overflow) continue;
                if (it.inBag.empty() && it.def.bag == 0 && it.obj &&
                    !it.obj->IsGold() && !GoldCoins::IsCoinForm(it.obj->GetFormID())) {
                    spill.push_back(&it);
                } else {
                    hardOverflow = true;
                }
            }
            for (const auto& bagKey : g_openBags) {
                if (spill.empty()) break;
                const Item* bagItem = nullptr;
                for (auto& it : tmp) {
                    if (it.key == bagKey && it.def.bag != 0) { bagItem = &it; break; }
                }
                if (!bagItem) continue;
                const int bw = (std::max)(1, bagItem->def.bw);
                const int bh = (std::max)(1, bagItem->def.bh);
                std::vector<Item*> occ;
                for (auto& it : tmp) {
                    if (it.inBag == bagKey) occ.push_back(&it);
                }
                for (auto sit = spill.begin(); sit != spill.end();) {
                    Item* cand = *sit;
                    std::vector<Item*> test = occ;
                    cand->col = -1;
                    cand->row = -1;
                    test.push_back(cand);
                    PlaceItems(test, bw, bh, bh);
                    if (!cand->overflow) {
                        cand->inBag = bagKey;
                        occ.push_back(cand);
                        sit = spill.erase(sit);
                    } else {
                        ++sit;
                    }
                }
            }
            return hardOverflow || !spill.empty();
        }
    }

    bool IsOverloaded() { return g_overloaded; }

    int SpaceUsed() { return g_spaceUsed; }

    int SpaceTotal() { return kCols * kMinRows; }

    int BagFreeCells()
    {
        // B: free cells across every OPEN bag (from the last rebuild) — the
        // R-key "take everything that fits" budget adds this so a full main
        // board still drains loot into bag space.
        int free = 0;
        for (const auto& v : g_views) {
            if (v.bagKey.empty()) continue;   // main board
            int used = 0;
            for (int idx : v.items) used += MaskCells(g_items[idx].mask.rows);
            free += (std::max)(0, v.cols * v.minRows - used);
        }
        return free;
    }

    void PickupPartial(RE::TESBoundObject* a_obj, int a_count,
                       const std::string& a_srcKey, int a_srcTotal)
    {
        // shift+left-click split (G4): the chosen quantity leaves its source
        // tile NOW and rides the cursor as a carried fragment (preSplit, no key
        // yet). Rebuild counts the fragment via g_held->count, so the form's
        // total stays == engine count — a later cancel is auto-restored by the
        // ACQUIRE path (the source tile is now short by a_count).
        if (!a_obj || a_count <= 0) return;

        // G4 GOLD split: a_count is a VALUE. Create a pin for the fragment
        // (subtracts from walking gold), reduce the source if it was itself a
        // pin, and carry the fragment as a coin of the value's band.
        if (GoldCoins::IsCoinForm(a_obj->GetFormID()) &&
            !GoldCoins::IsPouch(a_obj->GetFormID())) {
            const int val = (std::min)(a_count, GoldCoins::kCoinCap);
            if (const int sv = GoldCoins::PinnedValue(a_srcKey); sv >= 0) {
                const int rem = sv - val;   // source was a pin: shrink/remove it
                if (rem > 0) GoldCoins::PinAmount(a_srcKey, rem);
                else       { GoldCoins::UnpinTile(a_srcKey); g_layout.erase(a_srcKey); }
            } else {
                // AUTO source: converting only part of walking gold would let
                // Desired() reshuffle the OTHER auto tiles. Convert the WHOLE
                // source tile to a pin (its remainder stays at the source cell),
                // so sibling coins are untouched.
                const int srcRem = (std::max)(0, a_srcTotal - val);
                LayoutEntry pos{};
                bool havePos = false;
                if (auto li = g_layout.find(a_srcKey); li != g_layout.end()) {
                    pos = li->second;
                    havePos = true;
                }
                g_layout.erase(a_srcKey);
                if (srcRem > 0) {
                    PlacePin(srcRem, havePos ? pos.col : -1, havePos ? pos.row : -1,
                             havePos ? pos.bag : std::string{});
                }
            }
            auto* cform = GoldCoins::CoinForTier(GoldCoins::BandTier(val));
            if (!cform) return;
            const std::string pinKey = NextTileKey(FormKey(cform));
            GoldCoins::PinAmount(pinKey, val);   // walking -= val, fragment reserved
            const GridDef gd = g_resolver ? g_resolver(cform) : GridDef{};
            Held g;
            g.key = pinKey;          // real pin key; position assigned on drop
            g.obj = cform;
            g.mask = MaskOf(gd);
            g.count = 1;
            g.coinValue = val;
            g.defScale = gd.scale;
            g.offX = g.mask.w * CellPx() * 0.5f;
            g.offY = g.mask.h * CellPx() * 0.5f;
            g.justPicked = true;
            g.preSplit = true;
            g_held = g;
            if (g_sound) g_sound(cform, true);   // the purse audibly lifts
            g_needRebuild = true;
            return;
        }

        if (auto li = g_layout.find(a_srcKey); li != g_layout.end()) {
            li->second.count -= a_count;
            if (li->second.count <= 0) g_layout.erase(li);   // took the whole tile
        }
        const GridDef d = g_resolver ? g_resolver(a_obj) : GridDef{};
        Held h;
        h.key.clear();            // assigned on drop (new tile) or absorbed (merge)
        h.obj = a_obj;
        h.mask = MaskOf(d);
        h.count = a_count;
        h.isBag = d.bag != 0;
        h.defScale = d.scale;
        h.offX = h.mask.w * CellPx() * 0.5f;
        h.offY = h.mask.h * CellPx() * 0.5f;
        h.justPicked = true;
        h.preSplit = true;
        for (const auto& si : g_items) {   // GI36: carry the source tile's star
            if (si.key == a_srcKey) { h.fav = si.fav; break; }
        }
        g_held = h;
        if (g_sound) g_sound(a_obj, true);   // split confirmed -> pickup sound
        g_needRebuild = true;
    }

    int CellSpanOf(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return 1;
        const GridDef d = g_resolver ? g_resolver(a_obj) : GridDef{};
        const Mask m = MaskOf(d);
        int n = 0;
        for (int y = 0; y < m.h; ++y) {
            for (int x = 0; x < m.w; ++x) {
                if (m.rows[y][x]) ++n;
            }
        }
        return (std::max)(1, n);
    }

    void MarkCapacityDirty() { g_capacityDirty = true; }

    namespace
    {
        // W1v2: save-clean encumbrance via ESP ABILITIES. Ability effects are
        // stored in the save only as form references — deleting the mod makes
        // the engine purge them automatically (zero CarryWeight residue),
        // unlike the temporary-AV steering below (kept as fallback).
        //   0x807 GI_CarryBoost : ability, CarryWeight +50000
        //   0x808 GI_Overload   : ability, CarryWeight net negative
        RE::SpellItem* g_abBoost = nullptr;
        RE::SpellItem* g_abOver = nullptr;
        bool g_abTried = false;
        bool g_avResidueCleared = false;

        void ResolveAbilities()
        {
            if (g_abTried) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            g_abTried = true;
            g_abBoost = dh->LookupForm<RE::SpellItem>(0x807, "Grid Inventory.esp");
            g_abOver = dh->LookupForm<RE::SpellItem>(0x808, "Grid Inventory.esp");
            SKSE::log::info("[GRID] encumbrance mode: {}",
                (g_abBoost && g_abOver) ? "abilities (save-clean)" : "AV steering (fallback)");
        }
    }

    void CapacityTick()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return;

        if (g_capacityDirty) {
            g_capacityDirty = false;
            const bool was = g_overloaded;
            g_overloaded = ComputeOverloaded();
            if (g_overloaded && !was) {
                Sfx::FailNote(Lang::T(Lang::Str::Overloaded));
                SKSE::log::info("[GRID] capacity: OVERLOADED");
            } else if (!g_overloaded && was) {
                SKSE::log::info("[GRID] capacity: back to normal");
            }
        }

        // W1v2 preferred path: ESP abilities (no save residue). The boost is
        // always on; the overload debuff toggles with the capacity state.
        ResolveAbilities();
        auto* avo = player->AsActorValueOwner();
        if (g_abBoost && g_abOver) {
            // one-time per save: neutralise the OLD AV-steering residue
            // (ours was thousands; potion effects are tiny — leave those)
            if (!g_avResidueCleared) {
                g_avResidueCleared = true;
                const float t = player->GetActorValueModifier(
                    RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight);
                if (std::fabs(t) > 2000.0f) {
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                        RE::ActorValue::kCarryWeight, -t);
                    SKSE::log::info("[GRID] cleared legacy CW modifier ({:+.0f})", t);
                }
            }
            if (!player->HasSpell(g_abBoost)) player->AddSpell(g_abBoost);
            const bool has = player->HasSpell(g_abOver);
            if (g_overloaded && !has) player->AddSpell(g_abOver);
            else if (!g_overloaded && has) player->RemoveSpell(g_abOver);
            return;
        }

        // W1: weight never limits — keep effective CarryWeight comfortably
        // above the inventory weight. W2: while overloaded, hold it just BELOW
        // so the vanilla encumbrance (forced walk, no fast travel) engages.
        // Steered every frame through the TEMPORARY AV modifier, so whatever
        // perks/spells/other mods do to CarryWeight, the net stays on target.
        constexpr float kBuffer = 10000.0f;
        const float invW = avo->GetActorValue(RE::ActorValue::kInventoryWeight);
        const float cw = avo->GetActorValue(RE::ActorValue::kCarryWeight);
        const float target = g_overloaded ? (std::max)(0.0f, invW - 5.0f)
                                          : invW + kBuffer;
        const float delta = target - cw;
        if (std::fabs(delta) > 0.5f) {
            avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                RE::ActorValue::kCarryWeight, delta);
        }
    }

    int ItemValue(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return -1;
        const auto it = g_values.find(a_obj->GetFormID());
        return it == g_values.end() ? -1 : it->second;
    }

    int UnitValueWith(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl)
    {
        const int base = ItemValue(a_obj);
        if (!a_obj || !a_xl) return base;
        // Same throwaway-entry pattern the tooltip's damage/armor cards already
        // use in-game -- extended with the unit's real list for the value call,
        // then detached so the destructors see nothing engine-owned.
        RE::BSSimpleList<RE::ExtraDataList*> sl;
        // push_front on an EMPTY list constructs the item inside the embedded
        // head node -- no allocation. (front() here was a crash: begin() on an
        // empty list returns the null end() iterator, and release builds strip
        // the assert, so the assignment wrote through nullptr.)
        sl.push_front(a_xl);
        RE::InventoryEntryData e(a_obj, 1);
        e.extraLists = &sl;
        const int v = e.GetValue();
        e.extraLists = nullptr;         // detach BEFORE ~InventoryEntryData
        return v > 0 ? v : base;
    }

    // Public forwarders for the two instance resolvers (the implementations
    // live in the anonymous namespace above, next to the key grammar they
    // belong to). Callers outside Grid.cpp -- Equip, LootBarter -- need them to
    // name a sub-stack when they move or wear an item.
    RE::InventoryEntryData* LiveEntryOf(RE::TESObjectREFR* a_owner,
                                        RE::TESBoundObject* a_obj)
    {
        return LiveEntry(a_owner, a_obj);
    }

    RE::ExtraDataList* ExtraForInstance(RE::InventoryEntryData* a_entry,
                                        std::uint16_t a_uid, int a_xlIdx)
    {
        return ExtraForTile(a_entry, a_uid, a_xlIdx);
    }

    RE::ExtraDataList* ExtraForPool(RE::InventoryEntryData* a_entry,
                                    std::uint16_t a_uid, std::uint16_t a_sig)
    {
        return ExtraForPoolImpl(a_entry, a_uid, a_sig);
    }

    RE::ExtraDataList* ExtraForPoolOnPartner(RE::InventoryEntryData* a_entry,
                                             std::uint16_t a_uid, std::uint16_t a_sig)
    {
        return ExtraForPoolImpl(a_entry, a_uid, a_sig, /*allowWorn=*/true);
    }

    UnitChoice PoolChoice(RE::InventoryEntryData* a_entry, std::uint16_t a_uid,
                          std::uint16_t a_sig, bool a_nameWorn, bool a_wornLegal)
    {
        if (auto* xl = ExtraForPoolImpl(a_entry, a_uid, a_sig, a_nameWorn)) {
            return { PickKind::kNamed, xl };
        }
        // A named pool (uid or sig) that is ABSENT is a stale click, not a
        // licence for the engine to substitute something else.
        if (a_uid != 0 || a_sig != 0) return {};
        if (!a_entry || !a_entry->extraLists) return { PickKind::kAnyIsSafe, nullptr };
        bool ambiguous = false;
        for (auto* xl : *a_entry->extraLists) {
            if (!xl) continue;
            const bool wornHere = xl->HasType<RE::ExtraWorn>() ||
                                  xl->HasType<RE::ExtraWornLeft>();
            const auto* xu = xl->GetByType<RE::ExtraUniqueID>();
            const bool plain = InstanceSig(xl) == 0 && !(xu && xu->uniqueID != 0);
            if (wornHere && !a_wornLegal) return {};   // the body's unit is grabbable
            if (!plain) ambiguous = true;
        }
        return { ambiguous ? PickKind::kFallback : PickKind::kAnyIsSafe, nullptr };
    }

    std::uint16_t InstanceSigOf(RE::ExtraDataList* a_xl) { return InstanceSig(a_xl); }

    int StackCap(RE::TESBoundObject* a_obj)
    {
        return a_obj ? EffectiveCap(a_obj) : 1;
    }

    void EnumerateUnits(RE::InventoryEntryData* a_entry, int a_count,
                        std::vector<UnitRef>& a_out, bool a_skipWorn)
    {
        EnumerateUnitRefs(a_count, a_count, a_entry, a_out, a_skipWorn);
    }

    // a_hand: 0 = either, 1 = RIGHT (ExtraWorn), 2 = LEFT (ExtraWornLeft).
    //
    // The hand matters as soon as two copies of ONE form are worn at once -- a
    // dagger in each hand. "First worn list of this form" then answers the same
    // list for both slots, so the doll showed the left item's stats on the right,
    // lifting one unequipped the other, and a swap saw the two as one unit.
    RE::ExtraDataList* WornExtraOf(RE::InventoryEntryData* a_entry, int a_hand)
    {
        if (!a_entry || !a_entry->extraLists) return nullptr;
        for (auto* xl : *a_entry->extraLists) {
            if (!xl) continue;
            const bool L = xl->HasType<RE::ExtraWornLeft>();
            const bool R = xl->HasType<RE::ExtraWorn>();
            if (!L && !R) continue;
            if (a_hand == 1 && !R) continue;
            if (a_hand == 2 && !L) continue;
            return xl;
        }
        return nullptr;
    }

    // The worn list belonging to THIS unit. a_sig 0 with a_uid 0 means "the
    // plain one", which is still unambiguous per hand.
    RE::ExtraDataList* WornExtraMatching(RE::InventoryEntryData* a_entry,
                                         std::uint16_t a_uid, std::uint16_t a_sig,
                                         int a_hand)
    {
        if (!a_entry || !a_entry->extraLists) return nullptr;
        for (auto* xl : *a_entry->extraLists) {
            if (!xl) continue;
            const bool L = xl->HasType<RE::ExtraWornLeft>();
            const bool R = xl->HasType<RE::ExtraWorn>();
            if (!L && !R) continue;
            if (a_hand == 1 && !R) continue;
            if (a_hand == 2 && !L) continue;
            std::uint16_t uid = 0;
            if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
            if (uid == a_uid && InstanceSig(xl) == a_sig) return xl;
        }
        return WornExtraOf(a_entry, a_hand);   // fall back rather than show nothing
    }

    // GI1/D2: this used to scan the WHOLE entry and glow if ANY sub-stack was
    // enchanted -- so one enchanted sword in a stack of three lit all three.
    // The caller now names the sub-stack this pixel belongs to:
    //   a_xl == nullptr -> a plain unit (or no entry at all): base form only.
    std::uint8_t GlowBits(RE::TESBoundObject* a_obj, RE::InventoryEntryData*,
                          RE::ExtraDataList* a_xl)
    {
        if (!a_obj) return 0;
        std::uint8_t glow = 0;
        if (const auto* ef = a_obj->As<RE::TESEnchantableForm>();
            ef && ef->formEnchanting) {
            glow |= 1;
        }
        if (!(glow & 1) && a_xl) {
            if (const auto* xe = a_xl->GetByType<RE::ExtraEnchantment>();
                xe && xe->enchantment) {
                glow |= 1;
            }
        }
        if (!a_obj->Is(RE::FormType::Book) && HasDescCached(a_obj)) glow |= 2;
        // GI46: per-unit STATUS bits (drawn as border rings, not halos)
        if (a_xl) {
            if (const auto* xp = a_xl->GetByType<RE::ExtraPoison>();
                xp && xp->poison) {
                glow |= 4;
            }
            if (const auto* xh = a_xl->GetByType<RE::ExtraHealth>();
                xh && xh->health > 1.0f) {
                glow |= 8;
            }
        }
        return glow;
    }

    void DrawCountBadge(ImDrawList* a_dl, const ImVec2& a_tileMin, const char* a_text)
    {
        // Mabinogi-style: hugging the corner, full black outline so the count
        // reads on any icon underneath (all skins are dark-grounded)
        const ImVec2 tp(a_tileMin.x + 2.0f, a_tileMin.y - 1.0f);
        const ImU32 oc = IM_COL32(0, 0, 0, 255);
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) continue;
                a_dl->AddText(ImVec2(tp.x + ox, tp.y + oy), oc, a_text);
            }
        }
        a_dl->AddText(tp, Theme::Col(Theme::S().hi, 1.0f), a_text);
    }

    void DrawGlow(ImDrawList* a_dl, RE::TESBoundObject* a_obj, std::uint8_t a_bits,
                  const ImVec2& a_iconMin, const ImVec2& a_iconMax,
                  const ImVec2& a_boxMin, const ImVec2& a_boxMax)
    {
        if (!a_bits || !a_dl) return;
        const auto ga = [](int a_a) {
            return static_cast<std::uint32_t>((std::min)(255.0f,
                a_a * Theme::GlowGain()));
        };
        // GI46: bits 1|2 = rarity HALO (permanent identity), bits 4|8 = status
        // RINGS (poison / temper -- transient, drawn as border strokes so they
        // never fight the halo). The halo switch must see ONLY its own bits, or
        // a poison-only tile would fall into the "both rarities" red default.
        const std::uint8_t haloBits = a_bits & 0x3;
        if (haloBits) {
        ImU32 tint;
        switch (haloBits) {
        case 1:  tint = IM_COL32(90, 160, 255, ga(82)); break;   // enchanted
        case 2:  tint = IM_COL32(255, 190, 70, ga(56)); break;   // unique
        default: tint = IM_COL32(255, 80, 60, ga(74));  break;   // both
        }
        const auto* icon = IconCache::GetSingleton()->Get(a_obj);
        if (Theme::GlowStyle() == 1 && icon && icon->glowSrv &&
            icon->gw > icon->gpad * 2 && icon->gh > icon->gpad * 2) {
            const float dw = a_iconMax.x - a_iconMin.x;
            const float dh = a_iconMax.y - a_iconMin.y;
            const float sx = dw / static_cast<float>(icon->gw - icon->gpad * 2);
            const float sy = dh / static_cast<float>(icon->gh - icon->gpad * 2);
            a_dl->AddImage(reinterpret_cast<ImTextureID>(icon->glowSrv),
                ImVec2(a_iconMin.x - icon->gpad * sx, a_iconMin.y - icon->gpad * sy),
                ImVec2(a_iconMax.x + icon->gpad * sx, a_iconMax.y + icon->gpad * sy),
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        } else if (void* gtex = UIRoot::GlowTexture()) {
            // GI49 option A: 22% overscan (see DrawGlowPass) -- the radial
            // halo draws larger than the footprint so its bright core is not
            // swallowed by the sprite on multi-cell items
            const float outX = (a_boxMax.x - a_boxMin.x) * 0.11f;
            const float outY = (a_boxMax.y - a_boxMin.y) * 0.11f;
            a_dl->AddImage(reinterpret_cast<ImTextureID>(gtex),
                ImVec2(a_boxMin.x - outX, a_boxMin.y - outY),
                ImVec2(a_boxMax.x + outX, a_boxMax.y + outY),
                ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        }
        }   // haloBits

        // GI46: status rings -- shared implementation (see DrawStatusRings)
        DrawStatusRings(a_dl, a_bits, a_boxMin, a_boxMax);
    }

    void DrawItemTooltip(RE::TESBoundObject* a_obj, int a_count, int a_coinValue,
                         int a_price, bool a_isBuy, RE::TESObjectREFR* a_owner,
                         ExtraScope a_scope, std::uint16_t a_uid, int a_xlIdx,
                         std::uint16_t a_sig, int a_hand)
    {
        if (!a_obj) return;
        const auto& sk = Theme::S();
        ImGui::BeginTooltip();
        if (a_count > 1) {
            ImGui::TextColored(sk.hi, "%s  x%d", a_obj->GetName(), a_count);
        } else {
            ImGui::TextColored(sk.hi, "%s", a_obj->GetName());
        }
        if (a_coinValue >= 0) {   // G2: represented / stored gold
            ImGui::TextColored(sk.hi, "%dG", a_coinValue);
        }

        // one effect per line: "Name 50 (10s)" — shared by potions/enchants
        auto effectLine = [&](RE::Effect* a_e, const ImVec4& a_col) {
            if (!a_e || !a_e->baseEffect) return;
            const char* n = a_e->baseEffect->GetName();
            if (!n || !*n) return;
            const float mag = a_e->effectItem.magnitude;
            const std::uint32_t dur = a_e->effectItem.duration;
            char b[160];
            if (mag > 0.0f && dur > 0) {
                std::snprintf(b, sizeof(b), "%s %.0f (%us)", n, mag, dur);
            } else if (mag > 0.0f) {
                std::snprintf(b, sizeof(b), "%s %.0f", n, mag);
            } else if (dur > 0) {
                std::snprintf(b, sizeof(b), "%s (%us)", n, dur);
            } else {
                std::snprintf(b, sizeof(b), "%s", n);
            }
            ImGui::TextColored(a_col, "%s", b);
        };

        // The OWNER's inventory entry: poison/charge/soul/crafted-enchant extras
        // all live there, not on the base form.
        // D1: this used to read the player unconditionally, so a merchant's
        // ordinary sword displayed the player's own sword's extras.
        // The map OWNS the entry copies (unique_ptr) — it must outlive every
        // extraOf call below, or `entry` dangles (was a real CTD: freed
        // entry->extraLists reused by the heap mid-tooltip).
        auto* player = RE::PlayerCharacter::GetSingleton();   // SHIFT-compare / spell tome
        RE::TESObjectREFR* owner = a_owner;
        if (!owner) owner = player;
        RE::TESObjectREFR::InventoryItemMap inv;
        RE::InventoryEntryData* entry = nullptr;
        if (owner) {
            inv = owner->GetInventory(
                [&](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [o2, d2] : inv) {
                entry = d2.second.get();
                break;
            }
        }
        // GI1: and WHICH sub-stack of that entry. kAny keeps the historical
        // "first list carrying the trait" behaviour for aggregate cells.
        RE::ExtraDataList* scoped = nullptr;
        switch (a_scope) {
        case ExtraScope::kUnit: scoped = ExtraForTile(entry, a_uid, a_xlIdx); break;
        case ExtraScope::kWorn:
            scoped = WornExtraMatching(entry, a_uid, a_sig, a_hand);
            break;
        case ExtraScope::kAny:  break;
        }
        auto extraOf = [&]<class T>() -> T* {
            if (a_scope != ExtraScope::kAny) return scoped ? scoped->GetByType<T>() : nullptr;
            if (!entry || !entry->extraLists) return nullptr;
            for (auto* xl : *entry->extraLists) {
                if (!xl) continue;
                if (auto* x = xl->GetByType<T>()) return x;
            }
            return nullptr;
        };

        // Card values match vanilla only via the engine functions that fold in
        // the armor/weapon SKILL multiplier + perks + temper — GetArmorRating()/
        // GetAttackDamage() return only the base form value (Steel cuirass base
        // 31 vs card 31*(1+0.4*15/100)=32.86->round->33). PlayerCharacter::
        // GetArmorValue/GetDamage take an InventoryEntryData; a throwaway entry
        // (no extraLists = no temper) still applies skill+perks — which is what
        // the player asked to match. Round, don't truncate.
        // Diablo-style SHIFT compare: while shift is held over a weapon/armor,
        // find the equipped counterpart (same hand / overlapping biped slot),
        // append a signed diff to the stat line and show an "Equipped" card
        // beside this tooltip (rendered after EndTooltip below).
        RE::TESBoundObject* cmpObj = nullptr;
        int  cmpVal = 0;
        bool cmpIsWeap = false;
        const bool wantCmp = ImGui::GetIO().KeyShift;
        auto diffText = [&](int a_mine) {
            if (!cmpObj) return;
            const int d = a_mine - cmpVal;
            const ImVec4 c = d > 0 ? ImVec4(0.47f, 0.78f, 0.47f, 1.0f)
                           : d < 0 ? ImVec4(0.8f, 0.32f, 0.28f, 1.0f)
                                   : sk.inkDim;
            ImGui::SameLine();
            ImGui::TextColored(c, "(%+d)", d);
        };

        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
            int dmg = static_cast<int>(weap->GetAttackDamage());
            if (pc) {
                RE::InventoryEntryData e(a_obj, 1);
                dmg = static_cast<int>(std::lroundf(pc->GetDamage(&e)));
            }
            if (wantCmp && pc) {
                RE::TESForm* eq = pc->GetEquippedObject(false);
                if (!eq || !eq->As<RE::TESObjectWEAP>()) eq = pc->GetEquippedObject(true);
                if (auto* ew = eq ? eq->As<RE::TESObjectWEAP>() : nullptr) {
                    RE::InventoryEntryData ee(ew, 1);
                    cmpObj = ew;
                    cmpVal = static_cast<int>(std::lroundf(pc->GetDamage(&ee)));
                    cmpIsWeap = true;
                }
            }
            ImGui::TextColored(sk.inkDim, "%s %d", Lang::T(Lang::Str::Damage), dmg);
            diffText(dmg);
        } else if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            int arm = static_cast<int>(armo->GetArmorRating());
            if (pc) {
                RE::InventoryEntryData e(a_obj, 1);
                arm = static_cast<int>(std::lroundf(pc->GetArmorValue(&e)));
            }
            if (wantCmp && pc && player) {
                const auto hmask = static_cast<std::uint32_t>(armo->GetSlotMask());
                auto winv = player->GetInventory();
                for (auto& [o2, d2] : winv) {
                    auto* e2 = d2.second.get();
                    if (!o2 || !e2 || !e2->IsWorn()) continue;
                    auto* wa = o2->As<RE::TESObjectARMO>();
                    if (!wa) continue;
                    if ((static_cast<std::uint32_t>(wa->GetSlotMask()) & hmask) == 0) continue;
                    RE::InventoryEntryData ee(wa, 1);
                    cmpObj = wa;
                    cmpVal = static_cast<int>(std::lroundf(pc->GetArmorValue(&ee)));
                    break;
                }
            }
            ImGui::TextColored(sk.inkDim, "%s %d", Lang::T(Lang::Str::Armor), arm);
            diffText(arm);
        } else {
            RE::MagicItem* magic = a_obj->As<RE::AlchemyItem>();
            if (!magic) magic = a_obj->As<RE::IngredientItem>();
            if (magic) {
                for (auto* e : magic->effects) effectLine(e, sk.inkDim);
            }
        }

        // temper (grindstone / workbench). ExtraHealth::health is a damage/armour
        // MULTIPLIER: 1.0 = untempered, 1.25 = +25%. It gets its own line because
        // the stat line above is computed from a throwaway entry that carries no
        // extras by design -- and because tempering is the cheapest per-instance
        // difference a player can create, so this is what makes the GI1 tile
        // binding visible at all (before this, tempering showed NOTHING).
        if (const auto* xh = extraOf.operator()<RE::ExtraHealth>();
            xh && xh->health > 1.0f) {
            ImGui::TextColored(sk.sel, "%s +%d%%", Lang::T(Lang::Str::TemperLabel),
                static_cast<int>(std::lroundf((xh->health - 1.0f) * 100.0f)));
        }

        // spell tome: what it teaches (right-click learns it), known marker
        if (auto* book = const_cast<RE::TESObjectBOOK*>(a_obj->As<RE::TESObjectBOOK>());
            book && book->TeachesSpell()) {
            if (auto* spell = book->GetSpell()) {
                const bool known = player && player->HasSpell(spell);
                ImGui::TextColored(sk.sel, "%s: %s%s%s%s",
                    Lang::T(Lang::Str::Teaches), spell->GetName(),
                    known ? " (" : "", known ? Lang::T(Lang::Str::Known) : "",
                    known ? ")" : "");
            }
        }

        // enchantment — base record (EITM) or player-crafted (ExtraEnchantment)
        {
            RE::EnchantmentItem* ench = nullptr;
            std::uint16_t maxCharge = 0;
            if (const auto* ef = a_obj->As<RE::TESEnchantableForm>()) {
                ench = ef->formEnchanting;
                maxCharge = ef->amountofEnchantment;
            }
            if (auto* xe = extraOf.operator()<RE::ExtraEnchantment>();
                xe && xe->enchantment) {
                ench = xe->enchantment;
                maxCharge = xe->charge;
            }
            if (ench) {
                for (auto* e : ench->effects) effectLine(e, sk.sel);
                // charge (weapons drain per hit; armour enchants don't)
                if (a_obj->Is(RE::FormType::Weapon) && maxCharge > 0) {
                    float cur = static_cast<float>(maxCharge);
                    if (auto* xc = extraOf.operator()<RE::ExtraCharge>()) {
                        cur = xc->charge;
                    }
                    ImGui::TextColored(sk.inkDim, "%s %d / %d",
                        Lang::T(Lang::Str::ChargeLabel),
                        static_cast<int>(cur), static_cast<int>(maxCharge));
                }
            }
        }

        // applied poison (weapons)
        if (auto* xp = extraOf.operator()<RE::ExtraPoison>(); xp && xp->poison) {
            ImGui::TextColored(sk.sel, "%s: %s (x%u)",
                Lang::T(Lang::Str::PoisonLabel), xp->poison->GetName(), xp->count);
        }

        // soul gem fill state (entry override wins over the base record)
        if (const auto* gem = a_obj->As<RE::TESSoulGem>()) {
            RE::SOUL_LEVEL lvl = gem->GetContainedSoul();
            if (auto* xs = extraOf.operator()<RE::ExtraSoul>()) {
                lvl = xs->GetContainedSoul();
            }
            if (lvl != RE::SOUL_LEVEL::kNone) {
                static constexpr Lang::Str kSoulNames[] = {
                    Lang::Str::SoulPetty, Lang::Str::SoulPetty, Lang::Str::SoulLesser,
                    Lang::Str::SoulCommon, Lang::Str::SoulGreater, Lang::Str::SoulGrand,
                };
                const auto idx = (std::min)(static_cast<size_t>(lvl),
                    std::size(kSoulNames) - 1);
                ImGui::TextColored(sk.inkDim, "%s: %s",
                    Lang::T(Lang::Str::SoulLabel), Lang::T(kSoulNames[idx]));
            }
        }

        // flavour/effect description (artifacts, uniques) — books excluded,
        // their DESC is the whole book text
        if (!a_obj->Is(RE::FormType::Book)) {
            if (auto* desc = a_obj->As<RE::TESDescription>()) {
                RE::BSString out;
                desc->GetDescription(out, a_obj->As<RE::TESForm>());
                if (out.size() > 0 && out.c_str() && *out.c_str()) {
                    ImGui::PushTextWrapPos(300.0f * Theme::Scale());
                    ImGui::TextColored(sk.inkDim, "%s", out.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        }

        // weight is meaningless under the space system (W1) — value only.
        // GI43: THIS unit's value (temper folded in, vanilla parity) -- the
        // scoped list is already resolved above; kAny falls back to the base.
        ImGui::TextColored(sk.inkDim, "%s %d",
            Lang::T(Lang::Str::Value), UnitValueWith(a_obj, scoped));

        // Phase 4: barter price (buy on the merchant side, sell on the player
        // side) — crimson when the payer can't afford it (design pass E)
        if (a_price >= 0) {
            ImGui::Separator();
            const bool broke = a_isBuy
                ? g_gold < a_price
                : (a_price > 0 && LootBarter::MerchantGold() < a_price);
            ImGui::TextColored(broke ? ImVec4(0.8f, 0.32f, 0.28f, 1.0f) : sk.hi,
                "%s: %d",
                Lang::T(a_isBuy ? Lang::Str::BuyLabel : Lang::Str::SellLabel), a_price);
        }
        // discoverability: the 3D inspect key is quest-critical (claw glyphs)
        ImGui::TextColored(sk.inkDim, "%s", Lang::T(Lang::Str::InspectKeyHint));
        const ImVec2 tipPos = ImGui::GetWindowPos();
        const ImVec2 tipSize = ImGui::GetWindowSize();
        ImGui::EndTooltip();

        // SHIFT compare: the equipped counterpart's card beside the tooltip.
        // Drawn on the FOREGROUND draw list — always above every window (a
        // plain window sank behind the partner window, and a second
        // BeginTooltip APPENDS to the same tooltip in this ImGui version,
        // which stretched the main box instead of making a card). Flips to
        // the LEFT of the tooltip when the right side would leave the screen.
        if (cmpObj) {
            const char* en = cmpObj->GetName();
            if (!en || !*en) en = "?";
            char statBuf[64];
            std::snprintf(statBuf, sizeof(statBuf), "%s %d",
                Lang::T(cmpIsWeap ? Lang::Str::Damage : Lang::Str::Armor), cmpVal);
            char valBuf[64];
            std::snprintf(valBuf, sizeof(valBuf), "%s %d",
                Lang::T(Lang::Str::Value), cmpObj->GetGoldValue());

            const ImVec2 pad = ImGui::GetStyle().WindowPadding;
            const float lh = ImGui::GetTextLineHeightWithSpacing();
            const float cardW = (std::max)({
                ImGui::CalcTextSize(Lang::T(Lang::Str::EquippedLabel)).x,
                ImGui::CalcTextSize(en).x,
                ImGui::CalcTextSize(statBuf).x,
                ImGui::CalcTextSize(valBuf).x }) + pad.x * 2.0f;
            const float cardH = 4.0f * lh + pad.y * 2.0f;

            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            float cx = tipPos.x + tipSize.x + 6.0f;
            if (cx + cardW > disp.x) cx = tipPos.x - cardW - 6.0f;   // flip left
            if (cx < 0.0f) cx = 0.0f;
            float cy = tipPos.y;
            if (cy + cardH > disp.y) cy = (std::max)(0.0f, disp.y - cardH);

            auto* fdl = ImGui::GetForegroundDrawList();
            const ImVec2 c0(cx, cy);
            const ImVec2 c1(cx + cardW, cy + cardH);
            fdl->AddRectFilled(c0, c1, Theme::Col(sk.winBg, 1.0f), sk.rounding);
            fdl->AddRect(c0, c1, Theme::Col(sk.acc, 0.8f), sk.rounding);
            float ty = cy + pad.y;
            auto line = [&](const char* a_txt, const ImVec4& a_col) {
                fdl->AddText(ImVec2(cx + pad.x, ty), ImGui::GetColorU32(a_col), a_txt);
                ty += lh;
            };
            line(Lang::T(Lang::Str::EquippedLabel), sk.inkDim);
            line(en, sk.hi);
            line(statBuf, sk.inkDim);
            line(valBuf, sk.inkDim);
        }
    }

    void Draw()
    {
        if (g_views.empty()) return;
        const float gridW = g_views[0].cols * CellPx();
        // exact width; overflow rows (scripted adds) still wheel-scroll, the
        // bar itself is hidden so it never eats into the right margin.
        // Height is PINNED to the hard board: a fill-height child let the
        // overflow rows spill OVER the GOLD bar strip below (user-reported) —
        // now they clip + scroll inside instead.
        const ImVec2 clipTop = ImGui::GetCursorScreenPos();   // grid origin
        const float boardH = kMinRows * CellPx() + 1.0f;
        ImGui::BeginChild("fablerim_grid", ImVec2(gridW, boardH), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar);
        // ROOT CAUSE of the "items spill past the frame" saga: the grid's
        // draw commands were NOT clipped to this child (overflow rows — and,
        // once wheel-scrolled, even the top rows — rendered right through
        // the window). Clip every grid pass to the 10x14 board explicitly;
        // a small margin keeps the rarity glow's soft bleed from cutting
        // square at the edges.
        {
            auto* cdl = ImGui::GetWindowDrawList();
            const float m = 10.0f * Theme::Scale();
            cdl->PushClipRect(ImVec2(clipTop.x - m, clipTop.y - m),
                ImVec2(clipTop.x + gridW + m, clipTop.y + boardH + m), true);
            DrawGridView(g_views[0], 0);
            // Second border ON TOP of the item sprites (they z-cover the
            // chrome outline): fixed to the 10x14 board — same colour /
            // thickness / rect as the chrome outline (skin-aware). Must be
            // on THIS child's list AFTER the passes: the child renders
            // above its parent, so a parent-list border sat under the items
            // (user-reported). Other windows / the carried cursor icon
            // still draw above.
            // stronger alpha than the inner cell lines (13%) / old outline
            // (30%) so the board edge stays readable over item sprites
            cdl->AddRect(clipTop, ImVec2(clipTop.x + gridW, clipTop.y + boardH),
                Theme::Acc(0.45f));
            // +1px OUTWARD (user request): a second ring just outside the
            // board line — 2px total, growing out, not into the cells
            cdl->AddRect(ImVec2(clipTop.x - 1.0f, clipTop.y - 1.0f),
                ImVec2(clipTop.x + gridW + 1.0f, clipTop.y + boardH + 1.0f),
                Theme::Acc(0.45f));
            cdl->PopClipRect();
        }
        ImGui::EndChild();
    }

    // G2: coin-pouch withdraw window — same construction as the settings /
    // loadout confirm windows (fixed size + TitleBar + centred content).
    bool ClosePouch()
    {
        if (!g_pouchOpen) return false;
        g_pouchOpen = false;
        return true;
    }

    void DrawPouchWindow()
    {
        if (!g_pouchOpen) return;
        if (!GoldCoins::Ready()) { g_pouchOpen = false; return; }

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;
        const int stored = GoldCoins::PouchStored();
        if (g_pouchSlider > stored) g_pouchSlider = stored;

        char line[64];
        std::snprintf(line, sizeof(line), "%s: %d / %dG",
            Lang::T(Lang::Str::StoredLabel), stored, GoldCoins::PouchCap());
        const float sliderW = 220.0f * S;
        const float contentW = (std::max)({ btnRow, sliderW,
            ImGui::CalcTextSize(line).x });
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + ImGui::GetFrameHeight() + 6.0f * S +
                2.0f * sp + ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        wm->ApplyNext("pouch",
            ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f), size);
        ImGui::Begin("##grid_pouch", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        auto* pouch = GoldCoins::PouchForm();
        wm->TitleBar("pouch", pouch && pouch->GetName() ? pouch->GetName() : "?",
            0.0f, true);

        if (!ImGui::IsWindowAppearing() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered()) {
            g_pouchOpen = false;
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(line).x);
        ImGui::TextColored(sk.ink, "%s", line);
        center(sliderW);
        const int drawBefore = g_pouchSlider;
        if (Theme::ChromeSliderInt("##pouchdraw", &g_pouchSlider, 0, stored, sliderW, "%dG") &&
            g_pouchSlider != drawBefore) {
            static double s_lastTick = 0.0;
            const double now = ImGui::GetTime();
            if (now - s_lastTick > 0.06) {
                s_lastTick = now;
                if (g_pouchSlider > drawBefore) Sfx::SelectOn();
                else                            Sfx::SelectOff();
            }
        }
        // GI51: LEFT/RIGHT nudge by 1G (hold repeats; A/D arrive as arrows).
        // GI52: keys stand down while a text field owns the keyboard.
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && g_pouchSlider > 0) {
            --g_pouchSlider;
            Sfx::SelectOff();
        }
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && g_pouchSlider < stored) {
            ++g_pouchSlider;
            Sfx::SelectOn();
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        const bool can = g_pouchSlider > 0;
        // GI51: Enter/Space confirm (ESC already closes via CloseTopWindow)
        const bool keyOk = !typing &&
                           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Space, false));
        ImGui::BeginDisabled(!can);
        if (Sfx::Button(Lang::T(Lang::Str::Withdraw), ImVec2(btnW, 0)) || (can && keyOk)) {
            // the withdrawn amount rides the CURSOR as a pinned purse (same
            // flow as a stack split) instead of dropping straight into the
            // inventory. Carry caps at one purse (kCoinCap); any excess of a
            // larger withdrawal lands in the inventory as walking gold.
            const int v = g_pouchSlider;
            GoldCoins::Withdraw(v, false);   // pickup sound plays instead
            const int carry = (std::min)(v, GoldCoins::kCoinCap);
            if (auto* cform = GoldCoins::CoinForTier(GoldCoins::BandTier(carry))) {
                PickupPartial(cform, carry, {}, 0);   // pins from walking
            }
            g_pouchOpen = false;             // window closes; the purse rides
            g_pouchSlider = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 8.0f * S);
        if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
            g_pouchOpen = false;
        }
        ImGui::End();
    }

    void DrawBagWindows()
    {
        auto* wm = WinManager::GetSingleton();

        const float S = Theme::Scale();
        for (int vi = 1; vi < static_cast<int>(g_views.size()); ++vi) {
            auto& v = g_views[vi];
            // symmetric 12px margins around the bag grid (scale-aware) +
            // 2x frame inset for tornFrame skins (breathing room)
            const ImVec2 size(v.cols * CellPx() + 24.0f * S + 2.0f * Theme::FrameInsetX(),
                              v.rows * CellPx() + 54.0f * S + 2.0f * Theme::FrameInsetY());

            // default: flow to the right of the main window (E5)
            ImVec2 defPos(200.0f + vi * 60.0f, 200.0f);
            if (auto* mw = wm->Find("main")) {
                defPos = ImVec2(mw->pos.x + mw->size.x + 8.0f,
                                mw->pos.y + (vi - 1) * 60.0f);
            }
            wm->ApplyNext(v.bagKey, defPos, size);

            ImGui::Begin(("##bag_" + v.bagKey).c_str(), nullptr, kManagedWinFlags);
            wm->TitleBar(v.bagKey, v.bagName.c_str());
            // F2: dim crimson border marks the trash apart from real bags
            if (v.bagKey == kTrashKey) {
                auto* wdl = ImGui::GetWindowDrawList();
                const ImVec2 wp = ImGui::GetWindowPos();
                const ImVec2 we(wp.x + size.x, wp.y + size.y);
                wdl->PushClipRect(wp, we, false);
                wdl->AddRect(wp, we, IM_COL32(140, 40, 30, 150), Theme::S().rounding,
                    0, 2.0f);
                wdl->PopClipRect();
            }
            DrawGridView(v, vi);
            ImGui::End();
        }
    }

    namespace
    {
        // G4: pin a gold value onto a grid position, choosing the coin form by
        // the value's band (auto-repins to the right form when a merge crosses
        // a band). Returns the new tile key.
        std::string PlacePin(int a_value, int a_col, int a_row, const std::string& a_bag)
        {
            auto* f = GoldCoins::CoinForTier(GoldCoins::BandTier(a_value));
            if (!f) return {};
            const std::string key = NextTileKey(FormKey(f));
            GoldCoins::PinAmount(key, a_value);
            PlaceTile(key, a_col, a_row, a_bag, 1);
            return key;
        }

        // Phase 2: the ONE gold-on-gold merge (formerly two drifting copies —
        // fragment-onto-pin vs whole-tile-onto-tile). Both tiles are consumed,
        // up-to-cap lands as a pinned purse at the TARGET's cell (fallback:
        // the drop cell), the remainder keeps riding the cursor as a fresh
        // pin. Unpinning both first keeps the walking-gold ledger exact, so
        // sibling auto coins never reshuffle. a_held must be g_held's value.
        void MergeGoldInto(Held& a_held, const std::string& a_tgtKey, int a_tgtValue,
                           const LayoutEntry& a_fallbackPos)
        {
            const int combined = a_tgtValue + a_held.coinValue;
            const int placed = (std::min)(combined, GoldCoins::kCoinCap);
            const int leftover = combined - placed;
            LayoutEntry pos = a_fallbackPos;
            if (auto li = g_layout.find(a_tgtKey); li != g_layout.end()) pos = li->second;
            if (GoldCoins::PinnedValue(a_tgtKey) >= 0) GoldCoins::UnpinTile(a_tgtKey);
            g_layout.erase(a_tgtKey);
            if (GoldCoins::PinnedValue(a_held.key) >= 0) GoldCoins::UnpinTile(a_held.key);
            g_layout.erase(a_held.key);
            PlacePin(placed, pos.col, pos.row, pos.bag);
            if (g_sound) g_sound(a_held.obj, false);
            if (leftover > 0) {   // remainder keeps riding as a pin
                auto* lf = GoldCoins::CoinForTier(GoldCoins::BandTier(leftover));
                const std::string lk = NextTileKey(FormKey(lf));
                GoldCoins::PinAmount(lk, leftover);
                a_held.key = lk;
                a_held.obj = lf;
                a_held.coinValue = leftover;
                a_held.preSplit = true;   // fragment rules from here on
                a_held.mask = MaskOf(g_resolver ? g_resolver(lf) : GridDef{});
            } else {
                g_held.reset();
            }
        }
    }

    // ================= Phase 3: drop-target dispatch table =================
    // FinishFrame's 6~7-deep else-if drop state machine, re-expressed as
    // (held kind -> ordered route list). A route row = WHERE the drop landed
    // + a handler; the FIRST matching row whose handler returns true resolves
    // the drop, false falls through to the next row, and no consuming row =
    // keep carrying. Adding a new drop TARGET (e.g. the F2 trash window) is
    // one DropWhere case + one row per held kind that accepts it. Handler
    // bodies moved VERBATIM from the old chain — behaviour unchanged.
    namespace
    {
        // held icon rides the cursor above every window
        void DrawHeldCursorIcon(Held& a_held)
        {
            const auto& io = ImGui::GetIO();
            auto* fg = ImGui::GetForegroundDrawList();
            const float w = a_held.mask.w * CellPx();
            const float h = a_held.mask.h * CellPx();
            const ImVec2 a(io.MousePos.x - a_held.offX, io.MousePos.y - a_held.offY);
            RE::TESBoundObject* heldIconObj = a_held.obj;
            if (GoldCoins::IsPouch(a_held.obj->GetFormID())) {
                if (auto* v = GoldCoins::PouchIconObject()) heldIconObj = v;
            }
            auto* hc = IconCache::GetSingleton();
            const IconCache::Icon* heldIcon = hc->Get(heldIconObj);
            if (!heldIcon && heldIconObj != a_held.obj) {
                hc->QueueCapture(heldIconObj);
                heldIcon = hc->Get(a_held.obj);   // base icon until captured
            }
            if (const auto* icon = heldIcon) {
                const float target = (std::max)(w, h) * 0.95f * a_held.defScale;
                const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                const float dw = icon->w / ms * target;
                const float dh = icon->h / ms * target;
                const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.5f);
                UIRoot::DrawItemIcon(fg, icon->srv,
                    ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                    ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f));
            } else {
                fg->AddRect(a, ImVec2(a.x + w, a.y + h), IM_COL32(220, 200, 140, 200), 3.0f);
            }
        }

        enum class DropWhere : std::uint8_t
        {
            kEquipSlot,       // hovered doll slot (g_slotTarget)
            kTrashArea,       // F2: any cell of the trash view (park intake)
            kEmptyCell,       // grid cell, empty & item fits
            kBlockerSingle,   // grid cell occupied by exactly one item
            kCellArea,        // anywhere over a grid (valid or not) — TERMINAL
                              // for whole tiles (no fallthrough to partner)
            kPartnerLoot,     // container window hovered (loot mode)
            kPartnerBarter,   // merchant window hovered (barter mode)
            kPartnerPickpocket,   // F6b: mark's window hovered (reverse lift)
            kVoid,            // outside every window
            kAlways,
        };

        bool DropWhereMatches(DropWhere a_where)
        {
            const bool cell = g_target.has &&
                              g_target.view < static_cast<int>(g_views.size());
            switch (a_where) {
            case DropWhere::kEquipSlot:
                return !g_slotTarget.empty();
            case DropWhere::kTrashArea:
                return cell && g_views[g_target.view].bagKey == kTrashKey;
            case DropWhere::kEmptyCell:
                return cell && g_target.valid;
            case DropWhere::kBlockerSingle:
                return cell && !g_target.valid && g_target.blockers.size() == 1;
            case DropWhere::kCellArea:
                return cell;
            case DropWhere::kPartnerLoot:
                return LootBarter::IsPartnerHovered() &&
                       LootBarter::IsLootMode(LootBarter::CurrentMode());
            case DropWhere::kPartnerBarter:
                return LootBarter::IsPartnerHovered() &&
                       LootBarter::CurrentMode() == LootBarter::Mode::kBarter;
            case DropWhere::kPartnerPickpocket:
                return LootBarter::IsPartnerHovered() &&
                       LootBarter::CurrentMode() == LootBarter::Mode::kPickpocket;
            case DropWhere::kVoid:
                // AllowWhenBlockedByActiveItem: the drop CLICK activates the
                // tile button under the cursor — plain IsWindowHovered then
                // reported "no window" and DISCARDED in-window drops
                return !ImGui::IsWindowHovered(
                    ImGuiHoveredFlags_AnyWindow |
                    ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            case DropWhere::kAlways:
                return true;
            }
            return false;
        }

        // ---- PARTNER item (loot take / barter buy with direct placement) ----
        // Phase 5-B: lands exactly on the dropped cell (main grid OR a bag
        // window), not first-fit. Only a valid (empty, fits) cell accepts it;
        // a blocked cell, the partner window, or outside cancels (nothing
        // moved). Bag intake: dropping onto a bag's free cell routes the item
        // into that bag. Buys still gold-check.
        bool DropPartnerHeld(Held& a_held)
        {
            // F7 (kLoot/kSteal): dropping a partner-carried item back ON the
            // partner grid REARRANGES the container — empty cell = move, on
            // another item = swap (mirrors the player-grid grammar). Chrome
            // (titlebar / bottom bar) cancels back to its spot.
            if (LootBarter::IsPartnerHovered() &&
                LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                const auto sd = LootBarter::QueryStoreDrop();
                if (sd.onCell && sd.freeSpot) {
                    LootBarter::NoteStoreSpot(a_held.obj, sd.col, sd.row, HeldInstanceSig());
                    if (g_sound) g_sound(a_held.obj, false);
                    g_held.reset();
                } else if (sd.onCell && sd.occ) {
                    // swap: the held item takes the occupant's anchor, the
                    // occupant rides the cursor in its place
                    LootBarter::NoteStoreSpot(a_held.obj, sd.occCol, sd.occRow, HeldInstanceSig());
                    if (g_sound) g_sound(a_held.obj, false);
                    const auto occ = sd;   // copy: reset() invalidates the query
                    g_held.reset();
                    // GI24: the displaced occupant keeps its OWN identity and its
                    // OWN pool slot. Picking it up anonymously left its slot in
                    // the pool unreserved -- position order then handed that slot
                    // to a sibling, and the occupant itself came back down as a
                    // fresh arrival in the first free cell.
                    BeginPartnerCarry(occ.occ, occ.occCount, occ.occValue,
                                      -1.0f, -1.0f,
                                      occ.occUid, occ.occXlIdx, occ.occOrd);
                    LootBarter::NoteCarriedSpot(occ.occSpotKey);
                } else if (sd.onCell) {
                    // 2+ blockers: keep carrying (player-grid parity)
                } else {
                    g_held.reset();   // window chrome: cancel back
                }
                g_needRebuild = true;
                return true;
            }
            // F7-swap: a partner item dropped on a SINGLE occupied player tile
            // takes/buys into that spot and the displaced tile rides the
            // cursor (player-grid C4 grammar). Coins/pouch keep the old no-op.
            const Item* swapDisp = nullptr;
            if (g_target.has && !g_target.valid && g_target.blockers.size() == 1 &&
                !LootBarter::IsPartnerHovered() &&
                g_target.view < static_cast<int>(g_views.size())) {
                const Item& cand = g_items[g_target.blockers.front()];
                if (!GoldCoins::IsCoinForm(cand.obj->GetFormID()) &&
                    cand.coinValue < 0) {
                    swapDisp = &cand;
                }
            }
            if (g_target.has && (g_target.valid || swapDisp) &&
                !LootBarter::IsPartnerHovered() &&
                g_target.view < static_cast<int>(g_views.size())) {
                const auto& v = g_views[g_target.view];
                // F2: a partner item can't be taken INTO the trash — cancel
                if (v.bagKey == kTrashKey) {
                    g_held.reset();
                    g_needRebuild = true;
                    return true;
                }
                const int cnt = a_held.count;
                bool ok = true;
                if (LootBarter::CurrentMode() == LootBarter::Mode::kBarter && cnt <= 1) {
                    const int total = LootBarter::BuyPrice(a_held.obj, a_held.partnerValue);
                    if (GoldAmount() < total) {
                        Sfx::FailNote(Lang::T(Lang::Str::NotEnoughGold));
                        ok = false;
                    }
                }
                if (ok) {
                    if (LootBarter::IsLootMode(LootBarter::CurrentMode())) {
                        if (cnt > 1) LootBarter::OpenSlider(a_held.obj, cnt,
                            LootBarter::XferDir::kTake, {}, 0, a_held.uid, a_held.sig);
                        else LootBarter::RequestTake(a_held.obj, cnt,
                                                     a_held.uid, a_held.sig);
                    } else if (LootBarter::CurrentMode() ==
                               LootBarter::Mode::kPickpocket) {
                        // F6b: dragging out of a mark's pockets rolls too
                        if (cnt > 1) LootBarter::OpenSlider(a_held.obj, cnt,
                            LootBarter::XferDir::kPickTake, {}, 0, a_held.uid, a_held.sig);
                        else LootBarter::RequestPickTake(a_held.obj, cnt, a_held.uid, a_held.sig);
                    } else {   // kBarter
                        if (cnt > 1) LootBarter::OpenSlider(a_held.obj, cnt,
                            LootBarter::XferDir::kBuy, {}, a_held.partnerValue,
                            a_held.uid, a_held.sig);
                        else {
                            const int total = LootBarter::BuyPrice(a_held.obj, a_held.partnerValue);
                            LootBarter::RequestBuy(a_held.obj, 1, total, a_held.partnerValue,
                                                   a_held.uid, a_held.sig);
                        }
                    }
                    // B2: drop-cell placement as a one-shot HINT for the
                    // ACQUIRE pass, NOT a premature layout write — the old
                    // direct write clobbered an existing stack's saved
                    // position and leaked a full-count reservation when the
                    // quantity slider was cancelled.
                    const std::string hintBase = FormKey(a_held.obj);
                    // stale layout entries of this form (no live tile — e.g.
                    // the stack was sold out earlier this session) would win
                    // over the hint on rebuild and pull the purchase back to
                    // its OLD remembered cell — the explicit drop position
                    // must decide, so purge them
                    if (!GoldCoins::IsCoinForm(a_held.obj->GetFormID())) {
                        std::set<std::string> liveKeys;
                        for (const auto& gi : g_items) {
                            if (BaseKey(gi.key) == hintBase) liveKeys.insert(gi.key);
                        }
                        for (auto lt = g_layout.begin(); lt != g_layout.end();) {
                            if (BaseKey(lt->first) == hintBase &&
                                !liveKeys.contains(lt->first)) {
                                lt = g_layout.erase(lt);
                            } else {
                                ++lt;
                            }
                        }
                    }
                    g_dropHint = { hintBase,
                                   g_target.col, g_target.row, v.bagKey };
                    if (swapDisp) {
                        // free the displaced tile's spot for the incoming item
                        // and put it on the cursor (same as the C4 swap)
                        const Item d = *swapDisp;   // copy: reset invalidates it
                        g_layout.erase(d.key);
                        g_held.reset();
                        g_held = Held{ d.key, d.obj, d.mask, d.count,
                                       d.def.bag != 0, d.def.scale,
                                       d.mask.w * CellPx() * 0.5f,
                                       d.mask.h * CellPx() * 0.5f, true };
                        g_held->coinValue = d.coinValue;
                        g_held->xlIdx = d.xlIdx;   // GI1
                        g_held->uid = d.uid;
                        g_held->sig = d.sig;       // GI25
                        g_held->fav = d.fav;       // GI36
                        if (g_sound) g_sound(d.obj, true);
                        g_needRebuild = true;
                        return true;
                    }
                }
            }
            g_held.reset();
            g_needRebuild = true;
            return true;
        }

        // ---- GOLD fragment (G4; held.key is its pin) ----
        bool GoldFragOnEmptyCell(Held& a_held)
        {
            // (1) empty cell -> anchor the pin here
            const auto& v = g_views[g_target.view];
            PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, 1);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            return true;
        }

        bool GoldFragOnBlocker(Held& a_held)
        {
            const auto& v = g_views[g_target.view];
            const Item tgt = g_items[g_target.blockers.front()];
            const RE::FormID tf = tgt.obj->GetFormID();
            const bool tgtGold = GoldCoins::IsCoinForm(tf) &&
                                 !GoldCoins::IsPouch(tf) && tgt.coinValue >= 0;
            if (GoldCoins::IsPouch(tf)) {
                // (2c) dropped ON the pouch -> store the value (mirrors the
                // whole-tile rule: the pin returns to walking first so the
                // pouch can draw from it; no room -> the pin restores)
                const int v2 = a_held.coinValue;
                GoldCoins::UnpinTile(a_held.key);
                if (GoldCoins::StoreToPouch(v2) > 0) {
                    g_layout.erase(a_held.key);
                    if (g_sound) g_sound(a_held.obj, false);
                    g_held.reset();
                } else {
                    GoldCoins::PinAmount(a_held.key, v2);
                }
            } else if (tgtGold) {
                // (2) merge onto ANY gold tile (pin or auto) — the value
                // lands as a pinned purse at the target's cell
                MergeGoldInto(a_held, tgt.key, tgt.coinValue,
                    { g_target.col, g_target.row, v.bagKey, 1 });
            } else {
                // (2d) non-gold item -> SWAP (same rule as a whole tile):
                // the pin anchors at the drop cell, the displaced item
                // rides the cursor
                g_layout.erase(tgt.key);
                PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, 1);
                if (g_sound) {
                    g_sound(a_held.obj, false);
                    g_sound(tgt.obj, true);
                }
                g_held = Held{ tgt.key, tgt.obj, tgt.mask, tgt.count,
                               tgt.def.bag != 0, tgt.def.scale,
                               tgt.mask.w * CellPx() * 0.5f,
                               tgt.mask.h * CellPx() * 0.5f, true };
                g_held->coinValue = tgt.coinValue;
                g_held->xlIdx = tgt.xlIdx;   // GI1
                g_held->uid = tgt.uid;
                g_held->sig = tgt.sig;       // GI25
                g_held->fav = tgt.fav;       // GI36
            }
            return true;
        }

        bool GoldFragToVoid(Held& a_held)
        {
            GoldCoins::UnpinTile(a_held.key);   // back to walking first
            if (LootBarter::CurrentMode() == LootBarter::Mode::kNormal) {
                GoldCoins::DropAsGold(a_held.coinValue);   // (4) discard the gold
            }
            // loot/barter: (5) cancel — the value is already back in walking
            g_layout.erase(a_held.key);
            g_held.reset();
            return true;
        }

        // Same FORM is not the same POOL. A tempered dagger and a plain one
        // share a base form yet can never stack, so testing the form alone sent
        // them down the MERGE path -- which, for gear with a stack cap of 1,
        // absorbs nothing and returns false. The drop simply did nothing and the
        // two would not swap. Pools answer it correctly: same pool -> merge,
        // different pool -> swap, exactly as with two unrelated items.
        std::string HeldPool(const Held& a_held)
        {
            return PoolPrefix(FormKey(a_held.obj), a_held.uid, a_held.sig);
        }

        // ---- STACK fragment (G4 split; no key yet) ----
        bool StackFragOnEmptyCell(Held& a_held)
        {
            // (1) empty cell -> new tile owning this quantity
            const auto& v = g_views[g_target.view];
            const std::string nk = NextTileKey(HeldPool(a_held));
            PlaceTile(nk, g_target.col, g_target.row, v.bagKey, a_held.count);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            return true;
        }

        bool StackFragOnBlocker(Held& a_held)
        {
            const auto& v = g_views[g_target.view];
            const Item tgt = g_items[g_target.blockers.front()];
            // Same pool AND stackable -> merge. Same pool but NOT stackable (two
            // identical daggers) has no merge to do: the old code took the merge
            // branch, computed room = cap(1) - 1 = 0, and returned false, so the
            // drop did nothing at all and the item stayed stuck on the cursor.
            // Interchangeable or not, the player asked for these two cells to
            // trade places -- fall through to the swap.
            if (PoolOfKey(tgt.key) == HeldPool(a_held) && EffectiveCap(a_held.obj) > 1) {
                // (2) merge into the same-form stack up to cap (full target:
                // fall through — later rows won't match over the grid, so the
                // fragment keeps carrying instead of a pointless swap)
                const int cap = EffectiveCap(a_held.obj);
                const int room = (std::max)(0, cap - tgt.count);
                const int absorbed = (std::min)(room, a_held.count);
                if (absorbed <= 0) return false;
                g_layout[tgt.key].count = tgt.count + absorbed;
                a_held.count -= absorbed;
                if (g_sound) g_sound(a_held.obj, false);
                if (a_held.count <= 0) g_held.reset();
                return true;   // leftover (if any) keeps carrying
            }
            // (2b) different-form tile -> SWAP (same rule as a whole tile):
            // the fragment becomes a NEW tile at the drop cell, the
            // displaced item rides the cursor
            const std::string nk = NextTileKey(HeldPool(a_held));
            ParkTile(tgt.key);   // survives on the cursor -- keep its flags
            PlaceTile(nk, g_target.col, g_target.row, v.bagKey, a_held.count);
            if (g_sound) {
                g_sound(a_held.obj, false);
                g_sound(tgt.obj, true);
            }
            g_held = Held{ tgt.key, tgt.obj, tgt.mask, tgt.count,
                           tgt.def.bag != 0, tgt.def.scale,
                           tgt.mask.w * CellPx() * 0.5f,
                           tgt.mask.h * CellPx() * 0.5f, true };
            g_held->coinValue = tgt.coinValue;
            g_held->xlIdx = tgt.xlIdx;   // GI1
            g_held->uid = tgt.uid;
            g_held->sig = tgt.sig;       // GI25
            g_held->fav = tgt.fav;       // GI36
            return true;
        }

        bool FragQuestCancel(Held& a_held)
        {
            // Phase 7: quest items can't leave the inventory — an unresolved
            // quest fragment cancels back to its spot (Rebuild restores)
            if (!g_questItem.contains(HeldPoolKey(a_held))) return false;
            Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
            g_held.reset();
            return true;
        }

        bool StackFragStore(Held& a_held)
        {
            LootBarter::RequestStore(a_held.obj, a_held.count,
                                     HeldUidOf(a_held.key, a_held.uid), a_held.sig,
                                     a_held.fav);   // (3) store
            // fragment (empty key) = form-level pending only
            NotePendingRemove(a_held.obj, a_held.key, a_held.count);
            g_held.reset();
            return true;
        }

        bool StackFragSell(Held& a_held)
        {
            // (3b) SELL the fragment by dropping it on the merchant window —
            // the quantity was already chosen at the split slider, so no
            // second slider. The source tile is short by N since the split,
            // so a refusal (reset) restores it via ACQUIRE.
            const RE::FormID fid = a_held.obj->GetFormID();
            const int val = TileValue(a_held.obj,                          // GI43
                HeldUidOf(a_held.key, a_held.uid), a_held.sig);
            if (!LootBarter::MerchantBuys(a_held.obj, g_stolen.contains(
                    PoolPrefix(FormKey(a_held.obj), a_held.uid, a_held.sig)))) {
                Sfx::FailNote(Lang::T(Lang::Str::MerchantWontBuy));
            } else {
                const int total = val > 0
                    ? LootBarter::SellPriceTotal(a_held.obj, val, a_held.count) : 0;
                if (total > 0 && LootBarter::MerchantGold() < total) {
                    Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                } else {
                    LootBarter::RequestSell(a_held.obj, a_held.count,
                        total, val * a_held.count, a_held.uid, a_held.sig, a_held.fav);
                    // GI25: the split fragment still belongs to a POOL, and the
                    // pending bookkeeping has to say which one -- an empty key
                    // fell back to "deduct from the plain pool", the same
                    // mis-attribution that made a stored tempered dagger take a
                    // plain one's slot on its way out.
                    NotePendingRemove(a_held.obj, a_held.key, a_held.count);
                }
            }
            g_held.reset();
            return true;
        }

        bool StackFragToVoid(Held& a_held)
        {
            if (LootBarter::CurrentMode() == LootBarter::Mode::kNormal) {
                if (g_dropWorld) {
                    g_dropWorld(a_held.obj, a_held.count,   // GI36
                        ResolveExitUnit(a_held.obj, a_held.uid, a_held.sig, a_held.count,
                                        a_held.fav ? a_held.count : 0));
                }   // (4) discard N
            }
            g_held.reset();   // (5) loot/barter: cancel (Rebuild restores N)
            return true;
        }

        // ---- WHOLE tile ----
        bool WholeOnEquipSlot(Held& a_held)
        {
            // Dropping onto an OCCUPIED slot should read as a swap. The engine
            // unequips the old item for us, but nothing told the grid where to
            // put it, so it re-entered as a fresh pickup and took the first free
            // gap. Hand it the cell the carried item is vacating.
            // Rule 26: a drop onto an OCCUPIED slot is a swap, and a swap hands
            // the displaced item to the CURSOR -- exactly as dropping onto an
            // item in the grid does (rule 20). Sending it back to a cell instead
            // made the doll the one place where a swap behaved differently.
            //
            // Identify the occupant BEFORE the equip: comparing the form alone
            // said "same item" for a tempered sword dropped over a plain one, so
            // the swap was skipped in exactly the case pools exist for.
            RE::TESBoundObject* worn = Equip::WornObjectAt(g_slotTarget);
            auto*               wxl = worn ? Equip::WornExtraAt(g_slotTarget) : nullptr;
            // GI54: the hand is how the ENGINE marks the worn list -- weapons
            // take the slot's hand, a torch is left, and ARMOUR (a shield on
            // the shieldL slot included) is biped-worn with no hand mark.
            // Slot-based hand 2 made the landed-test look for an ExtraWornLeft
            // a shield never gets: the pending entry double-counted with the
            // applied worn list and the SPARE blinked out meanwhile.
            const auto engineHand = [&](RE::TESBoundObject* a_o) {
                if (!a_o) return 0;
                if (a_o->Is(RE::FormType::Weapon)) {
                    return g_slotTarget == "shieldL" ? 2 : 1;
                }
                if (a_o->Is(RE::FormType::Light)) return 2;
                return 0;
            };
            const int           wornHand = engineHand(worn);
            const std::uint16_t wsig = InstanceSig(wxl);
            std::uint16_t       wuid = 0;
            if (wxl) {
                if (const auto* xu = wxl->GetByType<RE::ExtraUniqueID>()) wuid = xu->uniqueID;
            }
            // The carried unit came off the board (hand 0) or off the OTHER hand;
            // either way it is not the unit standing in this slot. Comparing only
            // form+signature called two identical daggers "the same unit", so the
            // swap was skipped and the displaced one silently went to the pack.
            // ...but a carry that was DISPLACED by a swap can never be "the unit
            // already standing in this slot": something else took its place, and
            // that something has to come off. With two plain daggers the identity
            // test said "same unit" (uid 0, sig 0, same form, same hand), the
            // displaced one was never handed to the cursor, and it fell back into
            // the pack -- taking the parked star's slot on the way.
            const bool swapping = worn && !(worn == a_held.obj && wsig == a_held.sig &&
                                            a_held.fromDoll && !a_held.swappedOut &&
                                            a_held.hand == wornHand);

            // C6: dropped on an equip slot — the gate decides; a reject
            // snaps the item back (its layout entry is intact).
            // UidOf(key) alone lost the signature, so a tempered weapon dropped
            // on a slot equipped an arbitrary copy: pass the carried unit's own
            // identity instead of re-deriving a partial one from its key.
            const bool accepted = Equip::EquipItem(a_held.obj, g_slotTarget, a_held.uid,
                                                   a_held.xlIdx, a_held.sig, a_held.key);
            // Same interim gap as the right-click path: the carry ends NOW but
            // the engine equips later, so the tile would flicker back into the
            // cell it just left.
            if (g_poolTrace) {
                SKSE::log::info("[ACT] drop-on-slot '{}' <- '{}' key '{}' | occupant {} "
                                "swap={} accepted={}",
                    g_slotTarget, a_held.obj ? a_held.obj->GetName() : "?", a_held.key,
                    worn ? worn->GetName() : "(empty)", swapping, accepted);
            }
            if (accepted) {
                // GI54: the INCOMING item's engine hand, not the occupant's --
                // a shield replacing a left-hand sword is still hand 0.
                NotePendingEquip(a_held.obj, a_held.uid, a_held.sig,
                                 engineHand(a_held.obj), a_held.key);
                g_drainHint = { FormKey(a_held.obj), a_held.key };
            }
            g_held.reset();
            // Only an ACCEPTED equip that actually displaces something starts the
            // return carry. A potion or spell tome dropped on a slot is drunk or
            // read -- nothing comes off, and there is nothing to hand back.
            if (accepted && swapping) {
                // The engine has not unequipped it yet, so this is exactly a doll
                // pickup -- and it must name the HAND, or the worn-unit match can
                // consume the copy we just put IN and leave the displaced one
                // counted on the board as well as on the cursor.
                BeginCarry(worn, wuid, wsig, wornHand, /*swappedOut=*/true);
            }
            g_needRebuild = true;
            return true;
        }

        bool WholeOnCellArea(Held& a_held)
        {
            const auto& v = g_views[g_target.view];
            if (g_target.valid) {
                // C3: place (in-memory; the cosave persists on game save)
                if (g_poolTrace) {
                    SKSE::log::info("[ACT] drop-on-cell '{}' key '{}' -> [{},{}]",
                        a_held.obj ? a_held.obj->GetName() : "?", a_held.key,
                        g_target.col, g_target.row);
                }
                PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, a_held.count);
                if (g_sound) g_sound(a_held.obj, false);
                g_held.reset();
                g_needRebuild = true;
            } else if (g_target.blockers.size() == 1 &&
                       !(a_held.isBag && !v.bagKey.empty())) {
                const Item disp = g_items[g_target.blockers.front()];
                const RE::FormID hfid = a_held.obj->GetFormID();
                const RE::FormID dfid = disp.obj->GetFormID();
                const bool heldCoin = GoldCoins::IsCoinForm(hfid) &&
                                      !GoldCoins::IsPouch(hfid);
                const bool dispCoin = GoldCoins::IsCoinForm(dfid) &&
                                      !GoldCoins::IsPouch(dfid);
                bool doSwap = false;
                if (GoldCoins::IsPouch(dfid) && heldCoin) {
                    // G2: a coin dropped ON the pouch stores its value (up to
                    // the cap; no room -> keep carrying). G4: a pinned purse
                    // must return to walking first so the pouch can draw.
                    const int v2 = a_held.coinValue;
                    const bool wasPinned = GoldCoins::PinnedValue(a_held.key) >= 0;
                    if (wasPinned) GoldCoins::UnpinTile(a_held.key);
                    if (GoldCoins::StoreToPouch(v2) > 0) {
                        if (wasPinned) g_layout.erase(a_held.key);
                        if (g_sound) g_sound(a_held.obj, false);
                        g_held.reset();
                        g_needRebuild = true;
                    } else if (wasPinned) {
                        GoldCoins::PinAmount(a_held.key, v2);   // no room: restore
                    }
                } else if (heldCoin && dispCoin &&
                           a_held.coinValue >= 0 && disp.coinValue >= 0) {
                    // C4-G: a WHOLE gold tile dropped on another gold tile
                    // merges — shared MergeGoldInto (pouch mechanism,
                    // remainder rides the cursor as a pin)
                    MergeGoldInto(a_held, disp.key, disp.coinValue,
                        { g_target.col, g_target.row, v.bagKey, 1 });
                    g_needRebuild = true;
                } else if (!heldCoin && !dispCoin && disp.key != a_held.key &&
                           !a_held.isBag && disp.def.bag == 0 &&
                           PoolOfKey(disp.key) == HeldPool(a_held) &&
                           EffectiveCap(a_held.obj) > 1) {
                    // C4-S: a WHOLE stack tile dropped on a same-form tile
                    // merges up to the stack cap; the overflow stays on the
                    // cursor (its layout entry keeps the reduced count, so
                    // cancel restores it in place).
                    const int cap = EffectiveCap(a_held.obj);
                    const int room = (std::max)(0, cap - disp.count);
                    const int absorbed = (std::min)(room, a_held.count);
                    if (absorbed > 0) {
                        g_layout[disp.key].count = disp.count + absorbed;
                        a_held.count -= absorbed;
                        if (g_sound) g_sound(a_held.obj, false);
                        if (a_held.count <= 0) {
                            g_layout.erase(a_held.key);
                            g_held.reset();
                        } else {
                            g_layout[a_held.key].count = a_held.count;
                        }
                        g_needRebuild = true;
                    }
                    if (g_poolTrace) {
                        SKSE::log::info("[SWAP] merge '{}' into '{}' absorbed={} "
                                        "(cap {}), carrying {}",
                            a_held.key, disp.key, absorbed, cap, a_held.count);
                    }
                    // target full: swapping two same-form tiles only
                    // exchanged their positions (pointless churn) — keep
                    // carrying instead
                } else {
                    doSwap = true;
                }
                if (doSwap) {
                    if (g_poolTrace) {
                        SKSE::log::info("[SWAP] held '{}' (uid {:04X} sig {:04X}) onto "
                                        "'{}' (uid {:04X} sig {:04X}) at [{},{}] -> "
                                        "displaced rides the cursor",
                            a_held.key, a_held.uid, a_held.sig,
                            disp.key, disp.uid, disp.sig, g_target.col, g_target.row);
                    }
                    // C4: swap — free the displaced item's cell FIRST, then
                    // save mine; it snaps to the cursor immediately. PARK, not
                    // erase: the item survives, so its flags must too.
                    ParkTile(disp.key);
                    PlaceTile(a_held.key, g_target.col, g_target.row, v.bagKey, a_held.count);
                    if (g_sound) {
                        g_sound(a_held.obj, false);
                        g_sound(disp.obj, true);
                    }
                    g_held = Held{ disp.key, disp.obj, disp.mask, disp.count,
                                   disp.def.bag != 0, disp.def.scale,
                                   disp.mask.w * CellPx() * 0.5f,
                                   disp.mask.h * CellPx() * 0.5f, true };
                    g_held->coinValue = disp.coinValue;   // G4
                    g_held->xlIdx = disp.xlIdx;           // GI1
                    g_held->uid = disp.uid;
                    g_held->sig = disp.sig;               // GI25
                    g_held->fav = disp.fav;               // GI36
                    g_needRebuild = true;
                }
            }
            // other invalid targets (2+ blockers, bag-in-bag): keep carrying.
            // TERMINAL either way — a whole tile over a grid never falls
            // through to the partner/void rows (matches the old else-if).
            return true;
        }

        bool WholeStore(Held& a_held)
        {
            // dropped on the container window = STORE (coins excluded —
            // mirror artefacts). A stack (>1) opens the quantity slider
            // first. The pouch stores fine (gold travels via the sink).
            // F7 (kLoot/kSteal): the drop CELL is honoured — empty cell =
            // stored right there; occupied cell = swap (the stored item
            // takes the occupant's spot, the occupant jumps to the cursor).
            const RE::FormID fid = a_held.obj->GetFormID();
            if (g_questItem.contains(HeldPoolKey(a_held))) {   // Phase 7: can't store
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
            } else if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                const auto sd = LootBarter::QueryStoreDrop();   // F7 (dead outside kLoot/kSteal)
                if (a_held.count > 1) {
                    // srcKey rides along: pending-remove fires on CONFIRM (an
                    // immediate erase made the tile jump to the front while
                    // the engine removal was still queued).
                    // F7: an empty drop cell rides the slider as a spot hint
                    if (sd.onCell && sd.freeSpot) {
                        LootBarter::SetStoreSpotHint(a_held.obj, sd.col, sd.row, HeldInstanceSig());
                    }
                    LootBarter::OpenSlider(a_held.obj, a_held.count,
                        LootBarter::XferDir::kStore, a_held.key, 0, a_held.uid, a_held.sig,
                        false, a_held.fav);
                } else {
                    LootBarter::RequestStore(a_held.obj, a_held.count,
                                             HeldUidOf(a_held.key, a_held.uid), a_held.sig,
                                             a_held.fav);
                    NotePendingRemove(a_held.obj, a_held.key, a_held.count);
                    if (a_held.isBag) {   // bag contents reflow to main (E4)
                        g_openBags.erase(a_held.key);
                        for (auto& [k, le] : g_layout) {
                            if (le.bag == a_held.key) le.bag.clear();
                        }
                    }
                    if (sd.onCell) {
                        // `sd.occ != a_held.obj` was a FORM comparison: storing a
                        // dagger onto the container's dagger read as "the same
                        // thing", the swap was skipped, and the stored one
                        // first-fit into some other cell. Two units of a
                        // non-stackable form are DIFFERENT units, so that is a
                        // swap (rule 20) -- exactly what the partner's own
                        // rearrange path already does. Only a genuine stack
                        // merges, and the partner shows one aggregate cell per
                        // stackable form, so same form + cap>1 is that case.
                        const bool merging = sd.occ == a_held.obj &&
                                             EffectiveCap(a_held.obj) > 1;
                        if (sd.occ && !merging) {
                            // F7 rule 4: swap — stored item takes the
                            // occupant's anchor, the occupant rides the cursor
                            LootBarter::NoteStoreSpot(a_held.obj, sd.occCol, sd.occRow, HeldInstanceSig());
                            g_held.reset();
                            // GI24: same as the rearrange swap — the occupant
                            // keeps its identity and its pool slot
                            BeginPartnerCarry(sd.occ, sd.occCount, sd.occValue,
                                              -1.0f, -1.0f,
                                              sd.occUid, sd.occXlIdx, sd.occOrd);
                            LootBarter::NoteCarriedSpot(sd.occSpotKey);
                            g_needRebuild = true;
                            return true;
                        }
                        // F7 rule 3: a FREE spot = stored right there
                        // (2+ blockers leave no spot note -> first-fit)
                        if (sd.freeSpot) {
                            LootBarter::NoteStoreSpot(a_held.obj, sd.col, sd.row, HeldInstanceSig());
                        }
                    }
                }
            }
            g_held.reset();
            g_needRebuild = true;
            return true;
        }

        bool WholeSell(Held& a_held)
        {
            // SELL by dragging onto the merchant window. Coins excluded;
            // stack -> slider; single sells if the merchant can afford.
            // Bags/pouch sell only this way (right-click = manage). The
            // bag's contents reflow to main only here, on the real sale.
            const RE::FormID fid = a_held.obj->GetFormID();
            if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                const int val = TileValue(a_held.obj,                          // GI43
                    HeldUidOf(a_held.key, a_held.uid), a_held.sig);
                if (g_questItem.contains(HeldPoolKey(a_held))) {   // Phase 7: can't sell
                    Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                } else if (!LootBarter::MerchantBuys(a_held.obj, g_stolen.contains(
                               PoolPrefix(FormKey(a_held.obj), a_held.uid, a_held.sig)))) {
                    Sfx::FailNote(Lang::T(Lang::Str::MerchantWontBuy));   // Phase 6: category / stolen
                } else if (a_held.count > 1) {
                    // srcKey rides along: pending-remove fires on CONFIRM
                    LootBarter::OpenSlider(a_held.obj, a_held.count,
                        LootBarter::XferDir::kSell, a_held.key, val, a_held.uid, a_held.sig,
                        false, a_held.fav);
                } else {
                    const int total = val > 0 ? LootBarter::SellPrice(a_held.obj, val) : 0;
                    if (total > 0 && LootBarter::MerchantGold() < total) {
                        Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                    } else {
                        LootBarter::RequestSell(a_held.obj, 1, total, val, a_held.uid, a_held.sig,
                                                a_held.fav);
                        NotePendingRemove(a_held.obj, a_held.key, 1);
                        if (a_held.isBag) {   // contents reflow to main on sale (E4)
                            g_openBags.erase(a_held.key);
                            for (auto& [k, le] : g_layout) {
                                if (le.bag == a_held.key) le.bag.clear();
                            }
                        }
                    }
                }
            }
            g_held.reset();
            g_needRebuild = true;
            return true;
        }

        // ---- F6b: reverse pickpocket (planting items on the mark) ----
        bool WholePickStore(Held& a_held)
        {
            const RE::FormID fid = a_held.obj->GetFormID();
            if (g_questItem.contains(HeldPoolKey(a_held))) {   // quest items never leave
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
            } else if (!(GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid))) {
                // coins are mirror artefacts (gold planting isn't supported);
                // everything else rolls the engine attempt on the Tick.
                // Pending-remove is noted on the WIN inside the Tick.
                if (a_held.count > 1) {
                    LootBarter::OpenSlider(a_held.obj, a_held.count,
                        LootBarter::XferDir::kPickStore, a_held.key, 0, a_held.uid, a_held.sig,
                        false, a_held.fav);
                } else {
                    LootBarter::RequestPickStore(a_held.obj, 1, a_held.uid, a_held.sig, a_held.key,
                                                 a_held.fav);
                }
            }
            g_held.reset();
            g_needRebuild = true;
            return true;
        }

        bool FragPickStore(Held& a_held)
        {
            // a split fragment plants its quantity (no tile key: the source
            // count already dropped at split time; a lost roll force-closes
            // and the reconciler restores the units)
            LootBarter::RequestPickStore(a_held.obj, a_held.count, a_held.uid, a_held.sig, {},
                                         a_held.fav);
            g_held.reset();
            return true;
        }

        bool WholeCancelNonNormal(Held&)
        {
            // loot/barter mode: dropping into empty space must NOT discard
            // (accidental loss during looting) — cancel back to the spot.
            if (LootBarter::CurrentMode() == LootBarter::Mode::kNormal) return false;
            g_held.reset();
            g_needRebuild = true;
            return true;
        }

        bool WholeToVoid(Held& a_held)
        {
            // C5: dropped outside every window -> discard to the world. The
            // pouch drops like any item; its stored gold travels with it
            // (container sink handles the ledger).
            const RE::FormID fid = a_held.obj->GetFormID();
            if (GoldCoins::IsCoinForm(fid) && !GoldCoins::IsPouch(fid)) {
                // G2/G4: the coin's VALUE drops as a real Gold001 world ref;
                // a pinned purse returns to walking first so the debit lands.
                // The mirror then removes the coin tile.
                if (GoldCoins::PinnedValue(a_held.key) >= 0)
                    GoldCoins::UnpinTile(a_held.key);
                GoldCoins::DropAsGold(a_held.coinValue);
                g_layout.erase(a_held.key);   // free this coin's slot
                g_held.reset();
                g_needRebuild = true;
            } else if (g_questItem.contains(HeldPoolKey(a_held))) {
                // Phase 7: quest items can't be discarded — cancel back to
                // their spot (Rebuild restores the tile).
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                g_held.reset();
                g_needRebuild = true;
            } else {
                g_layout.erase(a_held.key);
                if (a_held.isBag) {
                    // dropping a bag releases its contents to main (E4)
                    g_openBags.erase(a_held.key);
                    for (auto& [k, le] : g_layout) {
                        if (le.bag == a_held.key) le.bag.clear();
                    }
                }
                // G3: a carried tile drops ITS units, not the whole form
                // stack (arrows 250 = tiles of 100/100/50)
                if (g_dropWorld) {
                    g_dropWorld(a_held.obj, a_held.count,   // GI36
                        ResolveExitUnit(a_held.obj, a_held.uid, a_held.sig, a_held.count,
                                        a_held.fav ? a_held.count : 0));
                }
                g_held.reset();
                g_needRebuild = true;
            }
            return true;
        }

        // ---- F2: trash intake / eviction helpers ----

        // live favorite state straight from the engine entry (Held carries
        // no fav flag) — same walk as ToggleFavorite
        // protection rules — false blocks the intake (note played)
        bool TrashIntakeAllowed(RE::TESBoundObject* a_obj, const std::string& a_heldKey,
                                bool a_isBag)
        {
            const RE::FormID fid = a_obj->GetFormID();
            if (g_questItem.contains(PoolOfKey(a_heldKey))) {
                Sfx::FailNote(Lang::T(Lang::Str::QuestItemLocked));
                return false;
            }
            // gold coins / pouch / raw gold: the discard path stays R / void
            // drop only (two delete paths for gold invite accidents)
            if (GoldCoins::IsCoinForm(fid) || a_obj->IsGold()) {
                Sfx::FailNote(Lang::T(Lang::Str::TrashGoldBlocked));
                return false;
            }
            if (a_isBag) {   // only an EMPTY bag may be trashed
                for (const auto& [k, le] : g_layout) {
                    if (le.bag == a_heldKey) {
                        Sfx::FailNote(Lang::T(Lang::Str::TrashBagBlocked));
                        return false;
                    }
                }
            }
            return true;
        }

        // can a_mask first-fit the 6x4 trash board with the CURRENT parked
        // layout (g_layout-based so an eviction frees space immediately)?
        bool TrashHasRoomFor(const Mask& a_mask)
        {
            bool occ[kTrashRows][kTrashCols] = {};
            for (const auto& [k, le] : g_layout) {
                if (le.bag != kTrashKey || le.col < 0) continue;
                if (g_held && k == g_held->key) continue;   // carried: cell free
                // parked tiles are always live g_items (rebuilt while open)
                const Mask* m = nullptr;
                for (const auto& it : g_items) {
                    if (it.key == k) { m = &it.mask; break; }
                }
                const int mw = m ? m->w : 1, mh = m ? m->h : 1;
                for (int y = 0; y < mh; ++y) {
                    for (int x = 0; x < mw; ++x) {
                        if (m && !m->rows[y][x]) continue;
                        const int r = le.row + y, c = le.col + x;
                        if (r >= 0 && r < kTrashRows && c >= 0 && c < kTrashCols) {
                            occ[r][c] = true;
                        }
                    }
                }
            }
            for (int r = 0; r + a_mask.h <= kTrashRows; ++r) {
                for (int c = 0; c + a_mask.w <= kTrashCols; ++c) {
                    bool ok = true;
                    for (int y = 0; ok && y < a_mask.h; ++y) {
                        for (int x = 0; ok && x < a_mask.w; ++x) {
                            if (a_mask.rows[y][x] && occ[r + y][c + x]) ok = false;
                        }
                    }
                    if (ok) return true;
                }
            }
            return false;
        }

        // CONFIRM a parked tile's deletion: the layout entry drains via the
        // shared pending-remove bridge and the engine RemoveItem is queued
        // for the Tick (never mid-frame). FIFO order is caller-managed.
        void ConfirmTrashDelete(const std::string& a_key)
        {
            const auto li = g_layout.find(a_key);
            if (li == g_layout.end() || li->second.bag != kTrashKey) return;
            RE::TESBoundObject* obj = nullptr;
            int count = (std::max)(1, li->second.count);
            bool fav = false;   // GI36
            for (const auto& it : g_items) {
                if (it.key == a_key) {
                    obj = it.obj; count = it.count; fav = it.fav;
                    break;
                }
            }
            if (obj) {
                NotePendingRemove(obj, a_key, count);   // drains + erases the entry
                g_trashDeleteQ.push_back({ obj->GetFormID(), count,
                                           UidOf(a_key), SigOf(a_key), fav });
            } else {
                g_layout.erase(a_key);   // unresolvable tile: just free the cell
            }
            g_trashReturn.erase(a_key);
            g_needRebuild = true;
        }

        // evict oldest parked tiles until a_mask fits (FIFO, per spec)
        void TrashMakeRoomFor(const Mask& a_mask)
        {
            while (!TrashHasRoomFor(a_mask) && !g_trashOrder.empty()) {
                const std::string victim = g_trashOrder.front();
                g_trashOrder.pop_front();
                ConfirmTrashDelete(victim);
            }
        }

        // park a KEYED tile (whole-tile intake + the favorite-ask resume).
        // a_col/a_row -1 = first-fit inside the trash board.
        void ParkKeyInTrash(const std::string& a_key, RE::TESBoundObject* a_obj,
                            int a_count, int a_col, int a_row)
        {
            LayoutEntry prev;   // pre-park spot -> right-click restore target
            prev.col = -1;
            prev.row = -1;
            if (const auto li = g_layout.find(a_key); li != g_layout.end()) prev = li->second;
            g_trashReturn[a_key] = prev;
            PlaceTile(a_key, a_col, a_row, kTrashKey, a_count);
            g_trashOrder.push_back(a_key);
            g_openBags.erase(a_key);   // a parked (empty) bag closes its window
            if (g_sound && a_obj) g_sound(a_obj, false);
            g_needRebuild = true;
        }

        // ---- F2: drop handlers ----
        bool WholeIntoTrash(Held& a_held)
        {
            // a tile already parked = in-trash reposition: normal grid grammar
            const auto cur = g_layout.find(a_held.key);
            if (cur != g_layout.end() && cur->second.bag == kTrashKey) return false;

            if (!TrashIntakeAllowed(a_held.obj, a_held.key, a_held.isBag)) {
                g_held.reset();   // blocked: snaps back (layout entry intact)
                g_needRebuild = true;
                return true;
            }
            const int col = g_target.valid ? g_target.col : -1;
            const int row = g_target.valid ? g_target.row : -1;
            if (a_held.fav) {   // GI36: the carry brought the answer with it
                // favorite: confirm first — the tile snaps back while asking
                g_trashAsk = { true, a_held.obj, a_held.key, a_held.count, col, row };
                Sfx::SelectOn();
                g_held.reset();
                g_needRebuild = true;
                return true;
            }
            TrashMakeRoomFor(a_held.mask);
            ParkKeyInTrash(a_held.key, a_held.obj, a_held.count, col, row);
            g_held.reset();
            return true;
        }

        bool StackFragIntoTrash(Held& a_held)
        {
            // a split fragment parks as its own NEW tile (no favorite ask:
            // the fragment can't survive the popup round-trip — reset would
            // re-absorb it into the source stack)
            if (!TrashIntakeAllowed(a_held.obj, {}, false)) {
                g_held.reset();
                return true;
            }
            TrashMakeRoomFor(a_held.mask);
            const std::string nk = NextTileKey(HeldPool(a_held));
            g_trashReturn[nk] = LayoutEntry{ -1, -1, {}, 0 };   // restore = first-fit
            g_layout[nk] = { g_target.valid ? g_target.col : -1,
                             g_target.valid ? g_target.row : -1,
                             kTrashKey, a_held.count };
            g_trashOrder.push_back(nk);
            if (g_sound) g_sound(a_held.obj, false);
            g_held.reset();
            return true;
        }

        bool GoldFragTrashBlock(Held&)
        {
            Sfx::FailNote(Lang::T(Lang::Str::TrashGoldBlocked));
            return true;   // note played; the pin keeps riding the cursor
        }

        struct DropRoute
        {
            DropWhere where;
            bool (*handler)(Held&);
        };

        constexpr DropRoute kPartnerHeldRoutes[] = {
            { DropWhere::kAlways, DropPartnerHeld },
        };
        // gold fragment: priority mirrors the stack case; (6) chrome /
        // unresolved = keep carrying (no consuming row)
        constexpr DropRoute kGoldFragRoutes[] = {
            { DropWhere::kTrashArea, GoldFragTrashBlock },   // F2: gold never parks
            { DropWhere::kEmptyCell, GoldFragOnEmptyCell },
            { DropWhere::kBlockerSingle, GoldFragOnBlocker },
            { DropWhere::kVoid, GoldFragToVoid },
        };
        constexpr DropRoute kStackFragRoutes[] = {
            { DropWhere::kTrashArea, StackFragIntoTrash },   // F2 (before kEmptyCell)
            { DropWhere::kEmptyCell, StackFragOnEmptyCell },
            { DropWhere::kBlockerSingle, StackFragOnBlocker },
            { DropWhere::kAlways, FragQuestCancel },   // unresolved quest frag: cancel
            { DropWhere::kPartnerLoot, StackFragStore },
            { DropWhere::kPartnerBarter, StackFragSell },
            { DropWhere::kPartnerPickpocket, FragPickStore },   // F6b
            { DropWhere::kVoid, StackFragToVoid },
        };
        constexpr DropRoute kWholeTileRoutes[] = {
            { DropWhere::kEquipSlot, WholeOnEquipSlot },
            { DropWhere::kTrashArea, WholeIntoTrash },   // F2 (falls through when
                                                         // repositioning INSIDE)
            { DropWhere::kCellArea, WholeOnCellArea },
            { DropWhere::kPartnerLoot, WholeStore },
            { DropWhere::kPartnerBarter, WholeSell },
            { DropWhere::kPartnerPickpocket, WholePickStore },   // F6b
            { DropWhere::kAlways, WholeCancelNonNormal },
            { DropWhere::kVoid, WholeToVoid },
        };

        void ResolveDrop(Held& a_held)
        {
            const DropRoute* rows = nullptr;
            size_t n = 0;
            bool alwaysRebuild = false;   // fragments rebuild on every attempt
            if (a_held.fromPartner) {
                rows = kPartnerHeldRoutes;
                n = std::size(kPartnerHeldRoutes);
            } else if (a_held.preSplit && a_held.coinValue >= 0) {
                rows = kGoldFragRoutes;
                n = std::size(kGoldFragRoutes);
                alwaysRebuild = true;
            } else if (a_held.preSplit) {
                rows = kStackFragRoutes;
                n = std::size(kStackFragRoutes);
                alwaysRebuild = true;
            } else {
                rows = kWholeTileRoutes;
                n = std::size(kWholeTileRoutes);
            }
            for (size_t i = 0; i < n; ++i) {
                if (!DropWhereMatches(rows[i].where)) continue;
                if (rows[i].handler(a_held)) break;
            }
            // no consuming route: keep carrying (window chrome etc.)
            if (alwaysRebuild) g_needRebuild = true;
        }
    }

    void FinishFrame()
    {
        if (g_held) {
            auto& held = *g_held;
            DrawHeldCursorIcon(held);

            if (held.justPicked) {
                held.justPicked = false;   // the pickup click must not drop (C1)
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                g_held.reset();            // C7: cancel, item resumes its spot
                Sfx::SelectOff();
                g_needRebuild = true;
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // Phase 3: the old 6~7-deep else-if chain lives in the
                // drop-route tables above (ResolveDrop) — no consuming
                // route (window chrome etc.) keeps the item carried
                ResolveDrop(held);
            }
        }

        // an item was just dropped/cancelled: the tile that materialises
        // under the cursor must not fire the hover blip
        static bool s_wasHeld = false;
        const bool nowHeld = g_held.has_value();
        if (s_wasHeld && !nowHeld) Sfx::HoverMute(0.35);
        s_wasHeld = nowHeld;

        g_target = {};        // recomputed by next frame's draws
        g_slotTarget.clear();

        if (g_needRebuild) {
            g_needRebuild = false;
            Rebuild();
        }
    }

    // ---- F2: trash window public surface ----

    bool IsTrashOpen() { return g_trashOpen; }

    void ToggleTrash()
    {
        if (g_trashOpen) {
            CloseTrash();
        } else {
            g_trashOpen = true;
            g_needRebuild = true;
            Sfx::BagOpen();
        }
    }

    bool CloseTrash()
    {
        if (!g_trashOpen) return false;
        // closing CONFIRMS every parked deletion (spec) — engine removals
        // run on the Tick via the delete queue
        std::vector<std::string> keys;
        for (const auto& [k, le] : g_layout) {
            if (le.bag == kTrashKey) keys.push_back(k);
        }
        for (const auto& k : keys) ConfirmTrashDelete(k);
        g_trashOrder.clear();
        g_trashReturn.clear();
        g_trashAsk = {};
        g_trashOpen = false;
        g_needRebuild = true;
        Sfx::BagClose();
        return true;
    }

    bool CloseTrashConfirm()
    {
        if (!g_trashAsk.active) return false;
        g_trashAsk = {};
        return true;
    }

    void DrawTrashConfirm()
    {
        if (!g_trashAsk.active) return;
        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;

        const char* name = g_trashAsk.obj ? g_trashAsk.obj->GetName() : "?";
        const char* msg = Lang::T(Lang::Str::TrashFavConfirm);
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float contentW = (std::max)({ btnRow,
            ImGui::CalcTextSize(msg).x,
            ImGui::CalcTextSize(name).x * 1.35f });
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + sp + 6.0f * S +
                ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        if (wm->BeginConfirmPopup("trashask", "##gi_trashask", name, size)) {
            g_trashAsk = {};   // outside click cancels (tile already snapped back)
            Sfx::SelectOff();
        }
        if (!g_trashAsk.active) { ImGui::End(); return; }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };
        center(ImGui::CalcTextSize(msg).x);
        ImGui::TextColored(sk.inkDim, "%s", msg);
        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        const bool typing = ImGui::GetIO().WantTextInput;   // GI52
        const bool ok = Sfx::Button(Lang::T(Lang::Str::Confirm), ImVec2(btnW, 0)) ||
                        (!typing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_Space, false)));
        ImGui::SameLine(0.0f, 8.0f * S);
        const bool cancel = Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ok && g_trashAsk.obj) {
            const Mask m = MaskOf(g_resolver ? g_resolver(g_trashAsk.obj) : GridDef{});
            TrashMakeRoomFor(m);
            ParkKeyInTrash(g_trashAsk.key, g_trashAsk.obj, g_trashAsk.count,
                g_trashAsk.col, g_trashAsk.row);
            g_trashAsk = {};
        } else if (cancel) {
            g_trashAsk = {};   // tile stays where it snapped back to
        }
        ImGui::End();
    }

    void ProcessTrashDeletes()
    {
        if (g_trashDeleteQ.empty()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) { g_trashDeleteQ.clear(); return; }
        int soundBudget = 2;   // a close-all burst must not machine-gun sounds
        for (const auto& d : g_trashDeleteQ) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(d.form);
            if (!obj) continue;
            const int count = d.count;
            player->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kRemove,
                ResolveExitUnit(obj, d.uid, d.sig, count, d.fav ? count : 0), nullptr);
            ClearPendingRemove(obj, count);
            if (g_sound && soundBudget-- > 0) g_sound(obj, false);
        }
        g_trashDeleteQ.clear();
        RequestRebuild();
        MarkCapacityDirty();
    }

    // ---- cosave persistence ('GLAY' v5) ----
    // v5 == v2 layout (v3/v4 experiments retired; v4's trailing fav byte is
    // read-and-discarded on load).
    // [u32 bagCount]{str} [u32 entryCount]{ str key, i32 col, i32 row, str bag,
    // i32 count (v2, G4 tile-owned quantity; v1 records load with count=0 =
    // "unspecified", which the reconciler fills like a fresh pickup) }
    // Keys are "Plugin.esp|0xLocalID" strings — load-order independent, no
    // ResolveFormID needed. main.cpp owns the record loop.

    // A RUNTIME-CREATED form (potion brewed at an alchemy lab, weapon the player
    // enchanted) has no source plugin, so FormKey names it "Dynamic|0x<FormID>".
    // Those FormIDs are handed out per session and are NOT stable across a
    // save/load, which makes such a key useless to persist and mildly harmful:
    //   - it can never match its own item again, so it is dead weight that grows
    //     with every distinct recipe the player ever brews, and
    //   - a DIFFERENT runtime form can be handed the same id after a load and
    //     inherit the old item's grid slot.
    // The stale-instance pruner does not collect these (it only walks '@'/'~'
    // suffixed keys), so filter them at the cosave boundary instead. In-memory
    // they stay live and behave normally for the rest of the session.
    bool IsPersistableKey(const std::string& a_key)
    {
        return !a_key.starts_with("Dynamic|");
    }

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, kCosaveVersion)) {
            SKSE::log::error("[GRID] cosave: OpenRecord failed");
            return;
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_openBags.size()));
        for (const auto& b : g_openBags) WriteStr(a_intfc, b);
        // F2: parked-in-trash entries never persist (a mid-menu F5 save would
        // otherwise strand items in a view that doesn't exist after load) —
        // they save at their PRE-park spot so a load simply restores them.
        std::uint32_t persisted = 0;
        for (const auto& [key, le] : g_layout) {
            if (!IsPersistableKey(key)) continue;
            ++persisted;
        }
        a_intfc->WriteRecordData(persisted);
        for (const auto& [key, le] : g_layout) {
            if (!IsPersistableKey(key)) continue;
            const LayoutEntry* out = &le;
            LayoutEntry back;
            if (le.bag == kTrashKey) {
                back.col = -1;
                back.row = -1;
                back.count = le.count;
                if (const auto ri = g_trashReturn.find(key); ri != g_trashReturn.end()) {
                    back = ri->second;
                    back.count = le.count;
                }
                out = &back;
            }
            WriteStr(a_intfc, key);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->col));
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->row));
            WriteStr(a_intfc, out->bag);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(out->count));   // v2
        }
        SKSE::log::info("[GRID] cosave: saved {} placements, {} open bags",
            g_layout.size(), g_openBags.size());
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        // v1 (no per-tile count) still loads: count stays 0 and the reconciler
        // fills it on the first Rebuild (batch absorb, layout preserved).
        //
        // GI1: this used to be a WHITELIST ("!= 1 && != kCosaveVersion") that
        // returned in SILENCE. Bumping kCosaveVersion with that in place would
        // have dropped every v2 save's entire layout without a single log line.
        // Range-check instead, and say so when a record is refused.
        if (a_version < 1 || a_version > kCosaveVersion) {
            SKSE::log::warn("[GRID] cosave: unsupported layout record v{} (max v{}) — skipped",
                            a_version, kCosaveVersion);
            return;
        }

        std::map<std::string, LayoutEntry> layout;
        std::set<std::string> bags;

        std::uint32_t bagCount = 0;
        if (!a_intfc->ReadRecordData(bagCount) || bagCount > kMaxEntries) return;
        for (std::uint32_t i = 0; i < bagCount; ++i) {
            std::string b;
            if (!ReadStr(a_intfc, b)) return;
            bags.insert(std::move(b));
        }
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count) || count > kMaxEntries) return;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string key;
            LayoutEntry le;
            std::int32_t col = 0, row = 0;
            if (!ReadStr(a_intfc, key) || !a_intfc->ReadRecordData(col) ||
                !a_intfc->ReadRecordData(row) || !ReadStr(a_intfc, le.bag)) {
                return;   // truncated: keep whatever parsed? no — bail clean
            }
            le.col = col;
            le.row = row;
            if (a_version >= 2) {   // v2: per-tile count (0 in v1 -> reconciled)
                std::int32_t cnt = 0;
                if (!a_intfc->ReadRecordData(cnt)) return;
                le.count = cnt;
            }
            if (a_version == 4) {
                // v4 ONLY (GI27 era): a per-tile favourite byte. The feature
                // was retired in GI33 (favourites are vanilla's), but saves
                // written by those builds still carry the byte -- read it so
                // the stream stays aligned, and throw it away.
                std::int32_t f = 0;
                if (!a_intfc->ReadRecordData(f)) return;
            }
            // Saves written before the filter above carry dynamic keys; drop
            // them on the way in so an existing playthrough gets cleaned too.
            if (!IsPersistableKey(key)) continue;
            layout[std::move(key)] = std::move(le);
        }

        g_layout = std::move(layout);
        g_openBags = std::move(bags);
        g_layoutLoaded = true;   // do NOT fall back to the legacy ini
        g_capacityDirty = true;
        SKSE::log::info("[GRID] cosave: loaded {} placements, {} open bags",
            g_layout.size(), g_openBags.size());
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        // clean slate before every load / new game. g_layoutLoaded=false keeps
        // the legacy ini as a one-shot migration source for saves that predate
        // the 'GLAY' record (LoadRecord overrides it when the record exists).
        g_layout.clear();
        g_openBags.clear();
        g_pendingEquip.clear();   // cross-frame set: must never outlive a load
        g_vacated.clear();        // GI28: a flash is about the frame it happened in
        g_layoutLoaded = false;
        g_prevKeys.clear();
        g_capacityDirty = true;
        g_avResidueCleared = false;   // legacy CW cleanup is per-save
        g_paidGold = 0;
        ClearAllPendingRemoves();
        // B6: defensive resets — these carried PREVIOUS-session state across
        // a load (g_held even held a stale TESBoundObject*). The menu-close
        // path usually cleans them, but a load with the menu open must not
        // rely on it.
        g_held.reset();
        g_items.clear();
        g_views.clear();
        g_target = {};
        g_slotTarget.clear();
        g_dropHint = {};
        g_pouchOpen = false;
        g_pouchSlider = 0;
        g_overloaded = false;
        g_spaceUsed = 0;
        // F2: trash state is per-session — parked items simply reappear on
        // their boards after a load (the engine inventory was never touched)
        g_trashOpen = false;
        g_trashOrder.clear();
        g_trashReturn.clear();
        g_trashDeleteQ.clear();
        g_trashAsk = {};
        RequestRebuild();
    }

    void MarkLayoutFresh()
    {
        // new game (kNewGame arrives after revert, and no load callback runs):
        // start EMPTY — the legacy-ini migration is for old saves only
        g_layout.clear();
        g_openBags.clear();
        g_layoutLoaded = true;
        g_capacityDirty = true;
    }
}
