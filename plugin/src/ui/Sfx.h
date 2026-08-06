#pragma once

#include <imgui.h>

#include "ui/Theme.h"

// Windows.h (winmm) defines PlaySound -> PlaySoundA, shadowing RE::PlaySound
#ifdef PlaySound
#    undef PlaySound
#endif

// Tiny UI sound helper: play a vanilla sound descriptor (SNDR) by its
// Skyrim.esm FormID (IDs verified against the shipped master).
namespace FUI::Sfx
{
    // 3D fallbacks for the bag open/close wrappers (custom SNDR absent)
    inline constexpr RE::FormID kSackOpen      = 0x00084D0E;  // DRScSackOpen  (cloth bag)
    inline constexpr RE::FormID kSackClose     = 0x00084D0F;  // DRScSackClose

    // pure UI (2D) sounds: the engine's global PlaySound (the console
    // command's implementation) — resolves SOUN editor IDs through the audio
    // registry. Both manual build paths (descriptor and editor-ID) stayed
    // silent for UI-category sounds; this is the engine's own route.
    inline void PlayUI(const char* a_editorID)
    {
        if (a_editorID) RE::PlaySound(a_editorID);
    }

    inline void Play(RE::FormID a_sndr)
    {
        auto* descr = RE::TESForm::LookupByID<RE::BGSSoundDescriptorForm>(a_sndr);
        auto* am = RE::BSAudioManager::GetSingleton();
        if (!descr || !am) return;
        RE::BSSoundHandle handle;
        if (am->BuildSoundDataFromDescriptor(handle, descr) && handle.IsValid()) {
            // WORLD (3D) descriptors like the sack open/close are silent
            // without a position — attach to the player; UI (2D) sounds
            // ignore the follow target, so this is safe for both kinds
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* root = player->Get3D()) {
                    handle.SetObjectToFollow(root);
                }
            }
            handle.Play();
        }
    }

    // ---- user-authored SNDR slots (Grid Inventory.esp, fine-grained set) --
    // The vanilla UI sounds proved too quiet through every plugin-side play
    // path, so the user authors their OWN records — when a slot exists it
    // wins, else the vanilla fallback plays.
    inline constexpr const char* kSfxPlugin = "Grid Inventory.esp";
    inline constexpr RE::FormID  kSndFavorite  = 0x00080F;  // UI_Favorite
    inline constexpr RE::FormID  kSndInvOpen   = 0x000810;  // UI_Inventory_Open_01
    inline constexpr RE::FormID  kSndInvClose  = 0x000811;  // UI_Inventory_Close_01
    inline constexpr RE::FormID  kSndBagOpen   = 0x000812;  // UI_Inventory_Open_02
    inline constexpr RE::FormID  kSndBagClose  = 0x000813;  // UI_Inventory_Close_02
    inline constexpr RE::FormID  kSndSelectOn  = 0x000814;  // UI_Select_On
    inline constexpr RE::FormID  kSndSelectOff = 0x000815;  // UI_Select_Off
    inline constexpr RE::FormID  kSndFocus     = 0x000816;  // UI_Menu_Focus
    inline constexpr RE::FormID  kSndFail      = 0x000817;  // UI_Activate_Fail

    inline bool PlayCustom(RE::FormID a_local)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* descr = dh ? dh->LookupForm<RE::BGSSoundDescriptorForm>(
                               a_local, kSfxPlugin)
                         : nullptr;
        auto* am = RE::BSAudioManager::GetSingleton();
        if (!descr || !am) return false;
        RE::BSSoundHandle handle;
        if (!am->BuildSoundDataFromDescriptor(handle, descr) || !handle.IsValid()) {
            return false;
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* root = player->Get3D()) {
                handle.SetObjectToFollow(root);
            }
        }
        handle.Play();
        return true;
    }

    // ---- semantic wrappers (custom slot first, vanilla fallback) ----------
    inline void MenuOpen()  { if (!PlayCustom(kSndInvOpen))   PlayUI("UIMenuBladeOpenSD"); }
    inline void MenuClose() { if (!PlayCustom(kSndInvClose))  PlayUI("UIMenuBladeCloseSD"); }
    inline void BagOpen()   { if (!PlayCustom(kSndBagOpen))   Play(kSackOpen); }
    inline void BagClose()  { if (!PlayCustom(kSndBagClose))  Play(kSackClose); }
    inline void Favorite()  { if (!PlayCustom(kSndFavorite))  PlayUI("UIMenuOK"); }
    // every non-cancel click / slider up / shift+click
    inline void SelectOn()  { if (!PlayCustom(kSndSelectOn))  PlayUI("UIMenuOK"); }
    // every cancel & close / slider down
    inline void SelectOff() { if (!PlayCustom(kSndSelectOff)) PlayUI("UIMenuCancel"); }
    // cursor entering an item tile or a button
    inline void Focus()     { if (!PlayCustom(kSndFocus))     PlayUI("UIMenuFocus"); }
    // rejection blip (quest-locked / not enough gold / inventory full ...)
    inline void Fail()      { if (!PlayCustom(kSndFail))      PlayUI("UIActivateFail"); }
    // corner notification + the rejection blip in one call — replaces the
    // old RE::DebugNotification(msg, "UIActivateFail") pattern
    inline void FailNote(const char* a_msg)
    {
        RE::DebugNotification(a_msg);
        Fail();
    }

    // ---- hover edge detection (one Focus per widget entered) --------------
    namespace detail
    {
        inline std::uint32_t g_hoverLast = 0;
        inline double        g_hoverWhen = 0.0;
    }
    inline void HoverNote(std::uint32_t a_id)
    {
        if (a_id == detail::g_hoverLast) return;
        detail::g_hoverLast = a_id;
        const double now = ImGui::GetTime();
        if (now - detail::g_hoverWhen < 0.06) return;   // fast sweeps: soft-throttle
        detail::g_hoverWhen = now;
        Focus();
    }
    inline void HoverReset()   // call when nothing is hovered (re-arms re-entry)
    {
        detail::g_hoverLast = 0;
    }
    // suppress hover blips for a moment (e.g. right after DROPPING an item:
    // the tile materialises under the cursor and would tick immediately)
    inline void HoverMute(double a_sec)
    {
        detail::g_hoverWhen = ImGui::GetTime() + a_sec - 0.06;
    }

    // ---- click-wired button (hover Focus + SelectOn / SelectOff) ----------
    // ★★A ROUNDED frame cannot contain its own fill. ImGui fills across the
    // whole rect and then strokes half a pixel INSIDE it, so the two arcs are
    // struck from centres 0.5px apart and the fill reaches ~0.71px past the
    // stroke at 45°. On a square frame nothing shows; on a rounded one a bright
    // face bleeds out of all four corners. Thickening the stroke covers it and
    // makes every button heavier — so instead: suppress ImGui's fill, and paint
    // our own INSET by the stroke on a channel BEHIND the widget. ImGui still
    // draws the label, on top, exactly where it always was.
    inline bool Button(const char* a_label, const ImVec2& a_size = ImVec2(0, 0),
                       bool a_cancel = false)
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const ImVec4 cIdle = st.Colors[ImGuiCol_Button];
        const ImVec4 cHov  = st.Colors[ImGuiCol_ButtonHovered];
        const ImVec4 cAct  = st.Colors[ImGuiCol_ButtonActive];
        const bool inset = Theme::S().lightPanel && st.FrameBorderSize > 0.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // ★★The label is drawn BY HAND, so it can be centred on its ink
        // rather than on its line box (Theme::TextInkCentered). ImGui centres
        // the box, which reserves descender room every string does not use —
        // "EDIT" then rides high and "+" sits low, each by its own amount, so
        // there is no single nudge that fixes them together.
        // The frame is still ImGui's: pass it an empty label at an explicit
        // size (the same size it would have computed) and it draws everything
        // but the text.
        char vis[128];
        {
            const char* hash = std::strstr(a_label, "##");
            size_t n = hash ? static_cast<size_t>(hash - a_label) : std::strlen(a_label);
            if (n > sizeof(vis) - 1) n = sizeof(vis) - 1;
            std::memcpy(vis, a_label, n);
            vis[n] = '\0';
        }
        const ImVec2 ts = ImGui::CalcTextSize(a_label, nullptr, true);
        ImVec2 sz = a_size;
        if (sz.x <= 0.0f) sz.x = ts.x + st.FramePadding.x * 2.0f;
        if (sz.y <= 0.0f) sz.y = ts.y + st.FramePadding.y * 2.0f;

        if (inset) {
            const ImVec4 clear(0.0f, 0.0f, 0.0f, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, clear);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, clear);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, clear);
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
        }
        ImGui::PushID(a_label);
        const bool pressed = ImGui::Button("##b", sz);
        ImGui::PopID();
        if (inset) {
            const ImVec4& face = ImGui::IsItemActive()    ? cAct
                               : ImGui::IsItemHovered()   ? cHov
                                                          : cIdle;
            dl->ChannelsSetCurrent(0);
            const float b = st.FrameBorderSize;
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const ImVec2 p1 = ImGui::GetItemRectMax();
            if (face.w > 0.0f) {
                dl->AddRectFilled(ImVec2(p0.x + b, p0.y + b),
                                  ImVec2(p1.x - b, p1.y - b),
                                  ImGui::GetColorU32(face),
                                  (st.FrameRounding > b) ? st.FrameRounding - b : 0.0f);
            }
            // ★BEVEL — light along the top and left, dark along the bottom and
            // right, one pixel inside the border. Pressed, the two swap and the
            // control actually reads as pushed in.
            // ★Runs even when the face is TRANSPARENT: an off button shows the
            // panel through it, and a gradient would have nothing to sit on,
            // but these are lines drawn on the frame itself. That is why this
            // treatment works here and a gradient does not.
            // The runs stop short of the corner radius — a straight line cannot
            // follow the arc, and at 3px nobody reads the gap as missing.
            const float r  = (st.FrameRounding > b) ? st.FrameRounding - b : 0.0f;
            const float in = b + 0.5f;
            // pressed = the light flips to the far side, which is what "pushed
            // in" looks like (Theme::Bevel* is the one home for the model)
            const ImU32 lt = ImGui::IsItemActive() ? Theme::BevelShd()
                                                   : Theme::BevelLit();
            const ImU32 dk = ImGui::IsItemActive() ? IM_COL32(255, 255, 255, 51)
                                                   : Theme::BevelShd();
            const float x0 = p0.x + in, x1 = p1.x - in;
            const float y0 = p0.y + in, y1 = p1.y - in;
            dl->AddLine(ImVec2(x0 + r, y0), ImVec2(x1 - r, y0), lt);   // top
            dl->AddLine(ImVec2(x0, y0 + r), ImVec2(x0, y1 - r), lt);   // left
            dl->AddLine(ImVec2(x0 + r, y1), ImVec2(x1 - r, y1), dk);   // bottom
            dl->AddLine(ImVec2(x1, y0 + r), ImVec2(x1, y1 - r), dk);   // right
            dl->ChannelsMerge();
            ImGui::PopStyleColor(3);
        }
        // after any channel merge, so the label lands on top of the face
        if (vis[0]) {
            Theme::TextInkCentered(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                   ImGui::GetColorU32(ImGuiCol_Text), vis);
        }
        if (ImGui::IsItemHovered()) HoverNote(ImGui::GetItemID());
        if (pressed) {
            if (a_cancel) SelectOff();
            else          SelectOn();
        }
        return pressed;
    }
}
