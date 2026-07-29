#pragma once

#include <imgui.h>

namespace FUI::Theme
{
    // B-7b skin system (mockup v9): a skin swaps colour tokens AND structural
    // grammar (rounding, title treatment, frame style, translucency). Layout
    // metrics never change between skins.
    struct Skin
    {
        const char* name;
        // colour tokens (from the v9 mockup CSS)
        ImVec4 acc;        // accent (lines, titles) — alpha applied per use
        ImVec4 hi;         // bright accent (values, badges)
        ImVec4 ink;        // body text
        ImVec4 inkDim;     // secondary text
        ImVec4 winBg;      // window background (kept fully opaque: parking)
        ImVec4 glyph;      // item/silhouette tint
        ImVec4 shade;      // occupied-cell overlay
        ImVec4 sel;        // selection / highlight
        ImVec4 filled;     // equipped-slot border accent
        // structural grammar
        // (the VELLUM/NORDIC grammar block — titleCentered / cornerBrackets /
        // slotBevel / paperTexture / serifTitle / darkText — was removed with
        // those skins; no surviving skin ever set any of them)
        float rounding;        // window/frame rounding
        float titleSpacing;    // extra px between title glyphs (at scale 1)
        // OATHVEIN UNTARNISHED (v10.4) grammar — default false for other skins
        bool  cornerFade = false;      // borders drawn as corner-fade lines (no full border)
        bool  topStrip = false;        // 2px crimson strip across the window top
        bool  titleGlow = false;       // glowing title + right-fading underline
        bool  diamondLabels = false;   // section labels: "◇ LABEL" in sel colour
        bool  tornFrame = false;       // 9-slice torn-paper panel texture (window bg)
        bool  tornGlowB = false;       // torn glow: false = thin rim (A), true = soft halo (B)
        // which torn texture this skin draws (skins 5/6 use the light CREAM
        // rebakes): 0 = glowA, 1 = glowB, 2 = creamA (V1), 3 = brightB (V2)
        int   tornTex = 0;
        // translucent windows (skins 5/6) are safe because the capture path
        // saves/restores the FULL backbuffer frame (ItemPreview Render step
        // 1/5), so the parked model never survives onto the visible frame
        bool  translucent = false;   // respect winBg alpha (others force 1.0)
        bool  bevelChrome = false;   // grey gradient titlebar + dark/light bevel border
    };

    // 9-slice source margins for the torn-paper frame texture (fraction of the
    // texture, from bake_torn_glow3.py — includes the transparent glow-fade
    // margin). Corners fixed, edges/centre stretch.
    // torn-frame 9-slice source margins. MEASURED from the textures' actual
    // ragged-border depth (99.5th pct alpha fringe: L100/R69/T215/B130 px of
    // 640x766, +1% headroom). The old 0.069/0.056 were shallower than the
    // fringe, so the edge slices carried transparent fringe INTO the window —
    // invisible on the big main window, but small windows (editor/settings)
    // showed wide empty bands ("skin 3/4 cut-off" bug).
    inline constexpr float kTornUM = 0.1663f;
    inline constexpr float kTornVM = 0.2901f;

    // H′: global UI scale (0.5~1.6), persisted as "!uiscale".
    [[nodiscard]] float Scale();
    void SetScale(float a_scale);

    // rarity glow style, persisted as "!glowstyle":
    // 1 = silhouette halo (icon's blurred alpha, plan A) — default
    // 0 = stretched radial across the footprint (previous look, kept for revert)
    [[nodiscard]] int  GlowStyle();
    void SetGlowStyle(int a_style);

    // rarity glow brightness multiplier (0.2~2.5, default 1.0), persisted
    // PER STYLE as "!glowgain0"/"!glowgain1". GlowGain()/SetGlowGain() act on
    // the ACTIVE style; the *Of variants address a specific one (persistence,
    // presets). Scales the per-rarity tint alphas at draw time.
    [[nodiscard]] float GlowGain();
    void SetGlowGain(float a_gain);
    [[nodiscard]] float GlowGainOf(int a_style);
    void SetGlowGainOf(int a_style, float a_gain);

    // item icon brightness (0.4~1.6, default 1.0), "!icongain". Applied LIVE
    // at draw time by UIRoot::DrawItemIcon: <=1 darkens via tint, >1
    // brightens via an extra additive-blend pass (a tint alone can't go up).
    [[nodiscard]] float IconGain();
    void SetIconGain(float a_gain);

    // active skin (1..4), persisted as "!skin"
    [[nodiscard]] int  SkinIndex();
    void SetSkin(int a_index);            // re-applies the ImGui style
    [[nodiscard]] const Skin& S();        // active skin tokens
    [[nodiscard]] int  SkinCount();
    [[nodiscard]] const Skin& SkinAt(int a_index);   // 1-based

    // apply the active skin to the ImGui style (call at init + on change)
    void Apply();

    // tornFrame breathing room: extra content inset (px, scaled) so the title
    // and body clear the ragged frame edge. 0 for every non-torn skin — each
    // managed window grows by 2x this and shifts its content in by it.
    [[nodiscard]] float FrameInsetX();
    [[nodiscard]] float FrameInsetY();

    // helpers: token -> ImU32 with alpha override
    [[nodiscard]] ImU32 Acc(float a_alpha);
    [[nodiscard]] ImU32 Col(const ImVec4& a_c, float a_alpha = -1.0f);

    // gauge/slider track chrome: translucent skins fill in sel (the EDIT
    // painter red) — a grey acc fill reads as dead space over the world
    [[nodiscard]] ImU32 GaugeFill();
    [[nodiscard]] ImU32 GaugeBorder();

    // v10.4: border drawn as 8 gradient segments — bright at the corners,
    // fading out toward each edge's middle (vertical runs are inset 1px so
    // the corner pixel never double-blends). a_col carries the peak alpha.
    void CornerFade(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col,
                    float a_frac = 0.32f);
    // GI49b: same 8 segments with per-edge colors (status rings split
    // temper/poison across left+top vs right+bottom)
    void CornerFadeEdges(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max,
                         ImU32 a_top, ImU32 a_left, ImU32 a_right, ImU32 a_bottom,
                         float a_frac);

    // 9-slice draw of a texture into [a_min,a_max]: corners fixed at a_margin
    // px, edges/centre stretched. a_um/a_vm are the source corner fractions.
    void NineSlice(ImDrawList* a_dl, void* a_tex, ImVec2 a_min, ImVec2 a_max,
                   float a_margin, float a_um, float a_vm);

    // Design-pass A: the ONE quantity-slider look (settings track chrome —
    // black .2 ground, acc .20 fill, acc .25 border, centred hi value) shared
    // by the quantity popup, the pouch withdraw and any future int slider.
    // Draws at the current cursor, a_w wide, frame height tall.
    bool ChromeSliderInt(const char* a_id, int* a_v, int a_min, int a_max,
                         float a_w, const char* a_fmt = "%d");

    // float twin, absolute position tracking (the settings rows previously
    // used DragFloat under slider-looking chrome — dragging felt broken
    // because the value crawled by drag-speed instead of following the mouse)
    bool ChromeSliderFloat(const char* a_id, float* a_v, float a_min, float a_max,
                           float a_w, const char* a_fmt = "%.2f");

    // Phase 2: the invisible-widget style for any Drag/Slider drawn OVER
    // custom track chrome (transparent frame + hi-coloured value text) —
    // formerly five hand copies across Editor and the Chrome sliders.
    // a_sliderGrab also hides the Slider* grab (Drag* widgets have none).
    void PushChromeStyle(bool a_sliderGrab);
    void PopChromeStyle(bool a_sliderGrab);
}
