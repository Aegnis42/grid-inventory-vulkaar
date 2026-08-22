# Costume signal (for mod authors)

Grid Inventory tells other SKSE plugins when the player **puts on, switches or
takes off a costume** — and which pieces it is made of.

Added in **1.4.1**.

---

## What goes out

Put a costume on, and this is what is sent:

```
tab 2 · 7 pieces
    0001B39F  Steel Armor
    0001B3A2  Steel Helmet
    0001B3A4  Steel Gauntlets
    0001B3A6  Steel Boots
    000261C0  Amulet of Talos
    ...
```

Take it off:

```
no tab · 0 pieces
```

So there are two things in every message:

| | |
|---|---|
| **The tab** | The loadout tab supplying the look. `-1` means no costume |
| **The pieces** | FormIDs of the armour that actually reaches the body |

A FormID is all you need — `TESForm::LookupByID` gives you the name, model and
slots from there.

The same thing is written to `GridInventory.log`, so you can confirm what left
our side:

```
[API] costume state broadcast: tab 2 (7 piece(s))
[API]   0001B39F 'Steel Armor'
[API]   0001B3A2 'Steel Helmet'
```

---

## Weapons are not included

A loadout tab holds weapons, a shield and a quiver too, but **a costume never
touches those** — they are held, not worn.

So the list contains only what is genuinely visible on the body. A tab with 15
items may well send 14 pieces.

---

## When it arrives

Not only when the player presses something — on **every** change of state:

| Situation | What you get |
|---|---|
| Costume put on | tab number + pieces |
| Switched to another tab | new tab + new pieces |
| Costume taken off | tab `-1`, 0 pieces |
| The tab a costume used was deleted | tab `-1`, 0 pieces |
| A save with a costume is loaded | tab number + pieces |
| Reverting to a new game | tab `-1`, 0 pieces |
| First moment of a session | whatever the state is |

**Loading a save re-sends even when the tab is unchanged.** Whatever you were
holding did not survive the load either, so telling you "nothing changed"
would leave your side wrong.

Every message means the state **actually moved**. The same state never arrives
twice in a row, so you can simply apply each one.

---

## Receiving it

```cpp
#include "GridInventoryAPI.h"   // plugin/src/api/GridInventoryAPI.h

static void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg || a_msg->type != GridInvAPI::kMsgCostumeState) return;
    if (a_msg->dataLen < sizeof(GridInvAPI::CostumeState))      return;

    const auto* st = static_cast<const GridInvAPI::CostumeState*>(a_msg->data);
    if (st->abiVersion != GridInvAPI::kABIVersion) return;

    if (st->tab < 0) {
        // no costume
        return;
    }
    for (std::uint32_t i = 0; i < st->pieceCount; ++i) {
        const RE::FormID id = st->pieces[i].base;
        // copy anything you need right here (see ★ below)
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    return true;
}
```

### ★ Do not leave out the `nullptr`

This one detail makes the difference between working and receiving nothing at
all. It is the most common mistake.

```cpp
RegisterListener(cb);            // ✗ receives nothing
RegisterListener(nullptr, cb);   // ✓ works
```

The one-argument form is shorthand for `RegisterListener("SKSE", cb)`, and
that **filters out everything sent by a plugin**. Our log still says the
signal went out while nothing happens on your side, which makes this
particularly hard to diagnose.

If you also handle SKSE's own messages (game loaded, and so on), register
**two** listeners. One cannot serve both: message numbers belong to whoever
sent them, so another plugin's "type 1" is indistinguishable from SKSE's.

---

## Three rules

### 1. The piece list is borrowed

`pieces` points into Grid Inventory's own array. It is **invalid once your
callback returns**, and the next signal overwrites it. Copy what you need
while you are still inside the callback.

### 2. It arrives on the game thread

Sent from a per-frame tick. Do not hold it up, and do not call anything that
opens or closes a menu.

### 3. The tab number is a position, not a name

It is where the loadout tab currently sits. Delete a tab below it and the
number shifts. You get a fresh signal when that happens, so the value is never
stale — but do not save it and trust it later.

---

## What this is not

- **Not an equipment signal.** What the player wears has not changed. Read
  equipment for stats; a costume only changes the look.
- **Not something you can answer.** The signal only goes outward. There is no
  way to block or alter a costume through it.
- **Not per-frame.** It fires on change. If you need the current state at an
  arbitrary moment, keep the last one you received.

---

## The structs

```cpp
namespace GridInvAPI
{
    inline constexpr std::uint32_t kABIVersion      = 1;
    inline constexpr std::uint32_t kMsgCostumeState = 0x47494353;  // 'GICS'

    struct ItemKey                 // 16 bytes
    {
        std::uint32_t owner;       // the player = 0x14
        std::uint32_t base;        // ★the armour's FormID -- this is "which item"
        std::uint16_t uid;         // always 0 for a costume
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };

    struct CostumeState            // 24 bytes
    {
        std::uint32_t  structSize;   // = sizeof(CostumeState)
        std::uint32_t  abiVersion;   // = kABIVersion; ignore the message if it differs
        std::int32_t   tab;          // tab number, -1 = no costume
        std::uint32_t  pieceCount;   // 0 when there is no costume
        const ItemKey* pieces;       // the array (borrowed)
    };
}
```

`uid` is always `0` here. A costume names **what to look like**, not one
particular copy in the inventory.

`structSize` exists so fields can be appended later without breaking existing
code — nothing above ever moves, so comparing sizes against your own copy of
the header keeps working.

---

## When it does not work

Check `GridInventory.log` for this line first:

```
[API] costume state broadcast: tab 2 (7 piece(s))
```

**Line present, callback never fires** → almost certainly the `nullptr` above.

**No line at all** → either the costume state did not actually change, or the
problem is on our side. Send the log along with the report.
