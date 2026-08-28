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
}
