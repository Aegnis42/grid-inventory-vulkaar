#pragma once

#include "ui/Grid.h"

#include <string>

namespace FUI::Equip
{
    // B-5 (PLAN_B §2-D): equipment doll. Slot strip (circlet + 4 accessories)
    // over a 3x4 doll; worn items render their cached icons; right-click
    // unequips; the carried item can be dropped on a slot to equip (C6/D4).

    // Equip an item with the vanilla type gate (WEAP/ARMO/AMMO/LIGHT/ALCH/
    // SCRL). a_slotId == "shieldL" routes one-handers/staves to the LEFT hand
    // (BGSEquipSlot 0x13F43). Returns true when the gate accepted the item.
    // GI1/D4: a_uid/a_xlIdx name WHICH copy to equip. Every ActorEquipManager
    // call used to pass a null ExtraDataList, which lets the ENGINE choose the
    // sub-stack -- so equipping the enchanted one of three identical swords was
    // a coin flip. nullptr/0/-1 keeps the old "engine picks" behaviour for
    // callers that genuinely have no instance in hand.
    bool EquipItem(RE::TESBoundObject* a_obj, const std::string& a_slotId,
                   std::uint16_t a_uid = 0, int a_xlIdx = -1, std::uint16_t a_sig = 0,
                   const std::string& a_srcKey = {});

    // The object / sub-stack worn in a doll slot ("weapon", "shieldL", "head"...).
    // Needed by anything that must act on the WORN copy rather than the form.
    [[nodiscard]] RE::TESBoundObject* WornObjectAt(const std::string& a_slotId);
    [[nodiscard]] RE::ExtraDataList*  WornExtraAt(const std::string& a_slotId);

    // Equip/unequip requests queue here and execute in the game-update hook
    // (UIRoot::Tick) — calling ActorEquipManager inside the UI RENDER pass
    // defers the 3D refresh until the menu closes (paused game).
    void ProcessPending();

    // Draw the doll panel at the current cursor pos (inside the main window).
    void Draw();

    // L2: loadout buy/delete confirm windows — call at TOP LEVEL (like the
    // settings window), NOT inside the equip panel child.
    void DrawLoadoutWindows();

    // GI53: drop popup state (buy/delete asks) when the menu closes — a
    // confirm left open used to reappear on the next open.
    void OnMenuClosed();
    // true while EITHER loadout popup is up. One layer to the close-order
    // stack; CloseTopPopup keeps its own buy-then-delete order inside.
    [[nodiscard]] bool IsPopupOpen();
    bool CloseTopPopup();   // I/ESC layering: close an open buy/delete popup

    // v9 spec: every slot = 2x2 inventory cells; weapon/body/shield = 2x4 (+5px)
    // ★kDollTrim: the doll is measured in cells like everything else, but it —
    // not the grid — sets how tall the left column is, and the stats panel
    // below it is TYPE, so it does not shrink with the cells. Taking 5% off
    // the slots (and halving the gaps) buys back the room the stats panel
    // needs at small cell sizes, without touching a single font size.
    inline constexpr float kDollTrim = 0.95f;
    [[nodiscard]] inline float SlotPx() { return Grid::CellPx() * 2.0f * kDollTrim; }
    [[nodiscard]] inline float GapPx() { return 2.5f * Theme::Scale(); }
    // ★A tall slot must equal TWO short ones plus the gap between them — that
    // is what keeps the weapon/body column level with the accessory column
    // beside it. The old literal +5 happened to equal the gap, so halving the
    // gap silently broke the identity and the two columns drifted apart by
    // exactly the difference. Say it as the identity instead of a number.
    [[nodiscard]] inline float TallPx() { return 2.0f * SlotPx() + GapPx(); }
    // vertical strip (circlet + acc x4) + doll 3 columns
    [[nodiscard]] inline float PanelW() { return 4 * SlotPx() + 3 * GapPx(); }
    // tabs row + doll rows [2u, 2x4, 2u, 2u]
    [[nodiscard]] inline float PanelH()
    {
        return 30.0f * Theme::Scale() + 3 * SlotPx() + TallPx() + 3 * GapPx();
    }

    // ★GI77: how far the first slot row sits below the column's top — i.e. the
    // height the loadout tab strip actually took. MEASURED during the last
    // Draw() rather than assumed: the 30*S above is a reservation that rounds
    // generously, and the item grid has to line up with the real slot edge,
    // not with a budget. 0 before the first draw.
    [[nodiscard]] float SlotsTopOffset();
}
