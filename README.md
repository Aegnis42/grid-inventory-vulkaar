# Grid Inventory — fork vulkaar

> **This is a modified fork** of
> [skypia0147-dev/grid-inventory](https://github.com/skypia0147-dev/grid-inventory)
> (Grid Inventory by lSmoothl,
> [Nexus 188733](https://www.nexusmods.com/skyrimspecialedition/mods/188733)),
> maintained for the **vulkaar** Skyrim RP multiplayer server (skymp).
> Modifications began on 2026-08-24; each change is dated in the git history
> and marked `[vulkaar]` in the source. Same license as upstream: GPL-3.0
> with the modding/linking exceptions (`LICENSE-GPL`, `EXCEPTIONS.txt`).
>
> **Ce dépôt est le fork vulkaar** du mod Grid Inventory, modifié pour le
> serveur RP multijoueur vulkaar (skymp). C'est la source correspondante de
> la `GridInventory.dll` que le launcher vulkaar installe. Ce qui change par
> rapport à l'amont :
>
> - **la bourse** : trois monnaies (septime, mède, titus) comptées hors
>   grille, trois vraies cases sans limite au bas du panneau d'équipement ;
> - **deux panneaux pleine hauteur** : la grille au bord gauche, l'équipement
>   (mannequin, zones de sacoches et de sac mesurées sur le contenant porté,
>   statistiques, bourse) au bord droit ;
> - **240 cases (10×24) pour tous**, et la disposition de chaque personnage
>   survit à la reconnexion (fichier par personnage — le cosave ne passe
>   jamais sous skymp) ;
> - **le menu carrefour (Tab) rendu au joueur** malgré le mode chargen
>   permanent de skymp.

An SKSE plugin that replaces the Skyrim SE/AE inventory with a Tetris-style
grid. Every item occupies real squares; containers, merchants and
pickpocketing share the same grid UI; icons are captured from each item's own
3D model at runtime, so any modded item works with no icon pack and no patches.

This repository holds the **plugin source**. The mod itself (esp, meshes,
sounds, icon pak) is distributed on Nexus Mods.

- Feature overview: [README_EN.md](README_EN.md) · [README_KO.md](README_KO.md)
- For mod authors — costume state signal: [API_COSTUME.md](API_COSTUME.md) · [API_COSTUME_KO.md](API_COSTUME_KO.md)

## Building

Requirements: Visual Studio 2022 (MSVC x64), CMake 3.24+, [vcpkg](https://github.com/microsoft/vcpkg).

```
cmake -B plugin/build -S plugin ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build plugin/build --config Release
```

CommonLibSSE-NG is pulled from source via FetchContent on the first configure,
pinned to a commit of [alandtse's NG line](https://github.com/alandtse/CommonLibVR);
spdlog / fmt / rapidcsv / imgui come from the vcpkg manifest. The build is a
single `GridInventory.dll` covering every SE/AE runtime (1.5.x–1.7.x) through
the Address Library.

> The upstream CharmedBaryon tree classifies runtimes by minor version and
> reads 1.7.99 as SE, which sends it looking for an address library that does
> not exist under that name. The NG line above classifies `>= 6` as AE and
> reads the library's format from the file, which is what Address Library v12
> needs. See the note above `FetchContent_Declare` in `plugin/CMakeLists.txt`.

`MOD_ROOT` (optional cache variable) names a local MO2 mod folder the built
DLL is copied into after each build; the step is skipped when the folder does
not exist, so a plain clone builds with no local setup.

## For mod authors — costume signal

Grid Inventory tells other SKSE plugins when the player puts on, switches or
takes off a **costume** (an appearance-only outfit), and which pieces it is
made of. Equipment cannot answer that: a costume changes how the player looks
without changing what they wear. Added in 1.4.1, no ABI change.

What is sent:

```
tab 2 · 7 pieces
    0001B39F  Steel Armor
    0001B3A2  Steel Helmet
    ...
```

`tab` is the loadout tab supplying the look (`-1` = no costume) and the pieces
are FormIDs of the armour that reaches the body — weapons, shields and quivers
are never part of a costume.

```cpp
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

// ★nullptr is required. RegisterListener(cb) filters to "SKSE" and never
//  sees this message.
SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
```

`pieces` is only valid inside the callback — copy what you need. Full
contract, including every case that fires it:
[API_COSTUME.md](API_COSTUME.md) · [API_COSTUME_KO.md](API_COSTUME_KO.md).
The header to copy is [`plugin/src/api/GridInventoryAPI.h`](plugin/src/api/GridInventoryAPI.h).

## License

The plugin source is distributed under **GPL-3.0** — see [LICENSE-GPL](LICENSE-GPL) —
with the modding/linking exceptions in [EXCEPTIONS.txt](EXCEPTIONS.txt)
(inherited unchanged from upstream Modex).

Portions are ported from [Modex](https://github.com/patchulidev/ModExplorerMenu)
(patchulidev, GPL-3.0): the IMenu bootstrap, the ImGui render-loop structure,
and the Inventory3DManager backbuffer-capture pipeline. Each ported file
carries an attribution header. Full credits: [CREDITS.md](CREDITS.md).
