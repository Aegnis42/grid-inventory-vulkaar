# Costume signal

Sends the player's costume state and its pieces to other SKSE plugins when a
costume is put on, switched or taken off. Added in 1.4.1.

## What is sent

```
tab 2 · 7 pieces
    0001B39F  Steel Armor
    0001B3A2  Steel Helmet
    0001B3A4  Steel Gauntlets
    ...
```

| | |
|---|---|
| `tab` | Loadout tab supplying the look. `-1` = no costume |
| `pieces` | FormIDs of the armour that reaches the body. 0 when there is no costume |

Weapons, shields and quivers are never part of a costume, so they are not in
the list.

The same is written to `GridInventory.log`:

```
[API] costume state broadcast: tab 2 (7 piece(s))
[API]   0001B39F 'Steel Armor'
```

## When it arrives

| Situation | `tab` |
|---|---|
| Costume put on / switched to another tab | tab number |
| Costume taken off / its tab deleted / new game | `-1` |
| Save with a costume loaded | tab number (re-sent even if unchanged) |
| First tick of a session | current state |

The same state never arrives twice in a row. Apply each message as it comes.

## Receiving

```cpp
#include "GridInventoryAPI.h"   // plugin/src/api/GridInventoryAPI.h

static void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg || a_msg->type != GridInvAPI::kMsgCostumeState) return;
    if (a_msg->dataLen < sizeof(GridInvAPI::CostumeState))      return;

    const auto* st = static_cast<const GridInvAPI::CostumeState*>(a_msg->data);
    if (st->abiVersion != GridInvAPI::kABIVersion) return;

    for (std::uint32_t i = 0; i < st->pieceCount; ++i) {
        const RE::FormID id = st->pieces[i].base;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    // ★nullptr is required. RegisterListener(cb) filters to "SKSE" and never
    //  sees this message.
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    return true;
}
```

Register two listeners if you also handle SKSE's own messages. One cannot
serve both — message numbers are per sender.

## Notes

- `pieces` is invalid once the callback returns. Copy what you need inside it.
- Arrives on the game thread. Do not call anything that opens or closes a menu.
- `tab` is a position, not an identity. It shifts when a tab below it is
  deleted. Do not persist it.
- `uid` is always 0. A costume names a form, not a copy in the inventory.
- Equipment is unchanged. Read equipment for stats.

## Structs

```cpp
namespace GridInvAPI
{
    inline constexpr std::uint32_t kABIVersion      = 1;
    inline constexpr std::uint32_t kMsgCostumeState = 0x47494353;  // 'GICS'

    struct ItemKey                 // 16 bytes
    {
        std::uint32_t owner;       // the player = 0x14
        std::uint32_t base;        // armour FormID
        std::uint16_t uid;         // always 0
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };

    struct CostumeState            // 24 bytes
    {
        std::uint32_t  structSize;
        std::uint32_t  abiVersion;   // ignore the message if it differs
        std::int32_t   tab;
        std::uint32_t  pieceCount;
        const ItemKey* pieces;
    };
}
```

## Troubleshooting

If the log has a `costume state broadcast` line and your callback never fires,
it is the `nullptr`. If the line is missing, send the log with the report.
