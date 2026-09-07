#include "ui/Riche.h"

#include "ui/Theme.h"
#include "ui/UIRoot.h"

#include <imgui.h>

#include <cfloat>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Voir Riche.h pour la grammaire, la règle d'adjacence, et pourquoi le rendu
// est une VUE. Ici : l'analyse (pure, éprouvable sans ImGui), la mise en page
// et le dessin (une seule fonction), et `Poser`, la seule qui touche au texte.

namespace FUI::Riche
{
    namespace
    {
        // ── LES STYLES EN LIGNE, EN BITS ──────────────────────────────────
        // Un masque plutôt qu'une pile : les quatre styles sont INDÉPENDANTS,
        // et l'ordre dans lequel on les referme n'a donc aucune importance.
        // « *a __b* c__ » donne « a » penché, « b » penché et souligné, « c »
        // souligné — un croisement que la pile aurait refusé, et qui ne coûte
        // rien à personne.
        enum Style : unsigned {
            kAucun    = 0u,
            kGras     = 1u << 0,
            kItalique = 1u << 1,
            kSouligne = 1u << 2,
            kBarre    = 1u << 3
        };

        /** Un morceau de LIGNE portant un seul masque de styles. Les indices
         *  sont des octets dans la ligne d'origine ; les marques, elles, ne
         *  sont dans aucun morceau — c'est ainsi qu'elles disparaissent du
         *  rendu sans jamais quitter le texte. */
        struct Morceau
        {
            std::size_t debut  = 0;
            std::size_t fin    = 0;
            unsigned    styles = kAucun;
        };

        /** Ce qu'une ligne EST, une fois son premier caractère lu. */
        enum class Genre { Vide, Titre1, Titre2, Puce, Citation, Filet, Paragraphe };

        // ── LES PETITES BRIQUES ───────────────────────────────────────────

        /** L'adjacence parle d'« espace » ; le tabulateur et le retour chariot
         *  en sont pour cette règle, même si le pont les fait disparaître : un
         *  fichier recousu à la main peut en contenir, et une marque ne doit
         *  pas s'ouvrir sur du vide parce qu'un octet exotique a traversé. */
        inline bool EstBlanc(char a_c)
        {
            return a_c == ' ' || a_c == '\t' || a_c == '\r' || a_c == '\n';
        }

        /** Le blanc du REPLI — celui qui sépare deux MOTS sur une ligne, et lui
         *  seul. `EstBlanc` en compte deux de plus pour l'adjacence des marques
         *  (\r et \n) ; ici ils ne peuvent pas se présenter, une ligne étant par
         *  définition ce qui tient entre deux sauts. Une seule définition, parce
         *  que le repli et la mesure du mot collé DOIVENT couper aux mêmes
         *  octets : c'est tout le sujet de la correction du 07/09. */
        inline bool BlancDeMot(char a_c)
        {
            return a_c == ' ' || a_c == '\t';
        }

        /** Un octet de CONTINUATION UTF-8 (10xxxxxx) : il ne commence pas un
         *  caractère, donc aucun indice ne doit jamais s'arrêter dessus. */
        inline bool Continuation(char a_c)
        {
            return (static_cast<unsigned char>(a_c) & 0xC0) == 0x80;
        }

        /** Combien de CARACTÈRES (points de code), pas d'octets : « é » en
         *  pèse un, et la borne des 2 000 doit valoir pareil pour qui écrit
         *  avec des accents. Même comptage que `Caracteres` dans Notes.cpp —
         *  les deux comptent la MÊME chose, sans quoi la borne refuserait ici
         *  ce que la frappe accepte là-bas. */
        int Caracteres(const std::string& a_s)
        {
            int n = 0;
            for (const char c : a_s) {
                if (!Continuation(c)) ++n;
            }
            return n;
        }

        /** IM_ROUND vit dans imgui_internal.h, que ce module ne tire pas : une
         *  taille de police n'est pas une raison d'ouvrir la porte des
         *  entrailles d'ImGui. Même formule, deux lignes. */
        inline float Arrondi(float a_v)
        {
            return static_cast<float>(static_cast<int>(a_v + 0.5f));
        }

        /** Le marqueur de chaque marque en ligne, ou nullptr pour un bloc.
         *  TOUS sont une suite d'un SEUL caractère répété : `MarqueurIci` s'en
         *  sert pour vérifier qu'une suite ne déborde pas. */
        const char* MarqueurDe(Marque a_m)
        {
            switch (a_m) {
                case Marque::Gras:     return "**";
                case Marque::Italique: return "*";
                case Marque::Souligne: return "__";
                case Marque::Barre:    return "~~";
                default:               return nullptr;
            }
        }

        // ── L'ANALYSE EN LIGNE ────────────────────────────────────────────

        /** Quel marqueur commence à l'octet `a_i`, et sur combien d'octets.
         *  LA MARQUE LONGUE PASSE AVANT LA COURTE : sans cela « **gras** »
         *  s'ouvrirait en italique sur sa première étoile et le second astérisque
         *  deviendrait le premier caractère du mot. */
        unsigned MarqueA(const char* a_s, std::size_t a_i, std::size_t a_n,
                         std::size_t& a_longueur)
        {
            const char c = a_s[a_i];
            if (c == '*') {
                if (a_i + 1 < a_n && a_s[a_i + 1] == '*') { a_longueur = 2; return kGras; }
                a_longueur = 1;
                return kItalique;
            }
            if (c == '_' && a_i + 1 < a_n && a_s[a_i + 1] == '_') { a_longueur = 2; return kSouligne; }
            if (c == '~' && a_i + 1 < a_n && a_s[a_i + 1] == '~') { a_longueur = 2; return kBarre; }
            a_longueur = 0;
            return kAucun;
        }

        /** La troisième règle d'adjacence : une ouvrante sans fermante SUR LA
         *  MÊME LIGNE n'est pas une ouvrante. On regarde donc devant avant
         *  d'ouvrir quoi que ce soit. Le balayage emploie la MÊME tokenisation
         *  que l'analyse (longue avant courte), sans quoi les deux passes
         *  seraient en désaccord sur ce qu'est une étoile.
         *  Coût quadratique dans le pire cas ; une page pèse 2 000 caractères
         *  répartis sur des lignes courtes, c'est du bruit. */
        bool FermanteExiste(const char* a_s, std::size_t a_n, std::size_t a_depart,
                            unsigned a_style)
        {
            std::size_t j = a_depart;
            while (j < a_n) {
                std::size_t longueur = 0;
                const unsigned style = MarqueA(a_s, j, a_n, longueur);
                if (style == kAucun) { ++j; continue; }
                if (style == a_style && j > 0 && !EstBlanc(a_s[j - 1])) return true;
                j += longueur;
            }
            return false;
        }

        /** Découpe UNE ligne en morceaux stylés. Les marques reconnues
         *  disparaissent (elles tombent entre deux morceaux) ; celles que
         *  l'adjacence refuse restent DANS le texte, à leur place, sans qu'un
         *  seul octet bouge. */
        void AnalyserLigne(const char* a_s, std::size_t a_n, std::vector<Morceau>& a_sortie)
        {
            a_sortie.clear();
            unsigned    ouverts = kAucun;
            std::size_t debut   = 0;   // début du morceau en cours, en octets
            std::size_t i       = 0;

            while (i < a_n) {
                std::size_t longueur = 0;
                const unsigned style = MarqueA(a_s, i, a_n, longueur);
                if (style == kAucun) { ++i; continue; }

                const bool fermante = (i > 0) && !EstBlanc(a_s[i - 1]);
                const bool ouvrante = (i + longueur < a_n) && !EstBlanc(a_s[i + longueur]);

                if ((ouverts & style) != 0u && fermante) {
                    if (i > debut) a_sortie.push_back(Morceau{ debut, i, ouverts });
                    ouverts &= ~style;
                    i += longueur;
                    debut = i;
                    continue;
                }
                if ((ouverts & style) == 0u && ouvrante &&
                    FermanteExiste(a_s, a_n, i + longueur, style)) {
                    if (i > debut) a_sortie.push_back(Morceau{ debut, i, ouverts });
                    ouverts |= style;
                    i += longueur;
                    debut = i;
                    continue;
                }

                /* Ni ouvrante ni fermante : les octets du marqueur sont du
                   texte comme les autres et RESTENT dans le morceau en cours.
                   On saute par-dessus en entier — reprendre à i+1 ferait
                   examiner la seconde étoile d'un « ** » littéral comme une
                   italique, et « 2 ** 3 » se mettrait à pencher. */
                i += longueur;
            }
            if (a_n > debut) a_sortie.push_back(Morceau{ debut, a_n, ouverts });
        }

        // ── L'ANALYSE EN BLOC ─────────────────────────────────────────────

        /** Saute tous les blancs à partir de `a_i`. Un joueur qui écrit
         *  « #   Titre » ne veut pas trois espaces devant son titre. */
        std::size_t SauterBlancs(const char* a_s, std::size_t a_n, std::size_t a_i)
        {
            while (a_i < a_n && EstBlanc(a_s[a_i])) ++a_i;
            return a_i;
        }

        /** Ce qu'est la ligne, et où commence son contenu.
         *  LES MARQUES DE BLOC SE LISENT EN TÊTE DE LIGNE, à l'octet zéro et
         *  nulle part ailleurs : une marque qu'on accepterait après quelques
         *  espaces ferait d'un texte indenté à la main un titre involontaire,
         *  et la règle deviendrait impossible à énoncer en une phrase dans
         *  l'aide de l'écran. */
        Genre GenreDeLigne(const char* a_s, std::size_t a_n, std::size_t& a_debutContenu)
        {
            a_debutContenu = 0;
            if (SauterBlancs(a_s, a_n, 0) == a_n) return Genre::Vide;

            if (a_s[0] == '-') {
                /* Le filet : au moins TROIS tirets et plus rien d'autre. Deux
                   tirets ne suffisent pas — « -- » ouvre trop de portes à un
                   trait involontaire au milieu d'une phrase coupée. */
                std::size_t t = 0;
                while (t < a_n && a_s[t] == '-') ++t;
                if (t >= 3 && SauterBlancs(a_s, a_n, t) == a_n) return Genre::Filet;
            }
            if (a_s[0] == '#') {
                std::size_t d = 0;
                while (d < a_n && a_s[d] == '#') ++d;
                a_debutContenu = SauterBlancs(a_s, a_n, d);
                /* Deux niveaux, pas davantage : au-delà on retombe sur le
                   sous-titre plutôt que d'inventer une hiérarchie qu'aucun
                   bouton de la barre ne sait poser. */
                return d == 1 ? Genre::Titre1 : Genre::Titre2;
            }
            if (a_s[0] == '-' && (a_n == 1 || EstBlanc(a_s[1]))) {
                a_debutContenu = SauterBlancs(a_s, a_n, 1);
                return Genre::Puce;
            }
            if (a_s[0] == '>') {
                a_debutContenu = SauterBlancs(a_s, a_n, 1);
                return Genre::Citation;
            }
            return Genre::Paragraphe;
        }

        // ── LA MISE EN PAGE ET LE DESSIN — UNE SEULE FONCTION ─────────────
        //
        // Deux passes séparées divergeraient : la hauteur réservée par la
        // première ne correspondrait plus à ce que la seconde peint, et le
        // décalage ne se verrait qu'en bas d'une page longue, chez un joueur.
        // Une seule fonction, un booléen `dessiner` : MESURER, c'est PEINDRE
        // sans encre. Aujourd'hui ce booléen sert à sauter l'encre des blocs
        // déjà passés sous le bord bas du cadre (une page de trente lignes
        // dans un cadre qui en montre huit), et la hauteur, elle, continue de
        // s'accumuler — c'est elle qui donne son ascenseur au cadre.

        struct Plume
        {
            ImDrawList* dl       = nullptr;
            bool        dessiner = true;
            ImVec2      origine{ 0.0f, 0.0f };   // coin haut-gauche du texte, en pixels écran
            float       largeur  = 0.0f;
            float       S        = 1.0f;
            ImU32       encre    = 0u;
            float       corps    = 0.0f;   // les TROIS tailles, et pas une de plus
            float       titre1   = 0.0f;
            float       titre2   = 0.0f;
            float       y        = 0.0f;   // ce qui a été consommé, depuis origine.y
            float       basCadre = 0.0f;   // au-delà, on ne peint plus
        };

        /** Baisse l'alpha d'une couleur sans toucher à sa teinte : la citation
         *  et les filets se tiennent en retrait de l'encre du corps, et il
         *  aurait fallu autant de jetons de thème que de nuances pour dire
         *  cela avec des couleurs nommées. */
        ImU32 Attenuer(ImU32 a_col, float a_facteur)
        {
            const unsigned a = (a_col >> IM_COL32_A_SHIFT) & 0xFFu;
            const unsigned n = static_cast<unsigned>(static_cast<float>(a) * a_facteur);
            return (a_col & ~IM_COL32_A_MASK) | ((n > 255u ? 255u : n) << IM_COL32_A_SHIFT);
        }

        /** La police d'un morceau. Elle prend le TEXTE parce que les faces
         *  penchées et grasses sont cuites avec le latin seul : un point de
         *  code qu'elles ne savent pas épeler renvoie toute la chaîne à la
         *  police principale — pas penchée, mais lisible, ce qui est le bon
         *  sens du compromis. */
        ImFont* PolicePour(unsigned a_styles, const char* a_texte)
        {
            ImFont* f = nullptr;
            const bool gras = (a_styles & kGras) != 0u;
            const bool ital = (a_styles & kItalique) != 0u;
            if (gras && ital)  f = UIRoot::BoldItalicFont(a_texte);
            else if (gras)     f = UIRoot::BoldFont(a_texte);
            else if (ital)     f = UIRoot::ItalicFont(a_texte);
            /* Le corps prend la police COURANTE, nommée plutôt que laissée à
               nullptr : `CalcTextSizeA` est une méthode d'ImFont, et mesurer
               avec une police et peindre avec une autre est exactement le
               genre d'écart qui ne se voit qu'à la marge droite. */
            return f != nullptr ? f : ImGui::GetFont();
        }

        /** Mesure un fragment avec SA police, à une taille FINALE en pixels.
         *  `CalcTextSizeA` est la seule façon de mesurer avec autre chose que
         *  la police courante d'ImGui. */
        float Mesurer(ImFont* a_police, float a_taille, const char* a_d, const char* a_f)
        {
            return a_police->CalcTextSizeA(a_taille, FLT_MAX, 0.0f, a_d, a_f).x;
        }

        /** Pose un fragment déjà mesuré, et ses filets. `a_taille` est une
         *  taille FINALE (la même unité que `ImGui::GetFontSize()`), jamais
         *  une taille de base : la surcharge à huit arguments d'`AddText` ne
         *  réapplique aucun facteur global, contrairement à `PushFont`. */
        void Peindre(Plume& a_p, ImFont* a_police, float a_taille, float a_retrait,
                     float a_x, float a_largeurMot, const char* a_d, const char* a_f,
                     ImU32 a_couleur, unsigned a_styles)
        {
            if (!a_p.dessiner || a_p.dl == nullptr) return;
            const ImVec2 pos(a_p.origine.x + a_retrait + a_x, a_p.origine.y + a_p.y);
            a_p.dl->AddText(a_police, a_taille, pos, a_couleur, a_d, a_f);

            /* Souligné et barré sont des TRAITS, pas des glyphes : aucune de
               nos polices ne porte de caractère combinant, et un souligné
               approché avec des tirets sauterait à chaque repli. */
            const float ep = a_p.S > 1.0f ? a_p.S : 1.0f;
            if ((a_styles & kSouligne) != 0u) {
                const float y = pos.y + a_taille * 0.92f;
                a_p.dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + a_largeurMot, y), a_couleur, ep);
            }
            if ((a_styles & kBarre) != 0u) {
                const float y = pos.y + a_taille * 0.56f;
                a_p.dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + a_largeurMot, y), a_couleur, ep);
            }
        }

        /** Le morceau `a_s` est-il COLLÉ au morceau `a_p` — c'est-à-dire le même
         *  MOT, coupé en deux par une marque tombée en plein milieu ?
         *
         *  Trois conditions, et chacune répond à une erreur possible : le
         *  morceau d'avant ne doit pas finir sur un blanc (sinon le mot était
         *  fini avant la marque : « un **mot** ») ; les octets qui séparent les
         *  deux morceaux dans la ligne — les marqueurs, que l'analyse a mis
         *  hors des morceaux — ne doivent en contenir aucun ; et le morceau
         *  d'après ne doit pas en commencer un.
         *
         *  Cette fonction est PURE et sans ImGui, et c'est délibéré : c'est
         *  elle que l'auto-épreuve peut mordre, alors que la largeur en pixels
         *  demande une police cuite, donc une trame de jeu. */
        bool MorceauColle(const std::string& a_ligne, const Morceau& a_p, const Morceau& a_s)
        {
            if (a_p.fin <= a_p.debut || a_s.fin <= a_s.debut) return false;
            if (a_p.fin > a_ligne.size() || a_s.fin > a_ligne.size()) return false;
            if (a_s.debut < a_p.fin) return false;
            if (BlancDeMot(a_ligne[a_p.fin - 1])) return false;
            if (BlancDeMot(a_ligne[a_s.debut])) return false;
            for (std::size_t b = a_p.fin; b < a_s.debut; ++b) {
                if (BlancDeMot(a_ligne[b])) return false;
            }
            return true;
        }

        /** Les fragments COLLÉS à la fin du morceau `a_k` : la suite du même
         *  mot, dans les morceaux qui suivent, jusqu'au premier blanc.
         *
         *  POURQUOI CETTE FONCTION EXISTE (07/09, constat 5) : `PoserMorceaux`
         *  découpait les mots à l'intérieur de CHAQUE morceau et décidait le
         *  repli morceau par morceau. Un mot que l'analyse a coupé en deux
         *  parce qu'une marque tombe au milieu était donc traité comme deux
         *  mots, et le repli pouvait passer entre eux alors qu'aucune espace ne
         *  les sépare : la ligne finissait par « Bonjour » et la suivante
         *  commençait par « , ami ». Pire pour une marque posée au milieu d'un
         *  mot (« l'*or* ») : là, la coupure tombe avec la probabilité d'un mot
         *  ordinaire, pas celle d'une virgule.
         *
         *  On mesure donc le mot ENTIER avant de peindre son premier fragment :
         *  une fois le fragment de tête posé, il est trop tard pour replier. */
        void EtendueCollee(const std::string& a_ligne, const std::vector<Morceau>& a_morceaux,
                           std::size_t a_k, std::vector<Morceau>& a_sortie)
        {
            a_sortie.clear();
            for (std::size_t j = a_k + 1; j < a_morceaux.size(); ++j) {
                if (!MorceauColle(a_ligne, a_morceaux[j - 1], a_morceaux[j])) break;
                std::size_t e = a_morceaux[j].debut;
                while (e < a_morceaux[j].fin && !BlancDeMot(a_ligne[e])) ++e;
                a_sortie.push_back(Morceau{ a_morceaux[j].debut, e, a_morceaux[j].styles });
                /* Le mot s'arrête DANS ce morceau : plus rien ne lui est collé,
                   et le morceau suivant commencera un mot à lui. */
                if (e < a_morceaux[j].fin) break;
            }
        }

        /**
         * LE REPLI, MOT À MOT — le cœur du module.
         *
         * Chaque mot est mesuré avec la police de SON morceau : un mot gras
         * est plus large que le même mot maigre, et une mesure faite avec la
         * police courante mettrait la marge droite en dents de scie sur toute
         * page un peu grasse.
         *
         * `a_retrait` vaut pour la première ligne, `a_retraitSuite` pour les
         * suivantes — c'est le retrait SUSPENDU des puces : le texte d'une
         * puce qui se replie revient sous lui-même, pas sous sa puce.
         *
         * Les blancs ne sont posés qu'au MILIEU d'une ligne : ceux qui
         * tomberaient en tête d'un repli, ou qui déborderaient à droite, sont
         * simplement oubliés. Sans cela un souligné traînerait une queue dans
         * la marge, et une ligne repliée commencerait de travers.
         */
        void PoserMorceaux(Plume& a_p, const std::string& a_ligne,
                           const std::vector<Morceau>& a_morceaux,
                           float a_retrait, float a_retraitSuite,
                           float a_taille, unsigned a_forces, ImU32 a_couleur)
        {
            const float interligne = a_taille * 1.18f;
            float       retrait    = a_retrait;
            float       x          = 0.0f;
            bool        pose       = false;   // a-t-on posé quelque chose sur CETTE ligne ?

            const auto utile = [&]() {
                const float u = a_p.largeur - retrait;
                return u > 1.0f ? u : 1.0f;
            };
            const auto replier = [&]() {
                a_p.y += interligne;
                x       = 0.0f;
                retrait = a_retraitSuite;
                pose    = false;
            };

            /* Réemployé d'un mot à l'autre plutôt qu'alloué à chaque fois : un
               mot coupé par des marques tient dans une poignée de fragments, et
               la mise en page tourne à chaque trame. */
            std::vector<Morceau> collee;

            /* Boucle par INDICE, et non sur la valeur : mesurer le mot entier
               demande de regarder DEVANT, dans les morceaux qui suivent. */
            for (std::size_t k = 0; k < a_morceaux.size(); ++k) {
                const Morceau&    m      = a_morceaux[k];
                if (m.fin <= m.debut || m.fin > a_ligne.size()) continue;
                const unsigned    styles = m.styles | a_forces;
                const std::string texte  = a_ligne.substr(m.debut, m.fin - m.debut);
                ImFont*           police = PolicePour(styles, texte.c_str());

                const char* const p0 = texte.c_str();
                const char* const pn = p0 + texte.size();
                const char*       c  = p0;

                while (c < pn) {
                    // 1) la suite de blancs
                    const char* q = c;
                    while (q < pn && BlancDeMot(*q)) ++q;
                    if (q > c) {
                        const float w = Mesurer(police, a_taille, c, q);
                        if (pose && x + w <= utile()) {
                            Peindre(a_p, police, a_taille, retrait, x, w, c, q, a_couleur, styles);
                            x += w;
                        }
                        c = q;
                        continue;
                    }

                    // 2) le mot
                    q = c;
                    while (q < pn && !BlancDeMot(*q)) ++q;
                    float w = Mesurer(police, a_taille, c, q);

                    /* LE MOT ENTIER, PAS LE FRAGMENT. Quand le mot touche la
                       fin du morceau, il peut se poursuivre de l'autre côté
                       d'une marque, sans qu'aucune espace ne l'ait coupé : on
                       ajoute à la mesure ce qui lui est collé, chaque fragment
                       avec SA police — un « , » maigre et un « or » penché ne
                       pèsent pas la même chose. Décidé AVANT de peindre le
                       fragment de tête : après, la ligne est déjà engagée. */
                    float suite = 0.0f;
                    if (pose && q == pn) {
                        EtendueCollee(a_ligne, a_morceaux, k, collee);
                        for (const Morceau& s : collee) {
                            const std::string frag =
                                a_ligne.substr(s.debut, s.fin - s.debut);
                            suite += Mesurer(PolicePour(s.styles | a_forces, frag.c_str()),
                                             a_taille, frag.c_str(),
                                             frag.c_str() + frag.size());
                        }
                    }

                    if (pose && x + w + suite > utile()) replier();

                    if (w > utile()) {
                        /* Un mot plus long que la ligne entière — une adresse,
                           une suite de tirets. On le COUPE au caractère plutôt
                           que de le laisser sortir du cadre : `CalcTextSizeA`
                           avec une largeur maximale rend le reste à traiter,
                           toujours sur une frontière UTF-8. */
                        while (c < q) {
                            const float dispo = (a_p.largeur - retrait - x) > 1.0f
                                                    ? (a_p.largeur - retrait - x)
                                                    : 1.0f;
                            const char* reste = nullptr;
                            const ImVec2 dim = police->CalcTextSizeA(a_taille, dispo, 0.0f, c, q, &reste);
                            if (reste == nullptr || reste <= c) {
                                /* Même UN caractère ne tient pas (un cadre
                                   large de trois pixels). On en pose un
                                   quand même : sans cela la boucle ne
                                   finirait jamais, et le jeu se fige. */
                                reste = c + 1;
                                while (reste < q && Continuation(*reste)) ++reste;
                                const float ww = Mesurer(police, a_taille, c, reste);
                                Peindre(a_p, police, a_taille, retrait, x, ww, c, reste, a_couleur, styles);
                                x += ww;
                            } else {
                                Peindre(a_p, police, a_taille, retrait, x, dim.x, c, reste, a_couleur, styles);
                                x += dim.x;
                            }
                            c    = reste;
                            pose = true;
                            if (c < q) replier();
                        }
                        continue;
                    }

                    Peindre(a_p, police, a_taille, retrait, x, w, c, q, a_couleur, styles);
                    x += w;
                    pose = true;
                    c    = q;
                }
            }

            /* La dernière ligne — celle qu'aucun repli n'a comptée. Une ligne
               sans un seul morceau (« # » tout seul) en consomme une aussi :
               elle existe dans le texte, elle occupe une place à l'écran. */
            a_p.y += interligne;
        }

        // ── LES BLOCS ─────────────────────────────────────────────────────

        /** Le filet de séparation : un trait fin pleine largeur, coupé en son
         *  milieu par un petit losange plein. Tout est tracé — un losange pris
         *  dans une police serait un tofu sur le premier poste amputé. */
        void BlocFilet(Plume& a_p)
        {
            const float haut = a_p.corps * 0.90f;
            if (a_p.dessiner && a_p.dl != nullptr) {
                const float y      = a_p.origine.y + a_p.y + haut * 0.5f;
                const float x0     = a_p.origine.x;
                const float x1     = a_p.origine.x + a_p.largeur;
                const float milieu = (x0 + x1) * 0.5f;
                const float r      = 3.5f * a_p.S;
                const float ecart  = r + 4.0f * a_p.S;
                const ImU32 or_    = Attenuer(Theme::GoldCol(), 0.55f);
                const float ep     = a_p.S > 1.0f ? a_p.S : 1.0f;
                if (milieu - ecart > x0) a_p.dl->AddLine(ImVec2(x0, y), ImVec2(milieu - ecart, y), or_, ep);
                if (x1 > milieu + ecart) a_p.dl->AddLine(ImVec2(milieu + ecart, y), ImVec2(x1, y), or_, ep);
                a_p.dl->AddNgonFilled(ImVec2(milieu, y), r, or_, 4);
            }
            a_p.y += haut;
        }

        /** Un bloc de texte, quel qu'il soit. Le retrait, la taille, la
         *  couleur et les styles imposés font toute la différence entre un
         *  titre et une citation ; le repli, lui, est le même pour tous. */
        void BlocTexte(Plume& a_p, Genre a_genre, const std::string& a_contenu,
                       std::vector<Morceau>& a_morceaux)
        {
            AnalyserLigne(a_contenu.c_str(), a_contenu.size(), a_morceaux);

            switch (a_genre) {
                case Genre::Titre1: {
                    /* Un titre respire au-dessus, sauf s'il ouvre la page :
                       une page qui commence par un blanc a l'air mal cadrée. */
                    if (a_p.y > 0.0f) a_p.y += a_p.corps * 0.35f;
                    const float yHaut = a_p.y;
                    PoserMorceaux(a_p, a_contenu, a_morceaux, 0.0f, 0.0f,
                                  a_p.titre1, kGras, Theme::GoldCol());
                    /* Le filet SOUS le titre se trace après coup, sur le même
                       drawlist : la hauteur du bloc n'est connue qu'une fois
                       le repli fait, et c'est précisément pour cela que la
                       mise en page et le dessin sont la même passe. */
                    if (a_p.dessiner && a_p.dl != nullptr && a_p.y > yHaut) {
                        const float y = a_p.origine.y + a_p.y - a_p.corps * 0.10f;
                        a_p.dl->AddLine(ImVec2(a_p.origine.x, y),
                                        ImVec2(a_p.origine.x + a_p.largeur, y),
                                        Attenuer(Theme::GoldCol(), 0.40f),
                                        a_p.S > 1.0f ? a_p.S : 1.0f);
                    }
                    a_p.y += a_p.corps * 0.30f;
                    break;
                }
                case Genre::Titre2: {
                    if (a_p.y > 0.0f) a_p.y += a_p.corps * 0.25f;
                    PoserMorceaux(a_p, a_contenu, a_morceaux, 0.0f, 0.0f,
                                  a_p.titre2, kGras, Theme::GoldCol());
                    a_p.y += a_p.corps * 0.15f;
                    break;
                }
                case Genre::Puce: {
                    const float retrait = 14.0f * a_p.S;
                    if (a_p.dessiner && a_p.dl != nullptr) {
                        /* Le disque, tracé à la main. Il se pose sur la
                           PREMIÈRE ligne du bloc, à mi-hauteur du corps. */
                        const ImVec2 centre(a_p.origine.x + retrait * 0.42f,
                                            a_p.origine.y + a_p.y + a_p.corps * 0.52f);
                        a_p.dl->AddCircleFilled(centre, 2.2f * a_p.S,
                                                Attenuer(a_p.encre, 0.75f), 0);
                    }
                    /* Retrait SUSPENDU : la suite revient sous le texte, pas
                       sous la puce — sinon une puce longue se relit comme
                       deux entrées de liste. */
                    PoserMorceaux(a_p, a_contenu, a_morceaux, retrait, retrait,
                                  a_p.corps, kAucun, a_p.encre);
                    break;
                }
                case Genre::Citation: {
                    const float retrait = 14.0f * a_p.S;
                    const float yHaut   = a_p.y;
                    /* La citation est penchée d'office : c'est la convention
                        des carnets, et elle reste lisible si la face penchée
                        manque (la police principale revient d'elle-même). */
                    PoserMorceaux(a_p, a_contenu, a_morceaux, retrait, retrait,
                                  a_p.corps, kItalique, Attenuer(a_p.encre, 0.85f));
                    if (a_p.dessiner && a_p.dl != nullptr && a_p.y > yHaut) {
                        const float x  = a_p.origine.x + 3.0f * a_p.S;
                        const float y0 = a_p.origine.y + yHaut + a_p.corps * 0.12f;
                        const float y1 = a_p.origine.y + a_p.y - a_p.corps * 0.12f;
                        a_p.dl->AddLine(ImVec2(x, y0), ImVec2(x, y1),
                                        Attenuer(Theme::GoldCol(), 0.50f), 2.0f * a_p.S);
                    }
                    break;
                }
                default:
                    PoserMorceaux(a_p, a_contenu, a_morceaux, 0.0f, 0.0f,
                                  a_p.corps, kAucun, a_p.encre);
                    break;
            }
        }

        /** Le texte entier, ligne par ligne. UNE ligne du texte = UN bloc :
         *  rien n'est recollé au voisin, parce qu'un joueur qui a appuyé sur
         *  Entrée a voulu une ligne — et que le carnet lui a déjà promis que
         *  la touche Entrée fait ce qu'elle dit. */
        void Composer(Plume& a_p, const char* a_texte)
        {
            if (a_texte == nullptr) return;

            std::vector<Morceau> morceaux;
            std::string          ligne;
            const char*          d    = a_texte;
            bool                 fini = false;

            while (!fini) {
                const char* f = d;
                while (*f != '\0' && *f != '\n') ++f;
                ligne.assign(d, static_cast<std::size_t>(f - d));
                /* « \r\n » est UN saut de ligne. Le pont n'en écrit pas, mais
                   un fichier recousu à la main le peut, et un retour chariot
                   traîné jusqu'ici se peindrait en boîte vide. */
                if (!ligne.empty() && ligne.back() == '\r') ligne.pop_back();

                /* Tout ce qui est déjà passé sous le bord bas du cadre ne
                   reçoit plus d'encre — la hauteur, elle, continue de compter,
                   sinon l'ascenseur du cadre mentirait sur la longueur de la
                   page. */
                if (a_p.dl != nullptr && a_p.origine.y + a_p.y > a_p.basCadre) {
                    a_p.dessiner = false;
                }

                std::size_t debutContenu = 0;
                const Genre g = GenreDeLigne(ligne.c_str(), ligne.size(), debutContenu);
                if (g == Genre::Vide) {
                    /* Une ligne vide sépare deux paragraphes : un blanc
                       franc, mais pas une ligne entière — deux paragraphes
                       séparés d'un interligne plein se lisent comme deux
                       pages. */
                    a_p.y += a_p.corps * 0.60f;
                } else if (g == Genre::Filet) {
                    BlocFilet(a_p);
                } else {
                    BlocTexte(a_p, g, ligne.substr(debutContenu), morceaux);
                }

                if (*f == '\0') fini = true;
                else            d = f + 1;
            }
        }

        // ── POSER : LA SEULE FONCTION QUI TOUCHE AU TEXTE DU JOUEUR ───────

        /** Le marqueur de `a_m` occupe EXACTEMENT [a_pos, a_pos+longueur) : les
         *  octets y sont les bons, ET la SUITE de caractères identiques qui les
         *  contient porte bien cette marque-là.
         *
         *  ON MESURE LA SUITE ENTIÈRE, JAMAIS LE SEUL VOISIN. La première
         *  écriture refusait le marqueur dès qu'un caractère identique le
         *  touchait. Cela protégeait bien un cas — retirer l'italique de
         *  « **gras** » ne doit pas manger une étoile de chaque paire — mais
         *  cela rendait IRRÉVERSIBLE un mot qui porte les deux marques : sur
         *  « ***mot*** », ni le gras ni l'italique ne se retiraient plus, et
         *  chaque appui empilait une paire de plus, invisible au rendu et
         *  pourtant comptée dans les 2 000 (07/09, constats 3 et 4).
         *
         *  La règle qui les sépare tient en une phrase : DEUX étoiles portent le
         *  gras, et l'italique n'est là que s'il en RESTE une impaire par-dessus
         *  les paires. Une suite de 1 est un italique seul, de 2 un gras seul,
         *  de 3 les deux, de 4 un gras redoublé. C'est aussi ce qui protège
         *  « **|gras** », curseur planté au milieu de l'ouvrante : la suite y
         *  vaut 2, elle est paire, l'italique n'y est pas et l'on n'y touche
         *  pas — la garde d'origine est CONSERVÉE, pas contournée.
         *
         *  RESTE ASSUMÉ : une page REÇUE d'un autre joueur peut déjà porter
         *  « ****mot**** » (suite de 4). On y voit le gras, pas l'italique, et
         *  le rendu de cette suite est « aucun style » — un texte qu'aucun de
         *  nos boutons ne sait plus produire, et dont le joueur voit les étoiles
         *  en clair dès le mode Écrire. */
        bool MarqueurIci(const std::string& a_t, int a_pos, Marque a_m)
        {
            const char* mk = MarqueurDe(a_m);
            if (mk == nullptr) return false;
            const int L = static_cast<int>(std::strlen(mk));
            const int n = static_cast<int>(a_t.size());
            if (a_pos < 0 || a_pos + L > n) return false;
            for (int k = 0; k < L; ++k) {
                if (a_t[static_cast<std::size_t>(a_pos + k)] != mk[0]) return false;
            }

            /* `__` et `~~` n'ont qu'UNE longueur : deux octets identiques posés
               là sont forcément les leurs, aucune ambiguïté de suite n'est
               possible. Seule l'étoile porte deux marques de longueurs
               différentes, et c'est d'elle seule qu'on mesure la suite. */
            if (mk[0] != '*') return true;
            /* Le gras occupe DEUX étoiles : dès qu'il y en a deux ici, elles
               sont les siennes, quelle que soit la longueur de la suite (une
               suite qui contient deux étoiles en compte au moins deux — le test
               « suite >= 2 » du constat est déjà acquis à ce point). */
            if (L == 2) return true;

            int gauche = 0;
            while (a_pos - gauche > 0 &&
                   a_t[static_cast<std::size_t>(a_pos - gauche - 1)] == '*') ++gauche;
            int droite = 0;
            while (a_pos + L + droite < n &&
                   a_t[static_cast<std::size_t>(a_pos + L + droite)] == '*') ++droite;
            return ((gauche + L + droite) % 2) == 1;
        }

        int DebutLigne(const std::string& a_t, int a_pos)
        {
            int i = a_pos;
            while (i > 0 && a_t[static_cast<std::size_t>(i - 1)] != '\n') --i;
            return i;
        }

        int FinLigne(const std::string& a_t, int a_pos)
        {
            const int n = static_cast<int>(a_t.size());
            int       i = a_pos;
            while (i < n && a_t[static_cast<std::size_t>(i)] != '\n') ++i;
            return i;
        }

        /** La ligne [a_deb, a_fin) est-elle un filet, et rien d'autre ? */
        bool LigneEstFilet(const std::string& a_t, int a_deb, int a_fin)
        {
            const std::string ligne = a_t.substr(static_cast<std::size_t>(a_deb),
                                                 static_cast<std::size_t>(a_fin - a_deb));
            std::size_t       dc    = 0;
            return GenreDeLigne(ligne.c_str(), ligne.size(), dc) == Genre::Filet;
        }

        /** Combien d'octets de préfixe de titre porte la ligne (dièses et
         *  blancs qui suivent), et de quel niveau. */
        int NiveauTitre(const std::string& a_t, int a_deb, int a_fin, int& a_octets)
        {
            a_octets = 0;
            int i = a_deb;
            while (i < a_fin && a_t[static_cast<std::size_t>(i)] == '#') ++i;
            const int dieses = i - a_deb;
            if (dieses == 0) return 0;
            while (i < a_fin && EstBlanc(a_t[static_cast<std::size_t>(i)])) ++i;
            a_octets = i - a_deb;
            return dieses == 1 ? 1 : 2;
        }

        /** Le préfixe d'une puce (« - ») ou d'une citation (« > »), et sa
         *  longueur en octets, blancs compris. */
        bool APrefixe(const std::string& a_t, int a_deb, int a_fin, char a_c, int& a_octets)
        {
            a_octets = 0;
            if (a_deb >= a_fin || a_t[static_cast<std::size_t>(a_deb)] != a_c) return false;
            if (a_c == '-' && LigneEstFilet(a_t, a_deb, a_fin)) return false;
            int i = a_deb + 1;
            while (i < a_fin && EstBlanc(a_t[static_cast<std::size_t>(i)])) ++i;
            a_octets = i - a_deb;
            return true;
        }

        /** Une édition en tête de ligne : `retire` octets remplacés par
         *  `ajoute`. Elles s'appliquent de la DERNIÈRE ligne vers la PREMIÈRE,
         *  pour que les positions des précédentes restent valables. */
        struct Edition
        {
            int         pos    = 0;
            int         retire = 0;
            std::string ajoute;
        };
    }

    // ── L'ENTRÉE PUBLIQUE DU DESSIN ───────────────────────────────────────

    void Dessiner(const char* a_texte, float a_largeur, float a_S, ImU32 a_encre)
    {
        if (a_texte == nullptr || *a_texte == '\0') return;

        Plume p;
        p.dl       = ImGui::GetWindowDrawList();
        p.dessiner = true;
        p.origine  = ImGui::GetCursorScreenPos();
        p.largeur  = a_largeur > 1.0f ? a_largeur : 1.0f;
        p.S        = a_S > 0.01f ? a_S : 1.0f;
        p.encre    = a_encre;
        /* LES TROIS TAILLES, calculées ICI et une seule fois. Chaque taille
           demandée cuit une variante de la police dans l'atlas d'ImGui 1.92 ;
           un jeu fini de tailles, arrondies au pixel, c'est un atlas qui se
           stabilise à la première page lue au lieu de grossir à chaque trame. */
        p.corps    = ImGui::GetFontSize();
        p.titre1   = Arrondi(p.corps * 1.25f);
        p.titre2   = Arrondi(p.corps * 1.10f);
        p.basCadre = p.dl != nullptr ? p.dl->GetClipRectMax().y : 0.0f;

        Composer(p, a_texte);

        /* Le curseur ImGui avance de la hauteur PEINTE, pas d'une hauteur
           calculée à part : c'est la même passe qui a rendu les deux. */
        ImGui::Dummy(ImVec2(p.largeur, p.y));
    }

    // ── POSER ─────────────────────────────────────────────────────────────

    Pose Poser(const std::string& a_texte, int a_debut, int a_fin, Marque a_marque, int a_borne)
    {
        const int taille = static_cast<int>(a_texte.size());

        /* ÉTAPE 1 — RENDRE LES INDICES HABITABLES. `Poser` reçoit ce que la
           sélection d'ImGui veut bien lui donner : elle peut être à l'envers
           (SelectionStart > SelectionEnd, c'est documenté), tomber hors du
           tampon si le texte a changé entre deux trames, et tomber AU MILIEU
           d'un caractère accentué — ImGui compte des octets. Tout cela est
           normalisé ici, une fois, et plus rien en dessous n'a le droit de
           supposer quoi que ce soit. */
        int d = a_debut;
        int f = a_fin;
        if (d > f) std::swap(d, f);
        d = d < 0 ? 0 : (d > taille ? taille : d);
        f = f < 0 ? 0 : (f > taille ? taille : f);
        /* On ÉLARGIT jusqu'aux frontières de caractères : reculer le début et
           avancer la fin ne peut jamais couper un « é » en deux, alors que
           tronquer le ferait à coup sûr. */
        while (d > 0 && Continuation(a_texte[static_cast<std::size_t>(d)])) --d;
        while (f < taille && Continuation(a_texte[static_cast<std::size_t>(f)])) ++f;

        const int nAvant = Caracteres(a_texte);

        /* LA BORNE. Le contrat du carnet la fait respecter À LA FRAPPE
           (BornerCaracteres, Notes.cpp) ; une insertion de marque, elle, ne
           passe pas par le rappel de saisie et pourrait la franchir en
           silence. Ici on refuse, et l'écran le dit en une ligne — plutôt
           qu'un enregistrement que le serveur rejetterait, c'est-à-dire une
           page perdue au moment où l'on referme.
           EXCEPTION : un résultat plus COURT que le texte de départ passe
           toujours. Sans elle, une page déjà trop longue (un texte reçu d'un
           autre joueur, une borne baissée un jour) ne pourrait même plus se
           faire RETIRER une marque. */
        const auto rendre = [&](std::string a_neuf, int a_d, int a_f) -> Pose {
            const int nApres = Caracteres(a_neuf);
            if (nApres > a_borne && nApres >= nAvant) return Pose{ false, a_texte, d, f };
            return Pose{ true, std::move(a_neuf), a_d, a_f };
        };

        const char* marqueur = MarqueurDe(a_marque);

        // ── LES MARQUES EN LIGNE ──────────────────────────────────────────
        if (marqueur != nullptr) {
            const int L = static_cast<int>(std::strlen(marqueur));

            /* Une marque en ligne NE FRANCHIT PAS un saut de ligne : une
               ouvrante sans fermante sur la même ligne serait rendue
               littérale (règle d'adjacence n° 3), donc en fabriquer une serait
               poser une marque qui ne marque rien. On s'arrête au premier
               saut. */
            const std::size_t saut = a_texte.find('\n', static_cast<std::size_t>(d));
            if (saut != std::string::npos && static_cast<int>(saut) < f) f = static_cast<int>(saut);

            /* Les blancs sortent de la sélection. Un double-clic ou un
               triple-clic emporte volontiers l'espace qui suit le mot ; la
               fermante se retrouverait alors précédée d'une espace, la règle
               d'adjacence la refuserait, et le joueur récolterait deux
               astérisques littéraux à la place d'un mot gras. */
            while (d < f && EstBlanc(a_texte[static_cast<std::size_t>(d)])) ++d;
            while (f > d && EstBlanc(a_texte[static_cast<std::size_t>(f - 1)])) --f;

            /* RETIRER, premier cas : les marqueurs sont JUSTE À L'EXTÉRIEUR de
               la sélection — ce qu'on obtient quand on double-clique le mot
               qu'on venait de mettre en gras. */
            if (MarqueurIci(a_texte, d - L, a_marque) && MarqueurIci(a_texte, f, a_marque)) {
                std::string t = a_texte;
                t.erase(static_cast<std::size_t>(f), static_cast<std::size_t>(L));
                t.erase(static_cast<std::size_t>(d - L), static_cast<std::size_t>(L));
                return rendre(std::move(t), d - L, f - L);
            }
            /* RETIRER, second cas : les marqueurs sont DANS la sélection —
               ce qu'on obtient en sélectionnant « **gras** » d'un trait. */
            if (f - d >= 2 * L && MarqueurIci(a_texte, d, a_marque) &&
                MarqueurIci(a_texte, f - L, a_marque)) {
                std::string t = a_texte;
                t.erase(static_cast<std::size_t>(f - L), static_cast<std::size_t>(L));
                t.erase(static_cast<std::size_t>(d), static_cast<std::size_t>(L));
                return rendre(std::move(t), d, f - 2 * L);
            }

            /* POSER. La fin d'abord : insérer au début décalerait la fin d'un
               marqueur, et le second `insert` tomberait à côté. Sélection
               vide (un simple curseur) : on obtient les deux marques et le
               curseur AU MILIEU — ce que le joueur veut quand il appuie sur
               Ctrl+B avant d'écrire. */
            std::string t = a_texte;
            t.insert(static_cast<std::size_t>(f), marqueur);
            t.insert(static_cast<std::size_t>(d), marqueur);
            return rendre(std::move(t), d + L, f + L);
        }

        // ── LE FILET, QUI EST UNE LIGNE À LUI SEUL ────────────────────────
        if (a_marque == Marque::Filet) {
            const int deb = DebutLigne(a_texte, d);
            const int fin = FinLigne(a_texte, deb);

            /* Déjà un filet sous le curseur : on l'enlève, avec son saut de
               ligne — sinon « poser puis reposer » laisserait une ligne vide
               derrière chaque essai. */
            if (LigneEstFilet(a_texte, deb, fin)) {
                std::string t   = a_texte;
                int         de  = deb;
                int         ver = fin;
                if (ver < static_cast<int>(t.size())) ++ver;          // le saut qui suit
                else if (de > 0)                      --de;           // ou celui qui précède
                t.erase(static_cast<std::size_t>(de), static_cast<std::size_t>(ver - de));
                return rendre(std::move(t), de, de);
            }
            /* Le curseur est sur une ligne vide : le filet la PREND, au lieu
               d'en ouvrir une de plus. C'est le cas de très loin le plus
               fréquent — on appuie sur Entrée, puis sur le bouton. */
            if (SauterBlancs(a_texte.c_str() + deb, static_cast<std::size_t>(fin - deb), 0)
                == static_cast<std::size_t>(fin - deb)) {
                std::string t = a_texte;
                t.replace(static_cast<std::size_t>(deb), static_cast<std::size_t>(fin - deb), "---");
                return rendre(std::move(t), deb + 3, deb + 3);
            }
            /* Sinon le filet s'ouvre SOUS la ligne où finit la sélection. */
            const int   apres = FinLigne(a_texte, f);
            std::string t     = a_texte;
            t.insert(static_cast<std::size_t>(apres), "\n---");
            return rendre(std::move(t), apres + 4, apres + 4);
        }

        // ── LES AUTRES MARQUES DE BLOC : UN PRÉFIXE PAR LIGNE TOUCHÉE ─────
        /* Toutes les lignes que la sélection touche reçoivent le MÊME sort,
           décidé sur la PREMIÈRE. Sans cela une sélection mêlant des lignes
           déjà marquées et d'autres non se mettrait à clignoter : chaque
           appui en marquerait la moitié et en démarquerait l'autre. */
        std::vector<int> departs;
        {
            int l = DebutLigne(a_texte, d);
            while (true) {
                departs.push_back(l);
                const int fl = FinLigne(a_texte, l);
                if (fl >= f || fl >= taille) break;
                l = fl + 1;
            }
        }

        int cibleTitre = 0;   // le niveau visé, pour Marque::Titre
        bool retirer   = false;
        {
            const int deb = departs.front();
            const int fin = FinLigne(a_texte, deb);
            int       oct = 0;
            if (a_marque == Marque::Titre) {
                /* LE CYCLE : rien -> titre -> sous-titre -> rien. Un seul
                   bouton pour les deux niveaux, et un troisième appui qui
                   rend la ligne ordinaire : c'est le geste que tout le monde
                   connaît, et il évite un bouton « enlever le titre ». */
                cibleTitre = (NiveauTitre(a_texte, deb, fin, oct) + 1) % 3;
            } else {
                const char c = (a_marque == Marque::Puce) ? '-' : '>';
                retirer      = APrefixe(a_texte, deb, fin, c, oct);
            }
        }

        std::vector<Edition> editions;
        editions.reserve(departs.size());
        for (const int deb : departs) {
            const int fin = FinLigne(a_texte, deb);
            Edition   e;
            e.pos = deb;
            if (a_marque == Marque::Titre) {
                int oct = 0;
                (void)NiveauTitre(a_texte, deb, fin, oct);
                e.retire = oct;
                e.ajoute = (cibleTitre == 1) ? "# " : (cibleTitre == 2 ? "## " : "");
            } else {
                const char c = (a_marque == Marque::Puce) ? '-' : '>';
                int        oct = 0;
                const bool la  = APrefixe(a_texte, deb, fin, c, oct);
                if (retirer) {
                    if (!la) continue;   // rien à retirer sur cette ligne-là
                    e.retire = oct;
                } else {
                    if (la) continue;    // déjà marquée : on ne double pas
                    e.ajoute = (c == '-') ? "- " : "> ";
                }
            }
            editions.push_back(std::move(e));
        }

        std::string t  = a_texte;
        int         nd = d;
        int         nf = f;
        /* DE LA DERNIÈRE ÉDITION VERS LA PREMIÈRE : les positions des lignes
           qui précèdent restent valables tant qu'on n'a rien changé devant
           elles. Le curseur, lui, se recale à chaque édition. */
        for (auto it = editions.rbegin(); it != editions.rend(); ++it) {
            t.replace(static_cast<std::size_t>(it->pos), static_cast<std::size_t>(it->retire), it->ajoute);
            const int ajoute = static_cast<int>(it->ajoute.size());
            const auto recale = [&](int& v) {
                if (v < it->pos) return;                              // avant l'édition : intact
                if (v >= it->pos + it->retire) v += ajoute - it->retire;
                else                           v = it->pos + ajoute;  // il était DANS le préfixe retiré
            };
            recale(nf);
            recale(nd);
        }
        return rendre(std::move(t), nd, nf);
    }

    // ── LES LIBELLÉS ──────────────────────────────────────────────────────

    const char* Nom(Marque a_m)
    {
        switch (a_m) {
            case Marque::Gras:     return "Gras";
            case Marque::Italique: return "Italique";
            case Marque::Souligne: return "Souligné";
            case Marque::Barre:    return "Barré";
            case Marque::Titre:    return "Titre";
            case Marque::Puce:     return "Puce";
            case Marque::Citation: return "Citation";
            case Marque::Filet:    return "Filet";
            default:               return "";
        }
    }

    const char* Exemple(Marque a_m)
    {
        switch (a_m) {
            case Marque::Gras:     return "**gras**";
            case Marque::Italique: return "*italique*";
            case Marque::Souligne: return "__souligné__";
            case Marque::Barre:    return "~~barré~~";
            case Marque::Titre:    return "# titre";
            case Marque::Puce:     return "- puce";
            case Marque::Citation: return "> citation";
            case Marque::Filet:    return "---";
            default:               return "";
        }
    }

    const char* Aide(Marque a_m)
    {
        switch (a_m) {
            case Marque::Gras:
                return "le passage ressort en gras — il s'écrit **ainsi** dans la page (Ctrl+B)";
            case Marque::Italique:
                return "le passage se penche — il s'écrit *ainsi* dans la page (Ctrl+I)";
            case Marque::Souligne:
                return "un filet sous le passage — il s'écrit __ainsi__ dans la page (Ctrl+U)";
            case Marque::Barre:
                return "un filet au travers du passage — il s'écrit ~~ainsi~~ (Ctrl+Maj+B)";
            case Marque::Titre:
                return "la ligne devient un titre ; encore un appui la passe en sous-titre, un troisième la rend ordinaire";
            case Marque::Puce:
                return "la ligne prend une puce ; un second appui la lui retire";
            case Marque::Citation:
                return "la ligne se met en retrait derrière un filet, et se penche ; un second appui la remet droite";
            case Marque::Filet:
                return "un trait de séparation sur sa propre ligne ; un second appui l'enlève";
            default:
                return "";
        }
    }

    // ── L'AUTO-ÉPREUVE ────────────────────────────────────────────────────
    //
    // Elle tourne à kDataLoaded, donc HORS de toute trame ImGui : elle
    // n'éprouve QUE ce qui est pur — l'analyse (marques, adjacence, blocs) et
    // `Poser`. C'est exactement la moitié du module qui peut abîmer une page ;
    // le dessin, lui, ne peut que faire une image laide pendant une trame, et
    // il s'éprouve à l'œil, en jeu.

    namespace
    {
        /** Le masque de styles en lettres : G I S B, un tiret pour « aucun ». */
        std::string Lettres(unsigned a_styles)
        {
            std::string r;
            if ((a_styles & kGras) != 0u)     r += 'G';
            if ((a_styles & kItalique) != 0u) r += 'I';
            if ((a_styles & kSouligne) != 0u) r += 'S';
            if ((a_styles & kBarre) != 0u)    r += 'B';
            if (r.empty()) r = "-";
            return r;
        }

        /** Une ligne analysée, rendue en une chaîne comparable :
         *  « GENRE|styles:texte|styles:texte| ». Les lettres sont G I S B, un
         *  tiret pour « aucun style ». */
        std::string RenduDEpreuve(const std::string& a_ligne)
        {
            std::size_t dc = 0;
            const Genre g  = GenreDeLigne(a_ligne.c_str(), a_ligne.size(), dc);

            std::string r;
            switch (g) {
                case Genre::Vide:     r = "VI|"; break;
                case Genre::Titre1:   r = "T1|"; break;
                case Genre::Titre2:   r = "T2|"; break;
                case Genre::Puce:     r = "PU|"; break;
                case Genre::Citation: r = "CI|"; break;
                case Genre::Filet:    r = "FI|"; break;
                default:              r = "PA|"; break;
            }
            if (g == Genre::Vide || g == Genre::Filet) return r;

            const std::string    contenu = a_ligne.substr(dc);
            std::vector<Morceau> ms;
            AnalyserLigne(contenu.c_str(), contenu.size(), ms);
            for (const Morceau& m : ms) {
                r += Lettres(m.styles);
                r += ':';
                r += contenu.substr(m.debut, m.fin - m.debut);
                r += '|';
            }
            return r;
        }

        /** CE QUE LE REPLI DOIT MESURER, morceau par morceau : le morceau
         *  lui-même, puis — précédés d'un « + » — les fragments qui lui sont
         *  COLLÉS, c'est-à-dire la suite de son dernier mot de l'autre côté
         *  d'une marque. « -:l'+I:or|I:or|-: du roi| » se lit : le repli qui
         *  décide du sort de « l' » doit peser « l'or » en entier.
         *
         *  C'est la moitié ÉPROUVABLE de la correction du constat 5 : la
         *  largeur en pixels demande une police cuite, donc une trame de jeu,
         *  mais SAVOIR quels octets forment un même mot ne demande rien. Un
         *  repli qui coupe « Bonjour, » en deux commence toujours par une
         *  erreur de lecture, et c'est celle-là qu'on grave. */
        std::string CollageDEpreuve(const std::string& a_ligne)
        {
            std::size_t dc = 0;
            const Genre g  = GenreDeLigne(a_ligne.c_str(), a_ligne.size(), dc);
            if (g == Genre::Vide || g == Genre::Filet) return "";

            const std::string    contenu = a_ligne.substr(dc);
            std::vector<Morceau> ms;
            AnalyserLigne(contenu.c_str(), contenu.size(), ms);

            std::vector<Morceau> collee;
            std::string          r;
            for (std::size_t k = 0; k < ms.size(); ++k) {
                r += Lettres(ms[k].styles);
                r += ':';
                r += contenu.substr(ms[k].debut, ms[k].fin - ms[k].debut);
                EtendueCollee(contenu, ms, k, collee);
                for (const Morceau& s : collee) {
                    r += '+';
                    r += Lettres(s.styles);
                    r += ':';
                    r += contenu.substr(s.debut, s.fin - s.debut);
                }
                r += '|';
            }
            return r;
        }

        /** Un texte de journal : les sauts de ligne se voient, sans quoi un
         *  écart sur un filet s'imprimerait sur trois lignes de journal. */
        std::string Lisible(const std::string& a_s)
        {
            std::string r;
            r.reserve(a_s.size() + 4);
            for (const char c : a_s) {
                if (c == '\n')      r += "\\n";
                else if (c == '\t') r += "\\t";
                else                r += c;
            }
            return r;
        }

        std::string PoseEnTexte(const Pose& a_p)
        {
            std::string r = a_p.possible ? "oui " : "non ";
            r += Lisible(a_p.texte);
            r += ' ';
            r += std::to_string(a_p.debut);
            r += "..";
            r += std::to_string(a_p.fin);
            return r;
        }

        struct CasAnalyse
        {
            const char* ligne;
            const char* attendu;
        };

        struct CasCollage
        {
            const char* ligne;
            const char* attendu;
        };

        struct CasPose
        {
            const char* nom;
            const char* texte;
            int         debut;
            int         fin;
            Marque      marque;
            int         borne;
            const char* attendu;
        };
    }

    void Autotest()
    {
        int cas    = 0;
        int ecarts = 0;

        // ── L'ANALYSE ─────────────────────────────────────────────────────
        static const CasAnalyse kAnalyses[] = {
            // les quatre marques en ligne
            { "texte simple",                       "PA|-:texte simple|" },
            { "**gras**",                           "PA|G:gras|" },
            { "*italique*",                         "PA|I:italique|" },
            { "__souligne__",                       "PA|S:souligne|" },
            { "~~barre~~",                          "PA|B:barre|" },
            // l'imbrication
            { "un **mot en *deux* styles** fin",    "PA|-:un |G:mot en |GI:deux|G: styles|-: fin|" },
            { "a __b~~c~~d__ e",                    "PA|-:a |S:b|SB:c|S:d|-: e|" },
            // l'adjacence : ces trois-la restent litterales
            { "2 * 3",                              "PA|-:2 * 3|" },
            { "10 h - 12 h",                        "PA|-:10 h - 12 h|" },
            { "un *mot* et 2 * 3",                  "PA|-:un |I:mot|-: et 2 * 3|" },
            /* Ces deux lignes-là éprouvent CHACUNE des deux moitiés de la
               règle d'adjacence, séparément. La première ne tombe que si l'on
               cesse d'exiger un caractère plein APRÈS l'ouvrante ; la seconde
               que si l'on cesse de l'exiger AVANT la fermante (là, le « ** »
               du milieu est précédé d'une espace : il n'a pas le droit de
               refermer le gras, et le mot suivant est encore dedans).
               Elles sont ici parce que les épreuves d'origine ne les
               couvraient PAS : mutation faite le 07/09, les deux gardes
               retirées tour à tour, et pas une épreuve ne tombait. */
            { "a * b* c",                           "PA|-:a * b* c|" },
            { "**a ** b**",                         "PA|G:a ** b|" },
            // une ouvrante sans fermante sur la ligne
            { "**pas ferme",                        "PA|-:**pas ferme|" },
            { "fin de *phrase",                     "PA|-:fin de *phrase|" },
            // les six blocs
            { "",                                   "VI|" },
            { "# Titre",                            "T1|-:Titre|" },
            { "## Sous-titre",                      "T2|-:Sous-titre|" },
            { "- une puce",                         "PU|-:une puce|" },
            { "> une citation",                     "CI|-:une citation|" },
            { "---",                                "FI|" },
            { "-----",                              "FI|" },
            // un bloc porte aussi des marques en ligne
            { "# Un **titre** gras",                "T1|-:Un |G:titre|-: gras|" },
        };
        for (const CasAnalyse& c : kAnalyses) {
            ++cas;
            const std::string obtenu = RenduDEpreuve(c.ligne);
            if (obtenu != c.attendu) {
                ++ecarts;
                SKSE::log::warn("[RICHE] écart — analyse « {} » : attendu « {} », obtenu « {} »",
                    Lisible(c.ligne), c.attendu, obtenu);
            }
        }

        // ── LE MOT COLLÉ, À TRAVERS LES MARQUES ───────────────────────────
        /* Ce que le repli doit peser d'un seul tenant. Ces cas-là ne
           couvraient RIEN avant le 07/09 : le module ne se demandait jamais si
           deux morceaux voisins étaient le même mot, et coupait donc « l'or »
           en « l' » et « or » à la marge droite. */
        static const CasCollage kCollages[] = {
            // une marque au MILIEU d'un mot : les trois morceaux n'en font qu'un
            { "l'*or* du roi",          "-:l'+I:or|I:or|-: du roi|" },
            { "mot__s__ suite",         "-:mot+S:s|S:s|-: suite|" },
            // le cas du constat : la fermante colle à la ponctuation
            { "... : **Bonjour**, ami", "-:... : |G:Bonjour+-:,|-:, ami|" },
            // des marques EMPILÉES : le mot traverse deux frontières
            { "a***b***c",              "-:a+GI:b+-:c|GI:b+-:c|-:c|" },
            /* La chaîne s'ARRÊTE au premier blanc, même si le morceau où il
               tombe est lui-même collé au suivant : « A, » est un mot, « xy »
               en est un autre, et rien ne doit les peser ensemble. Sans cette
               halte, un mot du milieu de ligne tirerait derrière lui tout le
               reste du paragraphe et la marge droite reculerait sans raison. */
            { "**A**, x**y**",          "G:A+-:,|-:, x+G:y|G:y|" },
            // et ce qui NE colle pas : une espace de chaque côté de la marque
            { "un **mot** ici",         "-:un |G:mot|-: ici|" },
            { "texte simple",           "-:texte simple|" },
        };
        for (const CasCollage& c : kCollages) {
            ++cas;
            const std::string obtenu = CollageDEpreuve(c.ligne);
            if (obtenu != c.attendu) {
                ++ecarts;
                SKSE::log::warn("[RICHE] écart — collage « {} » : attendu « {} », obtenu « {} »",
                    Lisible(c.ligne), c.attendu, obtenu);
            }
        }

        // ── POSER ─────────────────────────────────────────────────────────
        static const CasPose kPoses[] = {
            { "sans sélection, les deux marques et le curseur au milieu",
              "bonjour", 3, 3, Marque::Gras, 2000, "oui bon****jour 5..5" },
            { "avec sélection, on entoure",
              "bonjour", 0, 3, Marque::Gras, 2000, "oui **bon**jour 2..5" },
            { "sélection déjà marquée, marqueurs dehors : on retire",
              "**bon**jour", 2, 5, Marque::Gras, 2000, "oui bonjour 0..3" },
            { "sélection déjà marquée, marqueurs dedans : on retire",
              "**bon**jour", 0, 7, Marque::Gras, 2000, "oui bonjour 0..3" },
            { "sélection à l'envers (fin < début)",
              "bonjour", 3, 0, Marque::Gras, 2000, "oui **bon**jour 2..5" },
            { "indices hors bornes (négatif et au-delà du texte)",
              "bonjour", -20, 900, Marque::Italique, 2000, "oui *bonjour* 1..8" },
            { "indice au milieu d'un caractère accentué",
              "héros", 1, 2, Marque::Gras, 2000, "oui h**é**ros 3..5" },
            { "italique sur du gras ne mange pas une étoile de la paire",
              "**gras**", 2, 6, Marque::Italique, 2000, "oui ***gras*** 3..7" },
            { "les blancs sortent de la sélection",
              "a  bon  b", 1, 8, Marque::Gras, 2000, "oui a  **bon**  b 5..8" },
            { "une marque en ligne ne franchit pas le saut de ligne",
              "un\ndeux", 0, 7, Marque::Gras, 2000, "oui **un**\\ndeux 2..4" },
            { "texte vide, curseur à zéro",
              "", 0, 0, Marque::Gras, 2000, "oui **** 2..2" },
            { "cycle du titre 1 sur 3 : rien devient titre",
              "Titre", 0, 0, Marque::Titre, 2000, "oui # Titre 2..2" },
            { "cycle du titre 2 sur 3 : titre devient sous-titre",
              "# Titre", 0, 0, Marque::Titre, 2000, "oui ## Titre 3..3" },
            { "cycle du titre 3 sur 3 : sous-titre redevient ordinaire",
              "## Titre", 0, 0, Marque::Titre, 2000, "oui Titre 0..0" },
            { "puce posée",
              "ligne", 2, 2, Marque::Puce, 2000, "oui - ligne 4..4" },
            { "puce retirée",
              "- ligne", 4, 4, Marque::Puce, 2000, "oui ligne 2..2" },
            { "citation posée",
              "ligne", 0, 0, Marque::Citation, 2000, "oui > ligne 2..2" },
            { "citation retirée",
              "> ligne", 3, 3, Marque::Citation, 2000, "oui ligne 1..1" },
            { "une puce sur deux lignes sélectionnées",
              "une\ndeux", 1, 6, Marque::Puce, 2000, "oui - une\\n- deux 3..10" },
            { "filet posé sous la ligne courante",
              "a\nb", 1, 1, Marque::Filet, 2000, "oui a\\n---\\nb 5..5" },
            { "filet retiré",
              "a\n---\nb", 3, 3, Marque::Filet, 2000, "oui a\\nb 2..2" },
            { "filet sur une page vide",
              "", 0, 0, Marque::Filet, 2000, "oui --- 3..3" },
            { "la borne franchie : rien ne change",
              "abcde", 0, 5, Marque::Gras, 5, "non abcde 0..5" },
            { "la borne compte des CARACTÈRES, pas des octets",
              "ééé", 0, 6, Marque::Gras, 7, "oui **ééé** 2..8" },
            { "retirer une marque passe même sous la borne",
              "**abcde**", 2, 7, Marque::Gras, 5, "oui abcde 0..5" },

            /* ── UN MOT QUI PORTE LES DEUX MARQUES (constat 3) ─────────────
               Avant le 07/09, aucune de ces quatre-là ne retirait quoi que ce
               soit : elles empilaient une paire d'étoiles de plus à chaque
               appui, et « ****mot**** » se rendait SANS AUCUN style — le mot
               perdait son gras ET son italique d'un coup, à l'insu du joueur.
               C'est le trou exact que les épreuves d'origine laissaient : le
               seul voisin, « italique sur du gras », s'arrêtait à la POSE et ne
               tentait jamais le retour. */
            { "retirer l'italique d'un mot gras ET italique",
              "***bonjour***", 3, 10, Marque::Italique, 2000, "oui **bonjour** 2..9" },
            { "retirer le gras d'un mot gras ET italique",
              "***bonjour***", 3, 10, Marque::Gras, 2000, "oui *bonjour* 1..8" },
            { "aller-retour : gras, italique, puis italique redonne le gras seul",
              "**bonjour**", 2, 9, Marque::Italique, 2000, "oui ***bonjour*** 3..10" },
            { "mot gras ET italique, marqueurs DEDANS la sélection",
              "***bonjour***", 0, 13, Marque::Gras, 2000, "oui *bonjour* 0..9" },

            /* ── LES SUITES D'ÉTOILES, DE UNE À CINQ ───────────────────────
               La règle en une ligne : deux étoiles portent le gras, l'italique
               n'est là que s'il en reste une IMPAIRE par-dessus les paires. Les
               suites impaires se retirent, les paires n'ont pas d'italique à
               retirer et posent donc une paire de plus. Ces cinq cas gravent la
               parité elle-même : c'est elle, et non un voisinage, qui décide. */
            { "suite de 1 étoile : l'italique y est, on le retire",
              "*mot*", 1, 4, Marque::Italique, 2000, "oui mot 0..3" },
            { "suite de 2 : pas d'italique à retirer, on en pose un",
              "**mot**", 2, 5, Marque::Italique, 2000, "oui ***mot*** 3..6" },
            { "suite de 3 : l'italique y est, on le retire",
              "***mot***", 3, 6, Marque::Italique, 2000, "oui **mot** 2..5" },
            { "suite de 4 : pas d'italique, on en pose un",
              "****mot****", 4, 7, Marque::Italique, 2000, "oui *****mot***** 5..8" },
            { "suite de 5 : l'italique y est, on le retire",
              "*****mot*****", 5, 8, Marque::Italique, 2000, "oui ****mot**** 4..7" },
            { "suite de 1 étoile : le gras, lui, s'y ajoute",
              "*mot*", 1, 4, Marque::Gras, 2000, "oui ***mot*** 3..6" },

            /* ── LA MARQUE POSÉE PUIS DÉ-POSÉE AVANT D'ÉCRIRE (constat 4) ───
               Le joueur appuie sur G sur une page neuve, se ravise, rappuie :
               le geste doit se défaire. Il empilait quatre étoiles de plus à
               chaque essai — invisibles au rendu, comptées dans les 2 000, et
               envoyées au serveur. Le texte attendu est VIDE : d'où les deux
               espaces avant « 0..0 ». */
            { "seconde pression sur une paire vide de gras : elle se défait",
              "****", 2, 2, Marque::Gras, 2000, "oui  0..0" },
            { "la même règle vaut pour le souligné",
              "____", 2, 2, Marque::Souligne, 2000, "oui  0..0" },
            { "gras puis italique sur une page neuve : le gras s'y retire seul",
              "******", 3, 3, Marque::Gras, 2000, "oui ** 1..1" },
            /* LA GARDE QUI TIENT TOUT LE RESTE : le curseur planté au milieu de
               l'ouvrante d'un gras VIVANT est localement indiscernable d'une
               paire d'italiques vide. La parité tranche — la suite y vaut 2 —
               et l'on n'entame pas la paire. Ce cas est LAID (le texte gagne
               deux étoiles inutiles) mais il ne DÉTRUIT rien, et c'est le bon
               côté du compromis : une correction qui ferait perdre au joueur un
               gras qu'il avait posé serait pire que le défaut qu'elle répare. */
            { "curseur au milieu d'une ouvrante vivante : la paire est intacte",
              "**gras**", 1, 1, Marque::Italique, 2000, "oui ****gras** 2..2" },
        };
        for (const CasPose& c : kPoses) {
            ++cas;
            const std::string obtenu = PoseEnTexte(
                Poser(c.texte, c.debut, c.fin, c.marque, c.borne));
            if (obtenu != c.attendu) {
                ++ecarts;
                SKSE::log::warn("[RICHE] écart — {} : attendu « {} », obtenu « {} »",
                    c.nom, c.attendu, obtenu);
            }
        }

        SKSE::log::info("[RICHE] auto-épreuve : {} cas, {} écarts", cas, ecarts);
    }
}
