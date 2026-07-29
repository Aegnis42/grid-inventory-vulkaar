#include "game/GoldCoins.h"
#include "ui/Editor.h"
#include "ui/Grid.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace FUI::Editor
{
    namespace
    {
        Hooks               g_hooks;
        bool                g_editMode = false;
        RE::TESBoundObject* g_sel = nullptr;
        std::string         g_selKey;

        FullDef g_cur;                 // live-edited values
        bool    g_dirty = false;       // pending write (debounced)
        float   g_dirtyTimer = 0.0f;

        bool g_paint[6][6] = {};       // footprint painter cells
        bool g_painting = false;       // drag-paint state
        bool g_paintValue = true;      // painting or erasing this drag

        // load the current def into the painter cells
        void DefToPainter()
        {
            std::memset(g_paint, 0, sizeof(g_paint));
            if (!g_cur.shape.empty()) {
                int r = 0, c = 0;
                for (char ch : g_cur.shape) {
                    if (ch == '|') { ++r; c = 0; continue; }
                    if (r < 6 && c < 6) g_paint[r][c] = (ch == '1');
                    ++c;
                }
            } else {
                for (int r = 0; r < 6 && r < g_cur.h; ++r)
                    for (int c = 0; c < 6 && c < g_cur.w; ++c)
                        g_paint[r][c] = true;
            }
        }

        // painter -> trimmed shape/w/h (H2: full rectangle collapses to w/h)
        void PainterToDef()
        {
            int minR = 6, maxR = -1, minC = 6, maxC = -1;
            for (int r = 0; r < 6; ++r)
                for (int c = 0; c < 6; ++c)
                    if (g_paint[r][c]) {
                        minR = (std::min)(minR, r); maxR = (std::max)(maxR, r);
                        minC = (std::min)(minC, c); maxC = (std::max)(maxC, c);
                    }
            if (maxR < 0) return;   // empty painter: don't save (H2)

            const int w = maxC - minC + 1, h = maxR - minR + 1;
            bool full = true;
            std::string shape;
            for (int r = minR; r <= maxR; ++r) {
                if (r > minR) shape += '|';
                for (int c = minC; c <= maxC; ++c) {
                    shape += g_paint[r][c] ? '1' : '0';
                    if (!g_paint[r][c]) full = false;
                }
            }
            g_cur.w = w;
            g_cur.h = h;
            g_cur.shape = full ? std::string() : shape;
        }

        // Values apply LIVE (memory-only, every change); the dirty machinery
        // only debounces the ini FILE write — a slider drag would otherwise
        // rewrite the file every frame.
        void MarkDirty()
        {
            g_dirty = true;
            g_dirtyTimer = 0.5f;
        }

        void FlushDirty()
        {
            if (!g_dirty || !g_sel || !g_hooks.setOverride) return;
            g_dirty = false;
            g_hooks.setOverride(g_sel, g_cur, true);   // persist to ini
        }

        // mockup .drow track: [label][accent-filled value bar][defnote].
        // The fill/border draw first; the DragFloat sits on top with a
        // transparent frame so only the centred value text shows.
        void TrackChrome(ImVec2 a_p, float a_w, float a_h, float a_frac)
        {
            auto* dl = ImGui::GetWindowDrawList();
            const float r = Theme::S().rounding;
            const float f = (std::max)(0.0f, (std::min)(1.0f, a_frac));
            dl->AddRectFilled(a_p, ImVec2(a_p.x + a_w, a_p.y + a_h), IM_COL32(0, 0, 0, 51), r);
            if (f > 0.0f) {
                dl->AddRectFilled(a_p, ImVec2(a_p.x + a_w * f, a_p.y + a_h),
                    Theme::GaugeFill(), r);
            }
            dl->AddRect(a_p, ImVec2(a_p.x + a_w, a_p.y + a_h), Theme::GaugeBorder(), r);
        }

        constexpr float kLabelW = 46.0f;   // * scale
        constexpr float kTrackW = 158.0f;  // * scale

        // drag row with default-value comparison (H6 ⑤). DragFloat gives
        // pixel-proportional fine control; Ctrl+click types an exact value.
        bool AngleSlider(const char* a_label, float& a_val, float a_defVal)
        {
            const float S = Theme::Scale();
            ImGui::TextColored(Theme::S().inkDim, "%s", a_label);
            ImGui::SameLine(kLabelW * S);
            TrackChrome(ImGui::GetCursorScreenPos(), kTrackW * S,
                ImGui::GetFrameHeight(), (a_val + 180.0f) / 360.0f);
            Theme::PushChromeStyle(false);
            ImGui::SetNextItemWidth(kTrackW * S);
            const bool changed = ImGui::DragFloat((std::string("##") + a_label).c_str(),
                &a_val, 0.5f, -180.0f, 180.0f, "%.1f\xC2\xB0");
            Theme::PopChromeStyle(false);
            ImGui::SameLine();
            if (std::fabs(a_val - a_defVal) > 0.5f) {
                ImGui::TextColored(Theme::S().acc, "(def %.0f\xC2\xB0)", a_defVal);
            } else {
                ImGui::TextDisabled("(default)");
            }
            return changed;
        }
    }

    void SetHooks(Hooks a_hooks)
    {
        g_hooks = std::move(a_hooks);
    }

    bool IsEditMode() { return g_editMode; }

    void ToggleEditMode()
    {
        g_editMode = !g_editMode;
        if (!g_editMode) {
            FlushDirty();
            g_sel = nullptr;
            g_selKey.clear();
            IconCache::GetSingleton()->SetPin(nullptr);
        }
    }

    void Select(RE::TESBoundObject* a_obj, const std::string& a_key)
    {
        FlushDirty();   // switching items writes the previous one first
        g_sel = a_obj;
        g_selKey = a_key;
        if (g_hooks.getEffective) g_cur = g_hooks.getEffective(a_obj);
        DefToPainter();
        // keep the selection's model loaded: rotation edits re-capture fast
        IconCache::GetSingleton()->SetPin(a_obj);
    }

    bool IsSelected(const std::string& a_key)
    {
        return g_editMode && !g_selKey.empty() && g_selKey == a_key;
    }

    void OnMenuClosed()
    {
        FlushDirty();
        g_editMode = false;
        g_sel = nullptr;
        g_selKey.clear();
        IconCache::GetSingleton()->SetPin(nullptr);
    }

    void DrawPanel()
    {
        if (!g_editMode) return;

        // debounce
        if (g_dirty) {
            g_dirtyTimer -= ImGui::GetIO().DeltaTime;
            if (g_dirtyTimer <= 0.0f) FlushDirty();
        }

        auto* wm = WinManager::GetSingleton();
        const float s = Grid::CellPx() / 48.0f;   // = Theme::Scale()
        // tall enough that the body never needs a scrollbar (v9.2 feedback;
        // v10.6 merged the painter+bag row, so the panel shrank).
        // +2x frame inset for tornFrame skins (breathing room)
        const ImVec2 size(342.0f * s + 2.0f * Theme::FrameInsetX(),
                          666.0f * s + 2.0f * Theme::FrameInsetY());   // +Stack row (G3)
        ImVec2 defPos(60.0f, 120.0f);
        if (auto* mw = wm->Find("main")) {
            defPos = ImVec2(mw->pos.x - size.x - 8.0f, mw->pos.y);
        }
        wm->ApplyNext("editor", defPos, size);

        // Bake the torn-frame inset into the window padding: every line and
        // the body child then respect the ragged edges on BOTH sides (only
        // the first line honoured the TitleBar origin before — the right
        // margin vanished under the tear on skins 3/4).
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(12.0f + Theme::FrameInsetX(), 8.0f + Theme::FrameInsetY()));
        ImGui::Begin("##fablerim_editor", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        wm->TitleBar("editor", Lang::T(Lang::Str::Edit));

        // scrollable body child — the titlebar pins the content start to a
        // SCREEN position each frame, so window-level scrolling moves the
        // scrollbar but never the content; the child scrolls internally
        ImGui::BeginChild("##editor_body", ImVec2(0.0f, 0.0f));

        if (!g_sel) {
            ImGui::TextDisabled("%s", Lang::T(Lang::Str::SelectHint));
            ImGui::EndChild();
            ImGui::End();
            ImGui::PopStyleVar();   // WindowPadding (torn-frame inset)
            return;
        }

        const FullDef def = g_hooks.getDefault ? g_hooks.getDefault(g_sel) : FullDef{};

        // ---- ① selection info (no thumbnail — the grid tile IS the preview) ----
        ImGui::BeginGroup();
        ImGui::TextColored(Theme::S().hi, "%s", g_sel->GetName());   // mockup: name = hi
        ImGui::TextDisabled("%s", g_selKey.c_str());
        if (g_hooks.categoryName) {
            ImGui::TextDisabled("category: %s%s", g_hooks.categoryName(g_sel).c_str(),
                g_hooks.hasOverride && g_hooks.hasOverride(g_sel) ? "  [override]" : "");
        }
        ImGui::EndGroup();
        ImGui::Separator();

        // ---- ② rotation / ③ scale (applied live every frame) ----
        bool chOrient = false;   // rotation/scale: recapture only, no reflow
        bool chLayout = false;   // footprint/bag: grid placement changes
        chOrient |= AngleSlider("RX", g_cur.rx, def.rx);
        chOrient |= AngleSlider("RY", g_cur.ry, def.ry);
        chOrient |= AngleSlider("RZ", g_cur.rz, def.rz);
        {
            const float S0 = Theme::Scale();
            ImGui::TextColored(Theme::S().inkDim, "Scale");
            ImGui::SameLine(kLabelW * S0);
            TrackChrome(ImGui::GetCursorScreenPos(), kTrackW * S0,
                ImGui::GetFrameHeight(), (g_cur.scale - 0.05f) / 1.95f);
            Theme::PushChromeStyle(false);
            ImGui::SetNextItemWidth(kTrackW * S0);
            if (ImGui::DragFloat("##Scale", &g_cur.scale, 0.01f, 0.05f, 20.0f, "%.2f")) {
                chOrient = true;
            }
            Theme::PopChromeStyle(false);
            ImGui::SameLine();
            if (std::fabs(g_cur.scale - def.scale) > 0.005f) {
                ImGui::TextColored(Theme::S().acc, "(def %.2f)", def.scale);
            } else {
                ImGui::TextDisabled("(default)");
            }
        }
        ImGui::Separator();

        // ---- ④+⑤ footprint painter (left) + bag column (right): ONE row,
        // caption under the painter — mockup edrow2 layout (v10.6) ----
        const float S = Theme::Scale();
        {
            const float cell = 30.0f;
            auto* dl = ImGui::GetWindowDrawList();
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 base = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##painter", ImVec2(cell * 6, cell * 6));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int hc = static_cast<int>((mouse.x - base.x) / cell);
            const int hr = static_cast<int>((mouse.y - base.y) / cell);
            const bool inCell = hovered && hc >= 0 && hc < 6 && hr >= 0 && hr < 6;

            if (inCell && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_painting = true;
                g_paintValue = !g_paint[hr][hc];   // first cell decides paint vs erase
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_painting = false;
            if (g_painting && inCell && g_paint[hr][hc] != g_paintValue) {
                g_paint[hr][hc] = g_paintValue;
                PainterToDef();
                chLayout = true;
            }

            // mockup painter: ivory border + skin-specific fill (OATHVEIN
            // paints with the sel colour, the rest with the accent)
            const auto& skp = Theme::S();
            const ImU32 onCol = skp.diamondLabels
                ? Theme::Col(skp.sel, 0.60f) : Theme::Acc(0.55f);
            for (int r = 0; r < 6; ++r) {
                for (int c = 0; c < 6; ++c) {
                    const ImVec2 p0(base.x + c * cell, base.y + r * cell);
                    const ImVec2 p1(p0.x + cell - 2, p0.y + cell - 2);
                    dl->AddRectFilled(p0, p1,
                        g_paint[r][c] ? onCol : IM_COL32(0, 0, 0, 90), skp.rounding);
                    dl->AddRect(p0, p1, Theme::Acc(0.30f), skp.rounding);
                }
            }

            // caption BELOW the painter (mockup .pnote)
            ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + cell * 6 + 6.0f));
            ImGui::TextDisabled("%s · %dx%d%s", Lang::T(Lang::Str::FootprintHint),
                g_cur.w, g_cur.h, g_cur.shape.empty() ? "" : " (shape)");
            const float leftBottom = ImGui::GetCursorScreenPos().y;

            // ---- right column: Bag / W / H ----
            // stretches to the avail edge — the window padding now carries
            // the torn inset, so this stops at the proper right margin
            const float colX = base.x + cell * 6 + 16.0f;
            const float colW = (std::max)(90.0f, base.x + availW - colX);
            const float lblW = 22.0f * S;
            const float trackW = colW - lblW;
            const float rowH = ImGui::GetFrameHeight() + 9.0f * S;

            ImGui::SetCursorScreenPos(ImVec2(colX, base.y));
            // items whose right-click/consume/system flows would fight the
            // bag window toggle can never BE bags: weapons, consumables,
            // spell tomes, ammo, keys, lockpicks, jewelry, gold coins/pouch
            bool bagAllowed = !(g_sel->Is(RE::FormType::Weapon) ||
                g_sel->Is(RE::FormType::AlchemyItem) ||
                g_sel->Is(RE::FormType::Ingredient) ||
                g_sel->Is(RE::FormType::Scroll) ||
                g_sel->Is(RE::FormType::Ammo) ||
                g_sel->Is(RE::FormType::KeyMaster) ||
                g_sel->GetFormID() == 0x0000000A ||                // lockpick
                GoldCoins::IsCoinForm(g_sel->GetFormID()));        // coins + pouch
            if (bagAllowed) {
                if (const auto* book = g_sel->As<RE::TESObjectBOOK>();
                    book && book->TeachesSpell()) {
                    bagAllowed = false;   // spell tome: right-click = learn
                }
                if (const auto* armo = g_sel->As<RE::TESObjectARMO>()) {
                    using SB = RE::BGSBipedObjectForm::BipedObjectSlot;
                    if (armo->HasPartOf(SB::kRing) || armo->HasPartOf(SB::kAmulet)) {
                        bagAllowed = false;   // jewelry
                    }
                }
            }
            if (!bagAllowed && g_cur.bag != 0) {   // sanitize stale overrides
                g_cur.bag = 0;
                chLayout = true;
            }
            bool isBag = g_cur.bag != 0;
            // themed checkbox: dark ground + accent hover (kills the stock blue)
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Acc(0.08f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::Acc(0.14f));
            ImGui::PushStyleColor(ImGuiCol_CheckMark, Theme::S().hi);
            ImGui::BeginDisabled(!bagAllowed);
            if (ImGui::Checkbox(Lang::T(Lang::Str::Bag), &isBag)) {
                g_cur.bag = isBag ? 1 : 0;
                chLayout = true;
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(4);

            auto rightTrack = [&](const char* lbl, int& v, float y) {
                ImGui::SetCursorScreenPos(ImVec2(colX, y + 3.0f * S));
                ImGui::TextColored(Theme::S().inkDim, "%s", lbl);
                const ImVec2 tp(colX + lblW, y);
                TrackChrome(tp, trackW, ImGui::GetFrameHeight(), (v - 1) / 9.0f);
                ImGui::SetCursorScreenPos(tp);
                Theme::PushChromeStyle(true);
                ImGui::SetNextItemWidth(trackW);
                const bool ch2 = ImGui::SliderInt((std::string("##bag") + lbl).c_str(), &v, 1, 10);
                Theme::PopChromeStyle(true);
                return ch2;
            };
            chLayout |= rightTrack("W", g_cur.bw, base.y + rowH);
            chLayout |= rightTrack("H", g_cur.bh, base.y + rowH * 2.0f);

            // continue below whichever column is taller
            const float rightBottom = base.y + rowH * 2.0f + ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(
                ImVec2(base.x, (std::max)(leftBottom, rightBottom) + 4.0f));
        }

        // ---- G3: per-item stack cap (0 = category default; gear is always 1) ----
        {
            const bool stackable = !(g_sel->Is(RE::FormType::Weapon) ||
                                     g_sel->Is(RE::FormType::Armor));
            if (!stackable && g_cur.stack != 0) {   // sanitize stale overrides
                g_cur.stack = 0;
                chLayout = true;
            }
            ImGui::BeginDisabled(!stackable);
            const float S0 = Theme::Scale();
            ImGui::TextColored(Theme::S().inkDim, "Stack");
            ImGui::SameLine(kLabelW * S0);
            TrackChrome(ImGui::GetCursorScreenPos(), kTrackW * S0,
                ImGui::GetFrameHeight(),
                g_cur.stack > 0 ? g_cur.stack / 100.0f : 0.0f);
            Theme::PushChromeStyle(false);
            ImGui::SetNextItemWidth(kTrackW * S0);
            if (ImGui::DragInt("##StackCap", &g_cur.stack, 0.25f, 0, 999,
                    g_cur.stack > 0 ? "%d" : "auto")) {
                chLayout = true;
            }
            Theme::PopChromeStyle(false);
            ImGui::SameLine();
            if (g_cur.stack > 0) {
                ImGui::TextColored(Theme::S().acc, "(override)");
            } else {
                ImGui::TextDisabled("(default)");
            }
            ImGui::EndDisabled();
        }
        ImGui::Separator();

        if ((chOrient || chLayout) && g_hooks.setOverride) {
            g_hooks.setOverride(g_sel, g_cur, false);   // live apply, zero IO
            if (chLayout) Grid::RequestRebuild();       // footprint: reflow
            else          Grid::RefreshDefs();          // orientation: recapture only
            MarkDirty();                                // ini write, debounced
        }
        ImGui::Separator();

        // ---- ⑥ actions ----
        if (Sfx::Button(Lang::T(Lang::Str::ResetDefault))) {
            g_dirty = false;
            if (g_hooks.resetOverride) g_hooks.resetOverride(g_sel);
            if (g_hooks.getEffective) g_cur = g_hooks.getEffective(g_sel);
            DefToPainter();
            Grid::RequestRebuild();
        }
        ImGui::SameLine();
        if (Sfx::Button(Lang::T(Lang::Str::SaveCategory))) {
            FlushDirty();
            if (g_hooks.saveAsCategory) g_hooks.saveAsCategory(g_sel, g_cur);
            Grid::RequestRebuild();
        }

        // property clipboard — same semantics as the offline tool: rotation,
        // scale and tiles travel; bag/stack stay per-item
        static bool    s_clipSet = false;
        static FullDef s_clip;
        if (Sfx::Button(Lang::T(Lang::Str::CopyProps))) {
            s_clip = g_cur;
            s_clipSet = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_clipSet);
        if (Sfx::Button(Lang::T(Lang::Str::PasteProps))) {
            g_cur.w = s_clip.w;
            g_cur.h = s_clip.h;
            g_cur.shape = s_clip.shape;
            g_cur.rx = s_clip.rx;
            g_cur.ry = s_clip.ry;
            g_cur.rz = s_clip.rz;
            g_cur.scale = s_clip.scale;
            DefToPainter();
            if (g_hooks.setOverride) g_hooks.setOverride(g_sel, g_cur, false);
            Grid::RequestRebuild();
            MarkDirty();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::End();
        ImGui::PopStyleVar();   // WindowPadding (torn-frame inset)
    }
}
