#pragma once

#include <imgui.h>

namespace FUI
{
    // Ported from ModExplorerMenu (Modex) by patchulidev — ModexGUIMenu.
    // https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
    //
    // The kCustomRendering menu whose PostDisplay is the game's UI render
    // pass — the only stage where Inventory3DManager::Render() actually
    // draws (MODEX_TRANSITION_PLAN §2 root cause). Also relays Scaleform
    // input events into ImGui.
    class GridInventoryMenu : public RE::IMenu
    {
    public:
        static constexpr std::string_view MENU_NAME = "GridInventoryMenu";

        static void RegisterMenu();
        static void FlushInputState();

        // F1: the titlebar ✕ plays MenuClose itself before UIRoot::Close();
        // this suppresses OnHide's fallback so the sound doesn't double up.
        static void MarkCloseSfxPlayed();

        void PostDisplay() override;
        void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;
        RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;

    private:
        void OnShow();
        void OnHide();

        static RE::IMenu* Creator();

        static void ProcessScaleformEvent(const RE::BSUIScaleformData* a_data);
        static void OnMouseEvent(RE::GFxEvent* a_event, bool a_down);
        static void OnMouseWheelEvent(RE::GFxEvent* a_event);
        static void OnKeyEvent(RE::GFxEvent* a_event, bool a_down);
        static void OnCharEvent(RE::GFxEvent* a_event);
        static void ForceCursor();
        // show / hide the vanilla Cursor Menu. Both directions, because a pad
        // session draws its own pointer and has to put the engine's away --
        // and hand it back when the menu closes.
        static void SetGameCursor(bool a_want);

        static ImGuiKey GFxKeyToImGuiKey(RE::GFxKey::Code a_keyCode);
    };
}
