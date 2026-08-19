#pragma once

// 1.4 / B1 — OBSERVATION ONLY, one step above B0.
//
// B0 counted FORMS. That is enough to ask "did the engine tell us everything",
// and the answer was yes. It is not enough to run a board: two iron daggers are
// the same form and different things the moment one of them is tempered.
//
// B1 counts KINDS -- form + InstanceSig -- and reports what moved between them.
// The question it exists to answer is PLAN §3 / REVIEW B-2:
//
//   **When N units of one form all change value at once, can the vanished
//   kinds be paired with the appeared ones without guessing?**
//
// If they cannot, 1.4 has not removed the matching problem, only moved it into
// the census, and §9 has a stopping rule about that.
//
// ★It records the RAW VALUES too, not just the signature. The pairing rule in
// §3 is "closest value first" -- and a signature is a hash, which has no
// distance. That rule cannot be evaluated, let alone implemented, from sigs
// alone. Learning this is itself part of what B1 was for.
//
// OFF unless GridInventory_ui.ini says "!census = 1".
namespace FUI::Census
{
    [[nodiscard]] bool Enabled();
    void               SetEnabled(bool a_on);

    // Main thread only (walks the player's inventory).
    void Take(const char* a_when);

    // A load replaces the inventory wholesale -- the previous census describes
    // someone else. Same rule B0 had to learn the hard way.
    void Reset(const char* a_why);
}
