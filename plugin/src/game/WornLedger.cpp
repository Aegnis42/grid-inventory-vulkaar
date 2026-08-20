#include "game/WornLedger.h"

#include <map>

namespace FUI::WornLedger
{
    namespace
    {
        // worn STACKS per form -- see the header for why lists, not units
        std::map<RE::FormID, int> g_worn;
        bool                      g_have = false;

        bool Tracked(RE::TESBoundObject* a_obj)
        {
            // The equip event also announces spells, shouts and scroll casts;
            // the ledger is about the things the BOARD accounts for.
            return a_obj && (a_obj->Is(RE::FormType::Armor) ||
                             a_obj->Is(RE::FormType::Weapon) ||
                             a_obj->Is(RE::FormType::Light) ||
                             a_obj->Is(RE::FormType::Ammo));
        }

        std::map<RE::FormID, int> EngineWalk()
        {
            std::map<RE::FormID, int> out;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return out;   // 원칙 4
            for (auto& [obj, pair] : player->GetInventory()) {
                if (!Tracked(obj) || pair.first <= 0) continue;
                auto* entry = pair.second.get();
                if (!entry || !entry->extraLists) continue;
                int n = 0;
                for (auto* xl : *entry->extraLists) {
                    if (xl && (xl->HasType<RE::ExtraWorn>() ||
                               xl->HasType<RE::ExtraWornLeft>())) {
                        ++n;
                    }
                }
                if (n > 0) out[obj->GetFormID()] = n;
            }
            return out;
        }

        const char* NameOf(RE::FormID a_form)
        {
            const auto* f = RE::TESForm::LookupByID(a_form);
            const char* n = f ? f->GetName() : nullptr;
            return (n && *n) ? n : "?";
        }
    }

    void OnEquip(RE::FormID a_form)
    {
        if (!g_have) return;   // pre-baseline events describe nobody
        auto* form = RE::TESForm::LookupByID(a_form);
        if (!Tracked(form ? form->As<RE::TESBoundObject>() : nullptr)) return;
        ++g_worn[a_form];
    }

    void OnUnequip(RE::FormID a_form)
    {
        if (!g_have) return;
        auto* form = RE::TESForm::LookupByID(a_form);
        if (!Tracked(form ? form->As<RE::TESBoundObject>() : nullptr)) return;
        const auto it = g_worn.find(a_form);
        if (it == g_worn.end()) {
            // an unequip the ledger never saw go on -- itself a finding, and
            // exactly what this phase exists to surface
            logger::warn("[WORN] unequip of {:08X} '{}' the ledger never saw "
                         "worn", a_form, NameOf(a_form));
            return;
        }
        if (--it->second <= 0) g_worn.erase(it);
    }

    void Rebaseline(const char* a_why)
    {
        g_worn = EngineWalk();
        g_have = true;
        logger::info("[WORN] rebaseline ({}) -- {} worn form(s)", a_why,
                     g_worn.size());
    }

    void Audit(const char* a_when)
    {
        if (!g_have) {
            Rebaseline(a_when);
            return;
        }
        const auto engine = EngineWalk();
        int bad = 0;
        for (const auto& [f, n] : engine) {
            const auto it = g_worn.find(f);
            const int  mine = it == g_worn.end() ? 0 : it->second;
            if (mine != n) {
                ++bad;
                logger::warn("[WORN] @{} MISMATCH {:08X} '{}': ledger {} vs "
                             "engine {}", a_when, f, NameOf(f), mine, n);
            }
        }
        for (const auto& [f, n] : g_worn) {
            if (!engine.contains(f)) {
                ++bad;
                logger::warn("[WORN] @{} MISMATCH {:08X} '{}': ledger {} vs "
                             "engine 0", a_when, f, NameOf(f), n);
            }
        }
        if (bad == 0) {
            logger::info("[WORN] @{} ok -- {} worn form(s) agree", a_when,
                         engine.size());
        } else {
            // observation mode: the engine is still the authority. Bend, so
            // the next divergence is counted from a clean baseline instead of
            // echoing this one.
            g_worn = engine;
            logger::warn("[WORN] @{} ★{} mismatch(es) -- ledger bent to the "
                         "engine (observation mode)", a_when, bad);
        }
    }
}
