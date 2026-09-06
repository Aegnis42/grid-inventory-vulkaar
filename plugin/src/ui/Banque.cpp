#include "ui/Banque.h"
#include "ui/Appartenance.h"
#include "ui/Missives.h"   // [vulkaar] les missives tiennent la racine comme nous
#include "ui/Etabli.h"

#include "game/MonnaiesVulkaar.h"
#include "ui/IconCache.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// Voir Banque.h pour l'architecture — et pour la raison d'être d'un écran à
// part. Ici : le parsing TSV de l'état, la composition du comptoir, et
// l'écriture des gestes.

namespace FUI::Banque
{
    namespace
    {
        constexpr const char* kCheminEtat = "Data/SKSE/Plugins/GridInventory_banque_etat.txt";
        constexpr const char* kCheminGestes = "Data/SKSE/Plugins/GridInventory_banque.txt";

        /* Trois monnaies, dans l'ordre de MONNAIES (shared/monnaies.ts) :
           0 Septime, 1 Mède, 2 Titus. C'est aussi l'ordre des rangs de
           MonnaiesVulkaar (l'icône) et des colonnes d'un geste. */
        constexpr int kNbMonnaies = 3;

        struct Monnaie
        {
            bool        recue = false;   // la ligne `monnaie` de ce rang est arrivée
            std::string id;              // septime | mede | titus — la clé des gestes de change
            std::string nom;             // ce que le serveur veut qu'on lise
            int         valeur = 0;      // en Titus — le taux, reçu, jamais écrit ici
            int         enBanque = 0;
            int         surToi = 0;      // la bourse telle que le SERVEUR la voit
        };

        struct Operation
        {
            std::string quand;     // l'ISO tel que reçu ; rendu en heure locale au dessin
            std::string type;      // depot | retrait | prelevement
            std::string libelle;   // composé par le serveur (« Dépôt de 1 Septime et 2 Mèdes »)
        };

        // ---- état reçu (le serveur fait foi) ----
        bool                   g_ouvert = false;
        std::string            g_titre;
        std::string            g_message;
        Monnaie                g_monnaies[kNbMonnaies];
        std::vector<Operation> g_operations;   // la plus récente en tête
        unsigned long long     g_seqEtat = 0;

        // ---- état de l'écran (local, jamais envoyé tel quel) ----
        int g_quantite[kNbMonnaies] = {};   // le champ de chaque ligne
        /* Le change : de → vers, par RANG. Mède → Septime au départ — c'est
           l'exemple du contrat (« 10 Mèdes contre 1 Septime ») et le change le
           plus courant : on monte, on ne descend pas, quand on vient déposer. */
        int g_changeDe = 1;
        int g_changeVers = 0;
        int g_changeQuantite = 0;
        int g_messageRestant = 0;   // trames avant effacement du message

        /* Au-delà de ce nombre d'opérations, la liste passe dans un enfant
           défilant : le panneau est AlwaysAutoResize et grandirait sans fin. */
        constexpr std::size_t kOperationsVisibles = 6;
        /* La borne du serveur (QUANTITE_MAX de shared) : un champ qui la
           dépasserait ferait partir un geste refusé pour la forme. C'est la
           borne d'UN GESTE, et de rien d'autre. */
        constexpr int kQuantiteMax = 1000000;
        /* LA BORNE D'UN COMPTEUR N'EST PAS CELLE D'UN GESTE. Un solde n'a pas
           de plafond — le registre le dit mot pour mot (« un entier >= 0 par
           monnaie, SANS plafond : un solde accumule des gestes bornés, il
           n'est pas borné lui-même », depot.ts), et le gamemode ne le borne
           pas non plus en relisant (serviceComptes.ts). Les deux nombres reçus
           se lisent donc sur CETTE borne : sans elle, un joueur qui a
           3 000 000 Titus en banque lisait « 1,000,000 » au comptoir — un
           chiffre faux sur de l'argent. Large, et à l'abri du débordement
           d'un int. */
        constexpr int kAffichageMax = 2000000000;

        // ---- plomberie ----
        unsigned long long g_seqGeste = 0;
        bool               g_pret = false;
        int                g_tic = 0;
        int                g_dernierDessin = 0;

        /* Le chien de garde : la racine s'est refermée sans passer par nous
           (Tab, un menu vanilla) — sans cela le serveur garderait le joueur
           parmi les comptoirs ouverts et lui repousserait des plateaux. */
        constexpr int kTramesSansDessin = 120;
        constexpr int kTramesMessage = 420;   // ~7 s à 60 fps

        /** Une tabulation ou un retour dans un texte décalerait tous les
         *  champs du lecteur TS, qui est un split('\t') sans état. */
        std::string Assainir(const char* a_s)
        {
            std::string r = a_s == nullptr ? "" : a_s;
            for (char& c : r) {
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            }
            return r;
        }

        /** Les milliers séparés par une virgule — copie de `Grouped` d'UIRoot.cpp
         *  (espace anonyme, donc inaccessible) : la bourse en bas de
         *  l'inventaire écrit « 1,000 », le comptoir doit écrire pareil. */
        std::string Groupe(int a_v)
        {
            char brut[32];
            std::snprintf(brut, sizeof(brut), "%d", a_v < 0 ? -a_v : a_v);
            std::string sortie;
            const int n = static_cast<int>(std::strlen(brut));
            for (int i = 0; i < n; ++i) {
                if (i && (n - i) % 3 == 0) sortie += ',';
                sortie += brut[i];
            }
            return (a_v < 0 ? std::string("-") : std::string()) + sortie;
        }

        /** « Septimes », « Mèdes », « Titus » (invariable — il finit déjà par
         *  un s). La même règle que `libellePieces` côté serveur. */
        std::string Pluriel(const std::string& a_nom, int a_n)
        {
            if (a_n <= 1 || a_nom.empty() || a_nom.back() == 's') return a_nom;
            return a_nom + "s";
        }

        /** « 10 Mèdes » — le nombre groupé et le nom au pluriel. */
        std::string Libelle(int a_n, const Monnaie& a_m)
        {
            return Groupe(a_n) + " " + Pluriel(a_m.nom, a_n);
        }

        /** `<seq>\t<action>[\t<reste>]`. Le pont de la banque n'a qu'un
         *  sujet : pas de colonne de sujet, contrairement à l'appartenance. */
        void EcrireGeste(const char* a_action, const std::string& a_reste)
        {
            if (!g_pret) return;
            std::FILE* f = std::fopen(kCheminGestes, "a");
            if (!f) return;
            if (a_reste.empty()) std::fprintf(f, "%llu\t%s\n", ++g_seqGeste, a_action);
            else std::fprintf(f, "%llu\t%s\t%s\n", ++g_seqGeste, a_action, a_reste.c_str());
            std::fclose(f);
        }

        /** UN GESTE, UNE MONNAIE : `<septime>\t<mede>\t<titus>` avec la seule
         *  colonne du rang renseignée. L'ordre des colonnes est celui de
         *  MONNAIES, le même que les rangs. */
        std::string ColonnesPieces(int a_rang, int a_quantite)
        {
            int q[kNbMonnaies] = {};
            if (a_rang >= 0 && a_rang < kNbMonnaies) q[a_rang] = a_quantite;
            char reste[64];
            std::snprintf(reste, sizeof(reste), "%d\t%d\t%d", q[0], q[1], q[2]);
            return reste;
        }

        /** L'or du thème, assombri et rendu presque transparent : le filet
         *  d'un pixel que le propriétaire préfère à toute bordure pleine. */
        ImU32 OrSombre(float a_alpha)
        {
            const ImU32 o = Theme::GoldCol();
            const auto canal = [&](int a_decalage) {
                return static_cast<int>(((o >> a_decalage) & 0xFF) * 0.72f);
            };
            return IM_COL32(canal(IM_COL32_R_SHIFT), canal(IM_COL32_G_SHIFT), canal(IM_COL32_B_SHIFT),
                static_cast<int>(a_alpha * 255.0f));
        }

        /** Le voile de survol : 10 % de blanc, rien de plus. */
        ImU32 Voile(float a_alpha)
        {
            return IM_COL32(255, 255, 255, static_cast<int>(a_alpha * 255.0f));
        }

        /** Le rouge sombre du « pas exact » : un refus local, pas une alerte. */
        ImU32 RougeSombre()
        {
            return IM_COL32(178, 66, 54, 255);
        }

        /* Les teintes de repli, celles de la bourse d'UIRoot.cpp : or, argent,
           cuivre. Un disque de la couleur du métal tient la place de l'icône
           tant que la capture n'est pas revenue — jamais un trou. */
        constexpr ImU32 kTeintes[kNbMonnaies] = {
            IM_COL32(214, 176, 82, 255),    // Septime — or
            IM_COL32(198, 202, 210, 255),   // Mède    — argent
            IM_COL32(198, 124, 74, 255),    // Titus   — cuivre
        };

        // ── la lecture de l'état ──────────────────────────────────────────

        /** Découpe une ligne TSV en champs. Le lecteur reste sans état : un
         *  simple parcours, comme celui de l'établi. */
        int Champs(char* a_ligne, char* a_out[], int a_max)
        {
            int n = 0;
            char* p = a_ligne;
            a_out[n++] = p;
            while (*p && n < a_max) {
                if (*p == '\t') {
                    *p = '\0';
                    a_out[n++] = p + 1;
                }
                ++p;
            }
            for (int i = 0; i < n; ++i) {
                char* fin = a_out[i] + std::strlen(a_out[i]);
                while (fin > a_out[i] && (fin[-1] == '\n' || fin[-1] == '\r')) *--fin = '\0';
            }
            return n;
        }

        /** Un entier borné : `atoi` rendrait n'importe quoi sur un texte, et
         *  un compteur négatif ou géant griserait ou activerait un bouton à
         *  tort. */
        int EntierBorne(const char* a_s, int a_max)
        {
            const long v = std::strtol(a_s, nullptr, 10);
            if (v < 0) return 0;
            if (v > a_max) return a_max;
            return static_cast<int>(v);
        }

        void LireEtat()
        {
            std::FILE* f = std::fopen(kCheminEtat, "r");
            if (!f) return;

            /* LE NUMÉRO DE SÉQUENCE SE LIT EN PREMIER, ET ON SORT AUSSITÔT.
               Ce Tick est appelé à CHAQUE trame ; le fichier est réécrit ENTIER
               à chaque poussée serveur et sa première ligne porte « seq <n> ».
               Rien n'a bougé, pas un octet de plus. */
            {
                char premiere[256];
                if (std::fgets(premiere, sizeof(premiere), f)) {
                    char* p0[4];
                    const int n0 = Champs(premiere, p0, 4);
                    if (n0 >= 2 && std::strcmp(p0[0], "seq") == 0) {
                        const unsigned long long vu = std::strtoull(p0[1], nullptr, 10);
                        if (vu != 0 && vu == g_seqEtat) {
                            std::fclose(f);
                            return;
                        }
                    }
                }
                std::rewind(f);
            }

            unsigned long long seq = 0;
            bool ouvert = false;
            bool finVue = false;
            std::string titre, message;
            Monnaie monnaies[kNbMonnaies];
            std::vector<Operation> operations;

            char ligne[2048];
            char* c[12];
            while (std::fgets(ligne, sizeof(ligne), f)) {
                const int n = Champs(ligne, c, 12);
                if (n < 1 || c[0][0] == '\0') continue;
                /* La sentinelle est toujours la DERNIÈRE ligne : une ligne qui
                   la suivrait trahirait un fichier recousu, on l'ignore. */
                finVue = std::strcmp(c[0], "fin") == 0;
                if (finVue) continue;
                if (n < 2) continue;
                if (std::strcmp(c[0], "seq") == 0) {
                    seq = std::strtoull(c[1], nullptr, 10);
                } else if (std::strcmp(c[0], "phase") == 0) {
                    ouvert = std::strcmp(c[1], "ouverte") == 0;
                } else if (std::strcmp(c[0], "titre") == 0) {
                    titre = c[1];
                } else if (std::strcmp(c[0], "message") == 0) {
                    message = c[1];
                } else if (std::strcmp(c[0], "monnaie") == 0 && n >= 7) {
                    /* `monnaie <rang> <id> <nom> <valeur> <enBanque> <surToi>`.
                       Un rang hors 0..2 est un client d'un autre contrat :
                       ignoré plutôt que d'écraser une ligne voisine. */
                    const int rang = std::atoi(c[1]);
                    if (rang < 0 || rang >= kNbMonnaies) continue;
                    Monnaie& m = monnaies[rang];
                    m.recue = true;
                    m.id = c[2];
                    m.nom = c[3];
                    m.valeur = EntierBorne(c[4], kQuantiteMax);
                    /* Les COMPTEURS, pas des gestes : voir kAffichageMax. */
                    m.enBanque = EntierBorne(c[5], kAffichageMax);
                    m.surToi = EntierBorne(c[6], kAffichageMax);
                } else if (std::strcmp(c[0], "operation") == 0 && n >= 4) {
                    operations.push_back(Operation{ c[1], c[2], c[3] });
                }
            }
            std::fclose(f);

            /* RIEN N'EST COMMIS TANT QUE LE SEQ N'A PAS BOUGÉ, NI SANS LA
               SENTINELLE : le fichier est relu pendant qu'on l'écrit, et une
               lecture tronquée acceptée se lirait « une monnaie en moins » —
               un solde à zéro, un bouton grisé — et se figerait jusqu'à la
               prochaine poussée. Une lecture rejetée ne mémorise RIEN, pas
               même le seq : on la refera à la trame d'après. */
            if (seq == 0 || seq == g_seqEtat || !finVue) return;
            g_seqEtat = seq;

            const bool avant = g_ouvert;
            g_ouvert = ouvert;
            g_titre = std::move(titre);
            for (int i = 0; i < kNbMonnaies; ++i) g_monnaies[i] = std::move(monnaies[i]);
            g_operations = std::move(operations);

            if (!message.empty()) {
                g_message = std::move(message);
                g_messageRestant = kTramesMessage;
            }

            if (g_ouvert && !avant) {
                // Le comptoir s'ouvre : champs à zéro, et on ouvre la racine —
                // sans elle, UIRoot::Render n'est jamais appelé. Le chien de
                // garde part d'ici, sinon il mordrait avant la première trame
                // dessinée.
                for (int i = 0; i < kNbMonnaies; ++i) g_quantite[i] = 0;
                g_changeQuantite = 0;
                // g_message est GARDÉ : la poussée d'ouverture peut en porter un.
                g_dernierDessin = g_tic;
                UIRoot::Open();
            }
            if (!g_ouvert && avant) {
                /* Le SERVEUR nous ferme (trop loin du comptoir, déconnexion du
                   registre) : la racine, ouverte pour nous, se referme avec —
                   sinon le joueur tombe sur son inventaire sans l'avoir
                   demandé. Sauf si un autre de nos écrans la tient encore :
                   elle est à lui. [vulkaar] LES MISSIVES EN FONT PARTIE depuis
                   le 06/09/2026 : un écran de plus qui tient la racine, un oubli
                   de plus qui la lui volerait — et son chien de garde le
                   fermerait deux secondes plus tard sans un mot. */
                if (!Etabli::Ouvert() && !Appartenance::Ouvert() && !Missives::Ouvert()) UIRoot::Close();
            }
        }

        /** L'ISO du serveur (`2026-09-06T14:23:11.123Z`) en heure LOCALE
         *  courte « JJ/MM HH:MM ». Illisible ⇒ l'ISO brut, jamais rien : une
         *  opération sans date se lirait comme une opération de plus.
         *
         *  Le décalage en queue (« Z », « +02:00 », « -05:00 ») est honoré ;
         *  sans rien, c'est de l'UTC — c'est ce que `toISOString` écrit. Le
         *  signe se cherche APRÈS le « T » : la date en a déjà deux. */
        std::string HeureLocale(const std::string& a_iso)
        {
            int an = 0, mois = 0, jour = 0, heure = 0, minute = 0;
            if (std::sscanf(a_iso.c_str(), "%4d-%2d-%2dT%2d:%2d", &an, &mois, &jour, &heure, &minute) != 5) {
                return a_iso;
            }
            int seconde = 0;
            if (a_iso.size() >= 19 && a_iso[16] == ':') seconde = std::atoi(a_iso.c_str() + 17);

            int decalageMinutes = 0;
            const std::size_t posT = a_iso.find('T');
            std::size_t posSigne = std::string::npos;
            for (std::size_t i = (posT == std::string::npos ? 0 : posT + 1); i < a_iso.size(); ++i) {
                if (a_iso[i] == '+' || a_iso[i] == '-') posSigne = i;
            }
            if (posSigne != std::string::npos) {
                int dh = 0, dm = 0;
                if (std::sscanf(a_iso.c_str() + posSigne + 1, "%2d:%2d", &dh, &dm) >= 1) {
                    decalageMinutes = (a_iso[posSigne] == '-' ? -1 : 1) * (dh * 60 + dm);
                }
            }

            std::tm utc{};
            utc.tm_year = an - 1900;
            utc.tm_mon = mois - 1;
            utc.tm_mday = jour;
            utc.tm_hour = heure;
            utc.tm_min = minute;
            utc.tm_sec = seconde;
            const std::time_t t = _mkgmtime(&utc);
            if (t == static_cast<std::time_t>(-1)) return a_iso;
            const std::time_t instant = t - static_cast<std::time_t>(decalageMinutes) * 60;
            std::tm local{};
            if (localtime_s(&local, &instant) != 0) return a_iso;
            char sortie[32];
            std::snprintf(sortie, sizeof(sortie), "%02d/%02d %02d:%02d",
                local.tm_mday, local.tm_mon + 1, local.tm_hour, local.tm_min);
            return sortie;
        }

        // ── les morceaux du comptoir ──────────────────────────────────────

        /** Un filet d'un pixel sous le dernier widget posé. */
        void FiletSousItem()
        {
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), OrSombre(0.35f), 1.0f);
        }

        /** Les couleurs et marges de la table et des blocs : filets or,
         *  en-tête nu, boutons sans fond dont le survol est un voile, champs
         *  de saisie sur un voile plus léger encore. */
        void PousserStyle(float a_S)
        {
            ImGui::PushStyleColor(ImGuiCol_TableBorderLight, OrSombre(0.35f));
            ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, OrSombre(0.35f));
            ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Voile(0.14f));
            ImGui::PushStyleColor(ImGuiCol_Button, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Voile(0.16f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Voile(0.10f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f * a_S, 0.0f));
        }

        void RetirerStyle()
        {
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(12);
        }

        /** L'icône de la pièce, comme la bourse d'UIRoot.cpp : la forme de
         *  MonnaiesVulkaar par rang, photographiée par le cache de la grille ;
         *  en attendant la capture, un disque à la teinte du métal. */
        void Icone(int a_rang, const ImVec2& a_p0, float a_cote, float a_S)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            auto* icones = IconCache::GetSingleton();
            auto* forme = MonnaiesVulkaar::Forme(a_rang);
            const IconCache::Icon* ic = (icones && forme) ? icones->Get(forme) : nullptr;
            if (ic && ic->srv) {
                const float m = 2.0f * a_S;
                dl->AddImage(reinterpret_cast<ImTextureID>(ic->srv),
                    ImVec2(a_p0.x + m, a_p0.y + m), ImVec2(a_p0.x + a_cote - m, a_p0.y + a_cote - m));
            } else {
                dl->AddCircleFilled(ImVec2(a_p0.x + a_cote * 0.5f, a_p0.y + a_cote * 0.5f),
                    a_cote * 0.30f, kTeintes[a_rang], 20);
                if (forme && icones) icones->QueueCapture(forme);
            }
        }

        /** Le titre du serveur, et dessous le rapport CALCULÉ depuis les
         *  valeurs reçues : « 1 Septime = 10 Mèdes = 100 Titus ». Si un
         *  rapport n'est pas entier (un taux qu'on n'a pas prévu), on écrit
         *  les valeurs en Titus telles quelles plutôt qu'un chiffre faux. */
        void EnTete()
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::TextUnformatted(g_titre.empty() ? "Banque" : g_titre.c_str());
            ImGui::PopStyleColor();

            const Monnaie& forte = g_monnaies[0];
            if (!forte.recue || forte.valeur <= 0) return;
            bool entier = true;
            for (int i = 1; i < kNbMonnaies; ++i) {
                const Monnaie& m = g_monnaies[i];
                if (!m.recue || m.valeur <= 0 || forte.valeur % m.valeur != 0) entier = false;
            }
            std::string rapport;
            if (entier) {
                rapport = "1 " + forte.nom;
                for (int i = 1; i < kNbMonnaies; ++i) {
                    const Monnaie& m = g_monnaies[i];
                    const int n = forte.valeur / m.valeur;
                    rapport += " = " + Libelle(n, m);
                }
            } else {
                for (int i = 0; i < kNbMonnaies; ++i) {
                    const Monnaie& m = g_monnaies[i];
                    if (!m.recue) continue;
                    if (!rapport.empty()) rapport += "   ";
                    rapport += m.nom + " " + Groupe(m.valeur);
                }
                rapport += "   (en Titus)";
            }
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            ImGui::TextUnformatted(rapport.c_str());
            ImGui::PopStyleColor();
        }

        /** Un nombre cliquable : le clic reporte ce nombre dans la quantité
         *  de la ligne — c'est le « Tout » du contrat, sans ambiguïté entre
         *  « tout ce que j'ai sur moi » et « tout ce que j'ai en banque » :
         *  on clique le nombre qu'on veut bouger. */
        void NombreCliquable(const char* a_id, int a_valeur, ImU32 a_couleur, int& a_quantite,
            float a_largeur, float a_haut, const char* a_indice)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, a_couleur);
            const std::string etiquette = Groupe(a_valeur) + a_id;
            const bool clic = Sfx::Button(etiquette.c_str(), ImVec2(a_largeur, a_haut));
            ImGui::PopStyleColor();
            if (clic && a_valeur > 0) a_quantite = a_valeur;
            if (ImGui::IsItemHovered() && a_valeur > 0) ImGui::SetTooltip("%s", a_indice);
        }

        /** Les trois lignes : icône | monnaie | En banque | Sur toi | quantité |
         *  Déposer | Retirer. Un clic écrit UN geste avec la seule monnaie de
         *  la ligne ; la quantité de la ligne revient à zéro pour qu'un second
         *  clic ne double pas le geste par mégarde. */
        void TableMonnaies(float a_S)
        {
            const float haut = 36.0f * a_S;
            const float cadre = ImGui::GetFrameHeight();
            /* LA COLONNE SE TAILLE SUR LE PLUS GRAND NOMBRE RÉELLEMENT AFFICHÉ,
               jamais sous « 1,000,000 » : un solde n'a pas de plafond, et une
               fortune ne doit ni déborder de sa colonne ni faire danser la
               table à chaque geste ordinaire. */
            std::string reference = "1,000,000";
            for (int i = 0; i < kNbMonnaies; ++i) {
                const std::string enBanque = Groupe(g_monnaies[i].enBanque);
                const std::string surToi = Groupe(g_monnaies[i].surToi);
                if (enBanque.size() > reference.size()) reference = enBanque;
                if (surToi.size() > reference.size()) reference = surToi;
            }
            const float largeurNombre = ImGui::CalcTextSize(reference.c_str()).x + 16.0f * a_S;
            /* Le champ : le texte du plus grand nombre, plus les deux boutons
               « − » « + » d'InputInt, chacun carré à la hauteur du cadre. */
            const float largeurQuantite = ImGui::CalcTextSize("1,000,000").x + 2.0f * cadre + 12.0f * a_S;
            const float largeurBouton =
                (std::max)(ImGui::CalcTextSize("Déposer").x, ImGui::CalcTextSize("Retirer").x)
                + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f * a_S;

            PousserStyle(a_S);
            const bool table = ImGui::BeginTable("##vk_banque_monnaies", 7,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_NoPadOuterX);
            if (table) {
                ImGui::TableSetupColumn("##icone", ImGuiTableColumnFlags_WidthFixed, haut);
                ImGui::TableSetupColumn("Monnaie", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("En banque", ImGuiTableColumnFlags_WidthFixed, largeurNombre);
                ImGui::TableSetupColumn("Sur toi", ImGuiTableColumnFlags_WidthFixed, largeurNombre);
                ImGui::TableSetupColumn("Quantité", ImGuiTableColumnFlags_WidthFixed, largeurQuantite);
                ImGui::TableSetupColumn("##deposer", ImGuiTableColumnFlags_WidthFixed, largeurBouton);
                ImGui::TableSetupColumn("##retirer", ImGuiTableColumnFlags_WidthFixed, largeurBouton);

                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TableHeadersRow();
                ImGui::PopStyleColor();

                for (int rang = 0; rang < kNbMonnaies; ++rang) {
                    const Monnaie& m = g_monnaies[rang];
                    ImGui::PushID(rang);
                    ImGui::TableNextRow(0, haut);

                    // Icône
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 depart = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(haut, haut));
                    Icone(rang, depart, haut, a_S);

                    // Monnaie — le nom reçu ; « ? » si la ligne n'est pas venue
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (haut - ImGui::GetTextLineHeight()) * 0.5f);
                    ImGui::PushStyleColor(ImGuiCol_Text, m.recue ? Theme::Chrome(0.92f) : Theme::Chrome(0.45f));
                    ImGui::TextUnformatted(m.recue ? m.nom.c_str() : "?");
                    ImGui::PopStyleColor();

                    /* LA QUANTITÉ EST BORNÉE au plus grand des deux nombres :
                       elle peut viser un dépôt ou un retrait, on ne sait pas
                       encore lequel. Le bouton qui ne peut pas la servir se
                       grise. Après une poussée serveur les nombres ont pu
                       baisser : on rebornait à chaque trame, sinon un champ à
                       « 12 » sur un « 5 » laisserait les deux boutons gris
                       sans dire pourquoi. Et JAMAIS au-delà de ce que le
                       serveur accepte dans UN geste : un solde peut passer le
                       million, une demande non — elle serait refusée pour la
                       forme. */
                    int& q = g_quantite[rang];
                    const int borne = (std::min)((std::max)(m.enBanque, m.surToi), kQuantiteMax);
                    q = std::clamp(q, 0, borne);

                    // En banque — en or si le compte a quelque chose
                    ImGui::TableSetColumnIndex(2);
                    NombreCliquable("##enbanque", m.enBanque,
                        m.enBanque > 0 ? Theme::GoldCol() : Theme::Chrome(0.45f), q, largeurNombre, haut,
                        "prendre ce nombre comme quantité");

                    // Sur toi
                    ImGui::TableSetColumnIndex(3);
                    NombreCliquable("##surtoi", m.surToi,
                        m.surToi > 0 ? Theme::Chrome(0.92f) : Theme::Chrome(0.45f), q, largeurNombre, haut,
                        "prendre ce nombre comme quantité");

                    // Quantité — « − » « + » par 1, par 10 avec Ctrl
                    ImGui::TableSetColumnIndex(4);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (haut - cadre) * 0.5f);
                    ImGui::SetNextItemWidth(largeurQuantite - 4.0f * a_S);
                    ImGui::BeginDisabled(borne <= 0);
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.92f));
                    ImGui::InputInt("##quantite", &q, 1, 10);
                    ImGui::PopStyleColor();
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered() && borne > 0) {
                        ImGui::SetTooltip("%s", "combien de pièces bouger — Ctrl sur − / + : par dix");
                    }
                    q = std::clamp(q, 0, borne);

                    // Déposer — grisé si rien, ou plus que sur toi
                    ImGui::TableSetColumnIndex(5);
                    ImGui::BeginDisabled(q <= 0 || q > m.surToi);
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.85f));
                    if (Sfx::Button("Déposer##deposer", ImVec2(largeurBouton, haut))) {
                        EcrireGeste("deposer", ColonnesPieces(rang, q));
                        q = 0;
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndDisabled();

                    // Retirer — grisé si rien, ou plus qu'en banque
                    ImGui::TableSetColumnIndex(6);
                    ImGui::BeginDisabled(q <= 0 || q > m.enBanque);
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.85f));
                    if (Sfx::Button("Retirer##retirer", ImVec2(largeurBouton, haut))) {
                        EcrireGeste("retirer", ColonnesPieces(rang, q));
                        q = 0;
                    }
                    ImGui::PopStyleColor();
                    ImGui::EndDisabled();

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            RetirerStyle();
        }

        /** Un choix de monnaie : trois boutons plats, l'élu en or avec un
         *  filet dessous. Rend true si le choix a changé. */
        bool ChoixMonnaie(const char* a_id, int& a_rang, float a_largeur, float a_haut)
        {
            bool change = false;
            for (int i = 0; i < kNbMonnaies; ++i) {
                if (i > 0) ImGui::SameLine(0.0f, 0.0f);
                const Monnaie& m = g_monnaies[i];
                const bool elu = a_rang == i;
                ImGui::PushID(i);
                ImGui::BeginDisabled(!m.recue);
                ImGui::PushStyleColor(ImGuiCol_Text, elu ? Theme::GoldCol() : Theme::Chrome(0.70f));
                const std::string etiquette = (m.recue ? m.nom : std::string("?")) + a_id;
                if (Sfx::Button(etiquette.c_str(), ImVec2(a_largeur, a_haut)) && !elu) {
                    a_rang = i;
                    change = true;
                }
                ImGui::PopStyleColor();
                ImGui::EndDisabled();
                if (elu) FiletSousItem();
                ImGui::PopID();
            }
            return change;
        }

        /** LE CHANGE — sur la bourse portée, pas sur le compte, et EXACT :
         *  N pièces de `de` contre l'équivalent EXACT en `vers`. Vers le haut
         *  N doit être un multiple du rapport ; vers le bas c'est toujours
         *  exact. Rien ne naît, rien ne meurt.
         *
         *  Le calcul d'ici n'est qu'un CONFORT pour griser le bouton et dire
         *  pourquoi ; le serveur rejuge tout (relaisBanque.ts, `changer` de
         *  shared/monnaies.ts) et c'est sa phrase qui fera foi. */
        void BlocChange(float a_S)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            ImGui::TextUnformatted("Change");
            ImGui::PopStyleColor();

            PousserStyle(a_S);
            const float haut = ImGui::GetFrameHeight();
            const float largeurChoix = ImGui::CalcTextSize("Septime").x + 20.0f * a_S;

            /* de ≠ vers, toujours : choisir pour `de` la monnaie de `vers`
               pousse `vers` sur la suivante, et inversement. Un change d'une
               monnaie vers elle-même n'a pas de sens, le serveur le refuserait. */
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.70f));
            ImGui::TextUnformatted("Donner");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 12.0f * a_S);
            if (ChoixMonnaie("##de", g_changeDe, largeurChoix, haut) && g_changeVers == g_changeDe) {
                g_changeVers = (g_changeDe + 1) % kNbMonnaies;
            }
            ImGui::SameLine(0.0f, 12.0f * a_S);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.70f));
            ImGui::TextUnformatted("contre");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 12.0f * a_S);
            if (ChoixMonnaie("##vers", g_changeVers, largeurChoix, haut) && g_changeDe == g_changeVers) {
                g_changeDe = (g_changeVers + 1) % kNbMonnaies;
            }

            const Monnaie& de = g_monnaies[g_changeDe];
            const Monnaie& vers = g_monnaies[g_changeVers];

            /* LA QUANTITÉ DONNÉE EST BORNÉE À CE QUE LE SERVEUR ACCEPTE DANS
               UN GESTE, PAS À LA BOURSE. Bornée à `surToi`, la phrase « pas
               exact — tu n'as que … sur toi » quelques lignes plus bas ne
               pouvait JAMAIS s'afficher : le champ refusait le nombre avant
               qu'elle ait à le dire, et le joueur voyait un bouton gris sans
               raison. Le champ reste fermé quand il n'y a rien à donner —
               taper une quantité sur une bourse vide n'apprendrait rien. */
            const int borne = (de.recue && de.surToi > 0) ? kQuantiteMax : 0;
            g_changeQuantite = std::clamp(g_changeQuantite, 0, borne);
            const float largeurQuantite = ImGui::CalcTextSize("1,000,000").x + 2.0f * haut + 12.0f * a_S;
            ImGui::SetNextItemWidth(largeurQuantite);
            ImGui::BeginDisabled(borne <= 0);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.92f));
            ImGui::InputInt("##vk_banque_change_q", &g_changeQuantite, 1, 10);
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            g_changeQuantite = std::clamp(g_changeQuantite, 0, borne);

            /* Le résultat, ou la raison du refus local. Vers le haut, le
               multiple exigé est vers/de ; vers le bas tout est exact. La
               bourse est jugée APRÈS l'exactitude : un joueur qui n'a pas
               assez de pièces apprend d'abord ce qu'il faudrait donner. */
            std::string resultat;
            bool exact = false;
            if (de.recue && vers.recue && g_changeQuantite > 0 && de.valeur > 0 && vers.valeur > 0 &&
                g_changeDe != g_changeVers) {
                const long long donne = static_cast<long long>(g_changeQuantite) * de.valeur;
                if (donne % vers.valeur != 0) {
                    const int multiple = vers.valeur / de.valeur;
                    resultat = "pas exact — il faut un multiple de " + Groupe(multiple);
                } else if (g_changeQuantite > de.surToi) {
                    resultat = "pas exact — tu n'as que " + Libelle(de.surToi, de) + " sur toi";
                } else {
                    const int recu = static_cast<int>(donne / vers.valeur);
                    resultat = Libelle(g_changeQuantite, de) + " contre " + Libelle(recu, vers);
                    exact = true;
                }
            }

            /* [quantité] [Changer] puis le résultat, sur UNE ligne : le
               bouton juste après le champ, le texte après le bouton — rien
               ne se chevauche quel que soit le texte. */
            ImGui::SameLine(0.0f, 8.0f * a_S);
            const float largeurBouton = ImGui::CalcTextSize("Changer").x + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f * a_S;
            ImGui::BeginDisabled(!exact);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.85f));
            if (Sfx::Button("Changer##vk_banque_changer", ImVec2(largeurBouton, haut))) {
                // Les ids REÇUS (`septime|mede|titus`), jamais les noms affichés.
                EcrireGeste("changer",
                    Assainir(de.id.c_str()) + "\t" + Assainir(vers.id.c_str()) + "\t" + std::to_string(g_changeQuantite));
                g_changeQuantite = 0;
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();

            if (!resultat.empty()) {
                ImGui::SameLine(0.0f, 12.0f * a_S);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, exact ? Theme::Chrome(0.85f) : RougeSombre());
                ImGui::TextUnformatted(resultat.c_str());
                ImGui::PopStyleColor();
            }

            RetirerStyle();
        }

        /** Les dernières opérations — jusqu'à dix, la plus récente en tête,
         *  l'heure locale courte puis le libellé composé par le serveur. */
        void BlocOperations(float a_S)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            ImGui::TextUnformatted("Dernières opérations");
            ImGui::PopStyleColor();

            if (g_operations.empty()) {
                ImGui::TextDisabled("Aucune opération encore.");
                return;
            }

            const float ligne = ImGui::GetTextLineHeightWithSpacing();
            const bool defile = g_operations.size() > kOperationsVisibles;
            if (defile) {
                ImGui::BeginChild("##vk_banque_operations_def",
                    ImVec2(0.0f, ligne * static_cast<float>(kOperationsVisibles)), ImGuiChildFlags_None);
            }
            const float largeurQuand = ImGui::CalcTextSize("00/00 00:00").x + 12.0f * a_S;
            for (std::size_t i = 0; i < g_operations.size(); ++i) {
                const Operation& op = g_operations[i];
                const std::string quand = HeureLocale(op.quand);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TextUnformatted(quand.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(largeurQuand, 0.0f);
                // Un prélèvement (taxe, loyer) se lit plus effacé : ce n'est
                // pas un geste du joueur.
                const bool prelevement = op.type == "prelevement";
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(prelevement ? 0.60f : 0.85f));
                ImGui::TextUnformatted(op.libelle.c_str());
                ImGui::PopStyleColor();
            }
            if (defile) ImGui::EndChild();
        }
    }

    // ── l'interface publique ──────────────────────────────────────────────

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kCheminGestes, "w")) {
            std::fclose(f);
            g_pret = true;
            SKSE::log::info("[BANQUE] pont pret ({})", kCheminGestes);
        } else {
            SKSE::log::warn("[BANQUE] impossible d'ouvrir {} — le comptoir restera sourd", kCheminGestes);
        }
        // UN ÉTAT RESCAPÉ D'UN PLANTAGE ferait surgir le comptoir au lancement
        // du jeu : au boot g_seqEtat repart à 0, et le premier Tick lirait un
        // « phase ouverte » vieux d'une session. On repart d'une page blanche.
        if (std::FILE* f = std::fopen(kCheminEtat, "w")) {
            std::fclose(f);
        }
    }

    void Tick()
    {
        if (!g_pret) return;
        ++g_tic;
        LireEtat();

        if (g_messageRestant > 0) {
            if (--g_messageRestant == 0) g_message.clear();
        }

        /* Le chien de garde : la racine s'est refermée sans passer par notre
           couche Échap (Tab, un menu vanilla, un autre de nos écrans qui a la
           priorité au rendu) — après deux secondes sans une trame dessinée,
           on se ferme et on le DIT, sinon le serveur nous croit toujours
           devant le comptoir. */
        if (g_ouvert && g_tic - g_dernierDessin > kTramesSansDessin) {
            SKSE::log::info("[BANQUE] comptoir plus dessine depuis {} trames : fermeture", g_tic - g_dernierDessin);
            Fermer();
        }
    }

    bool Ouvert()
    {
        return g_ouvert;
    }

    bool Fermer()
    {
        if (!g_ouvert) return false;
        EcrireGeste("fermer", "");
        g_ouvert = false;
        return true;
    }

    void Dessiner()
    {
        if (!g_ouvert) return;
        g_dernierDessin = g_tic;

        const ImGuiIO& io = ImGui::GetIO();
        const float S = Theme::Scale();
        const float pad = Theme::PadX() * S;

        /* UN PANNEAU CENTRÉ ET DISCRET — le goût du propriétaire (29/08) :
           pas de bordure pleine, des filets d'un pixel, le monde reste
           visible autour. Plus large que le panneau d'appartenance : sept
           colonnes, dont trois nombres et deux boutons. Les bornes suivent
           l'échelle (S vaut 0,75 en 1080p, 2 en 4K) pour que la table tienne
           sur une ligne quelle que soit la taille de la police. */
        const float largeur = std::clamp(io.DisplaySize.x * 0.40f, 760.0f * S, 980.0f * S);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.40f),
            ImGuiCond_Always, ImVec2(0.5f, 0.35f));
        ImGui::SetNextWindowSize(ImVec2(largeur, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 9, 8, 236));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * S, 6.0f * S));
        ImGui::Begin("##vk_banque", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysAutoResize);
        /* IL N'EXISTE AUCUNE POLICE PLUS GRANDE : on demande une taille à la
           trame (imgui 1.92, atlas dynamique). Entière, sinon le rendu bave.
           UNE SEULE SORTIE à cette fonction — le PopFont est en bas. */
        ImGui::PushFont(nullptr, Theme::SnapPx(20.0f));

        // Les deux filets qui bordent le panneau, en haut et en bas.
        {
            const ImVec2 p = ImGui::GetWindowPos();
            const ImVec2 t = ImGui::GetWindowSize();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + t.x, p.y), OrSombre(0.35f), 1.0f);
            dl->AddLine(ImVec2(p.x, p.y + t.y - 1.0f), ImVec2(p.x + t.x, p.y + t.y - 1.0f), OrSombre(0.35f), 1.0f);
        }

        EnTete();
        ImGui::Spacing();

        TableMonnaies(S);
        ImGui::Spacing();
        ImGui::Spacing();

        BlocChange(S);
        ImGui::Spacing();
        ImGui::Spacing();

        BlocOperations(S);

        /* LE MESSAGE : ce que le serveur vient de répondre, sept secondes ;
           puis la ligne d'aide reprend sa place. */
        ImGui::Spacing();
        if (!g_message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::TextWrapped("%s", g_message.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.35f));
            ImGui::TextUnformatted("Échap : fermer");
            ImGui::PopStyleColor();
        }

        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }
}
