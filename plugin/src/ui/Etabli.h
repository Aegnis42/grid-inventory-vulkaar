#pragma once

// [vulkaar] L'ÉTABLI DU MENUISIER — l'écran de fabrication.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/metiers/etabli.ts) :
// quels gestes sont ouverts, jusqu'à quelle qualité la main sait aller, ce que
// le sac porte à chaque cran. Ici on ne fait que MONTRER et DEMANDER :
//   - l'état arrive par GridInventory_etabli_etat.txt, écrit par le client
//     skymp du même processus (etabliService.ts) à chaque poussée serveur ;
//   - les gestes (fabriquer, fermer) partent par GridInventory_etabli.txt,
//     que le même service consomme.
// Deux fichiers TSV nus — le pont éprouvé de l'échange, aucun JSON à parser
// en C++. Une PAIRE À NOUS : deux écrans qui numéroteraient dans le même
// fichier s'avaleraient mutuellement leurs gestes.
//
// LES LIBELLÉS VIENNENT DU SERVEUR, jamais d'ici : noms de rayons, de gestes,
// de qualités. Ce fichier n'écrit pas un mot de français destiné au joueur —
// sauf les quelques étiquettes de l'écran lui-même (« Fabriquer », la
// recherche), qui n'appartiennent à aucune donnée de la table.
//
// LES NOMS D'INGRÉDIENTS, EUX, SE RÉSOLVENT ICI. Le serveur ne les a pas : sa
// table ne porte qu'un EDID technique (« IngotIron »). Nous avons le jeu sous
// la main — TESForm::LookupByID puis GetName().
//
// L'ÉCRAN REMPLACE LES DEUX PANNEAUX de la racine (UIRoot::DrawMainWindow s'y
// dérive) : la liste de fabrication à gauche, l'aperçu et la recette à droite,
// le monde entre les deux. C'est la disposition du fork, avec notre contenu.

#include <RE/Skyrim.h>
#include <string>

namespace FUI::Etabli
{
    /** Tronque les deux fichiers du pont — à kDataLoaded.
     *  L'ÉTAT AUSSI, pas seulement les gestes : un état rescapé d'un plantage
     *  ferait surgir l'établi au lancement du jeu. */
    void Initialiser();

    /** Lit l'état, arme l'aperçu de l'objet choisi. Depuis UIRoot::Tick,
     *  HORS de la trame ImGui. */
    void Tick();

    /** L'écran entier. Appelé par UIRoot::DrawMainWindow À LA PLACE des deux
     *  panneaux quand Ouvert() est vrai. */
    void Dessiner();

    /** Le serveur a-t-il ouvert l'établi pour nous ? */
    [[nodiscard]] bool Ouvert();

    /** Échap : referme l'établi seul. Rend true si quelque chose a été fermé. */
    bool Fermer();

    // ── LA SORTIE SCALEFORM (29/08/2026) ──────────────────────────────────
    //
    // Le même état, servi à NOTRE film — `content/interface/etabli/` du dépôt,
    // chargé par un RE::IMenu à nous. L'écran ImGui ci-dessus reste en place :
    // rien n'est dupliqué, le film reçoit une VUE des mêmes variables et ses
    // actions les modifient comme les clics ImGui le font.
    //
    // Pourquoi Scaleform plutôt qu'ImGui : décision du propriétaire du
    // 29/08/2026, prise après que l'essai eut prouvé en jeu que le moteur
    // accepte un swf que nous fabriquons.

    /** Le plateau, dans la grammaire que le film attend. Les ingrédients du
     *  SEUL geste choisi y figurent : envoyer ceux des 5 586 gestes de la
     *  forge ferait 1,9 Mio, et le joueur n'en regarde qu'un. */
    [[nodiscard]] std::string PlateauPourFilm();

    /** Ce que le film demande : rayon, filtre, choix, qualite, fabriquer,
     *  fermer. Rend true si l'écran doit être repeint. */
    bool ActionDuFilm(const char* a_quoi, const char* a_valeur);

    /** LE RENDU VIVANT DU MOTEUR — a appeler depuis GridInventoryMenu::
     *  PostDisplay, APRES la sequence de capture d'ItemPreview et AVANT la
     *  soumission d'ImGui. Le modele se pose ainsi sur le monde, et les
     *  fenetres de l'ecran se posent sur lui.
     *
     *  Appele AVANT la capture, l'etape de restauration du fond d'ecran
     *  l'effacerait dans la trame meme ou il est peint. */
    void RendreModeleVivant();
}
