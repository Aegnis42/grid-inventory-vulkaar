#include "game/MonnaiesVulkaar.h"

#include <array>

namespace FUI::MonnaiesVulkaar
{
    namespace
    {
        // ── LES TROIS ENREGISTREMENTS ─────────────────────────────────────
        //
        // Identifiants LOCAUX, jamais complets. L'octet de poids fort d'un
        // formId complet est le rang du greffon dans l'ordre de chargement :
        // il vaut 5 aujourd'hui et changera au premier mod ajouté avant nous.
        // `LookupForm(local, greffon)` fait la résolution à l'exécution — c'est
        // exactement ce que fait GoldCoins pour ses propres pièces, et c'est la
        // seule façon correcte.
        //
        // Le registre de ces plages vit dans docs/plages-identifiants.md du
        // dépôt vulkaar. Ne pas les changer : un formId de base est gravé à vie
        // dans le changeForm de chaque personnage qui porte l'objet.
        constexpr const char* kGreffon = "vulkaar_accueil.esp";
        constexpr RE::FormID  kPremier = 0x33F74;   // Septime, puis Mède, Titus

        std::array<RE::TESBoundObject*, kNb> g_formes{};
        std::array<int, kNb>                 g_comptes{};
        bool                                 g_pret = false;
    }

    void InitForms()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        for (int i = 0; i < kNb; ++i) {
            g_formes[i] = dh->LookupForm<RE::TESObjectMISC>(kPremier + i, kGreffon);
        }
        g_pret = g_formes[0] && g_formes[1] && g_formes[2];

        if (g_pret) {
            SKSE::log::info("[VULKAAR] monnaies resolues ({}) : Septime={:08X} Mede={:08X} Titus={:08X}",
                kGreffon, g_formes[0]->GetFormID(), g_formes[1]->GetFormID(),
                g_formes[2]->GetFormID());
        } else {
            // INFO, pas WARN : quelqu'un qui joue à ce mod sans vulkaar n'a
            // rien fait de mal, et un avertissement rouge lui ferait croire à
            // une installation cassée.
            SKSE::log::info("[VULKAAR] '{}' absent — bourse desactivee, le mod se comporte normalement",
                kGreffon);
        }
    }

    bool Pret() { return g_pret; }

    int Rang(RE::FormID a_id)
    {
        if (!g_pret) return -1;
        for (int i = 0; i < kNb; ++i) {
            if (g_formes[i] && g_formes[i]->GetFormID() == a_id) return i;
        }
        return -1;
    }

    bool EstMonnaie(RE::FormID a_id) { return Rang(a_id) >= 0; }

    bool EstMonnaie(RE::TESBoundObject* a_obj)
    {
        return a_obj && EstMonnaie(a_obj->GetFormID());
    }

    RE::TESBoundObject* Forme(int a_rang)
    {
        if (a_rang < 0 || a_rang >= kNb) return nullptr;
        return g_formes[a_rang];
    }

    void RemettreComptesAZero() { g_comptes.fill(0); }

    bool NoterSiMonnaie(RE::TESBoundObject* a_obj, int a_compte)
    {
        const int rang = a_obj ? Rang(a_obj->GetFormID()) : -1;
        if (rang < 0) return false;
        // On ADDITIONNE au lieu d'affecter : le moteur peut rendre plusieurs
        // entrées pour une même forme quand certaines portent une ExtraDataList
        // (une pièce volée, une pièce d'un conteneur). Écraser ne montrerait
        // que la dernière, et le joueur croirait avoir perdu de l'argent.
        g_comptes[rang] += a_compte;
        return true;
    }

    int Compte(int a_rang)
    {
        if (a_rang < 0 || a_rang >= kNb) return 0;
        return g_comptes[a_rang];
    }
}
