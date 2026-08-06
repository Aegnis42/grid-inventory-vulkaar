#pragma once

// Inventory3DManager member functions absent from CommonLibSSE-NG (main).
// Address Library IDs verified against alandtse/CommonLibVR (ng branch) and
// already proven in-game by the preview-queue path in main.cpp.
// Render() itself exists natively in NG (RE/I/Inventory3DManager.h).
namespace FUI::Inv3D
{
    inline void Begin3D(RE::Inventory3DManager* a_mgr, RE::INTERFACE_LIGHT_SCHEME a_scheme)
    {
        using func_t = void (*)(RE::Inventory3DManager*, RE::INTERFACE_LIGHT_SCHEME);
        static REL::Relocation<func_t> func{ RELOCATION_ID(50881, 51754) };
        func(a_mgr, a_scheme);
    }

    inline void End3D(RE::Inventory3DManager* a_mgr)
    {
        using func_t = void (*)(RE::Inventory3DManager*);
        static REL::Relocation<func_t> func{ RELOCATION_ID(50883, 51756) };
        func(a_mgr);
    }

    // ★★NOTE THE MISMATCH, and do not "fix" it without reading this. NG maps
    // this very ID to Inventory3DManager::UpdateMagic3D(TESForm*, uint32_t),
    // while this wrapper (inherited from the Modex port) names the second
    // parameter ExtraDataList* and passes null. It loads and renders items
    // correctly, so one of the two names is wrong.
    //
    // ★★1.0.5 measured the alternative: RELOCATION_ID(50884, 51757), which NG
    // calls UpdateItem3D(InventoryEntryData*), IS the item loader and DOES
    // attach an enchantment's effect pass — the shader property appears in the
    // model tree with its texture and GPU data ready. It still buys nothing:
    //   - the FIRST capture of a freshly loaded model renders without the pass
    //     (measured byte-identical to this path: rgb 59,54,46, 0.0% tint)
    //   - a second capture reaches only ~11% of the tint a long-lived model
    //     shows, and the value keeps climbing for SECONDS afterwards
    // EDIT appeared to work only because a rotation drag re-shoots one model
    // continuously. Paying for it means seconds per item across a 1800-item
    // precache, for something the rarity halo already conveys at zero cost.
    // Reverted deliberately, with the finding kept here so it is not
    // rediscovered from scratch.
    inline void Load(RE::Inventory3DManager* a_mgr, RE::TESBoundObject* a_obj, RE::ExtraDataList* a_extra)
    {
        using func_t = void (*)(RE::Inventory3DManager*, RE::TESBoundObject*, RE::ExtraDataList*);
        static REL::Relocation<func_t> func{ RELOCATION_ID(50885, 51758) };
        func(a_mgr, a_obj, a_extra);
    }

    inline void Unload(RE::Inventory3DManager* a_mgr)
    {
        using func_t = void (*)(RE::Inventory3DManager*);
        static REL::Relocation<func_t> func{ RELOCATION_ID(50886, 51759) };
        func(a_mgr);
    }
}
