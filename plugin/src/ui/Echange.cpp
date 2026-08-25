#include "Echange.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <imgui.h>

#include "game/MonnaiesVulkaar.h"
#include "ui/Fallback.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

// Voir Echange.h pour l'architecture. Ici : le parsing TSV de l'état, la
// fenêtre à deux panneaux (mon offre interactive, la sienne en lecture), les
// trois cases de monnaies par côté, ACCEPTER/REFUSER, et l'écriture des
// gestes. Le serveur reste seul juge — un désaccord local se résout toujours
// par « le prochain état écrase tout ».

namespace FUI::Echange
{
    namespace
    {
        constexpr const char* kCheminEtat = "Data/SKSE/Plugins/GridInventory_echange_etat.txt";
        constexpr const char* kCheminGestes = "Data/SKSE/Plugins/GridInventory_echange.txt";
        constexpr const char* kCheminInteraction = "Data/SKSE/Plugins/GridInventory_interaction.txt";

        struct Ligne
        {
            RE::FormID form = 0;
            int count = 0;
        };

        enum class Phase
        {
            kAucune,
            kInvitation,
            kOuverte,
            kConclue,
            kFermee,
        };

        // ---- état reçu (le serveur fait foi) ----
        Phase g_phase = Phase::kAucune;
        std::string g_nom;
        std::string g_motif;
        bool g_jaiValide = false;
        bool g_ilAValide = false;
        std::vector<Ligne> g_sienne;
        unsigned long long g_seqEtat = 0;

        // ---- offre locale (objets hors monnaies + les trois compteurs) ----
        std::vector<Ligne> g_offre;
        int g_monnaies[MonnaiesVulkaar::kNb] = {};
        bool g_offreSale = false;
        int g_offreStable = 0;

        // ---- plomberie ----
        unsigned long long g_seqGeste = 0;
        bool g_pret = false;
        int g_ticAttente = 0;
        int g_toastRestant = 0;

        // ---- menu d'interaction (la touche X, côté client) ----
        unsigned long long g_interSeq = 0;
        bool g_interOuvert = false;
        bool g_interACible = false;
        std::string g_interEtiquette;
        int g_interDernierDessin = 0;

        void EcrireGeste(const char* a_action, const std::string& a_reste)
        {
            if (!g_pret) return;
            std::FILE* f = std::fopen(kCheminGestes, "a");
            if (!f) return;
            if (a_reste.empty()) std::fprintf(f, "%llu\t%s\n", ++g_seqGeste, a_action);
            else std::fprintf(f, "%llu\t%s\t%s\n", ++g_seqGeste, a_action, a_reste.c_str());
            std::fclose(f);
        }

        /** L'offre complète — objets puis monnaies — au format `hex:count`. */
        std::string OffreEnTexte()
        {
            std::string sortie;
            char morceau[32];
            for (const auto& l : g_offre) {
                std::snprintf(morceau, sizeof(morceau), "%x:%d ", l.form, l.count);
                sortie += morceau;
            }
            for (int i = 0; i < MonnaiesVulkaar::kNb; ++i) {
                if (g_monnaies[i] <= 0) continue;
                auto* forme = MonnaiesVulkaar::Forme(i);
                if (!forme) continue;
                std::snprintf(morceau, sizeof(morceau), "%x:%d ", forme->GetFormID(), g_monnaies[i]);
                sortie += morceau;
            }
            if (!sortie.empty()) sortie.pop_back();
            return sortie;
        }

        void MarquerOffreSale()
        {
            g_offreSale = true;
            g_offreStable = 0;
        }

        void ViderOffre()
        {
            g_offre.clear();
            std::memset(g_monnaies, 0, sizeof(g_monnaies));
            g_offreSale = false;
            g_offreStable = 0;
        }

        std::vector<Ligne> LireLignes(const char* a_texte)
        {
            std::vector<Ligne> sortie;
            const char* p = a_texte;
            while (*p) {
                while (*p == ' ') ++p;
                unsigned form = 0;
                int count = 0, lus = 0;
                if (std::sscanf(p, "%x:%d%n", &form, &count, &lus) == 2 && form && count > 0) {
                    sortie.push_back({ static_cast<RE::FormID>(form), count });
                    p += lus;
                } else {
                    break;
                }
            }
            return sortie;
        }

        void LireEtat()
        {
            std::FILE* f = std::fopen(kCheminEtat, "r");
            if (!f) return;
            char ligne[4096];
            unsigned long long seq = 0;
            Phase phase = g_phase;
            std::string nom = g_nom, motif = g_motif;
            bool jai = g_jaiValide, il = g_ilAValide;
            std::vector<Ligne> sienne = g_sienne;
            while (std::fgets(ligne, sizeof(ligne), f)) {
                char* tab = std::strchr(ligne, '\t');
                if (!tab) continue;
                *tab = '\0';
                char* val = tab + 1;
                if (char* fin = std::strchr(val, '\n')) *fin = '\0';
                if (std::strcmp(ligne, "seq") == 0) seq = std::strtoull(val, nullptr, 10);
                else if (std::strcmp(ligne, "phase") == 0) {
                    if (std::strcmp(val, "invitation") == 0) phase = Phase::kInvitation;
                    else if (std::strcmp(val, "ouverte") == 0) phase = Phase::kOuverte;
                    else if (std::strcmp(val, "conclue") == 0) phase = Phase::kConclue;
                    else if (std::strcmp(val, "fermee") == 0) phase = Phase::kFermee;
                    else phase = Phase::kAucune;
                }
                else if (std::strcmp(ligne, "nom") == 0) nom = val;
                else if (std::strcmp(ligne, "motif") == 0) motif = val;
                else if (std::strcmp(ligne, "jaiValide") == 0) jai = (val[0] == '1');
                else if (std::strcmp(ligne, "ilAValide") == 0) il = (val[0] == '1');
                else if (std::strcmp(ligne, "sienne") == 0) sienne = LireLignes(val);
            }
            std::fclose(f);
            if (seq == 0 || seq == g_seqEtat) return;
            g_seqEtat = seq;

            const Phase avant = g_phase;
            g_phase = phase;
            g_nom = nom;
            g_motif = motif;
            g_jaiValide = jai;
            g_ilAValide = il;
            g_sienne = std::move(sienne);

            if (g_phase != avant) {
                if (g_phase == Phase::kOuverte || g_phase == Phase::kInvitation) {
                    // L'échange se vit l'inventaire ouvert — on l'ouvre pour lui.
                    ViderOffre();
                    UIRoot::Open();
                }
                if (g_phase == Phase::kConclue || g_phase == Phase::kFermee) {
                    ViderOffre();
                    g_toastRestant = 300;   // ~5 s à 60 fps
                }
            }
        }

        /** L'état du menu d'interaction, poussé par le client à chaque appui
         *  sur X : `seq TAB ouvert TAB aCible TAB étiquette`. Une seule ligne,
         *  réécrite entière. */
        void LireInteraction()
        {
            std::FILE* f = std::fopen(kCheminInteraction, "r");
            if (!f) return;
            char ligne[512];
            const bool lu = std::fgets(ligne, sizeof(ligne), f) != nullptr;
            std::fclose(f);
            if (!lu) return;

            unsigned long long seq = 0;
            int ouvert = 0, aCible = 0, pos = 0;
            if (std::sscanf(ligne, "%llu\t%d\t%d\t%n", &seq, &ouvert, &aCible, &pos) < 3) return;
            if (seq == 0 || seq == g_interSeq) return;
            g_interSeq = seq;

            std::string etiquette = ligne + pos;
            while (!etiquette.empty() &&
                   (etiquette.back() == '\n' || etiquette.back() == '\r')) {
                etiquette.pop_back();
            }

            g_interOuvert = (ouvert == 1);
            g_interACible = (aCible == 1);
            g_interEtiquette = std::move(etiquette);
            if (g_interOuvert) {
                // Le rendu ne tourne que la racine ouverte : on l'ouvre, et la
                // grâce du chien de garde repart d'ici (kShow prend des trames).
                g_interDernierDessin = g_ticAttente;
                UIRoot::Open();
            }
        }

        // ---- dessin ----

        void CaseObjet(ImDrawList* a_dl, const ImVec2& a_p0, RE::FormID a_form, int a_count,
                       bool a_survolable)
        {
            const float cote = Grid::CellPx();
            Grid::DrawCellLattice(a_dl, a_p0, 1, 1);
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(a_form);
            const ImVec2 p1(a_p0.x + cote, a_p0.y + cote);
            if (obj) {
                const float marge = 3.0f * Theme::Scale();
                if (const auto* icone = IconCache::GetSingleton()->Get(obj); icone && icone->srv) {
                    a_dl->AddImage(reinterpret_cast<ImTextureID>(icone->srv),
                        ImVec2(a_p0.x + marge, a_p0.y + marge), ImVec2(p1.x - marge, p1.y - marge));
                } else if (const auto* repli = Fallback::Get(obj); repli && repli->srv) {
                    a_dl->AddImage(reinterpret_cast<ImTextureID>(repli->srv),
                        ImVec2(a_p0.x + marge, a_p0.y + marge), ImVec2(p1.x - marge, p1.y - marge));
                }
                if (a_survolable && ImGui::IsMouseHoveringRect(a_p0, p1)) {
                    Grid::DrawItemTooltip(obj, a_count);
                }
            }
            if (a_count > 1) {
                char texte[16];
                std::snprintf(texte, sizeof(texte), "%d", a_count);
                Grid::DrawCountBadge(a_dl, a_p0, texte);
            }
        }

        /** Un panneau d'offre : lattice fixe, une case par ligne. Rend la case
         *  cliquée (indice dans a_lignes) ou -1. */
        int Panneau(const char* a_id, const std::vector<Ligne>& a_lignes, int a_cols, int a_rows,
                    bool a_interactif)
        {
            const float cote = Grid::CellPx();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 base = ImGui::GetCursorScreenPos();
            Grid::DrawCellLattice(dl, base, a_cols, a_rows);
            int clique = -1;
            for (int i = 0; i < static_cast<int>(a_lignes.size()) && i < a_cols * a_rows; ++i) {
                const ImVec2 p0(base.x + (i % a_cols) * cote, base.y + (i / a_cols) * cote);
                CaseObjet(dl, p0, a_lignes[i].form, a_lignes[i].count, true);
                if (a_interactif && ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + cote, p0.y + cote)) &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    clique = i;
                }
            }
            ImGui::Dummy(ImVec2(a_cols * cote, a_rows * cote));
            ImGui::PushID(a_id);
            ImGui::PopID();
            return clique;
        }

        /** Les trois cases de monnaies d'un côté. `a_valeurs` en lecture, ou
         *  éditable : clic +1 (maj +10), clic droit -1 (maj -10), borné. */
        void CasesMonnaies(const char* a_id, int* a_valeurs, bool a_editable)
        {
            if (!MonnaiesVulkaar::Pret()) return;
            const float cote = Grid::CellPx();
            const float ecart = 4.0f * Theme::Scale();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 base = ImGui::GetCursorScreenPos();
            for (int i = 0; i < MonnaiesVulkaar::kNb; ++i) {
                const ImVec2 p0(base.x + i * (cote + ecart), base.y);
                auto* forme = MonnaiesVulkaar::Forme(i);
                if (!forme) continue;
                CaseObjet(dl, p0, forme->GetFormID(), a_valeurs[i], false);
                if (!a_editable) continue;
                const ImVec2 p1(p0.x + cote, p0.y + cote);
                if (!ImGui::IsMouseHoveringRect(p0, p1)) continue;
                const int pas = ImGui::GetIO().KeyShift ? 10 : 1;
                const int possede = MonnaiesVulkaar::Compte(i);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    const int avant = a_valeurs[i];
                    a_valeurs[i] = (std::min)(a_valeurs[i] + pas, possede);
                    if (a_valeurs[i] != avant) MarquerOffreSale();
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    const int avant = a_valeurs[i];
                    a_valeurs[i] = (std::max)(a_valeurs[i] - pas, 0);
                    if (a_valeurs[i] != avant) MarquerOffreSale();
                }
            }
            ImGui::Dummy(ImVec2(MonnaiesVulkaar::kNb * (cote + ecart), cote));
        }

        /** LE MENU D'INTERACTION — ce que X ouvre (voulu par le propriétaire,
         *  25/08/2026) : se présenter (à la cible), se présenter à tous (8 m),
         *  échanger. Sans cible visée à l'appui, les entrées ciblées sont
         *  éteintes. Chaque choix envoie son geste et le menu se ferme. */
        void FenetreInteraction()
        {
            g_interDernierDessin = g_ticAttente;
            const float S = Theme::Scale();
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.38f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##vk_interaction", nullptr,
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

            if (g_interACible) {
                ImGui::Text("Face à %s",
                    g_interEtiquette.empty() ? "un inconnu" : g_interEtiquette.c_str());
            } else {
                ImGui::TextDisabled("Personne dans le regard");
            }
            ImGui::Spacing();

            const ImVec2 taille(220.0f * S, 0.0f);
            ImGui::BeginDisabled(!g_interACible);
            if (ImGui::Button("Se présenter", taille)) {
                EcrireGeste("presenter", "");
                g_interOuvert = false;
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Se présenter à tous", taille)) {
                EcrireGeste("presenterTous", "");
                g_interOuvert = false;
            }
            ImGui::BeginDisabled(!g_interACible);
            if (ImGui::Button("Échanger", taille)) {
                EcrireGeste("echanger", "");
                g_interOuvert = false;
            }
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (ImGui::Button("Fermer", taille)) {
                EcrireGeste("fermerMenu", "");
                g_interOuvert = false;
            }
            ImGui::End();
        }

        void FenetreInvitation()
        {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.32f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##vk_echange_invite", nullptr,
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::Text("%s souhaite échanger avec toi.", g_nom.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Accepter")) {
                EcrireGeste("repondre", "1");
                g_phase = Phase::kAucune;   // le serveur confirmera par « ouvert »
            }
            ImGui::SameLine();
            if (ImGui::Button("Refuser")) {
                EcrireGeste("repondre", "0");
                g_phase = Phase::kAucune;
            }
            ImGui::End();
        }

        void FenetreEchange()
        {
            const float S = Theme::Scale();
            const float cote = Grid::CellPx();
            constexpr int kCols = 6;
            constexpr int kRows = 6;
            const float panneau = kCols * cote;
            const float largeur = panneau * 2 + 48.0f * S;
            const float hauteur = kRows * cote + cote + 130.0f * S;

            auto* wm = WinManager::GetSingleton();
            const ImGuiIO& io = ImGui::GetIO();
            wm->ApplyNext("echange",
                ImVec2(io.DisplaySize.x * 0.5f - largeur * 0.5f, io.DisplaySize.y * 0.12f),
                ImVec2(largeur, hauteur), WinManager::Anchor::kTopLeft, 0.0f);
            ImGui::Begin("##vk_echange", nullptr, kManagedWinFlags);
            wm->TitleBar("echange", ("Échange — " + g_nom).c_str());

            ImGui::Columns(2, "##vk_ech_cols", false);
            ImGui::TextUnformatted("Mon offre");
            const int retire = Panneau("mien", g_offre, kCols, kRows, true);
            if (retire >= 0) {
                auto& l = g_offre[static_cast<size_t>(retire)];
                const int pas = ImGui::GetIO().KeyShift ? l.count : 1;
                l.count -= pas;
                if (l.count <= 0) g_offre.erase(g_offre.begin() + retire);
                MarquerOffreSale();
            }
            CasesMonnaies("mien", g_monnaies, true);

            ImGui::NextColumn();
            ImGui::Text("%s%s", g_nom.c_str(), g_ilAValide ? "  — A ACCEPTÉ" : "");
            // Sa bourse arrive mêlée à son offre : on la sépare pour la bande.
            int sesMonnaies[MonnaiesVulkaar::kNb] = {};
            std::vector<Ligne> sesObjets;
            for (const auto& l : g_sienne) {
                bool monnaie = false;
                for (int i = 0; i < MonnaiesVulkaar::kNb; ++i) {
                    auto* forme = MonnaiesVulkaar::Forme(i);
                    if (forme && forme->GetFormID() == l.form) {
                        sesMonnaies[i] += l.count;
                        monnaie = true;
                        break;
                    }
                }
                if (!monnaie) sesObjets.push_back(l);
            }
            Panneau("sien", sesObjets, kCols, kRows, false);
            CasesMonnaies("sien", sesMonnaies, false);
            ImGui::Columns(1);

            ImGui::Spacing();
            // Le dépôt du porté : relâché n'importe où sur la fenêtre, l'objet
            // rejoint l'offre — l'inventaire, lui, ne bouge pas d'un octet.
            if (Grid::IsHolding() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                ImGui::IsMouseHoveringRect(ImGui::GetWindowPos(),
                    ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(),
                        ImGui::GetWindowPos().y + ImGui::GetWindowHeight()))) {
                RE::FormID form = 0;
                int count = 0;
                if (Grid::PorteVersEchange(form, count)) {
                    bool fusionne = false;
                    for (auto& l : g_offre) {
                        if (l.form == form) {
                            l.count += count;
                            fusionne = true;
                            break;
                        }
                    }
                    if (!fusionne) g_offre.push_back({ form, count });
                    MarquerOffreSale();
                }
            }

            if (g_jaiValide) {
                ImGui::TextUnformatted("En attente de l'autre…");
            } else if (ImGui::Button("ACCEPTER", ImVec2(140.0f * S, 0.0f))) {
                EcrireGeste("valider", "");
            }
            ImGui::SameLine();
            if (ImGui::Button("REFUSER", ImVec2(140.0f * S, 0.0f))) {
                EcrireGeste("annuler", "");
            }
            ImGui::End();
        }

        void Toast()
        {
            if (g_toastRestant <= 0) return;
            --g_toastRestant;
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.2f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::Begin("##vk_echange_toast", nullptr,
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoInputs);
            if (g_phase == Phase::kConclue) ImGui::TextUnformatted("Échange conclu.");
            else ImGui::Text("Échange fermé%s%s.", g_motif.empty() ? "" : " : ", g_motif.c_str());
            ImGui::End();
            if (g_toastRestant == 0) g_phase = Phase::kAucune;
        }
    }

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kCheminGestes, "w")) {
            std::fclose(f);
            g_pret = true;
            SKSE::log::info("[ECHANGE] pont pret ({})", kCheminGestes);
        } else {
            SKSE::log::warn("[ECHANGE] impossible d'ouvrir {} — l'echange restera sourd", kCheminGestes);
        }
        // Un état d'interaction rescapé d'un crash ferait surgir le menu au
        // boot : on repart d'une page blanche, le client réécrira au premier X.
        if (std::FILE* f = std::fopen(kCheminInteraction, "w")) {
            std::fclose(f);
        }
    }

    void Tick()
    {
        if (!g_pret) return;
        if (++g_ticAttente % 15 == 0) {
            LireEtat();
            LireInteraction();
        }
        if (g_offreSale && ++g_offreStable >= 20) {
            g_offreSale = false;
            EcrireGeste("offrir", OffreEnTexte());
        }
        // Chien de garde : la racine s'est refermée (Échap, Tab) alors que le
        // client croit le menu ouvert — on le lui dit, sinon le prochain X ne
        // ferait que « fermer » un menu déjà invisible.
        if (g_interOuvert && g_ticAttente - g_interDernierDessin > 120) {
            g_interOuvert = false;
            EcrireGeste("fermerMenu", "");
        }
    }

    void DrawFenetres()
    {
        // Le menu d'interaction s'efface devant la session d'échange — les
        // deux ne se chevauchent qu'un battement, le temps de l'aller-retour.
        if (g_interOuvert && g_phase != Phase::kInvitation && g_phase != Phase::kOuverte) {
            FenetreInteraction();
        }
        switch (g_phase) {
        case Phase::kInvitation:
            FenetreInvitation();
            break;
        case Phase::kOuverte:
            FenetreEchange();
            break;
        case Phase::kConclue:
        case Phase::kFermee:
            Toast();
            break;
        default:
            break;
        }
    }

    bool Actif()
    {
        return g_phase == Phase::kInvitation || g_phase == Phase::kOuverte;
    }
}
