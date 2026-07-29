#pragma once

#include <imgui.h>

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
    inline bool Button(const char* a_label, const ImVec2& a_size = ImVec2(0, 0),
                       bool a_cancel = false)
    {
        const bool pressed = ImGui::Button(a_label, a_size);
        if (ImGui::IsItemHovered()) HoverNote(ImGui::GetItemID());
        if (pressed) {
            if (a_cancel) SelectOff();
            else          SelectOn();
        }
        return pressed;
    }
}
