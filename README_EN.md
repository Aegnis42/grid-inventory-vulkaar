# Grid Inventory — a Tetris-style grid inventory

An SKSE plugin that replaces the vanilla inventory outright. Every item occupies
real squares on a grid, and **three screens** share the same grid UI:
**inventory · containers (chests, corpses, followers, pickpocketing) · merchants.**
Everything else (crafting stations, magic, favourites Q, gifts, journal, map, TAB)
stays vanilla/SkyUI.

Icons are captured from each item's own 3D model by the game itself — no icon
pack, and any modded item is supported automatically with no patches.

---

## Requirements

| | |
|------|------|
| Game | Skyrim Special Edition / Anniversary Edition (all of 1.5.x – 1.6.x) |
| Required | [SKSE64](https://skse.silverlock.org/) |
| Required | [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444) |

Built on CommonLibSSE-NG; one DLL covers every SE/AE runtime.
**No SkyUI, no MCM, no gamepad support** (mouse + keyboard only).

## Installation

Install the archive with your mod manager (MO2, Vortex). Contents:

```
Grid Inventory.esp          (coin/pouch + 2 bag forms + 2 encumbrance abilities + UI sounds)
SKSE/Plugins/GridInventory.dll
SKSE/Plugins/GridInventory_icons.pak      (pre-captured icons for all vanilla + AE CC items)
SKSE/Plugins/GridInventory_items.ini      (tuned footprint/rotation for every item)
SKSE/Plugins/GridInventory_categories.ini (category defaults — for unlisted items)
SKSE/Plugins/GridInventory_slots/       (equipment slot silhouettes)
meshes/, textures/, Sound/  (coin models, sounds)
```

The remaining settings files (`_ui.ini` etc.) are generated in `SKSE/Plugins/` on
first run — there is nothing to edit beforehand. Safe to add to an ongoing save.

## First run

1. **Vanilla and AE Creation Club items come with icons pre-captured** — they
   show instantly, no caching wait. Only modded items are captured as they first
   become visible — a **"caching icons… N"** counter shows next to the **ITEMS
   label** above the grid.
2. The default language is English; other languages switch instantly in Settings.
3. Recommended with many modded items: Settings → Icons → **Precache All** —
   captures every item in your load order, one per frame, while the inventory
   stays open (closing saves and stops).

---

## Features

### The grid
- 10×14 = **140 squares, fixed**. Real footprints per item (1×1 rings to 2×4
  greatbows), free shapes (L-pieces) supported. No auto-sort, search, or 90° rotation.
- **Pick-up-and-place**: left-click to lift, left-click the target square to set
  down (swap supported). Shift+left-click = stack/gold split slider.
- Enchanted (blue) / unique (gold) glow, **tempered = white border · poisoned =
  green border**, markers (favourite ◆ / quest ▲ / stolen ●), Shift comparison
  tooltip with an *Equipped* card.
- **Capacity**: exceeding 140 squares shows an overflow row and slows movement.
  A full grid blocks pickups (quest/script-granted items are deliberately never
  blocked — they come in and push you into overflow instead).
- **`C` inspect in 3D**: rotate and zoom the actual model (Dragon Claw glyphs etc.).
  Works on **your own items only**.
- **Equipment doll**: 17 slots, place to equip / right-click to unequip.
- **Gear-set tabs**: one click swaps the whole set (really equips; stats follow).
  Gear held by inactive tabs is hidden and takes no squares.
- **Bags**: general goods merchants sell the **Satchel** (1 square, 6×4
  inside) and **Knapsack** (2×2, 8×6 inside). Right-click opens the inner
  grid; EDIT mode can designate any other item as a bag too (up to 10×10).
- **Trash bin**: 6×4 staging area; right-click restores; **deletion is final when
  the window closes.**

### Physical gold
- Gold appears as coin tiles, one per 1,000 G plus remainder.
- **Coin Pouch** (2×2, sold by general goods vendors): banks up to 10,000 G,
  auto-stores incoming gold, right-click slider to withdraw. Pouched gold still
  counts toward your total and pays at shops.
- Drop a coin tile outside (or `R`) and a purse object lands on the ground.

### Containers / shops / pickpocketing
- Chests, corpses and followers open as the same grid, and **layouts are
  remembered** (128 containers; emptied squares stay reserved).
- Shops: haggled prices, **per-piece values with tempering included**, merchant
  gold shown, quantity slider with a **MAX** button, proper Speech XP.
- Pickpocketing: a success % on every square (for the whole stack), worn gear
  locked (Perfect Touch unlocks), reverse-pickpocketing + Poisoned-perk planting.
- Containers auto-open after a successful lockpick (yields to QuickLoot-style widgets).

### Settings (SETTINGS in the title bar)
- UI scale, 6 skins, 4 languages (live switch), glow style/brightness, icon
  brightness/style (realistic vs stylized — auto-derived from the captures,
  covers every item), icon cache reset, precache all,
  trade options (unlimited merchant gold / merchant buys anything).
- **Presets**: save the whole look — skin, every item definition, and the icon
  pictures — under a name; load any from a dropdown. Share the two files
  (`GridInventory_<name>.ini` + `GridInventory_<name>_icons.pak`) and another
  player gets your exact setup **with no caching wait.**

### Editing (EDIT in the title bar)
- Click an item → rotation / scale / footprint (6×6 painter) / bag / stack size,
  plus "save as category default". Share the results via presets.

---

## Controls

| Input | Action |
|------|------|
| Inventory key (default `I`) | Open / close — follows the game's own key binding |
| Left-click | Pick up → left-click the destination (on another item = swap) |
| Right-click | Equip/use · open bag · pouch withdraw · (loot/shop/pickpocket) store·sell·plant · (while carrying) cancel |
| Shift+Left-click | Stack / gold split slider |
| In quantity popups | `A`/`D`·`←`/`→` adjust by 1 · **MAX** button · `Enter`/`Space` confirm · `ESC` cancel |
| Shift (hold) | Compare against equipped |
| `C` | Inspect in 3D (own items only) — drag rotate · wheel zoom · `R` reset |
| `R` | Over your grid = drop one / over a container = take all |
| `F` | Favourite (feeds the vanilla Q menu) |
| Drop outside | Discard (cancelled while a chest/shop window is open) |
| `ESC` | One layer at a time: 3D inspect → popups → trash → pouch → settings → EDIT → inventory. A carried item is dropped first |

## Good to know

- **Opening the inventory pauses the game** (conflicts with Skyrim Souls RE).
- **The weight limit is retired by raising carry weight enormously** — weight
  management mods lose their point. The effect removes itself on uninstall.
- ⚠️ **A filled Coin Pouch left in a respawning container is lost with its gold.**
  Use player-home chests. Pouch contents and dropped purses live only in the
  co-save — **retrieve them before uninstalling.**
- Selling a pouch auto-withdraws the stored gold first; only the empty pouch sells.
- After swapping retextures, run Settings → Icons → **Icon cache reset**.
- No key rebinding (only the inventory key follows the game's control settings).
- CJK text uses your Windows system fonts (no fonts redistributed).
- Zero Papyrus scripts — no script load, nothing left in your save.
- Generated files (MO2: Overwrite): `GridInventory_ui.ini` (settings & windows),
  `_icons_styl.pak` (when the stylized style is on), and any presets you export.
  The bundled `_items.ini` / `_categories.ini` / `_icons.pak` are updated during
  play (EDIT changes, new captures) — a mod update overwrites them, so **back up
  your own tuning as a preset.**
  Log: `Documents\My Games\Skyrim Special Edition\SKSE\GridInventory.log`

## Credits / licence

See [CREDITS.md](CREDITS.md) — plugin source under **GPL-3.0** (with the Modding
Exception). Thanks to ModExplorerMenu (patchulidev), game-icons.net (Lorc,
Delapouite), Address Library (meh321), CommonLibSSE-NG, Dear ImGui, and the
SKSE team.
