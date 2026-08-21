#include "ui/Badges.h"
#include "ui/Editor.h"
#include "ui/Equip.h"
#include "ui/Fallback.h"
#include "ui/LootBarter.h"
#include "game/Ledger.h"

#include "game/BagFilter.h"
#include "game/GoldCoins.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <vector>

namespace FUI::LootBarter
{
    namespace
    {
        Mode                g_mode = Mode::kNormal;
        RE::ObjectRefHandle g_partner;

        bool g_partnerHovered = false;   // mouse over partner window (drag-to-store)

        // deferred item transfers — applied on Tick, game thread. Loot uses
        // kTake/kStore (no gold); barter uses kBuy/kSell (price settled too).
        struct XferReq
        {
            enum Dir { kTake, kStore, kBuy, kSell, kPickTake, kPickStore };
            Dir                 dir;
            RE::TESBoundObject* obj;
            int                 count;
            int                 price = 0;   // total gold for kBuy/kSell
            int                 base = 0;    // total BASE value (speech XP points)
            std::string         srcKey;      // kPickStore: tile the units leave
            // D4: WHICH sub-stack moves. 0/-1 = let the engine choose, which is
            // still right for fungible goods (arrows, ingots, gold).
            std::uint16_t       uid = 0;
            // GI25: the POOL, not a list position. A transfer runs on the next
            // Tick, and by then an index captured at click time can be stale --
            // the signature cannot be, it travels with the ExtraDataList.
            std::uint16_t       sig = 0;
            // The cell this came from was WORN. A worn unit with no signature
            // (a plain equipped dagger) cannot be named by pool at all -- uid 0
            // and sig 0 resolve to nullptr, the engine chooses, and a TEMPERED
            // spare in the same inventory can walk out instead of the one on the
            // body. The worn list is the only handle such a unit has.
            bool                fromWorn = false;
            // GI36: the outgoing units wore a star -- rule 58 kills it at the sink.
            bool                fav = false;
            // ★(1.3.3) ...and WHERE it sat, for the store/sell sinks. The pool
            // is a crowd -- two identical daggers share one signature, so the
            // sink took whichever list came first and the front one was never
            // what left. The position is VERIFIED at the sink (same pool,
            // unworn) so a stale one costs accuracy, never correctness -- which
            // is what the note above was guarding against.
            int                 xlIdx = -1;
            // ★(1.3.0) USE MODE: consume / read / learn the moment it lands,
            // rather than merely landing.
            // ★★LAST, and this time actually last. The first attempt put it
            // after `fromWorn` while calling it "appended last", and the three
            // braced initialisers further down silently shifted by one field --
            // the compiler caught it only because bool/int narrowing happened to
            // be involved. The same shape cost a whole test round in
            // OffBoardUnit, where no narrowing saved it. New fields go at the
            // END, and the site that sets this one builds the struct FIELD-WISE.
            bool                useAfter = false;
        };
        std::vector<XferReq> g_xfer;

        // ---- units this side has committed to hand over -----------------------
        //
        // The mirror of the player board's pending-remove. Taking, buying or
        // lifting drops the carry the instant the player commits, but the engine
        // does not move the item until the next Tick -- and CollectPartnerCells
        // reads the engine directly. For those frames the cell SAT BACK DOWN on
        // the partner board and was then snatched away: a blink on every drag
        // out of a container, corpse, shop or pocket.
        //
        // Keyed by (form, uid, sig) so a tempered unit on its way out never hides
        // a plain sibling that is staying put.
        std::map<std::string, int>            g_outPool;
        std::map<RE::FormID, int>             g_outForm;
        std::chrono::steady_clock::time_point g_outWhen{};
        constexpr std::chrono::seconds        kOutTTL{ 3 };

        std::string OutKey(RE::FormID a_id, std::uint16_t a_uid, std::uint16_t a_sig)
        {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%08X@%04X~%04X", a_id, a_uid, a_sig);
            return buf;
        }

        void NoteOut(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                     int a_count)
        {
            if (!a_obj || a_count <= 0) return;
            g_outPool[OutKey(a_obj->GetFormID(), a_uid, a_sig)] += a_count;
            g_outForm[a_obj->GetFormID()] += a_count;
            g_outWhen = std::chrono::steady_clock::now();
        }

        void ClearOut(RE::TESBoundObject* a_obj, std::uint16_t a_uid, std::uint16_t a_sig,
                      int a_count)
        {
            if (!a_obj || a_count <= 0) return;
            const auto k = OutKey(a_obj->GetFormID(), a_uid, a_sig);
            if (auto it = g_outPool.find(k); it != g_outPool.end()) {
                it->second -= a_count;
                if (it->second <= 0) g_outPool.erase(it);
            }
            if (auto it = g_outForm.find(a_obj->GetFormID()); it != g_outForm.end()) {
                it->second -= a_count;
                if (it->second <= 0) g_outForm.erase(it);
            }
        }

        // A request that never reached the engine (menu closed, roll lost, item
        // gone) must not keep hiding cells for the rest of the session.
        void SweepOut()
        {
            if (g_outForm.empty()) return;
            if (std::chrono::steady_clock::now() - g_outWhen > kOutTTL) {
                g_outPool.clear();
                g_outForm.clear();
            }
        }

        // Vanilla speech XP for a barter transaction: the skill-use points are
        // the BASE value of the goods (the haggled price doesn't matter), fed
        // through the AVIF skill-use curve xp = useMult * points + offsetMult.
        // Read the Speech AVSK live so skill-tweak mods apply automatically.
        float SpeechXP(int a_baseTotal)
        {
            if (a_baseTotal <= 0) return 0.0f;
            float useMult = 0.55f, offset = 0.0f;   // vanilla Speech AVSK defaults
            if (auto* list = RE::ActorValueList::GetSingleton()) {
                if (auto* info = list->GetActorValue(RE::ActorValue::kSpeech);
                    info && info->skill) {
                    useMult = info->skill->useMult;
                    offset = info->skill->offsetMult;
                }
            }
            return useMult * static_cast<float>(a_baseTotal) + offset;
        }

        // quantity slider (Shift+right-click on a stack)
        struct SliderState
        {
            bool                active = false;
            RE::TESBoundObject* obj = nullptr;
            int                 max = 1;
            int                 value = 1;
            XferDir             dir = XferDir::kTake;
            std::string         srcKey;      // kPickup: tile the split is taken from
            std::uint16_t       uid = 0;     // GI25: the pool the units leave
            std::uint16_t       sig = 0;
            int                 unitValue = 0;   // kBuy/kSell: base value per unit
            bool                worn = false;    // the cell came off the body
            bool                fav = false;     // GI36
            int                 xlIdx = -1;      // (1.3.3) which unit leaves
        };
        SliderState g_slider;

        // Phase 5: favorite-sale confirm popup — a single favorited item asks
        // before selling (prevents fat-finger loss of a marked item).
        struct SellConfirm
        {
            bool                active = false;
            RE::TESBoundObject* obj = nullptr;
            int                 count = 0;
            int                 price = 0;
            int                 base = 0;   // total base value (speech XP points)
            std::string         srcKey;     // tile the sale drains (in-place removal)
            std::uint16_t       uid = 0;     // GI25
            std::uint16_t       sig = 0;
            bool                fav = false;   // GI36
            int                 xlIdx = -1;    // (1.3.3) which unit leaves
        };
        SellConfirm g_confirm;

        // F3/F4 merchant option toggles (settings window; persisted in the
        // ui ini by WinManager alongside skin/lang).
        bool g_merchGoldInf = false;
        bool g_merchBuysAll = false;

        // ---- F7: per-container remembered spots (kLoot / kSteal) ----
        // Key = "Plugin.esp|0xLocalID" (load-order stable, mirrors the grid);
        // absent items KEEP their spot (re-storing the same form returns it).
        struct ContSpot
        {
            int col = -1;
            int row = -1;
            int w = 1;   // footprint at record time: absent spots still hold
            int h = 1;   // their shelf open (board height includes them)
            // GI62: quarter-turns clockwise. w/h ALREADY reflect it -- this is
            // kept so the sprite draws at the angle the player left it at, and
            // so taking the item back carries the turn home.
            int rot = 0;
            // ★★A coin pouch on the shelf keeps the gold that walked in with
            // it. Same answer the grid gives on its own board (the amount
            // hangs off the SLOT, not the item) -- the engine has nowhere
            // to hang per-instance data, so the shelf position is what
            // "this pouch" means here.
            int gold = 0;
            // ★(1.3.0-D) a stored BAG's contents, riding its spot the same
            // way the pouch's gold does. Their cells are hidden while the
            // bundle holds them; the spot's death by TAKE sends them home,
            // any other death simply spills them onto the shelf (the hiding
            // is derived from live bundles, so dropping one un-hides).
            std::vector<BundleItem> bundle;
            // ★★WHICH UNIT THIS SPOT IS SHOWING -- as data, not as a name.
            // These used to live inside the spot's KEY, which made a shelf
            // position depend on the item's mutable state: clear an ownership
            // stamp, spend a charge, and the spot belonged to a pool nothing
            // answered to. It was replaced by a fresh one, and because a
            // pouch's gold and a bag's contents hang off the SPOT, the amount
            // and the bag went with it. Mirrors Grid::LayoutEntry.
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            // GI41: a worn unit is never interchangeable with a spare (one is
            // locked, the other is not), so worn-ness partitions the match
            // rather than merely scoring it.
            bool worn = false;
        };
        struct ContLayout
        {
            std::map<std::string, ContSpot> spots;
            std::uint32_t                   stamp = 0;   // LRU recency
        };
        std::unordered_map<RE::FormID, ContLayout> g_contLayouts;
        // ★★ASKED OF THE LAYOUT, NOT CARRIED ON THE CELL. The amount was
        // being copied onto PartnerCell during slot assignment and read back
        // at draw time -- one hop too many: the cell list is rebuilt between
        // the two, so a pouch that HAD been shelved with 6,943 G still drew
        // the empty-pouch art. The layout is the thing that actually keeps
        // the number; read it where it is used.
        [[nodiscard]] int ShelfGoldOf(const std::string& a_spotKey);
        std::uint32_t                              g_contStamp = 0;
        constexpr size_t                           kContLayoutMax = 128;

        // ---- (1.3.0-D) bundles in transit --------------------------------
        // STORE side: contents queued alongside their bag, waiting for the
        // bag's shelf spot to be born (the spot exists only after the engine
        // transfer lands and the cell appears). Per container, so a bundle
        // can never hide same-form items in some OTHER chest.
        struct PendingBundle
        {
            RE::FormID              cont = 0;   // container ref this store aimed at
            RE::FormID              bagForm = 0;
            std::vector<BundleItem> items;
        };
        std::vector<PendingBundle> g_pendingBundles;
        // TAKE side: a bundle whose bag is on its way to the player. The
        // grid's rebuild claims it when the bag's fresh tile appears and
        // routes the arrived contents back INTO the bag.
        std::vector<PendingBundle> g_incomingBundles;

        // ---- (1.3.1/1.3.2c) OPEN shelf bags -------------------------------
        // One window per opened bag spot, ordered by open time (ESC closes
        // the newest first) -- the player-bag grammar, where every open bag
        // has its own window. The bag's form rides along for the title.
        struct ShelfBagWin
        {
            std::string spot;
            RE::FormID  form = 0;
        };
        std::vector<ShelfBagWin> g_shelfBags;

        // (1.3.2a) the shelf POUCH withdraw window (one at a time, like the
        // player's) -- spot + form + the slider's current pick
        std::string g_shelfPouchSpot;
        RE::FormID  g_shelfPouchForm = 0;
        int         g_shelfPouchSlider = 0;

        // a carry lifted out of that window. The engine item stays put (it is
        // already in the container, hidden by the bundle) -- consuming the
        // carry (take home / drop on the shelf) is what removes the entry and
        // lets the cell surface. col/row pin the EXACT entry (two identical
        // stacks are told apart only by their anchors).
        struct BundleCarry
        {
            bool          active = false;
            RE::FormID    cont = 0;
            std::string   spot;
            RE::FormID    form = 0;
            std::uint16_t sig = 0;
            int           col = -1;
            int           row = -1;
        };
        BundleCarry g_bundleCarry;

        // ★(1.3.2) the marker bits of the unit riding the cursor from the
        // PARTNER side (a shelf cell, or a bundle entry). The carry itself
        // has nowhere to keep them and the shelf cell's own record dies with
        // the lift, so they are parked here until the drop seats them.
        std::uint8_t g_carryGlow = 0;
        bool         g_carryStolen = false;   // (1.3.2) its ownership, likewise

        [[nodiscard]] std::vector<BundleItem> TakePendingBundle(RE::FormID a_bagForm)
        {
            auto* p = Partner();
            const RE::FormID cont = p ? p->GetFormID() : 0;
            for (auto it = g_pendingBundles.begin(); it != g_pendingBundles.end(); ++it) {
                if (it->bagForm != a_bagForm || it->cont != cont) continue;
                auto v = std::move(it->items);
                g_pendingBundles.erase(it);
                return v;
            }
            return {};
        }

        // pending drop-cell spot for a STACK store (slider round-trip)
        struct StoreHint
        {
            RE::TESBoundObject* obj = nullptr;
            int                 col = -1;
            int                 row = -1;
            std::uint16_t sig = 0;   // GI18
            int           rot = 0;   // GI62: the angle survives the slider too
        };
        StoreHint g_storeHint;

        // GI18: drop positions waiting for their item to actually arrive. The
        // engine transfer runs on the next Tick, so the cell does not exist yet
        // when the player lets go -- and by then its ordinal in THIS container
        // is unknown anyway. (form, sig) is the only identity that survives the
        // move: the ExtraDataList travels intact, so its signature does too.
        struct PendingSpot
        {
            RE::FormID    form = 0;
            std::uint16_t sig = 0;
            int           col = -1;
            int           row = -1;
            // GI23: for a REARRANGE, the slot the carry took with it. Empty for a
            // store, where the unit has no slot here yet. Guessing which slot to
            // move ("reuse the first one in the pool") picked a sibling's slot,
            // so dragging the second dagger somewhere moved the FIRST one.
            std::string   slotKey;
            // GI62: the angle the player dropped it at. This is the whole reason
            // a turn survives inventory -> container: the spot on the other side
            // is created FROM this hint, so it is created already turned.
            int           rot = 0;
        };
        std::vector<PendingSpot> g_pendingSpots;

        // GI20: the pool slot of the cell the player is acting on. A take must
        // free THAT slot; without it the pool merely loses its trailing position
        // and every cell after the taken one shuffles up -- which is what made
        // looting follow storage order instead of the cursor.
        std::string g_actingSpot;

        void ConsumeActingSpot(RE::TESBoundObject* a_obj)
        {
            if (g_actingSpot.empty()) return;
            std::vector<BundleItem> bundle;
            if (auto* p = Partner()) {
                if (const auto ci = g_contLayouts.find(p->GetFormID()); ci != g_contLayouts.end()) {
                    // ★★(1.3.0-B) A TAKEN POUCH BRINGS ITS GOLD HOME. This erase
                    // IS the take/buy path's slot retirement, and it is the only
                    // place that knows exactly which slot leaves. The old hook
                    // waited for the whole POOL to vanish from the board -- but
                    // absent items KEEP their spot by design, so the common
                    // single take never tripped it and the amount died with the
                    // spot ("pouch returned but nothing was away"). Deposit
                    // first, then retire.
                    if (const auto si = ci->second.spots.find(g_actingSpot);
                        si != ci->second.spots.end()) {
                        if (si->second.gold > 0) {
                            SKSE::log::info("[LOOT] pouch taken back with {} G ('{}')",
                                si->second.gold, g_actingSpot);
                            GoldCoins::GiveAwayGold(si->second.gold);
                        }
                        // ★(1.3.0-D) same retirement for a bag: its bundled
                        // contents leave WITH it (queued below, once the spot
                        // is gone and the acting key is cleared -- the takes
                        // re-enter this function and must find it empty).
                        bundle = std::move(si->second.bundle);
                    }
                    ci->second.spots.erase(g_actingSpot);
                }
            }
            g_actingSpot.clear();

            if (!bundle.empty() && a_obj) {
                // bundles only ever ride LOOT-container spots, where the
                // partner ref IS the source the takes will pull from
                if (auto* src = Partner()) {
                    auto inv = src->GetInventory();
                    for (const auto& b : bundle) {
                        auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(b.form);
                        if (!obj) continue;
                        int present = 0;
                        if (const auto ei = inv.find(obj); ei != inv.end()) {
                            present = ei->second.first;
                        }
                        // the chest may have respawned some of it away: take
                        // what is actually there, silently drop the rest
                        const int take = (std::min)(b.count, present);
                        if (take <= 0) continue;
                        RequestTake(obj, take, 0, b.sig, false);
                    }
                }
                SKSE::log::info("[LOOT] bag taken back, {} bundled kind(s) follow",
                    bundle.size());
                g_incomingBundles.push_back({ 0, a_obj->GetFormID(), std::move(bundle) });
                // a manifest whose bag never arrived (refused take) must not
                // pile up and mis-claim some future bag of the same form
                if (g_incomingBundles.size() > 8) {
                    g_incomingBundles.erase(g_incomingBundles.begin());
                }
            }
        }

        // GI41: pickpocket boards keep their layout too.
        //
        // They were excluded, so the whole pool/slot pass (everything under
        // `if (cl)`) never ran for them -- every frame was a fresh first-fit in
        // ENUMERATION order. Planting an item on a mark therefore re-dealt the
        // entire board, and because the cell a lock belongs to is identified by
        // where it sits, the lock appeared to move onto a different dagger.
        //
        // A merchant can shuffle harmlessly because nothing there is locked.
        // A pickpocket target has worn gear that must stay put, so it needs the
        // same stable layout a container gets.
        //
        // IsLootMode() is deliberately NOT widened -- it also drives the red
        // STEAL chrome and the take/store semantics, neither of which applies.
        // ★★The bottom strip's height, derived from the TEXT it has to hold.
        // It was a flat 30*S, which fit while the strip drew at the default
        // font size -- then the label moved to Theme::TextOutlined at
        // FontValue() (20px) to match the player's gold bar, and 8 above plus
        // 20 of glyph left two pixels under it. The number now follows the
        // font, so the same thing cannot happen the next time either changes.
        [[nodiscard]] float BottomStripH()
        {
            const float S = Theme::Scale();
            return 8.0f * S + Theme::FontValue() + 10.0f * S;
        }

        bool SpotMemoryOn()
        {
            return IsLootMode(g_mode) || g_mode == Mode::kPickpocket;
        }

        // ★★(1.3.3) A FOLLOWER CARRIES 10 x 8, AND NO MORE. A chest is a
        // hole in the world and may be bottomless; a companion is a person
        // with a pack, and an unbounded one turns every follower into a
        // second inventory with no cost. The board still GROWS past it when
        // the follower already holds more (their own gear, a quest item, a
        // gift from a script) -- nothing is ever hidden or dropped. The
        // limit governs what the PLAYER may add, which is the only half the
        // player controls.
        constexpr int kCompanionRows = 8;

        [[nodiscard]] bool CompanionPartner()
        {
            if (!IsLootMode(g_mode)) return false;
            auto* p = Partner();
            auto* a = p ? p->As<RE::Actor>() : nullptr;
            // dead followers are corpses: loot them like any other container
            return a && !a->IsDead() && a->IsPlayerTeammate();
        }

        // ---- F6b: pickpocket helpers ----

        // vanilla success chance via the ENGINE formula (weight / value /
        // skills / perks / sleep bonus / 90% cap all handled inside)
        int PickpocketChance(RE::TESBoundObject* a_obj, int a_count,
                             RE::InventoryEntryData* a_entry)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* partner = Partner();
            auto* target = partner ? partner->As<RE::Actor>() : nullptr;
            if (!player || !target || !a_obj) return 0;
            const float thief = player->AsActorValueOwner()->GetActorValue(
                RE::ActorValue::kPickpocket);
            const float mark = target->AsActorValueOwner()->GetActorValue(
                RE::ActorValue::kPickpocket);
            const int unitValue = a_entry ? a_entry->GetValue()
                                          : static_cast<int>(a_obj->GetGoldValue());
            const auto value = static_cast<std::uint32_t>(
                (std::max)(0, unitValue * a_count));
            const float weight = a_obj->GetWeight() * static_cast<float>(a_count);
            const bool detected = target->RequestDetectionLevel(player) > 0;
            const int chance = RE::AIFormulas::ComputePickpocketSuccess(
                thief, mark, value, weight, player, target, detected, a_obj);
            return std::clamp(chance, 0, 100);
        }

        // Perfect Touch (Skyrim.esm PerfectTouch 0x058205): worn gear is
        // otherwise locked
        bool HasPerfectTouch()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x00058205);
            return player && perk && player->HasPerk(perk);
        }

        // Poisoned (Skyrim.esm Poisoned 0x105F28): planted poisons silently
        // harm the mark
        bool HasPoisonedPerk()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(0x00105F28);
            return player && perk && player->HasPerk(perk);
        }

        // stable item key (same format as the grid's FormKey).
        //
        // GI13: it must be per UNIT now. Keyed by form alone, every cell of the
        // same form resolved to the SAME remembered spot and they were all drawn
        // on top of each other -- which is why the second and later daggers
        // retriggered the hover blip every frame: overlapping InvisibleButtons
        // traded IsItemHovered() between them, so Sfx::HoverNote saw a different
        // item id each frame and re-fired forever.
        std::string PartnerKey(RE::TESForm* a_form, std::uint16_t a_uid = 0,
                               std::uint16_t a_sig = 0, int a_ord = 0)
        {
            // GetLocalFormID() dereferences the source file unchecked, so a
            // runtime-created form (brewed potion, player enchantment) has to
            // fall back to its whole FormID -- it has no local id to take.
            auto* file = a_form->GetFile(0);
            std::string key = file ? std::string(file->GetFilename()) : "Dynamic";
            char buf[32];
            std::snprintf(buf, sizeof(buf), "|0x%06X",
                file ? a_form->GetLocalFormID() : a_form->GetFormID());
            key += buf;
            if (a_uid != 0) {
                std::snprintf(buf, sizeof(buf), "@%04X", a_uid);   // engine uniqueID
                key += buf;
            } else if (a_sig != 0) {
                std::snprintf(buf, sizeof(buf), "~%04X", a_sig);   // GI14 content signature
                key += buf;
            }
            if (a_ord > 0) {
                std::snprintf(buf, sizeof(buf), "#%d", a_ord);
                key += buf;
            }
            return key;
        }
    }

    RE::TESObjectREFR* SourceRef();   // defined below (merchant wares / container)

    Mode CurrentMode() { return g_mode; }

    void Enter(Mode a_mode, RE::TESObjectREFR* a_partner)
    {
        g_mode = a_mode;
        g_partner = a_partner ? a_partner->CreateRefHandle() : RE::ObjectRefHandle{};
        // §2-C: a pouch sold while bartering releases its stored gold first
        GoldCoins::SetBarterContext(a_mode == Mode::kBarter);

        // F7: touch this container's spot memory (LRU) — the map is capped;
        // the least recently OPENED container falls off first
        if (IsLootMode(a_mode) && a_partner) {
            g_contLayouts[a_partner->GetFormID()].stamp = ++g_contStamp;
            while (g_contLayouts.size() > kContLayoutMax) {
                // ★(1.3.0-B) never evict a shelf that still banks a pouch's
                // gold -- the pouch is still IN that chest and the layout is
                // the only record of the amount. Oldest goldless one goes; if
                // every remembered chest banks gold, keep them all (the cap is
                // a memory bound, not a correctness bound).
                auto victim = g_contLayouts.end();
                for (auto it = g_contLayouts.begin(); it != g_contLayouts.end(); ++it) {
                    bool banks = false;
                    for (const auto& [k, s] : it->second.spots) {
                        if (s.gold > 0) { banks = true; break; }
                    }
                    if (banks) continue;
                    if (victim == g_contLayouts.end() ||
                        it->second.stamp < victim->second.stamp) {
                        victim = it;
                    }
                }
                if (victim == g_contLayouts.end()) break;
                g_contLayouts.erase(victim);
            }
        }

        // B: prefetch the partner's WHOLE stock up front — one caching burst
        // when the window opens instead of a trickle while scrolling. For
        // barter this is the vendor chest (SourceRef), not the actor.
        if (auto* src = SourceRef()) {
            // §RELEASE-①: stock the coin pouch at general-goods vendors before
            // the prefetch walks the list, so the new item gets its icon in the
            // same caching burst as the rest of the stock.
            if (a_mode == Mode::kBarter && a_partner) {
                GoldCoins::SeedVendorStock(a_partner->As<RE::Actor>(), src);
            }
            auto* cache = IconCache::GetSingleton();
            for (const auto& [obj, data] : src->GetInventory()) {
                if (obj && data.first > 0) cache->Prefetch(obj);
            }
        }
    }

    namespace
    {
        // ★B4-3b: a struck queue withdraws its ledger entries. Outgoing
        // requests submit at COMMIT time now (RequestStore/Sell/PickStore),
        // so anything still queued when the queue is cleared -- a lost
        // pickpocket roll force-closing the menu, a teardown whose flush
        // failed -- holds an open entry that no event will ever confirm.
        // Left alone they only expire noisily (the recovery is harmless),
        // but a cancelled request's SLOT KEY must come back deliberately:
        // queued, it would be consumed by the form's next confirmation --
        // the bug the two-phase drop exists to prevent. Entries already
        // processed this Tick were confirmed by their own events and match
        // nothing here -- the cancel is naturally idempotent.
        void CancelQueuedOutgoing(const char* a_why)
        {
            for (const auto& q : g_xfer) {
                if (!q.obj) continue;
                if (q.dir != XferReq::kStore && q.dir != XferReq::kSell &&
                    q.dir != XferReq::kPickStore) {
                    continue;
                }
                for (const auto& e :
                     Ledger::Cancel(q.obj->GetFormID(), q.count, a_why)) {
                    if (!e.slot.empty()) Grid::CancelSlotDrop(e.form, e.slot);
                }
            }
        }
    }

    void Reset()
    {
        // B12: a transfer queued on the final render frame would be dropped
        // silently — flush BEFORE the session state is torn down.
        // (ProcessTransfers resolves the container through g_partner and the
        // removal reason through g_mode; the old clear-first order nulled
        // SourceRef() so this flush never actually ran.)
        if (!g_xfer.empty()) ProcessTransfers();
        g_mode = Mode::kNormal;
        g_partner = {};
        CancelQueuedOutgoing("session teardown");   // B4-3b: their ledger halves
        g_xfer.clear();   // anything STILL queued (flush failed) is dropped...
        Grid::ClearAllPendingRemoves();   // ...so their pending marks must go too
        Grid::ClearDropHint();            // B2
        g_slider.active = false;
        g_confirm.active = false;
        g_storeHint = {};        // F7: session-scoped drop hint (the grid geometry
        g_pendingSpots.clear();  // re-arms per frame in DrawWindows — kNormal
                                 // leaves it dead, so no teardown needed here)
        // (1.3.0-C) a leaving-pouch hint whose transfer never fired (lost
        // pick roll, refused move) must not name a tile for the NEXT session
        GoldCoins::NotePouchLeaving({});
        // (1.3.0-D) a store bundle whose bag spot was never born (the window
        // closed the same instant) is dropped -- the contents are simply
        // loose in the chest, visible next open. Incoming bundles SURVIVE:
        // their claim happens on the player-side rebuild, which may run
        // after this session is gone.
        g_pendingBundles.clear();
        g_shelfBags.clear();   // (1.3.1) the shelf windows die with the session
        g_shelfPouchSpot.clear();
        g_bundleCarry = {};
        g_carryGlow = 0;
        GoldCoins::SetBarterContext(false);
    }

    bool SliderActive() { return g_slider.active; }

    void OpenSlider(RE::TESBoundObject* a_obj, int a_max, XferDir a_dir,
                    const std::string& a_srcKey, int a_unitValue,
                    std::uint16_t a_uid, std::uint16_t a_sig, bool a_worn, bool a_fav,
                    int a_xlIdx)
    {
        if (!a_obj || a_max <= 1) return;
        // player-receiving dirs: cap the slider at what the boards (main +
        // open bags + partial stacks) can actually accept, so a stack buy/take
        // can never overflow the inventory (Phase 7 polish).
        if (a_dir == XferDir::kTake || a_dir == XferDir::kBuy ||
            a_dir == XferDir::kPickTake) {
            const int fit = Grid::MaxAcceptUnits(a_obj, a_max);
            if (fit <= 0) {
                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                return;
            }
            a_max = fit;   // may become 1: the slider then offers exactly one
        }
        // start at half the max (split-friendly default), min 1
        g_slider = {};
        g_slider.active = true;
        g_slider.obj = a_obj;
        g_slider.max = a_max;
        g_slider.value = (std::max)(1, a_max / 2);
        g_slider.dir = a_dir;
        g_slider.srcKey = a_srcKey;
        g_slider.unitValue = a_unitValue;
        g_slider.uid = a_uid;   // GI25
        g_slider.sig = a_sig;
        g_slider.worn = a_worn;
        g_slider.fav = a_fav;   // GI36
        g_slider.xlIdx = a_xlIdx;   // (1.3.3)
        Sfx::SelectOn();   // click / shift+click opened the quantity popup
    }

    bool IsPopupOpen()
    {
        return g_confirm.active || g_slider.active || !g_shelfBags.empty() ||
               !g_shelfPouchSpot.empty();
    }

    bool CloseTopPopup()
    {
        // I/ESC close the TOP sub-window first, the inventory only when
        // nothing is left (confirm sits above the slider)
        if (g_confirm.active) {
            g_confirm.active = false;
            return true;
        }
        if (g_slider.active) {
            g_slider.active = false;
            Grid::ClearDropHint();   // B2
            return true;
        }
        if (!g_shelfPouchSpot.empty()) {   // (1.3.2a) the pouch window
            g_shelfPouchSpot.clear();
            return true;
        }
        if (!g_shelfBags.empty()) {   // (1.3.2c) the NEWEST bag window
            g_shelfBags.pop_back();
            return true;
        }
        return false;
    }

    namespace
    {
        // GI42: may a worn unit legally leave the partner right now? Loot and
        // owned-container theft always may (a corpse's gear is the point);
        // a living mark only with Perfect Touch; a merchant never.
        bool WornExportLegal()
        {
            return IsLootMode(g_mode) ||
                   (g_mode == Mode::kPickpocket && HasPerfectTouch());
        }

        // GI42: one resolution for click time and Tick time. NEVER cache the
        // returned pointer across frames -- resolve again where it is used.
        Grid::UnitChoice ResolveSource(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                       std::uint16_t a_sig, bool a_fromWorn)
        {
            auto* src = SourceRef();
            auto* entry = src ? Grid::LiveEntryOf(src, a_obj) : nullptr;
            if (a_fromWorn) {
                // a worn unit may have neither uid nor signature -- being worn
                // is itself the handle
                if (auto* xl = Grid::WornExtraMatching(entry, a_uid, a_sig, 0)) {
                    return { Grid::PickKind::kNamed, xl };
                }
                return Grid::PoolChoice(entry, a_uid, a_sig, /*nameWorn*/ true,
                                        WornExportLegal());
            }
            // GI35: a spare cell must never NAME the worn list -- but that only
            // removed OUR ability to pick it, not the engine's. PoolChoice is
            // what removes the engine's.
            return Grid::PoolChoice(entry, a_uid, a_sig, /*nameWorn*/ false,
                                    WornExportLegal());
        }
    }

    void RequestTake(RE::TESBoundObject* a_obj, int a_count,
                     std::uint16_t a_uid, std::uint16_t a_sig, bool a_fromWorn,
                     bool a_useAfter)
    {
        if (a_obj && a_count > 0) {
            // GI42: refuse BEFORE arming any suppression -- a transfer that will
            // not run must not leave the board and the engine disagreeing.
            if (!ResolveSource(a_obj, a_uid, a_sig, a_fromWorn).ok()) {
                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                return;
            }
            XferReq req;
            req.dir      = XferReq::kTake;
            req.obj      = a_obj;
            req.count    = a_count;
            req.uid      = a_uid;
            req.sig      = a_sig;
            req.fromWorn = a_fromWorn;
            req.useAfter = a_useAfter;
            g_xfer.push_back(std::move(req));
            NoteOut(a_obj, a_uid, a_sig, a_count);
            ConsumeActingSpot(a_obj);   // GI20: the hovered cell's slot, not the last one
            ConsumeBundleCarry(a_obj, a_count);   // (1.3.1) a shelf-bag carry taken home
        }
    }

    void RequestStore(RE::TESBoundObject* a_obj, int a_count,
                      std::uint16_t a_uid, std::uint16_t a_sig, bool a_fav,
                      int a_xlIdx, const std::string& a_srcKey)
    {
        if (a_obj && a_count > 0) {
            g_xfer.push_back({ XferReq::kStore, a_obj, a_count, 0, 0, a_srcKey,
                               a_uid, a_sig, false, a_fav, a_xlIdx });
            // ★B4-3b: the ledger's books open when the PLAYER commits -- the
            // same moment the removal counters arm (NotePendingRemove rides
            // beside every caller of this function). Submitting from the
            // transfer Tick put the two on different clocks, and the click-
            // to-Tick window would go unsuppressed the day the counters are
            // absorbed. A queue struck before its Tick (a lost pickpocket
            // roll clears everything behind it) withdraws through Cancel.
            Ledger::Submit(a_obj->GetFormID(), -a_count, "store", a_uid, a_sig,
                           a_srcKey);
        }
    }

    void RequestBuy(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal,
                    std::uint16_t a_uid, std::uint16_t a_sig)
    {
        // (the guard had no braces: a rejected buy still consumed the acting
        // spot, so the next purchase landed on a cell it was not given)
        if (a_obj && a_count > 0) {
            if (!ResolveSource(a_obj, a_uid, a_sig, false).ok()) {   // GI42
                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                return;
            }
            g_xfer.push_back({ XferReq::kBuy, a_obj, a_count, a_price, a_baseTotal,
                               {}, a_uid, a_sig });
            NoteOut(a_obj, a_uid, a_sig, a_count);
            ConsumeActingSpot(a_obj);   // GI20
        }
    }

    void RequestSell(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal,
                     std::uint16_t a_uid, std::uint16_t a_sig, bool a_fav,
                     int a_xlIdx, const std::string& a_srcKey)
    {
        if (a_obj && a_count > 0) {
            g_xfer.push_back({ XferReq::kSell, a_obj, a_count, a_price, a_baseTotal,
                               a_srcKey, a_uid, a_sig, false, a_fav, a_xlIdx });
            // B4-3b: see RequestStore -- the books open at the commit
            Ledger::Submit(a_obj->GetFormID(), -a_count, "sell", a_uid, a_sig,
                           a_srcKey);
        }
    }

    void RequestPickTake(RE::TESBoundObject* a_obj, int a_count,
                         std::uint16_t a_uid, std::uint16_t a_sig, bool a_fromWorn)
    {
        // (the guard had no braces, so the spot was consumed even when nothing
        // was queued -- a rejected request stole the next cell's placement)
        if (a_obj && a_count > 0) {
            if (!ResolveSource(a_obj, a_uid, a_sig, a_fromWorn).ok()) {   // GI42
                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                return;
            }
            g_xfer.push_back({ XferReq::kPickTake, a_obj, a_count, 0, 0, {}, a_uid, a_sig,
                               a_fromWorn });
            NoteOut(a_obj, a_uid, a_sig, a_count);
            ConsumeActingSpot(a_obj);   // GI25
        }
    }

    void RequestPickStore(RE::TESBoundObject* a_obj, int a_count,
                          std::uint16_t a_uid, std::uint16_t a_sig,
                          const std::string& a_srcKey, bool a_fav, int a_xlIdx)
    {
        if (a_obj && a_count > 0) {
            g_xfer.push_back({ XferReq::kPickStore, a_obj, a_count, 0, 0, a_srcKey,
                               a_uid, a_sig, false, a_fav, a_xlIdx });
            // Every other player -> partner direction suppresses the tile at
            // REQUEST time; only this one waited for the Tick, so the planted
            // item sat back down on the board for a frame or two before being
            // taken away. Same rule as store/sell: the unit leaves the board the
            // moment the player commits, and ProcessTransfers clears it when the
            // engine count actually drops (or when the roll is lost).
            Grid::NotePendingRemove(a_obj, a_srcKey, a_count, a_xlIdx);
            // B4-3b: see RequestStore -- the books open at the commit
            Ledger::Submit(a_obj->GetFormID(), -a_count, "plant", a_uid, a_sig,
                           a_srcKey);
        }
    }

    namespace
    {
        // GI42: the Tick-time resolution -- same rules as click time, resolved
        // FRESH here because a list captured a frame ago may be gone.
        Grid::UnitChoice SourceUnit(RE::TESObjectREFR* a_source, const XferReq& a_r)
        {
            auto* entry = Grid::LiveEntryOf(a_source, a_r.obj);
            if (a_r.fromWorn) {
                if (auto* xl = Grid::WornExtraMatching(entry, a_r.uid, a_r.sig, 0)) {
                    return { Grid::PickKind::kNamed, xl };
                }
                return Grid::PoolChoice(entry, a_r.uid, a_r.sig, true, WornExportLegal());
            }
            return Grid::PoolChoice(entry, a_r.uid, a_r.sig, false, WornExportLegal());
        }

        // GI42 tripwire: the worn lists of one form, as VALUES (uid, sig, count,
        // hand) -- never pointers. Compared around an engine RemoveItem that ran
        // on an unproven nullptr; a difference means the engine moved a unit off
        // a body when we believed it could not.
        std::vector<std::array<int, 4>> WornPrint(RE::TESObjectREFR* a_ref,
                                                  RE::TESBoundObject* a_obj)
        {
            std::vector<std::array<int, 4>> out;
            auto* e = Grid::LiveEntryOf(a_ref, a_obj);
            if (!e || !e->extraLists) return out;
            for (auto* xl : *e->extraLists) {
                if (!xl) continue;
                const bool wr = xl->HasType<RE::ExtraWorn>();
                const bool wl = xl->HasType<RE::ExtraWornLeft>();
                if (!wr && !wl) continue;
                int u = 0;
                if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) u = xu->uniqueID;
                out.push_back({ u, static_cast<int>(Grid::InstanceSigOf(xl)),
                                (std::max)(1, xl->GetCount()), wl ? 2 : 1 });
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        // Wraps an engine removal whose unit choice is UNPROVEN (kFallback):
        // logs loudly if a worn unit moved. No behaviour change -- this is the
        // instrument that decides whether kFallback ever needs to become a
        // refusal too.
        template <class F>
        void GuardedRemove(RE::TESObjectREFR* a_holder, RE::TESBoundObject* a_obj,
                           bool a_guard, const char* a_tag, F&& a_remove)
        {
            if (!a_guard) { a_remove(); return; }
            const auto before = WornPrint(a_holder, a_obj);
            a_remove();
            if (WornPrint(a_holder, a_obj) != before) {
                SKSE::log::error("[XFER] TRIPWIRE {}: engine fallback moved a WORN "
                                 "unit of '{}'", a_tag, a_obj->GetName());
            }
        }
    }

    void ProcessTransfers()
    {
        if (g_xfer.empty()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* source = SourceRef();
        if (!player || !source) { g_xfer.clear(); return; }

        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(0x0000000F);   // Gold001
        bool goldMoved = false;
        bool goldIn = false;    // received gold (sell) vs paid (buy) — one clink
        // vanilla per-item pickup/putdown sounds. Budgeted per batch so
        // "take all" doesn't fire dozens of overlapping sounds at once.
        int soundBudget = 4;
        auto itemSound = [&](RE::TESBoundObject* a_obj, bool a_up) {
            if (soundBudget <= 0 || !a_obj) return;
            --soundBudget;
            player->PlayPickUpSound(a_obj, a_up, false);
        };

        for (const auto& r : g_xfer) {
            // ★1.4/B0 -- registered HERE, once, for all six directions: this is
            // the last point before the engine is asked, and every branch below
            // ends in a RemoveItem that will come back to us as an event. The
            // direction test is the same one RequestXfer uses (see kTake ||
            // kBuy || kPickTake above), so the two cannot drift apart.
            if (r.obj && Ledger::Enabled()) {
                const bool incoming = r.dir == XferReq::kTake ||
                                      r.dir == XferReq::kBuy ||
                                      r.dir == XferReq::kPickTake;
                const char* who =
                    r.dir == XferReq::kTake      ? "take"  :
                    r.dir == XferReq::kStore     ? "store" :
                    r.dir == XferReq::kBuy       ? "buy"   :
                    r.dir == XferReq::kSell      ? "sell"  :
                    r.dir == XferReq::kPickTake  ? "steal" : "plant";
                // ★uid+sig ride along: B0 proved the engine's events never
                // name the unit (§8-2), so this is the only record that does.
                // ★B4-3b: INCOMING only. The outgoing directions submit where
                // the player commits (RequestStore/Sell/PickStore), the same
                // moment their suppression arms -- submitting them here put
                // the ledger and the removal counters on different clocks.
                // An arrival has no click-time suppression and its engine
                // call is this very Tick, so here stays its moment.
                if (incoming) {
                    Ledger::Submit(r.obj->GetFormID(), r.count, who,
                                   r.uid, r.sig, {});
                }
            }
            switch (r.dir) {
            case XferReq::kTake: {
                // partner -> player. Engine moves the extra data too (charge,
                // enchant, poison). Gold folds into the ledger; the coin mirror
                // rebuilds it into tiles. F6a: taking from an OWNED container
                // uses kSteal so the engine flags the items stolen — and the
                // WITNESS/bounty roll is a separate engine call (StealAlarm),
                // which the vanilla menu fires per taken item; without it a
                // seen theft never gained a bounty (user-reported).
                const auto pick = SourceUnit(source, r);
                if (!pick.ok()) {   // GI42: nameless AND unsafe -- do not move
                    ClearOut(r.obj, r.uid, r.sig, r.count);
                    Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                    SKSE::log::error("[XFER] take refused: unnameable '{}' uid "
                        "{:04X} sig {:04X}", r.obj->GetName(), r.uid, r.sig);
                    break;
                }
                if (g_mode == Mode::kSteal) {
                    player->StealAlarm(source, r.obj, r.count,
                        r.obj->GetGoldValue() * r.count, source->GetOwner(), true);
                }
                GuardedRemove(source, r.obj,
                    pick.kind == Grid::PickKind::kFallback, "take", [&]() {
                    source->RemoveItem(r.obj, r.count,
                        g_mode == Mode::kSteal ? RE::ITEM_REMOVE_REASON::kSteal
                                               : RE::ITEM_REMOVE_REASON::kRemove,
                        pick.xl, player);
                });
                ClearOut(r.obj, r.uid, r.sig, r.count);   // engine moved it
                itemSound(r.obj, true);
                // ★(1.3.0) SHELF USE MODE. The unit is in the player's pack as
                // of the RemoveItem above, so from here it is an ordinary
                // right-click -- the player-side path already knows every kind
                // (potion, food, scroll, spell tome). QUEUED, not called: the
                // engine call belongs to Equip::ProcessPending, and this is not
                // its turn.
                // ★xlIdx -1 on purpose: a list POSITION captured on the shelf
                // means nothing in the pack it has just arrived in. The
                // signature travelled with the ExtraDataList and still names it.
                if (r.useAfter) {
                    // ★It is passing through, not moving in: a typed bag must not
                    // adopt a unit that is about to be drunk.
                    Grid::NoteTransientArrival(r.obj->GetFormID());
                    Equip::UseItem(r.obj, r.uid, -1, r.sig, {}, r.count);
                }
                break;
            }
            case XferReq::kStore: {
                // GI36: resolve + strip the star in one call, then hand the very
                // list we got to the engine (rule 58).
                auto* sxl = Grid::ResolveExitUnit(r.obj, r.uid, r.sig, r.count,
                                                  r.fav ? r.count : 0, r.xlIdx);
                // ★TEST ONLY (!simrefuse): skip the ENGINE CALL and nothing
                // else. A real refusal is RemoveItem quietly not taking -- our
                // own bookkeeping still runs, because the engine's silence does
                // not reach back and cancel it. The first version bailed out of
                // the whole case, which also skipped ClearPendingRemove, so the
                // tile stayed suppressed and the recovery rebuild had nothing to
                // show. That was a situation the game cannot produce: §10-7's
                // rule about stress tools, and I broke it the same day I wrote
                // it down.
                if (Ledger::SimRefuse()) {
                    SKSE::log::warn("[XFER] !simrefuse: engine call SKIPPED for "
                                    "store '{}' x{}", r.obj->GetName(), r.count);
                } else {
                    GuardedRemove(player, r.obj, sxl == nullptr, "store", [&]() {   // GI42
                        player->RemoveItem(r.obj, r.count,
                            RE::ITEM_REMOVE_REASON::kStoreInContainer, sxl, source);
                    });
                }
                // (B4-3c: no counter to drain -- the engine event that this
                // call just fired has already confirmed the ledger entry)
                itemSound(r.obj, false);
                break;
            }
            case XferReq::kBuy: {
                // merchant -> player; player pays, merchant receives.
                const auto pick = SourceUnit(source, r);
                if (!pick.ok()) {   // GI42: e.g. the merchant WEARS the twin
                    ClearOut(r.obj, r.uid, r.sig, r.count);
                    Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                    SKSE::log::error("[XFER] buy refused: unnameable '{}' uid "
                        "{:04X} sig {:04X}", r.obj->GetName(), r.uid, r.sig);
                    break;
                }
                GuardedRemove(source, r.obj,
                    pick.kind == Grid::PickKind::kFallback, "buy", [&]() {
                    source->RemoveItem(r.obj, r.count, RE::ITEM_REMOVE_REASON::kRemove,
                        pick.xl, player);
                });
                if (gold && r.price > 0) {
                    player->RemoveItem(gold, r.price, RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                    source->AddObjectToContainer(gold, nullptr, r.price, nullptr);
                    goldMoved = true;
                    // B: the payment dissolved coin tiles -> tell the spill pass
                    // to treat those cells as still occupied for placement.
                    Grid::NotePaidGold(r.price);
                }
                ClearOut(r.obj, r.uid, r.sig, r.count);   // engine moved it
                itemSound(r.obj, true);   // the purchase lands in your hands
                // vanilla speech XP: base value through the AVIF skill curve
                if (const float xp = SpeechXP(r.base); xp > 0.0f)
                    player->AddSkillExperience(RE::ActorValue::kSpeech, xp);
                break;
            }
            case XferReq::kSell:
                // D4: same reasoning as kStore -- name the unit that leaves.
                // player -> merchant; player receives, merchant pays. kSelling
                // lets the engine clear ownership/stolen flags.
                {
                auto* sxl = Grid::ResolveExitUnit(r.obj, r.uid, r.sig, r.count,   // GI36
                                                  r.fav ? r.count : 0, r.xlIdx);
                GuardedRemove(player, r.obj, sxl == nullptr, "sell", [&]() {   // GI42
                    player->RemoveItem(r.obj, r.count, RE::ITEM_REMOVE_REASON::kSelling,
                        sxl, source);
                });
                }
                if (gold && r.price > 0) {
                    player->AddObjectToContainer(gold, nullptr, r.price, nullptr);
                    source->RemoveItem(gold, r.price, RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                    goldMoved = true;
                    goldIn = true;
                }
                if (const float xp = SpeechXP(r.base); xp > 0.0f)
                    player->AddSkillExperience(RE::ActorValue::kSpeech, xp);
                break;
            case XferReq::kPickTake: {
                // GI42: judge the pick BEFORE the roll. Refusing after
                // AttemptPickpocket would leave the XP, the detection roll and
                // the sound already spent on a move that never happens.
                if (!SourceUnit(source, r).ok()) {
                    ClearOut(r.obj, r.uid, r.sig, r.count);
                    Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                    SKSE::log::error("[XFER] pick refused: unnameable '{}' uid "
                        "{:04X} sig {:04X}", r.obj->GetName(), r.uid, r.sig);
                    break;
                }
                // F6b: the ENGINE rolls the attempt (chance / detection /
                // crime response / XP live inside AttemptPickpocket).
                auto inv = source->GetInventory();
                const auto ei = inv.find(r.obj);
                if (ei == inv.end() || ei->second.first < r.count) break;
                const int before = ei->second.first;
                const bool won = player->AttemptPickpocket(source,
                    ei->second.second.get(), r.count, true);
                if (!won) {
                    // caught: the engine raised the alarm — force-close the
                    // menu; everything already taken this session stays.
                    // Nothing left the target, so drop every suppression this
                    // request and the ones still queued behind it armed.
                    g_outPool.clear();
                    g_outForm.clear();
                    CancelQueuedOutgoing("pickpocket caught");   // B4-3b
                    g_xfer.clear();
                    UIRoot::Close();
                    Grid::RequestRebuild();
                    Grid::MarkCapacityDirty();
                    return;
                }
                // defensive: some paths move the item inside the attempt —
                // move it ourselves only if it's still on the target
                int now = 0;
                for (auto& [obj2, d2] : source->GetInventory()) {
                    if (obj2 == r.obj) { now = d2.first; break; }
                }
                if (now >= before) {
                    // D4/GI42: resolve AGAIN -- the roll ran engine code, and a
                    // list captured before it is not trusted across that call.
                    const auto pick = SourceUnit(source, r);
                    if (!pick.ok()) {
                        ClearOut(r.obj, r.uid, r.sig, r.count);
                        SKSE::log::error("[XFER] pick refused post-roll: '{}'",
                            r.obj->GetName());
                        break;
                    }
                    GuardedRemove(source, r.obj,
                        pick.kind == Grid::PickKind::kFallback, "pick", [&]() {
                        source->RemoveItem(r.obj, r.count,
                            RE::ITEM_REMOVE_REASON::kSteal, pick.xl, player);
                    });
                }
                // ★★★KNOWN EXCEPTION, accepted (author's call) -- do not
                // re-investigate from scratch. AMMO the player reverse-
                // pickpocketed INTO the mark comes back WITHOUT a stolen tag.
                // Everything else does get one, ammo the mark owned itself does
                // get one, and every other STACKABLE (potions, ingredients) gets
                // one on the same round trip. Measured with a temporary
                // [PICKOWN] log, since removed; these three were ruled out:
                //   · AttemptPickpocket moving the item itself, which would skip
                //     the kSteal removal below   -> engineMovedItself = false
                //   · that removal not running   -> ourRemoveRan = true
                //   · a surviving PLAYER ownership stamp that IsOwnedBy then
                //     skips -> there is no stamp: the units arrive with NO
                //     ExtraDataList at all ("player lists: (none)")
                // With no list there is nothing to stamp either -- an
                // ExtraDataList cannot be created from a plugin -- so any fix
                // has to be upstream of the arrival, in whatever drops the list
                // on the way in or out. Not pursued.
                // ★Note this is also the one place our pickpocket matches VANILLA
                // and the rest of it does not: vanilla treats anything you put on
                // a mark as still yours, so taking it back is never theft. Ours
                // makes it the mark's property, which the author prefers and
                // deliberately kept -- ammo is simply the case that stayed
                // vanilla-shaped.
                ClearOut(r.obj, r.uid, r.sig, r.count);   // engine moved it
                itemSound(r.obj, true);
                break;
            }
            case XferReq::kPickStore: {
                // F6b reverse-pickpocket: same engine roll, item flows the
                // other way (fromContainer = false).
                auto pinv = player->GetInventory();
                const auto ei = pinv.find(r.obj);
                if (ei == pinv.end() || ei->second.first < r.count) break;
                const int before = ei->second.first;
                const bool won = player->AttemptPickpocket(source,
                    ei->second.second.get(), r.count, false);
                if (!won) {
                    // the roll failed: nothing leaves, so give back every
                    // suppression this request (and any still queued behind it)
                    // put in place at click time
                    g_outPool.clear();
                    g_outForm.clear();
                    // B4-3c: the counter drains that lived here went with the
                    // counters. r rides in g_xfer too -- its own ledger entry
                    // cancels with the queue's in one sweep.
                    CancelQueuedOutgoing("pickpocket roll lost");
                    g_xfer.clear();
                    UIRoot::Close();
                    Grid::RequestRebuild();
                    Grid::MarkCapacityDirty();
                    return;
                }
                // (the suppression was armed at REQUEST time -- see
                // RequestPickStore. Arming it here, one Tick after the click,
                // left the planted item on the board for those frames and then
                // yanked it: a plant blinked every single time.)
                int now = 0;
                for (auto& [obj2, d2] : player->GetInventory()) {
                    if (obj2 == r.obj) { now = d2.first; break; }
                }
                if (now >= before) {
                    // player side: the worn-excluding resolver, so planting an
                    // item can never hand over the copy you are wearing
                    // GI36: only inside the WIN branch -- a failed roll must
                    // leave the star exactly where it was.
                    auto* sxl = Grid::ResolveExitUnit(r.obj, r.uid, r.sig, r.count,
                                                      r.fav ? r.count : 0, r.xlIdx);
                    GuardedRemove(player, r.obj, sxl == nullptr, "plant", [&]() {   // GI42
                        player->RemoveItem(r.obj, r.count,
                            RE::ITEM_REMOVE_REASON::kStoreInContainer, sxl, source);
                    });
                }
                // (B4-3c: whichever branch moved the item -- the attempt
                // itself or our RemoveItem above -- its container event has
                // confirmed the ledger entry; no counter left to drain)
                // Poisoned perk: the vanilla menu applies a planted poison to
                // the mark OUTSIDE AttemptPickpocket, so mirror it here — cast
                // the poison's effects on the target (self-sourced caster, no
                // blame actor = silent, no aggro/crime) and consume ONE planted
                // unit; any extra units stay planted as regular items.
                if (auto* poison = r.obj->As<RE::AlchemyItem>();
                    poison && poison->IsPoison() && HasPoisonedPerk()) {
                    if (auto* mark = source->As<RE::Actor>()) {
                        if (auto* caster = mark->GetMagicCaster(
                                RE::MagicSystem::CastingSource::kInstant)) {
                            caster->CastSpellImmediate(poison, false, mark,
                                1.0f, false, 0.0f, nullptr);
                        }
                        mark->RemoveItem(r.obj, 1,
                            RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        SKSE::log::info("[PICK] Poisoned perk: applied to mark");
                    }
                }
                itemSound(r.obj, false);
                break;
            }
            }
        }
        g_xfer.clear();
        if (goldMoved && gold) {
            // one coin clink per transaction batch: picked up when selling,
            // put down when paying
            player->PlayPickUpSound(gold, goldIn, false);
        }
        if (goldMoved) GoldCoins::MarkDirty();   // rebuild the coin mirror
        // ★★1.4/B3: no tail rebuild any more, and the reason is measured
        // (!rbdrop, §3-9): fourteen asks dropped over a full partner-window
        // session -- withdraw, store, buy, sell, take-all -- and nothing went
        // stale. Every unit this batch moved raises a container event, and the
        // sink's per-form reconcile (or its own rebuild fallback) is the one
        // that answers it. The pickpocket-failure returns above keep theirs:
        // they close the menu with suppressions half-armed, which is exactly
        // the discontinuity a full rebuild is for.
        Grid::MarkCapacityDirty();
    }

    namespace
    {
        // GI46: largest n in [0, a_cap] the payer can afford. Totals round ONCE
        // on the whole amount (B7), so a division is off by one at the edges --
        // probe the real total function instead (monotonic, binary search).
        int MaxAffordable(bool a_buy, RE::TESBoundObject* a_obj, int a_unitValue,
                          int a_cap, int a_gold)
        {
            if (a_cap <= 0) return 0;
            const auto total = [&](int a_n) {
                return a_buy ? BuyPriceTotal(a_obj, a_unitValue, a_n)
                             : SellPriceTotal(a_obj, a_unitValue, a_n);
            };
            if (total(a_cap) <= a_gold) return a_cap;
            int lo = 0, hi = a_cap;   // total(lo) affordable, total(hi) not
            while (hi - lo > 1) {
                const int mid = lo + (hi - lo) / 2;
                (total(mid) <= a_gold ? lo : hi) = mid;
            }
            return lo;
        }
    }

    // ★★★WHAT TO PUT IN A POPUP'S TITLE, coins included. The coin forms carry NO
    // name on purpose (the esp blanks them so TrueHUD skips them, and the grid
    // shows the amount badge instead), so the "?" fallback -- meant for a form
    // with a genuinely missing name -- is what the player saw when splitting
    // gold. Name it what the rest of the UI calls it; Lang::Str::Gold follows
    // the language setting.
    // ★SentenceCase because that string is the stats panel's LABEL and is
    // spelled "GOLD" for that row. Every other title here is an item name in
    // ordinary case, so the shout stood out as the one word in caps. Non-ASCII
    // scripts pass through untouched.
    // ★One function because it was two copies, and the second one's comment
    // ("Same nameless-coin case as the slider above") was the tell.
    [[nodiscard]] std::string PopupTitleOf(RE::TESBoundObject* a_obj)
    {
        if (a_obj && GoldCoins::IsCoinForm(a_obj->GetFormID()) &&
            !GoldCoins::IsPouch(a_obj->GetFormID())) {
            return Lang::SentenceCase(Lang::T(Lang::Str::Gold));
        }
        const char* n = a_obj ? a_obj->GetName() : nullptr;
        return (n && *n) ? n : "?";
    }

    void DrawSlider()
    {
        if (!g_slider.active) return;

        // UNIFIED popup chrome (user feedback: the plain bordered box read as
        // a different design language): same managed-window construction as
        // the settings / pouch / loadout-confirm windows — WinManager pos +
        // tracked TitleBar (torn frame, crimson strip, drag) + centred body.
        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 3.0f * btnW + 16.0f * S;   // GI46: MAX | OK | Cancel
        const float sliderW = 220.0f * S;

        const std::string title = PopupTitleOf(g_slider.obj);
        const char* const name = title.c_str();
        const char* lbl = Lang::T(Lang::Str::TakeLabel);
        switch (g_slider.dir) {
        case XferDir::kStore:     lbl = Lang::T(Lang::Str::StoreLabel); break;
        case XferDir::kPickup:    lbl = Lang::T(Lang::Str::SplitLabel); break;
        case XferDir::kBuy:       lbl = Lang::T(Lang::Str::BuyLabel); break;
        case XferDir::kSell:      lbl = Lang::T(Lang::Str::SellLabel); break;
        case XferDir::kPickStore: lbl = Lang::T(Lang::Str::StoreLabel); break;
        default: break;   // kTake / kPickTake share the Take label
        }
        const bool barter = g_slider.dir == XferDir::kBuy ||
                            g_slider.dir == XferDir::kSell;

        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        // tracked (letter-spaced) title needs headroom over the raw text width
        const float contentW = (std::max)({ btnRow, sliderW,
            ImGui::CalcTextSize(name).x * 1.35f });
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + sp + ImGui::GetFrameHeight() +
                (barter ? lineH + sp : 0.0f) + 6.0f * S +
                ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        if (wm->BeginConfirmPopup("slider", "##gi_slider", name, size)) {
            g_slider.active = false;   // outside click closes
            Grid::ClearDropHint();     // B2
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(lbl).x);
        ImGui::TextColored(sk.inkDim, "%s", lbl);
        center(sliderW);
        const int qtyBefore = g_slider.value;
        // The floor is 0, not 1: dragging all the way down is how a player says
        // "none of it", and confirming there is the same as cancelling.
        const bool qtyMoved =
            Theme::ChromeSliderInt("##qty", &g_slider.value, 0, g_slider.max, sliderW);
        g_slider.value = (std::max)(0, (std::min)(g_slider.value, g_slider.max));
        if (qtyMoved && g_slider.value != qtyBefore) {
            // direction-aware scrub tick, rate-limited so a fast drag
            // doesn't machine-gun the blip
            static double s_lastTick = 0.0;
            const double now = ImGui::GetTime();
            if (now - s_lastTick > 0.06) {
                s_lastTick = now;
                if (g_slider.value > qtyBefore) Sfx::SelectOn();
                else                            Sfx::SelectOff();
            }
        }
        // GI51: LEFT/RIGHT nudge the quantity by 1 (hold repeats). In menu
        // mode the engine translates A/D into arrow GFx codes, so WASD works
        // through the same binding. GI52 (review): every popup key stands
        // down while a text field owns the keyboard -- typed spaces/enters
        // must never confirm a popup that happens to be open.
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && g_slider.value > 0) {
            --g_slider.value;
            Sfx::SelectOff();
        }
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) &&
            g_slider.value < g_slider.max) {
            ++g_slider.value;
            Sfx::SelectOn();
        }

        // live total price for barter (updates as the quantity moves) — turns
        // crimson when the payer can't afford it (buy: player, sell: merchant)
        if (barter) {
            const bool buy = g_slider.dir == XferDir::kBuy;
            const int total = buy
                ? BuyPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value)
                : SellPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value);
            const bool broke = buy ? Grid::GoldAmount() < total
                                   : (total > 0 && MerchantGold() < total);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d G", total);
            center(ImGui::CalcTextSize(buf).x);
            // ★Val(), not sk.hi. A figure is a figure on every skin, and on
            // SIMPLE sk.hi is a dark blue that reads as structure — a border,
            // not a number (see Theme::ValVec).
            ImGui::TextColored(broke ? ImVec4(0.8f, 0.32f, 0.28f, 1.0f)
                                     : Theme::ValVec(), "%s", buf);
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        // GI46: one-click whole-stack transfer. For barter the cap also folds
        // in the payer's purse -- buying 200 arrows with 50 arrows' gold moves
        // the 50, instead of buzzing "not enough gold" and moving nothing.
        bool maxPress = Sfx::Button(Lang::T(Lang::Str::MaxLabel), ImVec2(btnW, 0));
        if (maxPress) {
            int cap = g_slider.max;
            if (g_slider.dir == XferDir::kBuy) {
                cap = MaxAffordable(true, g_slider.obj, g_slider.unitValue, cap,
                                    Grid::GoldAmount());
            } else if (g_slider.dir == XferDir::kSell) {
                cap = MaxAffordable(false, g_slider.obj, g_slider.unitValue, cap,
                                    MerchantGold());
            }
            if (cap < 1) {
                Sfx::FailNote(Lang::T(g_slider.dir == XferDir::kBuy
                    ? Lang::Str::NotEnoughGold : Lang::Str::MerchantNoGold));
                maxPress = false;
            } else {
                g_slider.value = cap;   // the confirm path below reads this
            }
        }
        ImGui::SameLine(0.0f, 8.0f * S);
        const bool ok = maxPress ||
                        Sfx::Button(Lang::T(Lang::Str::Confirm), ImVec2(btnW, 0)) ||
                        (!typing && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                     ImGui::IsKeyPressed(ImGuiKey_Space, false)));
        ImGui::SameLine(0.0f, 8.0f * S);
        const bool cancel = Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true) ||
                            ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        // Confirming at zero means "none of it" -- take the cancel path rather
        // than firing a request for 0 units.
        if (ok && g_slider.value > 0) {
            switch (g_slider.dir) {
            case XferDir::kTake:   RequestTake(g_slider.obj, g_slider.value, g_slider.uid, g_slider.sig, g_slider.worn); break;
            case XferDir::kPickTake:
                RequestPickTake(g_slider.obj, g_slider.value, g_slider.uid, g_slider.sig,
                                g_slider.worn);
                break;
            case XferDir::kPickStore:
                // pending-remove is noted on the WIN inside the Tick (a lost
                // roll must leave the tile untouched)
                RequestPickStore(g_slider.obj, g_slider.value, g_slider.uid, g_slider.sig,
                                 g_slider.srcKey, g_slider.fav, g_slider.xlIdx);
                break;
            case XferDir::kStore:
                // ★(1.3.3) a follower's pack is 10 x 8 -- asked here too, so
                // the slider cannot walk around the check the click made
                if (!PartnerHasRoomFor(g_slider.obj, g_slider.value)) {
                    Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                    g_storeHint = {};
                    break;
                }
                RequestStore(g_slider.obj, g_slider.value, g_slider.uid, g_slider.sig,
                             g_slider.fav, g_slider.xlIdx, g_slider.srcKey);
                // outgoing units leave their tile IN PLACE (engine removal is
                // still queued on the Tick — without this the interim rebuild
                // re-seats them at the front)
                Grid::NotePendingRemove(g_slider.obj, g_slider.srcKey, g_slider.value,
                                        g_slider.xlIdx);
                // F7: a stack dropped on an empty container cell carried its
                // drop spot through the slider — apply it on confirm
                if (g_storeHint.obj == g_slider.obj && g_storeHint.col >= 0) {
                    NoteStoreSpot(g_storeHint.obj, g_storeHint.col, g_storeHint.row,
                                  g_storeHint.sig, g_storeHint.rot);
                }
                g_storeHint = {};   // GI18: the pending claim stays — the
                                    // item has not reached the container yet
                break;
            case XferDir::kPickup: Grid::PickupPartial(g_slider.obj, g_slider.value, g_slider.srcKey, g_slider.max); break;
            case XferDir::kBuy: {
                const int total = BuyPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value);
                if (Grid::GoldAmount() < total) {
                    Sfx::FailNote(Lang::T(Lang::Str::NotEnoughGold));
                } else if (Grid::MaxAcceptUnits(g_slider.obj, g_slider.value) < g_slider.value) {
                    // space may have changed since the slider opened
                    Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                } else {
                    RequestBuy(g_slider.obj, g_slider.value, total,
                        g_slider.unitValue * g_slider.value,
                        g_slider.uid, g_slider.sig);
                }
                break;
            }
            case XferDir::kSell: {
                const int total = SellPriceTotal(g_slider.obj, g_slider.unitValue, g_slider.value);
                if (total > 0 && MerchantGold() < total) {
                    Sfx::FailNote(Lang::T(Lang::Str::MerchantNoGold));
                } else {
                    RequestSell(g_slider.obj, g_slider.value, total,
                        g_slider.unitValue * g_slider.value,
                        g_slider.uid, g_slider.sig, g_slider.fav, g_slider.xlIdx,
                        g_slider.srcKey);
                    Grid::NotePendingRemove(g_slider.obj, g_slider.srcKey, g_slider.value,
                                            g_slider.xlIdx);
                }
                break;
            }
            }
            g_slider.active = false;
        } else if (cancel || ok) {   // `ok` here can only be a zero-quantity confirm
            g_slider.active = false;
            Grid::ClearDropHint();   // B2: the pending drop-cell hint dies with the slider
            g_storeHint = {};   // F7: the store-spot hint dies with it too
        }
        ImGui::End();
    }

    void AskSellConfirm(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal,
                        const std::string& a_srcKey, std::uint16_t a_uid, std::uint16_t a_sig,
                        bool a_fav, int a_xlIdx)
    {
        if (a_obj && a_count > 0) {
            g_confirm = { true, a_obj, a_count, a_price, a_baseTotal, a_srcKey, a_uid, a_sig,
                          a_fav, a_xlIdx };
            Sfx::SelectOn();
        }
    }

    bool ConfirmActive() { return g_confirm.active; }

    void DrawConfirm()
    {
        if (!g_confirm.active) return;

        // UNIFIED popup chrome — same managed construction as the slider /
        // pouch / loadout-confirm windows (title = item name, centred body).
        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;

        const std::string title = PopupTitleOf(g_confirm.obj);
        const char* const name = title.c_str();
        const char* question = Lang::T(Lang::Str::SellFavoriteConfirm);
        char priceLine[32];
        std::snprintf(priceLine, sizeof(priceLine), "%d G", g_confirm.price);

        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float contentW = (std::max)({ btnRow,
            ImGui::CalcTextSize(question).x,
            ImGui::CalcTextSize(name).x * 1.35f });
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + 2.0f * lineH + sp + 6.0f * S +
                ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        if (wm->BeginConfirmPopup("sellconfirm", "##gi_sellconfirm", name, size)) {
            g_confirm.active = false;   // outside click cancels
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(question).x);
        ImGui::TextColored(sk.ink, "%s", question);
        center(ImGui::CalcTextSize(priceLine).x);
        ImGui::TextColored(Theme::ValVec(), "%s", priceLine);   // a price is a figure

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
        if (ok) {
            RequestSell(g_confirm.obj, g_confirm.count, g_confirm.price, g_confirm.base,
                        g_confirm.uid, g_confirm.sig, g_confirm.fav, g_confirm.xlIdx,
                        g_confirm.srcKey);
            Grid::NotePendingRemove(g_confirm.obj, g_confirm.srcKey, g_confirm.count,
                                    g_confirm.xlIdx);
            g_confirm.active = false;
        } else if (cancel) {
            g_confirm.active = false;
        }
        ImGui::End();
    }

namespace
{
    int ShelfGoldOf(const std::string& a_spotKey)
    {
        if (a_spotKey.empty()) return 0;
        auto* p = Partner();
        if (!p) return 0;
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) return 0;
        const auto si = ci->second.spots.find(a_spotKey);
        return si == ci->second.spots.end() ? 0 : si->second.gold;
    }
}

    int HeldShelfGold()
    {
        // only a partner-side carry has a reserved shelf slot to ask
        if (g_actingSpot.empty() || !Grid::HeldPartnerObject()) return -1;
        return ShelfGoldOf(g_actingSpot);
    }

    void NoteBagBundle(RE::FormID a_bagForm, std::vector<BundleItem> a_items)
    {
        auto* p = Partner();
        if (!p || a_items.empty()) return;
        g_pendingBundles.push_back({ p->GetFormID(), a_bagForm, std::move(a_items) });
        SKSE::log::info("[LOOT] bag bundle noted: {} kind(s) follow bag {:08X}",
            g_pendingBundles.back().items.size(), a_bagForm);
    }

    std::vector<BundleItem> TakeIncomingBundle(RE::FormID a_bagForm)
    {
        for (auto it = g_incomingBundles.begin(); it != g_incomingBundles.end(); ++it) {
            if (it->bagForm != a_bagForm) continue;
            auto v = std::move(it->items);
            g_incomingBundles.erase(it);
            return v;
        }
        return {};
    }

    bool IsBundleCarry() { return g_bundleCarry.active; }

    bool ConsumeBundleCarry(RE::TESBoundObject* a_obj, int a_count)
    {
        if (!g_bundleCarry.active || !a_obj ||
            a_obj->GetFormID() != g_bundleCarry.form) {
            return false;
        }
        g_bundleCarry.active = false;
        const auto ci = g_contLayouts.find(g_bundleCarry.cont);
        if (ci == g_contLayouts.end()) return true;
        const auto si = ci->second.spots.find(g_bundleCarry.spot);
        if (si == ci->second.spots.end()) return true;
        auto& bundle = si->second.bundle;
        // the anchor names the exact entry; fall back to (form, sig)
        auto it = std::find_if(bundle.begin(), bundle.end(), [&](const BundleItem& b) {
            return b.form == g_bundleCarry.form && b.col == g_bundleCarry.col &&
                   b.row == g_bundleCarry.row;
        });
        if (it == bundle.end()) {
            it = std::find_if(bundle.begin(), bundle.end(), [&](const BundleItem& b) {
                return b.form == g_bundleCarry.form && b.sig == g_bundleCarry.sig;
            });
        }
        if (it != bundle.end()) {
            it->count -= a_count;
            if (it->count <= 0) bundle.erase(it);
        }
        return true;
    }

    // ★(1.3.1/1.3.2c) an OPEN shelf bag: a REAL bag window over the bundle
    // record -- the player-bag grammar on the container side. Items draw at
    // their anchors on the bag's own grid, lift onto the cursor (drop on
    // the player grid = take home, drop on the shelf = out of the bag, drop
    // back inside = rearrange, drop in ANOTHER open shelf bag = move over),
    // and a carried player item drops IN. Typed bags carry their COLLECT
    // button. The engine items are already in the container throughout;
    // only the bundle record moves until a transfer is actually queued.
    namespace
    {
        // one bag window. a_ord staggers the default position; returns
        // false when the window should close (the bag left the shelf).
        bool DrawOneShelfBag(RE::TESObjectREFR* p, ContLayout& a_cl,
                             const ShelfBagWin& a_w, int a_ord)
        {
        const auto si = a_cl.spots.find(a_w.spot);
        if (si == a_cl.spots.end()) return false;   // the bag left the shelf
        auto& bundle = si->second.bundle;
        auto* bagObj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_w.form);
        if (!bagObj) return false;
        const auto bagDef = Grid::ResolveDef(bagObj);
        const int cols = (std::max)(1, bagDef.bw);
        int rows = (std::max)(1, bagDef.bh);

        // ---- seat the bundle: saved anchors first, first-fit for the rest.
        // Fresh fits are WRITTEN BACK so the layout survives the session
        // (cosave v5/v6) exactly like a player bag's. A carry lifted from
        // THIS window is excluded entirely -- its cells free up while it
        // rides the cursor, and a cancel simply re-seats it (colliding
        // anchors refit, nothing is lost).
        const bool carryOut = g_bundleCarry.active &&
                              g_bundleCarry.spot == a_w.spot &&
                              g_bundleCarry.cont == p->GetFormID();
        auto isCarried = [&](const BundleItem& a_b) {
            return carryOut && a_b.form == g_bundleCarry.form &&
                   a_b.col == g_bundleCarry.col && a_b.row == g_bundleCarry.row;
        };
        auto dimsOf = [&](const BundleItem& a_b, RE::TESBoundObject* a_obj,
                          int& a_w, int& a_h) {
            const auto d = Grid::ResolveDef(a_obj);
            a_w = (std::max)(1, d.w);
            a_h = (std::max)(1, d.h);
            if (a_b.rot & 1) std::swap(a_w, a_h);
        };
        constexpr int kGrowRows = 16;
        const int maxRows = rows + kGrowRows;
        std::vector<char> occ(static_cast<std::size_t>(cols) * maxRows, 0);
        auto fits = [&](int a_c, int a_r, int a_w, int a_h) {
            if (a_c < 0 || a_r < 0 || a_c + a_w > cols || a_r + a_h > maxRows) return false;
            for (int y = 0; y < a_h; ++y)
                for (int x = 0; x < a_w; ++x)
                    if (occ[static_cast<std::size_t>(a_r + y) * cols + a_c + x]) return false;
            return true;
        };
        auto mark = [&](int a_c, int a_r, int a_w, int a_h, char a_v) {
            for (int y = 0; y < a_h; ++y)
                for (int x = 0; x < a_w; ++x)
                    occ[static_cast<std::size_t>(a_r + y) * cols + a_c + x] = a_v;
        };
        struct Seat
        {
            int                 idx;
            int                 col, row, w, h, rot;
            RE::TESBoundObject* obj;
        };
        std::vector<Seat> seats;
        for (int i = 0; i < static_cast<int>(bundle.size()); ++i) {
            if (isCarried(bundle[i])) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(bundle[i].form);
            if (!obj) continue;
            int w = 1, h = 1;
            dimsOf(bundle[i], obj, w, h);
            if (bundle[i].col >= 0 && fits(bundle[i].col, bundle[i].row, w, h)) {
                mark(bundle[i].col, bundle[i].row, w, h, 1);
                seats.push_back({ i, bundle[i].col, bundle[i].row, w, h,
                                  bundle[i].rot, obj });
            } else {
                bundle[i].col = bundle[i].row = -1;   // stale anchor: refit below
            }
        }
        for (int i = 0; i < static_cast<int>(bundle.size()); ++i) {
            if (bundle[i].col >= 0 || isCarried(bundle[i])) continue;
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(bundle[i].form);
            if (!obj) continue;
            int w = 1, h = 1;
            dimsOf(bundle[i], obj, w, h);
            for (int r = 0; r < maxRows && bundle[i].col < 0; ++r) {
                for (int c = 0; c < cols; ++c) {
                    if (!fits(c, r, w, h)) continue;
                    mark(c, r, w, h, 1);
                    bundle[i].col = c;
                    bundle[i].row = r;
                    seats.push_back({ i, c, r, w, h, bundle[i].rot, obj });
                    break;
                }
            }
        }
        for (const auto& s : seats) rows = (std::max)(rows, s.row + s.h);

        // ---- the window: the player bag-window chrome, one for one --------
        auto* wm = WinManager::GetSingleton();
        const float S = Theme::Scale();
        const float cell = Grid::CellPx();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float topPad = Theme::TitleTopPad();
        const ImVec2 size(cols * cell + 2.0f * Theme::PadX() * S +
                              2.0f * Theme::FrameInsetX(),
                          rows * cell + 54.0f * S + 2.0f * Theme::FrameInsetY());
        const std::string wid = "sb|" + a_w.spot;
        wm->ApplyNext(wid,
            ImVec2(disp.x * 0.52f + a_ord * 44.0f * S,
                   (disp.y - size.y) * 0.5f + a_ord * 36.0f * S),
            size, WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin(("##sb_" + a_w.spot).c_str(), nullptr, kManagedWinFlags);
        // ★★(1.3.2) NO NoteOverlayRect HERE. This is a BAG window, not a
        // popup -- and registering it made the window block its own cells:
        // every lift is gated on `IsItemHovered() && !MouseInOverlay()`, and
        // the mouse is always inside the rect the window itself just
        // registered. Dropping IN still worked (that path asks
        // IsWindowHovered, which knows nothing about overlays), so the
        // symptom was exactly "things go into the bag but cannot come out".
        // The player's own bag windows register nothing for the same reason;
        // z-order already keeps clicks from falling through to the boards.
        // ★★(1.3.2) the COLLECT button's width has to be RESERVED on the
        // title bar before it is drawn: TitleBar lays a full-width drag
        // strip across the row, and an unreserved button sits UNDER it --
        // present, correct, and unclickable (the player's own bag windows
        // pass the same reservation for the same reason).
        const char* colLbl = Lang::T(Lang::Str::BagCollect);
        const bool  hasCollect = !bagDef.accept.empty();
        const float colW = hasCollect
                               ? ImGui::CalcTextSize(colLbl).x + 16.0f * S
                               : 0.0f;
        wm->TitleBar(wid,
            bagObj->GetName() && *bagObj->GetName() ? bagObj->GetName() : "?",
            colW);

        // ★(1.3.2b) typed bags carry their COLLECT on the shelf too: sweep
        // this container's matching LOOSE items into the bag. The swept
        // cells vanish (the bundle hides them) and their spots die in the
        // ordinary dead-pool prune next frame.
        if (hasCollect && !Grid::IsHolding()) {
            const ImVec2 keep = ImGui::GetCursorScreenPos();
            const float lineH = ImGui::GetTextLineHeight();
            const float btnH = lineH + 6.0f * S;
            const float textTop = WinManager::TitleTextY(wid, lineH);
            ImGui::SetCursorScreenPos(ImVec2(
                ImGui::GetWindowPos().x + size.x - colW - Theme::TopControlRightPad(),
                textTop - (btnH - lineH) * 0.5f));
            const auto& sk = Theme::S();
            if (Sfx::Button(("##sbcollect_" + a_w.spot).c_str(), ImVec2(colW, btnH))) {
                // already-bundled counts across THIS container hide cells;
                // collect only what is still loose
                std::map<RE::FormID, int> taken;
                for (const auto& [k2, s2] : a_cl.spots) {
                    for (const auto& b2 : s2.bundle) taken[b2.form] += b2.count;
                }
                int swept = 0;
                if (auto* src = SourceRef()) {
                    for (auto& [obj2, data2] : src->GetInventory()) {
                        const int total = data2.first;
                        if (!obj2 || total <= 0 || obj2->IsGold()) continue;
                        const RE::FormID fid2 = obj2->GetFormID();
                        if (GoldCoins::IsCoinForm(fid2)) continue;   // incl. pouch
                        if (Grid::ResolveDef(obj2).bag != 0) continue;
                        const char* nm2 = obj2->GetName();
                        if (!nm2 || !*nm2 || !obj2->GetPlayable()) continue;
                        if (BagFilter::FilterOf(obj2) != bagDef.accept) continue;
                        const int loose = total - taken[fid2];
                        if (loose <= 0) continue;
                        bundle.push_back({ fid2, loose, 0, -1, -1, 0 });
                        taken[fid2] += loose;
                        swept += loose;
                    }
                }
                if (swept > 0) Sfx::BagOpen();
            }
            const bool hovC = ImGui::IsItemHovered();
            const ImVec2 bp = ImGui::GetItemRectMin();
            const ImVec2 bs = ImGui::GetItemRectSize();
            const ImVec2 ts = ImGui::CalcTextSize(colLbl);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(bp.x + (bs.x - ts.x) * 0.5f,
                       bp.y + (bs.y - ts.y) * 0.5f),
                hovC ? Theme::Val() : ImGui::GetColorU32(sk.inkDim), colLbl);
            ImGui::SetCursorScreenPos(keep);
        }

        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 winPos = ImGui::GetWindowPos();
        const ImVec2 base(winPos.x + Theme::PadX() * S + Theme::FrameInsetX(),
                          winPos.y + 40.0f * S + Theme::FrameInsetY() * 0.5f);

        // ★★RESERVE THE GRID AREA FIRST. SetCursorScreenPos alone does not
        // grow the window's content bounds, and ImGui 1.92 raises its error
        // overlay for every item button placed beyond them -- that overlay
        // then swallowed hover AND clicks, which read back as "the window
        // closes the moment an item is clicked" and "nothing can be dropped
        // in". One Dummy over the whole grid makes every later position a
        // known-inside one.
        ImGui::SetCursorScreenPos(base);
        ImGui::Dummy(ImVec2(cols * cell, rows * cell));

        // chrome, ground, then the lattice -- the partner board's order
        Grid::DrawCellLattice(dl, base, cols, rows);
        const ImU32 shade = Theme::OccupiedGround();
        for (const auto& s : seats) {
            dl->AddRectFilled(ImVec2(base.x + s.col * cell + 1.0f,
                                     base.y + s.row * cell + 1.0f),
                              ImVec2(base.x + (s.col + s.w) * cell - 1.0f,
                                     base.y + (s.row + s.h) * cell - 1.0f),
                shade);
        }
        Grid::DrawInkLattice(dl, base, cols, rows);

        auto* cache = IconCache::GetSingleton();
        for (const auto& s : seats) {
            const auto& b = bundle[s.idx];
            const ImVec2 p0(base.x + s.col * cell, base.y + s.row * cell);
            const float bw = s.w * cell, bh = s.h * cell;
            const IconCache::Icon* icon = cache->Get(s.obj);
            if (!icon) {
                cache->QueueCapture(s.obj);
                icon = Fallback::Get(s.obj);
            }
            if (icon && icon->srv) {
                // contain-fit inside the UPRIGHT box (the sprite is not
                // rotated, only its draw quad -- GI62), the one icon path
                // (gain/blend) + the same shadow as every board
                const float iw = static_cast<float>(icon->w);
                const float ih = static_cast<float>(icon->h);
                const float upW = ((s.rot & 1) ? s.h : s.w) * cell;
                const float upH = ((s.rot & 1) ? s.w : s.h) * cell;
                const float k = (std::min)(upW / (std::max)(1.0f, iw),
                                           upH / (std::max)(1.0f, ih)) * 0.92f;
                const float dw = iw * k, dh = ih * k;
                const float deg = s.rot * 90.0f;
                const ImVec2 ctr(p0.x + bw * 0.5f, p0.y + bh * 0.5f);
                // ★(1.3.2) the rarity halo, UNDER the sprite -- the same pass
                // and the same call both boards make. Its absence was the
                // whole "no markers inside a stored bag" report.
                Grid::DrawGlow(dl, s.obj, b.glow,
                    ImVec2(ctr.x - dw * 0.5f, ctr.y - dh * 0.5f),
                    ImVec2(ctr.x + dw * 0.5f, ctr.y + dh * 0.5f),
                    p0, ImVec2(p0.x + bw, p0.y + bh), s.rot);
                Grid::DrawItemShadow(dl, icon->srv, ctr, dw, dh, deg);
                UIRoot::DrawItemIconRot(dl, icon->srv, ctr, ImVec2(dw, dh), deg);
            }
            // ★(1.3.2) the marker tray: poison, and "stolen" when the whole
            // container is someone else's. No star -- a stored unit is not
            // the player's favourite any more (the store strips it).
            Grid::DrawMarkerTray(dl, p0, ImVec2(p0.x + bw, p0.y + bh),
                                 false, b.stolen || g_mode == Mode::kSteal,
                                 (b.glow & 0x4) != 0, false);
            {   // GI8: extension overlay (socket wells), same as every board
                Badges::TileShape shape;
                shape.w = s.w;
                shape.h = s.h;
                const bool hovB = UIRoot::CursorOwnsWindow() &&
                                  ImGui::IsMouseHoveringRect(
                                      p0, ImVec2(p0.x + bw, p0.y + bh), false);
                Badges::Draw(dl, p0, bw, bh, shape, p->GetFormID(),
                             b.form, 0, hovB);
            }
            if (b.count > 1) {
                char cnt[16];
                std::snprintf(cnt, sizeof(cnt), "%d", b.count);
                Grid::DrawCountBadge(dl, p0, cnt);
            }
            if (!Grid::IsHolding()) {
                char idbuf[24];
                std::snprintf(idbuf, sizeof(idbuf), "##sbc%d", s.idx);
                ImGui::SetCursorScreenPos(p0);
                ImGui::InvisibleButton(idbuf, ImVec2(bw, bh));
                if (ImGui::IsItemHovered() && !UIRoot::MouseInOverlay()) {
                    dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                        Theme::Acc(0.10f));
                    Grid::DrawItemTooltip(s.obj, b.count, -1, -1, false,
                                          SourceRef(), Grid::ExtraScope::kAny,
                                          0, -1, 0, 0,
                                          Grid::TileContext{ {}, false, false, true, false });
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        // lift it: the bundle keeps the entry until the carry
                        // is CONSUMED (take home / shelf drop); a cancel puts
                        // it visually straight back
                        g_bundleCarry = { true, p->GetFormID(), a_w.spot,
                                          b.form, b.sig, s.col, s.row };
                        g_carryGlow = b.glow;   // (1.3.2)
                        g_carryStolen = b.stolen;
                        Grid::BeginPartnerCarry(s.obj, b.count, 0,
                                                -1.0f, -1.0f, 0, -1, 0, s.rot);
                    }
                }
            }
        }

        // ★(1.3.2) drop ghost, the same one both boards draw: green = the
        // footprint seats here, red = it does not (a single blocker still
        // swaps, exactly as on the partner board, and reads red there too).
        // Its anchor is the DROP's anchor verbatim, so preview and result
        // cannot disagree.
        if (Grid::IsHolding() &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            int hw = 1, hh = 1;
            float gox = 0.0f, goy = 0.0f;
            if (Grid::HeldFootprint(hw, hh, gox, goy)) {
                const ImVec2 gm = ImGui::GetIO().MousePos;
                const int gc = static_cast<int>(std::floor(
                    (gm.x - base.x) / cell - (hw - 1) * 0.5f));
                const int gr = static_cast<int>(std::floor(
                    (gm.y - base.y) / cell - (hh - 1) * 0.5f));
                if (gc >= 0 && gr >= 0 && gc + hw <= cols && gr + hh <= rows) {
                    const ImU32 ghost = fits(gc, gr, hw, hh)
                                            ? IM_COL32(90, 170, 90, 90)
                                            : IM_COL32(190, 60, 60, 110);
                    const ImVec2 g0(base.x + gc * cell, base.y + gr * cell);
                    dl->AddRectFilled(g0,
                        ImVec2(g0.x + hw * cell, g0.y + hh * cell), ghost);
                }
            }
        }

        // ---- drops INTO the window ---------------------------------------
        // The full player-bag grammar: a free rect seats it, ONE blocker is
        // the C4 swap (incoming takes the occupant's anchor, the occupant
        // rides the cursor), more keeps carrying. Sources: a rearrange from
        // this bag, a PLAYER item (engine store rides along), or a plain
        // SHELF cell (already in the container -- its slot retires and the
        // entry simply hides it).
        if (Grid::IsHolding() &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            RE::TESBoundObject* hobj = nullptr;
            int hcount = 0, hrot = 0;
            std::uint16_t hsig = 0;
            bool fromPartner = false;
            const std::uint8_t hglow = g_carryGlow;   // (1.3.2) markers in transit
            if (Grid::PeekHeldForShelf(hobj, hcount, hsig, hrot, fromPartner)) {
                int hw = 1, hh = 1;
                float hox = 0.0f, hoy = 0.0f;
                Grid::HeldFootprint(hw, hh, hox, hoy);   // already-rotated dims
                const ImVec2 m = ImGui::GetIO().MousePos;
                // the carry centres on the cursor: centre the footprint too
                const int c = static_cast<int>(std::floor(
                    (m.x - base.x) / cell - (hw - 1) * 0.5f));
                const int r = static_cast<int>(std::floor(
                    (m.y - base.y) / cell - (hh - 1) * 0.5f));

                const bool bundleRe = g_bundleCarry.active;
                const bool sameBag = bundleRe &&
                                     g_bundleCarry.spot == a_w.spot &&
                                     g_bundleCarry.cont == p->GetFormID();
                const RE::FormID hfid = hobj->GetFormID();
                // eligibility: a rearrange is already inside; a shelf cell may
                // not be a bag (no nesting) or a coin/pouch (its gold lives on
                // the SLOT this move would retire); a player item answers to
                // HeldShelfStorable. Typed bags keep their filter -- a move
                // from ANOTHER bag answers to it too.
                const bool intakeOk =
                    bundleRe ||
                    (fromPartner ? !(Grid::ResolveDef(hobj).bag != 0 ||
                                     GoldCoins::IsCoinForm(hfid))
                                 : Grid::HeldShelfStorable() != nullptr);
                const bool filterOk = sameBag || bagDef.accept.empty() ||
                                      BagFilter::FilterOf(hobj) == bagDef.accept;
                if (intakeOk && filterOk) {
                    // seats blocking the drop rect
                    std::vector<int> blocks;
                    for (int sn = 0; sn < static_cast<int>(seats.size()); ++sn) {
                        const auto& s = seats[sn];
                        const bool hit = !(c + hw <= s.col || s.col + s.w <= c ||
                                           r + hh <= s.row || s.row + s.h <= r);
                        if (hit) blocks.push_back(sn);
                    }
                    int  dropC = c, dropR = r;
                    int  liftIdx = -1;
                    bool ok = false;
                    if (fits(c, r, hw, hh)) {
                        ok = true;
                    } else if (blocks.size() == 1) {
                        const auto& bs = seats[blocks[0]];
                        mark(bs.col, bs.row, bs.w, bs.h, 0);   // judge with it freed
                        if (fits(bs.col, bs.row, hw, hh)) {
                            dropC = bs.col;
                            dropR = bs.row;
                            liftIdx = blocks[0];
                            ok = true;
                        }
                    }
                    if (ok) {
                        // the displaced occupant, remembered BEFORE the bundle
                        // mutates under it
                        BundleItem lifted{};
                        RE::TESBoundObject* liftedObj = nullptr;
                        if (liftIdx >= 0) {
                            lifted = bundle[seats[liftIdx].idx];
                            liftedObj = seats[liftIdx].obj;
                        }
                        bool consumed = false;
                        if (bundleRe) {
                            // (1.3.2c) pull the entry out of the bundle it was
                            // lifted from -- which may be ANOTHER open bag --
                            // and seat it here
                            if (const auto sci = g_contLayouts.find(g_bundleCarry.cont);
                                sci != g_contLayouts.end()) {
                                if (const auto ssi =
                                        sci->second.spots.find(g_bundleCarry.spot);
                                    ssi != sci->second.spots.end()) {
                                    auto& srcB = ssi->second.bundle;
                                    const auto bit = std::find_if(srcB.begin(),
                                        srcB.end(), [&](const BundleItem& b) {
                                            return b.form == g_bundleCarry.form &&
                                                   b.col == g_bundleCarry.col &&
                                                   b.row == g_bundleCarry.row;
                                        });
                                    if (bit != srcB.end()) {
                                        BundleItem moved = *bit;
                                        srcB.erase(bit);
                                        moved.col = dropC;
                                        moved.row = dropR;
                                        moved.rot = hrot & 3;
                                        bundle.push_back(moved);
                                        consumed = true;
                                    }
                                }
                            }
                            g_bundleCarry.active = false;
                            Grid::DropHeldForShelf();
                        } else if (fromPartner) {
                            // off the shelf, into the bag: retire its slot --
                            // the engine item never moves, the entry hides it
                            if (!g_actingSpot.empty()) {
                                a_cl.spots.erase(g_actingSpot);
                                g_actingSpot.clear();
                            }
                            // off a shelf cell: the container's own ownership
                            // is all this side knows (kSteal = owned whole)
                            bundle.push_back({ hfid, hcount, hsig,
                                               dropC, dropR, hrot & 3, hglow,
                                               g_carryStolen ||
                                                   g_mode == Mode::kSteal });
                            consumed = true;
                            Grid::DropHeldForShelf();
                        } else {
                            RE::FormID    f = 0;
                            int           cnt = 0;
                            std::uint16_t sg = 0;
                            int           rt = 0;
                            std::uint8_t  gl = 0;
                            bool          st = false;
                            if (Grid::CommitHeldToShelfBag(f, cnt, sg, rt, gl, st)) {
                                bundle.push_back({ f, cnt, sg, dropC, dropR, rt,
                                                   gl, st });
                                consumed = true;
                            }
                        }
                        if (consumed && liftIdx >= 0 && liftedObj) {
                            // C4: the occupant rides the cursor in its place
                            g_bundleCarry = { true, p->GetFormID(), a_w.spot,
                                              lifted.form, lifted.sig,
                                              lifted.col, lifted.row };
                            g_carryGlow = lifted.glow;   // (1.3.2)
                            g_carryStolen = lifted.stolen;
                            Grid::BeginPartnerCarry(liftedObj, lifted.count, 0,
                                                    -1.0f, -1.0f, 0, -1, 0,
                                                    lifted.rot);
                        }
                    }
                }
            }
        }

        // player-bag parity: NO outside-click close -- a bag window closes by
        // its right-click toggle, or ESC (newest first)
        ImGui::End();
        return true;
        }
    }

    void DrawShelfBag()
    {
        if (g_shelfBags.empty()) return;
        if (!IsLootMode(g_mode)) { g_shelfBags.clear(); return; }
        auto* p = Partner();
        if (!p) { g_shelfBags.clear(); return; }
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) { g_shelfBags.clear(); return; }
        for (std::size_t i = 0; i < g_shelfBags.size();) {
            if (DrawOneShelfBag(p, ci->second, g_shelfBags[i], static_cast<int>(i))) {
                ++i;
            } else {
                g_shelfBags.erase(g_shelfBags.begin() +
                                  static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    // ★(1.3.2a) the shelf POUCH's withdraw window -- the player pouch's
    // grammar over the SLOT's amount. Withdrawing credits the ledger and
    // puts a pinned purse on the cursor, exactly like the player's.
    void DrawShelfPouch()
    {
        if (g_shelfPouchSpot.empty()) return;
        if (!IsLootMode(g_mode)) { g_shelfPouchSpot.clear(); return; }
        auto* p = Partner();
        if (!p) { g_shelfPouchSpot.clear(); return; }
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) { g_shelfPouchSpot.clear(); return; }
        const auto si = ci->second.spots.find(g_shelfPouchSpot);
        if (si == ci->second.spots.end()) {   // the pouch left the shelf
            g_shelfPouchSpot.clear();
            return;
        }
        const int stored = si->second.gold;
        if (g_shelfPouchSlider > stored) g_shelfPouchSlider = stored;

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;
        char line[64];
        std::snprintf(line, sizeof(line), "%s: %d / %dG",
            Lang::T(Lang::Str::StoredLabel), stored, GoldCoins::PouchCap());
        const float sliderW = 220.0f * S;
        const float contentW = (std::max)({ btnRow, sliderW,
            ImGui::CalcTextSize(line).x });
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float topPad = Theme::TitleTopPad();
        const ImVec2 size(
            contentW + 30.0f * S + 2.0f * insX,
            barH + 8.0f * S + lineH + ImGui::GetFrameHeight() + 6.0f * S +
                2.0f * sp + ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
        wm->ApplyNext("shelfpouch",
            ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f), size,
            WinManager::Anchor::kTopLeft, topPad);
        ImGui::Begin("##grid_shelfpouch", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        auto* pobj = RE::TESForm::LookupByID<RE::TESBoundObject>(g_shelfPouchForm);
        wm->TitleBar("shelfpouch",
            pobj && pobj->GetName() && *pobj->GetName() ? pobj->GetName() : "?",
            0.0f, true);

        if (!ImGui::IsWindowAppearing() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered()) {
            g_shelfPouchSpot.clear();
            Sfx::SelectOff();
        }

        auto center = [](float a_w) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_w) * 0.5f));
        };

        center(ImGui::CalcTextSize(line).x);
        ImGui::TextColored(sk.ink, "%s", line);
        center(sliderW);
        Theme::ChromeSliderInt("##shelfdraw", &g_shelfPouchSlider, 0, stored,
                               sliderW, "%dG");
        const bool typing = ImGui::GetIO().WantTextInput;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) &&
            g_shelfPouchSlider > 0) {
            --g_shelfPouchSlider;
        }
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) &&
            g_shelfPouchSlider < stored) {
            ++g_shelfPouchSlider;
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
        center(btnRow);
        const bool can = g_shelfPouchSlider > 0;
        const bool keyOk = !typing &&
                           (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_Space, false));
        ImGui::BeginDisabled(!can);
        if (Sfx::Button(Lang::T(Lang::Str::Withdraw), ImVec2(btnW, 0)) ||
            (can && keyOk)) {
            const int v = g_shelfPouchSlider;
            si->second.gold -= v;
            // the shelf's book shrinks; the ledger grows to match, and the
            // amount rides the cursor as a pinned purse (player grammar)
            GoldCoins::CreditLedger(v);
            Grid::CarryWithdrawnGold(v);
            g_shelfPouchSpot.clear();
            g_shelfPouchSlider = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, 8.0f * S);
        if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
            g_shelfPouchSpot.clear();
        }
        ImGui::End();
    }

    int DepositOnHoveredPouch(int a_value)
    {
        if (a_value <= 0 || !IsLootMode(g_mode)) return 0;
        const auto sd = QueryStoreDrop();
        if (!sd.onCell || !sd.occ || !GoldCoins::IsPouch(sd.occ->GetFormID())) {
            return 0;
        }
        auto* p = Partner();
        if (!p) return 0;
        const auto ci = g_contLayouts.find(p->GetFormID());
        if (ci == g_contLayouts.end()) return 0;
        const auto si = ci->second.spots.find(sd.occSpotKey);
        if (si == ci->second.spots.end()) return 0;
        const int room = GoldCoins::PouchCap() - si->second.gold;
        const int moved = (std::min)(a_value, room);
        if (moved <= 0) return 0;   // full: the coin keeps riding
        si->second.gold += moved;
        SKSE::log::info("[LOOT] deposited {} G into shelf pouch '{}' -> {}",
            moved, sd.occSpotKey, si->second.gold);
        return moved;
    }

    RE::TESObjectREFR* Partner()
    {
        if (!g_partner) return nullptr;
        auto ref = g_partner.get();
        return ref ? ref.get() : nullptr;
    }

    RE::TESObjectREFR* SourceRef()
    {
        auto* partner = Partner();
        if (!partner) return nullptr;
        // BARTER: the partner is the merchant ACTOR, but the wares live in the
        // vendor faction's merchantContainer — not the actor's own inventory
        // (that's the merchant's personal belongings). LOOT: the container /
        // corpse ref itself is the source.
        if (g_mode == Mode::kBarter) {
            if (auto* actor = partner->As<RE::Actor>()) {
                if (auto* fac = actor->GetVendorFaction();
                    fac && fac->vendorData.merchantContainer) {
                    return fac->vendorData.merchantContainer;
                }
            }
        }
        return partner;
    }

    namespace
    {
        // UESP barter formula: factor = fBarterMax - (fBarterMax-fBarterMin) *
        // min(speech,100)/100 (defaults 3.3, 2.0). Read the settings live so
        // balance mods (Trade & Barter etc.) apply automatically.
        float BasePriceFactor()
        {
            float bMax = 3.3f, bMin = 2.0f;
            if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
                if (auto* s = gs->GetSetting("fBarterMax")) bMax = s->GetFloat();
                if (auto* s = gs->GetSetting("fBarterMin")) bMin = s->GetFloat();
            }
            float skill = 15.0f;
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                if (auto* avo = p->AsActorValueOwner()) {
                    skill = avo->GetActorValue(RE::ActorValue::kSpeech);
                }
            }
            skill = std::clamp(skill, 0.0f, 100.0f);
            const float f = bMax - (bMax - bMin) * (skill / 100.0f);
            return f > 0.01f ? f : 0.01f;   // guard the sell-price divide
        }

        // Perk price coefficient (Haggling/Allure etc.) via HandleEntryPoint —
        // the engine multiplies `mod` in place by every kMultiplyValue perk on
        // the player for this entry point. 1.0 when no such perk applies.
        // Confirmed arg layout: (ENTRY_POINT, perkOwner, TESBoundObject*, float*).
        float PerkPriceMod(RE::TESBoundObject* a_item,
                           RE::BGSEntryPoint::ENTRY_POINT a_entry)
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p || !a_item) return 1.0f;
            float mod = 1.0f;
            RE::BGSEntryPoint::HandleEntryPoint(a_entry, p, a_item, &mod);
            return mod;
        }
    }

    int BuyPrice(RE::TESBoundObject* a_item, int a_baseValue)
    {
        if (a_baseValue <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModBuyPrices);
        return (std::max)(1,
            static_cast<int>(std::lround(a_baseValue * mod * BasePriceFactor())));
    }

    int SellPrice(RE::TESBoundObject* a_item, int a_baseValue)
    {
        if (a_baseValue <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices);
        // B7: floor 1, not 0 — a harsh barter multiplier used to round a
        // base-valuable item down to a ZERO-gold sale (item left, no pay)
        // (formula verified vanilla-exact 2026-07-28: lround(value*mod/factor),
        // mod/factor live-read -- the old +-1 was a polluted base, not this)
        return (std::max)(1,
            static_cast<int>(std::lround(a_baseValue * mod / BasePriceFactor())));
    }

    // B7: stack totals round ONCE on the total (vanilla behaviour) — the old
    // per-unit lround * count inflated cheap bulk buys (arrows: 100 x each
    // unit rounded up) and deflated bulk sells.
    int BuyPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count)
    {
        if (a_unitValue <= 0 || a_count <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModBuyPrices);
        return (std::max)(a_count, static_cast<int>(std::lround(
            static_cast<double>(a_unitValue) * a_count * mod * BasePriceFactor())));
    }

    int SellPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count)
    {
        if (a_unitValue <= 0 || a_count <= 0) return 0;
        const float mod = PerkPriceMod(a_item, RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices);
        return (std::max)(a_count, static_cast<int>(std::lround(
            static_cast<double>(a_unitValue) * a_count * mod / BasePriceFactor())));
    }

    int MerchantGold()
    {
        if (g_mode != Mode::kBarter) return 0;
        // F3: unlimited mode — every affordability check compares against
        // INT_MAX and passes; the bottom bar branches to the infinity glyph.
        if (g_merchGoldInf) return (std::numeric_limits<int>::max)();
        auto* src = SourceRef();
        if (!src) return 0;
        for (auto& [obj, data] : src->GetInventory()) {
            if (obj && obj->IsGold()) return data.first;
        }
        return 0;
    }

    bool MerchantGoldInfinite() { return g_merchGoldInf; }
    void SetMerchantGoldInfinite(bool a_on) { g_merchGoldInf = a_on; }
    bool MerchantBuysAll() { return g_merchBuysAll; }
    void SetMerchantBuysAll(bool a_on) { g_merchBuysAll = a_on; }

    namespace
    {
        // Phase 6: a vendor VEND list matches an item by exact base form or by
        // any of the item's keywords (vanilla lists hold VendorItem* keywords).
        bool VendorListMatches(RE::BGSListForm* a_list, RE::TESBoundObject* a_obj)
        {
            if (!a_list || !a_obj) return false;
            if (a_list->HasForm(a_obj)) return true;
            if (auto* kwf = a_obj->As<RE::BGSKeywordForm>()) {
                for (auto* kw : kwf->GetKeywords()) {
                    if (kw && a_list->HasForm(kw)) return true;
                }
            }
            return false;
        }
    }

    bool MerchantBuys(RE::TESBoundObject* a_obj, bool a_stolen)
    {
        if (g_mode != Mode::kBarter || !a_obj) return true;
        // the custom coin pouch is always sellable (its sale releases stored
        // gold, §2-C) — exempt from the vendor category/stolen filter.
        if (GoldCoins::IsPouch(a_obj->GetFormID())) return true;
        auto* partner = Partner();
        auto* actor = partner ? partner->As<RE::Actor>() : nullptr;
        auto* fac = actor ? actor->GetVendorFaction() : nullptr;
        if (!fac) return true;   // no vendor data (plain container): unrestricted
        const auto& vv = fac->vendorData.vendorValues;

        // stolen: only a FENCE (buysStolen) accepts stolen goods. Non-stolen
        // items are always acceptable — buysNonStolen is a rare "fence-only"
        // flag that is 0 on ordinary merchants, so requiring it would (wrongly)
        // reject everything. Don't use it.
        if (a_stolen && !vv.buysStolen) return false;

        // F4: category restriction lifted — the stolen rule above stays.
        if (g_merchBuysAll) return true;

        // category: the VEND list is a whitelist (notBuySell=false) or a
        // blacklist (notBuySell=true). No list -> category unrestricted.
        auto* list = fac->vendorData.vendorSellBuyList;
        if (!list) return true;
        const bool inList = VendorListMatches(list, a_obj);
        return vv.notBuySell ? !inList : inList;
    }

    bool IsPartnerHovered() { return g_partnerHovered; }


    // ---- Phase 3: partner-window stages (bodies moved verbatim) ----
    namespace
    {
        // one cell per item form, vanilla-style stack badge (no Mabinogi
        // split on the partner side, per spec)
        struct PartnerCell
        {
            RE::TESBoundObject* obj;
            int count; int value;
            int w; int h; int col; int row;
            std::uint8_t glow;
            int  chance = -1;    // F6b: pickpocket % (whole stack), -1 = n/a
            bool locked = false; // F6b: worn without Perfect Touch
            // GI13: which sub-stack this cell shows. Gear is enumerated per
            // UNIT here, exactly like the player grid, so a tempered dagger and
            // a plain one never share a cell.
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;       // GI14 content signature (uid-less units)
            int           xlIdx = -1;
            int           ord = 0;       // GI13: nth cell of this identity
            bool          perUnit = false;
            bool          worn = false;  // on the body (corpse / pickpocket target)
            // GI42: clicking this cell could make the engine grab an ILLEGAL
            // worn unit (same plain form on the body, no naming handle). Locked
            // up front -- vanilla locks the whole row for the same reason.
            bool          unnameable = false;
            // GI20: the pool slot this cell was assigned this frame. Taking the
            // cell must free THIS slot -- otherwise the pool just loses its
            // trailing position and everything after the taken cell shuffles up,
            // which is exactly "it loots in storage order, not the one I hovered".
            std::string   spotKey;
            // ★What the SHELF is holding for this cell -- a coin pouch's own
            // gold. Carried onto the cell during slot assignment so the draw
            // does not have to reach back into the layout.
            int           gold = 0;
            // GI62: quarter-turns clockwise. Set through SetRot so w/h and the
            // angle can never disagree -- every placement test on this side reads
            // w/h directly, and a stale pair would place the cell wrong.
            int           rot = 0;
            void SetRot(int a_rot)
            {
                if (((rot ^ a_rot) & 1) != 0) std::swap(w, h);
                rot = a_rot & 3;
            }
        };

        // F7: this frame's partner-grid geometry for drop-cell math — set by
        // DrawWindows while a spot-memory (kLoot/kSteal) grid is drawing
        bool                     g_partnerGridLive = false;
        ImVec2                   g_partnerBase{};      // scroll-adjusted origin
        ImVec2                   g_partnerClipMin{};   // grid child rect
        ImVec2                   g_partnerClipMax{};
        int                      g_partnerRows = 0;
        std::vector<PartnerCell> g_lastCells;          // this frame's placement
        // ★(1.3.3) measured height of everything above the board (title bar,
        // section label, window padding, title clearance). -1 = not measured
        // yet. Independent of the row count, so it converges on the second
        // frame and then holds. Reset when the skin or the scale changes.
        float                    g_partnerChromeH = -1.0f;

        // ★Search on the partner board. Its cells are rebuilt EVERY frame, so
        // there is no board version to hang a key-set off the way the player's
        // grid does — but a form's name does not change, so the answer caches
        // per FORM and survives every rebuild. The term itself is the player
        // grid's (Grid::SearchTerm): one box searches both sides of the trade,
        // which is the whole point when you are looking for what to buy.
        std::unordered_map<RE::FormID, bool> g_findCache;
        std::string                          g_findTerm;

        [[nodiscard]] bool FindMisses(RE::TESBoundObject* a_obj)
        {
            const std::string& term = Grid::SearchTerm();
            if (term.empty()) return false;
            if (term != g_findTerm) {   // a new term invalidates every answer
                g_findCache.clear();
                g_findTerm = term;
            }
            if (!a_obj) return true;
            const RE::FormID id = a_obj->GetFormID();
            if (const auto it = g_findCache.find(id); it != g_findCache.end()) {
                return !it->second;
            }
            const bool hit = Grid::SearchMatches(a_obj->GetName());
            g_findCache.emplace(id, hit);
            return !hit;
        }

        // stage 1: collect partner items (worn included, for corpses)
        std::vector<PartnerCell> CollectPartnerCells(RE::TESObjectREFR* source)
        {
            std::vector<PartnerCell> cells;
            // GI20: the acting slot only lives as long as the interaction that
            // named it. A carry that gets cancelled, or a quantity slider that
            // gets closed, must not leave it primed for the NEXT take.
            if (!Grid::IsHolding() && !g_slider.active && !g_confirm.active) {
                g_actingSpot.clear();
                g_carryGlow = 0;                // (1.3.2) markers die with it
                g_bundleCarry.active = false;   // (1.3.1) cancelled carry: the
                                                // entry was never removed, so
                                                // the item is simply back in
                                                // its bag -- nothing to undo
            }
            SweepOut();
            // working copy: units already committed to the player leave this
            // board NOW, not one Tick later (see g_outPool)
            auto outPool = g_outPool;
            auto outForm = g_outForm;
            // ★(1.3.0-D) how many of each form ride inside stored bags HERE:
            // spot bundles of this container, plus bundles still waiting for
            // their bag's spot to be born (so nothing flashes for a frame).
            // Rebuilt per pass -- dropping a bundle un-hides by itself.
            std::map<RE::FormID, int> bundled;
            if (auto* pref = Partner()) {
                if (const auto ci = g_contLayouts.find(pref->GetFormID());
                    ci != g_contLayouts.end()) {
                    for (const auto& [k, s] : ci->second.spots) {
                        for (const auto& b : s.bundle) bundled[b.form] += b.count;
                    }
                }
                for (const auto& pb : g_pendingBundles) {
                    if (pb.cont != pref->GetFormID()) continue;
                    for (const auto& b : pb.items) bundled[b.form] += b.count;
                }
            }
            // Vanilla-style: one cell per item form with a stack-count badge (no
            // Mabinogi split on the partner side, per spec). Read-only in Phase 2.
            for (auto& [obj, data] : source->GetInventory()) {
                const int count = data.first;
                if (!obj || count <= 0) continue;
                // barter: the merchant's gold is BOOKKEEPING (shown in the bottom
                // bar), not a purchasable ware — hide the raw Gold001 cell
                if (g_mode == Mode::kBarter && obj->IsGold()) continue;
                // F7: while a spot-memory grid item rides the cursor its cell
                // FREES UP (the remembered spot in `spots` restores it on a
                // cancel) — required so a swap can seat the incoming item on
                // the occupant's anchor. Barter keeps the old reserve-the-slot
                // behaviour (DrawPartnerCells hides it instead).
                // (the carried-unit skip is per UNIT now -- see the loop below)
                const char* name = obj->GetName();
                if (!name || !*name) continue;   // unnamed (e.g. our coins) skipped
                if (!obj->GetPlayable()) continue;   // vanilla parity: hidden forms
                const int value = data.second ? data.second->GetValue() : 0;
                const auto def = Grid::ResolveDef(obj);   // real footprint (sword 1x3 etc.)
                auto* entry = data.second.get();
                const int dw = (std::max)(1, def.w);
                const int dh = (std::max)(1, def.h);

                // GI13: gear (cap 1) is enumerated per UNIT, matching the player
                // grid. Before this a tempered dagger and a plain one collapsed
                // into ONE cell with a "x2" badge, so there was no way to see --
                // let alone take -- a specific one. Stackables stay aggregate:
                // their units really are interchangeable, and one cell per arrow
                // would be absurd.
                //
                // Worn units are KEPT (a_skipWorn=false): on a corpse or a
                // pickpocket target, what the NPC is wearing is the point.
                std::vector<Grid::UnitRef> units;
                if (Grid::StackCap(obj) <= 1) {
                    Grid::EnumerateUnits(entry, count, units, false);
                }
                if (units.empty()) units.push_back({ 0, 0, -1 });   // aggregate cell

                const bool perUnit = Grid::StackCap(obj) <= 1;
                int plainOrd = 0;             // ordinal among listless units
                int wornOrd = 0;              // GI41: worn units count separately
                int idRun = 0;                // ordinal within one identity's run
                std::uint32_t prevId = 0;
                for (const auto& u : units) {
                    int cellCount = perUnit ? 1 : count;
                    // already promised to the player: drop the unit (gear) or
                    // shave the badge (stack) so the board never shows it twice
                    {
                        auto pi = outPool.find(OutKey(obj->GetFormID(), u.uid, u.sig));
                        auto fi = outForm.find(obj->GetFormID());
                        int  owed = 0;
                        if (pi != outPool.end() && pi->second > 0)      owed = pi->second;
                        else if (fi != outForm.end() && fi->second > 0) owed = fi->second;
                        if (owed > 0) {
                            // both counters track the same units, so they have to
                            // fall together or a later cell gets hidden twice
                            const int take = (std::min)(owed, cellCount);
                            if (pi != outPool.end()) pi->second = (std::max)(0, pi->second - take);
                            if (fi != outForm.end()) fi->second = (std::max)(0, fi->second - take);
                            cellCount -= take;
                            if (cellCount <= 0) continue;   // whole cell is gone
                        }
                    }
                    // ★(1.3.0-D) contents riding inside a stored bag are the
                    // bag's, not the shelf's: hidden while the bundle holds
                    // them ("the chest remembers the bundle, no opening").
                    if (auto bi = bundled.find(obj->GetFormID());
                        bi != bundled.end() && bi->second > 0) {
                        const int hide = (std::min)(bi->second, cellCount);
                        bi->second -= hide;
                        cellCount -= hide;
                        if (cellCount <= 0) continue;
                    }
                    auto* xl = Grid::ExtraForInstance(entry, u.uid, u.xlIdx);
                    // GI43: a per-unit cell is priced from ITS list -- a
                    // tempered unit buys/sells at the vanilla tempered value.
                    const int cellValue = perUnit ? Grid::UnitValueWith(obj, xl)
                                                  : value;
                    PartnerCell pc{ obj, cellCount, cellValue, dw, dh, -1, -1,
                                    Grid::GlowBits(obj, entry, xl) };
                    pc.uid = u.uid;
                    pc.sig = u.sig;
                    pc.xlIdx = u.xlIdx;
                    pc.perUnit = perUnit;
                    // GI41: the walk knew this; do not re-derive it from xlIdx.
                    pc.worn = u.worn;
                    // spot key ordinal: restarts per uid so a uid-keyed cell is
                    // "@A31F", not "@A31F#3" just because plain siblings exist
                    const std::uint32_t id = u.uid != 0 ? (0x10000u | u.uid)
                                           : u.sig != 0 ? u.sig
                                                        : 0;
                    if (id != 0) {
                        idRun = (id == prevId) ? idRun + 1 : 0;
                        prevId = id;
                        pc.ord = idRun;
                    } else {
                        prevId = 0;
                        // GI41: a worn unit counts on its own. Sharing the plain
                        // run meant planting a spare shifted the worn cell's
                        // ordinal, and with it the slot it had been sitting in.
                        pc.ord = pc.worn ? wornOrd++ : plainOrd++;
                    }
                    // F7: while a spot-memory grid item rides the cursor its cell
                    // FREES UP (the remembered spot restores it on a cancel).
                    // GI17: only THAT unit's cell, not every cell of the form.
                    if (SpotMemoryOn() && Grid::IsHeldPartnerUnit(obj, u.uid, u.xlIdx, pc.ord)) {
                        continue;
                    }
                    // A STACKABLE gets one aggregate cell, and its placeholder
                    // unit (uid 0, xlIdx -1) resolves to no list at all -- so an
                    // equipped torch on a mark reported "not worn": no worn tint,
                    // and no pickpocket lock, which let it be lifted without the
                    // Perfect Touch perk that vanilla requires. Ask the entry
                    // instead: if any unit of the stack is on the body, the cell
                    // counts as worn.
                    if (!perUnit && !pc.worn && entry && entry->extraLists) {
                        for (auto* x2 : *entry->extraLists) {
                            if (x2 && (x2->HasType<RE::ExtraWorn>() ||
                                       x2->HasType<RE::ExtraWornLeft>())) {
                                pc.worn = true;
                                break;
                            }
                        }
                    }
                    // GI42: the lock's resolution must MATCH the naming
                    // resolution. Locking only the worn cell while a spare cell
                    // could still pull the worn unit out through the engine was
                    // the bypass.
                    pc.unnameable = Grid::PoolChoice(entry, pc.uid, pc.sig, pc.worn,
                                        WornExportLegal()).kind ==
                                    Grid::PickKind::kUnresolved;
                    // F6b: success % (engine formula) + worn lock
                    if (g_mode == Mode::kPickpocket) {
                        pc.chance = PickpocketChance(obj, cellCount, entry);
                        pc.locked = (pc.worn && !HasPerfectTouch()) || pc.unnameable;
                    } else {
                        pc.locked = pc.unnameable;
                    }
                    // ★★P2/3-5b: GOLD BANDS ON THIS SIDE TOO.
                    //
                    // Every other cell here is one cell per form, count in the
                    // badge -- fine for ingots, wrong for money. A chest with
                    // twelve thousand septims drew a single cell reading 12000,
                    // while the player's own board has never let more than
                    // kCoinCap sit in one square. Storing gold in a chest
                    // (P2/3-5) is what made the two sides look at each other,
                    // and this is the side that had no rule.
                    //
                    // Split into cap-sized cells, each with its own ordinal so
                    // the spot memory can tell them apart and each can be taken
                    // on its own. Deliberately gold-only: an ingot stack in one
                    // cell is not a bug, and banding everything would rearrange
                    // every container in the game for no one's benefit.
                    if (obj->IsGold() && pc.count > GoldCoins::kCoinCap) {
                        int left = pc.count;
                        while (left > 0) {
                            const int take = (std::min)(left, GoldCoins::kCoinCap);
                            PartnerCell band = pc;
                            band.count = take;
                            band.value = take;
                            band.ord = plainOrd++;
                            band.spotKey = std::format("{:08X}|g{}",
                                obj->GetFormID(), band.ord);
                            cells.push_back(std::move(band));
                            left -= take;
                        }
                        continue;
                    }
                    cells.push_back(std::move(pc));
                }
            }
            // ---- self-check: THE PARTNER BOARD ------------------------------
            // The player board has had [FLICK] since P1; this side had nothing,
            // so a container/corpse/merchant blink could only ever be judged by
            // eye. Logged only when the cell set CHANGES, so it stays quiet.
            {
                std::vector<std::string> ks;
                ks.reserve(cells.size());
                for (const auto& c : cells) {
                    ks.push_back(std::format("{}x{}{}", c.count,
                        c.obj ? c.obj->GetName() : "?", c.worn ? "*" : ""));
                }
                std::sort(ks.begin(), ks.end());
                std::string sig = std::to_string(cells.size());
                for (const auto& k : ks) { sig += " | "; sig += k; }
                static std::string s_prev;
                if (sig != s_prev) {
                    SKSE::log::info("[PFLICK] cells {} -> {}  {}",
                        s_prev.empty() ? std::string("-")
                                       : s_prev.substr(0, s_prev.find(' ')),
                        cells.size(), sig);
                    s_prev = sig;
                }
            }
            return cells;
        }

        // stage 2: footprint-aware placement on the 10-wide board; only the
        // SIZE is honoured (stacks stay one footprint + badge). F7: in a
        // spot-memory mode (kLoot/kSteal) remembered spots place FIRST
        // (fixed), the rest first-fits, and the result is recorded back —
        // the very first look auto-packs and immediately becomes the saved
        // arrangement. Returns the furthest occupied/remembered row.
        int PlacePartnerCells(std::vector<PartnerCell>& cells)
        {
            const int cols = Grid::kCols;
            // (1.3.3) a living follower's pack is capped -- see CompanionPartner
            const bool companionBoard = CompanionPartner();
            std::vector<std::vector<bool>> occ;
            auto ensureRow = [&](int r) {
                while (static_cast<int>(occ.size()) <= r) occ.emplace_back(cols, false);
            };
            auto fits = [&](int c, int r, int w, int h) {
                if (c < 0 || r < 0 || c + w > cols) return false;
                for (int y = 0; y < h; ++y) {
                    ensureRow(r + y);
                    for (int x = 0; x < w; ++x)
                        if (occ[r + y][c + x]) return false;
                }
                return true;
            };
            auto mark = [&](int c, int r, int w, int h) {
                for (int y = 0; y < h; ++y) {
                    ensureRow(r + y);
                    for (int x = 0; x < w; ++x) occ[r + y][c + x] = true;
                }
            };

            ContLayout* cl = nullptr;
            if (SpotMemoryOn()) {
                if (auto* p = Partner()) cl = &g_contLayouts[p->GetFormID()];
            }

            // ---- GI20: pools, exactly like the player grid ------------------
            //
            // A POOL is the set of cells that are interchangeable with each
            // other: one uid, or one content signature, or "no extras at all".
            // Inside a pool "which one" has no answer, so the only rules that
            // matter are (a) the pool has as many slots as cells and (b) taking
            // a cell frees THAT cell's slot.
            //
            // Cells are therefore matched to their pool's slots IN POSITION
            // ORDER. Nothing is ever renumbered, so nothing can jump -- which is
            // what the old exact-key lookup could not promise, because the key
            // carried an ordinal that shifted whenever the set changed.
            // The FORM part of a spot key: everything before the first @ ~ or #
            // that FOLLOWS the '|' (a plugin filename may legitimately contain
            // those characters). Handles keys from before 1.3.2, which still
            // carry a signature in the name, and new ones, which do not.
            auto baseOfSpotKey = [](const std::string& k) {
                const auto bar = k.find('|');
                const auto cut = k.find_first_of("@~#!",
                                                 bar == std::string::npos ? 0 : bar + 1);
                return cut == std::string::npos ? k : k.substr(0, cut);
            };
            auto baseOf = [](const PartnerCell& c) {
                return PartnerKey(c.obj, 0, 0, 0);
            };
            // ★Minted from the FORM, never from the pool: an ordinal is a spot
            // NAME now, and a name that never changes is the whole point.
            auto freshSpot = [&](const PartnerCell& c) {
                const std::string base = PartnerKey(c.obj, 0, 0, 0);
                for (int n = 0;; ++n) {
                    std::string k = n == 0 ? base : base + "#" + std::to_string(n);
                    if (!cl->spots.contains(k)) return k;
                }
            };

            if (cl) {
                // GI18: a pending drop position becomes a NEW slot in its pool,
                // and position order hands it to one of that pool's cells.
                // Recording it as a slot (rather than binding it to a cell) is
                // what lets it survive the frames between "player let go" and
                // "engine actually moved the item".
                for (auto ps = g_pendingSpots.begin(); ps != g_pendingSpots.end();) {
                    const PartnerCell* match = nullptr;
                    for (const auto& it : cells) {
                        if (it.obj->GetFormID() == ps->form && it.sig == ps->sig) { match = &it; break; }
                    }
                    if (!match) { ++ps; continue; }   // not here yet: keep waiting

                    // GI23: a REARRANGE names the slot it is moving; a STORE has
                    // none here yet and takes a fresh one. The old code guessed
                    // ("if the pool is already full, reuse its first slot"),
                    // which moved a SIBLING and left the dragged unit to inherit
                    // the vacated cell -- the two appeared to swap.
                    const bool reused =
                        !ps->slotKey.empty() && cl->spots.contains(ps->slotKey);
                    const std::string key = reused ? ps->slotKey : freshSpot(*match);
                    // GI62: the hint's angle defines the new slot's footprint --
                    // match->w/h is the upright pair, so swap it when the drop
                    // was made on its side.
                    const bool turned = (ps->rot & 1) != 0;
                    // ★A POUCH LANDING HERE CLAIMS ITS GOLD. The amount was
                    // parked in g_awayGold by OnPouchLeftPlayer as the item
                    // left the player; taking it now is what makes the shelf
                    // -- rather than a player-wide variable -- the thing that
                    // holds it, so the icon is right and the trip home works.
                    // ★(1.3.0-A) ...but only a NEW slot claims. A REARRANGE
                    // reuses its slot, whose gold is already ON it -- claiming
                    // here rewrote that slot at zero (nothing was away), and
                    // could even steal gold travelling with a DIFFERENT pouch.
                    const int prev = reused ? cl->spots[key].gold : 0;
                    const int carried =
                        (!reused && GoldCoins::IsPouch(match->obj->GetFormID()))
                            ? GoldCoins::TakeAwayGold() : 0;
                    // ★(1.3.0-D) field-wise on purpose: a REUSED slot keeps its
                    // bundle (rearranging a stored bag must not spill it), and
                    // a fresh one claims the bundle its store queued.
                    {
                        auto& sp = cl->spots[key];
                        sp.col = ps->col;
                        sp.row = ps->row;
                        sp.w = turned ? match->h : match->w;
                        sp.h = turned ? match->w : match->h;
                        sp.rot = ps->rot & 3;
                        sp.gold = prev + carried;
                        // ★Stamp the binding AT BIRTH. Pass 1 would stamp it a
                        // few lines later anyway, but only after choosing which
                        // cell this spot belongs to -- and with two identical
                        // items in the chest, an unstamped spot is exactly the
                        // ambiguity that makes it choose the other one.
                        sp.uid  = match->uid;
                        sp.sig  = match->sig;
                        sp.worn = match->worn;
                        if (!reused) {
                            sp.bundle = TakePendingBundle(match->obj->GetFormID());
                        }
                    }
                    if (carried > 0) {
                        SKSE::log::info("[LOOT] pouch shelved with {} G ('{}')",
                            carried, key);
                    }
                    ps = g_pendingSpots.erase(ps);
                }

                // ★The "drop slots of pools that are not here any more" sweep
                // that used to sit here is GONE, subsumed by the matcher's own
                // prune below -- which asks the better question. This one asked
                // whether a spot's POOL was still present, so a signature that
                // merely moved read as "that pool left the chest" and the spot
                // was destroyed with the pouch's gold inside it (handed home,
                // but the position and any bundle died). The matcher prunes
                // what NOTHING CLAIMED, which is what a dead spot actually is.

                // pass 1: bind each live cell to the shelf spot that was showing
                // it, then place it where that spot says.
                //
                // This used to group cells into POOLS and look each pool's spots
                // up BY NAME -- and a spot's name carried the item's signature,
                // so a stored item whose signature moved (ownership cleared by a
                // sale, a quest flag landing, a charge spent on a companion's
                // weapon) belonged to a pool with no spots at all. It got a fresh
                // spot: a new position, and -- because a pouch's gold and a bag's
                // contents hang off the SPOT -- a lost amount and a spilled bag.
                //
                // Matching instead asks: of the spots this FORM already owns in
                // this container, which was showing this cell? The tiers are
                // evidence, strongest first, and the last accepts any leftover,
                // so a signature that moved costs a weaker match rather than a
                // lost spot. Mirrors Grid::EnumerateUnitTiles exactly, on
                // purpose: two boards, one rule.
                //
                // WORN is a hard partition rather than a tier (GI41): a worn cell
                // is locked and a spare is not, so they are never interchangeable
                // and must never trade spots. Plant a spare on a pickpocket mark
                // and the worn cell stays exactly where it is.
                // a carried cell takes its spot with it. Excluding BOTH (the
                // cell is already absent from `cells`) is what keeps the
                // survivors where they are -- otherwise one cell faces two
                // spots, and position order hands the back dagger the front
                // spot the moment the front one is lifted.
                const bool carrying = Grid::HeldPartnerObject() != nullptr;
                std::vector<PartnerCell*> live;
                for (auto& it : cells) live.push_back(&it);

                std::vector<std::pair<std::string, ContSpot>> slots;
                for (const auto& [k, s] : cl->spots) {
                    if (carrying && k == g_actingSpot) continue;   // reserved
                    slots.push_back({ k, s });
                }
                std::sort(slots.begin(), slots.end(), [](const auto& x, const auto& y) {
                    if (x.second.row != y.second.row) return x.second.row < y.second.row;
                    return x.second.col < y.second.col;
                });

                std::vector<int>  spotOf(live.size(), -1);
                std::vector<char> claimed(slots.size(), 0);
                const auto bind = [&](auto&& a_ok) {
                    for (std::size_t u = 0; u < live.size(); ++u) {
                        if (spotOf[u] >= 0) continue;
                        for (std::size_t s = 0; s < slots.size(); ++s) {
                            if (claimed[s]) continue;
                            // form and worn-ness are absolute: never crossed
                            if (baseOfSpotKey(slots[s].first) != baseOf(*live[u])) continue;
                            if (slots[s].second.worn != live[u]->worn) continue;
                            if (!a_ok(*live[u], slots[s].second)) continue;
                            spotOf[u] = static_cast<int>(s);
                            claimed[s] = 1;
                            break;
                        }
                    }
                };
                bind([](const PartnerCell& c, const ContSpot& s) {
                    return c.uid != 0 && s.uid == c.uid; });
                bind([](const PartnerCell& c, const ContSpot& s) {
                    return c.uid == 0 && s.uid == 0 && s.sig == c.sig; });
                bind([](const PartnerCell&, const ContSpot&) { return true; });

                for (std::size_t u = 0; u < live.size(); ++u) {
                    PartnerCell* m = live[u];
                    if (spotOf[u] >= 0) {
                        m->spotKey = slots[spotOf[u]].first;
                        const auto& s = slots[spotOf[u]].second;
                        m->gold = s.gold;
                        // GI62: the slot remembers the angle -- adopt it BEFORE
                        // the fit test, which reads w/h.
                        m->SetRot(s.rot);
                        // (1.3.3) a COMPANION's board is 10 x 8, and a remembered
                        // spot from before that cap (or from the +2 spare rows
                        // every other container still gets) would hold the board
                        // open past it -- the pack drew nine rows for an item
                        // nobody could see down there. Out-of-cap spots are
                        // refused here and first-fit back inside by pass 2.
                        const bool inCap =
                            !companionBoard || s.row + m->h <= kCompanionRows;
                        if (inCap && fits(s.col, s.row, m->w, m->h)) {
                            m->col = s.col;
                            m->row = s.row;
                            mark(s.col, s.row, m->w, m->h);
                        } else if (m->rot != 0) {
                            m->SetRot(0);   // stand it up rather than lose it
                        }
                    } else {
                        m->spotKey = freshSpot(*m);
                        // THE OTHER DOOR. A shelf slot is born in two places:
                        // there (a drop the player aimed at a cell) and here
                        // (everything else -- a store with no aimed cell, an item
                        // that simply appeared). Only the first claimed the
                        // pouch's gold, so storing a full pouch any other way
                        // shelved it as EMPTY and left the amount stranded in
                        // g_awayGold. Both doors take it now.
                        m->gold = GoldCoins::IsPouch(m->obj->GetFormID())
                                      ? GoldCoins::TakeAwayGold() : 0;
                        if (m->gold > 0) {
                            SKSE::log::info("[LOOT] pouch shelved with {} G ('{}', fresh slot)",
                                            m->gold, m->spotKey);
                        }
                        // NEVER CLOBBER AN AMOUNT ALREADY ON THIS SHELF. This
                        // branch re-writes the whole slot, and it runs LATER IN
                        // THE SAME FRAME than the drop that shelved the pouch --
                        // so a pouch stored with 6,943 G had its slot rebuilt at
                        // zero a few lines further down, and drew empty. The
                        // position may be rewritten; the gold may not.
                        auto& sp = cl->spots[m->spotKey];
                        const int keep = sp.gold > 0 ? sp.gold : m->gold;
                        sp.col = -1;
                        sp.row = -1;
                        sp.w = m->w;
                        sp.h = m->h;
                        sp.rot = m->rot;
                        sp.gold = keep;
                        if (sp.bundle.empty()) {
                            // (1.3.0-D) the OTHER door a stored bag's spot can be
                            // born through -- its bundle rides too
                            sp.bundle = TakePendingBundle(m->obj->GetFormID());
                        }
                    }
                    // the spot records WHICH unit it is showing. A hint for the
                    // next rebuild, never a name: writing it cannot move a cell.
                    auto& sp = cl->spots[m->spotKey];
                    sp.uid  = m->uid;
                    sp.sig  = m->sig;
                    sp.worn = m->worn;
                }

                // spots nothing claimed: the shelf shrank
                if (!Grid::HeldPartnerObject()) {
                    for (std::size_t s = 0; s < slots.size(); ++s) {
                        if (claimed[s]) continue;
                        // (1.3.0-B) same rule as every other spot death: the gold
                        // goes home instead of dying with the slot. (Two pouches
                        // stored in one chest merge into ONE shelf cell -- the
                        // second slot lands here the very frame it was made.)
                        if (const auto si = cl->spots.find(slots[s].first);
                            si != cl->spots.end() && si->second.gold > 0) {
                            SKSE::log::info(
                                "[LOOT] shelf slot dropped, {} G goes home ('{}')",
                                si->second.gold, slots[s].first);
                            GoldCoins::GiveAwayGold(si->second.gold);
                        }
                        cl->spots.erase(slots[s].first);
                    }
                }
            }

            // pass 2: first-fit for everything unplaced
            // ★(1.3.3) a companion fills its 10 x 8 pack FIRST and only
            // grows past it when what it already carries genuinely will not
            // fit (its own gear, a script's gift). The cap governs what the
            // player may add; it never hides what is already inside.
            for (auto& it : cells) {
                if (companionBoard) {
                    for (int r = 0; r + it.h <= kCompanionRows && it.col < 0; ++r) {
                        for (int c = 0; c < cols; ++c) {
                            if (!fits(c, r, it.w, it.h)) continue;
                            it.col = c;
                            it.row = r;
                            mark(c, r, it.w, it.h);
                            break;
                        }
                    }
                    if (it.col >= 0) continue;
                }
                for (int r = companionBoard ? kCompanionRows : 0; it.col < 0; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        if (fits(c, r, it.w, it.h)) {
                            it.col = c;
                            it.row = r;
                            mark(c, r, it.w, it.h);
                            break;
                        }
                    }
                }
            }

            // pass 3 (F7): record the result under the ASSIGNED slot key —
            // idempotent per frame
            if (cl) {
                for (const auto& it : cells) {
                    if (!it.spotKey.empty()) {
                        // ★★(1.3.0-A) THE THIRD DOOR WAS THE OVERWRITER. This
                        // rewrite runs every frame, AFTER both shelving doors,
                        // and its braced init left `gold` value-initialised --
                        // so the amount either door had just recorded was
                        // zeroed in the SAME frame (the diag pair: col/row
                        // alive, gold 0). Position is this pass's to write;
                        // the gold (and D: the bundle) is the layout's and
                        // stays -- hence field-wise, never a whole-struct init.
                        auto& sp = cl->spots[it.spotKey];
                        sp.col = it.col;
                        sp.row = it.row;
                        sp.w = it.w;
                        sp.h = it.h;
                        sp.rot = it.rot;
                    }
                }
            }

            // actual height = furthest occupied row. GI15: absent items no
            // longer hold a shelf open -- their spots were just pruned.
            int placedRows = 0;
            for (auto& it : cells) placedRows = (std::max)(placedRows, it.row + it.h);
            return placedRows;
        }

        // stage 3: partner grid lines + cells (hover / tooltip / carry /
        // take / buy) + scroll extent — runs inside the grid child
        void DrawPartnerCells(std::vector<PartnerCell>& cells, int rows,
                              float gridW, float sbW, const ImVec2& base)
        {
            auto* dl = ImGui::GetWindowDrawList();
            auto* cache = IconCache::GetSingleton();
            const auto& sk = Theme::S();
            const float cell = Grid::CellPx();
            const int cols = Grid::kCols;
            // ★The board, drawn by the SAME function the player's grid uses.
            // This was a private accent hairline, which meant a skin that
            // carves or tiles its cells did so on one half of the screen only
            // — and on SIMPLE that hairline is near-invisible navy on a light
            // blue panel, so the merchant's board had no cells at all.
            Grid::DrawCellLattice(dl, base, cols, rows);

            // (the board's outer edge is drawn OUTSIDE this child, at a fixed
            //  position -- see PartnerBoardEdge)

            // GI16: occupied-cell shading, the same pass the player grid runs
            // (DrawOccupancyPass). Its absence here was never a deliberate
            // choice -- the partner window simply drew sprites onto bare grid
            // lines, so the two halves of the same screen read differently.
            // Worn gear takes a pale amber fill instead of the neutral shade:
            // on a corpse or a pickpocket target, "this is on the body" is the
            // one distinction that changes what the player does next.
            // same ground as the player's grid and the doll (see Grid.cpp)
            const ImU32 shadeCol = Theme::OccupiedGround();
            // ★★★NO COLOUR FOR "WORN". It used to take a hardcoded amber ground
            // -- the one colour on this board that did not come from the skin,
            // which is why it looked wrong on every one of them -- and the
            // colour could not carry the meaning anyway. Nothing teaches a
            // player that a pale ground means "this is on the body"; the state
            // was read as new-pickup instead, by the person who designed it.
            // Worse, it collided with what a ground already says here: our own
            // grid uses ground tints for new-pickup and open-bag.
            // ★It is a MARK now, in the corner, the way the lock says "you
            // cannot have this". See the worn silhouette further down.
            // ★The clearance is the skin's, not a constant. Same rule as the
            // player's grid: an engraved skin carves after the cell, an ink
            // skin rules ON TOP afterwards and so needs none, and only a plain
            // hairline wants a pixel either side. A fixed 1.0 left a bright
            // seam on the ink skins and shrank the cell on the engraved ones,
            // which is the "subtly different from the inventory" part.
            const float in0 = (sk.engravedCells || Theme::InkChrome()) ? 0.0f : 1.0f;
            const float in1 = sk.engravedCells
                                ? Theme::kGrooveW * Theme::Scale() * 0.5f
                            : Theme::InkChrome() ? 0.0f
                                                 : 1.0f;
            for (const auto& it : cells) {
                if (Grid::IsHeldPartnerUnit(it.obj, it.uid, it.xlIdx, it.ord)) continue;
                for (int y = 0; y < it.h; ++y) {
                    for (int x = 0; x < it.w; ++x) {
                        const int cc = it.col + x, rr = it.row + y;
                        const ImVec2 c0(base.x + cc * cell, base.y + rr * cell);
                        const ImVec2 q0(c0.x + (cc > 0 ? in1 : in0),
                                        c0.y + (rr > 0 ? in1 : in0));
                        const ImVec2 q1(c0.x + cell - (cc + 1 < cols ? in1 : in0),
                                        c0.y + cell - (rr + 1 < rows ? in1 : in0));
                        dl->AddRectFilled(q0, q1, shadeCol);
                    }
                }
            }

            // ★★★The ink skins' lattice, and the reason the partner board had
            // NO cells at all on them: DrawCellLattice deliberately draws
            // nothing under InkChrome -- the marks have to land OVER the
            // occupied ground, not be cleared around by it -- and the second
            // call that actually draws them was only ever made by the player's
            // grid. Same order as there: chrome, ground, THEN the lattice.
            Grid::DrawInkLattice(dl, base, cols, rows);

            // cells
            for (size_t i = 0; i < cells.size(); ++i) {
                auto& it = cells[i];
                // carried FROM here: keep the slot reserved (placement above still
                // counted it, so neighbours don't shift), just don't draw it — the
                // cell reads empty until the trade actually completes.
                if (Grid::IsHeldPartnerUnit(it.obj, it.uid, it.xlIdx, it.ord)) continue;
                const ImVec2 p0(base.x + it.col * cell, base.y + it.row * cell);
                const float bw = it.w * cell, bh = it.h * cell;   // footprint box

                // click target: right-click = TAKE (loot mode). Barter buy is
                // Phase 5. Gold ignores space; other items need a free cell.
                // While carrying, SKIP the cell buttons — they'd swallow the drop
                // click over an occupied cell (drag-to-store must reach the window
                // hover test regardless of what's under the cursor).
                if (!Grid::IsHolding()) {
                    char idbuf[16];
                    std::snprintf(idbuf, sizeof(idbuf), "##pc%zu", i);
                    ImGui::SetCursorScreenPos(p0);
                    ImGui::InvisibleButton(idbuf, ImVec2(bw, bh));
                    if (ImGui::IsItemHovered() && !UIRoot::MouseInOverlay()) {
                        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                            Theme::Acc(0.10f));
                        Sfx::HoverNote(ImGui::GetItemID());   // partner cell hover
                        // Phase 4: rich tooltip; barter side shows the BUY price
                        int price = -1;
                        if (g_mode == Mode::kBarter && it.value > 0 && !it.obj->IsGold()) {
                            price = BuyPrice(it.obj, it.value);
                        }
                        // D1: read the PARTNER's entry, not the player's —
                        // this is what leaked our own item's extras onto a
                        // merchant's identical ware. kAny: the cell is a
                        // per-form aggregate with a stack badge (by design).
                        // GI13: a per-unit cell must ALWAYS read its own
                        // sub-stack. Falling back to kAny for a plain unit (uid
                        // 0, no list) made every ordinary dagger in the chest
                        // display the tempered one's stats -- kAny means "first
                        // list carrying the trait", which is exactly the bug
                        // this whole refactor exists to kill.
                        // ★(1.3.0) a shelved pouch's tooltip prints its stored
                        // amount -- the icon already asked the layout (below),
                        // but this call passed -1 and the "N / cap G" line
                        // never drew.
                        const int tipGold =
                            GoldCoins::IsPouch(it.obj->GetFormID())
                                ? ShelfGoldOf(it.spotKey) : -1;
                        Grid::DrawItemTooltip(it.obj, it.count, tipGold, price, true,
                                              SourceRef(),
                                              it.perUnit ? Grid::ExtraScope::kUnit
                                                         : Grid::ExtraScope::kAny,
                                              it.uid, it.xlIdx, 0, 0,
                                              Grid::TileContext{ {}, false, false, true, false });
                        // C: 3D view, same as the player's grid. Vanilla files
                        // Item Zoom under the kItemMenu context, which the
                        // container and barter screens share with the
                        // inventory -- turning a ware over before buying it is
                        // vanilla behaviour, not an extra.
                        if (ImGui::IsKeyPressed(ImGuiKey_C, false) &&
                            !ImGui::GetIO().WantTextInput) {
                            UIRoot::OpenInspect(it.obj, Grid::DefKeyOf(it.obj));
                        }
                    }
                    // Phase 5-B: plain left-click (no shift) picks the item onto the
                    // cursor — drag to the player grid to TAKE (loot) / BUY (barter).
                    // Merchant gold isn't carriable.
                    if (UIRoot::MouseInOverlay()) {
                        // popup chrome overlaps this cell: no click-through
                    } else if (Editor::IsEditMode()) {
                        // ★★EDIT reaches the PARTNER board too. A def belongs to
                        // the FORM, so an item's footprint, icon and rotation are
                        // the same object whether the item is in the pack or on a
                        // merchant's shelf -- and a shelf is the only place many
                        // forms are ever seen. Having to buy something before it
                        // could be sized was an accident of where the click
                        // handler lived, not a rule anyone chose.
                        // ★It PRE-EMPTS the take / buy / lift branches below, the
                        // same contract the player grid keeps: in EDIT a click
                        // selects and does nothing else. Gold included -- the
                        // coin tiles are editable on our own board.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                            Editor::Select(it.obj, Grid::DefKeyOf(it.obj));
                        }
                    } else if (!ImGui::GetIO().KeyShift &&
                        ImGui::IsItemClicked(ImGuiMouseButton_Left) && !it.obj->IsGold()) {
                        if (it.locked) {
                            // F6b: worn without Perfect Touch / GI42: unnameable
                            Sfx::FailNote(Lang::T(it.unnameable
                                ? Lang::Str::AmbiguousUnit
                                : Lang::Str::PickpocketBlocked));
                        } else {
                            // GI62c: centred on the cursor, same as a player tile
                            // -- rotation has to spin about the cursor, and that
                            // only holds if the cursor is the item's middle from
                            // the moment it is lifted (-1 = centre).
                            g_actingSpot = it.spotKey;   // GI20: this cell's slot
                            g_carryGlow = it.glow;       // (1.3.2) markers ride along
                            Grid::BeginPartnerCarry(it.obj, it.count, it.value,
                                -1.0f, -1.0f, it.uid, it.xlIdx, it.ord, it.rot);
                        }
                    }
                    // TAKE trigger: right-click (whole move) OR shift+left-click
                    // (explicit split). Either way a stack (>1) opens the quantity
                    // slider first; a single item moves at once.
                    else if (IsLootMode(g_mode)) {
                        const bool rcRaw = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                        // ★(1.3.0) USE MODE -- SkyUI's "Shift = Equip Mode",
                        // spelled in our grammar. There the primary key is E and
                        // Shift changes what E does; here taking is the RIGHT
                        // button, so Shift changes what the right button does.
                        // It cannot collide with shift+LEFT (split): different
                        // button, which is how SkyUI keeps the two apart as well.
                        // kLoot covers container, corpse AND companion, which is
                        // the whole of the intended scope. Pickpocket has its own
                        // block below and is deliberately left out -- using an
                        // item off a living mark is not a thing to offer.
                        const bool useRc = rcRaw && ImGui::GetIO().KeyShift;
                        const bool rc = rcRaw && !useRc;
                        const bool splitLc = ImGui::GetIO().KeyShift &&
                                             ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        // ★(1.3.1) a BAG manages on right-click HERE TOO -- the
                        // player board's grammar (bag right-click is ALWAYS the
                        // window toggle; trade and storage happen by drag only)
                        // now holds on both sides. Taking the shelf bag home is
                        // the DRAG; right-click opens its bundle window.
                        // ★(1.3.2a) and the POUCH manages the same way: its
                        // right-click is the withdraw window over the SLOT's
                        // amount, exactly the player pouch's grammar.
                        if ((rc || splitLc) && GoldCoins::IsPouch(it.obj->GetFormID())) {
                            if (g_shelfPouchSpot == it.spotKey) {
                                g_shelfPouchSpot.clear();
                                Sfx::SelectOff();
                            } else if (!it.spotKey.empty()) {
                                g_shelfPouchSpot = it.spotKey;
                                g_shelfPouchForm = it.obj->GetFormID();
                                g_shelfPouchSlider =
                                    (std::max)(1, ShelfGoldOf(it.spotKey) / 2);
                                Sfx::SelectOn();
                            }
                        } else if ((rc || splitLc) && Grid::ResolveDef(it.obj).bag != 0) {
                            const auto sb = std::find_if(g_shelfBags.begin(),
                                g_shelfBags.end(), [&](const ShelfBagWin& a_w) {
                                    return a_w.spot == it.spotKey;
                                });
                            if (sb != g_shelfBags.end()) {
                                g_shelfBags.erase(sb);
                                Sfx::BagClose();
                            } else if (!it.spotKey.empty()) {
                                g_shelfBags.push_back({ it.spotKey,
                                                        it.obj->GetFormID() });
                                Sfx::BagOpen();
                            }
                        // ★★USE MODE lands here -- AFTER the pouch and the bag,
                        // whose right-click already means something of their own
                        // (withdraw, open). Ordering does the exclusion, so there
                        // is no list of special forms to keep in step with them.
                        // ★★★BOOKS ONLY, and the limit is deliberate.
                        //
                        // Consumables went through "take one unit, then use it",
                        // because the engine can only consume from the player's
                        // own inventory. That worked -- and reliably took the
                        // game down on the SECOND potion drunk out of a
                        // container, always in the same place: the icon path,
                        // making a virtual call on an object that is no longer
                        // one.
                        //
                        // Five hypotheses were tested against crash logs and
                        // ruled out: a stale view index, the engine's
                        // loadedModels list, a dead form pointer, typed-bag
                        // routing, and a tile object that was never on the
                        // board. The last one is instructive -- the pointer WAS
                        // valid at the previous rebuild and the memory behind it
                        // was gone by the time it was drawn, which no test that
                        // compares pointers can catch.
                        //
                        // Whatever that is, it is not this feature's to fix, and
                        // a convenience is not worth a reproducible CTD. Books,
                        // notes and spell tomes need no round trip through the
                        // pack at all (a book is read where it lies), never
                        // touched the failing path, and were verified working --
                        // so that is what the mode does. Anything else falls
                        // through to an ordinary take, which is what shift+right
                        // did before this existed.
                        } else if (useRc && it.obj->As<RE::TESObjectBOOK>()) {
                            if (it.locked) {   // GI42: the twin is worn
                                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                            } else if (auto* bk = it.obj->As<RE::TESObjectBOOK>();
                                       bk && !bk->TeachesSpell()) {
                                // ★READ IN PLACE. A book needs no owner to be
                                // read, so taking one first would leave a note
                                // you only meant to glance at sitting in the
                                // pack. A spell tome is NOT this case -- reading
                                // one destroys it, so it has to be ours first.
                                Grid::RequestBookRead(bk, it.uid, it.sig);
                            } else if (!Grid::CanFitNewItem(it.obj)) {
                                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                            } else {
                                // One unit, taken and then used: the engine can
                                // only consume from the player's own inventory,
                                // so passing through it is the only road there
                                // is, not a shortcut.
                                g_actingSpot = it.spotKey;   // GI20
                                RequestTake(it.obj, 1, it.uid, it.sig, it.worn,
                                            /*useAfter=*/true);
                            }
                        } else if (rc || splitLc) {
                            if (it.locked) {   // GI42
                                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                            } else if (!(it.obj->IsGold() || Grid::CanFitNewItem(it.obj))) {
                                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                            } else if (it.count > 1) {
                                g_actingSpot = it.spotKey;   // GI20
                                OpenSlider(it.obj, it.count, XferDir::kTake, {}, 0, it.uid, it.sig, it.worn);
                            } else {
                                g_actingSpot = it.spotKey;   // GI20
                                RequestTake(it.obj, it.count, it.uid, it.sig, it.worn);
                            }
                        }
                    } else if (g_mode == Mode::kPickpocket) {
                        // F6b: right-click / shift+left = ATTEMPT the lift
                        // (stack -> quantity slider). The roll runs on the Tick.
                        const bool rc = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                        const bool splitLc = ImGui::GetIO().KeyShift &&
                                             ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        if (rc || splitLc) {
                            if (it.locked) {
                                Sfx::FailNote(Lang::T(it.unnameable
                                    ? Lang::Str::AmbiguousUnit
                                    : Lang::Str::PickpocketBlocked));
                            } else if (!(it.obj->IsGold() || Grid::CanFitNewItem(it.obj))) {
                                Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                            } else if (it.count > 1) {
                                OpenSlider(it.obj, it.count, XferDir::kPickTake, {}, 0, it.uid, it.sig, it.worn);
                            } else {
                                g_actingSpot = it.spotKey;
                                RequestPickTake(it.obj, 1, it.uid, it.sig, it.worn);
                            }
                        }
                    } else if (g_mode == Mode::kBarter) {
                        // BUY: right-click the merchant's item. A stack opens the
                        // quantity slider; a single item buys at once. Blocked when
                        // the player can't afford it or has no room.
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !it.obj->IsGold()) {
                            if (it.locked) {   // GI42: merchant wears the twin
                                Sfx::FailNote(Lang::T(Lang::Str::AmbiguousUnit));
                            } else {
                            g_actingSpot = it.spotKey;   // GI20
                            if (it.count > 1) {
                                OpenSlider(it.obj, it.count, XferDir::kBuy, {}, it.value, it.uid, it.sig);
                            } else {
                                const int total = BuyPrice(it.obj, it.value);
                                if (Grid::GoldAmount() < total) {
                                    Sfx::FailNote(Lang::T(Lang::Str::NotEnoughGold));
                                } else if (!Grid::CanFitNewItem(it.obj)) {
                                    Sfx::FailNote(Lang::T(Lang::Str::InventoryFull));
                                } else {
                                    RequestBuy(it.obj, 1, total, it.value, it.uid, it.sig);
                                }
                            }
                            }
                        }
                    }
                }

                // GI51: same rule as the player's grid — a chest full of items
                // with no captures yet must still READ as a chest full of
                // items, which is exactly what you need to decide whether to
                // take anything.
                // ★A SHELVED POUCH DRAWS ITS OWN BAND. Without this the chest
                // showed the empty-pouch art for a pouch with 6,000 G in it:
                // the icon came from the bare form, and the amount lived
                // somewhere the shelf could not see.
                RE::TESBoundObject* cellIconObj = it.obj;
                const int shelfGold = GoldCoins::IsPouch(it.obj->GetFormID())
                                          ? ShelfGoldOf(it.spotKey) : 0;
                if (shelfGold > 0) {
                    if (auto* v = GoldCoins::PouchIconObjectFor(shelfGold)) cellIconObj = v;
                }
                const IconCache::Icon* cellIcon = cache->Get(cellIconObj);
                if (!cellIcon) {
                    cache->QueueCapture(cellIconObj);   // first sight — render next frames
                    cellIcon = Fallback::Get(cellIconObj);
                }
                if (const auto* icon = cellIcon) {
                    // contain the icon inside the footprint box, keeping aspect.
                    // ★GI62: measure against the UPRIGHT box. The sprite is not
                    // rotated -- only its draw quad is -- so a turned cell would
                    // otherwise fit a tall sword into a short cell height and
                    // shrink it to a third of its size (user-reported).
                    const bool turned = (it.rot & 1) != 0;
                    const float fitW = turned ? bh : bw;
                    const float fitH = turned ? bw : bh;
                    const float sc = (std::min)(fitW / static_cast<float>(icon->w),
                                                fitH / static_cast<float>(icon->h)) * 0.85f;
                    const float dw = icon->w * sc;
                    const float dh = icon->h * sc;
                    const ImVec2 ctr(p0.x + bw * 0.5f, p0.y + bh * 0.5f);
                    const ImVec2 i0(ctr.x - dw * 0.5f, ctr.y - dh * 0.5f);
                    const ImVec2 i1(ctr.x + dw * 0.5f, ctr.y + dh * 0.5f);
                    // rarity glow UNDER the sprite — same pass as the player grid
                    Grid::DrawGlow(dl, it.obj, it.glow, i0, i1,
                        p0, ImVec2(p0.x + bw, p0.y + bh), it.rot);
                    const bool cellFallback = cellIcon && !cache->Get(cellIconObj);
                    const auto cdef = Grid::ResolveDef(it.obj);
                    // both styles — the partner board draws the same item the
                    // player's board does, and one offset rule serves both
                    const ImVec2 nudge = Grid::RotatedOffset(cdef.fx, cdef.fy, it.rot);
                    const float pdeg = (cellFallback ? cdef.frot : 0.0f) + it.rot * 90.0f;
                    // ★1.0.5: same shadow as the player's board
                    Grid::DrawItemShadow(dl, icon->srv,
                                         ImVec2(ctr.x + nudge.x, ctr.y + nudge.y),
                                         dw, dh, pdeg);
                    UIRoot::DrawItemIconRot(dl, icon->srv,
                        ImVec2(ctr.x + nudge.x, ctr.y + nudge.y), ImVec2(dw, dh), pdeg);
                    // ★Same wash and the same alpha the player's board uses for
                    // a search miss — one search, one look, both windows.
                    if (FindMisses(it.obj)) {
                        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                            IM_COL32(6, 6, 10, 168));
                    }
                    // ★1.0.5: the shared marker tray, so poison keeps showing
                    // here now that DrawGlow no longer draws it. Favourite
                    // stays off -- a PartnerCell is a container's item, not the
                    // player's, and carries no star.
                    // ★"On the body" joins them rather than being drawn beside
                    // them: it used to own the same corner the tray starts from
                    // and covered the poison droplet outright. Not while LOCKED
                    // -- the lock already means worn and owns that square.
                    // ★★★STOLEN IS ON IN kSteal, and the mode is what says so --
                    // the cell carries no flag of its own because nothing has
                    // been taken yet. An owned container is owned WHOLE, so
                    // every item in it will become stolen goods the moment it
                    // moves, and the mark says that in advance.
                    // This is vanilla's own economy of symbols: the hand it puts
                    // beside an item in a stealable container is the SAME hand
                    // it puts beside that item once it is in your pack. One
                    // mark, one meaning -- "someone else's". Ours is the crimson
                    // dot, and it now works the same way on both sides.
                    // ★A per-item mark rather than the window's STEAL label
                    // alone, because the label did not reach the person who
                    // designed it: the behaviour (goods turning stolen) was
                    // noticed, the label was not.
                    Grid::DrawMarkerTray(dl, p0, ImVec2(p0.x + bw, p0.y + bh),
                                         false, g_mode == Mode::kSteal,
                                         (it.glow & 0x4) != 0,
                                         it.worn && !it.locked);
                }
                // ★The EDIT selection ring, so the partner board can say WHICH
                // form is being edited -- without it the click has no visible
                // answer and the panel's numbers look like they belong to
                // nothing. A plain rect rather than the player board's mask
                // outline because a partner cell IS a rectangle (no polyomino).
                // ★IsEditMode() FIRST. DefKeyOf builds a string, and this runs
                // once per visible cell every frame -- a merchant's hundred
                // wares was ~12k heap allocations a second to answer a question
                // that is "no" whenever EDIT is closed, which is almost always.
                // Rule 3: no string work on the per-frame path.
                if (Editor::IsEditMode() && Editor::IsSelected(Grid::DefKeyOf(it.obj))) {
                    dl->AddRect(p0, ImVec2(p0.x + bw, p0.y + bh),
                                Theme::Col(Theme::S().sel, 1.0f), 0.0f, 0, 2.0f);
                }
                // GI8: extension overlay (socket wells). ★This is the side that
                // matters most for the socket mod -- the point is to SEE what a
                // chest holds and decide whether to take it, so the badges have
                // to be on the partner cell, not only on our own grid.
                // The partner window draws plain rectangles (no polyomino mask),
                // so the default full-rect shape is correct here.
                {
                    Badges::TileShape shape;
                    shape.w = it.w;
                    shape.h = it.h;
                    auto       pr = g_partner.get();
                    // ★IsMouseHoveringRect is geometry only -- it clips, but it
                    // never asks who is on top, so a badge under another window
                    // still lit up. Cosmetic where the other two were not, but
                    // it is the same missing question, so it gets the same gate.
                    // (The cell's InvisibleButton is not available here: it is
                    // only submitted while nothing is being carried.)
                    const bool hov =
                        UIRoot::CursorOwnsWindow() &&
                        ImGui::IsMouseHoveringRect(
                            p0, ImVec2(p0.x + bw, p0.y + bh), false);
                    Badges::Draw(dl, p0, bw, bh, shape,
                                 pr ? pr->GetFormID() : 0u,
                                 it.obj->GetFormID(), it.uid, hov);
                }

                if (it.count > 1) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", it.count);
                    Grid::DrawCountBadge(dl, p0, buf);
                }

                // GI13: worn-on-body marker. Corpse looting shows the NPC's
                // equipped gear alongside their pack, and there was no way to
                // tell which was which. Accent hairline + a small up-chevron at
                // the bottom-right marker spot (the padlock's slot is only used
                // in pickpocket mode, and a locked cell is worn by definition).
                // F6b: corner grammar — bottom-left success %, colour-coded
                // (green 90+ / yellow 50~89 / red <50); locked worn gear dims
                // with a small padlock at the bottom-right marker spot.
                if (g_mode == Mode::kPickpocket) {
                    if (it.locked) {
                        // greyed out: "and you can't have it"
                        dl->AddRectFilled(p0, ImVec2(p0.x + bw, p0.y + bh),
                            IM_COL32(0, 0, 0, 90));
                        const float ps = 10.0f * Theme::Scale();
                        const ImVec2 lp(p0.x + bw - ps - 4.0f, p0.y + bh - ps - 4.0f);
                        const ImU32 lc = IM_COL32(220, 200, 150, 230);
                        // body + shackle
                        dl->AddRectFilled(ImVec2(lp.x, lp.y + ps * 0.45f),
                            ImVec2(lp.x + ps, lp.y + ps), lc, 1.5f);
                        dl->AddCircle(ImVec2(lp.x + ps * 0.5f, lp.y + ps * 0.30f),
                            ps * 0.30f, lc, 0, 1.5f);
                    } else if (it.chance >= 0) {
                        const ImU32 cc = it.chance >= 90 ? IM_COL32(120, 205, 110, 255)
                                       : it.chance >= 50 ? IM_COL32(225, 195, 90, 255)
                                                         : IM_COL32(225, 95, 80, 255);
                        char pb[8];
                        std::snprintf(pb, sizeof(pb), "%d%%", it.chance);
                        const float th = ImGui::GetTextLineHeight();
                        const ImVec2 tp(p0.x + 3.0f, p0.y + bh - th - 2.0f);
                        const ImU32 outline = IM_COL32(0, 0, 0, 255);
                        dl->AddText(ImVec2(tp.x - 1, tp.y), outline, pb);
                        dl->AddText(ImVec2(tp.x + 1, tp.y), outline, pb);
                        dl->AddText(ImVec2(tp.x, tp.y - 1), outline, pb);
                        dl->AddText(ImVec2(tp.x, tp.y + 1), outline, pb);
                        dl->AddText(tp, cc, pb);
                    }
                }
            }
            // F7: drop ghost — green = free spot (stores/moves here), red =
            // swap / invalid. Spot-memory modes only (barter auto-packs; a
            // preview would lie).
            // ★★(1.3.2) COMPUTED HERE, not via QueryStoreDrop. That helper
            // refuses unless `g_partnerHovered` is set -- and that flag is
            // published AFTER this function returns, having been cleared at
            // the top of the frame, so the query answered "not on a cell"
            // every single time and the ghost never drew on any container.
            // The anchor formula below is QueryStoreDrop's, verbatim, so
            // preview and result still cannot disagree.
            if (SpotMemoryOn() && Grid::IsHolding() &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                int hw = 1, hh = 1;
                float ox = 0.0f, oy = 0.0f;
                if (Grid::HeldFootprint(hw, hh, ox, oy)) {
                    const ImVec2 m = ImGui::GetIO().MousePos;
                    int gc = static_cast<int>(std::lround((m.x - base.x - ox) / cell));
                    int gr = static_cast<int>(std::lround((m.y - base.y - oy) / cell));
                    gc = (std::max)(0, (std::min)(Grid::kCols - hw, gc));
                    gr = (std::max)(0, (std::min)(rows - hh, gr));
                    int blockers = 0;
                    for (const auto& pc : cells) {
                        if (pc.col < 0 ||
                            Grid::IsHeldPartnerUnit(pc.obj, pc.uid, pc.xlIdx, pc.ord)) {
                            continue;
                        }
                        if (gc < pc.col + pc.w && gc + hw > pc.col &&
                            gr < pc.row + pc.h && gr + hh > pc.row) {
                            ++blockers;
                        }
                    }
                    const ImU32 ghost = blockers == 0 ? IM_COL32(90, 170, 90, 90)
                                                      : IM_COL32(190, 60, 60, 110);
                    const ImVec2 g0(base.x + gc * cell, base.y + gr * cell);
                    dl->AddRectFilled(g0,
                        ImVec2(g0.x + hw * cell, g0.y + hh * cell), ghost);
                }
            }

            // scroll extent = exactly the grid height. The cells were drawn with
            // absolute SetCursorScreenPos, so reset the cursor to `base` before the
            // Dummy — otherwise it stacks onto the last tile's position and the
            // wheel scrolls far past the grid.
            ImGui::SetCursorScreenPos(base);
            // +1 matches the child's +1 so the bottom grid line stays inside the
            // view even when scrolled fully to the end (otherwise it sits exactly
            // on the clip edge and vanishes).
            ImGui::Dummy(ImVec2(gridW + sbW, rows * cell + 1.0f));
        }

        // stage 4: bottom strip — barter = merchant GOLD bar, loot = the
        // R shortcut hint (design pass C/G); runs inside the window
        void DrawPartnerBottomBar(const ImVec2& size, float a_gridW)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            const float insX = Theme::FrameInsetX(), insY = Theme::FrameInsetY();
            // bottom strip (design pass C/G): same divider + label/value grammar
            // as the player window's GOLD bar. Barter = merchant gold; loot = the
            // R shortcut hint (discoverability).
            {
                auto* wdl = ImGui::GetWindowDrawList();
                const ImVec2 wp = ImGui::GetWindowPos();
                const float gy = wp.y + size.y - insY - BottomStripH();
                // ★★The window's content inset is (insX + PadX) -- that is what
                // this window's PushStyleVar(WindowPadding) sets. This strip is
                // drawn straight onto the draw list and so bypassed it, sitting
                // PadX further left than everything else in the window. On the
                // ink skins the frame is a brush stroke rather than a hairline,
                // so "slightly outside the inset" became the label lying ON the
                // border. Same inset as the content it sits under.
                const float pad = Theme::PadX() * S;
                const float x0 = wp.x + insX + pad;
                // ★★★TO THE BOARD'S RIGHT EDGE, NOT THE WINDOW'S. The window is
                // `gridW + sbW` wide, and sbW is the SCROLLBAR GUTTER -- 0 until
                // the partner happens to hold more than 12 rows of goods, 14px
                // after that. Ruling this strip off the window width therefore
                // ran it 14px past the last column and underneath the scrollbar,
                // on merchants only, which is exactly how a footer rule reads as
                // "the scrollbar has a border along its bottom" (user report,
                // twice). A container that does not scroll never showed it,
                // which is what made it look like a scrollbar defect.
                // ★The gutter is CHROME, not content: the strip belongs to the
                // board above it and must be the same width whether or not the
                // wares happen to overflow.
                const float x1 = x0 + a_gridW;
                wdl->AddLine(ImVec2(x0, gy), ImVec2(x1, gy), Theme::Rule());
                // ★★★THE SAME GLYPH STYLE AS THE PLAYER'S GOLD BAR. This strip
                // is the partner's half of one line across the screen, and it
                // was drawn with plain AddText at the default font size while
                // the player's half used Theme::TextOutlined at FontValue().
                // On a skin whose chrome carries outlines, the two halves then
                // wore different type -- same row, same grammar, different
                // lettering. One helper, one size, both sides.
                const float vpx = Theme::FontValue();
                const float ty  = gy + 8.0f * S;
                if (g_mode == Mode::kBarter) {
                    char amt[32];
                    if (g_merchGoldInf) {
                        std::snprintf(amt, sizeof(amt), "\xE2\x88\x9E");   // U+221E
                    } else {
                        std::snprintf(amt, sizeof(amt), "%d", MerchantGold());
                    }
                    char lbl[96];
                    if (sk.diamondLabels) {
                        std::snprintf(lbl, sizeof(lbl), "\xE2\x97\x87 %s",
                            Lang::T(Lang::Str::MerchantGoldLabel));
                    } else {
                        std::snprintf(lbl, sizeof(lbl), "%s",
                            Lang::T(Lang::Str::MerchantGoldLabel));
                    }
                    // ★Chrome at FULL alpha, as the player's label has it. The
                    // 0.72 here was a second opinion about the same token.
                    Theme::TextOutlined(wdl, ImVec2(x0, ty),
                        sk.diamondLabels ? Theme::Col(sk.sel, 1.0f) : Theme::Chrome(1.0f),
                        lbl, vpx);
                    const float amtW = Theme::TrackedSize(amt, vpx, 0.0f).x;
                    Theme::TextOutlined(wdl, ImVec2(x1 - amtW, ty),
                        Theme::GoldCol(), amt, vpx);
                } else if (g_mode == Mode::kPickpocket) {
                    // ★Nothing. The strip exists to say what the R key does or
                    // what the merchant can pay; a pickpocket has neither, and
                    // repeating the window's own title down here said the same
                    // word twice on one screen.
                } else {
                    Theme::TextOutlined(wdl, ImVec2(x0, ty), Theme::Col(sk.inkDim, 1.0f),
                        Lang::T(Lang::Str::HintTakeAll), vpx);
                }
            }
        }

        // stage 5: R = take everything that FITS (loot; window-hover gated)
        void TakeAllShortcut(std::vector<PartnerCell>& cells)
        {
            // R = take everything that FITS (loot). Gold ignores space; other
            // items consume free cells (approximate — ignores fragmentation, but
            // never over-takes). Stops once the grid is full. Gated on THIS window
            // being hovered — the key is global, so an ungated R also fired while
            // the cursor was over the PLAYER window (whose hover+R means "drop
            // one"), taking the whole container by accident (user-reported).
            if (IsLootMode(g_mode) && !g_slider.active &&
                ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_R, false) && !ImGui::GetIO().WantTextInput) {
                // total/used already span the bags (open or closed) — adding
                // bag room on top of that double-counted it and over-took
                int free = Grid::SpaceTotal() - Grid::SpaceUsed();
                for (auto& c : cells) {
                    if (c.obj->IsGold()) {
                        RequestTake(c.obj, c.count);   // gold bypasses space
                        continue;
                    }
                    const int span = Grid::CellSpanOf(c.obj);
                    if (span <= free && Grid::CanFitNewItem(c.obj)) {
                        // ★(1.3.0) name the slot so the take retires IT --
                        // pouch gold and bag bundles ride the slot, and the
                        // pool-prune fallback only ran for vanished pools.
                        g_actingSpot = c.spotKey;
                        RequestTake(c.obj, c.count);
                        free -= span;
                    }
                }
            }
        }
    }

    void DrawWindows()
    {
        g_partnerHovered = false;
        g_partnerGridLive = false;   // F7: re-armed below while drawing
        if (g_mode == Mode::kNormal) return;
        auto* partner = Partner();
        auto* source = SourceRef();   // merchant wares vs container/corpse
        if (!partner || !source) return;

        auto cells = CollectPartnerCells(source);        // stage 1
        const int placedRows = PlacePartnerCells(cells); // stage 2

        const char* title = partner->GetDisplayFullName();
        if (!title || !*title) {
            title = g_mode == Mode::kBarter ? "MERCHANT" : "CONTAINER";
        }

        const auto& sk = Theme::S();
        const float S    = Theme::Scale();
        const float cell = Grid::CellPx();
        const int   cols = Grid::kCols;
        // the measurement belongs to ONE skin at ONE scale: both change the
        // title bar's height and the padding, so a stale figure would size
        // the window for the skin the player just left
        {
            static float s_forScale = -1.0f;
            static int   s_forSkin = -1;
            const int    skin = Theme::SkinIndex();
            if (s_forScale != S || s_forSkin != skin) {
                s_forScale = S;
                s_forSkin = skin;
                g_partnerChromeH = -1.0f;
            }
        }
        // F7: spot-memory containers get +2 spare rows of arranging space
        // (fixed width, variable height per spec)
        // ★(1.3.3) a COMPANION's board is exactly its capacity -- 10 x 8, no
        // spare rows, so the pack the player may fill is the pack they see.
        // It still stretches when what is already inside overflows it (see
        // CompanionPartner): the cap governs additions, never visibility.
        const bool  companion = CompanionPartner();
        const int   rows = companion
                               ? (std::max)(kCompanionRows, placedRows)
                               : (std::max)(4, placedRows) + (SpotMemoryOn() ? 2 : 0);
        const int   visRows = (std::min)(rows, 12);   // scroll past 12 rows
        const float gridW = cols * cell;
        // reserve the scrollbar gutter ONLY when the list actually scrolls —
        // otherwise it leaves a permanent lopsided right margin (measured: the
        // right gap was insX + sbW while the left was just insX).
        const float sbW  = (rows > visRows) ? ImGui::GetStyle().ScrollbarSize : 0.0f;
        const float insX = Theme::FrameInsetX(), insY = Theme::FrameInsetY();
        const float barH = 26.0f * S, labelH = 24.0f * S;

        auto* wm = WinManager::GetSingleton();
        // width = grid + scrollbar gutter + symmetric frame insets (no lopsided
        // right margin). The child below is gridW + sbW so the 10 columns stay
        // full and the scrollbar sits in its own gutter, not over the tiles.
        // +30: bottom strip — barter = merchant GOLD bar (mirrors the player
        // window's), loot = shortcut hint line (design pass C/G)
        // ★+2 x PadX. TitleBar starts the content at PadX from the left edge
        // (every managed window does), so a width sized without it runs the
        // last cell column PadX past the right edge, where ImGui clips it.
        // Invisible on skins whose PadX is baked into a frame inset; on SIMPLE
        // (inset 0, PadX 8) the merchant's grid simply lost its right column.
        // ★Same clearance the player's window takes, paid for the same way --
        // the two sit side by side, so a pad on one title line and not the
        // other is visible as a step between them.
        const float topPad = Theme::TitleTopPad();
        // ★★(1.3.3) PadX()/PadY() ARE ALREADY SCALED (12 * g_scale). Multiplying
        // by S again made this window's padding S^2 -- the same fault the title
        // bar had in 1.2.1, invisible at the 1440p reference where S == 1 and
        // a growing gap everywhere else. The height's own 14 * S was a
        // hand-fitted stand-in for that padding and could only agree with it
        // by accident, which is the strip of empty grid under the board.
        // Both halves now name the SAME quantity, so the window is exactly
        // its content: title + label + rows + bottom strip + the padding.
        const float padX = Theme::PadX(), padY = Theme::PadY();
        // The chrome above the board -- title bar, section label, the window's
        // own top padding and the title's clearance -- MEASURED last frame
        // (see the note by EndChild). The first frame of a session uses the
        // old estimate and is corrected on the next one.
        // ★topPad is subtracted because ApplyNext adds it back: the measured
        // height already contains it (it is measured from the window's top).
        const float chromeH = g_partnerChromeH > 0.0f
                                  ? g_partnerChromeH
                                  : barH + labelH + 2.0f * padY;
        const ImVec2 size(gridW + sbW + 2.0f * (insX + padX),
                          chromeH + visRows * cell + 1.0f +
                              BottomStripH() + insY - topPad);
        ImVec2 defPos(120.0f, 200.0f);
        if (auto* m = wm->Find("main"); m && m->posKnown) {
            defPos = ImVec2(m->pos.x - size.x - 12.0f * S, m->pos.y);
        }
        wm->ApplyNext("partner", defPos, size, WinManager::Anchor::kTopLeft, topPad);
        // managed-window rule: WindowPadding must equal the frame inset, else
        // the content starts at the default padding (left) while the width was
        // sized from insX (right) — a lopsided right margin. (See memory note.)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(insX + padX, insY + padY));   // (1.3.3) no second scale
        ImGui::Begin("##gi_partner", nullptr, kManagedWinFlags);
        wm->TitleBar("partner", title);

        // F6a/F6b: red crime chrome — border + label line flip to crimson so
        // an owned container / a living mark is unmistakable.
        static constexpr ImU32 kStealRed = IM_COL32(168, 44, 32, 220);
        // ★★★NO RED WINDOW BORDER. It stroked a square 2px line where the SKIN
        // draws its own frame -- over a brush stroke on the ink skins, over a
        // torn edge on Fable, half-buried under the bevel elsewhere -- so it
        // read as a stray rectangle rather than as chrome. Moving it a
        // frame-inset inward did not help: it was still a second frame drawn in
        // a grammar no skin uses. The warning belongs to the LABEL, which is
        // where a player looks to find out what this window is.
        if (g_mode == Mode::kSteal || g_mode == Mode::kPickpocket) {
            // ★Same style as every other section label -- the skin decides
            // between a crimson diamond and outlined chrome -- with only the
            // COLOUR overridden. Drawing this one by hand is what left it
            // wearing the diamond on skins that had moved on from it.
            const ImVec4 warn = ImGui::ColorConvertU32ToFloat4(kStealRed);
            UIRoot::SectionLabel(Lang::T(g_mode == Mode::kSteal
                                             ? Lang::Str::StealTitle
                                             : Lang::Str::PickpocketTitle),
                                 &warn);
        } else {
            UIRoot::SectionLabel(g_mode == Mode::kLoot ? "CONTENTS" : "WARES");
        }
        // merchant gold moved to the bottom GOLD bar (design pass C) — the
        // player and partner windows now mirror each other's layout

        // (no NoScrollWithMouse — the wheel must scroll the item list)
        // +1px so the right/bottom grid lines (at exactly gridW / rows*cell)
        // aren't clipped away by the child's edge.
        ImGui::BeginChild("##partner_grid", ImVec2(gridW + sbW + 1.0f, visRows * cell + 1.0f),
            ImGuiChildFlags_None, ImGuiWindowFlags_None);
        const ImVec2 base = ImGui::GetCursorScreenPos();

        // ★★The BOARD's screen rect, published in EVERY mode. It used to be set
        // only under SpotMemoryOn because only the drop-cell maths read it --
        // but "is the cursor on the board?" is now the question that decides
        // whether a drop is a sale at all (see g_partnerHovered below), and a
        // merchant has to answer it too.
        g_partnerClipMin = ImGui::GetWindowPos();
        {
            const ImVec2 ws = ImGui::GetWindowSize();
            g_partnerClipMax = ImVec2(g_partnerClipMin.x + ws.x, g_partnerClipMin.y + ws.y);
        }

        // F7: publish this frame's grid geometry + placement for the
        // drop-cell math (QueryStoreDrop) — spot-memory modes only
        if (SpotMemoryOn()) {
            g_partnerGridLive = true;
            g_partnerBase = base;   // scroll-adjusted content origin
            g_partnerRows = rows;
            g_lastCells = cells;
        }

        DrawPartnerCells(cells, rows, gridW, sbW, base); // stage 3
        // ★★★TRIED AND REVERTED: a board-edge AddRect here, copied from
        // DrawGridChrome. It was never needed. Both lattices already close
        // their own boundary -- the hairline pass runs `c <= cols` / `r <=
        // rows` so the first and last lines ARE the edge, and the ink pass
        // strokes all four sides after its inner rules. The missing edge that
        // started this was the ink lattice not being called at all (fixed in
        // DrawPartnerCells), not a missing frame.
        // The rect only added faults of its own: anchored inside the child it
        // scrolled with the goods, and pulled out here it boxed in the
        // scrollbar -- a line under the bar on a merchant, which is what the
        // board's own edge never draws.
        ImGui::EndChild();

        // ★★(1.3.3) MEASURE THE CHROME, DO NOT GUESS IT. The bottom strip is
        // drawn at an ABSOLUTE offset from the window's bottom edge, so any
        // disagreement between the requested height and what the title, the
        // section label and the padding actually consume shows up as a band
        // of empty grid between the board and that strip -- which is what it
        // was. Three hand-fitted constants could only ever agree with the
        // real layout by luck, and did not on this skin/scale.
        // ★What is cached is the part that does NOT depend on the row count,
        // so it converges once and then holds: a value that never changes
        // cannot oscillate the window's size (the readback trap in
        // reference_imgui_pixel_traps).
        {
            const float used = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y;
            const float measured = std::round(used - (visRows * cell + 1.0f));
            if (measured > 0.0f && std::abs(measured - g_partnerChromeH) > 0.5f) {
                g_partnerChromeH = measured;
            }
        }

        DrawPartnerBottomBar(size, gridW);               // stage 4
        TakeAllShortcut(cells);                          // stage 5

        // record hover for the drag-to-store drop test (whole window + child)
        // ★AllowWhenBlockedByActiveItem, matching the player grid's gate. The
        // drop CLICK activates whatever ImGui item sits under the cursor, and
        // without this flag the window that owns it reports "not hovered" on
        // the one frame the drop is resolved. RootAndChildWindows because the
        // goods live in a scrolling child.
        //
        // ★★★...WHICH IS WHY THIS IS THE BOARD, NOT THE WINDOW. Every consumer
        // of this flag asks a DROP question (the three kPartner* routes, the
        // in-container rearrange, and two negative guards on player-grid drops),
        // and the contract they were written against is stated at the rearrange:
        // "Chrome (titlebar / bottom bar) cancels back to its spot."
        // The title bar used to honour it by accident -- its drag strip owned
        // ActiveId, so a plain IsWindowHovered went false there. Adding the flag
        // above took that accident away and a drop on the title bar SOLD the
        // item instead of cancelling. The rect test restores the contract on
        // purpose rather than by side effect.
        // ★MouseInOverlay for the other half: an overlay's chrome is drawn WIDER
        // than its ImGui rect, so z-order alone leaves a ~14px band where a
        // pickup is refused but a drop went through -- the same asymmetry the
        // player grid had, fixed there this release and still here.
        {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const bool onBoard = m.x >= g_partnerClipMin.x && m.x < g_partnerClipMax.x &&
                                 m.y >= g_partnerClipMin.y && m.y < g_partnerClipMax.y;
            g_partnerHovered =
                onBoard &&
                UIRoot::CursorOwnsWindow(ImGuiHoveredFlags_RootAndChildWindows);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ---- F7: container spot memory — public surface ----

    // ★(1.3.3) reads g_lastCells -- this frame's partner placement -- so it
    // lives down here with the other consumers of it.
    bool PartnerHasRoomFor(RE::TESBoundObject* a_obj, int a_count)
    {
        // only a living follower is bounded; chests, corpses and merchants
        // answer yes as they always have
        if (!a_obj || !CompanionPartner()) return true;
        // gold is weightless bookkeeping on both sides, and a stack that
        // MERGES needs no cell of its own
        if (a_obj->IsGold()) return true;
        const int cap = Grid::StackCap(a_obj);
        if (cap > 1) {
            int loose = 0;
            for (const auto& c : g_lastCells) {
                if (c.obj == a_obj) loose += c.count;
            }
            if (loose > 0 && (loose % cap) != 0 &&
                a_count <= cap - (loose % cap)) {
                return true;   // room inside a stack already on the shelf
            }
        }
        // ...otherwise it needs a free rect inside the 10 x kCompanionRows
        // pack, measured against the board the player is looking at.
        const auto d = Grid::ResolveDef(a_obj);
        const int w = (std::max)(1, d.w), h = (std::max)(1, d.h);
        const int cols = Grid::kCols;
        std::vector<char> occ(static_cast<std::size_t>(cols) * kCompanionRows, 0);
        for (const auto& c : g_lastCells) {
            if (c.col < 0) continue;
            for (int y = 0; y < c.h; ++y) {
                const int rr = c.row + y;
                if (rr < 0 || rr >= kCompanionRows) continue;
                for (int x = 0; x < c.w; ++x) {
                    const int cc = c.col + x;
                    if (cc < 0 || cc >= cols) continue;
                    occ[static_cast<std::size_t>(rr) * cols + cc] = 1;
                }
            }
        }
        for (int r = 0; r + h <= kCompanionRows; ++r) {
            for (int c = 0; c + w <= cols; ++c) {
                bool free = true;
                for (int y = 0; y < h && free; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (occ[static_cast<std::size_t>(r + y) * cols + c + x]) {
                            free = false;
                            break;
                        }
                    }
                }
                if (free) return true;
            }
        }
        return false;
    }

    StoreDrop QueryStoreDrop()
    {
        StoreDrop d;
        if (!g_partnerGridLive) return d;
        // ★★★The MIRROR of the player grid's fault, and it was here too: the
        // clip test below is pure geometry, so a bag window dragged on TOP of
        // the merchant still let a drop in the overlap land in the merchant's
        // board underneath. `g_partnerGridLive` says the board was drawn this
        // frame, not that it is the thing under the cursor.
        // ★This one cannot ask ImGui itself -- it is queried at drop time, long
        // after the partner's Begin/End scope closed -- so it reads the answer
        // the window recorded while it WAS current.
        if (!g_partnerHovered) return d;
        int hw = 1, hh = 1;
        float offX = 0.0f, offY = 0.0f;
        if (!Grid::HeldFootprint(hw, hh, offX, offY)) return d;
        const ImVec2 m = ImGui::GetIO().MousePos;
        // must be inside the VISIBLE grid child (the bottom bar / titlebar
        // share row coordinates once scrolled)
        if (m.x < g_partnerClipMin.x || m.x >= g_partnerClipMax.x ||
            m.y < g_partnerClipMin.y || m.y >= g_partnerClipMax.y) {
            return d;
        }
        // anchor = footprint top-left from the GRAB point, rounded and
        // clamped to the board — the player grid's C2 formula verbatim
        const float cell = Grid::CellPx();
        int c = static_cast<int>(std::lround((m.x - g_partnerBase.x - offX) / cell));
        int r = static_cast<int>(std::lround((m.y - g_partnerBase.y - offY) / cell));
        c = (std::max)(0, (std::min)(Grid::kCols - hw, c));
        r = (std::max)(0, (std::min)(g_partnerRows - hh, r));
        d.onCell = true;
        d.col = c;
        d.row = r;
        // footprint-overlap blockers (partner cells are plain rectangles):
        // none = free spot, exactly one = swap partner, 2+ = invalid
        int blockers = 0;
        for (const auto& pc : g_lastCells) {
            if (pc.col < 0 || Grid::IsHeldPartnerUnit(pc.obj, pc.uid, pc.xlIdx, pc.ord)) continue;
            if (c < pc.col + pc.w && c + hw > pc.col &&
                r < pc.row + pc.h && r + hh > pc.row) {
                if (++blockers == 1) {
                    d.occ = pc.obj;
                    d.occCount = pc.count;
                    d.occValue = pc.value;
                    d.occCol = pc.col;
                    d.occRow = pc.row;
                    d.occUid = pc.uid;        // GI24
                    d.occXlIdx = pc.xlIdx;
                    d.occOrd = pc.ord;
                    d.occSpotKey = pc.spotKey;
                    d.occRot = pc.rot;        // GI62
                }
            }
        }
        if (blockers == 0) {
            d.freeSpot = hw <= Grid::kCols && hh <= g_partnerRows;
        } else if (blockers > 1) {
            d.occ = nullptr;   // 2+ blockers: neither free nor a swap
        }
        return d;
    }

    void NoteStoreSpot(RE::TESBoundObject* a_obj, int a_col, int a_row,
                       std::uint16_t a_sig, int a_rot)
    {
        if (!a_obj || !SpotMemoryOn() || !Partner()) return;
        const RE::FormID form = a_obj->GetFormID();
        // one claim per (form, sig): a second drop of the same thing replaces
        // the first rather than queueing behind it
        for (auto& ps : g_pendingSpots) {
            if (ps.form == form && ps.sig == a_sig) {
                ps.col = a_col;
                ps.row = a_row;
                ps.slotKey = g_actingSpot;
                ps.rot = a_rot & 3;
                return;
            }
        }
        // GI23: g_actingSpot is still set here — the carry has not ended yet, so
        // this records exactly which slot is being moved (empty = a new arrival)
        g_pendingSpots.push_back({ form, a_sig, a_col, a_row, g_actingSpot, a_rot & 3 });
    }

    void NoteCarriedSpot(const std::string& a_spotKey)
    {
        g_actingSpot = a_spotKey;   // GI24: a swap starts a carry we did not click
    }

    void SetStoreSpotHint(RE::TESBoundObject* a_obj, int a_col, int a_row,
                          std::uint16_t a_sig, int a_rot)
    {
        g_storeHint = { a_obj, a_col, a_row, a_sig, a_rot & 3 };
    }

    // ---- F7: cosave 'GCLY' v1 ----
    // [u32 containers]{ u32 refID, u32 stamp, u32 spots,
    //                   { str key, i32 col, i32 row, i32 w, i32 h } }
    namespace
    {
        constexpr std::uint32_t kContMaxStr = 512;
        constexpr std::uint32_t kContMaxEntries = 65536;
        constexpr std::uint32_t kContCosaveVersion = 9;   // v2: per-spot rotation  v3: a stored pouch's gold  v4: a stored bag's bundle  v5: bundle anchors  v6: bundle rotation  v7: bundle markers  v8: bundle stolen flag  v9: the spot's binding hints

        // ★v9 migration: before this, a spot's binding lived inside its KEY
        // ("form~B825!worn#1"). Read it back out and put it where it belongs.
        // The key itself is left alone -- nothing recomputes it any more, so an
        // old one is simply an opaque spot name.
        void SpotHintsFromKey(const std::string& a_key, std::uint16_t& a_uid,
                              std::uint16_t& a_sig, bool& a_worn)
        {
            const auto bar = a_key.find('|');
            const auto from = bar == std::string::npos ? 0 : bar + 1;
            a_worn = a_key.find("!worn", from) != std::string::npos;
            const auto at = a_key.find('@', from);
            if (at != std::string::npos) {
                a_uid = static_cast<std::uint16_t>(
                    std::strtoul(a_key.c_str() + at + 1, nullptr, 16));
                return;
            }
            const auto tilde = a_key.find('~', from);
            if (tilde != std::string::npos) {
                a_sig = static_cast<std::uint16_t>(
                    std::strtoul(a_key.c_str() + tilde + 1, nullptr, 16));
            }
        }

        bool ContWriteStr(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const auto len = static_cast<std::uint32_t>(a_s.size());
            if (!a_intfc->WriteRecordData(len)) return false;
            return len == 0 || a_intfc->WriteRecordData(a_s.data(), len);
        }

        bool ContReadStr(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            std::uint32_t len = 0;
            if (!a_intfc->ReadRecordData(len) || len > kContMaxStr) return false;
            a_out.assign(len, '\0');
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len);
        }
    }

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kContRecordType, kContCosaveVersion)) {
            SKSE::log::error("[LOOT] cosave: OpenRecord GCLY failed");
            return;
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_contLayouts.size()));
        for (const auto& [refID, cl] : g_contLayouts) {
            a_intfc->WriteRecordData(refID);
            a_intfc->WriteRecordData(cl.stamp);
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(cl.spots.size()));
            for (const auto& [key, s] : cl.spots) {
                ContWriteStr(a_intfc, key);
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.col));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.row));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.w));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.h));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.rot));   // v2
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.gold));  // v3
                // v4: the stored bag's bundle (raw FormIDs; resolved on load)
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.bundle.size()));
                for (const auto& b : s.bundle) {
                    a_intfc->WriteRecordData(b.form);
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.count));
                    a_intfc->WriteRecordData(b.sig);
                    // v5: the entry's anchor inside the bag window
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.col));
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.row));
                    // v6: its quarter-turns
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.rot));
                    // v7: its marker bits
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.glow));
                    // v8: someone else's goods
                    a_intfc->WriteRecordData(static_cast<std::int32_t>(b.stolen ? 1 : 0));
                }
                // ★v9: which unit this spot is showing. A hint, not a name:
                // a stale one only weakens the next match, and the fallback
                // keeps the spot (and the gold and the bundle riding on it).
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.uid));
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.sig));
                a_intfc->WriteRecordData(static_cast<std::int32_t>(s.worn ? 1 : 0));
            }
        }
        SKSE::log::info("[LOOT] cosave: saved {} container layouts", g_contLayouts.size());
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        if (a_version < 1 || a_version > kContCosaveVersion) {
            SKSE::log::warn("[LOOT] cosave: unsupported container record v{} (max v{}) — skipped",
                            a_version, kContCosaveVersion);
            return;
        }
        std::unordered_map<RE::FormID, ContLayout> loaded;
        std::uint32_t nCont = 0;
        if (!a_intfc->ReadRecordData(nCont) || nCont > kContMaxEntries) return;
        std::uint32_t maxStamp = 0;
        for (std::uint32_t i = 0; i < nCont; ++i) {
            RE::FormID refID = 0;
            std::uint32_t stamp = 0, nSpots = 0;
            if (!a_intfc->ReadRecordData(refID) || !a_intfc->ReadRecordData(stamp) ||
                !a_intfc->ReadRecordData(nSpots) || nSpots > kContMaxEntries) {
                return;   // truncated: bail clean (partial data stays unused)
            }
            // load-order shift: resolve the ref id; unresolvable containers
            // are PRUNED (their spots are read past regardless)
            RE::FormID resolved = 0;
            const bool ok = a_intfc->ResolveFormID(refID, resolved);
            ContLayout cl;
            cl.stamp = stamp;
            for (std::uint32_t j = 0; j < nSpots; ++j) {
                std::string key;
                std::int32_t col = 0, row = 0, w = 1, h = 1, rot = 0;
                if (!ContReadStr(a_intfc, key) || !a_intfc->ReadRecordData(col) ||
                    !a_intfc->ReadRecordData(row) || !a_intfc->ReadRecordData(w) ||
                    !a_intfc->ReadRecordData(h)) {
                    return;
                }
                if (a_version >= 2) {   // GI62 (older saves: everything upright)
                    if (!a_intfc->ReadRecordData(rot)) return;
                }
                std::int32_t gold = 0;
                if (a_version >= 3) {   // a stored pouch's own gold
                    if (!a_intfc->ReadRecordData(gold)) return;
                }
                std::vector<BundleItem> bundle;
                if (a_version >= 4) {   // a stored bag's bundle
                    std::uint32_t nb = 0;
                    if (!a_intfc->ReadRecordData(nb) || nb > kContMaxEntries) return;
                    bundle.reserve(nb);
                    for (std::uint32_t bi = 0; bi < nb; ++bi) {
                        RE::FormID    f = 0;
                        std::int32_t  bc = 0;
                        std::uint16_t bs = 0;
                        if (!a_intfc->ReadRecordData(f) || !a_intfc->ReadRecordData(bc) ||
                            !a_intfc->ReadRecordData(bs)) {
                            return;
                        }
                        std::int32_t bcol = -1, brow = -1, brot = 0;
                        if (a_version >= 5) {   // bundle anchors
                            if (!a_intfc->ReadRecordData(bcol) ||
                                !a_intfc->ReadRecordData(brow)) {
                                return;
                            }
                        }
                        if (a_version >= 6) {   // bundle rotation
                            if (!a_intfc->ReadRecordData(brot)) return;
                        }
                        std::int32_t bglow = 0;
                        if (a_version >= 7) {   // bundle markers
                            if (!a_intfc->ReadRecordData(bglow)) return;
                        }
                        std::int32_t bstolen = 0;
                        if (a_version >= 8) {   // someone else's goods
                            if (!a_intfc->ReadRecordData(bstolen)) return;
                        }
                        // load-order shift: unresolvable contents are dropped
                        // (their engine items simply stay visible on the shelf)
                        RE::FormID rf = 0;
                        if (a_intfc->ResolveFormID(f, rf) && rf != 0 && bc > 0) {
                            bundle.push_back({ rf, bc, bs, bcol, brow, brot & 3,
                                               static_cast<std::uint8_t>(bglow),
                                               bstolen != 0 });
                        }
                    }
                }
                ContSpot sp{ col, row, w, h, rot & 3, gold, std::move(bundle) };
                if (a_version >= 9) {
                    std::uint32_t su = 0, ss = 0;
                    std::int32_t  sw = 0;
                    if (!a_intfc->ReadRecordData(su) || !a_intfc->ReadRecordData(ss) ||
                        !a_intfc->ReadRecordData(sw)) {
                        return;
                    }
                    sp.uid  = static_cast<std::uint16_t>(su);
                    sp.sig  = static_cast<std::uint16_t>(ss);
                    sp.worn = sw != 0;
                } else {
                    SpotHintsFromKey(key, sp.uid, sp.sig, sp.worn);
                }
                cl.spots[std::move(key)] = std::move(sp);
            }
            if (ok && resolved != 0) {
                maxStamp = (std::max)(maxStamp, stamp);
                loaded[resolved] = std::move(cl);
            }
        }
        g_contLayouts = std::move(loaded);
        g_contStamp = maxStamp;
        SKSE::log::info("[LOOT] cosave: loaded {} container layouts", g_contLayouts.size());
    }

    void RevertGame()
    {
        g_contLayouts.clear();
        g_contStamp = 0;
        g_storeHint = {};
        g_pendingSpots.clear();   // GI18
        g_pendingBundles.clear();   // (1.3.0-D) bundles from the previous save
        g_incomingBundles.clear();
        g_partnerGridLive = false;
        g_lastCells.clear();
    }
}
