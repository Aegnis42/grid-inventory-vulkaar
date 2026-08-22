# Grid Inventory

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

CommonLibSSE-NG is pulled from source via FetchContent on the first configure;
spdlog / fmt / rapidcsv / imgui come from the vcpkg manifest. The build is a
single `GridInventory.dll` covering every SE/AE runtime (1.5.x–1.6.x) through
the Address Library.

`MOD_ROOT` (optional cache variable) names a local MO2 mod folder the built
DLL is copied into after each build; the step is skipped when the folder does
not exist, so a plain clone builds with no local setup.

## License

The plugin source is distributed under **GPL-3.0** — see [LICENSE-GPL](LICENSE-GPL) —
with the modding/linking exceptions in [EXCEPTIONS.txt](EXCEPTIONS.txt)
(inherited unchanged from upstream Modex).

Portions are ported from [Modex](https://github.com/patchulidev/ModExplorerMenu)
(patchulidev, GPL-3.0): the IMenu bootstrap, the ImGui render-loop structure,
and the Inventory3DManager backbuffer-capture pipeline. Each ported file
carries an attribution header. Full credits: [CREDITS.md](CREDITS.md).
