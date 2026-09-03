#pragma once

// [vulkaar] LE PANNEAU DE LA MAISON — qui possède le bâtiment, qui y loge.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/monde/relaisMaison.ts) :
// qui a le droit de renommer, d'ajouter, de retirer, de basculer un rôle ;
// qui est encore dans la cellule ; ce que la recherche a le droit de rendre.
// Ici on ne fait que MONTRER et DEMANDER :
//   - l'état arrive par GridInventory_maison_etat.txt, écrit par le client
//     skymp du même processus (maisonService.ts) à chaque poussée serveur ;
//   - les gestes (renommer, chercher, ajouter, retirer, role, clefs, detacher,
//     fermer) partent par GridInventory_maison.txt, que le même service consomme.
//
// LES PORTES (étape 2) : un passage = la face visée et sa jumelle, désigné par
// la CLEF de sa première face (un descripteur « idLocal:Plugin.esp », jamais
// un FormID). Le staff rattache depuis le tchat ; ici on détache (×) et on
// donne les clefs : « toutes » (locataire) ou une liste cochée (invité) dans
// une fenêtre de choix nue, par-dessus le panneau.
// Deux fichiers TSV nus — le pont éprouvé de l'échange et de l'établi. Une
// PAIRE À NOUS : deux écrans qui numéroteraient dans le même fichier
// s'avaleraient mutuellement leurs gestes.
//
// LES IDENTITÉS VIENNENT DU SERVEUR, jamais d'ici : le registre des comptes
// décide quel nom le spectateur a le droit de lire (le vrai nom pour le
// staff, le nom seulement si la personne s'est présentée, sinon le
// matricule). Ce fichier n'écrit que les étiquettes de l'écran lui-même
// (« Bâtiment : », la recherche, « Tu es locataire ici. »).
//
// LES PERSONNES SE DÉSIGNENT PAR LEUR personnageId, le petit entier du
// registre. Aucun FormID ne transite : la maison est connue du serveur par la
// cellule où le joueur a tapé /maison.
//
// L'ÉCRAN REMPLACE LES DEUX PANNEAUX de la racine, comme l'établi (UIRoot::
// Render s'y dérive) : un panneau centré et discret, le monde autour. Se
// SUPERPOSER comme l'échange ferait surgir tout l'inventaire pour une liste
// de six lignes.

#include <RE/Skyrim.h>

namespace FUI::Maison
{
    /** Tronque les deux fichiers du pont — à kDataLoaded.
     *  L'ÉTAT AUSSI, pas seulement les gestes : un état rescapé d'un plantage
     *  ferait surgir le panneau au lancement du jeu. */
    void Initialiser();

    /** Lit l'état, fait partir la recherche stabilisée, tient le chien de
     *  garde. Depuis UIRoot::Tick, à CHAQUE trame, racine ouverte ou non. */
    void Tick();

    /** Le panneau entier. Appelé par UIRoot::Render À LA PLACE des deux
     *  panneaux quand Ouvert() est vrai. */
    void Dessiner();

    /** Le serveur a-t-il ouvert le panneau pour nous ? */
    [[nodiscard]] bool Ouvert();

    /** Échap : si la fenêtre de choix des clefs est ouverte, la referme SEULE
     *  (le panneau reste, rien n'est dit au serveur) ; sinon referme le panneau
     *  et le dit au serveur (geste « fermer »). Rend true si quelque chose a
     *  été fermé. */
    bool Fermer();
}
