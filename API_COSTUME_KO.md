# 코스튬 상태 신호

Grid Inventory는 플레이어의 **코스튬**(외형 전용 복장)이 바뀔 때마다 이를
SKSE 플러그인에게 알린다. 이 문서가 그 계약이다.

코스튬은 로드아웃 탭 하나가 가진 세트를 몸에 보여주되, 능력치는 실제로 착용
중인 장비에서 그대로 온다. 즉 **"플레이어가 지금 어떻게 보이는가"는 장비로
알 수 없다.** 코스튬은 착용을 바꾸지 않고 외형만 바꾸기 때문이다. 이 신호가
존재하는 이유가 그것이다.

**1.4.1**에 추가됐다. ABI 변경도, 등록 절차도 필요 없다. 이 메시지를 듣지 않는
플러그인은 그냥 보지 못할 뿐이다.

---

## 1. 메시지

```cpp
namespace GridInvAPI
{
    inline constexpr std::uint32_t kABIVersion     = 1;
    inline constexpr std::uint32_t kMsgCostumeState = 0x47494353;  // 'GICS'

    struct ItemKey                 // 16바이트, ABI v1부터 그대로
    {
        std::uint32_t owner;       // 컨테이너 REFR FormID, 플레이어는 0x14
        std::uint32_t base;        // TESBoundObject FormID
        std::uint16_t uid;         // ExtraUniqueID, 0 = 평범한 스택 단위
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };

    struct CostumeState            // 24바이트
    {
        std::uint32_t  structSize;   // = sizeof(CostumeState)
        std::uint32_t  abiVersion;   // = kABIVersion
        std::int32_t   tab;          // 외형을 공급하는 로드아웃 탭, -1 = 없음
        std::uint32_t  pieceCount;   // tab이 -1이면 0
        const ItemKey* pieces;       // 빌려주는 것 — §4 참조
    };
}
```

전체 헤더는 [`plugin/src/api/GridInventoryAPI.h`](plugin/src/api/GridInventoryAPI.h)에
있다. 프로젝트로 복사해 쓰면 되고, `<cstdint>` 외에 의존성이 없다.

---

## 2. 수신하기

```cpp
static void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg || a_msg->type != GridInvAPI::kMsgCostumeState) return;
    if (a_msg->dataLen < sizeof(GridInvAPI::CostumeState))      return;

    const auto* st = static_cast<const GridInvAPI::CostumeState*>(a_msg->data);
    if (st->abiVersion != GridInvAPI::kABIVersion) return;

    if (st->tab < 0) {
        // 코스튬 없음: 착용 장비 그대로 보인다.
        return;
    }
    for (std::uint32_t i = 0; i < st->pieceCount; ++i) {
        const RE::FormID armor = st->pieces[i].base;
        // ...반환하기 전에 필요한 것을 복사할 것 (§4)
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    return true;
}
```

### `nullptr`이 중요하다

`RegisterListener(cb)`는 `RegisterListener("SKSE", cb)`의 축약형이고, 이것은
**플러그인이 보낸 것을 전부 걸러낸다** — 이 메시지도 포함해서. 반드시
**발신자를 `nullptr`로 명시해** 등록해야 받는다.

SKSE 생명주기 메시지도 함께 다룬다면 리스너를 **두 개** 등록한다.

```cpp
RegisterListener(OnLifecycle);            // == RegisterListener("SKSE", ..)
RegisterListener(nullptr, OnApiMessage);  // 필터 없음
```

필터 없는 리스너 하나로 둘을 겸할 수 없다. **메시지 타입 번호는 발신자마다
따로 노는 이름공간**이기 때문이다. 다른 플러그인의 "타입 1"은 `kPostLoad`와
구분되지 않고, 그러면 생명주기 처리가 아무 때나 발화한다.

---

## 3. 언제 오는가

**모든 전환에서** 보낸다. 플레이어가 일으킨 것만이 아니다.

| 계기 | 받는 내용 |
|---|---|
| 로드아웃 탭을 코스튬으로 지정 | `tab = N`, 조각 목록 |
| 다른 탭으로 바꿈 | `tab = M`, 새 조각 목록 |
| 코스튬 해제 | `tab = -1`, `pieceCount = 0` |
| 코스튬이 가리키던 탭이 삭제됨 | `tab = -1`, `pieceCount = 0` |
| 코스튬이 든 세이브를 로드 | `tab = N`, 조각 목록 |
| 새 게임으로 되돌리기 | `tab = -1`, `pieceCount = 0` |
| 세션의 첫 틱 | 현재 상태 (무엇이든) |

**로드 후에는 탭이 같아도 다시 알린다.** 받는 쪽 상태도 로드로 사라졌을
테니, "변한 것 없음"은 틀린 답이기 때문이다.

메시지 하나는 상태가 **실제로 움직였다**는 뜻이다. 한 세션 안에서 같은 상태를
연달아 두 번 받지 않으므로, 비교 없이 매번 변경으로 취급해도 된다.

---

## 4. 규칙

### `pieces`는 빌려주는 것이다

Grid Inventory 내부 버퍼를 가리키며, **콜백이 도는 동안만 유효하다.** 반환하기
전에 필요한 것을 복사할 것. 버퍼는 다음 방송에서 재사용된다.

### 게임 스레드로 온다

Grid Inventory의 매 프레임 틱에서 보낸다. 블로킹하지 말고, 메뉴를 열거나 닫을
수 있는 호출을 하지 말 것.

### 방어구만 온다

로드아웃 탭은 무기·방패·화살통까지 포함한 한 벌이지만, **코스튬은 그것들을
건드리지 않는다** — 입는 것이 아니라 드는 것이기 때문이다. 실제로 몸에 오르는
조각만 목록에 들어가므로, 모든 항목은 플레이어가 지금 그렇게 *보이는* 것이다.

따라서 `pieceCount`는 보통 탭의 아이템 수보다 적다.

### 조각은 폼이지 스택 단위가 아니다

`base`는 방어구의 `FormID`, `owner`는 플레이어(`0x14`), `uid`는 항상 `0`이다.
코스튬은 **무엇처럼 보일지**를 지목하는 것이지 인벤토리의 특정 사본을
가리키지 않는다. 여기서 `uid`로 인스턴스를 식별하려 하지 말 것.

### `tab`은 인덱스이지 신원이 아니다

로드아웃 탭의 현재 위치일 뿐이다. 아래쪽 탭이 지워지면 번호가 밀리는데, 그때도
새 메시지가 오므로 값이 낡을 일은 없다. 세션을 넘겨 저장하지 말 것.

---

## 5. 버전

`abiVersion`을 확인하고 **맞지 않으면 무시하라.** Grid Inventory도 버전이 다른
확장을 전부 거절한다. 반대 방향에서도 같은 정도로 엄격한 것이 맞다.

`structSize`는 구조체가 자랄 수 있게 하려고 있다. 나중 버전이 필드를 뒤에
덧붙이면 `dataLen`과 `structSize`가 함께 커지고 위쪽은 제자리에 남으므로,
**자기 헤더 기준** `dataLen >= sizeof(CostumeState)` 비교가 계속 통한다.

---

## 6. 이것이 아닌 것

- **장비 신호가 아니다.** 착용 중인 것은 바뀌지 않았고 보이는 방식만 바뀌었다.
  능력치는 장비를 읽어야 한다.
- **요청 통로가 아니다.** 반환값은 읽지 않는다. 이 메시지는 바깥으로만 흐르며,
  이것으로 코스튬을 거부하거나 바꿀 방법은 없다.
- **매 프레임 오는 것이 아니다.** 전환 시에만 발화한다. 임의의 시점에 현재
  상태가 필요하면 마지막 메시지를 캐시할 것.

---

## 7. 문제 신고

`GridInventory.log`에서 나왔어야 할 줄과 함께 이슈를 열어주기 바란다.

```
[API] costume state broadcast: tab 2 (7 piece(s))
```

이 줄이 있는데 리스너가 발화하지 않았다면, 원인은 거의 항상 §2의 필터 함정이다.
