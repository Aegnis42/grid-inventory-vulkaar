#include "api/HostApi.h"
#include "game/BagFilter.h"
#include "game/Census.h"
#include "game/Costume.h"
#include "game/DeltaWatch.h"
#include "game/Ledger.h"
#include "game/MonnaiesVulkaar.h"
#include "game/SortiesVulkaar.h"
#include "ui/Echange.h"
#include "ui/Etabli.h"
#include "ui/Appartenance.h"
#include "ui/EssaiSwf.h"
#include "game/WornLedger.h"
#include "game/DualRing.h"
#include "game/GoldCoins.h"
#include "ui/Editor.h"
#include "ui/Fallback.h"
#include "ui/Lang.h"
#include "ui/Theme.h"
#include "ui/WinManager.h"
#include "ui/Loadout.h"
#include "ui/Grid.h"
#include "ui/LootBarter.h"
#include "ui/IconCache.h"
#include "ui/Sfx.h"
#include "ui/ItemPreview.h"
#include "ui/UIRoot.h"
#include "ui/Wheeler.h"

#include <cmath>
#include <cstdio>

#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <vector>
#include <string>
#include <unordered_map>
#include <vector>

// [vulkaar] TOUT CE QUE LE JEU ECOUTE, SAUF DE QUOI SORTIR.
//
// Une liste NOMMEE de drapeaux oublie toujours quelque chose : la premiere
// version citait combat, activation, saut, accroupi et POV — et le
// proprietaire a rapporte que C declenchait encore la marche auto (kMovement)
// et que E passait quand meme. La regle qu'il a posee est plus simple que
// n'importe quelle liste : « quand je suis dans un de nos menus, bloque les
// touches recues par le jeu ». On coupe donc TOUT, et on ne garde que les deux
// drapeaux qui permettent d'en sortir : kMenu (Echap, la touche d'inventaire)
// et kConsole. Les ecrans du greffon, eux, ne passent pas par ce masque : ils
// lisent ImGui, qui recoit le clavier par une autre route.
constexpr RE::ControlMap::UEFlag kControlesSuspendusParLaGrille = static_cast<RE::ControlMap::UEFlag>(
    static_cast<std::uint32_t>(RE::ControlMap::UEFlag::kAll) &
    ~(static_cast<std::uint32_t>(RE::ControlMap::UEFlag::kMenu) |
      static_cast<std::uint32_t>(RE::ControlMap::UEFlag::kConsole) |
      static_cast<std::uint32_t>(RE::ControlMap::UEFlag::kInvalid)));

// ============================================================================
//  GridInventory - Mabinogi/Diablo-style tetris inventory (ImGui)
//
//  The UI lives in src/ui/ (GridInventoryMenu + UIRoot + Grid/Equip/Editor;
//  icons are captured off the engine's Inventory3DManager render - see
//  ui/ItemPreview). This file owns the game-side data and wiring: item
//  defs / categories / presets, the InventoryMenu intercept, raw input,
//  and the per-frame update hook.
// ============================================================================

namespace
{
    bool g_planBPendingOpen = false;   // open our menu once InventoryMenu fully closed
    // ⑫ — the vanilla menu we swallowed and still owe a close for; empty =
    // nothing owed. A NAME rather than a flag because three different screens
    // come through the same door (see MenuCloseEchoTick).
    std::string_view g_echoMenu{};
    bool g_pendingPartnerOpen = false; // open our grid once Container/BarterMenu fully closed (loot/barter)
    bool g_movementOff = false;        // we disabled the movement handler (text input)
    // [vulkaar] les controles du jeu sont-ils coupes de NOTRE fait ? (voir le
    // crochet d'Update : reconciliation par trame, jamais bascule aux deux bords)
    bool g_controlesCoupes = false;
    bool g_reopenAfterMsg = false;     // we stepped aside for a MessageBox (poison confirm)

    // ★★★HOP TO THE VANILLA INVENTORY AND BACK, WITHOUT LEAVING THE GAME.
    //
    // GridInventory_vanilla.txt already did this, but reaching it meant
    // alt-tabbing out to touch a file. A key makes the comparison immediate:
    // open ours, open the engine's, and the SAME log holds both -- which is
    // the only way to tell "our path is wrong" from "the engine does this
    // too" for a report about one particular item.
    //
    // Written from the input thread, read from the UI thread, hence atomic.
    std::atomic<bool> g_vanillaKey{ false };

    // ★★★THE KEY IS UNASSIGNED UNLESS SOMEONE ASSIGNS IT.
    //
    // Handing the inventory to the engine mid-session is a diagnostic, not a
    // feature: a player who hits it by accident is left wondering why their
    // inventory suddenly looks different. So the scancode lives in
    // GridInventory_ui.ini beside the other test switches --
    //
    //     !vanillakey = 87        (87 = 0x57 = F11)
    //
    // -- and ships as 0, which matches no key. The mechanism stays whole, so
    // it is also something a REPORTER can be handed: "add this line, press
    // F11, tell us whether vanilla does the same thing." That comparison is
    // what pinned the book bug to us rather than to the engine.
    //
    // Read from UIRoot rather than a file: this sits on the raw input path,
    // where a filesystem call would run on every key a player ever presses
    // (원칙 3).

    // raw RefHandle -> reference (ContainerMenu/BarterMenu return a raw handle)
    RE::TESObjectREFR* HandleToRef(RE::RefHandle a_handle)
    {
        if (a_handle == 0) return nullptr;
        // ★the NG line exposes this as a free function; it used to be reached
        // through RE::Offset, which no longer exists there
        RE::NiPointer<RE::TESObjectREFR> ptr;
        RE::LookupReferenceByHandle(a_handle, ptr);
        return ptr.get();
    }

    // Typing into a text field (rename / preset name) must not leak A/D/W/S
    // into the movement handler. Re-asserted per frame from the Update hook:
    // the engine re-enables handlers on its own (equip / player-3D rebuild).
    void SetMoveInput(bool a_enable)
    {
        auto* pc = RE::PlayerControls::GetSingleton();
        if (pc && pc->movementHandler) {
            pc->movementHandler->inputEventHandlingEnabled = a_enable;
        }
    }
    void LockpickReopenTick();      // defined below (lockpick auto-open fallback)
    void RecolteTick();             // [vulkaar] defini plus bas (recolte par metier)
    void RecolteInitialiser();      // [vulkaar] page blanche a chaque session
    void PortesTick();              // [vulkaar] defini plus bas (noms des portes de maison)
    void PortesInitialiser();       // [vulkaar] page blanche a chaque session
    void MenuCloseEchoTick();        // defined below (⑫ — the close nobody heard)

    // ---- PlayerCharacter::Update vtable hook (index 0xAD) ----
    struct UpdateHook
    {
        static void thunk(RE::PlayerCharacter* a_this, float a_delta)
        {
            func(a_this, a_delta);
            // text input owns the keyboard: block WASD from moving the player
            if (FUI::UIRoot::IsTextInputActive()) {
                SetMoveInput(false);
                g_movementOff = true;
            } else if (g_movementOff) {
                SetMoveInput(true);
                g_movementOff = false;
            }
            // [vulkaar] LES CONTROLES DU JEU, TANT QU'UN DE NOS MENUS EST OUVERT.
            //
            // Notre menu ne met pas le jeu en pause (voulu en multijoueur) et
            // son contexte kInventory ne lie ni E, ni Espace, ni Ctrl, ni F :
            // ces touches retombaient en gameplay — une porte s'ouvrait, le
            // personnage sautait, pendant qu'on tapait dans le panneau d'une
            // maison.
            //
            // ★RECONCILIE A CHAQUE TRAME, jamais bascule aux deux bords. La
            // premiere ecriture coupait a l'ouverture du menu et rendait a sa
            // fermeture ; le 03/09/2026 l'activation est restee coupee et le
            // proprietaire ne pouvait plus ouvrir une porte du tout. Ici, quoi
            // qu'il arrive — un ecran ferme par une autre voie, un rechargement
            // a chaud, un plantage d'ecran — la trame suivante remet les choses
            // en place. Meme forme que le blocage du deplacement ci-dessus, et
            // pour la meme raison.
            {
                auto* ui = RE::UI::GetSingleton();
                auto* cm = RE::ControlMap::GetSingleton();
                const bool notreMenu = ui && ui->IsMenuOpen("GridInventoryMenu"sv);
                if (cm) {
                    if (notreMenu) {
                        // ★REAFFIRME, jamais bascule sur transition : le client
                        // skymp ecrit dans le MEME masque global
                        // (Game.enablePlayerControls, appele par le sas, les
                        // emotes, la camera). Il rouvrirait sous nos pieds ce
                        // qu'on vient de fermer, et un seul appel a l'ouverture
                        // ne s'en apercevrait jamais. Ici on repose le masque
                        // des qu'un seul de ses drapeaux est revenu.
                        if (cm->enabledControls.any(kControlesSuspendusParLaGrille)) {
                            cm->ToggleControls(kControlesSuspendusParLaGrille, false, true);
                            if (!g_controlesCoupes) {
                                g_controlesCoupes = true;
                                SKSE::log::info("[INV] controles du jeu suspendus (notre menu ouvert)");
                            }
                        }
                    } else if (g_controlesCoupes) {
                        cm->ToggleControls(kControlesSuspendusParLaGrille, true, true);
                        g_controlesCoupes = false;
                        SKSE::log::info("[INV] controles du jeu rendus (notre menu ferme)");
                    }
                }
            }
            // apply capture defs + park the preview model BEFORE this frame
            // renders. While the menu is open GridInventoryMenu::AdvanceMovie
            // drives Tick - this path covers the frames where it is closed.
            // [vulkaar] la garde est devenue NÉCESSAIRE : le menu ne met plus
            // pause, donc ce hook tourne aussi menu ouvert — sans elle,
            // UIRoot::Tick courrait deux fois par trame.
            {
                auto* ui = RE::UI::GetSingleton();
                if (!ui || !ui->IsMenuOpen("GridInventoryMenu"sv)) {
                    FUI::UIRoot::Tick();
                }
            }
            // ★Every tick, open or not: the CLOSE animation has to keep running
            // after the hotkey is already released, and it is what finally takes
            // the overlay menu down.
            FUI::Wheeler::Tick();
            LockpickReopenTick();      // lockpick auto-open fallback
            RecolteTick();              // [vulkaar] la liste des recoltes interdites
            PortesTick();               // [vulkaar] les noms des portes de maison
            MenuCloseEchoTick();        // ⑫ — the close, when it is true
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            // ★★★THE SLOT MOVES IN VR. CommonLibSSE splits it itself --
            // Actor.cpp: RelocateVirtual<&Actor::Update>(0x0AD, 0x0AF, ...).
            // Writing the SE/AE index on a VR runtime replaced Resurrect (VR
            // 0x0AD), whose signature is nothing like ours, and left the real
            // Update unhooked: no Tick ever ran and the game died the first
            // time anything was resurrected. Silent on SE/AE, fatal on VR.
            //
            // This build is ENABLE_SKYRIM_VR=OFF (EXCLUSIVE_SKYRIM_FLAT), so
            // IsVR() is a constant false here and only the 0xAD branch is ever
            // taken. The split stays: it costs nothing, and it is the one line
            // that has to be right on the day the option is turned back on.
            func = vtbl.write_vfunc(REL::Module::IsVR() ? 0xAF : 0xAD, thunk);
        }
    };

    // ---- Capacity system (Mabinogi rule): no free cells -> no pickup ----
    // PlayerCharacter::PickUpObject vtable hook (0xCC on SE/AE, 0xCE on VR): the manual
    // world-pickup path. Scripted/quest AddItem is deliberately NOT blocked
    // (bouncing those would break quests); such items overflow into extra
    // grid rows instead.
    struct PickUpHook
    {
        static void thunk(RE::Actor* a_this, RE::TESObjectREFR* a_object, std::int32_t a_count,
                          bool a_arg3, bool a_playSound)
        {
            if (a_this && a_this->IsPlayerRef() && a_object) {
                // G2: a Coin_Sack world ref IS gold — consume it as ledger
                // gold instead of picking up the sack item
                if (FUI::GoldCoins::TryPickUpSack(a_object)) {
                    return;
                }
                if (auto* base = a_object->GetBaseObject();
                    base && !FUI::Grid::CanFitNewItem(base)) {
                    FUI::Sfx::FailNote(FUI::Lang::T(FUI::Lang::Str::InventoryFull));
                    return;   // blocked: the reference stays in the world
                }
            }
            func(a_this, a_object, a_count, a_arg3, a_playSound);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            // ★Same split as UpdateHook above -- CommonLibSSE's Actor.cpp has
            // RelocateVirtual<&Actor::PickUpObject>(0x0CC, 0x0CE, ...). On VR
            // the SE/AE index is OnArmorActorValueChanged.
            func = vtbl.write_vfunc(REL::Module::IsVR() ? 0xCE : 0xCC, thunk);
        }
    };

    void NotifyInventoryFull();   // defined below (throttled toast)

    // G2: Coin_Sack activation intercept at the TESObjectMISC::Activate slot —
    // one level ABOVE PickUpObject, so TrueHUD's Recent Loot (which wraps
    // PickUpObject outside our hook) never sees a "Coinpurse" pickup. The
    // sack converts straight into ledger gold.
    struct SackActivateHook
    {
        static bool thunk(RE::TESObjectMISC* a_this, RE::TESObjectREFR* a_targetRef,
                          RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
                          RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            if (a_activatorRef && a_activatorRef->IsPlayerRef()) {
                if (FUI::GoldCoins::TryPickUpSack(a_targetRef)) {
                    return true;   // consumed as gold — no item pickup happens
                }
                // capacity gate for plain MISC items, at the SAME pre-TrueHUD
                // level as CapacityActivateHook (the sack conversion above
                // must run first: gold ignores grid space)
                if (!FUI::Grid::CanFitNewItem(a_this)) {
                    NotifyInventoryFull();
                    return false;   // blocked: the reference stays in the world
                }
            }
            return func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_TESObjectMISC[0] };
            func = vtbl.write_vfunc(0x37, thunk);
        }
    };

    // W2: equipping/unequipping frees/consumes grid cells (worn items leave the
    // board) — favorites/hotkey equips happen with our menu CLOSED, so the
    // rebuild path never sees them; recompute the capacity state instead.
    class EquipSink : public RE::BSTEventSink<RE::TESEquipEvent>
    {
    public:
        static EquipSink* GetSingleton()
        {
            static EquipSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override
        {
            // ★1.4/B0: this sink used to throw its delta away -- it read
            // IsPlayerRef() and nothing else. B0 measured what the event can
            // and cannot say (§8-2): uniqueID is always zero, so it never names
            // the unit -- but `equipped` is exactly the one bit we need.
            FUI::DeltaWatch::OnEquip(a_event);
            if (a_event && a_event->actor && a_event->actor->IsPlayerRef()) {
                // ★1.4/B3: an unequip puts a unit BACK on the board, and that
                // is the only direction without an optimistic path. Includes
                // the engine's own slot-conflict removals, which our equip code
                // never sees. Deferred to the main thread: these arrive on
                // whatever thread the engine was on.
                // ★B3 BODY: the partial update first -- it adds the returning
                // unit through the same walk and tile factory the rebuild
                // uses, scoped to one form, and declines (with a logged
                // reason) whenever it is not certain. Only a decline still
                // costs a full rebuild.
                if (!a_event->equipped) {
                    const RE::FormID fid = a_event->baseObject;
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        // B4-2 observation: the ledger hears every player
                        // unequip, including the engine's own slot-conflict
                        // removals -- the case our equip code never sees.
                        FUI::WornLedger::OnUnequip(fid);
                        // B4-4: a landed equip record's story ends at the
                        // unequip -- retire it before the partial reconcile
                        // reads the exclusions.
                        FUI::Grid::ReleaseLandedPendingEquip(fid);
                        if (!FUI::Grid::OnFormDelta(fid)) {
                            FUI::Grid::RequestRebuild();
                        }
                    });
                } else {
                    // ★The helmet that never showed (user report): while OUR
                    // menu holds the game paused, the engine's actor update --
                    // the pass that applies a finished equip to the biped 3D --
                    // does not run. ProcessPending forces one refresh two UI
                    // ticks after the request, but that is a GUESS about when
                    // the equip data has settled; when the engine applied
                    // late, the refresh redrew the old biped and the worn
                    // helmet stayed invisible until something else forced one
                    // (cycling loadout presets was the reported healer --
                    // Loadout.cpp forces the same refresh). THIS event is the
                    // engine saying the equip IS applied, the exact moment the
                    // frame count tried to approximate, so the refresh anchors
                    // here. Marshalled: equip events arrive on arbitrary
                    // threads (rule 4), and the menu/3D checks belong on the
                    // main thread anyway. Outside our menu the game is
                    // unpaused and refreshes itself -- skip.
                    const RE::FormID fid = a_event->baseObject;
                    SKSE::GetTaskInterface()->AddTask([fid]() {
                        // B4-2 observation: BEFORE the menu gate below -- the
                        // ledger listens whether our menu is open or not.
                        FUI::WornLedger::OnEquip(fid);
                        // B4-2c: same confirm, delivered to the equip queue --
                        // the worn-clock flips here now, not when our call
                        // returned.
                        FUI::Grid::NoteEquipLanded(fid);
                        // ★Ring session: a same-form SELF-SWAP fires OFF
                        // before ON, and the OFF-side reconcile ran while this
                        // request was still un-landed -- the displaced spare
                        // cancelled against the in-flight record ("nothing
                        // fresh") and stayed hidden until a sweep (user
                        // report: the doffed half appears late). Landed, the
                        // worn unit belongs to skipWorn, so the same one-form
                        // reconcile now sees the spare and draws it. Declines
                        // are IGNORED on this side -- the equip direction
                        // never needed a rebuild (B3, measured), and e.g. a
                        // torch's "still worn" decline must not start one.
                        // ★Still ignored -- but SAID, because it used not to
                        // be. The shared decline line claimed "full rebuild"
                        // for every caller, and this one does not rebuild, so
                        // a stale tile after an equip looked in the log like a
                        // tile a rebuild had already been past. Which of the
                        // two it is decides where to look next.
                        if (!FUI::Grid::OnFormDelta(fid)) {
                            // ★★ASK WHETHER A CLICK ALREADY DID IT.
                            //
                            // The decline used to be dropped outright, on the
                            // reasoning above -- true for an equip started by
                            // a grid click, which takes the tile off the board
                            // at the moment of the click. An equip from the
                            // QUICK WHEEL has no such click, and neither does
                            // a hotkey or a script: nothing removes the tile,
                            // and throwing the decline away left it standing
                            // until some unrelated rebuild wandered past.
                            // Measured -- a wheel equip, no removal, decline
                            // swallowed, and only luck cleaning up after.
                            if (FUI::Grid::ClaimOptimisticRemove(fid)) {
                                SKSE::log::info("[B3] equip-side decline ignored "
                                    "({:08X}) -- the click already took the tile", fid);
                            } else {
                                SKSE::log::info("[B3] equip-side decline escalated "
                                    "({:08X}) -- nothing removed the tile", fid);
                                FUI::Grid::RequestRebuild();
                            }
                        }
                        auto* ui = RE::UI::GetSingleton();
                        if (!ui || !ui->IsMenuOpen("GridInventoryMenu"sv)) return;
                        auto* player = RE::PlayerCharacter::GetSingleton();
                        if (!player || !player->Is3DLoaded()) return;
                        if (auto* proc =
                                player->GetActorRuntimeData().currentProcess) {
                            proc->Update3DModel(player);
                        }
                    });
                }
                FUI::Grid::MarkCapacityDirty();
                // The active preset IS what the player is wearing, so anything
                // that changes the worn gear changes that tab.
                FUI::Loadout::MarkActiveStale();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void NotifyInventoryFull()
    {
        // take-all spams one container event per stack — throttle the toast
        static std::uint32_t s_last = 0;
        const std::uint32_t now = RE::GetDurationOfApplicationRunTime();
        if (now - s_last > 700) {
            s_last = now;
            FUI::Sfx::FailNote(FUI::Lang::T(FUI::Lang::Str::InventoryFull));
        }
    }

    // ── [vulkaar] LA RECOLTE RESERVEE AU METIER ───────────────────────────
    //
    // Demande du proprietaire (30/08/2026) : « je veut que il ne puisse pas
    // interagire avec si il ne sont pas herboriste ».
    //
    // POURQUOI C EST ICI ET PAS SUR LE SERVEUR, et c est mesure : pour la
    // FLORE le moteur du client cueille TOUT SEUL — animation, don, marquage,
    // repousse — et ne previent le serveur qu une trame plus tard. Le veto
    // serveur arrive donc APRES : il retire bien l ingredient de l inventaire
    // qui fait foi, mais la plante, elle, est deja cueillie. Le joueur perd la
    // plante et ne recoit rien. (Pour les VEINES le chemin natif est inerte,
    // d ou un veto serveur qui « marche » — ce n est pas la meme mecanique.)
    //
    // Le seul point qui refuse AVANT que rien ne soit consomme est ce hook.
    // Le serveur calcule la liste — lui seul sait quels gestes sont ouverts a
    // ce joueur, a ce niveau — et le client TypeScript la pose en clair, un
    // formId de base par ligne en hexa, comme il pose deja l identifiant de
    // personnage pour la grille.
    //
    // On lit l INTERDIT, pas le permis : une base absente du fichier n est
    // pas a nous, et rien ne doit la bloquer.
    namespace
    {
        constexpr const char* kCheminRecolte =
            "Data/SKSE/Plugins/GridInventory_recolte.txt";

        std::unordered_set<RE::FormID> g_recolteInterdite;
        std::filesystem::file_time_type g_recolteDate{};
        bool                            g_recolteLue = false;
    }

    // ETAT D EXECUTION : la liste appartient au personnage de la session en
    // cours. Rescapee d une session precedente elle applique les metiers de
    // QUELQU UN D AUTRE — un herboriste laisse un fichier presque vide, et le
    // profane suivant cueille tout. Page blanche au chargement des donnees ;
    // le serveur repose la liste a l entree (relaisMetiers.surEntree).
    //
    // A ACCROCHER A kDataLoaded ET NULLE PART AILLEURS : kPreLoadGame et
    // kPostLoadGame arrivent APRES la connexion et effaceraient la liste
    // fraiche du bon personnage.
    void RecolteInitialiser()
    {
        std::error_code ec;
        std::filesystem::remove(kCheminRecolte, ec);
        g_recolteInterdite.clear();
        g_recolteDate = {};
        g_recolteLue  = false;
        logger::info("[vulkaar] recolte : page blanche, en attente de la liste du serveur");
    }

    // Relu quand le fichier bouge — le serveur le repose a chaque changement
    // d etat des metiers (entree, prise, abandon, passage de niveau, oubli).
    //
    // SEQUENCE ET SENTINELLE, comme les ponts freres (Echange.cpp jette toute
    // lecture sans seq). Une ecriture est TRONQUABLE : on peut lire pendant
    // que le client ecrit. Pour une liste d INTERDITS une lecture partielle ne
    // veut pas dire « anciennes valeurs » mais MOINS D INTERDITS — donc des
    // plantes rouvertes en silence, ce que personne ne verrait jamais. On
    // exige les deux bornes, et on NE MEMORISE PAS la date d une lecture
    // rejetee : sinon la troncature se figerait pour toujours.
    void RecolteTick()
    {
        std::error_code ec;
        const auto date = std::filesystem::last_write_time(kCheminRecolte, ec);
        if (ec) {
            // Pas de fichier : rien d interdit. C est le cas hors vulkaar, et
            // c est le bon defaut — on ne bloque jamais par accident.
            if (g_recolteLue) {
                g_recolteInterdite.clear();
                g_recolteLue = false;
            }
            return;
        }
        if (g_recolteLue && date == g_recolteDate) return;

        std::ifstream            f(kCheminRecolte);
        std::vector<std::string> lignes;
        std::string              ligne;
        while (std::getline(f, ligne)) {
            while (!ligne.empty() &&
                   (ligne.back() == '\r' || ligne.back() == '\n' || ligne.back() == ' '))
                ligne.pop_back();
            if (!ligne.empty()) lignes.push_back(ligne);
        }
        // Les deux bornes, sans quoi la lecture est jetee TELLE QUELLE : on
        // garde l ensemble precedent et on reessaiera au prochain tic.
        if (lignes.size() < 2 || lignes.front().rfind("seq", 0) != 0 ||
            lignes.back() != "fin") {
            return;
        }

        std::unordered_set<RE::FormID> neuf;
        for (std::size_t k = 1; k + 1 < lignes.size(); ++k) {
            char*               bout = nullptr;
            const std::uint32_t id =
                static_cast<std::uint32_t>(std::strtoul(lignes[k].c_str(), &bout, 16));
            if (bout && *bout == 0) neuf.insert(static_cast<RE::FormID>(id));
        }
        g_recolteInterdite.swap(neuf);
        g_recolteDate = date;
        g_recolteLue  = true;
        logger::info("[vulkaar] recolte : {} base(s) de flore interdite(s) a ce personnage ({})",
                     g_recolteInterdite.size(), lignes.front());
    }

    bool RecolteRefusee(RE::FormID a_base)
    {
        return !g_recolteInterdite.empty() && g_recolteInterdite.count(a_base) != 0;
    }

    void NotifyPasLeMetier()
    {
        static std::uint32_t s_last = 0;
        const std::uint32_t  now = RE::GetDurationOfApplicationRunTime();
        if (now - s_last > 3000) {
            s_last = now;
            FUI::Sfx::FailNote("Ce n est pas ton metier.");
        }
    }

    // [vulkaar] LE NOM AFFICHE AU RETICULE D UNE PORTE DE MAISON.
    //
    // Le nom d une porte a teleport ne vient NI de la reference NI de sa forme
    // de base : c est le FULL de la CELLULE DE DESTINATION. Mesure faite dans
    // Skyrim.esm : la porte 0x1DB63 a pour base « Door », sa cellule de
    // destination 0x1DB4E a pour FULL « Warmaiden's » — et c est « Warmaiden's »
    // que le jeu montre. Consequence : le SetDisplayName que le serveur poussait
    // n ecrit qu un ExtraTextDisplayData sur la reference, que
    // TESObjectDOOR::GetActivateText (slot 0x4C) ne lit pas. Le nom arrivait
    // bien jusqu au client, sans jamais pouvoir s afficher.
    //
    // Renommer la cellule reglerait l affichage et serait un desastre :
    // TESObjectCELL::ChangeFlags::kFullName existe, donc le changement partirait
    // dans la SAUVEGARDE du joueur, et une cellule n a qu UN nom (carte, ecran
    // de chargement, voyage rapide, toutes les portes qui y menent).
    //
    // On passe donc par un crochet de vtable, alimente par un fichier que le
    // client TypeScript pose — exactement le pont de la recolte ci-dessus, et
    // pour la meme raison : le pont CEF tronque les formId en int32.
    namespace
    {
        constexpr const char* kCheminPortes = "Data/SKSE/Plugins/GridInventory_portes.txt";

        // ── CE QUE LE PONT PORTE POUR UNE PORTE ─────────────────────────────
        //
        // LE NOM NE SUFFIT PLUS, ET VOICI POURQUOI. Le rafraichissement du
        // reticule (toute la mecanique de dette plus bas) s ouvre sur un DELTA.
        // Tant que ce delta se calculait sur le NOM SEUL, une bascule de verrou
        // ne changeait pas une ligne du fichier : delta vide, dette jamais
        // ouverte, et le texte restait fige sur « Deverrouiller » jusqu a ce que
        // le joueur detourne le regard. Or le verbe (« Ouvrir » /
        // « Deverrouiller ») et la ligne de difficulte viennent du MOTEUR, et le
        // moteur ne les recompose que sur evenement — c est justement ce qu on
        // vient chercher. Il faut donc que le verrou VOYAGE, pour que son
        // changement soit visible dans le delta.
        //
        // Le 04/09 le nom a perdu sa decoration « (verrouille) » — le jeu dit
        // deja le verbe et la difficulte — ce qui a rendu le nom seul MUET sur
        // la serrure. Le verrou revient donc dans le pont comme DONNEE, pas
        // comme decoration : il n est jamais affiche, il ne sert qu au delta.
        //
        // verrou : 1 verrouille, 0 non, -1 INCONNU (ligne a deux colonnes d un
        // client plus ancien). L inconnu ne clignote pas — voir le delta.
        struct PorteEtat
        {
            std::string nom;              // peut etre VIDE : « laisse le nom du jeu »
            int         verrou = -1;      // -1 inconnu, 0 ouvert, 1 verrouille
        };

        // formId de la REFERENCE (une entree par FACE) -> ce que le pont porte.
        std::unordered_map<RE::FormID, PorteEtat> g_nomsPortes;
        std::filesystem::file_time_type           g_portesDate{};
        bool                                      g_portesLues = false;

        // ── LA BORNE DES LECTURES REJETEES ──────────────────────────────────
        //
        // Une lecture sans ses bornes « seq »/« fin » est jetee SANS memoriser
        // sa date, a dessein : la date memorisee est le seul critere de
        // relecture, et la retenir figerait une troncature pour toujours (voir
        // PortesRelire). Mais l inverse n est pas gratuit : un fichier present,
        // stable et durablement invalide — troncature qu aucune reecriture ne
        // vient corriger, fichier a moitie ecrit par un client qui s est arrete
        // la — fait relire, redecouper et rejeter le fichier ENTIER a chaque
        // trame, 60 fois par seconde jusqu a la fin de la session. Une table de
        // 500 portes, c est 30 000 lignes decoupees par seconde sur le fil
        // d Update, pour rien.
        //
        // On garde donc l intention (ne jamais perdre une poussee) et on borne
        // les TENTATIVES SUR UNE MEME DATE. LE COMPROMIS, dit franchement : si
        // le client reecrit le fichier — ce qu il fait a chaque poussee — la
        // date change, le compteur repart, et la poussee n est pas perdue ;
        // c est le cas normal, y compris celui d une troncature qu une
        // reecriture repare. Ce qu on abandonne, c est le seul cas ou l on ne
        // gagnait de toute facon rien : le meme fichier casse, octet pour octet
        // et date pour date, relu indefiniment. Huit essais laissent passer
        // l ecriture en cours (elle dure quelques millisecondes, soit une trame
        // ou deux) sans laisser courir la boucle.
        std::filesystem::file_time_type g_portesDateRejetee{};
        int                             g_portesRejets   = 0;
        constexpr int                   kPortesRejetsMax = 8;

        // Le numero de sequence du dernier fichier accepte, GARDE POUR LE
        // JOURNAL SEUL : il rattache un appel de GetActivateText a la mutation
        // qui l a provoque. Ce n est PAS un critere de relecture — c est la
        // date du fichier qui l est, et le client fait deja croitre la seq.
        //
        // ATOMIQUE, ET UN ENTIER PLUTOT QU UNE CHAINE, parce que deux fils
        // PROUVES DISTINCTS y touchent : PortesTick ecrit depuis
        // PlayerCharacter::Update, le crochet de texte lit depuis le fil qui
        // compose le reticule. Un std::string partage ainsi, c est un pointeur
        // qui se rebatit sous la lecture — meme raison qui a fait de
        // g_vanillaKey un atomic en tete de fichier. 0 = aucune table posee.
        std::atomic<std::uint32_t> g_porteSeq{ 0 };

        // Le budget de lignes de mesure. Meme partage, meme remede : le tic le
        // recharge, le crochet le consomme. Il ne se recharge QUE lorsqu un
        // rafraichissement part vraiment (voir ReclamerTexteReticule) — le
        // recharger a chaque table neuve accrochait la cadence du journal a
        // celle des mutations DU SERVEUR ENTIER (une poussee a tous les joueurs
        // par maison touchee, la notre ou pas), et chaque ligne est un vidage
        // disque (flush_on(info)) sur le fil d Update. A 500-700 joueurs, c est
        // le journal qui deviendrait le cout.
        std::atomic<int> g_porteJournal{ 0 };

        // ── LA DETTE DE RAFRAICHISSEMENT ────────────────────────────────────
        //
        // POURQUOI UNE DETTE ET PAS UN TEST PONCTUEL. Tirer a la trame exacte
        // ou le fichier est relu rate son occasion deux fois sur deux :
        //  (a) rien ne garantit que la cible du reticule y soit lisible, et ON
        //      NE LE SAIT TOUJOURS PAS. Les 16 appels mesures du crochet ont
        //      tous dit « par parametre » — mais cela ne dit RIEN de
        //      GetActiveTarget : dans le crochet, la lecture par reticule est
        //      une branche « else if » qui n est PAS EXECUTEE quand la branche
        //      du parametre reussit. Seize succes du parametre, c est seize
        //      fois ou le reticule n a jamais ete interroge. En conclure « le
        //      reticule ne rend jamais la porte » serait une inference fausse,
        //      et elle a ete ecrite ici une fois : on ne la reecrit pas.
        //      L ignorance elle-meme est la raison d etre de la dette — c est
        //      pour cela qu on retente au lieu de tirer une fois, et pour cela
        //      que le cas sterile plus bas se journalise ;
        //  (b) la mutation arrive PENDANT que le panneau /maison est ouvert
        //      (mesure : table neuve posee 419 ms AVANT « menu hidden »), donc
        //      sous un menu a curseur, ou l on ne sait pas ce que devient la
        //      cible du pick ; et la fermeture ne rattrape pas de facon fiable
        //      (une fois +0,46 s, une autre fois rien pendant 12,4 s).
        // Or une occasion ratee est perdue POUR TOUJOURS, puisque la date du
        // fichier est memorisee : il n y aura pas de deuxieme lecture.
        //
        // On garde donc l ENSEMBLE des portes dont le nom a bouge, et on
        // retente a chaque trame tant qu il n est pas vide. Le cout par trame
        // ne court que pendant cette fenetre.
        std::unordered_set<RE::FormID> g_porteDette;
        std::uint32_t                  g_porteDetteFin{ 0 };      // instant limite (ms)
        bool                           g_porteDetteMenu = false;  // notre panneau etait-il ouvert ?
        bool                           g_porteDetteDite = false;  // le cas sterile, dit une seule fois

        // LA BORNE. Une dette qui ne trouve jamais son occasion couterait deux
        // lectures par trame pour le reste de la session, et elle finirait par
        // rafraichir un nom que le joueur a de toute facon revu autrement (une
        // porte franchie, un chargement). Huit secondes couvrent largement le
        // trajet « je referme le panneau, je regarde la porte » — au-dela il n y
        // a plus rien a rattraper.
        // QUINZE, PAS HUIT. Le seul contre-exemple MESURE d une fermeture de
        // panneau qui ne re-pique pas dure 12,4 s (une autre fois 0,46 s) :
        // huit secondes abandonnaient justement le cas pour lequel la dette a
        // ete ecrite. Quinze le couvrent, et une dette ne coute qu un test d
        // ensemble vide par trame tant qu elle attend.
        constexpr std::uint32_t kPorteDetteMs = 15000;

        // QUELLE PORTE LE JOUEUR REGARDE-T-IL A CET INSTANT ? Meme lecture que
        // dans le crochet de texte, et depuis la meme trame.
        RE::FormID PorteVisee()
        {
            auto* pick = RE::CrosshairPickData::GetSingleton();
            if (!pick) return 0;
            const auto cible = pick->GetActiveTarget().get();
            return cible ? cible->GetFormID() : 0;
        }

        // POURQUOI CE RAPPEL EXISTE. Mesure du 04/09 : sur 3 min 29 s le
        // moteur n a demande GetActivateText que 16 fois — il compose le texte
        // du reticule SUR EVENEMENT et le laisse fige ensuite. Renommer un
        // batiment ne touche a rien dans le moteur : le joueur devait detourner
        // le regard et reviser la porte pour lire le nom neuf.
        //
        // On ne compose donc PAS le texte nous-memes : on force le moteur a le
        // REDEMANDER, et notre crochet repasse derriere comme d habitude. Le
        // verbe du jeu (« Ouvrir » / « Deverrouiller ») est ainsi preserve.
        //
        // UN SEUL COUP PAR DETTE, jamais par trame (le reticule clignoterait)
        // et jamais depuis le crochet de texte (recursion).
        void ReclamerTexteReticule(RE::FormID porte, const char* pourquoi)
        {
            auto* joueur = RE::PlayerCharacter::GetSingleton();
            if (!joueur) return;
            // LE BUDGET DE MESURE SE RECHARGE ICI ET NULLE PART AILLEURS : ce
            // qu il faut lire, ce sont les appels qui SUIVENT un rafraichissement
            // reellement parti — c est cela qui dira si UpdateCrosshairs mord.
            //
            // ET IL S ARME AVANT L APPEL, PAS APRES. Si UpdateCrosshairs
            // recompose de facon SYNCHRONE — l hypothese la plus probable, c est
            // le propre d un « rafraichis maintenant » —, le GetActivateText
            // qu il declenche tourne AVANT que le store ne revienne : budget
            // encore a zero, aucune ligne, et la session conclurait « ca ne mord
            // pas » alors que ca a mordu. Faux negatif exactement sur la mesure
            // pour laquelle tout ce dispositif existe. La ligne « reticule
            // reclame » passe devant elle aussi, pour que l ordre du journal
            // rende l ordre reel : reclamation d abord, appels declenches
            // ensuite. Rien ne depend de l ordre a part la lisibilite — et si
            // l appel etait finalement asynchrone, armer trop tot ne coute rien.
            g_porteJournal.store(4, std::memory_order_relaxed);
            logger::info("[vulkaar] portes : reticule reclame ({}) pour 0x{:08X} (seq {})",
                         pourquoi, porte, g_porteSeq.load(std::memory_order_relaxed));
            joueur->UpdateCrosshairs();
        }

        // La dette s ouvre — ou S AGRANDIT. On note l instant limite et l etat
        // de NOTRE menu au moment meme : c est ce qui permet de reconnaitre sa
        // FERMETURE sans se declencher sur la trame d ouverture.
        //
        // ON FUSIONNE, ON NE REMPLACE PAS. Le serveur repose le fichier a CHAQUE
        // mutation de N IMPORTE QUELLE maison, et il le pousse a TOUT LE MONDE :
        // pendant les huit secondes d une dette a nous, une poussee qui ne nous
        // concerne pas (le verrou d un autre joueur, un droit accorde ailleurs)
        // arrive tres normalement. Une affectation ecraserait alors une dette
        // ENCORE DUE par un ensemble qui ne la contient pas : la porte que le
        // joueur vient de renommer garderait son ancien nom jusqu au prochain
        // chargement, et le symptome serait indiscernable de « UpdateCrosshairs
        // ne mord pas » — le defaut mangerait la mesure. Union des formId donc,
        // et borne repoussee : la porte la plus recemment changee merite ses
        // huit secondes pleines, et prolonger ne coute rien puisque la dette se
        // vide des le premier tir (PorteDetteHonorer efface TOUT l ensemble).
        void PorteDetteOuvrir(std::unordered_set<RE::FormID>&& quoi)
        {
            if (quoi.empty()) return;
            const bool premiere = g_porteDette.empty();
            g_porteDette.insert(quoi.begin(), quoi.end());
            g_porteDetteFin = RE::GetDurationOfApplicationRunTime() + kPorteDetteMs;
            // Du contenu neuf merite une nouvelle ligne du cas sterile : sans
            // cela une dette agrandie resterait muette sur ce qu elle attend.
            g_porteDetteDite = false;
            /* UN FILET DE MESURE, PETIT ET DELIBERE. Le budget du journal ne se
               recharge qu au TIR, c est-a-dire au bout de trois conditions en
               serie : un nom change, une dette s ouvre, une occasion se
               presente. Si l occasion ne vient jamais, la session entiere reste
               MUETTE sur l etat de la serrure — y compris si le symptome se
               rejoue sous les yeux du joueur. Deux lignes suffisent alors a
               savoir ce que le moteur voyait ; on les paye a chaque mutation qui
               nous concerne, pas a chaque poussee du serveur. */
            if (g_porteJournal.load(std::memory_order_relaxed) < 2) {
                g_porteJournal.store(2, std::memory_order_relaxed);
            }
            // L ETAT DU MENU NE SE REPREND QUE POUR UNE DETTE NEUVE. Sur une
            // dette deja en cours, c est PorteDetteHonorer qui suit ce drapeau
            // de trame en trame ; le reecrire ici avec l etat de la trame
            // COURANTE effacerait le souvenir de la trame precedente, et une
            // fermeture survenue precisement a cet instant passerait inapercue.
            if (premiere) {
                auto* ui         = RE::UI::GetSingleton();
                g_porteDetteMenu = ui && ui->IsMenuOpen("GridInventoryMenu"sv);
            }
        }

        // Une trame de la fenetre d attente. Sort tout de suite hors fenetre :
        // sans dette, ce tic ne coute qu un test d ensemble vide.
        void PorteDetteHonorer()
        {
            if (g_porteDette.empty()) return;

            // NOTRE PANNEAU VIENT-IL DE SE REFERMER ? C est l occasion la plus
            // sure : celui qui vient de renommer son batiment au panneau
            // /maison regarde justement la porte en sortant. On tire meme si la
            // cible du reticule reste illisible — c est le cas ordinaire.
            auto*      ui = RE::UI::GetSingleton();
            const bool notreMenu     = ui && ui->IsMenuOpen("GridInventoryMenu"sv);
            const bool vientDeFermer = g_porteDetteMenu && !notreMenu;
            g_porteDetteMenu         = notreMenu;

            const auto visee    = PorteVisee();
            const bool viseeDue = visee != 0 && g_porteDette.count(visee) != 0;

            /* NOTRE PANNEAU OUVERT NE CONSOMME PAS LA DETTE. Le geste de verrou
               part de l ecran d appartenance, qui NE SE FERME PAS apres le clic : le
               serveur repond en quelques dizaines de ms, donc le pont est
               reecrit panneau OUVERT, et ce crochet tourne menu ouvert. Tirer
               la, c est demander au moteur de recomposer un texte que le joueur
               ne voit pas — et vider la dette, de sorte qu a la fermeture il ne
               reste plus rien a rattraper. La dette survit donc jusqu a la
               fermeture, ou `vientDeFermer` tire a coup sur ; et le cas
               ordinaire — le joueur vise la porte hors panneau — n est pas
               touche. */
            if ((viseeDue && !notreMenu) || vientDeFermer) {
                ReclamerTexteReticule(visee, viseeDue ? "cible due" : "panneau referme");
                g_porteDette.clear();
                return;
            }

            // LE CAS STERILE DOIT SE VOIR, sinon une session de validation qui
            // ne montre rien serait indiscernable entre « UpdateCrosshairs ne
            // mord pas » et « on n a jamais appele ». Une fois par dette, pas
            // par trame : sinon ce serait 60 vidages disque par seconde.
            if (!g_porteDetteDite) {
                g_porteDetteDite = true;
                if (visee == 0)
                    logger::info("[vulkaar] portes : reticule — aucune cible lisible, "
                                 "{} porte(s) en attente",
                                 g_porteDette.size());
                else
                    logger::info("[vulkaar] portes : cible 0x{:08X} hors de la dette, "
                                 "{} porte(s) en attente",
                                 visee, g_porteDette.size());
            }

            // Difference SIGNEE : l horloge du jeu compte des millisecondes sur
            // 32 bits et repasse par zero ; une comparaison naive ferait durer
            // la dette 49 jours de plus a ce moment-la.
            const std::uint32_t maintenant = RE::GetDurationOfApplicationRunTime();
            if (static_cast<std::int32_t>(maintenant - g_porteDetteFin) >= 0) {
                logger::info("[vulkaar] portes : dette abandonnee apres {} ms — "
                             "{} porte(s) jamais rafraichie(s)",
                             kPorteDetteMs, g_porteDette.size());
                g_porteDette.clear();
            }
        }

        // « seq<TAB>N » -> N. Seul le NUMERO voyage entre les deux fils (voir
        // g_porteSeq) ; l en-tete lui-meme ne quitte pas le fil du tic.
        std::uint32_t PorteSeqDe(const std::string& enTete)
        {
            const auto chiffre = enTete.find_first_of("0123456789");
            if (chiffre == std::string::npos) return 0;
            return static_cast<std::uint32_t>(
                std::strtoul(enTete.c_str() + chiffre, nullptr, 10));
        }
    }

    // ETAT D EXECUTION : la liste appartient au personnage de la session en
    // cours. Rescapee d une session precedente, elle nommerait les maisons de
    // QUELQU UN D AUTRE. Page blanche au chargement des donnees ; le serveur
    // repose la liste a l entree.
    //
    // A ACCROCHER A kDataLoaded ET NULLE PART AILLEURS : kPreLoadGame et
    // kPostLoadGame arrivent APRES la connexion et effaceraient la liste
    // fraiche du bon personnage.
    void PortesInitialiser()
    {
        std::error_code ec;
        std::filesystem::remove(kCheminPortes, ec);
        g_nomsPortes.clear();
        g_portesDate        = {};
        g_portesLues        = false;
        g_portesDateRejetee = {};
        g_portesRejets      = 0;
        g_porteDette.clear();
        g_porteDetteMenu = false;
        g_porteDetteDite = false;
        g_porteSeq.store(0, std::memory_order_relaxed);
        // Une avance de mesure a l entree, une seule fois par session : elle
        // montre le tout premier etat du texte vanilla. Ensuite le budget ne
        // revient que par un rafraichissement reellement parti.
        g_porteJournal.store(6, std::memory_order_relaxed);
        logger::info("[vulkaar] portes : page blanche, en attente de la liste du serveur");
    }

    // Relu quand le fichier bouge — le serveur le repose a chaque mutation qui
    // change un nom ou un rattachement.
    //
    // SEQUENCE ET SENTINELLE, comme la recolte : une ecriture est TRONQUABLE,
    // on peut lire pendant que le client ecrit. Pour une table de NOMS une
    // lecture partielle ne dit pas « anciennes valeurs » mais des portes qui
    // reprennent en silence leur nom vanilla. On exige les deux bornes, et on
    // NE MEMORISE PAS la date d une lecture rejetee : sinon la troncature se
    // figerait pour toujours.
    void PortesRelire()
    {
        std::error_code ec;
        const auto      date = std::filesystem::last_write_time(kCheminPortes, ec);
        if (ec) {
            // Pas de fichier : aucune porte nommee. C est le cas hors vulkaar,
            // et c est le bon defaut — le jeu garde ses propres noms.
            if (g_portesLues) {
                // Le fichier disparait aussi SOUS LE REGARD du joueur (fin de
                // session, page blanche) : TOUTES les portes qu il nommait
                // doivent reprendre leur nom vanilla, donc toutes sont dues.
                std::unordered_set<RE::FormID> change;
                for (const auto& paire : g_nomsPortes) change.insert(paire.first);
                g_nomsPortes.clear();
                g_portesLues = false;
                g_porteSeq.store(0, std::memory_order_relaxed);
                PorteDetteOuvrir(std::move(change));
            }
            return;
        }
        if (g_portesLues && date == g_portesDate) return;
        // Cette date-la a deja epuise ses essais : on ne redecoupe pas le
        // fichier une 61e fois cette seconde (voir kPortesRejetsMax).
        if (g_portesRejets >= kPortesRejetsMax && date == g_portesDateRejetee) return;

        std::ifstream            f(kCheminPortes);
        std::vector<std::string> lignes;
        std::string              ligne;
        while (std::getline(f, ligne)) {
            // On ne rogne QUE les fins de ligne : ce qui suit la tabulation est
            // du texte affiche, et un nom n a pas a etre retaille par le pont.
            while (!ligne.empty() && (ligne.back() == '\r' || ligne.back() == '\n'))
                ligne.pop_back();
            if (!ligne.empty()) lignes.push_back(ligne);
        }
        // Les deux bornes, sans quoi la lecture est jetee TELLE QUELLE : on
        // garde la table precedente et on reessaiera au prochain tic — mais un
        // nombre BORNE de fois pour cette meme date (voir kPortesRejetsMax).
        if (lignes.size() < 2 || lignes.front().rfind("seq", 0) != 0 ||
            lignes.back() != "fin") {
            // On compte les essais PAR DATE : une date neuve, c est une
            // ecriture neuve, donc un compteur neuf — la poussee n est pas
            // perdue, c est toute l intention d origine.
            if (date != g_portesDateRejetee) {
                g_portesDateRejetee = date;
                g_portesRejets      = 0;
            }
            if (++g_portesRejets == kPortesRejetsMax) {
                logger::info("[vulkaar] portes : fichier sans ses bornes apres {} essais — "
                             "on attend une reecriture ({} ligne(s) lue(s))",
                             kPortesRejetsMax, lignes.size());
            }
            return;
        }

        std::unordered_map<RE::FormID, PorteEtat> neuf;
        for (std::size_t k = 1; k + 1 < lignes.size(); ++k) {
            const auto tab = lignes[k].find('\t');
            if (tab == std::string::npos) continue;
            const std::string  hexa = lignes[k].substr(0, tab);
            char*              bout = nullptr;
            const std::uint32_t id =
                static_cast<std::uint32_t>(std::strtoul(hexa.c_str(), &bout, 16));
            if (!bout || *bout != 0) continue;

            // ── DEUX COLONNES OU TROIS : ON NE CHOISIT PAS, ON RECONNAIT ────
            //
            // Le client et le greffon se deploient par des chaines DIFFERENTES
            // et peuvent etre decales. Un greffon neuf doit donc lire les deux
            // formes du fichier :
            //   « id<TAB>nom »        (client ancien) -> verrou INCONNU
            //   « id<TAB>nom<TAB>0|1 » (client neuf)  -> verrou connu
            // On reconnait la troisieme colonne a sa forme EXACTE — une
            // tabulation apres celle de l id, suivie d un seul caractere '0' ou
            // '1' et de rien d autre. Tout le reste est du NOM, pris tel quel :
            // devant un fichier qu on ne comprend pas, on retombe sur le
            // comportement d avant plutot que d amputer un nom.
            //
            // On cherche la DERNIERE tabulation, pas la deuxieme : un nom ne
            // devrait jamais en contenir (le pont est tabule), mais si un jour
            // il en contenait une, couper au dernier separateur garde le nom
            // entier et le drapeau a sa place.
            std::string reste  = lignes[k].substr(tab + 1);
            int         verrou = -1;
            const auto  tab2   = reste.rfind('\t');
            if (tab2 != std::string::npos && reste.size() == tab2 + 2 &&
                (reste[tab2 + 1] == '0' || reste[tab2 + 1] == '1')) {
                verrou = reste[tab2 + 1] - '0';
                reste.resize(tab2);
            }

            // Texte pris en UTF-8 TEL QUEL : les chaines du jeu le sont aussi
            // (« La Guerrière » porte C3 A8 dans skyrim_french.strings).
            // Le nom PEUT ETRE VIDE — une maison creee et pas encore nommee.
            // Elle compte quand meme, parce que son verrou bascule aussi et que
            // son texte doit suivre ; c est le crochet qui refusera d ecrire un
            // nom vide, pas le pont qui refusera de la porter.
            neuf.emplace(static_cast<RE::FormID>(id), PorteEtat{ std::move(reste), verrou });
        }
        /* QU EST-CE QUI A BOUGE ? On le decide AVANT l echange, tant que
           l ancienne table est encore en place : apparu, disparu, renomme — ou
           VERROUILLE/DEVERROUILLE. Le delta porte sur les DEUX, parce que les
           deux changent le texte affiche : le nom par notre crochet, le verrou
           par le verbe et la ligne de difficulte que le moteur recompose.
           Une poussee qui ne change rien (une mutation dans la maison d un
           autre — et le serveur pousse a TOUT LE MONDE a chaque mutation)
           laisse l ensemble vide et ne declenche RIEN : c est ce qui separe un
           rafraichissement d un clignotement. Le seul « seq » ne compte donc
           JAMAIS comme un changement.

           UN VERROU QUI PASSE A/DE L INCONNU N EST PAS UN CHANGEMENT. Si le
           client redescend a deux colonnes (retour arriere) ou remonte a trois,
           toutes les portes basculeraient d un coup et le reticule clignoterait
           sur une information qu on n a pas. On ne compte que les bascules
           entre deux etats CONNUS. */
        std::unordered_set<RE::FormID> change;
        int nomsChanges = 0, verrousChanges = 0;
        for (const auto& [id, etat] : neuf) {
            const auto avant = g_nomsPortes.find(id);
            if (avant == g_nomsPortes.end()) {
                change.insert(id);
                ++nomsChanges;
                continue;
            }
            bool bouge = false;
            if (avant->second.nom != etat.nom) {
                ++nomsChanges;
                bouge = true;
            }
            if (avant->second.verrou != etat.verrou && avant->second.verrou >= 0 &&
                etat.verrou >= 0) {
                ++verrousChanges;
                bouge = true;
            }
            if (bouge) change.insert(id);
        }
        for (const auto& paire : g_nomsPortes) {
            if (neuf.find(paire.first) == neuf.end()) {
                change.insert(paire.first);
                ++nomsChanges;
            }
        }

        g_nomsPortes.swap(neuf);
        g_portesDate = date;
        g_portesLues = true;
        // Une lecture acceptee solde les essais : la prochaine troncature
        // repartira avec son quota entier.
        g_portesDateRejetee = {};
        g_portesRejets      = 0;
        g_porteSeq.store(PorteSeqDe(lignes.front()), std::memory_order_relaxed);
        // LA LIGNE QUI PROUVE EN JEU. Elle separe le nom du verrou a dessein :
        // c est elle qui dira si une simple bascule de serrure — nom inchange,
        // « 0 nom(s) » et « 1 verrou(s) » — a bien ouvert une dette. Sans cette
        // separation, une dette ouverte resterait indiscernable d une dette
        // ouverte POUR LA BONNE RAISON.
        logger::info("[vulkaar] portes : {} porte(s) posee(s) pour ce personnage ({}), "
                     "{} nom(s) change(s), {} verrou(s) change(s), {} porte(s) due(s)",
                     g_nomsPortes.size(), lignes.front(), nomsChanges, verrousChanges,
                     change.size());
        PorteDetteOuvrir(std::move(change));
    }

    // LE TIC : relire le fichier quand il bouge, puis honorer la dette. Les
    // deux sont separes parce que la relecture s arrete des la premiere trame
    // (la date n a pas change) alors que la dette, elle, doit etre retentee
    // trame apres trame jusqu a trouver son occasion.
    void PortesTick()
    {
        PortesRelire();
        PorteDetteHonorer();
    }

    // Le crochet — TESObjectDOOR::GetActivateText, slot 0x4C de la vtable de la
    // CLASSE (une seule pour toutes les portes du jeu). Meme geste que
    // HarvestHook ci-dessous : un echange de pointeur de vtable, aucun
    // trampoline, aucune adresse a trouver, defait au dechargement de la DLL.
    //
    // Corollaire du « une seule vtable » : la table est une liste d AUTORISES
    // explicites par formId de REFERENCE. Une porte absente en ressort INTACTE
    // (return vanilla), jamais avec un texte vide.
    struct PorteTexteHook
    {
        static bool thunk(RE::TESObjectDOOR* a_this, RE::TESObjectREFR* a_activator,
                          RE::BSString& a_dst)
        {
            // LE JEU D ABORD, TOUJOURS : son texte est notre repli, et c est lui
            // que la mesure ci-dessous doit rapporter tel quel.
            const bool vanilla = func(a_this, a_activator, a_dst);

            // QUELLE REFERENCE EST VISEE ? CommonLibSSE nomme le parametre
            // « a_activator » (TESBoundObject.h:60), mais TESObjectDOOR a besoin
            // du XTEL de la PORTE pour composer son texte : le nom du parametre
            // ment peut-etre. On ne parie sur aucune des deux lectures — le test
            // de base tranche a l execution, et le reticule sert de recours
            // (GetActivateText peut aussi etre appele hors reticule).
            RE::TESObjectREFR* porte = nullptr;
            const char*        dOu = "aucune";
            if (a_activator && a_activator->GetBaseObject() == a_this) {
                porte = a_activator;
                dOu   = "parametre";
            } else if (auto* pick = RE::CrosshairPickData::GetSingleton()) {
                const auto cible = pick->GetActiveTarget().get();
                if (cible && cible->GetBaseObject() == a_this) {
                    porte = cible.get();
                    dOu   = "reticule";
                }
            }

            // LA MESURE, sur un budget que seul un rafraichissement reellement
            // parti recharge (voir g_porteJournal) : ces lignes-la sont celles
            // qui SUIVENT un UpdateCrosshairs, donc celles qui disent s il
            // mord. Elles rapportent aussi le texte vanilla tel quel — le
            // format est connu (mesure du 04/09, voir la composition plus bas),
            // ce qui reste a surveiller c est qu il ne change pas d une porte a
            // l autre — et si le parametre etait la porte ou l activateur.
            //
            // ── ET SURTOUT : CE QUE LE MOTEUR VOIT DE LA SERRURE ────────────
            //
            // Le constat a expliquer : le moteur a compose « Ouvrir » alors que
            // le verrou Papyrus etait pose ET relu conforme, sans une seule
            // ecriture de notre cote entre les deux. Aucune theorie ne tranchera
            // cela ; seule une lecture PRISE ICI, dans la trame meme ou le texte
            // se compose, peut le faire. On lit donc l etat de la reference a
            // l instant exact de la composition, pour le confronter au texte.
            //
            // PLUSIEURS LECTURES, parce qu elles PEUVENT DIVERGER et que c est
            // justement la divergence qui nommerait le fautif. Signatures
            // relevees dans les en-tetes REELS du depot
            // (build/_deps/commonlibsse-src), pas de memoire :
            //  - TESObjectREFR::IsLocked() (TESObjectREFR.h:459) n est PAS un
            //    drapeau : son corps (src/RE/T/TESObjectREFR.cpp:862) rend
            //    « GetLockLevel() != LOCK_LEVEL::kUnlocked ». C est une question
            //    de NIVEAU, pas d etat.
            //  - REFR_LOCK::IsLocked() (ExtraLock.h:30) est le vrai drapeau,
            //    « flags.all(Flag::kLocked) ». Les deux peuvent se contredire.
            //  - REFR_LOCK::baseLevel (ExtraLock.h:36) est le champ BRUT que
            //    Papyrus SetLockLevel ecrit. Il est declare std::int8_t : le 255
            //    que nous posons s y relit -1 en signe. Et LOCK_LEVEL::kUnlocked
            //    vaut precisement -1 (ExtraLock.h:11). On journalise les DEUX
            //    lectures du meme octet, signee puis non signee, pour que la
            //    comparaison se fasse sur des chiffres et non sur une idee.
            //  - BGSOpenCloseForm::GetOpenState (BGSOpenCloseForm.h:30) est
            //    STATIQUE et prend la reference : 1 kOpen, 3 kClosed.
            // CE QUI N EXISTE PAS SOUS LA FORME DEMANDEE, et il faut le dire :
            // il n y a pas de lecture du niveau BRUT par la reference.
            // TESObjectREFR::GetLockLevel() rend deja l ENUMERE (il passe par
            // REFR_LOCK::GetLockLevel, qui resout les serrures a niveau). Le
            // brut ne s obtient que par GetLock()->baseLevel — c est donc ce
            // qu on prend. Sans serrure attachee GetLock() rend nullptr, les
            // colonnes de serrure valent -1 (« pas de serrure du tout »), et
            // c est deja une reponse.
            //
            // Tout tient sur LA LIGNE DEJA BUDGETEE : pas un vidage disque de
            // plus qu avant.
            if (g_porteJournal.load(std::memory_order_relaxed) > 0) {
                g_porteJournal.fetch_sub(1, std::memory_order_relaxed);
                const RE::REFR_LOCK* serrure = porte ? porte->GetLock() : nullptr;
                const int verrouNiveau  = porte ? (porte->IsLocked() ? 1 : 0) : -1;
                const int verrouDrapeau = serrure ? (serrure->IsLocked() ? 1 : 0) : -1;
                const int brutSigne     = serrure ? static_cast<int>(serrure->baseLevel) : -1;
                const int brutOctet =
                    serrure ? static_cast<int>(static_cast<std::uint8_t>(serrure->baseLevel)) : -1;
                const int niveau    = porte ? static_cast<int>(porte->GetLockLevel()) : -99;
                // LA CAUSE LA PLUS BANALE, ET LA SEULE QU ON NE MESURAIT PAS :
                // en vanilla, une porte verrouillee DONT LE JOUEUR A LA CLEF
                // s annonce « Ouvrir », pas « Deverrouiller ». C est donc
                // l explication la plus ordinaire du constat a expliquer — le
                // moteur dit « Ouvrir » alors que notre verrou est bien la — et
                // sans cette colonne elle resterait invérifiable : il faudrait
                // rebatir la DLL et rejouer une session pour la lever. Deux
                // dereferencements sur un pointeur deja en main, sur la ligne
                // deja budgetee : rien de plus n est ecrit au disque.
                const int clef     = serrure ? (serrure->key ? 1 : 0) : -1;
                const int drapeaux = serrure ? static_cast<int>(serrure->flags.underlying()) : -1;
                const int ouverture =
                    porte ? static_cast<int>(RE::BGSOpenCloseForm::GetOpenState(porte)) : -1;
                // CE QUE LE PONT DIT DE LA MEME SERRURE, a confronter aux
                // colonnes du dessus. C est la seule facon de distinguer « le
                // serveur n a pas pousse la bascule » de « il l a poussee et le
                // moteur ne l a pas encore relue ». -2 = porte absente du pont
                // (elle ressort intacte du crochet), -1 = portee sans verrou
                // connu (client a deux colonnes). Une recherche de plus dans une
                // table deja en memoire, sur la ligne deja budgetee.
                int verrouPont = -2;
                if (porte) {
                    const auto vu = g_nomsPortes.find(porte->GetFormID());
                    if (vu != g_nomsPortes.end()) verrouPont = vu->second.verrou;
                }
                logger::info(
                    "[vulkaar] porte : appel apres seq {} — ref 0x{:08X} (par {}), "
                    "base 0x{:08X}, rendu={}, serrure={} verrouNiveau={} verrouDrapeau={} "
                    "brut={}/{} niveau={} ouverture={} clef={} drapeaux={} verrouPont={}, "
                    "texte vanilla = \"{}\"",
                    g_porteSeq.load(std::memory_order_relaxed),
                    porte ? porte->GetFormID() : 0u, dOu,
                    a_this ? a_this->GetFormID() : 0u, vanilla ? 1 : 0,
                    serrure ? "presente" : "absente", verrouNiveau, verrouDrapeau,
                    brutSigne, brutOctet, niveau, ouverture, clef, drapeaux, verrouPont,
                    a_dst.c_str());
            }

            if (!porte || g_nomsPortes.empty()) return vanilla;
            const auto it = g_nomsPortes.find(porte->GetFormID());
            if (it == g_nomsPortes.end()) return vanilla;
            // UN NOM VIDE VEUT DIRE « LAISSE AU JEU LE NOM QU IL DONNE », PAS
            // « efface le nom ». Une maison creee et pas encore nommee est
            // portee par le pont — il le faut, son verrou bascule et son texte
            // doit suivre — mais elle ne doit rien effacer a l ecran : le
            // joueur y perdrait le nom vanilla de la cellule sans rien gagner.
            // Elle compte pour la dette, elle ne compte pas pour le texte.
            const std::string& nom = it->second.nom;
            if (nom.empty()) return vanilla;

            // ── LE VERBE RESTE AU JEU, LE NOM SEUL EST A NOUS ───────────────
            //
            // Le texte du reticule est MULTILIGNE. Mesure du 04/09, en jeu :
            //   porte ouverte      « Ouvrir\nWarmaiden's »
            //   porte verrouillee  « Deverrouiller\nWarmaiden's\n<img
            //                        src='DiamondMarker' ...>Apprenti »
            // Le nom est donc la DEUXIEME ligne ; la premiere est le verbe et
            // la troisieme la difficulte du crochetage, avec sa puce en balise
            // <img>. La premiere version ecrasait TOUT le bloc : le joueur ne
            // lisait plus ni « Ouvrir », ni « Deverrouiller », ni le rang de la
            // serrure. On n echange que la ligne du nom.
            //
            // ON NE SUPPOSE PAS CETTE FORME, ON LA TESTE — le crochet vaut pour
            // TOUTES les portes du jeu (une seule vtable), et rien ne garantit
            // que chacune ait trois lignes :
            //   deux lignes ou plus -> seule la deuxieme est remplacee, la
            //                          queue (difficulte, et le reste) suit ;
            //   une seule ligne     -> c est le verbe d une porte sans
            //                          destination nommee : on AJOUTE le nom en
            //                          deuxieme ligne, on ne mange pas le verbe ;
            //   rien du tout        -> le jeu n avait rien a dire, le nom seul.
            // Un nom ne peut pas contenir de saut de ligne : le pont est un
            // fichier a une porte par ligne.
            const std::string_view brut{ a_dst.c_str(), a_dst.size() };
            std::string            compose;
            const auto             fin1 = brut.find('\n');
            if (brut.empty()) {
                compose = nom;
            } else if (fin1 == std::string_view::npos) {
                compose.assign(brut).append("\n").append(nom);
            } else {
                compose.assign(brut.substr(0, fin1 + 1)).append(nom);
                const auto fin2 = brut.find('\n', fin1 + 1);
                if (fin2 != std::string_view::npos) compose.append(brut.substr(fin2));
            }
            a_dst = compose.c_str();
            return true;
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_TESObjectDOOR[0] };
            func = vtbl.write_vfunc(0x4C, thunk);
        }
    };

    // Capacity: harvesting (flora / food-bearing trees) — TESBoundObject::
    // Activate override slot 0x37; the produce item is checked BEFORE the
    // engine harvests, so nothing is consumed on a blocked attempt.
    template <class T>
    struct HarvestHook
    {
        static bool thunk(T* a_this, RE::TESObjectREFR* a_targetRef,
                          RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
                          RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            if (a_activatorRef && a_activatorRef->IsPlayerRef()) {
                // [vulkaar] LE METIER D ABORD : ce test passe avant celui de la
                // place, parce qu une plante qu on n a pas le droit de toucher
                // n a pas a se plaindre d un sac plein.
                if (RecolteRefusee(a_this->GetFormID())) {
                    NotifyPasLeMetier();
                    return false;   // rien n est consomme : la plante reste
                }
                if (a_this->produceItem && !FUI::Grid::CanFitNewItem(a_this->produceItem)) {
                    NotifyInventoryFull();
                    return false;   // blocked: the plant stays harvestable
                }
            }
            return func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install(const REL::VariantID& a_vtbl0)
        {
            REL::Relocation<std::uintptr_t> vtbl{ a_vtbl0 };
            func = vtbl.write_vfunc(0x37, thunk);
        }
    };

    // Capacity gate at the ACTIVATE slot (0x37) — one level ABOVE
    // PickUpObject. TrueHUD's Recent Loot wraps PickUpObject OUTSIDE our
    // PickUpHook, so a pickup blocked down there still logged a phantom
    // "received" entry (user-reported: full inventory + E on a world item
    // spammed RECEIVED while the item stayed on the ground). Blocking at
    // this level, the pickup call never happens and TrueHUD stays silent —
    // same reasoning as SackActivateHook below. PickUpHook remains as the
    // backstop for paths that skip Activate (book-menu Take etc.).
    template <class T>
    struct CapacityActivateHook
    {
        static bool thunk(T* a_this, RE::TESObjectREFR* a_targetRef,
                          RE::TESObjectREFR* a_activatorRef, std::uint8_t a_arg3,
                          RE::TESBoundObject* a_object, std::int32_t a_targetCount)
        {
            if (a_activatorRef && a_activatorRef->IsPlayerRef() &&
                !FUI::Grid::CanFitNewItem(a_this)) {
                NotifyInventoryFull();
                return false;   // blocked: the reference stays in the world
            }
            return func(a_this, a_targetRef, a_activatorRef, a_arg3, a_object, a_targetCount);
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install(const REL::VariantID& a_vtbl0)
        {
            REL::Relocation<std::uintptr_t> vtbl{ a_vtbl0 };
            func = vtbl.write_vfunc(0x37, thunk);
        }
    };

    // Capacity: container take (loot / chests / pickpocket). The move has
    // already happened when the event fires, so this is a BOUNCE: put the
    // item back into the source. Scoped to an open ContainerMenu — scripted
    // quest handovers (no menu) must never bounce.
    // ★★★A RESPAWNED CONTAINER OWES NOBODY ANYTHING.
    //
    // The deposit ledger (LootBarter, ContLayout::deposits) remembers how many
    // of each form the player stored in a given container, so taking them back
    // is not theft. A cell reset empties that container and refills it from the
    // record -- the player's deposit is gone, but a ledger that outlived it
    // would hand those replacements over as "yours", which is a free steal on
    // every respawn.
    //
    // TESResetEvent is the engine saying exactly that happened, and it names
    // the ref. Without it the only alternative was to expire ledgers on a
    // guessed timer, and a guess is wrong in both directions.
    class ResetSink : public RE::BSTEventSink<RE::TESResetEvent>
    {
    public:
        static ResetSink* GetSingleton()
        {
            static ResetSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESResetEvent* a_event,
            RE::BSTEventSource<RE::TESResetEvent>*) override
        {
            if (a_event && a_event->object) {
                // ★Same reasoning as the pouch handlers in ContainerSink: this
                // erases from g_contLayouts, which LootBarter reads and writes
                // from the main thread with no lock. Read the id here (the
                // event is only valid for this call) and do the erase there.
                const RE::FormID id = a_event->object->GetFormID();
                SKSE::GetTaskInterface()->AddTask([id]() {
                    FUI::LootBarter::ForgetDeposits(id);
                });
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class ContainerSink : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static ContainerSink* GetSingleton()
        {
            static ContainerSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // ★1.4/B2+B3: the LEDGER consumes the event first -- one request,
            // one event (rule 1). This lives in the SINK, not in DeltaWatch:
            // the ledger is wiring and the watch is observation, and when
            // Confirm sat behind "!delta" the default configuration starved
            // the ledger of every confirmation -- 100% of requests expired.
            // Thread-safe (the ledger locks); no-op unless "!ledger = 1".
            const char* req = nullptr;
            if (a_event->newContainer == 0x14 || a_event->oldContainer == 0x14) {
                const std::int32_t signedCount = a_event->newContainer == 0x14
                                                     ? a_event->itemCount
                                                     : -a_event->itemCount;
                req = FUI::Ledger::Confirm(a_event->baseObj, signedCount);
            }
            // ★1.4/B0 next, before any consumer below reacts -- the whole point
            // is to see the delta as it ARRIVES, in the order it arrived, and
            // to know where the existing consumers sit relative to it. A sink
            // of our own would be delivered in an order we do not control.
            // Observation only; returns immediately unless "!delta = 1".
            FUI::DeltaWatch::OnContainer(a_event, req);
            // W2: any change touching the player's inventory can flip the
            // capacity state (shop buys, scripted AddItem, drops, sells)
            if (a_event->newContainer == 0x14 || a_event->oldContainer == 0x14) {
                FUI::Grid::MarkCapacityDirty();
                // ★★1.2.1: AND THE TILES, not only the numbers. Capacity and
                // gold were re-derived here while the board itself was left
                // alone, so an item arriving from OUTSIDE the UI (console
                // AddItem, Modex, a script, a quest reward) moved the SPACE
                // figure and showed no tile -- the player had to close and
                // reopen. Our own take/sell/drop paths already ask for this,
                // and the flag coalesces per frame, so the extra request is
                // free.
                // ★1.4: the delta applier, at last. Every player-side container
                // delta first tries the per-form partial (the same walk and
                // tile factory the rebuild uses, scoped to one form); anything
                // unproven declines into the full rebuild, and a pending flag
                // short-circuits the partial -- a take-all burst coalesces to
                // one rebuild exactly as before. Menu closed, the partial
                // declines quietly and the flag is consumed by the capacity
                // gates (H1) or the next open, unchanged.
                {
                    const RE::FormID deltaForm = a_event->baseObj;
                    SKSE::GetTaskInterface()->AddTask([deltaForm]() {
                        if (!FUI::Grid::OnFormDelta(deltaForm)) {
                            FUI::Grid::RequestRebuild();
                        }
                    });
                }
                // G1: ledger (Gold001) or coin-form movement -> re-mirror.
                // The reconciler's own edits re-mark dirty and settle at a
                // zero diff next tick (also renormalises console-given coins).
                if (a_event->baseObj == 0x0000000F ||
                    FUI::GoldCoins::IsCoinForm(a_event->baseObj)) {
                    FUI::GoldCoins::MarkDirty();
                }
                // pouch leaving/returning: stored gold travels with it
                // (sale releases it instead — see OnPouchLeftPlayer)
                if (FUI::GoldCoins::IsPouch(a_event->baseObj)) {
                    // ★★★ONTO THE MAIN THREAD, LIKE EVERYTHING ELSE IN HERE.
                    //
                    // A container event arrives on whatever thread moved the
                    // item -- a Papyrus VM worker for a scripted AddItem, and
                    // this file already knows it (the Grid delta twenty lines
                    // up goes through AddTask for exactly that reason; Ledger
                    // takes a mutex; WornLedger marshals). These two were the
                    // only consumers left calling straight through, and they
                    // are not read-only: OnPouchLeftPlayer push_backs into
                    // g_pending and erases from g_pouchStored while Tick() is
                    // iterating that same vector on the main thread. GoldCoins
                    // has no lock of its own -- so give it the thread instead.
                    const bool left = a_event->oldContainer == 0x14;
                    const bool back = a_event->newContainer == 0x14;
                    if (left || back) {
                        SKSE::GetTaskInterface()->AddTask([left]() {
                            if (left) FUI::GoldCoins::OnPouchLeftPlayer();
                            else      FUI::GoldCoins::OnPouchReturned();
                        });
                    }
                }
            }
            if (a_event->newContainer != 0x14 || a_event->oldContainer == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (auto* ui = RE::UI::GetSingleton();
                !ui || !ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) {
                return RE::BSEventNotifyControl::kContinue;
            }

            const RE::FormID     base = a_event->baseObj;
            const RE::FormID     src = a_event->oldContainer;
            const std::int32_t   count = a_event->itemCount;
            SKSE::GetTaskInterface()->AddTask([base, src, count]() {
                auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(base);
                auto* srcRef = RE::TESForm::LookupByID<RE::TESObjectREFR>(src);
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!obj || !srcRef || !player || obj->IsGold()) return;
                if (!FUI::Grid::WouldOverflow(obj)) return;
                // GI36: deliberately NOT ResolveExitUnit. This item never became
                // a tile and we never gave it a star -- any hotkey on it belongs
                // to the container's own copy. Rule 58 is about stars WE own.
                player->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kStoreInContainer,
                    nullptr, srcRef);
                NotifyInventoryFull();
            });
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void InitializeLog()
    {
        auto path = SKSE::log::log_directory();
        if (!path) {
            return;
        }
        *path /= "GridInventory.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S] [%l] %v");
    }

    // ---- Input sink ----
    class InputSink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink* GetSingleton()
        {
            static InputSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
            RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!a_event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* e = *a_event; e; e = e->next) {
                // ★The wheeler gets first refusal, on every device. It is a
                // gameplay overlay, so it has to see the hotkey before the
                // "only while our inventory is open" filters below throw the
                // event away — and it must see key-UP too, which the keyboard
                // branch further down drops.
                if (auto* ts = e->AsThumbstickEvent()) {
                    if (FUI::Wheeler::OnThumbstick(ts)) continue;
                } else if (auto* mm = e->AsMouseMoveEvent()) {
                    if (FUI::Wheeler::OnMouseMove(mm)) continue;
                } else if (auto* wb = e->AsButtonEvent()) {
                    if (FUI::Wheeler::OnButton(wb)) continue;
                }
                // ★★NOT muted here. Silencing the game's own input is done at
                // PlayerControls' and MenuControls' entry points instead (see
                // InputLock / MenuLock in Wheeler.cpp), because an event is one
                // object shared by the whole sink chain: blanking it in OUR
                // sink also blanks it for every listener that runs after us,
                // and which side of us the game sits on is not ours to decide.
                // Those hooks blank, call the original, and put the value back.
                if (FUI::Wheeler::IsOpen()) continue;

                // A real mouse event hands the pointer back from the pad.
                // This is the ONLY reliable signal — see UIRoot::NoteMouseInput.
                if (e->GetDevice() == RE::INPUT_DEVICE::kMouse) {
                    if (auto* ui = RE::UI::GetSingleton();
                        ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
                        FUI::UIRoot::NoteMouseInput();
                    }
                    continue;
                }

                // ---- gamepad: drive the grid's own pointer -----------------
                // Only while our menu owns the screen, so nothing here can
                // touch normal gameplay input.
                if (e->GetDevice() == RE::INPUT_DEVICE::kGamepad) {
                    auto* ui = RE::UI::GetSingleton();
                    if (!ui || !ui->IsMenuOpen("GridInventoryMenu"sv)) continue;
                    if (FUI::UIRoot::IsBookOpen()) continue;   // the book has input
                    if (auto* ts = e->AsThumbstickEvent()) {
                        FUI::UIRoot::NotePadStick(ts->IsRight(), ts->xValue, ts->yValue);
                        // let the engine's cursor move itself (see the header)
                        FUI::UIRoot::FeedEngineCursor(ts);
                    } else if (auto* gb = e->AsButtonEvent()) {
                        // held state, not the down EDGE: the UI needs press and
                        // release both (click-drag, the shift modifier)
                        FUI::UIRoot::NotePadButton(gb->GetIDCode(), gb->IsPressed());
                    }
                    continue;
                }

                auto* btn = e->AsButtonEvent();
                if (!btn || !btn->IsDown()) {
                    continue;
                }
                if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                // A book opened from our grid owns the keyboard until it is
                // dismissed — the Inventory key must not close us underneath it.
                // ★The console is the same story from the other direction: it
                // sits on top taking keystrokes, and an 'i' typed into a
                // command would otherwise close the menu behind it.
                if (FUI::UIRoot::IsBookOpen() || FUI::UIRoot::IsConsoleOpen()) {
                    continue;
                }
                // ★★F11 hands the next inventory to the engine, and F11 again
                // takes it back. Deliberately NOT gated on our menu being open:
                // the point is to arm the swap while nothing is open, then press
                // the inventory key and watch the OTHER screen do the same thing.
                // The state only reads at open time, so a mid-session press
                // never disturbs a screen already on show.
                /* [vulkaar] F9 OUVRAIT L'ECRAN SCALEFORM D'ESSAI. Retire le
                   29/08/2026 : la question qu'il posait est tranchee — le
                   moteur accepte nos swf, le pont fonctionne dans les deux
                   sens — et le proprietaire a choisi de garder l'ecran ImGui
                   pour l'etabli, « et juste mettre le rendu 3d du jeu
                   dessus ». Le code de EssaiSwf reste au depot : c'est la
                   fondation Scaleform, eprouvee, pour un ecran qui n'aura pas
                   besoin de 3D. */
                if (const int vk = FUI::UIRoot::VanillaKey();
                    vk != 0 && btn->GetIDCode() == static_cast<std::uint32_t>(vk)) {
                    const bool on = !g_vanillaKey.load();
                    g_vanillaKey.store(on);
                    logger::warn("[INV] vanilla passthrough {} (F11)",
                                 on ? "ON -- the engine gets the next inventory"
                                    : "OFF -- the grid is back");
                    SKSE::GetTaskInterface()->AddUITask([on]() {
                        FUI::Sfx::Notify(on ? "Vanilla inventory (F11)"
                                            : "Grid inventory (F11)");
                    });
                    continue;
                }
                // The game's Inventory key closes our menu. This sink sits
                // UPSTREAM of input-context filtering, so it still sees the
                // raw key while kMenuMode swallows the user event.
                /*
                 * [vulkaar] TAB ROUVRE LE MENU CARREFOUR, MEME SOUS SKYMP.
                 *
                 * Le client skymp laisse le jeu en mode chargen en permanence
                 * (enforceLimitationsService : setInChargen(true, true, ...)),
                 * pour interdire la sauvegarde et l attente en multijoueur.
                 * Le moteur verrouille la TOUCHE du menu carrefour derriere ce
                 * meme garde — alors que ses quatre boutons (Objets, Magie,
                 * Competences, Carte) ne touchent ni a l un ni a l autre. Le
                 * verrou est plus large que ce qu il protege.
                 *
                 * On passe donc par la file de messages : le garde est sur la
                 * touche, pas sur le menu. La sauvegarde et l attente restent
                 * interdites — on ne leve RIEN du mode chargen.
                 */
                {
                    auto* uiT = RE::UI::GetSingleton();
                    auto* cmT = RE::ControlMap::GetSingleton();
                    auto* ue  = RE::UserEvents::GetSingleton();
                    auto tscan = (cmT && ue)
                        ? cmT->GetMappedKey(ue->tweenMenu, RE::INPUT_DEVICE::kKeyboard)
                        : 0xFF;
                    if (tscan == 0xFF || tscan == 0xFFFFFFFF) tscan = 0x0F;   // Tab
                    if (btn->GetIDCode() == tscan && uiT && !uiT->GameIsPaused() &&
                        !uiT->IsMenuOpen(RE::TweenMenu::MENU_NAME) &&
                        !uiT->IsMenuOpen("GridInventoryMenu"sv) &&
                        !FUI::UIRoot::IsTextInputActive()) {
                        SKSE::GetTaskInterface()->AddUITask([]() {
                            auto* ui2 = RE::UI::GetSingleton();
                            // Reverifie sur le fil UI : l etat a pu bouger entre
                            // la touche et la tache.
                            if (!ui2 || ui2->GameIsPaused()) return;
                            if (ui2->IsMenuOpen(RE::TweenMenu::MENU_NAME)) return;
                            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                                mq->AddMessage(RE::TweenMenu::MENU_NAME,
                                    RE::UI_MESSAGE_TYPE::kShow, nullptr);
                                logger::info("[INV] Tab -> TweenMenu (contournement du mode chargen skymp)");
                            }
                        });
                    }
                }

                if (auto* ui = RE::UI::GetSingleton();
                    ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
                    auto* cm = RE::ControlMap::GetSingleton();
                    // NOTE: GetMappedKey returns 0xFF here (confirmed in
                    // the log) - fall back to the default I scancode.
                    auto scan = cm ? cm->GetMappedKey(
                        RE::UserEvents::GetSingleton()->inventory,
                        RE::INPUT_DEVICE::kKeyboard) : 0xFF;
                    if (scan == 0xFF || scan == 0xFFFFFFFF) scan = 0x17;   // default I
                    if (btn->GetIDCode() == scan) {
                        // input thread: defer state changes to the UI task
                        SKSE::GetTaskInterface()->AddUITask([]() {
                            if (FUI::UIRoot::IsTextInputActive()) {
                                return;   // typing 'i' into a text field
                            }
                            if (FUI::Grid::IsHolding()) {
                                FUI::Grid::CancelHold();
                            } else if (!FUI::UIRoot::CloseTopWindow()) {
                                // settings/EDIT close first; then the inventory
                                FUI::UIRoot::Close();
                            }
                        });
                    }
                    // ★(1.3.1) the game's MAGIC key hops out: close the grid
                    // and raise the vanilla MagicMenu, journal-style. The
                    // kItemMenu context never translates this key into a user
                    // event, so it is read raw here exactly like the
                    // Inventory key above (same 0xFF fallback story).
                    static const RE::BSFixedString s_magicEvent("Magic");
                    auto mscan = cm ? cm->GetMappedKey(s_magicEvent,
                        RE::INPUT_DEVICE::kKeyboard) : 0xFF;
                    if (mscan == 0xFF || mscan == 0xFFFFFFFF) mscan = 0x19;   // default P
                    if (btn->GetIDCode() == mscan) {
                        SKSE::GetTaskInterface()->AddUITask([]() {
                            if (FUI::UIRoot::IsTextInputActive()) {
                                return;   // typing 'p' into a text field
                            }
                            // [vulkaar] nos ecrans (maison, etabli) n'ont pas de
                            // saut de menu -- cette route brute passe AVANT le
                            // canal des evenements utilisateur, elle a sa garde.
                            if (FUI::Appartenance::Ouvert() || FUI::Etabli::Ouvert()) {
                                return;
                            }
                            // plain inventory only: a loot/barter session has
                            // a partner to tear down, and vanilla refuses menu
                            // hopping out of those screens too
                            if (FUI::LootBarter::CurrentMode() !=
                                FUI::LootBarter::Mode::kNormal) {
                                return;
                            }
                            FUI::UIRoot::Close();
                            if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                                q->AddMessage(RE::MagicMenu::MENU_NAME,
                                              RE::UI_MESSAGE_TYPE::kShow, nullptr);
                            }
                        });
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---- Tetris footprint / orientation definitions ----
    // Logic layer works purely in GRID CELLS (design-independent): an item
    // occupies w x h cells; pixel size, gaps and art are the UI layer's business.
    // Phase 2 D2: the def struct + its ini serialization live in ui/ItemDef.h
    // (ONE struct shared by every module; field metatable drives parse/format)
    using ItemDef = FUI::ItemDef;
    std::unordered_map<std::string, ItemDef> g_itemDefs;   // user overrides

    constexpr const char* kDefsPath = "Data/SKSE/Plugins/GridInventory_items.ini";
    constexpr const char* kUniquePath = "Data/SKSE/Plugins/GridInventory_unique.ini";

    // Stable item key across load orders: "Plugin.esp|0xLocalID"
    //
    // A runtime-created form (player-brewed potion, player-enchanted weapon)
    // has NO source file, and GetLocalFormID() dereferences that file without
    // checking it -- so it must never be called for one. Such a form has no
    // local id anyway; its whole FormID is the identity.
    std::string FormKey(RE::TESForm* a_form)
    {
        auto* file = a_form->GetFile(0);
        std::string key = file ? std::string(file->GetFilename()) : "Dynamic";
        char buf[16];
        std::snprintf(buf, sizeof(buf), "|0x%06X",
            file ? a_form->GetLocalFormID() : a_form->GetFormID());
        return key + buf;
    }

    RE::TESBoundObject* FormFromKey(const std::string& a_key)
    {
        const auto bar = a_key.find('|');
        if (bar == std::string::npos) return nullptr;
        std::uint32_t local = 0;
        try {
            local = static_cast<std::uint32_t>(
                std::stoul(a_key.substr(bar + 1), nullptr, 16));
        } catch (...) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return nullptr;
        auto* form = dh->LookupForm(local, a_key.substr(0, bar));
        return form ? form->As<RE::TESBoundObject>() : nullptr;
    }

    // ---- model-level def sharing ----
    // Enchant variants / keys / notes / potions are hundreds of records over
    // ONE nif (measured: 10,711 records -> 2,143 unique models). Tuning the
    // base item should tune every sibling: items without their own override
    // fall back to the def of ANY overridden item sharing their model path.
    std::string ModelPathOf(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return {};
        const char* p = nullptr;
        // ★★THIS CAST TOOK THE GAME DOWN, and the shape of the failure says
        // what kind of pointer reached it: As<> survived (it reads the form
        // TYPE, a plain field) and skyrim_cast threw __non_rtti_object (it
        // reads the VTABLE). So the memory was readable and the vtable was not
        // a game vtable -- a pointer into something that is not a form.
        //
        // Where it comes from is still open: the icon path is fed a tile's
        // object, and a view holds INDICES into g_items rather than copies, so
        // a stale index is the obvious candidate (guarded separately in
        // Grid.cpp). Either way a bad icon is not worth a CTD, and the log line
        // below is what will identify the source next time instead of another
        // round of guessing.
        // ★The pointer is printed, not the name: asking a suspect object for
        // its name is the very dereference that just failed.
        try {
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
                // armor is NOT a TESModel — its GND model lives on the biped form
                p = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
            } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
                p = mdl->GetModel();
            }
        } catch (...) {
            static std::set<const void*> s_said;
            if (s_said.size() < 8 && s_said.insert(a_obj).second) {
                SKSE::log::error("[DEFS] model lookup refused a non-form pointer "
                                 "({}); icon skipped", static_cast<const void*>(a_obj));
            }
            return {};
        }
        if (!p || !*p) return {};
        std::string s(p);
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (c == '/') c = '\\';
        }
        if (s.rfind("meshes\\", 0) == 0) s.erase(0, 7);
        return s;
    }

    // ★★Is this BOOK record a NOTE (a loose page / letter) rather than a book?
    //
    // The record cannot answer. OBJ_BOOK carries a Type with a kNoteScroll
    // value, and it is 0 on every book in the load order — 1,135 of 1,135,
    // matching an earlier count of 1,134 of 1,134 on a different load order.
    // Skyrim never fills the field. (The library could not read it anyway:
    // kNoteScroll is -1 while the member is a uint8_t, so 0xFF != -1 and the
    // comparison is always false.) The mesh is the only signal left.
    //
    // ★Matched on the FILE NAME's start, not anywhere in the path. A bare
    // find("note") also hits 'notebook', 'denote', and any folder someone
    // named "notes" — a short substring against a whole path is how the armour
    // classifier once mis-sorted silently. Vanilla notes are
    // 'clutter\books\note0N.nif', so the file simply begins with it.
    // The nif's file name alone (no directories) — every mesh-based rule below
    // works on this, never on the whole path, for the reason in the note above.
    [[nodiscard]] std::string ModelFileOf(RE::TESBoundObject* a_obj)
    {
        std::string mp = ModelPathOf(a_obj);   // lowercased, backslashes
        const auto slash = mp.rfind('\\');
        return slash == std::string::npos ? mp : mp.substr(slash + 1);
    }

    [[nodiscard]] bool IsNoteMesh(RE::TESBoundObject* a_obj)
    {
        return ModelFileOf(a_obj).starts_with("note");
    }

    // ★★An INGOT and an ORE carry the exact same keyword.
    //
    // Vanilla ships one VendorItemOreIngot for both — measured across two load
    // orders (60 and 90 records), no second keyword separates them anywhere.
    // So the mesh again. 'ingot' is long enough to match anywhere in the file
    // name safely, which is what picks up 'madnessingot01' and 'dummyingot'
    // alongside the plain 'ingotEbony' family.
    //
    // ★Only the ingot side gets a rule. The ore side would need find("ore"),
    // and a three-letter needle hides inside score/store/forest — the same trap
    // that IsNoteMesh() sidesteps. Everything that is not an ingot simply stays
    // misc_ore, exactly as before.
    [[nodiscard]] bool IsIngotMesh(RE::TESBoundObject* a_obj)
    {
        return ModelFileOf(a_obj).find("ingot") != std::string::npos;
    }

    // ★A DRINK and a plate of food also share one keyword (VendorItemFood), so
    // this is best effort: a drink whose mesh is not named after any of these
    // simply stays "food", which is where it sat before this category existed.
    //
    // ★No "ale" and no "rum" — three-letter needles hide inside whale and
    // drumstick. Every word here is >=4 chars and was checked against the 242
    // distinct food meshes of the test load orders for false hits.
    [[nodiscard]] bool IsDrinkMesh(RE::TESBoundObject* a_obj)
    {
        static constexpr std::string_view kWords[] = {
            "wine", "mead", "flagon", "tankard", "liquor", "brandy", "grog",
            "goblet", "bottle", "drink", "milkjug", "waterskin", "skooma",
            "jagga", "matze", "shein", "sujamma",
        };
        const std::string file = ModelFileOf(a_obj);
        if (file.empty()) return false;
        for (const auto w : kWords) {
            if (file.find(w) != std::string::npos) return true;
        }
        return false;
    }

    std::unordered_map<std::string, ItemDef> g_modelDefs;   // derived, lazy
    bool g_modelDefsDirty = true;

    void RebuildModelDefs()
    {
        g_modelDefsDirty = false;
        g_modelDefs.clear();
        // deterministic first-wins: iterate overrides in sorted key order
        std::vector<const std::string*> keys;
        keys.reserve(g_itemDefs.size());
        for (const auto& kv : g_itemDefs) keys.push_back(&kv.first);
        std::sort(keys.begin(), keys.end(),
            [](const std::string* a, const std::string* b) { return *a < *b; });
        for (const auto* k : keys) {
            auto* obj = FormFromKey(*k);
            if (!obj) continue;
            auto mp = ModelPathOf(obj);
            if (!mp.empty()) g_modelDefs.emplace(std::move(mp), g_itemDefs[*k]);
        }
        logger::info("[DEFS] model-level defs: {}", g_modelDefs.size());
    }

    // ---- category presets (H7: data-driven so preset files can carry them) ----
    std::unordered_map<std::string, ItemDef> g_catDefs;

    void InitCategoryDefs()   // factory values (the user-tuned in-game presets)
    {
        g_catDefs.clear();
        auto put = [&](const char* k, int w, int h, float rx, float ry, float rz, float sc) {
            ItemDef d;
            d.w = w; d.h = h; d.rx = rx; d.ry = ry; d.rz = rz; d.scale = sc;
            g_catDefs[k] = d;
        };
        // 세분화 분류 (Docs/스카이림_아이템_분류_및_상징_아이템.md): each new
        // key seeds from its legacy parent's factory values so resolved defs
        // (and pak capture keys) stay identical until the user tunes them.
        put("weap_dagger",     1, 2, -90, 0, 0, 1.0f);
        put("weap_sword",      1, 3, -90, 0, 0, 1.0f);     // <- weap_1h
        put("weap_waraxe",     1, 3, -90, 0, 0, 1.0f);     // <- weap_1h
        put("weap_mace",       1, 3, -90, 0, 0, 1.0f);     // <- weap_1h
        put("weap_greatsword", 1, 4, -90, 0, 0, 1.0f);     // <- weap_2h
        put("weap_staff",      1, 4, -90, 0, 0, 1.0f);     // <- weap_2h
        put("weap_battleaxe",  2, 4, -90, 0, 15, 1.0f);
        put("weap_warhammer",  2, 4, -90, 0, 15, 1.0f);    // <- weap_battleaxe
        put("weap_bow",        2, 4, -90, 0, 0, 1.0f);
        put("weap_crossbow",   2, 3, -90, 0, 0, 1.0f);
        put("weap_other",      1, 1, -90, 0, 0, 1.0f);
        put("ammo_arrow",      2, 2, -90, 0, -135, 1.2f);  // <- ammo
        put("ammo_bolt",       2, 2, -90, 0, -135, 1.2f);  // <- ammo
        put("armor_ring",      1, 1, -90, 0, 0, 1.0f);
        put("armor_amulet",    1, 1, -90, 0, 0, 1.0f);
        put("armor_circlet",   2, 2, -90, 0, 0, 1.0f);     // <- armor_head
        put("armor_body_heavy", 2, 3, -90, 0, 180, 0.8f);  // <- armor_body
        put("armor_body_light", 2, 3, -90, 0, 180, 0.8f);  // <- armor_body
        put("armor_cloth",     2, 3, -90, 0, 180, 0.8f);   // <- armor_body
        put("armor_shield",    2, 3, -90, 180, 0, 0.9f);
        put("armor_hands",     2, 2, -90, 0, 180, 1.0f);
        put("armor_gloves",    2, 2, -90, 0, 180, 1.0f);   // <- armor_hands
        put("armor_feet",      2, 2, 0, 0, 165, 1.0f);
        put("armor_shoes",     2, 2, 0, 0, 165, 1.0f);     // <- armor_feet
        put("armor_head",      2, 2, -90, 0, 0, 1.0f);
        put("armor_hood",      2, 2, -90, 0, 0, 1.0f);     // <- armor_head
        put("armor_accessory", 2, 3, -90, 0, 180, 0.8f);   // <- armor_cloth
        put("book",            1, 2, -90, 0, 0, 1.0f);
        put("book_skill",      1, 2, -90, 0, 0, 1.0f);     // <- book
        put("book_spell",      1, 2, -90, 0, 0, 1.0f);     // <- book
        put("book_note",       1, 2, -90, 0, 0, 1.0f);     // <- book
        put("scroll",          2, 1, -90, 0, 0, 1.0f);
        put("potion",          1, 1, 0, 0, -90, 1.0f);
        put("poison",          1, 1, 0, 0, -90, 1.0f);     // <- potion
        put("food",            1, 1, 0, 0, -90, 1.0f);     // <- potion
        put("food_raw",        1, 1, 0, 0, -90, 1.0f);     // <- food
        put("food_drink",      1, 1, 0, 0, -90, 1.0f);     // <- food
        put("ingredient",      1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("soulgem",         1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("key",             1, 1, 0, 0, 90, 9.5f);
        put("misc_ore",        1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_ingot",      1, 1, -90, 0, 0, 1.0f);     // <- misc_ore
        put("misc_gem",        1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_hide",       1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_animalpart", 1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_tool",       1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_clutter",    1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc",            1, 1, -90, 0, 0, 1.0f);
    }

    const char* CategoryOf(RE::TESBoundObject* a_obj)
    {
        if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) {
            switch (weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandDagger: return "weap_dagger";
            case RE::WEAPON_TYPE::kOneHandSword:  return "weap_sword";
            case RE::WEAPON_TYPE::kOneHandAxe:    return "weap_waraxe";
            case RE::WEAPON_TYPE::kOneHandMace:   return "weap_mace";
            case RE::WEAPON_TYPE::kTwoHandSword:  return "weap_greatsword";
            case RE::WEAPON_TYPE::kStaff:         return "weap_staff";
            case RE::WEAPON_TYPE::kTwoHandAxe:
                // the engine folds warhammers into TwoHandAxe; the keyword
                // is the canonical discriminator (same rule in the tool)
                return weap->HasKeywordString("WeapTypeWarhammer")
                           ? "weap_warhammer" : "weap_battleaxe";
            case RE::WEAPON_TYPE::kBow:           return "weap_bow";
            case RE::WEAPON_TYPE::kCrossbow:      return "weap_crossbow";
            default:                              return "weap_other";
            }
        }
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            using S = RE::BGSBipedObjectForm::BipedObjectSlot;
            if (armo->HasPartOf(S::kAmulet))  return "armor_amulet";
            if (FUI::Grid::IsRing(armo))      return "armor_ring";
            // circlet = slot 42 WITHOUT a head/hair slot (helmets add slot 42
            // to their mask just to hide circlets — those stay armor_head)
            if (armo->HasPartOf(S::kCirclet) && !armo->HasPartOf(S::kHead) &&
                !armo->HasPartOf(S::kHair)) {
                return "armor_circlet";
            }
            if (armo->HasPartOf(S::kBody)) {
                switch (armo->GetArmorType()) {
                case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor:
                    return "armor_body_heavy";
                case RE::BGSBipedObjectForm::ArmorType::kClothing:
                    return "armor_cloth";
                default:
                    return "armor_body_light";
                }
            }
            if (armo->HasPartOf(S::kShield)) return "armor_shield";
            // clothing-type limb/head gear is a glove, a shoe, a hood — soft
            // things that hang differently from the plate they share a slot
            // with. Same discriminator the body already splits on.
            const bool cloth =
                armo->GetArmorType() == RE::BGSBipedObjectForm::ArmorType::kClothing;
            if (armo->HasPartOf(S::kHands)) return cloth ? "armor_gloves" : "armor_hands";
            if (armo->HasPartOf(S::kFeet))  return cloth ? "armor_shoes"  : "armor_feet";
            if (armo->HasPartOf(S::kHead) || armo->HasPartOf(S::kHair) ||
                armo->HasPartOf(S::kCirclet)) {
                return cloth ? "armor_hood" : "armor_head";
            }
            // B10: custom biped slots (capes 46, backpacks, accessories...)
            // used to be swallowed by the armor_head fallback (2x2 helmet
            // defaults), then by armor_cloth — but a cape is not a robe, and
            // this is 908 records on a heavy load order, so it owns a category.
            return "armor_accessory";
        }
        if (auto* book = a_obj->As<RE::TESObjectBOOK>()) {
            if (book->TeachesSpell()) return "book_spell";
            if (book->TeachesSkill()) return "book_skill";
            if (IsNoteMesh(a_obj))    return "book_note";
            return "book";
        }
        if (a_obj->Is(RE::FormType::Scroll)) return "scroll";
        if (auto* ammo = a_obj->As<RE::TESAmmo>()) {
            // ★NOT TESAmmo::IsBolt() -- it reads a member whose offset moves
            // between SE and AE, so on AE every bolt came back an arrow. The one
            // correct answer lives in Fallback (see Fallback::IsBoltAmmo).
            return FUI::Fallback::IsBoltAmmo(ammo) ? "ammo_bolt" : "ammo_arrow";
        }
        if (auto* alch = a_obj->As<RE::AlchemyItem>()) {
            if (alch->IsPoison()) return "poison";
            if (alch->IsFood()) {
                // ★mesh before keyword here, the opposite of everywhere else:
                // a category only decides a default SHAPE, and the bottle is
                // what makes a drink different. Vanilla milk carries
                // VendorItemFoodRaw yet comes in a jug — it wants the bottle.
                if (IsDrinkMesh(a_obj))                        return "food_drink";
                if (alch->HasKeywordString("VendorItemFoodRaw")) return "food_raw";
                return "food";
            }
            return "potion";
        }
        if (a_obj->Is(RE::FormType::Ingredient)) return "ingredient";
        if (a_obj->Is(RE::FormType::SoulGem))    return "soulgem";
        if (a_obj->Is(RE::FormType::KeyMaster))  return "key";
        if (auto* misc = a_obj->As<RE::TESObjectMISC>()) {
            if (misc->HasKeywordString("VendorItemOreIngot")) {
                return IsIngotMesh(a_obj) ? "misc_ingot" : "misc_ore";
            }
            if (misc->HasKeywordString("VendorItemGem"))        return "misc_gem";
            if (misc->HasKeywordString("VendorItemAnimalHide")) return "misc_hide";
            // narrowest first: a bone or a hammer also carries VendorItemClutter
            if (misc->HasKeywordString("VendorItemAnimalPart")) return "misc_animalpart";
            if (misc->HasKeywordString("VendorItemTool"))       return "misc_tool";
            if (misc->HasKeywordString("VendorItemClutter"))    return "misc_clutter";
        }
        return "misc";
    }

    ItemDef DefaultDef(RE::TESBoundObject* a_obj)
    {
        if (g_catDefs.empty()) InitCategoryDefs();
        const auto it = g_catDefs.find(CategoryOf(a_obj));
        return it != g_catDefs.end() ? it->second : ItemDef{};
    }


    // Upsert (or remove, when a_def==nullptr) one item's line in the override ini —
    // the in-game editor writes through this, so hand-edits elsewhere are preserved.
    void UpsertDefLine(const std::string& a_key, const ItemDef* a_def, const std::string& a_name)
    {
        std::vector<std::string> lines;
        {
            std::ifstream in(kDefsPath);
            std::string l;
            while (std::getline(in, l)) lines.push_back(l);
        }
        if (lines.empty()) {
            lines.push_back("; GridInventory item overrides (edited in-game via the EDIT mode)");
            lines.push_back("; key = w:, h:, rx:, ry:, rz:, scale:   or   shape:11|10|10 (rows of 1/0)");
        }
        bool done = false;
        for (auto it = lines.begin(); it != lines.end(); ++it) {
            const auto eq = it->find('=');
            if (eq == std::string::npos) continue;
            std::string k = it->substr(0, eq);
            k.erase(0, k.find_first_not_of(" \t"));
            k.erase(k.find_last_not_of(" \t") + 1);
            if (k != a_key) continue;
            if (a_def) {
                *it = FormatItemDef(a_key, *a_def);
            } else {
                // ★Take the "; Name" comment written directly above with it.
                // Erasing the entry alone leaves the comment behind, where it
                // then reads as the label of the NEXT, unrelated item — 211 of
                // those had piled up in the shipped file. Index >= 2 keeps the
                // two header comments safe.
                auto first = it;
                if (it != lines.begin()) {
                    const auto prev = std::prev(it);
                    if (std::distance(lines.begin(), prev) >= 2 &&
                        !prev->empty() && prev->front() == ';') {
                        first = prev;
                    }
                }
                lines.erase(first, std::next(it));
            }
            done = true;
            break;
        }
        if (!done && a_def) {
            if (!a_name.empty()) lines.push_back("; " + a_name);
            lines.push_back(FormatItemDef(a_key, *a_def));
        }
        if (std::ofstream out(kDefsPath, std::ios::trunc); out) {
            for (const auto& l : lines) out << l << "\n";
        }
    }

    ItemDef DefFor(RE::TESBoundObject* a_obj)
    {
        if (auto it = g_itemDefs.find(FormKey(a_obj)); it != g_itemDefs.end()) {
            return it->second;
        }
        // G1: Coin_Pouch is 2x2 (4 cells) out of the box; coins keep the 1x1
        // misc default. User ini overrides above still win (icon tuning).
        if (FUI::GoldCoins::IsPouch(a_obj->GetFormID())) {
            ItemDef d = DefaultDef(a_obj);
            d.w = 2;
            d.h = 2;
            return d;
        }
        // model-level fallback: an overridden sibling sharing this nif
        // (enchant variants etc.) donates its def before the category default
        if (g_modelDefsDirty) RebuildModelDefs();
        if (!g_modelDefs.empty()) {
            if (auto mp = ModelPathOf(a_obj); !mp.empty()) {
                if (auto it = g_modelDefs.find(mp); it != g_modelDefs.end()) {
                    return it->second;
                }
            }
        }
        return DefaultDef(a_obj);
    }

    // ★★WHICH ITEMS WEAR THE UNIQUE MARK, beyond what the record can say.
    // The built-in rule is "its enchantment has no base enchantment", i.e. one
    // the player can never learn -- that finds the Daedric artifacts and the
    // named uniques that carry a bespoke enchantment, and nothing else. A
    // unique that is unenchanted (the Longhammer, Valdr's Lucky Dagger),
    // scripted (Nettlebane, the Bloodskal Blade) or plainly disenchantable
    // (Grimsever, Okin, Eduj) is indistinguishable from ordinary gear in the
    // data, and a MOD's artifacts are invisible to any rule at all.
    // One form per line, in the same key the rest of this file uses:
    //     Skyrim.esm|0x01C492          ; on
    //     Skyrim.esm|0x01C492 = 0      ; off -- overrides the rule the other way
    void LoadUniqueDefs()
    {
        std::unordered_map<RE::FormID, bool> out;
        std::ifstream in(kUniquePath);
        std::string   line;
        while (in && std::getline(in, line)) {
            if (const auto c = line.find_first_of(";#"); c != std::string::npos) {
                line.erase(c);
            }
            std::string key = line;
            bool        on = true;
            if (const auto eq = key.find('='); eq != std::string::npos) {
                std::string val = key.substr(eq + 1);
                key.erase(eq);
                val.erase(0, val.find_first_not_of(" 	"));
                on = !val.empty() && val[0] != '0';
            }
            key.erase(0, key.find_first_not_of(" 	"));
            if (const auto e = key.find_last_not_of(" 	"); e != std::string::npos) {
                key.erase(e + 1);
            } else {
                continue;
            }
            if (key.empty()) continue;
            // ★Resolved through the data handler, so the load order may put the
            // plugin anywhere -- and a line naming a plugin the player does not
            // have is simply skipped rather than being an error.
            if (auto* obj = FormFromKey(key)) out[obj->GetFormID()] = on;
        }
        FUI::Grid::SetUniqueOverrides(std::move(out));
    }

    // User override file, one line per item (hot-reloaded on every inventory open):
    //   Skyrim.esm|0x0001397E = w:1, h:4, rx:-90, ry:0, rz:0, scale:1.0
    void LoadItemDefs()
    {
        g_itemDefs.clear();
        {
            std::ifstream in(kDefsPath);
            std::string line;
            while (in && std::getline(in, line)) {
                if (line.empty() || line[0] == ';' || line[0] == '#') continue;
                const auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                // shared metatable parser (ui/ItemDef.h) over factory defaults
                g_itemDefs[key] = ParseItemDef(line.substr(eq + 1), ItemDef{});
            }
        }
        // §RELEASE-B: the SHIPPED bags (Grid Inventory.esp Satchel 0x818 /
        // Knapsack 0x819) must act as bags out of the box, with no ini to
        // ship. Values mirror the author's Default preset. A user line parsed
        // above always wins; "Reset Default" erases the line and these seeds
        // return on the next launch -- i.e. THIS is their factory default.
        static constexpr std::pair<const char*, const char*> kShippedBagDefs[] = {
            { "Grid Inventory.esp|0x000818",   // Satchel
              "w:1, h:1, rx:90, ry:0, rz:180, scale:1.00, bag:1, bw:4, bh:4" },
            { "Grid Inventory.esp|0x000819",   // Knapsack
              "w:2, h:2, rx:0, ry:1, rz:90, scale:1.00, bag:1, bw:8, bh:4" },
            // ---- typed bags: the accept token is what makes them typed ----
            { "Grid Inventory.esp|0x00081A",   // Alchemy Pouch
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:8, bh:5, accept:alchemy" },
            { "Grid Inventory.esp|0x00081B",   // Ore Sack
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:5, accept:ore" },
            { "Grid Inventory.esp|0x00081C",   // Hide Roll
              "w:2, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:5, accept:hide" },
            { "Grid Inventory.esp|0x00081D",   // Potion Bag
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:5, accept:potion" },
            { "Grid Inventory.esp|0x00081E",   // Soul Gem Pouch
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:4, accept:soulgem" },
            { "Grid Inventory.esp|0x00081F",   // Key Pouch
              "w:2, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:5, bh:5, accept:key" },
            // ---- general purpose ----
            { "Grid Inventory.esp|0x000820",   // Small Leather Pouch
              "w:1, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:4, bh:4" },
            { "Grid Inventory.esp|0x000821",   // Leather Satchel
              "w:1, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:4, bh:5" },
            { "Grid Inventory.esp|0x000822",   // Belt Pouch
              "w:2, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:6, bh:4" },
            { "Grid Inventory.esp|0x000823",   // Witching Pouch
              "w:1, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:4, bh:6" },
            { "Grid Inventory.esp|0x000824",   // Canvas Pack
              "w:3, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:8, bh:5" },
            { "Grid Inventory.esp|0x000825",   // Buckled Satchel
              "w:3, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:8, bh:6" },
            { "Grid Inventory.esp|0x000826",   // Backframe Pack
              "w:2, h:3, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:5, bh:10" },
            { "Grid Inventory.esp|0x000827",   // Adventure Satchel
              "w:3, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:9, bh:6" },
            { "Grid Inventory.esp|0x000828",   // Exploration Pack
              "w:2, h:2, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:10, bh:8" },
            { "Grid Inventory.esp|0x000829",   // Mysterious Bag
              "w:1, h:1, rx:90, ry:0, rz:0, scale:1.00, bag:1, bw:10, bh:14" },
        };
        for (const auto& [key, val] : kShippedBagDefs) {
            if (!g_itemDefs.contains(key)) {
                g_itemDefs[key] = ParseItemDef(val, ItemDef{});
            }
        }
        g_modelDefsDirty = true;

        // [vulkaar] Le bois du bucheron a SA place dans la planche.
        //
        // ★LA VIGNETTE EST UNE PHOTO, PAS UN DESSIN RETOUCHE. Regle posee par
        // le proprietaire le 27/08/2026, apres qu'il eut lache une buche au
        // sol : « ce n'est pas le meme model 3d que celui de l'inventaire ».
        // Il avait raison, et c'etait ma faute — j'avais fait emprunter a la
        // buche le modele du tronc (modelfrom) puis coupe l'image au quart
        // (cropy), et aminci le tronc au dessin (sqx). L'objet du monde, lui,
        // n'avait pas bouge : l'inventaire mentait sur ce qu'on portait.
        //
        // Ces champs de cuisson EXISTENT toujours (ItemDef.h) et restent bons
        // a prendre pour un cas ou l'image doit differer sciemment. Ils ne
        // servent PLUS ici : deux objets qui different a l'oeil doivent
        // differer par leur MODELE, dans l'esp, pas par leur vignette.
        //
        // Ce qui reste est du CADRAGE, qui ne change pas l'objet :
        //   spin:1  couche les pixels. A rx:-90 la capture sort DEBOUT
        //           (192 x 778 mesure au journal du greffon : longueur sur Y,
        //           diametre sur X, 778/192 = 4.05 colle a l'OBND du nif,
        //           432/106 = 4.08) alors que le cadre du tronc fait 9 de
        //           large sur 3 de haut. Une premiere lecture avait conclu
        //           l'inverse depuis une capture d'ecran ou l'objet avait ete
        //           tourne a la main : croire le journal, jamais l'oeil.
        //   w/h     l'emprise demandee par le proprietaire, et scale le zoom
        //           dans la tuile (draw seul, hors cle de cache).
        //
        // Semees comme les sacs d'usine : une ligne utilisateur dans
        // GridInventory_items.ini gagne toujours -- le proprietaire ajuste en
        // jeu (bouton EDIT), Save, et sa ligne prime sur celles-ci.
        static constexpr std::pair<const char*, const char*> kVulkaarDefs[] = {
            // ---- le tronc (8 qualites, vulkaar_metiers.esp) ----
            { "vulkaar_metiers.esp|0x0009A0",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A1",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A2",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A3",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A4",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A5",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A6",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            { "vulkaar_metiers.esp|0x0009A7",
              "w:9, h:3, rx:-90, ry:0, rz:0, scale:1.00, stack:2, spin:1" },
            // ---- la buche (8 qualites) : le tronc de la meme planche,
            //      scie au quart -- voir l'en-tete du bloc ----
            { "vulkaar_metiers.esp|0x0009B0",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B1",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B2",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B3",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B4",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B5",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B6",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            { "vulkaar_metiers.esp|0x0009B7",
              "w:3, h:3, rx:-90, ry:0, rz:0, scale:0.75, stack:5, spin:1" },
            // ---- le petit bois (8 qualites, vulkaar_metiers.esp) ----
            { "vulkaar_metiers.esp|0x0009B8",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009B9",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009BA",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009BB",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009BC",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009BD",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009BE",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
            { "vulkaar_metiers.esp|0x0009BF",
              "w:2, h:2, rx:-90, ry:0, rz:0, scale:1.00, stack:10" },
        };
        for (const auto& [key, val] : kVulkaarDefs) {
            if (!g_itemDefs.contains(key)) {
                g_itemDefs[key] = ParseItemDef(val, ItemDef{});
            }
        }

        // ★Typed bags: an accept token naming a filter that does not exist is
        // the quiet failure mode of this whole feature — the bag simply takes
        // nothing and looks like a routing bug. Name it at load, once.
        int bags = 0, typed = 0;
        for (const auto& [key, d] : g_itemDefs) {
            if (!d.bag) continue;
            ++bags;
            if (d.accept.empty()) continue;
            ++typed;
            bool known = false;
            for (int i = 0; i < FUI::BagFilter::Count(); ++i) {
                if (d.accept == FUI::BagFilter::Id(i)) { known = true; break; }
            }
            if (!known) {
                logger::warn("[DEFS] {}: accept:{} is not a known filter - "
                             "this bag will accept nothing", key, d.accept);
            }
        }
        logger::info("[DEFS] {} item overrides loaded ({} bags, {} typed)",
            g_itemDefs.size(), bags, typed);

        // ★Hand the merchant seeder the bag list derived from THESE defs. A
        // FormID table inside GoldCoins would be a second source of truth: add
        // a bag here and the shops would quietly never stock it.
        //
        // ★OUR esp only. Marking an item as a bag in EDIT says "I want to use
        // this as a bag", not "put this on a shopkeeper's shelf" — and putting
        // another mod's item into a vendor chest rewrites THAT mod's intended
        // acquisition. Measured on the author's load order: 16 foreign packs
        // had been designated, diluting the rotation pool to 28 so the 12
        // shipped general bags drew less than half the time (one merchant
        // rolled three foreign backpacks in a row).
        {
            constexpr std::string_view kOurs = "grid inventory.esp|";
            std::vector<FUI::GoldCoins::BagWare> wares;
            int foreign = 0;
            for (const auto& [key, d] : g_itemDefs) {
                if (!d.bag) continue;
                std::string lower = key.substr(0, (std::min)(key.size(), kOurs.size()));
                for (auto& c : lower) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (lower != kOurs) { ++foreign; continue; }
                if (auto* obj = FormFromKey(key)) wares.push_back({ obj, d.accept });
            }
            if (foreign > 0) {
                logger::info("[VENDOR] {} user-designated bag(s) from other plugins"
                             " are NOT stocked (by design)", foreign);
            }
            FUI::GoldCoins::SetBagWares(std::move(wares));
        }
    }

    // ---- category ini (H7) ----
    constexpr const char* kCatsPath = "Data/SKSE/Plugins/GridInventory_categories.ini";

    void SaveCategoryDefs()
    {
        std::ofstream out(kCatsPath, std::ios::trunc);
        if (!out) return;
        out << "; Category defaults for items without a per-item override\n";
        out << "; 개별 오버라이드가 없는 아이템에 적용되는 카테고리 기본값\n";
        // deterministic order (diff-able files, stable across sessions)
        const std::map<std::string, ItemDef> sorted(g_catDefs.begin(), g_catDefs.end());
        for (const auto& [name, d] : sorted) {
            out << FormatItemDef(name, d) << "\n";
        }
    }

    // ---- drawn-icon transforms (GI60) ----
    // ONE LINE PER ICON KEY, not per item. A drawing is shared by everything
    // that resolves to it (472 swords all draw wpn_sword), so how big it sits
    // and which way it points belongs to the picture. Written by IconStudio,
    // read here; an item def that names its own value still wins at draw time.
    constexpr const char* kFlatPath = "Data/SKSE/Plugins/GridInventory_flaticons.ini";

    void SaveFlatIconDefs()
    {
        std::ofstream out(kFlatPath, std::ios::trunc);
        if (!out) return;
        out << "; Drawn-icon transforms -- ONE LINE PER ICON, not per item.\n";
        out << "; 그림 아이콘 자체의 확대·회전·좌우 위치입니다.\n";
        out << "; 아이템에 개별 값이 있으면 그쪽이 우선합니다 (카테고리 기본값과 같은 규칙).\n";
        char buf[128];
        for (const auto& [key, x] : FUI::Fallback::Xforms()) {
            std::snprintf(buf, sizeof(buf), "%s = fscale:%.2f, frot:%.0f, fx:%.2f",
                key.c_str(), x.scale, x.rot, x.x);
            out << buf << "\n";
        }
    }

    void LoadFlatIconDefs()
    {
        FUI::Fallback::ClearXforms();
        std::ifstream in(kFlatPath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            const auto b = key.find_first_not_of(" \t");
            const auto e = key.find_last_not_of(" \t\r");
            if (b == std::string::npos) continue;
            key = key.substr(b, e - b + 1);
            // reuse the item-def grammar so there is ONE parser for
            // "fscale:… , frot:… , fx:…" and it cannot drift
            const ItemDef d = ParseItemDef(line.substr(eq + 1), ItemDef{});
            FUI::Fallback::SetXform(key, { d.fscale, d.frot, d.fx });
        }
    }

    void LoadCategoryDefs()
    {
        InitCategoryDefs();
        std::ifstream in(kCatsPath);
        if (!in) return;

        // legacy migration: pre-split files carry the coarse keys — each one
        // seeds its finer children so resolved defs (and pak capture keys)
        // stay identical. Two-pass: children with their own explicit line
        // anywhere in the file must NOT be overwritten by the parent seed
        // (file order is not guaranteed, e.g. "poison" sorts before "potion").
        static const std::vector<std::pair<const char*, std::vector<const char*>>>
            kLegacySplit = {
                { "armor_jewelry",  { "armor_ring", "armor_amulet" } },
                { "weap_1h",        { "weap_sword", "weap_waraxe", "weap_mace" } },
                { "weap_2h",        { "weap_greatsword", "weap_staff" } },
                { "weap_battleaxe", { "weap_warhammer" } },
                { "ammo",           { "ammo_arrow", "ammo_bolt" } },
                { "armor_body",     { "armor_body_heavy", "armor_body_light", "armor_cloth" } },
                { "armor_head",     { "armor_circlet", "armor_hood" } },
                { "armor_hands",    { "armor_gloves" } },
                { "armor_feet",     { "armor_shoes" } },
                { "armor_cloth",    { "armor_accessory" } },
                { "book",           { "book_skill", "book_spell", "book_note" } },
                { "potion",         { "poison", "food" } },
                { "food",           { "food_raw", "food_drink" } },
                { "misc_ore",       { "misc_ingot" } },
                { "misc",           { "ingredient", "soulgem", "misc_ore", "misc_gem",
                                      "misc_hide", "misc_animalpart", "misc_tool",
                                      "misc_clutter" } },
            };

        std::vector<std::pair<std::string, std::string>> entries;   // key, values
        std::set<std::string> explicitKeys;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            entries.emplace_back(key, line.substr(eq + 1));
            explicitKeys.insert(key);
        }
        for (const auto& [key, values] : entries) {
            for (const auto& [parent, children] : kLegacySplit) {
                if (key != parent) continue;
                for (const char* child : children) {
                    if (explicitKeys.contains(child)) continue;
                    g_catDefs[child] = ParseItemDef(values, g_catDefs[child]);
                }
            }
            const auto it = g_catDefs.find(key);
            if (it != g_catDefs.end()) {
                it->second = ParseItemDef(values, it->second);
            }
        }
    }

    void RewriteItemDefsFile()
    {
        std::ofstream out(kDefsPath, std::ios::trunc);
        if (!out) return;
        out << "; GridInventory item overrides (edited in-game via the EDIT mode)\n";
        out << "; key = w:, h:, rx:, ry:, rz:, scale:   or   shape:11|10|10 (rows of 1/0)\n";
        // Phase 2: deterministic (sorted) order — the old unordered dump
        // reshuffled the whole file on every preset load — and the editor's
        // per-item name comments are regenerated by live form lookup instead
        // of being silently dropped.
        auto* dh = RE::TESDataHandler::GetSingleton();
        const std::map<std::string, ItemDef> sorted(g_itemDefs.begin(), g_itemDefs.end());
        for (const auto& [key, d] : sorted) {
            if (dh) {
                const auto bar = key.find('|');
                if (bar != std::string::npos && key.compare(bar + 1, 2, "0x") == 0) {
                    const auto localID = static_cast<RE::FormID>(
                        std::strtoul(key.c_str() + bar + 3, nullptr, 16));
                    if (auto* f = dh->LookupForm(localID, key.substr(0, bar))) {
                        if (const char* nm = f->GetName(); nm && *nm) {
                            out << "; " << nm << "\n";
                        }
                    }
                }
            }
            out << FormatItemDef(key, d) << "\n";
        }
    }

    // ---- Intercept the vanilla InventoryMenu: block it and open our UI instead ----
    // ---- Phase 3: menu open/close handling, one function per menu concern ----
    // Each returns true when the event is fully handled (stop processing).

    // Engine MessageBoxes (e.g. "apply poison to weapon?") render UNDER the
    // movie-less menu and can't be clicked through it — step aside while the
    // box is up, come back when it closes.
    bool HandleMessageBoxAside(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::MessageBoxMenu::MENU_NAME) return false;
        auto* ui = RE::UI::GetSingleton();
        if (a_event.opening && ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
            g_reopenAfterMsg = true;
            FUI::UIRoot::Close();
            logger::info("[INV] MessageBox opened -> stepping aside");
        } else if (!a_event.opening && g_reopenAfterMsg) {
            g_reopenAfterMsg = false;
            FUI::UIRoot::Open();
            logger::info("[INV] MessageBox closed -> back to the grid");
        }
        return true;
    }

    // While an ImGui text field owns the keyboard, hotkey menus must not open
    // over us — J still opened the Journal even with the user-event channel
    // swallowed (its open path is global). Intercept-and-close.
    bool HandleTextInputHotkeyBlock(const RE::MenuOpenCloseEvent& a_event)
    {
        if (!a_event.opening || !FUI::UIRoot::IsTextInputActive()) return false;
        if (a_event.menuName != RE::JournalMenu::MENU_NAME &&
            a_event.menuName != RE::TweenMenu::MENU_NAME &&
            a_event.menuName != RE::MapMenu::MENU_NAME &&
            a_event.menuName != RE::MagicMenu::MENU_NAME &&
            a_event.menuName != RE::StatsMenu::MENU_NAME &&
            a_event.menuName != RE::FavoritesMenu::MENU_NAME) {
            return false;
        }
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(a_event.menuName, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
        logger::info("[INV] blocked {} (text input active)", a_event.menuName.c_str());
        return true;
    }

    // ★★The vanilla favourites menu is closed on sight -- the quick wheel has
    // taken its place and answers to the same key. Without this, one press
    // opens both: the wheel reads the key from the input stream while the menu
    // opens through the engine's own path, and the two are not the same road.
    // ★No "swallow-then-open" dance like the inventory below needs. The wheel
    // is already up by the time this arrives; there is nothing to wait for.
    bool HandleFavoritesMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::FavoritesMenu::MENU_NAME) return false;
        // ★Measurement, not decoration. "The vanilla menu still does not
        // appear" has two very different causes -- we closed it, or the engine
        // never opened it -- and only the event itself can tell them apart. If
        // no line appears when the key is pressed, the press never became a
        // menu request and nothing on our side is holding it.
        logger::info("[FAV] FavoritesMenu {} (wheel={} yielding={})",
            a_event.opening ? "opening" : "closing",
            FUI::Wheeler::Enabled() ? 1 : 0,
            FUI::Wheeler::YieldingToVanilla() ? 1 : 0);
        // ★★★THE SWITCH LIVES HERE, not only on the wheel's own input. This is
        // the half that gives the vanilla screen back: with the wheel off it
        // opens exactly as it always did, hotkey binding and all. Gating only
        // the wheel would have left the menu suppressed with nothing put in
        // its place -- the favourites key would simply have stopped working.
        if (!FUI::Wheeler::Enabled()) return false;
        // ★...and the same stand-down the hotkey makes. In beast form the wheel
        // passes the key through, the engine opens its own menu -- and this
        // line used to shut it again a frame later, so the player saw NOTHING
        // open and stayed locked in the form (measured: the yield fired on
        // every press, no menu). The takeover has two halves and both have to
        // let go.
        if (FUI::Wheeler::YieldingToVanilla()) return false;
        if (!a_event.opening) return false;
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(RE::FavoritesMenu::MENU_NAME,
                RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
        return true;
    }

    // Vanilla InventoryMenu: swallow-then-open (our grid opens once the
    // vanilla menu finished closing).
    // ★★★ONE PLACE DECIDES WHETHER THE ENGINE KEEPS A SCREEN.
    //
    // Both switches -- the F11 key and the watch file -- used to be checked in
    // the inventory intercept alone. So F11 swapped your own bags and left the
    // MERCHANT's window as ours (reported), which is exactly the comparison the
    // key exists to make: a shop's stock is built by levelled lists, and the
    // only way to prove we hide none of it is to open the same shop both ways.
    //
    // Every screen we take over asks here now: inventory, container, barter.
    [[nodiscard]] bool VanillaPassthrough(const char* a_tag)
    {
        if (g_vanillaKey.load()) {
            logger::warn("[{}] vanilla passthrough is ON (F11) -- "
                         "the engine keeps this screen", a_tag);
            return true;
        }
        // ★ⓔⓖ WATCH: drop a file named GridInventory_vanilla.txt beside the
        // plugin and the vanilla screens open instead of ours, for as long as
        // it is there. It exists so the engine's own behaviour can be watched
        // from the same session that watches ours -- delete the file and the
        // grid comes back, no restart.
        std::error_code ec;
        if (std::filesystem::exists(
                "Data/SKSE/Plugins/GridInventory_vanilla.txt", ec)) {
            static bool s_said = false;
            if (!s_said) {
                s_said = true;
                logger::warn("[{}] GridInventory_vanilla.txt present -- "
                             "the vanilla screens are being left alone", a_tag);
            }
            return true;
        }
        return false;
    }

    bool HandleInventoryMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::InventoryMenu::MENU_NAME) return false;
        if (!a_event.opening && g_planBPendingOpen) {
            g_planBPendingOpen = false;
            FUI::UIRoot::Open();
            logger::info("[INV] InventoryMenu closed -> GridInventoryMenu opening");
            return true;
        }
        if (a_event.opening) {
            if (VanillaPassthrough("INV")) return false;
            // close the vanilla inventory that just opened
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::InventoryMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
                // launched from the TAB (Tween) menu: our instant hide robs
                // it of its normal close-on-select, so it lingers under the
                // grid and is still there after we close
                auto* ui = RE::UI::GetSingleton();
                if (ui && ui->IsMenuOpen(RE::TweenMenu::MENU_NAME)) {
                    mq->AddMessage(RE::TweenMenu::MENU_NAME,
                        RE::UI_MESSAGE_TYPE::kHide, nullptr);
                }
            }
            g_planBPendingOpen = true;
            g_echoMenu = RE::InventoryMenu::MENU_NAME;   // ⑫
            logger::info("[INV] intercepted InventoryMenu -> deferring GridInventoryMenu open");
        }
        return false;   // opening intercept falls through (matches old flow)
    }

    // ★★★⑫ THE CLOSE NOBODY HEARD.
    //
    // Measured, not guessed: TK Dodge's `aaaTKDodgeScript.pex` decompiles to
    //
    //     Event OnMenuOpen(String menuName)
    //         GotoState("Busy")            ; where OnKeyDown is an EMPTY function
    //     EndEvent
    //     Event OnMenuClose(String menuName)
    //         If !Utility.IsInMenuMode()   ; <-- the whole bug lives on this line
    //             GotoState("")
    //         EndIf
    //     EndEvent
    //
    // and the chain it meets on our side is:
    //
    //   1. the vanilla InventoryMenu opens        -> TK goes Busy
    //   2. we kHide it in that same event         -> a close is announced
    //   3. we open OUR menu inside that handler   -> kPausesGame, so menu mode
    //                                                is ON again
    //   4. Papyrus delivers the close (later)     -> IsInMenuMode() is true, so
    //                                                TK never leaves Busy
    //   5. our grid closes                        -> we are not a menu TK
    //                                                registered for: silence
    //
    // Dodge stays dead until some OTHER registered menu closes with nothing
    // open -- which is exactly the report: "press ESC and come back and it
    // works, open the inventory again and it stops."
    //
    // TK's guard is right (do not go idle while a menu is still up). What is
    // wrong is OUR story: we let the engine announce an inventory that opened,
    // then announce it closed a frame later while the inventory is in fact
    // still on screen, and we say nothing at all when it really ends. Every
    // Papyrus mod listening for "the inventory closed" hears it at the wrong
    // moment. This says it at the right one -- once, on the first unpaused
    // frame after our grid is gone, and only for an open the engine itself
    // announced.
    //
    // Mods that act on the early close will now see two; that is strictly
    // better than the one they see being a lie.
    //
    // ★★AND IT IS NOT ONLY THE INVENTORY. ContainerMenu and BarterMenu come
    // through the identical swallow-then-open door, and TK registers for both
    // (checked in its string table). The report named the inventory because
    // that is what the reporter tried; looting a chest breaks dodge exactly the
    // same way. Fixing one of three would have sent the bug straight back.
    //
    // FavoritesMenu is swallowed too and is NOT here -- deliberately. What
    // replaces it is the wheel, which is not kPausesGame (Wheeler.cpp), so menu
    // mode really has ended by the time that close is delivered and the guard
    // passes on its own.
    void MenuCloseEchoTick()
    {
        if (g_echoMenu.empty()) return;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        // This tick only runs unpaused (kPausesGame stops the Update hook), so
        // both tests are belt and braces -- and both are the condition the
        // listener is about to evaluate for itself. Waiting for BOTH is what
        // makes the hop-outs work: the magic key closes our grid and raises
        // MagicMenu in the same breath, and this simply keeps owing until that
        // screen is gone too.
        if (ui->IsMenuOpen("GridInventoryMenu"sv) || ui->GameIsPaused()) return;
        RE::MenuOpenCloseEvent e{};
        e.menuName = g_echoMenu;
        e.opening = false;
        g_echoMenu = {};
        // UI is three event sources at once; name the one we mean
        static_cast<RE::BSTEventSource<RE::MenuOpenCloseEvent>*>(ui)->SendEvent(&e);
        logger::info("[INV] session over -> announcing '{}' close", e.menuName.c_str());
    }

    // ---- lockpick auto-open fallback ----
    // Vanilla re-activates a container automatically after a successful pick;
    // in this load order that never happens (no ContainerMenu event at all —
    // diagnosed 2026-07-24, some mod suppresses it). Fallback: when the
    // lockpicking menu closes with the lock OPEN, re-activate the container
    // ourselves a few frames later — unless something else (vanilla path,
    // QuickLoot's widget, our own grid) claimed the moment first.
    RE::ObjectRefHandle g_pickTarget;        // captured at menu OPEN
    RE::ObjectRefHandle g_pickReopen;        // armed at menu CLOSE (unlocked)
    int                 g_pickReopenDelay = 0;

    bool HandleLockpickAutoReopen(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::LockpickingMenu::MENU_NAME) return false;
        // ★GetTargetReference hands back a smart pointer on the NG line
        if (a_event.opening) {
            const auto tp = RE::LockpickingMenu::GetTargetReference();
            auto* target = tp.get();
            g_pickTarget = target ? target->CreateRefHandle() : RE::ObjectRefHandle{};
            return false;   // observation only
        }
        const auto tp = RE::LockpickingMenu::GetTargetReference();
        auto* target = tp.get();
        if (!target) target = g_pickTarget.get().get();
        g_pickTarget = {};
        if (target && !target->IsLocked() && target->GetBaseObject() &&
            target->GetBaseObject()->Is(RE::FormType::Container)) {
            g_pickReopen = target->CreateRefHandle();
            g_pickReopenDelay = 10;   // ~0.15s head start for the native path
        }
        return false;   // never consumes the event
    }

    // called per unpaused frame from UpdateHook
    void LockpickReopenTick()
    {
        if (g_pickReopenDelay <= 0) return;
        if (--g_pickReopenDelay > 0) return;
        const auto handle = g_pickReopen;
        g_pickReopen = {};
        auto ref = handle.get();
        auto* ui = RE::UI::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!ref || !ui || !player) return;
        // yield when anything already claimed the moment
        if (ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME) ||
            ui->IsMenuOpen("LootMenu") ||                    // QuickLoot widget
            ui->IsMenuOpen("GridInventoryMenu") ||
            ui->GameIsPaused()) {
            return;
        }
        logger::info("[LOOT] lockpick auto-open fallback -> activating container");
        ref->ActivateRef(player, 0, nullptr, 1, false);
    }

    // LOOT: ContainerMenu — every mode is intercepted now: kLoot chest/corpse,
    // kNPCMode companion, kSteal owned containers (F6a) and kPickpocket
    // living targets (F6b, vanilla roll via AttemptPickpocket). Same
    // swallow-then-open pattern.
    bool HandleContainerMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::ContainerMenu::MENU_NAME) return false;
        if (!a_event.opening && g_pendingPartnerOpen) {
            g_pendingPartnerOpen = false;
            FUI::UIRoot::Open();
            logger::info("[LOOT] ContainerMenu closed -> grid opening");
            return true;
        }
        if (a_event.opening) {
            if (VanillaPassthrough("LOOT")) return false;
            auto* ui = RE::UI::GetSingleton();
            auto  menu = ui ? ui->GetMenu<RE::ContainerMenu>() : nullptr;
            if (!menu) {   // unreadable mode: leave the vanilla menu
                logger::warn("[LOOT] ContainerMenu opening but the menu object "
                             "isn't registered yet — not intercepted");
                return false;
            }
            const auto cmode = menu->GetContainerMode();
            FUI::LootBarter::Mode gmode;
            switch (cmode) {
            case RE::ContainerMenu::ContainerMode::kLoot:
            case RE::ContainerMenu::ContainerMode::kNPCMode:
                gmode = FUI::LootBarter::Mode::kLoot;
                break;
            case RE::ContainerMenu::ContainerMode::kSteal:
                gmode = FUI::LootBarter::Mode::kSteal;
                break;
            case RE::ContainerMenu::ContainerMode::kPickpocket:
                gmode = FUI::LootBarter::Mode::kPickpocket;
                break;
            default:
                return false;   // unknown future mode: vanilla
            }
            auto* ref = HandleToRef(RE::ContainerMenu::GetTargetRefHandle());
            FUI::LootBarter::Enter(gmode, ref);
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::ContainerMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            g_pendingPartnerOpen = true;
            g_echoMenu = RE::ContainerMenu::MENU_NAME;   // ⑫
            logger::info("[LOOT] intercepted ContainerMenu (mode {}) -> deferring grid",
                static_cast<int>(cmode));
        }
        return false;
    }

    // BARTER: BarterMenu (merchant).
    bool HandleBarterMenuIntercept(const RE::MenuOpenCloseEvent& a_event)
    {
        if (a_event.menuName != RE::BarterMenu::MENU_NAME) return false;
        if (!a_event.opening && g_pendingPartnerOpen) {
            g_pendingPartnerOpen = false;
            FUI::UIRoot::Open();
            logger::info("[BARTER] BarterMenu closed -> grid opening");
            return true;
        }
        if (a_event.opening) {
            if (VanillaPassthrough("BARTER")) return false;
            // BarterMenu::GetTargetRefHandle returns the PLAYER, not the
            // merchant — the merchant is the dialogue partner.
            RE::TESObjectREFR* ref = nullptr;
            if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
                if (auto sp = mtm->speaker.get()) ref = sp.get();
                // ★the dialogue may have closed a frame before the shop opened;
                // the engine keeps the partner here for exactly that window
                // ("the dialogue menu was closed but the NPC is still talking")
                if (!ref) {
                    if (auto sp = mtm->lastSpeaker.get()) ref = sp.get();
                }
            }
            // ★★★AND IF WE STILL DO NOT KNOW WHO THE MERCHANT IS, WE STAND
            // DOWN. This used to fall back to GetTargetRefHandle -- which the
            // comment above already says is the PLAYER -- so a shop we could
            // not identify was rendered with the player seated as the
            // merchant: your own inventory on both sides of the window.
            //
            // Reported against Faction Camps, and the shape fits: a camp opens
            // its shop from a script when you activate a tent, with no
            // conversation at all, so there is no speaker to find. Vanilla
            // merchants are always reached through dialogue, which is why this
            // never showed up in testing.
            //
            // Standing down is the honest answer -- we cannot draw a shelf for
            // a shop we cannot name. The vanilla barter screen opens instead
            // and the trade works; only our grid is missing.
            if (!ref || ref == RE::PlayerCharacter::GetSingleton()) {
                logger::warn("[BARTER] no merchant identified (a script-opened "
                             "shop?) -> leaving the vanilla menu up");
                return false;
            }
            FUI::LootBarter::Enter(FUI::LootBarter::Mode::kBarter, ref);
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::BarterMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            g_pendingPartnerOpen = true;
            g_echoMenu = RE::BarterMenu::MENU_NAME;   // ⑫
            logger::info("[BARTER] intercepted BarterMenu -> deferring grid");
        }
        return false;
    }



    class InvMenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static InvMenuSink* GetSingleton()
        {
            static InvMenuSink s;
            return &s;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (a_event) {
                // ★★WHILE TRANSFORMED, NAME EVERY MENU. "Q does nothing in
                // beast form" has three different causes that look identical
                // from outside: no menu was raised at all, the favourites menu
                // was raised and shut again, or the game raised something else
                // entirely (a beast form need not map that key the way a body
                // does). Only the engine's own event stream tells them apart,
                // so while the player is a beast we write down every menu that
                // opens or closes -- and the yield line from the wheel sits
                // right above it in the same log, at the same second.
                // Scoped to the transformation so ordinary play stays quiet.
                if (FUI::Wheeler::YieldingToVanilla()) {
                    logger::info("[BEAST] menu '{}' {}", a_event->menuName.c_str(),
                        a_event->opening ? "OPENING" : "closing");
                }
                // one handler per menu concern; true = event fully handled
                HandleLockpickAutoReopen(*a_event);   // observation only
                HandleMessageBoxAside(*a_event) ||
                    HandleTextInputHotkeyBlock(*a_event) ||
                    HandleFavoritesMenuIntercept(*a_event) ||
                    HandleInventoryMenuIntercept(*a_event) ||
                    HandleContainerMenuIntercept(*a_event) ||
                    HandleBarterMenuIntercept(*a_event);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    void Setup()
    {
        // ---- UI bootstrap ----
        FUI::UIRoot::RegisterMenu();
        FUI::Wheeler::RegisterMenu();
        FUI::UIRoot::TryInitD3D();   // renderer is live at kDataLoaded; retried in Open() if not
        // capture orientation = same resolution path as the old pipeline:
        // items ini override -> category preset (PLAN_B §2-G3)
        // D2: IconDef/GridDef are the SAME struct as ItemDef now — the old
        // field-by-field converters collapse to the resolver itself
        FUI::IconCache::GetSingleton()->SetDefResolver(
            [](RE::TESBoundObject* a_obj) -> FUI::IconDef { return DefFor(a_obj); });
        FUI::Grid::SetDefResolver(
            [](RE::TESBoundObject* a_obj) -> FUI::Grid::GridDef { return DefFor(a_obj); });
        FUI::Grid::SetGameCallbacks(
            [](RE::TESBoundObject* a_obj, bool a_up) {   // vanilla per-item sounds (I2)
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->PlayPickUpSound(a_obj, a_up, false);
                }
            },
            [](RE::TESBoundObject* a_obj, int a_count,
               RE::ExtraDataList* a_xl) {   // C5/D1: world drop
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!player) return;
                int owned = 0;
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == a_obj; });
                for (auto& [o2, d2] : inv) owned = d2.first;
                if (owned <= 0) return;
                // a_count > 0 = partial (D1: hover+R drops one); <= 0 = stack
                const int count = a_count > 0 ? (std::min)(a_count, owned) : owned;
                // RemoveItem(kDropping) = the vanilla drop path; DropObject
                // direct is a known CTD from task context
                player->PlayPickUpSound(a_obj, false, false);
                // 1.4/B0: the first round showed drops arriving as req=? simply
                // because nothing had registered them -- which inflates the
                // "external delta" share and makes the echo figure unreadable.
                // ★Slotless by design: this callback never learns the tile key.
                // A slotless confirmation consumes nobody's queued cell
                // (CommitSlotDrop is keyed), and the queue entry the drop DID
                // create expires into the rebuild sweep once its layout entry
                // is pruned.
                FUI::Ledger::Submit(a_obj->GetFormID(), -count, "drop");
                // [vulkaar] le serveur skymp doit APPRENDRE ce jet, sinon sa
                // reapplication d'inventaire le defait — voir SortiesVulkaar.h.
                FUI::SortiesVulkaar::Noter("jeter", a_obj->GetFormID(), count);
                player->RemoveItem(a_obj, count, RE::ITEM_REMOVE_REASON::kDropping,
                    a_xl, nullptr);   // GI25: the named sub-stack
            });
        // ---- B-6: editor hooks (def storage / ini / presets live here) ----
        // D2: FullDef == ItemDef — the toFull/fromFull converters are gone;
        // only the editor's shape-bounds re-derivation survives.
        {
            FUI::Editor::Hooks hooks;
            hooks.getEffective = [](RE::TESBoundObject* o) { return DefFor(o); };
            hooks.getDefault = [](RE::TESBoundObject* o) { return DefaultDef(o); };
            hooks.hasOverride = [](RE::TESBoundObject* o) {
                return g_itemDefs.contains(FormKey(o));
            };
            hooks.setOverride = [](RE::TESBoundObject* o, const FUI::Editor::FullDef& f,
                                   bool a_persist) {
                const std::string key = FormKey(o);
                ItemDef d = f;
                DeriveShapeBounds(d);   // the editor may have repainted the mask
                g_itemDefs[key] = d;   // live: the resolvers see it immediately
                g_modelDefsDirty = true;
                if (a_persist) {
                    UpsertDefLine(key, &d, o->GetName() ? o->GetName() : "");
                }
            };
            hooks.resetOverride = [](RE::TESBoundObject* o) {
                const std::string key = FormKey(o);
                g_itemDefs.erase(key);
                g_modelDefsDirty = true;
                UpsertDefLine(key, nullptr, "");
            };
            hooks.saveAsCategory = [](RE::TESBoundObject* o, const FUI::Editor::FullDef& f) {
                ItemDef d = f;
                DeriveShapeBounds(d);
                d.bag = 0;   // bags are per-item, never a category trait
                d.accept.clear();   // ...and so is what a bag accepts
                g_catDefs[CategoryOf(o)] = d;
                SaveCategoryDefs();
            };
            hooks.categoryName = [](RE::TESBoundObject* o) { return std::string(CategoryOf(o)); };
            FUI::Editor::SetHooks(std::move(hooks));
        }

        // Per-item drawn icons are named after this exact string. Handing the
        // function over instead of letting Fallback spell it again keeps ONE
        // definition of "which item is this line about" — the item ini and the
        // PNG file name can then never disagree.
        FUI::Fallback::SetFormKeyResolver([](RE::TESForm* f) { return FormKey(f); });

        // GI47: the settings-window preset is the ONE share file -- it carries
        // the item/category defs alongside the style keys. The def storage
        // lives here, so WinManager takes it through hooks.
        FUI::WinManager::GetSingleton()->SetPresetDefsHooks(
            [](std::ostream& o) {
                o << "[categories]\n";
                const std::map<std::string, ItemDef> sc(g_catDefs.begin(), g_catDefs.end());
                for (const auto& [name, d] : sc) o << FormatItemDef(name, d) << "\n";
                // GI47: the WHOLE universe, not just the overrides. An item the
                // author left at default is a CHOICE ("the default look is
                // right"), and on import it must beat the reader's local tweak
                // of that same item -- so every playable item's EFFECTIVE def
                // travels. Items from mods only the reader has never appear
                // here, so their tweaks survive the import untouched.
                o << "[items]\n";
                std::map<std::string, ItemDef> si;
                auto sweepDefs = [&](const auto& a_arr) {
                    for (auto* form : a_arr) {
                        auto* obj = form ? form->template As<RE::TESBoundObject>() : nullptr;
                        if (!obj || !obj->GetPlayable()) continue;
                        const char* nm = obj->GetName();
                        if (!nm || !nm[0]) continue;
                        if (!obj->GetFile(0)) continue;   // runtime form: no stable key
                        si[FormKey(obj)] = FUI::Grid::ResolveDef(obj);
                    }
                };
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    sweepDefs(dh->GetFormArray<RE::TESObjectWEAP>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectARMO>());
                    sweepDefs(dh->GetFormArray<RE::TESAmmo>());
                    sweepDefs(dh->GetFormArray<RE::AlchemyItem>());
                    sweepDefs(dh->GetFormArray<RE::IngredientItem>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectBOOK>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectMISC>());
                    sweepDefs(dh->GetFormArray<RE::TESSoulGem>());
                    sweepDefs(dh->GetFormArray<RE::TESKey>());
                    sweepDefs(dh->GetFormArray<RE::ScrollItem>());
                    sweepDefs(dh->GetFormArray<RE::TESObjectLIGH>());
                }
                for (const auto& [key, d] : si) o << FormatItemDef(key, d) << "\n";
                // GI60: drawn-icon transforms travel too — they are part of
                // how the author's inventory LOOKS, which is what a preset is.
                o << "[flat]\n";
                char fb[128];
                for (const auto& [key, x] : FUI::Fallback::Xforms()) {
                    std::snprintf(fb, sizeof(fb), "%s = fscale:%.2f, frot:%.0f, fx:%.2f",
                        key.c_str(), x.scale, x.rot, x.x);
                    o << fb << "\n";
                }
            },
            [](int a_section, const std::string& a_key, const std::string& a_val) {
                if (a_section == 4) {   // [flat]
                    const ItemDef d = ParseItemDef(a_val, ItemDef{});
                    FUI::Fallback::SetXform(a_key, { d.fscale, d.frot, d.fx });
                } else if (a_section == 1) {
                    if (const auto it = g_catDefs.find(a_key); it != g_catDefs.end()) {
                        it->second = ParseItemDef(a_val, it->second);
                    }
                } else {
                    // GI47: a preset line is that item's WHOLE def -- parsed
                    // over the factory default, never over the reader's tweak,
                    // so every shared-universe item becomes exactly the
                    // author's (untouched-by-author included).
                    g_itemDefs[a_key] = ParseItemDef(a_val, ItemDef{});
                }
            },
            []() {
                SaveCategoryDefs();
                RewriteItemDefsFile();
                SaveFlatIconDefs();
                g_modelDefsDirty = true;
                FUI::Grid::RequestRebuild();
                FUI::Grid::MarkCapacityDirty();
            });

        // Load the def inis NOW (kDataLoaded) — not only on first menu open.
        // The post-load capacity compute runs BEFORE any menu: with g_itemDefs
        // empty it lost every user override (bag flags above all), counted bag
        // CONTENTS as main-board occupants and reported a false overload
        // ("slow until the inventory is opened", log-verified).
        // ★GI71: BEFORE any settings read. "!lang" is stored as an id, so the
        // pack list has to exist for that id to resolve to anything — load them
        // afterwards and a user on a pack silently reverts to English once,
        // then saves that revert back over their choice.
        FUI::Lang::LoadPacks();
        // ★Typed bags: BEFORE LoadItemDefs. That loader validates every bag's
        // accept token against this list, so an empty list would report each
        // typed bag as naming an unknown filter — the loudest possible version
        // of the exact false alarm the check exists to prevent. Keyword lookup
        // needs the game data, which kDataLoaded guarantees.
        FUI::BagFilter::SetCategoryResolver(
            [](RE::TESBoundObject* o) { return std::string(CategoryOf(o)); });
        FUI::BagFilter::Load();

        LoadCategoryDefs();
        LoadItemDefs();
        LoadUniqueDefs();
        LoadFlatIconDefs();

        // ★★★AND THE UI INI, HERE -- not when a window first asks for its
        // place. WinManager loads it lazily from ApplyNext, so until the
        // INVENTORY had been opened once nothing in that file was in effect;
        // and the wheel does not use ApplyNext at all, because it is a
        // full-screen overlay with no managed window.
        //
        // ★★That made the quick wheel come up in the wrong skin AND with
        // every icon a category drawing, on a machine whose pak was complete:
        // `!caplight` is part of every cache KEY, so a wheel drawn before the
        // ini was read hashed its lookups against the default lamp (0,0) and
        // missed a pak captured at the player's own angle -- ALL of it, every
        // time, until a bag was opened. Which is why the file's own comment
        // ("loaded BEFORE any icon is asked for") read as true and was not:
        // it describes the order INSIDE Load, and Load itself came late.
        //
        // ★A settings file is read once, at load, before anything can ask a
        // question it answers. Wheeler::LoadSettings already had to reach
        // past this for `!wheelon` alone (see its comment); that is the same
        // bug reported once and fixed one key at a time.
        FUI::WinManager::GetSingleton()->Load();

        FUI::UIRoot::SetVisibilityCallbacks(
            []() {   // menu shown
                LoadCategoryDefs();   // hot-reload category defaults (H7)
                LoadItemDefs();       // hot-reload user overrides (same as legacy path)
                LoadUniqueDefs();     // ...and the unique declarations beside them
                LoadFlatIconDefs();   // hot-reload IconStudio's drawn-icon edits
                // typed bags phase 0: classify what the player is carrying and
                // write the tally out. ONCE per session — this is an
                // observation, not a feature, and it must not cost anything on
                // every open. Nothing is routed or moved.
                static bool s_bagDumped = false;
                if (!s_bagDumped) {
                    s_bagDumped = true;
                    FUI::BagFilter::DumpFormDatabase();
                    FUI::BagFilter::DumpPlayerInventory();
                }
                // NOTE (A3, 2026-07-13): the attack handler is NOT disabled any
                // more. kPausesGame + the kInventory menu context already keep
                // clicks from the gameplay layer, and toggling
                // inputEventHandlingEnabled mid-hold corrupted the held-input
                // bookkeeping — closing after an in-menu right-click fired a
                // POWER ATTACK (the legacy PrismaUI-era block was for a
                // context-less overlay and no longer applies).
                //
                // [vulkaar] A3 disait vrai À DEUX CONDITIONS — et kPausesGame
                // est tombée le 25/08/2026 (menu sans pause) : R (dégainer) et
                // le clic droit atteignaient la couche gameplay pendant que la
                // grille est ouverte. On suspend les CONTRÔLES DE COMBAT par la
                // voie haute (ControlMap::ToggleControls), jamais par
                // inputEventHandlingEnabled — A3 a mesuré ce que ce bricolage
                // coûte. Les touches du menu (R = jeter un, clic droit =
                // inspection) ne passent pas par ces handlers : elles
                // continuent de marcher.
                // [vulkaar] 03/09/2026 : pas seulement le combat. Le menu ne met
                // pas le jeu en pause et son contexte kInventory ne lie pas E,
                // Espace, Ctrl, F : ils retombaient en gameplay -- une porte
                // s'ouvrait, le personnage sautait, pendant qu'on gerait le
                // registre d'une maison. Meme voie haute, memes drapeaux rendus a
                // la fermeture. JAMAIS kMenu ni kConsole : Echap et la console
                // doivent toujours repondre.
                // [vulkaar] LA SUSPENSION DES CONTROLES N'EST PLUS ICI : une
                // bascule a deux bords (ouverture / fermeture) FUIT. Le
                // 03/09/2026 elle a laisse l'activation coupee — plus moyen
                // d'ouvrir une porte, meme menu ferme. Elle se reconcilie
                // desormais A CHAQUE TRAME dans le crochet d'Update, comme le
                // blocage du deplacement juste a cote.
            },
            []() {   // menu hidden
                // [vulkaar] une capture pendante meurt avec le menu : jamais
                // d'impulsion de pause tenue sans pompe pour la relâcher.
                FUI::IconCache::GetSingleton()->RelacherImpulsionPause();
                // [vulkaar] (les controles se rendent d'eux-memes a la trame
                // suivante — voir la reconciliation dans le crochet d'Update.)
            });

        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputSink::GetSingleton());
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(InvMenuSink::GetSingleton());
        }
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            // capacity: container-take bounce (menu-scoped, see ContainerSink)
            holder->AddEventSink<RE::TESContainerChangedEvent>(ContainerSink::GetSingleton());
            holder->AddEventSink<RE::TESResetEvent>(ResetSink::GetSingleton());
            // W2: worn state changes the board occupancy
            holder->AddEventSink<RE::TESEquipEvent>(EquipSink::GetSingleton());
        }
        logger::info("[SETUP] ready (ImGui inventory)");
    }

    // Player 3D is rebuilt across save load / new game: every cached node pointer
    // becomes stale (rule 4-3 #2). Drop them all and hide the UI if it was open.
    void ResetSession()
    {
        // restore ONLY if we disabled it (never touch input state during load otherwise)
        if (g_movementOff) {
            SetMoveInput(true);
            g_movementOff = false;
        }
        // [vulkaar] meme dette : des controles coupes ne survivent pas a un
        // chargement. La reconciliation par trame les recouperait aussitot si
        // notre menu etait encore ouvert.
        if (g_controlesCoupes) {
            if (auto* cm = RE::ControlMap::GetSingleton()) {
                cm->ToggleControls(kControlesSuspendusParLaGrille, true, true);
            }
            g_controlesCoupes = false;
        }
        g_planBPendingOpen = false;
        g_reopenAfterMsg = false;
        // ★★★A DEBT OWED TO A SAVE THAT IS GONE. g_echoMenu names a vanilla
        // menu whose close we still have to announce; left set across a load,
        // MenuCloseEchoTick fires it on the FIRST unpaused frame of the new
        // session, and every Papyrus mod listening for that menu (TK Dodge and
        // friends) is told the previous game's container just closed.
        g_echoMenu = {};
        g_pendingPartnerOpen = false;
        // ★Same shape, shorter fuse: a lockpick handle armed at close will
        // ActivateRef ten frames into whatever game is loaded next.
        g_pickReopen = {};
        g_pickReopenDelay = 0;
        // NOTE: Loadout reset moved to the serialization REVERT callback (L3) —
        // kPostLoadGame arrives AFTER the cosave load and would wipe the tabs.
        FUI::UIRoot::Close();   // hide across load/new game
    }

    // Registered with sender == "SKSE", so every `type` here really is a
    // lifecycle value. GI10's ABI messages arrive on a SEPARATE listener
    // (HostApi::Install) precisely so that stays true.
    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            FUI::HostApi::Broadcast();   // GI10: announce the host to providers
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            Setup();
            FUI::GoldCoins::InitForms();   // G1: resolve Grid Inventory.esp
            FUI::MonnaiesVulkaar::InitForms();   // vulkaar : Septime / Mede / Titus
            FUI::SortiesVulkaar::Initialiser();  // vulkaar : journal jets/destructions
            FUI::Echange::Initialiser();         // vulkaar : pont de la fenetre d echange
            FUI::Etabli::Initialiser();        // vulkaar : pont de l ecran d etabli
            FUI::Appartenance::Initialiser();        // vulkaar : pont du panneau de la maison
            RecolteInitialiser();              // vulkaar : la liste de recolte est de CETTE session
            PortesInitialiser();               // vulkaar : les noms de portes sont de CETTE session
            // FUI::EssaiSwf::Initialiser();  // vulkaar : ecran Scaleform d'essai —
            //   inscrit plus rien depuis le 29/08 (voir le retrait de F9 ci-dessus).
            //   Le rallumer suffit a retrouver un menu Scaleform qui marche.
            // ★B3-a: close the loop the ledger opened. Registered once, here,
            // where the forms are already resolved.
            // ★A confirmation commits ITS OWN cell and no other: the slot key
            // rides the request (Ledger.h), so a slotless drop or use can
            // never pop a pending store's key -- the count-based version did
            // exactly that whenever two paths moved the same form.
            FUI::Ledger::SetOnExpire([](const FUI::Ledger::Expired& a_e) {
                FUI::Grid::OnRequestExpired(a_e.form, a_e.delta, a_e.who, a_e.slot);
            });
            FUI::Ledger::SetOnConfirm([](const FUI::Ledger::Expired& a_e) {
                if (a_e.delta < 0) FUI::Grid::CommitSlotDrop(a_e.form, a_e.slot);
                // ★A confirmed consume releases its suppression entry NOW --
                // see ReleaseAppliedPendingEquip. Without this the entry
                // overlapped the dropped engine count for one rebuild and the
                // stack was subtracted twice ("one drink removed two", and the
                // last unit lost its cell to the front gap).
                if (a_e.delta < 0 && a_e.who && std::strcmp(a_e.who, "use") == 0) {
                    FUI::Grid::ReleaseAppliedPendingEquip(a_e.form);
                }
            });
            break;
        case SKSE::MessagingInterface::kNewGame:
            ResetSession();
            FUI::DeltaWatch::Reset("new game");
            FUI::Census::Reset("new game");
            FUI::Ledger::Reset("new game");
            SKSE::GetTaskInterface()->AddTask([]() {
                FUI::WornLedger::Rebaseline("new game");
            });
            // no cosave load callback fires on new game — start with an empty
            // grid layout instead of migrating the legacy ini (old saves only)
            FUI::Grid::MarkLayoutFresh();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            ResetSession();
            // ★Before, not after: the engine swaps the inventory during the
            // load and any event that crosses it belongs to neither side.
            FUI::DeltaWatch::Reset("load");
            FUI::Census::Reset("load");
            FUI::Ledger::Reset("load");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            ResetSession();
            // B4-2: a load is a discontinuity (rule 3) -- the worn ledger
            // rebuilds from the engine once, here, where the new inventory is
            // real. Deferred a task so the walk runs after the engine settles.
            SKSE::GetTaskInterface()->AddTask([]() {
                FUI::WornLedger::Rebaseline("load");
            });
            // ★The costume has to be put on again -- more than once. See
            // Costume::NoteGameLoaded: the engine rebuilds the actor for a
            // while after this message, and every rebuild undoes it.
            FUI::Costume::NoteGameLoaded();
            // ★The equip survived the save; the LOAN did not -- the engine
            // re-read the carrier from the plugin. Re-lend before the player
            // can notice a second ring that stopped working.
            FUI::DualRing::OnLoad();
            break;
        }
    }

    // ---- SKSE cosave: one record loop, dispatched by type ----
    void SaveCallback(SKSE::SerializationInterface* a_intfc)
    {
        FUI::Loadout::SaveGame(a_intfc);
        FUI::Grid::SaveGame(a_intfc);
        FUI::GoldCoins::SaveGame(a_intfc);
        FUI::Costume::SaveGame(a_intfc);
        FUI::DualRing::SaveGame(a_intfc);
        FUI::LootBarter::SaveGame(a_intfc);   // F7: container spot memory (GCLY)
        FUI::Wheeler::SaveGame(a_intfc);      // quick-wheel slot order (GWHL)
    }

    void LoadCallback(SKSE::SerializationInterface* a_intfc)
    {
        std::uint32_t type = 0, version = 0, length = 0;
        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type == FUI::Loadout::kRecordType) {
                FUI::Loadout::LoadRecord(a_intfc, version);
            } else if (type == FUI::Grid::kRecordType) {
                FUI::Grid::LoadRecord(a_intfc, version);
            } else if (type == FUI::GoldCoins::kRecordType) {
                FUI::GoldCoins::LoadRecord(a_intfc, version);
            } else if (type == FUI::Costume::kRecordType) {
                FUI::Costume::LoadRecord(a_intfc, version);
            } else if (type == FUI::DualRing::kRecordType) {
                FUI::DualRing::LoadRecord(a_intfc, version);
            } else if (type == FUI::Wheeler::kRecordType) {
                FUI::Wheeler::LoadRecord(a_intfc, version);
            } else if (type == FUI::LootBarter::kContRecordType) {
                FUI::LootBarter::LoadRecord(a_intfc, version);   // F7 (GCLY)
            } else {
                // P2: unknown records are skipped by SKSE automatically, but
                // silently — log them so a future-type/corruption case is
                // diagnosable instead of invisible
                logger::warn("[COSAVE] unknown record type {:08X} v{} ({} bytes) skipped",
                    type, version, length);
            }
        }
        FUI::Grid::RequestRebuild();   // reserved gear + placements just changed
    }

    void RevertCallback(SKSE::SerializationInterface* a_intfc)
    {
        FUI::Loadout::RevertGame(a_intfc);
        FUI::Costume::RevertGame(a_intfc);
        FUI::DualRing::RevertGame(a_intfc);
        FUI::Wheeler::RevertGame(a_intfc);
        FUI::Grid::RevertGame(a_intfc);
        FUI::GoldCoins::RevertGame(a_intfc);
        FUI::LootBarter::RevertGame();   // F7: container spot memory
        FUI::IconCache::GetSingleton()->OnRevert();   // drop work queued
                                                      // against dying forms
    }
}

SKSEPluginInfo(
    .Version              = { 1, 4, 4, 0 },
    .Name                 = "GridInventory",
    .Author               = "Smooth",
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::Init(a_skse);
    // ★Say so in the log itself. A diagnostic build is otherwise
    // indistinguishable from the release one, and a report is worth much less
    // when nobody can tell which binary produced it.
    if (FUI::Grid::PoolTrace()) {
        SKSE::log::info("=== DIAGNOSTIC BUILD: pool/take tracing is ON by default ===");
    }

    // no trampoline: every hook here is a vtable swap (write_vfunc)
    UpdateHook::Install();
    PickUpHook::Install();                                        // capacity: world pickup
    HarvestHook<RE::TESFlora>::Install(RE::VTABLE_TESFlora[0]);   // capacity: plants
    HarvestHook<RE::TESObjectTREE>::Install(RE::VTABLE_TESObjectTREE[0]);   // capacity: trees
    PorteTexteHook::Install();   // [vulkaar] le nom affiche au reticule d une porte de maison
    SackActivateHook::Install();   // G2: coin sack -> gold, silent to loot HUDs
    // capacity gate at the Activate slot for every direct-pickup form type —
    // pre-TrueHUD, so a blocked pickup can't log a phantom "received"
    // (MISC is covered inside SackActivateHook; books keep the menu flow)
    CapacityActivateHook<RE::TESObjectWEAP>::Install(RE::VTABLE_TESObjectWEAP[0]);
    CapacityActivateHook<RE::TESObjectARMO>::Install(RE::VTABLE_TESObjectARMO[0]);
    CapacityActivateHook<RE::TESAmmo>::Install(RE::VTABLE_TESAmmo[0]);
    CapacityActivateHook<RE::AlchemyItem>::Install(RE::VTABLE_AlchemyItem[0]);
    CapacityActivateHook<RE::IngredientItem>::Install(RE::VTABLE_IngredientItem[0]);
    CapacityActivateHook<RE::TESSoulGem>::Install(RE::VTABLE_TESSoulGem[0]);
    CapacityActivateHook<RE::TESKey>::Install(RE::VTABLE_TESKey[0]);
    CapacityActivateHook<RE::ScrollItem>::Install(RE::VTABLE_ScrollItem[0]);
    CapacityActivateHook<RE::TESObjectLIGH>::Install(RE::VTABLE_TESObjectLIGH[0]);

    // Lifecycle: sender == "SKSE" (the 1.0 path, unchanged). GI10's ABI messages
    // come in on a separate listener so a stray "type 4" from an unrelated
    // plugin can never be mistaken for kPostLoadGame here.
    SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
    FUI::HostApi::Install();

    // L3: loadout tabs + grid layout persist in the SKSE cosave (per-save,
    // load-order safe). The global layout ini stays as a legacy fallback only.
    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID('FBIV');
        ser->SetSaveCallback(SaveCallback);
        ser->SetLoadCallback(LoadCallback);
        ser->SetRevertCallback(RevertCallback);
    }
    return true;
}
