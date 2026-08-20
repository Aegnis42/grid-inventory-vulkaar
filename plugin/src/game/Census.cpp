#include "game/Census.h"

#include "ui/Grid.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace FUI::Census
{
    namespace
    {
        bool g_on = false;

        // One row per (form, sig). The raw values ride along because the §3
        // pairing rule is stated in terms of distance, and a hash has none.
        struct Row
        {
            RE::FormID    form = 0;
            std::uint16_t sig = 0;
            int           count = 0;
            float         health = 0.0f;   // ExtraHealth (temper)
            float         charge = 0.0f;   // ExtraCharge
            int           poison = 0;      // ExtraPoison, remaining uses
            std::uint32_t soul = 0;        // ExtraSoul grade
        };

        using Key = std::pair<RE::FormID, std::uint16_t>;
        std::map<Key, Row> g_prev;
        bool               g_have = false;

        // Everything the sig hashes that has a MAGNITUDE. Read straight off the
        // list so the census and the signature cannot describe different things.
        void ReadValues(RE::ExtraDataList* a_xl, Row& a_row)
        {
            if (!a_xl) return;
            if (const auto* x = a_xl->GetByType<RE::ExtraHealth>()) a_row.health = x->health;
            if (const auto* x = a_xl->GetByType<RE::ExtraCharge>()) a_row.charge = x->charge;
            if (const auto* x = a_xl->GetByType<RE::ExtraPoison>()) a_row.poison = static_cast<int>(x->count);
            if (const auto* x = a_xl->GetByType<RE::ExtraSoul>())   a_row.soul = static_cast<std::uint32_t>(x->soul.get());
        }

        std::map<Key, Row> Snapshot()
        {
            std::map<Key, Row> out;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return out;   // 원칙 4

            for (auto& [obj, pair] : player->GetInventory()) {
                const int count = pair.first;
                if (!obj || count <= 0) continue;
                if (obj->IsGold()) continue;   // a ledger, not a kind

                const RE::FormID form = obj->GetFormID();
                auto* entry = pair.second.get();

                // ★plain = count - SUM(list counts). The same identity the grid
                // itself uses; deriving it any other way invites the two to
                // disagree about the very thing being audited.
                int listed = 0;
                if (entry && entry->extraLists) {
                    for (auto* xl : *entry->extraLists) {
                        if (!xl) continue;
                        const int n = (std::max)(1, xl->GetCount());
                        listed += n;
                        Row r;
                        r.form = form;
                        r.sig = Grid::InstanceSigOf(xl);
                        ReadValues(xl, r);
                        auto& slot = out[{ form, r.sig }];
                        if (slot.form == 0) slot = r;   // first of its kind: take its values
                        slot.count += n;
                    }
                }
                const int plain = count - listed;
                if (plain > 0) {
                    auto& slot = out[{ form, std::uint16_t{ 0 } }];
                    slot.form = form;
                    slot.sig = 0;
                    slot.count += plain;
                }
            }
            return out;
        }

        const char* NameOf(RE::FormID a_form)
        {
            const auto* f = RE::TESForm::LookupByID(a_form);
            const char* n = f ? f->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }

        // ★§8-4's own verdict, finally in the code it was measured with: the
        // real signal is not the SIZE of a distance but WHICH AXES moved. A
        // pair that changed one axis is the plausible relabel; a pair that
        // grew an axis from nothing is two different items. The primary key
        // is therefore the changed-axis count, and the scalar below is only
        // the tiebreak within it.
        int ChangedAxes(const Row& a, const Row& b)
        {
            int n = 0;
            if (std::fabs(a.health - b.health) > 0.001f) ++n;
            if (std::fabs(a.charge - b.charge) > 0.001f) ++n;
            if (a.poison != b.poison) ++n;
            if (a.soul != b.soul) ++n;
            return n;
        }

        // Tiebreak scalar. ★Charge is NORMALISED: enchant charges run in the
        // hundreds-to-thousands while temper is a ~1.x multiplier, so a raw
        // |Δcharge| drowned every other axis in exactly the case §5-2 named
        // as this rule's natural habitat -- several weapons draining in
        // combat (REVIEW B-2). A fraction of the larger charge keeps every
        // axis on the same ~0-100 scale.
        float Distance(const Row& a, const Row& b)
        {
            const float chMax = (std::max)({ std::fabs(a.charge),
                                             std::fabs(b.charge), 1.0f });
            return std::fabs(a.health - b.health) * 100.0f +
                   std::fabs(a.charge - b.charge) / chMax * 100.0f +
                   static_cast<float>(std::abs(a.poison - b.poison)) * 10.0f +
                   (a.soul == b.soul ? 0.0f : 50.0f);
        }

        std::string Describe(const Row& r)
        {
            return std::format("sig {:04X} hp {:.2f} ch {:.1f} po {} soul {}",
                r.sig, r.health, r.charge, r.poison, r.soul);
        }
    }

    bool Enabled() { return g_on; }

    void SetEnabled(bool a_on)
    {
        g_on = a_on;
        if (a_on) logger::info("[CENSUS] kind audit ON (B1)");
    }

    void Reset(const char* a_why)
    {
        if (!g_on) return;
        g_prev.clear();
        g_have = false;
        logger::info("[CENSUS] reset ({}) -- next take re-baselines", a_why);
    }

    void Take(const char* a_when)
    {
        if (!g_on) return;
        auto now = Snapshot();
        if (now.empty()) return;

        if (!g_have) {
            g_have = true;
            g_prev = std::move(now);
            logger::info("[CENSUS] baseline @{}: {} kinds", a_when, g_prev.size());
            return;
        }

        // Group the differences by FORM: a kind only ever turns into another
        // kind of the SAME form, so that is the widest set a pairing has to
        // consider.
        std::map<RE::FormID, std::vector<Row>> gone, appeared;
        for (const auto& [k, prev] : g_prev) {
            const auto it = now.find(k);
            const int after = it == now.end() ? 0 : it->second.count;
            if (after < prev.count) {
                Row r = prev;
                r.count = prev.count - after;
                gone[k.first].push_back(r);
            }
        }
        for (const auto& [k, cur] : now) {
            const auto it = g_prev.find(k);
            const int before = it == g_prev.end() ? 0 : it->second.count;
            if (cur.count > before) {
                Row r = cur;
                r.count = cur.count - before;
                appeared[k.first].push_back(r);
            }
        }

        int forms = 0, unique = 0, ambiguous = 0, netOnly = 0;
        for (const auto& [form, lost] : gone) {
            const auto ait = appeared.find(form);
            if (ait == appeared.end()) {
                // nothing of this form appeared: a plain loss (used, sold,
                // dropped). Not a relabel, nothing to pair.
                ++netOnly;
                continue;
            }
            ++forms;
            const auto& made = ait->second;

            if (lost.size() == 1 && made.size() == 1) {
                ++unique;
                logger::info("[CENSUS]   {:08X} '{}': {} -> {}  (x{}, d={:.1f}) UNIQUE",
                    form, NameOf(form), Describe(lost[0]), Describe(made[0]),
                    lost[0].count, Distance(lost[0], made[0]));
                continue;
            }

            // ★★This is the case REVIEW B-2 said would come back, and here it
            // is. Candidates are RANKED by the §8-4 rule -- fewest changed
            // axes first, normalised distance as the tiebreak -- and the top
            // pick is marked, so when N→M finally happens in the wild the log
            // shows what the rule would have chosen, judged on real numbers.
            ++ambiguous;
            logger::warn("[CENSUS]   {:08X} '{}': ★{} gone x {} appeared -- AMBIGUOUS",
                form, NameOf(form), lost.size(), made.size());
            for (const auto& l : lost) {
                struct Cand { const Row* m; int axes; float dist; };
                std::vector<Cand> cands;
                cands.reserve(made.size());
                for (const auto& m : made) {
                    cands.push_back({ &m, ChangedAxes(l, m), Distance(l, m) });
                }
                std::sort(cands.begin(), cands.end(),
                    [](const Cand& x, const Cand& y) {
                        if (x.axes != y.axes) return x.axes < y.axes;
                        return x.dist < y.dist;
                    });
                for (std::size_t c = 0; c < cands.size(); ++c) {
                    logger::warn("[CENSUS]       {} -> {}  axes={} d={:.1f}{}",
                        Describe(l), Describe(*cands[c].m), cands[c].axes,
                        cands[c].dist, c == 0 ? "  <- rule picks" : "");
                }
            }
        }

        // Forms where something appeared but nothing went away are ordinary
        // acquisitions; they need no pairing and would only be noise.
        logger::info("[CENSUS] @{}: {} kinds, relabels on {} form(s) -- "
                     "{} unique / ★{} ambiguous, {} plain loss",
            a_when, now.size(), forms, unique, ambiguous, netOnly);

        g_prev = std::move(now);
    }
}
