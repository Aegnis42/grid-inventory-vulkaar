#include "ui/Notes.h"
#include "ui/Appartenance.h"
#include "ui/Banque.h"
#include "ui/Etabli.h"
#include "ui/Missives.h"

#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// Voir Notes.h pour l'architecture — et pour les trois règles qui font qu'une
// page ne se perd pas. Ici : le parsing TSV de l'état, les deux colonnes, et
// l'écriture des gestes.

namespace FUI::Notes
{
    namespace
    {
        constexpr const char* kCheminEtat = "Data/SKSE/Plugins/GridInventory_notes_etat.txt";
        constexpr const char* kCheminGestes = "Data/SKSE/Plugins/GridInventory_notes.txt";

        /* LES BORNES DU SERVEUR (contrat des notes : 60 caractères de titre,
           2 000 de texte), en CARACTÈRES et non en octets. Contrairement au
           courrier, elles ne se contentent pas de rougir un compteur : elles
           ARRÊTENT LA FRAPPE (voir BornerCaracteres). Un champ qui dépasserait
           ferait un enregistrement refusé, et un enregistrement refusé au moment
           où l'on referme le panneau, c'est une page perdue. */
        constexpr int kTitreMax = 60;
        constexpr int kTexteMax = 2000;
        /* Les tampons, en OCTETS : chaque caractère peut en peser quatre en
           UTF-8, plus le zéro final. Larges exprès — ImGui borne la saisie aux
           octets, et un joueur qui écrit avec des accents n'a pas à taper moins
           que les autres ; c'est BornerCaracteres qui compte les caractères. */
        constexpr int kOctetsTitre = kTitreMax * 4 + 1;
        constexpr int kOctetsTexte = kTexteMax * 4 + 1;

        /* Les deux bornes, en variables : le rappel de saisie d'ImGui ne reçoit
           qu'un `void*`, et lui passer l'adresse d'une constante demanderait un
           const_cast à chaque appel — une verrue pour rien. */
        int g_borneTitre = kTitreMax;
        int g_borneTexte = kTexteMax;

        struct Page
        {
            int         id = 0;
            /* Vide = on n'a pas encore su comment l'appeler : la liste affiche
               alors « Page n », son RANG, jamais son id — un id de registre ne
               veut rien dire pour un joueur, et deux carnets n'ont pas les
               mêmes. */
            std::string titre;
            std::string modifieeLe;   // l'ISO du serveur, rendu en heure locale au dessin
            /* LE DONNEUR, quand la page a été reçue. Vide = page écrite de ma
               main : un matricule ne peut pas être vide pour un personnage qui
               existe, donc cette colonne SUFFIT à dire la provenance. */
            std::string matricule;
            std::string nom;          // vide = on ne s'est pas présentés → le matricule
        };

        struct Voisin
        {
            int         personnageId = 0;
            std::string matricule;
            std::string nom;
        };

        // ---- état reçu (le serveur fait foi) ----
        bool                 g_ouvert = false;
        std::string          g_message;
        std::vector<Page>    g_pages;
        std::vector<Voisin>  g_autour;
        /* LA PAGE OUVERTE SELON LE SERVEUR, et son texte. Les deux descendent
           ENSEMBLE, de la même lecture du registre : le plateau ne porte jamais
           le texte d'une page fermée (les pages sont illimitées, il grossirait
           sans borne). `g_ouverteServeur` à 0 veut dire « aucune », et l'écran
           n'a alors rien à charger — il ne devine pas un texte vide. */
        int                  g_ouverteServeur = 0;
        std::string          g_texteServeur;
        unsigned long long   g_seqEtat = 0;

        // ---- état de l'écran (local, jamais envoyé tel quel) ----
        /* LA PAGE RÉCLAMÉE : on a cliqué, le geste `page` est parti, le texte
           n'est pas là. Tant qu'elle vaut autre chose que 0, la colonne de
           droite dit qu'elle ouvre — elle ne montre NI un texte vide, NI celui
           de la page d'avant sous le titre d'une autre. */
        int         g_demandee = 0;
        /* LA TRAME OÙ ON L'A RÉCLAMÉE — l'échéance de l'attente. Sans elle,
           `g_demandee` n'avait AUCUNE sortie quand le serveur refusait le geste
           `page` : il n'est effacé que si le serveur ouvre CETTE page ou si
           elle quitte le carnet, et un refus ne fait ni l'un ni l'autre. La
           colonne de droite restait alors sur « Ouverture de la page… » pour
           toujours, la page cliquée en or à gauche, sans que rien ne démente.
           Correction de relecture croisée, 07/09. */
        int         g_demandeeDepuis = 0;
        /** LA PAGE CHARGÉE : celle dont le titre et le texte sont dans les
         *  tampons ci-dessous. 0 = les tampons ne valent rien. */
        int         g_chargee = 0;
        char        g_titre[kOctetsTitre] = {};
        char        g_texte[kOctetsTexte] = {};
        /* CE QUE LE SERVEUR NOUS A DONNÉ, ou ce qu'il a CONFIRMÉ avoir reçu.
           « Modifiée » se juge là-dessus, et ces deux-là ne bougent QUE sur une
           confirmation — jamais au clic sur « Enregistrer ». Un enregistrement
           refusé (le registre qui ne répond pas) laisse donc le bouton actif et
           le texte à l'écran : c'est le joueur qui décide de réessayer, et rien
           ne lui a été pris. */
        std::string g_titreOrigine;
        std::string g_texteOrigine;
        /* LA DATE DE LA VERSION CHARGÉE. C'est elle qui reconnaît la
           confirmation : le serveur regrave `modifieeLe` à chaque écriture, donc
           une date qui a bougé sur NOTRE page veut dire « c'est enregistré ». */
        std::string g_dateChargee;
        bool        g_enregistrementEnvoye = false;
        std::string g_titreEnvoye;
        std::string g_texteEnvoye;
        /** La trame du dernier envoi — voir `kTramesEnvoiRepete`. */
        int         g_envoyeA = 0;
        /* CE QU'ON A ENVOYÉ, ET NON CE QUI EST À L'ÉCRAN : le joueur a pu taper
           trois mots pendant l'aller-retour, et ceux-là ne sont pas enregistrés.
           Recopier les tampons courants à la confirmation les déclarerait sauvés
           et les perdrait au prochain plateau. */

        bool        g_partage = false;    // la liste « à qui tends-tu cette page ? »
        bool        g_confirme = false;   // la confirmation de suppression
        int         g_messageRestant = 0; // trames avant effacement du message

        // ---- plomberie ----
        unsigned long long g_seqGeste = 0;
        /* CE QUE LE CLIENT DIT AVOIR LU, et les octets écrits depuis le dernier
           rognage : voir `RognerLesGestes`. Le fichier des gestes est relu
           ENTIER quatre fois par seconde par le service client, et une ligne
           « ecrire » y pèse jusqu'à huit kilo-octets. MESURÉ le 07/09 :
           1,5 Mio coûtent 3,2 ms par passe — la moitié d'une trame à 144 images
           par seconde, quatre fois par seconde. */
        unsigned long long g_luParLeClient = 0;
        unsigned long long g_octetsGestes = 0;
        bool               g_pret = false;
        int                g_tic = 0;
        int                g_dernierDessin = 0;

        /* Le chien de garde : la racine s'est refermée sans passer par nous
           (Tab, un menu vanilla) — sans cela le serveur garderait le joueur
           parmi les panneaux ouverts et lui repousserait des plateaux. */
        constexpr int kTramesSansDessin = 120;
        constexpr int kTramesMessage = 420;   // ~7 s à 60 fps
        /* AU-DELÀ DE CE POIDS, le fichier des gestes se rogne dès que le client
           a tout lu (`RognerLesGestes`). Soixante-quatre kilo-octets : en
           dessous, la passe du client coûte moins d'un dixième de milliseconde,
           et remuer le fichier coûterait plus cher que le laisser grossir. */
        constexpr unsigned long long kOctetsGestesMax = 64ull * 1024ull;
        /* COMBIEN DE TRAMES ON ATTEND LE TEXTE D'UNE PAGE avant de renoncer et
           de le DIRE. Deux secondes à soixante images par seconde, le même
           renoncement que le chien de garde : un aller au serveur suivi d'un
           aller au registre tient très en dessous, et au-delà c'est que la
           réponse ne viendra pas (refus avalé, registre muet). Renoncer tout
           haut vaut mieux qu'attendre en silence une réponse qui n'arrive
           plus. */
        constexpr int kTramesAttentePage = 120;
        /* COMBIEN DE TRAMES ON S'INTERDIT DE RENVOYER LE MÊME TEXTE. Voir
           `EnregistrerSiModifiee` : le relais borne les écritures à une par
           250 ms et par joueur, et un doublon parti dans cette fenêtre s'y fait
           écarter pour rien. Soixante trames couvrent la fenêtre même à 144
           images par seconde (417 ms) — et au-delà, le renvoi REDEVIENT permis :
           c'est le seul rattrapage d'un enregistrement que le registre aurait
           refusé, et il ne faut surtout pas le supprimer. */
        constexpr int kTramesEnvoiRepete = 60;

        // ── l'échappement des textes ──────────────────────────────────────
        //
        // UNE PAGE A DES PARAGRAPHES, et le pont est du TSV nu. Un saut de ligne
        // couperait la ligne en deux (la seconde moitié serait lue comme une
        // ligne au premier mot inconnu, donc jetée en silence) ; une tabulation
        // décalerait toutes les colonnes suivantes.
        //
        // LA RÈGLE, ET LES TROIS CÔTÉS LA FONT PAREIL (contrat des notes, §6) :
        //   écrire   `\r\n` et `\r` seul → un saut de ligne ; tabulation → UN
        //            espace ; `\` → `\\` ; saut de ligne → `\n` (deux
        //            caractères). RIEN D'AUTRE — ni les guillemets, ni les
        //            virgules, ni les accents.
        //   lire     `\\` → `\` ; `\n` → saut de ligne ; un `\` suivi de
        //            n'importe quoi d'autre — ou en fin de texte — est un `\`
        //            littéral, et le caractère qui suit est lu normalement.
        //
        // LES MIROIRS sont `pontTexte.ts` (vulkaar-engine, où la paire s'éprouve
        // dans les deux sens) et `Missives.cpp` : c'est LE MÊME module TS pour
        // les deux écrans, et deux copies C++ de la même règle. Une ligne
        // corrigée ici se corrige aux deux autres endroits, sans quoi le pont
        // ment dans un sens — et il ne mentirait que pour un écran sur deux, ce
        // qui est la façon la plus lente de s'en apercevoir.
        //
        // OCTET PAR OCTET, et c'est sûr en UTF-8 : les octets de continuation
        // valent tous 0x80 ou plus, donc aucun ne peut être pris pour une barre
        // oblique ni pour un « n ».

        std::string Echapper(const std::string& a_texte)
        {
            std::string r;
            r.reserve(a_texte.size() + 16);
            for (std::size_t i = 0; i < a_texte.size(); ++i) {
                const char c = a_texte[i];
                if (c == '\\') {
                    r += "\\\\";
                } else if (c == '\r') {
                    r += "\\n";
                    // `\r\n` est UN saut de ligne, pas deux.
                    if (i + 1 < a_texte.size() && a_texte[i + 1] == '\n') ++i;
                } else if (c == '\n') {
                    r += "\\n";
                } else if (c == '\t') {
                    r += ' ';
                } else {
                    r += c;
                }
            }
            return r;
        }

        std::string Desechapper(const std::string& a_texte)
        {
            std::string r;
            r.reserve(a_texte.size());
            for (std::size_t i = 0; i < a_texte.size(); ++i) {
                const char c = a_texte[i];
                if (c != '\\') {
                    r += c;
                    continue;
                }
                if (i + 1 >= a_texte.size()) {
                    r += '\\';   // une barre en fin de texte est une barre
                    break;
                }
                const char suivant = a_texte[++i];
                if (suivant == '\\') r += '\\';
                else if (suivant == 'n') r += '\n';
                else {
                    // Ni l'un ni l'autre : les deux caractères restent. Rien ne
                    // se perd, rien n'explose — la DLL n'écrit jamais cela, mais
                    // un fichier recousu le peut.
                    r += '\\';
                    r += suivant;
                }
            }
            return r;
        }

        /** Une tabulation ou un retour dans un TITRE décalerait tous les champs
         *  du lecteur TS, qui est un split('\t') sans état. Le titre est UNE
         *  ligne (le registre le normalise ainsi) ; ceci est la seconde serrure.
         *  Copie de celle du panneau d'appartenance (espace anonyme, donc
         *  inaccessible). */
        std::string Assainir(const char* a_s)
        {
            std::string r = a_s == nullptr ? "" : a_s;
            for (char& c : r) {
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            }
            return r;
        }

        /** Combien de CARACTÈRES (pas d'octets) : « é » en fait un seul, et les
         *  bornes doivent valoir pareil pour qui écrit avec des accents. Copie
         *  de celle du panneau d'appartenance (espace anonyme). */
        int Caracteres(const char* a_s)
        {
            int n = 0;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(a_s); *p; ++p) {
                if ((*p & 0xC0) != 0x80) ++n;
            }
            return n;
        }

        /**
         * LA BORNE À LA FRAPPE — et c'est une décision, pas un détail.
         *
         * Le courrier laisse dépasser et grise « Envoyer » : une lettre trop
         * longue ne part pas, on la raccourcit, rien n'est perdu. Une PAGE, si :
         * elle s'enregistre AUSSI quand on referme le panneau (contrat §1.8), et
         * un enregistrement que le serveur refuserait pour la longueur, c'est
         * exactement la page que le joueur vient d'écrire qui s'en va.
         *
         * On coupe donc à la SAISIE, où cela se voit : le compteur se fige à
         * 2 000 / 2 000, la lettre suivante n'entre pas. ImGui borne déjà le
         * tampon en OCTETS ; ce rappel-ci compte les CARACTÈRES, sans quoi un
         * texte accentué serait coupé bien avant la borne annoncée.
         *
         * `DeleteChars` est la seule façon correcte de rogner : elle recale le
         * curseur et la sélection d'ImGui, ce qu'un `Buf[i] = 0` ne fait pas.
         */
        int BornerCaracteres(ImGuiInputTextCallbackData* a_donnees)
        {
            const int max = *static_cast<const int*>(a_donnees->UserData);
            int n = 0;
            int coupe = -1;
            for (int i = 0; i < a_donnees->BufTextLen; ++i) {
                if ((static_cast<unsigned char>(a_donnees->Buf[i]) & 0xC0) != 0x80) {
                    ++n;
                    if (n == max + 1) {
                        coupe = i;
                        break;
                    }
                }
            }
            if (coupe >= 0) a_donnees->DeleteChars(coupe, a_donnees->BufTextLen - coupe);
            return 0;
        }

        /** `<seq>\t<action>[\t<reste>]`. Le pont des notes n'a qu'un sujet : pas
         *  de colonne de sujet, contrairement à l'appartenance. */
        void EcrireGeste(const char* a_action, const std::string& a_reste)
        {
            if (!g_pret) return;
            std::FILE* f = std::fopen(kCheminGestes, "a");
            if (!f) return;
            const int ecrits = a_reste.empty()
                ? std::fprintf(f, "%llu\t%s\n", ++g_seqGeste, a_action)
                : std::fprintf(f, "%llu\t%s\t%s\n", ++g_seqGeste, a_action, a_reste.c_str());
            std::fclose(f);
            /* CE QUE PÈSE LE FICHIER, compté au vol : `RognerLesGestes` en a
               besoin, et un `stat` par geste serait un aller au disque pour
               apprendre ce qu'on vient d'écrire. */
            if (ecrits > 0) g_octetsGestes += static_cast<unsigned long long>(ecrits);
            /* LA TRACE DU CLIC, comme chez les missives (07/09/2026, « en
               cliquant sur envoyer rien ne se faisait ») : pour savoir si un
               geste a seulement quitté l'écran, il fallait aller lire le FICHIER
               du pont, qui ne garde que ce qui est parti. Une ligne par geste
               répond du premier coup. LE TEXTE D'UNE PAGE N'Y FIGURE JAMAIS : on
               note le geste, pas ce que le joueur écrit. */
            SKSE::log::info("[NOTES] geste ecrit : {} (seq {})", a_action, g_seqGeste);
        }

        /**
         * LE FICHIER DES GESTES SE ROGNE QUAND LE CLIENT A TOUT LU.
         *
         * POURQUOI IL LE FAUT ICI ET NULLE PART AILLEURS. Les quatre autres
         * écrans écrivent des lignes courtes et rares — une missive se poste
         * deux fois par soirée. Le carnet, lui, envoie le TEXTE ENTIER d'une
         * page à chaque changement de page, à chaque « Nouvelle page », à
         * chaque partage et à chaque fermeture : jusqu'à huit kilo-octets par
         * ligne. Or le service client relit le fichier ENTIER quatre fois par
         * seconde. MESURÉ le 07/09 (Ryzen 7 2700X, `readFileSync` + `split`) :
         *   282 Kio → 1,1 ms par passe ; 1,5 Mio → 3,2 ms ; 7,8 Mio → 14 ms.
         * Trois millisecondes, c'est la moitié d'une trame à 144 images par
         * seconde, quatre fois par seconde — une séance d'écriture d'une heure
         * y arrive sans effort.
         *
         * ET C'EST SÛR, PARCE QUE LE CLIENT NOUS DIT CE QU'IL A LU. La ligne
         * `lu <seq>` de l'état porte sa dernière séquence consommée ; on ne
         * rogne que si elle a rattrapé la nôtre. Rogner à l'aveugle (« au bout
         * de deux secondes », « quand le panneau se ferme ») perdrait le geste
         * écrit entre la dernière lecture et le rognage — c'est-à-dire, un jour
         * sur mille, la page qu'un joueur vient d'écrire. On ne parie pas
         * là-dessus.
         *
         * `g_seqGeste` NE REPART PAS À ZÉRO : le client écarte les séquences
         * déjà vues, et remettre le compteur à zéro lui ferait jeter les gestes
         * suivants. Seul le FICHIER se vide.
         */
        void RognerLesGestes()
        {
            if (!g_pret) return;
            if (g_octetsGestes < kOctetsGestesMax) return;
            if (g_luParLeClient < g_seqGeste) return;   // il lui en reste
            std::FILE* f = std::fopen(kCheminGestes, "w");
            if (!f) return;
            std::fclose(f);
            SKSE::log::info("[NOTES] fichier des gestes rogne ({} octets, tout lu jusqu a la seq {})",
                g_octetsGestes, g_luParLeClient);
            g_octetsGestes = 0;
        }

        /** L'or du thème, assombri et rendu presque transparent : le filet d'un
         *  pixel que le propriétaire préfère à toute bordure pleine. */
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

        /** Le rouge sombre d'un refus local : un manque à corriger, pas une
         *  alerte. */
        ImU32 RougeSombre()
        {
            return IM_COL32(178, 66, 54, 255);
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

        /** Un entier positif de l'état : `atoi` rendrait 0 sur un texte, et un
         *  id nul écarte la ligne — c'est ce qu'on veut. */
        int Identifiant(const char* a_s)
        {
            const long v = std::strtol(a_s, nullptr, 10);
            if (v <= 0 || v > 2000000000L) return 0;
            return static_cast<int>(v);
        }

        const Page* PageParId(int a_id)
        {
            for (const auto& p : g_pages) {
                if (p.id == a_id) return &p;
            }
            return nullptr;
        }

        bool Modifiee()
        {
            return g_chargee != 0 && (g_titreOrigine != g_titre || g_texteOrigine != g_texte);
        }

        /**
         * L'ENREGISTREMENT, ET IL PART DE PARTOUT OÙ L'ON QUITTE UNE PAGE.
         *
         * Le bouton « Enregistrer » n'est pas le seul chemin : changer de page,
         * créer une page, ouvrir la liste du partage et refermer le panneau
         * passent tous par ici d'abord. « Perdre ce qu'un joueur vient d'écrire
         * est la seule faute que ce chantier ne doit pas commettre » — et un
         * joueur qui tape Échap ne pense pas avoir donné un ordre destructeur.
         *
         * LE PARTAGE EN A BESOIN AUTANT QUE LE RESTE : le serveur donne la page
         * TELLE QU'ELLE EST AU REGISTRE, pas telle qu'elle est à l'écran. Tendre
         * une page qu'on vient d'écrire sans l'enregistrer, ce serait tendre la
         * version d'avant, et personne ne le verrait.
         */
        void EnregistrerSiModifiee()
        {
            if (!Modifiee()) return;
            /* LE MÊME TEXTE NE PART PAS DEUX FOIS DANS LA MÊME SECONDE.
               `Modifiee()` reste vrai jusqu'à la CONFIRMATION du serveur (les
               origines ne bougent qu'à la date regravée), si bien qu'un joueur
               qui clique « Enregistrer » puis tape Échap dans la foulée
               renvoyait mot pour mot ce qui était déjà en route. Le relais
               borne les écritures à une par 250 ms : ce doublon-là se faisait
               écarter pour rien, et il gonflait le fichier des gestes du texte
               entier d'une page à chaque fois.
               PASSÉ LE DÉLAI, LE RENVOI REDEVIENT PERMIS, et c'est vital : si le
               registre a refusé l'écriture, ce renvoi est la seule chance qui
               reste à la page. On ne supprime pas un rattrapage, on l'espace. */
            if (g_enregistrementEnvoye && g_titreEnvoye == g_titre && g_texteEnvoye == g_texte &&
                g_tic - g_envoyeA <= kTramesEnvoiRepete) {
                return;
            }
            const std::string titre = Assainir(g_titre);
            EcrireGeste("ecrire", std::to_string(g_chargee) + "\t" + titre + "\t" + Echapper(g_texte));
            g_titreEnvoye = g_titre;
            g_texteEnvoye = g_texte;
            g_envoyeA = g_tic;
            g_enregistrementEnvoye = true;
        }

        /** Les tampons ne valent plus rien : plus aucune page n'est chargée.
         *  N'ENREGISTRE RIEN — les appelants décident, et l'un d'eux (la
         *  suppression) ne doit surtout pas réécrire ce qu'il vient d'effacer. */
        void Vider()
        {
            g_chargee = 0;
            g_titre[0] = '\0';
            g_texte[0] = '\0';
            g_titreOrigine.clear();
            g_texteOrigine.clear();
            g_dateChargee.clear();
            g_enregistrementEnvoye = false;
            g_partage = false;
            g_confirme = false;
        }

        /** Charge dans les tampons la page que le SERVEUR dit ouverte. C'est le
         *  SEUL endroit où la saisie en cours est remplacée : une poussée qui
         *  laisse la même page ouverte ne touche à rien (voir Notes.h). */
        void ChargerPageOuverte()
        {
            const Page* p = PageParId(g_ouverteServeur);
            if (p == nullptr) {
                /* Le plateau se contredit : il ouvre une page qui n'est pas dans
                   sa propre liste. On ne charge rien plutôt que d'inventer un
                   titre. */
                SKSE::log::warn("[NOTES] page {} dite ouverte mais absente de la liste : rien charge", g_ouverteServeur);
                Vider();
                return;
            }
            /* UNE TRONCATURE NE DOIT JAMAIS DEVENIR UN ENREGISTREMENT. Le
               serveur borne à 60 et 2 000 caractères, donc à 240 et 8 000 octets
               au pire ; les tampons tiennent cela. S'il descendait davantage,
               `snprintf` couperait en silence — et le prochain enregistrement
               renverrait la version coupée par-dessus la vraie. On refuse. */
            if (p->titre.size() >= sizeof(g_titre) || g_texteServeur.size() >= sizeof(g_texte)) {
                SKSE::log::warn("[NOTES] page {} trop grande pour les tampons ({} + {} octets) : rien charge",
                    p->id, p->titre.size(), g_texteServeur.size());
                Vider();
                return;
            }
            std::snprintf(g_titre, sizeof(g_titre), "%s", p->titre.c_str());
            std::snprintf(g_texte, sizeof(g_texte), "%s", g_texteServeur.c_str());
            g_titreOrigine = g_titre;
            g_texteOrigine = g_texte;
            g_dateChargee = p->modifieeLe;
            g_chargee = g_ouverteServeur;
            g_enregistrementEnvoye = false;
            g_partage = false;
            g_confirme = false;
        }

        void LireEtat()
        {
            std::FILE* f = std::fopen(kCheminEtat, "r");
            if (!f) return;

            /* LE NUMÉRO DE SÉQUENCE SE LIT EN PREMIER, ET ON SORT AUSSITÔT.
               Ce Tick est appelé à CHAQUE trame ; le fichier est réécrit ENTIER
               à chaque poussée serveur et sa première ligne porte « seq <n> ».
               Ici l'habitude compte double : un carnet n'a AUCUN plafond de
               pages, et la ligne du texte pèse à elle seule jusqu'à seize
               kilo-octets. */
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
            /* CE QUE LE CLIENT DIT AVOIR LU de nos gestes — voir
               `RognerLesGestes`. Il descend dans l'état parce que c'est le seul
               canal client → DLL qui existe, et il ne coûte qu'une ligne. */
            unsigned long long lu = 0;
            bool ouvert = false;
            bool finVue = false;
            std::string message;
            std::vector<Page> pages;
            std::vector<Voisin> autour;
            int ouverte = 0;
            int texteId = 0;
            std::string texteLu;

            /* LE TAMPON D'UNE LIGNE, ET LE COMPTE EST FAIT : un texte pèse au
               plus 2 000 caractères, dont chacun peut valoir quatre octets en
               UTF-8 — 8 000 octets. L'échappement peut au pire DOUBLER ce
               nombre (chaque octet devenant `\\` ou `\n`), mais seuls des octets
               d'un seul octet peuvent l'être : les deux pires cas ne se cumulent
               pas, et 16 384 les couvre tous deux. Le reste est de la marge.
               Alloué APRÈS la sortie rapide : une trame qui ne lit rien ne paie
               rien. */
            std::vector<char> ligne(24576);
            char* c[12];
            while (std::fgets(ligne.data(), static_cast<int>(ligne.size()), f)) {
                const int n = Champs(ligne.data(), c, 12);
                if (n < 1 || c[0][0] == '\0') continue;
                /* La sentinelle est toujours la DERNIÈRE ligne : une ligne qui
                   la suivrait trahirait un fichier recousu, on l'ignore. */
                finVue = std::strcmp(c[0], "fin") == 0;
                if (finVue) continue;
                if (n < 2) continue;
                if (std::strcmp(c[0], "seq") == 0) {
                    seq = std::strtoull(c[1], nullptr, 10);
                } else if (std::strcmp(c[0], "lu") == 0) {
                    lu = std::strtoull(c[1], nullptr, 10);
                } else if (std::strcmp(c[0], "phase") == 0) {
                    ouvert = std::strcmp(c[1], "ouverte") == 0;
                } else if (std::strcmp(c[0], "message") == 0) {
                    message = c[1];
                } else if (std::strcmp(c[0], "ouverte") == 0) {
                    ouverte = Identifiant(c[1]);
                } else if (std::strcmp(c[0], "page") == 0 && n >= 6) {
                    Page p;
                    p.id = Identifiant(c[1]);
                    if (p.id == 0) continue;
                    p.titre = c[2];
                    p.modifieeLe = c[3];
                    p.matricule = c[4];
                    p.nom = c[5];
                    pages.push_back(std::move(p));
                } else if (std::strcmp(c[0], "texte") == 0 && n >= 3) {
                    texteId = Identifiant(c[1]);
                    texteLu = Desechapper(c[2]);
                } else if (std::strcmp(c[0], "autour") == 0 && n >= 4) {
                    Voisin v;
                    v.personnageId = Identifiant(c[1]);
                    if (v.personnageId == 0) continue;
                    v.matricule = c[2];
                    v.nom = c[3];
                    autour.push_back(std::move(v));
                }
            }
            std::fclose(f);

            /* RIEN N'EST COMMIS TANT QUE LE SEQ N'A PAS BOUGÉ, NI SANS LA
               SENTINELLE : le fichier est relu pendant qu'on l'écrit, et une
               lecture tronquée acceptée se lirait « une page en moins » — ou,
               pire, une page sans son texte — et se figerait jusqu'à la
               prochaine poussée. Une lecture rejetée ne mémorise RIEN, pas même
               le seq : on la refera à la trame d'après. */
            if (seq == 0 || seq == g_seqEtat || !finVue) return;
            g_seqEtat = seq;
            /* L'ACCUSÉ DE LECTURE NE SE RETIENT QU'APRÈS LA SENTINELLE, comme
               tout le reste : une lecture rejetée ne mémorise RIEN. Et il ne
               RECULE jamais — le service client ne réécrit sa dernière séquence
               qu'en avançant, mais un fichier recousu, lui, peut dire n'importe
               quoi, et rogner sur un accusé qui a reculé perdrait des gestes. */
            if (lu > g_luParLeClient) g_luParLeClient = lu;

            /* LA PAGE OUVERTE ET SON TEXTE SE PORTENT L'UN L'AUTRE. La ligne
               `texte` redit l'id de la page, et cette redondance est le garde-
               fou : un plateau qui ouvrirait une page sans porter son texte — ou
               en portant celui d'une autre — ne doit RIEN ouvrir du tout. Un
               texte vide inventé ici s'enregistrerait par-dessus le vrai. */
            if (ouverte != 0 && texteId != ouverte) {
                SKSE::log::warn("[NOTES] plateau qui ouvre la page {} avec le texte de {} : page tenue pour fermee",
                    ouverte, texteId);
                ouverte = 0;
                texteLu.clear();
            }
            if (ouverte == 0) texteLu.clear();

            const bool avant = g_ouvert;
            g_ouvert = ouvert;
            g_pages = std::move(pages);
            g_autour = std::move(autour);
            g_ouverteServeur = ouverte;
            g_texteServeur = std::move(texteLu);

            if (!message.empty()) {
                g_message = std::move(message);
                g_messageRestant = kTramesMessage;
            }

            if (g_ouvert && !avant) {
                /* Le panneau s'ouvre : écran neuf, et on ouvre la racine — sans
                   elle, UIRoot::Render n'est jamais appelé. Le chien de garde
                   part d'ici, sinon il mordrait avant la première trame
                   dessinée. Les tampons repartent vides : il n'y a rien à
                   perdre, on vient d'arriver.
                   g_message est GARDÉ : la poussée d'ouverture peut en porter un. */
                Vider();
                g_demandee = 0;
                g_dernierDessin = g_tic;
                UIRoot::Open();
            }
            if (!g_ouvert && avant) {
                /* Le SERVEUR nous ferme (déconnexion, registre parti) : la
                   racine, ouverte pour nous, se referme avec — sinon le joueur
                   tombe sur son inventaire sans l'avoir demandé. Sauf si un
                   autre de nos écrans la tient encore : elle est à lui. */
                if (!Etabli::Ouvert() && !Appartenance::Ouvert() && !Banque::Ouvert() && !Missives::Ouvert()) {
                    UIRoot::Close();
                }
            }

            /* ── CE QUE LE PLATEAU CHANGE AUX TAMPONS, ET C'EST PRESQUE RIEN ──
               Le serveur repousse un plateau toutes les cinq secondes dès que
               quelqu'un marche autour de nous (la veille sur `autour`). Si cela
               emportait la saisie, écrire une page serait impossible en ville :
               la même page reste ouverte, DONC ON NE TOUCHE À RIEN. */
            if (g_ouverteServeur == 0) {
                if (g_chargee != 0) {
                    /* Le serveur a refermé la page — supprimée, plus à nous, ou
                       c'est le panneau entier qu'il a fermé (`evt: ferme`). On
                       TENTE l'enregistrement : il sera peut-être refusé (plus de
                       session), et un bandeau le dira, mais perdre la page sans
                       essayer serait pire. Deux chemins n'y passent PAS, et
                       c'est voulu : la suppression et Échap, qui vident tous
                       deux les tampons avant que cette ligne ne les voie. */
                    EnregistrerSiModifiee();
                    Vider();
                }
            } else if (g_ouverteServeur != g_chargee) {
                // Une AUTRE page s'ouvre. Les gestes qui mènent ici enregistrent
                // déjà d'eux-mêmes ; ce rappel couvre le cas où le serveur
                // changerait de page sans qu'on l'ait demandé.
                EnregistrerSiModifiee();
                ChargerPageOuverte();
            } else if (g_enregistrementEnvoye) {
                /* MÊME PAGE, ET UN ENREGISTREMENT EN L'AIR : le serveur regrave
                   `modifieeLe` à chaque écriture, donc une date qui a bougé sur
                   NOTRE page est sa confirmation. On adopte alors CE QU'ON AVAIT
                   ENVOYÉ, jamais ce qui est à l'écran : trois mots tapés pendant
                   l'aller-retour ne sont pas enregistrés, et les déclarer tels
                   les perdrait au plateau suivant. */
                const Page* p = PageParId(g_chargee);
                if (p != nullptr && p->modifieeLe != g_dateChargee) {
                    g_titreOrigine = g_titreEnvoye;
                    g_texteOrigine = g_texteEnvoye;
                    g_dateChargee = p->modifieeLe;
                    g_enregistrementEnvoye = false;
                }
            }

            // La demande d'ouverture est satisfaite dès que le serveur a
            // répondu ; et une page qui a quitté le carnet n'est plus attendue.
            if (g_demandee != 0 && (g_demandee == g_ouverteServeur || PageParId(g_demandee) == nullptr)) {
                g_demandee = 0;
            }
        }

        // ── les dates ─────────────────────────────────────────────────────

        /** L'instant d'un ISO du serveur (`2026-09-07T14:23:11.123Z`), en temps
         *  absolu. Rend false si la date est illisible.
         *
         *  Le décalage en queue (« Z », « +02:00 », « -05:00 ») est honoré ;
         *  sans rien, c'est de l'UTC — c'est ce que `toISOString` écrit. Le
         *  signe se cherche APRÈS le « T » : la date en a déjà deux. Même
         *  lecture que celle du courrier et du comptoir de la banque (espace
         *  anonyme, donc recopiée). */
        bool InstantDe(const std::string& a_iso, std::time_t& a_out)
        {
            int an = 0, mois = 0, jour = 0, heure = 0, minute = 0;
            if (std::sscanf(a_iso.c_str(), "%4d-%2d-%2dT%2d:%2d", &an, &mois, &jour, &heure, &minute) != 5) {
                return false;
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
            if (t == static_cast<std::time_t>(-1)) return false;
            a_out = t - static_cast<std::time_t>(decalageMinutes) * 60;
            return true;
        }

        /** « JJ/MM HH:MM » en heure LOCALE. Illisible ⇒ l'ISO brut, jamais
         *  rien : une page sans date se lirait comme une page de plus. */
        std::string HeureLocale(const std::string& a_iso)
        {
            std::time_t instant = 0;
            if (!InstantDe(a_iso, instant)) return a_iso;
            std::tm local{};
            if (localtime_s(&local, &instant) != 0) return a_iso;
            char sortie[32];
            std::snprintf(sortie, sizeof(sortie), "%02d/%02d %02d:%02d",
                local.tm_mday, local.tm_mon + 1, local.tm_hour, local.tm_min);
            return sortie;
        }

        // ── les petits morceaux ───────────────────────────────────────────

        /** Ce qu'on affiche de quelqu'un : son NOM si on le connaît, sinon son
         *  MATRICULE. La règle du jeu entier, et le serveur a déjà décidé ce
         *  qu'on a le droit de lire — un nom vide veut dire « on ne s'est pas
         *  présentés ». */
        const char* Affiche(const std::string& a_nom, const std::string& a_matricule)
        {
            return a_nom.empty() ? a_matricule.c_str() : a_nom.c_str();
        }

        /** L'ÉTIQUETTE D'UNE PAGE : son titre, ou « Page n » — son RANG dans le
         *  carnet, jamais son id (un id de registre ne veut rien dire pour un
         *  joueur, et deux carnets n'ont pas les mêmes). */
        std::string Etiquette(const Page& a_p, std::size_t a_rang)
        {
            if (!a_p.titre.empty()) return a_p.titre;
            char brut[32];
            std::snprintf(brut, sizeof(brut), "Page %zu", a_rang);
            return brut;
        }

        /** Un filet d'un pixel sous le dernier widget posé. */
        void FiletSousItem()
        {
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), OrSombre(0.35f), 1.0f);
        }

        /** Un filet d'un pixel sur toute la largeur utile, là où le curseur en
         *  est. Sépare deux blocs d'une colonne. */
        void FiletTravers(float a_S)
        {
            ImGui::Spacing();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float largeur = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + largeur, p.y), OrSombre(0.30f), 1.0f);
            ImGui::Dummy(ImVec2(0.0f, 2.0f * a_S));
        }

        /** Le titre discret d'un bloc. */
        void TitreBloc(const char* a_texte)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            ImGui::TextUnformatted(a_texte);
            ImGui::PopStyleColor();
        }

        /** Les couleurs communes aux listes : pas de fond, un voile au survol. */
        void PousserStyleListe()
        {
            ImGui::PushStyleColor(ImGuiCol_Header, Voile(0.0f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Voile(0.14f));
        }

        void RetirerStyleListe()
        {
            ImGui::PopStyleColor(3);
        }

        /** Les couleurs d'un bouton du panneau : un voile, aucun cadre. */
        void PousserStyleBouton()
        {
            ImGui::PushStyleColor(ImGuiCol_Button, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Voile(0.16f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.85f));
        }

        void RetirerStyleBouton()
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
        }

        /** La largeur d'un bouton, mesurée sur son libellé. */
        float LargeurBouton(const char* a_libelle, float a_S)
        {
            return ImGui::CalcTextSize(a_libelle).x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f * a_S;
        }

        // ── la colonne de gauche : la liste des pages ─────────────────────

        /** DEMANDER UNE PAGE — le seul chemin vers un texte. On enregistre ce
         *  qui était en cours AVANT de partir : la page qu'on quitte ne se
         *  perd pas parce qu'on en a ouvert une autre. */
        void DemanderPage(int a_id)
        {
            EnregistrerSiModifiee();
            EcrireGeste("page", std::to_string(a_id));
            g_demandee = a_id;
            g_demandeeDepuis = g_tic;
            g_partage = false;
            g_confirme = false;
        }

        void ColonneGauche(float a_S)
        {
            TitreBloc("Mes pages");
            ImGui::Spacing();

            const float haut = ImGui::GetFrameHeight();
            PousserStyleBouton();
            /* LA LARGEUR EST DONNÉE, PAS DEMANDÉE : `Sfx::Button` traite toute
               taille ≤ 0 comme « à la mesure du libellé » — le −1 d'ImGui, qui
               veut dire « tout ce qui reste », y ferait un bouton court sans
               rien signaler. */
            /* LA SONDE DU CLIC (07/09/2026) — « l'interface s'affiche mais
               quand je clique sur nouvelle page rien ne se passe ». Le fichier
               du pont était VIDE et le journal ne portait aucun « geste ecrit » :
               le clic n'atteignait donc pas le bouton, et l'analyse du code
               n'a rien trouvé qui l'explique. On ne devine pas deux fois — on
               MESURE. Cette sonde ne parle QUE sur un clic gauche, panneau
               ouvert : elle dit où est la souris, où est le bouton, et si
               ImGui le tenait pour survolé. Elle s'enlèvera quand la cause
               sera connue. */
            const ImVec2 avantBouton = ImGui::GetCursorScreenPos();
            const float largeurBouton = ImGui::GetContentRegionAvail().x;
            const bool clicSonde = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            if (Sfx::Button("Nouvelle page##vk_notes_creer",
                    ImVec2(largeurBouton, haut))) {
                /* Le serveur crée la page et répond par un plateau ; c'est LUI
                   qui dit quel id elle a reçu, et c'est lui qui l'ouvre. On
                   n'essaie pas de deviner « la dernière de la liste » : deux
                   carnets ne se rangent pas pareil, et deviner ici, c'est
                   ouvrir la page d'à côté un jour sur dix. */
                EnregistrerSiModifiee();
                EcrireGeste("creer", "");
            }
            if (clicSonde) {
                const ImVec2 m = ImGui::GetIO().MousePos;
                const ImVec2 r0 = ImGui::GetItemRectMin();
                const ImVec2 r1 = ImGui::GetItemRectMax();
                SKSE::log::info(
                    "[NOTES] sonde clic : souris ({:.0f},{:.0f}) bouton ({:.0f},{:.0f})-({:.0f},{:.0f}) "
                    "large={:.0f} survole={} actif={} fenetreSurvolee={} fenetreActive={} capture={}",
                    m.x, m.y, r0.x, r0.y, r1.x, r1.y, largeurBouton,
                    ImGui::IsItemHovered() ? 1 : 0, ImGui::IsItemActive() ? 1 : 0,
                    ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ? 1 : 0,
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ? 1 : 0,
                    ImGui::GetIO().WantCaptureMouse ? 1 : 0);
                (void)avantBouton;
            }
            RetirerStyleBouton();
            ImGui::Spacing();
            FiletTravers(a_S);

            if (g_pages.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TextWrapped("%s", "Ton carnet est vide. « Nouvelle page » en ouvre une.");
                ImGui::PopStyleColor();
                return;
            }

            /* L'ÉLUE : la page réclamée s'il y en a une, sinon celle qui est
               chargée. La liste montre donc TOUT DE SUITE le clic, avant même
               que le serveur ait répondu — c'est la colonne de droite, elle, qui
               attend le texte pour changer. */
            const int elue = g_demandee != 0 ? g_demandee : g_chargee;

            ImGui::BeginChild("##vk_notes_liste", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            PousserStyleListe();
            std::size_t rang = 0;
            for (const auto& p : g_pages) {
                ++rang;
                const bool recue = !p.matricule.empty();
                const float hauteur = recue ? 42.0f * a_S : 30.0f * a_S;
                ImGui::PushID(p.id);
                const ImVec2 depart = ImGui::GetCursorScreenPos();
                const bool clic = ImGui::Selectable("##page", p.id == elue,
                    ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, hauteur));
                const float largeur = ImGui::GetItemRectSize().x;
                if (ImGui::IsItemHovered() && !p.modifieeLe.empty()) {
                    ImGui::SetTooltip("modifiée le %s", HeureLocale(p.modifieeLe).c_str());
                }

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float lh = ImGui::GetTextLineHeight();
                const float y = recue ? depart.y + 4.0f * a_S : depart.y + (hauteur - lh) * 0.5f;
                const std::string etiquette = Etiquette(p, rang);
                dl->AddText(ImVec2(depart.x + 8.0f * a_S, y),
                    p.id == elue ? Theme::GoldCol() : Theme::Chrome(p.titre.empty() ? 0.62f : 0.92f),
                    etiquette.c_str());
                if (recue) {
                    /* LA PROVENANCE SE DIT SOUS L'ÉTIQUETTE, et elle compte : une
                       page reçue est une COPIE qu'on nous a tendue, l'original
                       ne bouge plus chez son auteur. Le joueur doit savoir
                       laquelle des deux il a sous les yeux. */
                    char venue[96];
                    std::snprintf(venue, sizeof(venue), "reçue de %s", Affiche(p.nom, p.matricule));
                    dl->AddText(ImVec2(depart.x + 8.0f * a_S, depart.y + 4.0f * a_S + lh + 2.0f * a_S),
                        Theme::Chrome(0.50f), venue);
                } else if (!p.modifieeLe.empty()) {
                    const std::string quand = HeureLocale(p.modifieeLe);
                    const float l = ImGui::CalcTextSize(quand.c_str()).x;
                    dl->AddText(ImVec2(depart.x + largeur - l - 8.0f * a_S, y), Theme::Chrome(0.40f),
                        quand.c_str());
                }
                dl->AddLine(ImVec2(depart.x, depart.y + hauteur), ImVec2(depart.x + largeur, depart.y + hauteur),
                    OrSombre(0.18f), 1.0f);
                if (p.id == elue) {
                    dl->AddRectFilled(ImVec2(depart.x, depart.y),
                        ImVec2(depart.x + 2.0f * a_S, depart.y + hauteur), Theme::GoldCol());
                }
                if (clic) {
                    /* Cliquer la page DÉJÀ chargée ne coûte pas un aller : elle
                       est là. Cliquer n'importe quelle autre — ou revenir sur
                       celle-ci pendant qu'on en attend une autre — repart. */
                    if (p.id != g_chargee || g_demandee != 0) DemanderPage(p.id);
                }
                ImGui::PopID();
            }
            RetirerStyleListe();
            ImGui::EndChild();
        }

        // ── la colonne de droite ─────────────────────────────────────────

        /** LA LISTE DU PARTAGE — « à qui tends-tu cette page ? ». Elle prend
         *  toute la colonne plutôt qu'une fenêtre par-dessus : on ne tend pas
         *  une page en même temps qu'on l'écrit, et la page a déjà été
         *  enregistrée en arrivant ici.
         *
         *  ELLE SE RAFRAÎCHIT TOUTE SEULE : `autour` est repoussé par le serveur
         *  tant que le panneau est ouvert (les gens marchent), et cet écran ne
         *  fait que relire la liste à chaque trame. Celui qui s'éloigne
         *  disparaît, et le serveur remesurera de toute façon les cinq mètres à
         *  l'instant du clic. */
        void BlocPartage(const Page& a_p, std::size_t a_rang, float a_S)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::Text("Tendre « %s »", Etiquette(a_p, a_rang).c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            ImGui::TextWrapped("%s", "Tu en donnes une COPIE : elle sera à lui, et la tienne ne bougera plus.");
            ImGui::PopStyleColor();

            FiletTravers(a_S);

            const float hautPied = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
            ImGui::BeginChild("##vk_notes_autour", ImVec2(0.0f, -hautPied), ImGuiChildFlags_None);
            if (g_autour.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TextWrapped("%s", "Personne à cinq mètres. Approche-toi de quelqu'un.");
                ImGui::PopStyleColor();
            } else {
                PousserStyleListe();
                const float hauteur = 28.0f * a_S;
                for (const auto& v : g_autour) {
                    ImGui::PushID(v.personnageId);
                    const ImVec2 depart = ImGui::GetCursorScreenPos();
                    const bool clic = ImGui::Selectable("##qui", false,
                        ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, hauteur));
                    const float largeur = ImGui::GetItemRectSize().x;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const float milieu = depart.y + (hauteur - ImGui::GetTextLineHeight()) * 0.5f;
                    dl->AddText(ImVec2(depart.x + 8.0f * a_S, milieu),
                        Theme::Chrome(v.nom.empty() ? 0.60f : 0.92f), Affiche(v.nom, v.matricule));
                    if (!v.nom.empty()) {
                        const float l = ImGui::CalcTextSize(v.matricule.c_str()).x;
                        dl->AddText(ImVec2(depart.x + largeur - l - 8.0f * a_S, milieu), Theme::Chrome(0.45f),
                            v.matricule.c_str());
                    }
                    if (clic) {
                        /* Le serveur remesure les cinq mètres À CET INSTANT :
                           celui qui s'est éloigné pendant qu'on choisissait ne
                           reçoit rien, et il le dit. On n'anticipe pas son
                           verdict — on ne referme la liste qu'après le geste. */
                        EnregistrerSiModifiee();
                        EcrireGeste("donner",
                            std::to_string(a_p.id) + "\t" + std::to_string(v.personnageId));
                        g_partage = false;
                    }
                    ImGui::PopID();
                }
                RetirerStyleListe();
            }
            ImGui::EndChild();

            PousserStyleBouton();
            if (Sfx::Button("Annuler##vk_notes_partage_annuler",
                    ImVec2(LargeurBouton("Annuler", a_S), ImGui::GetFrameHeight()))) {
                g_partage = false;
            }
            RetirerStyleBouton();
        }

        /** LA PAGE : son titre, son texte, et les trois gestes qui la
         *  concernent. */
        void BlocPage(const Page& a_p, std::size_t a_rang, float a_S)
        {
            // ---- le titre ----
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::SetNextItemWidth(-1.0f);
            /* LE TITRE PEUT RESTER VIDE (décision du propriétaire, 07/09) : on
               crée une page avant de savoir comment l'appeler, et la liste
               affiche alors « Page n ». L'invite le dit, plutôt qu'un astérisque
               qui laisserait croire à un champ obligatoire. */
            ImGui::InputTextWithHint("##vk_notes_titre", "titre de la page (facultatif)",
                g_titre, sizeof(g_titre), ImGuiInputTextFlags_CallbackEdit, &BornerCaracteres,
                &g_borneTitre);
            ImGui::PopStyleColor(4);
            FiletSousItem();

            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            if (!a_p.matricule.empty()) {
                ImGui::Text("reçue de %s — modifiée le %s", Affiche(a_p.nom, a_p.matricule),
                    HeureLocale(a_p.modifieeLe).c_str());
            } else {
                ImGui::Text("modifiée le %s", HeureLocale(a_p.modifieeLe).c_str());
            }
            ImGui::PopStyleColor();

            FiletTravers(a_S);

            // ---- le texte ----
            const int caracteres = Caracteres(g_texte);
            const bool modifiee = Modifiee();

            /* Le pied (compteur, boutons, la ligne qui dit ce qui manque, et la
               confirmation quand elle est là) a sa place réservée AVANT le
               texte : un champ qui se mesurerait sur ce qui reste APRÈS les
               aurait poussés dehors dès la première ligne de trop. */
            const float ligne = ImGui::GetTextLineHeightWithSpacing();
            const float hautPied = ImGui::GetFrameHeight() * (g_confirme ? 2.0f : 1.0f) +
                                   ligne + ImGui::GetStyle().ItemSpacing.y * 4.0f;
            const float hautSaisie = (std::max)(ImGui::GetContentRegionAvail().y - hautPied, ligne * 3.0f);

            ImGui::PushStyleColor(ImGuiCol_FrameBg, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Voile(0.08f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Voile(0.08f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.92f));
            /* Les retours à la ligne SURVIVENT jusqu'au bout — le champ est
               multiligne, le pont les échappe, le registre les garde. Une page a
               des paragraphes.
               PAS DE TABULATION DANS LA SAISIE (le défaut d'ImGui) : le pont la
               changerait en espace, et le joueur verrait son retrait disparaître
               entre l'écriture et la relecture. */
            ImGui::InputTextMultiline("##vk_notes_texte", g_texte, sizeof(g_texte),
                ImVec2(-1.0f, hautSaisie), ImGuiInputTextFlags_CallbackEdit, &BornerCaracteres,
                &g_borneTexte);
            ImGui::PopStyleColor(4);

            // ---- le compteur et les trois boutons ----
            char compteur[32];
            std::snprintf(compteur, sizeof(compteur), "%d / %d", caracteres, kTexteMax);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text,
                caracteres >= kTexteMax ? Theme::GoldCol() : Theme::Chrome(0.45f));
            ImGui::TextUnformatted(compteur);
            ImGui::PopStyleColor();

            const float haut = ImGui::GetFrameHeight();
            const float lEnr = LargeurBouton("Enregistrer", a_S);
            const float lPar = LargeurBouton("Partager", a_S);
            const float lSup = LargeurBouton("Supprimer", a_S);
            const float ecart = 8.0f * a_S;
            ImGui::SameLine(
                (std::max)(ImGui::GetContentRegionAvail().x - (lEnr + lPar + lSup + ecart * 2.0f), 0.0f), 0.0f);

            PousserStyleBouton();
            ImGui::BeginDisabled(!modifiee);
            if (Sfx::Button("Enregistrer##vk_notes_enregistrer", ImVec2(lEnr, haut))) {
                EnregistrerSiModifiee();
            }
            ImGui::EndDisabled();
            /* LE SURVOL D'UN BOUTON GRISÉ COMPTE, comme chez les missives : sans
               le drapeau, ImGui ne rapporte aucun survol sur un item désactivé,
               et un bouton grisé est MUET. */
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !modifiee) {
                ImGui::SetTooltip("%s", "rien n'a changé depuis le dernier enregistrement");
            }
            ImGui::SameLine(0.0f, ecart);
            if (Sfx::Button("Partager##vk_notes_partager", ImVec2(lPar, haut))) {
                /* ON ENREGISTRE AVANT DE TENDRE : le serveur donne la page telle
                   qu'elle est AU REGISTRE. Tendre une page qu'on vient d'écrire
                   sans l'enregistrer, ce serait tendre la version d'avant, et
                   personne ne le verrait. */
                EnregistrerSiModifiee();
                g_confirme = false;
                g_partage = true;
            }
            ImGui::SameLine(0.0f, ecart);
            if (Sfx::Button("Supprimer##vk_notes_supprimer", ImVec2(lSup, haut))) {
                g_confirme = true;
            }
            RetirerStyleBouton();

            // ---- ce qui reste à dire ----
            if (g_confirme) {
                /* UNE SUPPRESSION SE CONFIRME, et la confirmation se lit sur la
                   page qu'on va perdre — pas dans une fenêtre qui la cacherait. */
                ImGui::PushStyleColor(ImGuiCol_Text, RougeSombre());
                ImGui::AlignTextToFramePadding();
                ImGui::Text("Supprimer « %s » ? C'est sans retour.", Etiquette(a_p, a_rang).c_str());
                ImGui::PopStyleColor();
                const float lOui = LargeurBouton("Oui, supprimer", a_S);
                const float lNon = LargeurBouton("Annuler", a_S);
                ImGui::SameLine((std::max)(ImGui::GetContentRegionAvail().x - (lOui + lNon + ecart), 0.0f), 0.0f);
                PousserStyleBouton();
                if (Sfx::Button("Oui, supprimer##vk_notes_supprimer_oui", ImVec2(lOui, haut))) {
                    EcrireGeste("supprimer", std::to_string(a_p.id));
                    /* LES TAMPONS PARTENT AVEC ELLE, ET TOUT DE SUITE : sans
                       cela, le plateau qui refermera la page passerait par
                       `EnregistrerSiModifiee` et RÉÉCRIRAIT ce qu'on vient
                       d'effacer. Si le serveur refuse la suppression, la page
                       reste dans la liste et se rouvre d'un clic. */
                    Vider();
                }
                ImGui::SameLine(0.0f, ecart);
                if (Sfx::Button("Annuler##vk_notes_supprimer_non", ImVec2(lNon, haut))) {
                    g_confirme = false;
                }
                RetirerStyleBouton();
            } else if (!modifiee) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.35f));
                ImGui::TextUnformatted("À jour.");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.55f));
                ImGui::TextUnformatted("Modifiée — « Enregistrer », ou referme, ce qui l'enregistre aussi.");
                ImGui::PopStyleColor();
            }
        }

        void ColonneDroite(float a_S)
        {
            /* LA PAGE RÉCLAMÉE PASSE AVANT TOUT : tant que son texte n'est pas
               arrivé, on le DIT. On n'invente pas un texte vide — il
               s'enregistrerait par-dessus le vrai — et on ne laisse pas celui de
               la page d'avant sous le titre d'une autre. */
            if (g_demandee != 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.55f));
                ImGui::TextWrapped("%s", "Ouverture de la page…");
                ImGui::PopStyleColor();
                return;
            }
            if (g_chargee == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TextWrapped("%s", "Choisis une page à gauche, ou crée-en une.");
                ImGui::PopStyleColor();
                return;
            }
            /* LE RANG SERT L'ÉTIQUETTE des pages sans titre : il se compte sur la
               liste, pas sur l'id. */
            std::size_t rang = 0;
            const Page* page = nullptr;
            std::size_t i = 0;
            for (const auto& p : g_pages) {
                ++i;
                if (p.id == g_chargee) {
                    page = &p;
                    rang = i;
                }
            }
            if (page == nullptr) {
                // La page chargée a quitté le carnet entre deux trames : le
                // prochain plateau videra les tampons, on ne dessine rien d'ici là.
                ImGui::TextDisabled("Cette page n'est plus dans ton carnet.");
                return;
            }
            if (g_partage) BlocPartage(*page, rang, a_S);
            else BlocPage(*page, rang, a_S);
        }
    }

    // ── l'interface publique ──────────────────────────────────────────────

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kCheminGestes, "w")) {
            std::fclose(f);
            g_octetsGestes = 0;
            g_pret = true;
            SKSE::log::info("[NOTES] pont pret ({})", kCheminGestes);
        } else {
            SKSE::log::warn("[NOTES] impossible d'ouvrir {} — le carnet restera sourd", kCheminGestes);
        }
        // UN ÉTAT RESCAPÉ D'UN PLANTAGE ferait surgir le panneau au lancement du
        // jeu : au boot g_seqEtat repart à 0, et le premier Tick lirait un
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
        /* APRÈS la lecture, jamais avant : c'est elle qui vient d'apprendre du
           client ce qu'il a consommé. */
        RognerLesGestes();

        if (g_messageRestant > 0) {
            if (--g_messageRestant == 0) g_message.clear();
        }

        /* UN REFUS DOIT DÉMENTIR L'ATTENTE, ET RIEN NE LE FAISAIT.
           `g_demandee` n'a que trois sorties — le serveur ouvre CETTE page, la
           page quitte le carnet, le panneau se ferme — et un `page` REFUSÉ n'en
           est aucune : la page reste dans la liste, le plateau suivant porte
           l'ancienne page ouverte, et la colonne de droite affirmait
           « Ouverture de la page… » pour toujours. Le joueur s'en sortait en
           recliquant, mais rien à l'écran ne le lui suggérait — on lui disait
           qu'on ouvrait sa page, et on ne l'ouvrait pas.
           Deux chemins y menaient : la cadence du relais (corrigée le même
           jour) et le registre muet, qui reste. On renonce donc tout haut. */
        if (g_ouvert && g_demandee != 0 && g_tic - g_demandeeDepuis > kTramesAttentePage) {
            SKSE::log::warn("[NOTES] page {} toujours pas ouverte apres {} trames : on renonce",
                g_demandee, g_tic - g_demandeeDepuis);
            g_demandee = 0;
            g_message = "La page n'a pas voulu s'ouvrir — reclique dessus.";
            g_messageRestant = kTramesMessage;
        }

        /* Le chien de garde : la racine s'est refermée sans passer par notre
           couche Échap (Tab, un menu vanilla, un autre de nos écrans qui a la
           priorité au rendu) — après deux secondes sans une trame dessinée, on
           se ferme et on le DIT, sinon le serveur nous croit toujours devant le
           panneau. LA PAGE MODIFIÉE PART AVEC, comme sur un Échap : c'est le
           même départ, il n'a simplement pas de bouton. */
        if (g_ouvert && g_tic - g_dernierDessin > kTramesSansDessin) {
            SKSE::log::info("[NOTES] panneau plus dessine depuis {} trames : fermeture", g_tic - g_dernierDessin);
            // Les sous-écrans d'abord, sinon Fermer() ne fermerait qu'eux et le
            // panneau attendrait deux secondes de plus.
            g_partage = false;
            g_confirme = false;
            Fermer();
        }
    }

    bool Ouvert()
    {
        return g_ouvert;
    }

    bool Fermer()
    {
        if (!g_ouvert) {
            // Un sous-écran sans panneau n'a pas de sens.
            g_partage = false;
            g_confirme = false;
            return false;
        }
        /* ÉCHAP FERME LE SOUS-ÉCRAN D'ABORD, et rien d'autre : le panneau reste,
           le serveur n'entend pas « fermer ». Le prochain Échap fermera le
           panneau — la couche garde sa place dans la file, comme la fenêtre de
           choix des clefs du panneau d'appartenance. */
        if (g_partage) {
            g_partage = false;
            return true;
        }
        if (g_confirme) {
            g_confirme = false;
            return true;
        }
        /* REFERMER ENREGISTRE (contrat §1.8). Le geste `ecrire` part AVANT le
           `fermer` : un joueur qui tape Échap ne pense pas avoir donné un ordre
           destructeur, et perdre ce qu'il vient d'écrire est la seule faute que
           ce chantier ne doit pas commettre.
           LE CANAL CLIENT → SERVEUR N'EST PAS ORDONNÉ (mesuré : `Client::Send`
           de skymp5-server/cpp/mp_common/Networking.cpp envoie en RELIABLE, pas
           en RELIABLE_ORDERED), donc l'enregistrement peut arriver APRÈS la
           fermeture. C'est au relais de juger `ecrire` sur l'APPARTENANCE de la
           page et non sur un panneau ouvert — ET IL NE LE FAISAIT PAS : ce
           cartouche affirmait une garde absente, `traiter` refusait tout geste
           sans session, et la page arrivée en second se faisait répondre
           « Ouvre d'abord ton carnet » alors que nos tampons étaient déjà
           vidés. Elle n'existait plus nulle part. Corrigé au relais le 07/09
           (relecture croisée), avec l'épreuve qui le garde. Ici, on envoie
           toujours dans le bon ordre et on ne bricole pas de délai qui ne
           prouverait rien. */
        EnregistrerSiModifiee();
        EcrireGeste("fermer", "");
        g_ouvert = false;
        g_demandee = 0;
        /* ET LES TAMPONS PARTENT AVEC. Sans cela, l'état vide que le service
           réécrit juste après (phase « aucune ») retomberait dans le ménage de
           `LireEtat` avec une page encore CHARGÉE et MODIFIÉE — les origines ne
           bougent qu'à la confirmation du serveur, qui n'a pas eu le temps de
           venir —, et un SECOND « ecrire » partirait vers un panneau déjà
           fermé. Le serveur le refuserait, et le joueur verrait un bandeau
           « Ouvre d'abord ton carnet » une seconde après l'avoir refermé.
           Rien n'est perdu : l'enregistrement, lui, est parti à la ligne du
           dessus. */
        Vider();
        return true;
    }

    void Dessiner()
    {
        if (!g_ouvert) return;
        g_dernierDessin = g_tic;
        /* SONDE (07/09/2026, voir la sonde du clic) : la preuve que ce panneau
           est dessiné, une ligne par seconde et pas une de plus. Sans elle, un
           écran muet et un écran ABSENT se ressemblent dans un journal. */
        if (g_tic % 60 == 0) {
            /* `clavierPris` est le témoin décisif d'Échap : GridMenu avale TOUT
               le canal des touches quand `UIRoot::IsTextInputActive()` est vrai
               (« pendant qu'un champ tient le clavier, chaque touche est du
               texte »). Si Échap ne ferme pas ET que ce témoin vaut 1 alors
               qu'aucune page n'est ouverte, la cause est là et nulle part
               ailleurs. */
            SKSE::log::info(
                "[NOTES] sonde : panneau dessine (tic {}, {} page(s), chargee={}, demandee={}, "
                "sourisCapturee={}, clavierPris={}, wantTexte={}, fenetreSurvolee={})",
                g_tic, g_pages.size(), g_chargee, g_demandee,
                ImGui::GetIO().WantCaptureMouse ? 1 : 0,
                UIRoot::IsTextInputActive() ? 1 : 0,
                ImGui::GetIO().WantTextInput ? 1 : 0,
                ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) ? 1 : 0);
        }

        const ImGuiIO& io = ImGui::GetIO();
        const float S = Theme::Scale();
        const float pad = Theme::PadX() * S;

        /* UN PANNEAU CENTRÉ ET DISCRET — le goût du propriétaire (29/08) : pas
           de bordure pleine, des filets d'un pixel, le monde reste visible
           autour. DEUX COLONNES et une HAUTEUR FIXE, comme le courrier : une
           page de deux mille caractères ferait grandir un panneau qui s'ajuste
           jusqu'à dépasser l'écran, et il changerait de taille à chaque page
           ouverte. Les bornes suivent l'échelle (S vaut 0,75 en 1080p, 2 en 4K). */
        const float largeur = std::clamp(io.DisplaySize.x * 0.48f, 860.0f * S, 1160.0f * S);
        const float hauteur = std::clamp(io.DisplaySize.y * 0.68f, 460.0f * S, 860.0f * S);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.40f),
            ImGuiCond_Always, ImVec2(0.5f, 0.35f));
        ImGui::SetNextWindowSize(ImVec2(largeur, hauteur), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 9, 8, 236));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * S, 6.0f * S));
        ImGui::Begin("##vk_notes", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
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

        ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
        ImGui::TextUnformatted("Notes");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
        ImGui::TextUnformatted("Tes pages te suivent partout. Une page se tend à cinq mètres, et c'est une copie.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        /* LES DEUX COLONNES. Le pied (le message du serveur, ou la ligne d'aide)
           a sa place réservée d'avance : deux lignes, parce qu'un message peut
           se replier. */
        const float hautPied = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
        const float hautColonnes = (std::max)(ImGui::GetContentRegionAvail().y - hautPied,
            ImGui::GetTextLineHeightWithSpacing() * 4.0f);
        const float largeurGauche = ImGui::GetContentRegionAvail().x * 0.36f;

        ImGui::BeginChild("##vk_notes_gauche", ImVec2(largeurGauche, hautColonnes), ImGuiChildFlags_None);
        ColonneGauche(S);
        ImGui::EndChild();

        // Le filet qui sépare les deux colonnes, entre les deux enfants.
        {
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(b.x + 8.0f * S, a.y), ImVec2(b.x + 8.0f * S, b.y),
                OrSombre(0.25f), 1.0f);
        }

        ImGui::SameLine(0.0f, 17.0f * S);
        ImGui::BeginChild("##vk_notes_droite", ImVec2(0.0f, hautColonnes), ImGuiChildFlags_None);
        ColonneDroite(S);
        ImGui::EndChild();

        /* LE MESSAGE : ce que le serveur vient de répondre (« Page remise à
           X. »), sept secondes ; puis la ligne d'aide reprend sa place. */
        ImGui::Spacing();
        if (!g_message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::TextWrapped("%s", g_message.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.35f));
            ImGui::TextUnformatted("Échap : fermer — ta page s'enregistre en se refermant.");
            ImGui::PopStyleColor();
        }

        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
    }
}
