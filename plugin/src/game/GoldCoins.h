#pragma once

#include <string>

// G1: gold as physical coin items (Mabinogi style). Gold001 stays the hidden
// LEDGER (shops/quests keep working untouched); the player's visible gold is
// MIRRORED into real MISC items from Grid Inventory.esp:
//   Coin_icon_01 (0x800) 1~4 G   | Coin_icon_02 (0x801) 5~9 G
//   Coin_icon_03 (0x802) 10~99 G | Coin_icon_04 (0x803) 100~1000 G (stacks)
//   Coin_Pouch   (0x804) 2x2, manual storage up to 10,000 G (G2: store/draw)
//   Coin_purseLarge (0x809) world-drop coin bag (abilities: 0x807/0x808)
// Decomposition: one 04 per full 1000, remainder becomes ONE coin of its
// tier. Same-form coins stack into a single grid tile with a count badge.
// The reconciler runs on the game thread (Tick) whenever the ledger moved;
// its own Add/Remove re-fire container events, which re-mark dirty and then
// reconcile to a zero diff — no loop.

namespace FUI::GoldCoins
{
    void InitForms();   // kDataLoaded: resolve the ESP records (soft-fails)
    [[nodiscard]] bool Ready();

    // coins + pouch — drop/sell style guards check this (G1: drops blocked)
    [[nodiscard]] bool IsCoinForm(RE::FormID a_id);
    [[nodiscard]] bool IsPouch(RE::FormID a_id);

    [[nodiscard]] int PouchStored();   // 0..10000, cosave-persisted
    [[nodiscard]] int PouchCap();      // 10000
    [[nodiscard]] RE::TESBoundObject* PouchForm();
    // ICON to draw for the pouch tile right now: 0x804 N (0~2 G) /
    // 0x80C S (3~9) / 0x80D M (10~9999) / 0x80E F (full). Draw-time only —
    // the inventory item itself never changes form.
    [[nodiscard]] RE::TESBoundObject* PouchIconObject();   // window title etc.

    // G2: value represented by ONE coin tile of a_form at split-stack index
    // a_index (04 tiles show one-per-coin: index < fullThousands -> 1000,
    // last one -> the 100..999 remainder; tiers 1..3 always index 0).
    [[nodiscard]] int InstanceValue(RE::FormID a_form, int a_index);

    // Number of grid tiles this coin form should display, computed from
    // WALKING gold (pending drops already subtracted). The rebuild uses this
    // instead of the live item count, which lags a tick behind a drop.
    // NOTE: AUTO tiles only — pinned purses (below) are counted separately.
    [[nodiscard]] int CoinTileCount(RE::FormID a_form);

    // Total AUTO coin cells (all tiers, 1x1 each) a given WALKING-gold amount
    // would occupy. Grid's spill pass uses this to add back the coin tiles a
    // barter payment just dissolved, so a purchase doesn't reuse the freed
    // cells (the item spills into a bag as if gold still filled the board).
    [[nodiscard]] int CoinTilesFor(int a_walking);

    // Current walking gold (ledger - pouch - pinned): the pool the auto coin
    // tiles decompose from. Grid's spill pass pairs it with CoinTilesFor above.
    [[nodiscard]] int WalkingGoldValue();

    // ---- G4: pinned gold purses (Mabinogi split, symmetric with stacks) ----
    // A split fixes an amount (1..1000) onto a specific grid tile key; that
    // gold is subtracted from WALKING gold so the auto tier-decomposition
    // ignores it, and the purse keeps its exact value & position until merged
    // or dropped back. Value cap == one 04 coin (1000); larger never occurs.
    inline constexpr int kCoinCap = 1000;
    [[nodiscard]] int PinnedValue(const std::string& a_tileKey);   // -1 if not pinned
    [[nodiscard]] int PinnedTotal();                               // Σ pinned values
    [[nodiscard]] int BandTier(int a_value);                       // value -> coin tier 0..3
    [[nodiscard]] RE::TESBoundObject* CoinForTier(int a_tier);     // 0x800..0x803
    void PinAmount(const std::string& a_tileKey, int a_value);     // create/replace a purse
    void UnpinTile(const std::string& a_tileKey);                  // merged / dropped / cancelled

    // G2 interactions — mutate the ledger/pouch and mark the mirror dirty.
    int  StoreToPouch(int a_value);   // returns the amount actually stored
    // pouch -> walking gold. a_sound=false when the caller immediately lifts
    // the amount onto the cursor (the pickup sound plays instead).
    void Withdraw(int a_value, bool a_sound = true);
    void DropAsGold(int a_value);     // ledger down; a Coin_Sack (0x805,
                                      // coinbaglarge.nif) ref lands at the feet
                                      // carrying the value; falls back to a
                                      // vanilla gold ref when the ESP record
                                      // is missing

    // PickUpHook: a Coin_Sack world ref activates as GOLD — consume the ref,
    // credit its recorded value to the ledger. Returns true when handled.
    bool TryPickUpSack(RE::TESObjectREFR* a_ref);

    // The pouch ITEM can leave the player (chest storage, drop, companion,
    // sale) — space-penalty design: banking gold away is the point.
    //  - SALE (BarterMenu open): stored gold pops back to walking coins
    //    first; the vendor buys an empty pouch (no exploit).
    //  - everything else: the gold TRAVELS with the pouch (ledger debited);
    //    any pouch re-entering the inventory brings it back (credit).
    // Both are called from the container sink; ledger ops run on Tick.
    void OnPouchLeftPlayer();
    void OnPouchReturned();

    // How the player is meant to GET a pouch. The esp carries no leveled-list
    // or vendor-list edit, so nothing stocks it -- and vendors sell out of the
    // vendor faction's merchantContainer, not their own inventory, which is why
    // SPID (an NPC distributor) cannot put it on the shelf either.
    //
    // So we stock it ourselves when a barter session opens: one pouch into this
    // merchant's chest -- and (§RELEASE-B) one Satchel and one Knapsack, the
    // shipped bag items -- but only for general-goods vendors and only when
    // the chest has none. Nothing is added to the esp's vendor lists, so no
    // leveled-list override can conflict with a merchant mod, and existing
    // saves are covered on the next visit. A chest restock that clears them
    // is re-seeded the visit after.
    void SeedVendorStock(RE::Actor* a_merchant, RE::TESObjectREFR* a_container);

    void MarkDirty();   // player's Gold001 (or a coin form) changed
    void Tick();        // reconcile mirror -> inventory (game thread)

    // Phase 5: our custom barter UI doesn't open the vanilla BarterMenu, so it
    // flags "selling context" here — OnPouchLeftPlayer uses it to release the
    // stored gold on a pouch sale (§2-C) instead of letting it travel away.
    void SetBarterContext(bool a_open);

    inline constexpr std::uint32_t kRecordType = 'GPCH';
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
}
