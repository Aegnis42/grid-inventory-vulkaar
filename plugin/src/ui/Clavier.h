#pragma once

// [vulkaar] LE CLAVIER DÉCLARÉ — les lettres que le joueur tape dans NOS champs.
//
// Skyrim ne connaît que des POSITIONS de touches (codes de balayage). Jusqu'ici
// la lettre tapée dans la recherche de l'établi ou le renommage d'un preset
// venait de Windows : WM_CHAR, c'est-à-dire ToUnicode sur la disposition du
// PROCESSUS du jeu — que SkyrimPlatform peut faire tourner à l'insu du joueur
// (Maj+Ctrl / Maj+Alt dans le tchat). Un joueur qui a déclaré QWERTY dans le
// launcher, mais dont le jeu croit à l'AZERTY, tape « q » et lit « a ».
// Décision du propriétaire (02/09/2026) : LA DISPOSITION DÉCLARÉE DANS LE
// LAUNCHER FAIT FOI dans nos menus.
//
// Ce module traduit donc lui-même : la position (le sc) et la table de la
// disposition déclarée disent quelle lettre s'écrit. La table n'est PAS écrite
// ici : ClavierTables.gen.h est dérivé de LA table de la page CEF
// (vulkaar rp / packages/ui/src/clavier.js) par
// scripts/generer-clavier-greffon.mjs. Deux tables écrites à la main
// divergeraient un jour, en silence — une lettre se corrige là-bas, puis on
// régénère. Traduire() est le MIROIR EXACT de traduire() là-bas : les deux
// consommateurs doivent rendre la même lettre pour la même frappe.
//
// Ce qu'on traduit : les touches qui font des mots — lettres, rangée des
// chiffres, ponctuation du bloc principal, espace. Entrée, Échap, flèches,
// pavé numérique, F1-F12 ne sont PAS dans la table : Traduire() rend false et
// Windows/ImGui font comme avant. Un raccourci (Ctrl+A, Ctrl+V) n'est pas une
// lettre : l'appelant ne nous le soumet pas. « autre » = « je ne sais pas » :
// rien n'est traduit, Windows décide, comme avant.
//
// Fidèle aux PILOTES Windows (KBDUS, KBDFR, KBDGR), mesurés : AltGr+Maj sans
// colonne propre n'écrit RIEN (SHFT_INVALID — seul ẞ en a une) ; Verr. Maj
// n'inverse que ce que chaque pilote inverse (le verdict par touche est dans
// la table générée, pas une règle ici).
//
// Les touches mortes (^ ¨ ~ ` ´) attendent la lettre suivante, comme sous
// Windows : ^ puis e donne ê ; ^ puis espace donne ^ ; ^ puis x donne ^x ; deux
// mortes de suite sortent leurs deux accents. Devant une touche HORS TABLE,
// même chose que Windows et que la page : ^ puis Entrée (Tab, Échap) sort
// l'accent PUIS laisse passer la touche ; ^ puis Retour arrière efface
// l'attente et rien d'autre (TerminerMorte / AnnulerMorte ci-dessous).
// Flèches, Suppr et modificateurs ne touchent pas l'attente.
//
// Ce que le greffon fait de tout cela : UIRoot.cpp, les deux routes des
// caractères (la fenêtre — WndProcThunk — et le repli par sondage —
// PollTypedCharacters).

#include <cstdint>
#include <iterator>   // std::size : l'en-tête généré s'en sert sans l'inclure
#include "ui/ClavierTables.gen.h"

namespace vk::clavier
{
    /** La disposition déclarée par le joueur, ou nullptr : fichier absent ou
     *  illisible, valeur « autre » ou inconnue — Windows décide alors, comme
     *  avant. Lue UNE fois par processus : le launcher l'écrit avant de lancer
     *  le jeu, et un joueur ne change pas de clavier en cours de partie. */
    const tables::Disposition* Declaree();

    /** Ce qu'une frappe écrit : 0, 1 ou 2 unités UTF-16, terminées par zéro. */
    struct Frappe
    {
        wchar_t texte[3];
        int     n;
    };

    /** Traduit une position de touche selon la disposition déclarée.
     *
     *  Rend false quand la touche n'est pas à nous — aucune disposition
     *  déclarée, ou rien à cette position (Entrée, pavé numérique...) :
     *  l'appelant laisse Windows/ImGui faire comme avant.
     *
     *  Rend true quand la touche est PRISE : `out` porte alors 0, 1 ou 2
     *  caractères. Zéro = rien ne s'écrit, mais la touche est consommée —
     *  colonne absente ou muette (AltGr+E sur un clavier sans AltGr, AltGr+Maj
     *  sur toute touche sauf ß), ou touche morte qui vient d'être posée et
     *  attend sa lettre. */
    bool Traduire(std::uint8_t sc, bool maj, bool altgr, bool verrMaj, Frappe& out);

    /** UNE MORTE EN ATTENTE devant une touche HORS TABLE — le miroir de la fin
     *  de surTouche() côté page, et ce que fait Windows.
     *
     *  Entrée, Tab et Échap font SORTIR l'accent seul, puis agissent (Entrée
     *  envoie l'accent, Tab passe au champ suivant...). Rend true et pose
     *  l'accent dans `out` quand `vk` (le code de touche virtuelle, WPARAM du
     *  WM_KEYDOWN) est l'une de ces trois touches ET qu'un accent attendait :
     *  l'appelant le pousse à ImGui AVANT la touche, puis laisse la touche
     *  suivre son cours normal. Rend false sinon : touche ordinaire, rien à
     *  faire ici. */
    bool TerminerMorte(int vk, Frappe& out);

    /** Retour arrière : sous Windows l'accent sortirait puis serait effacé —
     *  net, RIEN. Rend true quand `vk` est Retour arrière ET qu'un accent
     *  attendait : l'attente est effacée, et l'appelant AVALE la touche (rien
     *  d'autre ne doit s'effacer). Rend false sinon : la touche efface comme
     *  d'habitude.
     *
     *  Flèches, Suppr, modificateurs : ni TerminerMorte ni AnnulerMorte ne les
     *  connaissent — l'attente leur survit, comme sous Windows. */
    bool AnnulerMorte(int vk);

    /** Efface l'accent en attente d'une touche morte. À appeler quand le champ
     *  perd le focus ou que le menu se ferme : un accent posé ici ne doit pas
     *  tomber dans le champ suivant. */
    void OublierMorte();
}
