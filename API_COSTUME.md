# Costume state signal

Grid Inventory announces the player's **costume** — an appearance-only outfit —
to any SKSE plugin that cares to listen. This document is the contract.

A costume makes the body show the set held by one loadout tab while the stats
keep coming from whatever is really equipped. **Equipment cannot answer "what
does the player look like"**, because a costume changes the look without
changing what is worn. That is what this signal is for.

Added in **1.4.1**. Requires no ABI change and no registration: a plugin that
does not listen for it simply never sees it.

---

## 1. The message

```cpp
namespace GridInvAPI
{
    inline constexpr std::uint32_t kABIVersion     = 1;
    inline constexpr std::uint32_t kMsgCostumeState = 0x47494353;  // 'GICS'

    struct ItemKey                 // 16 bytes, unchanged since ABI v1
    {
        std::uint32_t owner;       // container REFR FormID; the player is 0x14
        std::uint32_t base;        // TESBoundObject FormID
        std::uint16_t uid;         // ExtraUniqueID, 0 = plain stack unit
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };

    struct CostumeState            // 24 bytes
    {
        std::uint32_t  structSize;   // = sizeof(CostumeState)
        std::uint32_t  abiVersion;   // = kABIVersion
        std::int32_t   tab;          // loadout tab supplying the look; -1 = none
        std::uint32_t  pieceCount;   // 0 when tab is -1
        const ItemKey* pieces;       // BORROWED — see §4
    };
}
```

The full header is [`plugin/src/api/GridInventoryAPI.h`](plugin/src/api/GridInventoryAPI.h).
Copy it into your project; it has no dependencies beyond `<cstdint>`.

---

## 2. Listening

```cpp
static void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg || a_msg->type != GridInvAPI::kMsgCostumeState) return;
    if (a_msg->dataLen < sizeof(GridInvAPI::CostumeState))      return;

    const auto* st = static_cast<const GridInvAPI::CostumeState*>(a_msg->data);
    if (st->abiVersion != GridInvAPI::kABIVersion) return;

    if (st->tab < 0) {
        // No costume: the player looks like whatever is equipped.
        return;
    }
    for (std::uint32_t i = 0; i < st->pieceCount; ++i) {
        const RE::FormID armor = st->pieces[i].base;
        // ...copy what you need before returning (see §4)
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    return true;
}
```

### The `nullptr` matters

`RegisterListener(cb)` is shorthand for `RegisterListener("SKSE", cb)`, which
filters out everything a *plugin* sends — including this. **Register with an
explicit `nullptr` sender** to receive it.

If you also handle SKSE lifecycle messages, register **two** listeners:

```cpp
RegisterListener(OnLifecycle);            // == RegisterListener("SKSE", ..)
RegisterListener(nullptr, OnApiMessage);  // no filter
```

One unfiltered listener cannot serve both, because **message type numbers live
in the sender's namespace**. Another plugin's "type 1" is indistinguishable
from `kPostLoad`, so lifecycle handling would fire at random.

---

## 3. When it arrives

The signal is sent on **every transition**, not only the ones a player causes:

| Cause | What you get |
|---|---|
| Player checks a loadout tab as costume | `tab = N`, the pieces |
| Player switches to a different tab | `tab = M`, the new pieces |
| Player unchecks / clears the costume | `tab = -1`, `pieceCount = 0` |
| The tab a costume pointed at is deleted | `tab = -1`, `pieceCount = 0` |
| A save that had a costume is loaded | `tab = N`, the pieces |
| Reverting to a new game | `tab = -1`, `pieceCount = 0` |
| First tick of a session | the current state, whatever it is |

**A load always re-announces, even when the tab is unchanged.** Your own state
did not survive the load either, so "nothing changed" would be the wrong thing
to tell you.

Each message means the state **actually moved**. You will not receive the same
state twice in a row within one session, so you may treat every message as a
change without comparing.

---

## 4. Rules

### `pieces` is borrowed

It points into Grid Inventory's own buffer and is valid **for the duration of
your callback and no longer**. Copy what you need before returning. The buffer
is reused on the next broadcast.

### It arrives on the game thread

Sent from Grid Inventory's per-frame tick. Do not block, and do not call
anything that can open or close a menu.

### Armour only

A loadout tab holds a full kit — weapons, shield and quiver included. **A
costume never touches those**, because they are held rather than worn. Only
pieces that actually reach the body are listed, so every entry is something
the player is now *seen* in.

This means `pieceCount` is normally smaller than the tab's item count.

### A piece is a form, not a stack unit

`base` is the armour's `FormID`; `owner` is the player (`0x14`); `uid` is
always `0`. A costume names **what to look like**, not one particular copy in
the inventory. Do not expect `uid` to identify an instance here.

### `tab` is an index, not an identity

It is the loadout tab's current position. Tabs renumber when one below them is
deleted — and when that happens you receive a fresh message, so the value is
never stale. Do not persist it across sessions.

---

## 5. Versioning

Check `abiVersion` and **ignore the message if it does not match**. Grid
Inventory refuses any provider whose version disagrees, and you should be as
strict in the other direction.

`structSize` is there so the struct can grow. If a later version appends
fields, `dataLen` and `structSize` grow with it while everything above stays
where it is — comparing `dataLen >= sizeof(CostumeState)` against **your**
copy of the header keeps working.

---

## 6. What this is not

- **Not an equipment signal.** What the player wears has not changed; only how
  they look. Read equipment for stats.
- **Not a request channel.** Nothing you return is read. This message runs
  outward only; there is no way to veto or alter a costume through it.
- **Not per-frame.** It fires on transitions. If you need the current state at
  an arbitrary moment, cache the last message.

---

## 7. Reporting problems

Open an issue with the `GridInventory.log` line that should have appeared:

```
[API] costume state broadcast: tab 2 (7 piece(s))
```

If that line is present and your listener did not fire, the cause is almost
always the filtered-listener trap in §2.
