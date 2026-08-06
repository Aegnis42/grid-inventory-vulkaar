#pragma once

#include <string>

// PLAN_LOOT_BARTER — the partner window (container / merchant) that opens
// ALONGSIDE the player grid, plus the global UI mode that the player-side
// right-click handler branches on so looting/bartering never collides with
// the normal equip action.
//
// Phase 1: mode state, vanilla-menu interception glue, and an empty docked
// partner window. Item display (Phase 2) and item movement (Phase 3/5) build
// on this scaffold.

namespace FUI::LootBarter
{
    enum class Mode
    {
        kNormal,   // plain inventory — right-click equips
        kLoot,     // container / corpse / companion — right-click takes/stores
        kBarter,   // merchant — right-click buys/sells
        kSteal,    // F6a: owned container — loot behaviour, takes use the
                   // engine's kSteal removal (crime/stolen handled for us)
        kPickpocket// F6b: living target — every move rolls the vanilla
                   // pickpocket formula (AttemptPickpocket); a failed roll
                   // raises the engine's crime response and force-closes
    };

    [[nodiscard]] Mode CurrentMode();

    // kSteal behaves exactly like kLoot everywhere except the removal reason
    // and the red STEAL chrome — every "is this the loot flow?" check goes
    // through here so the two can never drift apart.
    [[nodiscard]] inline bool IsLootMode(Mode a_mode)
    {
        return a_mode == Mode::kLoot || a_mode == Mode::kSteal;
    }

    // Enter loot/barter mode with the container/merchant reference. Called
    // from the menu-open interception (main.cpp) as the vanilla menu is
    // swallowed and our grid is opened. Stored as a handle internally.
    void Enter(Mode a_mode, RE::TESObjectREFR* a_partner);

    // Back to kNormal — called when our grid menu closes (UIRoot::OnClose).
    void Reset();

    // The container/merchant reference, resolved & null-checked (null in
    // kNormal or if the handle went stale).
    [[nodiscard]] RE::TESObjectREFR* Partner();

    // Draw the partner-side window(s). Called from UIRoot::Render inside the
    // ImGui frame. No-op in kNormal.
    void DrawWindows();

    // True if the mouse is over the partner window this frame (drag-to-store
    // target). Valid after DrawWindows ran; false in kNormal.
    [[nodiscard]] bool IsPartnerHovered();

    // ---- item transfer (loot mode) ----------------------------------------
    // Queued from the render pass, applied on Tick (game thread) — engine
    // inventory moves must not run mid-frame.
    // D4: a_uid/a_xlIdx name WHICH sub-stack moves (0/-1 = engine picks, still
    // correct for fungible goods). partner -> player
    // a_fromWorn: the cell came from the actor'''s BODY. A worn unit with no
    // signature has no pool handle at all, so this is the only way to say
    // "the equipped one, not a spare".
    void RequestTake(RE::TESBoundObject* a_obj, int a_count,
                     std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                     bool a_fromWorn = false);
    void RequestStore(RE::TESBoundObject* a_obj, int a_count,
                      std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                      bool a_fav = false);   // player -> partner (GI36: a_fav)
    // barter (Phase 5): item move + gold settlement + speech xp, all on Tick.
    // a_baseTotal = total BASE value of the goods — vanilla speech XP points
    // (the haggled price doesn't matter for XP).
    void RequestBuy(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal = 0,
                    std::uint16_t a_uid = 0, std::uint16_t a_sig = 0);   // merchant -> player
    void RequestSell(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal = 0,
                     std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                     bool a_fav = false);  // player -> merchant (GI36: a_fav)
    // F6b: pickpocket moves — each rolls PlayerCharacter::AttemptPickpocket
    // on the Tick (crime / XP / detection handled by the engine); a failed
    // roll force-closes the menu, already-succeeded moves stay.
    void RequestPickTake(RE::TESBoundObject* a_obj, int a_count,                      // target -> player
                         std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                         bool a_fromWorn = false);
    void RequestPickStore(RE::TESBoundObject* a_obj, int a_count,                       // player -> target
                          std::uint16_t a_uid, std::uint16_t a_sig,
                          const std::string& a_srcKey = {},
                          bool a_fav = false);                                          // (reverse-pickpocket)
    void ProcessTransfers();   // UIRoot::Tick

    // Quantity slider (Shift+right-click on a stack). Opens a small popup to
    // pick how many to move; confirm queues the transfer.
    enum class XferDir { kTake, kStore, kPickup, kBuy, kSell,    // kPickup = split onto cursor
                         kPickTake, kPickStore };                // F6b pickpocket rolls
    // a_srcKey (kPickup only): the grid tile the split quantity is taken from.
    // a_unitValue (kBuy/kSell only): the item's base value per unit, so the
    // confirmed quantity is priced (buy/sell price * count).
    // GI25: a_uid/a_sig name the POOL the units come from, so the deferred
    // confirm still moves the right sub-stack.
    void OpenSlider(RE::TESBoundObject* a_obj, int a_max, XferDir a_dir,
                    const std::string& a_srcKey = {}, int a_unitValue = 0,
                    std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                    bool a_worn = false,    // a_worn: the cell came off the body
                    bool a_fav = false);    // GI36: the cell wore a star
    void DrawSlider();   // UIRoot::Render (top level)
    [[nodiscard]] bool SliderActive();

    // Phase 5: favorite-sale confirm popup. a_baseTotal = speech XP points.
    void AskSellConfirm(RE::TESBoundObject* a_obj, int a_count, int a_price, int a_baseTotal = 0,
                        const std::string& a_srcKey = {},
                        std::uint16_t a_uid = 0, std::uint16_t a_sig = 0,
                        bool a_fav = true);   // GI25 / GI36 (this popup only fires for a star)
    void DrawConfirm();   // UIRoot::Render (top level)
    [[nodiscard]] bool ConfirmActive();

    // I/ESC layering: close the topmost open popup (confirm > slider);
    // returns false when neither is open (the caller may close more UI)
    // true while EITHER sub-popup is up (confirm sits above the slider). The
    // close-order stack treats this module as one layer and lets CloseTopPopup
    // keep its own internal order.
    [[nodiscard]] bool IsPopupOpen();
    bool CloseTopPopup();

    // ---- barter pricing (Phase 4) -----------------------------------------
    // UESP formula: factor = fBarterMax - (fBarterMax-fBarterMin)*min(speech,100)/100
    // buy  = round(value * buyMod  * factor)   (player buys FROM the merchant)
    // sell = round(value * sellMod / factor)   (player sells TO the merchant)
    // buyMod/sellMod = perk coefficients (Haggling/Allure) from HandleEntryPoint
    // (kModBuyPrices/kModSellPrices); 1.0 with no perks -> pure speech factor.
    [[nodiscard]] int BuyPrice(RE::TESBoundObject* a_item, int a_baseValue);
    [[nodiscard]] int SellPrice(RE::TESBoundObject* a_item, int a_baseValue);
    // B7: stack totals round ONCE on the total (per-unit rounding x count
    // inflated cheap bulk buys / deflated bulk sells); floor 1 gold per unit
    [[nodiscard]] int BuyPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count);
    [[nodiscard]] int SellPriceTotal(RE::TESBoundObject* a_item, int a_unitValue, int a_count);
    [[nodiscard]] int MerchantGold();   // SourceRef()'s Gold001 count; 0 outside kBarter

    // F3 (settings): unlimited merchant gold. While ON, MerchantGold() reports
    // INT_MAX so every "can the merchant pay?" check passes; the bottom bar
    // shows an infinity glyph instead. Settlement still moves real coins (the
    // merchant's stash simply bottoms out at 0 — harmless, never shown).
    [[nodiscard]] bool MerchantGoldInfinite();
    void SetMerchantGoldInfinite(bool a_on);

    // F4 (settings): lift the vendor CATEGORY whitelist/blacklist so any
    // merchant buys anything. The stolen-goods rule is NOT lifted — stolen
    // items still need a fence (buysStolen), matching the confirmed spec.
    [[nodiscard]] bool MerchantBuysAll();
    void SetMerchantBuysAll(bool a_on);

    // ---- F7: container spot memory (kLoot / kSteal) ------------------------
    // The partner grid remembers per-container item spots ("organize your
    // chest" like the player board). Barter keeps the auto-pack.

    // Store-drop target for the carried item: onCell = the grab-adjusted
    // footprint anchor landed on the partner board (spot-memory mode).
    // freeSpot = the whole footprint fits with no overlap; occ = EXACTLY one
    // overlapping item (the swap partner); neither = 2+ blockers (invalid).
    // Anchor math mirrors the player grid (mouse - grab offset, clamped).
    struct StoreDrop
    {
        bool                onCell = false;
        bool                freeSpot = false;
        int                 col = -1;
        int                 row = -1;
        RE::TESBoundObject* occ = nullptr;
        int                 occCount = 0;
        int                 occValue = 0;
        int                 occCol = 0;   // occupant's anchor (swap target spot)
        int                 occRow = 0;
        // GI24: WHICH occupant. A swap hands it to the cursor, so the carry has
        // to inherit its sub-stack AND its pool slot -- otherwise it is picked up
        // as an anonymous (uid 0, xlIdx -1, ord 0) unit, its slot stays behind
        // unreserved, and dropping it lands in a fresh slot at the front.
        std::uint16_t       occUid = 0;
        int                 occXlIdx = -1;
        int                 occOrd = 0;
        std::string         occSpotKey;
        int                 occRot = 0;   // GI62: and the angle it lies at
    };
    [[nodiscard]] StoreDrop QueryStoreDrop();

    // GI24: adopt a pool slot for a carry that did not start with a click (the
    // swap hands the displaced occupant to the cursor). Its slot must be held
    // for it, or the next drop treats it as a brand-new arrival.
    void NoteCarriedSpot(const std::string& a_spotKey);

    // remember a_obj's spot in the ACTIVE container layout (drop-to-cell /
    // swap / in-container move); footprint size comes from the def resolver
    // GI18: a_sig = Grid::HeldInstanceSig() of the unit being stored (0 for a
    // plain one). The spot is claimed by the matching cell once it lands.
    // GI62: a_rot = the quarter-turn the player dropped it at. This is how a
    // rotation crosses from the inventory into a container -- the spot on this
    // side is created from the hint, so it is created already turned.
    void NoteStoreSpot(RE::TESBoundObject* a_obj, int a_col, int a_row,
                       std::uint16_t a_sig = 0, int a_rot = 0);

    // drop-cell spot for a STACK store: kept until the quantity slider
    // confirms (kStore applies it) or cancels
    void SetStoreSpotHint(RE::TESBoundObject* a_obj, int a_col, int a_row,
                          std::uint16_t a_sig = 0, int a_rot = 0);

    // cosave 'GCLY' v1: container ref FormID -> (item key -> spot), LRU 128.
    // main.cpp owns the record loop.
    inline constexpr std::uint32_t kContRecordType = 'GCLY';
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame();

    // Phase 6: does the ACTIVE merchant BUY this player item? Enforces the
    // vendor faction's category list (vendorSellBuyList + notBuySell) and the
    // stolen policy (buysStolen / buysNonStolen). Always true outside kBarter or
    // when the partner has no vendor faction (plain container / corpse).
    // a_stolen = the item is NOT owned by the player (Grid caches this).
    [[nodiscard]] bool MerchantBuys(RE::TESBoundObject* a_obj, bool a_stolen);
}
