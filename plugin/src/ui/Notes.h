#pragma once

// [vulkaar] LES NOTES — un carnet de pages, et une page qu'on tend à qui est là.
//
// CE QUE CET ÉCRAN FAIT (propriétaire, 07/09/2026) : « on va faire un système
// de note qui sera sur F4 ; on peut écrire une note sur une page, créer une
// nouvelle page, et partager une page à la personne choisie dans un rayon de
// 5 m ». Deux colonnes : à gauche la LISTE des pages et le bouton « Nouvelle
// page », à droite la PAGE choisie — son titre, son texte, et les trois gestes
// qui la concernent (enregistrer, partager, supprimer).
//
// UN CINQUIÈME ÉCRAN, ET CHACUN DES QUATRE AUTRES DOIT L'APPRENDRE. L'établi,
// le panneau d'appartenance, le comptoir de la banque et le courrier ouvrent
// tous la racine de la grille pour se dessiner, et chacun la referme en partant
// — SAUF si un autre la tient encore. Cette liste de voisins est recopiée à
// cinq endroits (ici, dans les trois autres écrans, et dans `CloseLayer` d'
// UIRoot.cpp) et le défaut a déjà été payé DEUX fois : un écran neuf qu'on
// oublie d'ajouter se fait éteindre la racine sous les pieds, reste sans une
// trame dessinée, et son chien de garde le ferme deux secondes plus tard sans
// un mot pour le joueur. Ajouter un sixième écran, c'est six fichiers à relire.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/notes/relaisNotes.ts) :
// à qui appartient une page, ce qu'un titre et un texte ont le droit de peser
// (60 et 2 000 caractères), qui est à cinq mètres — 350 unités, MESURÉES À
// L'INSTANT DU PARTAGE. Ici on ne fait que MONTRER et DEMANDER.
//
// ── LE PLATEAU NE PORTE QU'UN SEUL TEXTE, ET C'EST TOUTE L'ARCHITECTURE ────
// Les pages sont ILLIMITÉES (décision du propriétaire, 07/09) : rien ne plafonne
// leur nombre. Un plateau qui porterait le texte de toutes grossirait donc sans
// borne. Il ne porte que la LISTE (id, titre, date, provenance) et le texte de
// la SEULE page ouverte. D'où la règle qui commande cet écran :
//
//   CLIQUER UNE PAGE ENVOIE LE GESTE `page`, ET ON ATTEND SON TEXTE. Tant qu'il
//   n'est pas arrivé, l'écran dit qu'il l'ouvre. Il n'invente pas un texte vide
//   — ce vide-là s'enregistrerait PAR-DESSUS le vrai —, et le texte de la page
//   précédente ne s'affiche jamais sous le titre d'une autre.
//
// C'EST LE SERVEUR QUI DIT QUELLE PAGE EST OUVERTE (`ouverte` + `texte`, poussés
// ensemble, lus de la même lecture du registre). L'écran le suit ; il ne décide
// pas tout seul qu'une page est chargée.
//
// ── CE QUI NE DOIT PAS SE PERDRE ──────────────────────────────────────────
// « Perdre ce qu'un joueur vient d'écrire est la seule faute que ce chantier ne
// doit pas commettre » (contrat, §1.8). Trois règles en découlent, et aucune
// n'est décorative :
//   1. LE TEXTE EN COURS D'ÉDITION NE S'EFFACE JAMAIS SUR UNE POUSSÉE SERVEUR.
//      Le serveur repousse un plateau toutes les cinq secondes quand les gens
//      marchent autour de nous (la veille sur `autour`) : si cela emportait la
//      saisie, écrire une page serait impossible en ville. Seule l'OUVERTURE
//      d'une AUTRE page remet les champs à zéro.
//   2. REFERMER ENREGISTRE. Échap envoie l'enregistrement de la page modifiée
//      AVANT le « fermer ». Changer de page, créer une page, ouvrir la liste du
//      partage : tous enregistrent d'abord, pour la même raison.
//      CE QUI FAIT DE CET ÉCRAN LE SEUL DES CINQ À ENVOYER DEUX GESTES POUR UN
//      CLIC — `ecrire` puis `page`, `creer` ou `donner` —, et les deux partent
//      dans la MÊME trame. Le relais a dû apprendre à les laisser passer tous
//      les deux : l'écriture y a sa PROPRE cadence, séparée de celle de la
//      navigation (`CADENCE_ECRITURE_MS`, relaisNotes.ts). Avec une cadence
//      unique, le second geste était refusé à tous les coups — un clic sur deux
//      ne faisait rien (relecture croisée du 07/09). Le sixième écran qui
//      voudra enregistrer en partant devra en faire autant.
//   3. LES CHAMPS NE PEUVENT PAS DÉPASSER LES BORNES DU SERVEUR. Le titre est
//      borné à 60 caractères et le texte à 2 000 À LA FRAPPE (le compteur se
//      fige, la lettre suivante n'entre pas) plutôt qu'au départ : un
//      enregistrement que le serveur refuserait pour la longueur, c'est une
//      page perdue au moment précis où l'on referme.
//
// LES PERSONNES SE DÉSIGNENT PAR LEUR personnageId, le petit entier du registre,
// et les pages par leur id de note. AUCUN FORMID NE TRANSITE. Les identités
// viennent du serveur : un nom vide veut dire « on ne s'est pas présentés », et
// l'on affiche alors le MATRICULE — la règle du jeu entier.
//
// LES DEUX FICHIERS DU PONT :
//   - l'état arrive par GridInventory_notes_etat.txt, écrit par le client skymp
//     du même processus (notesService.ts) à chaque poussée serveur ;
//   - les gestes partent par GridInventory_notes.txt, que le même service
//     consomme (une trame sur quinze). CE FICHIER SE ROGNE, contrairement à
//     ceux des quatre autres écrans : une ligne « ecrire » porte le texte
//     ENTIER d'une page (jusqu'à huit kilo-octets) et le service le relit
//     ENTIER quatre fois par seconde — 1,5 Mio coûtent 3,2 ms par passe
//     (mesuré le 07/09). Le service accuse donc réception dans l'état (ligne
//     `lu <seq>`), et la DLL vide le fichier quand tout a été lu : jamais à
//     l'aveugle, sinon on perdrait le geste écrit entre les deux. Voir
//     `RognerLesGestes`.
//
// L'ÉCHAPPEMENT DES TEXTES, ET LES TROIS CÔTÉS LE FONT PAREIL — une page a des
// paragraphes, comme une lettre. La règle est écrite au-dessus de `Echapper` /
// `Desechapper` dans Notes.cpp ; ses miroirs vivent dans `pontTexte.ts` (dépôt
// vulkaar-engine, où elle s'éprouve dans les deux sens) et dans Missives.cpp.
// C'est le seul point du pont où l'on peut diverger sans que rien ne tombe :
// une colonne décalée fait une ligne jetée, qui se voit ; un texte mal
// déséchappé, lui, s'affiche — simplement faux.
//
// ── LA PLUME : DE BELLES PAGES, ET LE RENDU N'EST QU'UNE VUE ──────────────
// « je veux que pour la partie écriture on puisse faire saut de ligne, retour à
// la ligne, mise en gras, italique, et tout ce qui permet de faire de belles
// notes » (propriétaire, 07/09/2026).
//
// LES MARQUES VIVENT DANS LE TEXTE DE LA PAGE, et nulle part ailleurs : le
// registre n'en sait rien, si bien qu'une page partagée emporte sa mise en
// forme sans qu'un seul octet de protocole ait changé. En ligne, `**gras**`,
// `*italique*`, `__souligné__`, `~~barré~~` ; en tête de ligne, `# titre`,
// `## sous-titre`, `- puce`, `> citation`, et trois tirets ou plus font un
// filet. Une ligne vide sépare deux paragraphes.
//
// IL N'Y A AUCUN CARACTÈRE D'ÉCHAPPEMENT, ET C'EST LE PONT QUI L'INTERDIT : la
// tabulation y devient une espace et l'antislash s'y double (voir `Echapper`).
// C'est donc UNE RÈGLE D'ADJACENCE qui tient ce rôle — une ouvrante n'en est
// une que collée au caractère qui suit, une fermante que collée à celui qui
// précède, et une ouvrante non fermée sur la MÊME ligne reste du texte. Ainsi
// « 2 * 3 » et « 10 h - 12 h » s'écrivent sans y penser, et personne n'a à
// apprendre à protéger une étoile.
//
// LE RENDU EST UNE VUE, JAMAIS UNE RÉÉCRITURE. `Riche::Dessiner` peint le
// texte ; il ne le retouche pas, et l'écran ne réenregistre jamais ce qu'il
// vient d'afficher. La conséquence est la seule qui compte ici : un défaut de
// l'analyse fait une page MAL PEINTE, jamais une page ABÎMÉE. C'est ce qui
// permet de toucher à la grammaire sans risquer ce que Notes.h interdit par
// ailleurs — perdre ce qu'un joueur vient d'écrire.
//
// DEUX MODES, PARCE QU'IMGUI NE LAISSE PAS LE CHOIX. Un champ de saisie ne sait
// afficher qu'UNE police et UNE couleur : le gras NE PEUT PAS s'y montrer. On
// écrit donc les marques en clair (mode Écrire) et on les lit posées (mode
// Lire) ; la bascule est une porte qui quitte la page, donc elle ENREGISTRE en
// passant, comme toutes les autres. Une page reçue s'ouvre en Lire, une page à
// nous en Écrire, et l'on va de l'un à l'autre dans les deux sens : une page
// reçue est une COPIE QUI NOUS APPARTIENT.
//
// UN ORDRE DE MARQUE NE SE POSE PAS QUAND C'EST LE TITRE QUI TENAIT LE CLAVIER,
// et cette garde-là a été payée. Le carnet a DEUX champs, ImGui n'a qu'UN SEUL
// état de saisie : dès que le titre l'a tenu, le corps ne le recycle plus et
// revient avec son curseur à ZÉRO. Un `G` cliqué pendant qu'on écrit le titre
// arrachait donc le clavier au titre et posait « **** » au TOUT DÉBUT du corps —
// quatre étoiles que le rendu ne montre même pas (une marque vide), comptées
// dans les 2 000, et une page modifiée que personne n'avait demandé de modifier.
// L'écran retient donc le DERNIER champ à avoir tenu le clavier ; le drapeau de
// la trame n'y suffisait pas, un clic vole l'`ActiveId` dès l'ENFONCEMENT, une
// trame avant que le bouton ne déclenche. Il refuse alors en une ligne : « Place
// d'abord ton curseur dans le texte de la page. » Quand PERSONNE n'a encore tenu
// le clavier, l'ordre PASSE et se pose au début du corps : c'est le geste d'une
// page neuve, et il est légitime. Voir `g_dernierClavier` dans Notes.cpp.
//
// LA BORNE DE 2 000 CARACTÈRES COMPTE LES MARQUES, et c'est honnête : le champ
// les montre, donc le joueur voit ce qui est compté. La monter serait une
// décision du propriétaire, et il faudrait la monter chez ses TROIS GARDIENS à
// la fois — `kTexteMax` ici, `TEXTE_PAGE_MAX` du relais
// (`packages/gamemode/src/domain/notes/notes.ts`) et `TEXTE_PAGE_MAX` du
// registre (`packages/comptes/src/depot.ts`). Les trois valeurs sont déclarées
// séparément et RIEN NE LES COMPARE : en monter deux ne fait tomber aucune
// épreuve, et une page acceptée à la frappe se ferait refuser à
// l'enregistrement — c'est-à-dire au moment où l'on referme.
//
// ÉCHAP FERME, ET NE PREND PLUS RIEN. Il fallait le corriger : tant qu'un champ
// a le clavier, `GridMenu` avale tout le canal des événements utilisateur, si
// bien qu'Échap n'allait pas fermer le panneau — il allait à ImGui, qui
// ANNULAIT la frappe et laissait le panneau ouvert, alors que le pied promet
// « ta page s'enregistre en se refermant ». L'écran garde donc une copie
// d'ombre de chaque champ, la restitue sur la trame de l'annulation, et demande
// la fermeture hors de la trame ImGui. Voir `GarderContreEchap` dans Notes.cpp.
//
// L'ÉCRAN REMPLACE LES DEUX PANNEAUX de la racine, comme l'établi, le panneau
// d'appartenance, le comptoir de la banque et le courrier.

#include <RE/Skyrim.h>

#include <cstdint>

namespace FUI::Notes
{
    /** Tronque les deux fichiers du pont — à kDataLoaded.
     *  L'ÉTAT AUSSI, pas seulement les gestes : un état rescapé d'un plantage
     *  ferait surgir le panneau au lancement du jeu. */
    void Initialiser();

    /** Lit l'état, tient le chien de garde et le compte à rebours du message.
     *  Depuis UIRoot::Tick, à CHAQUE trame, racine ouverte ou non. */
    void Tick();

    /** Le panneau entier. Appelé par UIRoot::Render À LA PLACE des deux
     *  panneaux quand Ouvert() est vrai. */
    void Dessiner();

    /** Le serveur a-t-il ouvert le panneau pour nous ? */
    [[nodiscard]] bool Ouvert();

    /** Échap : referme d'abord le sous-écran ouvert (la liste du partage, la
     *  confirmation de suppression), sinon ENREGISTRE la page modifiée puis
     *  referme le panneau et le dit au serveur (geste « fermer »).
     *  Rend true si quelque chose a été fermé. */
    bool Fermer();
}
