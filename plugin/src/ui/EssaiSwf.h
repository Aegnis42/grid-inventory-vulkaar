#pragma once

// [vulkaar] L'ESSAI DU SWF — une question, et une seule.
//
// LE SCALEFORM DE SKYRIM ACCEPTE-T-IL UN SWF QUE NOUS AVONS FABRIQUÉ ?
//
// C'est le verrou de tout un chantier. Faire nos interfaces « comme SkyUI »
// suppose de charger nos propres films ; or Scaleform n'est qu'une
// réimplémentation PARTIELLE de Flash, et rien ne garantit qu'il lise ce que
// FFDec écrit. Tant que ce verdict n'est pas tombé, rien ne doit être conçu
// dessus. Cet essai le fait tomber en une session de jeu.
//
// CE QUI EST ÉPROUVÉ, et rien d'autre :
//   1. BSScaleformManager::LoadMovieEx rend-il vrai sur notre fichier ?
//   2. Le moteur relit-il les dimensions, la version et le nombre d'images
//      que nous avons écrites ? C'est la preuve qu'il a VRAIMENT analysé le
//      film, et pas seulement ouvert le fichier.
//
// Le film d'essai, Data/Interface/vulkaar_essai.swf, est né d'un XML écrit à
// la main : 48 octets sur le disque, signature CWS (zlib), version 8,
// 1280 x 720 px, 30 i/s, UNE image. Corps décompressé de 36 octets, cinq
// balises : FileAttributes, SetBackgroundColor, DoAction, ShowFrame, End —
// et rien après le End. Décodé octet par octet le 29/08. AUCUN octet de
// Bethesda — c'est tout l'intérêt, puisque c'est la dérivation d'un swf du jeu
// qui avait fait refuser cette voie le 23/08/2026, pas la technologie.
//
// Touche F9. Tout part au journal, préfixe [ESSAI-SWF].
//
// CE FICHIER EST JETABLE : il se retire dès que la question est tranchée.

#include <RE/Skyrim.h>

namespace FUI::EssaiSwf
{
    /** Inscrit le menu d'essai auprès du moteur — à kDataLoaded. */
    void Initialiser();

    /** F9 : ouvre le menu, ou le referme s'il est déjà là. */
    void Basculer();
}
