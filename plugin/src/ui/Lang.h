#pragma once

namespace FUI::Lang
{
    // i18n (v9): English default; persisted as "!lang" (0 EN / 1 KO / 2 ZH / 3 JA).
    //
    // Phase 2 X-macro: ONE row per string — the enum and all four tables are
    // generated from THIS list, so the former positional desync (a missing
    // entry nullptr-crashed at runtime with no compile error) is physically
    // impossible. Adding a string = adding one row here.
    //   X(name, en, ko, zh, ja)
    #define FUI_LANG_STRINGS(X)                                                                                              \
        X(Inventory, "INVENTORY", "인벤토리", "物品栏", "インベントリ")                          /* main title */             \
        X(Edit, "EDIT", "편집", "编辑", "編集")                                                                              \
        X(Settings, "SETTINGS", "설정", "设置", "設定")                                                                      \
        X(ScaleLabel, "SCALE", "크기", "缩放", "スケール")                                                                   \
        X(SkinLabel, "SKIN", "스킨", "皮肤", "スキン")                                                                       \
        X(LanguageLabel, "LANGUAGE", "언어", "语言", "言語")                                                                 \
        X(Gold, "GOLD", "골드", "金币", "ゴールド")                                                                          \
        X(Items, "ITEMS", "아이템", "物品", "アイテム")                                                                      \
        X(EquipTab, "EQUIP", "장비", "装备", "装備")                                                                         \
        X(CloseHint, "I / ESC to close", "I / ESC 닫기", "I / ESC 关闭", "I / ESC で閉じる")                                 \
        X(ResetDefault, "Reset to default", "기본값 복귀", "恢复默认", "デフォルトに戻す")                                   \
        X(SaveCategory, "Save as category default", "카테고리 기본값으로 저장", "保存为类别默认", "カテゴリ既定値として保存") \
        X(Preset, "PRESET", "프리셋", "预设", "プリセット")                                                                  \
        X(Save, "Save", "저장", "保存", "保存")                                                                              \
        X(Load, "Load", "불러오기", "加载", "読み込み")                                                                      \
        X(LoadHint,                                                                                                          \
            "Load replaces your current setup (items + category defaults)",                                                  \
            "불러오기는 현재 설정 전체를 교체합니다 (아이템 + 카테고리 기본값)",                                             \
            "加载将替换当前全部设置（物品 + 类别默认值）",                                                                   \
            "読み込みは現在の設定全体を置き換えます（アイテム＋カテゴリ既定値）")                                            \
        X(Bag, "Bag", "가방", "背包", "バッグ")                                                                              \
        X(FootprintHint, "Footprint - drag to paint", "칸 모양 - 드래그로 칠하기", "格子形状 - 拖动绘制", "マス形状 - ドラッグで塗る") \
        X(Caching, "caching icons...", "아이콘 캐싱 중...", "正在缓存图标...", "アイコンをキャッシュ中...")                  \
        X(SelectHint, "click an item to edit", "편집할 아이템을 클릭하세요", "点击物品进行编辑", "アイテムをクリックして編集") \
        X(Damage, "Damage", "공격력", "伤害", "攻撃力")                                          /* tooltip stats (I1) */    \
        X(Armor, "Armor", "방어력", "护甲", "防御力")                                                                        \
        X(Weight, "Weight", "무게", "重量", "重量")                                                                          \
        X(Value, "Value", "가격", "价值", "価値")                                                                            \
        X(InventoryFull,                                                                        /* capacity: pickup blocked */ \
            "Inventory is full - no room for this item", "소지품에 빈 칸이 없습니다",                                        \
            "背包已满，无法拾取", "所持品がいっぱいです")                                                                    \
        X(BuyPresetTab, "Buy Preset Tab", "프리셋 탭 구매", "购买预设标签", "プリセットタブ購入") /* L2 popups */             \
        X(CostLabel, "Cost", "비용", "费用", "コスト")                                                                       \
        X(NotEnoughGold, "Not enough gold", "골드 부족", "金币不足", "ゴールド不足")                                         \
        X(Confirm, "OK", "확인", "确认", "確認")                                                                             \
        X(MaxLabel, "MAX", "최대", "最大", "最大")                             /* GI46 */                                                                             \
        X(Cancel, "Cancel", "취소", "取消", "キャンセル")                                                                    \
        X(DeleteLabel, "Delete", "삭제", "删除", "削除")                                                                     \
        X(DeletePresetConfirm, "Delete this preset?", "이 프리셋을 삭제할까요?", "删除此预设？", "このプリセットを削除？")   \
        X(Overloaded,                                                                           /* W2 encumbrance */         \
            "Inventory space exceeded - movement slowed", "소지 공간 초과 - 이동이 느려집니다",                              \
            "背包空间不足 - 移动速度下降", "所持スペース超過 - 移動が遅くなります")                                          \
        X(StatArmor, "Armor", "방어도", "护甲", "防御力")                                        /* S1 stats panel */        \
        X(StatDamage, "Weapon Dmg", "무기 피해", "武器伤害", "武器ダメージ")                                                 \
        X(StatSpeed, "Atk Speed", "공격 속도", "攻击速度", "攻撃速度")                                                       \
        X(StatCrit, "Critical", "치명타", "暴击", "クリティカル")                                                            \
        X(StatSpace, "Space", "사용 공간", "使用空间", "使用スペース")                                                       \
        X(Withdraw, "Withdraw", "인출", "取出", "引き出す")                                      /* G2 coin pouch */         \
        X(StoredLabel, "Stored", "보관", "存放", "保管")                                                                     \
        X(Teaches, "Teaches", "가르침", "教授", "習得魔法")                                      /* tooltip extras (I1+) */  \
        X(Known, "known", "습득함", "已学会", "習得済み")                                                                    \
        X(PoisonLabel, "Poison", "독", "毒", "毒")                                                                           \
        X(ChargeLabel, "Charge", "충전", "充能", "チャージ")                                                                 \
        X(TemperLabel, "Tempered", "강화", "锻造强化", "強化")                                                  \
        X(SoulLabel, "Soul", "영혼", "灵魂", "魂")                                                                           \
        X(SoulPetty, "Petty", "최하급", "微小", "最小")                                                                      \
        X(SoulLesser, "Lesser", "하급", "次级", "小")                                                                        \
        X(SoulCommon, "Common", "중급", "普通", "中")                                                                        \
        X(SoulGreater, "Greater", "상급", "高级", "大")                                                                      \
        X(SoulGrand, "Grand", "최상급", "极品", "極大")                                                                      \
        X(IconStyleLabel, "ICON STYLE", "아이콘 스타일", "图标风格", "アイコンスタイル")         /* two-pak icon style */    \
        X(StyleRealistic, "Realistic", "리얼리스틱", "写实", "リアル")                                                        \
        X(StyleStylized, "Stylized", "스타일", "风格化", "スタイライズ")                                                      \
        X(GlowLabel, "GLOW", "후광", "光晕", "光彩")                                             /* settings: rarity glow */ \
        X(GlowSilhouette, "Silhouette", "실루엣", "轮廓", "シルエット")                                                      \
        X(GlowRadial, "Radial", "라디얼", "径向", "放射")                                                                    \
        X(GlowBrightLabel, "GLOW LEVEL", "후광 밝기", "光晕亮度", "光彩の強さ")                                              \
        X(IconBrightLabel, "ICON LIGHT", "아이콘 밝기", "图标亮度", "アイコンの明るさ")                                      \
        X(CacheLabel, "ICON CACHE", "아이콘 캐시", "图标缓存", "アイコンキャッシュ")                                         \
        X(CacheReset, "Reset", "초기화", "重置", "初期化")                                                                   \
        X(BuyLabel, "Buy", "구매가", "买入", "購入")                                             /* Phase 4 barter */        \
        X(SellLabel, "Sell", "판매가", "卖出", "売却")                                                                       \
        X(MerchantGoldLabel, "Merchant Gold", "상인 소지금", "商人金币", "商人の所持金")                                     \
        X(MerchantNoGold,                                                                                                    \
            "Merchant can't afford that", "상인 소지금이 부족합니다",                                                        \
            "商人金币不足", "商人の所持金が足りません")                                                                      \
        X(SellFavoriteConfirm,                                                                                               \
            "Sell this favorited item?", "즐겨찾기 아이템을 판매할까요?",                                                    \
            "出售收藏的物品？", "お気に入りを売却しますか？")                                                                \
        X(MerchantWontBuy,                                                                      /* Phase 6 restriction */   \
            "The merchant doesn't deal in that", "상인이 취급하지 않는 물품입니다",                                          \
            "商人不收购此类物品", "その商品は取り扱っていません")                                                            \
        X(QuestItemLocked,                                                                      /* Phase 7 quest guard */   \
            "Quest items can't be removed", "퀘스트 아이템은 옮길 수 없습니다",                                              \
            "任务物品无法移除", "クエストアイテムは手放せません")                                                            \
        X(TakeLabel, "Take", "획득", "取得", "取得")                                             /* slider action labels */  \
        X(StoreLabel, "Store", "보관", "存放", "保管")                                                                       \
        X(SplitLabel, "Split", "나누기", "拆分", "分割")                                                                     \
        X(EquippedLabel, "Equipped", "착용 중", "已装备", "装備中")                              /* shift-compare card */    \
        X(HintTakeAll, "R  Take all", "R  모두 획득", "R  全部拾取", "R  すべて取得")                                        \
        X(PrecacheLabel, "Precache All", "전체 사전 캐싱", "全部预缓存", "事前キャッシュ")                                   \
        X(PrecacheStart, "Start", "시작", "开始", "開始")                                                                    \
        X(CopyProps, "Copy props", "속성 복사", "复制属性", "属性コピー")                        /* editor prop clipboard */ \
        X(PasteProps, "Paste props", "속성 붙여넣기", "粘贴属性", "属性貼り付け")                                            \
        X(SectionGeneral, "GENERAL", "일반", "常规", "一般")                                     /* F5 settings sections */  \
        X(SectionDisplay, "DISPLAY", "표시", "显示", "表示")                                                                 \
        X(SectionTrade, "TRADE", "거래", "交易", "取引")                                                                     \
        X(SectionIcons, "ICONS", "아이콘", "图标", "アイコン")                                                               \
        X(MerchGoldSetLabel, "MERCHANT GOLD", "상인 소지금", "商人金币", "商人の所持金")         /* F3 */                    \
        X(MerchStockSetLabel, "MERCHANT BUYS", "상인 매입", "商人收购", "商人の買取")            /* F4 */                    \
        X(ToggleDefault, "Default", "기본", "默认", "標準")                                                                  \
        X(ToggleUnlimited, "Unlimited", "무제한", "无限", "無制限")                                                          \
        X(ToggleAnything, "Anything", "모든 품목", "所有物品", "すべて")                                                     \
        X(StealTitle, "STEAL", "훔치기", "偷窃", "盗む")                                         /* F6a steal container */  \
        X(TrashTitle, "TRASH", "휴지통", "垃圾箱", "ゴミ箱")                                     /* F2 trash window */      \
        X(TrashFavConfirm,                                                                                                   \
            "Trash this favorited item?", "즐겨찾기 아이템을 버릴까요?",                                                     \
            "丢弃收藏的物品？", "お気に入りを捨てますか？")                                                                  \
        X(TrashGoldBlocked,                                                                                                  \
            "Gold can't be trashed - drop it outside instead", "골드는 휴지통에 넣을 수 없습니다 - 바깥에 드랍하세요",       \
            "金币无法丢入垃圾箱 - 请丢到窗外", "ゴールドはゴミ箱に入れられません - 外にドロップ")                            \
        X(TrashBagBlocked,                                                                                                   \
            "Empty the bag before trashing it", "가방을 비운 뒤에 버릴 수 있습니다",                                         \
            "先清空背包才能丢弃", "バッグを空にしてから捨ててください")                                                      \
        X(PickpocketTitle, "PICKPOCKET", "소매치기", "扒窃", "スリ")                             /* F6b */                   \
        X(PresetLabel, "PRESET", "프리셋", "预设", "プリセット")                     /* GI46/47 */ \
        X(PresetExport, "Export", "내보내기", "导出", "書き出し")                                                                \
        X(PresetImport, "Import", "가져오기", "导入", "読み込み")                                                                \
        X(PresetMissing,                                                                                                 \
            "No preset file found", "프리셋 파일이 없습니다",                                                                    \
            "未找到预设文件", "プリセットファイルがありません")                  \
        X(PickpocketBlocked,                                                                                                 \
            "Stealing worn gear needs the Perfect Touch perk", "착용 장비는 퍼펙트 터치 퍼크가 필요합니다",                  \
            "偷窃穿戴装备需要妙手神偷特技", "装備中の品はパーフェクトタッチが必要")                                       \
        X(AmbiguousUnit,                                                                              /* GI42 */             \
            "Can't tell it apart from the worn copy", "착용 중인 사본과 구분할 수 없습니다",                                             \
            "无法与穿戴中的同类物品区分", "装備中の同一品と区別できません")          \
        X(InspectKeyHint, "C  inspect in 3D", "C  3D로 살펴보기", "C  3D 查看", "C  3D で見る") /* vanilla Item Zoom */    \
        X(InspectHint,                                                                        /* inspect overlay */       \
            "drag rotate · wheel zoom · R reset · C / ESC close",                                                         \
            "드래그 회전 · 휠 확대 · R 초기화 · C / ESC 닫기",                                                            \
            "拖动旋转 · 滚轮缩放 · R 重置 · C / ESC 关闭",                                                                \
            "ドラッグ回転 · ホイール拡大 · R リセット · C / ESC 閉じる")

    enum class Str : int
    {
#define FUI_LANG_ENUM_ROW(name, en, ko, zh, ja) name,
        FUI_LANG_STRINGS(FUI_LANG_ENUM_ROW)
#undef FUI_LANG_ENUM_ROW
        Count_
    };

    [[nodiscard]] int  Get();       // 0..3
    void SetLang(int a_lang);
    [[nodiscard]] const char* T(Str a_key);
}
