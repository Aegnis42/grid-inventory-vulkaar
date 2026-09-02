#include "ui/Clavier.h"

#include <Windows.h>   // VK_RETURN, VK_TAB, VK_ESCAPE, VK_BACK — les touches hors table qui touchent une morte

#include <cctype>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

namespace vk::clavier
{
    namespace
    {
        // Là où le launcher écrit les préférences du joueur. RELATIF à la racine
        // du jeu, comme Data/SKSE/Plugins/GridInventory_ui.ini (WinManager.cpp) :
        // le jeu tourne toujours depuis sa racine, c'est la convention de tout
        // le greffon. Le dossier PluginsNoLoad est celui que SkyrimPlatform
        // n'exécute pas — le fichier est une donnée déguisée en script.
        constexpr const char* kCheminPreferences =
            "Data/Platform/PluginsNoLoad/vulkaar-preferences-no-load.js";

        // L'accent d'une touche morte posée et pas encore résolue (0 = aucun).
        // UN SEUL pour tout le greffon : un seul champ a le focus à la fois, et
        // les deux routes (fenêtre, sondage) ne servent jamais le même joueur
        // en même temps — la seconde ne s'éveille que si la première est morte.
        wchar_t g_morte = 0;

        // Frappe porte deux unités UTF-16 au plus : accent + lettre. La règle
        // tient parce que la table n'a que des valeurs d'UNE unité (la page
        // compare la chaîne entière ; c'est identique tant que c'est vrai). Si
        // le générateur écrit un jour une valeur plus longue, le miroir casse
        // ici, à la compilation, plutôt qu'en jeu.
        constexpr bool ValeursDUneUnite(const tables::Disposition& d)
        {
            for (std::size_t i = 0; i < d.n; ++i) {
                for (int c = 0; c < 4; ++c) {
                    const wchar_t* v = d.touches[i].col[c];
                    if (v && v[0] != L'\0' && v[1] != L'\0') return false;
                }
            }
            return true;
        }
        constexpr bool ToutesLesValeursDUneUnite()
        {
            for (const auto& d : tables::kDispositions) {
                if (!ValeursDUneUnite(d)) return false;
            }
            return true;
        }
        static_assert(ToutesLesValeursDUneUnite(),
            "ClavierTables.gen.h : une valeur fait plus d'une unité UTF-16 — "
            "Frappe::texte et la composition supposent une lettre = une unité");

        // Analyse MINIMALE du fichier de préférences. Ce projet n'a pas de
        // bibliothèque JSON et n'en veut pas pour lire UNE clé : on cherche
        // l'objet "clavier", puis sa clé "disposition", puis la chaîne entre
        // guillemets après le « : ». Le fichier est écrit par NOTRE launcher,
        // sa forme est connue :  //{"audio":{...},"clavier":{"disposition":"qwerty",...}}
        // Rend la valeur brute (non normalisée), ou vide.
        std::string LireDispositionDeclaree()
        {
            std::ifstream in(kCheminPreferences, std::ios::binary);
            if (!in) return {};
            const std::string texte((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            constexpr auto npos = std::string::npos;

            std::size_t p = 0;
            if (texte.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;   // BOM UTF-8, au cas où
            while (p < texte.size() && std::isspace(static_cast<unsigned char>(texte[p]))) ++p;
            // Les deux « // » de tête rendent le fichier inerte pour tout
            // lecteur JavaScript ; derrière, c'est du JSON pur.
            if (texte.compare(p, 2, "//") == 0) p += 2;

            const auto clavier = texte.find("\"clavier\"", p);
            if (clavier == npos) return {};
            const auto ouvrante = texte.find('{', clavier);
            if (ouvrante == npos) return {};
            const auto cle = texte.find("\"disposition\"", ouvrante);
            if (cle == npos) return {};
            const auto deuxPoints = texte.find(':', cle + sizeof("\"disposition\"") - 1);
            if (deuxPoints == npos) return {};
            const auto guillemet = texte.find_first_not_of(" \t\r\n", deuxPoints + 1);
            if (guillemet == npos || texte[guillemet] != '"') return {};
            const auto fin = texte.find('"', guillemet + 1);
            if (fin == npos) return {};
            return texte.substr(guillemet + 1, fin - guillemet - 1);
        }

        const tables::Disposition* Charger()
        {
            std::string nom = LireDispositionDeclaree();
            for (auto& c : nom) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const auto& d : tables::kDispositions) {
                if (nom == d.nom) {
                    SKSE::log::info("[clavier] disposition déclarée : {}", d.nom);
                    return &d;
                }
            }
            // « autre », valeur inconnue, fichier absent : même verdict — on ne
            // traduit rien. On journalise ce qu'on a lu pour qu'un rapport dise
            // lequel des trois c'était.
            if (nom.empty()) {
                SKSE::log::info("[clavier] disposition déclarée : aucune — Windows décide");
            } else {
                SKSE::log::info("[clavier] disposition déclarée : aucune — Windows décide (lu « {} »)", nom);
            }
            return nullptr;
        }
    }

    const tables::Disposition* Declaree()
    {
        // Une lecture et une ligne de journal par processus, quel que soit le
        // premier appelant (la fenêtre ou le repli) : l'initialisation d'une
        // statique locale est faite une fois, et une seule.
        static const tables::Disposition* const s_declaree = Charger();
        return s_declaree;
    }

    bool Traduire(std::uint8_t sc, bool maj, bool altgr, bool verrMaj, Frappe& out)
    {
        out.n = 0;
        out.texte[0] = L'\0';

        const tables::Disposition* d = Declaree();
        if (!d) return false;
        const tables::Touche* touche = nullptr;
        for (std::size_t i = 0; i < d->n; ++i) {
            if (d->touches[i].sc == sc) {
                touche = &d->touches[i];
                break;
            }
        }
        if (!touche) return false;   // pas à nous : Windows/ImGui comme avant

        // La colonne — MÊME règle que traduire() côté page. AltGr prend la
        // troisième, ou la quatrième si Maj est tenue — et PAS de repli sur la
        // troisième quand la quatrième manque : le pilote (SHFT_INVALID dans
        // KBDFR et KBDGR) n'écrit rien sous AltGr+Maj, seul ẞ (QWERTZ, ß) a une
        // quatrième colonne. Une colonne nullptr suit donc la voie « prise,
        // rien ne s'écrit » ci-dessous. Sans AltGr, Verr. Maj n'inverse que ce
        // que le pilote inverse (les lettres, et ce que chaque DLL y ajoute :
        // tout le bloc en AZERTY, chiffres et ß # , . en QWERTZ) — le verdict
        // est calculé par le générateur, pas ici.
        int colonne;
        if (altgr) {
            colonne = maj ? 3 : 2;
        } else {
            const bool inverse = verrMaj && touche->verrMaj;
            colonne = (maj != inverse) ? 1 : 0;
        }
        const wchar_t* valeur = touche->col[colonne];
        // Colonne absente ou muette : la touche est prise, rien ne s'écrit —
        // c'est ce que fait Windows (AltGr+E sur un clavier US n'écrit rien).
        if (!valeur || valeur[0] == L'\0') return true;

        if (touche->morte & (1u << colonne)) {
            const wchar_t accent = valeur[0];
            if (g_morte) {
                // Deux mortes de suite : Windows sort les deux accents, rien n'attend.
                out.texte[0] = g_morte;
                out.texte[1] = accent;
                out.texte[2] = L'\0';
                out.n = 2;
                g_morte = 0;
                return true;
            }
            g_morte = accent;   // posée : la prochaine lettre dira
            return true;
        }

        if (g_morte) {
            const wchar_t enAttente = g_morte;
            g_morte = 0;
            for (const auto& c : tables::kCompositions) {
                if (c.accent == enAttente && c.lettre == valeur[0]) {
                    out.texte[0] = c.resultat;
                    out.texte[1] = L'\0';
                    out.n = 1;
                    return true;
                }
            }
            if (valeur[0] == L' ') {
                // Morte puis espace : l'accent seul, comme sous Windows.
                out.texte[0] = enAttente;
                out.texte[1] = L'\0';
                out.n = 1;
                return true;
            }
            // Pas de composition : l'accent, puis la lettre (« ^x »).
            out.texte[0] = enAttente;
            out.texte[1] = valeur[0];
            out.texte[2] = L'\0';
            out.n = 2;
            return true;
        }

        out.texte[0] = valeur[0];
        out.texte[1] = L'\0';
        out.n = 1;
        return true;
    }

    bool TerminerMorte(int vk, Frappe& out)
    {
        out.n = 0;
        out.texte[0] = L'\0';
        if (!g_morte) return false;
        // Les mêmes trois touches que TERMINE_LA_MORTE côté page : Windows sort
        // l'accent, puis exécute la touche. Toute autre touche hors table
        // (flèches, Suppr, F1...) laisse l'attente en place.
        if (vk != VK_RETURN && vk != VK_TAB && vk != VK_ESCAPE) return false;
        out.texte[0] = g_morte;
        out.texte[1] = L'\0';
        out.n = 1;
        g_morte = 0;
        return true;
    }

    bool AnnulerMorte(int vk)
    {
        // « ^ » puis Retour arrière : sous Windows l'accent sort puis s'efface —
        // net, rien. On saute les deux étapes : l'attente meurt, et l'appelant
        // avale la touche pour que rien d'autre ne s'efface.
        if (!g_morte || vk != VK_BACK) return false;
        g_morte = 0;
        return true;
    }

    void OublierMorte()
    {
        g_morte = 0;
    }
}
