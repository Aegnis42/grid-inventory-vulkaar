#include "game/GoldCoins.h"
#include "ui/Editor.h"
#include "ui/Fallback.h"
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
        // ★★1.0.5: editing is a SESSION. Selecting an item takes a baseline;
        // every change applies to memory at once (so the icon updates as you
        // drag) but touches no file until SAVE. Leaving without saving puts
        // the baseline back.
        // The old model auto-wrote 0.5s after any change, which meant there
        // was no moment the player could point at as "committed" — and the
        // only way back was "Reset to default", which threw away every other
        // adjustment along with the one mistake.
        FullDef g_base;                // values this session started from
        bool    g_baseOverride = false;   // did the item HAVE an ini line then?
        bool    g_dirty = false;       // unsaved changes exist
        double  g_revertNote = 0.0;    // bottom-bar "discarded" until this time

        // ★★1.0.5 — 8, not 6. The socket overlay packs a footprint into a
        // uint64 (Badges::TileShape.cells = 8x8), so 8 is the ceiling the rest
        // of the code already lives with; the painter was the only thing
        // stopping at 6. Anything past 8 would mean widening that mask, and no
        // single item has business covering more than 64 cells.
        // Keep in step with IconStudio's painter (app.js shapeToCells).
        inline constexpr int kPaintN = 8;
        bool g_paint[kPaintN][kPaintN] = {};   // footprint painter cells
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
                    if (r < kPaintN && c < kPaintN) g_paint[r][c] = (ch == '1');
                    ++c;
                }
            } else {
                for (int r = 0; r < kPaintN && r < g_cur.h; ++r)
                    for (int c = 0; c < kPaintN && c < g_cur.w; ++c)
                        g_paint[r][c] = true;
            }
        }

        // painter -> trimmed shape/w/h (H2: full rectangle collapses to w/h)
        void PainterToDef()
        {
            int minR = kPaintN, maxR = -1, minC = kPaintN, maxC = -1;
            for (int r = 0; r < kPaintN; ++r)
                for (int c = 0; c < kPaintN; ++c)
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
        void MarkDirty() { g_dirty = true; }

        // SAVE: the one place a change reaches the file. The baseline moves up
        // to here, so from now on "revert" means back to what was just saved.
        void SaveSession()
        {
            if (!g_dirty || !g_sel || !g_hooks.setOverride) return;
            g_hooks.setOverride(g_sel, g_cur, true);   // persist to ini
            g_base = g_cur;
            g_baseOverride = true;                     // the line exists now
            g_dirty = false;
        }

        // LEAVE WITHOUT SAVING: put the item back exactly as it was found.
        // ★The two cases are not the same. If the item already had an ini
        // line, restoring means writing the baseline back to MEMORY (the file
        // still holds it, untouched). If it had none, the live-apply during
        // this session CREATED a memory override, and the way to undo that is
        // to drop it — writing the baseline back would leave the item carrying
        // an override it never had.
        void RevertSession()
        {
            if (!g_dirty || !g_sel) return;
            g_cur = g_base;
            if (g_baseOverride) {
                if (g_hooks.setOverride) g_hooks.setOverride(g_sel, g_base, false);
            } else if (g_hooks.resetOverride) {
                g_hooks.resetOverride(g_sel);
            }
            g_dirty = false;
            g_revertNote = ImGui::GetTime() + 2.5;
            Grid::RequestRebuild();
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

        // ★★Right-click a track to put THAT ONE field back to its default —
        // the same gesture the settings sliders answer to. It restores exactly
        // the number already printed at the end of the row, so there is nothing
        // new to learn, and "Reset to default" stays the button that resets
        // everything at once.
        // Call it immediately after the DragFloat, while IsItemHovered still
        // refers to it.
        // ★★Right-click a track to put THAT ONE field back to where this
        // editing session STARTED — not to the category default, which is what
        // the "Reset to default" button is for. Those are different questions:
        // "undo what I just did to this row" and "forget this item's tuning
        // entirely", and the row note below names whichever one applies.
        bool ResetOnRightClick(float& a_val, float a_base)
        {
            if (!ImGui::IsItemHovered()) return false;
            // the row prints the value itself; the hint only names the gesture
            UIRoot::NoteHoverHint(Lang::T(Lang::Str::HintRowRevert));
            if (!ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return false;
            a_val = a_base;
            return true;
        }

        // the "(was 90°)" / "(unchanged)" note that closes every editable row
        void RowNote(bool a_changed, const char* a_fmt, float a_base)
        {
            if (a_changed) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), a_fmt, Lang::T(Lang::Str::EditWas), a_base);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Chrome(0.85f)),
                    "%s", buf);
            } else {
                ImGui::TextDisabled("(%s)", Lang::T(Lang::Str::EditUnchanged));
            }
        }

        // drag row against the session baseline (H6 ⑤). DragFloat gives
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
            bool changed = ImGui::DragFloat((std::string("##") + a_label).c_str(),
                &a_val, 0.5f, -180.0f, 180.0f, "%.1f\xC2\xB0");
            Theme::PopChromeStyle(false);
            changed |= ResetOnRightClick(a_val, a_defVal);
            ImGui::SameLine();
            RowNote(std::fabs(a_val - a_defVal) > 0.5f, "(%s %.0f\xC2\xB0)", a_defVal);
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
            RevertSession();   // leaving EDIT without saving discards
            g_sel = nullptr;
            g_selKey.clear();
            IconCache::GetSingleton()->SetPin(nullptr);
        }
    }

    void Select(RE::TESBoundObject* a_obj, const std::string& a_key)
    {
        // ★Switching items DISCARDS unsaved work on the previous one — it used
        // to silently save it instead, which is the behaviour that made the
        // whole thing feel like it had no commit point.
        RevertSession();
        g_sel = a_obj;
        g_selKey = a_key;
        if (g_hooks.getEffective) g_cur = g_hooks.getEffective(a_obj);
        g_base = g_cur;
        g_baseOverride = g_hooks.hasOverride ? g_hooks.hasOverride(a_obj) : false;
        g_dirty = false;
        DefToPainter();
        // keep the selection's model loaded: rotation edits re-capture fast
        IconCache::GetSingleton()->SetPin(a_obj);
    }

    bool IsSelected(const std::string& a_key)
    {
        return g_editMode && !g_selKey.empty() && g_selKey == a_key;
    }

    bool HasUnsavedEdits() { return g_dirty && g_sel != nullptr; }
    bool DiscardNoteActive() { return ImGui::GetTime() < g_revertNote; }

    void OnMenuClosed()
    {
        RevertSession();   // ...and so does closing the whole menu
        g_editMode = false;
        g_sel = nullptr;
        g_selKey.clear();
        IconCache::GetSingleton()->SetPin(nullptr);
    }

    void DrawPanel()
    {
        if (!g_editMode) return;
        // (no debounce any more — nothing writes until SAVE)

        auto* wm = WinManager::GetSingleton();
        // ★Ask Theme directly. This used to read the cell size back out, on
        // the assumption that CellPx == 48 x Scale() — true until the board
        // got a scale of its own, after which the editor panel would have
        // shrunk along with the grid instead of with the text.
        const float s = Theme::Scale();
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
            ImVec2(Theme::PadX() + Theme::FrameInsetX(),
                   Theme::PadY() + Theme::FrameInsetY()));
        ImGui::Begin("##fablerim_editor", nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        wm->TitleBar("editor", Lang::SentenceCase(Lang::T(Lang::Str::Edit)).c_str());

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

        // (the category default is no longer read here — every row now compares
        //  against the session baseline; "Reset to default" fetches it itself)

        // ---- ① selection info (no thumbnail — the grid tile IS the preview) ----
        ImGui::BeginGroup();
        // ★The unsaved marker rides the NAME, not a corner of the panel: the
        // name is what the player is looking at while editing, and losing work
        // by closing the window is the failure this whole model introduces.
        if (g_dirty) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Val()),
                "%s  \xE2\x97\x8F %s", g_sel->GetName(), Lang::T(Lang::Str::EditUnsaved));
        } else {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Val()),
                "%s", g_sel->GetName());
        }
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
        // GI52: which pair is being edited follows the ICON STYLE. Three
        // angles orient a 3D model and mean nothing to a flat drawing; the
        // drawn icons get one angle and their own zoom, stored in their own
        // fields so switching styles back never finds the other's numbers.
        const bool drawnStyle = IconCache::GetSingleton()->FlatStyle();
        if (drawnStyle) {
            // ★What to name a custom drawing. The whole customisation story is
            // "drop a PNG in the folder", which only works if the name is
            // knowable — and it is not derivable by looking at the item (the
            // key comes from classification rules, and the per-item name is a
            // FormID spelling). So the editor just says it, for both levels,
            // and copies it. Without this row the feature needs a tool again.
            {
                const float S2 = Theme::Scale();
                // ★These two do NOT share the label column the sliders use. A
                // plugin name rides in the per-item one ("Grid Inventory.esp_
                // 0x000824.png"), so the value is routinely three times the
                // width of a "1.00" — putting it at the slider column ran it
                // straight through the label and off the panel. Own line,
                // indented, wrapped: length stops mattering.
                //
                // click-to-copy on a plain text line: hover/click resolve off
                // the last item's rect, so each line handles its own. (A
                // BeginGroup wrapper would put the tooltip on the group, whose
                // hover semantics are the one thing here worth not relying on.)
                // ★shown and COPIED are not the same string. The line has to
                // say "item\" or nobody knows which folder it goes in, but the
                // clipboard feeds a Save As box, where a stray folder prefix
                // makes the save land somewhere else or fail outright.
                auto copyLine = [&](const ImVec4& a_col, const std::string& a_shown,
                                    const std::string& a_copy) {
                    if (a_shown.empty()) return;
                    ImGui::PushStyleColor(ImGuiCol_Text, a_col);
                    ImGui::PushTextWrapPos(0.0f);   // wrap at the panel edge
                    ImGui::TextUnformatted(a_shown.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", Lang::T(Lang::Str::IconKeyHint));
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            ImGui::SetClipboardText(a_copy.c_str());
                        }
                    }
                };
                const std::string itemFile = Fallback::ItemFileName(g_sel);
                const std::string keyFile = Fallback::KeyFileName(g_sel);
                ImGui::TextColored(Theme::S().inkDim, "%s", Lang::T(Lang::Str::IconKeyLabel));
                ImGui::Indent(10.0f * S2);
                copyLine(ImGui::ColorConvertU32ToFloat4(Theme::Val()),
                    itemFile.empty() ? std::string{} : "item\\" + itemFile, itemFile);
                copyLine(Theme::S().inkDim, keyFile, keyFile);
                ImGui::Unindent(10.0f * S2);
                ImGui::Spacing();
            }
            chOrient |= AngleSlider("ROT", g_cur.frot, g_base.frot);
            {   // sideways nudge: rotating a drawing shifts where it LOOKS centred
                const float S1 = Theme::Scale();
                ImGui::TextColored(Theme::S().inkDim, "Off X");
                ImGui::SameLine(kLabelW * S1);
                TrackChrome(ImGui::GetCursorScreenPos(), kTrackW * S1,
                    ImGui::GetFrameHeight(), (g_cur.fx + 1.0f) * 0.5f);
                Theme::PushChromeStyle(false);
                ImGui::SetNextItemWidth(kTrackW * S1);
                if (ImGui::DragFloat("##OffX", &g_cur.fx, 0.005f, -1.0f, 1.0f, "%.2f")) {
                    chOrient = true;
                }
                Theme::PopChromeStyle(false);
                if (ResetOnRightClick(g_cur.fx, g_base.fx)) chOrient = true;
                ImGui::SameLine();
                RowNote(std::fabs(g_cur.fx - g_base.fx) > 0.005f, "(%s %.2f)", g_base.fx);
            }
        } else {
            chOrient |= AngleSlider("RX", g_cur.rx, g_base.rx);
            chOrient |= AngleSlider("RY", g_cur.ry, g_base.ry);
            chOrient |= AngleSlider("RZ", g_cur.rz, g_base.rz);
        }
        {
            const float S0 = Theme::Scale();
            float&      zoom = drawnStyle ? g_cur.fscale : g_cur.scale;
            const float zdef = drawnStyle ? g_base.fscale : g_base.scale;
            const float zmin = drawnStyle ? 0.2f : 0.05f;
            const float zmax = drawnStyle ? 4.0f : 20.0f;
            ImGui::TextColored(Theme::S().inkDim, "Scale");
            ImGui::SameLine(kLabelW * S0);
            TrackChrome(ImGui::GetCursorScreenPos(), kTrackW * S0,
                ImGui::GetFrameHeight(), (zoom - zmin) / (drawnStyle ? 3.8f : 1.95f));
            Theme::PushChromeStyle(false);
            ImGui::SetNextItemWidth(kTrackW * S0);
            if (ImGui::DragFloat("##Scale", &zoom, 0.01f, zmin, zmax, "%.2f")) {
                chOrient = true;
            }
            Theme::PopChromeStyle(false);
            if (ResetOnRightClick(zoom, zdef)) chOrient = true;
            ImGui::SameLine();
            RowNote(std::fabs(zoom - zdef) > 0.005f, "(%s %.2f)", zdef);
        }
        // ---- capture light -------------------------------------------------
        // ★The scene has ONE lamp, so an item's brightness is decided by which
        // face it turns toward it. Rather than hunt for a rotation that is both
        // recognisable AND lit, move the lamp for this item. Only the 3D styles
        // use it — a drawn icon is flat art.
        // ★★These are offsets from wherever SETTINGS > ICONS > CAPTURE LIGHT
        // has aimed the rig, not from the shipped angle, so 0/0 means "whatever
        // the global says" and stays true after the global moves. Tune the
        // global first for the whole set; come here only for the items that
        // still read badly under it.
        if (!drawnStyle) {
            const float S3 = Theme::Scale();
            auto lightRow = [&](const char* a_label, float& a_val, float a_base,
                                float a_lo, float a_hi) {
                ImGui::TextColored(Theme::S().inkDim, "%s", a_label);
                ImGui::SameLine(kLabelW * S3);
                TrackChrome(ImGui::GetCursorScreenPos(), kTrackW * S3,
                    ImGui::GetFrameHeight(), (a_val - a_lo) / (a_hi - a_lo));
                Theme::PushChromeStyle(false);
                ImGui::SetNextItemWidth(kTrackW * S3);
                bool ch = ImGui::DragFloat((std::string("##") + a_label).c_str(),
                    &a_val, 0.5f, a_lo, a_hi, "%.0f\xC2\xB0");
                Theme::PopChromeStyle(false);
                ch |= ResetOnRightClick(a_val, a_base);
                ImGui::SameLine();
                RowNote(std::fabs(a_val - a_base) > 0.5f, "(%s %.0f\xC2\xB0)", a_base);
                return ch;
            };
            if (lightRow("Lgt X", g_cur.lightAz, g_base.lightAz, -180.0f, 180.0f)) {
                chOrient = true;
            }
            if (lightRow("Lgt Y", g_cur.lightEl, g_base.lightEl, -80.0f, 80.0f)) {
                chOrient = true;
            }
        }
        ImGui::Separator();

        // ---- ④+⑤ footprint painter (left) + bag column (right): ONE row,
        // caption under the painter — mockup edrow2 layout (v10.6) ----
        const float S = Theme::Scale();
        {
            // ★The painter grew from 6x6 to 8x8 but its BLOCK did not: 180px
            // was tuned against the bag column beside it, so the cell shrinks
            // to keep the same total. 22.5 * 8 == 30 * 6.
            constexpr float kPaintBlock = 180.0f;
            const float cell = kPaintBlock / static_cast<float>(kPaintN);
            auto* dl = ImGui::GetWindowDrawList();
            const float availW = ImGui::GetContentRegionAvail().x;
            const ImVec2 base = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##painter", ImVec2(kPaintBlock, kPaintBlock));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int hc = static_cast<int>((mouse.x - base.x) / cell);
            const int hr = static_cast<int>((mouse.y - base.y) / cell);
            const bool inCell = hovered && hc >= 0 && hc < kPaintN &&
                                hr >= 0 && hr < kPaintN;

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
            // ★the painted footprint and the open-bag tile mean the same
            // thing — "this is the one" — so they are the same colour
            const ImU32 onCol = skp.lightPanel    ? Theme::Col(skp.bagOpen, 0.85f)
                              : skp.diamondLabels ? Theme::Col(skp.sel, 0.60f)
                                                  : Theme::Acc(0.55f);
            for (int r = 0; r < kPaintN; ++r) {
                for (int c = 0; c < kPaintN; ++c) {
                    const ImVec2 p0(base.x + c * cell, base.y + r * cell);
                    const ImVec2 p1(p0.x + cell - 2, p0.y + cell - 2);
                    dl->AddRectFilled(p0, p1,
                        g_paint[r][c] ? onCol : IM_COL32(0, 0, 0, 90), skp.rounding);
                    dl->AddRect(p0, p1, Theme::Acc(0.30f), skp.rounding);
                }
            }

            // caption BELOW the painter (mockup .pnote)
            ImGui::SetCursorScreenPos(ImVec2(base.x, base.y + kPaintBlock + 6.0f));
            ImGui::TextDisabled("%s · %dx%d%s", Lang::T(Lang::Str::FootprintHint),
                g_cur.w, g_cur.h, g_cur.shape.empty() ? "" : " (shape)");
            const float leftBottom = ImGui::GetCursorScreenPos().y;

            // ---- right column: Bag / W / H ----
            // stretches to the avail edge — the window padding now carries
            // the torn inset, so this stops at the proper right margin
            const float colX = base.x + kPaintBlock + 16.0f;
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
            ImGui::PushStyleColor(ImGuiCol_CheckMark, Theme::ValVec());
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
                // ★1..16, matching the def clamp (ui/ItemDef.h). At 1..10 this
                // slider was a QUIET editor: opening EDIT on a 10x14 bag pulled
                // it down to 10 and saved that, so simply looking at the bag
                // shrank it.
                constexpr int kBagMax = 16;
                const ImVec2 tp(colX + lblW, y);
                TrackChrome(tp, trackW, ImGui::GetFrameHeight(),
                    static_cast<float>(v - 1) / static_cast<float>(kBagMax - 1));
                ImGui::SetCursorScreenPos(tp);
                Theme::PushChromeStyle(true);
                ImGui::SetNextItemWidth(trackW);
                const bool ch2 = ImGui::SliderInt((std::string("##bag") + lbl).c_str(),
                                                  &v, 1, kBagMax);
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
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Theme::Chrome(0.85f)), "(override)");
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
        // ★SAVE leads, and it is the only control that writes. Lit while there
        // is something to write so the unsaved state is visible from the one
        // place that resolves it.
        // ★★Latch the state BEFORE drawing. The button's own action clears
        // g_dirty, so testing it again after the call took the other branch and
        // left two PushStyleColor unpopped for that frame — ImGui repairs the
        // stack at end-of-frame, which is why it showed up as chrome flashing
        // once per click instead of as a hard failure.
        // Any push/pop pair that straddles a widget has to read a LATCHED copy,
        // never the live state the widget can change.
        const bool wasDirty = g_dirty;
        if (wasDirty) {
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::BtnOn());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::BtnOnInkVec());
        }
        ImGui::BeginDisabled(!wasDirty);
        if (Sfx::Button(Lang::T(Lang::Str::EditSave))) SaveSession();
        ImGui::EndDisabled();
        if (wasDirty) ImGui::PopStyleColor(2);
        ImGui::SameLine();

        // ★Reset asks a different question from the right-click revert: it
        // throws away this item's tuning ENTIRELY and goes back to whatever
        // the category says. It stays a session edit — nothing is written
        // until Save, so it can itself be undone by leaving without saving.
        if (Sfx::Button(Lang::T(Lang::Str::ResetDefault))) {
            const FullDef d = g_hooks.getDefault ? g_hooks.getDefault(g_sel) : FullDef{};
            g_cur = d;
            if (g_hooks.setOverride) g_hooks.setOverride(g_sel, g_cur, false);
            DefToPainter();
            Grid::RequestRebuild();
            MarkDirty();
        }
        ImGui::SameLine();
        // ★Only a SAVED item may set the category default. Pushing a value
        // that is not yet committed out to every item in its category is the
        // one action here that reaches past the item in front of you.
        // same latching rule — this one does not currently change g_dirty, but
        // pairing a Begin/End against live state is the shape of the bug above
        ImGui::BeginDisabled(wasDirty);
        if (Sfx::Button(Lang::T(Lang::Str::SaveCategory))) {
            if (g_hooks.saveAsCategory) g_hooks.saveAsCategory(g_sel, g_cur);
            Grid::RequestRebuild();
        }
        ImGui::EndDisabled();
        if (wasDirty && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            UIRoot::NoteHoverHint(Lang::T(Lang::Str::EditUnsaved));
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
            g_cur.fscale = s_clip.fscale;   // GI52: the drawn pair travels too
            g_cur.frot = s_clip.frot;
            g_cur.fx = s_clip.fx;
            // the lamp travels with the orientation it was tuned against —
            // pasting a rotation without its light would hand the copy a pose
            // that was only legible under the angle left behind
            g_cur.lightAz = s_clip.lightAz;
            g_cur.lightEl = s_clip.lightEl;
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
