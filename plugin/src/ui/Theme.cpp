#include "ui/Theme.h"

#include "ui/IconCache.h"   // IconStyleSlot(): which style's values are live

#include <imgui_internal.h>   // ImTextCharFromUtf8 (TextInkCentered)

#include <algorithm>
#include <cmath>

namespace FUI::Theme
{
    namespace
    {
        constexpr ImVec4 Rgba(int r, int g, int b, float a = 1.0f)
        {
            return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
        }

        // ── light-panel chrome fallbacks ───────────────────────────────────
        // What a light-panel skin gets when it names none of the lp* tokens.
        // These are SIMPLE's own values, which is why every theme derived from
        // it must name its own — silence here means "inherit blue".
        //
        // ★A `kDarkInk` used to sit alongside these, for labels and body text.
        // It is gone: Chrome() and ValVec() read the SKIN's ink now, so a
        // second dark ink would be a copy of a decision each skin already
        // made. On SIMPLE that ink is white with a black outline (21:1 over
        // any panel); on parchment it is the brown the body already uses.
        constexpr ImVec4 kBtnOnFace = Rgba(143, 211, 222); // active button face
        constexpr ImVec4 kBtnOnInk  = Rgba(14, 48, 56);    // ink ON that face
        constexpr ImVec4 kRuleInk   = Rgba(13, 32, 46);    // rules, drawn at .70

        // "this skin names it, else use the built-in" — the light-panel
        // palette defaults to SIMPLE's colours (alpha 0 = not named)
        [[nodiscard]] const ImVec4& LP(const ImVec4& a_skin, const ImVec4& a_def)
        {
            return a_skin.w > 0.0f ? a_skin : a_def;
        }

        // v9 mockup skin table (+ 5/6: light CREAM torn rebakes, dark ink)
        // ★No size on purpose: the SIMPLE family gained five colour variants in
        // 1.0.5 and a literal here is one more place to forget. SkinCount()
        // reads std::size of this table, and everything else reads SkinCount().
        const Skin kSkins[] = {
            // ★GI73: named by CHROME FAMILY first, colour second — Fable /
            // Parchment / Glass. The old names were grouped the other way
            // round, by colour, which hid the fact that Parchment is Fable
            // with a torn frame and that the two Glass skins differ only in
            // transparency. The frame is what the eye actually picks out; the
            // accent colour is the sub-choice inside it.
            // ★1.0.5: "Fable Amber" was removed at the author's request. It
            // and Parchment Amber carried IDENTICAL colours — only the frame
            // differed — so the amber look is not gone, it simply lives in one
            // place now. Saved numbers are migrated (SetSkinLegacy).
            {   // 1 Fable Crimson (v10.4) — 순흑 워밍 패널 + 코너-페이드
                // 테두리 + 상단 크림슨 스트립 + 글로우 타이틀 + ◇ 크림슨 라벨
                "Fable Crimson",
                Rgba(230, 226, 216), Rgba(245, 242, 234), Rgba(220, 216, 206),
                Rgba(220, 216, 206, 0.45f), Rgba(32, 32, 32),   // 목업 #202020
                Rgba(236, 232, 222), Rgba(0, 0, 0, 0.5f),
                Rgba(168, 64, 47), Rgba(230, 226, 216),
                0.0f, 5.0f,
                true, true, true, true,   // cornerFade / topStrip / titleGlow / diamondLabels
            },
            {   // 2 Parchment Amber — REAL parchment: a pale sheet in a torn
                // frame, dark ink on it.
                // ★★The frame was always parchment; the panel behind it was
                // near-black, which read as a parchment mount with black paper
                // in it. Warming the panel helped but never made it PAPER —
                // paper is light, and everything written on light paper is
                // dark. So this skin flips: lightPanel and dark ink.
                // ★Nothing here is new machinery. SIMPLE already carries the
                // whole light-panel grammar; it simply had its blues compiled
                // in. Those are skin tokens now (lp*), and this is the second
                // skin to use them.
                "Parchment Amber",
                Rgba(110, 85, 53),                      // acc    lines, hairline grid
                Rgba(122, 90, 18),                      // hi     (unused on a light panel)
                Rgba(58, 46, 30),                       // ink    the writing — dark
                Rgba(58, 46, 30, 0.70f),                // inkDim
                // ★winBg finally reaches the screen. It never did on a torn
                // skin — the texture was the window and this value was dead.
                // ★★Every earlier value here was tuned against a ReShade-
                // tinted screen. With the preset off the sheet measured
                // #998D76 — the specified colour exactly, and 15% saturation,
                // which is grey card, not paper. The post-process had been
                // adding the warmth AND +23% brightness, so each "too bright"
                // correction darkened a colour that was never the problem.
                // This is the material colour: same hue, 29% saturation, and
                // light enough that ink on it reads as ink (6.1:1).
                // ★-6% off the first cream on sight. Everything below that is
                // derived from the sheet moves WITH it (see bagOpen and the
                // button family) so the relationships stay where they were
                // chosen; only the whole thing sits a step lower.
                Rgba(189, 174, 147),                    // winBg — cream parchment
                Rgba(120, 95, 60),                      // glyph  slot silhouettes
                Rgba(120, 95, 60, 0.18f),               // shade  occupied cell
                Rgba(150, 110, 40),                     // sel
                Rgba(122, 90, 18),                      // filled
                4.0f, 3.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                true,                                   // tornFrame
                false, false,                           // not translucent, no bevelChrome
                true,                                   // ★lightPanel
                false,                                  // hairline grid, not carved
                Rgba(120, 95, 60, 0.13f),               // cellBg — a seat for the item
                {}, {}, {},                             // groove/grooveLt/btnFace
                // ★bagOpen has to be OUT of family to work — it is the one
                // mark that says "this tile is the bag you have open", and a
                // warm brown on warm paper says nothing (it also collides
                // with `shade`, which already means "occupied").
                // ★★Out of family is not licence to be loud. This is painted
                // OVER `shade`, not over the sheet, and ink blue landed the
                // tile 42 luma below its neighbours while ALSO inverting the
                // hue — the tile read as a hole punched in the board.
                // Brightness is what shouts; hue is what distinguishes. Sage
                // sits at the luma of an ordinary occupied tile (+9) and
                // turns only the hue, which keeps the answer and drops the
                // shouting.
                Rgba(133, 154, 118, 0.85f),             // bagOpen — sage, iso-luminant
                Rgba(92, 64, 8),                        // goldNum — darker: the
                                                        // sheet is pale, so money
                                                        // reads by being DEEP
                24.0f,                                  // titleSize
                // light-panel palette: darker than the sheet, same family
                // ★These used to be LIGHTER than the sheet — the line above
                // described an intention, not the values — which only passed
                // unnoticed while the "sheet" was a dark card. On real paper a
                // control has to read as something pressed INTO it, so the
                // whole family moves below the sheet and the comment is now
                // true. Keep them in this order: btn < hov, act < btn.
                Rgba(162, 141, 102),                    // lpBtn
                Rgba(171, 152, 117),                    // lpBtnHov
                Rgba(145, 124,  84),                    // lpBtnAct
                Rgba(110, 85, 53),                      // lpBorder
                Rgba(140, 110, 60),                     // lpBtnOnFace — an inked stamp
                Rgba(245, 235, 216),                    // lpBtnOnInk  — paper on ink
                Rgba(90, 70, 45),                       // lpRule
            },
            {   // 3 Parchment Crimson — 크림슨 + 찢긴 프레임 + 글로우 타이틀
                // + ◇ 크림슨 라벨. (텍스처 시절의 소프트 글로우 변형은 사라짐 —
                // 찢김은 이제 그려지고 변형이 하나뿐이다.)
                // ★winBg: this used to be the SAME value as Parchment Amber,
                // which is why the crimson strip sank into it. Sending the
                // amber skin warm and this one cool separates them and buys
                // the accent something to sit against — red reads reddest
                // next to its opposite. #171A1C is warmth −5: still black to
                // the eye, but the strip and the ◇ labels now carry the only
                // colour in the window.
                "Parchment Crimson",
                Rgba(230, 226, 216), Rgba(245, 242, 234), Rgba(220, 216, 206),
                Rgba(220, 216, 206, 0.45f), Rgba(23, 26, 28),
                Rgba(236, 232, 222), Rgba(0, 0, 0, 0.5f),
                Rgba(168, 64, 47), Rgba(230, 226, 216),
                0.0f, 5.0f,
                true, false, true, true,   // cornerFade + titleGlow + diamondLabels
                true,                      // tornFrame
            },
            {   // 4 Glass Dark — QuickLoot IE-style near-black translucent
                // panel: white ink, silver hairlines (corner-fade chrome),
                // rust-red selection bar. Shares the translucent machinery
                // (caching card / centre park / line boost) with Glass Clear.
                "Glass Dark",
                Rgba(212, 212, 216), Rgba(240, 240, 244), Rgba(228, 228, 232),
                Rgba(228, 228, 232, 0.55f), Rgba(12, 12, 14, 0.58f),
                // ★★shade goes UP, not down. On a translucent panel the mark is
                // alpha*(shade - panel), and a BLACK shade makes that
                // -alpha*panel — which dies with the panel. In a cave the panel
                // is already near black, so no alpha rescues it (0.85 still
                // measured -9), while on snow the same value punches a -120
                // hole. A light shade inverts the term: the darker the room,
                // the STRONGER the mark. Silver keeps it in the acc family.
                Rgba(200, 200, 204), Rgba(212, 212, 216, 0.16f),
                Rgba(134, 38, 28), Rgba(212, 212, 216),
                0.0f, 2.0f,
                true, false, false, false,   // cornerFade (silver hairlines)
                false,                       // no torn frame
                true, false,                 // translucent (no bevel chrome)
                false,                       // lightPanel
                false,                       // engravedCells
                {}, {}, {}, {},              // cellBg / groove / grooveLt / btnFace
                // ★★The default sage was tuned when an occupied tile was DARK.
                // Turning that ground silver moved the floor out from under it:
                // sage and the new tile meet in the middle, and on snow the
                // difference goes NEGATIVE (-2.7) — the mark inverts and
                // vanishes. Same lesson as the parchment skin: a mark that sits
                // at mid brightness gets overtaken whenever the room changes.
                // Pale cyan clears the tile by +20 at the worst background and
                // still reads as a different COLOUR (dE 10) rather than "more
                // silver", which is what separates "this bag is open" from
                // "this cell is occupied".
                Rgba(170, 225, 232, 0.35f),  // bagOpen
            },
            {   // 5 Glass Clear — Glass Dark with the panel FAR more
                // transparent; everything else identical except the tile
                // shade (0.50: tiles must stay readable on the much thinner
                // panel). (MABINOGI GREY was removed by user request; the
                // bevelChrome grammar stays available for future skins.)
                "Glass Clear",
                Rgba(212, 212, 216), Rgba(240, 240, 244), Rgba(228, 228, 232),
                Rgba(228, 228, 232, 0.55f), Rgba(12, 12, 14, 0.38f),
                // a step higher than Glass Dark: this panel lets more of the
                // world through, so the ground it sits on is brighter and the
                // same alpha would buy less separation
                Rgba(200, 200, 204), Rgba(212, 212, 216, 0.20f),
                Rgba(134, 38, 28), Rgba(212, 212, 216),
                0.0f, 2.0f,
                true, false, false, false,   // cornerFade (silver hairlines)
                false,                       // no torn frame
                true, false,                 // translucent (no bevel chrome)
                false,                       // lightPanel
                false,                       // engravedCells
                {}, {}, {}, {},              // cellBg / groove / grooveLt / btnFace
                // ★★The default sage was tuned when an occupied tile was DARK.
                // Turning that ground silver moved the floor out from under it:
                // sage and the new tile meet in the middle, and on snow the
                // difference goes NEGATIVE (-2.7) — the mark inverts and
                // vanishes. Same lesson as the parchment skin: a mark that sits
                // at mid brightness gets overtaken whenever the room changes.
                // Pale cyan clears the tile by +20 at the worst background and
                // still reads as a different COLOUR (dE 10) rather than "more
                // silver", which is what separates "this bag is open" from
                // "this cell is occupied".
                Rgba(170, 225, 232, 0.35f),  // bagOpen
            },
            {   // 6 Simple — a plain blue windowed panel. Two things carry it
                // and neither is a colour: the border is TWO lines (dark outer
                // + bright inner, via bevelChrome), and the cell grid is CARVED
                // rather than drawn (engravedCells). Buttons are recessed for
                // the same reason — face darker than the chrome, same two-line
                // edge.
                // ★Every blue here is a step below the measured reference. The
                // reference colours live inside a 24px window frame on a 2003
                // client; ours cover half a modern screen at 1440p, and the
                // same value spread over that area is simply a brighter object.
                // Matching the swatch is not the goal — matching how it reads is.
                "Simple",
                Rgba(24, 58, 112),                      // acc    borders/ink
                Rgba(74, 138, 166),                     // hi — the inner rim sits just ABOVE the panel, not a bright ring
                Rgba(255, 255, 255),                    // ink
                Rgba(214, 232, 242, 0.80f),             // inkDim
                Rgba(82, 149, 185, 0.68f),              // winBg (-15%: see-through board)
                Rgba(58, 116, 130),                     // glyph
                Rgba(28, 58, 66, 1.0f),                 // shade (occupied cell)
                Rgba(84, 158, 184),                     // sel
                Rgba(84, 158, 184),                     // filled
                0.0f, 0.0f,                             // rounding / titleSpacing (plain titles)
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                // ★Pulled DOWN from the measured #468896. The reference cell
                // sits under a 24px-wide window frame; ours is the same colour
                // under a much larger panel, and at that scale the two read as
                // one flat field. The cell has to be a clear step below the
                // chrome or the board stops looking like a board.
                // ★These carry their OWN alpha now (Grid reads it instead of
                // forcing 1.0). Grooves and faces never overlap, so what is
                // written here is what lands on screen.
                // ★The face now carries what the old stack composited to:
                // groove .85 under face .85 = .9775 effective, and the colour
                // that pair landed on. The groove is no longer drawn (the gap
                // IS the panel), so without this every cell would lighten.
                Rgba(43, 88, 102, 0.9775f),             // cellBg
                Rgba(34, 72, 86, 0.85f),                // cellGroove (inner shadow only)
                Rgba(62, 118, 146, 0.85f),              // cellGrooveLt (unused)
                Rgba(42, 92, 122),                      // btnFace
                Rgba(190, 158, 166, 0.50f),             // bagOpen: pale, half strength
                Rgba(238, 206, 118),                    // goldNum — money, not "a value"
                24.0f,                                  // titleSize (34px bar leaves 5px above and below)
            },
            {   // Simple Silver — 무채색에 가까운 은빛 — XP Luna Silver 계열
                // ★Derived from Simple's own tokens: hue moved, saturation and
                // the lightness STEPS between chrome / cell / groove preserved.
                // Same grammar (bevelChrome + engravedCells + translucent).
                "Simple Silver",
                Rgba(67, 71, 79),                       // borders/ink
                Rgba(119, 127, 131),                    // inner rim
                Rgba(255, 255, 255),                    // ink
                Rgba(232, 233, 235, 0.8f),              // inkDim
                Rgba(132, 139, 145, 0.68f),             // winBg (translucent board)
                Rgba(94, 101, 104),                     // glyph
                Rgba(49, 53, 55),                       // shade (occupied cell)
                Rgba(133, 141, 145),                    // sel
                Rgba(133, 141, 145),                    // filled
                0.0f, 0.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                Rgba(73, 79, 82, 0.9775f),              // cellBg
                Rgba(61, 66, 69, 0.85f),                // cellGroove
                Rgba(103, 110, 115, 0.85f),             // cellGrooveLt (unused)
                Rgba(82, 88, 93),                       // btnFace
                Rgba(194, 171, 159, 0.5f),              // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                    // goldNum — money is money on every theme
                24.0f,                                  // titleSize
                // ★★light-panel palette. Simple leaves these at alpha 0 and
                // inherits the fallbacks in Theme.cpp — which are ITS blues.
                // A theme that stays silent gets a blue preset "+" and a blue
                // toggled-ON chip on an otherwise silver window, so every
                // one of them is named here.
                Rgba(98, 104, 108),                     // lpBtn
                Rgba(118, 124, 129),                    // lpBtnHov
                Rgba(81, 86, 90),                       // lpBtnAct
                Rgba(73, 78, 82),                       // lpBorder
                Rgba(183, 190, 192),                    // lpBtnOnFace — the ON state
                Rgba(37, 42, 43),                       // lpBtnOnInk
                Rgba(32, 35, 37),                       // lpRule
            },
            {   // Simple Graphite — 먹빛 중성. 이 가족에서 가장 어둡다
                // ★Derived from Simple's own tokens: hue moved, saturation and
                // the lightness STEPS between chrome / cell / groove preserved.
                // Same grammar (bevelChrome + engravedCells + translucent).
                "Simple Graphite",
                Rgba(47, 51, 63),                       // borders/ink
                Rgba(98, 108, 116),                     // inner rim
                Rgba(255, 255, 255),                    // ink
                Rgba(211, 215, 220, 0.8f),              // inkDim
                Rgba(109, 120, 132, 0.68f),             // winBg (translucent board)
                Rgba(74, 83, 88),                       // glyph
                Rgba(31, 35, 37),                       // shade (occupied cell)
                Rgba(110, 123, 132),                    // sel
                Rgba(110, 123, 132),                    // filled
                0.0f, 0.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                Rgba(54, 61, 65, 0.9775f),              // cellBg
                Rgba(43, 48, 52, 0.85f),                // cellGroove
                Rgba(83, 91, 99, 0.85f),                // cellGrooveLt (unused)
                Rgba(62, 69, 77),                       // btnFace
                Rgba(187, 164, 148, 0.5f),              // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                    // goldNum — money is money on every theme
                24.0f,                                  // titleSize
                // ★★light-panel palette. Simple leaves these at alpha 0 and
                // inherits the fallbacks in Theme.cpp — which are ITS blues.
                // A theme that stays silent gets a blue preset "+" and a blue
                // toggled-ON chip on an otherwise graphite window, so every
                // one of them is named here.
                Rgba(78, 85, 92),                       // lpBtn
                Rgba(97, 105, 113),                     // lpBtnHov
                Rgba(61, 67, 73),                       // lpBtnAct
                Rgba(54, 59, 66),                       // lpBorder
                Rgba(160, 174, 180),                    // lpBtnOnFace — the ON state
                Rgba(19, 23, 25),                       // lpBtnOnInk
                Rgba(15, 16, 19),                       // lpRule
            },
            {   // Simple Copper — 구리·적갈. 따뜻한 금속
                // ★Derived from Simple's own tokens: hue moved, saturation and
                // the lightness STEPS between chrome / cell / groove preserved.
                // Same grammar (bevelChrome + engravedCells + translucent).
                "Simple Copper",
                Rgba(97, 62, 28),                       // borders/ink
                Rgba(152, 90, 77),                      // inner rim
                Rgba(255, 255, 255),                    // ink
                Rgba(237, 215, 209, 0.8f),              // inkDim
                Rgba(174, 103, 83, 0.68f),              // winBg (translucent board)
                Rgba(118, 63, 60),                      // glyph
                Rgba(56, 30, 28),                       // shade (occupied cell)
                Rgba(173, 96, 85),                      // sel
                Rgba(173, 96, 85),                      // filled
                0.0f, 0.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                Rgba(91, 49, 44, 0.9775f),              // cellBg
                Rgba(75, 40, 35, 0.85f),                // cellGroove
                Rgba(133, 79, 65, 0.85f),               // cellGrooveLt (unused)
                Rgba(109, 60, 45),                      // btnFace
                Rgba(153, 186, 190, 0.5f),              // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                    // goldNum — money is money on every theme
                24.0f,                                  // titleSize
                // ★★light-panel palette. Simple leaves these at alpha 0 and
                // inherits the fallbacks in Theme.cpp — which are ITS blues.
                // A theme that stays silent gets a blue preset "+" and a blue
                // toggled-ON chip on an otherwise copper window, so every
                // one of them is named here.
                Rgba(122, 75, 64),                      // lpBtn
                Rgba(145, 93, 80),                      // lpBtnHov
                Rgba(100, 60, 49),                      // lpBtnAct
                Rgba(94, 54, 41),                       // lpBorder
                Rgba(213, 142, 141),                    // lpBtnOnFace — the ON state
                Rgba(45, 16, 15),                       // lpBtnOnInk
                Rgba(36, 20, 13),                       // lpRule
            },
            {   // Simple Wine — 적포도주
                // ★Derived from Simple's own tokens: hue moved, saturation and
                // the lightness STEPS between chrome / cell / groove preserved.
                // Same grammar (bevelChrome + engravedCells + translucent).
                "Simple Wine",
                Rgba(92, 34, 39),                       // borders/ink
                Rgba(147, 83, 108),                     // inner rim
                Rgba(255, 255, 255),                    // ink
                Rgba(235, 211, 219, 0.8f),              // inkDim
                Rgba(167, 90, 117, 0.68f),              // winBg (translucent board)
                Rgba(113, 64, 89),                      // glyph
                Rgba(54, 30, 42),                       // shade (occupied cell)
                Rgba(166, 91, 124),                     // sel
                Rgba(166, 91, 124),                     // filled
                0.0f, 0.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                Rgba(87, 48, 66, 0.9775f),              // cellBg
                Rgba(72, 38, 53, 0.85f),                // cellGroove
                Rgba(128, 70, 91, 0.85f),               // cellGrooveLt (unused)
                Rgba(104, 50, 67),                      // btnFace
                Rgba(153, 190, 173, 0.5f),              // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                    // goldNum — money is money on every theme
                24.0f,                                  // titleSize
                // ★★light-panel palette. Simple leaves these at alpha 0 and
                // inherits the fallbacks in Theme.cpp — which are ITS blues.
                // A theme that stays silent gets a blue preset "+" and a blue
                // toggled-ON chip on an otherwise wine window, so every
                // one of them is named here.
                Rgba(117, 68, 86),                      // lpBtn
                Rgba(140, 85, 106),                     // lpBtnHov
                Rgba(96, 53, 69),                       // lpBtnAct
                Rgba(90, 45, 59),                       // lpBorder
                Rgba(208, 147, 181),                    // lpBtnOnFace — the ON state
                Rgba(43, 17, 30),                       // lpBtnOnInk
                Rgba(34, 15, 20),                       // lpRule
            },
            {   // Simple Forest — 짙은 상록
                // ★Derived from Simple's own tokens: hue moved, saturation and
                // the lightness STEPS between chrome / cell / groove preserved.
                // Same grammar (bevelChrome + engravedCells + translucent).
                "Simple Forest",
                Rgba(34, 91, 69),                       // borders/ink
                Rgba(84, 146, 103),                     // inner rim
                Rgba(255, 255, 255),                    // ink
                Rgba(211, 235, 220, 0.8f),              // inkDim
                Rgba(91, 166, 117, 0.68f),              // winBg (translucent board)
                Rgba(65, 113, 74),                      // glyph
                Rgba(30, 54, 35),                       // shade (occupied cell)
                Rgba(92, 165, 111),                     // sel
                Rgba(92, 165, 111),                     // filled
                0.0f, 0.0f,                             // rounding / titleSpacing
                false, false, false, false,             // no cornerFade/strip/glow/◇
                false,                                  // no torn frame
                true, true,                             // translucent + bevelChrome
                true,                                   // lightPanel
                true,                                   // engravedCells
                Rgba(48, 87, 57, 0.9775f),              // cellBg
                Rgba(38, 72, 47, 0.85f),                // cellGroove
                Rgba(71, 127, 90, 0.85f),               // cellGrooveLt (unused)
                Rgba(51, 103, 70),                      // btnFace
                Rgba(190, 153, 181, 0.5f),              // bagOpen (complement, so it never reads as the panel)
                Rgba(238, 206, 118),                    // goldNum — money is money on every theme
                24.0f,                                  // titleSize
                // ★★light-panel palette. Simple leaves these at alpha 0 and
                // inherits the fallbacks in Theme.cpp — which are ITS blues.
                // A theme that stays silent gets a blue preset "+" and a blue
                // toggled-ON chip on an otherwise forest window, so every
                // one of them is named here.
                Rgba(69, 117, 85),                      // lpBtn
                Rgba(86, 140, 104),                     // lpBtnHov
                Rgba(54, 96, 68),                       // lpBtnAct
                Rgba(46, 89, 63),                       // lpBorder
                Rgba(148, 207, 156),                    // lpBtnOnFace — the ON state
                Rgba(17, 42, 22),                       // lpBtnOnInk
                Rgba(15, 34, 23),                       // lpRule
            },
        };

        // ★Initialised FROM the exported defaults, never from a second copy of
        // the number — the reset gesture reads the same constants.
        float g_scale = kDefScale;
        // ★Default 0.80, not 1.0. At 1.0 the window is 912x818 — a third of a
        // 1440p screen — and the only way to shrink it used to be the UI
        // scale, which took the text down with it.
        float g_cellScale = kDefCellScale;
        int   g_skin = 3;   // 1-based (release default = Parchment Crimson)
        // ★Capture-lamp offset shared by every icon. Deliberately NOT in the
        // per-skin block below — see Theme.h.
        float g_capLightAz = kDefCapLightAz;
        float g_capLightEl = kDefCapLightEl;

        // GI59: glow and icon light are kept PER ICON STYLE — [0] realistic
        // (3D captures), [1] drawn (flat art), [2] pixel. A photographed model
        // and a flat drawing do not take the same fill light.
        // ★★1.0.5: every DISPLAY setting is per SKIN as well.
        constexpr int kSkinSlots = static_cast<int>(std::size(kSkins));

        int   g_glowStyle[kSkinSlots][3] = {};      // legacy, ini format only
        int   g_iconStyle[kSkinSlots] = {};         // 0 realistic, ditto
        float g_glowGain[kSkinSlots][3][2] = {};    // legacy, ini format only
        float g_iconGain[kSkinSlots][3] = {};
        // ★★1.0.5 item shadow: [skin][icon style][0 dist / 1 blur / 2 opacity].
        float g_shadow[kSkinSlots][3][3] = {};
        // ★Seeded from the SAME constants the reset gesture reads, so a fresh
        // skin and a right-clicked slider can never disagree.
        const bool g_dispSeeded = [] {
            for (int s = 0; s < kSkinSlots; ++s) {
                for (int t = 0; t < 3; ++t) {
                    g_iconGain[s][t]    = DefIconGain(t);
                    g_glowGain[s][t][0] = DefGlowGain(0);
                    g_glowGain[s][t][1] = DefGlowGain(1);
                    for (int a = 0; a < 3; ++a) g_shadow[s][t][a] = DefShadow(a);
                }
            }
            return true;
        }();

        [[nodiscard]] constexpr int C01(int a_v)
        {
            return a_v < 0 ? 0 : (a_v > 2 ? 2 : a_v);
        }
        [[nodiscard]] constexpr int CAxis(int a_v)
        {
            return a_v < 0 ? 0 : (a_v > 2 ? 2 : a_v);
        }
        [[nodiscard]] constexpr float ClampShadow(int a_axis, float a_v)
        {
            const float hi = a_axis == 2 ? 1.0f : 8.0f;   // opacity is a fraction
            return a_v < 0.0f ? 0.0f : (a_v > hi ? hi : a_v);
        }

        // skin index is 1-BASED everywhere it is spoken about (ini, chips,
        // SkinIndex); the arrays are 0-based. One converter, clamped.
        [[nodiscard]] constexpr int CSkin(int a_skin1)
        {
            const int i = a_skin1 - 1;
            return i < 0 ? 0 : (i >= kSkinSlots ? kSkinSlots - 1 : i);
        }
        [[nodiscard]] int SkinSlot() { return CSkin(g_skin); }

        // ★The icon style is the SKIN's, so Theme holds the number and the
        // cache is told about it. Raw values are IconCache::Style's own
        // (0 realistic / 2 drawn / 3 pixel — 1 is retired).
        void ApplyIconStyle()
        {
            auto* ic = IconCache::GetSingleton();
            if (!ic) return;
            switch (g_iconStyle[SkinSlot()]) {
            case 2:  ic->SetStyle(IconCache::Style::kFlat);  break;
            case 3:  ic->SetStyle(IconCache::Style::kPixel); break;
            default: ic->SetStyle(IconCache::Style::kRealistic); break;
            }
        }
    }

    int IconStyleSlot()
    {
        // The singleton can be absent while settings load at startup; realistic
        // is the right assumption then, and the loader never relies on this.
        const auto* ic = IconCache::GetSingleton();
        if (!ic) return 0;
        switch (ic->GetStyle()) {
        case IconCache::Style::kFlat:  return 1;
        case IconCache::Style::kPixel: return 2;
        default:                       return 0;
        }
    }

    // ---- DISPLAY settings: [skin][icon style][glow style] --------------------
    // The no-argument forms act on whatever is live (this skin, this icon
    // style) — every UI call site uses those. The *Of / *At forms name every
    // axis outright and are what persistence uses.

    float IconGain() { return g_iconGain[SkinSlot()][IconStyleSlot()]; }
    void  SetIconGain(float a_gain) { SetIconGainOf(g_skin, IconStyleSlot(), a_gain); }
    float IconGainOf(int a_skin, int a_slot)
    {
        return g_iconGain[CSkin(a_skin)][C01(a_slot)];
    }

    void SetIconGainOf(int a_skin, int a_slot, float a_gain)
    {
        g_iconGain[CSkin(a_skin)][C01(a_slot)] =
            (std::max)(0.4f, (std::min)(1.6f, a_gain));
    }

    // ---- item shadow ---------------------------------------------------------
    float ShadowAxis(int a_axis)
    {
        return g_shadow[SkinSlot()][IconStyleSlot()][CAxis(a_axis)];
    }
    float ShadowDist()    { return ShadowAxis(0); }
    float ShadowBlur()    { return ShadowAxis(1); }
    float ShadowOpacity() { return ShadowAxis(2); }

    void SetShadowAxis(int a_axis, float a_v)
    {
        SetShadowAt(g_skin, IconStyleSlot(), a_axis, a_v);
    }

    float ShadowAt(int a_skin, int a_slot, int a_axis)
    {
        return g_shadow[CSkin(a_skin)][C01(a_slot)][CAxis(a_axis)];
    }

    void SetShadowAt(int a_skin, int a_slot, int a_axis, float a_v)
    {
        const int ax = CAxis(a_axis);
        g_shadow[CSkin(a_skin)][C01(a_slot)][ax] = ClampShadow(ax, a_v);
    }

    int  GlowStyle() { return g_glowStyle[SkinSlot()][IconStyleSlot()]; }
    void SetGlowStyle(int a_style) { SetGlowStyleOf(g_skin, IconStyleSlot(), a_style); }
    int  GlowStyleOf(int a_skin, int a_slot)
    {
        return g_glowStyle[CSkin(a_skin)][C01(a_slot)];
    }

    void SetGlowStyleOf(int a_skin, int a_slot, int a_style)
    {
        g_glowStyle[CSkin(a_skin)][C01(a_slot)] = C01(a_style);
    }

    float GlowGain()
    {
        const int sk = SkinSlot(), slot = IconStyleSlot();
        return g_glowGain[sk][slot][g_glowStyle[sk][slot]];
    }

    float DefaultGlowGain() { return DefGlowGain(g_glowStyle[SkinSlot()][IconStyleSlot()]); }
    float DefaultIconGain() { return DefIconGain(IconStyleSlot()); }

    void SetGlowGain(float a_gain)
    {
        const int sk = SkinSlot(), slot = IconStyleSlot();
        SetGlowGainAt(g_skin, slot, g_glowStyle[sk][slot], a_gain);
    }

    float GlowGainOf(int a_style)
    {
        return GlowGainAt(g_skin, IconStyleSlot(), a_style);
    }

    void SetGlowGainOf(int a_style, float a_gain)
    {
        SetGlowGainAt(g_skin, IconStyleSlot(), a_style, a_gain);
    }

    float GlowGainAt(int a_skin, int a_slot, int a_style)
    {
        return g_glowGain[CSkin(a_skin)][C01(a_slot)][C01(a_style)];
    }

    void SetGlowGainAt(int a_skin, int a_slot, int a_style, float a_gain)
    {
        g_glowGain[CSkin(a_skin)][C01(a_slot)][C01(a_style)] =
            (std::max)(0.2f, (std::min)(2.5f, a_gain));
    }

    int  IconStyleOf(int a_skin) { return g_iconStyle[CSkin(a_skin)]; }

    void SetIconStyleOf(int a_skin, int a_style)
    {
        g_iconStyle[CSkin(a_skin)] = a_style;
        if (CSkin(a_skin) == SkinSlot()) ApplyIconStyle();
    }

    void SetIconStyle(int a_style) { SetIconStyleOf(g_skin, a_style); }

    // ---- capture light -------------------------------------------------------
    float CaptureLightAz() { return g_capLightAz; }
    float CaptureLightEl() { return g_capLightEl; }

    void SetCaptureLight(float a_azDeg, float a_elDeg)
    {
        g_capLightAz = (std::max)(-180.0f, (std::min)(180.0f, a_azDeg));
        g_capLightEl = (std::max)(-80.0f, (std::min)(80.0f, a_elDeg));
    }

    // ---- scale ---------------------------------------------------------------
    float Scale() { return g_scale; }

    void SetScale(float a_scale)
    {
        g_scale = (std::max)(0.5f, (std::min)(1.6f, a_scale));
    }

    float CellScale() { return g_cellScale; }

    void SetCellScale(float a_scale)
    {
        g_cellScale = (std::max)(0.5f, (std::min)(1.4f, a_scale));
    }

    // ---- skin ----------------------------------------------------------------
    int SkinIndex() { return g_skin; }

    int SkinCount() { return static_cast<int>(std::size(kSkins)); }

    const Skin& SkinAt(int a_index)
    {
        // ★clamp against the TABLE, not a literal: an out-of-range index would
        // otherwise read past the array, and a saved number from a build with
        // more skins is exactly that.
        const int i = (std::max)(1, (std::min)(SkinCount(), a_index));
        return kSkins[i - 1];
    }

    const Skin& S() { return SkinAt(g_skin); }

    void SetSkin(int a_index)
    {
        g_skin = (std::max)(1, (std::min)(SkinCount(), a_index));
        if (ImGui::GetCurrentContext()) Apply();
        ApplyIconStyle();
    }

    void SetSkinLegacy(int a_oldIndex)
    {
        // ★"Fable Amber" sat at index 2 and was removed; everything above it
        // shifted down by one. A file that says "!skin" was written in the old
        // numbering, so 1 stays 1, 2 (the removed one) becomes Parchment
        // Amber's 2, and everything past it drops.
        const int i = a_oldIndex <= 2 ? (std::max)(1, a_oldIndex)
                                      : a_oldIndex - 1;
        SetSkin(i);
    }

    // ---- colour helpers ------------------------------------------------------
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

    const ImVec4& ValVec()
    {
        const Skin& sk = S();
        // ★★A light panel has no headroom above it — `hi` is tuned to glow on a
        // dark ground and simply fades into a bright one. The answer is the
        // skin's own INK: on SIMPLE that is white (and every number carries a
        // black outline, so it reads at 21:1 whatever the panel is doing), on
        // parchment it is the same brown the body text uses.
        // ★NOT acc. Acc is the darkest token in a light skin — it is the frame
        // colour — so numbers painted in it read as engraved into the panel
        // rather than as the figures the eye is hunting for.
        return sk.lightPanel ? sk.ink : sk.hi;
    }

    ImU32 Val(float a_alpha) { return Col(ValVec(), a_alpha); }

    ImU32 GoldCol()
    {
        const Skin& sk = S();
        return sk.goldNum.w > 0.0f ? Col(sk.goldNum, 1.0f) : Val();
    }

    ImU32 Chrome(float a_alpha)
    {
        const Skin& sk = S();
        // ★Over a LIGHT panel acc is the darkest thing on screen, and headings
        // painted in it stop reading as headings — they read as borders. The
        // answer is the skin's own INK, not a darker colour still: on SIMPLE
        // that is white (and TextOutlined gives it a black edge, 21:1), on
        // parchment it is the brown the body text already uses. Each skin
        // already decided what "written on this panel" looks like.
        return sk.lightPanel ? Col(sk.ink, a_alpha) : Col(sk.acc, a_alpha);
    }

    ImU32 OccupiedGround()
    {
        // ★ONE answer for the board, the doll and the partner window. They used
        // to each reach for sk.shade with their own alpha, so the same colour
        // said "occupied" loudly on one half of the window and almost nothing
        // on the other. The token carries its own alpha; nobody overrides it.
        return Col(S().shade);
    }

    // ---- metrics -------------------------------------------------------------
    float PadX() { return 12.0f * g_scale; }
    float PadY() { return 8.0f * g_scale; }

    // ★★Skin::rounding == 0 means "this skin does not name one", NOT "square".
    // Windows and frames want different amounts — 6px on a 700px window is a
    // soft corner, the same 6px on a 17px button is a lozenge — so the default
    // is a PAIR, and a skin that sets `rounding` overrides both with its one
    // value (that is the "single knob" the header speaks of).
    float WinRounding()
    {
        const float r = S().rounding;
        return r > 0.0f ? r : 6.0f;
    }

    float FrameRounding()
    {
        const float r = S().rounding;
        return r > 0.0f ? (std::min)(r, 4.0f) : 3.0f;
    }

    // larger for the torn frame: the panel is inset from the window edge by
    // the ragged margin, so content must clear it. translucent skins get a
    // small margin too — with zero inset every separator ran edge-to-edge.
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

    float BorderPx() { return (std::max)(1.0f, std::round(g_scale)); }

    ImU32 WinBorder()
    {
        // ★OPAQUE on purpose: two translucent frames stacked on one pixel row
        // blend to a darker line, which is the doubled seam the snap overlap
        // exists to remove.
        const Skin& sk = S();
        return Col(LP(sk.lpBorder, Rgba(13, 32, 46)), 1.0f);
    }

    float BorderOverlap()
    {
        // ★A window's right frame sits at right-0.5 and its neighbour's left
        // frame at left+0.5. They coincide when left = right-1 — whatever the
        // stroke width. Overlapping by the full stroke put the two lines a
        // pixel apart and the join went back to two.
        return S().lightPanel ? 1.0f : 0.0f;
    }

    ImU32  BtnOnInk()    { const Skin& sk = S();
                           return sk.lightPanel ? Col(LP(sk.lpBtnOnInk, kBtnOnInk)) : Val(); }
    ImVec4 BtnOnInkVec() { const Skin& sk = S();
                           return sk.lightPanel ? LP(sk.lpBtnOnInk, kBtnOnInk) : ValVec(); }
    ImU32  TitleInk()    { const Skin& sk = S(); return Col(sk.ink, 1.0f); }

    ImU32 BtnOn(float a_alpha)
    {
        const Skin& sk = S();
        return sk.lightPanel ? Col(LP(sk.lpBtnOnFace, kBtnOnFace), a_alpha)
                             : Acc(0.28f * a_alpha);
    }

    // tooltip palette — read off the reference tooltip, one colour per KIND
    // of fact rather than one per emphasis level
    namespace
    {
        const ImVec4 kTipHead = Rgba(232, 200, 110);   // section label, gold
        const ImVec4 kTipVal  = Rgba(111, 200, 240);   // name, numbers
        const ImVec4 kTipGood = Rgba(127, 201, 138);   // temper, enchantment
        const ImVec4 kTipBad  = Rgba(232, 106, 106);   // restriction, overload
        const ImVec4 kTipSub  = Rgba(151, 163, 172);   // hints, shop price
        const ImVec4 kTipBody = Rgba(230, 237, 242);   // running text
    }

    const ImVec4& TipHead() { return kTipHead; }
    const ImVec4& TipVal()  { return kTipVal; }
    const ImVec4& TipGood() { return kTipGood; }
    const ImVec4& TipBad()  { return kTipBad; }
    const ImVec4& TipSub()  { return kTipSub; }
    const ImVec4& TipBody() { return kTipBody; }

    void PushTipStyle()
    {
        // ★The tooltip goes DARK on every skin. Over a light panel a tooltip
        // painted in the same blue lands on the window it describes and
        // dissolves — it needs an edge of its own.
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.055f, 0.063f, 0.075f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.62f, 0.66f, 0.70f, 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text, kTipBody);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    }

    void PopTipStyle()
    {
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(3);
    }

    // ---- one lighting model --------------------------------------------------
    ImU32 BevelLit(bool a_window)
    {
        // light from the TOP-LEFT everywhere. a_window drops to about two
        // thirds: the same alpha reads far stronger along a long edge.
        return IM_COL32(255, 255, 255, a_window ? 34 : 50);
    }

    ImU32 BevelShd(bool a_window)
    {
        return IM_COL32(0, 0, 0, a_window ? 52 : 78);
    }

    bool InkNeedsOutline()
    {
        // ★Asked of the INK, not of a flag: the outline exists to lift LIGHT
        // ink off a mid-tone panel. Dark ink on a pale panel is already the
        // strongest thing there, and an edge in the same value range just
        // fattens every stroke until the word reads as a blob.
        const ImVec4& c = S().ink;
        const float lum = 0.299f * c.x + 0.587f * c.y + 0.114f * c.z;
        return lum > 0.55f;
    }

    // ---- type scale ----------------------------------------------------------
    float SnapPx(float a_size)
    {
        // ★Whole pixels. The atlas is baked at integer sizes, and asking for
        // 17.6 gets a scaled blit of the 18 — which is exactly the smearing
        // the bake was meant to avoid.
        return (std::max)(1.0f, std::round(a_size * g_scale));
    }

    float FontValue()   { return SnapPx(20.0f); }
    float FontBody()    { return SnapPx(17.0f); }
    float FontCaption() { return SnapPx(12.0f); }

    ImU32 Rule()
    {
        const Skin& sk = S();
        // ★A rule on a LIGHT panel has to be dark. White at .42 measures
        // 1.74:1 — it is not a divider, it is a smudge.
        return sk.lightPanel ? Col(LP(sk.lpRule, kRuleInk), 0.70f) : Acc(0.25f);
    }

    // ★★A gauge on a LIGHT panel fills UPWARD in brightness. Everywhere else
    // the track is a dark well with an accent fill, but on a light panel the
    // accent IS the darkest token — a track drawn that way reads as a groove
    // cut into the panel, and the filled part is indistinguishable from the
    // empty part. The ON-button face is already the skin's "brightest, means
    // active" colour, so the fill borrows it and the whole control reads at a
    // glance.
    ImU32 GaugeTrack()
    {
        const Skin& sk = S();
        return sk.lightPanel ? IM_COL32(255, 255, 255, 38) : IM_COL32(0, 0, 0, 51);
    }

    ImU32 GaugeFill()
    {
        const Skin& sk = S();
        // ★WHITE on a light panel, not the skin's brightest token. A gauge is
        // read at a glance, and the only thing that survives being that small
        // against a mid-tone panel is the value the panel cannot reach — a
        // tinted fill has to compete with the panel's own hue.
        //
        // ★★HALF strength, because the VALUE sits on top of it. At full white
        // a filled slider swallowed its own number, and worst of all at half
        // fill: the digits were sliced down the middle, half of them legible.
        // Something read halfway is more distracting than something not read.
        // The capacity bar keeps full white (UIRoot) — nothing is written over
        // that one, so it has no reason to give any brightness back.
        if (sk.lightPanel) return IM_COL32(255, 255, 255, 107);
        return sk.translucent ? Col(sk.sel, 0.55f) : Acc(0.20f);
    }

    ImU32 GaugeBorder()
    {
        const Skin& sk = S();
        if (sk.lightPanel) return Col(LP(sk.lpBorder, Rgba(39, 80, 106)), 0.75f);
        return sk.translucent ? Col(sk.sel, 0.60f) : Acc(0.25f);
    }

    // ---- text ----------------------------------------------------------------
    ImVec2 TrackedSize(const char* a_text, float a_size, float a_spacing)
    {
        if (!a_text || !*a_text) return ImVec2(0.0f, 0.0f);
        ImFont* f = ImGui::GetFont();
        const float sz = a_size > 0.0f ? a_size : ImGui::GetFontSize();
        if (a_spacing <= 0.0f) return f->CalcTextSizeA(sz, FLT_MAX, 0.0f, a_text);
        float w = 0.0f;
        int   n = 0;
        for (const char* p = a_text; *p; ) {
            unsigned int cp = 0;
            const int adv = ImTextCharFromUtf8(&cp, p, nullptr);
            p += adv > 0 ? adv : 1;
            char buf[8] = {};
            std::memcpy(buf, p - (adv > 0 ? adv : 1), static_cast<size_t>(adv > 0 ? adv : 1));
            w += f->CalcTextSizeA(sz, FLT_MAX, 0.0f, buf).x;
            ++n;
        }
        if (n > 1) w += a_spacing * static_cast<float>(n - 1);
        return ImVec2(w, sz);
    }

    void TextOutlined(ImDrawList* a_dl, ImVec2 a_pos, ImU32 a_col, const char* a_text,
                      float a_size, float a_spacing)
    {
        if (!a_dl || !a_text || !*a_text) return;
        ImFont*     f  = ImGui::GetFont();
        const float sz = a_size > 0.0f ? a_size : ImGui::GetFontSize();
        const ImU32 blk = IM_COL32(0, 0, 0, 190);
        auto put = [&](ImVec2 p, ImU32 c) {
            if (a_spacing <= 0.0f) {
                a_dl->AddText(f, sz, p, c, a_text);
                return;
            }
            // tracking needs the glyphs drawn one at a time
            float x = p.x;
            for (const char* q = a_text; *q; ) {
                unsigned int cp = 0;
                const int adv = ImTextCharFromUtf8(&cp, q, nullptr);
                const int n = adv > 0 ? adv : 1;
                char buf[8] = {};
                std::memcpy(buf, q, static_cast<size_t>(n));
                a_dl->AddText(f, sz, ImVec2(x, p.y), c, buf);
                x += f->CalcTextSizeA(sz, FLT_MAX, 0.0f, buf).x + a_spacing;
                q += n;
            }
        };
        if (InkNeedsOutline()) {
            put(ImVec2(a_pos.x - 1.0f, a_pos.y), blk);
            put(ImVec2(a_pos.x + 1.0f, a_pos.y), blk);
            put(ImVec2(a_pos.x, a_pos.y - 1.0f), blk);
            put(ImVec2(a_pos.x, a_pos.y + 1.0f), blk);
        }
        put(a_pos, a_col);
    }

    void TextInkCentered(ImDrawList* a_dl, const ImVec2& a_p0, const ImVec2& a_p1,
                         ImU32 a_col, const char* a_text, float a_size)
    {
        if (!a_dl || !a_text || !*a_text) return;
        ImFont*     f  = ImGui::GetFont();
        const float sz = a_size > 0.0f ? a_size : ImGui::GetFontSize();
        const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.0f, a_text);
        // ★★Centre by the INK, not by the line box. ImGui reserves descender
        // room whether or not the string has one, so all-caps labels ride high
        // and x-height ones sit low — by a different amount each, which is why
        // no single nudge straightens them.
        // ★ImGui 1.92 moved glyph metrics behind ImFontBaked: a glyph's box is
        // only meaningful at a SIZE, so the font hands out a baked set per size
        // instead of one set that callers scaled themselves.
        // ★FindGlyph, not FindGlyphNoFallback: a string whose glyphs are all
        // missing would otherwise fall through to the line box and centre the
        // ImGui way — the exact behaviour this function exists to replace.
        ImFontBaked* baked = f->GetFontBaked(sz);
        float top = FLT_MAX, bot = -FLT_MAX;
        for (const char* p = a_text; *p; ) {
            unsigned int cp = 0;
            const int adv = ImTextCharFromUtf8(&cp, p, nullptr);
            p += adv > 0 ? adv : 1;
            const ImFontGlyph* g =
                baked ? baked->FindGlyph(static_cast<ImWchar>(cp)) : nullptr;
            // a space has no ink and would drag `top` to its own empty box
            if (g && g->Y1 > g->Y0) {
                top = (std::min)(top, g->Y0);
                bot = (std::max)(bot, g->Y1);
            }
        }
        if (top > bot) { top = 0.0f; bot = ts.y; }
        // ★NOT rounded. The caller's rect is already at whatever subpixel
        // position the layout put it, and snapping only the text moves the
        // label off the centre of the box it belongs to — by up to half a
        // pixel, in whichever direction, per label.
        const float cx = a_p0.x + ((a_p1.x - a_p0.x) - ts.x) * 0.5f;
        const float cy = a_p0.y + ((a_p1.y - a_p0.y) - (bot - top)) * 0.5f - top;
        a_dl->AddText(f, sz, ImVec2(cx, cy), a_col, a_text);
    }

    // ---- chrome widgets ------------------------------------------------------
    namespace
    {
        // ★★The slider's number, drawn by US so it can carry the black edge
        // ImGui cannot give it. On a light panel the fill is white and the ink
        // is white: at full fill the value vanished, and at HALF fill it was
        // sliced down the middle with one half legible — worse than gone,
        // because a half-read string keeps pulling the eye back.
        // Centred on the whole track, which is where ImGui put it.
        void GaugeValue(ImDrawList* a_dl, const ImVec2& a_p, float a_w, float a_h,
                        const char* a_fmt, float a_v, bool a_isInt)
        {
            if (!a_dl || !a_fmt) return;
            char buf[64];
            if (a_isInt) {
                std::snprintf(buf, sizeof(buf), a_fmt, static_cast<int>(a_v));
            } else {
                std::snprintf(buf, sizeof(buf), a_fmt, a_v);
            }
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            TextOutlined(a_dl,
                ImVec2(a_p.x + (a_w - ts.x) * 0.5f, a_p.y + (a_h - ts.y) * 0.5f),
                Val(), buf);
        }
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
        dl->AddRectFilled(p, ImVec2(p.x + a_w, p.y + h), GaugeTrack(), sk.rounding);
        if (frac > 0.0f) {
            dl->AddRectFilled(p, ImVec2(p.x + a_w * frac, p.y + h),
                GaugeFill(), sk.rounding);
        }
        dl->AddRect(p, ImVec2(p.x + a_w, p.y + h), GaugeBorder(), sk.rounding);
        PushChromeStyle(true);
        // ★ImGui draws the value itself and cannot outline it, so it draws
        // NOTHING (transparent ink) and we put the number back below. See the
        // float version for why the outline matters.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::SetNextItemWidth(a_w);
        const bool ch = ImGui::SliderInt(a_id, a_v, a_min, a_max, a_fmt,
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopStyleColor();
        PopChromeStyle(true);
        GaugeValue(dl, p, a_w, h, a_fmt, static_cast<float>(*a_v), true);
        return ch;
    }

    bool ChromeSliderFloat(const char* a_id, float* a_v, float a_min, float a_max,
                           float a_w, const char* a_fmt, float a_resetTo)
    {
        const auto& sk = S();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        const float frac = a_max > a_min
            ? (std::max)(0.0f, (std::min)(1.0f, (*a_v - a_min) / (a_max - a_min)))
            : 0.0f;
        dl->AddRectFilled(p, ImVec2(p.x + a_w, p.y + h), GaugeTrack(), sk.rounding);
        if (frac > 0.0f) {
            dl->AddRectFilled(p, ImVec2(p.x + a_w * frac, p.y + h),
                GaugeFill(), sk.rounding);
        }
        dl->AddRect(p, ImVec2(p.x + a_w, p.y + h), GaugeBorder(), sk.rounding);
        PushChromeStyle(true);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::SetNextItemWidth(a_w);
        bool ch = ImGui::SliderFloat(a_id, a_v, a_min, a_max, a_fmt,
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopStyleColor();
        PopChromeStyle(true);
        GaugeValue(dl, p, a_w, h, a_fmt, *a_v, false);
        // ★Right-click restores the default. One gesture, defined here so every
        // settings slider has it without each row remembering to add it.
        if (a_resetTo > -1.0e8f && ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            *a_v = a_resetTo;
            ch = true;
        }
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
        ImGui::PushStyleColor(ImGuiCol_Text, ValVec());
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

    namespace
    {
        // value noise on an INTEGER lattice — the shape depends on how far
        // along the edge we are, not on where the window sits, so dragging a
        // window does not make its paper crawl
        [[nodiscard]] float Hash01(int a_i, unsigned int a_seed)
        {
            unsigned int x = static_cast<unsigned int>(a_i) * 374761393u + a_seed * 668265263u;
            x = (x ^ (x >> 13)) * 1274126177u;
            return static_cast<float>((x ^ (x >> 16)) & 0xFFFFu) / 65535.0f;
        }
        [[nodiscard]] float VNoise(float a_x, unsigned int a_seed)
        {
            const float fl = std::floor(a_x);
            const int   i  = static_cast<int>(fl);
            float f = a_x - fl;
            const float a = Hash01(i, a_seed), b = Hash01(i + 1, a_seed);
            f = f * f * (3.0f - 2.0f * f);
            return a + (b - a) * f;
        }
        // how deep the paper is bitten at distance t along an edge, in px
        // ★Periods and depth both scale with the UI, so the tearing keeps its
        // proportions — but NOT with the window, which is the whole point.
        [[nodiscard]] float TornDepth(float a_t, unsigned int a_seed, float a_s)
        {
            const float n = 0.50f * VNoise(a_t / (2.750f * a_s), a_seed)
                          + 0.32f * VNoise(a_t / (1.075f * a_s), a_seed + 7u)
                          + 0.18f * VNoise(a_t / (0.425f * a_s), a_seed + 19u);
            // a deeper bite now and then; without it the edge reads as fur
            const float bite =
                (std::max)(0.0f, VNoise(a_t / (10.25f * a_s), a_seed + 53u) - 0.86f) * 2.2f;
            return (n + bite) * 7.0f * a_s;
        }
    }

    void TornPanel(ImDrawList* a_dl, const ImVec2& a_min, const ImVec2& a_max,
                   ImU32 a_col, unsigned int a_seed)
    {
        if (!a_dl) return;
        const float S = Scale();
        a_dl->AddRectFilled(a_min, a_max, a_col);   // the flat sheet
        const float step = 3.0f * S;
        auto run = [&](ImVec2 a_org, ImVec2 a_dir, ImVec2 a_nrm, float a_len,
                       unsigned int a_s) {
            for (float t = 0.0f; t < a_len; ) {
                const float t2 = (std::min)(t + step, a_len);
                const float d1 = TornDepth(t, a_s, S);
                const float d2 = TornDepth(t2, a_s, S);
                const ImVec2 q[4] = {
                    { a_org.x + a_dir.x * t,  a_org.y + a_dir.y * t },
                    { a_org.x + a_dir.x * t2, a_org.y + a_dir.y * t2 },
                    { a_org.x + a_dir.x * t2 + a_nrm.x * d2,
                      a_org.y + a_dir.y * t2 + a_nrm.y * d2 },
                    { a_org.x + a_dir.x * t  + a_nrm.x * d1,
                      a_org.y + a_dir.y * t  + a_nrm.y * d1 },
                };
                a_dl->AddConvexPolyFilled(q, 4, a_col);
                t = t2;
            }
        };

        const float w = a_max.x - a_min.x, h = a_max.y - a_min.y;
        run(ImVec2(a_min.x, a_min.y), ImVec2(1, 0), ImVec2(0, -1), w, a_seed);
        run(ImVec2(a_min.x, a_max.y), ImVec2(1, 0), ImVec2(0,  1), w, a_seed + 101u);
        run(ImVec2(a_min.x, a_min.y), ImVec2(0, 1), ImVec2(-1, 0), h, a_seed + 211u);
        run(ImVec2(a_max.x, a_min.y), ImVec2(0, 1), ImVec2( 1, 0), h, a_seed + 307u);
    }

    void Apply()
    {
        const Skin& sk = S();
        auto& style = ImGui::GetStyle();
        style.WindowRounding    = WinRounding();
        style.ChildRounding     = WinRounding();
        style.PopupRounding     = WinRounding();
        style.FrameRounding     = FrameRounding();
        style.GrabRounding      = FrameRounding();
        style.TabRounding       = FrameRounding();
        // cornerFade / tornFrame replace the full window border — kill the
        // geometry, not just the colour (decisive regardless of style state)
        style.WindowBorderSize  = (sk.cornerFade || sk.tornFrame || sk.bevelChrome) ? 0.0f : 1.0f;
        // mockup: every field/button/checkbox carries a visible border —
        // without it they read as invisible black boxes (v10.5 feedback)
        // ★Stays 1px. Widening it to hide a fill that bleeds past a rounded
        // corner was the wrong fix twice over: 2px made every small control
        // heavy, and the 1.5px in between was WORSE than 1 — baked-texture AA
        // only accepts integer widths, so a fractional stroke drops to the
        // geometric path where the solid core is (width - 1). The bleed is
        // fixed at the source instead: Sfx::Button insets the fill.
        style.FrameBorderSize   = 1.0f;
        // ★KEEP baked-texture AA lines ON. Turning it off to help the outline
        // pass (a baked line's uvs differ, so the pass read borders as glyphs)
        // cost every 1px border its definition: the geometric path spreads one
        // line across the centre vertex plus a transparent vertex 1px out on
        // each side, so on a half-pixel boundary the ink splits between two
        // pixels and neither reaches the colour asked for. Borders went pale
        // everywhere. The outline pass now recognises baked lines by their
        // constant v instead — see BuildTextOutline.
        style.AntiAliasedLinesUseTex = true;
        // ★-4px of side padding on a light panel. The frame is drawn ON the
        // window edge here rather than inset, so the content already reads as
        // held in; 12px on top of that pushed every column away from its own
        // border. Vertical stays — the title bar sets that rhythm.
        style.WindowPadding     = ImVec2(PadX(), PadY());
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);

        auto mix = [&](float a) { ImVec4 c = sk.acc; c.w = a; return c; };
        auto* c = style.Colors;
        ImVec4 win = sk.winBg;
        // opaque: the parked capture model hides behind windows. translucent
        // skins opt out — their park point is covered by the caching card.
        if (!sk.translucent) win.w = 1.0f;
        // tornFrame: Theme::TornPanel paints the (opaque) fill instead, so the
        // ImGui bg rect must be transparent or it squares off the tears
        c[ImGuiCol_WindowBg]         = sk.tornFrame ? ImVec4(0, 0, 0, 0) : win;
        // transparent: children must not paint over the window's frame chrome
        c[ImGuiCol_ChildBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        // ★★A popup is a READING surface. `win` carries the skin's own alpha,
        // so on Glass (0.58 / 0.38) the tooltip inherited it and the text had
        // the whole world showing through it. The WINDOW may be glass; the
        // card you read on top of it may not.
        c[ImGuiCol_PopupBg]          = ImVec4(win.x, win.y, win.z, 0.94f);
        // Border colours FRAMES (fields/buttons/checkbox — mockup .field/.abtn);
        // the skin-2 WINDOW border is killed via WindowBorderSize = 0 instead
        c[ImGuiCol_Border]           = mix(0.40f);
        // ★Everything stays WHITE. Contrast comes from the black outline
        // (21:1 whatever the panel does), not from flipping the ink dark —
        // dark labels next to white button text read as two different UIs.
        // Widget text carries the no-outline tag (Theme::Plain): ImGui draws
        // it and cannot outline it, so anything that needs the outline is
        // drawn by us through Theme::TextOutlined instead.
        c[ImGuiCol_Text]             = sk.ink;
        c[ImGuiCol_TextDisabled]     = sk.inkDim;
        // ★A black wash on a PALE panel comes out as dirty grey — sliders and
        // text fields read as smudges rather than as sunken tracks. A light
        // skin sinks its fields with a much lighter touch.
        c[ImGuiCol_FrameBg]          = sk.lightPanel ? ImVec4(0, 0, 0, 0.10f)
                                                     : ImVec4(0, 0, 0, 0.25f);
        c[ImGuiCol_FrameBgHovered]   = mix(0.12f);
        c[ImGuiCol_FrameBgActive]    = mix(0.18f);
        // ★A recessed button is a FACE colour, not an accent wash: the
        // reference paints it DARKER than the chrome it sits on, which an
        // alpha of the border colour can only approximate by accident.
        // btnFace alpha 0 = the skin does not use it, keep the wash.
        if (sk.btnFace.w > 0.0f) {
            auto lift = [&](float a) {
                return ImVec4(sk.btnFace.x + (sk.hi.x - sk.btnFace.x) * a,
                              sk.btnFace.y + (sk.hi.y - sk.btnFace.y) * a,
                              sk.btnFace.z + (sk.hi.z - sk.btnFace.z) * a,
                              sk.btnFace.w);
            };
            c[ImGuiCol_Button]        = sk.btnFace;
            c[ImGuiCol_ButtonHovered] = lift(0.28f);
            c[ImGuiCol_ButtonActive]  = lift(0.48f);
        } else {
            c[ImGuiCol_Button]        = mix(0.10f);
            c[ImGuiCol_ButtonHovered] = mix(0.22f);
            c[ImGuiCol_ButtonActive]  = mix(0.30f);
        }
        c[ImGuiCol_Header]           = mix(0.16f);
        c[ImGuiCol_HeaderHovered]    = mix(0.22f);
        c[ImGuiCol_HeaderActive]     = mix(0.28f);
        c[ImGuiCol_SliderGrab]       = mix(0.45f);
        c[ImGuiCol_SliderGrabActive] = ValVec();
        c[ImGuiCol_CheckMark]        = ValVec();
        c[ImGuiCol_Separator]        = sk.lightPanel ? ImVec4(sk.ink.x, sk.ink.y, sk.ink.z, 0.42f)
                                                     : mix(0.25f);
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
            // ★An OFF button shows the panel through it — no face of its own,
            // just the frame. Painting winBg here would not match the panel, it
            // would DOUBLE it: the button sits on the panel, so a second coat
            // of the same translucent colour lands at 0.90 instead of 0.68.
            // Transparent is the only fill that actually equals the ground.
            // ★A mint face, not grey. A grey control on an all-blue panel was
            // the one object on screen with no hue in common with anything
            // else, and it read as borrowed from another program.
            c[ImGuiCol_Button]         = sk.lightPanel
                ? LP(sk.lpBtn, Rgba(62, 110, 134)) : ImVec4(0.32f, 0.32f, 0.34f, 0.85f);
            c[ImGuiCol_ButtonHovered]  = sk.lightPanel
                ? LP(sk.lpBtnHov, Rgba(78, 132, 158)) : ImVec4(0.42f, 0.42f, 0.44f, 0.90f);
            c[ImGuiCol_ButtonActive]   = sk.lightPanel
                ? LP(sk.lpBtnAct, Rgba(48, 90, 112)) : ImVec4(0.26f, 0.26f, 0.28f, 0.92f);
            c[ImGuiCol_FrameBg]        = ImVec4(0.16f, 0.16f, 0.18f, 0.45f);
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.22f, 0.24f, 0.55f);
            c[ImGuiCol_FrameBgActive]  = ImVec4(0.28f, 0.28f, 0.30f, 0.65f);
            c[ImGuiCol_Border]         = sk.lightPanel
                ? LP(sk.lpBorder, Rgba(39, 80, 106)) : ImVec4(0.24f, 0.24f, 0.26f, 0.85f);
            c[ImGuiCol_PopupBg]        = ImVec4(win.x, win.y, win.z, 0.92f);
        }
    }
}
