#pragma once

#include <cstdint>

namespace SKSE
{
    class SerializationInterface;
}

// THE SECOND RING.
//
// The engine wears exactly one. BipedAnim is `BIPOBJECT objects[kTotal]` --
// one item per slot -- and kRing is a single bit, so a second ring has
// nowhere to go. Moving the slot does not help: probed three ways (both
// masks moved, ARMO only, and claiming NO slot at all) the engine took the
// first ring off every time, so whatever it single-ends on, it is not the
// biped slot. ETYP is not it either -- both rings report 0.
//
// So the second ring is not worn AS a ring. A CARRIER of ours is equipped in
// its place -- costume anchor 32 (0x84A), which is not a ring and so nothing
// single-ends it -- borrowing the ring's ENCHANTMENT. The ring itself stays in
// the pack, and the UI shows it on the doll's second ring slot.
//
// ★★★NO MODEL, and that is a settled question rather than an omission. A
// skinned mesh carries its own slot number inside the NIF
// (BSDismemberSkinInstance::partitions[i].slot), and for every ring it is 36 --
// which the FIRST ring already occupies. Nothing set on the ARMO or the ARMA
// changes it, so within the equipment system the second ring cannot be drawn:
// make the addon claim kRing and it loses the contest for 36; take kRing off it
// and nothing matches 36 at all. (Measured: the second ring appeared the
// instant the first was removed, i.e. when 36 came free.)
// Outside the equipment system it is no better -- a model loaded by hand
// through BSModelDB comes back with `skin=NO`, because binding a skin to the
// skeleton is work the engine does while building a part.
// Both routes were taken to the end. The effect applies; the ring is invisible.
namespace FUI::DualRing
{
    // ---- what is on the second slot --------------------------------------
    // The ring the carrier is standing in for, or null. This is the RING, not
    // the carrier: everything above the game layer -- doll, tooltips,
    // transfers -- wants the item the player thinks they are wearing.
    [[nodiscard]] RE::TESObjectARMO* Second();

    // ★The carrier is not the player's property. Hide it exactly where the
    // costume anchors are hidden -- grid, doll, capacity, tooltips, transfers.
    [[nodiscard]] bool IsCarrier(const RE::TESForm* a_form);

    // ---- the rules --------------------------------------------------------
    // ★One place, asked by both the drop target and the act. The UI must be
    // able to refuse a drag WITHOUT restating these, or the two copies drift
    // and the player gets a slot that accepts a ring and then does nothing.
    // ★An empty FIRST slot is deliberately not a refusal. Picking a ring up
    // empties the slot it came from, so refusing on that basis made the drag
    // asymmetric -- left-to-right took the ring off instead of moving it.
    // Where a ring lands is Wear's decision, not a veto here.
    enum class Verdict : std::uint8_t
    {
        kOk,
        kNotARing,
        kAlreadyWorn,    // this very ring is already on one of the two slots
        kSameEffect,     // ★the feature's whole point: no stacking a duplicate
        kNoCarrier,      // the ESP record is missing
        kNoFreeSlot,
    };
    [[nodiscard]] Verdict CanWear(RE::TESObjectARMO* a_ring);
    // English, for the log. A player-facing string would mean a new Lang key
    // and four translations; nothing shows these to the player yet.
    [[nodiscard]] const char* VerdictText(Verdict a_v);

    // ---- acts -------------------------------------------------------------
    // ★Equip-QUEUE only (game thread, outside the render pass): both call
    // ActorEquipManager, and doing that inside the render pass defers the 3D
    // refresh until the menu closes.
    // Wear also decides WHERE the ring goes -- it fills an empty first slot,
    // or trades places with the ring already on the second.
    bool Wear(RE::TESObjectARMO* a_ring, RE::ExtraDataList* a_xl);
    void TakeOff();
    // Queued form of TakeOff, for callers inside the render pass.
    void RequestTakeOff();

    // ---- lifecycle --------------------------------------------------------
    // Per game-update tick. ★Drops the whole thing if the ring left the
    // inventory -- sold, dropped, taken by a script. Observing beats
    // remembering: the world can change behind this system's back, and the
    // stale case then repairs itself on the next tick.
    void Tick();

    inline constexpr std::uint32_t kRecordType = 'DRNG';
    void RevertGame(SKSE::SerializationInterface* a_intfc);   // new game / pre-load
    void SaveGame(SKSE::SerializationInterface* a_intfc);
    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    // ★The engine re-read the forms, so the enchantment we lent the carrier is
    // gone while the EQUIP survived in the save -- a carrier worn with no
    // enchantment is a second ring that quietly stopped working. Re-lends it.
    void OnLoad();
}
