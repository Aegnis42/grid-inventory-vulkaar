#pragma once

// [vulkaar] LA DURABILITÉ — la table des maximums, et l'échelle de la jauge.
//
// UN OBJET DE LA FORGE (vulkaar_forge.esp) porte une jauge de points qui
// s'use aux coups et se répare à l'établi. Le serveur en est le seul juge ;
// le greffon ne fait que LIRE deux choses pour l'afficher :
//
//   1. LE MAXIMUM de chaque forme, dans une table dérivée que le générateur
//      de l'esp écrit à côté de lui et que le paquet client embarque :
//      Data/SKSE/Plugins/GridInventory_durabilite.txt. Une ligne d'en-tête
//      « # vulkaar_forge.esp <sha court> <n> », puis « <idLocalHex6> TAB
//      <max> ». Lue UNE fois à kDataLoaded : les idLocaux se résolvent en
//      FormID d'exécution par TESDataHandler::LookupFormID, ce qui exige que
//      l'ordre de chargement soit connu — il l'est à ce moment-là et jamais
//      avant. Aucune règle de barème n'est recopiée ici : la DLL lit une
//      table, elle ne calcule rien (contrat du 08/09/2026, §3).
//
//   2. LA VALEUR COURANTE, dans l'ExtraHealth de l'exemplaire — le seul
//      porte-état par exemplaire que le moteur nous laisse. L'échelle est
//      `health = 1 + (points + 1) / 100 000` : toujours dans [1,00001 ;
//      1,09999], donc jamais 1,0 (le moteur efface l'extra) et jamais 1,1 (le
//      moteur allume le suffixe « (Raffiné) », mesuré dans server-run.log).
//      Une entrée NUE vaut le maximum : un objet forgé ne porte rien à sa
//      naissance, seule l'usure pose un health.
//
// LES JUMEAUX de cette échelle : packages/shared/src/durabilite.ts (dépôt
// vulkaar rp, côté gamemode) et skymp5-client/src/sync/sante.ts (dépôt
// vulkaar-engine, côté client). Trois copies assumées — aucun des trois
// dépôts n'importe les autres — qui partagent UN nombre magique, 100 000, et
// deux formules. Changer l'une sans les deux autres casse l'ensemble en
// silence : c'est écrit ici pour que personne ne le fasse.
//
// SUR TOUT PONT TEXTE (les TSV de l'établi et de l'échange) la santé voyage
// en CENT-MILLIÈMES ENTIERS, `round((health − 1) × 100 000)`, jamais en
// float : 0 = nue. Points = cent-millièmes − 1.
//
// REPLI SILENCIEUX : sans la table (ou sans l'esp), MaxDe ne répond jamais et
// la grille garde sa ligne « Tempered » d'origine. Ce fork reste utilisable
// par quelqu'un qui n'a pas vulkaar.

#include <RE/Skyrim.h>

#include <cstdint>
#include <optional>

namespace FUI::Durabilite
{
    /** Le diviseur commun aux trois copies (voir le cartouche). */
    inline constexpr int kDiviseur = 100000;

    /** kDataLoaded : lit la table et résout les idLocaux. Échoue en silence
     *  (une ligne INFO au journal), et alors MaxDe ne répond jamais. */
    void Initialiser();

    /** La table a-t-elle été lue et au moins une forme résolue ? */
    [[nodiscard]] bool Prete();

    /** Le maximum de la jauge de cette forme, ou rien si elle n'est pas de
     *  la forge (ou si la table manque). C'est LE test « est-ce un objet à
     *  durabilité » : la grille et l'échange n'en ont pas d'autre. */
    [[nodiscard]] std::optional<std::uint32_t> MaxDe(RE::FormID a_id);

    /** `round((health − 1) × 100 000)` — la forme que prend la santé sur un
     *  pont texte. 0 pour une entrée nue (pas d'ExtraHealth, ou health ≤ 1).
     *  Les valeurs de l'ancien régime (1,1 … 1,8 = qualité des lingots)
     *  passent telles quelles : 10 000 … 80 000. */
    [[nodiscard]] int CentMilliemesDe(float a_health);

    /** Les points de la jauge : `centMilliemes − 1`, borné à [0 ; max].
     *  Nue (0) → max, par convention (voir le cartouche). */
    [[nodiscard]] std::uint32_t PointsDe(float a_health, std::uint32_t a_max);

    /** La même chose depuis des cent-millièmes déjà entiers (une ligne de
     *  pont) — sans repasser par un float, qui n'ajouterait qu'un arrondi. */
    [[nodiscard]] std::uint32_t PointsDeCentMilliemes(int a_centMilliemes, std::uint32_t a_max);
}
