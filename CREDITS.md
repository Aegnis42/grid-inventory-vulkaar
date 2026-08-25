# Grid Inventory — Credits / 크레딧

## Fork vulkaar

This fork is maintained by the **vulkaar** project (Skyrim RP multiplayer
server on skymp) and carries its modifications on top of the upstream
source — marked `[vulkaar]` in the code and described in the fork section
of `README.md`. All original work remains credited below; the fork's
changes are distributed under the same **GPL-3.0** with the same
modding/linking exceptions.

Ce fork est maintenu par le projet **vulkaar** (serveur RP multijoueur
Skyrim sur skymp). Les modifications sont marquées `[vulkaar]` dans la
source et décrites dans `README.md` ; elles sont distribuées sous la même
licence que l'amont (**GPL-3.0** + exceptions modding/linking).

## License / 라이선스

This SKSE plugin's source contains code ported from Modex (GPL-3.0).
The plugin source is therefore distributed under **GPL-3.0** (see
`LICENSE-GPL`) with the same modding/linking exceptions as upstream
(`EXCEPTIONS.txt`).

이 SKSE 플러그인 소스에는 Modex(GPL-3.0)에서 이식한 코드가 포함되어
있으며, 따라서 플러그인 소스는 **GPL-3.0**(`LICENSE-GPL`)으로 배포됩니다 —
업스트림과 동일한 모딩/링킹 예외(`EXCEPTIONS.txt`) 포함.

## Ported code / 이식 코드

- **Modex** by *patchulidev* — GPL-3.0 with Modding Exception
  https://github.com/patchulidev/ModExplorerMenu
  - ImGui bootstrap / render-loop structure → `plugin/src/ui/UIRoot.cpp`
  - Item3DPreview backbuffer-capture pipeline (Inventory3DManager render →
    save/clear/capture/restore) → `plugin/src/ui/ItemPreview.cpp`
  - Individual files carry their own attribution headers.
    각 파일 상단에 출처 주석이 명기되어 있습니다.

## Libraries / 라이브러리

- **CommonLibSSE-NG** (CharmedBaryon) — MIT
- **Dear ImGui** (ocornut) — MIT
- **spdlog / fmt** — MIT
- **SKSE** team
- **Address Library for SKSE Plugins** (meh321) — the version-independent
  address resolution every `REL::ID` in this plugin rides on (runtime
  dependency; no files redistributed)
  / 버전 독립 주소 해석의 기반(런타임 의존, 파일 미동봉)

## Fonts / 폰트

The plugin loads system fonts at runtime (Malgun Gothic, Microsoft YaHei,
Meiryo, Georgia, Batang); no font files are redistributed.
플러그인은 시스템 폰트를 런타임에 로드하며, 폰트 파일을 재배포하지 않습니다.
