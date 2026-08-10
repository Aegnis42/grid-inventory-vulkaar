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
    // a tab restores the tier. No refunds. Hard cap: 20 preset tabs.
    inline constexpr int kMaxPresets = 20;
    [[nodiscard]] int  NextCost();
    [[nodiscard]] int  PlayerGold();
    [[nodiscard]] bool AtCap();          // preset count reached kMaxPresets

    void RequestSwitch(int a_target);    // deferred to Tick (never from render pass)
    void RequestPurchase();              // buy + add a preset + switch (deferred)
    void RequestRemove(int a_index);     // delete a preset (deferred; index>=1)
    void ProcessPending();               // Tick: perform deferred purchase/remove/switch

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
    // of a tab without equipping it.
    // ★Only meaningful for an INACTIVE tab. The active tab's list is a snapshot
    // taken when it was last switched TO, so it goes stale the moment the player
    // equips anything; the live answer for the active tab is the worn gear
    // itself. Costume::CanBeTab already excludes the active tab, which is what
    // keeps this from being asked the question it cannot answer.
    [[nodiscard]] std::vector<RE::FormID> FormsOf(int a_index);

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
