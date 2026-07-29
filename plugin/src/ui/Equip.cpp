#include "ui/Equip.h"
#include "ui/Grid.h"
#include "ui/Loadout.h"
#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/Sfx.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Equipment doll, v9 mockup: vertical accessory strip (circlet + acc x4) +
// 3-column doll [earring/head/necklace, weapon/body/shield (tall), gaunt/
// boots/ammo, ringR/accM/ringL]. Every slot is 2x2 inventory cells; the
// middle row is 2x4. Empty slots show game-icons silhouettes (CC BY 3.0 —
// Lorc, Delapouite), rasterised white and tinted by the active skin.
// Equip/unequip logic ported from main.cpp's PrismaUI-era listeners.

namespace FUI::Equip
{
    namespace
    {
        struct SlotDef
        {
            const char* id;
            const char* icon;   // silhouette file: slot_<icon>.fic
        };

        constexpr SlotDef kStrip[5] = {
            { "circlet", "circlet" }, { "acc1", "acc" }, { "acc2", "acc" },
            { "acc3", "acc" },        { "acc4", "acc" },
        };
        constexpr SlotDef kDoll[4][3] = {
            { { "earring", "earring" }, { "head", "head" },   { "necklace", "necklace" } },
            { { "weapon", "weapon" },   { "body", "body" },   { "shieldL", "shield" } },
            { { "gaunt", "gaunt" },     { "boots", "boots" }, { "ammo", "ammo" } },
            { { "ringR", "ring" },      { "accM", "acc" },    { "ringL", "ring" } },
        };

        struct EquipEntry
        {
            RE::TESBoundObject* obj = nullptr;
            int                 count = 1;
            std::uint8_t        glow = 0;   // rarity glow bits (Grid::GlowBits)
            // WHICH unit this slot is wearing. The doll used to hold a bare form
            // and re-resolve "the first worn list of that form" on every use, so
            // with a copy in each hand the two slots pointed at ONE list: the
            // right slot showed the left item's stats, lifting one unequipped the
            // other, and a swap could not tell them apart at all.
            std::uint16_t       uid = 0;
            std::uint16_t       sig = 0;
            int                 hand = 0;   // 0 none, 1 right, 2 left
        };

        // equip/unequip actions deferred to the game-update hook (Tick):
        // running them inside the render pass left the player's 3D stale
        // until the menu closed.
        struct PendingAction
        {
            RE::FormID    id = 0;
            std::string   slotId;
            bool          unequip = false;
            // GI1/D4: which sub-stack. 0/-1 = let the engine choose (the old
            // behaviour, still correct when no instance is in hand).
            std::uint16_t uid = 0;
            int           xlIdx = -1;
            // The content signature identifies units the engine gave no uid --
            // tempered and player-enchanted gear, i.e. exactly the copies a
            // player cares about equipping. Without it a drop onto a doll slot
            // fell back to uid 0 / xlIdx -1 and wore an arbitrary copy.
            std::uint16_t sig = 0;
            // Which TILE the item left. Rule 13 forgets exactly that cell -- the
            // one the player acted on -- rather than guessing among siblings.
            std::string   srcKey;
        };
        std::vector<PendingAction> g_pending;
        int                        g_rebuildLag = 0;   // rebuild AFTER the queued task applied

        // L2 loadout tab UI state — shared between the tab strip (sets these) and
        // the confirm windows drawn at TOP LEVEL (like Settings). Rendering the
        // confirm inside the equip panel's child window crashed; a top-level
        // window is the proven pattern.
        bool g_buyOpen = false;    // "+" purchase confirm window open
        int  g_delTarget = -1;     // preset index pending delete confirm (>=1)

        // silhouettes: loaded once, kept for the process lifetime
        std::unordered_map<std::string, IconCache::Icon> g_silhouettes;

        const IconCache::Icon* Silhouette(const char* a_name)
        {
            const auto it = g_silhouettes.find(a_name);
            if (it != g_silhouettes.end()) {
                return it->second.srv ? &it->second : nullptr;
            }
            IconCache::Icon icon;
            const std::string path =
                std::string("Data/SKSE/Plugins/GridInventory_slots/slot_") + a_name + ".fic";
            IconCache::LoadFicTexture(path, icon);   // leaves srv null on failure
            g_silhouettes[a_name] = icon;
            return icon.srv ? &g_silhouettes[a_name] : nullptr;
        }

        const char* SlotForArmor(RE::TESObjectARMO* a_armo)
        {
            using S = RE::BGSBipedObjectForm::BipedObjectSlot;
            if (a_armo->HasPartOf(S::kBody))    return "body";
            if (a_armo->HasPartOf(S::kHead) || a_armo->HasPartOf(S::kHair)) return "head";
            if (a_armo->HasPartOf(S::kHands))   return "gaunt";
            if (a_armo->HasPartOf(S::kFeet))    return "boots";
            if (a_armo->HasPartOf(S::kShield))  return "shieldL";
            if (a_armo->HasPartOf(S::kAmulet))  return "necklace";
            if (a_armo->HasPartOf(S::kRing))    return "ringR";
            if (a_armo->HasPartOf(S::kCirclet)) return "circlet";
            if (a_armo->HasPartOf(S::kEars))    return "earring";
            return nullptr;
        }

        // Does this item belong in the slot it was dropped on?
        //
        // Without this the drop TARGET was ignored entirely: EquipItem only ever
        // read the slot id to spot "shieldL" for a one-hander, so a helmet dropped
        // on the boots slot still went on the head -- and the swap logic then
        // pulled the BOOTS off, handed them to the cursor, and they re-equipped
        // themselves the moment the player tried to put them down.
        bool SlotAccepts(RE::TESBoundObject* a_obj, const std::string& a_slotId)
        {
            if (!a_obj || a_slotId.empty()) return true;   // no target = engine picks
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
                const char* home = SlotForArmor(armo);
                if (!home) return a_slotId.rfind("acc", 0) == 0;   // odd armour -> accessory
                if (a_slotId == home) return true;
                // rings occupy either hand; the doll splits them into two slots
                if (std::string_view(home) == "ringR") {
                    return a_slotId == "ringR" || a_slotId == "ringL";
                }
                return false;
            }
            if (a_obj->Is(RE::FormType::Weapon) || a_obj->Is(RE::FormType::Light)) {
                // weapons take either hand; shieldL doubles as the left hand
                return a_slotId == "weapon" || a_slotId == "shieldL";
            }
            if (a_obj->Is(RE::FormType::Ammo)) return a_slotId == "ammo";
            // potions / scrolls / spell tomes are USED, not worn: any slot is a
            // drop target for them and the engine decides what that means
            return true;
        }

        // Everything currently equipped, mapped to UI slots (D2).
        void CollectEquipment(std::unordered_map<std::string, EquipEntry>& a_out)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            const char* accPool[] = { "acc1", "acc2", "acc3", "acc4", "accM" };
            int accNext = 0;

            auto add = [&](const char* slot, RE::TESBoundObject* obj, int count,
                           std::uint8_t glow = 0, RE::ExtraDataList* xl = nullptr,
                           int hand = 0) {
                if (!obj) return;
                const char* s = slot;
                if (!s) s = (accNext < 5) ? accPool[accNext++] : nullptr;
                if (!s) return;
                std::uint16_t uid = 0;
                if (xl) {
                    if (const auto* xu = xl->GetByType<RE::ExtraUniqueID>()) uid = xu->uniqueID;
                }
                a_out[s] = { obj, count, glow, uid, Grid::InstanceSigOf(xl), hand };
            };

            // equipped SPELLS have no world model (garbage capture) and are
            // not doll items — the hands simply stay empty for them
            auto* right = player->GetEquippedObject(false);
            auto* left = player->GetEquippedObject(true);
            if (right && right->Is(RE::FormType::Spell)) right = nullptr;
            if (left && left->Is(RE::FormType::Spell)) left = nullptr;
            auto* rightObj = right ? right->As<RE::TESBoundObject>() : nullptr;
            // 33-B: `left != right` was a FORM comparison, so wielding two copies
            // of the same weapon reported the left hand as empty. The case it was
            // really guarding is a TWO-HANDER, which the engine reports in both
            // hands -- test for that instead of for form identity.
            bool twoHanded = false;
            if (right == left) {
                if (auto* w = right ? right->As<RE::TESObjectWEAP>() : nullptr) {
                    using WT = RE::WEAPON_TYPE;
                    const auto t = w->GetWeaponType();
                    twoHanded = t == WT::kTwoHandSword || t == WT::kTwoHandAxe ||
                                t == WT::kBow || t == WT::kCrossbow || t == WT::kStaff;
                } else {
                    twoHanded = true;   // not a weapon and reported in both hands
                }
            }
            auto* leftObj = (left && !twoHanded) ? left->As<RE::TESBoundObject>() : nullptr;
            if (rightObj || leftObj) {
                // entry-aware glow (player-crafted enchants live on the entry)
                std::uint8_t rGlow = Grid::GlowBits(rightObj, nullptr, nullptr);
                std::uint8_t lGlow = Grid::GlowBits(leftObj, nullptr, nullptr);
                RE::ExtraDataList* rXl = nullptr;
                RE::ExtraDataList* lXl = nullptr;
                auto inv = player->GetInventory([&](RE::TESBoundObject& o) {
                    return &o == rightObj || &o == leftObj;
                });
                for (auto& [obj, data] : inv) {
                    // GI1/D2: the doll shows the WORN unit — and with the same form
                    // in both hands that means the list for THIS hand, not the first
                    // worn list of the form.
                    if (obj == rightObj) {
                        rXl = Grid::WornExtraOf(data.second.get(), 1);
                        rGlow = Grid::GlowBits(obj, data.second.get(), rXl);
                    }
                    if (obj == leftObj) {
                        lXl = Grid::WornExtraOf(data.second.get(), 2);
                        lGlow = Grid::GlowBits(obj, data.second.get(), lXl);
                    }
                }
                if (rightObj) add("weapon", rightObj, 1, rGlow, rXl, 1);
                if (leftObj) add("shieldL", leftObj, 1, lGlow, lXl, 2);
            }

            if (auto* ammo = player->GetCurrentAmmo()) {
                int n = 1;
                std::uint8_t g = Grid::GlowBits(ammo, nullptr, nullptr);
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == ammo; });
                for (auto& [obj, data] : inv) {
                    n = data.first;
                    g = Grid::GlowBits(obj, data.second.get(),
                                       Grid::WornExtraOf(data.second.get()));
                }
                add("ammo", ammo, n, g);
            }

            auto inv = player->GetInventory(
                [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); });
            bool ringUsed = false;
            for (auto& [obj, data] : inv) {
                auto& [count, entry] = data;
                if (count <= 0 || !entry || !entry->IsWorn()) continue;
                auto* armo = obj->As<RE::TESObjectARMO>();
                if (!armo) continue;
                const char* slot = SlotForArmor(armo);
                if (slot && std::string_view(slot) == "shieldL") {
                    // B9: the left-hand path normally reports the shield —
                    // but if it didn't (mod edge cases / spell in left hand
                    // data), a WORN shield must still show on the doll
                    if (!a_out.contains("shieldL")) {
                        add("shieldL", obj, 1,
                            Grid::GlowBits(obj, entry.get(), Grid::WornExtraOf(entry.get())));
                    }
                    continue;
                }
                if (slot && std::string_view(slot) == "ringR") {
                    if (ringUsed) slot = "ringL";
                    ringUsed = true;
                }
                add(slot, obj, 1, Grid::GlowBits(obj, entry.get(), Grid::WornExtraOf(entry.get())));
            }

            // ---- self-check: THE DOLL SHOWS WHAT THE BODY WEARS ---------------
            // Everything else in this log describes the BOARD. Nothing described
            // the doll, so "the same weapon in both hands" -- where the whole
            // question is whether the left slot resolves to its own unit --
            // produced no evidence at all and had to be judged by eye.
            //
            // Logged only when the doll CHANGES, so this stays quiet.
            {
                std::vector<std::pair<std::string, const EquipEntry*>> rows;
                rows.reserve(a_out.size());
                for (const auto& [k, v] : a_out) rows.push_back({ k, &v });
                std::sort(rows.begin(), rows.end(),
                          [](const auto& x, const auto& y) { return x.first < y.first; });
                std::string line;
                for (const auto& [k, v] : rows) {
                    line += std::format("{}='{}'(u{:04X}/s{:04X}/h{}) ", k,
                        v->obj ? v->obj->GetName() : "?", v->uid, v->sig, v->hand);
                }
                static std::string s_prev;
                if (line != s_prev) {
                    s_prev = line;
                    SKSE::log::info("[DOLL] {}", line.empty() ? "(empty)" : line);
                    // The engine is the authority: every worn unit of a form must
                    // have exactly one doll slot. Fewer slots than worn units is
                    // the dual-wield failure (one hand shows nothing); more is a
                    // unit drawn twice.
                    std::map<RE::TESBoundObject*, int> shown;
                    for (const auto& [k, v] : rows) if (v->obj) ++shown[v->obj];
                    for (const auto& [obj, n] : shown) {
                        auto* e = Grid::LiveEntryOf(player, obj);
                        int wornUnits = 0;
                        if (e && e->extraLists) {
                            for (auto* xl : *e->extraLists) {
                                if (xl && (xl->HasType<RE::ExtraWorn>() ||
                                           xl->HasType<RE::ExtraWornLeft>())) {
                                    wornUnits += (std::max)(1, xl->GetCount());
                                }
                            }
                        }
                        if (wornUnits != n) {
                            SKSE::log::warn("[DOLL] MISMATCH '{}' body wears {} but doll "
                                            "shows {} slot(s)", obj->GetName(), wornUnits, n);
                        }
                    }
                }
            }
        }

        void DrawSlot(const SlotDef& a_slot, float a_w, float a_h,
                      std::unordered_map<std::string, EquipEntry>& a_eq)
        {
            const auto& sk = Theme::S();
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const ImVec2 p1(p0.x + a_w, p0.y + a_h);

            const auto it = a_eq.find(a_slot.id);
            const EquipEntry* eq = it != a_eq.end() ? &it->second : nullptr;

            // frame (v9: border only; filled slots get the skin's filled
            // accent + faint fill)
            if (eq) {
                dl->AddRectFilled(p0, p1, Theme::Col(sk.filled, 0.05f), sk.rounding);
                if (sk.cornerFade) {   // v10.4: equipped highlight = corner fade
                    Theme::CornerFade(dl, p0, p1, Theme::Col(sk.filled, 0.70f));
                } else {
                    dl->AddRect(p0, p1, Theme::Col(sk.filled, 0.8f), sk.rounding);
                }
            } else {
                dl->AddRect(p0, p1, Theme::Acc(sk.cornerFade ? 0.14f : 0.28f), sk.rounding);
            }

            if (eq) {
                if (const auto* icon = IconCache::GetSingleton()->Get(eq->obj)) {
                    // alpha-trimmed sprite. Rings capture close-up and looked
                    // oversized — half size. Everything else CONTAIN-fits the
                    // slot BOX (the old min-side square fit left tall sprites
                    // small inside the tall body/weapon slots).
                    const bool ringSlot = std::string_view(a_slot.id) == "ringR" ||
                                          std::string_view(a_slot.id) == "ringL";
                    float dw, dh;
                    if (ringSlot) {
                        const float target = (std::min)(a_w, a_h) * 0.46f;
                        const float ms = static_cast<float>((std::max)(icon->w, icon->h));
                        dw = icon->w / ms * target;
                        dh = icon->h / ms * target;
                    } else {
                        float s = (std::min)(a_w * 0.92f / icon->w,
                                             a_h * 0.92f / icon->h);
                        // CONTAIN-fitting alone BLOWS UP small items: a dagger
                        // draws 1x2 cells in the grid but the weapon slot box is
                        // far bigger, so the fit scaled it past life size. Cap at
                        // the size the grid would give it — same rule as there
                        // (long axis = footprint long axis * 0.95 * def.scale) —
                        // so a slot can shrink an item to fit but never enlarge it.
                        const auto  def = Grid::ResolveDef(eq->obj);
                        const float gridLong =
                            static_cast<float>((std::max)(def.w, def.h)) *
                            Grid::CellPx() * 0.95f * def.scale;
                        const float longPx = (std::max)(icon->w, icon->h) * s;
                        if (gridLong > 0.0f && longPx > gridLong) s *= gridLong / longPx;
                        dw = icon->w * s;
                        dh = icon->h * s;
                    }
                    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                    // rarity glow + status rings UNDER the icon -- the shared
                    // renderer (GI49: this was a THIRD hand copy of the halo
                    // switch; unmasked, it read poison/temper status bits as
                    // "both rarities" red and never drew the rings)
                    if (eq->glow) {
                        Grid::DrawGlow(dl, eq->obj, eq->glow,
                            ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                            ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f),
                            p0, p1);
                    }
                    UIRoot::DrawItemIcon(dl, icon->srv,
                        ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                        ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f));
                } else {
                    IconCache::GetSingleton()->QueueCapture(eq->obj);
                }
                // GI30: this slot wears the pool's favourite -- draw the mark
                // here so it does not read as "the star disappeared". Same
                // shape/colour as the grid's favourite diamond, bottom-right.
                if (Grid::IsPoolStarWorn(eq->obj, eq->uid, eq->sig)) {
                    const float S2 = Theme::Scale();
                    // the doll slot is 2x2 cells, so the grid's 4px marker read
                    // as a speck here -- size it to the slot, not to a tile
                    const float r = 6.5f * S2;
                    const ImVec2 cc(p1.x - r - 4.0f * S2, p1.y - r - 4.0f * S2);
                    const ImVec2 d0(cc.x, cc.y - r), d1(cc.x + r, cc.y);
                    const ImVec2 d2(cc.x, cc.y + r), d3(cc.x - r, cc.y);
                    dl->AddQuadFilled(d0, d1, d2, d3, IM_COL32(0, 0, 0, 235));
                    const float ri = r - 1.2f;
                    dl->AddQuadFilled(ImVec2(cc.x, cc.y - ri), ImVec2(cc.x + ri, cc.y),
                                      ImVec2(cc.x, cc.y + ri), ImVec2(cc.x - ri, cc.y),
                                      Theme::Col(sk.hi, 1.0f));
                }
                if (eq->count > 1) {   // eqqty (ammo stack)
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", eq->count);
                    const ImVec2 ts = ImGui::CalcTextSize(buf);
                    const ImVec2 tp(p1.x - ts.x - 4.0f, p0.y + 2.0f);
                    dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 230), buf);
                    dl->AddText(tp, Theme::Col(sk.hi, 1.0f), buf);
                }
            } else if (const auto* sil = Silhouette(a_slot.icon)) {
                // white silhouette tinted by the skin accent (46% of the SLOT
                // unit, not the tall height — v9 keeps tall-slot icons modest)
                const float unit = SlotPx();
                const float target = unit * 0.46f;
                const float ms = static_cast<float>((std::max)(sil->w, sil->h));
                const float dw = sil->w / ms * target;
                const float dh = sil->h / ms * target;
                const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                dl->AddImage(reinterpret_cast<ImTextureID>(sil->srv),
                    ImVec2(c.x - dw * 0.5f, c.y - dh * 0.5f),
                    ImVec2(c.x + dw * 0.5f, c.y + dh * 0.5f),
                    ImVec2(0, 0), ImVec2(1, 1), Theme::Acc(0.35f));
            }

            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton((std::string("##eq_") + a_slot.id).c_str(),
                ImVec2(a_w, a_h));

            if (!Grid::IsHolding()) {
                if (eq && ImGui::IsItemHovered()) {
                    // I1: name + stats. D1: the doll shows the WORN unit.
                    // kWorn resolves "the first worn list of this form"; with a
                    // copy in each hand that is the wrong one for one of the two
                    // slots. Hand the tooltip the list this slot actually wears.
                    Grid::DrawItemTooltip(eq->obj, eq->count, -1, -1, false, nullptr,
                                          Grid::ExtraScope::kWorn,
                                          eq->uid, -1, eq->sig, eq->hand);
                }
                if (eq && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {   // D5
                    SKSE::log::info("[ACT] rclick-unequip '{}' slot '{}' hand={}",
                                    eq->obj->GetName(), a_slot.id, eq->hand);
                    g_pending.push_back({ eq->obj->GetFormID(), "", true,
                                          eq->uid, -1, eq->sig });
                }
                if (eq && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    // v9.2: left-click PICKS the equipped item up — unequip
                    // (deferred to Tick) + start carrying it right away
                    // The slot already knows WHICH unit it is wearing -- take it
                    // from there instead of re-resolving "the first worn list of
                    // this form", which answered the other hand's item whenever
                    // the same form was worn twice.
                    g_pending.push_back({ eq->obj->GetFormID(), "", true,
                                          eq->uid, -1, eq->sig });
                    Grid::BeginCarry(eq->obj, eq->uid, eq->sig, eq->hand);
                }
            } else if (ImGui::IsItemHovered()) {
                // C6: carried item over a slot — highlight; click = equip try
                dl->AddRect(p0, p1, Theme::Col(sk.hi, 0.8f), sk.rounding, 0, 2.0f);
                Grid::NotifySlotDropTarget(a_slot.id);
            }
        }
    }

    bool EquipItem(RE::TESBoundObject* a_obj, const std::string& a_slotId,
                   std::uint16_t a_uid, int a_xlIdx, std::uint16_t a_sig,
                   const std::string& a_srcKey)
    {
        if (!a_obj) return false;

        // D3 type gate (sync — callers key refresh off the verdict).
        // Spell tomes pass too: right-click = learn (handled in ProcessPending).
        const auto* book = a_obj->As<RE::TESObjectBOOK>();
        const bool spellTome = book && book->TeachesSpell();
        if (!(a_obj->Is(RE::FormType::Weapon) || a_obj->Is(RE::FormType::Armor) ||
              a_obj->Is(RE::FormType::Ammo) || a_obj->Is(RE::FormType::Light) ||
              a_obj->Is(RE::FormType::AlchemyItem) || a_obj->Is(RE::FormType::Scroll) ||
              spellTome)) {
            return false;
        }

        if (!SlotAccepts(a_obj, a_slotId)) return false;   // wrong slot: reject the drop

        g_pending.push_back({ a_obj->GetFormID(), a_slotId, false, a_uid, a_xlIdx,
                              a_sig, a_srcKey });
        return true;
    }

    // GI7: the doll's own collection lives in an anonymous namespace, so
    // nothing outside could ask "what is worn in this slot". Extensions and the
    // equip path both need it. Re-resolved on every call -- an ExtraDataList*
    // must never outlive the frame it was fetched in.
    RE::TESBoundObject* WornObjectAt(const std::string& a_slotId)
    {
        std::unordered_map<std::string, EquipEntry> eq;
        CollectEquipment(eq);
        const auto it = eq.find(a_slotId);
        return it == eq.end() ? nullptr : it->second.obj;
    }

    RE::ExtraDataList* WornExtraAt(const std::string& a_slotId)
    {
        std::unordered_map<std::string, EquipEntry> eq;
        CollectEquipment(eq);
        const auto it = eq.find(a_slotId);
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (it == eq.end() || !it->second.obj || !player) return nullptr;
        return Grid::WornExtraMatching(Grid::LiveEntryOf(player, it->second.obj),
                                       it->second.uid, it->second.sig, it->second.hand);
    }

    namespace
    {
        std::uint16_t UidOfList(RE::ExtraDataList* a_xl)
        {
            if (!a_xl) return 0;
            const auto* xu = a_xl->GetByType<RE::ExtraUniqueID>();
            return xu ? xu->uniqueID : 0;
        }
    }

    void ProcessPending()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!player || !em) { g_pending.clear(); return; }

        // late pass: rebuild + FORCE the biped 3D refresh once the equip data
        // has settled — while paused the engine never runs the actor update
        // that would apply it (the "only visible after closing" root cause)
        if (g_rebuildLag > 0 && --g_rebuildLag == 0) {
            Grid::RequestRebuild();
            if (auto* proc = player->GetActorRuntimeData().currentProcess) {
                proc->Update3DModel(player);
            }
        }
        if (g_pending.empty()) return;

        for (const auto& act : g_pending) {
            auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(act.id);
            if (!obj) continue;

            if (act.unequip) {
                // D4: unequip the WORN sub-stack explicitly -- with a null list
                // the engine picks, and it does not have to pick the one on the
                // body when spares of the same form sit in the pack.
                auto* wornList = Grid::WornExtraMatching(Grid::LiveEntryOf(player, obj),
                                                         act.uid, act.sig);
                em->UnequipObject(player, obj, wornList, 1, nullptr,
                    false, false, true, true);
                SKSE::log::info("[EQUIP] unequip {}", obj->GetName());
                continue;
            }

            // Spell tome: right-click = LEARN (a BookMenu can't open inside
            // our movie-less UI, so teach directly and consume the tome —
            // same net result as vanilla reading)
            if (auto* book = obj->As<RE::TESObjectBOOK>(); book && book->TeachesSpell()) {
                auto* spell = book->GetSpell();
                if (spell && !player->HasSpell(spell)) {
                    player->AddSpell(spell);
                    RE::DebugNotification(spell->GetName(), "UISpellLearned");
                    // GI36: name the copy being consumed instead of letting the
                    // engine pick, and let rule 58 take its star with it.
                    auto* bxl = Grid::ExtraForInstance(
                        Grid::LiveEntryOf(player, book), act.uid, act.xlIdx);
                    const int starred =
                        (bxl && bxl->HasType<RE::ExtraHotkey>()) ? 1 : 0;
                    player->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove,
                        Grid::ResolveExitUnit(book, act.uid, act.sig, 1, starred),
                        nullptr);
                    Grid::RequestRebuild();
                    SKSE::log::info("[EQUIP] learned spell '{}'", spell->GetName());
                }
                continue;
            }

            // Same-slot conflict resolution BY HAND: while paused the engine's
            // own queued conflict pass is unreliable (stacked body armour) —
            // unequip everything sharing a biped slot with the incoming piece.
            if (auto* armo = obj->As<RE::TESObjectARMO>()) {
                const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask());
                auto worn = player->GetInventory(
                    [](RE::TESBoundObject& o) { return o.Is(RE::FormType::Armor); });
                for (auto& [o2, d2] : worn) {
                    if (d2.first <= 0 || !d2.second || !d2.second->IsWorn()) continue;
                    // Skipping the whole FORM meant a tempered helmet dropped on
                    // a slot wearing a PLAIN copy of the same helmet unequipped
                    // nothing -- the incoming piece had nowhere to go and the
                    // swap silently did not happen. Only the very unit being
                    // equipped is exempt, and units differ by signature.
                    if (o2 == obj) {
                        auto* w2 = Grid::WornExtraOf(d2.second.get());
                        const std::uint16_t s2 = Grid::InstanceSigOf(w2);
                        const bool sameUnit = (act.sig == s2) &&
                                              (act.uid == 0 || act.uid == UidOfList(w2));
                        if (sameUnit) continue;
                    }
                    auto* a2 = o2->As<RE::TESObjectARMO>();
                    if (!a2) continue;
                    if (static_cast<std::uint32_t>(a2->GetSlotMask()) & mask) {
                        em->UnequipObject(player, o2,
                            Grid::WornExtraOf(Grid::LiveEntryOf(player, o2)), 1, nullptr,
                            false, false, false, true);
                        SKSE::log::info("[EQUIP] slot conflict: unequip {}", o2->GetName());
                    }
                }
            }

            // D4: a one-hander (or staff) dropped on the shield slot = left hand
            const RE::BGSEquipSlot* slot = nullptr;
            if (act.slotId == "shieldL") {
                if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    using WT = RE::WEAPON_TYPE;
                    const auto wt = weap->GetWeaponType();
                    if (wt == WT::kOneHandSword || wt == WT::kOneHandDagger ||
                        wt == WT::kOneHandAxe || wt == WT::kOneHandMace || wt == WT::kStaff) {
                        slot = RE::TESForm::LookupByID<RE::BGSEquipSlot>(0x13F43);
                    }
                }
            }

            // D4: equip THIS copy. Resolving late (here, not at request time)
            // is deliberate -- ExtraDataList* must never be cached across
            // frames, the engine reallocates and frees them.
            // Identity first, POSITION last. xlIdx is a list index captured a
            // frame or more before this runs, and the engine reorders lists --
            // a stale index still resolves to a REAL list, just the wrong one,
            // so trying it first meant a carried tempered dagger re-equipped as
            // the plain copy and the signature fallback never even ran.
            RE::ExtraDataList* srcList = nullptr;
            if (act.uid != 0 || act.sig != 0) {
                srcList = Grid::ExtraForPool(Grid::LiveEntryOf(player, obj),
                                             act.uid, act.sig);
            }
            if (!srcList) {
                srcList = Grid::ExtraForInstance(Grid::LiveEntryOf(player, obj),
                                                 act.uid, act.xlIdx);
            }
            em->EquipObject(player, obj, srcList, 1, slot, false, false, true, true);

            // Rule 13: equipping forgets the cell, exactly like selling or
            // storing. A stack that stays visible keeps its tile -- only the
            // unit that actually left is forgotten, and for a stack the tile is
            // still occupied by the rest.
            // The old guard counted the whole FORM, so owning a second copy
            // skipped the forget entirely -- which is exactly the case rule 13
            // is about. What matters is whether THIS TILE still has anything in
            // it: gear holds one unit per tile and is now empty, while a stack
            // tile still shows the rest and keeps its cell.
            // Gear holds one unit per tile, so its tile is now empty. A
            // STACKABLE item (torch, scroll, arrows) also empties its tile when
            // it was the only one -- the old cap-only test let those keep their
            // cell and they walked straight back to it on unequip.
            int cnt = 0;
            {
                auto inv = player->GetInventory(
                    [&](RE::TESBoundObject& o) { return &o == obj; });
                for (auto& [o2, d2] : inv) cnt = d2.first;
            }
            if (Grid::StackCap(obj) <= 1 || cnt <= 1) Grid::ForgetTile(act.srcKey);
            SKSE::log::info("[EQUIP] {}{}", obj->GetName(), slot ? " (left hand)" : "");
        }
        g_pending.clear();
        Grid::MarkEquipsApplied();   // suppression has done its job
        Grid::RequestRebuild();
        g_rebuildLag = 2;
    }

    // L2: custom loadout tab strip — one tab per loadout, clipped horizontal
    // wheel-scroll, trailing "+" purchase button, double-click rename, right-click
    // delete. Replaces ImGui's rigid tab bar so all four interactions fit.
    void DrawLoadoutTabs()
    {
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        static int   s_renaming = -1;
        static bool  s_renameFocus = false;
        static char  s_renameBuf[64] = {};

        const float tabH = ImGui::GetFrameHeight();
        const float gap = 4.0f * S;

        // R3: the strip is a CLIPPED child of the panel width; the mouse wheel
        // scrolls it horizontally and the active tab auto-scrolls into view.
        // (The old suspicion that child+SetScrollX caused the "+" CTD was
        // wrong — the crash was Actor::GetGoldAmount.) The "+" stays OUTSIDE
        // the scroll region so it is always reachable; hidden at the cap.
        const float plusW = Loadout::AtCap() ? 0.0f : tabH + gap;
        const float stripW = PanelW() - plusW;
        const ImVec2 stripPos = ImGui::GetCursorScreenPos();

        ImGui::BeginChild("##lt_strip", ImVec2(stripW, tabH), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            ImGui::SetScrollX(ImGui::GetScrollX() -
                ImGui::GetIO().MouseWheel * 60.0f * S);
        }
        static int s_lastActive = -1;
        const bool activeChanged = s_lastActive != Loadout::Active();

        for (int i = 0; i < Loadout::Count(); ++i) {
            if (i > 0) ImGui::SameLine(0, gap);

            if (s_renaming == i) {
                ImGui::SetNextItemWidth(120.0f * S);
                if (s_renameFocus) { ImGui::SetKeyboardFocusHere(); s_renameFocus = false; }
                const bool enter = ImGui::InputText("##rn", s_renameBuf, sizeof(s_renameBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                if (enter || ImGui::IsItemDeactivated()) {
                    if (s_renameBuf[0]) Loadout::SetName(i, s_renameBuf);
                    s_renaming = -1;
                }
                continue;
            }

            const bool active = (i == Loadout::Active());
            if (active && activeChanged) {
                ImGui::SetScrollHereX(0.5f);   // keep the active tab visible
            }
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Col(active ? sk.sel : sk.acc, active ? 0.30f : 0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Col(sk.sel, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Col(sk.sel, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Col(active ? sk.hi : sk.ink, 1.0f));
            const std::string lbl = std::string(Loadout::Name(i)) + "##lt" + std::to_string(i);
            if (Sfx::Button(lbl.c_str(), ImVec2(0, tabH))) {
                if (!active) Loadout::RequestSwitch(i);
            }
            ImGui::PopStyleColor(4);

            if (i >= 1 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                s_renaming = i;
                s_renameFocus = true;
                std::snprintf(s_renameBuf, sizeof(s_renameBuf), "%s", Loadout::Name(i));
            }
            if (i >= 1 && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                g_delTarget = i;
                g_buyOpen = false;
                Sfx::SelectOn();
            }
        }

        if (activeChanged) s_lastActive = Loadout::Active();
        const float scrollX = ImGui::GetScrollX();
        const float scrollMax = ImGui::GetScrollMaxX();
        ImGui::EndChild();

        // edge fades hint that more tabs are hidden beyond the clip
        if (scrollMax > 0.0f) {
            auto* dl = ImGui::GetWindowDrawList();
            const float fw = 16.0f * S;
            if (scrollX > 1.0f) {
                dl->AddRectFilledMultiColor(stripPos,
                    ImVec2(stripPos.x + fw, stripPos.y + tabH),
                    IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0),
                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170));
            }
            if (scrollX < scrollMax - 1.0f) {
                dl->AddRectFilledMultiColor(
                    ImVec2(stripPos.x + stripW - fw, stripPos.y),
                    ImVec2(stripPos.x + stripW, stripPos.y + tabH),
                    IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 170),
                    IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 0));
            }
        }

        if (!Loadout::AtCap()) {
            ImGui::SameLine(0, gap);
            if (Sfx::Button("+##addlt", ImVec2(tabH, tabH))) {
                g_buyOpen = true;
                g_delTarget = -1;
            }
        }
        // The confirm UI is drawn by DrawLoadoutWindows() at TOP LEVEL (not here
        // inside the equip child) — see UIRoot::Render.
    }

    // L2: buy / delete confirm windows — SAME construction as the settings
    // window (WinManager::ApplyNext fixed size + TitleBar + managed flags),
    // drawn at top level from UIRoot::Render. NOTE: the long "+"-click CTD was
    // never these windows — it was Loadout::PlayerGold -> Actor::GetGoldAmount
    // dereferencing a garbage BGSDefaultObjectManager slot (symbolized crash
    // chain). PlayerGold now returns Grid's cached gold, which is render-safe.
    bool CloseTopPopup()
    {
        if (g_buyOpen) {
            g_buyOpen = false;
            return true;
        }
        if (g_delTarget >= 1) {
            g_delTarget = -1;
            return true;
        }
        return false;
    }

    void DrawLoadoutWindows()
    {
        if (!g_buyOpen && g_delTarget < 1) return;

        auto* wm = WinManager::GetSingleton();
        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float barH = 34.0f * S;
        const float btnW = 96.0f * S;
        const float btnRow = 2.0f * btnW + 8.0f * S;
        // every content line is centred on the window width (the TitleBar's
        // left-anchored content origin left all the slack on the right side)
        auto center = [](float a_itemW) {
            const float w = ImGui::GetWindowSize().x;
            ImGui::SetCursorPosX((std::max)(0.0f, (w - a_itemW) * 0.5f));
        };

        if (g_buyOpen) {
            const int cost = Loadout::NextCost();
            const int gold = Loadout::PlayerGold();   // Grid cache — render-safe
            const bool afford = gold >= cost;

            char costLine[96];
            std::snprintf(costLine, sizeof(costLine), "%s: %dG",
                Lang::T(Lang::Str::CostLabel), cost);
            char goldLine[96];
            std::snprintf(goldLine, sizeof(goldLine), "%s (%dG)",
                Lang::T(Lang::Str::NotEnoughGold), gold);

            const float contentW = (std::max)({ btnRow, 200.0f * S,
                ImGui::CalcTextSize(costLine).x,
                afford ? 0.0f : ImGui::CalcTextSize(goldLine).x,
                ImGui::CalcTextSize(Lang::T(Lang::Str::BuyPresetTab)).x });
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const float sp = ImGui::GetStyle().ItemSpacing.y;
            // left margin 12S (TitleBar's content origin) + matching 18S right
            const ImVec2 size(
                contentW + 30.0f * S + 2.0f * insX,
                barH + 8.0f * S + lineH + (afford ? 0.0f : lineH) +
                    6.0f * S + sp + ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
            if (wm->BeginConfirmPopup("ltbuy", "##fablerim_ltbuy",
                    Lang::T(Lang::Str::BuyPresetTab), size)) {
                g_buyOpen = false;
                Sfx::SelectOff();
            }

            center(ImGui::CalcTextSize(costLine).x);
            ImGui::TextColored(sk.ink, "%s", costLine);
            if (!afford) {
                center(ImGui::CalcTextSize(goldLine).x);
                ImGui::TextColored(ImVec4(0.80f, 0.32f, 0.28f, 1.0f), "%s", goldLine);
            }
            ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
            center(afford ? btnRow : btnW);
            // GI51: Enter/Space confirm (ESC closes via Equip::CloseTopPopup)
            // GI52: never while a text field (loadout rename) owns the keyboard
            const bool keyOk = !ImGui::GetIO().WantTextInput &&
                               (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_Space, false));
            if (afford) {
                if (Sfx::Button(Lang::T(Lang::Str::Confirm), ImVec2(btnW, 0)) || keyOk) {
                    Loadout::RequestPurchase();
                    g_buyOpen = false;
                }
                ImGui::SameLine(0.0f, 8.0f * S);
            }
            if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
                g_buyOpen = false;
            }
            ImGui::End();
        }

        if (g_delTarget >= 1) {
            const char* question = Lang::T(Lang::Str::DeletePresetConfirm);
            const float contentW = (std::max)({ btnRow, 200.0f * S,
                ImGui::CalcTextSize(question).x });
            const float lineH = ImGui::GetTextLineHeightWithSpacing();
            const float sp = ImGui::GetStyle().ItemSpacing.y;
            const ImVec2 size(
                contentW + 30.0f * S + 2.0f * insX,
                barH + 8.0f * S + lineH + 6.0f * S + sp +
                    ImGui::GetFrameHeight() + 18.0f * S + 2.0f * insY);
            if (wm->BeginConfirmPopup("ltdel", "##fablerim_ltdel",
                    Lang::T(Lang::Str::DeleteLabel), size)) {
                g_delTarget = -1;
                Sfx::SelectOff();
            }

            center(ImGui::CalcTextSize(question).x);
            ImGui::TextColored(sk.ink, "%s", question);
            ImGui::Dummy(ImVec2(0.0f, 6.0f * S));
            center(btnRow);
            // GI51: Enter/Space confirm (ESC closes via Equip::CloseTopPopup)
            // GI52: never while a text field (loadout rename) owns the keyboard
            if (Sfx::Button(Lang::T(Lang::Str::DeleteLabel), ImVec2(btnW, 0)) ||
                (!ImGui::GetIO().WantTextInput &&
                 (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                  ImGui::IsKeyPressed(ImGuiKey_Space, false)))) {
                Loadout::RequestRemove(g_delTarget);
                g_delTarget = -1;
            }
            ImGui::SameLine(0.0f, 8.0f * S);
            if (Sfx::Button(Lang::T(Lang::Str::Cancel), ImVec2(btnW, 0), true)) {
                g_delTarget = -1;
            }
            ImGui::End();
        }
    }

    void Draw()
    {
        DrawLoadoutTabs();   // L2: loadout tab strip (switch / + buy / rename / delete / wheel)

        std::unordered_map<std::string, EquipEntry> eq;
        CollectEquipment(eq);

        const float S2 = SlotPx();
        const float gap = GapPx();
        const ImVec2 start = ImGui::GetCursorScreenPos();

        // vertical strip: circlet + acc x4 (total height == doll height)
        for (int i = 0; i < 5; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + i * (S2 + gap)));
            DrawSlot(kStrip[i], S2, S2, eq);
        }

        // doll: 3 columns, rows [2u, tall, 2u, 2u]
        const float rowY[4] = {
            start.y,
            start.y + S2 + gap,
            start.y + S2 + gap + TallPx() + gap,
            start.y + S2 + gap + TallPx() + gap + S2 + gap,
        };
        for (int r = 0; r < 4; ++r) {
            const float h = (r == 1) ? TallPx() : S2;
            for (int c = 0; c < 3; ++c) {
                ImGui::SetCursorScreenPos(
                    ImVec2(start.x + S2 + gap + c * (S2 + gap), rowY[r]));
                DrawSlot(kDoll[r][c], S2, h, eq);
            }
        }

        // reserve extent for the layout cursor
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(PanelW(), 3 * S2 + TallPx() + 3 * gap));
    }
}
