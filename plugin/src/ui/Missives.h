#pragma once

// [vulkaar] LES MISSIVES — écrire à qui l'on connaît, lire en ville.
//
// CE QUE CET ÉCRAN FAIT (propriétaire, 06/09/2026) : « on va faire un système
// de missive, envoi de message via une interface avec une liste de toutes les
// personnes qui se sont présentées à toi et à qui tu t'es présenté ; on pourra
// leur écrire un message qui prendra plus ou moins de temps avec la distance,
// et pour pouvoir lire le message il faudra être dans une ville ou un
// village ». Deux colonnes : à gauche le CARNET et le courrier reçu, à droite
// la LETTRE choisie ou la COMPOSITION.
//
// UN ÉCRAN À PART, PAS UN TROISIÈME `Sujet` D'APPARTENANCE — pour la même
// raison que la banque : le panneau des maisons et des coffres a été
// généralisé parce que ses deux sujets partageaient sept cents lignes
// (membres, rôles, recherche, verrou). Les missives n'en partagent AUCUNE : ni
// membre, ni clef, ni verrou. Ce qui est vraiment commun — la sentinelle
// `fin`, le `seq` relu en premier, le chien de garde, les filets or — est
// recopié ici à l'identique, et c'est un choix assumé : le pont des missives a
// SA paire de fichiers, donc son propre `seq`, et aucun geste ne peut partir
// au mauvais registre puisqu'il n'y en a qu'un au bout.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/missives/relaisMissives.ts) :
// qui est dans le carnet, à qui l'on a le droit d'écrire, combien de temps la
// route prend, quand une lettre est arrivée, et SURTOUT si le lecteur est dans
// une ville ou un village. Ici on ne fait que MONTRER et DEMANDER.
//
// LE TEXTE D'UNE LETTRE NE DESCEND PAS TANT QU'ON N'A PAS LE DROIT DE LA LIRE,
// et c'est la force de la règle : le serveur ne l'envoie pas au client hors
// d'un lieu habité, donc un client bricolé n'a rien à lire. Cet écran ne fait
// que le CONSTATER — quand `ici` est faux, la lettre choisie affiche « Il faut
// être dans une ville ou un village pour lire son courrier. » à la place d'un
// texte qui, de toute façon, n'est pas là. N'écris jamais de repli qui
// devinerait le texte : il n'y a rien à deviner.
//
// LE CARNET EST LE SEUL ANNUAIRE. On n'écrit qu'à quelqu'un qui s'y trouve —
// ceux qui se sont présentés à nous OU à qui nous nous sommes présentés. La
// loupe de la colonne de gauche est un filtre LOCAL sur ces lignes-là, jamais
// une recherche envoyée au serveur : le contrat du pont n'a pas de geste
// « chercher », et il ne doit pas en avoir un — la liste est déjà là.
//
// ET RECEVOIR UNE LETTRE NE MET PAS SON AUTEUR AU CARNET. Le carnet est
// l'UNION des deux sens des présentations, et le second sens ne s'écrit que
// depuis le 06/09/2026 : qui s'est présenté avant ce jour-là n'a laissé de
// trace que chez celui qui a APPRIS son nom, et peut donc écrire à quelqu'un
// qui, lui, ne l'a pas au carnet. « Répondre » est alors GRISÉ, avec
// l'infobulle qui dit le geste qui manque. Ne le dégrise pas : le serveur
// refuserait l'envoi (« Tu n'as jamais croisé cette personne — présente-toi à
// elle d'abord. ») une fois la lettre déjà tapée.
//
// LES PERSONNES SE DÉSIGNENT PAR LEUR personnageId, le petit entier du
// registre, et les lettres par leur id de missive. AUCUN FORMID NE TRANSITE.
// Les identités viennent du serveur : un nom vide veut dire « on ne s'est pas
// présentés », et l'on affiche alors le MATRICULE — la règle du jeu entier.
//
// LES DEUX FICHIERS DU PONT :
//   - l'état arrive par GridInventory_missives_etat.txt, écrit par le client
//     skymp du même processus (missivesService.ts) à chaque poussée serveur ;
//   - les gestes partent par GridInventory_missives.txt, que le même service
//     consomme (une trame sur quinze).
//
// L'ÉCHAPPEMENT DES TEXTES, ET LES DEUX CÔTÉS LE FONT PAREIL — c'est le seul
// point du pont où l'on peut diverger sans que rien ne tombe : une colonne
// décalée ferait une ligne jetée, qui se voit ; un texte mal déséchappé, lui,
// s'affiche, simplement faux. La règle est écrite au-dessus de `Echapper` /
// `Desechapper` dans Missives.cpp, et son miroir vit dans missivesTexte.ts
// (dépôt vulkaar rp / vulkaar-engine), où elle s'éprouve dans les deux sens.
//
// L'ÉCRAN REMPLACE LES DEUX PANNEAUX de la racine, comme l'établi, le panneau
// d'appartenance et le comptoir de la banque.

#include <RE/Skyrim.h>

#include <cstdint>

namespace FUI::Missives
{
    /** Tronque les deux fichiers du pont — à kDataLoaded.
     *  L'ÉTAT AUSSI, pas seulement les gestes : un état rescapé d'un plantage
     *  ferait surgir le panneau au lancement du jeu. */
    void Initialiser();

    /** Lit l'état, réclame la lecture d'une lettre ouverte, tient le chien de
     *  garde et le compte à rebours du message. Depuis UIRoot::Tick, à CHAQUE
     *  trame, racine ouverte ou non. */
    void Tick();

    /** Le panneau entier. Appelé par UIRoot::Render À LA PLACE des deux
     *  panneaux quand Ouvert() est vrai. */
    void Dessiner();

    /** Le serveur a-t-il ouvert le panneau pour nous ? */
    [[nodiscard]] bool Ouvert();

    /** Échap : referme le panneau et le dit au serveur (geste « fermer »).
     *  Rend true si quelque chose a été fermé. */
    bool Fermer();
}
