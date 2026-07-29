#pragma once

#include "ui/ItemDef.h"
#include "ui/Theme.h"

#include <functional>
#include <string>

namespace FUI::Grid
{
    // GridDef is an alias of the ONE shared FUI::ItemDef (Phase 2 D2) —
    // resolved by main.cpp through the ini-override -> category-preset path.
    using DefResolver = std::function<GridDef(RE::TESBoundObject*)>;
    void SetDefResolver(DefResolver a_resolver);

    // Resolve an item's footprint def via the registered resolver (same path
    // the main grid uses). LootBarter reuses this so the partner window draws
    // items at their real size (sword 1x3, helmet 2x2) instead of 1x1.
    [[nodiscard]] GridDef ResolveDef(RE::TESBoundObject* a_obj);

    // Game-side actions (main.cpp wires these):
    //  sound(obj, up): vanilla per-item pick-up (up=true) / put-down sound
    //  dropToWorld(obj, count, xl): RemoveItem(kDropping) at the player's feet;
    //  count > 0 drops that many, <= 0 drops the whole stack.
    //  GI25: xl names the sub-stack to drop (nullptr = engine's choice, which is
    //  correct only for units that carry no extras at all).
    void SetGameCallbacks(std::function<void(RE::TESBoundObject*, bool)> a_sound,
                          std::function<void(RE::TESBoundObject*, int,
                                             RE::ExtraDataList*)> a_dropToWorld);

    // Click-carry state (C1~C7). ESC/right-click cancels; the menu's Cancel
    // handler must check IsHolding() before closing (A2).
    [[nodiscard]] bool IsHolding();
    void CancelHold();

    // Phase 5-B: the partner item currently carried on the cursor (null if the
    // held item isn't from a partner). The partner window hides it so its source
    // cell reads as empty while carried.
    [[nodiscard]] RE::TESBoundObject* HeldPartnerObject();

    // GI17: the same question narrowed to ONE sub-stack. Partner windows must
    // use this -- with several cells per form, the form-level test hides them
    // all the moment one is lifted.
    [[nodiscard]] bool IsHeldPartnerUnit(RE::TESBoundObject* a_obj,
                                         std::uint16_t a_uid, int a_xlIdx, int a_ord);

    // GI30: the favourite of this unit's pool is currently worn (its tile is
    // parked without a cell). The doll shows the star meanwhile.
    [[nodiscard]] bool IsPoolStarWorn(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                                      std::uint16_t a_sig);

    // GI18: content signature of the carried unit (0 = nothing, or a plain
    // unit). Survives a container move, unlike an ordinal or a uniqueID, so a
    // drop position recorded before the engine transfer can still be matched to
    // the cell that appears afterwards.
    [[nodiscard]] std::uint16_t HeldInstanceSig();

    // F7: carried footprint (cells) + grab offset (px) for the partner
    // grid's drop ghost / drop-cell math. False when nothing is carried.
    bool HeldFootprint(int& a_w, int& a_h, float& a_offX, float& a_offY);

    // v9.2: start carrying an item that is NOT in the grid (equipment doll
    // pickup — the unequip runs deferred, the carry starts immediately).
    // GI25: a_uid/a_sig identify the sub-stack being lifted (the doll's pickup
    // is the WORN one, which is usually the tempered or enchanted copy).
    // a_swappedOut: this unit was DISPLACED by a drop onto an occupied slot, so
    // the engine unequips it as part of that same equip. It stops counting as
    // worn the moment the equip lands, not when an unequip of its own runs.
    void BeginCarry(RE::TESBoundObject* a_obj, std::uint16_t a_uid = 0,
                    std::uint16_t a_sig = 0, int a_hand = 0,
                    bool a_swappedOut = false);

    // Phase 5-B: carry a PARTNER (merchant/container) item on the cursor.
    // Dropping it onto the player grid takes (loot) or buys (barter).
    // a_value = base value per unit (for buy pricing).
    // a_offX/a_offY: grab offset px within the footprint (F7: pick up where
    // clicked, like player tiles); negative = centre (swap pickups).
    // D4: a_uid/a_xlIdx = the partner sub-stack picked up, so the eventual
    // take/buy moves THAT unit and not whichever one the engine fancies.
    void BeginPartnerCarry(RE::TESBoundObject* a_obj, int a_count, int a_value,
                           float a_offX = -1.0f, float a_offY = -1.0f,
                           std::uint16_t a_uid = 0, int a_xlIdx = -1, int a_ord = 0);

    // Deferred rebuild (safe to request mid-draw; runs at FinishFrame).
    void RequestRebuild();

    // Light path for rotation/scale edits: re-resolve defs + queue captures
    // WITHOUT re-placing the grid (footprints unchanged — no reflow, no IO).
    void RefreshDefs();

    // D3: equipping forgets the saved grid spot (fresh-pickup re-entry).
    // Rule 13: forget ONE tile's remembered cell (equip / sell / store / drop).
    void ForgetTile(const std::string& a_key);

    // Hide a unit whose EQUIP is queued for the next Tick, so it does not
    // flicker back into the cell it just left. NOT NotePendingRemove: that
    // resolves on a stock-count drop, which equipping never causes.
    // a_srcKey: the board tile the unit left from ("" if it came off the doll).
    // The pool holds that cell open until the engine applies the equip.
    void NotePendingEquip(RE::TESBoundObject* a_obj, std::uint16_t a_uid,
                          std::uint16_t a_sig, int a_hand = 0,
                          const std::string& a_srcKey = {});
    // GI32: apply queued favourite toggles. MUST run on the game thread --
    // the native SetFavorite refuses to add a second favourite from inside the
    // render pass.
    void ProcessFavorites();
    void ClearPendingEquips();   // menu close / reset
    // Equip::ProcessPending: the engine has run the queue. The next
    // rebuild consumes these and stops suppressing them.
    void MarkEquipsApplied();

    // C6: Equip::DrawSlot reports the hovered slot while an item is carried;
    // a left-click that frame tries to equip onto it.
    void NotifySlotDropTarget(const std::string& a_slotId);

    // Collect the player's inventory, place items (saved spots -> first-fit,
    // grid seniority), persist new placements, queue icon captures.
    void Rebuild();

    // Capacity system: true when a_obj could be added right now — it stacks
    // onto an existing tile, or its footprint first-fits into the HARD
    // 10 x 14 main board after every current occupant placed (bag-assigned
    // items consume their bag, not main). Headless: no UI, no persistence.
    [[nodiscard]] bool CanFitNewItem(RE::TESBoundObject* a_obj);

    // Phase 7: how many units (<= a_want) the inventory can accept right now —
    // partial-stack room + new tiles on the hard board + new tiles in open
    // bags. Stack buy/take sliders clamp their max to this.
    [[nodiscard]] int MaxAcceptUnits(RE::TESBoundObject* a_obj, int a_want);

    // Post-add capacity check (container-take bounce): the item is ALREADY in
    // the inventory — place everything on the hard board and report whether
    // a_obj's own tile failed to fit (stacking onto an owned tile never does).
    [[nodiscard]] bool WouldOverflow(RE::TESBoundObject* a_obj);

    // W1+W2: capacity-based encumbrance. Weight never limits the player (big
    // CarryWeight buffer); instead, when the hard board can't hold everything
    // (shop / console / scripted AddItem bypass the pickup gate), CarryWeight
    // is pushed below the inventory weight so the VANILLA over-encumbrance
    // (forced walk, no fast travel) kicks in.
    [[nodiscard]] bool IsOverloaded();   // S2 reads this for the crimson space value
    void MarkCapacityDirty();            // inventory/equip/loadout changed — recompute
    void CapacityTick();                 // per-frame: recompute when dirty + enforce CW

    // S2: stats panel "Space X / Y" — used cells on the main board (from the
    // last Rebuild; can exceed the total while overloaded) and the hard cap.
    [[nodiscard]] int SpaceUsed();
    [[nodiscard]] int SpaceTotal();
    [[nodiscard]] int BagFreeCells();   // B: free cells across open bags (take-all budget)
    [[nodiscard]] int CellSpanOf(RE::TESBoundObject* a_obj);   // grid cells an item occupies
    // shift+lclick split -> held fragment. a_srcKey = the tile it leaves.
    // a_srcTotal (coins only) = the source tile's full gold value, so an AUTO
    // coin tile is converted whole-to-pin before the split (siblings untouched).
    void PickupPartial(RE::TESBoundObject* a_obj, int a_count,
                       const std::string& a_srcKey, int a_srcTotal = 0);

    // Draw the main tetris grid inside the current ImGui window.
    void Draw();

    [[nodiscard]] int GoldAmount();   // v9: UIRoot draws the GOLD bar

    // B: report gold spent by a barter purchase this frame. The next Rebuild's
    // spill pass adds back the coin tiles the payment dissolved, so the bought
    // item spills into a bag instead of reusing the freed cells.
    void NotePaidGold(int a_price);

    // Phase 7: sold/stored units whose engine removal is still queued on the
    // transfer Tick. The rebuild subtracts them immediately and drains the
    // NAMED tile in place (a_key may be empty for carried fragments). Clear is
    // called right after the engine RemoveItem lands; ClearAll on window close.
    void NotePendingRemove(RE::TESBoundObject* a_obj, const std::string& a_key, int a_count);
    void ClearPendingRemove(RE::TESBoundObject* a_obj, int a_count);
    void ClearAllPendingRemoves();
    // B2: expire the partner-drop placement hint (qty slider cancelled/closed)
    void ClearDropHint();

    // I1: rich tooltip — name + damage/armor/effects + weight/value.
    // Shared by grid tiles and equip slots; call while the item is hovered.
    // a_coinValue >= 0 adds a highlighted gold line (G2: coin tiles show the
    // amount they represent; the pouch shows its stored amount).
    // a_price >= 0 (Phase 4) adds a barter price line — Buy (a_isBuy) or Sell.
    //
    // GI1/D1: a_owner names WHOSE inventory the per-instance extras come from.
    // It used to be hard-wired to the player, so hovering a merchant's ordinary
    // steel sword printed the poison / charge / temper / enchant of the player's
    // OWN steel sword. nullptr keeps the player default.
    //
    // a_scope names WHICH sub-stack of that entry the tooltip describes:
    //   kUnit — exactly the one at (a_uid, a_xlIdx): a player grid tile, one unit
    //   kWorn — the sub-stack on the body: the equipment doll
    //   kAny  — the first sub-stack carrying each trait: aggregate partner cells,
    //           which are one cell per FORM with a stack badge (by design)
    enum class ExtraScope { kUnit, kWorn, kAny };

    void DrawItemTooltip(RE::TESBoundObject* a_obj, int a_count, int a_coinValue = -1,
                         int a_price = -1, bool a_isBuy = false,
                         RE::TESObjectREFR* a_owner = nullptr,
                         ExtraScope a_scope = ExtraScope::kAny,
                         std::uint16_t a_uid = 0, int a_xlIdx = -1,
                         // kWorn only: WHICH worn unit. With one copy in each
                         // hand "the worn list of this form" is ambiguous, so the
                         // doll passes the identity its slot recorded.
                         std::uint16_t a_sig = 0, int a_hand = 0);

    // Item base value for barter pricing (InventoryEntryData::GetValue cached
    // at Rebuild). -1 if unknown / not a priceable item.
    [[nodiscard]] int ItemValue(RE::TESBoundObject* a_obj);

    // Rarity glow, shared with the partner (loot/barter) window so its items
    // glow exactly like the player grid's. GlowBits: bit1 = enchanted (EITM or
    // crafted ExtraEnchantment), bit2 = unique (DESC, cached). DrawGlow paints
    // the tinted silhouette halo mapped onto the DRAWN icon rect (radial
    // across the cell box as the style-0 / no-sprite fallback).
    // GI1/D2: a_xl names the SUB-STACK being drawn -- pass nullptr for a plain
    // unit (one with no ExtraDataList of its own) or when there is no entry.
    // Before GI1 this scanned the whole entry, so a single enchanted sword in a
    // stack of three made all three glow.
    [[nodiscard]] std::uint8_t GlowBits(RE::TESBoundObject* a_obj,
                                        RE::InventoryEntryData* a_entry,
                                        RE::ExtraDataList* a_xl);

    // GI1: one entry's units, in a stable order, each bound to the sub-stack it
    // belongs to. uid = ExtraUniqueID (0 when the engine assigned none),
    // xlIdx = position in entry->extraLists (-1 = a plain unit with no list).
    struct UnitRef
    {
        std::uint16_t uid = 0;     // ExtraUniqueID, 0 = the engine assigned none
        std::uint16_t sig = 0;     // GI14 content signature, 0 = a plain unit
        int           xlIdx = -1;  // position in entry->extraLists, -1 = plain
        // GI41: what the WALK knew and used to throw away. Asking again later
        // means asking by xlIdx, and a position stops being true the moment a
        // list is added or removed -- planting an item on a pickpocket mark
        // moved the "worn" answer onto a different cell, so the lock jumped to
        // an item the target was not wearing. Carry it instead.
        bool          worn = false;
        int           hand = 0;    // 1 right, 2 left (0 = not worn)
    };

    // Walk an entry into per-unit refs. a_skipWorn=false keeps the body-worn
    // unit (corpses and pickpocket targets show what the NPC wears).
    void EnumerateUnits(RE::InventoryEntryData* a_entry, int a_count,
                        std::vector<UnitRef>& a_out, bool a_skipWorn = true);

    // Units a single tile/cell may hold: 1 for gear and coins, else the
    // category cap (with the editor's per-item stack:N override applied).
    [[nodiscard]] int StackCap(RE::TESBoundObject* a_obj);

    // The worn sub-stack of an entry (equipment doll: the unit on the body).
    // a_hand: 0 = either, 1 = RIGHT (ExtraWorn), 2 = LEFT (ExtraWornLeft). The
    // hand is what separates two copies of one form worn at the same time.
    [[nodiscard]] RE::ExtraDataList* WornExtraOf(RE::InventoryEntryData* a_entry,
                                                 int a_hand = 0);
    [[nodiscard]] RE::ExtraDataList* WornExtraMatching(RE::InventoryEntryData* a_entry,
                                                       std::uint16_t a_uid,
                                                       std::uint16_t a_sig,
                                                       int a_hand = 0);

    // GI1: the engine's OWN entry for a form in a container (GetInventory hands
    // out copies, so anything mutated through those is silently discarded).
    [[nodiscard]] RE::InventoryEntryData* LiveEntryOf(RE::TESObjectREFR* a_owner,
                                                      RE::TESBoundObject* a_obj);

    // GI1: the sub-stack named by (uid, xlIdx). uid wins when the engine
    // assigned one; otherwise the recorded position, revalidated against the
    // CURRENT list. nullptr = a plain unit, or that instance is gone.
    // NEVER store the result -- the engine reallocates and frees these.
    [[nodiscard]] RE::ExtraDataList* ExtraForInstance(RE::InventoryEntryData* a_entry,
                                                      std::uint16_t a_uid, int a_xlIdx);

    // GI25: resolve by POOL (uid, else content signature) and never by list
    // position. Transfers must use this: they are queued and run a frame or more
    // later, by which time a captured position can be stale.
    [[nodiscard]] RE::ExtraDataList* ExtraForPool(RE::InventoryEntryData* a_entry,
                                                  std::uint16_t a_uid, std::uint16_t a_sig);

    // GI36: resolve the sub-stack that is really LEAVING the bag and strip its
    // favourite star in the same call. Outbound sinks (sell / store / plant /
    // trash / world drop) call this instead of ExtraForPool and pass the result
    // straight to RemoveItem. Keeping resolution and star-removal together is
    // the point: clearing by pool name stripped every look-alike's star.
    //   a_starred = how many of the a_count outgoing units wore a star (0 = none).
    //               The caller always knows this; the sink never can.
    // Returns nullptr for "let the engine pick", only within a pool whose members
    // are genuinely interchangeable.
    [[nodiscard]] RE::ExtraDataList* ResolveExitUnit(RE::TESBoundObject* a_obj,
                                                     std::uint16_t a_uid,
                                                     std::uint16_t a_sig,
                                                     int a_count, int a_starred);

    // GI42: how a transfer NAMES the unit it moves. RemoveItem's a_extraList is
    // the only selector the engine has, and nullptr is not a fallback -- it is
    // "engine, pick one". That is only legal when every unit the engine could
    // pick is interchangeable with the one that was clicked, under both content
    // (sig/uid) and the rules in force (may a worn unit leave the body?).
    // The pickpocket lock bypass came from conflating "could not resolve" with
    // "no list, any unit is fine" -- both were nullptr.
    enum class PickKind : std::uint8_t
    {
        kNamed,       // this exact ExtraDataList
        kAnyIsSafe,   // no list: every candidate identical -- nullptr is PROVEN safe
        kFallback,    // nullptr, unproven: content look-alikes exist (tolerated + logged)
        kUnresolved   // a WORN unit the rules forbid could be grabbed -- do not move
    };
    struct UnitChoice
    {
        PickKind           kind = PickKind::kUnresolved;
        RE::ExtraDataList* xl = nullptr;
        [[nodiscard]] bool ok() const { return kind != PickKind::kUnresolved; }
    };
    // The proof, three lines: any worn list + illegal => kUnresolved; any list
    // whose content differs from a plain unit => kFallback; else kAnyIsSafe.
    // Needs NO knowledge of the engine's selection order -- that is the point.
    //   a_nameWorn : may the worn list be NAMED (only for a request from a worn cell)
    //   a_wornLegal: may a worn unit legally leave (loot/steal, or perk in pickpocket)
    [[nodiscard]] UnitChoice PoolChoice(RE::InventoryEntryData* a_entry,
                                        std::uint16_t a_uid, std::uint16_t a_sig,
                                        bool a_nameWorn, bool a_wornLegal);

    // GI43: the ENGINE's own price for ONE unit. A throwaway stack entry points
    // at the unit's REAL list and is priced by the same native the vanilla
    // barter row uses, so temper folds in exactly like vanilla (10 -> 11 at
    // +10%) with no formula of ours. The single-element BSSimpleList lives
    // entirely on the stack (head node is embedded -- no allocation) and the
    // list is DETACHED before the entry is destroyed, so no engine-owned
    // memory is ever touched by a destructor of ours.
    // a_xl == nullptr (a plain unit / an aggregate) falls back to the base value.
    [[nodiscard]] int UnitValueWith(RE::TESBoundObject* a_obj, RE::ExtraDataList* a_xl);

    // Same, for a NON-PLAYER source (container / corpse / mark / merchant). The
    // player-side resolver refuses worn lists on purpose -- it must never hand a
    // sell or a trash the item in your hand. The partner board deliberately SHOWS
    // worn gear, though, so a take there has to be able to name it; otherwise the
    // resolver answers nullptr, the engine chooses, and looting an equipped sword
    // can move an unequipped copy instead.
    [[nodiscard]] RE::ExtraDataList* ExtraForPoolOnPartner(RE::InventoryEntryData* a_entry,
                                                           std::uint16_t a_uid,
                                                           std::uint16_t a_sig);

    // Content signature of a sub-stack (0 = none). Stable across container moves.
    [[nodiscard]] std::uint16_t InstanceSigOf(RE::ExtraDataList* a_xl);
    void DrawGlow(ImDrawList* a_dl, RE::TESBoundObject* a_obj, std::uint8_t a_bits,
                  const ImVec2& a_iconMin, const ImVec2& a_iconMax,
                  const ImVec2& a_boxMin, const ImVec2& a_boxMax);

    // Phase 2 shared cell renderer: count/value badge hugging the tile's
    // top-left corner, full black outline (player tiles + partner grid).
    void DrawCountBadge(ImDrawList* a_dl, const ImVec2& a_tileMin, const char* a_text);

    // G2: coin-pouch withdraw window (slider) — top level, settings pattern.
    void DrawPouchWindow();
    bool ClosePouch();   // I/ESC layering: close the withdraw window if open

    // F2: trash window — a 6x4 virtual bag view ("__trash") holding items
    // PARKED for deletion. Parked items stay in the engine inventory and
    // occupy no board space; deletion is confirmed when the window (or the
    // whole menu) closes, oldest-first when the board needs room (FIFO).
    [[nodiscard]] bool IsTrashOpen();
    void ToggleTrash();          // the trash-can button
    bool CloseTrash();           // I/ESC layering: confirm-all + close if open
    bool CloseTrashConfirm();    // I/ESC layering: dismiss the favorite ask
    void DrawTrashConfirm();     // favorite-intake confirm popup (top level)
    void ProcessTrashDeletes();  // UIRoot::Tick — engine RemoveItem, deferred

    // Draw one managed window per OPEN bag (call from UIRoot::Render after
    // the main window). E1~E5: multi-open, remembered, fixed bw x bh grids.
    void DrawBagWindows();

    // End-of-frame: held-item cursor icon, drop/swap/discard input, deferred
    // rebuilds. Call after every grid/window drew, before WinManager::Update.
    void FinishFrame();

    // Per-save persistence of placements + open bags (SKSE cosave). The global
    // layout ini caused cross-save contamination (any load showed the LAST
    // session's arrangement); it remains only as a one-shot legacy fallback
    // for saves that predate the 'GLAY' record. main.cpp owns the record loop
    // and dispatches by type.
    inline constexpr std::uint32_t kRecordType = 'GLAY';
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
    void MarkLayoutFresh();   // kNewGame: start empty, skip the legacy-ini fallback

    // Grid pixel metrics (main window sizes itself around these).
    inline constexpr int   kCols    = 10;
    inline constexpr int   kMinRows = 14;
    inline constexpr float kCell    = 48.0f;   // base cell at UI scale 1.0

    // H′: every layout metric goes through the global UI scale.
    [[nodiscard]] inline float CellPx() { return kCell * Theme::Scale(); }
}
