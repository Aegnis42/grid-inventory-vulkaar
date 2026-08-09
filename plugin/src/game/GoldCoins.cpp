#include "game/GoldCoins.h"

#include "game/BagFilter.h"
#include "ui/Grid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace FUI::GoldCoins
{
    namespace
    {
        constexpr const char*   kPlugin = "Grid Inventory.esp";
        constexpr RE::FormID    kGold001 = 0x0000000F;
        constexpr int           kPouchCap = 10000;
        constexpr std::uint32_t kVersion = 5;   // v2:+world sacks v3:+away gold v4:+pinned purses v5:+vendor cycle

        RE::TESBoundObject* g_coins[4] = {};   // 0x800..0x803 (tiers 1..4)
        RE::TESBoundObject* g_pouch = nullptr;
        // world-drop purses by amount (all optional; larger stands in for a
        // missing smaller): 1~4 G drops as a vanilla loose-gold ref instead
        RE::TESBoundObject* g_sack = nullptr;       // 0x809 Coin_purseLarge (100~1000)
        RE::TESBoundObject* g_sackMed = nullptr;    // 0x80A Coin_purseMed   (10~99)
        RE::TESBoundObject* g_sackSmall = nullptr;  // 0x80B Coin_purseSmall (5~9)
        // pouch ICON variants by stored amount (0x804 N itself is 0~2 G);
        // draw-time substitutes only — the pouch ITEM stays 0x804
        RE::TESBoundObject* g_pouchS = nullptr;     // 0x80C Coin_Pouch_S_01 (3~9)
        RE::TESBoundObject* g_pouchM = nullptr;     // 0x80D Coin_Pouch_M_01 (10~9999)
        RE::TESBoundObject* g_pouchF = nullptr;     // 0x80E Coin_Pouch_F_01 (full)
        int                 g_pouchStored = 0;
        bool                g_dirty = true;
        bool                g_applying = false;   // reentrancy guard (Tick)

        // G4: pinned gold purses — grid tile key -> fixed amount (1..1000).
        // Subtracted from walking gold so the auto tier-decomposition ignores
        // them; each keeps its exact value & position until merged/dropped.
        std::map<std::string, int> g_pinned;

        // ★Vendor restock: the last stock CYCLE we seeded for each merchant.
        // Not a timestamp — a cycle index, floor(daysPassed / iDaysToRespawnVendor).
        // Storing the index rather than the time is what makes the shelf stable:
        // every visit inside one cycle computes the same number, so re-opening
        // the barter window cannot re-roll the lineup or refill something the
        // player just bought (which is exactly what the old "stock it whenever
        // it is missing" rule did — bags were infinitely purchasable).
        std::map<RE::FormID, std::uint32_t> g_vendorCycle;

        // ★★Merchants re-seeded since the last game load. NOT serialized, and
        // cleared on every revert — it is a per-load allowance, not save state.
        //
        // The cycle counter above says "I stocked this merchant this cycle",
        // and that is not the same as "the goods are still on the shelf". The
        // vendor chest respawns on its own and takes our wares with it (they
        // are not in its leveled entries), and that respawn is processed on the
        // FIRST load after the game is launched. Measured on one save, same
        // merchant, same cycle: first load after launch, the chest held 0 of
        // ours; reloading the same save in the same session, 8. The counter
        // said "done" both times, so the shelf stayed empty until the game was
        // restarted.
        // ★Bounded to once per load on purpose. Plain "stock it whenever it is
        // missing" is what made bags infinitely purchasable before (see above):
        // buy the last one, reopen, and it came back. With this the shelf can
        // only be replenished after the chest was emptied by something other
        // than the player's own shopping.
        std::set<RE::FormID> g_reseeded;

        // how many general-purpose bags a general store shows per cycle
        constexpr int kGenericBagsPerCycle = 3;
        std::vector<BagWare> g_bagWares;

        // stored gold travelling INSIDE a pouch that left the inventory
        // (chest storage / drop / companion). Global concept, not per-item:
        // pouches are MISC stacks with no per-instance identity — any pouch
        // re-entering the inventory restores it (gold can never be lost to
        // the tracking itself; a respawning container eating the pouch is
        // the intended penalty).
        int g_awayGold = 0;

        // auto-store: ledger snapshot of the PREVIOUS tick (after our own
        // ops). -1 = uninitialised (skip the first tick after load/new game
        // so the starting gold is NOT mistaken for a fresh pickup).
        int g_lastLedger = -1;

        // ---- single deferred-ledger queue ----------------------------------
        // Every engine gold mutation triggered from the RENDER pass or an
        // event sink is deferred to Tick (game thread). One queue replaces the
        // old trio (pendingDrops + ledgerCredit + ledgerDebit). Each op also
        // declares its ledger delta so WalkingGold reflects it THE SAME FRAME
        // — the coins/pouch update instantly even though the engine mutation
        // lands next tick.
        struct LedgerOp
        {
            enum Kind { kDropCoin, kPouchLeave, kPouchReturn };
            Kind kind;
            int  value;
        };
        std::vector<LedgerOp> g_pending;

        // net change the pending ops will make to the ledger (+ credit, - debit)
        int PendingLedgerDelta()
        {
            int d = 0;
            for (const auto& op : g_pending) {
                d += (op.kind == LedgerOp::kPouchReturn) ? op.value : -op.value;
            }
            return d;
        }

        int PinnedSum()
        {
            int s = 0;
            for (const auto& [k, v] : g_pinned) s += v;
            return s;
        }

        // string (de)serialisation for the pinned map (cosave v4)
        bool WriteStr(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const std::uint32_t len = static_cast<std::uint32_t>(a_s.size());
            if (!a_intfc->WriteRecordData(len)) return false;
            return len == 0 || a_intfc->WriteRecordData(a_s.data(), len);
        }
        bool ReadStr(SKSE::SerializationInterface* a_intfc, std::string& a_out)
        {
            constexpr std::uint32_t kMaxStr = 256;
            std::uint32_t len = 0;
            if (!a_intfc->ReadRecordData(len) || len > kMaxStr) return false;
            a_out.resize(len);
            return len == 0 || a_intfc->ReadRecordData(a_out.data(), len);
        }

        // world Coin_Sack refs -> gold value they carry (cosave v2)
        std::map<RE::FormID, int> g_sackRefs;

        int CountOf(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_obj)
        {
            if (!a_obj) return 0;
            int cnt = 0;
            auto inv = a_p->GetInventory([&](RE::TESBoundObject& o) { return &o == a_obj; });
            for (auto& [o, d] : inv) cnt = d.first;
            return cnt;
        }

        // walking gold -> per-tier item counts. One 04 per full 1000; the
        // remainder becomes ONE coin of its band (1~4 / 5~9 / 10~99 / 100+).
        void Desired(int a_gold, int a_out[4])
        {
            a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0;
            if (a_gold <= 0) return;
            a_out[3] = a_gold / 1000;
            const int r = a_gold % 1000;
            if (r >= 100)     a_out[3] += 1;
            else if (r >= 10) a_out[2] += 1;
            else if (r >= 5)  a_out[1] += 1;
            else if (r >= 1)  a_out[0] += 1;
        }
    }

    void InitForms()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;
        for (int i = 0; i < 4; ++i) {
            g_coins[i] = dh->LookupForm<RE::TESObjectMISC>(0x800 + i, kPlugin);
        }
        g_pouch = dh->LookupForm<RE::TESObjectMISC>(0x804, kPlugin);
        g_pouchS = dh->LookupForm<RE::TESObjectMISC>(0x80C, kPlugin);
        g_pouchM = dh->LookupForm<RE::TESObjectMISC>(0x80D, kPlugin);
        g_pouchF = dh->LookupForm<RE::TESObjectMISC>(0x80E, kPlugin);
        SKSE::log::info("[GOLD] pouch icon variants: S={} M={} F={}",
            g_pouchS ? "ok" : "missing", g_pouchM ? "ok" : "missing",
            g_pouchF ? "ok" : "missing");
        g_sack      = dh->LookupForm<RE::TESObjectMISC>(0x809, kPlugin);   // Coin_purseLarge
        g_sackMed   = dh->LookupForm<RE::TESObjectMISC>(0x80A, kPlugin);   // Coin_purseMed
        g_sackSmall = dh->LookupForm<RE::TESObjectMISC>(0x80B, kPlugin);   // Coin_purseSmall
        if (g_sack) {
            SKSE::log::info("[GOLD] purses: L={:08X} M={} S={}", g_sack->GetFormID(),
                g_sackMed ? "ok" : "missing", g_sackSmall ? "ok" : "missing");
        } else {
            SKSE::log::info("[GOLD] no Coin_purseLarge (0x809) — drops fall back to vanilla gold refs");
        }
        if (Ready()) {
            SKSE::log::info("[GOLD] coin forms resolved ({})", kPlugin);
        } else {
            SKSE::log::warn("[GOLD] '{}' missing or incomplete — coin mirror disabled", kPlugin);
        }
        g_dirty = true;
    }

    bool Ready()
    {
        return g_coins[0] && g_coins[1] && g_coins[2] && g_coins[3] && g_pouch;
    }

    bool IsCoinForm(RE::FormID a_id)
    {
        for (auto* c : g_coins) {
            if (c && c->GetFormID() == a_id) return true;
        }
        return g_pouch && g_pouch->GetFormID() == a_id;
    }

    bool IsPouch(RE::FormID a_id)
    {
        return g_pouch && g_pouch->GetFormID() == a_id;
    }

    const char* FallbackIconKey(RE::FormID a_id)
    {
        static constexpr const char* kTiers[4] = {
            "msc_gold1", "msc_gold2", "msc_gold3", "msc_gold4",
        };
        for (int i = 0; i < 4; ++i) {
            if (g_coins[i] && g_coins[i]->GetFormID() == a_id) return kTiers[i];
        }
        // the pouch, its draw-time icon variants, and the purse sizes all read
        // as one thing: a bag of coins
        for (auto* p : { g_pouch, g_pouchS, g_pouchM, g_pouchF,
                         g_sack, g_sackMed, g_sackSmall }) {
            if (p && p->GetFormID() == a_id) return "msc_coinpouch";
        }
        return nullptr;
    }

    int PouchStored() { return g_pouchStored; }

    RE::TESBoundObject* PouchIconObject()
    {
        // stored-amount band -> icon variant (user spec 2026-07-22):
        // 0~2 = N (the pouch item itself), 3~9 = S, 10~9999 = M, cap = F.
        // A missing variant record falls through to the next lower band.
        if (g_pouchStored >= kPouchCap && g_pouchF) return g_pouchF;
        if (g_pouchStored >= 10 && g_pouchM) return g_pouchM;
        if (g_pouchStored >= 3 && g_pouchS) return g_pouchS;
        return g_pouch;
    }

    int PouchCap() { return kPouchCap; }

    RE::TESBoundObject* PouchForm() { return g_pouch; }

    bool PouchHeld()
    {
        // ★Asked from a click handler, so it may run before the forms resolve
        // and on a frame where the player is not there at all (main menu).
        auto* p = RE::PlayerCharacter::GetSingleton();
        return p && g_pouch && p->GetItemCount(g_pouch) > 0;
    }

    void SetBagWares(std::vector<BagWare> a_wares)
    {
        g_bagWares = std::move(a_wares);
        int typed = 0;
        for (const auto& w : g_bagWares) typed += w.accept.empty() ? 0 : 1;
        logger::info("[VENDOR] {} bag ware(s): {} typed, {} general",
            g_bagWares.size(), typed, g_bagWares.size() - typed);
    }

    void SeedVendorStock(RE::Actor* a_merchant, RE::TESObjectREFR* a_container)
    {
        // ★★Every exit below used to be a silent `return`, and the log for a
        // shop visit was therefore EMPTY -- indistinguishable from "never
        // called". A run of these is the whole reason the "no bags on the first
        // load after launching the game" report could not be read off a log.
        // Each one names itself now; the reason must reach the log even when
        // the answer is "nothing to do".
        const auto bail = [&](const char* why) {
            logger::info("[VENDOR] skip ({}) - merchant {}", why,
                a_merchant ? a_merchant->GetDisplayFullName() : "<null>");
        };
        if (!a_merchant || !a_container) { bail("no merchant/container"); return; }

        auto* fac = a_merchant->GetVendorFaction();
        if (!fac) { bail("no vendor faction"); return; }
        auto* list = fac->vendorData.vendorSellBuyList;
        // unrestricted vendor: not a general store either
        if (!list) { bail("no sell/buy list"); return; }

        // "General goods" = the vendor's category list covers clutter. Testing
        // the KEYWORD rather than our own item matters: the pouch is a custom
        // MISC record with no VendorItem* keyword of its own, so matching it
        // against the list directly would exclude every merchant. Keywords keep
        // their EditorID at runtime, so no po3 Tweaks dependency here.
        // ★NOT a function-local static any more. A static caches the FIRST
        // lookup for the whole process, so one early miss would poison every
        // shop until the game is restarted -- exactly the shape of a
        // "only after a fresh launch" bug, and not something to leave standing
        // while investigating one.
        auto* clutter = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("VendorItemClutter");
        if (!clutter) { bail("VendorItemClutter keyword not found"); return; }
        const bool inList = list->HasForm(clutter);
        // The list is a whitelist unless notBuySell flips it into a blacklist.
        // ★No longer an early return: a SPECIALIST (alchemist, blacksmith) is
        // not a general store but still stocks its own typed bag, so the two
        // questions had to come apart.
        const bool isGeneral = !(fac->vendorData.vendorValues.notBuySell ? inList : !inList);

        // ---- restock cycle ------------------------------------------------
        // The merchant chest respawns on its own (CONT DATA bit1 is set on
        // every Merchant* chest, verified) every iDaysToRespawnVendor days,
        // and that reset WIPES what we put in — our items were never in the
        // chest's leveled entries, so they are not regenerated, just gone.
        // Nothing to clean up on our side; we only decide what goes back.
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal) { bail("no Calendar"); return; }
        float days = 2.0f;
        if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
            if (auto* set = gs->GetSetting("iDaysToRespawnVendor")) {
                days = static_cast<float>(set->GetSInt());
            }
        }
        // ★Read the setting, never hardcode 2: merchant overhauls routinely
        // change it, and a hardcoded rhythm would leave OUR wares on a
        // different clock from every other item on the shelf.
        if (days < 0.1f) days = 2.0f;
        const auto cycle = static_cast<std::uint32_t>(cal->GetDaysPassed() / days);

        // ★★How many of OUR wares are on the shelf RIGHT NOW. This is not a
        // statistic -- it is the thing the restock decision turns on, because
        // "I stocked this cycle" and "the goods are still there" are different
        // claims and the chest's own respawn is what pulls them apart.
        // ★One pass, not one per ware: GetInventory() walks the whole list and
        // allocates a map on every call, and this now runs on every shop open.
        int oursInChest = 0;
        {
            std::set<RE::TESBoundObject*> ours;
            for (const auto& w : g_bagWares) {
                if (w.obj) ours.insert(w.obj);
            }
            if (g_pouch) ours.insert(g_pouch);
            for (const auto& [obj, data] : a_container->GetInventory()) {
                if (data.first > 0 && ours.contains(obj)) ++oursInChest;
            }
        }

        const RE::FormID mid = a_merchant->GetFormID();
        const auto it = g_vendorCycle.find(mid);
        const bool cycleDone = it != g_vendorCycle.end() && it->second >= cycle;
        // Nothing of ours on the shelf and we have not already replenished this
        // merchant since the load -> the chest was emptied by something that is
        // not the player's shopping (its own respawn), so the counter is lying.
        const bool emptied = oursInChest == 0 && !g_reseeded.contains(mid);

        if (cycleDone && !emptied) {
            // already stocked this cycle — buying does not refill it
            logger::info("[VENDOR] {}: cycle {} already stocked, {} of ours on the shelf",
                a_merchant->GetDisplayFullName(), cycle, oursInChest);
            return;
        }
        logger::info("[VENDOR] {} (0x{:08X}): cycle {} (day {:.1f} / {:.0f}), general={}, "
                     "{} of ours on the shelf -- {}",
            a_merchant->GetDisplayFullName(), mid, cycle, cal->GetDaysPassed(), days,
            isGeneral, oursInChest,
            cycleDone ? "chest respawned since we stocked it, re-seeding once this load"
                      : "new cycle");
        g_vendorCycle[mid] = cycle;
        g_reseeded.insert(mid);

        auto place = [&](RE::TESBoundObject* item, const char* why) {
            if (!item) return;
            for (const auto& [obj, data] : a_container->GetInventory(
                     [&](RE::TESBoundObject& o) { return &o == item; })) {
                (void)obj;
                if (data.first > 0) return;   // already there (player sold one back)
            }
            a_container->AddObjectToContainer(item, nullptr, 1, nullptr);
            logger::info("[VENDOR] cycle {} {}: stocked '{}' ({})",
                cycle, a_merchant->GetDisplayFullName(), item->GetName(), why);
        };

        // the pouch is core kit, not a lucky find: general goods, every cycle
        if (isGeneral) place(g_pouch, "always");

        // ---- typed bags: the shop that trades what the bag holds -----------
        // Guaranteed, not rotated. A player who walks to the alchemist for an
        // alchemy pouch should find one; making it a lucky draw would turn a
        // deliberate errand into a chore.
        std::vector<RE::TESBoundObject*> generic;
        for (const auto& w : g_bagWares) {
            if (!w.obj) continue;
            if (w.accept.empty()) { generic.push_back(w.obj); continue; }
            auto* kw = BagFilter::VendorKeyword(w.accept);
            if (kw && list->HasForm(kw) == !fac->vendorData.vendorValues.notBuySell) {
                place(w.obj, "typed");
            }
        }

        // ---- general-purpose bags: a fresh draw at the general store --------
        // Deterministic in (merchant, cycle): re-opening the window cannot
        // reshuffle the shelf, and no roll has to be stored anywhere.
        if (isGeneral && !generic.empty()) {
            const int n = static_cast<int>(generic.size());
            const int want = (std::min)(kGenericBagsPerCycle, n);
            std::uint32_t h = mid * 2654435761u ^ (cycle * 40503u);
            std::vector<int> idx(n);
            for (int i = 0; i < n; ++i) idx[i] = i;
            // Fisher-Yates off the same hash: picking `want` distinct entries
            // rather than `want` independent rolls, so the shelf never shows
            // the same bag twice.
            for (int i = n - 1; i > 0; --i) {
                h = h * 1664525u + 1013904223u;
                const int j = static_cast<int>(h % static_cast<std::uint32_t>(i + 1));
                std::swap(idx[i], idx[j]);
            }
            for (int i = 0; i < want; ++i) place(generic[idx[i]], "rotating");
        }
        logger::info("[VENDOR]   {} typed candidate(s), {} generic in the pool",
            g_bagWares.size() - generic.size(), generic.size());
    }

    namespace
    {
        int TierOf(RE::FormID a_id)
        {
            for (int i = 0; i < 4; ++i) {
                if (g_coins[i] && g_coins[i]->GetFormID() == a_id) return i;
            }
            return -1;
        }

        int WalkingGold()
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return 0;
            auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
            if (!gold) return 0;
            // ONE formula: (ledger + pending ledger change) - pouch - pinned.
            // The pending delta makes every deferred op (coin drop, pouch
            // leave/return) reflect in the coins THIS frame, before Tick runs
            // the actual engine mutation. Pouch leave/return net to zero here
            // (stored moves with the ledger), so walking gold is stable; a
            // coin drop nets negative, so the dropped tile vanishes at once.
            // Pinned purses (G4) are user-fixed amounts pulled OUT of the
            // auto-decomposed walking pool.
            return (std::max)(0,
                (CountOf(p, gold) + PendingLedgerDelta()) - g_pouchStored - PinnedSum());
        }

        // world-drop purse by amount (same bands as the coin tiers).
        // nullptr = 1~4 G: drop a vanilla loose-gold ref instead.
        RE::TESBoundObject* SackFor(int a_value)
        {
            if (a_value >= 100) return g_sack;
            if (a_value >= 10)  return g_sackMed ? g_sackMed : g_sack;
            if (a_value >= 5)   return g_sackSmall ? g_sackSmall : g_sack;
            return nullptr;
        }
    }

    int InstanceValue(RE::FormID a_form, int a_index)
    {
        const int tier = TierOf(a_form);
        if (tier < 0) return 0;
        const int walking = WalkingGold();
        if (walking <= 0) return 0;
        const int full = walking / 1000;
        const int rem = walking % 1000;
        switch (tier) {
        case 3:
            if (a_index < full) return 1000;
            return rem >= 100 ? rem : 0;
        case 2: return (rem >= 10 && rem <= 99) ? rem : 0;
        case 1: return (rem >= 5 && rem <= 9) ? rem : 0;
        default: return (rem >= 1 && rem <= 4) ? rem : 0;
        }
    }

    int CoinTileCount(RE::FormID a_form)
    {
        // Tiles this coin form should show, from WALKING gold (pending drops
        // already subtracted) — NOT the live item count, which lags a tick
        // behind a drop and would let the rebuild self-refill the dropped cell.
        const int tier = TierOf(a_form);
        if (tier < 0) return 0;
        const int walking = WalkingGold();
        if (walking <= 0) return 0;
        const int full = walking / 1000;
        const int rem = walking % 1000;
        switch (tier) {
        case 3:  return full + (rem >= 100 ? 1 : 0);
        case 2:  return (rem >= 10 && rem <= 99) ? 1 : 0;
        case 1:  return (rem >= 5 && rem <= 9) ? 1 : 0;
        default: return (rem >= 1 && rem <= 4) ? 1 : 0;
        }
    }

    int WalkingGoldValue() { return WalkingGold(); }

    int CoinTilesFor(int a_walking)
    {
        // Same tier breakdown as CoinTileCount, summed over every tier, for an
        // arbitrary walking-gold amount (not the live value).
        if (a_walking <= 0) return 0;
        const int full = a_walking / 1000;
        const int rem = a_walking % 1000;
        int n = full + (rem >= 100 ? 1 : 0);   // tier 3 (100..1000, one per 1000)
        n += (rem >= 10 && rem <= 99) ? 1 : 0;  // tier 2
        n += (rem >= 5 && rem <= 9) ? 1 : 0;    // tier 1
        n += (rem >= 1 && rem <= 4) ? 1 : 0;    // tier 0
        return n;
    }

    // ---- G4: pinned gold purses ------------------------------------------
    int BandTier(int a_value)
    {
        if (a_value >= 100) return 3;
        if (a_value >= 10)  return 2;
        if (a_value >= 5)   return 1;
        if (a_value >= 1)   return 0;
        return -1;
    }

    RE::TESBoundObject* CoinForTier(int a_tier)
    {
        return (a_tier >= 0 && a_tier < 4) ? g_coins[a_tier] : nullptr;
    }

    int PinnedValue(const std::string& a_tileKey)
    {
        const auto it = g_pinned.find(a_tileKey);
        return it == g_pinned.end() ? -1 : it->second;
    }

    int PinnedTotal() { return PinnedSum(); }

    void PinAmount(const std::string& a_tileKey, int a_value)
    {
        const int v = (std::min)(a_value, kCoinCap);
        if (v <= 0) { g_pinned.erase(a_tileKey); }
        else        { g_pinned[a_tileKey] = v; }
        g_dirty = true;
    }

    void UnpinTile(const std::string& a_tileKey)
    {
        if (g_pinned.erase(a_tileKey) > 0) g_dirty = true;
    }

    int StoreToPouch(int a_value)
    {
        const int room = kPouchCap - g_pouchStored;
        const int s = (std::min)({ a_value, room, WalkingGold() });
        if (s <= 0) return 0;
        g_pouchStored += s;
        g_dirty = true;
        SKSE::log::info("[GOLD] pouch: +{} -> {}", s, g_pouchStored);
        return s;
    }

    void Withdraw(int a_value, bool a_sound)
    {
        const int w = (std::min)(a_value, g_pouchStored);
        if (w <= 0) return;
        g_pouchStored -= w;
        g_dirty = true;
        // gold putdown sfx — the coins audibly land back in your inventory
        if (a_sound) {
            if (auto* p = RE::PlayerCharacter::GetSingleton()) {
                if (auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001)) {
                    p->PlayPickUpSound(gold, false, false);
                }
            }
        }
        SKSE::log::info("[GOLD] pouch: -{} -> {}", w, g_pouchStored);
    }

    namespace
    {
        // SELL context = the pouch is being sold to a vendor: the stored gold
        // must pop back into walking coins first, then only the empty pouch
        // changes hands (no exploit). STORAGE/DROP instead lets the gold
        // travel with the pouch.
        // Currently detected by the vanilla BarterMenu. PLAN_LOOT_BARTER
        // Phase 5: our custom trade UI does NOT open BarterMenu, so OR in
        // `g_uiMode == kBarter` here once that global exists.
        bool g_barterContext = false;   // our custom barter UI open (Phase 5)

        bool InSellContext()
        {
            if (g_barterContext) return true;   // our grid barter UI
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
        }
    }

    void SetBarterContext(bool a_open) { g_barterContext = a_open; }

    void OnPouchLeftPlayer()
    {
        if (g_pouchStored <= 0) return;

        // SALE: the vendor buys the pouch at its base (empty) value — the
        // stored gold pops back into walking coins first (no exploit).
        if (InSellContext()) {
            SKSE::log::info("[GOLD] pouch sold -> releasing {} G", g_pouchStored);
            Withdraw(g_pouchStored);
            return;
        }

        // STORAGE / DROP: the gold TRAVELS with the pouch (space-penalty
        // design: banking gold away is the point). Stored -> away immediately
        // (logical state), and a kPouchLeave op debits the ledger on Tick.
        // WalkingGold is unchanged this frame (stored and ledger fall together).
        SKSE::log::info("[GOLD] pouch left with {} G inside", g_pouchStored);
        g_awayGold += g_pouchStored;
        g_pending.push_back({ LedgerOp::kPouchLeave, g_pouchStored });
        g_pouchStored = 0;
        g_dirty = true;
    }

    void OnPouchReturned()
    {
        if (g_awayGold <= 0) return;
        SKSE::log::info("[GOLD] pouch returned with {} G inside", g_awayGold);
        // away -> stored immediately; a kPouchReturn op credits the ledger on
        // Tick. Over-cap (multi-pouch merge) stays walking after the credit.
        g_pending.push_back({ LedgerOp::kPouchReturn, g_awayGold });
        g_pouchStored = (std::min)(g_pouchStored + g_awayGold, kPouchCap);
        g_awayGold = 0;
        g_dirty = true;
    }

    namespace
    {
        // The actual drop, run on the GAME thread from Tick (PlaceObjectAtMe
        // in the render pass leaves the ref's 3D unattached — invisible bag).
        // GI53: returns the gold actually debited (0 when placement failed),
        // so the caller's lastLedger stays honest.
        int ProcessDrop(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_gold, int a_value)
        {
            const int v = (std::min)(a_value, CountOf(a_p, a_gold));
            if (v <= 0) return 0;

            a_p->PlayPickUpSound(a_gold, false, false);
            // purse size follows the amount (same bands as the coin tiers);
            // 1~4 G has no purse — a vanilla loose-gold ref reads better
            auto* sack = SackFor(v);
            if (sack) {
                // Direct placement — no inventory round-trip, so loot-HUD
                // widgets don't spam "Coinpurse received". The display name
                // carries the stored amount.
                if (auto ref = a_p->PlaceObjectAtMe(sack, false)) {
                    const RE::NiPoint3 ppos = a_p->GetPosition();
                    const float heading = a_p->GetAngleZ();
                    ref->SetPosition(RE::NiPoint3(
                        ppos.x + 60.0f * std::sin(heading),
                        ppos.y + 60.0f * std::cos(heading),
                        ppos.z + 8.0f));
                    char name[96];
                    std::snprintf(name, sizeof(name), "%s (%dG)",
                        sack->GetName() ? sack->GetName() : "?", v);
                    ref->SetDisplayName(name, true);
                    g_sackRefs[ref->GetFormID()] = v;
                    a_p->RemoveItem(a_gold, v, RE::ITEM_REMOVE_REASON::kRemove,
                        nullptr, nullptr);
                    SKSE::log::info("[GOLD] dropped a {} G sack ({:08X})",
                        v, ref->GetFormID());
                    return v;
                }
                SKSE::log::error("[GOLD] sack placement failed — gold kept");
                return 0;
            }
            // 1~4 G (or missing records): vanilla loose-gold ref
            a_p->RemoveItem(a_gold, v, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
            SKSE::log::info("[GOLD] dropped {} G to the world", v);
            return v;
        }

        // Apply one deferred op on the game thread. It also adjusts g_lastLedger
        // by its own ledger delta so the auto-store step that follows sees ONLY
        // external pickups, never our own mutations (order-independent).
        void ApplyLedgerOp(RE::PlayerCharacter* a_p, RE::TESBoundObject* a_gold,
                           const LedgerOp& a_op)
        {
            switch (a_op.kind) {
            case LedgerOp::kDropCoin:
                {
                    const int d = ProcessDrop(a_p, a_gold, a_op.value);
                    if (g_lastLedger >= 0) g_lastLedger -= d;
                }
                break;
            case LedgerOp::kPouchLeave:
                {
                    const int d = (std::min)(a_op.value, CountOf(a_p, a_gold));
                    if (d > 0) {
                        a_p->RemoveItem(a_gold, d, RE::ITEM_REMOVE_REASON::kRemove,
                            nullptr, nullptr);
                    }
                    // GI53: the ledger could cover only d of the promised value
                    // (a payment queued the same tick ran first). awayGold must
                    // shrink by the shortfall or the pouch comes back with free
                    // gold; lastLedger must track the ACTUAL debit or the next
                    // tick reads the difference as an external pickup.
                    if (d < a_op.value) {
                        g_awayGold = (std::max)(0, g_awayGold - (a_op.value - d));
                    }
                    if (g_lastLedger >= 0) g_lastLedger -= d;
                }
                break;
            case LedgerOp::kPouchReturn:
                a_p->AddObjectToContainer(a_gold, nullptr, a_op.value, nullptr);
                if (g_lastLedger >= 0) g_lastLedger += a_op.value;
                break;
            }
        }
    }

    void DropAsGold(int a_value)
    {
        // Enqueue only — WalkingGold subtracts pending ops immediately, so the
        // coin tile disappears THIS frame; the ledger debit + sack spawn happen
        // on the next Tick (game thread). This keeps tiles == slots during the
        // rebuild, so the dropped cell stays the emptied one.
        const int v = (std::min)(a_value, WalkingGold());
        if (v <= 0) return;
        g_pending.push_back({ LedgerOp::kDropCoin, v });
        g_dirty = true;
    }

    bool TryPickUpSack(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref) return false;
        auto* base = a_ref->GetBaseObject();
        if (!base) return false;
        if (base != g_sack && base != g_sackMed && base != g_sackSmall) return false;

        int v = 0;
        if (const auto it = g_sackRefs.find(a_ref->GetFormID()); it != g_sackRefs.end()) {
            v = it->second;
            g_sackRefs.erase(it);
        } else if (const char* nm = a_ref->GetDisplayFullName()) {
            // GI53: a sack the map no longer knows (lost cosave entry) must not
            // be destroyed for 0 G. The amount also rides the display name
            // "<sack> (1234G)" -- recover it from there.
            if (const char* par = std::strrchr(nm, '(')) v = std::atoi(par + 1);
        }
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
        if (!p || !gold || v <= 0) {
            SKSE::log::error("[GOLD] sack {:08X} not redeemable -- left in place",
                a_ref->GetFormID());
            return true;   // swallow the activation, keep the sack (no 0 G burn)
        }
        p->PlayPickUpSound(gold, true, false);
        p->AddObjectToContainer(gold, nullptr, v, nullptr);
        a_ref->Disable();
        a_ref->SetDelete(true);
        g_dirty = true;
        SKSE::log::info("[GOLD] picked up a {} G sack", v);
        return true;
    }

    void MarkDirty() { g_dirty = true; }

    void Tick()
    {
        if (!g_dirty || g_applying || !Ready()) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->Is3DLoaded()) return;
        g_dirty = false;

        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(kGold001);
        if (!gold) return;

        // STEP 1 — deferred ledger ops (coin drops + pouch leave/return). All
        // engine gold mutations from the render pass / event sinks land here on
        // the game thread. Each op self-adjusts g_lastLedger so step 2 sees
        // only external pickups. WalkingGold already reflected these ops the
        // frame they were queued (PendingLedgerDelta), so the UI never lagged.
        if (!g_pending.empty()) {
            g_applying = true;
            for (const auto& op : g_pending) ApplyLedgerOp(p, gold, op);
            g_pending.clear();
            g_applying = false;
        }

        // STEP 2 — auto-store: while a pouch is held, gold newly PICKED UP from
        // OUTSIDE (loot, sale income, rewards) goes straight into the pouch;
        // gold you were already walking with is left alone (banked manually).
        // Our own ops in step 1 already moved g_lastLedger, so only external
        // increases register here. First tick after load is skipped
        // (g_lastLedger < 0) so starting gold isn't swept in.
        {
            const int pre = CountOf(p, gold);
            if (g_lastLedger >= 0 && pre > g_lastLedger && CountOf(p, g_pouch) > 0) {
                const int room = kPouchCap - g_pouchStored;
                const int store = (std::min)(pre - g_lastLedger, room);
                if (store > 0) {
                    g_pouchStored += store;
                    SKSE::log::info("[GOLD] auto-stored {} G into pouch -> {}",
                        store, g_pouchStored);
                }
            }
        }

        // STEP 3 — baseline snapshot + mirror the walking gold into coins.
        const int total = CountOf(p, gold);   // the LEDGER (authoritative)
        g_lastLedger = total;                 // auto-store baseline for next tick

        // the pouch can never hold more than you own (shops spend the ledger)
        g_pouchStored = (std::min)({ g_pouchStored, total, kPouchCap });
        if (g_pouchStored < 0) g_pouchStored = 0;

        // G4: pinned purses can't outlast the ledger either — if gold was spent
        // (shop/quest) below pouch+pinned, trim purses (highest map key first —
        // effectively arbitrary) so walking never goes negative. Rare; keeps
        // the mirror consistent.
        {
            int budget = total - g_pouchStored;   // gold available to pinned+walking
            int psum = PinnedSum();
            while (psum > budget && !g_pinned.empty()) {
                auto last = std::prev(g_pinned.end());
                const int over = psum - budget;
                if (last->second > over) { last->second -= over; psum -= over; }
                else { psum -= last->second; g_pinned.erase(last); }
            }
        }

        int want[4];
        Desired(total - g_pouchStored - PinnedSum(), want);
        // pinned purses are real coin tiles too — add one coin of each purse's
        // band so the mirror keeps the correct per-form item counts.
        for (const auto& [k, v] : g_pinned) {
            const int t = BandTier(v);
            if (t >= 0 && t < 4) want[t] += 1;
        }

        g_applying = true;
        bool changed = false;
        for (int i = 0; i < 4; ++i) {
            const int cur = CountOf(p, g_coins[i]);
            const int diff = want[i] - cur;
            if (diff > 0) {
                p->AddObjectToContainer(g_coins[i], nullptr, diff, nullptr);
                changed = true;
            } else if (diff < 0) {
                p->RemoveItem(g_coins[i], -diff, RE::ITEM_REMOVE_REASON::kRemove,
                    nullptr, nullptr);
                changed = true;
            }
        }
        g_applying = false;

        if (changed) {
            SKSE::log::info("[GOLD] mirror: {} G walking (+{} pouch) -> {}/{}/{}/{}",
                total - g_pouchStored, g_pouchStored,
                want[0], want[1], want[2], want[3]);
            Grid::RequestRebuild();
            Grid::MarkCapacityDirty();
        }
    }

    // ---- cosave: pouch stored amount (+v2: world sack refs) ----
    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, kVersion)) return;
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_pouchStored));
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_sackRefs.size()));
        for (const auto& [id, val] : g_sackRefs) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(static_cast<std::uint32_t>(val));
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_awayGold));   // v3
        // v4: pinned gold purses (tileKey -> value)
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_pinned.size()));
        for (const auto& [key, val] : g_pinned) {
            WriteStr(a_intfc, key);
            a_intfc->WriteRecordData(static_cast<std::int32_t>(val));
        }
        // v5: which restock cycle each merchant was last stocked for
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_vendorCycle.size()));
        for (const auto& [id, cyc] : g_vendorCycle) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(cyc);
        }
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
    {
        std::uint32_t v = 0;
        if (a_intfc->ReadRecordData(v)) {
            g_pouchStored = static_cast<int>((std::min)(v, static_cast<std::uint32_t>(kPouchCap)));
        }
        if (a_version >= 2) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 4096) {
                // GI53: an impossible count means the record is corrupt. The n
                // entries are still IN the stream -- skipping the loop but
                // reading on would parse sack pairs as awayGold (free gold).
                SKSE::log::error("[GOLD] GPCH sack count {} rejected -- record dropped", n);
                return;
            }
            {
                for (std::uint32_t i = 0; i < n; ++i) {
                    RE::FormID id = 0;
                    std::uint32_t val = 0;
                    if (!a_intfc->ReadRecordData(id) || !a_intfc->ReadRecordData(val)) break;
                    RE::FormID resolved = 0;
                    if (a_intfc->ResolveFormID(id, resolved)) {
                        g_sackRefs[resolved] = static_cast<int>(val);
                    }
                }
            }
        }
        if (a_version >= 3) {
            std::uint32_t away = 0;
            if (a_intfc->ReadRecordData(away)) {
                g_awayGold = static_cast<int>((std::min)(away, 1000000u));
            }
        }
        if (a_version >= 4) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 65536) {   // GI53: corrupt count -> stop (see v2 note)
                SKSE::log::error("[GOLD] GPCH pin count {} rejected -- record dropped", n);
                return;
            }
            {
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::string  key;
                    std::int32_t val = 0;
                    if (!ReadStr(a_intfc, key) || !a_intfc->ReadRecordData(val)) break;
                    if (val > 0) g_pinned[key] = (std::min)(val, kCoinCap);
                }
            }
        }
        if (a_version >= 5) {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n)) return;
            if (n > 65536) {   // GI53: corrupt count -> stop (see v2 note)
                SKSE::log::error("[GOLD] GPCH vendor count {} rejected -- record dropped", n);
                return;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID    id = 0;
                std::uint32_t cyc = 0;
                if (!a_intfc->ReadRecordData(id) || !a_intfc->ReadRecordData(cyc)) break;
                RE::FormID resolved = 0;
                if (a_intfc->ResolveFormID(id, resolved)) g_vendorCycle[resolved] = cyc;
            }
        }
        g_dirty = true;
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        g_pouchStored = 0;
        g_sackRefs.clear();
        g_awayGold = 0;
        g_pinned.clear();
        // per-save state: a new game must not inherit the last save's cycles,
        // or its first merchants look "already stocked" and skip a rotation
        g_vendorCycle.clear();
        // per-LOAD state: the re-seed allowance is renewed by every load, which
        // is exactly when the chest respawn that spends it happens
        g_reseeded.clear();
        g_pending.clear();
        g_lastLedger = -1;   // skip auto-store on the first tick after load
        g_dirty = true;
    }
}
