#pragma once

#include <cstdio>
#include <string>

namespace FUI
{
    // Phase 2 D2: the ONE item definition. Formerly four drifting copies —
    // main.cpp ItemDef / Grid::GridDef / Editor::FullDef / IconDef — plus
    // field-by-field converters at every module boundary; a new def field
    // meant touching seven places. Every module now shares THIS struct (the
    // old names live on below as aliases), and the ini serialization walks
    // the field metatable, so a new numeric field = one member + one row.
    struct ItemDef
    {
        int         w = 1;
        int         h = 1;
        std::string shape;   // polyomino mask, rows of 1/0 joined by '|'
                             // ("11|10|10" = an L). Empty = plain w x h
                             // rectangle. Overrides w/h.
        float rx = -90.0f;   // model orientation in degrees (default: stand weapons up)
        float ry = 0.0f;
        float rz = 0.0f;
        float scale = 1.0f;  // multiplier on the auto fit-to-tile scale
        int   bag = 0;       // 1 = bag item: right-click opens its own grid window
        int   bw = 4;        // bag grid size (columns x rows)
        int   bh = 4;
        int   stack = 0;     // G3: per-item stack cap (0 = category default)
    };

    // the old per-module names — every one is THIS struct now. Kept so call
    // sites read naturally (icon code says IconDef, grid code says GridDef).
    using IconDef = ItemDef;
    namespace Grid { using GridDef = ItemDef; }
    namespace Editor { using FullDef = ItemDef; }

    // bounding box from a "11|10|10" mask (w/h tokens ignored when shape set)
    inline void DeriveShapeBounds(ItemDef& a_def)
    {
        if (a_def.shape.empty()) return;
        int w = 0, h = 1, run = 0;
        for (char ch : a_def.shape) {
            if (ch == '|') { ++h; run = 0; }
            else { w = (std::max)(w, ++run); }
        }
        a_def.w = (std::max)(1, w);
        a_def.h = h;
    }

    namespace detail
    {
        // ---- ini serialization metatable ("k:v, k:v" lines) ----
        // ONE row per numeric token — ParseItemDef and FormatItemDef both
        // walk this list. `shape` (string) is handled explicitly by both.
        enum class DefRule
        {
            kCoreDims,   // w/h: parsed always, WRITTEN only without a shape
            kCore,       // always parsed and written
            kBagBlock,   // bag/bw/bh: written only when bag is set
            kIfPositive, // stack: written only when > 0
        };

        struct DefField
        {
            const char*      key;
            float ItemDef::* fmem;      // exactly one of fmem/imem is set
            int   ItemDef::* imem;
            float            minv;
            float            maxv;
            int              decimals;  // printf precision (float fields)
            DefRule          rule;
        };

        inline constexpr float kNoClamp = 1.0e9f;
        inline constexpr DefField kDefFields[] = {
            { "w",     nullptr,         &ItemDef::w,     1.0f,     kNoClamp, 0, DefRule::kCoreDims },
            { "h",     nullptr,         &ItemDef::h,     1.0f,     kNoClamp, 0, DefRule::kCoreDims },
            { "rx",    &ItemDef::rx,    nullptr,        -kNoClamp, kNoClamp, 0, DefRule::kCore },
            { "ry",    &ItemDef::ry,    nullptr,        -kNoClamp, kNoClamp, 0, DefRule::kCore },
            { "rz",    &ItemDef::rz,    nullptr,        -kNoClamp, kNoClamp, 0, DefRule::kCore },
            { "scale", &ItemDef::scale, nullptr,        -kNoClamp, kNoClamp, 2, DefRule::kCore },
            { "bag",   nullptr,         &ItemDef::bag,   0.0f,     1.0f,     0, DefRule::kBagBlock },
            { "bw",    nullptr,         &ItemDef::bw,    1.0f,     10.0f,    0, DefRule::kBagBlock },
            { "bh",    nullptr,         &ItemDef::bh,    1.0f,     10.0f,    0, DefRule::kBagBlock },
            { "stack", nullptr,         &ItemDef::stack, 0.0f,     999.0f,   0, DefRule::kIfPositive },
        };

        // token position ANCHORED at a field boundary (start / after ',' or
        // whitespace) — a plain find() matched "w:" inside "bw:" and "h:"
        // inside "bh:" on hand-edited lines that order the bag block first
        inline size_t FieldPos(const std::string& a_s, const std::string& a_pat)
        {
            size_t p = a_s.find(a_pat);
            while (p != std::string::npos) {
                if (p == 0 || a_s[p - 1] == ',' || a_s[p - 1] == ' ' || a_s[p - 1] == '\t') {
                    return p;
                }
                p = a_s.find(a_pat, p + 1);
            }
            return std::string::npos;
        }

        inline float FieldNum(const std::string& a_s, const char* a_k, float a_def)
        {
            const std::string pat = std::string(a_k) + ":";
            const auto p = FieldPos(a_s, pat);
            if (p == std::string::npos) return a_def;
            try { return std::stof(a_s.substr(p + pat.size())); } catch (...) { return a_def; }
        }

        inline std::string FieldStr(const std::string& a_s, const char* a_k)
        {
            const std::string pat = std::string(a_k) + ":";
            auto p = FieldPos(a_s, pat);
            if (p == std::string::npos) return "";
            p += pat.size();
            const auto e = a_s.find(',', p);
            std::string v = a_s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t\r") + 1);
            return v;
        }
    }

    // Parse a "k:v, k:v" value string over a_base (absent tokens keep the
    // base's value). Replaces the THREE hand-copied parsers that used to
    // live in main.cpp (item ini loader / ParseDefValues / preset loader).
    [[nodiscard]] inline ItemDef ParseItemDef(const std::string& a_val, const ItemDef& a_base)
    {
        ItemDef d;
        d.shape = detail::FieldStr(a_val, "shape");
        if (d.shape.empty()) d.shape = a_base.shape;
        for (const auto& f : detail::kDefFields) {
            const float base = f.fmem ? a_base.*(f.fmem)
                                      : static_cast<float>(a_base.*(f.imem));
            float v = detail::FieldNum(a_val, f.key, base);
            v = (std::max)(f.minv, (std::min)(f.maxv, v));
            if (f.fmem) d.*(f.fmem) = v;
            else        d.*(f.imem) = static_cast<int>(v);
        }
        DeriveShapeBounds(d);   // shape (if any) owns w/h
        return d;
    }

    // One ini line: "key = shape:S|w:N, h:N, rx:.., ry:.., rz:.., scale:..
    // [, bag:1, bw:N, bh:N][, stack:N]" — same output the hand-written
    // formatter produced.
    [[nodiscard]] inline std::string FormatItemDef(const std::string& a_key, const ItemDef& a_def)
    {
        std::string line = a_key + " =";
        bool first = true;
        auto emit = [&](const std::string& a_frag) {
            line += first ? " " : ", ";
            line += a_frag;
            first = false;
        };
        if (!a_def.shape.empty()) emit("shape:" + a_def.shape);
        char buf[64];
        for (const auto& f : detail::kDefFields) {
            if (f.rule == detail::DefRule::kCoreDims && !a_def.shape.empty()) continue;
            if (f.rule == detail::DefRule::kBagBlock && !a_def.bag) continue;
            if (f.rule == detail::DefRule::kIfPositive && a_def.*(f.imem) <= 0) continue;
            if (f.fmem) {
                std::snprintf(buf, sizeof(buf), "%s:%.*f", f.key, f.decimals, a_def.*(f.fmem));
            } else {
                std::snprintf(buf, sizeof(buf), "%s:%d", f.key, a_def.*(f.imem));
            }
            emit(buf);
        }
        return line;
    }
}
