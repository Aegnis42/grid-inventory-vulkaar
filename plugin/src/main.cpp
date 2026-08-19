#include "api/HostApi.h"
#include "game/BagFilter.h"
#include "game/Census.h"
#include "game/Costume.h"
#include "game/DeltaWatch.h"
#include "game/Ledger.h"
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
#include <string>
#include <unordered_map>
#include <vector>

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
    bool g_pendingPartnerOpen = false; // open our grid once Container/BarterMenu fully closed (loot/barter)
    bool g_movementOff = false;        // we disabled the movement handler (text input)
    bool g_reopenAfterMsg = false;     // we stepped aside for a MessageBox (poison confirm)

    // raw RefHandle -> reference (ContainerMenu/BarterMenu return a raw handle)
    RE::TESObjectREFR* HandleToRef(RE::RefHandle a_handle)
    {
        if (a_handle == 0) return nullptr;
        RE::NiPointer<RE::TESObjectREFR> ptr;
        using func_t = bool(RE::RefHandle&, RE::NiPointer<RE::TESObjectREFR>&);
        static REL::Relocation<func_t> func{ RE::Offset::LookupReferenceByHandle };
        func(a_handle, ptr);
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
    void LockpickReopenTick();   // defined below (lockpick auto-open fallback)

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
            // apply capture defs + park the preview model BEFORE this frame
            // renders. While the menu is open (game paused) GridInventoryMenu::
            // AdvanceMovie drives Tick - this path covers unpaused frames.
            FUI::UIRoot::Tick();
            // ★Every tick, open or not: the CLOSE animation has to keep running
            // after the hotkey is already released, and it is what finally takes
            // the overlay menu down.
            FUI::Wheeler::Tick();
            LockpickReopenTick();   // lockpick auto-open fallback
        }
        static inline REL::Relocation<decltype(thunk)> func;

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
            func = vtbl.write_vfunc(0xAD, thunk);
        }
    };

    // ---- Capacity system (Mabinogi rule): no free cells -> no pickup ----
    // PlayerCharacter::PickUpObject vtable hook (index 0xCC): the manual
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
            func = vtbl.write_vfunc(0xCC, thunk);
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
            // ★1.4/B0: this sink is the one that genuinely throws its delta
            // away -- it reads IsPlayerRef() and nothing else. Before it can be
            // extended (PLAN §2 row 2) we need to see whether baseObject +
            // uniqueID + equipped actually name the unit that moved.
            FUI::DeltaWatch::OnEquip(a_event);
            if (a_event && a_event->actor && a_event->actor->IsPlayerRef()) {
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
            if (a_activatorRef && a_activatorRef->IsPlayerRef() &&
                a_this->produceItem && !FUI::Grid::CanFitNewItem(a_this->produceItem)) {
                NotifyInventoryFull();
                return false;   // blocked: the plant stays harvestable
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
            // ★1.4/B0 FIRST, before any consumer below reacts -- the whole point
            // is to see the delta as it ARRIVES, in the order it arrived, and
            // to know where the existing consumers sit relative to it. A sink
            // of our own would be delivered in an order we do not control.
            // Observation only; returns immediately unless "!delta = 1".
            FUI::DeltaWatch::OnContainer(a_event);
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
                // ★Only while our menu is up: closed, the next open rebuilds
                // from scratch anyway.
                if (auto* ui = RE::UI::GetSingleton();
                    ui && ui->IsMenuOpen("GridInventoryMenu"sv)) {
                    FUI::Grid::RequestRebuild();
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
                    if (a_event->oldContainer == 0x14) {
                        FUI::GoldCoins::OnPouchLeftPlayer();
                    } else if (a_event->newContainer == 0x14) {
                        FUI::GoldCoins::OnPouchReturned();
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
                // The game's Inventory key closes our menu. This sink sits
                // UPSTREAM of input-context filtering, so it still sees the
                // raw key while kMenuMode swallows the user event.
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
        // ★★★THE SWITCH LIVES HERE, not only on the wheel's own input. This is
        // the half that gives the vanilla screen back: with the wheel off it
        // opens exactly as it always did, hotkey binding and all. Gating only
        // the wheel would have left the menu suppressed with nothing put in
        // its place -- the favourites key would simply have stopped working.
        if (!FUI::Wheeler::Enabled()) return false;
        if (!a_event.opening) return false;
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(RE::FavoritesMenu::MENU_NAME,
                RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
        return true;
    }

    // Vanilla InventoryMenu: swallow-then-open (our grid opens once the
    // vanilla menu finished closing).
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
            logger::info("[INV] intercepted InventoryMenu -> deferring GridInventoryMenu open");
        }
        return false;   // opening intercept falls through (matches old flow)
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
        if (a_event.opening) {
            auto* target = RE::LockpickingMenu::GetTargetReference();
            g_pickTarget = target ? target->CreateRefHandle() : RE::ObjectRefHandle{};
            return false;   // observation only
        }
        auto* target = RE::LockpickingMenu::GetTargetReference();
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
            // BarterMenu::GetTargetRefHandle returns the PLAYER, not the
            // merchant — the merchant is the dialogue partner. Use the
            // MenuTopicManager speaker (falls back to the handle if null).
            RE::TESObjectREFR* ref = nullptr;
            if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
                if (auto sp = mtm->speaker.get()) ref = sp.get();
            }
            if (!ref) ref = HandleToRef(RE::BarterMenu::GetTargetRefHandle());
            FUI::LootBarter::Enter(FUI::LootBarter::Mode::kBarter, ref);
            if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
                mq->AddMessage(RE::BarterMenu::MENU_NAME,
                    RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
            g_pendingPartnerOpen = true;
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
                FUI::Ledger::Submit(a_obj->GetFormID(), -count, "drop");
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
            },
            []() {   // menu hidden
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
        g_planBPendingOpen = false;
        g_reopenAfterMsg = false;
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
            // ★B3-a: close the loop the ledger opened. Registered once, here,
            // where the forms are already resolved.
            FUI::Ledger::SetOnExpire([](const FUI::Ledger::Expired& a_e) {
                FUI::Grid::OnRequestExpired(a_e.form, a_e.delta, a_e.who);
            });
            FUI::Ledger::SetOnConfirm([](const FUI::Ledger::Expired& a_e) {
                if (a_e.delta < 0) FUI::Grid::CommitSlotDrop(a_e.form, -a_e.delta);
            });
            break;
        case SKSE::MessagingInterface::kNewGame:
            ResetSession();
            FUI::DeltaWatch::Reset("new game");
            FUI::Census::Reset("new game");
            FUI::Ledger::Reset("new game");
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
    .Version              = { 1, 3, 1, 0 },
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
