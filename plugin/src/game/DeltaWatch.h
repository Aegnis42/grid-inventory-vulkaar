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
//      outside delta cannot roll anything back. We measure how often the
//      (form, direction, count) triple is ambiguous.
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
    void OnContainer(const RE::TESContainerChangedEvent* a_event);
    void OnEquip(const RE::TESEquipEvent* a_event);

    // (Requests moved to Ledger in B2 -- see Ledger.h. This module reads the
    // ledger to label an arriving event; it no longer keeps its own copy.)

    // Main thread only. Compares baseline + accumulated deltas against a fresh
    // count of the player's inventory, reports every disagreement by form, and
    // re-baselines. Menu open/close are the natural moments.
    void Reconcile(const char* a_when);

    // ★Throws the running total away. A load replaces the whole inventory
    // without a single event, so a baseline from before it describes a
    // different character -- comparing across that boundary reports every form
    // the player owns as a disagreement, which is exactly what round 3 did.
    void Reset(const char* a_why);
}
