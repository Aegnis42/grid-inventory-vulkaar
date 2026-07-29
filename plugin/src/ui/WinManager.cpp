#include "ui/IconCache.h"
#include "ui/Lang.h"
#include "ui/LootBarter.h"
#include "ui/Theme.h"
#include "ui/UIRoot.h"
#include "ui/WinManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace FUI
{
    static constexpr const char* kUiIniPath = "Data/SKSE/Plugins/GridInventory_ui.ini";
    // GI48: named presets live beside the plugin as GridInventory_<name>.ini
    static constexpr const char* kPresetPrefix = "Data/SKSE/Plugins/GridInventory_";

    // Windows-illegal filename characters flattened; blank falls back to
    // "Default" (the no-name export the settings row promises)
    static std::string SanitizePresetName(const std::string& a_name)
    {
        std::string out;
        for (char c : a_name) {
            const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' ||
                             c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
            out += bad ? '_' : c;
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
        return out.empty() ? "Default" : out;
    }

    // config files share the GridInventory_ prefix -- never list them as presets
    static bool ReservedPresetName(std::string a_name)
    {
        for (auto& c : a_name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static constexpr const char* kReserved[] = {
            "ui", "layout", "items", "categories", "icons", "iconfail", "slots"
        };
        for (const char* r : kReserved) {
            if (a_name == r) return true;
        }
        return a_name.ends_with("_icons");   // icon bundles (defensive)
    }

    std::string WinManager::PresetIniPath(const std::string& a_name)
    {
        return kPresetPrefix + SanitizePresetName(a_name) + ".ini";
    }

    std::string WinManager::PresetPakPath(const std::string& a_name)
    {
        return kPresetPrefix + SanitizePresetName(a_name) + "_icons.pak";
    }

    std::vector<std::string> WinManager::ListPresets() const
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("Data/SKSE/Plugins", ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".ini") continue;
            const std::string stem = e.path().stem().string();
            constexpr std::string_view prefix = "GridInventory_";
            if (!stem.starts_with(prefix)) continue;
            std::string name = stem.substr(prefix.size());
            if (name.empty() || ReservedPresetName(name)) continue;
            out.push_back(std::move(name));
        }
        std::sort(out.begin(), out.end());
        const auto def = std::find(out.begin(), out.end(), "Default");
        if (def != out.end()) std::rotate(out.begin(), def, def + 1);
        return out;
    }

    WinManager* WinManager::GetSingleton()
    {
        static WinManager singleton;
        return std::addressof(singleton);
    }

    WinManager::Win& WinManager::Ensure(const std::string& a_key)
    {
        for (auto& w : m_wins) {
            if (w.key == a_key) return w;
        }
        m_wins.push_back({});
        m_wins.back().key = a_key;
        return m_wins.back();
    }

    WinManager::Win* WinManager::Find(const std::string& a_key)
    {
        for (auto& w : m_wins) {
            if (w.key == a_key) return &w;
        }
        return nullptr;
    }

    bool WinManager::IsOpen(const Win& a_win) const
    {
        // drawn within the last couple of ImGui frames = participates in
        // magnet/dock (closed bag windows must not attract or adopt)
        return a_win.lastSeen >= 0 && ImGui::GetFrameCount() - a_win.lastSeen <= 2;
    }

    ImVec2 WinManager::MainCenter(ImVec2 a_fallback)
    {
        if (auto* m = Find("main"); m && m->posKnown) {
            return ImVec2(m->pos.x + m->size.x * 0.5f, m->pos.y + m->size.y * 0.5f);
        }
        return a_fallback;
    }

    // ---- persistence (F6: replaces localStorage fabinv_winpos/winparent) ----
    // line format:  key = x,y,w,h[,parent:<key>]

    void WinManager::Load()
    {
        m_loaded = true;
        std::ifstream in(kUiIniPath);
        if (!in) return;

        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '[') continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                const auto b = s.find_first_not_of(" \t\r");
                const auto e = s.find_last_not_of(" \t\r");
                return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
            };
            const std::string key = trim(line.substr(0, eq));
            if (key.empty()) continue;

            std::string rest = trim(line.substr(eq + 1));
            if (key == "!uiscale") {   // H′: global UI scale
                try { Theme::SetScale(std::stof(rest)); } catch (...) {}
                continue;
            }
            if (key == "!skin") {
                try { Theme::SetSkin(std::stoi(rest)); } catch (...) {}
                continue;
            }
            if (key == "!lang") {
                try { Lang::SetLang(std::stoi(rest)); } catch (...) {}
                continue;
            }
            if (key == "!glowstyle") {   // 1 silhouette / 0 radial (revert)
                try { Theme::SetGlowStyle(std::stoi(rest)); } catch (...) {}
                continue;
            }
            if (key == "!glowgain0" || key == "!glowgain1") {   // per-style brightness
                try { Theme::SetGlowGainOf(key[9] - '0', std::stof(rest)); } catch (...) {}
                continue;
            }
            if (key == "!icongain") {    // item icon brightness (texture bake)
                try { Theme::SetIconGain(std::stof(rest)); } catch (...) {}
                continue;
            }
            if (key == "!merchgoldinf") {   // F3: unlimited merchant gold
                try { LootBarter::SetMerchantGoldInfinite(std::stoi(rest) != 0); } catch (...) {}
                continue;
            }
            if (key == "!merchbuyall") {    // F4: vendor category lift
                try { LootBarter::SetMerchantBuysAll(std::stoi(rest) != 0); } catch (...) {}
                continue;
            }
            if (key == "!iconstyle") {      // 0 realistic / 1 low-poly pak
                try {
                    IconCache::GetSingleton()->SetStylizedStyle(std::stoi(rest) != 0);
                } catch (...) {}
                continue;
            }
            if (key == "!glowgain") {    // legacy single-value key -> both styles
                try {
                    const float g = std::stof(rest);
                    Theme::SetGlowGainOf(0, g);
                    Theme::SetGlowGainOf(1, g);
                } catch (...) {}
                continue;
            }
            std::string parent;
            if (const auto pp = rest.find("parent:"); pp != std::string::npos) {
                parent = trim(rest.substr(pp + 7));
                rest = rest.substr(0, pp);
            }
            float x = 0, y = 0, w = 0, h = 0;
            char c;
            std::istringstream ss(rest);
            if (!(ss >> x >> c >> y >> c >> w >> c >> h)) continue;

            auto& win = Ensure(key);
            win.pos = ImVec2(x, y);
            if (w > 0 && h > 0) win.size = ImVec2(w, h);
            win.parent = parent;
            win.posKnown = true;
        }
    }

    namespace
    {
        // GI47: def sections travel through these (registered by main.cpp)
        std::function<void(std::ostream&)>                            g_presetDefsWrite;
        std::function<void(int, const std::string&, const std::string&)> g_presetDefApply;
        std::function<void()>                                         g_presetDefsDone;
    }

    void WinManager::SetPresetDefsHooks(std::function<void(std::ostream&)> a_write,
                                        std::function<void(int, const std::string&,
                                                           const std::string&)> a_apply,
                                        std::function<void()> a_done)
    {
        g_presetDefsWrite = std::move(a_write);
        g_presetDefApply = std::move(a_apply);
        g_presetDefsDone = std::move(a_done);
    }

    void WinManager::ExportPreset(const std::string& a_name) const
    {
        const std::string iniPath = PresetIniPath(a_name);
        std::ofstream out(iniPath, std::ios::trunc);
        if (!out) {
            SKSE::log::error("[PRESET] cannot write {}", iniPath);
            return;
        }
        out << "; GridInventory preset -- ONE file: UI style + item grid definitions.\n";
        out << "; 프리셋 파일입니다 -- 이 파일 하나로 UI 스타일과 아이템 배치 정의까지 공유됩니다.\n";
        out << "; (창 위치·개인 설정은 담기지 않습니다 / window layout & cheats never travel)\n";
        out << "!uiscale = " << Theme::Scale() << "\n";
        out << "!skin = " << Theme::SkinIndex() << "\n";
        out << "!glowstyle = " << Theme::GlowStyle() << "\n";
        out << "!glowgain0 = " << Theme::GlowGainOf(0) << "\n";
        out << "!glowgain1 = " << Theme::GlowGainOf(1) << "\n";
        out << "!icongain = " << Theme::IconGain() << "\n";
        out << "!iconstyle = " << (IconCache::GetSingleton()->StylizedStyle() ? 1 : 0) << "\n";
        // GI47: the item/category defs ride along -- one file, whole look
        if (g_presetDefsWrite) g_presetDefsWrite(out);
        out.close();
        // and the captured icons, so the reader never waits on re-captures
        IconCache::GetSingleton()->ExportPakTo(PresetPakPath(a_name).c_str());
        SKSE::log::info("[PRESET] preset '{}' exported (style + defs + icons)",
            SanitizePresetName(a_name));
    }

    bool WinManager::ImportPreset(const std::string& a_name)
    {
        std::ifstream in(PresetIniPath(a_name));
        if (!in) return false;
        // 0 = top (style keys), 1 = [categories], 2 = [items],
        // 3 = [glow] (old editor-preset files -- accepted for compatibility)
        int section = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            // a section header carries no '=' -- plugin names may legally
            // START with '[' ("[ELLE] Sol.esp|0x... = w:2..."), so bracket
            // alone must not switch sections (real data was lost to that once)
            if (line[0] == '[' && line.find('=') == std::string::npos) {
                if (line.find("[categories]") != std::string::npos) section = 1;
                else if (line.find("[items]") != std::string::npos) section = 2;
                else if (line.find("[glow]") != std::string::npos) section = 3;
                else section = 0;
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string a_s) {
                const auto b = a_s.find_first_not_of(" \t\r");
                const auto e = a_s.find_last_not_of(" \t\r");
                return b == std::string::npos ? std::string{}
                                              : a_s.substr(b, e - b + 1);
            };
            const std::string key = trim(line.substr(0, eq));
            const std::string rest = trim(line.substr(eq + 1));
            if (key.empty()) continue;
            // STYLE keys -- language, layout and trade toggles are the
            // reader's own even if a hand-edited file tries to smuggle them.
            if (key[0] == '!') {
                try {
                    if (key == "!uiscale")        Theme::SetScale(std::stof(rest));
                    else if (key == "!skin")      Theme::SetSkin(std::stoi(rest));
                    else if (key == "!glowstyle") Theme::SetGlowStyle(std::stoi(rest));
                    else if (key == "!glowgain0") Theme::SetGlowGainOf(0, std::stof(rest));
                    else if (key == "!glowgain1") Theme::SetGlowGainOf(1, std::stof(rest));
                    else if (key == "!icongain")  Theme::SetIconGain(std::stof(rest));
                    else if (key == "!iconstyle")
                        IconCache::GetSingleton()->SetStylizedStyle(std::stoi(rest) != 0);
                } catch (...) {}
                continue;
            }
            if (section == 3) {   // old editor-preset glow grammar
                try {
                    if (key == "style")      Theme::SetGlowStyle(std::stoi(rest));
                    else if (key == "gain0") Theme::SetGlowGainOf(0, std::stof(rest));
                    else if (key == "gain1") Theme::SetGlowGainOf(1, std::stof(rest));
                } catch (...) {}
                continue;
            }
            if ((section == 1 || section == 2) && g_presetDefApply) {
                g_presetDefApply(section, key, rest);   // merge: preset wins
            }
        }
        if (g_presetDefsDone) g_presetDefsDone();
        SKSE::log::info("[PRESET] preset '{}' imported (style + defs)",
            SanitizePresetName(a_name));
        return true;
    }

    void WinManager::Save() const
    {
        std::ofstream out(kUiIniPath, std::ios::trunc);
        if (!out) return;
        out << "; GridInventory window layout (auto-generated)\n";
        out << "; key = x,y,w,h[,parent:<key>]\n";
        out << "!uiscale = " << Theme::Scale() << "\n";
        out << "!skin = " << Theme::SkinIndex() << "\n";
        out << "!lang = " << Lang::Get() << "\n";
        out << "!glowstyle = " << Theme::GlowStyle() << "\n";
        out << "!glowgain0 = " << Theme::GlowGainOf(0) << "\n";
        out << "!glowgain1 = " << Theme::GlowGainOf(1) << "\n";
        out << "!icongain = " << Theme::IconGain() << "\n";
        out << "!merchgoldinf = " << (LootBarter::MerchantGoldInfinite() ? 1 : 0) << "\n";
        out << "!merchbuyall = " << (LootBarter::MerchantBuysAll() ? 1 : 0) << "\n";
        out << "!iconstyle = " << (IconCache::GetSingleton()->StylizedStyle() ? 1 : 0) << "\n";
        for (const auto& w : m_wins) {
            if (!w.posKnown) continue;
            out << w.key << " = "
                << static_cast<int>(w.pos.x) << ',' << static_cast<int>(w.pos.y) << ','
                << static_cast<int>(w.size.x) << ',' << static_cast<int>(w.size.y);
            if (!w.parent.empty()) out << ",parent:" << w.parent;
            out << "\n";
        }
    }

    // ---- per-window draw helpers ----

    void WinManager::ApplyNext(const std::string& a_key, ImVec2 a_defaultPos, ImVec2 a_defaultSize)
    {
        if (!m_loaded) Load();
        auto& w = Ensure(a_key);
        if (!w.posKnown) {
            w.pos = a_defaultPos;
            w.posKnown = true;
        }
        // Size is ALWAYS code-defined (windows aren't user-resizable); only
        // the position persists — otherwise a stale saved size wins forever.
        w.size = a_defaultSize;
        ImGui::SetNextWindowPos(w.pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(w.size, ImGuiCond_Always);
    }

    // uppercase + letter-tracked title text (skins set the tracking)
    static float TrackedTextWidth(ImFont* a_font, float a_size, const char* a_text, float a_spacing)
    {
        float width = 0.0f;
        const char* p = a_text;
        while (*p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            const int len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xE0) == 0xC0 ? 2 : 1;
            width += a_font->CalcTextSizeA(a_size, FLT_MAX, 0.0f, p, p + len).x + a_spacing;
            p += len;
        }
        return width > 0.0f ? width - a_spacing : 0.0f;
    }

    static void DrawTrackedText(ImDrawList* a_dl, ImFont* a_font, float a_size, ImVec2 a_pos,
                                ImU32 a_col, const char* a_text, float a_spacing)
    {
        const char* p = a_text;
        float x = a_pos.x;
        while (*p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            const int len = (c & 0xF8) == 0xF0 ? 4 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xE0) == 0xC0 ? 2 : 1;
            a_dl->AddText(a_font, a_size, ImVec2(x, a_pos.y), a_col, p, p + len);
            x += a_font->CalcTextSizeA(a_size, FLT_MAX, 0.0f, p, p + len).x + a_spacing;
            p += len;
        }
    }

    void WinManager::TitleBar(const std::string& a_key, const char* a_label, float a_reserveRight,
                              bool a_centerTitle)
    {
        auto& w = Ensure(a_key);
        w.pos = ImGui::GetWindowPos();
        w.size = ImGui::GetWindowSize();
        w.lastSeen = ImGui::GetFrameCount();

        const auto& sk = Theme::S();
        const float S = Theme::Scale();
        const float barH = 34.0f * S;

        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = w.pos;
        const ImVec2 we(wp.x + w.size.x, wp.y + w.size.y);

        // Window chrome must reach the EDGE pixels, but the window drawlist
        // is clipped ~half-padding inside the window (edge-hugging strips and
        // corner-fade lines silently vanished). Visual-only clip override.
        dl->PushClipRect(wp, we, false);

        // OATHVEIN TORN: 9-slice torn-paper panel fills the window (opaque
        // centre hides the parked model; ragged edges show the world). Drawn
        // first so all chrome/content lands on top.
        if (sk.tornFrame) {
            void* tex = nullptr;
            switch (sk.tornTex) {
            case 3:  tex = UIRoot::TornBrightB(); break;   // skin 6 (V2)
            case 2:  tex = UIRoot::TornCreamA(); break;    // skin 5 (V1)
            case 1:  tex = UIRoot::TornGlowB(); break;     // skin 4
            default: tex = UIRoot::TornGlowA(); break;     // skin 3
            }
            if (tex) {
                Theme::NineSlice(dl, tex, wp, we, 30.0f * S, Theme::kTornUM, Theme::kTornVM);
            }
        }
        // bevelChrome (kept for future skins): grey gradient titlebar + full
        // bevel border (dark
        // outer line, light inner line) — classic beveled chrome. The
        // gradient covers the whole inset title zone and ends exactly on the
        // generic (inset) title separator — no extra full-width line that
        // would poke into the border (v12.7).
        if (sk.bevelChrome) {
            const float tbB = wp.y + Theme::FrameInsetY() + barH;
            // v12.2: titlebar even MORE transparent (user feedback)
            dl->AddRectFilledMultiColor(wp, ImVec2(we.x, tbB),
                IM_COL32(150, 150, 154, 80), IM_COL32(150, 150, 154, 80),
                IM_COL32(108, 108, 112, 80), IM_COL32(108, 108, 112, 80));
            dl->AddRect(wp, we, IM_COL32(56, 56, 60, 255), sk.rounding);
            dl->AddRect(ImVec2(wp.x + 1.0f, wp.y + 1.0f),
                ImVec2(we.x - 1.0f, we.y - 1.0f), IM_COL32(216, 216, 220, 110),
                (std::max)(0.0f, sk.rounding - 1.0f));
        }
        // OATHVEIN: 2px crimson strip + corner-fade border (full border is off)
        if (sk.topStrip) {
            dl->AddRectFilled(wp, ImVec2(we.x, wp.y + 2.0f), IM_COL32(122, 30, 22, 140));
        }
        // window corner-fade border — suppressed when a torn frame is drawn
        // (the torn texture is the border; slots/items still use cornerFade)
        if (sk.cornerFade && !sk.tornFrame) {
            const float top = wp.y + (sk.topStrip ? 2.0f : 0.0f);
            Theme::CornerFade(dl, ImVec2(wp.x, top), we, Theme::Acc(0.55f));
        }

        {
            const float ly = wp.y + Theme::FrameInsetY() + barH;
            dl->AddLine(ImVec2(wp.x + Theme::FrameInsetX(), ly),
                ImVec2(we.x - Theme::FrameInsetX(), ly),
                Theme::Acc(sk.cornerFade ? 0.10f : 0.25f));
        }

        // title: tracked uppercase
        ImFont* font = ImGui::GetFont();
        const float fontSize = 15.0f * S;
        const float spacing = sk.titleSpacing * S;
        // tornFrame: nudge the title in so it clears the ragged frame edge
        const float insX = Theme::FrameInsetX();
        const float insY = Theme::FrameInsetY();
        const float textW = TrackedTextWidth(font, fontSize, a_label, spacing);
        const float ty = wp.y + insY + (barH - fontSize) * 0.5f;
        float tx = a_centerTitle ? wp.x + (w.size.x - textW) * 0.5f
                                 : wp.x + 12.0f * S + insX;
        if (sk.titleGlow) {
            // poor-man's bloom: 4 offset passes under the main text, then a
            // 1px underline fading out to the right (v10 title treatment)
            const ImU32 glow = Theme::Col(sk.hi, 0.12f);
            const float o = 1.0f;
            DrawTrackedText(dl, font, fontSize, ImVec2(tx - o, ty), glow, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx + o, ty), glow, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty - o), glow, a_label, spacing);
            DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty + o), glow, a_label, spacing);
            const float uy = ty + fontSize + 3.0f * S;
            dl->AddRectFilledMultiColor(ImVec2(tx, uy), ImVec2(tx + textW, uy + 1.0f),
                Theme::Col(sk.hi, 0.75f), Theme::Col(sk.hi, 0.0f),
                Theme::Col(sk.hi, 0.0f), Theme::Col(sk.hi, 0.75f));
        }
        DrawTrackedText(dl, font, fontSize, ImVec2(tx, ty),
            sk.titleGlow ? Theme::Col(sk.hi, 1.0f) : Theme::Acc(1.0f), a_label, spacing);

        dl->PopClipRect();   // back to the window's normal clip

        ImGui::SetCursorScreenPos(wp);
        const float stripW = (std::max)(40.0f, w.size.x - a_reserveRight);
        ImGui::InvisibleButton(("##titlebar_" + a_key).c_str(), ImVec2(stripW, insY + barH));
        if (ImGui::IsItemActivated() && !m_dragLock && !m_drag.active) {
            StartDrag(a_key);
        }

        // content begins under the strip (inset in for tornFrame skins)
        ImGui::SetCursorScreenPos(
            ImVec2(wp.x + 12.0f * S + insX, wp.y + insY + barH + 8.0f * S));
    }

    // ---- drag machinery (JS startWinDrag / mousemove / mouseup) ----

    std::vector<std::string> WinManager::SubtreeOf(const std::string& a_key) const
    {
        // every OPEN window whose parent chain reaches a_key
        std::vector<std::string> out;
        for (const auto& w : m_wins) {
            if (w.key == a_key || w.key == "main" || !IsOpen(w)) continue;
            std::string cur = w.parent;
            int hops = 0;
            while (!cur.empty() && hops++ < 32) {
                if (cur == a_key) { out.push_back(w.key); break; }
                if (cur == "main") break;
                const Win* p = nullptr;
                for (const auto& c : m_wins) {
                    if (c.key == cur) { p = &c; break; }
                }
                cur = p ? p->parent : std::string();
            }
        }
        return out;
    }

    void WinManager::StartDrag(const std::string& a_key)
    {
        auto& w = Ensure(a_key);
        m_drag = {};
        m_drag.active = true;
        m_drag.key = a_key;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        m_drag.grab = ImVec2(mouse.x - w.pos.x, mouse.y - w.pos.y);

        // the dragged window carries its whole subtree
        m_drag.extMin = ImVec2(0.0f, 0.0f);
        m_drag.extMax = w.size;
        for (const auto& k : SubtreeOf(a_key)) {
            if (auto* f = Find(k)) {
                const ImVec2 off(f->pos.x - w.pos.x, f->pos.y - w.pos.y);
                m_drag.followers.push_back({ k, off });
                m_drag.extMin.x = (std::min)(m_drag.extMin.x, off.x);
                m_drag.extMin.y = (std::min)(m_drag.extMin.y, off.y);
                m_drag.extMax.x = (std::max)(m_drag.extMax.x, off.x + f->size.x);
                m_drag.extMax.y = (std::max)(m_drag.extMax.y, off.y + f->size.y);
            }
        }
    }

    float WinManager::ContactLen(ImVec2 a_min, ImVec2 a_max, ImVec2 b_min, ImVec2 b_max)
    {
        constexpr float eps = 1.5f;
        const float vo = (std::min)(a_max.y, b_max.y) - (std::max)(a_min.y, b_min.y);
        const float ho = (std::min)(a_max.x, b_max.x) - (std::max)(a_min.x, b_min.x);
        float len = 0.0f;
        if ((std::fabs(a_max.x - b_min.x) <= eps || std::fabs(a_min.x - b_max.x) <= eps) && vo > eps)
            len = (std::max)(len, vo);
        if ((std::fabs(a_max.y - b_min.y) <= eps || std::fabs(a_min.y - b_max.y) <= eps) && ho > eps)
            len = (std::max)(len, ho);
        return len;
    }

    ImVec2 WinManager::Magnetize(ImVec2 a_pos, ImVec2 a_size,
                                 const std::vector<std::string>& a_excluded) const
    {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        float x = a_pos.x, y = a_pos.y;
        const float w = a_size.x, h = a_size.y;

        // NEAREST candidate wins on each axis (last-wins let a window 10px away
        // steal the snap from the one being docked against)
        struct Best { float d = kMagnet; float v = 0.0f; bool has = false; };
        Best bx, by;
        auto consider = [](Best& best, float cur, float snapped) {
            const float dist = std::fabs(cur - snapped);
            if (dist < best.d) { best.d = dist; best.v = snapped; best.has = true; }
        };
        consider(bx, x, 0.0f);
        consider(bx, x, disp.x - w);
        consider(by, y, 0.0f);
        consider(by, y, disp.y - h);

        for (const auto& t : m_wins) {
            if (!t.posKnown || !IsOpen(t)) continue;
            bool skip = t.key == m_drag.key;
            for (const auto& ex : a_excluded) {
                if (t.key == ex) { skip = true; break; }
            }
            if (skip) continue;

            const float tl = t.pos.x, tt = t.pos.y;
            const float tr = t.pos.x + t.size.x, tb = t.pos.y + t.size.y;
            // x snaps only while vertically overlapping the target (and vice
            // versa), so docked windows slide along each other without letting go
            const bool vOverlap = y < tb + kMagnet && y + h > tt - kMagnet;
            const bool hOverlap = x < tr + kMagnet && x + w > tl - kMagnet;
            if (vOverlap) {
                consider(bx, x, tr);        // my left | its right
                consider(bx, x, tl - w);    // my right | its left
                consider(bx, x, tl);        // left edges align
                consider(bx, x, tr - w);    // right edges align
            }
            if (hOverlap) {
                consider(by, y, tb);
                consider(by, y, tt - h);
                consider(by, y, tt);
                consider(by, y, tb - h);
            }
        }
        if (bx.has) x = bx.v;
        if (by.has) y = by.v;
        return ImVec2(x, y);
    }

    bool WinManager::BeginConfirmPopup(const std::string& a_key, const char* a_imguiId,
                                       const char* a_title, ImVec2 a_size)
    {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        ApplyNext(a_key, ImVec2((disp.x - a_size.x) * 0.5f,
                                (disp.y - a_size.y) * 0.5f), a_size);
        ImGui::Begin(a_imguiId, nullptr, kManagedWinFlags);
        UIRoot::NoteOverlayRect();
        TitleBar(a_key, a_title, 0.0f, true);
        // outside click cancels (settings pattern) — never on the opening
        // frame: a popup first drawn on the SAME frame as the opening click
        // would read that click as "outside" and instantly close
        return !ImGui::IsWindowAppearing() &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
               !ImGui::IsWindowHovered();
    }

    void WinManager::Update()
    {
        if (!m_drag.active) return;

        auto* w = Find(m_drag.key);
        if (!w) { m_drag.active = false; return; }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            ImVec2 p(mouse.x - m_drag.grab.x, mouse.y - m_drag.grab.y);

            std::vector<std::string> excl;
            excl.reserve(m_drag.followers.size());
            for (const auto& f : m_drag.followers) excl.push_back(f.key);
            p = Magnetize(p, w->size, excl);

            // whole-group containment (left/top win if the group is larger
            // than the screen)
            const ImVec2 disp = ImGui::GetIO().DisplaySize;
            p.x = (std::max)(-m_drag.extMin.x, (std::min)(disp.x - m_drag.extMax.x, p.x));
            p.y = (std::max)(-m_drag.extMin.y, (std::min)(disp.y - m_drag.extMax.y, p.y));

            w->pos = p;
            for (const auto& f : m_drag.followers) {
                if (auto* fw = Find(f.key)) {
                    fw->pos = ImVec2(p.x + f.off.x, p.y + f.off.y);
                }
            }
        } else {
            EndDrag();
        }
    }

    void WinManager::EndDrag()
    {
        auto* w = Find(m_drag.key);
        if (!w) { m_drag.active = false; return; }

        const ImVec2 rMin = w->pos;
        const ImVec2 rMax(w->pos.x + w->size.x, w->pos.y + w->size.y);
        const ImVec2 c0(rMin.x + w->size.x * 0.5f, rMin.y + w->size.y * 0.5f);

        auto isMine = [&](const std::string& key) {
            for (const auto& f : m_drag.followers) {
                if (f.key == key) return true;
            }
            return false;
        };

        if (m_drag.key != "main") {
            // released snapped onto a window -> that window adopts it; released
            // in the open -> the link breaks. Own subtree can't adopt (no cycles).
            // Among everything we ended flush against, adopt the window whose
            // CENTER is nearest (contact length ties constantly).
            std::string parent;
            float best = FLT_MAX;
            for (const auto& t : m_wins) {
                if (t.key == m_drag.key || !t.posKnown || !IsOpen(t) || isMine(t.key)) continue;
                const ImVec2 tMax(t.pos.x + t.size.x, t.pos.y + t.size.y);
                if (ContactLen(rMin, rMax, t.pos, tMax) <= 0.0f) continue;
                const float d = std::hypot(t.pos.x + t.size.x * 0.5f - c0.x,
                                           t.pos.y + t.size.y * 0.5f - c0.y);
                if (d < best) { best = d; parent = t.key; }
            }
            w->parent = parent;   // "" clears the link
        } else {
            // MAIN snapped onto a bag: main can never be a child, so the intent
            // inverts — that bag's whole tree becomes main's (and rides with it)
            std::string target;
            float best = FLT_MAX;
            for (const auto& t : m_wins) {
                if (t.key == "main" || !t.posKnown || !IsOpen(t) || isMine(t.key)) continue;
                const ImVec2 tMax(t.pos.x + t.size.x, t.pos.y + t.size.y);
                if (ContactLen(rMin, rMax, t.pos, tMax) <= 0.0f) continue;
                const float d = std::hypot(t.pos.x + t.size.x * 0.5f - c0.x,
                                           t.pos.y + t.size.y * 0.5f - c0.y);
                if (d < best) { best = d; target = t.key; }
            }
            if (!target.empty()) {
                std::string root = target;
                int hops = 0;
                while (hops++ < 32) {
                    auto* r = Find(root);
                    if (!r || r->parent.empty() || r->parent == "main") break;
                    root = r->parent;
                }
                if (auto* r = Find(root)) r->parent = "main";
            }
        }

        m_drag.active = false;
        Save();
    }
}
