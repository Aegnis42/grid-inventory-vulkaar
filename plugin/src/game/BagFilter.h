#pragma once

namespace FUI::BagFilter
{
    // ★Typed bags, phase 0 (PLAN_TYPED_BAGS.md §5).
    //
    // Which BAG may hold an item is a different question from which SIZING
    // CATEGORY the item follows, and they are kept apart on purpose: fold them
    // together and retuning a footprint silently moves the item to another bag.
    // The filter READS the category (it is already the correct, battle-tested
    // classifier) and adds only what it lacks.
    //
    // Rules live in Data/SKSE/Plugins/GridInventory_bagfilters.ini so a new
    // classification does not need a rebuild. Missing file = the built-in
    // defaults below, so the feature works out of the box.
    //
    //   alchemy = cat:ingredient
    //   hide    = cat:misc_hide, kwd:VendorItemAnimalPart
    //
    // cat: the string CategoryOf() returns.  kwd: a keyword EDITOR ID.

    // CategoryOf() lives in main.cpp's anonymous namespace; it reaches us the
    // same way the editor's hooks do rather than being duplicated here.
    void SetCategoryResolver(std::function<std::string(RE::TESBoundObject*)> a_fn);

    void Load();   // once, at plugin load

    // "" when nothing claims it. Cached per form — the keyword lookups behind
    // this are string work and must not run per frame.
    [[nodiscard]] const std::string& FilterOf(RE::TESBoundObject* a_obj);

    // Player-facing name for a filter ("ore" -> "ore and ingots"),
    // localized. Falls back to the id for filters added in the ini.
    [[nodiscard]] const char* DisplayName(const std::string& a_filter);

    // Which merchant stocks a bag of this filter: the keyword their
    // vendorSellBuyList must carry. null = no shop sells it.
    [[nodiscard]] RE::BGSKeyword* VendorKeyword(const std::string& a_filter);

    [[nodiscard]] int         Count();
    [[nodiscard]] const char* Id(int a_index);

    // Phase 0 observation. Nothing is routed or moved; the point is to find the
    // items a rule misses BEFORE any of this is load-bearing.
    //
    // The inventory dump reports what the player actually carries — useful, but
    // it can only test the filters that character happens to have items for.
    // The sweep walks every named item form in the load order, so a mod's ore
    // is checked whether or not anyone owns one. Run both.
    void DumpPlayerInventory();
    void DumpFormDatabase();
}
