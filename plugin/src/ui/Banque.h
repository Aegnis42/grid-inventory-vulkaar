#pragma once

// [vulkaar] LE COMPTOIR DE LA BANQUE — déposer, retirer, faire le change.
//
// CE QUE CET ÉCRAN FAIT, ET RIEN D'AUTRE (propriétaire, 06/09/2026) : « la
// banque permet de stocker notre argent et rien d'autre / on peut y déposer
// nos pièces, faire le change entre or, argent, cuivre et récupérer notre
// argent ». Trois lignes, une par monnaie — Septime (or), Mède (argent),
// Titus (cuivre) — avec ce qu'on a EN BANQUE et ce qu'on a SUR SOI, une
// quantité, « Déposer », « Retirer » ; un bloc de change ; les dernières
// opérations. Pas d'objets, pas de coffre : un compte, c'est des pièces.
//
// UN ÉCRAN À PART, PAS UN TROISIÈME `Sujet` D'APPARTENANCE. Le panneau des
// maisons et des coffres a été généralisé parce que ses deux sujets
// partageaient sept cents lignes — membres, rôles, recherche, verrou. La
// banque n'en partage AUCUNE : ni membre, ni clef, ni verrou. Y greffer un
// sujet aurait mis trois `if` de plus dans chaque fonction pour ne rien
// réutiliser que le chrome. Ce qui est vraiment commun — la sentinelle `fin`,
// le `seq` relu en premier, le chien de garde, les filets or — est recopié
// ici à l'identique, et c'est un choix assumé : le pont de la banque a SA
// paire de fichiers, donc son propre `seq`, et aucun geste ne peut être routé
// au mauvais registre puisqu'il n'y a qu'un registre au bout.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/banque/relaisBanque.ts) :
// la portée du comptoir, la cadence, les quantités, l'existence des pièces
// dans la bourse, l'exactitude du change, le solde. Ici on ne fait que
// MONTRER et DEMANDER. Le calcul du change à l'écran (« 10 Mèdes contre
// 1 Septime », ou « pas exact ») n'est qu'un CONFORT pour griser un bouton :
// le serveur rejuge tout, et c'est sa phrase que le joueur lit ensuite.
//
// AUCUN TAUX N'EST ÉCRIT ICI. Les valeurs des monnaies (en Titus) arrivent
// dans l'état, ligne par ligne ; la source unique est
// packages/shared/src/monnaies.ts. Le jour où le rapport change, cet écran
// suit sans être recompilé — la ligne « 1 Septime = 10 Mèdes = 100 Titus »
// est CALCULÉE depuis ce qui a été reçu.
//
// « SUR TOI » EST LE COMPTE DU SERVEUR, pas MonnaiesVulkaar::Compte(). Le
// cache local de la bourse se remplit pendant la collecte de la grille, qui
// n'a pas forcément tourné depuis le dernier geste ; un bouton « Déposer »
// actif sur un nombre d'une trame en retard ferait partir un geste que le
// serveur refuserait. Le serveur relit l'inventaire à chaque poussée : c'est
// lui qu'on affiche. MonnaiesVulkaar ne sert qu'à l'ICÔNE, par RANG (0..2 =
// l'ordre de MONNAIES, le même des deux côtés du pont).
//
// UN GESTE, UNE MONNAIE : « Déposer » sur la ligne des Mèdes écrit
// `deposer 0 <n> 0`. Les colonnes du geste sont dans l'ordre de MONNAIES
// (septime, mède, titus), et les deux autres restent à zéro. Le change est
// le seul geste qui nomme deux monnaies, par leur id (`septime|mede|titus`)
// tel que reçu dans l'état — jamais un FormID, jamais un nom affiché.
//
// LES NOMBRES SONT GROUPÉS PAR TROIS avec une virgule, comme la bourse en bas
// de l'inventaire (`Grouped` d'UIRoot.cpp, qui vit dans un espace anonyme et
// est donc recopié) : « 1,000 Titus » ici et « 1,000 » là doivent se lire de
// la même façon dans le même jeu.
//
// LES DEUX FICHIERS DU PONT :
//   - l'état arrive par GridInventory_banque_etat.txt, écrit par le client
//     skymp du même processus (banqueService.ts) à chaque poussée serveur ;
//   - les gestes partent par GridInventory_banque.txt, que le même service
//     consomme (une trame sur quinze).
//
// L'ÉCRAN REMPLACE LES DEUX PANNEAUX de la racine, comme l'établi et le
// panneau d'appartenance.

#include <RE/Skyrim.h>

#include <cstdint>

namespace FUI::Banque
{
    /** Tronque les deux fichiers du pont — à kDataLoaded.
     *  L'ÉTAT AUSSI, pas seulement les gestes : un état rescapé d'un plantage
     *  ferait surgir le comptoir au lancement du jeu. */
    void Initialiser();

    /** Lit l'état, tient le chien de garde et le compte à rebours du message.
     *  Depuis UIRoot::Tick, à CHAQUE trame, racine ouverte ou non. */
    void Tick();

    /** Le comptoir entier. Appelé par UIRoot::Render À LA PLACE des deux
     *  panneaux quand Ouvert() est vrai. */
    void Dessiner();

    /** Le serveur a-t-il ouvert le comptoir pour nous ? */
    [[nodiscard]] bool Ouvert();

    /** Échap : referme le comptoir et le dit au serveur (geste « fermer »).
     *  Rend true si quelque chose a été fermé. */
    bool Fermer();
}
