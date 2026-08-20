#include "game/Ledger.h"

#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace FUI::Ledger
{
    namespace
    {
        // ★Promoted to PERMANENT WIRING (was B2 observation, "!ledger = 1").
        // The two-phase slot drop's confirm half only runs through this, so a
        // default-off ledger left gear cells to the rebuild prune and let
        // trash-parked gear leak layout entries forever (REVIEW §E-2 -- "the
        // middle state is the worst of both"). The escape hatch remains:
        // "!ledger = 0" in the ini disables it for a session, for bisecting a
        // report -- never for shipping.
        bool g_on = true;

        // ★Events arrive on arbitrary threads (B0 saw five), so Confirm can be
        // called from anywhere while Submit/Tick run on the main thread.
        std::mutex g_mtx;

        struct Entry
        {
            std::uint32_t form = 0;
            std::int32_t  delta = 0;
            const char*   who = "?";
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            int           frames = 0;
            // ★Appended LAST: the cell this request empties ("" = none). See
            // Submit's a_slot note in the header.
            std::string   slot;
        };
        std::deque<Entry> g_open;

        // Generous on purpose. B0 measured confirmations arriving in the same
        // second, so this is not a latency budget -- it is the point at which
        // "the engine refused and said nothing" becomes the better explanation.
        constexpr int kMaxFrames = 180;

        int      g_simRefuse = 0;   // TEST ONLY, see SimRefuse
        ExpireFn  g_onExpire = nullptr;
        ConfirmFn g_onConfirm = nullptr;
        std::deque<Entry> g_landed;   // confirmed, waiting for the main thread

        // per-session tally, reported by Flush
        std::uint32_t g_submitted = 0;
        std::uint32_t g_confirmed = 0;
        std::uint32_t g_expired = 0;
        std::uint32_t g_surplus = 0;   // events that matched no open request
        std::uint32_t g_ambiguous = 0; // events with 2+ (form, direction) candidates
    }

    bool Enabled() { return g_on; }

    void SetEnabled(bool a_on)
    {
        g_on = a_on;
        // ON is the default and not worth a line; OFF is the unusual state
        // and the log has to say the safety net is gone.
        if (!a_on) logger::warn("[LEDGER] ★request ledger DISABLED (!ledger = 0) "
                                "-- slot drops fall back to the rebuild prune");
    }

    void SetOnExpire(ExpireFn a_fn) { g_onExpire = a_fn; }
    void SetOnConfirm(ConfirmFn a_fn) { g_onConfirm = a_fn; }

    void Submit(std::uint32_t a_form, std::int32_t a_delta, const char* a_who,
                std::uint16_t a_uid, std::uint16_t a_sig, const std::string& a_slot)
    {
        if (!g_on || a_delta == 0) return;
        std::lock_guard lk(g_mtx);
        // Field-wise on purpose (§10-6): this struct has grown once already.
        Entry e;
        e.form = a_form;
        e.delta = a_delta;
        e.who = a_who ? a_who : "?";
        e.uid = a_uid;
        e.sig = a_sig;
        e.slot = a_slot;
        g_open.push_back(std::move(e));
        ++g_submitted;
    }

    const char* Confirm(std::uint32_t a_form, std::int32_t a_delta)
    {
        if (!g_on) return nullptr;
        std::lock_guard lk(g_mtx);
        // ★B-1: HOW OFTEN is the pick a guess? When two or more open requests
        // share (form, direction), FIFO chooses the oldest and cannot know it
        // chose right. §8-2 reported this as "ambiguous 0" from a counter that
        // was never incremented -- the number below is the measured one, and
        // the ledger measures it because the ledger does the matching. It runs
        // in every session now (the ledger is permanent wiring), so the tail
        // case B0 never produced naturally gets counted whenever it happens.
        int candidates = 0;
        for (const auto& e : g_open) {
            if (e.form == a_form && (e.delta < 0) == (a_delta < 0)) ++candidates;
        }
        if (candidates >= 2) {
            ++g_ambiguous;
            logger::warn("[LEDGER] ★ambiguous confirm: {:08X} {:+d} matches {} open "
                         "request(s) -- FIFO takes the oldest",
                a_form, a_delta, candidates);
        }
        // ★OLDEST match wins, and the count is deliberately not compared: the
        // engine splits and merges stacks on its own, so requiring the numbers
        // to agree would reject confirmations that are perfectly real.
        for (auto it = g_open.begin(); it != g_open.end(); ++it) {
            if (it->form != a_form) continue;
            if ((it->delta < 0) != (a_delta < 0)) continue;
            const char* who = it->who;
            g_landed.push_back(*it);   // handed to the main thread in Tick
            g_open.erase(it);
            ++g_confirmed;
            return who;
        }
        // ★Not an error. The second half of a consumable pair lands here every
        // single time (§8-3), and so does every genuine outside delta. Telling
        // those two apart is B3's problem; counting them is B2's.
        ++g_surplus;
        return nullptr;
    }

    std::map<std::uint32_t, int> OpenOutgoing()
    {
        std::map<std::uint32_t, int> out;
        if (!g_on) return out;
        std::lock_guard lk(g_mtx);
        for (const auto& e : g_open) {
            // "use" is the equip queue's jurisdiction -- see the header
            if (e.delta < 0 && (!e.who || std::strcmp(e.who, "use") != 0)) {
                out[e.form] += -e.delta;
            }
        }
        return out;
    }

    std::vector<Expired> Cancel(std::uint32_t a_form, int a_count, const char* a_why)
    {
        std::vector<Expired> out;
        if (!g_on || a_count <= 0) return out;
        std::lock_guard lk(g_mtx);
        int left = a_count;
        for (auto it = g_open.begin(); it != g_open.end() && left > 0;) {
            if (it->form != a_form || it->delta >= 0 ||
                (it->who && std::strcmp(it->who, "use") == 0)) {
                ++it;
                continue;
            }
            const int units = -it->delta;
            const int take = (std::min)(units, left);
            Expired e;
            e.form  = it->form;
            e.delta = -take;
            e.who   = it->who;
            e.uid   = it->uid;
            e.sig   = it->sig;
            e.slot  = it->slot;
            out.push_back(std::move(e));
            left -= take;
            if (take == units) {
                it = g_open.erase(it);
            } else {
                it->delta += take;   // partial: the rest of the request stands
                ++it;
            }
        }
        if (!out.empty()) {
            logger::info("[LEDGER] cancelled {} entr{} of {:08X} ({})",
                out.size(), out.size() == 1 ? "y" : "ies", a_form, a_why);
        }
        return out;
    }

    void Tick()
    {
        if (!g_on) return;
        std::vector<Entry> fired;
        std::vector<Entry> landed;
        {
            std::lock_guard lk(g_mtx);
            landed.assign(g_landed.begin(), g_landed.end());
            g_landed.clear();
            for (auto& e : g_open) ++e.frames;
            // The queue is ordered by age, so expiry only comes off the front.
            while (!g_open.empty() && g_open.front().frames > kMaxFrames) {
                fired.push_back(g_open.front());
                g_open.pop_front();
                ++g_expired;
            }
        }
        // ★Outside the lock. The handler asks the grid to rebuild, and holding
        // our mutex across someone else's call is how a deadlock gets built by
        // accident -- especially with Confirm arriving on other threads.
        for (const auto& e : landed) {
            if (g_onConfirm) g_onConfirm(Expired{ e.form, e.delta, e.who, e.uid, e.sig, e.slot });
        }
        for (const auto& e : fired) {
            logger::warn("[LEDGER] ★expired: {:08X} {:+d} '{}' uid {:04X} sig {:04X} "
                         "slot '{}' -- {} frames, never confirmed",
                e.form, e.delta, e.who, e.uid, e.sig, e.slot, kMaxFrames);
            if (g_onExpire) g_onExpire(Expired{ e.form, e.delta, e.who, e.uid, e.sig, e.slot });
        }
    }

    void Flush(const char* a_why)
    {
        if (!g_on) return;
        std::vector<Entry> landed;
        std::vector<Entry> orphaned;
        {
            std::lock_guard lk(g_mtx);
            landed.assign(g_landed.begin(), g_landed.end());
            g_landed.clear();
            orphaned.assign(g_open.begin(), g_open.end());
            g_open.clear();
            logger::info("[LEDGER] @{}: submitted {} / confirmed {} / expired {} / "
                         "outstanding {} -- surplus {} / ambiguous {}",
                a_why, g_submitted, g_confirmed, g_expired, orphaned.size(),
                g_surplus, g_ambiguous);
            g_submitted = g_confirmed = g_expired = g_surplus = g_ambiguous = 0;
        }
        // ★Outside the lock, same reasoning as Tick. Confirmations that landed
        // while no menu was ticking are DELIVERED, not dropped -- the engine
        // did take those items, so their cells must still be committed. What
        // was never confirmed EXPIRES through the same hook as a timeout: the
        // silent clear this used to be left the request's slot key queued
        // forever, waiting to be consumed by someone else's confirmation.
        for (const auto& e : landed) {
            if (g_onConfirm) g_onConfirm(Expired{ e.form, e.delta, e.who, e.uid, e.sig, e.slot });
        }
        for (const auto& e : orphaned) {
            logger::warn("[LEDGER] outstanding at {}: {:08X} {:+d} '{}' "
                         "uid {:04X} sig {:04X} slot '{}' ({} frames) -- expiring",
                a_why, e.form, e.delta, e.who, e.uid, e.sig, e.slot, e.frames);
            if (g_onExpire) g_onExpire(Expired{ e.form, e.delta, e.who, e.uid, e.sig, e.slot });
        }
    }

    bool SimRefuse()
    {
        // ★Gated on the ledger: without it the request is never on the books,
        // so skipping the engine call would have NO recovery path at all --
        // that is a state the game cannot produce (§10-7), not a test.
        if (!g_on) return false;
        std::lock_guard lk(g_mtx);
        if (g_simRefuse <= 0) return false;
        --g_simRefuse;
        logger::warn("[LEDGER] !simrefuse armed: {} left after this one", g_simRefuse);
        return true;
    }

    void SetSimRefuse(int a_count)
    {
        std::lock_guard lk(g_mtx);
        g_simRefuse = a_count;
        if (a_count > 0 && !g_on) {
            logger::warn("[LEDGER] !simrefuse ignored -- the ledger is disabled "
                         "(!ledger = 0) and a refusal without a ledger entry has "
                         "no recovery path");
            g_simRefuse = 0;
            return;
        }
        if (a_count > 0) {
            logger::warn("[LEDGER] ★!simrefuse = {} -- the next {} store request(s) "
                         "will NOT reach the engine", a_count, a_count);
        }
    }

    void Reset(const char* a_why)
    {
        // Unconditional: clearing an empty ledger is free, and a boundary
        // reset that depends on a switch is rule 6's next victim.
        std::lock_guard lk(g_mtx);
        g_open.clear();
        g_landed.clear();
        g_submitted = g_confirmed = g_expired = g_surplus = g_ambiguous = 0;
        logger::info("[LEDGER] reset ({})", a_why);
    }
}
