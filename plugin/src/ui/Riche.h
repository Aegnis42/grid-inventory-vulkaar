#pragma once

// [vulkaar] LA PLUME — la mise en forme des pages du carnet.
//
// CE QUE LE PROPRIÉTAIRE A DEMANDÉ (07/09/2026) : « pour la partie écriture on
// puisse faire saut de ligne, retour à la ligne, mise en gras, italique, et
// tout ce qui permet de faire de belles notes ». Ce module est la moitié qui
// DESSINE ; l'autre moitié — la barre d'outils, les deux modes, le champ de
// saisie — vit dans Notes.cpp et n'appelle ici que `Dessiner` et `Poser`.
//
// ── LA GRAMMAIRE, ET POURQUOI ELLE TIENT EN ASCII IMPRIMABLE ──────────────
// Les marques vivent DANS le texte de la page. Le registre ne sait rien
// d'elles : une page partagée emporte donc sa mise en forme sans qu'une seule
// ligne de serveur change, et une page écrite avant ce chantier reste
// exactement ce qu'elle était.
//
//   En ligne :  **gras**   *italique*   __souligné__   ~~barré~~
//   En tête de ligne :  # titre   ## sous-titre   - puce   > citation
//   Une ligne faite de trois tirets ou plus (---) est un filet.
//   Une ligne vide sépare deux paragraphes ; une ligne ordinaire reste une
//   ligne (rien n'est recollé au voisin).
//
// DEUX CARACTÈRES SONT INTERDITS DE SÉJOUR, et ce sont eux qui décident de
// toute la grammaire. La TABULATION : le pont des notes est du TSV, `Echapper`
// la change en espace — une marque qui l'emploierait disparaîtrait entre deux
// machines. L'ANTISLASH : le pont le double, donc un texte qui en contient
// n'est plus celui qu'on a tapé. IL N'Y A DONC AUCUN CARACTÈRE D'ÉCHAPPEMENT
// DANS CETTE GRAMMAIRE — rien qui permette d'écrire « une étoile littérale ».
//
// ── LA RÈGLE D'ADJACENCE, QUI REMPLACE L'ÉCHAPPEMENT ──────────────────────
// À défaut de pouvoir échapper, on reconnaît les marques à leur VOISINAGE, la
// règle des carnets de bord et des messageries :
//   1. une ouvrante n'en est une que si le caractère qui la SUIT n'est ni une
//      espace ni une fin de ligne ;
//   2. une fermante n'en est une que si le caractère qui la PRÉCÈDE n'est pas
//      une espace ;
//   3. une ouvrante sans fermante SUR LA MÊME LIGNE reste du texte ordinaire.
// Ainsi « 2 * 3 » et « 10 h - 12 h » s'écrivent sans y penser, et « **note » se
// lit tel quel. Les marques s'imbriquent (chaque style est un bit, l'ordre de
// fermeture n'a donc pas à être respecté), et l'on essaie toujours la marque
// LONGUE avant la courte : `**` avant `*`.
//
// ── UNE SUITE D'ÉTOILES SE LIT TOUT ENTIÈRE ───────────────────────────────
// Le gras et l'italique partagent un caractère : « *** » est donc les DEUX à
// la fois, et c'est la LONGUEUR DE LA SUITE, jamais le voisin immédiat, qui
// dit laquelle des deux marques est là. Deux étoiles portent le gras ;
// l'italique n'y est que s'il en reste une IMPAIRE par-dessus les paires. Une
// suite de 1 est un italique seul, de 2 un gras seul, de 3 les deux.
// C'est ce qui permet à `Poser` de RETIRER une marque d'un mot qui porte les
// deux — le second appui doit défaire le premier, sans quoi le geste n'est
// qu'un aller sans retour (constat du 07/09) — tout en laissant intacte la
// paire d'un gras vivant quand le curseur se plante entre ses deux étoiles :
// la suite y vaut 2, elle est paire, l'italique n'y est pas.
//
// ── LE RENDU EST UNE VUE, ET C'EST LA GARANTIE QUI COMPTE ─────────────────
// Ce module NE RÉÉCRIT JAMAIS le texte du joueur. `Dessiner` lit une chaîne et
// pose des pixels ; elle ne rend rien, ne modifie rien, n'enregistre rien. Un
// défaut d'analyse — une marque mal reconnue, un repli de travers — abîme donc
// l'IMAGE d'une page pendant une trame, jamais la page. C'est la raison pour
// laquelle le carnet peut se permettre une grammaire devinée plutôt que
// déclarée : le pire cas est laid, il n'est pas destructeur.
//
// La SEULE fonction qui touche à ce que le joueur a écrit est `Poser`, et elle
// est TOTALE : aucun triplet (texte, début, fin), si absurde soit-il — indices
// négatifs, plus grands que le texte, à l'envers, tombant au milieu d'un
// caractère accentué —, ne la fait sortir de ses bornes ni rendre un texte
// cassé. Quand elle ne peut pas faire ce qu'on lui demande, elle le DIT
// (`Pose::possible` à faux) et rend le texte d'origine, octet pour octet.
//
// ── DEUX RÈGLES DE DESSIN QUI ONT UNE RAISON PRÉCISE ──────────────────────
// UN JEU FINI DE TAILLES. `Dessiner` n'emploie que trois tailles, calculées
// une fois par appel à partir de `ImGui::GetFontSize()` : le corps, le titre
// (corps × 1,25) et le sous-titre (corps × 1,10), toutes arrondies au pixel.
// Depuis ImGui 1.92 chaque taille demandée CUIT une variante de la police dans
// l'atlas ; une taille inédite à chaque trame — une animation, un facteur
// continu — finit par faire tomber une assertion de l'atlas, et en attendant
// elle cuit pour rien. Trois tailles, cuites une fois, pour toute la partie.
//
// AUCUN CARACTÈRE NON-ASCII N'EST DESSINÉ ICI. La puce, le losange du filet,
// les traits du souligné, du barré et de la citation sont tracés à la main sur
// le drawlist (le précédent maison est la loupe, Appartenance.cpp:668). Un
// glyphe décoratif suppose une police qui le porte : sur un poste sans
// `seguisym.ttf` il se peindrait en tofu, et RIEN ne le dirait — ni journal,
// ni erreur, juste un carré à la place d'une puce chez un joueur sur dix.
//
// Le gras, l'italique et le gras-italique demandent leur police à
// `UIRoot::BoldFont` / `ItalicFont` / `BoldItalicFont`, qui rendent la police
// principale dès qu'un seul point de code leur échappe : un mot pas penché
// vaut mieux qu'un mot en tofu.

#include <imgui.h>
#include <string>

namespace FUI::Riche
{
    enum class Marque {
        Gras, Italique, Souligne, Barre,   // en ligne
        Titre, Puce, Citation, Filet       // en bloc
    };

    /** Dessine le texte MIS EN FORME à la position courante, sur `a_largeur`
     *  pixels, et avance le curseur ImGui de la hauteur consommée — comme
     *  ImGui::TextWrapped. `a_encre` est la couleur du corps (l'encre maison
     *  est Theme::Chrome(0.92f)), `a_S` l'échelle (Theme::Scale()). */
    void Dessiner(const char* a_texte, float a_largeur, float a_S, ImU32 a_encre);

    struct Pose {
        bool        possible;   // false : la borne serait franchie, rien ne change
        std::string texte;      // le texte neuf
        int         debut;      // la sélection à replacer, en OCTETS
        int         fin;
    };

    /** Pose la marque sur [a_debut, a_fin) — des OCTETS, l'unité d'ImGui — ou
     *  la RETIRE si elle y est déjà. `a_borne` est le nombre maximal de
     *  CARACTÈRES (points de code) du résultat. Fonction TOTALE : aucun
     *  argument, si absurde soit-il, ne la fait sortir de ses bornes. */
    [[nodiscard]] Pose Poser(const std::string& a_texte, int a_debut, int a_fin,
                             Marque a_marque, int a_borne);

    [[nodiscard]] const char* Nom(Marque a_m);       // « Gras »
    [[nodiscard]] const char* Exemple(Marque a_m);   // « **gras** »
    [[nodiscard]] const char* Aide(Marque a_m);      // l'infobulle, une phrase

    /** Éprouve l'analyse et Poser sur un jeu de cas gravés et JOURNALISE
     *  (« [RICHE] auto-épreuve : N cas, M écarts » + une ligne par écart).
     *  N'appelle RIEN d'ImGui : elle tourne à kDataLoaded. */
    void Autotest();
}
