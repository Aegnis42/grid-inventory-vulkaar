#pragma once

#include <cstdint>

namespace RE
{
    struct TESContainerChangedEvent;
    struct TESEquipEvent;
}

// 1.4 / B0 — OBSERVATION ONLY. Changes nothing about how the board is built.
//
// The question B0 exists to answer is one sentence: **does the sum of the
// engine's deltas always equal what a full recount says?** If it does not,
// reproducibly, 1.4 stops (PLAN §9).
//
// It answers three more along the way, all of which the review made into
// prerequisites rather than details:
//
//   1. ECHO. Our own systems call RemoveItem, and that raises another
//      ContainerChanged. An applier that cannot tell its own echo from an
//      outside delta cannot roll anything back. ★The ambiguity measurement
//      (2+ open requests sharing form+direction) lives in the LEDGER, which
//      does the matching -- this module only counts matched/unmatched.
//   2. THREADING. These events arrive on arbitrary threads. Arrival order and
//      apply order are not the same thing, and board deltas are order
//      sensitive, so both are stamped.
//   3. THE WHEEL. It deliberately does not pause the game, so it is the one
//      place our requests interleave with a live world.
//
// OFF unless GridInventory_ui.ini says "!delta = 1".
//
// ★Handlers run on unknown threads: this records FormIDs and numbers ONLY.
// No form lookups, no handle dereferences, no name strings. Everything that
// needs the game is done in Reconcile, which runs on the main thread.
namespace FUI::DeltaWatch
{
    [[nodiscard]] bool Enabled();
    void               SetEnabled(bool a_on);

    // Called FIRST inside the existing sinks -- not from a sink of our own.
    // A separate sink would be delivered in an order we do not control, and
    // "where does the applier sit among the existing consumers" is exactly
    // what B0 is here to see.
    //
    // ★a_req is the ledger's verdict for this event -- the label of the
    // request it confirmed, or nullptr for a surplus/outside delta. The SINK
    // asks the ledger and passes the answer down, because the ledger is
    // WIRING and this module is observation: when Confirm lived behind our
    // own enable switch, turning "!delta" off silently starved the ledger of
    // every confirmation and 100% of requests expired.
    void OnContainer(const RE::TESContainerChangedEvent* a_event, const char* a_req);
    void OnEquip(const RE::TESEquipEvent* a_event);

    // Main thread only. Compares baseline + accumulated deltas against a fresh
    // count of the player's inventory, reports every disagreement by form, and
    // re-baselines. Menu open/close are the natural moments.
    void Reconcile(const char* a_when);

    // ★Main thread, once per frame (UIRoot::Tick -- alive both paused and
    // unpaused). Refreshes the menu-context snapshot the EVENT handlers stamp
    // into their log lines. The handlers used to ask RE::UI directly, which
    // broke this header's own rule ("no lookups on unknown threads") -- the
    // UI's menu map belongs to the UI thread, and a diagnostic that can crash
    // is worse than no diagnostic (REVIEW B-3, PLAN §10-10).
    void RefreshMenuSnapshot();

    // ★Throws the running total away. A load replaces the whole inventory
    // without a single event, so a baseline from before it describes a
    // different character -- comparing across that boundary reports every form
    // the player owns as a disagreement, which is exactly what round 3 did.
    void Reset(const char* a_why);
}
