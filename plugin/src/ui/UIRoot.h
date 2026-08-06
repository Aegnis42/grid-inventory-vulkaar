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
    // One line of help for the BOTTOM prompt bar, good for this frame only.
    // Call it while a control is hovered; the bar shows it in place of its
    // ambient hints. Keeps help out from under the cursor.
    void NoteHoverHint(const char* a_text);

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

    // ---- gamepad -----------------------------------------------------------
    // The engine hides its Cursor Menu (and stops advancing MenuCursor) when a
    // controller is driving, which left this UI with no pointer at all. These
    // feed a cursor we own and draw ourselves; the input sink in main.cpp calls
    // them for pad events only while our menu is open. A real mouse movement
    // hands control straight back.
    //   a_right : right stick (scroll) vs left stick (pointer)
    //   a_idCode: RE::BSWin32GamepadDevice::Keys value
    void NotePadStick(bool a_right, float a_x, float a_y);
    void NotePadButton(std::uint32_t a_idCode, bool a_pressed);
    // A genuine mouse event hands the pointer back. Must come from a real
    // device event: the OS cursor's POSITION is useless as a signal here,
    // because the game parks and re-warps it while a pad is driving.
    void NoteMouseInput();
    [[nodiscard]] bool IsPadActive();
    // Should GridMenu keep asking for the vanilla Cursor Menu? False only when
    // we have taken over the pointer ourselves (see PadCursorMode).
    [[nodiscard]] bool WantsGameCursor();

    // ---- control hints -----------------------------------------------------
    // What to PRINT for an action in the tooltip's hint lines. While a pad is
    // driving this is the button actually bound to it ("A" / "X" / "LT"), which
    // is only knowable by asking the engine — the player may have rebound it,
    // and a non-Xbox pad reports its own layout. Otherwise it is the keyboard /
    // mouse key. The pad side is resolved once per menu open and cached: the
    // lookup does string work, and this is called every frame a tooltip is up.
    enum class Act : std::uint8_t
    {
        kPrimary,     // pick up / place
        kSecondary,   // equip / read / bag ...
        kDrop,
        kFavorite,
        kInspect,
        kSplit,       // split a stack / hold to compare
        // GI63: rotation is OURS, not a rebindable game action, so these have no
        // ControlMap entry and always resolve to their keyboard labels. Routing
        // them through KeyLabel anyway keeps every prompt on one code path -- and
        // the day a pad binding exists, only the table below changes.
        kRotateCCW,
        kRotateCW,
    };
    [[nodiscard]] const char* KeyLabel(Act a_act);

    // ★Hand the engine's OWN Cursor Menu the real thumbstick event so it moves
    // its own arrow — `CursorMenu` is a MenuEventHandler with a ProcessThumbstick
    // of its own (that is how the world map's pad cursor works). Our menu is not
    // in MenuControls' handler list, so the event never reaches it by itself.
    // Poking MenuCursor::cursorPos* instead does NOT work: the arrow only
    // re-reads that pair when an input event arrives, so it just teleported on
    // the next button press. We pass the live event straight through — no
    // synthesised event, no vtable games.
    void FeedEngineCursor(RE::ThumbstickEvent* a_event);

    // ★True while the game's own Book Menu is up on top of us (right-click on
    // a book / note). Our overlay renders LAST, so it would paint straight
    // over the book; we stand down entirely instead — no draw, and every
    // input channel passes through, or the book could not even be closed.
    [[nodiscard]] bool IsBookOpen();

    // ★★True while the game's console is up. Unlike the book it does NOT hide
    // us — it draws over the bottom of the screen and takes the keyboard —
    // so we keep rendering and only stand down from KEY input. The engine
    // delivers the same keystrokes to both menus, which is how typing a
    // console command also filled the item search box.
    [[nodiscard]] bool IsConsoleOpen();

    // main.cpp installs these to keep legacy state (attack-input block) in sync
    void SetVisibilityCallbacks(std::function<void()> a_onShow, std::function<void()> a_onHide);

    // item icon draw with the ICON LIGHT setting applied live: <=1 darkens
    // via tint, >1 brightens via an extra additive pass (Theme::IconGain).
    // Every item-sprite AddImage must go through this for a consistent look.
    void DrawItemIcon(ImDrawList* a_dl, void* a_srv, const ImVec2& a_min, const ImVec2& a_max);
    // GI52: same passes, drawn around a centre at an angle. Drawn (category)
    // icons carry their own rotation, which a 3D capture never needed — the
    // capture bakes its orientation into the pixels.
    void DrawItemIconRot(ImDrawList* a_dl, void* a_srv, const ImVec2& a_centre,
                         const ImVec2& a_size, float a_deg);

    // skin support
    [[nodiscard]] void*   GlowTexture();    // radial falloff SRV (rarity glow)
}
