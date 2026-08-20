#include "game/DualRing.h"

#include "game/Costume.h"
#include "game/WornLedger.h"
#include "ui/Grid.h"

#include <vector>

namespace FUI::DualRing
{
    namespace
    {
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        using BO   = RE::BIPED_OBJECTS::BIPED_OBJECT;

        constexpr std::uint32_t kSlots = BO::kEditorTotal;   // 32

        // ★Costume anchor 32. Costume::kAnchorCount is 31 precisely so this one
        // is never raised OR dropped by that system: `need` there is
        // `j < groups.size()` and groups can hold at most 31 entries, so the
        // costume could never want it -- but its loop would still drop one it
        // found held, which is why the count was lowered rather than shared.
        constexpr std::uint32_t kCarrierId = 0x84A;
        constexpr const char*   kPlugin    = "Grid Inventory.esp";

        RE::FormID    g_ringId  = 0;   // the ring the carrier stands in for
        std::uint16_t g_ringSig = 0;   // its content signature (see SecondSig)
        bool          g_wantOff = false;  // take-off asked for from the render pass

        // ★What the carrier borrowed, so it can be handed back. The carrier is
        // OUR form and no other actor wears it, so this is bookkeeping rather
        // than the safety problem it would be for a shared armour -- but a
        // stale enchantment would still follow it into its next use.
        struct Lent
        {
            RE::EnchantmentItem* ench = nullptr;
            bool                 held = false;
        };
        Lent g_lent;

        [[nodiscard]] RE::TESObjectARMO* Carrier()
        {
            // Not a function-local static initialiser: a miss must be
            // retryable. This can run before the data handler has the plugin,
            // and caching null there would disable the feature for the session.
            static RE::TESObjectARMO* cached = nullptr;
            if (!cached) {
                if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                    cached = dh->LookupForm<RE::TESObjectARMO>(kCarrierId, kPlugin);
                }
                // ★★★THE CARRIER MUST BE NAKED. It is a byte-for-byte clone of
                // the costume anchors, and the TEMPLATE carries a circlet's
                // whole wardrobe: BOD2 on the HAIR and circlet slots, a
                // circlet ARMA on its armature. The ordinary anchors never
                // show it -- the costume overwrites their armorAddons with
                // donor lists -- but the carrier is anchor 32 precisely so
                // the costume leaves it alone, which also left the authored
                // circlet addon LIVE. Wearing it therefore claimed the hair
                // slot and fought the helmet's addon: helmet invisible over a
                // bald head, on WHATEVER biped slot we parked the ARMO
                // (measured: 60 and 59 alike -- the ARMO slot was never the
                // actor, its armature was). Strip the armature once, here,
                // where the form is first resolved; the slot mask is
                // rewritten per wear by Lend already.
                if (cached && !cached->armorAddons.empty()) {
                    cached->armorAddons.clear();
                    SKSE::log::info("[DUALRING] carrier armature stripped "
                                    "(authored circlet addon)");
                }
            }
            return cached;
        }

        [[nodiscard]] RE::TESObjectARMO* RingById(RE::FormID a_id)
        {
            if (!a_id) return nullptr;
            auto* f = RE::TESForm::LookupByID(a_id);
            return f ? f->As<RE::TESObjectARMO>() : nullptr;
        }

        [[nodiscard]] const char* NameOf(RE::TESForm* a_f)
        {
            const char* n = a_f ? a_f->GetName() : nullptr;
            return (n && *n) ? n : "<unnamed>";
        }

        // The ring the ENGINE is wearing on kRing, if any.
        [[nodiscard]] RE::TESObjectARMO* FirstRing(RE::PlayerCharacter* a_p)
        {
            if (!a_p) return nullptr;
            for (const auto& [obj, data] : a_p->GetInventory(
                     [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
                if (data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                auto* a = obj->As<RE::TESObjectARMO>();
                if (!a || Costume::IsAnchor(a) || IsCarrier(a)) continue;
                if (Grid::IsRing(a)) return a;
            }
            return nullptr;
        }

        // ★★"Same effect" is NOT "same enchantment form". Two rings of one
        // family at different strengths are separate forms carrying the same
        // EffectSetting, and stacking those is exactly what this feature exists
        // to prevent -- so the comparison is on the base effects.
        [[nodiscard]] bool ShareAnEffect(RE::TESObjectARMO* a_x, RE::TESObjectARMO* a_y)
        {
            if (!a_x || !a_y) return false;
            auto* ex = a_x->formEnchanting;
            auto* ey = a_y->formEnchanting;
            if (!ex || !ey) return false;   // an unenchanted ring stacks with anything
            if (ex == ey) return true;
            for (auto* px : ex->effects) {
                if (!px || !px->baseEffect) continue;
                for (auto* py : ey->effects) {
                    if (py && py->baseEffect == px->baseEffect) return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::uint32_t WornMask(RE::PlayerCharacter* a_p)
        {
            std::uint32_t used = 0;
            for (const auto& [obj, data] : a_p->GetInventory(
                     [](RE::TESBoundObject& o) { return o.IsArmor(); })) {
                if (data.first <= 0 || !data.second || !data.second->IsWorn()) continue;
                if (auto* a = obj->As<RE::TESObjectARMO>()) {
                    used |= static_cast<std::uint32_t>(a->GetSlotMask());
                    // ★★THE ADDONS TOO. A modded helmet often draws through an
                    // ArmorAddon that covers slots its ARMO mask never names
                    // (hair physics on 60 is the classic). The ARMO mask alone
                    // called those slots free, the carrier sat down on one,
                    // and the engine culled the helmet's addon over the
                    // conflict: wear both rings, and the helmet turns
                    // invisible over a bald head (user report -- the preset
                    // cycling that "healed" it was removing the second ring).
                    for (auto* arma : a->armorAddons) {
                        if (arma) used |= static_cast<std::uint32_t>(arma->GetSlotMask());
                    }
                }
            }
            return used;
        }

        // A biped slot nothing is wearing right now.
        // ★"!ring2slot = N" (editor 44..60): the player's word on where the
        // carrier may sit. Slot habits are a MODLIST fact no mask can read --
        // the measurement below proves it -- so the escape hatch has to be an
        // ini line, not another heuristic.
        int g_slotOverride = -1;   // bit index; -1 = pick automatically

        // ★Searched from the TOP: the low custom slots (44-49) are where
        // cloaks, backpacks and lanterns live, so taking one of those picks a
        // fight with whatever the player already has installed. kFX01 (31) is
        // skipped as well -- it is the effect slot and builds no armour.
        [[nodiscard]] int FreeSlot(RE::PlayerCharacter* a_p)
        {
            const std::uint32_t used = WornMask(a_p);
            if (g_slotOverride >= 0 && !(used & (1u << g_slotOverride))) {
                return g_slotOverride;
            }
            for (int i = static_cast<int>(kSlots) - 3; i >= 14; --i) {
                // ★editor slots 50/51 (bits 20/21): the DECAPITATION slots.
                // Equipping anything there culls the head outright -- never a
                // valid parking spot however crowded the rest of the biped is.
                if (i == 20 || i == 21) continue;
                if (!(used & (1u << i))) return i;
            }
            return -1;
        }
        // ★★...and the scan now stops BELOW editor slot 60 (bit 30). Measured:
        // no worn ARMO or addon claimed 60, the carrier sat there, and the
        // helmet still went invisible over a bald head -- something in the
        // MODLIST watches that slot (hair-physics and helmet-toggle systems
        // are the usual tenants). A mask cannot see a watcher; the only
        // honest move is to stay out of the known bad neighbourhood and hand
        // the player the "!ring2slot" override for whatever their list does.

        // Lend the ring's enchantment to the carrier and put it on a_mask.
        // ★A ring's enchantment is kConstantEffect (verified: no vanilla ARMO
        // carries EAMT at all), so there is no charge to manage -- lending the
        // form IS lending the effect. It must be in place BEFORE the equip,
        // because that is when the engine reads it.
        void Lend(RE::TESObjectARMO* a_carrier, RE::TESObjectARMO* a_ring,
                  std::uint32_t a_mask)
        {
            if (!g_lent.held) {
                g_lent.ench = a_carrier->formEnchanting;
                g_lent.held = true;
            }
            a_carrier->formEnchanting = a_ring->formEnchanting;
            a_carrier->bipedModelData.bipedObjectSlots = static_cast<Slot>(a_mask);
        }

        void Reclaim()
        {
            auto* c = Carrier();
            if (!c || !g_lent.held) return;
            c->formEnchanting = g_lent.ench;
            g_lent = {};
        }
    }

    void TakeOffImpl(bool a_standalone);   // defined below TakeOff

    void SetSlotOverride(int a_editorSlot)
    {
        // editor numbers run 30..61; bits run 0..31. Decapitation (50/51) and
        // FX01 (61) are refused even by hand -- they cull the head or build
        // no armour at all.
        const int bit = a_editorSlot - 30;
        const bool ok = bit >= 14 && bit <= 30 && bit != 20 && bit != 21;
        g_slotOverride = ok ? bit : -1;
        if (ok) {
            SKSE::log::info("[DUALRING] carrier slot pinned to {} (!ring2slot)",
                            a_editorSlot);
        }
    }

    int SlotOverride()
    {
        return g_slotOverride < 0 ? -1 : g_slotOverride + 30;
    }

    RE::TESObjectARMO* Second() { return RingById(g_ringId); }

    std::uint16_t SecondSig() { return g_ringSig; }

    bool WouldDuplicate(RE::TESObjectARMO* a_ring)
    {
        auto* second = RingById(g_ringId);
        return second && ShareAnEffect(second, a_ring);
    }

    bool SharesEffect(RE::TESObjectARMO* a_x, RE::TESObjectARMO* a_y)
    {
        return ShareAnEffect(a_x, a_y);
    }

    bool IsCarrier(const RE::TESForm* a_form)
    {
        auto* c = Carrier();
        return c && a_form && a_form->GetFormID() == c->GetFormID();
    }

    Verdict CanWear(RE::TESObjectARMO* a_ring)
    {
        if (!a_ring || !Grid::IsRing(a_ring)) return Verdict::kNotARing;
        if (!Carrier()) return Verdict::kNoCarrier;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return Verdict::kNoCarrier;

        // ★★The OTHER ring, whichever slot it is on. Asking only about the
        // first slot made the drop rules asymmetric: dragging the LEFT ring
        // onto the right slot picks it up first, which empties the left slot,
        // and the drop was then refused for having no first ring -- so the
        // ring came off instead of moving. The right-to-left direction worked
        // only because that check happened to pass.
        auto* other = FirstRing(p);
        if (!other) other = RingById(g_ringId);
        if (other == a_ring) {
            // ★FORM identity, not unit identity. A SPARE unit of the same form
            // is a second ring, and a plain pair of one form is legal (the
            // rule is the EFFECT, one test below) -- so refuse only when the
            // player owns a single unit, where "wear it beside itself" is the
            // only thing this drop could mean. The old form-level refusal
            // blocked plain pairs outright (user spec correction).
            int owned = 0;
            for (const auto& [obj2, data] : p->GetInventory(
                     [&](RE::TESBoundObject& o) { return &o == a_ring; })) {
                owned = data.first;
                (void)obj2;
            }
            if (owned <= 1) return Verdict::kAlreadyWorn;
        }
        if (ShareAnEffect(other, a_ring)) return Verdict::kSameEffect;
        if (FreeSlot(p) < 0) return Verdict::kNoFreeSlot;
        // ★An empty first slot is no longer a refusal. Where the ring lands is
        // Wear's decision -- it fills the first slot, or trades places with the
        // ring already on the second -- because a slot that takes a drag and
        // then drops the item on the floor is the worst of the options.
        return Verdict::kOk;
    }

    const char* VerdictText(Verdict a_v)
    {
        switch (a_v) {
        case Verdict::kNotARing:    return "not a ring";
        case Verdict::kAlreadyWorn: return "already worn";
        case Verdict::kSameEffect:  return "the same effect is already worn";
        case Verdict::kNoCarrier:   return "carrier form missing (is the esp loaded?)";
        case Verdict::kNoFreeSlot:  return "no free biped slot";
        default:                    return "";
        }
    }

    bool Wear(RE::TESObjectARMO* a_ring, RE::ExtraDataList* a_xl)
    {
        const auto v = CanWear(a_ring);
        if (v != Verdict::kOk) {
            SKSE::log::info("[DUALRING] refused '{}': {}", NameOf(a_ring), VerdictText(v));
            return false;
        }
        auto* p  = RE::PlayerCharacter::GetSingleton();
        auto* c  = Carrier();
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!p || !c || !em) return false;

        // ★★Where it actually goes. The second slot cannot hold a ring on its
        // own -- the doll would show a gap with nothing above it -- so an empty
        // first slot is filled instead of refused, and that is also what makes
        // the drag symmetric.
        if (!FirstRing(p)) {
            if (g_ringId) {
                // ★SWAP. The player dragged the first ring onto the second
                // slot; the two trade places. The one on the carrier goes back
                // to the engine's own ring slot, and the incoming one takes the
                // carrier.
                auto* prev = RingById(g_ringId);
                TakeOffImpl(/*a_standalone=*/false);   // B4-4: handoff, no redraw
                if (prev) {
                    em->EquipObject(p, prev, nullptr, 1, nullptr,
                                    false, false, false, true);
                    SKSE::log::info("[DUALRING] swap: '{}' moved to the first slot",
                        NameOf(prev));
                }
            } else {
                // Nothing on either slot: this belongs on the FIRST one.
                em->EquipObject(p, a_ring, nullptr, 1, nullptr,
                                false, false, false, true);
                SKSE::log::info("[DUALRING] '{}' -> first slot (the second cannot be "
                                "filled alone)", NameOf(a_ring));
                return true;
            }
        }

        if (g_ringId) TakeOffImpl(/*a_standalone=*/false);   // B4-4: handoff

        const int slot = FreeSlot(p);
        if (slot < 0) return false;
        const std::uint32_t mask = 1u << slot;

        Lend(c, a_ring, mask);
        // The carrier is not the player's item; it goes in the pack purely so
        // the engine has something to equip, and comes back out on removal.
        p->AddObjectToContainer(c, nullptr, 1, nullptr);
        em->EquipObject(p, c, nullptr, 1, nullptr, false, false, false, true);

        g_ringId = a_ring->GetFormID();
        g_ringSig = Grid::InstanceSigOf(a_xl);   // nullptr -> 0 (plain)
        // B4-2b: the ring's own equip never runs on this path -- the carrier
        // stands in -- so the worn ledger's pending for it would go stale
        // (measured, round one of the state machine). Withdraw it here.
        WornLedger::CancelPending(g_ringId);
        // ★B4-4: and the GRID's equip-queue entry retires here too -- this
        // moment IS the carrier route's landing. Waiting for the TTL let a
        // swap spam pile up entries that each excluded one more unit of the
        // form, and a same-form spare in the pack blinked out until the sweep
        // (user report). The ring2 exclusion takes over seamlessly: the
        // none_of guard that was waiting on this entry opens the instant it
        // goes.
        Grid::ReleasePendingEquipFor(g_ringId);
        // ★The carrier bypasses the engine's equip of the RING itself, which
        // is where the vanilla equip sound lives -- so the second slot wore
        // rings in total silence (user report). The pickup clink is the same
        // substitute every board action already uses.
        p->PlayPickUpSound(a_ring, true, false);
        SKSE::log::info("[DUALRING] second ring '{}' on slot {} (0x{:08X}), ench '{}'",
            NameOf(a_ring), slot + 30, mask,
            a_ring->formEnchanting ? NameOf(a_ring->formEnchanting) : "none");
        return true;
    }

    void TakeOff() { TakeOffImpl(/*a_standalone=*/true); }

    void TakeOffImpl(bool a_standalone)
    {
        if (!g_ringId) return;
        auto* ring = RingById(g_ringId);
        auto* p    = RE::PlayerCharacter::GetSingleton();
        auto* c    = Carrier();
        if (p && c) {
            if (auto* em = RE::ActorEquipManager::GetSingleton()) {
                em->UnequipObject(p, c, nullptr, 1, nullptr, false, false, false, true);
            }
            // Count high enough to sweep duplicates a crossed save could leave.
            p->RemoveItem(c, 99, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        }
        Reclaim();
        // ★Both INSIDE the gate (규칙 6): every STANDALONE caller needs them.
        // The sound for the same reason as Wear's; the rebuild because the
        // carrier's stand-down is a board return with NO engine unequip event
        // -- the !rbdrop interrogation of Grid.cpp:11162 measured the ring
        // vanishing until the next unrelated rebuild without it.
        // ★★B4-4: the HANDOFF calls inside Wear are the exception the old
        // "every caller" claim missed. In a swap the displaced ring goes to
        // the CURSOR (WholeOnDoll starts that carry) or straight onto the
        // FIRST slot -- either way it never lands on the board here, so the
        // redraw painted a frame in the middle of the exclusion handoff for
        // nothing. That mid-swap frame is the deferred ring-blink's habitat
        // (survived the worn clocks and the counter absorption -- the
        // rebuild ITSELF was the remaining suspect).
        const bool wantDraw = a_standalone || !Grid::CarrierCarryActive();
        // ★The quiet handoff assumed the displaced ring rides the cursor.
        // True for the DROP swap -- its carry starts before Wear runs --
        // and false for the right-click ROUTER, which displaces with no
        // carry at all: the old ring went back to the pack with nothing
        // to redraw it, and right-clicking through several rings appeared
        // to wear them all (user report). No carrier carry up means the
        // return still needs its draw.
        if (a_standalone && p && ring) p->PlayPickUpSound(ring, false, false);
        const RE::FormID retId = g_ringId;
        SKSE::log::info("[DUALRING] second ring '{}' removed{}", NameOf(ring),
                        a_standalone ? "" : " (handoff)");
        // ★Ring session: state DOWN before the draw -- the partial add asks
        // the ring2 exclusion, and with g_ringId still set it would hide the
        // very unit it is trying to draw ("nothing fresh").
        g_ringId = 0;
        g_ringSig = 0;
        if (wantDraw) {
            // One form's return, not a repaint: the full rebuild here ran in
            // the middle of the swap window (the blink's habitat). The
            // partial declines -> the old rebuild, same fallback bargain as
            // every B3 path.
            if (!Grid::OnFormDelta(retId)) Grid::RequestRebuild();
        }
    }

    void RequestTakeOff() { g_wantOff = true; }

    void CancelTakeOff() { g_wantOff = false; }

    bool TakeOffPending() { return g_wantOff; }

    void Tick()
    {
        if (g_wantOff) {
            g_wantOff = false;
            TakeOff();   // rebuild + sound live inside the gate now
        }
        if (!g_ringId) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->Is3DLoaded()) return;

        // ★The ring can leave without going through us -- sold, dropped, taken
        // by a script. Observing beats remembering: the moment it is not in the
        // pack the carrier is standing in for nothing.
        auto* ring = RingById(g_ringId);
        if (!ring) { TakeOff(); return; }
        bool have = false;
        for (const auto& [obj, data] : p->GetInventory(
                 [&](RE::TESBoundObject& o) { return &o == ring; })) {
            have = data.first > 0;
            (void)obj;
        }
        if (!have) {
            SKSE::log::info("[DUALRING] the second ring left the inventory -- dropping it");
            TakeOff();
        }
    }

    void RevertGame(SKSE::SerializationInterface*)
    {
        Reclaim();
        g_ringId  = 0;
        g_wantOff = false;
    }

    void OnLoad()
    {
        if (!g_ringId) return;
        // ★The equip survived in the save; the LOAN did not. The engine re-read
        // the carrier from the plugin, so its enchantment is the record's own
        // again -- and a carrier worn with no enchantment is a second ring that
        // quietly stopped working.
        auto* p    = RE::PlayerCharacter::GetSingleton();
        auto* c    = Carrier();
        auto* ring = RingById(g_ringId);
        if (!p || !c || !ring) { g_ringId = 0; return; }
        g_lent = {};   // whatever we recorded belongs to the previous session
        const auto mask = static_cast<std::uint32_t>(c->GetSlotMask());
        Lend(c, ring, mask ? mask : (1u << 30));
        SKSE::log::info("[DUALRING] load: re-lent '{}' to the carrier", NameOf(ring));
    }

    void SaveGame(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc->OpenRecord(kRecordType, 1)) return;
        a_intfc->WriteRecordData(&g_ringId, sizeof(g_ringId));
    }

    void LoadRecord(SKSE::SerializationInterface* a_intfc, std::uint32_t)
    {
        RE::FormID id = 0;
        a_intfc->ReadRecordData(&id, sizeof(id));
        // ★Through the resolver: a load order change moves every FormID, and a
        // raw one would name a different item entirely.
        RE::FormID resolved = 0;
        g_ringId = (id && a_intfc->ResolveFormID(id, resolved)) ? resolved : 0;
    }
}
