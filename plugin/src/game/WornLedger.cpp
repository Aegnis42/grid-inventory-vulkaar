#include "game/WornLedger.h"

#include "ui/Grid.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <vector>

namespace FUI::WornLedger
{
    namespace
    {
        enum class State { pending, worn };

        struct Entry
        {
            RE::FormID    form = 0;
            std::uint16_t uid = 0;
            std::uint16_t sig = 0;
            int           hand = 0;    // 0 none / 1 right / 2 left
            int           units = 1;   // what the action moved (data only)
            State         state = State::pending;
            std::chrono::steady_clock::time_point when{};
        };
        std::vector<Entry> g_entries;
        bool               g_have = false;

        bool Tracked(RE::TESBoundObject* a_obj)
        {
            // The equip event also announces spells, shouts and scroll casts;
            // the ledger is about the things the BOARD accounts for.
            return a_obj && (a_obj->Is(RE::FormType::Armor) ||
                             a_obj->Is(RE::FormType::Weapon) ||
                             a_obj->Is(RE::FormType::Light) ||
                             a_obj->Is(RE::FormType::Ammo));
        }

        bool TrackedForm(RE::FormID a_form)
        {
            auto* form = RE::TESForm::LookupByID(a_form);
            return Tracked(form ? form->As<RE::TESBoundObject>() : nullptr);
        }

        // engine truth: worn LISTS per form, with the identity read off each
        // list so a rebaseline starts with real entries, not blanks
        std::vector<Entry> EngineWalk()
        {
            std::vector<Entry> out;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return out;   // 원칙 4
            for (auto& [obj, pair] : player->GetInventory()) {
                if (!Tracked(obj) || pair.first <= 0) continue;
                auto* entry = pair.second.get();
                if (!entry || !entry->extraLists) continue;
                for (auto* xl : *entry->extraLists) {
                    if (!xl) continue;
                    const bool wornL = xl->HasType<RE::ExtraWornLeft>();
                    if (!wornL && !xl->HasType<RE::ExtraWorn>()) continue;
                    Entry e;
                    e.form = obj->GetFormID();
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) {
                        e.uid = xu->uniqueID;
                    }
                    e.sig   = Grid::InstanceSigOf(xl);
                    e.hand  = wornL ? 2 : 1;
                    e.units = (std::max)(1, static_cast<int>(xl->GetCount()));
                    e.state = State::worn;
                    out.push_back(std::move(e));
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

        std::map<RE::FormID, int> CountByForm(const std::vector<Entry>& a_v,
                                              State a_state)
        {
            std::map<RE::FormID, int> out;
            for (const auto& e : a_v) {
                if (e.state == a_state) ++out[e.form];
            }
            return out;
        }
    }

    void NotePending(RE::FormID a_form, std::uint16_t a_uid, std::uint16_t a_sig,
                     int a_hand, int a_units)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        Entry e;
        e.form  = a_form;
        e.uid   = a_uid;
        e.sig   = a_sig;
        e.hand  = a_hand;
        e.units = (std::max)(1, a_units);
        e.state = State::pending;
        e.when  = std::chrono::steady_clock::now();
        g_entries.push_back(std::move(e));
    }

    void OnEquip(RE::FormID a_form)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        // Our own request first -- the pending entry carries the identity the
        // event cannot (rule 2). The event names no hand either, so form is
        // the whole key; with two pendings of one form the oldest lands
        // first, which is also the order the engine ran them.
        for (auto& e : g_entries) {
            if (e.form == a_form && e.state == State::pending) {
                e.state = State::worn;
                e.when  = std::chrono::steady_clock::now();
                return;
            }
        }
        // Nobody asked: the engine wore it on its own authority (a loadout
        // apply, a script, vanilla favourites). A real entry, identity
        // unknown -- exactly what rule 2 predicts an event can carry.
        Entry e;
        e.form  = a_form;
        e.state = State::worn;
        e.when  = std::chrono::steady_clock::now();
        g_entries.push_back(std::move(e));
    }

    void OnUnequip(RE::FormID a_form)
    {
        if (!g_have || !TrackedForm(a_form)) return;
        // Retire one worn entry of the form. The event cannot name which; the
        // OLDEST goes, mirroring OnEquip's order so a same-form pair cycles
        // instead of starving one entry.
        for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
            if (it->form == a_form && it->state == State::worn) {
                g_entries.erase(it);
                return;
            }
        }
        // ...a pending that got unequipped before its equip event would be a
        // genuine finding; so is an unequip of something never seen worn.
        logger::warn("[WORN] unequip of {:08X} '{}' the ledger never saw worn",
                     a_form, NameOf(a_form));
    }

    void Rebaseline(const char* a_why)
    {
        g_entries = EngineWalk();
        g_have = true;
        logger::info("[WORN] rebaseline ({}) -- {} worn list(s)", a_why,
                     g_entries.size());
    }

    void Audit(const char* a_when)
    {
        if (!g_have) {
            Rebaseline(a_when);
            return;
        }
        const auto engine = EngineWalk();
        const auto eByForm = CountByForm(engine, State::worn);
        const auto lByForm = CountByForm(g_entries, State::worn);

        int bad = 0;
        for (const auto& [f, n] : eByForm) {
            const auto it = lByForm.find(f);
            const int  mine = it == lByForm.end() ? 0 : it->second;
            if (mine != n) {
                ++bad;
                logger::warn("[WORN] @{} MISMATCH {:08X} '{}': ledger {} vs "
                             "engine {}", a_when, f, NameOf(f), mine, n);
            }
        }
        for (const auto& [f, n] : lByForm) {
            if (!eByForm.contains(f)) {
                ++bad;
                logger::warn("[WORN] @{} MISMATCH {:08X} '{}': ledger {} vs "
                             "engine 0", a_when, f, NameOf(f), n);
            }
        }

        // lifecycle residue: a pending that never met its event. In-flight
        // ones are legitimate for a few frames; an old one means an equip
        // request the engine silently refused (or an event we missed) and
        // must not sit in the books forever.
        const auto now = std::chrono::steady_clock::now();
        int stuck = 0;
        std::erase_if(g_entries, [&](const Entry& e) {
            if (e.state != State::pending) return false;
            if (now - e.when < std::chrono::seconds(2)) return false;
            ++stuck;
            logger::warn("[WORN] @{} ★stale pending {:08X} '{}' (uid {:04X} "
                         "sig {:04X} hand {}) -- dropped", a_when, e.form,
                         NameOf(e.form), e.uid, e.sig, e.hand);
            return true;
        });

        if (bad == 0 && stuck == 0) {
            logger::info("[WORN] @{} ok -- {} worn list(s) agree", a_when,
                         engine.size());
        }
        if (bad > 0) {
            // observation mode: the engine is still the authority. Bend, so
            // the next divergence is counted from a clean baseline instead of
            // echoing this one.
            g_entries = engine;
            logger::warn("[WORN] @{} ★{} mismatch(es) -- ledger bent to the "
                         "engine (observation mode)", a_when, bad);
        }
    }
}
