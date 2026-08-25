#pragma once

// [vulkaar] LE JOURNAL DES SORTIES D'INVENTAIRE — jeter, détruire.
//
// Sous skymp, l'inventaire fait autorité CÔTÉ SERVEUR : un RemoveItem local
// que le serveur n'apprend pas est défait à la réapplication suivante —
// l'objet « revient tout seul » (vécu le 25/08/2026). Le client skymp du même
// processus sait parler au serveur (MsgType.DropItem, paquets custom), mais
// il ne peut pas DEVINER l'intention depuis les événements moteur : un
// containerChanged « joueur → nulle part » est aussi bien un jet qu'une
// destruction qu'une consommation.
//
// Donc LE MOD DIT CE QU'IL FAIT, et le client relaie : chaque jet et chaque
// destruction s'écrit ici (une ligne « seq \t action \t formId \t count »),
// dans un fichier que sortiesGrilleService.ts (skymp5-client) consomme.
// Fichier d'ÉTAT D'EXÉCUTION, comme GridInventory_personnage.txt : tronqué à
// chaque lancement, jamais distribué.

#include <RE/Skyrim.h>

namespace FUI::SortiesVulkaar
{
    /** Tronque le journal — à appeler à kDataLoaded, avant tout jeu. */
    void Initialiser();

    /** Note une sortie voulue par le joueur. `a_action` : "jeter" ou
     *  "detruire". Sans effet si le journal n'est pas initialisé. */
    void Noter(const char* a_action, RE::FormID a_baseId, int a_count);
}
