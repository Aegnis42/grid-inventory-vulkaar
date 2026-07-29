#pragma once

#include <functional>
#include <iosfwd>

#include <imgui.h>

#include <string>
#include <vector>

namespace FUI
{
    // Phase 2: the ONE window-flag set for every managed (WinManager-framed)
    // window — six hand copies converged here. WinManager owns move/size, so
    // ImGui's own chrome and interactions are fully disabled.
    inline constexpr ImGuiWindowFlags kManagedWinFlags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    // B-3 (PLAN_B §2-F): window system — titlebar drag, magnet snapping,
    // parent-child docking, group move, viewport clamping, persistence.
    // 1:1 port of the JS implementation in view/GridInventory/index.html
    // (startWinDrag / magnetize / contactLen / subtreeOf / mouseup docking).
    class WinManager
    {
    public:
        struct Win
        {
            std::string key;               // "main" or bag key
            ImVec2      pos = ImVec2(100.0f, 100.0f);
            ImVec2      size = ImVec2(200.0f, 150.0f);
            std::string parent;            // "" = root; "main" or another key
            bool        posKnown = false;  // has a position (loaded or defaulted)
            int         lastSeen = -1;     // ImGui frame the window was last drawn
        };

        static WinManager* GetSingleton();

        void Load();         // read the ui ini once (call again to hot-reload)
        void Save() const;
        // GI46-48: NAMED share files. "Default" -> GridInventory_Default.ini,
        // "P1" -> GridInventory_P1.ini ... each beside its icon bundle
        // (GridInventory_<name>_icons.pak). One ini carries the style subset
        // of the ui ini (skin, scale, glow, icon settings) AND -- via the
        // hooks below -- the item/category defs, so it reproduces the whole
        // look on another machine. Window layout is resolution-bound and the
        // merchant toggles are personal, so neither ever travels.
        void ExportPreset(const std::string& a_name) const;
        bool ImportPreset(const std::string& a_name);   // false = unreadable file
        [[nodiscard]] static std::string PresetIniPath(const std::string& a_name);
        [[nodiscard]] static std::string PresetPakPath(const std::string& a_name);
        // every GridInventory_<name>.ini next to the plugin, minus the mod's
        // own config files (_ui/_items/...); "Default" sorts first
        [[nodiscard]] std::vector<std::string> ListPresets() const;
        // main.cpp owns the def storage; it registers these so the preset can
        // carry [categories]/[items] without this module knowing the format.
        //   apply(section 1=categories 2=items, key, value)
        void SetPresetDefsHooks(std::function<void(std::ostream&)> a_write,
                                std::function<void(int, const std::string&,
                                                   const std::string&)> a_apply,
                                std::function<void()> a_done);

        // Call BEFORE ImGui::Begin: applies the stored position.
        void ApplyNext(const std::string& a_key, ImVec2 a_defaultPos, ImVec2 a_defaultSize);

        // Call right after ImGui::Begin: draws the drag strip, records the
        // window rect, and starts a drag when the strip is grabbed.
        // a_reserveRight: right-side strip exclusion so titlebar controls
        // (drawn later the same frame) can receive clicks — the strip would
        // otherwise claim ActiveId first and eat them.
        // a_centerTitle: force a centred title regardless of the skin (small
        // confirm dialogs — loadout buy/delete — look lopsided left-anchored).
        void TitleBar(const std::string& a_key, const char* a_label, float a_reserveRight = 0.0f,
                      bool a_centerTitle = false);

        // Phase 2: shared chrome for the centred confirm-style popups
        // (quantity slider / sell-confirm / loadout buy / delete):
        // screen-centred ApplyNext + Begin(kManagedWinFlags) + overlay-rect
        // registration (hover-through prevention can't be forgotten any more)
        // + centred TitleBar. Returns TRUE when an outside click cancelled
        // the popup THIS frame — the caller resets its own state and plays
        // its cancel sound. ImGui::End() is always the caller's job.
        bool BeginConfirmPopup(const std::string& a_key, const char* a_imguiId,
                               const char* a_title, ImVec2 a_size);

        // Once per frame after every window drew: drag / magnet / dock / clamp.
        void Update();

        void SetDragLock(bool a_lock) { m_dragLock = a_lock; }   // F1: item carry locks window drag

        [[nodiscard]] Win*   Find(const std::string& a_key);
        [[nodiscard]] ImVec2 MainCenter(ImVec2 a_fallback);      // park anchor

    private:
        WinManager() = default;

        Win&                     Ensure(const std::string& a_key);
        [[nodiscard]] bool       IsOpen(const Win& a_win) const;
        std::vector<std::string> SubtreeOf(const std::string& a_key) const;
        void                     StartDrag(const std::string& a_key);
        void                     EndDrag();

        // length of the shared (flush) edge between two rects; 0 = not touching
        static float ContactLen(ImVec2 a_min, ImVec2 a_max, ImVec2 b_min, ImVec2 b_max);

        ImVec2 Magnetize(ImVec2 a_pos, ImVec2 a_size,
                         const std::vector<std::string>& a_excluded) const;

        static constexpr float kMagnet = 14.0f;   // snap distance (JS: M)

        struct DragState
        {
            bool        active = false;
            std::string key;
            ImVec2      grab = ImVec2(0.0f, 0.0f);   // mouse - winPos at start
            struct Follower
            {
                std::string key;
                ImVec2      off;   // follower pos - dragged pos (fixed)
            };
            std::vector<Follower> followers;
            ImVec2 extMin = ImVec2(0.0f, 0.0f);      // group extent rel. to pos
            ImVec2 extMax = ImVec2(0.0f, 0.0f);
        };

        std::vector<Win> m_wins;
        DragState        m_drag;
        bool             m_dragLock = false;
        bool             m_loaded = false;
    };
}
