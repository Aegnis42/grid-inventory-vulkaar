#pragma once

// [vulkaar] LA FENÊTRE D'ÉCHANGE — la vitrine du troc entre joueurs.
//
// TOUT L'ARBITRAGE EST AU SERVEUR (vulkaar rp, domain/echange) : possession,
// accords, bascule atomique. Ici on ne fait que MONTRER et DEMANDER :
//   - l'état arrive par GridInventory_echange_etat.txt, écrit par le client
//     skymp du même processus (echangeService.ts) à chaque poussée serveur ;
//   - les gestes (répondre, offrir, valider, annuler) partent par
//     GridInventory_echange.txt, que le même service consomme.
// Deux fichiers TSV nus — le pont éprouvé du journal des sorties, aucun JSON
// à parser en C++.
//
// L'OFFRE EST VIRTUELLE : glisser un objet dans son panneau ne déplace RIEN
// (Grid::PorteVersEchange abandonne le carry, l'objet n'a jamais quitté
// l'inventaire). Les monnaies s'offrent par leurs trois cases dédiées.
//
// LE MENU D'INTERACTION vit aussi ici : la touche X (client) écrit son état
// dans GridInventory_interaction.txt (seq/ouvert/aCible/étiquette), la DLL
// dessine « Se présenter / Se présenter à tous / Échanger » et répond par
// les gestes du même nom sur le pont des gestes. Un chien de garde prévient
// le client quand la racine s'est refermée sous le menu (Échap).

#include <RE/Skyrim.h>

namespace FUI::Echange
{
    /** Tronque le fichier des gestes — à kDataLoaded. */
    void Initialiser();

    /** Lit l'état, pousse l'offre en attente. Depuis UIRoot::Tick. */
    void Tick();

    /** Invitation, fenêtre d'échange, toasts. Depuis UIRoot::Render,
     *  après LootBarter::DrawWindows. */
    void DrawFenetres();

    /** Une session (invitation ou échange) est-elle en cours ? */
    [[nodiscard]] bool Actif();
}
