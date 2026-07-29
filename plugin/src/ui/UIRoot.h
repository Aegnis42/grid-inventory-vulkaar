#pragma once

#include <functional>

struct ImDrawList;
struct ImFont;
struct ImVec2;

// Plan B core: owns the ImGui context (init from the game's D3D11 device),
// runs the per-frame ImGui pass from GridInventoryMenu::PostDisplay, and
// opens/closes the menu via the UI message queue.
namespace FUI::UIRoot
{
    void RegisterMenu();  // RE::UI::Register (call at kDataLoaded)
    bool TryInitD3D();    // idempotent ImGui init from renderer data
    bool IsInitialized();

    void Open();   // queue kShow for GridInventoryMenu
    void Close();  // queue kHide

    // I/ESC close staging: an open settings popup or EDIT mode closes FIRST
    // (returns true); only when nothing was closed does the caller close the
    // whole inventory.
    bool CloseTopWindow();

    // overlay hover blocking: popups/settings/EDIT draw chrome WIDER than
    // their ImGui window rect, so the grid underneath still hover-reacts in
    // the chrome margin. Overlay windows register their (margin-padded) rect
    // each frame; the grids skip hover/clicks while the mouse is inside one.
    void NoteOverlayRect();   // call INSIDE the overlay window's Begin scope
    bool MouseInOverlay();    // previous frame's rects (draw-order safe)

    // INSPECT overlay — C key, matching vanilla's "Item Zoom" binding
    // (controlmap Item Zoom = 0x2E). Rotatable full-size view of the engine's
    // own model render: the ONLY way to read detail that lives on the model
    // (dragon-claw glyphs drive the Bleak Falls Barrow door puzzle).
    void OpenInspect(RE::TESBoundObject* a_obj, const std::string& a_key);
    [[nodiscard]] bool IsInspectOpen();
    bool CloseInspect();      // false when it wasn't open (ESC chain)

    void OnShow();   // menu received kShow
    void OnClose();  // menu received kHide
    void Render();   // full ImGui frame; called from PostDisplay only
    void Tick();     // game-update hook (pre-render): def apply + model parking

    void AddScrollEvent(float a_x, float a_y);

    // true while an ImGui text field owns the keyboard (preset name etc.) —
    // the raw-input I-key close must not fire while the user is typing
    [[nodiscard]] bool IsTextInputActive();

    // main.cpp installs these to keep legacy state (attack-input block) in sync
    void SetVisibilityCallbacks(std::function<void()> a_onShow, std::function<void()> a_onHide);

    // item icon draw with the ICON LIGHT setting applied live: <=1 darkens
    // via tint, >1 brightens via an extra additive pass (Theme::IconGain).
    // Every item-sprite AddImage must go through this for a consistent look.
    void DrawItemIcon(ImDrawList* a_dl, void* a_srv, const ImVec2& a_min, const ImVec2& a_max);

    // skin support (WinManager::TitleBar consumes these)
    [[nodiscard]] void*   GlowTexture();    // radial falloff SRV (rarity glow)
    [[nodiscard]] void*   TornGlowA();      // torn 9-slice SRV, thin cream rim (skin 3)
    [[nodiscard]] void*   TornGlowB();      // torn 9-slice SRV, soft cream halo (skin 4)
    [[nodiscard]] void*   TornCreamA();     // V1 cream rebake of glowA (skin 5)
    [[nodiscard]] void*   TornBrightB();    // V2 bright rebake of glowB (skin 6)
}
