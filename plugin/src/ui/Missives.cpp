#include "ui/Missives.h"
#include "ui/Appartenance.h"
#include "ui/Banque.h"
#include "ui/Etabli.h"

#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// Voir Missives.h pour l'architecture — et pour la raison d'être d'un écran à
// part. Ici : le parsing TSV de l'état, la composition des deux colonnes, et
// l'écriture des gestes.

namespace FUI::Missives
{
    namespace
    {
        constexpr const char* kCheminEtat = "Data/SKSE/Plugins/GridInventory_missives_etat.txt";
        constexpr const char* kCheminGestes = "Data/SKSE/Plugins/GridInventory_missives.txt";

        /* La borne du serveur (contrat des missives : « 1 à 700 caractères »),
           en CARACTÈRES et non en octets. Elle ne coupe rien ici : elle rougit
           le compteur et grise « Envoyer ». Le serveur refuse en parlant, et
           une lettre tronquée en silence serait pire qu'un refus. */
        constexpr int kTexteMax = 700;
        /* Le tampon de la composition, en OCTETS : sept cents caractères dont
           chacun peut en peser quatre en UTF-8, plus le zéro final. Large
           exprès — ImGui borne la saisie aux octets, et un joueur qui écrit en
           accents n'a pas à taper moins que les autres. */
        constexpr int kOctetsTexte = kTexteMax * 4 + 1;

        struct Correspondant
        {
            int         personnageId = 0;
            std::string matricule;
            std::string nom;      // vide = on ne s'est pas présentés → le matricule
            bool        enJeu = false;
        };

        struct Recue
        {
            int         id = 0;
            int         personnageId = 0;   // l'EXPÉDITEUR
            std::string matricule;
            std::string nom;
            std::string arrivee;            // l'ISO du serveur, rendu en heure locale au dessin
            bool        lue = false;
            /* LE SERVEUR TRANCHE L'ARRIVÉE D'UNE REÇUE, pas nous : c'est elle
               qui commande un texte, et l'horloge du poste n'a pas voix au
               chapitre. Colonne absente ⇒ PAS arrivée, et c'est le bon défaut :
               une lettre montrée « en route » se lit une minute plus tard,
               une lettre ouverte trop tôt ne se referme pas.
               DEPUIS LE 06/09/2026 AU SOIR, ELLE VAUT TOUJOURS 1 : le serveur ne
               pousse plus qu'une reçue LISIBLE, donc arrivée (voir `g_ici`). La
               colonne reste dans le pont — un pont qui change de largeur casse
               des deux côtés le même jour —, et on continue de la LIRE : un 0
               (ligne recousue, serveur plus vieux que la DLL) doit se voir « en
               route » plutôt que se faire passer pour une lettre à lire. */
            bool        arriveeFaite = false;
            /* Le texte de la lettre. Il n'arrive qu'avec le droit de la lire, et
               depuis le 06/09/2026 au soir la ligne elle-même n'arrive plus sans
               lui : une reçue est ici parce qu'elle se lit. VIDE reste possible
               — un serveur qui régresserait —, et cela se DIT (« Cette missive
               est vide. ») ; on ne devine jamais une lettre. */
            std::string texte;
        };

        struct Envoyee
        {
            int         id = 0;
            int         personnageId = 0;   // le DESTINATAIRE
            std::string matricule;
            std::string nom;
            std::string arrivee;
            bool        lue = false;
        };

        // ---- état reçu (le serveur fait foi) ----
        bool                       g_ouvert = false;
        /* Suis-je dans une ville ou un village — la seule chose que le lieu
           commande. On écrit de partout ; on ne LIT que d'ici.
           LE PROPRIÉTAIRE L'A REDIT LE 06/09/2026 AU SOIR, mot pour mot :
           « l'envoi doit se faire de n'importe où, c'est uniquement la
           réception qui se fait en ville ». Ne pose JAMAIS de garde de lieu sur
           la composition — la symétrie paraîtra naturelle, elle est fausse.
           ET DEPUIS CE MÊME SOIR, `ici` COMMANDE AUSSI LA LISTE DES REÇUES : le
           serveur ne pousse une lettre reçue que si elle est ARRIVÉE et qu'on
           est ICI. Hors d'un lieu habité, `g_recues` est donc VIDE — une lettre
           n'apparaît même pas tant qu'elle ne se lit pas. Les ENVOYÉES, elles,
           descendent de partout : c'est SA lettre, elle ne se cache pas de lui. */
        bool                       g_ici = false;
        std::string                g_message;
        std::vector<Correspondant> g_carnet;
        std::vector<Recue>         g_recues;     // les plus récentes d'abord
        std::vector<Envoyee>       g_envoyees;
        unsigned long long         g_seqEtat = 0;

        // ---- état de l'écran (local, jamais envoyé tel quel) ----
        char g_filtre[64] = {};        // le filtre LOCAL du carnet : nom ou matricule
        int  g_destinataire = 0;       // personnageId choisi ; 0 = personne
        int  g_lettre = 0;             // id de la missive ouverte ; 0 = on compose
        char g_texte[kOctetsTexte] = {};
        int  g_messageRestant = 0;     // trames avant effacement du message
        /* LA LETTRE DONT ON A DÉJÀ RÉCLAMÉ LA LECTURE. Sans cette mémoire, la
           condition « ouverte, arrivée, pas lue, et je suis en ville » serait
           vraie à CHAQUE trame jusqu'à ce que le serveur réponde : soixante
           gestes par seconde dans le fichier, et autant de refus de cadence. */
        int  g_lireDemande = 0;

        /* Au-delà de ce nombre de lignes, une liste défile dans son enfant. Les
           colonnes ont une hauteur fixe (voir Dessiner) : ces bornes servent à
           partager cette hauteur, pas à faire grandir le panneau. */
        constexpr std::size_t kEnvoyeesVisibles = 4;

        // ---- plomberie ----
        unsigned long long g_seqGeste = 0;
        bool               g_pret = false;
        int                g_tic = 0;
        int                g_dernierDessin = 0;

        /* Le chien de garde : la racine s'est refermée sans passer par nous
           (Tab, un menu vanilla) — sans cela le serveur garderait le joueur
           parmi les panneaux ouverts et lui repousserait des plateaux. */
        constexpr int kTramesSansDessin = 120;
        constexpr int kTramesMessage = 420;   // ~7 s à 60 fps

        // ── l'échappement des textes ──────────────────────────────────────
        //
        // UNE LETTRE A DES PARAGRAPHES, et le pont est du TSV nu. Un saut de
        // ligne couperait la ligne en deux (la seconde moitié serait lue comme
        // une ligne au premier mot inconnu, donc jetée en silence) ; une
        // tabulation décalerait toutes les colonnes suivantes.
        //
        // LA RÈGLE, ET LES DEUX CÔTÉS LA FONT PAREIL (contrat des missives) :
        //   écrire   `\r\n` et `\r` seul → un saut de ligne ; tabulation → UN
        //            espace ; `\` → `\\` ; saut de ligne → `\n` (deux
        //            caractères). RIEN D'AUTRE — ni les guillemets, ni les
        //            virgules, ni les accents.
        //   lire     `\\` → `\` ; `\n` → saut de ligne ; un `\` suivi de
        //            n'importe quoi d'autre — ou en fin de texte — est un `\`
        //            littéral, et le caractère qui suit est lu normalement.
        //
        // LE MIROIR TS est `missivesTexte.ts` (vulkaar-engine), où la paire
        // s'éprouve dans les deux sens ; une ligne corrigée ici se corrige
        // là-bas, sans quoi le pont ment dans un sens.
        //
        // OCTET PAR OCTET, et c'est sûr en UTF-8 : les octets de continuation
        // valent tous 0x80 ou plus, donc aucun ne peut être pris pour une
        // barre oblique ni pour un « n ».

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
                    // se perd, rien n'explose — la DLL n'écrit jamais cela,
                    // mais un fichier recousu le peut.
                    r += '\\';
                    r += suivant;
                }
            }
            return r;
        }

        /** Combien de CARACTÈRES (pas d'octets) : « é » en fait un seul, et le
         *  compteur des sept cents doit valoir pareil pour qui écrit avec des
         *  accents. Copie de celle du panneau d'appartenance (espace anonyme,
         *  donc inaccessible). */
        int Caracteres(const char* a_s)
        {
            int n = 0;
            for (const unsigned char* p = reinterpret_cast<const unsigned char*>(a_s); *p; ++p) {
                if ((*p & 0xC0) != 0x80) ++n;
            }
            return n;
        }

        /** `<seq>\t<action>[\t<reste>]`. Le pont des missives n'a qu'un sujet :
         *  pas de colonne de sujet, contrairement à l'appartenance. */
        void EcrireGeste(const char* a_action, const std::string& a_reste)
        {
            if (!g_pret) return;
            std::FILE* f = std::fopen(kCheminGestes, "a");
            if (!f) return;
            if (a_reste.empty()) std::fprintf(f, "%llu\t%s\n", ++g_seqGeste, a_action);
            else std::fprintf(f, "%llu\t%s\t%s\n", ++g_seqGeste, a_action, a_reste.c_str());
            std::fclose(f);
            /* LA TRACE DU CLIC, ET ELLE A UNE HISTOIRE (07/09/2026) : « en
               cliquant sur envoyer rien ne se faisait ». Pour savoir si le
               clic avait seulement quitté l'écran, il a fallu aller lire le
               FICHIER du pont, qui ne garde que ce qui est parti — ni l'heure,
               ni le reste. Une ligne par geste ne coûte qu'un appel de journal
               quand le joueur agit, et elle répond du premier coup à « le geste
               est-il parti ? ». LE TEXTE D'UNE LETTRE N'Y FIGURE JAMAIS : on
               note le geste, pas la correspondance. */
            SKSE::log::info("[MISSIVES] geste ecrit : {} (seq {})", a_action, g_seqGeste);
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

        /** Le rouge sombre du dépassement : un refus local, pas une alerte. */
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

        void LireEtat()
        {
            std::FILE* f = std::fopen(kCheminEtat, "r");
            if (!f) return;

            /* LE NUMÉRO DE SÉQUENCE SE LIT EN PREMIER, ET ON SORT AUSSITÔT.
               Ce Tick est appelé à CHAQUE trame ; le fichier est réécrit ENTIER
               à chaque poussée serveur et sa première ligne porte « seq <n> ».
               Ici l'habitude compte double : un carnet de cent correspondants
               et cent lettres font un fichier autrement plus lourd que le
               panneau d'appartenance. */
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
            bool ici = false;
            bool finVue = false;
            std::string message;
            std::vector<Correspondant> carnet;
            std::vector<Recue> recues;
            std::vector<Envoyee> envoyees;

            /* LE TAMPON D'UNE LIGNE EST PLUS GRAND QUE PARTOUT AILLEURS, et le
               compte est fait : une lettre pèse au plus sept cents caractères,
               dont chacun peut valoir quatre octets en UTF-8 — 2 800 octets.
               L'échappement double les barres obliques, qui n'en pèsent qu'un :
               les deux pires cas ne peuvent pas se cumuler. Les huit autres
               colonnes d'une ligne `recue` (deux entiers, un matricule, un nom,
               une date) tiennent largement dans le reste. Un tampon de 2 048
               couperait la lettre en deux, et la seconde moitié serait lue
               comme une ligne au premier mot inconnu — perdue en silence. */
            char ligne[4096];
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
                } else if (std::strcmp(c[0], "ici") == 0) {
                    /* Seul un « 1 » EXACT ouvre la lecture. Une ligne absente,
                       vide ou illisible laisse le courrier fermé — l'erreur qui
                       coûte est celle qui montre une lettre à qui n'y a pas
                       droit. */
                    ici = std::strcmp(c[1], "1") == 0;
                } else if (std::strcmp(c[0], "message") == 0) {
                    message = c[1];
                } else if (std::strcmp(c[0], "carnet") == 0 && n >= 5) {
                    Correspondant p;
                    p.personnageId = Identifiant(c[1]);
                    if (p.personnageId == 0) continue;
                    p.matricule = c[2];
                    p.nom = c[3];
                    p.enJeu = std::strcmp(c[4], "1") == 0;
                    carnet.push_back(std::move(p));
                } else if (std::strcmp(c[0], "recue") == 0 && n >= 8) {
                    Recue m;
                    m.id = Identifiant(c[1]);
                    m.personnageId = Identifiant(c[2]);
                    if (m.id == 0 || m.personnageId == 0) continue;
                    m.matricule = c[3];
                    m.nom = c[4];
                    m.arrivee = c[5];
                    m.lue = std::strcmp(c[6], "1") == 0;
                    m.arriveeFaite = std::strcmp(c[7], "1") == 0;
                    // La 9e colonne peut être absente (rien à lire d'ici) ou
                    // vide (la même chose) : les deux se lisent pareil.
                    if (n >= 9) m.texte = Desechapper(c[8]);
                    recues.push_back(std::move(m));
                } else if (std::strcmp(c[0], "envoyee") == 0 && n >= 7) {
                    Envoyee m;
                    m.id = Identifiant(c[1]);
                    m.personnageId = Identifiant(c[2]);
                    if (m.id == 0 || m.personnageId == 0) continue;
                    m.matricule = c[3];
                    m.nom = c[4];
                    m.arrivee = c[5];
                    m.lue = std::strcmp(c[6], "1") == 0;
                    envoyees.push_back(std::move(m));
                }
            }
            std::fclose(f);

            /* RIEN N'EST COMMIS TANT QUE LE SEQ N'A PAS BOUGÉ, NI SANS LA
               SENTINELLE : le fichier est relu pendant qu'on l'écrit, et une
               lecture tronquée acceptée se lirait « une lettre en moins » — ou,
               pire, une lettre sans son texte — et se figerait jusqu'à la
               prochaine poussée. Une lecture rejetée ne mémorise RIEN, pas même
               le seq : on la refera à la trame d'après. */
            if (seq == 0 || seq == g_seqEtat || !finVue) return;
            g_seqEtat = seq;

            const bool avant = g_ouvert;
            g_ouvert = ouvert;
            g_ici = ici;
            g_carnet = std::move(carnet);
            g_recues = std::move(recues);
            g_envoyees = std::move(envoyees);

            if (!message.empty()) {
                g_message = std::move(message);
                g_messageRestant = kTramesMessage;
            }

            /* CE QUI A DISPARU NE RESTE PAS CHOISI. Le registre efface la plus
               ancienne lettre LUE au-delà de cent, et un correspondant peut
               sortir du carnet : sans ce ménage, la colonne de droite
               resterait sur un fantôme et « Envoyer » viserait quelqu'un que le
               serveur refuserait.
               DEPUIS LE 06/09/2026 AU SOIR, C'EST AUSSI LE CHEMIN ORDINAIRE :
               sortir d'une ville vide `recues` d'un coup (le serveur ne pousse
               plus que les lisibles), et la lettre ouverte doit se refermer
               d'elle-même — la colonne de droite revient à la composition, qui,
               elle, marche de partout. Le destinataire choisi, lui, SURVIT :
               le carnet ne dépend pas du lieu. */
            if (g_lettre != 0) {
                bool encore = false;
                for (const auto& m : g_recues) {
                    if (m.id == g_lettre) encore = true;
                }
                if (!encore) g_lettre = 0;
            }
            if (g_destinataire != 0) {
                bool encore = false;
                for (const auto& p : g_carnet) {
                    if (p.personnageId == g_destinataire) encore = true;
                }
                if (!encore) g_destinataire = 0;
            }

            if (g_ouvert && !avant) {
                /* Le panneau s'ouvre : écran neuf, et on ouvre la racine — sans
                   elle, UIRoot::Render n'est jamais appelé. Le chien de garde
                   part d'ici, sinon il mordrait avant la première trame
                   dessinée. LE TEXTE EN COURS S'EFFACE ICI ET NULLE PART
                   AILLEURS : une poussée serveur (une lettre qui arrive
                   pendant qu'on écrit) ne doit pas emporter ce que le joueur
                   est en train de taper. */
                g_filtre[0] = '\0';
                g_texte[0] = '\0';
                g_destinataire = 0;
                g_lettre = 0;
                g_lireDemande = 0;
                // g_message est GARDÉ : la poussée d'ouverture peut en porter un.
                g_dernierDessin = g_tic;
                UIRoot::Open();
            }
            if (!g_ouvert && avant) {
                /* Le SERVEUR nous ferme (déconnexion, registre parti) : la
                   racine, ouverte pour nous, se referme avec — sinon le joueur
                   tombe sur son inventaire sans l'avoir demandé. Sauf si un
                   autre de nos écrans la tient encore : elle est à lui. */
                if (!Etabli::Ouvert() && !Appartenance::Ouvert() && !Banque::Ouvert()) UIRoot::Close();
            }
        }

        // ── les dates ─────────────────────────────────────────────────────

        /** L'instant d'un ISO du serveur (`2026-09-06T14:23:11.123Z`), en temps
         *  absolu. Rend false si la date est illisible.
         *
         *  Le décalage en queue (« Z », « +02:00 », « -05:00 ») est honoré ;
         *  sans rien, c'est de l'UTC — c'est ce que `toISOString` écrit. Le
         *  signe se cherche APRÈS le « T » : la date en a déjà deux. Même
         *  lecture que celle du comptoir de la banque (espace anonyme, donc
         *  recopiée). */
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
         *  rien : une lettre sans date se lirait comme une lettre de plus. */
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

        /** UNE LETTRE ENVOYÉE EST-ELLE ARRIVÉE ? L'horloge du POSTE répond, et
         *  c'est permis parce qu'AUCUNE RÈGLE n'en dépend : le serveur tranche
         *  l'arrivée d'une lettre REÇUE (elle commande un texte), jamais celle
         *  d'une lettre partie — on n'y regarde que si la sienne est rendue.
         *  Une horloge en avance de deux minutes l'annonce deux minutes trop
         *  tôt, et c'est tout ce qu'elle peut faire. Une date illisible se lit
         *  « en route » : on n'affirme pas une arrivée qu'on ne sait pas dater. */
        bool EnvoiArrive(const Envoyee& a_m)
        {
            if (a_m.lue) return true;   // lue, donc arrivée : le serveur l'a dit
            std::time_t instant = 0;
            if (!InstantDe(a_m.arrivee, instant)) return false;
            return std::time(nullptr) >= instant;
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

        /** La loupe, dessinée à la main : les polices cuites du greffon n'ont
         *  pas de glyphe pour elle. Copie de celle du panneau d'appartenance
         *  (espace anonyme, donc inaccessible). */
        void Loupe(const ImVec2& a_case, float a_S)
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float r = 5.5f * a_S;
            const ImVec2 centre(a_case.x + r + 2.0f * a_S, a_case.y + r + 3.0f * a_S);
            dl->AddCircle(centre, r, Theme::Chrome(0.6f), 0, 1.5f * a_S);
            dl->AddLine(ImVec2(centre.x + r * 0.7f, centre.y + r * 0.7f),
                ImVec2(centre.x + r * 1.7f, centre.y + r * 1.7f), Theme::Chrome(0.6f), 1.8f * a_S);
        }

        /** Le titre discret d'un bloc. */
        void TitreBloc(const char* a_texte)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            ImGui::TextUnformatted(a_texte);
            ImGui::PopStyleColor();
        }

        /** Les couleurs communes aux listes : pas de fond, un voile au survol,
         *  des champs de saisie sur un voile plus léger encore. */
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

        /** Le filtre LOCAL : la ligne contient-elle ce qu'on a tapé ? Sans
         *  casse, sur le nom ET sur le matricule — on cherche « Ém » comme
         *  « 0A2F3 ». Comparaison octet par octet en ASCII : les accents ne se
         *  replient pas, et c'est assumé — le matricule n'en a pas, et un nom
         *  se retrouve par son début. */
        bool Retient(const char* a_filtre, const std::string& a_nom, const std::string& a_matricule)
        {
            if (a_filtre[0] == '\0') return true;
            const auto contient = [&](const std::string& a_ou) {
                if (a_ou.empty()) return false;
                const std::size_t n = std::strlen(a_filtre);
                if (n > a_ou.size()) return false;
                for (std::size_t i = 0; i + n <= a_ou.size(); ++i) {
                    std::size_t j = 0;
                    while (j < n && std::tolower(static_cast<unsigned char>(a_ou[i + j])) ==
                                        std::tolower(static_cast<unsigned char>(a_filtre[j]))) {
                        ++j;
                    }
                    if (j == n) return true;
                }
                return false;
            };
            return contient(a_nom) || contient(a_matricule);
        }

        // ── la colonne de gauche : le carnet, puis le courrier reçu ───────

        /** Une ligne de personne : la pastille « en jeu », le nom (ou le
         *  matricule), et le matricule en gris à droite. Rend true au clic. */
        bool LignePersonne(const Correspondant& a_p, bool a_elu, float a_haut, float a_S)
        {
            const ImVec2 depart = ImGui::GetCursorScreenPos();
            const bool clic = ImGui::Selectable("##ligne", a_elu,
                ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, a_haut));
            const float largeur = ImGui::GetItemRectSize().x;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", a_p.enJeu ? "en jeu" : "absent");
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float milieu = depart.y + (a_haut - ImGui::GetTextLineHeight()) * 0.5f;
            // La pastille : en or quand la personne est en jeu, éteinte sinon.
            dl->AddCircleFilled(ImVec2(depart.x + 7.0f * a_S, depart.y + a_haut * 0.5f), 3.0f * a_S,
                a_p.enJeu ? Theme::GoldCol() : Theme::Chrome(0.25f), 12);
            const char* affiche = Affiche(a_p.nom, a_p.matricule);
            dl->AddText(ImVec2(depart.x + 18.0f * a_S, milieu),
                a_elu ? Theme::GoldCol() : Theme::Chrome(a_p.nom.empty() ? 0.55f : 0.92f), affiche);
            // Le matricule, à droite — sauf s'il tient déjà lieu de nom.
            if (!a_p.nom.empty()) {
                const float l = ImGui::CalcTextSize(a_p.matricule.c_str()).x;
                dl->AddText(ImVec2(depart.x + largeur - l - 8.0f * a_S, milieu), Theme::Chrome(0.50f),
                    a_p.matricule.c_str());
            }
            if (a_elu) {
                dl->AddRectFilled(ImVec2(depart.x, depart.y), ImVec2(depart.x + 2.0f * a_S, depart.y + a_haut),
                    Theme::GoldCol());
            }
            return clic;
        }

        /** Le carnet, filtré. Un clic choisit le destinataire ET revient à la
         *  composition : on a cliqué sur quelqu'un pour lui écrire. */
        void BlocCarnet(float a_S)
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
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.92f));
            /* AUCUN GESTE NE PART D'ICI : le carnet est déjà descendu en
               entier, et le contrat du pont n'a pas de « chercher ». Ce champ
               ne fait que cacher des lignes. */
            ImGui::InputTextWithHint("##vk_missives_filtre", "filtrer — nom ou matricule",
                g_filtre, sizeof(g_filtre));
            ImGui::PopStyleColor(4);
            FiletSousItem();
            ImGui::Spacing();

            if (g_carnet.empty()) {
                ImGui::TextDisabled("Personne encore. Présente-toi à quelqu'un (touche X).");
                return;
            }

            const float haut = 28.0f * a_S;
            /* La liste prend ce qui reste de la moitié haute de la colonne :
               les deux blocs se partagent la hauteur, et aucun ne pousse
               l'autre hors de l'écran. */
            ImGui::BeginChild("##vk_missives_carnet", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            PousserStyleListe();
            int vues = 0;
            for (const auto& p : g_carnet) {
                if (!Retient(g_filtre, p.nom, p.matricule)) continue;
                ++vues;
                ImGui::PushID(p.personnageId);
                if (LignePersonne(p, p.personnageId == g_destinataire, haut, a_S)) {
                    g_destinataire = p.personnageId;
                    // On revient à la composition : c'est pour écrire qu'on
                    // clique un nom, pas pour changer la lettre affichée.
                    g_lettre = 0;
                }
                ImGui::PopID();
            }
            if (vues == 0) ImGui::TextDisabled("personne ne répond à ce nom");
            RetirerStyleListe();
            ImGui::EndChild();
        }

        /** Le courrier reçu : l'expéditeur, et « arrivée » ou « en route ». Les
         *  NON LUES en or — c'est ce qu'on vient chercher.
         *
         *  CETTE LISTE NE PORTE QUE DES LETTRES LISIBLES depuis le 06/09/2026 au
         *  soir : le serveur ne pousse une reçue que si elle est arrivée ET que
         *  le lecteur est dans un lieu habité. Hors d'un lieu habité, elle est
         *  donc VIDE — et c'est là que « Aucune missive. » se mettait à mentir,
         *  puisqu'une lettre peut très bien attendre à la poste. */
        void BlocRecues(float a_S)
        {
            int attendent = 0;
            for (const auto& m : g_recues) {
                if (m.arriveeFaite && !m.lue) ++attendent;
            }
            char titre[64];
            if (attendent > 0) std::snprintf(titre, sizeof(titre), "Courrier reçu  (%d à lire)", attendent);
            else std::snprintf(titre, sizeof(titre), "%s", "Courrier reçu");
            TitreBloc(titre);

            if (g_recues.empty()) {
                /* DEUX VIDES QUI NE VEULENT PAS DIRE LA MÊME CHOSE. En ville, la
                   boîte est vraiment vide. Ailleurs, on ne sait RIEN d'elle : le
                   serveur ne descend pas les reçues, et le bandeau de l'entrée en
                   jeu est le seul à en donner le compte. Dire « Aucune missive. »
                   hors d'un lieu habité, c'était affirmer un fait qu'on n'a pas.

                   LA PHRASE DU DEHORS DIT LA RÈGLE, PAS UN FAIT, et c'est la même
                   raison retournée : « ton courrier t'ATTEND » affirmerait qu'il y
                   en a, alors qu'on n'en sait rien — un joueur dont la boîte est
                   vide traverserait la carte pour lire « Aucune missive. ». On dit
                   donc où le courrier SE LIT, ce qui reste vrai que la boîte soit
                   pleine ou vide. Le seul endroit qui a le droit d'annoncer un
                   COMPTE est le bandeau de l'entrée en jeu, parce que lui, il a
                   compté.
                   TextWrapped et non TextDisabled : la colonne de gauche fait
                   quatre dixièmes du panneau, et la phrase du dehors est longue. */
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TextWrapped("%s", g_ici ? "Aucune missive."
                                               : "Ton courrier se lit dans une ville ou un village.");
                ImGui::PopStyleColor();
                return;
            }

            const float haut = 30.0f * a_S;
            ImGui::BeginChild("##vk_missives_recues", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            PousserStyleListe();
            for (const auto& m : g_recues) {
                const bool elu = m.id == g_lettre;
                ImGui::PushID(m.id);
                const ImVec2 depart = ImGui::GetCursorScreenPos();
                const bool clic = ImGui::Selectable("##lettre", elu,
                    ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, haut));
                const float largeur = ImGui::GetItemRectSize().x;
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s %s", m.arriveeFaite ? "arrivée le" : "arrivera le",
                        HeureLocale(m.arrivee).c_str());
                }

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float milieu = depart.y + (haut - ImGui::GetTextLineHeight()) * 0.5f;
                /* EN OR TANT QU'ELLE N'EST PAS LUE, et seulement si elle est
                   arrivée : une lettre encore en route n'a rien à réclamer.
                   « en route » NE SE VOIT PLUS depuis le 06/09/2026 au soir (le
                   serveur ne pousse que des arrivées) ; le rendu reste, parce
                   que la colonne se lit toujours et qu'un 0 venu d'ailleurs doit
                   se montrer pour ce qu'il est, jamais se déguiser en « à lire ». */
                const bool aLire = m.arriveeFaite && !m.lue;
                const ImU32 couleur = aLire ? Theme::GoldCol()
                                            : Theme::Chrome(m.arriveeFaite ? 0.80f : 0.50f);
                dl->AddText(ImVec2(depart.x + 8.0f * a_S, milieu), couleur, Affiche(m.nom, m.matricule));
                const char* etat = m.arriveeFaite ? (m.lue ? "lue" : "arrivée") : "en route";
                const float l = ImGui::CalcTextSize(etat).x;
                dl->AddText(ImVec2(depart.x + largeur - l - 8.0f * a_S, milieu),
                    Theme::Chrome(m.arriveeFaite ? 0.55f : 0.40f), etat);
                dl->AddLine(ImVec2(depart.x, depart.y + haut), ImVec2(depart.x + largeur, depart.y + haut),
                    OrSombre(0.18f), 1.0f);
                if (elu) {
                    dl->AddRectFilled(ImVec2(depart.x, depart.y), ImVec2(depart.x + 2.0f * a_S, depart.y + haut),
                        Theme::GoldCol());
                }
                if (clic) {
                    /* Ouvrir une AUTRE lettre remet à zéro la demande de
                       lecture : revenir sur celle-ci pourra la redemander si le
                       geste s'était perdu. */
                    if (g_lettre != m.id) g_lireDemande = 0;
                    g_lettre = m.id;
                }
                ImGui::PopID();
            }
            RetirerStyleListe();
            ImGui::EndChild();
        }

        void ColonneGauche(float a_S)
        {
            /* LES DEUX BLOCS SE PARTAGENT LA COLONNE : le carnet en haut, le
               courrier en bas, chacun dans son enfant défilant. Sans ce partage
               le carnet d'un joueur bavard pousserait les lettres hors de
               l'écran — et c'est pour les lettres qu'on est venu. Une FRACTION
               de la colonne, jamais un reste calculé : le haut porte un titre,
               une loupe et un champ, et un reste devient négatif dès que la
               fenêtre est petite. */
            const float hautHaut = ImGui::GetContentRegionAvail().y * 0.52f;

            TitreBloc("Carnet");
            ImGui::BeginChild("##vk_missives_gauche_haut", ImVec2(0.0f, hautHaut), ImGuiChildFlags_None);
            BlocCarnet(a_S);
            ImGui::EndChild();

            FiletTravers(a_S);

            ImGui::BeginChild("##vk_missives_gauche_bas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
            BlocRecues(a_S);
            ImGui::EndChild();
        }

        // ── la colonne de droite : la lettre, ou la composition ───────────

        /** La lettre choisie. Le texte n'est là que si le serveur l'a fait
         *  descendre — et depuis le 06/09/2026 au soir, une lettre qui est ICI
         *  est une lettre qu'on a le droit de lire : le serveur ne pousse plus
         *  les autres.
         *
         *  D'OÙ LE PAVÉ QUI A DISPARU. On affichait, quand `ici` était faux,
         *  « Il faut être dans une ville ou un village pour lire son courrier. »
         *  à la place du texte. Il est devenu INATTEIGNABLE, et surtout il se
         *  contredirait : `ici` et la liste des reçues viennent du MÊME plateau,
         *  donc une lettre sous les yeux prouve qu'on est en ville. Le dehors se
         *  dit maintenant là où il est vrai — la liste vide de `BlocRecues`. */
        void BlocLettre(const Recue& a_m, float a_S)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
            ImGui::Text("De %s", Affiche(a_m.nom, a_m.matricule));
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
            if (a_m.arriveeFaite) ImGui::Text("arrivée le %s", HeureLocale(a_m.arrivee).c_str());
            else ImGui::Text("en route — arrivée vers %s", HeureLocale(a_m.arrivee).c_str());
            ImGui::PopStyleColor();

            FiletTravers(a_S);

            /* Le pied de la colonne : les deux boutons. On leur réserve leur
               place AVANT le texte, sinon une longue lettre les pousserait
               dehors. */
            const float hautBoutons = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
            ImGui::BeginChild("##vk_missives_lettre", ImVec2(0.0f, -hautBoutons), ImGuiChildFlags_None);
            /* « PAS ENCORE ARRIVÉE » NE SE VOIT PLUS non plus, et il reste pour
               la même raison que le libellé « en route » de la liste : la
               colonne d'arrivée se lit toujours, et un 0 doit se dire. Ce
               pavé-là, lui, ne contredit RIEN — d'où la différence avec celui du
               lieu, qui est parti. */
            if (!a_m.arriveeFaite) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.60f));
                ImGui::TextWrapped("%s", "Cette missive n'est pas encore arrivée. Laisse au courrier le temps de faire la route.");
                ImGui::PopStyleColor();
            } else if (a_m.texte.empty()) {
                /* Le serveur nous croit en droit de lire, et pourtant rien
                   n'est venu : on ne devine pas une lettre, on le dit. */
                ImGui::TextDisabled("Cette missive est vide.");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.92f));
                ImGui::TextWrapped("%s", a_m.texte.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();

            const float haut = ImGui::GetFrameHeight();
            const float large = ImGui::CalcTextSize("Répondre").x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f * a_S;
            ImGui::PushStyleColor(ImGuiCol_Button, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Voile(0.16f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.85f));
            /* RÉPONDRE : l'expéditeur devient le destinataire et l'on repasse à
               la composition.
               ET IL N'EST PAS AU CARNET « PAR CONSTRUCTION » — on l'a cru une
               passe entière, et c'était faux. Le carnet est l'UNION des deux
               sens des présentations, et le second sens ne s'écrit que depuis
               le 06/09/2026 : qui s'est présenté avant ce jour-là n'a laissé
               de trace que chez celui qui a APPRIS son nom. Il peut donc
               écrire à quelqu'un qui, lui, ne l'a pas au carnet. Sans ce
               grisage, le geste le plus naturel de l'écran menait à une
               composition qui refusait d'envoyer sans jamais dire pourquoi —
               et le plateau suivant effaçait le choix en silence. */
            bool auCarnet = false;
            for (const auto& p : g_carnet) {
                if (p.personnageId == a_m.personnageId) auCarnet = true;
            }
            ImGui::BeginDisabled(!auCarnet);
            if (Sfx::Button("Répondre##vk_missives_repondre", ImVec2(large, haut))) {
                g_destinataire = a_m.personnageId;
                g_lettre = 0;
            }
            ImGui::EndDisabled();
            /* LE SURVOL D'UN BOUTON GRISÉ COMPTE ICI, comme pour « Envoyer » :
               le joueur doit apprendre la règle, pas buter dessus. */
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !auCarnet) {
                ImGui::SetTooltip("%s",
                    "cette personne n'est pas dans ton carnet — présente-toi à elle pour pouvoir lui écrire");
            }
            ImGui::SameLine(0.0f, 8.0f * a_S);
            if (Sfx::Button("Écrire##vk_missives_ecrire", ImVec2(large, haut))) {
                g_lettre = 0;
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
        }

        /** Les lettres parties : à qui, et où elles en sont. Le contrat les
         *  fait descendre, et c'est la seule façon de savoir si la sienne est
         *  arrivée — son texte, lui, ne redescend jamais : on l'a écrit.
         *
         *  ELLES SE VOIENT DE PARTOUT, et c'est voulu : le lieu ne commande que
         *  la LECTURE du courrier reçu. Depuis le 06/09/2026 au soir les reçues
         *  disparaissent de l'écran hors d'un lieu habité ; celles-ci restent —
         *  c'est SA lettre, elle n'a pas à se cacher de lui. */
        void BlocEnvoyees(float a_S)
        {
            TitreBloc("Envoyées");
            if (g_envoyees.empty()) {
                ImGui::TextDisabled("Aucune missive envoyée.");
                return;
            }
            const float ligne = ImGui::GetTextLineHeightWithSpacing();
            ImGui::BeginChild("##vk_missives_envoyees",
                ImVec2(0.0f, ligne * static_cast<float>(kEnvoyeesVisibles)), ImGuiChildFlags_None);
            for (const auto& m : g_envoyees) {
                const bool arrivee = EnvoiArrive(m);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(arrivee ? 0.80f : 0.55f));
                ImGui::TextUnformatted(Affiche(m.nom, m.matricule));
                ImGui::PopStyleColor();
                const char* etat = m.lue ? "lue" : (arrivee ? "arrivée" : "en route");
                const float l = ImGui::CalcTextSize(etat).x;
                ImGui::SameLine((std::max)(ImGui::GetContentRegionAvail().x - l - 4.0f * a_S, 0.0f), 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(m.lue ? 0.65f : 0.45f));
                ImGui::TextUnformatted(etat);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s %s", arrivee ? "arrivée le" : "arrivera le",
                        HeureLocale(m.arrivee).c_str());
                }
            }
            ImGui::EndChild();
        }

        /** La composition : à qui, le texte, le compteur, « Envoyer ». */
        void BlocComposition(float a_S)
        {
            const Correspondant* cible = nullptr;
            for (const auto& p : g_carnet) {
                if (p.personnageId == g_destinataire) cible = &p;
            }

            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.70f));
            ImGui::TextUnformatted("À");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0f, 10.0f * a_S);
            if (cible == nullptr) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
                ImGui::TextUnformatted("choisis quelqu'un dans ton carnet");
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
                ImGui::TextUnformatted(Affiche(cible->nom, cible->matricule));
                ImGui::PopStyleColor();
            }

            FiletTravers(a_S);

            const int caracteres = Caracteres(g_texte);
            const bool tropLong = caracteres > kTexteMax;

            /* La saisie prend tout ce qui reste, moins le pied (compteur,
               bouton) et le bloc des envoyées. On réserve leur place AVANT :
               un champ qui se mesurerait sur ce qui reste APRÈS les aurait
               poussés dehors dès la première ligne de trop. */
            const float ligne = ImGui::GetTextLineHeightWithSpacing();
            const float hautPied = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 3.0f +
                                   ligne * static_cast<float>(kEnvoyeesVisibles + 1);
            const float hautSaisie = (std::max)(ImGui::GetContentRegionAvail().y - hautPied, ligne * 3.0f);

            ImGui::PushStyleColor(ImGuiCol_FrameBg, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Voile(0.08f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Voile(0.08f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.92f));
            /* Les retours à la ligne SURVIVENT jusqu'au bout — le champ est
               multiligne, le pont les échappe, le registre les garde. Une
               lettre a des paragraphes. */
            /* PAS DE TABULATION DANS LA SAISIE (le défaut d'ImGui) : le pont la
               changerait en espace, et le joueur verrait son retrait
               disparaître entre l'écriture et la lecture. */
            ImGui::InputTextMultiline("##vk_missives_texte", g_texte, sizeof(g_texte),
                ImVec2(-1.0f, hautSaisie));
            ImGui::PopStyleColor(4);

            // Le compteur, puis le bouton, sur la même ligne.
            char compteur[32];
            std::snprintf(compteur, sizeof(compteur), "%d / %d", caracteres, kTexteMax);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, tropLong ? RougeSombre() : Theme::Chrome(0.45f));
            ImGui::TextUnformatted(compteur);
            ImGui::PopStyleColor();

            const float haut = ImGui::GetFrameHeight();
            const float large = ImGui::CalcTextSize("Envoyer").x + ImGui::GetStyle().FramePadding.x * 2.0f + 16.0f * a_S;
            ImGui::SameLine((std::max)(ImGui::GetContentRegionAvail().x - large, 0.0f), 0.0f);

            /* GRISÉ TANT QU'IL MANQUE UN DESTINATAIRE OU UN TEXTE — et tant que
               le texte dépasse la borne du serveur, qui le refuserait pour la
               forme. Ceci ne fait que griser un bouton : le serveur rejuge
               tout, et c'est sa phrase que le joueur lit ensuite.
               `g_ici` N'EST PAS DANS CETTE CONDITION, ET IL NE DOIT PAS Y
               ENTRER. Le propriétaire l'a tranché le 06/09/2026 au soir :
               « l'envoi doit se faire de n'importe où, c'est uniquement la
               réception qui se fait en ville ». On écrit d'une grotte, d'un
               campement, du fond d'une mine ; le `poster` de relaisMissives.ts
               ne refuse que sur la demande illisible, soi-même, le hors-carnet,
               le texte et le registre — aucune garde de lieu, et c'est LU, pas
               supposé. Ajouter la symétrie ici paraîtra naturel : ce serait un
               défaut. */
            const bool peutEnvoyer = cible != nullptr && caracteres > 0 && !tropLong;
            ImGui::PushStyleColor(ImGuiCol_Button, Voile(0.04f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Voile(0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Voile(0.16f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::BeginDisabled(!peutEnvoyer);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.85f));
            if (Sfx::Button("Envoyer##vk_missives_envoyer", ImVec2(large, haut))) {
                EcrireGeste("poster", std::to_string(g_destinataire) + "\t" + Echapper(g_texte));
                /* Le champ se vide tout de suite : le serveur répondra par un
                   plateau qui dit le délai, et une lettre qu'on renverrait d'un
                   second clic partirait deux fois. */
                g_texte[0] = '\0';
            }
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            /* LE SURVOL D'UN BOUTON GRISÉ COMPTE ICI, et c'est tout l'intérêt :
               le joueur doit apprendre POURQUOI il ne peut pas envoyer. Sans le
               drapeau, ImGui ne rapporte aucun survol sur un item désactivé. */
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !peutEnvoyer) {
                ImGui::SetTooltip("%s", cible == nullptr ? "choisis d'abord quelqu'un dans ton carnet"
                                      : tropLong         ? "trop long — sept cents caractères au plus"
                                                         : "écris quelque chose");
            }

            /* ET IL LE DIT SANS QU'ON AIT À SURVOLER — 07/09/2026, après un
               essai en jeu du propriétaire : « en cliquant sur envoyer rien ne
               se faisait ». Rien ne se faisait, en effet : le bouton était
               grisé, et un bouton grisé est MUET. L'infobulle ci-dessus ne se
               découvre que si l'on soupçonne déjà qu'il y a quelque chose à
               découvrir ; un manque se dit à l'écran, sous le bouton, ou il ne
               se dit pas.

               MESURÉ CE JOUR-LÀ, et c'est ce qui fixe le texte : le fichier du
               pont ne portait AUCUN geste « poster » à l'heure de l'essai — le
               clic n'avait jamais quitté l'écran. Ni le serveur ni cet écran
               n'ont de garde de LIEU sur l'envoi (on écrit de partout) : il
               manquait un destinataire, un texte, ou les deux.

               ELLE SE TAIT SUR UNE FEUILLE VIERGE que personne n'a commencée :
               un reproche en rouge y serait du bruit. Elle parle dès qu'il y a
               une intention — un nom choisi, ou une lettre commencée. */
            if (!peutEnvoyer && (cible != nullptr || caracteres > 0)) {
                ImGui::PushStyleColor(ImGuiCol_Text, RougeSombre());
                ImGui::TextWrapped("%s", cible == nullptr
                        ? "Choisis d'abord quelqu'un dans ton carnet, à gauche."
                        : tropLong ? "Ta lettre dépasse sept cents caractères."
                                   : "Écris ta lettre avant de l'envoyer.");
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            BlocEnvoyees(a_S);
        }

        void ColonneDroite(float a_S)
        {
            const Recue* lettre = nullptr;
            for (const auto& m : g_recues) {
                if (m.id == g_lettre) lettre = &m;
            }
            if (lettre != nullptr) BlocLettre(*lettre, a_S);
            else BlocComposition(a_S);
        }
    }

    // ── l'interface publique ──────────────────────────────────────────────

    void Initialiser()
    {
        if (std::FILE* f = std::fopen(kCheminGestes, "w")) {
            std::fclose(f);
            g_pret = true;
            SKSE::log::info("[MISSIVES] pont pret ({})", kCheminGestes);
        } else {
            SKSE::log::warn("[MISSIVES] impossible d'ouvrir {} — le panneau restera sourd", kCheminGestes);
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

        /* LA LECTURE SE RÉCLAME D'ICI, PAS DU CLIC : elle se juge sur l'ÉTAT —
           une lettre choisie, arrivée, pas encore lue — et jamais sur un
           événement, dont on ne saurait pas s'il a porté. Le clic ne fait que
           poser `g_lettre` ; cette garde-ci, qui tourne à chaque trame, en tire
           le geste une fois et une seule.
           `g_ici` RESTE DANS LA CONDITION alors qu'il est désormais impliqué :
           depuis le 06/09/2026 au soir, une lettre reçue n'entre dans
           `g_recues` que si elle est lisible, donc `g_lettre != 0` dit déjà
           qu'on est en ville. Redire le fait ne coûte rien et ne ment pas.
           LA VEILLE DU SERVEUR NE DÉBLOQUE PLUS UNE LETTRE OUVERTE : ELLE LA
           FAIT APPARAÎTRE. `relaisMissives.veiller()` regarde toutes les cinq
           secondes les panneaux OUVERTS et repousse un plateau sur DEUX motifs
           — la BASCULE du lieu (dedans ⇄ dehors) et l'ARRIVÉE d'une lettre
           quand on est en ville. En entrant, le courrier surgit dans la colonne
           de gauche ; en sortant, il s'en va, et le ménage de `LireEtat` lâche
           la lettre choisie ; et l'heure qui passe le fait surgir sans que
           personne n'ait bougé — sans ce second motif, l'écran affirmait
           « Aucune missive. » en pleine ville sur une lettre parfaitement
           lisible, et ne se démentait jamais. Avant cette veille,
           `ici` restait l'instantané du dernier geste et la boucle était fermée :
           le joueur chevauchait jusqu'à Blancherive sans qu'aucun geste ne
           parte, l'écran lui répondant en pleine ville qu'il faut être en ville.
           `g_lireDemande` retient l'id déjà réclamé : sans lui, la condition
           resterait vraie à chaque trame jusqu'à la réponse du serveur, et le
           fichier des gestes prendrait soixante lignes par seconde. */
        if (g_ouvert && g_ici && g_lettre != 0 && g_lettre != g_lireDemande) {
            for (const auto& m : g_recues) {
                if (m.id != g_lettre) continue;
                if (m.arriveeFaite && !m.lue) {
                    EcrireGeste("lire", std::to_string(m.id));
                    g_lireDemande = m.id;
                }
                break;
            }
        }

        /* Le chien de garde : la racine s'est refermée sans passer par notre
           couche Échap (Tab, un menu vanilla, un autre de nos écrans qui a la
           priorité au rendu) — après deux secondes sans une trame dessinée, on
           se ferme et on le DIT, sinon le serveur nous croit toujours devant le
           panneau. */
        if (g_ouvert && g_tic - g_dernierDessin > kTramesSansDessin) {
            SKSE::log::info("[MISSIVES] panneau plus dessine depuis {} trames : fermeture", g_tic - g_dernierDessin);
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

        /* UN PANNEAU CENTRÉ ET DISCRET — le goût du propriétaire (29/08) : pas
           de bordure pleine, des filets d'un pixel, le monde reste visible
           autour. DEUX COLONNES, donc plus large que le panneau
           d'appartenance ; et une HAUTEUR FIXE, contrairement à tous nos autres
           écrans : ceux-là s'ajustent à leur contenu (AlwaysAutoResize), une
           lettre de sept cents caractères ferait grandir celui-ci jusqu'à
           dépasser l'écran, et il changerait de taille à chaque lettre ouverte.
           Les bornes suivent l'échelle (S vaut 0,75 en 1080p, 2 en 4K). */
        const float largeur = std::clamp(io.DisplaySize.x * 0.46f, 820.0f * S, 1120.0f * S);
        const float hauteur = std::clamp(io.DisplaySize.y * 0.64f, 420.0f * S, 820.0f * S);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.40f),
            ImGuiCond_Always, ImVec2(0.5f, 0.35f));
        ImGui::SetNextWindowSize(ImVec2(largeur, hauteur), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 9, 8, 236));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * S, 6.0f * S));
        ImGui::Begin("##vk_missives", nullptr,
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

        /* L'en-tête : le titre, et la règle du lieu dite d'entrée de jeu. Les
           deux phrases restent VRAIES après le 06/09/2026 au soir, et la
           seconde porte désormais double sens : elle annonce l'écriture permise
           de partout ET explique la colonne de gauche restée vide. */
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::GoldCol());
        ImGui::TextUnformatted("Missives");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Chrome(0.45f));
        ImGui::TextUnformatted(g_ici
            ? "Tu es dans un lieu habité : tu peux lire ton courrier."
            : "Tu peux écrire d'ici. On ne lit son courrier qu'en ville ou au village.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        /* LES DEUX COLONNES. Le pied (le message du serveur, ou la ligne
           d'aide) a sa place réservée d'avance : deux lignes, parce qu'un
           message peut se replier. */
        const float hautPied = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
        const float hautColonnes = (std::max)(ImGui::GetContentRegionAvail().y - hautPied,
            ImGui::GetTextLineHeightWithSpacing() * 4.0f);
        const float largeurGauche = ImGui::GetContentRegionAvail().x * 0.40f;

        ImGui::BeginChild("##vk_missives_gauche", ImVec2(largeurGauche, hautColonnes), ImGuiChildFlags_None);
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
        ImGui::BeginChild("##vk_missives_droite", ImVec2(0.0f, hautColonnes), ImGuiChildFlags_None);
        ColonneDroite(S);
        ImGui::EndChild();

        /* LE MESSAGE : ce que le serveur vient de répondre (« Missive confiée au
           courrier. Elle arrivera dans environ 7 minutes. »), sept secondes ;
           puis la ligne d'aide reprend sa place. Le nombre de l'exemple tient
           sous le plafond de dix minutes du 06/09 au soir — un exemple hors des
           bornes ferait lire le monde d'avant. */
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
