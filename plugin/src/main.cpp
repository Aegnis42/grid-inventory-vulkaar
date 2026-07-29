#include "game/GoldCoins.h"
#include "ui/Editor.h"
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
            if (a_event && a_event->actor && a_event->actor->IsPlayerRef()) {
                FUI::Grid::MarkCapacityDirty();
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
            // W2: any change touching the player's inventory can flip the
            // capacity state (shop buys, scripted AddItem, drops, sells)
            if (a_event->newContainer == 0x14 || a_event->oldContainer == 0x14) {
                FUI::Grid::MarkCapacityDirty();
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
                auto* btn = e->AsButtonEvent();
                if (!btn || !btn->IsDown()) {
                    continue;
                }
                if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
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
        const char* p = nullptr;
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            // armor is NOT a TESModel — its GND model lives on the biped form
            p = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
        } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
            p = mdl->GetModel();
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
        put("armor_feet",      2, 2, 0, 0, 165, 1.0f);
        put("armor_head",      2, 2, -90, 0, 0, 1.0f);
        put("book",            1, 2, -90, 0, 0, 1.0f);
        put("book_skill",      1, 2, -90, 0, 0, 1.0f);     // <- book
        put("book_spell",      1, 2, -90, 0, 0, 1.0f);     // <- book
        put("scroll",          2, 1, -90, 0, 0, 1.0f);
        put("potion",          1, 1, 0, 0, -90, 1.0f);
        put("poison",          1, 1, 0, 0, -90, 1.0f);     // <- potion
        put("food",            1, 1, 0, 0, -90, 1.0f);     // <- potion
        put("ingredient",      1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("soulgem",         1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("key",             1, 1, 0, 0, 90, 9.5f);
        put("misc_ore",        1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_gem",        1, 1, -90, 0, 0, 1.0f);     // <- misc
        put("misc_hide",       1, 1, -90, 0, 0, 1.0f);     // <- misc
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
            if (armo->HasPartOf(S::kRing))    return "armor_ring";
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
            if (armo->HasPartOf(S::kHands))  return "armor_hands";
            if (armo->HasPartOf(S::kFeet))   return "armor_feet";
            if (armo->HasPartOf(S::kHead) || armo->HasPartOf(S::kHair) ||
                armo->HasPartOf(S::kCirclet)) {
                return "armor_head";
            }
            // B10: custom biped slots (capes 46, backpacks, accessories...)
            // used to be swallowed by the armor_head fallback (2x2 helmet
            // defaults) — a body-like default fits them far better
            return "armor_cloth";
        }
        if (auto* book = a_obj->As<RE::TESObjectBOOK>()) {
            if (book->TeachesSpell()) return "book_spell";
            if (book->TeachesSkill()) return "book_skill";
            return "book";
        }
        if (a_obj->Is(RE::FormType::Scroll)) return "scroll";
        if (auto* ammo = a_obj->As<RE::TESAmmo>()) {
            return ammo->IsBolt() ? "ammo_bolt" : "ammo_arrow";
        }
        if (auto* alch = a_obj->As<RE::AlchemyItem>()) {
            if (alch->IsPoison()) return "poison";
            if (alch->IsFood())   return "food";
            return "potion";
        }
        if (a_obj->Is(RE::FormType::Ingredient)) return "ingredient";
        if (a_obj->Is(RE::FormType::SoulGem))    return "soulgem";
        if (a_obj->Is(RE::FormType::KeyMaster))  return "key";
        if (auto* misc = a_obj->As<RE::TESObjectMISC>()) {
            if (misc->HasKeywordString("VendorItemOreIngot"))   return "misc_ore";
            if (misc->HasKeywordString("VendorItemGem"))        return "misc_gem";
            if (misc->HasKeywordString("VendorItemAnimalHide")) return "misc_hide";
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
            if (a_def) { *it = FormatItemDef(a_key, *a_def); }
            else       { lines.erase(it); }
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

    // User override file, one line per item (hot-reloaded on every inventory open):
    //   Skyrim.esm|0x0001397E = w:1, h:4, rx:-90, ry:0, rz:0, scale:1.0
    void LoadItemDefs()
    {
        g_itemDefs.clear();
        std::ifstream in(kDefsPath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            // shared metatable parser (ui/ItemDef.h) over factory defaults
            g_itemDefs[key] = ParseItemDef(line.substr(eq + 1), ItemDef{});
        }
        g_modelDefsDirty = true;
        logger::info("[DEFS] {} item overrides loaded", g_itemDefs.size());
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
                { "armor_head",     { "armor_circlet" } },
                { "book",           { "book_skill", "book_spell" } },
                { "potion",         { "poison", "food" } },
                { "misc",           { "ingredient", "soulgem", "misc_ore", "misc_gem", "misc_hide" } },
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
                g_catDefs[CategoryOf(o)] = d;
                SaveCategoryDefs();
            };
            hooks.categoryName = [](RE::TESBoundObject* o) { return std::string(CategoryOf(o)); };
            FUI::Editor::SetHooks(std::move(hooks));
        }

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
            },
            [](int a_section, const std::string& a_key, const std::string& a_val) {
                if (a_section == 1) {
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
                g_modelDefsDirty = true;
                FUI::Grid::RequestRebuild();
                FUI::Grid::MarkCapacityDirty();
            });

        // Load the def inis NOW (kDataLoaded) — not only on first menu open.
        // The post-load capacity compute runs BEFORE any menu: with g_itemDefs
        // empty it lost every user override (bag flags above all), counted bag
        // CONTENTS as main-board occupants and reported a false overload
        // ("slow until the inventory is opened", log-verified).
        LoadCategoryDefs();
        LoadItemDefs();

        FUI::UIRoot::SetVisibilityCallbacks(
            []() {   // menu shown
                LoadCategoryDefs();   // hot-reload category defaults (H7)
                LoadItemDefs();       // hot-reload user overrides (same as legacy path)
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

    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            Setup();
            FUI::GoldCoins::InitForms();   // G1: resolve Grid Inventory.esp
            break;
        case SKSE::MessagingInterface::kNewGame:
            ResetSession();
            // no cosave load callback fires on new game — start with an empty
            // grid layout instead of migrating the legacy ini (old saves only)
            FUI::Grid::MarkLayoutFresh();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            ResetSession();
            break;
        }
    }

    // ---- SKSE cosave: one record loop, dispatched by type ----
    void SaveCallback(SKSE::SerializationInterface* a_intfc)
    {
        FUI::Loadout::SaveGame(a_intfc);
        FUI::Grid::SaveGame(a_intfc);
        FUI::GoldCoins::SaveGame(a_intfc);
        FUI::LootBarter::SaveGame(a_intfc);   // F7: container spot memory (GCLY)
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
        FUI::Grid::RevertGame(a_intfc);
        FUI::GoldCoins::RevertGame(a_intfc);
        FUI::LootBarter::RevertGame();   // F7: container spot memory
        FUI::IconCache::GetSingleton()->OnRevert();   // drop work queued
                                                      // against dying forms
    }
}

SKSEPluginInfo(
    .Version              = { 1, 0, 0, 0 },
    .Name                 = "GridInventory",
    .Author               = "Fablerim",
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    InitializeLog();
    SKSE::Init(a_skse);

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

    SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);

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
