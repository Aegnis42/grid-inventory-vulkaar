# 코스튬 신호

플레이어가 코스튬을 입거나 바꾸거나 벗을 때, 그 상태와 구성 장비를 다른 SKSE
플러그인에 보낸다. 1.4.1 추가.

## 나가는 내용

```
tab 2 · 7 pieces
    0001B39F  Steel Armor
    0001B3A2  Steel Helmet
    0001B3A4  Steel Gauntlets
    ...
```

| | |
|---|---|
| `tab` | 외형을 공급하는 로드아웃 탭. `-1` = 코스튬 없음 |
| `pieces` | 몸에 오르는 방어구의 FormID 목록. 코스튬이 없으면 0개 |

무기·방패·화살통은 코스튬이 건드리지 않으므로 목록에 없다.

같은 내용이 `GridInventory.log`에도 남는다.

```
[API] costume state broadcast: tab 2 (7 piece(s))
[API]   0001B39F 'Steel Armor'
```

## 오는 시점

| 상황 | `tab` |
|---|---|
| 코스튬 입음 / 다른 탭으로 바꿈 | 탭 번호 |
| 코스튬 벗음 / 코스튬이 걸린 탭 삭제 / 새 게임 | `-1` |
| 코스튬이 있는 세이브 로드 | 탭 번호 (탭이 같아도 다시 보낸다) |
| 게임 시작 후 첫 틱 | 현재 상태 |

같은 상태가 연달아 오지 않는다. 올 때마다 갱신하면 된다.

## 받는 법

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
    // ★nullptr 필수. RegisterListener(cb)는 "SKSE" 필터가 걸려 이 메시지를 못 받는다.
    SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnApiMessage);
    return true;
}
```

SKSE 자체 메시지도 받으려면 리스너를 두 개 등록한다. 메시지 번호가 발신자별로
따로 놀아서 하나로 겸할 수 없다.

## 주의

- `pieces`는 콜백이 끝나면 무효. 필요한 값은 콜백 안에서 복사할 것.
- 게임 스레드로 온다. 메뉴를 열거나 닫는 호출 금지.
- `tab`은 현재 위치일 뿐이다. 아래 탭이 지워지면 밀린다. 저장해 두고 나중에
  그 번호를 믿으면 안 된다.
- `uid`는 항상 0이다. 코스튬은 폼을 지목하지 인벤토리의 특정 사본을 가리키지
  않는다.
- 착용 장비는 바뀌지 않는다. 능력치는 장비를 직접 읽을 것.

## 구조체

```cpp
namespace GridInvAPI
{
    inline constexpr std::uint32_t kABIVersion      = 1;
    inline constexpr std::uint32_t kMsgCostumeState = 0x47494353;  // 'GICS'

    struct ItemKey                 // 16바이트
    {
        std::uint32_t owner;       // 플레이어 = 0x14
        std::uint32_t base;        // 장비 FormID
        std::uint16_t uid;         // 항상 0
        std::uint16_t _pad0;
        std::uint32_t _pad1;
    };

    struct CostumeState            // 24바이트
    {
        std::uint32_t  structSize;
        std::uint32_t  abiVersion;   // 다르면 무시
        std::int32_t   tab;
        std::uint32_t  pieceCount;
        const ItemKey* pieces;
    };
}
```

## 안 될 때

로그에 `costume state broadcast` 줄이 있는데 콜백이 안 불리면 `nullptr` 문제다.
줄 자체가 없으면 로그와 함께 알려주기 바란다.
