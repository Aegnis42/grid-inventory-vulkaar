# 인수인계 — 1.3.0 작업 (2026-08-18)

브랜치 `feat/1.3.0-per-pouch-gold`, 커밋 `09ad0a7`. `main`은 손대지 않음.

---

## 0. 지금 상태 한 줄

**1.2.1 수정 3건은 끝났고 검증됐다. 1.3.0 파우치 작업은 절반이 되고 절반이 안 된다. 배포하면 안 된다.**

| 위치 | 내용 |
|---|---|
| `release/stage` | **안전한 되돌림 지점.** 타이틀바 수정까지만 든 1.2.1. 파우치 작업 없음 |
| `release/Grid Inventory 1.2.1.7z` | 스테이지와 동기. DLL SHA-256 `b4d7873f…` |
| `H:\Game\Fablerim\mods\Grid Inventory` | **작업 중 빌드.** 진단 로그 포함. 배포용 아님 |

---

## 1. 완료 — 1.2.1 (인게임 검증됨)

### 1-1. 타이틀바 배율 2중 적용

`Theme::SnapPx(x)`는 인자에 `g_scale`을 곱해 반올림한다. 세 곳이 **이미 배율이 곱해진 값**을 넘겨 `S²`가 됐다.

```
WinManager:1190  SnapPx(titleSize * S)      → 24·S²
UIRoot:2178      SnapPx(GetFontSize())      → 17·S²   (GetFontSize는 이미 픽셀)
UIRoot:2210      SnapPx(titleSize * Scale())→ 24·S²
```

`S`가 상수 0.90이던 시절엔 전부 10% 작았을 뿐이고, **1440p에서는 S=1이라 S²=S — 차이가 0이다.** 기준 해상도만 테스트하면 절대 안 보인다. 4K(S=1.5)에서 51px 바에 54px 제목이 들어가고 EDIT/SETTINGS/✕가 겹쳤다.

해결: `Theme::SnapAbs(px)` 신설(배율 없이 반올림만) + 세 곳 정정 + `TitleCloseMul()`로 ✕ 배율 일원화.

| 화면 | 제목 | EDIT | ✕ |
|---|---|---|---|
| 1080p | 14 → **18** | 10 → **13** | 11 → **18** |
| 1440p | 24 → 24 | 17 → 17 | 24 → 24 |
| 4K | **54 → 36** | **38 → 26** | **81 → 36** |

### 1-2. 프리캐시 취소 버튼이 안 눌리던 것

`Sfx::Button`이 `ImGui::PushID(a_label)` — **라벨 전체**로 ID를 만든다. 취소 라벨엔 남은 개수가 들어가 매 프레임 ID가 바뀌었고, 누른 프레임과 뗀 프레임의 위젯이 달라 클릭이 성립할 수 없었다. 취소 로직은 멀쩡했다.

해결: ID를 `##` 뒤에서 뽑도록(ImGui 관례) + `BtnW`도 `##` 뒤 무시 + `##precachecancel` / `##deferredretry` 부여.

### 1-3. 툴팁 착용부위

`Equip::SlotLabel()` — 이름 바로 아래에 부위 표기. 무기는 종류명(Sword/Battleaxe…), 모르는 모드 슬롯은 `Accessory (47)`처럼 슬롯 번호.

---

## 2. 종결 — 아이콘 캡처 밝기 (수정 불가로 확정)

### 증상
같은 아이템을 실내에서 캡처하면 실외 대낮 캡처의 **0.54배**로 어둡게 찍힌다. 배포 pak은 실외에서 만들어졌으므로, 나중에 실내에서 찍힌 아이콘만 톤이 어긋난다.

### 측정 (전부 실측, 추측 아님)
- 새 캡처 = 옛 캡처의 0.46~0.68배, **R/G/B 비율 동일** → 순수 노출
- 알파 **0픽셀 차이** → 모델·각도·프레이밍 완전 동일
- 옛것 포화 4.7% vs 새것 0.2% → 광량 약 2배 차이
- 월드 앰비언트 실측: **실내 `base 0.074` / 실외 `base 0.852`** (11.5배)

### 왜 못 고치는가 — 세 조건이 모두 참
1. 앰비언트는 `BSShaderManager::State::directionalAmbientTransform` **전역 하나**. `ShadowSceneNode`(씬별 객체)엔 조명·안개·포털만 있고 앰비언트가 **없다** → 씬을 나눠도 같은 값을 본다
2. 프레임 순서가 `Tick → 월드 렌더 → PostDisplay(캡처)`. **월드가 먼저** 그려지므로, 캡처에 닿을 만큼 일찍 쓰면 월드도 물든다
3. `inv->Render()` 직전에 쓰면 **안 먹는다** — 프레임 셰이더 상수가 이미 업로드됨 (실측)

시도하고 되돌린 것: 전역 덮어쓰기(아이콘은 1.00배로 완벽히 고쳐졌으나 월드가 밝아짐), "아이콘 큐가 바쁠 때만" 게이트(캐시가 비면 인벤토리 여는 내내 바쁨). 바닐라 인벤토리도 동일하게 월드 조명 영향을 받는 것까지 확인.

**결론: 현상 유지.** 전모를 `ItemPreview.cpp`의 `PlaceLight` 위 주석에 남겼다. 남은 우회로는 램프(씬 전용, 월드 무영향)를 세게 키우는 것뿐이나 방향광이 채움광을 대체 못 해 그림자가 딱딱해지고 인게임 튜닝 왕복이 여러 번 필요하다.

**대응**: README/모드페이지에 "가장 균일한 결과를 원하면 낮에 실외에서 Precache all" 안내.

---

## 3. 진행 중 — 1.3.0 파우치·가방

### 3-1. 사용자 요구 (확정)

1. 코인 파우치 여러 개가 **각자 다른 금액**을 갖고 외형도 따로여야 한다
2. 상자에 보관해도 **금액·아이콘·툴팁이 유지**되어야 한다
3. 보관 시 소지금이 실제로 줄고, 꺼내면 복구 (**이미 되던 동작이며 깨뜨리면 안 됨**)
4. 가방을 상자에 넣으면 **내용물이 따라가야** 한다 (상인 판매 시 쏟아지는 건 정상)
5. 파우치 상한은 **파우치당 10,000**
6. 상자 안 가방은 **묶음만 기억, 열기는 없음**

### 3-2. Phase 0 조사 결과 (완료)

가방·파우치에 개체 데이터를 붙일 수 있는지 인게임 확인:

```
평범한 상태        lists=-1   (ExtraDataList 자체가 없음)
상자 왕복 후       lists=1, uid=0000, own   ← ExtraOwnership 때문이지 정체성 아님
상자 안에서        lists=-1   (있다 사라짐 → 신뢰 불가)
파우치 2개         count=2    (엔진 스택 하나로 합쳐짐)
```

`InventoryChanges::SetUniqueID`(RELOCATION_ID 15907/16149)는 존재하나 **리스트가 있어야** 부를 수 있고, 리스트는 새로 만들 수 없다(기록 `reference_skyrim_per_item_instance_data`).

**→ 방식 B 채택**: `ExtraDataList`를 쓰지 않고 플러그인 자체 개체 모델(슬롯)을 확장. 코드가 이미 같은 결론을 내려둔 상태였다 — `LayoutEntry::rot`, `LayoutEntry::coin`(v8), `g_pinned`가 모두 "값은 슬롯에 산다".

### 3-3. 구현한 것

```
플레이어 보드   Grid::g_layout           map<tileKey, LayoutEntry>
상자 보드       LootBarter::g_contLayouts map<FormID, map<spotKey, ContSpot>>
```

- `g_pouchStored`: `int` → **`map<tileKey,int>`** (`g_pinned`와 같은 모양)
- `PouchStoredOf(key)` / `PouchIconObjectFor(amount)` / `StoreToPouch(key,v)` / `WithdrawFrom(key,v)` 추가. 기존 `PouchStored()`는 합계를 돌려주어 미수정 호출부가 계속 빌드됨
- `kPouchCap 10000`을 **파우치당** 적용
- `ContSpot`에 `int gold` 추가 (코세이브 v3)
- 코세이브 v6: **머리의 uint32는 자리에 두고** 맵을 꼬리에 추가 → 옛 세이브 파싱 안 깨짐
- 옛 세이브·귀환 금액은 예약 키 `##pouch_incoming`에 파킹 → 리빌드 때 `ClaimReturned(PouchTiles())`가 실제 타일에 넘김
- `g_pouchTile`: 인출 창이 **어느 파우치**를 보는지 기억

### 3-4. 검증됨

인벤토리 안에서 파우치별 입금·인출·아이콘·툴팁 **정상 동작** (사용자 확인).

### 3-5. ★ 안 되는 것 — 여기서부터 시작할 것

#### (A) 상자에 보관하면 빈 아이콘, 툴팁에 금액 없음

로그가 모순돼 보이지만 둘 다 사실이다:

```
[LOOT]     pouch shelved with 6943 G ('Grid Inventory.esp|0x000804')   ← 썼다
[LOOTDIAG] spot 'Grid Inventory.esp|0x000804' col=7 row=3 gold=0       ← 같은 프레임에 0
```

`col/row`는 살아있고 `gold`만 0 → **쓴 뒤 같은 프레임 안에서 그 자리가 재작성된다.**

`LootBarter.cpp`에서 `cl->spots[...]`에 대입하는 곳이 최소 셋이다:
- 약 1781행 — 펜딩 스팟 해소 (여기서 `TakeAwayGold()`로 금액을 넣음)
- 약 1866행 — 슬롯 배정의 "새 자리" 분기 (**금액 보존하도록 이미 고쳤으나 증상 그대로**)
- 약 1836행 — 부재 슬롯 유지 분기 (**아직 안 봄. 유력 용의자**)

**다음 수**: 세 곳 전부에 "쓰기 직전/직후 값"을 찍어 **어느 대입이 0으로 덮는지** 특정. 추측으로 한 곳씩 고치는 건 세 번 실패했다.

#### (B) 상자에서 꺼내면 골드 증발

```
[GOLD] pouch returned but nothing was away
```

회수 훅을 **"선반 자리 삭제"**에 걸었는데, `ContSpot` 설계 주석이 말하듯 *absent items KEEP their spot* — 자리는 일부러 남는다. 그 코드는 풀 전체가 사라질 때만 돌고 상자 창을 닫으면 아예 안 돈다.

**올바른 위치**: 꺼내는 행위 자체 — `LootBarter::RequestTake` / `RequestPickTake` / `RequestBuy` (`LootBarter.h:76~90`). 이 경로들은 **어느 슬롯에서 꺼내는지 안다.** 거기서 `spot.gold`를 읽어 `GoldCoins::GiveAwayGold()`로 입금하고 그 자리의 `gold`를 0으로.

`GiveAwayGold()`는 이미 이벤트 비의존으로 고쳐둠 — 원장에 직접 `kPouchReturn`을 넣고 예약 키에 파킹한다. (처음엔 `g_awayGold`에 넣고 `OnPouchReturned`를 기다렸는데, 그 이벤트는 **한 프레임 먼저** 지나가서 영원히 안 온다.)

#### (C) 파우치 2개일 때 어느 것이 나갔는지 모름

`OnPouchLeftPlayer()`는 `TESContainerChangedEvent`로만 불리고 이벤트엔 타일 키가 없다. 지금은 `FullestPouch()`로 **추정**한다 → **A(3000)를 들고 B(500)를 보관하면 A의 돈이 나간다.**

**해결 방향**: UI가 이동 직전에 대상 타일을 넘겨주는 배관. `g_storeHint` / `PendingSpot`이 이미 "엔진 전송은 다음 Tick이라 놓는 순간엔 칸이 없다" 문제를 푼 기구이므로 거기에 태우면 된다.

#### (D) Phase 2 — 가방 내용물 (미착수)

`Item::inBag`은 **UI 꼬리표일 뿐**이고 내용물은 전부 플레이어 인벤토리에 그냥 있다. `LootBarter.cpp`에 `inBag` 참조가 **0건** → 가방만 옮겨지고 내용물은 남는다.

필요한 것: 가방이 상자로 갈 때 `inBag` 아이템도 실제로 옮기고 `ContSpot::bag`에 묶음 기록, 꺼낼 때 복원. 판매는 현행 유지(쏟아짐).

---

## 4. 지워야 할 진단 코드

배포 전 제거:

- `LootBarter.cpp` — `[LOOTDIAG]` 블록 (그리기 루프 안, 파우치 셀당 1회)

남겨도 되는 것 (세션당 1줄, 재발 시 유용):
- `ItemPreview.cpp` — `[LIGHT] lamp … -> …`, `diffuse … fade …`, 램프 못 찾을 때 경고

---

## 5. 이 저장소에서 다칠 수 있는 것들

- **개행이 파일마다 다르다.** `Grid.cpp`·`UIRoot.cpp`·`Theme.*`·`WinManager.*`·`LootBarter.cpp`·`ItemPreview.cpp` = **CRLF + BOM**. `GoldCoins.cpp/.h`·`Sfx.h`·`Equip.h`·`Lang.h` = **LF, BOM 없음**. `main.cpp`은 CRLF인데 **문자열 리터럴 안에 고립된 CR**이 있다(`" \t\r\n"`). 파이썬 `read_text()`/`write_text()`는 이걸 파괴한다 — **반드시 바이트로 패치**하고 파일별 개행을 먼저 확인할 것
- 배시 히어독(`<<'PY'`)이 백슬래시를 먹는다. 앵커에 `\x`가 들어가면 매칭이 실패한다
- `git checkout`으로 되돌리기 전에 **그 파일의 다른 미커밋 변경**을 확인할 것 (오늘 한 번 잃었다)
- 빌드: `cmake --build "F:/Anti/GridInventory/plugin/build" --config Release`. POST_BUILD가 `H:\Game\Fablerim\mods\Grid Inventory`로 자동 배포

---

## 6. 권장 순서

1. **(A) 먼저** — 세 대입 지점에 로그를 넣어 덮어쓰는 놈을 특정. 보관이 제대로 되기 전에 (B)를 고쳐도 확인이 안 된다
2. **(B)** — 회수 훅을 `RequestTake` 계열로 이동
3. **(C)** — 이동 직전 타일 키 배관. 여기까지 되면 파우치 요구 1~3, 5 완료
4. **(D)** — 가방 내용물. (C)의 배관을 그대로 재사용
5. 진단 코드 제거 → `release/stage`에 DLL 반영 → **1.3.0** 패키징

---

## 7. 사용자 확인이 필요한 남은 항목

- 4K 테스터 보고 **미해결**: 일부 아이템(양손도끼·지팡이 = `h:4` 대형)이 격자 이동 안 됨. "집는 건 되고 내려놓는 게 안 된다". 빨간 고스트가 뜨는지(자리 없음 = 정상 거부) 아무 표시도 없는지(진짜 버그) 확인 필요 — 아직 답 못 받음
- 빈 손 슬롯의 검 아이콘은 `slot_weapon.fic`(빈 슬롯 실루엣)으로, 사양일 가능성이 높음
