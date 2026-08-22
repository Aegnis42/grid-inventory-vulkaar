#pragma once

// =============================================================================
//  GI10 -- extension ABI, host side
// =============================================================================
//  Grid Inventory publishes a tiny POD service table and accepts "providers"
//  (extension plugins that own gameplay data the grid knows nothing about).
//  The contract itself lives in api/GridInventoryAPI.h; this header is only the
//  host's internal entry points.
//
//  The host NEVER links the provider and never calls into ImGui on its behalf:
//  a provider returns data, the host draws it with its own atlas and theme.
// =============================================================================

#include "api/GridInventoryAPI.h"

namespace FUI::HostApi
{
    // Registers the ABI listener. Call from SKSEPluginLoad.
    //
    // ★TWO listeners, split by sender, and that split is the whole design:
    //
    //   RegisterListener("SKSE", MessageHandler)   <- lifecycle, untouched 1.0 path
    //   RegisterListener(nullptr, <ours>)          <- any sender, ABI messages only
    //
    // A single unfiltered listener cannot serve both, because message type
    // numbers live in the SENDER's namespace: another plugin's "type 4" is
    // indistinguishable from kPostLoadGame, and acting on it would reset our
    // session state at random. Keeping lifecycle on the "SKSE"-filtered
    // listener makes that collision impossible by construction.
    void Install();

    // Called from the lifecycle handler on kPostLoad: announce the host.
    void Broadcast();

    // ★★THE ONE SIGNAL THAT RUNS OUTWARD (kMsgCostumeState).
    //
    // Announce the costume the player is wearing the look of. a_tab is the
    // loadout tab supplying it, -1 when no costume is in force; a_forms lists
    // the pieces (empty when a_tab is -1). Requested by a mod author who needs
    // to follow the player's APPEARANCE, which equipment does not report -- a
    // costume changes how you look without changing what you wear.
    //
    // Called from the game thread only (Costume::Tick), and only when the state
    // actually moved -- listeners are entitled to treat each one as a change.
    void BroadcastCostume(int a_tab, const RE::FormID* a_forms, std::uint32_t a_count);

    // Number of providers that passed the version handshake (0 or 1 today).
    [[nodiscard]] std::uint32_t ProviderCount();

    // The overlay for one instance, or nullptr when no provider claims it.
    // HOT PATH: called once per visible tile per frame by GI8 once that lands.
    // Returns a pointer into a per-call static buffer -- consume it immediately.
    [[nodiscard]] const GridInvAPI::Overlay* Overlay(const GridInvAPI::ItemKey& a_key);

    // Provider tooltip lines. Returns how many were written into a_out.
    [[nodiscard]] std::uint32_t TooltipLines(const GridInvAPI::ItemKey& a_key,
                                             GridInvAPI::TooltipLine* a_out,
                                             std::uint32_t a_capacity);

    // Drop routing: consulted before the host's own tables, hover and commit.
    [[nodiscard]] GridInvAPI::DropVerdict OfferDrop(const GridInvAPI::DropQuery& a_query);
}
