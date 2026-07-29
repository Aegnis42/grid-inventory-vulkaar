#include "ui/Fallback.h"

#include <cstring>
#include <string>
#include <unordered_map>

namespace FUI::Fallback
{
    namespace
    {
        constexpr const char* kDir = "Data/SKSE/Plugins/GridInventory_fallback/";

        // key -> loaded icon; srv==null entries are negative cache (missing
        // file), so each key hits the filesystem at most once per session
        std::unordered_map<std::string, IconCache::Icon> g_cache;
        bool g_anyLoaded = false;

        [[nodiscard]] std::string LowerModel(RE::TESBoundObject* a_obj)
        {
            const auto* model = skyrim_cast<RE::TESModel*>(a_obj);
            const char* path = model ? model->GetModel() : nullptr;
            std::string s = path ? path : "";
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        [[nodiscard]] bool Has(const std::string& a_hay, const char* a_needle)
        {
            return a_hay.find(a_needle) != std::string::npos;
        }

        // material keyword (WeapMaterial*/ArmorMaterial*/DLC variants) ->
        // palette name used by the baked @variant files
        [[nodiscard]] const char* MaterialOf(RE::TESBoundObject* a_obj)
        {
            const auto* kwf = skyrim_cast<RE::BGSKeywordForm*>(a_obj);
            if (!kwf) return nullptr;
            for (std::uint32_t i = 0; i < kwf->numKeywords; ++i) {
                const auto* kw = kwf->keywords ? kwf->keywords[i] : nullptr;
                if (!kw) continue;
                const char* ed = kw->formEditorID.c_str();
                if (!ed || !std::strstr(ed, "Material")) continue;
                if (std::strstr(ed, "Daedric"))    return "daedric";
                if (std::strstr(ed, "Dragonbone") || std::strstr(ed, "DragonPlate") ||
                    std::strstr(ed, "Dragonplate") || std::strstr(ed, "Dragonscale") ||
                    std::strstr(ed, "DragonScale")) return "dragonbone";
                if (std::strstr(ed, "Stalhrim"))   return "stalhrim";
                if (std::strstr(ed, "Nordic"))     return "nordic";
                if (std::strstr(ed, "Ebony"))      return "ebony";
                if (std::strstr(ed, "Glass"))      return "glass";
                if (std::strstr(ed, "Elven"))      return "elven";
                if (std::strstr(ed, "Dwarven"))    return "dwarven";
                if (std::strstr(ed, "Orc"))        return "orcish";
                if (std::strstr(ed, "Steel"))      return "steel";   // SteelPlate too
                if (std::strstr(ed, "Imperial"))   return "steel";
                if (std::strstr(ed, "Iron"))       return "iron";
                if (std::strstr(ed, "Draugr"))     return "iron";
                if (std::strstr(ed, "Silver"))     return "silver";
                if (std::strstr(ed, "Wood"))       return "wood";
                if (std::strstr(ed, "Scaled"))     return "leather";
                if (std::strstr(ed, "Leather"))    return "leather";
                if (std::strstr(ed, "Hide") || std::strstr(ed, "Fur")) return "fur";
            }
            return nullptr;
        }

        // Unique items (daedric artifacts + main quest / faction signatures).
        // Local FormIDs in Skyrim.esm; unresolvable entries silently drop out.
        struct UniqueDef { std::uint32_t localID; const char* key; };
        constexpr UniqueDef kUniques[] = {
            { 0x0002AC6F, "unq_wabbajack" },
            { 0x000240D2, "unq_mehrunes_razor" },
            { 0x00063B27, "unq_azura_star" },
            { 0x00063B29, "unq_azura_star" },      // Black Star shares the icon
            { 0x000233E3, "unq_molag_mace" },
            { 0x0002ACD2, "unq_volendrung" },
            { 0x000D2846, "unq_clavicus_masque" },
            { 0x00043E1E, "unq_rueful_axe" },
            { 0x0002AC61, "unq_saviors_hide" },
            { 0x0001A332, "unq_oghma_infinium" },
            { 0x0004A38F, "unq_ebony_blade" },
            { 0x0003A070, "unq_skeleton_key" },
            { 0x00045F96, "unq_spellbreaker" },
            { 0x0002C37B, "unq_namira_ring" },
            { 0x0001CB36, "unq_sanguine_rose" },
            { 0x0004E4EE, "unq_dawnbreaker" },
            { 0x0002D513, "unq_elder_scroll" },
            { 0x0003A3DD, "unq_horn_jurgen" },
            { 0x000956B5, "unq_wuuthrad" },
            { 0x0009CCDC, "unq_blade_of_woe" },
            { 0x000F8313, "unq_chillrend" },
            { 0x000C4A57, "unq_eye_magnus" },
            { 0x00035369, "unq_staff_magnus" },
            { 0x000F71D0, "unq_dragonbane" },
        };

        [[nodiscard]] const std::unordered_map<RE::FormID, const char*>& UniqueMap()
        {
            static std::unordered_map<RE::FormID, const char*> map = [] {
                std::unordered_map<RE::FormID, const char*> m;
                auto* dh = RE::TESDataHandler::GetSingleton();
                if (dh) {
                    for (const auto& u : kUniques) {
                        if (const auto* f = dh->LookupForm(u.localID, "Skyrim.esm")) {
                            m[f->GetFormID()] = u.key;
                        }
                    }
                }
                return m;
            }();
            return map;
        }

        struct Resolved
        {
            const char* base = nullptr;
            std::string variant;   // "" = neutral only
        };

        [[nodiscard]] Resolved ResolveWeapon(RE::TESObjectWEAP* a_weap)
        {
            const char* base = nullptr;
            switch (a_weap->GetWeaponType()) {
            case RE::WEAPON_TYPE::kOneHandDagger: base = "wpn_dagger"; break;
            case RE::WEAPON_TYPE::kOneHandSword:  base = "wpn_sword"; break;
            case RE::WEAPON_TYPE::kOneHandAxe:    base = "wpn_waraxe"; break;
            case RE::WEAPON_TYPE::kOneHandMace:   base = "wpn_mace"; break;
            case RE::WEAPON_TYPE::kTwoHandSword:  base = "wpn_greatsword"; break;
            case RE::WEAPON_TYPE::kStaff:         return { "wpn_staff", "" };
            case RE::WEAPON_TYPE::kTwoHandAxe:
                base = a_weap->HasKeywordString("WeapTypeWarhammer")
                           ? "wpn_warhammer" : "wpn_battleaxe";
                break;
            case RE::WEAPON_TYPE::kBow:           base = "wpn_bow"; break;
            case RE::WEAPON_TYPE::kCrossbow:      base = "wpn_crossbow"; break;
            default:                              base = "wpn_sword"; break;
            }
            const char* mat = MaterialOf(a_weap);
            return { base, mat ? mat : "" };
        }

        [[nodiscard]] Resolved ResolveArmor(RE::TESObjectARMO* a_armo)
        {
            using S = RE::BGSBipedObjectForm::BipedObjectSlot;
            using A = RE::BGSBipedObjectForm::ArmorType;
            if (a_armo->HasPartOf(S::kAmulet)) return { "arm_necklace", "" };
            if (a_armo->HasPartOf(S::kRing))   return { "arm_ring", "" };
            if (a_armo->HasPartOf(S::kCirclet) && !a_armo->HasPartOf(S::kHead) &&
                !a_armo->HasPartOf(S::kHair)) {
                return { "arm_circlet", "" };
            }

            const A    type = a_armo->GetArmorType();
            const bool heavy = type == A::kHeavyArmor;
            const char* mat = MaterialOf(a_armo);
            const std::string var = mat ? mat : "";
            if (a_armo->HasPartOf(S::kShield)) return { "arm_shield", var };
            if (a_armo->HasPartOf(S::kBody)) {
                if (type == A::kClothing) {
                    return { Has(LowerModel(a_armo), "robe") ? "arm_robe" : "arm_clothes", "" };
                }
                return { heavy ? "arm_cuirass_h" : "arm_cuirass_l", var };
            }
            if (a_armo->HasPartOf(S::kHands)) {
                return { heavy ? "arm_gauntlets_h" : "arm_gauntlets_l", var };
            }
            if (a_armo->HasPartOf(S::kFeet)) {
                return { heavy ? "arm_boots_h" : "arm_boots_l", var };
            }
            return { heavy ? "arm_helmet_h" : "arm_helmet_l", var };
        }

        [[nodiscard]] Resolved ResolveAlchemy(RE::AlchemyItem* a_alch)
        {
            if (a_alch->IsPoison()) return { "con_poison", "" };
            if (a_alch->IsFood()) {
                const std::string m = LowerModel(a_alch);
                if (Has(m, "bread"))  return { "con_bread", "" };
                if (Has(m, "cheese")) return { "con_cheese", "" };
                if (Has(m, "meat") || Has(m, "beef") || Has(m, "venison") ||
                    Has(m, "salmon") || Has(m, "chicken") || Has(m, "ham")) {
                    return { "con_meat", "" };
                }
                if (Has(m, "wine") || Has(m, "mead") || Has(m, "bottle") ||
                    Has(m, "flagon") || Has(m, "ale") || Has(m, "sujamma")) {
                    return { "con_drink", "" };
                }
                return { "con_stew", "" };
            }
            // potion: variant from the costliest effect's primary actor value
            const char* var = "";
            if (const auto* eff = a_alch->GetCostliestEffectItem();
                eff && eff->baseEffect) {
                switch (eff->baseEffect->data.primaryAV) {
                case RE::ActorValue::kHealth:  var = "health"; break;
                case RE::ActorValue::kMagicka: var = "magicka"; break;
                case RE::ActorValue::kStamina: var = "stamina"; break;
                default:                       var = "fortify"; break;
                }
            }
            return { "con_potion", var };
        }

        [[nodiscard]] Resolved ResolveMisc(RE::TESObjectMISC* a_misc)
        {
            const auto id = a_misc->GetFormID();
            if (id == 0x0000000F) return { "msc_gold3", "" };
            if (id == 0x0000000A) return { "msc_lockpick", "" };

            const std::string m = LowerModel(a_misc);
            if (Has(m, "dragonclaw")) return { "unq_dragon_claw", "" };
            if (a_misc->HasKeywordString("VendorItemOreIngot")) {
                return { Has(m, "ingot") ? "msc_ingot" : "msc_ore",
                         MaterialOf(a_misc) ? MaterialOf(a_misc) : "" };
            }
            if (a_misc->HasKeywordString("VendorItemAnimalHide")) {
                return { Has(m, "strip") ? "msc_strip" : "msc_hide", "" };
            }
            if (a_misc->HasKeywordString("VendorItemGem")) {
                return { (Has(m, "rough") || Has(m, "uncut")) ? "msc_gem_rough"
                                                              : "msc_gem_cut", "" };
            }
            if (Has(m, "dragon") && (Has(m, "bone") || Has(m, "scale"))) {
                return { "msc_dragonbone", "" };
            }
            if (Has(m, "skull") || Has(m, "bone") || Has(m, "rib")) return { "msc_bone", "" };
            if (Has(m, "goblet") || Has(m, "silverware") || Has(m, "platter") ||
                Has(m, "jug") || Has(m, "urn") || Has(m, "candlestick")) {
                return { "msc_silverware", "" };
            }
            if (Has(m, "bowl") || Has(m, "tankard") || Has(m, "cup") ||
                Has(m, "kettle") || Has(m, "plate")) {
                return { "msc_bowl", "" };
            }
            if (Has(m, "strip"))     return { "msc_strip", "" };
            if (Has(m, "coinpurse")) return { "msc_coinpouch", "" };
            if (Has(m, "lockpick"))  return { "msc_lockpick", "" };
            if (Has(m, "ingot"))     return { "msc_ingot", "" };
            if (Has(m, "ore"))       return { "msc_ore", "" };
            return { "msc_tool", "" };
        }

        [[nodiscard]] Resolved Resolve(RE::TESBoundObject* a_obj)
        {
            // unique artifacts trump category matching
            const auto& uniq = UniqueMap();
            if (const auto it = uniq.find(a_obj->GetFormID()); it != uniq.end()) {
                return { it->second, "" };
            }

            if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) return ResolveWeapon(weap);
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) return ResolveArmor(armo);
            if (auto* ammo = a_obj->As<RE::TESAmmo>()) {
                const char* mat = MaterialOf(ammo);
                return { ammo->IsBolt() ? "amm_bolt" : "amm_arrow", mat ? mat : "" };
            }
            if (auto* gem = a_obj->As<RE::TESSoulGem>()) {
                if (gem->CanHoldNPCSoul()) return { "knw_soulgem", "black" };
                switch (gem->GetMaximumCapacity()) {
                case RE::SOUL_LEVEL::kPetty:   return { "knw_soulgem", "petty" };
                case RE::SOUL_LEVEL::kLesser:  return { "knw_soulgem", "lesser" };
                case RE::SOUL_LEVEL::kCommon:  return { "knw_soulgem", "common" };
                case RE::SOUL_LEVEL::kGreater: return { "knw_soulgem", "greater" };
                default:                       return { "knw_soulgem", "grand" };
                }
            }
            if (auto* alch = a_obj->As<RE::AlchemyItem>()) return ResolveAlchemy(alch);
            if (auto* book = a_obj->As<RE::TESObjectBOOK>()) {
                const std::string m = LowerModel(book);
                if (Has(m, "note") || Has(m, "letter")) return { "knw_letter", "" };
                return { "knw_book", "" };
            }
            if (a_obj->Is(RE::FormType::Scroll))     return { "con_scroll", "" };
            if (a_obj->Is(RE::FormType::KeyMaster))  return { "msc_key", "" };
            if (a_obj->Is(RE::FormType::Light))      return { "wpn_torch", "" };
            if (a_obj->Is(RE::FormType::Ingredient)) {
                const std::string m = LowerModel(a_obj);
                if (Has(m, "plant") || Has(m, "flower") || Has(m, "mushroom") ||
                    Has(m, "pod") || Has(m, "root")) {
                    return { "con_ingr_plant", "" };
                }
                if (Has(m, "ore") || Has(m, "crystal") || Has(m, "salt") ||
                    Has(m, "pearl") || Has(m, "gem")) {
                    return { "con_ingr_mineral", "" };
                }
                return { "con_ingr_monster", "" };
            }
            if (auto* misc = a_obj->As<RE::TESObjectMISC>()) return ResolveMisc(misc);
            return {};
        }

        [[nodiscard]] const IconCache::Icon* Lookup(const std::string& a_key)
        {
            auto it = g_cache.find(a_key);
            if (it == g_cache.end()) {
                IconCache::Icon icon{};
                IconCache::LoadFicTexture(std::string(kDir) + a_key + ".fic",
                    icon, 0, /*makeGlow*/ true);
                it = g_cache.emplace(a_key, icon).first;
                if (icon.srv) g_anyLoaded = true;
            }
            return it->second.srv ? &it->second : nullptr;
        }
    }

    const IconCache::Icon* Get(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return nullptr;
        const Resolved r = Resolve(a_obj);
        if (!r.base) return nullptr;
        if (!r.variant.empty()) {
            if (const auto* icon = Lookup(std::string(r.base) + "@" + r.variant)) {
                return icon;
            }
        }
        return Lookup(r.base);
    }

    bool AssetsSeen() { return g_anyLoaded; }
}
