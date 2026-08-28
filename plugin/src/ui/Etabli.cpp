#include "ui/Etabli.h"

#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Voir Etabli.h pour l'architecture. Ici : le parsing TSV de l'état, la
// composition de l'écran, et l'écriture des gestes.

namespace FUI::Etabli
{
    namespace
    {
        constexpr const char* kCheminEtat = "Data/SKSE/Plugins/GridInventory_etabli_etat.txt";
        constexpr const char* kCheminGestes = "Data/SKSE/Plugins/GridInventory_etabli.txt";

        constexpr int kNbQualites = 8;

        struct Ingredient
        {
            RE::FormID  form = 0;
            std::string edid;              // repli quand la forme est introuvable
            int         requis = 0;
            int         possede[kNbQualites] = {};
        };

        struct Geste
        {
            std::string id;
            std::string nom;
            std::string rayon;
            std::string metier;
            int         niveau = 1;
            RE::FormID  produit = 0;
            std::string produitNom;
            int         min = 1;
            int         max = 1;
            int         qualiteMax = 1;
            /** Les 8 formes du produit décliné, dans l ordre des qualités.
             *  Vide pour un produit d atelier, dont la qualité vit dans un
             *  extra et non dans la forme. */
            std::vector<RE::FormID> declines;
            std::vector<Ingredient> ingredients;
        };

        struct Rayon
        {
            std::string id;
            std::string nom;
        };

        // ---- état reçu (le serveur fait foi) ----
        bool                      g_ouvert = false;
        std::string               g_titre;
        std::string               g_message;
        std::string               g_qualites[kNbQualites];
        std::vector<Rayon>        g_rayons;
        std::vector<Geste>        g_gestes;
        unsigned long long        g_seqEtat = 0;

        // ---- état de l'écran (local, jamais envoyé) ----
        std::string g_rayonChoisi;          // vide = tous
        int         g_qualiteChoisie = 1;
        char        g_recherche[64] = {};
        std::string g_gesteChoisi;
        int         g_messageRestant = 0;   // trames avant effacement du bandeau

        // ---- l'aperçu (mode INSPECT de l'IconCache : sa propre texture) ----
        RE::FormID g_apercuArme = 0;        // ce que Tick a demandé
        bool       g_apercuActif = false;
        // L'orientation d'ouverture, celle des icônes de la maison : le modèle
        // couché sur le dos, comme dans les cases de l'inventaire.
        constexpr float kRx0 = -90.0f, kRy0 = 0.0f, kRz0 = 0.0f;
        float g_apRx = kRx0, g_apRy = kRy0, g_apRz = kRz0;
        float g_apZoom = 1.0f;
        bool  g_apTire = false;             // le bouton gauche est tenu

        // ---- plomberie ----
        unsigned long long g_seqGeste = 0;
        bool               g_pret = false;

        void EcrireGeste(const char* a_action, const std::string& a_reste)
        {
            if (!g_pret) return;
            std::FILE* f = std::fopen(kCheminGestes, "a");
            if (!f) return;
            if (a_reste.empty()) std::fprintf(f, "%llu\t%s\n", ++g_seqGeste, a_action);
            else std::fprintf(f, "%llu\t%s\t%s\n", ++g_seqGeste, a_action, a_reste.c_str());
            std::fclose(f);
        }

        /** La forme du jeu derrière un formId. Nulle = le plugin n'est pas
         *  chargé chez ce joueur : on retombera sur l'EDID du serveur. */
        RE::TESBoundObject* Forme(RE::FormID a_id)
        {
            return a_id == 0 ? nullptr : RE::TESForm::LookupByID<RE::TESBoundObject>(a_id);
        }

        /** Une chaîne est-elle de l'UTF-8 valide ? On ne convertit que ce qui
         *  ne l'est pas : un texte déjà en UTF-8 serait sinon encodé deux fois. */
        bool EstUtf8(const char* a_s)
        {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(a_s);
            while (*p) {
                int suite;
                if (*p < 0x80) { ++p; continue; }
                else if ((*p & 0xE0) == 0xC0) suite = 1;
                else if ((*p & 0xF0) == 0xE0) suite = 2;
                else if ((*p & 0xF8) == 0xF0) suite = 3;
                else return false;
                ++p;
                for (int i = 0; i < suite; ++i) {
                    if ((*p & 0xC0) != 0x80) return false;
                    ++p;
                }
            }
            return true;
        }

        /** LES NOMS DU JEU SONT EN WINDOWS-1252, ImGui LIT DE L'UTF-8.
         *
         *  Un plugin Bethesda écrit ses FULL en 1252 : « é » y vaut l'octet
         *  0xE9. ImGui le prend pour l'amorce d'une séquence de trois octets et
         *  avale LES DEUX SUIVANTS — « qualité grossière » s'affichait
         *  « qualit?rossi? », l'espace et le « g » mangés par le caractère d'à
         *  côté. Constaté en jeu le 28/08/2026. */
        std::string VersUtf8(const char* a_s)
        {
            if (a_s == nullptr || *a_s == '\0') return std::string();
            if (EstUtf8(a_s)) return a_s;
            const int large = MultiByteToWideChar(1252, 0, a_s, -1, nullptr, 0);
            if (large <= 0) return a_s;
            std::wstring w(static_cast<size_t>(large), L'\0');
            MultiByteToWideChar(1252, 0, a_s, -1, w.data(), large);
            const int octets = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (octets <= 0) return a_s;
            std::string sortie(static_cast<size_t>(octets), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, sortie.data(), octets, nullptr, nullptr);
            sortie.resize(std::strlen(sortie.c_str()));
            return sortie;
        }

        /** Le nom LISIBLE d'un objet. Le serveur ne l'a pas pour les
         *  ingrédients — sa table ne porte qu'un EDID technique. Nous, si. */
        std::string NomDe(RE::FormID a_id, const std::string& a_repli)
        {
            if (auto* f = Forme(a_id)) {
                std::string n = VersUtf8(f->GetName());
                if (!n.empty()) return n;
            }
            return a_repli;
        }

        /** L'ENCOMBREMENT dans la grille : hauteur x largeur en cases, et le
         *  nombre de cases VRAIMENT occupees — une piece trouee en prend moins
         *  que sa boite. Rend faux quand la forme est introuvable : ResolveDef
         *  rendrait alors un def vide, soit « 1 x 1 », un mensonge silencieux. */
        bool Encombrement(RE::FormID a_id, int& a_haut, int& a_larg, int& a_cases)
        {
            RE::TESBoundObject* obj = Forme(a_id);
            if (obj == nullptr) return false;
            const auto def = Grid::ResolveDef(obj);
            // Le def borne la largeur a 16, le plateau n'en fait que kCols :
            // annoncer 16 promettrait une place qui n'existe pas.
            a_larg = (std::min)((std::max)(1, def.w), Grid::kCols);
            a_haut = (std::max)(1, def.h);
            a_cases = (std::max)(1, Grid::CellSpanOf(obj));
            return true;
        }

        /** LA FICHE DE L'OBJET — ce que le proprietaire appelle « les stats »
         *  (28/08/2026) : l'encombrement, puis les degats ou l'armure.
         *
         *  Les valeurs sont celles de la FORME DE BASE, jamais celles ajustees
         *  au porteur : `GetDamage(entree)` du joueur replie sa competence et
         *  ses perks, et deux joueurs devant le meme etabli liraient alors des
         *  nombres differents pour le meme produit. */
        void Fiche(RE::FormID a_id)
        {
            int h = 0, l = 0, cases = 0;
            if (Encombrement(a_id, h, l, cases)) {
                if (cases == h * l) ImGui::Text("Encombrement   %d x %d", h, l);
                // Une piece trouee : la boite ment sur ce qu'elle coute vraiment.
                else ImGui::Text("Encombrement   %d x %d  (%d cases)", h, l, cases);
            }
            RE::TESBoundObject* obj = Forme(a_id);
            if (obj == nullptr) return;
            if (auto* arme = obj->As<RE::TESObjectWEAP>()) {
                ImGui::Text("Degats   %d", static_cast<int>(arme->GetAttackDamage()));
            } else if (auto* armure = obj->As<RE::TESObjectARMO>()) {
                // GetArmorRating n'est PAS const et rend un float : le pointeur
                // doit rester non const, et la valeur se tronque a l'entier.
                ImGui::Text("Armure   %d", static_cast<int>(armure->GetArmorRating()));
            }
        }

        /** LE NOM DE CE QU ON VA FABRIQUER, à la qualité choisie.
 *
         *  Le propriétaire l a demandé le 28/08/2026 : la liste affichait
         *  « Petite meule », le nom du TOUR DE MAIN, quand le joueur cherche
         *  « Charbon pauvre (qualité grossière) » — ce qu il obtient.
 *
         *  Un objet décliné existe en huit FORMES, et chacune porte sa qualité
         *  dans son nom : il suffit de prendre la bonne. Un produit d atelier,
         *  lui, n en a qu une — sa qualité vit dans un extra — alors on la lui
         *  ajoute, pour que la liste réponde toujours à la même question. */
        std::string NomFabrique(const Geste& a_g, int a_qualite)
        {
            const int q = (a_qualite < 1 || a_qualite > kNbQualites) ? 1 : a_qualite;
            if (static_cast<size_t>(q) <= a_g.declines.size()) {
                return NomDe(a_g.declines[static_cast<size_t>(q) - 1], a_g.produitNom);
            }
            std::string nom = NomDe(a_g.produit, a_g.produitNom);
            if (!g_qualites[q - 1].empty()) nom += " (qualité " + g_qualites[q - 1] + ")";
            return nom;
        }

        const Geste* GesteChoisi()
        {
            for (const auto& g : g_gestes) {
                if (g.id == g_gesteChoisi) return &g;
            }
            return nullptr;
        }

        /** Combien de lots ce geste permet à cette qualité, sac en main. Le
         *  serveur refera le calcul : ceci n'est QUE de l'affichage. */
        int LotsFaisables(const Geste& a_g, int a_qualite)
        {
            if (a_qualite < 1 || a_qualite > kNbQualites) return 0;
            if (a_qualite > a_g.qualiteMax) return 0;
            if (a_g.ingredients.empty()) return 0;
            int mini = 999;
            for (const auto& i : a_g.ingredients) {
                if (i.requis <= 0) continue;
                mini = (std::min)(mini, i.possede[a_qualite - 1] / i.requis);
            }
            return mini == 999 ? 0 : mini;
        }

        std::string EnMinuscules(const std::string& a_s)
        {
            std::string r = a_s;
            std::transform(r.begin(), r.end(), r.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return r;
        }

        // ── la lecture de l'état ──────────────────────────────────────────

        /** Découpe une ligne TSV en champs. Le lecteur reste sans état : un
         *  simple parcours, comme celui de l'échange. */
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

        void LireEtat()
        {
            std::FILE* f = std::fopen(kCheminEtat, "r");
            if (!f) return;

            unsigned long long seq = 0;
            bool ouvert = false;
            std::string titre, message;
            std::string qualites[kNbQualites];
            std::vector<Rayon> rayons;
            std::vector<Geste> gestes;

            char ligne[2048];
            char* c[16];
            while (std::fgets(ligne, sizeof(ligne), f)) {
                const int n = Champs(ligne, c, 16);
                if (n < 2) continue;
                if (std::strcmp(c[0], "seq") == 0) {
                    seq = std::strtoull(c[1], nullptr, 10);
                } else if (std::strcmp(c[0], "phase") == 0) {
                    ouvert = std::strcmp(c[1], "ouverte") == 0;
                } else if (std::strcmp(c[0], "titre") == 0) {
                    titre = c[1];
                } else if (std::strcmp(c[0], "message") == 0) {
                    message = c[1];
                } else if (std::strcmp(c[0], "qual") == 0 && n >= 3) {
                    const int rang = std::atoi(c[1]);
                    if (rang >= 1 && rang <= kNbQualites) qualites[rang - 1] = c[2];
                } else if (std::strcmp(c[0], "cat") == 0 && n >= 3) {
                    rayons.push_back(Rayon{ c[1], c[2] });
                } else if (std::strcmp(c[0], "geste") == 0 && n >= 11) {
                    Geste g;
                    g.id = c[1];
                    g.nom = c[2];
                    g.rayon = c[3];
                    g.niveau = std::atoi(c[4]);
                    g.produit = static_cast<RE::FormID>(std::strtoul(c[5], nullptr, 16));
                    g.produitNom = c[6];
                    g.min = std::atoi(c[7]);
                    g.max = std::atoi(c[8]);
                    g.qualiteMax = std::atoi(c[9]);
                    g.metier = c[10];
                    // Le 12e champ est facultatif : un produit non décliné
                    // n a pas de formes, et la ligne s arrête au 11e.
                    if (n >= 12) {
                        const char* d = c[11];
                        while (d && *d) {
                            const RE::FormID f = static_cast<RE::FormID>(std::strtoul(d, nullptr, 16));
                            if (f != 0) g.declines.push_back(f);
                            const char* virgule = std::strchr(d, ',');
                            d = virgule ? virgule + 1 : nullptr;
                        }
                    }
                    gestes.push_back(std::move(g));
                } else if (std::strcmp(c[0], "ing") == 0 && n >= 6) {
                    // Une ligne `ing` suit TOUJOURS le geste qu'elle décrit :
                    // le service les écrit dans cet ordre.
                    if (gestes.empty() || gestes.back().id != c[1]) continue;
                    Ingredient i;
                    i.form = static_cast<RE::FormID>(std::strtoul(c[2], nullptr, 16));
                    i.edid = c[3];
                    i.requis = std::atoi(c[4]);
                    const char* p = c[5];
                    for (int q = 0; q < kNbQualites && p && *p; ++q) {
                        i.possede[q] = std::atoi(p);
                        const char* virgule = std::strchr(p, ',');
                        p = virgule ? virgule + 1 : nullptr;
                    }
                    gestes.back().ingredients.push_back(std::move(i));
                }
            }
            std::fclose(f);

            // RIEN N'EST COMMIS TANT QUE LE SEQ N'A PAS BOUGÉ : le fichier est
            // relu quatre fois par seconde, et une lecture qui tombe pendant la
            // réécriture verrait des lignes tronquées.
            if (seq == 0 || seq == g_seqEtat) return;
            g_seqEtat = seq;

            const bool avant = g_ouvert;
            g_ouvert = ouvert;
            g_titre = std::move(titre);
            g_rayons = std::move(rayons);
            g_gestes = std::move(gestes);
            for (int q = 0; q < kNbQualites; ++q) g_qualites[q] = std::move(qualites[q]);

            if (!message.empty()) {
                g_message = std::move(message);
                g_messageRestant = 420;   // ~7 s à 60 fps
            }

            if (g_ouvert && !avant) {
                // L'établi s'ouvre : écran neuf, et on ouvre la racine — sans
                // elle, UIRoot::Render n'est jamais appelé.
                g_rayonChoisi.clear();
                g_recherche[0] = '\0';
                g_gesteChoisi = g_gestes.empty() ? std::string() : g_gestes.front().id;
                g_qualiteChoisie = 1;
                g_apRx = kRx0; g_apRy = kRy0; g_apRz = kRz0; g_apZoom = 1.0f;
                UIRoot::Open();
            }
            if (!g_ouvert && avant) g_gesteChoisi.clear();

            // Le geste choisi a pu disparaître du plateau : on ne garde jamais
            // un pointeur sur du vide.
            if (!g_gesteChoisi.empty() && GesteChoisi() == nullptr) {
                g_gesteChoisi = g_gestes.empty() ? std::string() : g_gestes.front().id;
            }
        }

        // ── les morceaux de l'écran ───────────────────────────────────────

        /** La rangée des rayons, en tête de colonne. « Tous » d'abord. */
        void RangeeRayons(float a_largeur)
        {
            const float S = Theme::Scale();
            const int nb = static_cast<int>(g_rayons.size()) + 1;
            const float espace = 4.0f * S;
            const float cote = (std::max)(56.0f * S, (a_largeur - espace * (nb - 1)) / (std::max)(1, nb));
            const float haut = 60.0f * S;

            for (int i = 0; i < nb; ++i) {
                const bool tous = (i == 0);
                const std::string id = tous ? std::string() : g_rayons[i - 1].id;
                const char* libelle = tous ? "Tous" : g_rayons[i - 1].nom.c_str();
                const bool actif = (g_rayonChoisi == id);

                if (i > 0) ImGui::SameLine(0.0f, espace);
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Button, actif ? Theme::Acc(0.40f) : Theme::Chrome(0.16f));
                ImGui::PushStyleColor(ImGuiCol_Text, actif ? Theme::GoldCol() : Theme::Chrome(0.85f));
                if (ImGui::Button(libelle, ImVec2(cote, haut))) g_rayonChoisi = id;
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }
        }

        /** Les huit qualités. Un rang que la main ne sait pas faire, ou que le
         *  sac ne peut pas fournir, est ÉTEINT — et pour deux raisons
         *  différentes, qui ne se confondent pas. */
        void RangeeQualites(float a_largeur, const Geste* a_g)
        {
            const float S = Theme::Scale();
            const float espace = 3.0f * S;
            const float cote = (std::max)(34.0f * S, (a_largeur - espace * (kNbQualites - 1)) / kNbQualites);
            const float haut = 36.0f * S;

            for (int q = 1; q <= kNbQualites; ++q) {
                const bool horsMain = (a_g == nullptr) || (q > a_g->qualiteMax);
                const bool horsSac = (a_g != nullptr) && !horsMain && LotsFaisables(*a_g, q) == 0;
                const bool actif = (g_qualiteChoisie == q);

                if (q > 1) ImGui::SameLine(0.0f, espace);
                ImGui::PushID(1000 + q);
                ImGui::PushStyleColor(ImGuiCol_Button, actif ? Theme::Acc(0.50f) : Theme::Chrome(0.16f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    actif ? Theme::GoldCol() : (horsSac ? Theme::Chrome(0.40f) : Theme::Chrome(0.85f)));
                ImGui::BeginDisabled(horsMain);
                char n[8];
                std::snprintf(n, sizeof(n), "%d", q);
                if (ImGui::Button(n, ImVec2(cote, haut))) g_qualiteChoisie = q;
                ImGui::EndDisabled();
                ImGui::PopStyleColor(2);
                ImGui::PopID();

                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    const char* nom = g_qualites[q - 1].empty() ? "?" : g_qualites[q - 1].c_str();
                    if (horsMain) ImGui::SetTooltip("%s — ta main ne sait pas encore", nom);
                    else if (horsSac) ImGui::SetTooltip("%s — il te manque de quoi", nom);
                    else ImGui::SetTooltip("%s", nom);
                }
            }
        }

        /** La liste des fabrications, filtrée par rayon et par recherche. */
        void ListeGestes()
        {
            const float S = Theme::Scale();
            const float haut = 38.0f * S;
            const std::string filtre = EnMinuscules(g_recherche);

            for (const auto& g : g_gestes) {
                if (!g_rayonChoisi.empty() && g.rayon != g_rayonChoisi) continue;
                // La recherche porte sur CE QU ON FABRIQUE, comme la liste :
                // taper « charbon » doit trouver la meule qui en donne.
                const std::string affiche = NomFabrique(g, g_qualiteChoisie);
                if (!filtre.empty() && EnMinuscules(affiche).find(filtre) == std::string::npos) continue;

                const bool actif = (g.id == g_gesteChoisi);
                const bool faisable = LotsFaisables(g, g_qualiteChoisie) > 0;

                ImGui::PushID(g.id.c_str());
                const ImVec2 depart = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##ligne", actif, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, haut))) {
                    g_gesteChoisi = g.id;
                    // Une qualité hors de portée du nouveau geste se replie sur
                    // ce qu'il sait faire, plutôt que de rester éteinte.
                    if (g_qualiteChoisie > g.qualiteMax) g_qualiteChoisie = g.qualiteMax;
                }
                const float largeur = ImGui::GetItemRectSize().x;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float milieu = depart.y + (haut - ImGui::GetTextLineHeight()) * 0.5f;
                dl->AddText(ImVec2(depart.x + 10.0f * S, milieu),
                    faisable ? Theme::Chrome(0.95f) : Theme::Chrome(0.45f), affiche.c_str());

                char niv[24];
                std::snprintf(niv, sizeof(niv), "NIV %d", g.niveau);
                const float l = ImGui::CalcTextSize(niv).x;
                dl->AddText(ImVec2(depart.x + largeur - l - 12.0f * S, milieu), Theme::Chrome(0.50f), niv);

                // Un filet sous chaque ligne : la maquette du propriétaire les
                // sépare, et une liste nue devient illisible passé dix entrées.
                dl->AddLine(ImVec2(depart.x, depart.y + haut), ImVec2(depart.x + largeur, depart.y + haut),
                    Theme::Chrome(0.12f), 1.0f);
                ImGui::PopID();
            }
        }

        /** L'APERÇU — la capture d'inspection, dessinée grand par-dessus le
         *  monde, ET TOURNABLE : glisser fait pivoter, la molette rapproche,
         *  R remet la pose d'ouverture. C'est la mécanique de la touche C du
         *  greffon, reprise telle quelle. */
        void Apercu(const ImVec2& a_pos, const ImVec2& a_taille)
        {
            auto* icones = IconCache::GetSingleton();
            if (!icones) return;

            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(a_pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(a_taille, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::Begin("##vk_etabli_apercu", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoBackground);

            /* Rotation à la main plutôt qu'avec un bouton invisible : celui-ci
               s'approprierait l'ActiveId sur toute la zone et se battrait avec
               les widgets voisins (le motif est écrit dans UIRoot::DrawInspect). */
            if (!g_apTire && io.MouseClicked[0] && ImGui::IsWindowHovered()) g_apTire = true;
            if (g_apTire && !io.MouseDown[0]) g_apTire = false;

            bool bouge = false;
            if (g_apTire && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                constexpr float kDegParPixel = 0.6f;
                if (io.KeyShift) {
                    g_apRy += io.MouseDelta.x * kDegParPixel;
                } else {
                    g_apRz += io.MouseDelta.x * kDegParPixel;
                    g_apRx += io.MouseDelta.y * kDegParPixel;
                }
                auto borner = [](float& v) {
                    while (v > 180.0f) v -= 360.0f;
                    while (v < -180.0f) v += 360.0f;
                };
                borner(g_apRx); borner(g_apRy); borner(g_apRz);
                bouge = true;
            }
            if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) {
                g_apZoom = std::clamp(g_apZoom * (1.0f + io.MouseWheel * 0.12f), 0.5f, 3.0f);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.WantTextInput) {
                g_apRx = kRx0; g_apRy = kRy0; g_apRz = kRz0; g_apZoom = 1.0f;
                bouge = true;
            }
            /* La capture ne se refait QUE sur un changement d'orientation. Le
               zoom, lui, met simplement à l'échelle le sprite déjà là — le
               re-photographier à chaque trame ferait tomber le jeu à 30 images
               par seconde pour une image identique. */
            if (bouge) icones->SetInspectRot(g_apRx, g_apRy, g_apRz);

            /* LA CAPTURE PEUT ÊTRE REFUSÉE, et c est voulu : sous 2 Go de
               mémoire libre le greffon suspend ses prises, parce que le
               chargeur de maillages du moteur a déjà planté sous pression.
               Constaté en jeu le 28/08/2026 — l écran ne montrait alors RIEN
               du tout. On se rabat sur l icône de la tuile, qui est déjà en
               cache et ne coûte pas une prise de plus. */
            const IconCache::Icon* ic = icones->InspectIcon();
            if (ic == nullptr || ic->srv == nullptr) {
                if (auto* forme = Forme(g_apercuArme)) ic = icones->Get(forme);
            }
            if (ic && ic->srv && ic->w > 0 && ic->h > 0) {
                /* À SON RAPPORT D'ASPECT. Le forcer en carré étirait la capture
                   et rendait une bouillie — constaté en jeu le 28/08/2026. */
                const float boite = (std::min)(a_taille.x, a_taille.y) * g_apZoom;
                const float ech = (std::min)(boite / static_cast<float>(ic->w),
                                             boite / static_cast<float>(ic->h));
                const ImVec2 sz(static_cast<float>(ic->w) * ech, static_cast<float>(ic->h) * ech);
                const ImVec2 c(a_pos.x + a_taille.x * 0.5f, a_pos.y + a_taille.y * 0.5f);
                ImGui::GetWindowDrawList()->AddImage(reinterpret_cast<ImTextureID>(ic->srv),
                    ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                    ImVec2(c.x + sz.x * 0.5f, c.y + sz.y * 0.5f));
            } else {
                // Ni prise ni icône : on le DIT. Un cadre vide passerait
                // pour une panne alors que le greffon attend simplement.
                const char* mot = "l'aperçu attend que la mémoire se libère";
                const ImVec2 t = ImGui::CalcTextSize(mot);
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(a_pos.x + (a_taille.x - t.x) * 0.5f, a_pos.y + a_taille.y * 0.5f),
                    Theme::Chrome(0.45f), mot);
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        /** Le panneau de droite : le nom, la recette, et le bouton. */
        void PanneauDetail(const Geste* a_g, const ImVec2& a_pos, const ImVec2& a_taille)
        {
            const float S = Theme::Scale();
            ImGui::SetNextWindowPos(a_pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(a_taille, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 11, 10, 224));
            ImGui::Begin("##vk_etabli_detail", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);
            /* Le detail se lit de loin : sa propre taille de texte. ATTENTION,
               cette fonction a DEUX sorties — le retour anticipe et la fin —
               et la pile de polices deborderait dans le reste de la trame si
               l une des deux oubliait son PopFont. */
            ImGui::PushFont(nullptr, Theme::SnapPx(21.0f));

            if (a_g == nullptr) {
                ImGui::TextDisabled("Rien à fabriquer ici.");
                ImGui::Spacing();
                ImGui::TextWrapped("Cet établi ne sert que les métiers que tu exerces.");
                ImGui::PopFont();
                ImGui::End();
                ImGui::PopStyleColor();
                return;
            }

            const std::string nom = NomFabrique(*a_g, g_qualiteChoisie);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::TextUnformatted(nom.c_str());
            ImGui::PopStyleColor();

            const char* nomQualite = g_qualites[g_qualiteChoisie - 1].empty()
                ? "?" : g_qualites[g_qualiteChoisie - 1].c_str();
            // Le NOM porte déjà la qualité : la répéter ici serait du bruit.
            (void)nomQualite;
            if (a_g->min == a_g->max) ImGui::TextDisabled("×%d", a_g->min);
            else ImGui::TextDisabled("×%d à %d", a_g->min, a_g->max);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Fiche");
            Fiche(a_g->produit);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Recette");
            for (const auto& i : a_g->ingredients) {
                const int ai = i.possede[g_qualiteChoisie - 1];
                const bool assez = ai >= i.requis;
                if (!assez) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Col(ImVec4(0.80f, 0.32f, 0.28f, 1.0f)));
                ImGui::Text("%s   %d / %d", NomDe(i.form, i.edid).c_str(), ai, i.requis);
                if (!assez) ImGui::PopStyleColor();
            }

            const int lots = LotsFaisables(*a_g, g_qualiteChoisie);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::BeginDisabled(lots <= 0);
            if (ImGui::Button("Fabriquer", ImVec2(-1.0f, 40.0f * S))) {
                char reste[128];
                std::snprintf(reste, sizeof(reste), "%s\t%d", a_g->id.c_str(), g_qualiteChoisie);
                EcrireGeste("crafter", reste);
            }
            ImGui::EndDisabled();
            if (lots > 0) ImGui::TextDisabled("de quoi en faire %d", lots);
            else ImGui::TextDisabled("il te manque de quoi");

            ImGui::PopFont();
            ImGui::End();
            ImGui::PopStyleColor();
        }
    }

    // ── l'interface publique ──────────────────────────────────────────────

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kCheminGestes, "w")) {
            std::fclose(f);
            g_pret = true;
            SKSE::log::info("[ETABLI] pont pret ({})", kCheminGestes);
        } else {
            SKSE::log::warn("[ETABLI] impossible d'ouvrir {} — l'etabli restera sourd", kCheminGestes);
        }
        // UN ÉTAT RESCAPÉ D'UN PLANTAGE ferait surgir l'établi au lancement du
        // jeu : au boot g_seqEtat repart à 0, et le premier Tick lirait un
        // « phase ouverte » vieux d'une session. On repart d'une page blanche.
        if (std::FILE* f = std::fopen(kCheminEtat, "w")) {
            std::fclose(f);
        }
    }

    void Tick()
    {
        LireEtat();

        if (g_messageRestant > 0) {
            if (--g_messageRestant == 0) g_message.clear();
        }

        /* L'APERÇU S'ARME ICI, hors de la trame : ClearInspect libère une SRV
           que la liste d'affichage de la trame en cours peut encore référencer
           (le motif est écrit dans IconCache.h). */
        auto* icones = IconCache::GetSingleton();
        if (!icones) return;

        const Geste* g = g_ouvert ? GesteChoisi() : nullptr;
        const RE::FormID voulu = (g != nullptr) ? g->produit : 0;
        if (voulu == g_apercuArme) return;

        g_apercuArme = voulu;
        if (voulu == 0) {
            if (g_apercuActif) {
                icones->ClearInspect();
                g_apercuActif = false;
            }
            return;
        }
        if (auto* forme = Forme(voulu)) {
            // Changer d'objet remet la pose d'ouverture : garder l'angle du
            // précédent montrerait le suivant de dos, sans raison.
            g_apRx = kRx0; g_apRy = kRy0; g_apRz = kRz0; g_apZoom = 1.0f;
            icones->SetInspect(forme, g_apRx, g_apRy, g_apRz);
            g_apercuActif = true;
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
        g_gesteChoisi.clear();
        return true;
    }

    void Dessiner()
    {
        if (!g_ouvert) return;

        const ImGuiIO& io = ImGui::GetIO();
        const float S = Theme::Scale();
        const float pad = Theme::PadX() * S;

        /* LA COLONNE DE GAUCHE prend la place du sac : même bord, même hauteur,
           un fond OPAQUE. La première mouture la laissait transparente et sans
           marges — le propriétaire n'y a reconnu ni sa maquette ni un panneau
           (28/08/2026). */
        /* LA LARGEUR EST UNE FRACTION DE L ECRAN, jamais un plafond en
           unites de design : Theme::Scale() vaut 0.75 en 1080p (il derive
           de la hauteur d ecran), et « 620 * S » rendait 465 px la ou la
           maquette en demandait 768 — 24 % au lieu de 40 %. Mesure du
           28/08/2026. Les bornes en pixels gardent l ecran utilisable des
           deux cotes : lisible en 1280, pas demesure en 4K. */
        const float largeur = std::clamp(io.DisplaySize.x * 0.40f, 420.0f, 900.0f);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(largeur, io.DisplaySize.y), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 9, 8, 236));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * S, 6.0f * S));
        ImGui::Begin("##vk_etabli", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
        /* IL N EXISTE AUCUNE POLICE PLUS GRANDE : les deux faces du greffon
           sont cuites a 17. On demande donc une taille a la trame — c est la
           voie d imgui 1.92, dont l atlas est dynamique. La taille DOIT etre
           entiere (SnapPx l arrondit), sans quoi le rendu bave. */
        ImGui::PushFont(nullptr, Theme::SnapPx(20.0f));

        const float interne = ImGui::GetContentRegionAvail().x;
        const Geste* choisi = GesteChoisi();

        if (!g_titre.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::TextUnformatted(g_titre.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        RangeeRayons(interne);
        ImGui::Spacing();
        RangeeQualites(interne, choisi);
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##vk_etabli_rech", "recherche", g_recherche, sizeof(g_recherche));
        ImGui::Spacing();
        ImGui::Separator();

        ImGui::BeginChild("##vk_etabli_liste", ImVec2(0.0f, 0.0f), false);
        ListeGestes();
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        /* L'APERÇU, au milieu du monde — comme la maquette. Tournable. */
        const float libre = io.DisplaySize.x - largeur;
        const ImVec2 tailleAp((std::min)(libre * 0.62f, io.DisplaySize.y * 0.46f),
                              io.DisplaySize.y * 0.46f);
        const ImVec2 posAp(largeur + (libre - tailleAp.x) * 0.5f, io.DisplaySize.y * 0.06f);
        Apercu(posAp, tailleAp);

        /* LA RECETTE, sous l'aperçu. */
        /* Agrandir le texte n agrandit NI la fenetre NI les marges : la boite
           doit monter du meme facteur, et la fiche y ajoute trois lignes. */
        const ImVec2 tailleDetail(std::clamp(libre * 0.50f, 340.0f, 640.0f),
                                  std::clamp(io.DisplaySize.y * 0.36f, 260.0f, 520.0f));
        const ImVec2 posDetail(largeur + (libre - tailleDetail.x) * 0.5f, io.DisplaySize.y * 0.58f);
        PanneauDetail(choisi, posDetail, tailleDetail);

        /* LE BANDEAU : ce que le serveur vient de répondre. */
        if (!g_message.empty()) {
            ImGui::SetNextWindowPos(ImVec2(largeur + libre * 0.5f, io.DisplaySize.y - 48.0f * S),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            ImGui::Begin("##vk_etabli_msg", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextUnformatted(g_message.c_str());
            ImGui::End();
        }

        /* L'AIDE, discrète, en pied de l'aperçu : sans elle personne ne
           devinerait qu'on peut tourner l'objet. */
        /* Dessinee hors de toute fenetre : aucun PushFont ne l atteint, sa
           taille doit etre passee explicitement (forme a cinq arguments). */
        ImGui::GetForegroundDrawList()->AddText(
            ImGui::GetFont(), Theme::SnapPx(15.0f),
            ImVec2(posAp.x, posAp.y + tailleAp.y + 4.0f * S), Theme::Chrome(0.45f),
            "glisser : tourner   ·   maj+glisser : lacet   ·   molette : zoom   ·   R : remettre");
    }
}
