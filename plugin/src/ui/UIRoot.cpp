#include "ui/UIRoot.h"
#include "ui/Editor.h"
#include "ui/Equip.h"
#include "game/GoldCoins.h"
#include "ui/Loadout.h"
#include "ui/GridMenu.h"
#include "ui/Grid.h"
#include "ui/LootBarter.h"
#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/WinManager.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>   // ClearActiveID (drop text-field focus on close)

#include <d3d11.h>
#include <filesystem>

// imgui_impl_win32.h leaves this for the app to declare (per its docs)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ImGui bootstrap/render-loop structure ported from ModExplorerMenu (Modex)
// by patchulidev — UIManager.cpp (GPL-3.0 with Modding Exception).
// Visuals implement the v9 mockup (design contract).

namespace FUI::UIRoot
{
    namespace
    {
        std::atomic<bool>     g_initialized = false;
        std::function<void()> g_onShow;
        std::function<void()> g_onHide;

        ImVec2          g_scrollEnergy = ImVec2(0.0f, 0.0f);
        constexpr float kScrollMultiplier = 1.5f;
        constexpr float kScrollSmoothing  = 10.0f;

        ImFont* g_fontMain = nullptr;
        ID3D11ShaderResourceView* g_glowSRV = nullptr;   // radial falloff (rarity glow)

        // icon-brightness UP pass: additive blend state (src*a + dst). A tint
        // can only darken, so gains above 1.0 draw the icon a second time
        // additively — live, no texture rebake.
        ID3D11BlendState* g_addBlend = nullptr;

        void AdditiveBlendCB(const ImDrawList*, const ImDrawCmd*)
        {
            auto* data = RE::BSGraphics::Renderer::GetRendererData();
            auto* ctx = data ? reinterpret_cast<ID3D11DeviceContext*>(data->context) : nullptr;
            if (ctx && g_addBlend) {
                const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                ctx->OMSetBlendState(g_addBlend, bf, 0xFFFFFFFF);
            }
        }

        void CreateAdditiveBlend(ID3D11Device* a_device)
        {
            if (g_addBlend) return;
            D3D11_BLEND_DESC bd = {};
            bd.RenderTarget[0].BlendEnable           = TRUE;
            bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend             = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ONE;
            bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            a_device->CreateBlendState(&bd, &g_addBlend);
        }
        IconCache::Icon g_tornGlowA;   // torn 9-slice panel, thin cream rim (skin 3)
        IconCache::Icon g_tornGlowB;   // torn 9-slice panel, soft cream halo (skin 4)
        IconCache::Icon g_tornCreamA;  // V1 cream rebake (skin 5)
        IconCache::Icon g_tornBrightB; // V2 bright rebake (skin 6)

        // B11: written from the render path, read from the message-queue path
        // — both are the main thread today, but atomics cost nothing and the
        // assumption is now explicit
        std::atomic<bool> g_showSettings = false;
        std::atomic<bool> g_textInputOn = false;   // ImGui WantTextInput mirror (no engine calls)

        // ---- INSPECT overlay (C key) ----
        // The rotation is euler, exactly like a def, so the whole capture path
        // (key hash, pin recycling, "rotation applied" gate) works unchanged:
        // horizontal drag -> RZ (screen-vertical axis), vertical -> RX,
        // Shift+drag -> RY. Gimbal-ish at extreme angles, but predictable and
        // it maps 1:1 onto what the EDIT sliders write.
        RE::TESBoundObject* g_inspObj = nullptr;
        std::string         g_inspKey;
        float g_inspRx = 0.0f, g_inspRy = 0.0f, g_inspRz = 0.0f;
        float g_inspRx0 = 0.0f, g_inspRy0 = 0.0f, g_inspRz0 = 0.0f;   // R resets here
        float g_inspZoom = 1.0f;
        // Grid's C press and the overlay's C toggle run in the SAME ImGui frame
        // (grid draws first) — without this the view would open and shut at once
        int   g_inspOpenFrame = -1;
        bool  g_inspDrag = false;

        // The movie-less menu never receives GFxCharEvents (same reason the
        // raw I-key needed InputSink), so keyboard/char input is taken
        // straight off the game window — chained WndProc into the ImGui
        // Win32 backend (Modex-style). Mouse stays on the Scaleform relay.
        WNDPROC g_origWndProc = nullptr;

        LRESULT CALLBACK WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            switch (m) {
            case WM_CHAR:
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                if (auto* ui = RE::UI::GetSingleton();
                    ui && ui->IsMenuOpen("GridInventoryMenu"sv) && ImGui::GetCurrentContext()) {
                    ImGui_ImplWin32_WndProcHandler(h, m, w, l);
                }
                break;
            default:
                break;
            }
            return CallWindowProcA(g_origWndProc, h, m, w, l);
        }

        // fonts are BAKED at the current UI scale (bitmap-scaling hangul via
        // FontGlobalScale mushes the strokes). While the slider drags we
        // preview via FontGlobalScale ratio; on release the atlas rebakes.
        std::atomic<bool> g_fontsDirty = false;
        float             g_bakedScale = 1.0f;

        // ICON CACHE reset request (settings, two-click armed). Consumed in
        // Tick — SRVs must never be released inside the ImGui frame.
        std::atomic<bool> g_iconsReset = false;
        // GI47: a preset icon bundle waits to be merged (frame-outside).
        // The pak path rides in g_presetMergePak (same-thread handoff).
        std::atomic<bool> g_iconsMergePreset = false;
        std::string       g_presetMergePak;

        // (re)build the font atlas at 17px * UI scale. Call OUTSIDE the
        // NewFrame/Render pair only.
        void BuildFonts()
        {
            auto& io = ImGui::GetIO();
            const float k = Theme::Scale();

            static ImVector<ImWchar> mainRanges;
            if (mainRanges.empty()) {
                ImFontGlyphRangesBuilder b;
                b.AddRanges(io.Fonts->GetGlyphRangesKorean());
                // gear, pencil, degree, em-dash, middot, ▾, ×, ∞ (F3 merchant gold)
                b.AddText("\xE2\x9A\x99\xE2\x9C\x8E\xC2\xB0\xE2\x80\x94\xC2\xB7\xE2\x96\xBE\xC3\x97\xE2\x88\x9E");
                b.BuildRanges(&mainRanges);
            }

            auto exists = [](const char* p) { return std::filesystem::exists(p); };
            const char* kMalgun = "C:\\Windows\\Fonts\\malgun.ttf";
            const char* kYaHei  = "C:\\Windows\\Fonts\\msyh.ttc";
            const char* kMeiryo = "C:\\Windows\\Fonts\\meiryo.ttc";
            const char* kYuGoth = "C:\\Windows\\Fonts\\YuGothM.ttc";

            io.Fonts->Clear();
            g_fontMain = nullptr;

            if (exists(kMalgun)) {
                ImFontConfig base;
                base.OversampleH = 2;
                base.OversampleV = 2;   // default V=1 leaves dense hangul strokes rough
                g_fontMain = io.Fonts->AddFontFromFileTTF(kMalgun, 17.0f * k, &base, mainRanges.Data);
                ImFontConfig mc;
                mc.MergeMode = true;
                mc.OversampleH = 2;
                mc.OversampleV = 2;
                if (exists(kYaHei)) {
                    io.Fonts->AddFontFromFileTTF(kYaHei, 17.0f * k, &mc,
                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                }
                const char* jp = exists(kMeiryo) ? kMeiryo : (exists(kYuGoth) ? kYuGoth : nullptr);
                if (jp) {
                    io.Fonts->AddFontFromFileTTF(jp, 17.0f * k, &mc, io.Fonts->GetGlyphRangesJapanese());
                }
                // malgun has no U+2699 (gear) / U+270E (pencil) — Segoe UI Symbol
                // supplies them (merge skips codepoints malgun already covers)
                const char* kSegSym = "C:\\Windows\\Fonts\\seguisym.ttf";
                static const ImWchar symRanges[] = { 0x2010, 0x2BFF, 0 };
                if (exists(kSegSym)) {
                    io.Fonts->AddFontFromFileTTF(kSegSym, 17.0f * k, &mc, symRanges);
                }
            } else {
                g_fontMain = io.Fonts->AddFontDefault();
            }

            io.Fonts->Build();
            ImGui_ImplDX11_InvalidateDeviceObjects();   // font texture recreates on NewFrame
            g_bakedScale = k;
        }

        void MouseHandler()
        {
            if (auto* ui = RE::UI::GetSingleton()) {
                POINT cursorPos;
                if (ui->IsMenuOpen(RE::CursorMenu::MENU_NAME)) {
                    const auto* menuCursor = RE::MenuCursor::GetSingleton();
                    ImGui::GetIO().AddMouseSourceEvent(ImGuiMouseSource_Mouse);
                    ImGui::GetIO().AddMousePosEvent(menuCursor->cursorPosX, menuCursor->cursorPosY);
                } else if (GetCursorPos(&cursorPos) != FALSE) {
                    ImGui::GetIO().AddMousePosEvent(
                        static_cast<float>(cursorPos.x), static_cast<float>(cursorPos.y));
                }
            }
        }

        void ScrollHandler()
        {
            auto& io = ImGui::GetIO();
            ImVec2 now = ImVec2(0.0f, 0.0f);

            if (std::abs(g_scrollEnergy.x) > 0.01f) {
                now.x = g_scrollEnergy.x * io.DeltaTime * kScrollSmoothing;
                g_scrollEnergy.x -= now.x;
            } else {
                g_scrollEnergy.x = 0.0f;
            }
            if (std::abs(g_scrollEnergy.y) > 0.01f) {
                now.y = g_scrollEnergy.y * io.DeltaTime * kScrollSmoothing;
                g_scrollEnergy.y -= now.y;
            } else {
                g_scrollEnergy.y = 0.0f;
            }

            io.MouseWheel  = now.y;
            io.MouseWheelH = -now.x;
        }

        // Smooth radial gradient (white, alpha falloff) — the rarity glow
        // stretches this over the item's footprint. A real per-pixel falloff:
        // no banding, and elongated items get an elliptical halo for free.
        void CreateGlowTexture(ID3D11Device* a_device)
        {
            constexpr int N = 128;
            std::vector<std::uint8_t> px(N * N * 4);
            for (int y = 0; y < N; ++y) {
                for (int x = 0; x < N; ++x) {
                    const float dx = (x + 0.5f) / N - 0.5f;
                    const float dy = (y + 0.5f) / N - 0.5f;
                    const float d = (std::min)(1.0f, std::sqrt(dx * dx + dy * dy) * 2.0f);
                    const float a = std::pow(1.0f - d, 1.6f);   // soft shoulder
                    const size_t i = (static_cast<size_t>(y) * N + x) * 4;
                    px[i + 0] = 255;
                    px[i + 1] = 255;
                    px[i + 2] = 255;
                    px[i + 3] = static_cast<std::uint8_t>(a * 255.0f + 0.5f);
                }
            }
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = N; td.Height = N; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init = { px.data(), N * 4, 0 };
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(a_device->CreateTexture2D(&td, &init, &tex))) {
                a_device->CreateShaderResourceView(tex, nullptr, &g_glowSRV);
                tex->Release();
            }
        }

        // ---- SETTINGS window (⚙): scale + skin swatches + language ----
        // ---- Phase 3: settings rows as a table ----
        // One row = a function drawing "dim label at the shared column +
        // control"; DrawSettingsWindow just walks kSettingsRows with the
        // standard gap. Adding an option (F3/F4 merchant toggles etc.) =
        // one function + one table entry; F5's sectioning builds on this.
        struct SettingsCtx
        {
            float padLabelW;   // label column width (child-local since F5)
            float trackW;      // slider track width
            float S;           // UI scale
        };

        void SettingLabel(const SettingsCtx& a_c, Lang::Str a_label)
        {
            ImGui::TextColored(Theme::S().inkDim, "%s", Lang::T(a_label));
            ImGui::SameLine(a_c.padLabelW);
        }

        // SCALE — mockup track: black .2 bg, acc .20 fill, centred value.
        // Pending-apply: while dragging only this local value moves — live
        // per-frame resizing of every managed window read as a ghosted /
        // doubled image (user-reported), and the font rebake was deferred to
        // release anyway. Scale, save and rebake all land on release.
        void RowScale(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::ScaleLabel);
            static float s_pending = -1.0f;
            float sc = s_pending > 0.0f ? s_pending : Theme::Scale();
            if (Theme::ChromeSliderFloat("##uiscale", &sc, 0.5f, 1.6f, a_c.trackW)) {
                s_pending = sc;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (s_pending > 0.0f) {
                    Theme::SetScale(s_pending);
                    s_pending = -1.0f;
                }
                WinManager::GetSingleton()->Save();
                g_fontsDirty.store(true);   // rebake the atlas at the new scale
            } else if (!ImGui::IsItemActive()) {
                s_pending = -1.0f;   // stale pending (menu closed mid-drag)
            }
        }

        // SKIN — mockup swatch colours (representative, not raw winBg)
        void RowSkin(const SettingsCtx& a_c)
        {
            const auto& sk = Theme::S();
            auto* dl = ImGui::GetWindowDrawList();
            struct SwCol { ImU32 fill; ImU32 inner; };
            static constexpr SwCol kSw[6] = {
                { IM_COL32(0xD8, 0xB8, 0x78, 255), 0 },                                  // amber
                { IM_COL32(0x0B, 0x0A, 0x09, 255), IM_COL32(0xA8, 0x40, 0x2F, 255) },    // black+crimson
                { IM_COL32(0x1A, 0x18, 0x16, 255), IM_COL32(0xD8, 0xB8, 0x78, 255) },    // amber torn
                { IM_COL32(0x1A, 0x18, 0x16, 255), IM_COL32(0xA8, 0x40, 0x2F, 255) },    // oathvein torn
                { IM_COL32(0x14, 0x14, 0x16, 255), IM_COL32(0x86, 0x26, 0x1C, 255) },    // quickloot dark
                { IM_COL32(0x3A, 0x3A, 0x40, 255), IM_COL32(0xD4, 0xD4, 0xD8, 255) },    // quickloot glass
            };
            SettingLabel(a_c, Lang::Str::SkinLabel);
            for (int i = 1; i <= Theme::SkinCount(); ++i) {
                ImGui::PushID(i);
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float side = 24.0f * a_c.S;
                ImGui::InvisibleButton("##skin", ImVec2(side, side));
                const ImVec2 p1(p0.x + side, p0.y + side);
                dl->AddRectFilled(p0, p1, kSw[i - 1].fill, 4.0f);
                if (kSw[i - 1].inner) {
                    dl->AddRect(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p1.y - 1),
                        kSw[i - 1].inner, 3.0f);
                }
                dl->AddRect(p0, p1, Theme::Acc(0.4f), 4.0f);
                if (Theme::SkinIndex() == i) {
                    dl->AddRect(ImVec2(p0.x - 2, p0.y - 2), ImVec2(p1.x + 2, p1.y + 2),
                        Theme::Col(sk.hi, 1.0f), 5.0f, 0, 2.0f);
                }
                if (ImGui::IsItemClicked()) {
                    Theme::SetSkin(i);
                    WinManager::GetSingleton()->Save();
                }
                ImGui::PopID();
                if (i < Theme::SkinCount()) ImGui::SameLine(0.0f, 10.0f * a_c.S);
            }
        }

        // language chip labels — shared by the width budget and the row
        constexpr const char* kLangChips[4] = { "EN", "한국어", "中文", "日本語" };

        // LANGUAGE — padded chips (image-3 spacing)
        void RowLanguage(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::LanguageLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            for (int i = 0; i < 4; ++i) {
                const bool on = Lang::Get() == i;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Acc(0.28f));
                if (Sfx::Button(kLangChips[i])) {
                    Lang::SetLang(i);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor();
                if (i < 3) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
        }

        // PRESET (GI46-48): the dropdown lists every GridInventory_<name>.ini
        // beside the plugin (Default, P1, P2, ...); Import applies the picked
        // one on the spot. The next row exports under a chosen name.
        void RowPreset(const SettingsCtx& a_c)
        {
            static std::vector<std::string> s_list;
            static int  s_sel = -1;
            static bool s_wasOpen = false;

            SettingLabel(a_c, Lang::Str::PresetLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            ImGui::SetNextItemWidth(140.0f * a_c.S);
            const bool picked = s_sel >= 0 && s_sel < static_cast<int>(s_list.size());
            const bool open = ImGui::BeginCombo("##presetpick",
                picked ? s_list[s_sel].c_str() : "...");
            if (open) {
                if (!s_wasOpen) {   // rescan the folder as the combo drops down
                    const std::string keep = picked ? s_list[s_sel] : "";
                    s_list = WinManager::GetSingleton()->ListPresets();
                    s_sel = -1;
                    for (int i = 0; i < static_cast<int>(s_list.size()); ++i) {
                        if (s_list[i] == keep) { s_sel = i; break; }
                    }
                }
                for (int i = 0; i < static_cast<int>(s_list.size()); ++i) {
                    if (ImGui::Selectable(s_list[i].c_str(), s_sel == i)) s_sel = i;
                }
                ImGui::EndCombo();
            }
            s_wasOpen = open;
            ImGui::SameLine(0.0f, 6.0f * a_c.S);
            if (Sfx::Button(Lang::T(Lang::Str::PresetImport))) {
                if (s_sel >= 0 && s_sel < static_cast<int>(s_list.size()) &&
                    WinManager::GetSingleton()->ImportPreset(s_list[s_sel])) {
                    WinManager::GetSingleton()->Save();   // persist into the ui ini
                    g_fontsDirty.store(true);             // scale may have changed
                    g_presetMergePak = WinManager::PresetPakPath(s_list[s_sel]);
                    g_iconsMergePreset.store(true);       // GI47: icons on the Tick
                } else {
                    Sfx::FailNote(Lang::T(Lang::Str::PresetMissing));
                }
            }
            ImGui::PopStyleVar();
        }

        // EXPORT row: preset name ("Default" when left blank) + save button
        void RowPresetExport(const SettingsCtx& a_c)
        {
            static char s_name[64] = {};
            SettingLabel(a_c, Lang::Str::PresetExport);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            ImGui::SetNextItemWidth(140.0f * a_c.S);
            ImGui::InputTextWithHint("##presetname", "Default", s_name, sizeof(s_name));
            ImGui::SameLine(0.0f, 6.0f * a_c.S);
            if (Sfx::Button(Lang::T(Lang::Str::Save))) {
                WinManager::GetSingleton()->ExportPreset(s_name[0] ? s_name : "Default");
            }
            ImGui::PopStyleVar();
        }

        // GLOW — rarity glow style chips (silhouette=1 / radial=0)
        // ICON STYLE — realistic auto-captures vs the tool-authored low-poly
        // pak (uncovered low-poly items fall back to realistic)
        void RowIconStyle(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::IconStyleLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            auto* icons = IconCache::GetSingleton();
            for (int style : { 0, 1 }) {
                const bool on = icons->LowPolyStyle() == (style == 1);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Acc(0.28f));
                ImGui::PushID(style);
                if (Sfx::Button(Lang::T(style == 1 ? Lang::Str::StyleLowPoly
                                                   : Lang::Str::StyleRealistic))) {
                    icons->SetLowPolyStyle(style == 1);
                    WinManager::GetSingleton()->Save();
                }
                ImGui::PopID();
                if (on) ImGui::PopStyleColor();
                if (style == 0) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
        }

        void RowGlowStyle(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::GlowLabel);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            for (int style : { 1, 0 }) {
                const bool on = Theme::GlowStyle() == style;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Acc(0.28f));
                if (Sfx::Button(Lang::T(style == 1 ? Lang::Str::GlowSilhouette
                                                   : Lang::Str::GlowRadial))) {
                    Theme::SetGlowStyle(style);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor();
                if (style == 1) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
        }

        // GLOW LEVEL — brightness multiplier, same track chrome as SCALE
        void RowGlowGain(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::GlowBrightLabel);
            float gg = Theme::GlowGain();
            if (Theme::ChromeSliderFloat("##glowgain", &gg, 0.2f, 2.5f, a_c.trackW)) {
                Theme::SetGlowGain(gg);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WinManager::GetSingleton()->Save();
            }
        }

        // ICON LIGHT — item icon brightness, LIVE: <=1 darkens via tint,
        // >1 brightens via the additive pass (see DrawItemIcon)
        void RowIconGain(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::IconBrightLabel);
            float ig = Theme::IconGain();
            if (Theme::ChromeSliderFloat("##icongain", &ig, 0.4f, 1.6f, a_c.trackW)) {
                Theme::SetIconGain(ig);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WinManager::GetSingleton()->Save();
            }
        }

        // ICON CACHE — manual reset for retexture installs: every icon
        // re-renders from the CURRENT meshes/textures. Two-click armed
        // (3s window) so a stray click can't wipe the cache.
        void RowCacheReset(const SettingsCtx& a_c)
        {
            const auto& sk = Theme::S();
            SettingLabel(a_c, Lang::Str::CacheLabel);
            static double s_armedUntil = 0.0;
            const bool armed = ImGui::GetTime() < s_armedUntil;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            if (armed) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col(sk.sel, 0.55f));
            if (Sfx::Button(Lang::T(armed ? Lang::Str::Confirm : Lang::Str::CacheReset))) {
                if (armed) {
                    g_iconsReset.store(true);
                    s_armedUntil = 0.0;
                } else {
                    s_armedUntil = ImGui::GetTime() + 3.0;
                }
            }
            if (armed) ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // PRECACHE (C): one-shot batch capture of every inventory form in
        // the load order. Runs through the normal queue while the menu stays
        // open; already-on-disk items are skipped for free, captures land in
        // the pak only (VRAM stays flat). Click again to cancel; visible
        // items re-queue themselves as usual.
        void RowPrecache(const SettingsCtx& a_c)
        {
            const auto& sk = Theme::S();
            SettingLabel(a_c, Lang::Str::PrecacheLabel);
            static bool s_precacheOn = false;
            auto* cache = IconCache::GetSingleton();
            const size_t q = cache->QueuedCount();
            if (s_precacheOn && q == 0) s_precacheOn = false;   // drained
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            if (!s_precacheOn) {
                if (Sfx::Button(Lang::T(Lang::Str::PrecacheStart))) {
                    s_precacheOn = cache->PrecacheAll() > 0;
                }
            } else {
                char lbl[64];
                std::snprintf(lbl, sizeof(lbl), "%s (%zu)",
                    Lang::T(Lang::Str::Cancel), q);
                ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col(sk.sel, 0.55f));
                if (Sfx::Button(lbl, ImVec2(0, 0), true)) {   // cancel
                    cache->CancelPrecache();
                    s_precacheOn = false;
                }
                ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar();
        }

        // F3 — MERCHANT GOLD: Default / Unlimited chips (GlowStyle grammar).
        // PushID: both trade rows share the "Default" chip label.
        void RowMerchantGold(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::MerchGoldSetLabel);
            ImGui::PushID("merchgold");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            for (int inf : { 0, 1 }) {
                const bool on = LootBarter::MerchantGoldInfinite() == (inf == 1);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Acc(0.28f));
                if (Sfx::Button(Lang::T(inf ? Lang::Str::ToggleUnlimited
                                            : Lang::Str::ToggleDefault))) {
                    LootBarter::SetMerchantGoldInfinite(inf == 1);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor();
                if (inf == 0) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
        }

        // F4 — MERCHANT BUYS: Default / Anything chips. The stolen-goods rule
        // stays either way (fence-only), only the category list is lifted.
        void RowMerchantStock(const SettingsCtx& a_c)
        {
            SettingLabel(a_c, Lang::Str::MerchStockSetLabel);
            ImGui::PushID("merchstock");
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * a_c.S, 3.0f * a_c.S));
            for (int all : { 0, 1 }) {
                const bool on = LootBarter::MerchantBuysAll() == (all == 1);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Acc(0.28f));
                if (Sfx::Button(Lang::T(all ? Lang::Str::ToggleAnything
                                            : Lang::Str::ToggleDefault))) {
                    LootBarter::SetMerchantBuysAll(all == 1);
                    WinManager::GetSingleton()->Save();
                }
                if (on) ImGui::PopStyleColor();
                if (all == 0) ImGui::SameLine(0.0f, 6.0f * a_c.S);
            }
            ImGui::PopStyleVar();
            ImGui::PopID();
        }

        using SettingsRowFn = void (*)(const SettingsCtx&);

        // F5: rows grouped into titled sections (GENERAL / DISPLAY / TRADE /
        // ICONS). Adding an option = one row function + one entry here.
        struct SettingsSection
        {
            Lang::Str            title;
            const SettingsRowFn* rows;
            size_t               count;
        };
        constexpr SettingsRowFn kRowsGeneral[] = { RowScale, RowSkin, RowLanguage, RowPreset, RowPresetExport };
        constexpr SettingsRowFn kRowsDisplay[] = { RowIconStyle, RowGlowStyle, RowGlowGain, RowIconGain };
        constexpr SettingsRowFn kRowsTrade[]   = { RowMerchantGold, RowMerchantStock };
        constexpr SettingsRowFn kRowsIcons[]   = { RowCacheReset, RowPrecache };
        constexpr SettingsSection kSettingsSections[] = {
            { Lang::Str::SectionGeneral, kRowsGeneral, std::size(kRowsGeneral) },
            { Lang::Str::SectionDisplay, kRowsDisplay, std::size(kRowsDisplay) },
            { Lang::Str::SectionTrade,   kRowsTrade,   std::size(kRowsTrade) },
            { Lang::Str::SectionIcons,   kRowsIcons,   std::size(kRowsIcons) },
        };

        // section title + thin rule running to the right edge
        void SettingsSectionHeader(const SettingsCtx& a_c, Lang::Str a_title)
        {
            const char* txt = Lang::T(a_title);
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddLine(ImVec2(p.x + ts.x + 10.0f * a_c.S, p.y + ts.y * 0.55f),
                ImVec2(p.x + availW, p.y + ts.y * 0.55f), Theme::Acc(0.22f));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Acc(0.85f)), "%s", txt);
            ImGui::Dummy(ImVec2(0.0f, 6.0f * a_c.S));
        }

        void DrawSettingsWindow()
        {
            if (!g_showSettings) return;

            auto* wm = WinManager::GetSingleton();
            const float S = Theme::Scale();
            // label column sized to the WIDEST label (e.g. "LANGUAGE") so the
            // value column never overlaps it in any language; generous
            // label->control gap (32px) with a floor so short labels (KO
            // "크기") don't collapse the column
            const float labelW = (std::max)(84.0f * S, 32.0f * S + (std::max)({
                ImGui::CalcTextSize(Lang::T(Lang::Str::ScaleLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::SkinLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::LanguageLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::PresetLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::PresetExport)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::GlowLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::GlowBrightLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::IconBrightLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::CacheLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::PrecacheLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::MerchGoldSetLabel)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::MerchStockSetLabel)).x }));
            const float trackW = 176.0f * S;
            // language chips sized like image-3: real padding, so the window
            // width follows the actual row width (no dead right margin)
            float langW = 0.0f;
            for (int i = 0; i < 4; ++i) {
                langW += ImGui::CalcTextSize(kLangChips[i]).x + 16.0f * S;
                if (i < 3) langW += 6.0f * S;
            }
            const float swatchW = Theme::SkinCount() * 24.0f * S +
                                  (Theme::SkinCount() - 1) * 10.0f * S;
            // glow style chips (silhouette / radial), same chip metrics as langs
            const float glowW =
                ImGui::CalcTextSize(Lang::T(Lang::Str::GlowSilhouette)).x + 16.0f * S +
                6.0f * S +
                ImGui::CalcTextSize(Lang::T(Lang::Str::GlowRadial)).x + 16.0f * S;
            // trade chips (F3/F4): widest of the two Default/<on> pairs
            const float defW = ImGui::CalcTextSize(Lang::T(Lang::Str::ToggleDefault)).x;
            const float tradeW = defW + 16.0f * S + 6.0f * S + 16.0f * S + (std::max)(
                ImGui::CalcTextSize(Lang::T(Lang::Str::ToggleUnlimited)).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::ToggleAnything)).x);
            const float insX = Theme::FrameInsetX();
            const float insY = Theme::FrameInsetY();

            // F5: rows live in a scrollable child, so the label column is
            // CHILD-local (the window padding below already carries the
            // torn-frame inset — the old +insX label shift is baked in there).
            const float ctrlW = (std::max)({ trackW, langW, swatchW, glowW, tradeW });

            // height: measured content from the previous frame, clamped to the
            // screen — beyond the clamp the child scrolls (F5). First frame
            // falls back near the old fixed height; one frame later it snaps.
            static float s_wantH = 0.0f;   // desired full window height
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            const float maxH = disp.y - 80.0f * S;
            const bool clamped = s_wantH > 0.0f && s_wantH > maxH;
            const float winH = s_wantH > 0.0f ? (std::min)(s_wantH, maxH)
                                              : 440.0f * S + 2.0f * insY;
            const ImVec2 size(
                12.0f + insX + labelW + ctrlW + 12.0f + insX +
                    (clamped ? ImGui::GetStyle().ScrollbarSize : 0.0f),
                winH);
            ImVec2 defPos(200.0f, 200.0f);
            if (auto* mw = wm->Find("main")) {
                defPos = ImVec2(mw->pos.x + mw->size.x - size.x, mw->pos.y + 40.0f * S);
            }
            wm->ApplyNext("settings", defPos, size);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                ImVec2(12.0f + insX, 8.0f + insY));
            ImGui::Begin("##fablerim_settings", nullptr, kManagedWinFlags);
            NoteOverlayRect();
            wm->TitleBar("settings", Lang::T(Lang::Str::Settings));

            // EDIT-style lifetime (user request): stays open until the gear
            // toggle or ESC. The old click-outside-closes popup rule ALSO ate
            // titlebar grabs whenever another window overlapped (hover
            // resolved to the front window), so settings could never be
            // dragged freely.

            const SettingsCtx ctx{ labelW, trackW, S };
            const float childTop = ImGui::GetCursorPosY();
            ImGui::BeginChild("##settings_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                ImGuiWindowFlags_NoBackground);
            ImGui::Dummy(ImVec2(0.0f, 4.0f * S));
            for (size_t s = 0; s < std::size(kSettingsSections); ++s) {
                const auto& sec = kSettingsSections[s];
                if (s > 0) ImGui::Dummy(ImVec2(0.0f, 14.0f * S));
                SettingsSectionHeader(ctx, sec.title);
                for (size_t i = 0; i < sec.count; ++i) {
                    sec.rows[i](ctx);
                    if (i + 1 < sec.count) ImGui::Dummy(ImVec2(0.0f, 9.0f * S));
                }
            }
            const float bodyH = ImGui::GetCursorPosY() + 4.0f * S;   // bottom margin
            ImGui::EndChild();
            s_wantH = childTop + bodyH + 8.0f + insY;   // + bottom window padding

            ImGui::End();
            ImGui::PopStyleVar();   // WindowPadding (torn-frame inset)
        }

        // ---- S1: stats panel data ----
        // Displayed weapon damage via the engine routine the vanilla inventory
        // uses (skill/perk adjusted). Falls back to the unarmed AV bare-handed.
        float StatDamageValue()
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return 0.0f;
            auto* entry = p->GetEquippedEntryData(false);
            if (!entry) entry = p->GetEquippedEntryData(true);
            if (!entry) {
                return p->AsActorValueOwner()->GetActorValue(RE::ActorValue::kUnarmedDamage);
            }
            using func_t = float(RE::PlayerCharacter*, RE::InventoryEntryData*);
            static REL::Relocation<func_t> func{ RE::Offset::PlayerCharacter::GetDamage };
            return func(p, entry);
        }

        // total height of the stats block (bodyH reserves this under the doll)
        [[nodiscard]] float StatsPanelH() { return 112.0f * Theme::Scale(); }

        // S1/S2: [divider, armor/damage/speed/crit, divider, space] pinned
        // ABOVE the GOLD bar in the left column. Space turns crimson while the
        // grid is overloaded (W2) — no extra status line, per the final spec.
        void DrawStatsPanel(float a_leftW, float a_bodyH)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const float rowH = 19.0f * S;
            float y = cp.y + a_bodyH - 30.0f * S - StatsPanelH();

            auto row = [&](const char* a_label, const char* a_val, ImU32 a_valCol) {
                dl->AddText(ImVec2(cp.x + 2.0f, y), Theme::Col(sk.inkDim, 1.0f), a_label);
                const float w = ImGui::CalcTextSize(a_val).x;
                dl->AddText(ImVec2(cp.x + a_leftW - w - 2.0f, y), a_valCol, a_val);
                y += rowH;
            };

            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* avo = p ? p->AsActorValueOwner() : nullptr;
            const ImU32 hi = Theme::Col(sk.hi, 1.0f);
            char buf[48];

            dl->AddLine(ImVec2(cp.x, y), ImVec2(cp.x + a_leftW, y), Theme::Acc(0.25f));
            y += 7.0f * S;

            std::snprintf(buf, sizeof(buf), "%.0f", StatDamageValue());
            row(Lang::T(Lang::Str::StatDamage), buf, hi);

            std::snprintf(buf, sizeof(buf), "%.0f",
                avo ? avo->GetActorValue(RE::ActorValue::kDamageResist) : 0.0f);
            row(Lang::T(Lang::Str::StatArmor), buf, hi);

            float spd = avo ? avo->GetActorValue(RE::ActorValue::kWeaponSpeedMult) : 0.0f;
            if (spd <= 0.01f) spd = 1.0f;   // engine treats 0 as unmodified
            std::snprintf(buf, sizeof(buf), "%.2fx", spd);
            row(Lang::T(Lang::Str::StatSpeed), buf, hi);

            unsigned crit = 0;
            if (p) {
                if (auto* right = p->GetEquippedObject(false)) {
                    if (auto* weap = right->As<RE::TESObjectWEAP>()) {
                        crit = weap->GetCritDamage();
                    }
                }
            }
            std::snprintf(buf, sizeof(buf), "%u", crit);
            row(Lang::T(Lang::Str::StatCrit), buf, hi);

            y += 2.0f * S;
            dl->AddLine(ImVec2(cp.x, y), ImVec2(cp.x + a_leftW, y), Theme::Acc(0.25f));
            y += 7.0f * S;

            const bool over = Grid::IsOverloaded();
            std::snprintf(buf, sizeof(buf), "%d / %d", Grid::SpaceUsed(), Grid::SpaceTotal());
            row(Lang::T(Lang::Str::StatSpace), buf,
                over ? IM_COL32(204, 81, 72, 255) : hi);
        }

        // Main inventory window (v9): [tabs + equip doll + GOLD] | [ITEMS + grid]
        // ---- Phase 3: DrawMainWindow helpers (bodies moved verbatim) ----

        // titlebar text button (v10.6): dim tracked text, hover brightens,
        // active = hi + underline (fade on skin 2). Fires on RELEASE — a
        // press-time toggle opened the settings popup and the same click's
        // outside-close check shut it in the very same frame.
        // a_fontMul: draw the label above the shared baseline size (the ✕
        // close glyph was unreadably small at 1.0 — user feedback).
        // a_hitPad: grow the INVISIBLE hitbox around the glyph on every side
        // (the ✕ was readable but needed pixel-perfect aim — user feedback).
        bool TitleBarTextButton(float a_x, float a_ty, const char* a_lbl, float a_w, bool a_on,
                                float a_fontMul = 1.0f, float a_hitPad = 0.0f)
        {
            const auto& sk = Theme::S();
            const float lh = ImGui::GetTextLineHeight() * a_fontMul;
            ImGui::SetCursorScreenPos(ImVec2(a_x - a_hitPad, a_ty - 2.0f - a_hitPad));
            const bool pressed = ImGui::InvisibleButton(a_lbl,
                ImVec2(a_w + 2.0f * a_hitPad, lh + 7.0f + 2.0f * a_hitPad));
            const bool hov = ImGui::IsItemHovered();
            if (hov) Sfx::HoverNote(ImGui::GetItemID());
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec4 col = a_on ? sk.hi : (hov ? sk.ink : sk.inkDim);
            // scaled glyphs sit centred on the normal text line
            const float dy = (ImGui::GetTextLineHeight() - lh) * 0.5f;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * a_fontMul,
                ImVec2(a_x, a_ty + dy), ImGui::GetColorU32(col), a_lbl);
            if (a_on) {   // mockup .tbtn.on::after
                const float uy = a_ty + ImGui::GetTextLineHeight() + 3.0f;
                if (sk.cornerFade) {
                    dl->AddRectFilledMultiColor(ImVec2(a_x, uy),
                        ImVec2(a_x + a_w, uy + 1.0f),
                        Theme::Acc(0.8f), Theme::Acc(0.0f),
                        Theme::Acc(0.0f), Theme::Acc(0.8f));
                } else {
                    dl->AddRectFilled(ImVec2(a_x, uy), ImVec2(a_x + a_w, uy + 1.0f),
                        Theme::Acc(0.7f));
                }
            }
            return pressed;
        }

        // right-aligned titlebar controls (F1): … EDIT SETTINGS ✕ — the close
        // button sits at the right edge and closes EVERYTHING at once (no
        // sub-window cascade), with the close sound played up front.
        void DrawTitleBarControls(const ImVec2& a_mainSize, float a_barH, float a_pad,
                                  float a_insX, float a_insY,
                                  const char* a_editLbl, const char* a_setLbl,
                                  float a_editW, float a_setW, float a_btnGap)
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const float ty = wp.y + a_insY + (a_barH - ImGui::GetTextLineHeight()) * 0.5f;
            const char* closeLbl = "\xC3\x97";   // × (U+00D7, already baked)
            constexpr float kCloseMul = 1.55f;   // × alone is unreadably small
            const float closeW = ImGui::CalcTextSize(closeLbl).x * kCloseMul;
            const float xClose = wp.x + a_mainSize.x - a_pad - a_insX - closeW;
            const float xSet = xClose - a_btnGap - a_setW;
            const float xEdit = xSet - a_btnGap - a_editW;
            if (TitleBarTextButton(xEdit, ty, a_editLbl, a_editW, Editor::IsEditMode())) {
                Editor::ToggleEditMode();
                if (Editor::IsEditMode()) Sfx::SelectOn();
                else                      Sfx::SelectOff();
            }
            if (TitleBarTextButton(xSet, ty, a_setLbl, a_setW, g_showSettings)) {
                g_showSettings = !g_showSettings;
                if (g_showSettings) Sfx::SelectOn();
                else                Sfx::SelectOff();
            }
            // generous invisible hitbox: the glyph stays this size, the click
            // target doesn't (8px < half the 18px button gap — no overlap)
            const float closePad = 8.0f * Theme::Scale();
            if (TitleBarTextButton(xClose, ty, closeLbl, closeW, false, kCloseMul, closePad)) {
                Sfx::MenuClose();
                GridInventoryMenu::MarkCloseSfxPlayed();   // no OnHide double-play
                Close();
            }
        }

        // park the engine-drawn model: behind the opaque main window, or (for
        // translucent skins) at SCREEN CENTRE under the caching card
        void ParkPreviewModel(const ImVec2& a_mainSize)
        {
            if (Theme::S().translucent) {
                const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
                ItemPreview::GetSingleton()->SetParkPos(
                    ImVec2(static_cast<float>(sz.width) * 0.5f,
                           static_cast<float>(sz.height) * 0.5f));
            } else {
                const ImVec2 wp = ImGui::GetWindowPos();
                ItemPreview::GetSingleton()->SetParkPos(
                    ImVec2(wp.x + a_mainSize.x * 0.5f, wp.y + a_mainSize.y * 0.5f));
            }
        }

        // GOLD bar — pinned to the bottom of a column of width a_colW.
        // a_rightReserve (F2): the trash-can button shares the compact strip,
        // so the amount shifts left of it.
        void DrawGoldBar(float a_colW, float a_bodyH, float a_rightReserve = 0.0f)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const float gy = cp.y + a_bodyH - 30.0f * S;
            dl->AddLine(ImVec2(cp.x, gy), ImVec2(cp.x + a_colW, gy), Theme::Acc(0.25f));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d", Grid::GoldAmount());
            const float amtW = ImGui::CalcTextSize(buf).x;
            if (sk.diamondLabels) {   // v10.4: crimson "◇ GOLD"
                char lbl[48];
                std::snprintf(lbl, sizeof(lbl), "\xE2\x97\x87 %s", Lang::T(Lang::Str::Gold));
                dl->AddText(ImVec2(cp.x + 2.0f, gy + 8.0f * S),
                    Theme::Col(sk.sel, 1.0f), lbl);
            } else {
                dl->AddText(ImVec2(cp.x + 2.0f, gy + 8.0f * S), Theme::Acc(0.7f),
                    Lang::T(Lang::Str::Gold));
            }
            dl->AddText(ImVec2(cp.x + a_colW - amtW - 2.0f - a_rightReserve, gy + 8.0f * S),
                Theme::Col(sk.hi, 1.0f), buf);
        }

        // F2: trash-can button in the bottom-right strip of the grid column.
        // Vector glyph (lid + body + ribs) — no baked icon needed. Toggles
        // the 6x4 trash window.
        void DrawTrashButton(float a_colW, float a_bodyH)
        {
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 cp = ImGui::GetWindowPos();
            const float side = 18.0f * S;
            const ImVec2 p0(cp.x + a_colW - side - 2.0f,
                            cp.y + a_bodyH - 30.0f * S + (30.0f * S - side) * 0.5f + 2.0f * S);
            ImGui::SetCursorScreenPos(ImVec2(p0.x - 4.0f * S, p0.y - 4.0f * S));
            const bool pressed = ImGui::InvisibleButton("##gi_trashbtn",
                ImVec2(side + 8.0f * S, side + 8.0f * S));
            const bool hov = ImGui::IsItemHovered();
            if (hov) Sfx::HoverNote(ImGui::GetItemID());
            const bool on = Grid::IsTrashOpen();
            const ImU32 col = on    ? Theme::Col(sk.sel, 1.0f)
                              : hov ? ImGui::GetColorU32(sk.ink)
                                    : ImGui::GetColorU32(sk.inkDim);
            const float w = side, h = side;
            const float t = (std::max)(1.0f, 1.2f * S);
            // lid + handle
            dl->AddLine(ImVec2(p0.x + 0.10f * w, p0.y + 0.18f * h),
                        ImVec2(p0.x + 0.90f * w, p0.y + 0.18f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.35f * w, p0.y + 0.06f * h),
                        ImVec2(p0.x + 0.65f * w, p0.y + 0.06f * h), col, t);
            // tapered body
            dl->AddLine(ImVec2(p0.x + 0.18f * w, p0.y + 0.18f * h),
                        ImVec2(p0.x + 0.28f * w, p0.y + 0.95f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.82f * w, p0.y + 0.18f * h),
                        ImVec2(p0.x + 0.72f * w, p0.y + 0.95f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.28f * w, p0.y + 0.95f * h),
                        ImVec2(p0.x + 0.72f * w, p0.y + 0.95f * h), col, t);
            // ribs
            dl->AddLine(ImVec2(p0.x + 0.42f * w, p0.y + 0.32f * h),
                        ImVec2(p0.x + 0.44f * w, p0.y + 0.82f * h), col, t);
            dl->AddLine(ImVec2(p0.x + 0.58f * w, p0.y + 0.32f * h),
                        ImVec2(p0.x + 0.56f * w, p0.y + 0.82f * h), col, t);
            if (pressed) Grid::ToggleTrash();
        }

        void DrawMainWindow()
        {
            const auto& io = ImGui::GetIO();
            const auto& sk = Theme::S();
            const float S = Theme::Scale();
            const float insX = Theme::FrameInsetX();   // tornFrame breathing room
            const float insY = Theme::FrameInsetY();
            auto* wm = WinManager::GetSingleton();

            // A안: loot/barter mode hides the EQUIP doll + stats — the player
            // window is just the item grid + GOLD bar, mirroring the partner
            // window. Plain inventory keeps the full left column.
            const bool  compact = LootBarter::CurrentMode() != LootBarter::Mode::kNormal;
            const float barH   = 34.0f * S;
            const float pad    = 12.0f * S;
            const float leftW  = compact ? 0.0f : Equip::PanelW();
            // exact grid width — the legacy +20 scrollbar slack made the
            // right margin visibly wider than the left (v10.7 feedback)
            const float gridW  = Grid::kCols * Grid::CellPx();
            // grid column height = ITEMS label row + the grid itself + the
            // 30px bottom strip (GOLD bar / trash button). The label row was
            // missing here, so the strip's baseline sat ON the last grid row
            // and the trash button overlapped the cells (user-reported).
            const float itemsLabelH = ImGui::GetTextLineHeightWithSpacing() + 3.0f * S;
            const float gridBodyH = itemsLabelH + Grid::kMinRows * Grid::CellPx() + 30.0f * S;
            // left column must fit doll + stats panel + GOLD bar (S1); compact
            // reserves the GOLD-bar strip under the grid instead
            const float bodyH  = (compact
                ? gridBodyH + 30.0f * S
                : (std::max)(Equip::PanelH() + 44.0f * S + StatsPanelH(), gridBodyH)) + 8.0f * S;
            const ImVec2 mainSize(compact
                    ? pad + gridW + pad + 2.0f * insX
                    : pad + leftW + pad + 1.0f + pad + gridW + pad + 2.0f * insX,
                barH + bodyH + pad + 2.0f * insY);

            wm->ApplyNext("main",
                ImVec2((io.DisplaySize.x - mainSize.x) * 0.5f,
                       (io.DisplaySize.y - mainSize.y) * 0.5f),
                mainSize);

            if (!ImGui::Begin("##fablerim_main", nullptr, kManagedWinFlags)) {
                ImGui::End();
                return;
            }
            const char* editLbl = Lang::T(Lang::Str::Edit);
            const char* setLbl = Lang::T(Lang::Str::Settings);
            const float editW = ImGui::CalcTextSize(editLbl).x;
            const float setW = ImGui::CalcTextSize(setLbl).x;
            const float btnGap = 18.0f * S;
            const float closeW = ImGui::CalcTextSize("\xC3\x97").x * 1.55f;   // F1 ✕ (kCloseMul)
            // strip excludes the right-aligned control zone (EDIT + SETTINGS
            // + ✕) so the buttons below actually receive their clicks
            wm->TitleBar("main", Lang::T(Lang::Str::Inventory),
                pad + insX + editW + setW + closeW + 2.0f * btnGap + 14.0f * S);

            const ImVec2 bodyTop = ImGui::GetCursorScreenPos();

            DrawTitleBarControls(mainSize, barH, pad, insX, insY,
                editLbl, setLbl, editW, setW, btnGap);
            ParkPreviewModel(mainSize);

            // controls moved the cursor — body starts back under the titlebar
            ImGui::SetCursorScreenPos(bodyTop);

            // ---- left column: tabs + doll + GOLD bar (plain inventory only) ----
            if (!compact) {
                ImGui::BeginChild("fab_left", ImVec2(leftW, bodyH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                Equip::Draw();
                DrawStatsPanel(leftW, bodyH);   // S1/S2: combat + space, above GOLD
                DrawGoldBar(leftW, bodyH);
                ImGui::EndChild();

                // vertical divider
                auto* dl = ImGui::GetWindowDrawList();
                const float dx = bodyTop.x + leftW + pad;
                dl->AddLine(ImVec2(dx, bodyTop.y), ImVec2(dx, bodyTop.y + bodyH), Theme::Acc(0.18f));
            }

            // ---- right column: ITEMS label + grid (+ GOLD bar when compact) ----
            const float rightX = compact ? bodyTop.x : bodyTop.x + leftW + pad + 1.0f + pad;
            ImGui::SetCursorScreenPos(ImVec2(rightX, bodyTop.y));
            ImGui::BeginChild("fab_right", ImVec2(gridW, bodyH), ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            {
                if (sk.diamondLabels) {   // v10.4: "◇ LABEL" in crimson
                    ImGui::TextColored(sk.sel, "\xE2\x97\x87 %s", Lang::T(Lang::Str::Items));
                } else {
                    ImGui::TextColored(ImVec4(sk.acc.x, sk.acc.y, sk.acc.z, 0.6f), "%s",
                        Lang::T(Lang::Str::Items));
                }
                auto* cache = IconCache::GetSingleton();
                if (cache->IsBusy()) {
                    ImGui::SameLine();
                    ImGui::TextColored(sk.inkDim, "  %s %zu",
                        Lang::T(Lang::Str::Caching), cache->QueuedCount());
                }
            }
            Grid::Draw();
            if (compact) {
                // GOLD strip under the grid; amount clears the trash button
                DrawGoldBar(gridW, bodyH, 26.0f * S);
            }
            DrawTrashButton(gridW, bodyH);   // F2: bottom-right of the grid column
            ImGui::EndChild();

            ImGui::End();
        }
    }


    void DrawItemIcon(ImDrawList* a_dl, void* a_srv, const ImVec2& a_min, const ImVec2& a_max)
    {
        const float g = Theme::IconGain();
        const auto  tex = reinterpret_cast<ImTextureID>(a_srv);
        // <=1: plain darkening tint. >1: full draw + additive top-up of
        // icon*(g-1) — exact linear gain, applied LIVE (no texture rebake)
        const auto c = static_cast<std::uint32_t>(
            255.0f * (std::min)(1.0f, g) + 0.5f);
        a_dl->AddImage(tex, a_min, a_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
            IM_COL32(c, c, c, 255));
        if (g > 1.0f && g_addBlend) {
            const auto t = static_cast<std::uint32_t>(
                (std::min)(1.0f, g - 1.0f) * 255.0f + 0.5f);
            a_dl->AddCallback(&AdditiveBlendCB, nullptr);
            a_dl->AddImage(tex, a_min, a_max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                IM_COL32(255, 255, 255, t));
            a_dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }
    }

    void* GlowTexture()
    {
        return g_glowSRV;
    }

    void* TornGlowA()
    {
        return g_tornGlowA.srv;
    }

    void* TornGlowB()
    {
        return g_tornGlowB.srv;
    }

    void* TornCreamA()
    {
        return g_tornCreamA.srv;
    }

    void* TornBrightB()
    {
        return g_tornBrightB.srv;
    }

    void RegisterMenu()
    {
        GridInventoryMenu::RegisterMenu();
    }

    bool TryInitD3D()
    {
        if (g_initialized.load()) return true;

        auto* data = RE::BSGraphics::Renderer::GetRendererData();
        if (!data || !data->forwarder || !data->context) {
            SKSE::log::warn("[UI] TryInitD3D: renderer data unavailable");
            return false;
        }

        auto* swapChain = data->renderWindows[0].swapChain;
        if (!swapChain) {
            SKSE::log::warn("[UI] TryInitD3D: swap chain unavailable");
            return false;
        }

        REX::W32::DXGI_SWAP_CHAIN_DESC desc{};
        if (swapChain->GetDesc(&desc) < 0) {
            SKSE::log::error("[UI] TryInitD3D: GetDesc failed");
            return false;
        }

        auto* device  = reinterpret_cast<ID3D11Device*>(data->forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        auto  hwnd    = reinterpret_cast<HWND>(desc.outputWindow);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        if (!ImGui_ImplWin32_Init(hwnd)) {
            SKSE::log::error("[UI] ImGui_ImplWin32_Init failed");
            return false;
        }

        // keyboard/char input: chained WndProc (see WndProcThunk above)
        if (!g_origWndProc) {
            g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
                hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProcThunk)));
        }
        if (!ImGui_ImplDX11_Init(device, context)) {
            SKSE::log::error("[UI] ImGui_ImplDX11_Init failed");
            return false;
        }

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigNavMoveSetMousePos = false;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        io.IniFilename = nullptr;

        // fonts are baked at the saved UI scale (WinManager may not have
        // loaded yet here — OnShow re-requests a bake if the scale differs)
        BuildFonts();

        ImGui::StyleColorsDark();
        Theme::Apply();
        CreateGlowTexture(device);
        CreateAdditiveBlend(device);

        // OATHVEIN TORN 9-slice panels: baked from the user's torn PNG with a
        // light rim/glow (in-game has no CSS filter — glow is in the texture)
        IconCache::LoadFicTexture(
            "Data/SKSE/Plugins/GridInventory_slots/frame_torn_glowA.fic", g_tornGlowA);
        IconCache::LoadFicTexture(
            "Data/SKSE/Plugins/GridInventory_slots/frame_torn_glowB.fic", g_tornGlowB);
        // skins 5/6: light cream rebakes (V1/V2, bake_torn_skins.py)
        IconCache::LoadFicTexture(
            "Data/SKSE/Plugins/GridInventory_slots/frame_torn_creamA.fic", g_tornCreamA);
        IconCache::LoadFicTexture(
            "Data/SKSE/Plugins/GridInventory_slots/frame_torn_brightB.fic", g_tornBrightB);

        g_initialized.store(true);
        SKSE::log::info("[UI] ImGui initialized (hwnd={:#x})", reinterpret_cast<uintptr_t>(hwnd));
        return true;
    }

    bool IsInitialized()
    {
        return g_initialized.load();
    }

    void Open()
    {
        if (!TryInitD3D()) {
            SKSE::log::error("[UI] Open aborted: ImGui not initialized");
            return;
        }
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(GridInventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
        }
    }

    void Close()
    {
        if (auto* mq = RE::UIMessageQueue::GetSingleton()) {
            mq->AddMessage(GridInventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
        }
    }

    void OpenInspect(RE::TESBoundObject* a_obj, const std::string& a_key)
    {
        if (!a_obj) return;
        g_inspObj = a_obj;
        g_inspKey = a_key;
        // start where the icon already looks, so the view is continuous with
        // the tile the player pressed C on
        const IconDef d = IconCache::GetSingleton()->ResolveDef(a_obj);
        g_inspRx = g_inspRx0 = d.rx;
        g_inspRy = g_inspRy0 = d.ry;
        g_inspRz = g_inspRz0 = d.rz;
        g_inspZoom = 1.0f;
        g_inspDrag = false;
        g_inspOpenFrame = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
        IconCache::GetSingleton()->SetInspect(a_obj, g_inspRx, g_inspRy, g_inspRz);
    }

    bool IsInspectOpen() { return g_inspObj != nullptr; }

    bool CloseInspect()
    {
        if (!g_inspObj) return false;
        g_inspObj = nullptr;
        g_inspKey.clear();
        g_inspDrag = false;
        IconCache::GetSingleton()->ClearInspect();
        Grid::RefreshDefs();   // tiles go back to their own def orientation
        return true;
    }

    namespace
    {
        // Modal 3D inspect: full-screen dim + the live engine capture drawn
        // large. Drawn LAST so ImGui hover/click blocking makes it modal, and
        // NoteOverlayRect keeps the grid's raw-mouse paths out.
        void DrawInspect()
        {
            if (!g_inspObj) return;
            auto* icons = IconCache::GetSingleton();
            auto& io = ImGui::GetIO();
            const float S = Theme::Scale();
            const auto& sk = Theme::S();

            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(6, 5, 4, 232));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            const bool open = ImGui::Begin("##fablerim_inspect", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoCollapse);
            if (open) {
                NoteOverlayRect();

                // Rotate by dragging — handled MANUALLY rather than with a
                // full-screen InvisibleButton, which would own ActiveId across
                // the whole screen. The overlay is deliberately WIDGET-FREE
                // (a button here fought that same ActiveId), so a plain
                // hover + mouse-down test is all it takes.
                if (!g_inspDrag && io.MouseClicked[0] && ImGui::IsWindowHovered()) {
                    g_inspDrag = true;
                }
                if (g_inspDrag && !io.MouseDown[0]) g_inspDrag = false;
                bool moved = false;
                if (g_inspDrag && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)) {
                    constexpr float kDegPerPx = 0.6f;
                    if (io.KeyShift) {
                        g_inspRy += io.MouseDelta.x * kDegPerPx;
                    } else {
                        g_inspRz += io.MouseDelta.x * kDegPerPx;
                        g_inspRx += io.MouseDelta.y * kDegPerPx;
                    }
                    auto wrap = [](float& v) {
                        while (v > 180.0f) v -= 360.0f;
                        while (v < -180.0f) v += 360.0f;
                    };
                    wrap(g_inspRx);
                    wrap(g_inspRy);
                    wrap(g_inspRz);
                    moved = true;
                }
                if (io.MouseWheel != 0.0f) {
                    g_inspZoom = std::clamp(g_inspZoom * (1.0f + io.MouseWheel * 0.12f),
                        0.5f, 3.5f);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.WantTextInput) {
                    g_inspRx = g_inspRx0;
                    g_inspRy = g_inspRy0;
                    g_inspRz = g_inspRz0;
                    g_inspZoom = 1.0f;
                    moved = true;
                }
                // the capture itself is driven by IconCache::PreRender, which
                // gives the preview to the inspected item every frame
                if (moved) icons->SetInspectRot(g_inspRx, g_inspRy, g_inspRz);

                // the sprite: native pixels, capped so it always fits
                const float boxH = io.DisplaySize.y * 0.62f * g_inspZoom;
                const float boxW = io.DisplaySize.x * 0.62f * g_inspZoom;
                if (const auto* ic = icons->InspectIcon(); ic && ic->srv && ic->w > 0) {
                    const float sc = (std::min)(boxW / static_cast<float>(ic->w),
                        boxH / static_cast<float>(ic->h));
                    const ImVec2 sz(static_cast<float>(ic->w) * sc,
                        static_cast<float>(ic->h) * sc);
                    const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
                    ImGui::GetWindowDrawList()->AddImage(
                        reinterpret_cast<ImTextureID>(ic->srv),
                        ImVec2(c.x - sz.x * 0.5f, c.y - sz.y * 0.5f),
                        ImVec2(c.x + sz.x * 0.5f, c.y + sz.y * 0.5f));
                } else {
                    const char* wait = Lang::T(Lang::Str::Caching);
                    const ImVec2 tw = ImGui::CalcTextSize(wait);
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2((io.DisplaySize.x - tw.x) * 0.5f,
                               (io.DisplaySize.y - tw.y) * 0.5f),
                        Theme::Col(sk.inkDim, 1.0f), wait);
                }

                // name (top) + control hint / adopt button (bottom)
                auto* dl = ImGui::GetWindowDrawList();
                if (const char* nm = g_inspObj->GetName(); nm && nm[0]) {
                    const ImVec2 nw = ImGui::CalcTextSize(nm);
                    dl->AddText(ImVec2((io.DisplaySize.x - nw.x) * 0.5f, 26.0f * S),
                        Theme::Col(sk.hi, 1.0f), nm);
                }
                const char* hint = Lang::T(Lang::Str::InspectHint);
                const ImVec2 hw = ImGui::CalcTextSize(hint);
                dl->AddText(ImVec2((io.DisplaySize.x - hw.x) * 0.5f,
                                   io.DisplaySize.y - 40.0f * S),
                    Theme::Col(sk.inkDim, 1.0f), hint);

                // (no widgets here by design — see the drag comment above:
                // icon rotation is edited in the EDIT panel / IconStudio)
            }
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // C toggles it shut (ESC goes through the CloseTopWindow chain) —
            // never on the opening frame, where the grid's own C is still down
            if (g_inspObj && ImGui::GetFrameCount() != g_inspOpenFrame &&
                ImGui::IsKeyPressed(ImGuiKey_C, false) && !io.WantTextInput) {
                CloseInspect();
            }
        }
    }

    bool CloseTopWindow()
    {
        if (CloseInspect()) return true;   // modal 3D inspect sits on top
        // sub-popups close first, one per keypress (topmost priority):
        // quantity slider / sale confirm -> loadout popups -> coin pouch ->
        // settings -> EDIT mode; only when all are closed does the caller
        // close the inventory itself
        if (Grid::CloseTrashConfirm()) return true;   // F2 favorite ask (popup tier)
        if (LootBarter::CloseTopPopup()) return true;
        if (Equip::CloseTopPopup()) return true;
        if (Grid::CloseTrash()) return true;          // F2: confirm-all + close
        if (Grid::ClosePouch()) return true;
        if (g_showSettings) {
            g_showSettings = false;
            return true;
        }
        if (Editor::IsEditMode()) {
            Editor::ToggleEditMode();   // same path as clicking EDIT off
            return true;
        }
        return false;
    }

    namespace
    {
        // menu-open whoosh: at kShow time the sound was swallowed (transition)
        // — play a few frames later once the menu is actually rendering
        int g_menuOpenSfx = 0;
    }

    void OnShow()
    {
        g_menuOpenSfx = 10;   // clear of the open transition (3 was too early)
        // Park anchor = the SAVED main-window centre — set BEFORE the first
        // capture request so no frame is ever exposed.
        {
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            const ImVec2 center(static_cast<float>(sz.width) * 0.5f,
                                static_cast<float>(sz.height) * 0.5f);
            auto* wm = WinManager::GetSingleton();
            wm->Load();
            ItemPreview::GetSingleton()->SetParkPos(
                Theme::S().translucent ? center : wm->MainCenter(center));

            // ini scale arrived after the init-time bake -> rebake once
            if (std::fabs(Theme::Scale() - g_bakedScale) > 0.005f) {
                g_fontsDirty.store(true);
            }
        }

        // Callback FIRST: it hot-reloads the item defs which the grid and the
        // capture queue key off (building before the reload uses stale defs).
        if (g_onShow) g_onShow();

        Grid::Rebuild();

        // B: prefetch EVERYTHING the player carries the moment the menu
        // opens — one up-front caching burst instead of per-scroll/per-bag
        // trickle (bag contents live in the same inventory, so this covers
        // them too). Disk-cached items are skipped by pak index (no GPU).
        if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
            auto* cache = IconCache::GetSingleton();
            for (const auto& [obj, data] : pl->GetInventory()) {
                if (obj && data.first > 0) cache->Prefetch(obj);
            }
        }

        SKSE::log::info("[UI] menu shown ({} icons cached)",
            IconCache::GetSingleton()->CachedCount());
    }

    void OnClose()
    {
        CloseInspect();           // release the pinned inspect model + engine scale
        Editor::OnMenuClosed();   // flush pending edits, drop selection
        // F2: closing the whole menu confirms every parked deletion; flush
        // immediately (same context LootBarter::Reset moves items in)
        if (Grid::CloseTrash()) Grid::ProcessTrashDeletes();
        LootBarter::Reset();      // back to kNormal (loot/barter mode ends)
        Grid::ClearPendingEquips();   // no queued equip outlives the menu
        if (Grid::IsHolding()) Grid::CancelHold();   // never close mid-carry
        g_showSettings = false;
        g_textInputOn = false;
        if (ImGui::GetCurrentContext()) {
            ImGui::ClearActiveID();   // drop text-field focus: a stale ActiveId
                                      // keeps WantTextInput true past the close
        }
        WinManager::GetSingleton()->Save();   // window layout persists (F6)
        if (g_onHide) g_onHide();
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().ClearInputKeys();
        }
        SKSE::log::info("[UI] menu hidden");
    }

    // (caching card removed: the full-frame capture restore guarantees the
    // parked model never shows on screen, so translucent skins need no cover;
    // progress still shows via the main window's "Caching N" label)

    namespace
    {
        // overlay rects (x1,y1,x2,y2), double-buffered: the grid draws BEFORE
        // the overlays each frame, so it tests the PREVIOUS frame's rects
        std::vector<ImVec4> g_overlayNow;
        std::vector<ImVec4> g_overlayPrev;
    }

    void NoteOverlayRect()
    {
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        const float m = 14.0f * Theme::Scale();   // torn-frame chrome margin
        g_overlayNow.emplace_back(p.x - m, p.y - m, p.x + s.x + m, p.y + s.y + m);
    }

    bool MouseInOverlay()
    {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        for (const auto& r : g_overlayPrev) {
            if (mp.x >= r.x && mp.y >= r.y && mp.x <= r.z && mp.y <= r.w) {
                return true;
            }
        }
        return false;
    }

    void Render()
    {
        if (!g_initialized.load()) return;

        // rebake outside the frame; the DX11 backend recreates the font
        // texture inside NewFrame after InvalidateDeviceObjects
        if (g_fontsDirty.exchange(false)) BuildFonts();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        auto& io = ImGui::GetIO();
        // B11(P2): queried per frame — a once-cached size went stale after a
        // borderless/fullscreen switch and desynced from OnShow's park math
        const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
        io.DisplaySize.x = static_cast<float>(screenSize.width);
        io.DisplaySize.y = static_cast<float>(screenSize.height);
        // H′: crisp text at any scale — 1.0 when baked; a live bitmap-scale
        // preview only while the slider is mid-drag
        io.FontGlobalScale = Theme::Scale() / g_bakedScale;

        MouseHandler();
        ScrollHandler();

        // NOTE: ControlMap::AllowTextInput is deliberately NOT used. Chars
        // arrive via the chained WndProc (engine machinery not needed), and
        // hotkey suppression is handled by the user-event swallow + the
        // menu-open intercept. Touching the engine counter broke controls:
        // the engine pairs its OWN release on menu close, so our balanced
        // grant/release still went NET -1 per typing session (log-proven,
        // textEntryCount -1 -> -2) and movement/attack died.
        g_textInputOn = io.WantTextInput;

        if (g_menuOpenSfx > 0 && --g_menuOpenSfx == 0) {
            // UIInventoryOpen resolved but stayed inaudible — the Tab-menu
            // blade whoosh is the clearly audible vanilla menu sound
            Sfx::MenuOpen();
        }

        ImGui::NewFrame();
        g_overlayPrev.swap(g_overlayNow);
        g_overlayNow.clear();
        // hover-sound edge detection re-arms once nothing is hovered, so
        // leaving and re-entering the same widget ticks again
        if (!ImGui::IsAnyItemHovered()) Sfx::HoverReset();
        WinManager::GetSingleton()->SetDragLock(Grid::IsHolding());   // F1
        DrawMainWindow();
        Grid::DrawBagWindows();   // one managed window per open bag (E2/E5)
        LootBarter::DrawWindows();  // container/merchant partner window (loot/barter)
        DrawSettingsWindow();     // ⚙ popup (scale / skin / language)
        Equip::DrawLoadoutWindows();   // L2: loadout +buy / delete confirm (top level)
        Grid::DrawPouchWindow();       // G2: coin-pouch withdraw (top level)
        Grid::DrawTrashConfirm();      // F2: favorite-intake confirm (top level)
        LootBarter::DrawSlider();      // loot/barter quantity slider (top level)
        LootBarter::DrawConfirm();     // favorite-sale confirm popup (top level)
        Editor::DrawPanel();      // B-6 EDIT panel (edit mode only)
        DrawInspect();            // C key: modal 3D inspect (drawn last = modal)
        Grid::FinishFrame();      // carry input + deferred rebuilds
        WinManager::GetSingleton()->Update();   // drag / magnet / dock / clamp
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    void Tick()
    {
        Grid::ProcessFavorites();  // GI32: favourites, same reason
        Equip::ProcessPending();   // equip/unequip OUTSIDE the render pass
        Loadout::ProcessPending();  // L1: deferred loadout tab switch
        LootBarter::ProcessTransfers();   // loot take/store OUTSIDE the render pass
        Grid::ProcessTrashDeletes();      // F2: confirmed deletions (engine RemoveItem)
        Grid::CapacityTick();       // W1+W2: weight bypass / space overload
        GoldCoins::Tick();          // G1: mirror the gold ledger into coins
        ItemPreview::GetSingleton()->Tick();

        // free the retired inspect texture (ClearInspect can run mid-frame,
        // where this frame's draw list still references it)
        IconCache::GetSingleton()->ProcessDeferredRelease();

        // ICON CACHE reset (settings): outside the ImGui frame — this frame's
        // draw list no longer references the SRVs being released. The next
        // draw re-queues every visible item for a fresh engine capture.
        if (g_iconsMergePreset.exchange(false)) {   // GI47: bundled icons
            IconCache::GetSingleton()->MergePak(g_presetMergePak.c_str());
        }
        if (g_iconsReset.exchange(false)) {
            IconCache::GetSingleton()->ResetDiskCache();
            Grid::RequestRebuild();
        }
    }

    bool IsTextInputActive()
    {
        return g_textInputOn;
    }

    void AddScrollEvent(float a_x, float a_y)
    {
        const float sx = a_x * kScrollMultiplier;
        const float sy = a_y * kScrollMultiplier;

        // Immediately stop if direction changes
        if (g_scrollEnergy.x * sx < 0.0f) g_scrollEnergy.x = 0.0f;
        if (g_scrollEnergy.y * sy < 0.0f) g_scrollEnergy.y = 0.0f;

        g_scrollEnergy.x += sx;
        g_scrollEnergy.y += sy;
    }

    void SetVisibilityCallbacks(std::function<void()> a_onShow, std::function<void()> a_onHide)
    {
        g_onShow = std::move(a_onShow);
        g_onHide = std::move(a_onHide);
    }
}
