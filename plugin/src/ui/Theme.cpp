#include "ui/Theme.h"

namespace FUI::Theme
{
    namespace
    {
        constexpr ImVec4 Rgba(int r, int g, int b, float a = 1.0f)
        {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        // v9 mockup skin table (+ 5/6: light CREAM torn rebakes, dark ink)
        const Skin kSkins[6] = {
            {   // 1 FABLE AMBER (확정 기본) — winBg = 목업 확정값 #1A1A1A
                // (26,26,26) 무채색 검정. v10.7: corner-fade borders adopted
                // from skin 2; strip/glow/diamond labels stay OATHVEIN-only.
                "FABLE AMBER",
                Rgba(216, 184, 120), Rgba(240, 216, 144), Rgba(216, 200, 168),
                Rgba(216, 200, 168, 0.55f), Rgba(26, 26, 26),
                Rgba(232, 210, 160), Rgba(0, 0, 0, 0.42f),
                Rgba(240, 216, 144), Rgba(216, 184, 120),
                4.0f, 3.0f,
                true, false, false, false,   // cornerFade only
            },
            {   // 2 OATHVEIN UNTARNISHED (v10.4) — 순흑 워밍 패널 + 코너-페이드
                // 테두리 + 상단 크림슨 스트립 + 글로우 타이틀 + ◇ 크림슨 라벨
                "OATHVEIN UNTARNISHED",
                Rgba(230, 226, 216), Rgba(245, 242, 234), Rgba(220, 216, 206),
                Rgba(220, 216, 206, 0.45f), Rgba(32, 32, 32),   // 목업 #202020
                Rgba(236, 232, 222), Rgba(0, 0, 0, 0.5f),
                Rgba(168, 64, 47), Rgba(230, 226, 216),
                0.0f, 5.0f,
                true, true, true, true,   // cornerFade / topStrip / titleGlow / diamondLabels
            },
            {   // 3 FABLE AMBER TORN — 스킨 1(앰버) + 찢긴 프레임 + 크림
                // 라이트림(A, frame_torn_glowA). 슬롯/아이템은 앰버 코너페이드.
                "FABLE AMBER TORN",
                Rgba(216, 184, 120), Rgba(240, 216, 144), Rgba(216, 200, 168),
                Rgba(216, 200, 168, 0.55f), Rgba(26, 24, 22),
                Rgba(232, 210, 160), Rgba(0, 0, 0, 0.42f),
                Rgba(240, 216, 144), Rgba(216, 184, 120),
                4.0f, 3.0f,
                true, false, false, false,   // cornerFade (slots/items), no glow/◇
                true, false,                 // tornFrame + glow A (thin rim)
            },
            {   // 4 OATHVEIN TORN — 스킨 2(크림슨) + 찢긴 프레임 + 소프트
                // 글로우(B, frame_torn_glowB) + 글로우 타이틀 + ◇ 크림슨 라벨.
                "OATHVEIN TORN",
                Rgba(230, 226, 216), Rgba(245, 242, 234), Rgba(220, 216, 206),
                Rgba(220, 216, 206, 0.45f), Rgba(26, 24, 22),
                Rgba(236, 232, 222), Rgba(0, 0, 0, 0.5f),
                Rgba(168, 64, 47), Rgba(230, 226, 216),
                0.0f, 5.0f,
                true, false, true, true,   // cornerFade + titleGlow + diamondLabels
                true, true,                // tornFrame + glow B (soft halo)
                1,                         // tornTex = glowB
            },
            {   // 5 QUICKLOOT DARK — QuickLoot IE-style near-black translucent
                // panel: white ink, silver hairlines (corner-fade chrome),
                // rust-red selection bar. Shares the translucent machinery
                // (caching card / centre park / line boost) with skin 6.
                "QUICKLOOT DARK",
                Rgba(212, 212, 216), Rgba(240, 240, 244), Rgba(228, 228, 232),
                Rgba(228, 228, 232, 0.55f), Rgba(12, 12, 14, 0.58f),
                Rgba(200, 200, 204), Rgba(0, 0, 0, 0.40f),
                Rgba(134, 38, 28), Rgba(212, 212, 216),
                0.0f, 2.0f,
                true, false, false, false,   // cornerFade (silver hairlines)
                false, false,                // no torn frame
                0,                           // tornTex unused
                true, false,                 // translucent (no bevel chrome)
            },
            {   // 6 QUICKLOOT GLASS — skin 5 with the panel FAR more
                // transparent; everything else identical except the tile
                // shade (0.50: tiles must stay readable on the much thinner
                // panel). (MABINOGI GREY was removed by user request; the
                // bevelChrome grammar stays available for future skins.)
                "QUICKLOOT GLASS",
                Rgba(212, 212, 216), Rgba(240, 240, 244), Rgba(228, 228, 232),
                Rgba(228, 228, 232, 0.55f), Rgba(12, 12, 14, 0.38f),
                Rgba(200, 200, 204), Rgba(0, 0, 0, 0.50f),
                Rgba(134, 38, 28), Rgba(212, 212, 216),
                0.0f, 2.0f,
                true, false, false, false,   // cornerFade (silver hairlines)
                false, false,                // no torn frame
                0,                           // tornTex unused
                true, false,                 // translucent (no bevel chrome)
            },
        };

        float g_scale = 1.0f;
        int   g_skin = 6;   // 1-based (GI46: release default = skin 6)
        int   g_glowStyle = 1;   // rarity glow: 1 silhouette (A), 0 radial (revert)
        // brightness multiplier PER STYLE ([0] radial, [1] silhouette) — the
        // two looks need different strengths, so each keeps its own value
        // GI46 release defaults (tuned in playtesting): radial 1.34 /
        // silhouette 0.5, icons slightly brightened.
        float g_glowGain[2] = { 1.34f, 0.5f };
        float g_iconGain = 1.19f;  // item icon brightness (baked at texture upload)
    }

    float IconGain() { return g_iconGain; }

    void SetIconGain(float a_gain)
    {
        g_iconGain = (std::max)(0.4f, (std::min)(1.6f, a_gain));
    }

    int GlowStyle() { return g_glowStyle; }

    void SetGlowStyle(int a_style)
    {
        g_glowStyle = (std::max)(0, (std::min)(1, a_style));
    }

    float GlowGain() { return g_glowGain[g_glowStyle]; }

    void SetGlowGain(float a_gain)
    {
        SetGlowGainOf(g_glowStyle, a_gain);
    }

    float GlowGainOf(int a_style)
    {
        return g_glowGain[(std::max)(0, (std::min)(1, a_style))];
    }

    void SetGlowGainOf(int a_style, float a_gain)
    {
        g_glowGain[(std::max)(0, (std::min)(1, a_style))] =
            (std::max)(0.2f, (std::min)(2.5f, a_gain));
    }

    float Scale() { return g_scale; }

    void SetScale(float a_scale)
    {
        g_scale = (std::max)(0.5f, (std::min)(1.6f, a_scale));
    }

    int SkinIndex() { return g_skin; }

    int SkinCount() { return 6; }

    const Skin& SkinAt(int a_index)
    {
        const int i = (std::max)(1, (std::min)(6, a_index));
        return kSkins[i - 1];
    }

    const Skin& S() { return SkinAt(g_skin); }

    void SetSkin(int a_index)
    {
        g_skin = (std::max)(1, (std::min)(6, a_index));
        if (ImGui::GetCurrentContext()) Apply();
    }

    ImU32 Col(const ImVec4& a_c, float a_alpha)
    {
        ImVec4 c = a_c;
        if (a_alpha >= 0.0f) c.w = a_alpha;
        return ImGui::GetColorU32(c);
    }

    ImU32 Acc(float a_alpha)
    {
        // translucent skins: hairlines at paper-era alphas vanish against the
        // world showing through — boost every acc line/grid across the board
        if (S().translucent && a_alpha >= 0.0f) {
            a_alpha = (std::min)(1.0f, a_alpha * 1.9f);
        }
        return Col(S().acc, a_alpha);
    }

    // larger for the glow textures: the torn panel is inset from the window
    // edge by the transparent glow-fade margin, so content must clear it.
    // translucent skins get a small margin too: with zero inset every
    // separator and grid line ran edge-to-edge ("poking lines").
    float FrameInsetX()
    {
        const Skin& sk = S();
        return sk.tornFrame ? 22.0f * g_scale : sk.translucent ? 10.0f * g_scale : 0.0f;
    }

    float FrameInsetY()
    {
        const Skin& sk = S();
        return sk.tornFrame ? 24.0f * g_scale : sk.translucent ? 6.0f * g_scale : 0.0f;
    }

    // gauge fill/border: sel (EDIT painter red) on translucent skins so the
    // filled portion pops; acc-tinted chrome everywhere else (unchanged)
    ImU32 GaugeFill()
    {
        const Skin& sk = S();
        return sk.translucent ? Col(sk.sel, 0.55f) : Acc(0.20f);
    }

    ImU32 GaugeBorder()
    {
        const Skin& sk = S();
        return sk.translucent ? Col(sk.sel, 0.60f) : Acc(0.25f);
    }

    bool ChromeSliderInt(const char* a_id, int* a_v, int a_min, int a_max,
                         float a_w, const char* a_fmt)
    {
        const auto& sk = S();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float frac = a_max > a_min
            ? static_cast<float>(*a_v - a_min) / static_cast<float>(a_max - a_min)
            : 0.0f;
        dl->AddRectFilled(p, ImVec2(p.x + a_w, p.y + h), IM_COL32(0, 0, 0, 51), sk.rounding);
        if (frac > 0.0f) {
            dl->AddRectFilled(p, ImVec2(p.x + a_w * frac, p.y + h),
                GaugeFill(), sk.rounding);
        }
        dl->AddRect(p, ImVec2(p.x + a_w, p.y + h), GaugeBorder(), sk.rounding);
        PushChromeStyle(true);
        ImGui::SetNextItemWidth(a_w);
        const bool ch = ImGui::SliderInt(a_id, a_v, a_min, a_max, a_fmt,
            ImGuiSliderFlags_AlwaysClamp);
        PopChromeStyle(true);
        return ch;
    }

    bool ChromeSliderFloat(const char* a_id, float* a_v, float a_min, float a_max,
                           float a_w, const char* a_fmt)
    {
        const auto& sk = S();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float frac = a_max > a_min
            ? (std::max)(0.0f, (std::min)(1.0f, (*a_v - a_min) / (a_max - a_min)))
            : 0.0f;
        dl->AddRectFilled(p, ImVec2(p.x + a_w, p.y + h), IM_COL32(0, 0, 0, 51), sk.rounding);
        if (frac > 0.0f) {
            dl->AddRectFilled(p, ImVec2(p.x + a_w * frac, p.y + h),
                GaugeFill(), sk.rounding);
        }
        dl->AddRect(p, ImVec2(p.x + a_w, p.y + h), GaugeBorder(), sk.rounding);
        PushChromeStyle(true);
        ImGui::SetNextItemWidth(a_w);
        const bool ch = ImGui::SliderFloat(a_id, a_v, a_min, a_max, a_fmt,
            ImGuiSliderFlags_AlwaysClamp);
        PopChromeStyle(true);
        return ch;
    }

    void PushChromeStyle(bool a_sliderGrab)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // chrome owns the border
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1, 1, 1, 0.04f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        if (a_sliderGrab) {
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, S().hi);
    }

    void PopChromeStyle(bool a_sliderGrab)
    {
        ImGui::PopStyleColor(a_sliderGrab ? 6 : 4);
        ImGui::PopStyleVar();
    }

    void CornerFadeEdges(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max,
                         ImU32 a_top, ImU32 a_left, ImU32 a_right, ImU32 a_bottom,
                         float a_frac)
    {
        const auto t = [](ImU32 a_c) { return a_c & 0x00FFFFFFu; };   // alpha 0
        const float fw = (a_max.x - a_min.x) * a_frac;
        const float fh = (a_max.y - a_min.y) * a_frac;
        // top
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_min.y),
            ImVec2(a_min.x + fw, a_min.y + 1.0f), a_top, t(a_top), t(a_top), a_top);
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - fw, a_min.y),
            ImVec2(a_max.x, a_min.y + 1.0f), t(a_top), a_top, a_top, t(a_top));
        // bottom
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_max.y - 1.0f),
            ImVec2(a_min.x + fw, a_max.y), a_bottom, t(a_bottom), t(a_bottom), a_bottom);
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - fw, a_max.y - 1.0f),
            ImVec2(a_max.x, a_max.y), t(a_bottom), a_bottom, a_bottom, t(a_bottom));
        // verticals inset 1px so the corner pixel never double-blends (v10.3)
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_min.y + 1.0f),
            ImVec2(a_min.x + 1.0f, a_min.y + 1.0f + fh), a_left, a_left, t(a_left), t(a_left));
        a_dl->AddRectFilledMultiColor(ImVec2(a_min.x, a_max.y - 1.0f - fh),
            ImVec2(a_min.x + 1.0f, a_max.y - 1.0f), t(a_left), t(a_left), a_left, a_left);
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - 1.0f, a_min.y + 1.0f),
            ImVec2(a_max.x, a_min.y + 1.0f + fh), a_right, a_right, t(a_right), t(a_right));
        a_dl->AddRectFilledMultiColor(ImVec2(a_max.x - 1.0f, a_max.y - 1.0f - fh),
            ImVec2(a_max.x, a_max.y - 1.0f), t(a_right), t(a_right), a_right, a_right);
    }

    void CornerFade(ImDrawList* a_dl, ImVec2 a_min, ImVec2 a_max, ImU32 a_col, float a_frac)
    {
        CornerFadeEdges(a_dl, a_min, a_max, a_col, a_col, a_col, a_col, a_frac);
    }

    void NineSlice(ImDrawList* a_dl, void* a_tex, ImVec2 a_min, ImVec2 a_max,
                   float a_margin, float a_um, float a_vm)
    {
        if (!a_tex) return;
        const auto tex = reinterpret_cast<ImTextureID>(a_tex);
        // clamp corner size so tiny windows don't overlap their own corners
        const float mx = (std::min)(a_margin, (a_max.x - a_min.x) * 0.5f);
        const float my = (std::min)(a_margin, (a_max.y - a_min.y) * 0.5f);
        const float x0 = a_min.x, x1 = a_min.x + mx, x2 = a_max.x - mx, x3 = a_max.x;
        const float y0 = a_min.y, y1 = a_min.y + my, y2 = a_max.y - my, y3 = a_max.y;
        const float u0 = 0.0f, u1 = a_um, u2 = 1.0f - a_um, u3 = 1.0f;
        const float v0 = 0.0f, v1 = a_vm, v2 = 1.0f - a_vm, v3 = 1.0f;
        auto q = [&](float dx0, float dy0, float dx1, float dy1,
                     float su0, float sv0, float su1, float sv1) {
            a_dl->AddImage(tex, ImVec2(dx0, dy0), ImVec2(dx1, dy1),
                ImVec2(su0, sv0), ImVec2(su1, sv1));
        };
        q(x0, y0, x1, y1, u0, v0, u1, v1);   // TL
        q(x1, y0, x2, y1, u1, v0, u2, v1);   // T
        q(x2, y0, x3, y1, u2, v0, u3, v1);   // TR
        q(x0, y1, x1, y2, u0, v1, u1, v2);   // L
        q(x1, y1, x2, y2, u1, v1, u2, v2);   // C
        q(x2, y1, x3, y2, u2, v1, u3, v2);   // R
        q(x0, y2, x1, y3, u0, v2, u1, v3);   // BL
        q(x1, y2, x2, y3, u1, v2, u2, v3);   // B
        q(x2, y2, x3, y3, u2, v2, u3, v3);   // BR
    }

    void Apply()
    {
        const Skin& sk = S();
        auto& style = ImGui::GetStyle();
        style.WindowRounding    = sk.rounding;
        style.ChildRounding     = sk.rounding;
        style.FrameRounding     = sk.rounding;
        style.PopupRounding     = sk.rounding;
        style.GrabRounding      = sk.rounding;
        style.TabRounding       = sk.rounding;
        // cornerFade / tornFrame replace the full window border — kill the
        // geometry, not just the colour (decisive regardless of style state)
        style.WindowBorderSize  = (sk.cornerFade || sk.tornFrame || sk.bevelChrome) ? 0.0f : 1.0f;
        // mockup: every field/button/checkbox carries a visible border —
        // without it they read as invisible black boxes (v10.5 feedback)
        style.FrameBorderSize   = 1.0f;
        style.WindowPadding     = ImVec2(12.0f, 8.0f);
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);

        auto mix = [&](float a) { ImVec4 c = sk.acc; c.w = a; return c; };
        auto* c = style.Colors;
        ImVec4 win = sk.winBg;
        // opaque: the parked capture model hides behind windows. translucent
        // skins opt out — their park point is covered by the caching card.
        if (!sk.translucent) win.w = 1.0f;
        // tornFrame: the 9-slice texture paints the (opaque) fill instead, so
        // the ImGui bg rect must be transparent or it squares off the tears
        c[ImGuiCol_WindowBg]         = sk.tornFrame ? ImVec4(0, 0, 0, 0) : win;
        // transparent: children must not paint over the window's frame chrome
        c[ImGuiCol_ChildBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        c[ImGuiCol_PopupBg]          = win;
        // Border colours FRAMES (fields/buttons/checkbox — mockup .field/.abtn);
        // the skin-2 WINDOW border is killed via WindowBorderSize = 0 instead
        c[ImGuiCol_Border]           = mix(0.40f);
        c[ImGuiCol_Text]             = sk.ink;
        c[ImGuiCol_TextDisabled]     = sk.inkDim;
        c[ImGuiCol_FrameBg]          = ImVec4(0, 0, 0, 0.25f);
        c[ImGuiCol_FrameBgHovered]   = mix(0.12f);
        c[ImGuiCol_FrameBgActive]    = mix(0.18f);
        c[ImGuiCol_Button]           = mix(0.10f);
        c[ImGuiCol_ButtonHovered]    = mix(0.22f);
        c[ImGuiCol_ButtonActive]     = mix(0.30f);
        c[ImGuiCol_Header]           = mix(0.16f);
        c[ImGuiCol_HeaderHovered]    = mix(0.22f);
        c[ImGuiCol_HeaderActive]     = mix(0.28f);
        c[ImGuiCol_SliderGrab]       = mix(0.45f);
        c[ImGuiCol_SliderGrabActive] = sk.hi;
        c[ImGuiCol_CheckMark]        = sk.hi;
        c[ImGuiCol_Separator]        = mix(0.25f);
        c[ImGuiCol_Tab]              = mix(0.06f);
        c[ImGuiCol_TabHovered]       = mix(0.20f);
        c[ImGuiCol_TabSelected]      = mix(0.16f);
        c[ImGuiCol_TitleBg]          = win;
        c[ImGuiCol_TitleBgActive]    = win;
        c[ImGuiCol_ScrollbarBg]      = win;
        c[ImGuiCol_ScrollbarGrab]    = mix(0.25f);

        // bevelChrome (kept for future skins): dark translucent beveled
        // buttons/fields so white ink reads — acc-tinted fills read flat on
        // a translucent grey ground
        if (sk.bevelChrome) {
            c[ImGuiCol_Button]         = ImVec4(0.32f, 0.32f, 0.34f, 0.85f);
            c[ImGuiCol_ButtonHovered]  = ImVec4(0.42f, 0.42f, 0.44f, 0.90f);
            c[ImGuiCol_ButtonActive]   = ImVec4(0.26f, 0.26f, 0.28f, 0.92f);
            c[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.16f, 0.18f, 0.45f);
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.24f, 0.55f);
            c[ImGuiCol_FrameBgActive]  = ImVec4(0.28f, 0.28f, 0.30f, 0.65f);
            c[ImGuiCol_Border]         = ImVec4(0.24f, 0.24f, 0.26f, 0.85f);
            c[ImGuiCol_PopupBg]        = ImVec4(win.x, win.y, win.z, 0.92f);
        }
    }
}
