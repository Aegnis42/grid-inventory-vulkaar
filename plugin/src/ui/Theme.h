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
        // ★The frame is DRAWN now, not sampled — see Theme::TornPanel. Its two
        // texture-era companions (tornGlowB, tornTex) were removed with the
        // four .fic sheets they used to select between: nothing picks a torn
        // variant any more because there is only one, and its silhouette comes
        // from noise rather than from a bitmap.
        bool  tornFrame = false;       // procedural torn-paper panel (window bg)
        // translucent windows (skins 5/6) are safe because the capture path
        // saves/restores the FULL backbuffer frame (ItemPreview Render step
        // 1/5), so the parked model never survives onto the visible frame
        bool  translucent = false;   // respect winBg alpha (others force 1.0)
        bool  bevelChrome = false;   // gradient titlebar + dark/light bevel border

        // ★The panel is LIGHT, and that inverts a rule this UI was built on.
        // Every "emphasis" here is a BRIGHTER colour (hi) over a dark ground —
        // gauge fills, slider values, stat numbers. Over a light panel there is
        // no headroom above it, so those all fade into the panel instead of
        // standing out: the sliders read as empty tracks and the numbers as
        // disabled text. Emphasis has to go the other way, toward acc.
        // `translucent` cannot carry this — skins 5/6 are translucent AND dark.
        bool  lightPanel = false;

        // ── SIMPLE grammar (skin 7) ────────────────────────────────────────
        // ★Cells are separated by a carved GROOVE, not a hairline: a dark gap
        // with a 1px light line down its middle, plus a 1px dark shadow inside
        // each cell's top-left. The light centre line is what makes it read as
        // carved — a single flat line cannot, at any colour.
        bool   engravedCells = false;
        // Every token below defaults to ALPHA 0 = "this skin does not use it",
        // and each draw site falls back to exactly what it did before. That is
        // why six existing skins need no edits at all.
        ImVec4 cellBg{ 0.0f, 0.0f, 0.0f, 0.0f };        // empty-cell fill
        ImVec4 cellGroove{ 0.0f, 0.0f, 0.0f, 0.0f };    // groove, dark
        ImVec4 cellGrooveLt{ 0.0f, 0.0f, 0.0f, 0.0f };  // groove, centre line
        ImVec4 btnFace{ 0.0f, 0.0f, 0.0f, 0.0f };       // button face
        // 규칙 96: the open bag's tile. Was a file-local constant in Grid.cpp;
        // the default IS that constant, so nothing moves for the other skins.
        ImVec4 bagOpen{ 122.0f / 255.0f, 154.0f / 255.0f, 122.0f / 255.0f, 64.0f / 255.0f };
        // The money figure. Every other number in this UI is "a value"; this
        // one is money, and the reference gives it its own colour rather than
        // the shared emphasis. Alpha 0 = follow Val() like everything else.
        ImVec4 goldNum{ 0.0f, 0.0f, 0.0f, 0.0f };
        // Window title size. 15 was picked against the atlas's 17px body
        // text; a skin may want the two closer together.
        // ★Defaulted fields go at the END. Every skin above is a POSITIONAL
        // initialiser, so a new member in the middle silently feeds the next
        // value into it and shifts all six of them by one.
        // ★24 for every skin. The bar is 34px tall and the title is centred in
        // it (WinManager), so one size gives them all the same 5px above and
        // below — the older 15 left the dark skins with a caption-sized title
        // floating in a bar built for something bigger.
        float  titleSize = 24.0f;

        // ── light-panel palette ────────────────────────────────────────────
        // ★Read only when lightPanel is set, and ALPHA 0 means "use the
        // built-in value" — so SIMPLE names none of these and is unchanged.
        // They exist because the light-panel machinery was written for ONE
        // skin and had its blues compiled in; a second light skin (the
        // parchment one) shares every rule and none of the colours.
        ImVec4 lpBtn{};        // idle button face
        ImVec4 lpBtnHov{};
        ImVec4 lpBtnAct{};
        ImVec4 lpBorder{};     // button + frame border
        ImVec4 lpBtnOnFace{};  // a toggled-ON button
        ImVec4 lpBtnOnInk{};   // ink on that face
        ImVec4 lpRule{};       // dividers
    };

    // Gap between cells (px at scale 1). ★It is no longer a carved groove —
    // nothing is painted there, the panel simply shows through — so the width
    // is now about how far apart the tiles sit, not how deep the cut looks.
    // Each of the two neighbours gives up half of it.
    inline constexpr float kGrooveW = 2.0f;

    // 9-slice source margins for the torn-paper frame texture, as a fraction
    // of the image. Corners fixed, edges stretch one way, centre both.
    //
    // ★★These MUST match kTornDest below in real pixels, or the corner and
    // edge slices are squeezed on their way to the screen. The previous pair
    // (0.1663 / 0.2901 = 106 x 222 px of a 640x766 image, drawn into 30 px)
    // crushed them 3.55x across and 7.41x down — and because the two factors
    // differ by 2.09x, the four corners were sheared as well as flattened.
    // That is what made the torn edge read as a smudge rather than as paper.
    //
    // ★The old numbers came from the 99.5th percentile of the alpha fringe,
    // i.e. the single longest whisker of torn paper. Measured properly, the
    // tearing is over by 27-38 px on every side; everything past that was
    // FLAT SHEET being folded into the corner slice. 48 px covers the ragged
    // band with room to spare and leaves the flat middle to stretch, which is
    // the one part that can stretch without showing.
    inline constexpr float kTornPx = 48.0f;    // the band, in source pixels
    inline constexpr float kTornUM = kTornPx / 640.0f;   // = 0.0750
    inline constexpr float kTornVM = kTornPx / 766.0f;   // = 0.0627
    // ★★Head-room OUTSIDE the window rect for the drawn tearing to live in.
    // The sheet itself is the window rect exactly — the teeth stick out past
    // it — so this only has to clear the deepest bite (TornDepth peaks at
    // ~9.2px) plus a margin.
    // ★It is NOT the sheet's size. Making the clip and the sheet the same
    // rectangle is what produced a perfectly straight edge on the first try:
    // every tooth was drawn and then clipped away by the very box it was
    // supposed to escape.
    inline constexpr float kTornOut = 14.0f;

    // H′: global UI scale (0.5~1.6), persisted as "!uiscale".
    [[nodiscard]] float Scale();
    void SetScale(float a_scale);

    // ★★The BOARD's own scale, on top of Scale() — persisted as "!cellscale".
    // Measured on the main window: the item grid and the equipment doll are
    // both built out of cell-sized blocks (10x14 cells; every slot is 2x2 of
    // them, weapons 2x4), and together they are 97% of the window's width.
    // Padding is 4%. So "make the window smaller without shrinking the text"
    // has exactly one lever, and this is it — Scale() moves type, buttons and
    // spacing, CellScale() moves the board and the doll.
    // ★It multiplies Scale(), so the two compose: a player who wants
    // everything smaller still just moves the UI scale.
    [[nodiscard]] float CellScale();
    void SetCellScale(float a_scale);

    // ── GI59: glow + icon light are stored PER ICON STYLE ──────────────────
    // Slot 0 = realistic (3D captures), 1 = drawn (flat art). They need
    // different light, so each keeps its own numbers.
    //
    // ★The no-argument forms act on the ACTIVE style. PERSISTENCE MUST NOT USE
    // THEM: the ini is read line by line and "!glowstyle" comes before
    // "!iconstyle", so at load time the active style is not known yet and the
    // value would land in the wrong slot. Save/load through the *Of / *At
    // forms, which name the slot outright.
    [[nodiscard]] int IconStyleSlot();   // 0 realistic / 1 drawn (active)

    // rarity glow style: 1 = silhouette halo (blurred icon alpha) — default,
    // 0 = stretched radial across the footprint (previous look, kept for revert)
    // ★★1.0.5: every setting below is stored per SKIN as well. The no-argument
    // forms act on the live skin and live icon style — UI call sites use those
    // and none of their signatures changed. The *Of / *At forms take a 1-BASED
    // skin index first and are what persistence walks.
    // ★★1.0.5 ITEM SHADOW — three plain numbers, because that is what a drop
    // shadow is everywhere else: how far it falls, how soft it is, how dark.
    //   axis 0  DISTANCE px  — offset toward the lower right; 0 = ambient
    //   axis 1  BLUR px      — how far the edge spreads; 0 = a hard silhouette
    //   axis 2  OPACITY 0..1 — the black's alpha
    // Stored per skin AND per icon style, like every other DISPLAY row.
    //
    // ★These replaced a SHADOW + SHADOW STYLE pair in which "Soft" and "Sharp"
    // were an implementation detail wearing a setting's clothes — one path
    // sampled a baked 96px silhouette, the other stamped the sprite. The blur
    // is drawn at full sprite resolution now, so the shape follows the number
    // instead of the number picking between two shapes.
    [[nodiscard]] float ShadowDist();
    [[nodiscard]] float ShadowBlur();
    [[nodiscard]] float ShadowOpacity();
    [[nodiscard]] float ShadowAxis(int a_axis);         // the same three, by index
    void SetShadowAxis(int a_axis, float a_v);          // acts on the live pair
    [[nodiscard]] float ShadowAt(int a_skin, int a_slot, int a_axis);
    void SetShadowAt(int a_skin, int a_slot, int a_axis, float a_v);

    // ---- legacy, kept for the ini format only -------------------------------
    // ★The rarity halo these drove is gone (rarity is a corner wedge) and so is
    // the shadow style that briefly borrowed the slot. Nothing READS them any
    // more; they stay so the "!disp<skin>" line keeps its 13 fields and an ini
    // written by any 1.0.x build still parses. Do not add call sites.
    [[nodiscard]] int  GlowStyle();
    void SetGlowStyle(int a_style);
    [[nodiscard]] int  GlowStyleOf(int a_skin, int a_slot);
    void SetGlowStyleOf(int a_skin, int a_slot, int a_style);

    // rarity glow brightness (0.2~2.5). Kept per GLOW style as well, so the
    // full key is [icon style][glow style]. GlowGain()/SetGlowGain() act on the
    // active pair; *Of names the glow style within the active icon style; *At
    // names both and is what persistence uses. Scales per-rarity tint alphas.
    [[nodiscard]] float GlowGain();
    void SetGlowGain(float a_gain);
    [[nodiscard]] float GlowGainOf(int a_style);
    void SetGlowGainOf(int a_style, float a_gain);
    [[nodiscard]] float GlowGainAt(int a_skin, int a_slot, int a_style);
    void SetGlowGainAt(int a_skin, int a_slot, int a_style, float a_gain);

    // item icon brightness (0.4~1.6). Applied LIVE at draw time by
    // UIRoot::DrawItemIcon: <=1 darkens via tint, >1 brightens via an extra
    // additive-blend pass (a tint alone cannot go up).
    [[nodiscard]] float IconGain();
    void SetIconGain(float a_gain);
    [[nodiscard]] float IconGainOf(int a_skin, int a_slot);
    void SetIconGainOf(int a_skin, int a_slot, float a_gain);

    // ★★1.0.5 CAPTURE LIGHT — where the menu scene's single lamp sits while
    // an icon is being photographed, as an OFFSET in degrees from the shipped
    // rig. NOT per skin: the capture is a photograph of the model, and the
    // skin it is later drawn under does not change it. Storing it per skin
    // would invalidate every icon on a skin switch, which is the one thing a
    // skin switch must stay cheap enough to do freely.
    //
    // The SAME units and origin as an item def's own lightAz/lightEl, so the
    // final angle is base + this + the item's. Move this and every untuned
    // item follows; the handful that were tuned keep their relative offset.
    [[nodiscard]] float CaptureLightAz();
    [[nodiscard]] float CaptureLightEl();
    void SetCaptureLight(float a_azDeg, float a_elDeg);

    // ★The ICON STYLE is the skin's too, and Theme owns the number: it hands
    // the choice to IconCache whenever it changes or a skin is entered. Call
    // SetIconStyle instead of IconCache::SetStyle, or the pick is not saved.
    // Raw values are IconCache::Style's (0 realistic / 2 drawn / 3 pixel).
    [[nodiscard]] int  IconStyleOf(int a_skin);
    void SetIconStyleOf(int a_skin, int a_style);
    void SetIconStyle(int a_style);   // the live skin

    // active skin (1-based), persisted as "!skin2"
    [[nodiscard]] int  SkinIndex();
    void SetSkin(int a_index);            // re-applies the ImGui style
    // ★A number saved before "Fable Amber" was removed. Files written by an
    // older build say "!skin"; every skin after the removed one shifted down
    // by one, so reading such a file raw would hand the player a DIFFERENT
    // skin — silently, and on the very first launch after updating. The key
    // was renamed for exactly this reason: "!skin" means "needs converting",
    // "!skin2" means "already in the new numbering".
    void SetSkinLegacy(int a_oldIndex);
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
    // The EMPHASIS colour for values, numbers and active chrome text. `hi` on
    // a dark panel, `acc` on a light one — see Skin::lightPanel. Call sites
    // that mean "make this stand out" use this instead of naming hi directly.
    [[nodiscard]] ImU32 Val(float a_alpha = 1.0f);
    [[nodiscard]] const ImVec4& ValVec();   // the same choice, unconverted
    // The money figure — Skin::goldNum when the skin sets one, Val() otherwise.
    [[nodiscard]] ImU32 GoldCol();
    // Chrome text (window titles, section headings, title-bar controls). `acc`
    // over a dark panel; over a LIGHT one acc is the darkest thing on screen
    // and these stop reading as headings — they read as borders.
    [[nodiscard]] ImU32 Chrome(float a_alpha = 1.0f);
    [[nodiscard]] ImU32 Col(const ImVec4& a_c, float a_alpha = -1.0f);

    // Window side padding, at scale 1. ★Every window has to ask HERE, not
    // hard-code 12: the managed windows size themselves from the padding
    // (width = pad + content + pad) and three of them push their own
    // WindowPadding on top. Changing only the style value moved nothing —
    // the four sites disagreed and the hard-coded ones won.
    [[nodiscard]] float PadX();
    [[nodiscard]] float PadY();

    // ── rounding ───────────────────────────────────────────────────────────
    // Windows and frames round by DIFFERENT amounts: 6px on a 700px window is
    // a soft corner, 6px on a 17px button is a lozenge. Skin::rounding stays
    // the single knob for skins that want one value everywhere.
    [[nodiscard]] float WinRounding();
    [[nodiscard]] float FrameRounding();

    // The window frame, and how far a snapped window overlaps its neighbour.
    // ★The frame is OPAQUE on purpose. Two translucent frames stacked on one
    // pixel row blend to a darker line, which is exactly the doubled seam the
    // overlap is meant to remove — opaque, drawing the same colour twice
    // lands on the same value, so a docked pair reads as one line.
    [[nodiscard]] ImU32 WinBorder();
    [[nodiscard]] float BorderPx();        // stroke width at the current scale
    [[nodiscard]] float BorderOverlap();   // snap overlap = one stroke, 0 if unframed

    // Ink for a button that is ON — its face is the brightest thing in the
    // skin, so white would sit on white. Dark on light, the way round the
    // rest of this panel now works.
    [[nodiscard]] ImU32 BtnOnInk();
    [[nodiscard]] ImVec4 BtnOnInkVec();

    // Window TITLE ink. Split from Chrome(): the title keeps white + a black
    // outline (21:1 whatever the panel does), while every other piece of
    // chrome went to dark ink. They used to share one colour.
    [[nodiscard]] ImU32 TitleInk();

    // The face of a button that is ON. Skins without a light panel keep the
    // accent wash they always had.
    [[nodiscard]] ImU32 BtnOn(float a_alpha = 1.0f);

    // ── item tooltip ───────────────────────────────────────────────────────
    // ★The tooltip stops being "the panel, floating". Over a light panel a
    // tooltip painted in the same blue has no edge at all — it lands on the
    // window it describes and dissolves. It goes DARK instead, and then each
    // kind of fact gets its own colour: heading, value, benefit, restriction.
    // Skins without a light panel keep the tokens they always used.
    [[nodiscard]] const ImVec4& TipHead();   // section label
    [[nodiscard]] const ImVec4& TipVal();    // name, numbers
    [[nodiscard]] const ImVec4& TipGood();   // temper, enchantment
    [[nodiscard]] const ImVec4& TipBad();    // "cannot", overload
    [[nodiscard]] const ImVec4& TipSub();    // hints, shop price
    [[nodiscard]] const ImVec4& TipBody();   // running text
    // push/pop around BeginTooltip: dark ground + pale hairline + body ink
    void PushTipStyle();
    void PopTipStyle();

    // Draw text with the 4-way 1px black outline the window title uses.
    // ★For the strings the eye SEEKS — values, totals, money. White on this
    // panel measures 3.30:1 on its own; against its own black edge it is 21:1
    // whatever the panel does. Reading strings do NOT get this: an outline
    // thickens the stroke, and a settings panel full of 9px labels goes heavy.
    // a_size 0 = the current font size; a_spacing adds tracking, which needs
    // the glyphs drawn one at a time (section labels use it).
    void TextOutlined(ImDrawList* a_dl, ImVec2 a_pos, ImU32 a_col, const char* a_text,
                      float a_size = 0.0f, float a_spacing = 0.0f);
    [[nodiscard]] ImVec2 TrackedSize(const char* a_text, float a_size, float a_spacing);

    // ── one lighting model ─────────────────────────────────────────────────
    // ★The light comes from the TOP-LEFT, everywhere: buttons rise (lit top
    // and left, shaded bottom and right) and grid cells sink — the same light
    // with the sign flipped. One rule, so the skin reads as a material rather
    // than as coloured shapes.
    // ★The WINDOW is the exception, and deliberately: it takes the lit line on
    // ALL FOUR sides. A dark line down a large face's bottom and right reads
    // as a second frame, not as depth — the window wants an edge, not a
    // direction. So BevelShd(true) exists for symmetry and is not used.
    // ★a_window also drops the strength to about two thirds. The same alpha
    // reads FAR stronger there — the eye follows a longer edge, and a
    // translucent panel sets its light line against the dark game world
    // instead of against its own face.
    [[nodiscard]] ImU32 BevelLit(bool a_window = false);
    [[nodiscard]] ImU32 BevelShd(bool a_window = false);


    // ★★Centre a whole LABEL in a rect by its ink. ImGui centres the line
    // BOX, which reserves descender room whether or not the string has one —
    // so "EDIT" (all caps, no descender) rides high inside its button and "+"
    // (x-height, no ascender either) sits low. Every label is off by a
    // different amount, which is why no single nudge can straighten them: the
    // amount is a property of the STRING, not of the button.
    // Measures the tallest ascent and deepest descent actually present.
    void TextInkCentered(ImDrawList* a_dl, const ImVec2& a_p0, const ImVec2& a_p1,
                         ImU32 a_col, const char* a_text, float a_size = 0.0f);

    // ★★Does text on this skin want a black edge behind it?
    // The outline exists to lift LIGHT ink off a mid-tone panel — SIMPLE's
    // white numbers measure 3.30:1 on their own and 21:1 against their own
    // edge. Dark ink on a PALE panel is already the strongest thing there
    // (parchment: 8.10:1), so an edge in the same value range as the ink just
    // fattens every stroke until the word reads as a blob.
    // Asked of the ink itself, so a new light skin needs no flag — and all
    // three places that outline text (body, window title, count badge) give
    // the same answer.
    [[nodiscard]] bool InkNeedsOutline();

    // ★The ground under an occupied cell — the board, the equipment doll and
    // the partner window must all ask HERE. They used to each reach for
    // sk.shade with their own alpha (doll 1.0, board the token's own), so one
    // colour said "occupied" loudly on one half of the window and almost
    // nothing on the other.
    [[nodiscard]] ImU32 OccupiedGround();

    // ── type scale ─────────────────────────────────────────────────────────
    // ★Four steps, and the title is the only maximum. They were all 20 —
    // heading, label, value and button — so nothing had a size of its own and
    // the close x, whose multiplier still assumed a 17px body, came out at 31
    // and outgrew the title it sat beside.
    [[nodiscard]] float SnapPx(float a_size);   // whole-pixel: see Theme.cpp
    [[nodiscard]] float FontValue();    // numbers the eye seeks
    [[nodiscard]] float FontBody();     // labels, buttons, running text
    [[nodiscard]] float FontCaption();  // section headings, family names

    // Horizontal rules between stat blocks (and ImGui's own separators).
    // acc over a dark panel; over a LIGHT one acc is the darkest thing on
    // screen, so the rule stops reading as a divider and reads as a crack.
    [[nodiscard]] ImU32 Rule();

    // ★The Plain()/kPlainAlpha tagging that used to live here is gone with the
    // frame-wide outline pass it existed for. It marked widget text by setting
    // alpha 254 so the pass would skip it; once every string that wants an edge
    // asks for one directly (Theme::TextOutlined), nothing reads the tag and
    // the alpha is simply 255 like every other skin's already was.

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

    // ★★A torn-paper panel, DRAWN — no texture, so the tearing is the same
    // size in a 2x2 bag as in the main window. The nine-slice it replaces had
    // no way to be: it stretches its edge strips, which carry the torn
    // silhouette, so a small window compressed them (0.16x on an Ore Sack) and
    // the paper turned into a row of needles. Cropping instead only moved the
    // problem to the seam.
    // The edge is a 1-D function of distance travelled — three octaves of
    // value noise plus a rare deeper bite — and each 3px step becomes one
    // trapezoid, which is convex and therefore drawable as-is.
    // a_seed: derive it from the window key so each window keeps its own
    // silhouette and it never changes as the window moves or resizes.
    void TornPanel(ImDrawList* a_dl, const ImVec2& a_min, const ImVec2& a_max,
                   ImU32 a_col, unsigned int a_seed);

    // Design-pass A: the ONE quantity-slider look (settings track chrome —
    // black .2 ground, acc .20 fill, acc .25 border, centred hi value) shared
    // by the quantity popup, the pouch withdraw and any future int slider.
    // Draws at the current cursor, a_w wide, frame height tall.
    bool ChromeSliderInt(const char* a_id, int* a_v, int a_min, int a_max,
                         float a_w, const char* a_fmt = "%d");

    // float twin, absolute position tracking (the settings rows previously
    // used DragFloat under slider-looking chrome — dragging felt broken
    // because the value crawled by drag-speed instead of following the mouse)
    // ★a_def: right-click restores it. Pass a negative to opt out. ImGui has
    // no such gesture of its own, and the settings rows have no room for the
    // EDIT panel's "(def 1.00)" column — the caller puts the wording in the
    // bottom bar instead, so the affordance costs no pixels.
    bool ChromeSliderFloat(const char* a_id, float* a_v, float a_min, float a_max,
                           float a_w, const char* a_fmt = "%.2f",
                           float a_def = -1.0f);

    // ★The defaults live HERE and the runtime values are initialised FROM
    // them, so "restore the default" and "what a fresh install had" cannot
    // drift apart. Glow and icon gain differ per style/slot, which is exactly
    // why a hard-coded 1.0 in the reset path would have been wrong.
    inline constexpr float kDefScale     = 1.00f;
    inline constexpr float kDefCellScale = 0.80f;
    // ★0/0 means "the shipped rig", not "no light" — the offset origin. That
    // is what makes a right-click reset here identical to a fresh install, and
    // what lets an item def's 0/0 mean "whatever the global says".
    inline constexpr float kDefCapLightAz = 0.0f;
    inline constexpr float kDefCapLightEl = 0.0f;
    [[nodiscard]] constexpr float DefGlowGain(int a_style) { return a_style == 0 ? 1.55f : 0.75f; }
    [[nodiscard]] constexpr float DefIconGain(int a_slot)  { return a_slot  == 0 ? 1.35f : 1.02f; }
    // ★★1.0.5 ITEM SHADOW defaults — the ambient-65% mockup the author picked,
    // stated in the same three numbers the sliders now expose.
    inline constexpr float kDefShadowDist = 0.0f;    // px, toward lower-right
    inline constexpr float kDefShadowBlur = 2.5f;    // px of edge spread
    inline constexpr float kDefShadowOpac = 0.65f;   // 0..1
    [[nodiscard]] constexpr float DefShadow(int a_axis)
    {
        return a_axis == 0 ? kDefShadowDist
             : a_axis == 1 ? kDefShadowBlur
                           : kDefShadowOpac;
    }
    // ...and the same two for whatever style/slot is live right now, which is
    // what a reset on the visible slider needs. Keeps the slot arithmetic in
    // one file instead of at every call site.
    [[nodiscard]] float DefaultGlowGain();
    [[nodiscard]] float DefaultIconGain();

    // Phase 2: the invisible-widget style for any Drag/Slider drawn OVER
    // custom track chrome (transparent frame + hi-coloured value text) —
    // formerly five hand copies across Editor and the Chrome sliders.
    // a_sliderGrab also hides the Slider* grab (Drag* widgets have none).
    void PushChromeStyle(bool a_sliderGrab);
    void PopChromeStyle(bool a_sliderGrab);
}
