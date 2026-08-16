#include "ui/Fallback.h"

#include "game/GoldCoins.h"
#include "ui/Grid.h"

#include <d3d11.h>   // ReloadAssets releases the drawings it is holding

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

        // Substring match with a WORD BOUNDARY on the needle. This whole file
        // keeps getting bitten by short words hiding inside longer ones —
        // ox/Fox, pot/potion, ale/whale, moth/mammoth, bee/beef, crab/crabapple
        // — and each was patched with its own "&& !Has(...)" exception. For a
        // needle short enough that the exceptions can't be enumerated, ask for
        // a boundary instead: "wig" then matches wig.nif and wigs\ but not
        // wigwm, without anyone having to predict "wigwm" in advance.
        [[nodiscard]] bool HasToken(const std::string& a_hay, const char* a_needle)
        {
            const size_t n = std::strlen(a_needle);
            if (!n) return false;
            for (size_t p = a_hay.find(a_needle); p != std::string::npos;
                 p = a_hay.find(a_needle, p + n)) {
                const auto alpha = [](char c) {
                    return std::isalpha(static_cast<unsigned char>(c)) != 0;
                };
                const bool before = p == 0 || !alpha(a_hay[p - 1]);
                const bool after = p + n >= a_hay.size() || !alpha(a_hay[p + n]);
                if (before && after) return true;
            }
            return false;
        }

        // ★ARMO does NOT inherit TESModel. An armour's paths live in
        // TESBipedModelForm::worldModels[male|female] (the ground model) and,
        // failing that, in each ARMA's bipedModels (the worn mesh) — so
        // skyrim_cast<TESModel*> on one returns NULL and LowerModel() hands
        // back "". Measured, not guessed: the export showed mesh empty for
        // 3494 of 3494 armours. Every string rule in ResolveArmor was therefore
        // reading an empty string — "robe" never matched (arm_robe drew ZERO
        // items in the whole load order) and every modded accessory fell past
        // its own rule onto the generic trinket.
        [[nodiscard]] std::string ArmorModel(RE::TESObjectARMO* a_armo)
        {
            auto lower = [](const char* a_p) {
                std::string s = a_p ? a_p : "";
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            for (const auto& wm : a_armo->worldModels) {
                if (std::string s = lower(wm.GetModel()); !s.empty()) return s;
            }
            for (auto* arma : a_armo->armorAddons) {
                if (!arma) continue;
                for (const auto& bm : arma->bipedModels) {
                    if (std::string s = lower(bm.GetModel()); !s.empty()) return s;
                }
            }
            return {};
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

        // Ore chunks and ingots carry NO *Material* keyword — MaterialOf always
        // came back null for them, so every ore drew the same neutral vein even
        // though 16 tinted variants were being baked. The mesh name is the one
        // thing that says what they are. Order matters: "quicksilver" contains
        // "silver".
        [[nodiscard]] const char* OreMaterialOfMesh(const std::string& a_mesh)
        {
            if (Has(a_mesh, "ebony"))       return "ebony";
            if (Has(a_mesh, "stalhrim"))    return "stalhrim";
            if (Has(a_mesh, "malachite"))   return "glass";     // glass is smelted from it
            if (Has(a_mesh, "moonstone"))   return "elven";
            if (Has(a_mesh, "orichalc"))    return "orcish";
            if (Has(a_mesh, "dwarven") || Has(a_mesh, "dwemer")) return "dwarven";
            if (Has(a_mesh, "quicksilver")) return "silver";
            if (Has(a_mesh, "silver"))      return "silver";
            if (Has(a_mesh, "gold"))        return "gold";
            if (Has(a_mesh, "corundum"))    return "steel";
            if (Has(a_mesh, "steel"))       return "steel";
            if (Has(a_mesh, "iron"))        return "iron";
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
            // ★A pickaxe is a one-hand AXE to the engine, so all six drew as
            // war axes while the drawn set carried an actual pickaxe that
            // nothing used. In a bag a mining tool reads as a tool, not as a
            // weapon. Match the whole word: "pick" alone lives inside
            // lockpick. The woodcutter's axe is NOT included — it looks like
            // an axe, and drawing it as a pickaxe would be the same error
            // pointing the other way.
            if (Has(LowerModel(a_weap), "pickaxe")) return { "msc_tool", "" };

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
            if (Grid::IsRing(a_armo))          return { "arm_ring", "" };
            if (a_armo->HasPartOf(S::kCirclet) && !a_armo->HasPartOf(S::kHead) &&
                !a_armo->HasPartOf(S::kHair)) {
                return { "arm_circlet", "" };
            }

            const A    type = a_armo->GetArmorType();
            const bool heavy = type == A::kHeavyArmor;
            const char* mat = MaterialOf(a_armo);
            const std::string var = mat ? mat : "";
            const std::string m = ArmorModel(a_armo);
            // A backpack is a backpack whatever slot it claims. Fuse00's hangs
            // off the HAIR slot, so it came out a helmet — and then, the moment
            // wigs got a rule, a wig: "wigwm" contains "wig".
            if (Has(m, "backpack") || Has(m, "satchel") || Has(m, "knapsack")) {
                return { "msc_backpack", "" };
            }
            if (a_armo->HasPartOf(S::kShield)) return { "arm_shield", var };
            if (a_armo->HasPartOf(S::kBody)) {
                if (type == A::kClothing) {
                    if (Has(m, "cloak") || Has(m, "cape")) return { "arm_cloak", "" };
                    return { Has(m, "robe") ? "arm_robe" : "arm_clothes", "" };
                }
                return { heavy ? "arm_cuirass_h" : "arm_cuirass_l", var };
            }
            if (a_armo->HasPartOf(S::kHands)) {
                return { heavy ? "arm_gauntlets_h" : "arm_gauntlets_l", var };
            }
            if (a_armo->HasPartOf(S::kFeet)) {
                return { heavy ? "arm_boots_h" : "arm_boots_l", var };
            }
            // ★The head must be ASKED FOR. This used to be the catch-all
            // return, so every armour sitting on a slot we don't name came out
            // a helmet -- and that is exactly what the custom accessory slots
            // mods add (cloaks, backpacks, belts, earrings, shoulders) are.
            // Reported in the wild: "every accessory shows as a helmet".
            if (a_armo->HasPartOf(S::kHead) || a_armo->HasPartOf(S::kHair) ||
                a_armo->HasPartOf(S::kLongHair)) {
                // A hood and a wig sit on the same slots a helmet does, so the
                // slot alone cannot tell them apart — the path can.
                if (Has(m, "hood") || Has(m, "cowl")) return { "arm_hood", "" };
                if (HasToken(m, "wig") || HasToken(m, "wigs") ||
                    Has(m, "hairdo") || Has(m, "\\hair\\")) {
                    return { "arm_wig", "" };
                }
                return { heavy ? "arm_helmet_h" : "arm_helmet_l", var };
            }

            // Unnamed slot = a modded accessory. No convention says which slot
            // number means what, so read the mesh path -- the one thing the
            // author has to name. Order matters: "earring" contains "ring".
            if (a_armo->HasPartOf(S::kEars) || Has(m, "earring") || Has(m, "piercing")) {
                return { "arm_necklace", "" };
            }
            // Armour PIECES worn on a custom slot. These are not accessories:
            // the mesh names the body part, so the piece can wear that part's
            // icon. Twenty-two Dread Sovereign items were drawing as jewellery
            // off a file plainly called "cuirassheavygnd.nif".
            if (Has(m, "cuirass") || Has(m, "torso") || Has(m, "breast")) {
                return { heavy ? "arm_cuirass_h" : "arm_cuirass_l", var };
            }
            if (Has(m, "vambrace") || Has(m, "wristguard") ||
                Has(m, "elbowguard") || Has(m, "ellbowguard") || Has(m, "bracer")) {
                return { heavy ? "arm_gauntlets_h" : "arm_gauntlets_l", var };
            }
            if (Has(m, "greave") || Has(m, "cuisse") || Has(m, "legsarmor")) {
                return { heavy ? "arm_boots_h" : "arm_boots_l", var };
            }
            if (Has(m, "belt") || Has(m, "sash") || Has(m, "bandolier") ||
                Has(m, "strap")) {
                return { "msc_strip", "" };
            }
            if (Has(m, "necklace") || Has(m, "amulet") || Has(m, "pendant") ||
                Has(m, "torc")) {
                return { "arm_necklace", "" };
            }
            if (Has(m, "ring") || Has(m, "bracelet") || Has(m, "bangle")) {
                return { "arm_ring", "" };
            }
            if (Has(m, "circlet") || Has(m, "crown") || Has(m, "diadem") ||
                Has(m, "tiara")) {
                return { "arm_circlet", "" };
            }
            if (Has(m, "hood") || Has(m, "cowl")) return { "arm_hood", "" };
            if (HasToken(m, "wig") || HasToken(m, "wigs") || Has(m, "hairdo")) {
                return { "arm_wig", "" };
            }
            // Cloaks get their own icon now: they hang off custom slots in
            // every cloak mod, and a cloak is not a robe.
            if (Has(m, "cloak") || Has(m, "cape") || Has(m, "mantle")) {
                return { "arm_cloak", "" };
            }
            if (Has(m, "robe") || Has(m, "shawl")) return { "arm_robe", "" };
            // Named garments stay garments...
            if (Has(m, "dress") || Has(m, "outfit") || Has(m, "tunic") ||
                Has(m, "shirt") || Has(m, "skirt") || Has(m, "pants") ||
                Has(m, "trousers") || Has(m, "vest") || Has(m, "jacket") ||
                Has(m, "coat") || Has(m, "clothes")) {
                return { "arm_clothes", "" };
            }
            // ...and everything else lands on the generic trinket. A thing worn
            // on a slot the game has no name for is an ACCESSORY far more often
            // than it is a garment — and the icon says "some jewellery" rather
            // than claiming a kind it might not be.
            return { "arm_trinket", "" };
        }

        [[nodiscard]] Resolved ResolveAlchemy(RE::AlchemyItem* a_alch)
        {
            if (a_alch->IsPoison()) return { "con_poison", "" };
            if (a_alch->IsFood()) {
                const std::string m = LowerModel(a_alch);
                // Fish and shellfish FIRST. "salmon" and "clam" are in the meat
                // rule below, so eight records — salmon steaks, clam meat, crab
                // meat, the CC tuna — were landing on the red meat icon.
                // "crabapple" is a fruit, and the one word that would hide here.
                if (Has(m, "fish") || Has(m, "salmon") || Has(m, "clam") ||
                    Has(m, "shrimp") || Has(m, "oyster") || Has(m, "lobster") ||
                    (Has(m, "crab") && !Has(m, "crabapple"))) {
                    return { "con_fish", "" };
                }
                if (Has(m, "bread"))  return { "con_bread", "" };
                if (Has(m, "cheese")) return { "con_cheese", "" };
                if (Has(m, "meat") || Has(m, "beef") || Has(m, "venison") ||
                    Has(m, "chicken") || Has(m, "ham")) {
                    return { "con_meat", "" };
                }
                // Baked and sweet. Ahead of produce so an apple pie stays a pie.
                if (Has(m, "pie") || Has(m, "tart") || Has(m, "dumpling") ||
                    Has(m, "sweetroll") || Has(m, "taffy") || Has(m, "cream") ||
                    Has(m, "honey") || Has(m, "cake") || Has(m, "pastry") ||
                    Has(m, "pudding") || Has(m, "candy") || Has(m, "biscuit")) {
                    return { "con_pie", "" };
                }
                // Fruit and vegetables. The Plants folder is safe here because
                // this branch only sees FOOD — harvestable ingredients are a
                // different form type and never reach it.
                if (Has(m, "apple") || Has(m, "cabbage") || Has(m, "potato") ||
                    Has(m, "leek") || Has(m, "tomato") || Has(m, "carrot") ||
                    Has(m, "gourd") || Has(m, "yam") || Has(m, "onion") ||
                    Has(m, "lettuce") || Has(m, "pumpkin") || Has(m, "melon") ||
                    Has(m, "grape") || Has(m, "\\plants\\")) {
                    return { "con_produce", "" };
                }
                // Drink. "ale" used to be in this list and never matched once —
                // Skyrim names none of its drinks that — while being short
                // enough to hide inside scale/whale/pale. Dropped for the
                // specific names the game and its DLC actually use.
                if (Has(m, "wine") || Has(m, "mead") || Has(m, "bottle") ||
                    Has(m, "flagon") || Has(m, "liquor") || Has(m, "brew") ||
                    Has(m, "sujamma") || Has(m, "matze") || Has(m, "shein") ||
                    Has(m, "skooma") || Has(m, "reserve") || Has(m, "milk") ||
                    Has(m, "tankard") || Has(m, "cider") || Has(m, "brandy")) {
                    return { "con_drink", "" };
                }
                return { "con_stew", "" };
            }
            // potion: variant from the costliest effect's primary actor value
            const char* var = "";
            if (const auto* eff = a_alch->GetCostliestEffectItem();
                eff && eff->baseEffect) {
                using AV = RE::ActorValue;
                switch (eff->baseEffect->data.primaryAV) {
                // The *RateMult values are regeneration potions — "Elixir of
                // Regeneration", "of Vigor", "of Lasting Potency". They were
                // measured sitting in the fortify purple: a health regen potion
                // is a health potion, so it wears that colour.
                case AV::kHealth:  case AV::kHealRate:    case AV::kHealRateMult:
                    var = "health"; break;
                case AV::kMagicka: case AV::kMagickaRate: case AV::kMagickaRateMult:
                    var = "magicka"; break;
                case AV::kStamina: case AV::kStaminaRate: case AV::kStaminaRateMult:
                    var = "stamina"; break;
                // All resistances share one colour. The palette has carried a
                // "resist" entry since this set was drawn and NOTHING ever
                // selected it — every resist potion was going to the fortify
                // purple along with everything else.
                case AV::kDamageResist:  case AV::kPoisonResist:
                case AV::kResistFire:    case AV::kResistShock:
                case AV::kResistFrost:   case AV::kResistMagic:
                case AV::kResistDisease:
                    var = "resist"; break;
                // Utility: the potions you dig through a full bag looking for.
                case AV::kInvisibility:  case AV::kNightEye:
                case AV::kWaterBreathing: case AV::kWaterWalking:
                case AV::kDetectLifeRange: case AV::kParalysis:
                case AV::kTelekinesis:
                    var = "utility"; break;
                default: var = "fortify"; break;
                }
            }
            return { "con_potion", var };
        }

        // Actor value names for the export only — the eighteen skills a book can
        // teach, plus the values a potion's costliest effect commonly names.
        // Anything else comes back null and prints as a raw number, which is
        // exactly the case worth seeing: whatever is still piling into the
        // fortify colour has no name here yet.
        [[nodiscard]] const char* AVName(RE::ActorValue a_av)
        {
            using AV = RE::ActorValue;
            switch (a_av) {
            case AV::kHealth:          return "Health";
            case AV::kMagicka:         return "Magicka";
            case AV::kStamina:         return "Stamina";
            case AV::kHealRate:        return "HealRate";
            case AV::kMagickaRate:     return "MagickaRate";
            case AV::kStaminaRate:     return "StaminaRate";
            case AV::kHealRateMult:    return "HealRateMult";
            case AV::kMagickaRateMult: return "MagickaRateMult";
            case AV::kStaminaRateMult: return "StaminaRateMult";
            // The eighteen skill "power modifier" values, which is what a
            // Fortify <Skill> potion actually names. Deliberately collapsed to
            // one label: for the only question the export asks — what is still
            // in the purple — "a skill fortify" IS the whole answer, and
            // eighteen separate strings would just bury it.
            case AV::kOneHandedPowerModifier:   case AV::kTwoHandedPowerModifier:
            case AV::kMarksmanPowerModifier:    case AV::kBlockPowerModifier:
            case AV::kSmithingPowerModifier:    case AV::kHeavyArmorPowerModifier:
            case AV::kLightArmorPowerModifier:  case AV::kPickpocketPowerModifier:
            case AV::kLockpickingPowerModifier: case AV::kSneakingPowerModifier:
            case AV::kAlchemyPowerModifier:     case AV::kSpeechcraftPowerModifier:
            case AV::kAlterationPowerModifier:  case AV::kConjurationPowerModifier:
            case AV::kDestructionPowerModifier: case AV::kIllusionPowerModifier:
            case AV::kRestorationPowerModifier: case AV::kEnchantingPowerModifier:
                return "FortifySkill";
            case AV::kDamageResist:    return "DamageResist";
            case AV::kPoisonResist:    return "PoisonResist";
            case AV::kResistFire:      return "ResistFire";
            case AV::kResistShock:     return "ResistShock";
            case AV::kResistFrost:     return "ResistFrost";
            case AV::kResistMagic:     return "ResistMagic";
            case AV::kResistDisease:   return "ResistDisease";
            case AV::kInvisibility:    return "Invisibility";
            case AV::kNightEye:        return "NightEye";
            case AV::kWaterBreathing:  return "WaterBreathing";
            case AV::kWaterWalking:    return "WaterWalking";
            case AV::kDetectLifeRange: return "DetectLife";
            case AV::kParalysis:       return "Paralysis";
            case AV::kTelekinesis:     return "Telekinesis";
            case AV::kCarryWeight:     return "CarryWeight";
            case AV::kSpeedMult:       return "SpeedMult";
            case AV::kCriticalChance:  return "CriticalChance";
            case AV::kMeleeDamage:     return "MeleeDamage";
            case AV::kUnarmedDamage:   return "UnarmedDamage";
            case AV::kBowSpeedBonus:   return "BowSpeed";
            case AV::kWeaponSpeedMult: return "WeaponSpeed";
            case AV::kShoutRecoveryMult: return "ShoutRecovery";
            case AV::kMovementNoiseMult: return "MovementNoise";
            case AV::kOneHanded:   return "OneHanded";
            case AV::kTwoHanded:   return "TwoHanded";
            case AV::kArchery:     return "Archery";
            case AV::kBlock:       return "Block";
            case AV::kSmithing:    return "Smithing";
            case AV::kHeavyArmor:  return "HeavyArmor";
            case AV::kLightArmor:  return "LightArmor";
            case AV::kPickpocket:  return "Pickpocket";
            case AV::kLockpicking: return "Lockpicking";
            case AV::kSneak:       return "Sneak";
            case AV::kAlchemy:     return "Alchemy";
            case AV::kSpeech:      return "Speech";
            case AV::kAlteration:  return "Alteration";
            case AV::kConjuration: return "Conjuration";
            case AV::kDestruction: return "Destruction";
            case AV::kIllusion:    return "Illusion";
            case AV::kRestoration: return "Restoration";
            case AV::kEnchanting:  return "Enchanting";
            default:               return nullptr;
            }
        }

        // The three guardian-stone lines, exactly as the game partitions its
        // eighteen skills. Not a taste call: the skill tree, the standing
        // stones and the level-up screen all use this same grouping, so a
        // skill book inherits it rather than inventing one.
        [[nodiscard]] const char* SkillLineOf(RE::ActorValue a_av)
        {
            using AV = RE::ActorValue;
            switch (a_av) {
            case AV::kOneHanded:  case AV::kTwoHanded:   case AV::kArchery:
            case AV::kBlock:      case AV::kHeavyArmor:  case AV::kSmithing:
                return "knw_skill_warrior";
            case AV::kLightArmor: case AV::kSneak:       case AV::kLockpicking:
            case AV::kPickpocket: case AV::kSpeech:      case AV::kAlchemy:
                return "knw_skill_thief";
            case AV::kAlteration: case AV::kConjuration: case AV::kDestruction:
            case AV::kIllusion:   case AV::kRestoration: case AV::kEnchanting:
                return "knw_skill_mage";
            default:
                return nullptr;   // modded book advancing something that is no skill
            }
        }

        // Which school a spell belongs to. GetAssociatedSkill() is the engine's
        // own answer, read off the spell's cached AV effect; walking the effect
        // list is only for modded spells where that cache never resolved.
        [[nodiscard]] RE::ActorValue SchoolOf(RE::SpellItem* a_spell)
        {
            auto av = a_spell->GetAssociatedSkill();
            if (av != RE::ActorValue::kNone) return av;
            for (const auto* eff : a_spell->effects) {
                if (eff && eff->baseEffect) {
                    const auto s = eff->baseEffect->GetMagickSkill();
                    if (s != RE::ActorValue::kNone) return s;
                }
            }
            return RE::ActorValue::kNone;
        }

        // Books answer for themselves — there is no mesh guessing in here.
        // A book either teaches a spell, or advances a skill, or is neither,
        // and the record states which; the spell in turn names its school.
        // Mesh text only gets a say on the last branch, and even there the
        // CK's own Note/Scroll type leads, because that is the same field
        // vanilla's UI uses to tell a letter from a tome.
        [[nodiscard]] Resolved ResolveBook(RE::TESObjectBOOK* a_book)
        {
            if (auto* spell = a_book->GetSpell()) {   // null unless kTeachesSpell
                switch (SchoolOf(spell)) {
                case RE::ActorValue::kDestruction: return { "knw_tome_destruction", "" };
                case RE::ActorValue::kRestoration: return { "knw_tome_restoration", "" };
                case RE::ActorValue::kAlteration:  return { "knw_tome_alteration", "" };
                case RE::ActorValue::kConjuration: return { "knw_tome_conjuration", "" };
                case RE::ActorValue::kIllusion:    return { "knw_tome_illusion", "" };
                default: break;   // a tome for something outside the five schools
                }
            }
            if (a_book->TeachesSkill()) {
                if (const char* line = SkillLineOf(a_book->GetSkill())) return { line, "" };
            }
            // Letters, by mesh — and that is not a shortcut, it is the only
            // signal there is. IsNoteScroll() reads the CK's Book/Note type
            // field, which was MEASURED across this whole load order: all
            // 1,134 books hold 0 (= Book/Tome), notes included. Skyrim simply
            // does not populate that field, so a rule built on it would never
            // fire. (The library's own comparison is broken too — kNoteScroll
            // is declared -1 against an unsigned byte — but that never gets a
            // chance to matter.) Do not "fix" this by calling IsNoteScroll().
            const std::string m = LowerModel(a_book);
            if (Has(m, "note") || Has(m, "letter")) return { "knw_letter", "" };
            return { "knw_book", "" };
        }

        [[nodiscard]] Resolved ResolveMisc(RE::TESObjectMISC* a_misc)
        {
            const auto id = a_misc->GetFormID();
            if (id == 0x0000000F) return { "msc_gold3", "" };
            if (id == 0x0000000A) return { "msc_lockpick", "" };

            const std::string m = LowerModel(a_misc);
            // Dragon-claw puzzle keys. The Amethyst Claw's two halves are the
            // same kind of door key but are named the other way round.
            if (Has(m, "dragonclaw") || Has(m, "clawamethyst")) {
                return { "unq_dragon_claw", "" };
            }
            // A critter in a jar. Ask for the JAR, not the folder: DLC01\
            // Critters\ also holds bone hawk claws, feathers and skulls, which
            // are remains — measured, they came out as three bugs in bottles.
            // Every jarred critter vanilla ships ends in "InJar".
            if (Has(m, "injar")) return { "msc_bug_jar", "" };
            // A MISC record whose mesh sits under Weapons\ is a broken piece
            // of one: Wuuthrad's eleven fragments, Mehrunes' Razor's four.
            // Before the gem keyword — the Razor's "gem" is a fragment too.
            if (Has(m, "weapons\\") || Has(m, "ysgramorsblade")) {
                return { "msc_weapon_shard", "" };
            }
            // Soul gems and loose gemstones that are MISC records rather than
            // SLGM — Malyn's black gem, soul gem shards, Olava's sapphire.
            if (Has(m, "soulgem")) return { "knw_soulgem", "" };
            if (Has(m, "gemstones\\")) return { "msc_gem_cut", "" };
            // The dungeon-puzzle family: paragons, moon crests, resonance
            // gems, attunement spheres, lexicon cubes, sigil stones. They look
            // like gems and carry gem keywords, but they are keys — filing
            // them as jewellery told the player the wrong thing about them.
            if (Has(m, "portalgem") || Has(m, "sigilstone") || Has(m, "controlgem") ||
                Has(m, "aetherium") || Has(m, "lexiconcube") || Has(m, "puzzlecube") ||
                Has(m, "controlcube") || Has(m, "atunement") || Has(m, "attunement") ||
                Has(m, "crest") || Has(m, "heartstone") || Has(m, "barrierkey") ||
                Has(m, "azurastar") || Has(m, "dawnbreakerpedestal") ||
                Has(m, "silvermold")) {
                return { "msc_arcanestone", "" };
            }
            if (a_misc->HasKeywordString("VendorItemOreIngot")) {
                const char* mat = MaterialOf(a_misc);
                if (!mat) mat = OreMaterialOfMesh(m);
                return { Has(m, "ingot") ? "msc_ingot" : "msc_ore", mat ? mat : "" };
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
            // ★Rings and amulets registered as MISC rather than ARMO — the
            // armour rules never see them, so they fell to the catch-all and
            // drew a pickaxe. The mesh folder names them outright.
            if (Has(m, "amuletsandrings")) {
                return { Has(m, "ring") ? "arm_ring" : "arm_necklace", "" };
            }
            // ★Clothing registered as MISC (the Hearthfire children's outfits,
            // bloody rags, linen wraps). Same story.
            if (Has(m, "tunic") || Has(m, "dress") || Has(m, "rags") ||
                Has(m, "linenwrap") || Has(m, "childrenclothes")) {
                return { "arm_clothes", "" };
            }
            // Instruments. Ahead of the bone rule: a war horn is blown, not
            // worn, while a DRINKING horn is tableware — so name both rather
            // than letting the generic "horn" decide.
            if (Has(m, "flute") || Has(m, "lute") || Has(m, "drum") ||
                Has(m, "warhorn") || Has(m, "windcallerhorn")) {
                return { "msc_instrument", "" };
            }
            // Body parts, horns and tusks. The old list only knew skull/bone/
            // rib, which missed the vampire remains, the skeleton horse, every
            // tusk and every horn — 20 items in one load order.
            if (Has(m, "skull") || Has(m, "bone") || Has(m, "rib") ||
                Has(m, "remains") || Has(m, "skeleton") || Has(m, "tusk") ||
                Has(m, "horn") || Has(m, "chitin") || Has(m, "fang") ||
                // a MISC record whose mesh comes out of Actors\ is a piece of
                // one — a hagraven's head, an albino spider's pod
                Has(m, "actors\\")) {
                return { "msc_bone", "" };
            }
            // Paper: maps, dispatches, schematics, rubbings, stone tablets.
            // A rolled scroll keeps its own icon; "mapflag" is a war-map
            // marker, not a document.
            if (Has(m, "scroll")) return { "con_scroll", "" };
            if (Has(m, "parchment") || Has(m, "tablet") || Has(m, "bookruined") ||
                Has(m, "documenttube") || Has(m, "rubbing") || Has(m, "schematic") ||
                (Has(m, "map") && !Has(m, "mapflag"))) {
                return { "msc_document", "" };
            }
            // Bottles. Before the container rules — a mead keg matches "keg"
            // and skooma's mesh lives under Clutter\Potions.
            if (Has(m, "wine") || Has(m, "bottle") || Has(m, "decanter") ||
                Has(m, "keg") || Has(m, "spigot") || Has(m, "skooma") ||
                Has(m, "honningbrew")) {
                return { "msc_bottle", "" };
            }
            // Alchemy glassware and brews left as MISC: the White Phial, a
            // quest mixture, Tolfdir's alembic. Poison names itself.
            if (Has(m, "poison")) return { "con_poison", "" };
            if (Has(m, "potions\\") || Has(m, "phial") || Has(m, "alembic")) {
                return { "con_potion", "" };
            }
            // Ingredients registered as MISC. Split by what they ARE, same
            // three shapes the real ingredient records use.
            if (Has(m, "salt")) return { "con_ingr_mineral", "" };
            if (Has(m, "briarheart")) return { "con_ingr_monster", "" };
            if (Has(m, "ingredients\\") || Has(m, "plants\\") ||
                Has(m, "sapling") || Has(m, "bark") || Has(m, "taproot") ||
                Has(m, "gleamsap")) {
                return { "con_ingr_plant", "" };
            }
            if (Has(m, "roast")) return { "con_meat", "" };
            // Statuettes, crowns and models — a figure on a base.
            if (Has(m, "statue") || Has(m, "crown") || Has(m, "grayfox") ||
                Has(m, "modelship") || Has(m, "modelboat") || Has(m, "doorknocker")) {
                return { "msc_statue", "" };
            }
            // Things you carry other things IN. Ahead of the tableware rules:
            // a honey pot matches "pot", and a canopic jar is a container
            // rather than a serving piece.
            if (Has(m, "basket") || Has(m, "bucket") || Has(m, "barrel") ||
                Has(m, "crate") || Has(m, "canopic") || Has(m, "sack") ||
                Has(m, "honeypot") || Has(m, "jar")) {
                return { "msc_basket", "" };
            }
            // Tableware. These rules existed but were written against words the
            // meshes don't use: the game ships BasicFork01, DweSpoon,
            // CastIronPot, WoodenLadle, Flagon01 — none of which contain
            // "goblet", "cup" or "bowl". 31 items sat in the catch-all.
            if (Has(m, "goblet") || Has(m, "silverware") || Has(m, "platter") ||
                Has(m, "jug") || Has(m, "urn") || Has(m, "candlestick") ||
                Has(m, "pitcher") || Has(m, "ewer") || Has(m, "chalice") ||
                Has(m, "flagon") || Has(m, "drinkinghorn")) {
                return { "msc_silverware", "" };
            }
            if (Has(m, "bowl") || Has(m, "tankard") || Has(m, "cup") ||
                Has(m, "kettle") || Has(m, "plate") || Has(m, "fork") ||
                Has(m, "spoon") || Has(m, "ladle") || Has(m, "pan") ||
                Has(m, "dining") || Has(m, "kitchen") || Has(m, "dweknife") ||
                // "pot" would swallow every potion — ask for the pot alone
                (Has(m, "pot") && !Has(m, "potion"))) {
                return { "msc_bowl", "" };
            }
            if (Has(m, "strip"))     return { "msc_strip", "" };
            if (Has(m, "coinpurse") || Has(m, "coinbag")) {
                return { "msc_coinpouch", "" };
            }
            if (Has(m, "lockpick"))  return { "msc_lockpick", "" };
            if (Has(m, "ingot") || Has(m, "ore")) {   // no vendor keyword: mesh only
                const char* mat = OreMaterialOfMesh(m);
                return { Has(m, "ingot") ? "msc_ingot" : "msc_ore", mat ? mat : "" };
            }
            // ---- the rest of the misc pile, split by trade (GI54) --------
            // Forge set. The pickaxe below keeps digging and cutting work.
            if (Has(m, "blacksmith")) return { "msc_smith", "" };
            // The grim sets, split by weight: torture is blunt mass, the
            // embalmer's kit is fine blades. That contrast survives 40px;
            // "two different sets of small knives" would not.
            if (Has(m, "torturetool")) return { "msc_torture", "" };
            if (Has(m, "brainpick") || Has(m, "scalpel") || Has(m, "scissor") ||
                Has(m, "ruinsknife")) {
                return { "msc_grimtool", "" };
            }
            // House-building stock.
            if (Has(m, "nail0") || Has(m, "hinge") || Has(m, "lock0") ||
                Has(m, "ironfittings") || Has(m, "claybrick") || Has(m, "glass0") ||
                Has(m, "straw")) {
                return { "msc_material", "" };
            }
            if (Has(m, "firewood") || Has(m, "coal")) return { "msc_fuel", "" };
            // Named exactly: "candlestick" is silverware and matched earlier,
            // so this must not ask for a bare "candle".
            if (Has(m, "candlelantern") || Has(m, "glazedcandles")) {
                return { "msc_lamp", "" };
            }
            if (Has(m, "quill") || Has(m, "inkwell")) return { "msc_quill", "" };
            if (Has(m, "mapflag")) return { "msc_flag", "" };
            if (Has(m, "doll")) return { "msc_doll", "" };
            if (Has(m, "pillow")) return { "msc_pillow", "" };
            if (Has(m, "extractor") || Has(m, "extract")) {
                return { "msc_apparatus", "" };
            }
            if (Has(m, "saw0"))       return { "msc_saw", "" };
            if (Has(m, "shovel"))     return { "msc_shovel", "" };
            if (Has(m, "broom"))      return { "msc_broom", "" };
            if (Has(m, "drawknife"))  return { "msc_drawknife", "" };
            if (Has(m, "iron0"))      return { "msc_iron", "" };   // "ironfittings" is above
            if (Has(m, "shipoar"))    return { "msc_oar", "" };
            if (Has(m, "snow"))       return { "msc_snow", "" };   // snow-elf pitcher matched earlier
            // The pickaxe is now purely the CATCH-ALL: nothing vanilla lands
            // here any more, so what shows up is a modded record no rule
            // names — which is exactly what a generic tool should mean.
            return { "msc_tool", "" };
        }

        [[nodiscard]] Resolved Resolve(RE::TESBoundObject* a_obj)
        {
            // ★Our OWN forms first. Every rule below is written against vanilla
            // records, so coins, purses, the pouch and the bags — which are
            // Grid Inventory's own items — matched nothing and fell through to
            // the catch-all: the player saw a pickaxe where their gold was.
            if (const char* gold = GoldCoins::FallbackIconKey(a_obj->GetFormID())) {
                return { gold, "" };
            }
            // A bag is not a form we can name — it is whatever the item defs
            // say holds a grid of its own, so ask them.
            if (Grid::ResolveDef(a_obj).bag != 0) return { "msc_backpack", "" };

            // unique artifacts trump category matching
            const auto& uniq = UniqueMap();
            if (const auto it = uniq.find(a_obj->GetFormID()); it != uniq.end()) {
                return { it->second, "" };
            }

            if (auto* weap = a_obj->As<RE::TESObjectWEAP>()) return ResolveWeapon(weap);
            if (auto* armo = a_obj->As<RE::TESObjectARMO>()) return ResolveArmor(armo);
            if (auto* ammo = a_obj->As<RE::TESAmmo>()) {
                const char* mat = MaterialOf(ammo);
                return { IsBoltAmmo(ammo) ? "amm_bolt" : "amm_arrow", mat ? mat : "" };
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
            if (auto* book = a_obj->As<RE::TESObjectBOOK>()) return ResolveBook(book);
            if (a_obj->Is(RE::FormType::Scroll))     return { "con_scroll", "" };
            if (a_obj->Is(RE::FormType::KeyMaster))  return { "msc_key", "" };
            if (a_obj->Is(RE::FormType::Light))      return { "wpn_torch", "" };
            if (a_obj->Is(RE::FormType::Ingredient)) {
                const std::string m = LowerModel(a_obj);
                // Insect parts. "\bee\" and NOT "bee" — BeefMeat is an
                // ingredient too, and three letters hide anywhere.
                // "moth" hides inside MAMMOTH — PowderedMammothTusk came out an
                // insect. Same shape as ox/Fox, pot/potion, ale/whale before it.
                if ((Has(m, "moth") && !Has(m, "mammoth")) ||
                    Has(m, "dragonfly") || Has(m, "firefly") ||
                    Has(m, "\\bee\\") || Has(m, "antennae") || Has(m, "chitin") ||
                    Has(m, "butterfly") || Has(m, "beetle") || Has(m, "torchbug") ||
                    Has(m, "wasp") || Has(m, "spideregg")) {
                    return { "con_ingr_insect", "" };
                }
                // Powders and dusts.
                if (Has(m, "bonemeal") || Has(m, "dust") || Has(m, "powder") ||
                    Has(m, "sugar") || Has(m, "spawnash")) {
                    return { "con_ingr_dust", "" };
                }
                // Plants — including the fungi that name themselves. Not one of
                // "tapinella", "pholiota" or "flamestalk" contains the word
                // "mushroom", so all three were filed as creature parts.
                if (Has(m, "plant") || Has(m, "flower") || Has(m, "mushroom") ||
                    Has(m, "pod") || Has(m, "root") || Has(m, "tapinella") ||
                    Has(m, "pholiota") || Has(m, "stalk") || Has(m, "amanita") ||
                    Has(m, "toadstool") || Has(m, "fungus") || Has(m, "shroom") ||
                    Has(m, "leaf") || Has(m, "seed") || Has(m, "berry")) {
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

        // ★PNG first, .fic second. The shipped set is PNG now — the folder IS
        // the documentation, so what sits in it has to be a file a player can
        // open, paint over and save. .fic is still read because an install
        // upgraded in place still has 1.0.4's files sitting there, and reading
        // a format we already parse costs nothing.
        [[nodiscard]] const IconCache::Icon* Lookup(const std::string& a_key)
        {
            auto it = g_cache.find(a_key);
            if (it == g_cache.end()) {
                IconCache::Icon icon{};
                const std::string stem = std::string(kDir) + a_key;
                if (!IconCache::LoadPngTexture(stem + ".png", icon, /*makeGlow*/ true)) {
                    IconCache::LoadFicTexture(stem + ".fic", icon, 0, /*makeGlow*/ true);
                }
                it = g_cache.emplace(a_key, icon).first;
                if (icon.srv) g_anyLoaded = true;
            }
            return it->second.srv ? &it->second : nullptr;
        }

        // main.cpp owns the "Plugin.esp|0xLocalID" spelling (it is what the
        // item ini is keyed by). Borrowing it through a hook rather than
        // re-deriving it here is the point: a second implementation would drift
        // and then two files would disagree about which item a line describes.
        std::function<std::string(RE::TESForm*)> g_formKey;

        // ...and the same string as a FILE NAME. '|' is illegal in one, so the
        // per-item drawing is "Skyrim.esm_0x012E49.png". The editor shows
        // exactly this text, so nobody has to know the rule.
        [[nodiscard]] std::string ItemFileStem(RE::TESBoundObject* a_obj)
        {
            if (!g_formKey) return {};
            std::string s = g_formKey(a_obj);
            for (auto& c : s) {
                if (c == '|') c = '_';
            }
            return s;
        }

        // per-item override: one drawing for one form, ahead of every category
        // rule. PNG only — this layer never shipped, so it has no legacy.
        [[nodiscard]] const IconCache::Icon* LookupItem(RE::TESBoundObject* a_obj)
        {
            const std::string stem = ItemFileStem(a_obj);
            if (stem.empty()) return nullptr;
            const std::string ck = "item/" + stem;
            auto it = g_cache.find(ck);
            if (it == g_cache.end()) {
                IconCache::Icon icon{};
                IconCache::LoadPngTexture(std::string(kDir) + ck + ".png",
                    icon, /*makeGlow*/ true);
                it = g_cache.emplace(ck, icon).first;
                if (icon.srv) {
                    g_anyLoaded = true;
                    // rare by nature (a player overrides a handful of items),
                    // so saying so costs no log volume and answers "did it
                    // actually pick my file up" without a second build
                    SKSE::log::info("[ICONS] per-item drawing: {}.png", stem);
                }
            }
            return it->second.srv ? &it->second : nullptr;
        }
    }

    void SetFormKeyResolver(std::function<std::string(RE::TESForm*)> a_fn)
    {
        g_formKey = std::move(a_fn);
    }

    std::string ItemFileName(RE::TESBoundObject* a_obj)
    {
        const std::string stem = ItemFileStem(a_obj);
        return stem.empty() ? std::string{} : stem + ".png";
    }

    std::string KeyFileName(RE::TESBoundObject* a_obj)
    {
        const Resolved r = Resolve(a_obj);
        if (!r.base) return {};
        std::string key = r.base;
        // name the file that WOULD be used — the variant if one exists on disk,
        // otherwise the base. Reporting a variant the player has no drawing for
        // would send them to author a file that never wins.
        if (!r.variant.empty() && Lookup(key + "@" + r.variant)) {
            key += "@" + r.variant;
        }
        return key + ".png";
    }

    void ReloadAssets()
    {
        size_t held = 0;
        for (auto& [k, ic] : g_cache) {
            if (ic.srv) { ic.srv->Release(); ++held; }
            if (ic.tex) ic.tex->Release();
            if (ic.glowSrv) ic.glowSrv->Release();
            if (ic.glowTex) ic.glowTex->Release();
        }
        const size_t entries = g_cache.size();
        g_cache.clear();
        g_anyLoaded = false;

        // ★Count what is ON DISK from inside the game, not from Explorer. Under
        // a mod manager the plugin sees a VIRTUAL Data folder assembled at
        // launch, and a file added to a mod folder afterwards may simply not be
        // in it — which looks exactly like a reload button that does nothing.
        // These two numbers separate the two cases in one line.
        std::error_code ec;
        auto countPng = [&](const std::string& a_dir) {
            size_t n = 0;
            for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
                if (e.path().extension() == ".png") ++n;
            }
            return n;
        };
        SKSE::log::info("[ICONS] drawn reload: dropped {} cached ({} drawings); "
                        "on disk {} category png, {} per-item png",
            entries, held, countPng(kDir), countPng(std::string(kDir) + "item/"));
    }

    const IconCache::Icon* Get(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return nullptr;
        if (const auto* mine = LookupItem(a_obj)) return mine;
        const Resolved r = Resolve(a_obj);
        if (!r.base) return nullptr;
        if (!r.variant.empty()) {
            if (const auto* icon = Lookup(std::string(r.base) + "@" + r.variant)) {
                return icon;
            }
        }
        return Lookup(r.base);
    }

    namespace
    {
        // Only keys that were actually adjusted live here — an untouched icon
        // costs nothing and never reaches the ini.
        std::map<std::string, KeyXform> g_xform;

        [[nodiscard]] float Clamp(float a_v, float a_lo, float a_hi)
        {
            return a_v < a_lo ? a_lo : (a_v > a_hi ? a_hi : a_v);
        }
    }

    KeyXform XformOf(const std::string& a_key)
    {
        const auto it = g_xform.find(a_key);
        return it != g_xform.end() ? it->second : KeyXform{};
    }

    void SetXform(const std::string& a_key, const KeyXform& a_x)
    {
        KeyXform v;
        v.scale = Clamp(a_x.scale, 0.2f, 4.0f);
        v.rot = Clamp(a_x.rot, -180.0f, 180.0f);
        v.x = Clamp(a_x.x, -1.0f, 1.0f);
        if (v.IsDefault()) {
            g_xform.erase(a_key);   // back to untouched = gone, not stored as 1/0/0
        } else {
            g_xform[a_key] = v;
        }
    }

    void ClearXforms() { g_xform.clear(); }

    const std::map<std::string, KeyXform>& Xforms() { return g_xform; }

    Drawn GetDrawn(RE::TESBoundObject* a_obj)
    {
        Drawn out;
        if (!a_obj) return out;
        // ★A per-item drawing gets NO key transform. That transform corrects
        // the framing of OUR category artwork; a player's own picture has its
        // own framing, and inheriting a nudge authored for a different image
        // would move it off-centre for reasons nothing on screen explains.
        // The item def's own fscale/frot/fx still applies (it wins at draw
        // time), so per-item tuning is still available — it is just not
        // inherited.
        if ((out.icon = LookupItem(a_obj))) return out;
        const Resolved r = Resolve(a_obj);
        if (!r.base) return out;
        if (!r.variant.empty()) {
            out.icon = Lookup(std::string(r.base) + "@" + r.variant);
        }
        if (!out.icon) out.icon = Lookup(r.base);
        // the base key owns the transform: a variant is the same drawing
        out.x = XformOf(r.base);
        return out;
    }

    bool AssetsSeen() { return g_anyLoaded; }

    // ★See the header for why TESAmmo::IsBolt() may never be called. Lives here
    // because the two ingredients -- LowerModel and the runtime-data read -- are
    // this file's, and one exported answer is what keeps the icon fallback and
    // the category assignment from drifting apart (they had: main.cpp was still
    // calling the broken API long after this file stopped).
    bool IsBoltAmmo(RE::TESAmmo* a_ammo)
    {
        if (!a_ammo) return false;
        const std::string m = LowerModel(a_ammo);
        if (Has(m, "bolt")) return true;
        if (Has(m, "arrow")) return false;
        return a_ammo->GetRuntimeData().data.flags.none(RE::AMMO_DATA::Flag::kNonBolt);
    }

    Assignment Classify(RE::TESBoundObject* a_obj)
    {
        if (!a_obj) return {};
        const Resolved r = Resolve(a_obj);
        return { r.base, r.variant };
    }

    namespace
    {
        // minimal JSON string escape — names and mesh paths carry quotes and
        // backslashes, and one unescaped byte makes the whole file unreadable
        void JsonStr(std::ofstream& a_out, const char* a_s)
        {
            a_out << '"';
            for (const char* p = a_s ? a_s : ""; *p; ++p) {
                const unsigned char c = static_cast<unsigned char>(*p);
                switch (c) {
                case '"':  a_out << "\\\""; break;
                case '\\': a_out << "\\\\"; break;
                case '\n': a_out << "\\n"; break;
                case '\r': a_out << "\\r"; break;
                case '\t': a_out << "\\t"; break;
                default:
                    if (c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04X", c);
                        a_out << b;
                    } else {
                        a_out << *p;   // UTF-8 passes through
                    }
                }
            }
            a_out << '"';
        }
    }

    size_t ExportAssignments()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return 0;
        constexpr const char* kOut = "Data/SKSE/Plugins/GridInventory_iconmap.json";
        std::ofstream out(kOut, std::ios::binary | std::ios::trunc);
        if (!out) {
            SKSE::log::error("[ICONMAP] cannot write {}", kOut);
            return 0;
        }
        out << "{\n  \"version\": 1,\n  \"items\": [\n";

        size_t n = 0;
        auto sweep = [&](const auto& a_arr, const char* a_type) {
            for (auto* form : a_arr) {
                auto* obj = form ? form->template As<RE::TESBoundObject>() : nullptr;
                if (!obj) continue;
                const char* nm = obj->GetName();
                if (!nm || !nm[0]) continue;          // nameless = system form
                if (!obj->GetPlayable()) continue;    // hidden scripting forms

                const Assignment a = Classify(obj);
                const auto* file = obj->GetFile(0);
                // Print what the RULES actually read. An armour's path is not
                // on TESModel, so the old cast printed "" for all 3,494 of them
                // and the table quietly agreed with the broken rules instead of
                // exposing them.
                std::string mesh;
                if (auto* armo = obj->template As<RE::TESObjectARMO>()) {
                    mesh = ArmorModel(armo);
                } else if (const auto* model = skyrim_cast<RE::TESModel*>(obj)) {
                    if (const char* p = model->GetModel()) mesh = p;
                }

                if (n) out << ",\n";
                out << "    {\"id\":\"";
                char idbuf[16];
                std::snprintf(idbuf, sizeof(idbuf), "0x%08X", obj->GetFormID());
                out << idbuf << "\",\"plugin\":";
                JsonStr(out, file ? file->GetFilename().data() : "");
                // GI63: the ITEM-DEF KEY, verbatim. IconStudio used to rebuild
                // it from "id" with (formID & 0xFFFFFF), which is right for a
                // full plugin and WRONG for an ESL — those keep only the low 12
                // bits, so every ccbgssse*.esl item failed to match and the tool
                // reported "no assignment" for them. Two places computing the
                // same key is the bug; ship the key instead.
                out << ",\"key\":";
                JsonStr(out, Grid::DefKeyOf(obj).c_str());
                out << ",\"name\":";
                JsonStr(out, nm);
                out << ",\"type\":\"" << a_type << "\",\"mesh\":";
                JsonStr(out, mesh.c_str());
                out << ",\"icon\":";
                JsonStr(out, a.base ? a.base : "");
                out << ",\"variant\":";
                JsonStr(out, a.variant.c_str());
                // GI54: books carry what the RECORD says they teach, so the
                // export can be checked against the icon rather than against
                // itself. Books only — 9,500 empty fields would just be noise.
                if (auto* bk = obj->template As<RE::TESObjectBOOK>()) {
                    char buf[64];
                    const char* teaches = "";
                    if (auto* sp = bk->GetSpell()) {
                        const auto av = SchoolOf(sp);
                        if (const char* n = AVName(av)) {
                            std::snprintf(buf, sizeof(buf), "spell:%s", n);
                        } else {
                            std::snprintf(buf, sizeof(buf), "spell:#%d", static_cast<int>(av));
                        }
                        teaches = buf;
                    } else if (bk->TeachesSkill()) {
                        const auto av = bk->GetSkill();
                        if (const char* n = AVName(av)) {
                            std::snprintf(buf, sizeof(buf), "skill:%s", n);
                        } else {
                            std::snprintf(buf, sizeof(buf), "skill:#%d", static_cast<int>(av));
                        }
                        teaches = buf;
                    }
                    out << ",\"teaches\":";
                    JsonStr(out, teaches);
                }
                // GI55: what the potion's costliest effect actually names, so
                // the colour can be checked against the cause instead of
                // against itself. Whatever is left in "fortify" is then a list
                // of real actor values, not a shrug.
                if (auto* al = obj->template As<RE::AlchemyItem>()) {
                    char abuf[32];
                    const char* avn = "";
                    if (const auto* eff = al->GetCostliestEffectItem();
                        eff && eff->baseEffect) {
                        const auto av = eff->baseEffect->data.primaryAV;
                        if (const char* n = AVName(av)) {
                            avn = n;
                        } else {
                            std::snprintf(abuf, sizeof(abuf), "#%d", static_cast<int>(av));
                            avn = abuf;
                        }
                    }
                    out << ",\"av\":";
                    JsonStr(out, avn);
                }
                // GI62: the raw DATA flag byte next to what IsBolt() concluded.
                // Every real bolt in this load order draws the ARROW icon, so
                // one of the two is lying — print both and let the table say
                // which, instead of guessing at the struct layout.
                out << "}";
                ++n;
            }
        };
        sweep(dh->GetFormArray<RE::TESObjectWEAP>(), "WEAP");
        sweep(dh->GetFormArray<RE::TESObjectARMO>(), "ARMO");
        sweep(dh->GetFormArray<RE::TESAmmo>(), "AMMO");
        sweep(dh->GetFormArray<RE::AlchemyItem>(), "ALCH");
        sweep(dh->GetFormArray<RE::IngredientItem>(), "INGR");
        sweep(dh->GetFormArray<RE::TESObjectBOOK>(), "BOOK");
        sweep(dh->GetFormArray<RE::TESObjectMISC>(), "MISC");
        sweep(dh->GetFormArray<RE::TESSoulGem>(), "SLGM");
        sweep(dh->GetFormArray<RE::TESKey>(), "KEYM");
        sweep(dh->GetFormArray<RE::ScrollItem>(), "SCRL");
        sweep(dh->GetFormArray<RE::TESObjectLIGH>(), "LIGH");

        out << "\n  ],\n  \"count\": " << n << "\n}\n";
        out.close();
        SKSE::log::info("[ICONMAP] {} items -> {}", n, kOut);
        return n;
    }
}
