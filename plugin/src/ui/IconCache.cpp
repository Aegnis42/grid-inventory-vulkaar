#include "ui/IconCache.h"
#include "ui/ItemPreview.h"
#include "ui/Theme.h"

#include <d3d11.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace FUI
{
    static constexpr const char* kIconDir = "Data/SKSE/Plugins/GridInventory_icons";   // legacy (absorbed)
    static constexpr const char* kPakPath = "Data/SKSE/Plugins/GridInventory_icons.pak";
    // low-poly style pak: AUTHORED by IconStudio (PNG import), read-only in
    // game — keys are model slot << 32 (no rotation hash), same v5 records
    static constexpr const char* kLpPakPath = "Data/SKSE/Plugins/GridInventory_icons_lowpoly.pak";
    // capture keys that permanently failed (e.g. mod items whose preview
    // model never renders) — persisted so a relog does NOT retry them
    static constexpr const char* kFailPath = "Data/SKSE/Plugins/GridInventory_iconfail.txt";
    static constexpr const char* kPakTmp  = "Data/SKSE/Plugins/GridInventory_icons.pak.tmp";
    // 'FIC5': v5 = rotation-verified captures — v4 files may hold the engine's
    // default pose from the landing-frame race (older files re-capture)
    static constexpr std::uint32_t kIconMagic = 0x35434946;
    // 'FIC6': v6 stored the ROOT's extra rotation — measured identity for
    // every item (the hidden rotation lives BELOW the root, on engine-touched
    // child nodes), so v6 rotation data is meaningless. Entries stay readable
    // as pixels; their rotation field is ignored.
    static constexpr std::uint32_t kIconMagicRot6 = 0x36434946;
    // 'FIC7': records the FIRST GEOMETRY's world.rotate at capture (9 f32
    // row-major) — the transform the renderer actually draws with, wherever
    // the engine hid its extra rotation. The offline tool combines it with
    // the capture def + its own nif chain for an EXACT preview orientation.
    static constexpr std::uint32_t kIconMagicRot = 0x37434946;

    // ---- single-pak disk cache ----
    // One append-only file instead of thousands of per-item .fic files:
    //   v5: [magic u32 | key u64 | w u32 | h u32 | fmt u32 | len u32 | pixels]
    //   v6: [magic u32 | key u64 | w u32 | h u32 | fmt u32 | rot 9*f32 |
    //        len u32 | pixels]
    // Re-captures append a NEW record (last one wins at scan); superseded
    // bytes are compacted at scan time once they exceed 30%. A truncated
    // tail (crash mid-append) just ends the scan — icons are re-derivable
    // cache data, the worst case is a re-render.
    static constexpr std::uint64_t kPakHdrSize    = 4 + 8 + 4 * 4;        // 28
    static constexpr std::uint64_t kPakHdrSizeRot = kPakHdrSize + 36;     // 64

    namespace
    {
        struct PakEntry
        {
            std::uint64_t off = 0;   // record start (header)
            std::uint32_t w = 0, h = 0, fmt = 0, len = 0;
            bool          rotOnDisk = false;   // header carries the 36B field
            bool          hasRot = false;      // ...and it is v7-valid data
            float         rot[9] = {};

            [[nodiscard]] std::uint64_t hdrSize() const
            {
                return rotOnDisk ? kPakHdrSizeRot : kPakHdrSize;
            }
        };
        std::unordered_map<std::uint64_t, PakEntry> g_pakIndex;
        bool g_pakScanned = false;

        // persistent append handle: opening/closing the pak PER ICON was a
        // per-capture disk hitch during mass precache. flush() per append
        // keeps the same durability the old close() gave (stdio -> OS write;
        // neither ever fsync'd). MUST be closed before rename/remove.
        std::ofstream g_pakOut;

        void ClosePakHandle()
        {
            if (g_pakOut.is_open()) g_pakOut.close();
        }

        // NEW entries are always plain v5 — the v6/v7 rot record is write-dead
        // (the offline tool's orientation bugs were fixed at the source);
        // reading old v6/v7 entries stays supported in ScanPak/CompactPak.
        bool AppendToPakRaw(std::uint64_t a_key, std::uint32_t a_w, std::uint32_t a_h,
                            std::uint32_t a_fmt, const std::uint8_t* a_px, std::uint32_t a_len)
        {
            if (!g_pakOut.is_open()) {
                g_pakOut.clear();
                g_pakOut.open(kPakPath, std::ios::binary | std::ios::app);
                if (!g_pakOut) return false;
            }
            g_pakOut.seekp(0, std::ios::end);
            const std::uint64_t off = static_cast<std::uint64_t>(g_pakOut.tellp());
            g_pakOut.write(reinterpret_cast<const char*>(&kIconMagic), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_key), 8);
            g_pakOut.write(reinterpret_cast<const char*>(&a_w), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_h), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_fmt), 4);
            g_pakOut.write(reinterpret_cast<const char*>(&a_len), 4);
            g_pakOut.write(reinterpret_cast<const char*>(a_px), a_len);
            g_pakOut.flush();
            if (!g_pakOut) {
                ClosePakHandle();
                return false;
            }
            g_pakIndex[a_key] = PakEntry{ off, a_w, a_h, a_fmt, a_len };
            return true;
        }

        void CompactPak()
        {
            ClosePakHandle();   // the rename below fails on an open handle
            std::ofstream out(kPakTmp, std::ios::binary | std::ios::trunc);
            std::ifstream in(kPakPath, std::ios::binary);
            if (!out || !in) return;
            std::unordered_map<std::uint64_t, PakEntry> fresh;
            std::vector<std::uint8_t> px;
            for (const auto& [key, en] : g_pakIndex) {
                px.resize(en.len);
                in.seekg(static_cast<std::streamoff>(en.off + en.hdrSize()));
                if (!in.read(reinterpret_cast<char*>(px.data()), en.len)) return;
                const std::uint64_t off = static_cast<std::uint64_t>(out.tellp());
                // v6 entries (meaningless root-rotation experiment) compact
                // back to v5 — the junk 36B field is dropped
                const std::uint32_t magic = en.hasRot ? kIconMagicRot : kIconMagic;
                out.write(reinterpret_cast<const char*>(&magic), 4);
                out.write(reinterpret_cast<const char*>(&key), 8);
                out.write(reinterpret_cast<const char*>(&en.w), 4);
                out.write(reinterpret_cast<const char*>(&en.h), 4);
                out.write(reinterpret_cast<const char*>(&en.fmt), 4);
                if (en.hasRot) {
                    out.write(reinterpret_cast<const char*>(en.rot), 36);
                }
                out.write(reinterpret_cast<const char*>(&en.len), 4);
                out.write(reinterpret_cast<const char*>(px.data()), en.len);
                if (!out) return;
                PakEntry ne = en;
                ne.off = off;
                ne.rotOnDisk = en.hasRot;
                fresh[key] = ne;
            }
            in.close();
            out.close();
            // old pak is only removed AFTER the tmp completed: a crash here
            // leaves either a full old pak or a full tmp + old pak (tmp wins
            // nothing — it's simply rewritten next time)
            std::error_code ec;
            std::filesystem::remove(kPakPath, ec);
            std::filesystem::rename(kPakTmp, kPakPath, ec);
            if (!ec) {
                g_pakIndex = std::move(fresh);
                SKSE::log::info("[ICONS] pak compacted ({} icons)", g_pakIndex.size());
            } else {
                // B1: the old pak is already GONE here — keeping the old
                // index would point future appends/reads at wrong offsets in
                // a brand-new file (silently reading other items' pixels).
                // Drop the index and force a rescan of whatever survives.
                g_pakIndex.clear();
                g_pakScanned = false;
                SKSE::log::error("[ICONS] pak compaction rename failed ({}) — "
                                 "index dropped, rescanning", ec.message());
            }
        }

        void ScanPak()
        {
            if (g_pakScanned) return;
            g_pakScanned = true;
            g_pakIndex.clear();

            std::uint64_t fileSize = 0;
            {
                std::ifstream in(kPakPath, std::ios::binary);
                if (in) {
                    in.seekg(0, std::ios::end);
                    fileSize = static_cast<std::uint64_t>(in.tellg());
                    in.seekg(0);
                    while (true) {
                        const std::uint64_t off = static_cast<std::uint64_t>(in.tellg());
                        std::uint32_t magic = 0, w = 0, h = 0, fmt = 0, len = 0;
                        std::uint64_t key = 0;
                        float rot[9] = {};
                        if (!in.read(reinterpret_cast<char*>(&magic), 4) ||
                            (magic != kIconMagic && magic != kIconMagicRot &&
                                magic != kIconMagicRot6) ||
                            !in.read(reinterpret_cast<char*>(&key), 8) ||
                            !in.read(reinterpret_cast<char*>(&w), 4) ||
                            !in.read(reinterpret_cast<char*>(&h), 4) ||
                            !in.read(reinterpret_cast<char*>(&fmt), 4)) {
                            break;   // clean EOF or truncated tail — stop
                        }
                        const bool rotField = magic != kIconMagic;
                        const bool hasRot = magic == kIconMagicRot;   // v7 only
                        if (rotField &&
                            !in.read(reinterpret_cast<char*>(rot), 36)) {
                            break;
                        }
                        if (!in.read(reinterpret_cast<char*>(&len), 4)) {
                            break;
                        }
                        const std::uint64_t hdr = rotField ? kPakHdrSizeRot : kPakHdrSize;
                        if (w == 0 || h == 0 || w > 2048 || h > 2048 ||
                            len != w * h * 4 || off + hdr + len > fileSize) {
                            break;   // corrupt record: drop it and the tail
                        }
                        PakEntry en{ off, w, h, fmt, len };
                        en.rotOnDisk = rotField;
                        if (hasRot) {
                            en.hasRot = true;
                            std::memcpy(en.rot, rot, 36);
                        }
                        g_pakIndex[key] = en;
                        in.seekg(static_cast<std::streamoff>(off + hdr + len));
                    }
                }
            }

            // one-time migration: absorb the legacy per-item .fic directory
            // (stale-version files are deleted without absorbing — re-render)
            std::error_code ec;
            if (std::filesystem::exists(kIconDir, ec)) {
                std::vector<std::filesystem::path> files;
                for (const auto& e : std::filesystem::directory_iterator(kIconDir, ec)) {
                    if (e.is_regular_file() && e.path().extension() == ".fic") {
                        files.push_back(e.path());
                    }
                }
                int absorbed = 0;
                for (const auto& f : files) {
                    std::uint64_t key = 0;
                    try {
                        key = std::stoull(f.stem().string(), nullptr, 16);
                    } catch (...) {
                        continue;
                    }
                    {
                        std::ifstream in(f, std::ios::binary);
                        std::uint32_t magic = 0, w = 0, h = 0, fmt = 0;
                        if (in.read(reinterpret_cast<char*>(&magic), 4) &&
                            magic == kIconMagic &&
                            in.read(reinterpret_cast<char*>(&w), 4) &&
                            in.read(reinterpret_cast<char*>(&h), 4) &&
                            in.read(reinterpret_cast<char*>(&fmt), 4) &&
                            w > 0 && h > 0 && w <= 2048 && h <= 2048) {
                            std::vector<std::uint8_t> px(static_cast<size_t>(w) * h * 4);
                            if (in.read(reinterpret_cast<char*>(px.data()),
                                    static_cast<std::streamsize>(px.size()))) {
                                if (AppendToPakRaw(key, w, h, fmt, px.data(),
                                        static_cast<std::uint32_t>(px.size()))) {
                                    ++absorbed;
                                }
                            }
                        }
                    }
                    std::filesystem::remove(f, ec);
                }
                std::filesystem::remove(kIconDir, ec);   // only succeeds when empty
                if (absorbed) {
                    SKSE::log::info("[ICONS] migrated {} legacy .fic files into the pak", absorbed);
                }
                std::ifstream in(kPakPath, std::ios::binary | std::ios::ate);
                if (in) fileSize = static_cast<std::uint64_t>(in.tellg());
            }

            // compact when superseded records exceed 30% (and 8+ MB)
            std::uint64_t live = 0;
            for (const auto& [key, en] : g_pakIndex) live += en.hdrSize() + en.len;
            if (fileSize > live) {
                const std::uint64_t dead = fileSize - live;
                if (dead * 10 >= fileSize * 3 && dead > (8ull << 20)) {
                    SKSE::log::info("[ICONS] pak compaction: {} MB dead of {} MB",
                        dead >> 20, fileSize >> 20);
                    CompactPak();
                }
            }

            SKSE::log::info("[ICONS] pak scanned: {} icons", g_pakIndex.size());
        }

        // ---- low-poly pak (read-only) ----
        std::unordered_map<std::uint64_t, PakEntry> g_lpIndex;
        bool g_lpScanned = false;

        // v5-records-only reader: no migration, no compaction, no writes —
        // the file is authored by the offline tool ("last key wins" like the
        // capture pak, so a re-import just appends)
        void ScanLpPak()
        {
            if (g_lpScanned) return;
            g_lpScanned = true;
            g_lpIndex.clear();

            std::ifstream in(kLpPakPath, std::ios::binary);
            if (!in) return;
            in.seekg(0, std::ios::end);
            const std::uint64_t fileSize = static_cast<std::uint64_t>(in.tellg());
            in.seekg(0);
            while (true) {
                const std::uint64_t off = static_cast<std::uint64_t>(in.tellg());
                std::uint32_t magic = 0, w = 0, h = 0, fmt = 0, len = 0;
                std::uint64_t key = 0;
                if (!in.read(reinterpret_cast<char*>(&magic), 4) ||
                    magic != kIconMagic ||
                    !in.read(reinterpret_cast<char*>(&key), 8) ||
                    !in.read(reinterpret_cast<char*>(&w), 4) ||
                    !in.read(reinterpret_cast<char*>(&h), 4) ||
                    !in.read(reinterpret_cast<char*>(&fmt), 4) ||
                    !in.read(reinterpret_cast<char*>(&len), 4)) {
                    break;   // clean EOF or truncated tail
                }
                if (w == 0 || h == 0 || w > 2048 || h > 2048 ||
                    len != w * h * 4 || off + kPakHdrSize + len > fileSize) {
                    break;   // corrupt record: drop it and the tail
                }
                g_lpIndex[key] = PakEntry{ off, w, h, fmt, len };
                in.seekg(static_cast<std::streamoff>(off + kPakHdrSize + len));
            }
            SKSE::log::info("[ICONS] low-poly pak scanned: {} icons", g_lpIndex.size());
        }
    }

    // Silhouette glow sprite (rarity halo, plan A): downscale the sprite's
    // alpha into a small padded canvas, dilate + blur, upload as a white
    // texture the tile draw tints per rarity. Built once per icon at cache
    // time (capture harvest or disk load) — never per frame. ~20KB each;
    // the padding lets the halo bleed OUTSIDE the icon rect.
    static constexpr int kGlowCoreSize = 48;   // long side of the downscaled alpha
    static constexpr int kGlowPadPx    = 12;   // halo margin each side (glow px)

    static void BuildGlowSprite(ID3D11Device* a_device, const std::uint8_t* a_rgba,
                                int a_w, int a_h, IconCache::Icon& a_icon)
    {
        if (!a_device || !a_rgba || a_w <= 0 || a_h <= 0) return;
        const int longSide = (std::max)(a_w, a_h);
        const int cw = (std::max)(1, a_w * kGlowCoreSize / longSide);
        const int ch = (std::max)(1, a_h * kGlowCoreSize / longSide);
        const int gw = cw + kGlowPadPx * 2;
        const int gh = ch + kGlowPadPx * 2;

        std::vector<float> a(static_cast<size_t>(gw) * gh, 0.0f);
        std::vector<float> b(a.size(), 0.0f);
        auto at = [&](const std::vector<float>& v, int x, int y) -> float {
            if (x < 0 || y < 0 || x >= gw || y >= gh) return 0.0f;
            return v[static_cast<size_t>(y) * gw + x];
        };

        // box-average downsample of the alpha channel into the padded canvas
        for (int y = 0; y < ch; ++y) {
            const int sy0 = y * a_h / ch;
            const int sy1 = (std::max)(sy0 + 1, (y + 1) * a_h / ch);
            for (int x = 0; x < cw; ++x) {
                const int sx0 = x * a_w / cw;
                const int sx1 = (std::max)(sx0 + 1, (x + 1) * a_w / cw);
                float sum = 0.0f;
                for (int sy = sy0; sy < sy1; ++sy) {
                    for (int sx = sx0; sx < sx1; ++sx) {
                        sum += a_rgba[(static_cast<size_t>(sy) * a_w + sx) * 4 + 3];
                    }
                }
                a[static_cast<size_t>(y + kGlowPadPx) * gw + (x + kGlowPadPx)] =
                    sum / static_cast<float>((sy1 - sy0) * (sx1 - sx0));
            }
        }

        // dilate r=1 (thin blades gain enough body to glow), then 2x
        // separable box blur r=4 — a gaussian-like falloff wide enough to
        // read as a soft halo once stretched onto the tile
        for (int y = 0; y < gh; ++y) {
            for (int x = 0; x < gw; ++x) {
                float m = 0.0f;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        m = (std::max)(m, at(a, x + dx, y + dy));
                    }
                }
                b[static_cast<size_t>(y) * gw + x] = m;
            }
        }
        a.swap(b);
        constexpr int kR = 4;
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < gh; ++y) {
                for (int x = 0; x < gw; ++x) {
                    float s = 0.0f;
                    for (int d = -kR; d <= kR; ++d) s += at(a, x + d, y);
                    b[static_cast<size_t>(y) * gw + x] = s / (kR * 2 + 1);
                }
            }
            a.swap(b);
            for (int y = 0; y < gh; ++y) {
                for (int x = 0; x < gw; ++x) {
                    float s = 0.0f;
                    for (int d = -kR; d <= kR; ++d) s += at(a, x, y + d);
                    b[static_cast<size_t>(y) * gw + x] = s / (kR * 2 + 1);
                }
            }
            a.swap(b);
        }

        // gain: plateau near 1 under the silhouette so the draw tint's alpha
        // alone sets the perceived strength; the blur tail is the falloff
        std::vector<std::uint8_t> px(static_cast<size_t>(gw) * gh * 4);
        for (size_t i = 0; i < a.size(); ++i) {
            const float v = (std::min)(255.0f, a[i] * 2.6f);
            px[i * 4 + 0] = 255;
            px[i * 4 + 1] = 255;
            px[i * 4 + 2] = 255;
            px[i * 4 + 3] = static_cast<std::uint8_t>(v + 0.5f);
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = static_cast<UINT>(gw);
        td.Height           = static_cast<UINT>(gh);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem     = px.data();
        init.SysMemPitch = static_cast<UINT>(gw * 4);

        ID3D11Texture2D* tex = nullptr;
        if (FAILED(a_device->CreateTexture2D(&td, &init, &tex))) return;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(a_device->CreateShaderResourceView(tex, nullptr, &srv))) {
            tex->Release();
            return;
        }
        a_icon.glowTex = tex;
        a_icon.glowSrv = srv;
        a_icon.gw      = gw;
        a_icon.gh      = gh;
        a_icon.gpad    = kGlowPadPx;
    }

    // pixels -> GPU texture (+ optional silhouette glow) — shared by the pak
    // loader and the loose-file loader (slot silhouettes, torn frames)
    static bool CreateIconTexture(const std::uint8_t* a_px, int a_w, int a_h,
                                  std::uint32_t a_fmt, bool a_makeGlow,
                                  IconCache::Icon& a_out)
    {
        auto* data = RE::BSGraphics::Renderer::GetRendererData();
        if (!data) return false;
        auto* device = reinterpret_cast<ID3D11Device*>(data->forwarder);
        if (!device) return false;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = static_cast<UINT>(a_w);
        td.Height           = static_cast<UINT>(a_h);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = static_cast<DXGI_FORMAT>(a_fmt);
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem     = a_px;
        init.SysMemPitch = static_cast<UINT>(a_w * 4);

        IconCache::Icon icon;
        icon.w = a_w;
        icon.h = a_h;
        if (FAILED(device->CreateTexture2D(&td, &init, &icon.tex))) return false;
        if (FAILED(device->CreateShaderResourceView(icon.tex, nullptr, &icon.srv))) {
            icon.tex->Release();
            return false;
        }
        if (a_makeGlow) {
            BuildGlowSprite(device, a_px, a_w, a_h, icon);
        }
        a_out = icon;
        return true;
    }

    IconCache* IconCache::GetSingleton()
    {
        static IconCache singleton;
        return std::addressof(singleton);
    }

    IconDef IconCache::ResolveDef(RE::TESBoundObject* a_obj) const
    {
        // deliberately UNTOUCHED by inspect: the drag rotation is injected at
        // the Request site only, so tile keys / Get() / the pak stay identical
        // whether or not something is being inspected
        return m_resolver ? m_resolver(a_obj) : IconDef{};
    }

    void IconCache::ReleaseIcon(Icon& a_icon)
    {
        if (a_icon.srv) a_icon.srv->Release();
        if (a_icon.tex) a_icon.tex->Release();
        if (a_icon.glowSrv) a_icon.glowSrv->Release();
        if (a_icon.glowTex) a_icon.glowTex->Release();
        a_icon = Icon{};
    }

    void IconCache::SetInspect(RE::TESBoundObject* a_obj, float a_rx, float a_ry, float a_rz)
    {
        if (!a_obj) {
            ClearInspect();
            return;
        }
        m_inspect = a_obj;
        SetInspectRot(a_rx, a_ry, a_rz);
        m_inspectValid = false;   // nothing captured yet ("caching" for a frame)
        // no pin, no cache key: PreRender simply gives the preview to this item
        // while the overlay is open, and the result lands in m_inspectIcon
        ItemPreview::GetSingleton()->SetInspectScale(kInspectModelScale);
    }

    void IconCache::SetInspectRot(float a_rx, float a_ry, float a_rz)
    {
        m_inspectDef.rx = a_rx;
        m_inspectDef.ry = a_ry;
        m_inspectDef.rz = a_rz;
    }

    void IconCache::ClearInspect()
    {
        if (!m_inspect) return;
        m_inspect = nullptr;
        ItemPreview::GetSingleton()->SetInspectScale(0.0f);
        m_inspectValid = false;   // stop drawing it immediately
        m_inspectRetire = true;   // ...free it next Tick (see ProcessDeferredRelease)
    }

    void IconCache::ProcessDeferredRelease()
    {
        if (!m_inspectRetire) return;
        m_inspectRetire = false;
        ReleaseIcon(m_inspectIcon);
    }

    // Rotation only (whole-degree quantised): scale is a DRAW-time zoom
    // and must not force a re-capture or split cache keys.
    static std::uint32_t RotHash(const IconDef& a_def)
    {
        return static_cast<std::uint32_t>(static_cast<int>(a_def.rx)) * 73856093u ^
               static_cast<std::uint32_t>(static_cast<int>(a_def.ry)) * 19349663u ^
               static_cast<std::uint32_t>(static_cast<int>(a_def.rz)) * 83492791u;
    }

    // Model-shared slot: FNV-1a of the normalised world-model path. Items
    // sharing one nif render identically, so they share one capture
    // (measured: 10,711 records over 2,143 unique models = 80% duplicates).
    // Items with ALTERNATE TEXTURES (same nif, different pixels) and items
    // without a model path keep the per-FormID slot. A stale hit would need a
    // full 64-bit collision incl. the rotation hash — effectively impossible.
    static std::uint32_t ModelSlot32(RE::TESBoundObject* a_obj)
    {
        const char* p = nullptr;
        std::uint32_t altCount = 0;
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            // armor is NOT a TESModel (skyrim_cast returns null — that made
            // every ARMO fall back to per-FormID keys): its GND model lives
            // on TESBipedModelForm::worldModels
            const auto& wm = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale];
            p = wm.GetModel();
            altCount = wm.numAlternateTextures;
        } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
            p = mdl->GetModel();
            if (const auto* swap = skyrim_cast<RE::TESModelTextureSwap*>(a_obj)) {
                altCount = swap->numAlternateTextures;
            }
        }
        if (!p || !*p) return a_obj->GetFormID();
        if (altCount > 0) return a_obj->GetFormID();   // same nif, other pixels
        const char* s = p;
        if (_strnicmp(s, "meshes", 6) == 0 && (s[6] == '\\' || s[6] == '/')) {
            s += 7;
        }
        std::uint32_t h = 2166136261u;
        for (; *s; ++s) {
            char c = *s;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c == '/') c = '\\';
            h = (h ^ static_cast<std::uint8_t>(c)) * 16777619u;
        }
        return h;
    }

    // Display-only forms (Hearthfire build-menu previews under \Interface\,
    // furniture markers): named MISC forms the player can never obtain —
    // precaching them wastes ~314 captures of house walls/roofs.
    static bool IsDisplayOnlyModel(RE::TESBoundObject* a_obj)
    {
        const char* p = nullptr;
        if (auto* armo = a_obj->As<RE::TESObjectARMO>()) {
            p = armo->worldModels[RE::TESBipedModelForm::Sexes::kMale].GetModel();
        } else if (const auto* mdl = skyrim_cast<RE::TESModel*>(a_obj)) {
            p = mdl->GetModel();
        }
        if (!p || !*p) return false;
        std::string s(p);
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c == '/') c = '\\';
        }
        return s.find("\\interface\\") != std::string::npos ||
               s.find("marker") != std::string::npos;
    }

    std::uint64_t IconCache::KeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const
    {
        return (static_cast<std::uint64_t>(ModelSlot32(a_obj)) << 32) | RotHash(a_def);
    }

    std::uint64_t IconCache::LegacyKeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const
    {
        return (static_cast<std::uint64_t>(a_obj->GetFormID()) << 32) | RotHash(a_def);
    }

    const IconCache::Icon* IconCache::Get(RE::TESBoundObject* a_obj) const
    {
        if (!a_obj) return nullptr;
        // low-poly style wins per model slot (rotation-independent). The
        // PINNED editor item is exempt so live rotation edits stay visible.
        // LAZY LOAD here, not just in QueueCapture: items whose realistic
        // icon is already cached never reach QueueCapture (Get returns it),
        // so a style toggle would otherwise never pull their low-poly
        // sprite off disk. One disk probe per slot per session (m_lpTried).
        if (m_lowPoly && a_obj != m_pin) {
            const auto slot = ModelSlot32(a_obj);
            auto lp = m_lpIcons.find(slot);
            if (lp == m_lpIcons.end() && !m_lpTried.contains(slot)) {
                auto* self = const_cast<IconCache*>(this);
                self->m_lpTried.insert(slot);
                if (self->LoadLowPolyFromDisk(slot)) {
                    lp = m_lpIcons.find(slot);
                }
            }
            if (lp != m_lpIcons.end()) return &lp->second;
        }
        const IconDef def = ResolveDef(a_obj);
        auto it = m_icons.find(KeyFor(a_obj, def));
        if (it == m_icons.end()) {
            it = m_icons.find(LegacyKeyFor(a_obj, def));   // pre-migration pak
        }
        if (it != m_icons.end()) return &it->second;

        // live edit: while the current key's capture is in flight, keep
        // showing the pinned item's latest completed capture (no flicker)
        if (a_obj == m_pin && m_pinLastKey) {
            const auto it2 = m_icons.find(m_pinLastKey);
            if (it2 != m_icons.end()) return &it2->second;
        }
        return nullptr;
    }

    void IconCache::SetPin(RE::TESBoundObject* a_obj)
    {
        if (m_pin && m_pin != a_obj && m_pinLastKey && !m_pinSprite.empty()) {
            // deferred disk write: only the FINAL edited sprite hits disk
            // (a drag would otherwise write one file per degree)
            SaveToDisk(m_pinLastKey, m_pinW, m_pinH, m_pinFmt, m_pinSprite);
        }
        if (m_pin != a_obj) {
            m_pinLastKey = 0;
            m_pinSprite.clear();
        }
        m_pin = a_obj;
    }

    bool IconCache::LoadFicTexture(const std::string& a_path, Icon& a_out,
                                   std::uint32_t a_exactMagic, bool a_makeGlow)
    {
        std::ifstream in(a_path, std::ios::binary);
        if (!in) return false;

        std::uint32_t magic = 0, w = 0, h = 0, fmt = 0;
        in.read(reinterpret_cast<char*>(&magic), 4);
        in.read(reinterpret_cast<char*>(&w), 4);
        in.read(reinterpret_cast<char*>(&h), 4);
        in.read(reinterpret_cast<char*>(&fmt), 4);
        const bool magicOk = a_exactMagic ? magic == a_exactMagic
                                          : (magic & 0x00FFFFFFu) == 0x00434946u;   // "FIC?"
        if (!in || !magicOk || w == 0 || h == 0 || w > 2048 || h > 2048) return false;

        std::vector<std::uint8_t> pixels(static_cast<size_t>(w) * h * 4);
        in.read(reinterpret_cast<char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
        if (!in) return false;

        return CreateIconTexture(pixels.data(), static_cast<int>(w),
            static_cast<int>(h), fmt, a_makeGlow, a_out);
    }

    bool IconCache::LoadFromDisk(std::uint64_t a_key)
    {
        ScanPak();
        const auto it = g_pakIndex.find(a_key);
        if (it == g_pakIndex.end()) return false;

        std::ifstream in(kPakPath, std::ios::binary);
        if (!in) return false;
        in.seekg(static_cast<std::streamoff>(it->second.off + it->second.hdrSize()));
        std::vector<std::uint8_t> px(it->second.len);
        if (!in.read(reinterpret_cast<char*>(px.data()),
                static_cast<std::streamsize>(px.size()))) {
            return false;
        }

        Icon icon;
        if (!CreateIconTexture(px.data(), static_cast<int>(it->second.w),
                static_cast<int>(it->second.h), it->second.fmt, true, icon)) {
            return false;
        }
        m_icons[a_key] = icon;
        return true;
    }

    bool IconCache::LoadLowPolyFromDisk(std::uint32_t a_slot)
    {
        ScanLpPak();
        const auto it = g_lpIndex.find(static_cast<std::uint64_t>(a_slot) << 32);
        if (it == g_lpIndex.end()) return false;

        std::ifstream in(kLpPakPath, std::ios::binary);
        if (!in) return false;
        in.seekg(static_cast<std::streamoff>(it->second.off + kPakHdrSize));
        std::vector<std::uint8_t> px(it->second.len);
        if (!in.read(reinterpret_cast<char*>(px.data()),
                static_cast<std::streamsize>(px.size()))) {
            return false;
        }

        Icon icon;
        if (!CreateIconTexture(px.data(), static_cast<int>(it->second.w),
                static_cast<int>(it->second.h), it->second.fmt, true, icon)) {
            return false;
        }
        m_lpIcons[a_slot] = icon;
        return true;
    }

    void IconCache::SetLowPolyStyle(bool a_on)
    {
        if (a_on && !m_lowPoly) {
            // re-index on enable so a tool re-import during this session is
            // picked up (index rebuild only — no SRVs are touched, so this
            // is safe inside the ImGui frame; already-loaded sprites keep
            // their old pixels until the next menu Clear)
            g_lpScanned = false;
            m_lpTried.clear();   // let previous misses retry on the new index
        }
        m_lowPoly = a_on;
    }

    void IconCache::SaveToDisk(std::uint64_t a_key, int a_w, int a_h, std::uint32_t a_fmt,
                               const std::vector<std::uint8_t>& a_pixels)
    {
        ScanPak();   // index/migration must exist before the first append
        AppendToPakRaw(a_key, static_cast<std::uint32_t>(a_w),
            static_cast<std::uint32_t>(a_h), a_fmt, a_pixels.data(),
            static_cast<std::uint32_t>(a_pixels.size()));
    }

    // THREADING NOTE (B11): g_pakIndex/g_pakOut are unsynchronized. This is
    // safe because every access path runs on the MAIN thread — Tick (player
    // Update hook / AdvanceMovie) and PostDisplay (menu render) never overlap.
    // If a worker thread ever touches the pak, add a mutex here first.
    void IconCache::ResetDiskCache()
    {
        // NEVER call from inside the ImGui frame: Clear() releases SRVs the
        // current draw list may still reference (consumed from UIRoot::Tick)
        Clear();
        g_pakIndex.clear();
        g_pakScanned = true;   // stays empty until new captures append
        ClosePakHandle();      // the remove below fails on an open handle
        std::error_code ec;
        std::filesystem::remove(kPakPath, ec);
        std::filesystem::remove(kPakTmp, ec);
        std::filesystem::remove(kFailPath, ec);      // failed keys get a fresh chance
        std::filesystem::remove_all(kIconDir, ec);   // legacy leftovers too
        SKSE::log::info("[ICONS] disk cache reset (retexture refresh)");
    }

    bool IconCache::ExportPakTo(const char* a_path)
    {
        ClosePakHandle();   // flush pending appends before the copy
        std::error_code ec;
        if (!std::filesystem::exists(kPakPath, ec)) {
            // no captures yet: make sure no STALE bundle rides along either
            std::filesystem::remove(a_path, ec);
            SKSE::log::info("[ICONS] preset export: no capture pak to bundle");
            return false;
        }
        std::filesystem::copy_file(kPakPath, a_path,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            SKSE::log::error("[ICONS] preset pak copy failed: {}", ec.message());
            return false;
        }
        SKSE::log::info("[ICONS] preset pak bundled ({} keys)", g_pakIndex.size());
        return true;
    }

    bool IconCache::MergePak(const char* a_path)
    {
        std::ifstream in(a_path, std::ios::binary);
        if (!in) return false;
        // sanity: the stream must OPEN with a known record magic, or we would
        // append garbage the scanner then trips over
        std::uint32_t magic = 0;
        if (!in.read(reinterpret_cast<char*>(&magic), 4) ||
            (magic != kIconMagic && magic != kIconMagicRot &&
             magic != kIconMagicRot6)) {
            SKSE::log::error("[ICONS] preset pak rejected (bad magic)");
            return false;
        }
        in.seekg(0);
        ClosePakHandle();
        {
            std::ofstream out(kPakPath, std::ios::binary | std::ios::app);
            if (!out) return false;
            out << in.rdbuf();
        }
        // frame-outside only: SRVs drop here, tiles reload lazily from the
        // merged pak (same visual as a cache reset, minus the re-captures)
        Clear();
        g_pakScanned = false;
        ScanPak();
        SKSE::log::info("[ICONS] preset pak merged -> {} keys indexed",
            g_pakIndex.size());
        return true;
    }

    void IconCache::EnsureFailLoaded()
    {
        if (m_failLoaded) return;
        m_failLoaded = true;
        std::ifstream in(kFailPath);
        if (!in) return;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == ';') continue;
            const auto key = std::strtoull(line.c_str(), nullptr, 16);
            if (key) {
                m_failed.insert(key);
                ++n;
            }
        }
        if (n) {
            SKSE::log::info("[ICONS] {} permanently-failed capture keys loaded", n);
        }
    }

    void IconCache::PersistFail(std::uint64_t a_key)
    {
        std::ofstream out(kFailPath, std::ios::app);
        if (!out) return;
        if (out.tellp() == 0) {
            out << "; Capture keys that permanently failed - never retried across sessions\n";
            out << "; 캡처가 계속 실패한 키 목록 - 재접속 후에도 재시도하지 않습니다 (캐시 초기화 시 함께 삭제)\n";
        }
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%016llX\n",
            static_cast<unsigned long long>(a_key));
        out << buf;
    }

    namespace
    {
        // Leveled-item stubs (unresolved LVLI entries inside merchant chests /
        // containers) have no model; feeding one to Inv3D::Load makes the
        // engine's NewInventoryMenuItemLoadTask deref a null model and CTD.
        // The grid never draws them (name/playable filtered), but Prefetch
        // walks raw GetInventory() output — gate every queue entry here.
        bool Capturable(RE::TESBoundObject* a_obj)
        {
            return a_obj && !a_obj->Is(RE::FormType::LeveledItem);
        }
    }

    void IconCache::QueueCapture(RE::TESBoundObject* a_obj)
    {
        if (!Capturable(a_obj)) return;
        // low-poly style: a covered slot needs no realistic work at all —
        // uncovered slots fall through to the normal capture/load path
        // (that fallback is what keeps a partial low-poly set seamless)
        if (m_lowPoly && a_obj != m_pin) {
            const auto slot = ModelSlot32(a_obj);
            if (m_lpIcons.contains(slot) || LoadLowPolyFromDisk(slot)) return;
        }
        const auto key = KeyFor(a_obj, ResolveDef(a_obj));

        // live edit: a drag produces a new key every frame — drop the stale
        // backlog so only the LATEST value gets captured (no lag-behind)
        if (a_obj == m_pin) {
            for (auto it = m_queue.begin(); it != m_queue.end();) {
                if (it->obj == a_obj && it->key != key) {
                    m_queued.erase(it->key);
                    it = m_queue.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (m_icons.contains(key) || m_queued.contains(key)) return;
        EnsureFailLoaded();
        if (m_failed.contains(key)) return;   // gave up on this one — stay out
        if (m_pendingBusy && m_pending.key == key) return;

        // Session-persistent icons: load from disk before spending any
        // engine renders (the slow path runs once per item, ever).
        if (LoadFromDisk(key)) return;
        // pre-migration pak entries (FormID keys) satisfy the item too
        if (const auto legacy = LegacyKeyFor(a_obj, ResolveDef(a_obj));
            legacy != key && (m_icons.contains(legacy) || LoadFromDisk(legacy))) {
            return;
        }

        m_queue.push_back({ a_obj, a_obj->GetFormID(), key });
        m_queued.insert(key);
    }

    void IconCache::Prefetch(RE::TESBoundObject* a_obj, bool a_evictAfter)
    {
        if (!Capturable(a_obj)) return;
        // B5: the on-disk checks below need the pak index — without this,
        // a precache that runs before the first LoadFromDisk saw an empty
        // index and re-queued every icon already on disk
        ScanPak();
        const IconDef def = ResolveDef(a_obj);
        const auto key = KeyFor(a_obj, def);
        if (m_icons.contains(key) || m_queued.contains(key)) return;
        EnsureFailLoaded();
        if (m_failed.contains(key)) return;
        if (m_pendingBusy && m_pending.key == key) return;
        // already on disk: NO texture upload here — a grid that actually
        // draws the item loads it lazily via the normal QueueCapture path
        // (legacy FormID-keyed entries from the pre-model-key pak count too)
        if (g_pakIndex.contains(key)) return;
        if (const auto legacy = LegacyKeyFor(a_obj, def);
            legacy != key && g_pakIndex.contains(legacy)) {
            return;
        }
        m_queue.push_back({ a_obj, a_obj->GetFormID(), key, a_evictAfter });
        m_queued.insert(key);
    }

    size_t IconCache::PrecacheAll()
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return 0;
        const size_t before = m_queue.size();
        auto sweep = [&](const auto& a_arr) {
            for (auto* form : a_arr) {
                auto* obj = form ? form->template As<RE::TESBoundObject>() : nullptr;
                if (!obj) continue;
                const char* nm = obj->GetName();
                if (!nm || !nm[0]) continue;   // nameless = dummy/system form
                // guaranteed-failure guard: an empty world model never renders
                // (As<TESModel> is null for armor — its model lives on ARMA)
                if (auto* mdl = obj->template As<RE::TESModel>();
                    mdl && (!mdl->GetModel() || !mdl->GetModel()[0])) {
                    continue;
                }
                if (IsDisplayOnlyModel(obj)) continue;
                if (!obj->GetPlayable()) continue;   // hidden scripting forms
                Prefetch(obj, true);   // pak-only: VRAM stays flat
            }
        };
        sweep(dh->GetFormArray<RE::TESObjectWEAP>());
        sweep(dh->GetFormArray<RE::TESObjectARMO>());
        sweep(dh->GetFormArray<RE::TESAmmo>());
        sweep(dh->GetFormArray<RE::AlchemyItem>());
        sweep(dh->GetFormArray<RE::IngredientItem>());
        sweep(dh->GetFormArray<RE::TESObjectBOOK>());
        sweep(dh->GetFormArray<RE::TESObjectMISC>());
        sweep(dh->GetFormArray<RE::TESSoulGem>());
        sweep(dh->GetFormArray<RE::TESKey>());
        sweep(dh->GetFormArray<RE::ScrollItem>());
        const size_t queued = m_queue.size() - before;
        SKSE::log::info("[ICONS] precache: {} queued ({} already on disk)",
            queued, g_pakIndex.size());
        return queued;
    }

    void IconCache::CancelPrecache()
    {
        // visible items re-queue themselves next frame via the grid draw
        m_queue.clear();
        m_queued.clear();
    }

    void IconCache::PreRender()
    {
        auto* pv = ItemPreview::GetSingleton();
        if (!pv->IsRunning()) return;

        // INSPECT owns the preview while the overlay is open: it re-arms every
        // frame (continuous live view at the dragged rotation) and its result
        // goes to m_inspectIcon, so the tile queue is simply paused — no keys,
        // no disk, nothing of the icon cache is touched.
        if (m_inspect && !m_pendingBusy) {
            m_pending = Pending{ m_inspect, m_inspect ? m_inspect->GetFormID() : 0u, 0 };
            m_pendingInspect = true;
            m_pendingBusy = true;
            m_frames = 0;
        }
        if (!m_pendingBusy) {
            while (!m_queue.empty()) {
                const Pending p = m_queue.front();
                m_queue.pop_front();
                m_queued.erase(p.key);
                // The form may have been destroyed while this entry waited (a
                // brewed potion whose save was reloaded). Re-resolve by id and
                // drop the entry rather than touching freed memory.
                if (p.id != 0 &&
                    RE::TESForm::LookupByID<RE::TESBoundObject>(p.id) != p.obj) {
                    continue;
                }
                if (!m_icons.contains(p.key)) {
                    m_pending = p;
                    m_pendingInspect = false;
                    m_pendingBusy = true;
                    m_frames = 0;
                    break;
                }
            }
        }
        if (!m_pendingBusy) {
            // translucent skins: lingering models sit at the park point in
            // PLAIN SIGHT (no opaque window covers them). Once the queue
            // drains, purge stragglers that resisted Unload (pre-landing
            // Unload is a no-op). Throttled: ResetScene is End3D+Begin3D.
            if (Theme::S().translucent && m_queue.empty()) {
                static int s_idleFrames = 0;
                if (pv->SceneModelCount() > 0) {
                    // a deferred reset (load in flight) keeps the counter hot
                    // so it retries every frame until it lands
                    if (++s_idleFrames > 30 && pv->ResetScene()) {
                        s_idleFrames = 0;
                    }
                } else {
                    s_idleFrames = 0;
                }
            }
            return;
        }

        m_stampBefore = pv->GetCaptureStamp();
        IconDef def = ResolveDef(m_pending.obj);
        // the drag rotation is injected HERE and nowhere else — ResolveDef
        // itself stays clean, so tile keys never move during an inspect
        if (m_pendingInspect) {
            def.rx = m_inspectDef.rx;
            def.ry = m_inspectDef.ry;
            def.rz = m_inspectDef.rz;
        }
        // Always capture at the STANDARD crop — def.scale is applied when the
        // tile draws (linear, instant, no capture-boundary nonlinearities).
        // Inspect asks for a bigger box to match its enlarged model.
        const float boxPx = m_pendingInspect ? kInspectRequestSize : kIconRequestSize;
        pv->Request(m_pending.obj, ImVec2(0.0f, 0.0f),
            ImVec2(boxPx, boxPx), -1.0f, 0.0f, 0.0f, &def);
        if (m_pending.boost > 0.0f) {
            pv->BoostCapture(m_pending.boost);   // B4: resume the clip-boost ladder
        }
    }

    void IconCache::GiveUpPending(const char* a_why)
    {
        constexpr int kMaxAttempts = 4;
        SKSE::log::warn("[ICONS] '{}' skipped ({})", m_pending.obj->GetName(), a_why);
        ItemPreview::GetSingleton()->UnloadCurrent();
        m_pendingBusy = false;
        // an inspect frame carries no cache key (0): its failures must never
        // reach the attempt counters or the PERSISTED fail list
        if (m_pendingInspect) return;
        // repeat offender -> permanent skip, PERSISTED across sessions
        // (mod items whose preview never renders retried every relog)
        if (++m_attempts[m_pending.key] >= kMaxAttempts) {
            if (m_failed.insert(m_pending.key).second) {
                PersistFail(m_pending.key);
            }
        }
    }

    // Phase 3: PostRender stage 1 — every wait/abandon decision before the
    // pixel pipeline runs (body moved verbatim from the old front half).
    IconCache::GateResult IconCache::CheckPendingGates()
    {
        constexpr int kMaxAttempts = 4;
        auto* pv = ItemPreview::GetSingleton();

        // INSPECT: a live view, not a cache fill — no requeue, no attempt
        // counting, and never the PERSISTED fail list (its "key" is 0, which
        // is not a cache key at all). A stalled frame just leaves the previous
        // capture on screen; releasing the slot lets PreRender re-arm.
        if (m_pendingInspect) {
            if (m_frames > kTimeoutFrames) {
                m_pendingBusy = false;
                return GateResult::kAbandoned;
            }
            if (pv->GetCaptureStamp() == m_stampBefore) return GateResult::kNotReady;
            auto* mdl = pv->FindCurrentModel();
            if (!mdl || mdl->worldBound.radius <= 0.0f) return GateResult::kNotReady;
            if (!pv->RotationApplied()) return GateResult::kNotReady;
            return GateResult::kReady;
        }

        // Precache entries WAIT IN PLACE instead of requeueing: a requeue is
        // Unload+reLoad, which throws away the in-flight async model load and
        // restarts it — the main "stall cluster" during a mass precache. One
        // generous window, then give up for good (single attempt).
        if (m_pending.evict && m_frames > 45) {
            // DIAGNOSTIC: which gate starved? (stamp = captures ran at all,
            // model/rot = scene state, content probe logs separately below)
            auto* dmdl = pv->FindCurrentModel();
            SKSE::log::warn(
                "[ICONS] precache gates '{}': model={} radius={:.1f} rot={} stamp={}",
                m_pending.obj->GetName(), dmdl != nullptr,
                dmdl ? dmdl->worldBound.radius : -1.0f,
                pv->RotationApplied(), pv->GetCaptureStamp());
            if (m_failed.insert(m_pending.key).second) {   // no second chance,
                PersistFail(m_pending.key);                // relogs included
            }
            GiveUpPending("precache timeout");
            return GateResult::kAbandoned;
        }

        // Soft skip: a straggler must NOT stall the whole queue (3 stalled
        // items once accounted for 6s of a 6.5s first render). Requeue it at
        // the back quickly and keep moving; it retries after the scene state
        // has moved on. Hard skip only after several attempts.
        constexpr int kSoftFrames = 20;   // ~0.33s: 3x the normal load latency
        if (!m_pending.evict && m_frames > kSoftFrames) {
            const int tries = ++m_attempts[m_pending.key];
            if (tries < kMaxAttempts) {
                SKSE::log::info("[ICONS] '{}' slow - requeued (attempt {})",
                    m_pending.obj->GetName(), tries);
                // B4: keep any clip-boost progress — the next Request resets
                // the boost on item change, which restarted the 1.6x ladder
                // and made oversized items time out into the permanent
                // fail list before ever reaching a big enough box
                m_pending.boost = pv->CaptureBoost();
                pv->UnloadCurrent();
                if (auto* inv = RE::Inventory3DManager::GetSingleton();
                    inv && inv->GetRuntimeData().loadedModels.size() >= 5) {
                    pv->ResetScene();
                }
                m_queue.push_back(m_pending);
                m_queued.insert(m_pending.key);
                m_pendingBusy = false;
                return GateResult::kAbandoned;
            }
            GiveUpPending("timeout");
            return GateResult::kAbandoned;
        }

        // A capture must have completed THIS frame.
        if (pv->GetCaptureStamp() == m_stampBefore) return GateResult::kNotReady;

        // Accept only when OUR item's model is render-ready (the recentre in
        // ItemPreview::Render also keys off the matching entry, so the crop is
        // centred correctly even while stale async loads are still landing).
        auto* pvModel = pv->FindCurrentModel();
        if (!pvModel || pvModel->worldBound.radius <= 0.0f) {
            // not landed yet — the soft-skip above (frames > 20) requeues
            // before any deeper recovery could ever trigger here
            return GateResult::kNotReady;
        }

        // Landing-frame race: the engine stomps the node with its default
        // pose after our rotation ran, and that first capture would bake the
        // wrong orientation into the cache PERMANENTLY (square diagonal-sword
        // icons). Accept only once the node carries the requested rotation —
        // UpdateParking re-applies it next frame.
        if (!pv->RotationApplied()) return GateResult::kNotReady;

        return GateResult::kReady;
    }

    void IconCache::PostRender()
    {
        if (!m_pendingBusy) return;
        ++m_frames;

        auto* pv = ItemPreview::GetSingleton();

        // stage 1: timeouts / soft-skip / capture readiness gates
        if (CheckPendingGates() != GateResult::kReady) return;

        // stage 2+: pixel pipeline (readback -> trim/sprite -> persist). Kept
        // in one body: its locals (crop rect, mapped rows, trim bounds) flow
        // straight through — splitting them would only add plumbing structs.
        auto giveUp = [&](const char* a_why) { GiveUpPending(a_why); };

        // Pixel rect of the FULL margin region (kSafetyMargin x inner box):
        // rotation diagonals that outgrow the inner box stay uncut; tiles
        // draw the icon kSafetyMargin larger to compensate.
        ImVec2 uv0, uv1;
        pv->GetMarginUV(uv0, uv1);
        const float kTex = static_cast<float>(ItemPreview::kTexSize);
        int x0 = static_cast<int>(uv0.x * kTex);
        int y0 = static_cast<int>(uv0.y * kTex);
        int x1 = static_cast<int>(uv1.x * kTex);
        int y1 = static_cast<int>(uv1.y * kTex);

        const auto cap = pv->GetCapturedSize();
        x0 = (std::max)(0, x0);
        y0 = (std::max)(0, y0);
        x1 = (std::min)(x1, static_cast<int>(cap.x));
        y1 = (std::min)(y1, static_cast<int>(cap.y));
        const int w = x1 - x0;
        const int h = y1 - y0;
        if (w <= 0 || h <= 0) { giveUp("empty crop"); return; }

        auto* srcTex = pv->GetTexture();
        auto* data = RE::BSGraphics::Renderer::GetRendererData();
        if (!srcTex || !data) { giveUp("no texture"); return; }
        auto* device  = reinterpret_cast<ID3D11Device*>(data->forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        if (!device || !context) { giveUp("no device"); return; }

        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcTex->GetDesc(&srcDesc);

        // Content gate + alpha-trim: read the whole margin region back, find
        // the model's true pixel bounds, and store ONLY that rect. A sprite
        // built this way can never bake in clipping, no matter how far
        // rotation/scale pushed the projection. (Empty read = the engine has
        // not drawn yet — retry next frame; timeout still bounds us.)
        std::vector<std::uint8_t> pixels;
        int trimX = 0, trimY = 0, trimW = 0, trimH = 0;
        {
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width            = static_cast<UINT>(w);
            sd.Height           = static_cast<UINT>(h);
            sd.MipLevels        = 1;
            sd.ArraySize        = 1;
            sd.Format           = srcDesc.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage            = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

            ID3D11Texture2D* staging = nullptr;
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) { giveUp("stage fail"); return; }

            D3D11_BOX cbox = {};
            cbox.left   = static_cast<UINT>(x0);
            cbox.top    = static_cast<UINT>(y0);
            cbox.front  = 0;
            cbox.right  = static_cast<UINT>(x1);
            cbox.bottom = static_cast<UINT>(y1);
            cbox.back   = 1;
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, srcTex, 0, &cbox);

            int nonBg = 0;
            int minX = w, minY = h, maxX = -1, maxY = -1;
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                pixels.resize(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; ++y) {
                    const auto* row = static_cast<const std::uint8_t*>(map.pData) +
                                      static_cast<size_t>(y) * map.RowPitch;
                    std::memcpy(pixels.data() + static_cast<size_t>(y) * w * 4, row,
                        static_cast<size_t>(w) * 4);
                    for (int x = 0; x < w; ++x) {
                        // content = anything that is not chroma magenta
                        // (symmetric in R/B, so BGRA vs RGBA never matters)
                        const bool bg = row[x * 4 + 0] > 200 && row[x * 4 + 2] > 200 &&
                                        row[x * 4 + 1] < 60;
                        if (!bg) {
                            ++nonBg;
                            minX = (std::min)(minX, x);
                            maxX = (std::max)(maxX, x);
                            minY = (std::min)(minY, y);
                            maxY = (std::max)(maxY, y);
                        }
                    }
                }
                context->Unmap(staging, 0);
            }
            staging->Release();

#ifdef GI_CAPTURE_DIAG
            // DIAGNOSTIC: is the capture rect empty magenta (engine drew
            // nothing) or does it hold content that some later gate rejects?
            if (m_pending.evict && m_frames == 30) {
                SKSE::log::info("[ICONS] content probe '{}': nonBg={} rect={}x{}",
                    m_pending.obj->GetName(), nonBg, w, h);
            }
#endif
            if (nonBg < 40) return;   // engine hasn't drawn yet — retry next frame

            // Clipped capture: content touches the margin edge, i.e. the
            // engine rendered the model larger than the box (per-record
            // oversize like Moth Priest Robes — invisible on worldBound).
            // Grow the box and recapture instead of baking a cropped sprite.
            if (minX <= 0 || minY <= 0 || maxX >= w - 1 || maxY >= h - 1) {
                const ImVec2 cover = pv->CaptureCover();
                const float cur = (std::max)(cover.x, cover.y);
                if (cur < static_cast<float>(ItemPreview::kTexSize) - 1.0f) {
                    pv->BoostCapture(cur * 1.6f);
                    SKSE::log::info("[ICONS] '{}' clipped at {}x{} — retry with {:.0f}px box",
                        m_pending.obj->GetName(), w, h, cur * 1.6f);
                    return;
                }
            }

            trimX = (std::max)(0, minX - 2);
            trimY = (std::max)(0, minY - 2);
            trimW = (std::min)(w - 1, maxX + 2) - trimX + 1;
            trimH = (std::min)(h - 1, maxY + 2) - trimY + 1;
        }

        // Trim + chroma key into the final sprite buffer. Keyed pixels get
        // RGB zeroed so bilinear sampling can't bleed magenta at draw time.
        std::vector<std::uint8_t> sprite(static_cast<size_t>(trimW) * trimH * 4);
        for (int y = 0; y < trimH; ++y) {
            const auto* src = pixels.data() + (static_cast<size_t>(trimY + y) * w + trimX) * 4;
            auto* dst = sprite.data() + static_cast<size_t>(y) * trimW * 4;
            std::memcpy(dst, src, static_cast<size_t>(trimW) * 4);
            for (int x = 0; x < trimW; ++x) {
                const bool key = dst[x * 4 + 0] > 200 && dst[x * 4 + 2] > 200 &&
                                 dst[x * 4 + 1] < 60;
                if (key) {
                    dst[x * 4 + 0] = 0;
                    dst[x * 4 + 1] = 0;
                    dst[x * 4 + 2] = 0;
                    dst[x * 4 + 3] = 0;
                } else {
                    dst[x * 4 + 3] = 255;
                }
            }
        }

        // Defringe: anti-aliased EDGE pixels are model+magenta blends that
        // survive the strict key and show as a purple halo. Only for pixels
        // touching transparency (interior purples — potions, enchant glows —
        // stay untouched): strip the magenta cast (min(R,B)-G) and fade alpha
        // by the spill amount.
        {
            std::vector<std::uint8_t> mask(static_cast<size_t>(trimW) * trimH);
            for (int i = 0; i < trimW * trimH; ++i) mask[i] = sprite[i * 4 + 3];
            auto alphaAt = [&](int x, int y) -> std::uint8_t {
                if (x < 0 || y < 0 || x >= trimW || y >= trimH) return 0;
                return mask[static_cast<size_t>(y) * trimW + x];
            };
            for (int y = 0; y < trimH; ++y) {
                for (int x = 0; x < trimW; ++x) {
                    auto* px = sprite.data() + (static_cast<size_t>(y) * trimW + x) * 4;
                    if (px[3] == 0) continue;
                    const bool edge =
                        alphaAt(x - 1, y) == 0 || alphaAt(x + 1, y) == 0 ||
                        alphaAt(x, y - 1) == 0 || alphaAt(x, y + 1) == 0;
                    if (!edge) continue;
                    const int spill = (std::min)(static_cast<int>(px[0]),
                                          static_cast<int>(px[2])) - px[1];
                    if (spill <= 0) continue;
                    px[0] = static_cast<std::uint8_t>(px[0] - spill);
                    px[2] = static_cast<std::uint8_t>(px[2] - spill);
                    px[3] = static_cast<std::uint8_t>((std::max)(0, 255 - spill * 2));
                }
            }
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width            = static_cast<UINT>(trimW);
        td.Height           = static_cast<UINT>(trimH);
        td.MipLevels        = 1;
        td.ArraySize        = 1;
        td.Format           = srcDesc.Format;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem     = sprite.data();
        init.SysMemPitch = static_cast<UINT>(trimW * 4);

        Icon icon;
        icon.w = trimW;
        icon.h = trimH;
        if (FAILED(device->CreateTexture2D(&td, &init, &icon.tex))) { giveUp("tex fail"); return; }
        if (FAILED(device->CreateShaderResourceView(icon.tex, nullptr, &icon.srv))) {
            icon.tex->Release();
            giveUp("srv fail");
            return;
        }
        // precache (evict) captures skip the glow sprite: it is CPU-built
        // (dilate+blur) and would be released unused a few lines below
        if (!m_pending.evict) {
            BuildGlowSprite(device, sprite.data(), trimW, trimH, icon);
        }

        // (v7's capture-time geometry rotation record was removed: the offline
        // tool no longer needs it — its projection/root-transform bugs were
        // fixed at the source. Reading old v6/v7 pak entries stays supported;
        // all NEW writes are plain v5.)

        // INSPECT: its own slot, keyed by nothing, never persisted. Replacing
        // the previous texture here is as safe as the pin recycle below —
        // PostRender runs BEFORE UIRoot::Render builds this frame's draw list.
        if (m_pendingInspect) {
            if (m_inspect) {
                ReleaseIcon(m_inspectIcon);
                m_inspectIcon = icon;
                m_inspectValid = true;
            } else {
                // closed mid-capture: this texture was never drawn — drop it
                ReleaseIcon(icon);
            }
            m_pendingBusy = false;
            return;
        }

        m_icons[m_pending.key] = icon;
        m_attempts.erase(m_pending.key);   // success: forget prior soft-skips
        if (m_pending.obj == m_pin) {
            // live edit: keep ONE slot — drop the previous intermediate
            // texture and defer the disk write until the pin moves/clears
            if (m_pinLastKey && m_pinLastKey != m_pending.key) {
                if (auto old = m_icons.find(m_pinLastKey); old != m_icons.end()) {
                    if (old->second.srv) old->second.srv->Release();
                    if (old->second.tex) old->second.tex->Release();
                    if (old->second.glowSrv) old->second.glowSrv->Release();
                    if (old->second.glowTex) old->second.glowTex->Release();
                    m_icons.erase(old);
                }
            }
            m_pinLastKey = m_pending.key;
            m_pinSprite  = std::move(sprite);
            m_pinW = trimW;
            m_pinH = trimH;
            m_pinFmt = static_cast<std::uint32_t>(srcDesc.Format);
        } else {
            SaveToDisk(m_pending.key, trimW, trimH,
                static_cast<std::uint32_t>(srcDesc.Format), sprite);
            SKSE::log::info("[ICONS] cached '{}' {}x{} ({} total, {} queued)",
                m_pending.obj->GetName(), trimW, trimH, m_icons.size(), m_queue.size());
            // mass precache: keep only the pak copy — the icon was created
            // this call and never drawn, so releasing it here is safe. A grid
            // that later shows the item reloads it from disk on demand.
            if (m_pending.evict) {
                if (auto ev = m_icons.find(m_pending.key); ev != m_icons.end()) {
                    if (ev->second.srv) ev->second.srv->Release();
                    if (ev->second.tex) ev->second.tex->Release();
                    if (ev->second.glowSrv) ev->second.glowSrv->Release();
                    if (ev->second.glowTex) ev->second.glowTex->Release();
                    m_icons.erase(ev);
                }
            }
        }
        m_pendingBusy = false;

        // Unload NOW, after the model landed — unloading before it lands is a
        // no-op and lets the 7-slot array fill up (the timeout root cause).
        // Exception: the editor's pinned item stays loaded so rotation edits
        // re-capture within a few frames (no reload round-trip).
        // translucent skins also unload the pinned item: keeping it loaded
        // parks a visible model at screen centre for the whole EDIT session.
        // Cost: rotation edits pay a reload round-trip (~a few frames).
        // Precache (evict) keeps the model loaded instead: the same-nif fast
        // path in ItemPreview::Request then chains enchanted variants without
        // any reload (a different-nif Request unloads it, and the idle purge
        // clears the tail once the queue drains).
        if (!m_pending.evict && (m_pending.obj != m_pin || Theme::S().translucent)) {
            pv->UnloadCurrent();
        }
    }

    void IconCache::OnRevert()
    {
        // Never touch m_pending.obj here -- if it went stale that dereference is
        // the very crash this exists to prevent. Drop the request by flag only.
        if (m_pendingBusy) ItemPreview::GetSingleton()->UnloadCurrent();
        m_pendingBusy = false;
        m_pendingInspect = false;
        m_pending = Pending{};
        m_queue.clear();
        m_queued.clear();
    }

    void IconCache::Clear()
    {
        for (auto& [key, icon] : m_icons) ReleaseIcon(icon);
        for (auto& [slot, icon] : m_lpIcons) ReleaseIcon(icon);
        ReleaseIcon(m_inspectIcon);
        m_inspectValid = false;
        m_inspectRetire = false;
        m_pendingInspect = false;
        m_lpIcons.clear();
        m_lpTried.clear();
        m_icons.clear();
        m_queue.clear();
        m_queued.clear();
        m_attempts.clear();
        m_failed.clear();
        m_failLoaded = false;   // persisted fail keys reload on next access
        m_pendingBusy = false;
        m_pinLastKey = 0;
        m_pinSprite.clear();
    }
}
