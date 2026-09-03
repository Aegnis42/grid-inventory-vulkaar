#include "ui/Maison.h"
#include "ui/Etabli.h"

#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Voir Maison.h pour l'architecture. Ici : le parsing TSV de l'état, la
// composition du panneau, et l'écriture des gestes.

namespace FUI::Maison
{
    namespace
    {
        constexpr const char* kCheminEtat = "Data/SKSE/Plugins/GridInventory_maison_etat.txt";
        constexpr const char* kCheminGestes = "Data/SKSE/Plugins/GridInventory_maison.txt";

        struct Membre
        {
            int         personnageId = 0;
            std::string matricule;
            std::string nom;       // vide = le spectateur n'a pas le droit de le lire
            std::string role;      // proprietaire | locataire — la clé, jamais affichée
            std::string libelle;   // ce que le serveur veut qu'on lise
            bool        enJeu = false;
        };

        struct Candidat
        {
            int         personnageId = 0;
            std::string matricule;
            std::string nom;
        };

        // ---- état reçu (le serveur fait foi) ----
        bool                  g_ouvert = false;
        std::string           g_nom;
        bool                  g_gerer = false;      // ajouter / retirer / basculer
        bool                  g_renommer = false;
        std::string           g_moi;                // mon matricule : ma ligne se reconnaît
        std::string           g_message;
        std::vector<Membre>   g_membres;
        std::string           g_rechercheEcho;      // la requête à laquelle répondent les résultats
        std::vector<Candidat> g_resultats;
        unsigned long long    g_seqEtat = 0;

        // ---- état de l'écran (local, jamais envoyé tel quel) ----
        char g_nomEdite[256] = {};   // 40 caractères du registre, jusqu'à 4 octets chacun
        /* Le champ du nom a-t-il le clavier ? Tant qu'oui, l'écho du serveur
           ne doit pas y recopier `nom` : chaque poussée (un membre ajouté par
           le staff) effacerait ce que le joueur est en train de taper. */
        bool g_nomActif = false;
        char g_recherche[64] = {};
        bool g_rechercheSale = false;      // le texte a changé depuis le dernier envoi
        int  g_rechercheStable = 0;        // trames sans frappe
        int  g_messageRestant = 0;         // trames avant effacement du message

        // ---- plomberie ----
        unsigned long long g_seqGeste = 0;
        bool               g_pret = false;
        int                g_tic = 0;
        int                g_dernierDessin = 0;

        /* Le temps qu'une poussée serveur attend avant de partir : vingt
           trames sans frappe, comme l'offre de l'échange. Le serveur a sa
           cadence propre pour `chercher` (300 ms) ; sans ce délai chaque
           lettre tapée ferait un aller-retour HTTP et le joueur verrait les
           refus de cadence défiler. */
        constexpr int kTramesStables = 20;
        /* Le chien de garde : la racine s'est refermée sans passer par nous
           (Tab, un menu vanilla) — sans cela le serveur garderait le joueur
           parmi les panneaux ouverts et lui repousserait des états. */
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

        /** Combien de CARACTÈRES (pas d'octets) : « é » en fait un seul, et
         *  la règle des deux caractères doit valoir pareil pour « Ém ». */
        int Caracteres(const char* a_s)
        {
            int n = 0;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(a_s); *p; ++p) {
                if ((*p & 0xC0) != 0x80) ++n;
            }
            return n;
        }

        void EcrireGeste(const char* a_action, const std::string& a_reste)
        {
            if (!g_pret) return;
            std::FILE* f = std::fopen(kCheminGestes, "a");
            if (!f) return;
            if (a_reste.empty()) std::fprintf(f, "%llu\t%s\n", ++g_seqGeste, a_action);
            else std::fprintf(f, "%llu\t%s\t%s\n", ++g_seqGeste, a_action, a_reste.c_str());
            std::fclose(f);
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

        void LireEtat()
        {
            std::FILE* f = std::fopen(kCheminEtat, "r");
            if (!f) return;

            /* LE NUMÉRO DE SÉQUENCE SE LIT EN PREMIER, ET ON SORT AUSSITÔT.
               Ce Tick est appelé à CHAQUE trame ; le fichier est réécrit ENTIER
               à chaque poussée serveur et sa première ligne porte « seq <n> ».
               Le panneau fait quelques centaines d'octets, mais l'habitude
               vaut d'être gardée : rien n'a bougé, pas un octet de plus. */
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
            bool gerer = false, renommer = false;
            bool finVue = false;
            std::string nom, moi, message, echo;
            std::vector<Membre> membres;
            std::vector<Candidat> resultats;

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
                } else if (std::strcmp(c[0], "nom") == 0) {
                    nom = c[1];
                } else if (std::strcmp(c[0], "droits") == 0 && n >= 3) {
                    gerer = std::atoi(c[1]) != 0;
                    renommer = std::atoi(c[2]) != 0;
                } else if (std::strcmp(c[0], "moi") == 0) {
                    moi = c[1];
                } else if (std::strcmp(c[0], "message") == 0) {
                    message = c[1];
                } else if (std::strcmp(c[0], "membre") == 0 && n >= 7) {
                    Membre m;
                    m.personnageId = std::atoi(c[1]);
                    m.matricule = c[2];
                    m.nom = c[3];
                    m.role = c[4];
                    m.libelle = c[5];
                    m.enJeu = std::atoi(c[6]) != 0;
                    membres.push_back(std::move(m));
                } else if (std::strcmp(c[0], "recherche") == 0) {
                    echo = c[1];
                } else if (std::strcmp(c[0], "resultat") == 0 && n >= 4) {
                    resultats.push_back(Candidat{ std::atoi(c[1]), c[2], c[3] });
                }
            }
            std::fclose(f);

            /* RIEN N'EST COMMIS TANT QUE LE SEQ N'A PAS BOUGÉ, NI SANS LA
               SENTINELLE : le fichier est relu pendant qu'on l'écrit, et une
               lecture tronquée acceptée se lirait « un membre en moins » — et
               se figerait jusqu'à la prochaine poussée. Une lecture rejetée ne
               mémorise RIEN, pas même le seq : on la refera à la trame d'après. */
            if (seq == 0 || seq == g_seqEtat || !finVue) return;
            g_seqEtat = seq;

            const bool avant = g_ouvert;
            g_ouvert = ouvert;
            g_nom = std::move(nom);
            g_gerer = gerer;
            g_renommer = renommer;
            g_moi = std::move(moi);
            g_membres = std::move(membres);
            g_rechercheEcho = std::move(echo);
            g_resultats = std::move(resultats);

            if (!message.empty()) {
                g_message = std::move(message);
                g_messageRestant = kTramesMessage;
            }

            if (g_ouvert && !avant) {
                // Le panneau s'ouvre : écran neuf, et on ouvre la racine — sans
                // elle, UIRoot::Render n'est jamais appelé. Le chien de garde
                // part d'ici, sinon il mordrait avant la première trame dessinée.
                g_recherche[0] = '\0';
                g_rechercheSale = false;
                g_rechercheStable = 0;
                g_nomActif = false;
                // g_message est GARDÉ : la poussée d'ouverture peut en porter un
                // (« Maison créée. ») — il vient d'être posé quelques lignes plus haut.
                g_dernierDessin = g_tic;
                UIRoot::Open();
            }
            if (!g_ouvert && avant) {
                /* Le SERVEUR nous ferme (retiré de la maison, sorti de la
                   cellule) : la racine, ouverte pour nous, se referme avec —
                   sinon le joueur tombe sur son inventaire sans l'avoir demandé.
                   Sauf si l'établi la tient encore : elle est à lui. */
                if (!Etabli::Ouvert()) UIRoot::Close();
            }

            if (!g_nomActif) {
                std::snprintf(g_nomEdite, sizeof(g_nomEdite), "%s", g_nom.c_str());
            }
        }

        /* Rogne les blancs aux deux bords (espaces, tabulations, retours). */
        std::string Rogner(const std::string& a_texte)
        {
            const char* blancs = " \t\r\n";
            const std::size_t debut = a_texte.find_first_not_of(blancs);
            if (debut == std::string::npos) return std::string();
            const std::size_t fin = a_texte.find_last_not_of(blancs);
            return a_texte.substr(debut, fin - debut + 1);
        }

        // ── les morceaux du panneau ───────────────────────────────────────

        /** Un filet d'un pixel sous le dernier widget posé. */
        void FiletSousItem()
        {
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), OrSombre(0.35f), 1.0f);
        }

        /** « Bâtiment : » et le nom — éditable si le serveur le permet. */
        void LigneNom(float a_S)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Bâtiment :");
            ImGui::SameLine(0.0f, 10.0f * a_S);

            if (!g_renommer) {
                ImGui::PushStyleColor(ImGuiCol_Text, g_nom.empty() ? Theme::Chrome(0.45f) : Theme::GoldCol());
                ImGui::TextUnformatted(g_nom.empty() ? "sans nom" : g_nom.c_str());
                ImGui::PopStyleColor();
                g_nomActif = false;
                return;
            }

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            if (ImGui::InputTextWithHint("##vk_maison_nom", "donne un nom au bâtiment", g_nomEdite,
                    sizeof(g_nomEdite), ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string voulu = Assainir(g_nomEdite);
                // Entrée sur un nom inchangé n'a rien à demander au serveur.
                if (voulu != g_nom) EcrireGeste("renommer", voulu);
            }
            ImGui::PopStyleColor(4);
            g_nomActif = ImGui::IsItemActive();
            FiletSousItem();
        }

        /** La loupe, dessinée à la main : les polices cuites du greffon n'ont
         *  pas de glyphe pour elle. */
        void Loupe(const ImVec2& a_case, float a_S)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = 5.5f * a_S;
            const ImVec2 centre(a_case.x + r + 2.0f * a_S, a_case.y + r + 3.0f * a_S);
            dl->AddCircle(centre, r, Theme::Chrome(0.6f), 0, 1.5f * a_S);
            dl->AddLine(ImVec2(centre.x + r * 0.7f, centre.y + r * 0.7f),
                ImVec2(centre.x + r * 1.7f, centre.y + r * 1.7f), Theme::Chrome(0.6f), 1.8f * a_S);
        }

        /** La recherche et ses candidats. Gestion seulement : un locataire
         *  n'a personne à ajouter. */
        void Recherche(float a_S)
        {
            const float hautChamp = ImGui::GetFrameHeight();
            const ImVec2 caseLoupe = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(22.0f * a_S, hautChamp));
            Loupe(caseLoupe, a_S);
            ImGui::SameLine(0.0f, 4.0f * a_S);

            ImGui::SetNextItemWidth(-1.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Voile(0.10f));
            if (ImGui::InputTextWithHint("##vk_maison_rech", "ajouter quelqu'un — nom ou matricule",
                    g_recherche, sizeof(g_recherche))) {
                g_rechercheSale = true;
                g_rechercheStable = 0;
            }
            ImGui::PopStyleColor(3);
            FiletSousItem();

            /* Les candidats d'une AUTRE requête ne s'affichent pas : le joueur
               a retapé depuis, et un clic ajouterait la mauvaise personne. */
            // Le serveur rogne la requête avant d'en faire l'écho, le client la
            // compare rognée : ici aussi, sinon un blanc en tête cache la liste.
            const std::string courante = Rogner(Assainir(g_recherche));
            if (courante.empty() || courante != Rogner(g_rechercheEcho)) return;

            const float haut = 30.0f * a_S;
            if (g_resultats.empty()) {
                ImGui::TextDisabled("personne ne répond à ce nom");
                return;
            }
            ImGui::PushStyleColor(ImGuiCol_Header, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Voile(0.14f));
            for (const auto& cand : g_resultats) {
                ImGui::PushID(cand.personnageId);
                const ImVec2 depart = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##cand", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, haut))) {
                    EcrireGeste("ajouter", std::to_string(cand.personnageId));
                    // Le champ se vide : la personne est demandée, la liste
                    // n'a plus de raison d'attendre un second clic.
                    g_recherche[0] = '\0';
                    g_rechercheSale = false;
                    g_resultats.clear();
                    g_rechercheEcho.clear();
                    ImGui::PopID();
                    break;
                }
                const float largeur = ImGui::GetItemRectSize().x;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float milieu = depart.y + (haut - ImGui::GetTextLineHeight()) * 0.5f;
                const char* affiche = cand.nom.empty() ? cand.matricule.c_str() : cand.nom.c_str();
                dl->AddText(ImVec2(depart.x + 26.0f * a_S, milieu), Theme::Chrome(0.92f), affiche);
                const float l = ImGui::CalcTextSize(cand.matricule.c_str()).x;
                dl->AddText(ImVec2(depart.x + largeur - l - 8.0f * a_S, milieu), Theme::Chrome(0.50f),
                    cand.matricule.c_str());
                dl->AddLine(ImVec2(depart.x, depart.y + haut), ImVec2(depart.x + largeur, depart.y + haut),
                    OrSombre(0.20f), 1.0f);
                ImGui::PopID();
            }
            ImGui::PopStyleColor(3);
        }

        /** Les membres : Nom | ID | Rôle | ✕. Le propriétaire en or et sans
         *  croix (il ne se retire pas : on désigne d'abord un successeur —
         *  règle du serveur, l'écran ne fait que ne pas la proposer). */
        void TableMembres(float a_S)
        {
            const float haut = 34.0f * a_S;
            const float largeurId = ImGui::CalcTextSize("ZZZZZ").x + 16.0f * a_S;

            ImGui::PushStyleColor(ImGuiCol_TableBorderLight, OrSombre(0.35f));
            ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, OrSombre(0.35f));
            ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Voile(0.14f));
            ImGui::PushStyleColor(ImGuiCol_Button, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Voile(0.16f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f * a_S, 0.0f));

            const bool table = ImGui::BeginTable("##vk_maison_membres", 4,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_NoPadOuterX);
            if (table) {
                ImGui::TableSetupColumn("Nom", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, largeurId);
                ImGui::TableSetupColumn("Rôle", ImGuiTableColumnFlags_WidthFixed, 130.0f * a_S);
                ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, 30.0f * a_S);

                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TableHeadersRow();
                ImGui::PopStyleColor();

                for (const auto& m : g_membres) {
                    const bool proprietaire = (m.role == "proprietaire");
                    const bool moi = !g_moi.empty() && m.matricule == g_moi;
                    ImGui::PushID(m.personnageId);
                    ImGui::TableNextRow(0, haut);

                    // Nom — le voile de survol court sur toute la ligne, les
                    // boutons des colonnes suivantes passent par-dessus.
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 depart = ImGui::GetCursorScreenPos();
                    ImGui::Selectable("##ligne", false,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                        ImVec2(0.0f, haut));
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", m.enJeu ? "en jeu" : "absent");
                    }
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float milieu = depart.y + (haut - ImGui::GetTextLineHeight()) * 0.5f;
                    const bool connu = !m.nom.empty();
                    const char* affiche = connu ? m.nom.c_str() : m.matricule.c_str();
                    ImU32 couleur = proprietaire ? Theme::GoldCol() : Theme::Chrome(connu ? 0.92f : 0.55f);
                    if (!m.enJeu && !proprietaire) couleur = Theme::Chrome(0.45f);
                    dl->AddText(ImVec2(depart.x + 6.0f * a_S, milieu), couleur, affiche);
                    if (moi) {
                        const float l = ImGui::CalcTextSize(affiche).x;
                        dl->AddText(ImVec2(depart.x + 6.0f * a_S + l + 8.0f * a_S, milieu), Theme::Chrome(0.40f), "toi");
                    }

                    // ID
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (haut - ImGui::GetTextLineHeight()) * 0.5f);
                    ImGui::TextDisabled("%s", m.matricule.c_str());

                    // Rôle — cliquer bascule ; le serveur décide du sens.
                    ImGui::TableSetColumnIndex(2);
                    if (g_gerer) {
                        ImGui::PushStyleColor(ImGuiCol_Text, proprietaire ? Theme::GoldCol() : Theme::Chrome(0.85f));
                        if (Sfx::Button((m.libelle + "##role").c_str(), ImVec2(-FLT_MIN, haut))) {
                            EcrireGeste("role", std::to_string(m.personnageId));
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", proprietaire ? "déjà propriétaire — clique un locataire pour lui passer la main"
                                                                 : "en faire le propriétaire");
                        }
                    } else {
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (haut - ImGui::GetTextLineHeight()) * 0.5f);
                        ImGui::PushStyleColor(ImGuiCol_Text, proprietaire ? Theme::GoldCol() : Theme::Chrome(0.70f));
                        ImGui::TextUnformatted(m.libelle.c_str());
                        ImGui::PopStyleColor();
                    }

                    // ✕ — jamais sur le propriétaire, jamais en lecture seule.
                    ImGui::TableSetColumnIndex(3);
                    if (g_gerer && !proprietaire) {
                        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.70f));
                        // « × » (U+00D7) : la croix de la maquette, avec un glyphe
                        // que les polices cuites du greffon possèdent à coup sûr.
                        if (Sfx::Button("×##retirer", ImVec2(-FLT_MIN, haut), true)) {
                            EcrireGeste("retirer", std::to_string(m.personnageId));
                        }
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("retirer de la maison");
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(9);

            if (g_membres.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled(g_gerer ? "Personne encore. Le premier ajouté devient propriétaire."
                                            : "Personne encore.");
            }
        }
    }

    // ── l'interface publique ──────────────────────────────────────────────

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kCheminGestes, "w")) {
            std::fclose(f);
            g_pret = true;
            SKSE::log::info("[MAISON] pont pret ({})", kCheminGestes);
        } else {
            SKSE::log::warn("[MAISON] impossible d'ouvrir {} — le panneau restera sourd", kCheminGestes);
        }
        // UN ÉTAT RESCAPÉ D'UN PLANTAGE ferait surgir le panneau au lancement
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

        /* LA RECHERCHE PART QUAND LA FRAPPE SE POSE — et seulement si elle a
           de quoi chercher : sous deux caractères le serveur refuserait, et
           un champ vidé n'a rien à demander. */
        if (g_ouvert && g_gerer && g_rechercheSale && ++g_rechercheStable >= kTramesStables) {
            g_rechercheSale = false;
            const std::string q = Assainir(g_recherche);
            if (Caracteres(q.c_str()) >= 2) EcrireGeste("chercher", q);
        }

        /* Le chien de garde : la racine s'est refermée sans passer par notre
           couche Échap (Tab, un menu vanilla, l'établi qui a la priorité au
           rendu) — après deux secondes sans une trame dessinée, on se ferme
           et on le DIT, sinon le serveur nous croit toujours devant le panneau. */
        if (g_ouvert && g_tic - g_dernierDessin > kTramesSansDessin) {
            SKSE::log::info("[MAISON] panneau plus dessine depuis {} trames : fermeture", g_tic - g_dernierDessin);
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
        g_nomActif = false;
        g_rechercheSale = false;
        g_resultats.clear();
        g_rechercheEcho.clear();
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
           visible autour. La largeur est une fraction de l'écran, bornée en
           pixels pour rester lisible en 1280 et raisonnable en 4K. */
        const float largeur = std::clamp(io.DisplaySize.x * 0.32f, 380.0f, 640.0f);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.40f),
            ImGuiCond_Always, ImVec2(0.5f, 0.35f));
        ImGui::SetNextWindowSize(ImVec2(largeur, 0.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 9, 8, 236));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * S, 6.0f * S));
        ImGui::Begin("##vk_maison", nullptr,
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

        LigneNom(S);
        ImGui::Spacing();

        if (g_gerer) {
            Recherche(S);
            ImGui::Spacing();
        }

        TableMembres(S);

        if (!g_gerer) {
            ImGui::Spacing();
            ImGui::TextDisabled("Tu es locataire ici.");
        }

        /* LE MESSAGE : ce que le serveur vient de répondre, sept secondes. */
        if (!g_message.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::TextWrapped("%s", g_message.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.35f));
        ImGui::TextUnformatted("Échap : fermer");
        ImGui::PopStyleColor();

        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }
}
