#include "game/Durabilite.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

// Voir Durabilite.h pour l'architecture. Ici : la lecture de la table, la
// résolution des idLocaux, et les deux formules de l'échelle.

namespace FUI::Durabilite
{
    namespace
    {
        constexpr const char* kChemin = "Data/SKSE/Plugins/GridInventory_durabilite.txt";
        /* LE GREFFON EST FIXÉ PAR LE CONTRAT (§5.5), pas lu dans l'en-tête :
           l'en-tête le NOMME pour qu'une table d'un autre esp soit refusée,
           jamais résolue contre le mauvais ordre de chargement. */
        constexpr const char* kGreffon = "vulkaar_forge.esp";

        std::unordered_map<RE::FormID, std::uint32_t> g_max;
        bool                                          g_prete = false;

        void Rogner(std::string& a_s)
        {
            while (!a_s.empty() &&
                   (a_s.back() == '\r' || a_s.back() == '\n' || a_s.back() == ' ' || a_s.back() == '\t'))
                a_s.pop_back();
        }

        /** Le mot suivant d'une ligne séparée par des blancs (l'en-tête). */
        std::string Mot(const std::string& a_s, std::size_t& a_pos)
        {
            while (a_pos < a_s.size() && (a_s[a_pos] == ' ' || a_s[a_pos] == '\t')) ++a_pos;
            const std::size_t debut = a_pos;
            while (a_pos < a_s.size() && a_s[a_pos] != ' ' && a_s[a_pos] != '\t') ++a_pos;
            return a_s.substr(debut, a_pos - debut);
        }
    }

    void Initialiser()
    {
        g_max.clear();
        g_prete = false;

        std::ifstream f(kChemin);
        if (!f) {
            // INFO, pas WARN : sans vulkaar il n'y a pas de table, et le mod se
            // comporte comme avant (même raison que MonnaiesVulkaar).
            logger::info("[DURABILITE] pas de {} — la grille garde la ligne de trempe", kChemin);
            return;
        }

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            logger::warn("[DURABILITE] TESDataHandler absent a kDataLoaded — table ignoree");
            return;
        }

        std::string ligne;
        // ── l'en-tête : « # vulkaar_forge.esp <sha court> <nombre de lignes> »
        std::string greffon, sha;
        unsigned long annonce = 0;
        if (std::getline(f, ligne)) {
            Rogner(ligne);
            std::size_t pos = 0;
            const std::string diese = Mot(ligne, pos);
            if (diese == "#") {
                greffon = Mot(ligne, pos);
                sha = Mot(ligne, pos);
                annonce = std::strtoul(Mot(ligne, pos).c_str(), nullptr, 10);
            } else if (!diese.empty() && diese[0] == '#') {
                // « #vulkaar_forge.esp » collé : on tolère l'absence d'espace.
                greffon = diese.substr(1);
                sha = Mot(ligne, pos);
                annonce = std::strtoul(Mot(ligne, pos).c_str(), nullptr, 10);
            }
        }
        if (greffon.empty()) {
            logger::warn("[DURABILITE] {} : premiere ligne sans en-tete « # <esp> <sha> <n> » — table ignoree",
                         kChemin);
            return;
        }
        if (_stricmp(greffon.c_str(), kGreffon) != 0) {
            logger::warn("[DURABILITE] {} : l'en-tete nomme « {} », la table attendue est celle de « {} » — ignoree",
                         kChemin, greffon, kGreffon);
            return;
        }

        // ── le corps : « <idLocalHex6> TAB <max> », une forme par ligne
        std::size_t lues = 0, resolues = 0, malformees = 0;
        while (std::getline(f, ligne)) {
            Rogner(ligne);
            if (ligne.empty() || ligne[0] == '#') continue;
            const std::size_t tab = ligne.find('\t');
            if (tab == std::string::npos) {
                ++malformees;
                continue;
            }
            char* bout = nullptr;
            const unsigned long idLocal = std::strtoul(ligne.c_str(), &bout, 16);
            if (bout != ligne.c_str() + tab) {   // des caractères qui ne sont pas de l'hexa
                ++malformees;
                continue;
            }
            const unsigned long max = std::strtoul(ligne.c_str() + tab + 1, &bout, 10);
            if (bout == ligne.c_str() + tab + 1 || max == 0) {
                ++malformees;
                continue;
            }
            ++lues;
            const RE::FormID id = dh->LookupFormID(static_cast<RE::FormID>(idLocal), kGreffon);
            if (id == 0) continue;   // l'esp n'est pas chargé, ou l'idLocal n'y est pas
            g_max[id] = static_cast<std::uint32_t>(max);
            ++resolues;
        }

        g_prete = resolues > 0;
        logger::info("[DURABILITE] {} formes lues, {} resolues ({} annoncee(s), sha {}, {} ligne(s) malformee(s))",
                     lues, resolues, annonce, sha.empty() ? "?" : sha, malformees);
        if (annonce != 0 && annonce != lues) {
            // Le nombre annoncé ne colle pas : fichier tronqué à la copie, ou
            // générateur qui a changé de format. La table reste utilisable, mais
            // il faut que quelqu'un le voie.
            logger::warn("[DURABILITE] l'en-tete annonce {} ligne(s), {} lue(s) — table partielle ?", annonce, lues);
        }
        if (lues > 0 && resolues == 0) {
            logger::info("[DURABILITE] « {} » absent de l'ordre de chargement — aucune forme resolue", kGreffon);
        }
    }

    bool Prete() { return g_prete; }

    std::optional<std::uint32_t> MaxDe(RE::FormID a_id)
    {
        if (!g_prete || a_id == 0) return std::nullopt;
        const auto it = g_max.find(a_id);
        if (it == g_max.end()) return std::nullopt;
        return it->second;
    }

    int CentMilliemesDe(float a_health)
    {
        if (!(a_health > 1.0f)) return 0;   // nue — et un NaN tombe ici aussi
        /* En DOUBLE : le float32 porte ~7 chiffres, et « 1,00601 » n'y est
           qu'approché ; multiplier en float pouvait rendre 600,9999 puis 600
           au lieu de 601. Le jumeau TS calcule en double lui aussi. */
        const double d = (static_cast<double>(a_health) - 1.0) * kDiviseur;
        return static_cast<int>(std::lround(d));
    }

    std::uint32_t PointsDeCentMilliemes(int a_centMilliemes, std::uint32_t a_max)
    {
        if (a_centMilliemes <= 0) return a_max;   // nue = maximum
        const long long p = static_cast<long long>(a_centMilliemes) - 1;
        if (p <= 0) return 0;
        /* Un health de l'ancien régime (1,1 = lingot de qualité 2) sur une
           forme de la forge n'existe pas par construction ; s'il arrivait
           quand même, afficher « 9 999 / 600 » serait pire que borner. */
        return p >= static_cast<long long>(a_max) ? a_max : static_cast<std::uint32_t>(p);
    }

    std::uint32_t PointsDe(float a_health, std::uint32_t a_max)
    {
        return PointsDeCentMilliemes(CentMilliemesDe(a_health), a_max);
    }
}
