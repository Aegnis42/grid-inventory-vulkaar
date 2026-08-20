#pragma once

// 1.4 / B4-2 -- the WORN LEDGER, observation mode.
//
// The board holds only unworn units, and re-reads who is worn from the
// engine's ExtraWorn on every derivation -- which is why half the exclusion
// ladder exists (PLAN_B4_DEMOLITION §2). Before that arithmetic can flip to
// "the board knows", the ledger has to prove it can stay in step with the
// engine on events alone.
//
// This phase therefore only COUNTS: worn stacks per form, advanced by the
// player's TESEquipEvents (a quiver equips as one list however many arrows
// ride in it, and the event fires once -- lists are the unit both sides can
// agree on), rebaselined wholesale at every load (rule 3: a load is a
// discontinuity, and the engine is the authority across one). Nothing
// consumes it. Every menu open and close audits the ledger against a fresh
// ExtraWorn walk and logs agreement or mismatch -- the B0~B2 precedent:
// measure to a standstill first, then promote.
namespace FUI::WornLedger
{
    // Both arrive already marshalled to the main thread (the sink AddTasks;
    // equip events land on arbitrary threads -- rule 4).
    void OnEquip(RE::FormID a_form);
    void OnUnequip(RE::FormID a_form);

    // Load replaces the inventory wholesale: rebuild from the engine, once.
    void Rebaseline(const char* a_why);

    // Compare against the engine's ExtraWorn walk and log the verdict. In
    // observation mode a mismatch also bends the ledger back to the engine --
    // the engine is still the authority, and each divergence must be counted
    // from a clean baseline or one miss would echo through every later audit.
    void Audit(const char* a_when);
}
