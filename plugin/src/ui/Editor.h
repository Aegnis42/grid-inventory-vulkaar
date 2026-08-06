#pragma once

#include "ui/ItemDef.h"

#include <functional>
#include <string>
#include <vector>

namespace FUI::Editor
{
    // B-6 (PLAN_B H6): redesigned EDIT mode. (Preset sharing moved to the
    // settings window -- GI47 unified it with the style preset.)
    // FullDef is an alias of the ONE shared FUI::ItemDef (Phase 2 D2) —
    // the editor edits it and writes through the hooks below.

    // main.cpp wires these (def storage/ini live there).
    struct Hooks
    {
        std::function<FullDef(RE::TESBoundObject*)>                  getEffective;    // ini override -> category
        std::function<FullDef(RE::TESBoundObject*)>                  getDefault;      // category preset only
        std::function<bool(RE::TESBoundObject*)>                     hasOverride;
        // persist=false: memory-only live apply (every change, zero IO);
        // persist=true: also upsert the ini line (debounced / on release)
        std::function<void(RE::TESBoundObject*, const FullDef&, bool)> setOverride;
        std::function<void(RE::TESBoundObject*)>                     resetOverride;   // remove line
        std::function<void(RE::TESBoundObject*, const FullDef&)>     saveAsCategory;  // tune the whole category
        std::function<std::string(RE::TESBoundObject*)>              categoryName;
    };
    void SetHooks(Hooks a_hooks);

    [[nodiscard]] bool IsEditMode();
    void ToggleEditMode();

    // Grid integration: clicks select instead of carrying while editing.
    void Select(RE::TESBoundObject* a_obj, const std::string& a_key);
    [[nodiscard]] bool IsSelected(const std::string& a_key);

    // Draw the editor panel window (call from UIRoot::Render while editing).
    void DrawPanel();

    void OnMenuClosed();   // drop selection, discard unsaved edits

    // ★Editing is a session: the values apply live but reach the ini only on
    // SAVE, and leaving an item (or EDIT, or the menu) puts the baseline back.
    // These two let the bottom bar say so — the model's one risk is losing
    // work silently, so both states are reported where the player is looking.
    [[nodiscard]] bool HasUnsavedEdits();
    [[nodiscard]] bool DiscardNoteActive();
}
