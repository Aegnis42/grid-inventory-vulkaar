#include "ui/Lang.h"

namespace FUI::Lang
{
    namespace
    {
        int g_lang = 0;   // 0 EN (default), 1 KO, 2 ZH, 3 JA

        constexpr int N = static_cast<int>(Str::Count_);

        // all four tables generated from the ONE list in Lang.h — positional
        // desync is a compile error now, not a runtime nullptr (B8)
#define FUI_LANG_ROW_EN(name, en, ko, zh, ja) en,
#define FUI_LANG_ROW_KO(name, en, ko, zh, ja) ko,
#define FUI_LANG_ROW_ZH(name, en, ko, zh, ja) zh,
#define FUI_LANG_ROW_JA(name, en, ko, zh, ja) ja,
        const char* kEN[] = { FUI_LANG_STRINGS(FUI_LANG_ROW_EN) };
        const char* kKO[] = { FUI_LANG_STRINGS(FUI_LANG_ROW_KO) };
        const char* kZH[] = { FUI_LANG_STRINGS(FUI_LANG_ROW_ZH) };
        const char* kJA[] = { FUI_LANG_STRINGS(FUI_LANG_ROW_JA) };
#undef FUI_LANG_ROW_EN
#undef FUI_LANG_ROW_KO
#undef FUI_LANG_ROW_ZH
#undef FUI_LANG_ROW_JA

        const char** kTables[4] = { kEN, kKO, kZH, kJA };
    }

    int Get() { return g_lang; }

    void SetLang(int a_lang)
    {
        g_lang = (std::max)(0, (std::min)(3, a_lang));
    }

    static_assert(std::size(kEN) == static_cast<size_t>(N), "kEN out of sync with Str");
    static_assert(std::size(kKO) == static_cast<size_t>(N), "kKO out of sync with Str");
    static_assert(std::size(kZH) == static_cast<size_t>(N), "kZH out of sync with Str");
    static_assert(std::size(kJA) == static_cast<size_t>(N), "kJA out of sync with Str");

    const char* T(Str a_key)
    {
        const int i = static_cast<int>(a_key);
        if (i < 0 || i >= N) return "?";
        const char* s = kTables[g_lang][i];
        return s ? s : "?";   // never hand ImGui a null format string
    }
}
