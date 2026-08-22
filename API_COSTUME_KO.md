# 코스튬 신호 (모드 제작자용)

Grid Inventory는 플레이어가 **코스튬을 입거나 바꾸거나 벗을 때** 다른 SKSE
플러그인에게 알린다. 무엇을 입고 있는지, 그것이 어떤 장비들로 이루어졌는지가
함께 간다.

**1.4.1**에 추가됐다.

---

## 무엇이 나가는가

코스튬을 입으면 이런 내용이 나간다.

```
탭 2번 · 장비 7개
    0001B39F  Steel Armor
    0001B3A2  Steel Helmet
    0001B3A4  Steel Gauntlets
    0001B3A6  Steel Boots
    000261C0  Amulet of Talos
    ...
```

벗으면 이렇게 나간다.

```
탭 없음 · 장비 0개
```

즉 두 가지가 간다.

| | |
|---|---|
| **탭 번호** | 외형을 공급하는 로드아웃 탭. `-1`이면 코스튬 없음 |
| **장비 목록** | 그 탭에서 **실제로 몸에 오르는** 방어구들의 FormID |

FormID만 받으면 이름·모델·슬롯은 `TESForm::LookupByID`로 전부 얻을 수 있다.

같은 내용이 `GridInventory.log`에도 남으니, 잘 나가는지는 로그로 확인하면
된다.

```
[API] costume state broadcast: tab 2 (7 piece(s))
[API]   0001B39F 'Steel Armor'
[API]   0001B3A2 'Steel Helmet'
```

---

## 무기는 안 나간다

로드아웃 탭에는 무기·방패·화살통도 들어 있지만 **코스튬은 그것들을 건드리지
않는다.** 입는 것이 아니라 드는 것이기 때문이다.

그래서 목록에는 **정말 몸에 보이는 것만** 담긴다. 탭에 15개가 들어 있어도
장비 목록은 14개일 수 있다.

---

## 언제 오는가

플레이어가 누른 순간만이 아니라 **상태가 바뀌는 모든 경우**에 온다.

| 상황 | 오는 내용 |
|---|---|
| 코스튬을 입음 | 탭 번호 + 장비 목록 |
| 다른 탭으로 바꿈 | 새 탭 번호 + 새 장비 목록 |
| 코스튬을 벗음 | 탭 `-1`, 장비 0개 |
| 코스튬이 걸린 탭을 삭제함 | 탭 `-1`, 장비 0개 |
| 코스튬이 있는 세이브를 불러옴 | 탭 번호 + 장비 목록 |
| 새 게임으로 돌아감 | 탭 `-1`, 장비 0개 |
| 게임을 켠 뒤 첫 순간 | 그때의 상태 |

**세이브를 불러오면 탭이 같아도 다시 보낸다.** 받는 쪽이 들고 있던 정보도
불러오기로 사라졌을 테니, "변한 것 없음"이라고 하면 그쪽 상태가 틀린 채로
남는다.

한 번 오는 것은 **실제로 바뀌었다**는 뜻이다. 같은 내용이 연달아 두 번 오지
않으므로, 올 때마다 그냥 갱신하면 된다.

---

## 받는 법

```cpp
#include "GridInventoryAPI.h"   // plugin/src/api/GridInventoryAPI.h

static void OnApiMessage(SKSE::MessagingInterface::Message* a_msg)
{
    if (!a_msg || a_msg->type != GridInvAPI::kMsgCostumeState) return;
    if (a_msg->dataLen < sizeof(GridInvAPI::CostumeState))      return;

    const auto* st = static_cast<const GridInvAPI::CostumeState*>(a_msg->data);
    if (st->abiVersion != GridInvAPI::kABIVersion) return;

    if (st->tab < 0) {
        // 코스튬 없음
        return;
    }
    for (std::uint32_t i = 0; i < st->pieceCount; ++i) {
        const RE::FormID id = st->pieces[i].base;
        // 필요한 것을 여기서 복사해 둘 것 (아래 ★ 참고)
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    return true;
}
```

### ★ 등록할 때 `nullptr`을 빠뜨리지 말 것

이것 하나로 아무것도 못 받는다. 가장 흔한 실수다.

```cpp
RegisterListener(콜백);            // ✗ 못 받는다
RegisterListener(nullptr, 콜백);   // ✓ 받는다
```

인자 하나짜리는 `RegisterListener("SKSE", 콜백)`의 줄임말이고, 이것은 **다른
플러그인이 보낸 것을 전부 걸러낸다.** 우리 로그에는 신호가 나갔다고 찍히는데
그쪽에서는 아무 일도 안 일어나므로, 원인을 찾기가 특히 어렵다.

SKSE 자체 메시지(게임 로드 등)도 받아야 한다면 **리스너를 두 개** 등록한다.
하나로 겸할 수 없다 — 메시지 번호는 보낸 쪽마다 따로 노는 값이라, 다른
플러그인의 "1번"과 SKSE의 "1번"이 구분되지 않기 때문이다.

---

## 지켜야 할 것 세 가지

### 1. 장비 목록은 빌려주는 것이다

`pieces`는 Grid Inventory 내부 배열을 가리킨다. **콜백이 끝나면 무효**이고 다음
신호에서 덮어쓴다. 필요한 값은 콜백 안에서 복사해 둘 것.

### 2. 게임 스레드로 온다

매 프레임 도는 곳에서 보낸다. 오래 붙잡지 말고, 메뉴를 열거나 닫는 호출은
하지 말 것.

### 3. 탭 번호는 이름표가 아니다

로드아웃 탭의 **현재 위치**일 뿐이다. 아래쪽 탭이 지워지면 번호가 밀린다.
밀릴 때도 새 신호가 오니 값이 낡을 일은 없지만, 세이브에 저장해 두고 나중에
그 번호를 믿으면 안 된다.

---

## 이런 용도는 아니다

- **장비 신호가 아니다.** 착용 중인 장비는 그대로다. 능력치가 필요하면 장비를
  직접 읽어야 한다. 코스튬은 겉모습만 바꾼다.
- **되돌려줄 수 없다.** 이 신호는 나가기만 한다. 여기서 코스튬을 막거나
  바꿀 방법은 없다.
- **매 프레임 오지 않는다.** 바뀔 때만 온다. 아무 때나 현재 상태가 필요하면
  마지막으로 받은 것을 들고 있을 것.

---

## 구조체

```cpp
namespace GridInvAPI
{
    inline constexpr std::uint32_t kABIVersion      = 1;
    inline constexpr std::uint32_t kMsgCostumeState = 0x47494353;  // 'GICS'

    struct ItemKey                 // 16바이트
    {
        std::uint32_t owner;       // 플레이어 = 0x14
        std::uint32_t base;        // ★장비의 FormID — 이것이 "어떤 장비인지"
        std::uint16_t uid;         // 코스튬에서는 항상 0
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };

    struct CostumeState            // 24바이트
    {
        std::uint32_t  structSize;   // = sizeof(CostumeState)
        std::uint32_t  abiVersion;   // = kABIVersion, 다르면 무시할 것
        std::int32_t   tab;          // 탭 번호, -1 = 코스튬 없음
        std::uint32_t  pieceCount;   // 장비 개수, 코스튬이 없으면 0
        const ItemKey* pieces;       // 장비 배열 (빌려주는 것)
    };
}
```

`uid`는 코스튬에서 항상 `0`이다. 코스튬은 **무엇처럼 보일지**를 정하는 것이지
인벤토리의 특정 사본을 가리키지 않기 때문이다.

`structSize`는 나중에 항목이 늘어나도 기존 코드가 깨지지 않게 하려고 있다.
위쪽 항목은 자리를 옮기지 않으므로, 자기 헤더 기준으로 크기를 비교하면 계속
동작한다.

---

## 안 될 때

`GridInventory.log`에 이 줄이 있는지 먼저 본다.

```
[API] costume state broadcast: tab 2 (7 piece(s))
```

**줄이 있는데 콜백이 안 불린다면** → 위의 `nullptr` 문제일 가능성이 가장 크다.

**줄 자체가 없다면** → 코스튬 상태가 실제로 바뀌지 않았거나, 우리 쪽 문제다.
로그와 함께 알려주기 바란다.
