#pragma once

#include <optional>

// 1.4 / B1 — kind-level audit, one step above B0. Observation-only at birth;
// promoted to permanent wiring below (★★PROMOTED).
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
// ★★PROMOTED: ON BY DEFAULT, and no longer observation-only. Each Take now
// ASSIGNS the pairs it used to merely rank -- greedily, fewest changed axes
// first, normalised distance as the tiebreak (the B1-measured rule, PLAN
// §8-4) -- and the rebuild's relabel block consumes the assignment through
// TakePair. Before this, N vacated pools met M arriving pools in HASH ORDER,
// which is no order at all: two same-form blades re-tempered in one
// grindstone session (values move, counts do not, no event fires) could come
// back seated in each other's cells at the next menu open, the very §1(b)
// violation the census was built to measure. The assignment only concerns
// UNWORN units -- a worn item is off the board, so combat charge drain has
// nothing to relabel. "!census = 0" remains as an emergency cutoff (the
// ledger's promotion pattern).
namespace FUI::Census
{
    [[nodiscard]] bool Enabled();
    void               SetEnabled(bool a_on);

    // Main thread only (walks the player's inventory).
    // ★Returns whether anything MOVED between kinds since the last take
    // (B4-1): the menu-open rebuild is conditional now, and a value that
    // changed without a count changing -- a grindstone, above all -- fires no
    // engine event and so raises no rebuild flag. The census is the only
    // witness, and its yes is the integrity gate that requests the rebuild.
    // A baseline take (or an empty snapshot) returns false.
    bool Take(const char* a_when);

    // A load replaces the inventory wholesale -- the previous census describes
    // someone else. Same rule B0 had to learn the hard way.
    void Reset(const char* a_why);

    // The verdict, one pair at a time: which appeared kind the rule assigned
    // to this vanished kind. CONSUMING -- a pair answers once, then retires
    // (§1 rule 7's shape: a confirmation retires only its own entry), so a
    // stale verdict cannot steer a later, unrelated relabel. Empty when the
    // census has no opinion; sig 0 (the plain pool) is a legitimate answer,
    // which is why the miss is an empty optional and not a zero.
    [[nodiscard]] std::optional<std::uint16_t> TakePair(RE::FormID a_form,
                                                        std::uint16_t a_goneSig);

    // Non-consuming twin for the GEAR matcher, whose tier predicate runs once
    // per (unit, slot) pair and so cannot afford to retire the answer on
    // first read. Its binding retires itself instead: the slot's sig is
    // rewritten to the unit's on commit, so the next rebuild matches on
    // contents without asking here again. (All the relabels B1 ever measured
    // were gear -- this, not the stackable block, is the rule's main seat.)
    [[nodiscard]] std::optional<std::uint16_t> PeekPair(RE::FormID a_form,
                                                        std::uint16_t a_goneSig);
}
