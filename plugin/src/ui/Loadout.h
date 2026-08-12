#pragma once

#include <vector>

// Loadout tabs (UI label "프리셋"): each tab is an independent equipment set.
// The ACTIVE tab's set is what the character actually wears. Switching snapshots
// the leaving tab's worn gear, unequips all, then re-equips the target set. Gear
// held by an inactive tab is hidden from the grid (see IsReserved). Real equip —
// stats follow the set, no fragile hooks.
//
// Internal name "Loadout" avoids clashing with the pre-existing editor "preset"
// (icon/category config sharing).

namespace FUI::Loadout
{
    [[nodiscard]] int  Count();          // number of loadouts (>=1; [0]=EQUIP base)
    [[nodiscard]] int  Active();         // active index
    [[nodiscard]] const char* Name(int a_index);
    void SetName(int a_index, const char* a_name);   // presets only (index>=1)

    // purchase economics (L2): cost = 5000 * (owned preset tabs + 1) — deleting
    // a tab restores the tier. No refunds.
    //
    // ★★NINE, because the quick wheel has ten places and EQUIP takes one of
    // them. The cap used to be 20, which the inventory strip could show and the
    // wheel could not: tab eleven onward simply never appeared there, with
    // nothing said about it. A limit the player cannot see is worse than a
    // lower one they can, and "buy a tab that half the mod ignores" is not a
    // purchase anyone would make knowingly.
    // ★This counts PRESETS, not tabs -- EQUIP is not a preset and is not sold.
    // Wheeler.cpp static_asserts that the two still agree, so raising this
    // without widening the wheel fails the build rather than the player.
    // ★Saves made under the old cap keep every tab they have: nothing is
    // deleted, only further purchases stop. Those extra tabs still work in the
    // inventory; they are the ones the wheel could never show anyway.
    inline constexpr int kMaxPresets = 9;
    [[nodiscard]] int  NextCost();
    [[nodiscard]] int  PlayerGold();
    [[nodiscard]] bool AtCap();          // preset count reached kMaxPresets

    void RequestSwitch(int a_target);    // deferred to Tick (never from render pass)
    void RequestPurchase();              // buy + add a preset + switch (deferred)
    void RequestRemove(int a_index);     // delete a preset (deferred; index>=1)
    void ProcessPending();               // Tick: perform deferred purchase/remove/switch

    // ★★THIS LIST HAS NO REARRANGE, and that is a decision rather than a gap.
    // A tab's index is its identity everywhere it is referred to -- the costume
    // points at one, the cosave writes them, EQUIP is 0 because it is not a
    // preset -- so moving tabs about means renumbering all of that for a
    // preference about where a thing appears.
    // The quick wheel wanted exactly that preference and keeps it itself, as an
    // arrangement of slots over these indices (see Wheeler's g_setOrder). What
    // exists is this list's business; where it sits on screen is the wheel's.

    // True if a_id belongs to an INACTIVE loadout tab: that gear is held by the
    // tab, so it must be hidden from the grid (character is undressed for it but
    // it does NOT return to the inventory). Active-tab gear = normal worn check.
    // HOW MANY units the inactive tabs hold back, not merely whether any do.
    // A form-level yes/no hid the whole inventory entry, so once a preset
    // referenced (say) an iron dagger, every iron dagger the player owned or
    // FORGED vanished from the board and from the capacity sum -- unreachable,
    // since the vanilla inventory is suppressed.
    [[nodiscard]] int ReservedCount(RE::FormID a_id);
    // The signatures of those reserved units, so the grid can hold back the
    // RIGHT one. Without this the generic "plain pool first" rule hid the plain
    // dagger while the preset was actually wearing the tempered one.
    [[nodiscard]] std::vector<std::uint16_t> ReservedSigs(RE::FormID a_id);
    [[nodiscard]] bool IsReserved(RE::FormID a_id);

    // The forms a tab is holding, for the costume system to read an outfit out
    // of a tab without equipping it, and for the quick menu to draw it.
    // ★★Answers for the ACTIVE tab too, and answers CURRENTLY. It used to hand
    // back a snapshot taken when the tab was last switched to -- correct for
    // every tab except the one being worn, which is the one that changes. The
    // rule was "do not ask about the active tab", the costume system obeyed it
    // by excluding that tab, and the quick menu did not: it drew the weapon a
    // preset held at the last switch, so equipping a sword changed nothing
    // until the player switched away and back. A getter with a caveat is a
    // getter that will be called wrongly, so the caveat is gone instead: the
    // active tab is brought back in step with the worn gear every tick.
    // ★It is a plain read, safe from the render pass. The re-read itself is an
    // inventory scan and lives in ProcessPending, on the game thread -- so the
    // list can be at most one tick behind, and never scanned from under the
    // thread that is allowed to change it.
    [[nodiscard]] std::vector<RE::FormID> FormsOf(int a_index);

    // "The worn gear moved" -- the active tab is re-read on the next tick.
    // Cheap: a flag, not a scan, and many of them collapse into one re-read.
    // Called from the equip event sink, and by anything about to show the
    // active set that would rather not trust the event stream (the quick menu
    // does this when it opens).
    void MarkActiveStale();

    void ResetSession();                 // load/new-game: back to base, clear presets

    // L3: SKSE cosave persistence. Revert (fires before every load AND on new
    // game) resets; LoadRecord restores one 'LODT' record (main.cpp owns the
    // GetNextRecordInfo loop and dispatches by type). Do NOT also reset from
    // kPostLoadGame: that message arrives AFTER the load callback and would
    // wipe the freshly loaded tabs.
    inline constexpr std::uint32_t kRecordType = 'LODT';
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void RevertGame(SKSE::SerializationInterface* a_intfc);
}
