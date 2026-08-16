#pragma once

#include <cstdint>
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
            // ★GI72: the size we last ASKED ImGui for. `size` is what ImGui
            // hands back, and it floors to whole pixels -- so "did the layout
            // change?" has to be answered against the request, never against
            // the readback, or a fractional request looks like a change on
            // every single frame.
            ImVec2      reqSize = ImVec2(0.0f, 0.0f);
            // What ApplyNext added to this window's height for the title's top
            // clearance, so TitleBar offsets the name by the same number.
            float       topPad = 0.0f;
        };

        static WinManager* GetSingleton();

        void Load();         // read the ui ini once (call again to hot-reload)
        void Save() const;

        // ★★Read ONE switch out of the ui ini, without the rest of Load.
        //
        // Load runs when the inventory is first drawn, because that is when the
        // things it configures (skins, scale, window boxes) first matter. The
        // quick wheel is not one of those: it answers a key during ordinary
        // play, so a player who turns it off and restarts had it back until
        // they happened to open their bag -- the wheel would open and suppress
        // the vanilla menu it was supposed to have handed back.
        //
        // ★Not a second copy of the value: Save() still writes it in one place
        // and Load() still reads it with everything else. This is the same key
        // read earlier, for the one setting that is needed before any menu
        // exists. Returns the default when the file or key is absent.
        [[nodiscard]] static bool ReadWheelEnabled(bool a_default);
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

        // ★Which corner stays put when a window's CODE-DEFINED size changes
        //  while it is open. Sizes are not user-editable, but they do change:
        //  the main window drops its whole equipment column in loot/barter mode
        //  (~412px). Holding the top-LEFT fixed there slid the item grid
        //  sideways by that width and stranded — or swallowed — whatever the
        //  user had docked to the edge that moved (user report).
        //  The main window pins its RIGHT edge because the grid is the part
        //  that survives both modes and it is right-aligned in the frame; the
        //  left column then appears and disappears leftwards, into the space
        //  the partner window uses anyway.
        enum class Anchor : std::uint8_t
        {
            kTopLeft = 0,   // default — position IS the top-left corner
            kTopRight       // right edge holds; the window grows/shrinks leftwards
        };

        // Call BEFORE ImGui::Begin: applies the stored position.
        // ★★a_topPad: clearance ABOVE the title (Theme::TitleTopPad). It is taken
        // HERE, not at TitleBar, because this is the one call that owns the SIZE
        // and still runs before Begin -- so the height grows by exactly what the
        // title is offset by, out of one argument, and the two cannot drift.
        // A window that does not pass it gets neither, which is the opt-in the
        // old separate TitleBar parameter was for; the difference is that it can
        // no longer be passed to one and forgotten at the other.
        void ApplyNext(const std::string& a_key, ImVec2 a_defaultPos, ImVec2 a_defaultSize,
                       Anchor a_anchor = Anchor::kTopLeft, float a_topPad = 0.0f);

        // Call right after ImGui::Begin: draws the drag strip, records the
        // window rect, and starts a drag when the strip is grabbed.
        // a_reserveRight: right-side strip exclusion so titlebar controls
        // (drawn later the same frame) can receive clicks — the strip would
        // otherwise claim ActiveId first and eat them.
        // a_centerTitle: force a centred title regardless of the skin (small
        // confirm dialogs — loadout buy/delete — look lopsided left-anchored).
        // ★The title's top pad is NOT a parameter here any more -- it is read
        // back from what ApplyNext recorded for this key. It used to be passed
        // to both, and the comment describing which windows opted out went stale
        // the same day four of them opted in.
        void TitleBar(const std::string& a_key, const char* a_label, float a_reserveRight = 0.0f,
                      bool a_centerTitle = false);

        // Height of the title strip, scale included. Anything a caller draws
        // INTO that strip (EDIT / SETTINGS / a bag's COLLECT) centres itself
        // against this — hardcoding the number at each call site is how those
        // controls drifted off the title's line.
        [[nodiscard]] static float TitleBarH();

        // ★★Where a glyph of this height sits on the title's LINE, for the
        // window named by a_key. Three call sites derived it by hand -- the
        // title itself, the main window's EDIT / SETTINGS / x, and a bag's
        // COLLECT -- and the third took the frame inset WHOLE where the other
        // two take half, so it sat 12px low on every torn skin and 3px low on
        // every translucent one. Two terms, both easy to get wrong: HALF the
        // frame inset (the title belongs to the bar, not under the frame) and
        // ALL of the top pad (the window's height already paid for it).
        [[nodiscard]] static float TitleTextY(const std::string& a_key, float a_glyphH);

        // Phase 2: shared chrome for the centred confirm-style popups
        // (quantity slider / sell-confirm / loadout buy / delete):
        // ★Opens AT THE CURSOR (clamped so the whole popup stays on screen),
        // re-placed on every open -- a popup is answered and dismissed, so it
        // has no position worth remembering and no reason to make the player
        // travel to the middle of the display.
        // ApplyNext + Begin(kManagedWinFlags) + overlay-rect
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
        // a_openOnly=false includes docked windows that are currently closed —
        // a resize must move them too, or they reopen detached.
        std::vector<std::string> SubtreeOf(const std::string& a_key,
                                           bool a_openOnly = true) const;
        void                     StartDrag(const std::string& a_key);
        void                     EndDrag();

        // Re-place a_key for a new code-defined size: honour a_anchor and drag
        // everything docked to it along the edge it was docked to.
        void Reanchor(const std::string& a_key, ImVec2 a_newSize, Anchor a_anchor);

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
