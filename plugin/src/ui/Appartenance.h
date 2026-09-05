#pragma once

// [vulkaar] LE PANNEAU D'APPARTENANCE — qui possède, qui a la clef.
//
// UN SEUL ÉCRAN POUR DEUX SUJETS (05/09/2026) : la MAISON (`/maison`) et le
// COFFRE (`/coffre`). Le panneau des coffres est celui des maisons privé du
// champ de nom, de la colonne des clefs et de la section des portes, avec un
// bouton de verrou unique à la place. Le reste — la lecture du TSV et sa
// sentinelle, la recherche stabilisée, la table des membres, le chien de
// garde, le chrome du panneau, la fermeture — est RIGOUREUSEMENT le même.
//
// POURQUOI GÉNÉRALISER PLUTÔT QUE RECOPIER. Un second écran aurait dupliqué
// sept cents lignes dont la moitié sont des pièges déjà payés : la sentinelle
// `fin` qui protège d'une lecture pendant l'écriture, le `seq` relu en
// premier, le verrou qui se lit « verrouillé » sur une colonne absente, le
// chien de garde qui prévient le serveur quand la racine s'est refermée
// ailleurs. Deux copies, et celle des deux qui prendrait du retard ouvrirait
// un coffre que le serveur croit fermé — sans que rien ne le signale. Un
// écran, un `Sujet` dans l'état, et les trois morceaux qui diffèrent
// vraiment sont gardés par un `if`.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/monde/relaisMaison.ts
// et son pendant des coffres) : qui a le droit de renommer, d'ajouter, de
// retirer, de basculer un rôle, de tourner un verrou ; qui est encore dans la
// cellule ; ce que la recherche a le droit de rendre. Ici on ne fait que
// MONTRER et DEMANDER.
//
// UNE SEULE PAIRE DE FICHIERS POUR LES DEUX SUJETS :
//   - l'état arrive par GridInventory_appartenance_etat.txt, écrit par le
//     client skymp du même processus (appartenanceService.ts) à chaque
//     poussée serveur ; sa ligne `sujet` dit de quoi il parle ;
//   - les gestes partent par GridInventory_appartenance.txt, que le même
//     service consomme ; CHAQUE LIGNE PORTE SON SUJET en deuxième colonne.
//
// LE SUJET VOYAGE DANS LA LIGNE, ET C'EST LA RAISON D'ÊTRE DE CETTE COLONNE :
// un geste écrit sur le panneau de la maison peut être lu par le service
// après que le joueur a ouvert celui du coffre (le service relit quatre fois
// par seconde, le joueur clique plus vite). Router sur « le sujet ouvert au
// moment de la lecture » enverrait le geste au mauvais registre. Router sur
// le sujet ÉCRIT DANS LA LIGNE ne se trompe jamais.
//
// DEUX PAIRES DE FICHIERS auraient été l'autre voie : chacune son compteur,
// aucun croisement possible. Elle est refusée pour ce qu'elle coûte AILLEURS :
// deux modules C++ (deux `g_pret`, deux chiens de garde, deux `Ouvert()` —
// donc deux écrans qui peuvent se croire ouverts en même temps, dont un seul
// est dessiné et dont l'autre enverrait un « fermer » deux secondes plus
// tard), deux services TS, et le double des ouvertures de fichier par trame.
// Un écran qui ne peut tenir QU'UN sujet à la fois rend cette faute
// impossible à écrire.
//
// LES PORTES ET LES CLEFS N'EXISTENT QUE POUR LA MAISON. Un coffre n'a
// qu'une ancre — sa propre référence — donc pas de trousseau à distribuer :
// être locataire, c'est avoir la clef. La colonne « Clefs » et la fenêtre de
// choix disparaissent avec elle.
//
// LE VERROU DU COFFRE EST CELUI D'UNE PORTE (règle du 03/09) : verrouillé =
// fermé à TOUT LE MONDE, propriétaire compris ; la clef ne fait que le
// TOURNER. Le bouton dit l'état actuel et demande l'état VOULU, jamais une
// bascule aveugle — deux panneaux ouverts se croiseraient.
//
// IL N'Y A AUCUN VERROU CÔTÉ CLIENT POUR LES COFFRES, et ce n'est pas un
// oubli : `dealWithRef` bloque déjà l'activation de TOUT `FormType.Container`
// sans exception, le serveur seul juge, et il n'y a pas de crochetage à
// rendre possible (Skyrim OUVRE un conteneur à la réussite du minijeu,
// contrairement à une porte). Voir le cartouche de `serruresService.ts`.
//
// LES IDENTITÉS VIENNENT DU SERVEUR, jamais d'ici : le registre des comptes
// décide quel nom le spectateur a le droit de lire. Ce fichier n'écrit que
// les étiquettes de l'écran lui-même.
//
// LES PERSONNES SE DÉSIGNENT PAR LEUR personnageId, le petit entier du
// registre. Aucun FormID ne transite.
//
// L'ÉCRAN REMPLACE LES DEUX PANNEAUX de la racine, comme l'établi.

#include <RE/Skyrim.h>

#include <cstdint>

namespace FUI::Appartenance
{
    /** De quoi le panneau parle. `kAucun` = fermé. */
    enum class Sujet : std::uint8_t
    {
        kAucun = 0,
        kMaison,
        kCoffre
    };

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

    /** Le serveur a-t-il ouvert un panneau pour nous ? */
    [[nodiscard]] bool Ouvert();

    /** Lequel — pour qui veut s'adresser au bon registre. */
    [[nodiscard]] Sujet SujetCourant();

    /** Échap : si la fenêtre de choix des clefs est ouverte (maison seule),
     *  la referme SEULE (le panneau reste, rien n'est dit au serveur) ; sinon
     *  referme le panneau et le dit au serveur (geste « fermer »). Rend true
     *  si quelque chose a été fermé. */
    bool Fermer();
}
