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
