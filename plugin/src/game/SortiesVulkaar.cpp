#include "SortiesVulkaar.h"

#include <cstdio>

// Voir SortiesVulkaar.h pour le POURQUOI. Ici, seulement la mécanique :
// des appends courts, flushés ligne à ligne — le lecteur (skymp5-client)
// vit dans le même processus et lit à la trame près, un tampon retenu lui
// mentirait d'une demi-seconde.

namespace FUI::SortiesVulkaar
{
    namespace
    {
        constexpr const char* kChemin = "Data/SKSE/Plugins/GridInventory_sorties.txt";
        unsigned long long g_seq = 0;
        bool g_pret = false;
    }

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kChemin, "w")) {
            std::fclose(f);
            g_seq = 0;
            g_pret = true;
            SKSE::log::info("[SORTIES] journal des sorties tronque ({})", kChemin);
        } else {
            SKSE::log::warn("[SORTIES] impossible d'ouvrir {} — jets et destructions ne remonteront pas au serveur", kChemin);
        }
    }

    void Noter(const char* a_action, RE::FormID a_baseId, int a_count)
    {
        if (!g_pret || a_count <= 0) return;
        std::FILE* f = std::fopen(kChemin, "a");
        if (!f) return;
        std::fprintf(f, "%llu\t%s\t%08X\t%d\n", ++g_seq, a_action, a_baseId, a_count);
        std::fclose(f);
    }
}
