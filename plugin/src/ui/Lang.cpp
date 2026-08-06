#include "ui/Lang.h"

#include <filesystem>
#include <fstream>

namespace FUI::Lang
{
    namespace
    {
        // 0 = compiled English; 1.. = files, in #order then id order
        int g_lang = 0;

        constexpr int N = static_cast<int>(Str::Count_);

        static constexpr const char* kPackDir = "Data/SKSE/Plugins/GridInventory_lang";
        static constexpr const char* kEnId    = "en";

        // The English table AND the key names come from the one list in
        // Lang.h, so the positional desync that used to nullptr-crash at
        // runtime is a compile error instead (B8).
#define FUI_LANG_ROW_EN(name, en) en,
#define FUI_LANG_ROW_ID(name, en) #name,
        const char* kEN[]    = { FUI_LANG_STRINGS(FUI_LANG_ROW_EN) };
        const char* kNames[] = { FUI_LANG_STRINGS(FUI_LANG_ROW_ID) };
#undef FUI_LANG_ROW_EN
#undef FUI_LANG_ROW_ID

        struct Pack
        {
            std::string id;
            std::string name;
            std::string font;
            std::string range;
            int         order = 100;
            std::vector<std::string> str;   // index-aligned; empty = untranslated
        };
        std::vector<Pack> g_packs;      // files that ADD a language, in list order
        bool              g_loaded = false;
        // ★A file called en.ini does not become a second English — it overlays
        // the compiled one, so it is held apart from the list rather than in
        // it. Overlay and not replace: a half-written en.ini then costs the
        // lines it got wrong, not the whole UI.
        Pack g_en;
        bool g_hasEn = false;

        [[nodiscard]] std::string Trim(std::string_view a_s)
        {
            const auto b = a_s.find_first_not_of(" \t\r\n");
            if (b == std::string_view::npos) return {};
            const auto e = a_s.find_last_not_of(" \t\r\n");
            return std::string(a_s.substr(b, e - b + 1));
        }

        [[nodiscard]] int IndexOfName(std::string_view a_name)
        {
            // linear over ~150 entries, once per line at load — a map would
            // cost more to build than the scans it saves
            for (int i = 0; i < N; ++i) {
                if (a_name == kNames[i]) return i;
            }
            return -1;
        }

        // "\n" in an ini is two characters; a translator writing a line break
        // means the escape, not a literal backslash-n.
        [[nodiscard]] std::string Unescape(std::string a_s)
        {
            std::string out;
            out.reserve(a_s.size());
            for (size_t i = 0; i < a_s.size(); ++i) {
                if (a_s[i] == '\\' && i + 1 < a_s.size() && a_s[i + 1] == 'n') {
                    out.push_back('\n');
                    ++i;
                } else {
                    out.push_back(a_s[i]);
                }
            }
            return out;
        }

        // printf conversion signature: the ordered specifier letters of a
        // format string ("Holds %s (%d)" -> "sd"). %% is skipped; a '*'
        // width/precision consumes an argument, so it joins the signature.
        [[nodiscard]] std::string FmtSig(const char* a_s)
        {
            constexpr std::string_view flags = "-+ #0123456789.";
            constexpr std::string_view sizes = "hljztL";
            std::string sig;
            for (const char* p = a_s; *p; ++p) {
                if (*p != '%') continue;
                ++p;
                if (!*p) break;
                if (*p == '%') continue;
                while (*p && (flags.find(*p) != std::string_view::npos || *p == '*')) {
                    if (*p == '*') sig.push_back('*');
                    ++p;
                }
                while (*p && sizes.find(*p) != std::string_view::npos) ++p;
                if (!*p) break;
                sig.push_back(*p);
            }
            return sig;
        }

        bool ParsePack(const std::filesystem::path& a_path, Pack& a_out)
        {
            std::ifstream in(a_path);
            if (!in) return false;
            a_out.id = a_path.stem().string();
            a_out.str.assign(static_cast<size_t>(N), std::string{});

            std::string line;
            bool        first = true;
            int         hits = 0, misses = 0;
            while (std::getline(in, line)) {
                if (first) {
                    first = false;
                    // a UTF-8 BOM would otherwise glue itself to the first
                    // directive and silently lose #name
                    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                        static_cast<unsigned char>(line[1]) == 0xBB &&
                        static_cast<unsigned char>(line[2]) == 0xBF) {
                        line.erase(0, 3);
                    }
                }
                const auto t = Trim(line);
                if (t.empty() || t[0] == ';') continue;
                const auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                const auto key = Trim(std::string_view(t).substr(0, eq));
                const auto val = Unescape(Trim(std::string_view(t).substr(eq + 1)));
                if (key.empty()) continue;
                if (key[0] == '#') {
                    const std::string_view d(key.c_str() + 1);
                    if (d == "name")  a_out.name = val;
                    else if (d == "font")  a_out.font = val;
                    else if (d == "range") a_out.range = val;
                    else if (d == "order") { try { a_out.order = std::stoi(val); } catch (...) {} }
                    else {
                        // Say so rather than swallowing it. A directive that
                        // does nothing in silence is worse than none at all —
                        // a translator writes it, sees no effect, and has no
                        // way to tell a typo from an unsupported feature.
                        SKSE::log::warn("[LANG] {}: unknown directive '{}' ignored",
                            a_out.id, key);
                    }
                    continue;
                }
                const int idx = IndexOfName(key);
                if (idx < 0) {
                    ++misses;
                    continue;
                }
                // ★A translated line is handed to printf-style calls with the
                // ARGUMENTS the English string was written for. A '%' sequence
                // that differs from the English one (missing %s, stray %d) is
                // not a style choice but a crash at draw time — drop the line
                // and let that one string fall back to English instead.
                if (kEN[idx] && FmtSig(val.c_str()) != FmtSig(kEN[idx])) {
                    SKSE::log::warn(
                        "[LANG] {}: '{}' format specifiers differ from English"
                        " - line ignored (falls back to English)",
                        a_out.id, key);
                    continue;
                }
                a_out.str[static_cast<size_t>(idx)] = val;
                ++hits;
            }
            if (a_out.name.empty()) a_out.name = a_out.id;
            SKSE::log::info("[LANG] '{}' ({}): {}/{} strings, {} unknown keys",
                a_out.id, a_out.name, hits, N, misses);
            return hits > 0;
        }

        [[nodiscard]] const Pack* PackFor(int a_lang)
        {
            if (a_lang == 0) return g_hasEn ? &g_en : nullptr;
            const auto i = static_cast<size_t>(a_lang - 1);
            return i < g_packs.size() ? &g_packs[i] : nullptr;
        }
    }

    int Get() { return g_lang; }

    void SetLang(int a_lang)
    {
        g_lang = (std::max)(0, (std::min)(Count() - 1, a_lang));
    }

    static_assert(std::size(kEN) == static_cast<size_t>(N), "kEN out of sync with Str");
    static_assert(std::size(kNames) == static_cast<size_t>(N), "kNames out of sync with Str");

    const char* T(Str a_key)
    {
        const int i = static_cast<int>(a_key);
        if (i < 0 || i >= N) return "?";
        if (const Pack* p = PackFor(g_lang)) {
            const auto& s = p->str[static_cast<size_t>(i)];
            if (!s.empty()) return s.c_str();
        }
        const char* s = kEN[i];
        return s ? s : "?";   // never hand ImGui a null format string
    }

    const char* KeyName(Str a_key)
    {
        const int i = static_cast<int>(a_key);
        return (i >= 0 && i < N) ? kNames[i] : "?";
    }

    int Count() { return 1 + static_cast<int>(g_packs.size()); }

    const char* DisplayName(int a_index)
    {
        if (a_index < 0 || a_index >= Count()) return "?";
        if (a_index == 0) return g_hasEn ? g_en.name.c_str() : "EN";
        return g_packs[static_cast<size_t>(a_index - 1)].name.c_str();
    }

    const char* Id(int a_index)
    {
        if (a_index <= 0 || a_index >= Count()) return kEnId;
        return g_packs[static_cast<size_t>(a_index - 1)].id.c_str();
    }

    int IndexOfId(const char* a_id)
    {
        if (!a_id || !a_id[0]) return -1;
        const std::string_view want(a_id);
        for (int i = 0; i < Count(); ++i) {
            if (want == Id(i)) return i;
        }
        return -1;
    }

    void LoadPacks()
    {
        if (g_loaded) return;
        g_loaded = true;
        std::error_code ec;
        if (!std::filesystem::is_directory(kPackDir, ec)) {
            SKSE::log::info("[LANG] no {} - English only", kPackDir);
            return;
        }
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(kPackDir, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto ext = e.path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".ini") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());

        for (const auto& f : files) {
            Pack p;
            if (!ParsePack(f, p)) {
                SKSE::log::warn("[LANG] '{}' had no usable strings - skipped",
                    f.filename().string());
                continue;
            }
            if (p.id == kEnId) {
                g_en = std::move(p);   // overlays slot 0 instead of joining the list
                g_hasEn = true;
                continue;
            }
            g_packs.push_back(std::move(p));
        }
        // #order first, then id — the row must not reshuffle between sessions,
        // and shipped languages want a chosen order rather than an alphabetical
        // one (ja/ko/zh would otherwise read as a filing cabinet)
        std::sort(g_packs.begin(), g_packs.end(), [](const Pack& a, const Pack& b) {
            return a.order != b.order ? a.order < b.order : a.id < b.id;
        });
        SKSE::log::info("[LANG] {} language(s) available{}", Count(),
            g_hasEn ? " (English overlaid by en.ini)" : "");
    }

    const char* FontPath()
    {
        const Pack* p = PackFor(g_lang);
        return p ? p->font.c_str() : "";
    }

    const char* FontRange()
    {
        const Pack* p = PackFor(g_lang);
        return p ? p->range.c_str() : "";
    }
}
