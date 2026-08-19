#include "game/DeltaWatch.h"

#include "game/Ledger.h"

#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace FUI::DeltaWatch
{
    namespace
    {
        constexpr RE::FormID kPlayerRef = 0x14;

        bool g_on = false;

        // ★One lock for everything below. The container event arrives on
        // whatever thread the engine happened to be on; Reconcile runs on the
        // main thread. Without this the map is torn, and a diagnostic that
        // crashes is worse than no diagnostic (PLAN §10-10).
        std::mutex g_mtx;

        // what a full recount said, at the last reconcile
        std::unordered_map<RE::FormID, std::int32_t> g_baseline;
        // what the deltas have said since
        std::unordered_map<RE::FormID, std::int32_t> g_shadow;

        bool          g_haveBaseline = false;
        std::uint64_t g_seq = 0;      // arrival order, monotonic
        std::uint64_t g_events = 0;   // since the last reconcile

        // ---- echo bookkeeping (the requests themselves live in Ledger) --------
        std::uint32_t g_matched = 0;
        std::uint32_t g_unmatched = 0;
        std::uint32_t g_ambiguous = 0;

        // Thread ids are long and meaningless; a small dense index is what we
        // actually read ("did #142 and #143 arrive on different threads").
        std::vector<std::thread::id> g_threads;

        int ThreadSlot()
        {
            const auto id = std::this_thread::get_id();
            for (std::size_t i = 0; i < g_threads.size(); ++i) {
                if (g_threads[i] == id) return static_cast<int>(i);
            }
            g_threads.push_back(id);
            return static_cast<int>(g_threads.size()) - 1;
        }

        // Which of our surfaces is up. Cheap enough for an event handler and it
        // is the context that makes a delta readable after the fact.
        const char* MenuState()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return "-";
            const bool grid = ui->IsMenuOpen("GridInventoryMenu");
            const bool wheel = ui->IsMenuOpen("GridWheelerMenu");
            const bool cont = ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME);
            if (wheel && cont)  return "wheel+cont";
            if (wheel)          return "wheel";     // ★the live one: no kPausesGame
            if (grid && cont)   return "grid+cont";
            if (grid)           return "grid";
            if (cont)           return "cont";
            return "none";
        }

        // ★B2: the echo test moved to the LEDGER and this is now a reader of
        // it. Keeping a second copy here would mean two answers to "was this
        // ours", and the one the board acts on has to be the only one.
        const char* ClaimRequest(RE::FormID a_form, std::int32_t a_delta)
        {
            const char* who = Ledger::Confirm(a_form, a_delta);
            if (who) { ++g_matched; return who; }
            ++g_unmatched;
            return "?";
        }
    }

    bool Enabled() { return g_on; }

    void SetEnabled(bool a_on)
    {
        g_on = a_on;
        if (a_on) logger::info("[DELTA] observation ON (B0)");
    }

    void OnContainer(const RE::TESContainerChangedEvent* a_event)
    {
        if (!g_on || !a_event) return;
        // Only what crosses the player's own boundary changes our board.
        const bool in = a_event->newContainer == kPlayerRef;
        const bool out = a_event->oldContainer == kPlayerRef;
        if (!in && !out) return;

        const std::int32_t signed_ = in ? a_event->itemCount : -a_event->itemCount;
        const char*        menu = MenuState();   // touches no state of ours

        std::lock_guard lk(g_mtx);
        const int           slot = ThreadSlot();   // ★mutates g_threads: lock first
        const std::uint64_t n = ++g_seq;
        ++g_events;
        g_shadow[a_event->baseObj] += signed_;
        const char* who = ClaimRequest(a_event->baseObj, signed_);

        // ★FormID only -- no LookupByID on an unknown thread. Reconcile puts
        // names on the ones that turn out to matter.
        logger::info("[DELTA] #{} t{} CONT {:08X} {:+d}  {:08X}->{:08X} uid {:04X} "
                     "ref {} menu={} req={}",
            n, slot, a_event->baseObj, signed_,
            a_event->oldContainer, a_event->newContainer, a_event->uniqueID,
            a_event->reference.native_handle(), menu, who);
    }

    void OnEquip(const RE::TESEquipEvent* a_event)
    {
        if (!g_on || !a_event || !a_event->actor) return;
        if (!a_event->actor->IsPlayerRef()) return;

        const char* menu = MenuState();

        std::lock_guard lk(g_mtx);
        const int           slot = ThreadSlot();   // ★mutates g_threads: lock first
        const std::uint64_t n = ++g_seq;
        ++g_events;
        // ★Equipping moves nothing in or out -- shadow is untouched on purpose.
        // What this line is for is §2 row 2: whether the event carries enough
        // to name the UNIT (uniqueID) and not merely the form.
        logger::info("[DELTA] #{} t{} EQUIP {:08X} {} uid {:04X} orig {:08X} menu={}",
            n, slot, a_event->baseObject, a_event->equipped ? "ON " : "OFF",
            a_event->uniqueID, a_event->originalRefr, menu);
    }

    void Reset(const char* a_why)
    {
        if (!g_on) return;
        std::lock_guard lk(g_mtx);
        g_haveBaseline = false;
        g_baseline.clear();
        g_shadow.clear();
        g_events = 0;
        g_matched = g_unmatched = g_ambiguous = 0;
        logger::info("[DELTA] reset ({}) -- next reconcile re-baselines", a_why);
    }

    void Reconcile(const char* a_when)
    {
        if (!g_on) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;   // main menu / loading (원칙 4)

        // The recount. This is the "truth" side of the comparison.
        std::unordered_map<RE::FormID, std::int32_t> actual;
        for (const auto& [obj, count] : player->GetInventoryCounts()) {
            if (obj) actual[obj->GetFormID()] = count;
        }

        std::lock_guard lk(g_mtx);

        // ★First call of the session has nothing to compare against. The test
        // is "have we ever counted", NOT "have any events arrived" -- events
        // reach us before the menu is ever opened (a script handing out gear on
        // load, a follower picking something up), and the earlier version then
        // took the comparison branch against an empty baseline and reported
        // EVERY form the player owns as a mismatch. 206 of them, once.
        if (!g_haveBaseline) {
            g_haveBaseline = true;
            g_baseline = std::move(actual);
            logger::info("[DELTA] baseline @{}: {} forms ({} event(s) before it, "
                         "discarded)", a_when, g_baseline.size(), g_events);
            g_shadow.clear();
            g_events = 0;
            g_matched = g_unmatched = g_ambiguous = 0;
            return;
        }

        // Every form named by ANY of the three maps has to be checked -- a form
        // that arrived from nowhere is exactly the interesting case, and it is
        // in `actual` alone.
        std::vector<RE::FormID> forms;
        forms.reserve(actual.size() + g_shadow.size());
        for (const auto& [f, _] : actual)     forms.push_back(f);
        for (const auto& [f, _] : g_shadow)   if (!actual.contains(f)) forms.push_back(f);
        for (const auto& [f, _] : g_baseline) if (!actual.contains(f) && !g_shadow.contains(f)) forms.push_back(f);

        int bad = 0;
        for (const RE::FormID f : forms) {
            const auto bIt = g_baseline.find(f);
            const auto sIt = g_shadow.find(f);
            const auto aIt = actual.find(f);
            const std::int32_t base = bIt != g_baseline.end() ? bIt->second : 0;
            const std::int32_t delta = sIt != g_shadow.end() ? sIt->second : 0;
            const std::int32_t act = aIt != actual.end() ? aIt->second : 0;
            const std::int32_t predicted = base + delta;
            if (predicted == act) continue;

            ++bad;
            // Names are safe here (main thread) and this is the one place they
            // are worth the lookup.
            const auto* form = RE::TESForm::LookupByID(f);
            const char* name = form ? form->GetName() : "?";
            logger::error("[DELTA] MISMATCH {:08X} '{}': base {} {:+d} = {} but actual {} "
                          "(off by {:+d})",
                f, (name && *name) ? name : "?", base, delta, predicted, act,
                act - predicted);
        }

        logger::info("[DELTA] reconcile @{}: {} events, {} forms, echo matched {} / "
                     "unmatched {} / ambiguous {} -- {}",
            a_when, g_events, forms.size(), g_matched, g_unmatched, g_ambiguous,
            bad == 0 ? "CLEAN" : "SEE MISMATCH ABOVE");

        g_baseline = std::move(actual);
        g_shadow.clear();
        g_events = 0;
        g_matched = g_unmatched = g_ambiguous = 0;
    }
}
