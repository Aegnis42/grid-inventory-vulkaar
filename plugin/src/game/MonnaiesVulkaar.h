#pragma once

#include <string>

// =============================================================================
//  Les trois monnaies de vulkaar — Septime, Mède, Titus.
// =============================================================================
//
//  CE QUE CE MODULE FAIT, ET POURQUOI IL NE RESSEMBLE PAS À GoldCoins.
//
//  `GoldCoins` est un MIROIR : l'or vanilla reste le grand livre, et le module
//  le recopie en pièces réelles pour qu'il occupe des cases. Quatre paliers,
//  décomposés automatiquement, d'une seule et même monnaie.
//
//  Ici c'est l'inverse sur les deux points. Nos trois monnaies sont de VRAIS
//  objets que le joueur possède — pas le reflet d'un compteur — et elles sont
//  INDÉPENDANTES : cent Titus ne deviennent pas un Mède. Le taux de change,
//  s'il y en a un, sera une décision d'économie prise ailleurs et plus tard ;
//  ce module n'en présume rien.
//
//  Et elles n'occupent AUCUNE case. Le joueur peut en porter autant qu'il veut.
//  C'est voulu : dans une économie tenue à cent pour cent par les joueurs,
//  l'argent ne doit pas être ce qui remplit un sac.
//
//  ── LE CHEMIN QU'ELLES EMPRUNTENT ─────────────────────────────────────────
//
//  Exactement celui de l'or vanilla, et pas celui des pièces du mod. La grille
//  n'a qu'une seule façon de priver une forme de tuile : sortir de la boucle
//  de collecte avant de l'inscrire, ce que fait `if (obj->IsGold()) continue;`.
//  Les pièces du mod, elles, ONT des tuiles — `IsCoinForm` y marque un
//  comportement (ni vente, ni corbeille), pas une invisibilité. Copier ce
//  chemin-là aurait donné le contraire de ce qu'on veut.
//
//  ── DEUX MODIFICATIONS QUI NE SE SÉPARENT PAS ─────────────────────────────
//
//  Sauter la collecte ne suffit pas. `MaxAcceptUnits` décide si un objet du
//  monde peut être ramassé, et il n'exempte que l'or vanilla ; sans y ajouter
//  nos monnaies, un joueur dont la grille est pleine ne pourrait plus ramasser
//  son propre argent. Les deux lignes vont ensemble ou pas du tout.
//
//  ── REPLI SILENCIEUX ──────────────────────────────────────────────────────
//
//  Si `vulkaar_accueil.esp` n'est pas chargé, `Pret()` est faux et
//  `EstMonnaie` rend toujours faux : le mod se comporte alors exactement comme
//  avant, sans un message d'erreur ni une case en moins. Ce fork doit rester
//  utilisable par quelqu'un qui n'a pas vulkaar.
// =============================================================================

namespace FUI::MonnaiesVulkaar
{
    inline constexpr int kNb = 3;   // Septime, Mède, Titus — dans cet ordre

    // kDataLoaded : résout les trois enregistrements. Échoue en silence.
    void InitForms();

    // Vrai quand les trois formes ont été trouvées.
    [[nodiscard]] bool Pret();

    // 0 Septime, 1 Mède, 2 Titus — ou -1 si la forme n'est pas des nôtres.
    [[nodiscard]] int Rang(RE::FormID a_id);

    [[nodiscard]] bool EstMonnaie(RE::FormID a_id);
    [[nodiscard]] bool EstMonnaie(RE::TESBoundObject* a_obj);

    // La forme d'un rang, pour dessiner son icône. Nul si non résolue.
    [[nodiscard]] RE::TESBoundObject* Forme(int a_rang);

    // ── les compteurs ─────────────────────────────────────────────────────
    //
    // Mis en cache pendant la collecte, comme `g_gold`, plutôt que relus à
    // chaque image : la barre est dessinée soixante fois par seconde et
    // parcourir tout l'inventaire à chaque fois pour trois nombres serait du
    // gaspillage pur.

    // À appeler là où `g_gold` est remis à zéro, pas ailleurs : sans ça, une
    // monnaie tombée à zéro garderait son dernier compte à l'écran.
    void RemettreComptesAZero();

    // Rend vrai si la forme est une des nôtres — l'appelant enchaîne alors sur
    // `continue`, et l'objet n'obtient jamais de tuile.
    bool NoterSiMonnaie(RE::TESBoundObject* a_obj, int a_compte);

    [[nodiscard]] int Compte(int a_rang);
}
