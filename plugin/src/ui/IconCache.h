#pragma once

#include "ui/ItemDef.h"

#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace FUI
{
    // B-2: per-item icon cache (PLAN_B §2-G4). Each queued item is run once
    // through ItemPreview's backbuffer capture; the model-centred crop is
    // copied into an item-owned texture whose SRV lives for the process
    // lifetime, so re-opening the inventory shows icons instantly with no
    // further engine renders.
    class IconCache
    {
    public:
        struct Icon
        {
            ID3D11ShaderResourceView* srv = nullptr;
            ID3D11Texture2D*          tex = nullptr;
            int                       w = 0;
            int                       h = 0;
            // silhouette rarity halo: this icon's alpha channel downscaled,
            // dilated and blurred — white RGB, tinted per rarity at draw time.
            // gpad px of padding is baked around the core on each side so the
            // halo can bleed OUTSIDE the icon rect. May be null (slot
            // silhouettes / creation failure) — callers must check.
            ID3D11ShaderResourceView* glowSrv = nullptr;
            ID3D11Texture2D*          glowTex = nullptr;
            int                       gw = 0;     // glow canvas incl. padding
            int                       gh = 0;
            int                       gpad = 0;   // margin baked on each side
        };

        static IconCache* GetSingleton();

        // Generic .fic (raw RGBA) loader — also used for slot silhouettes.
        // a_exactMagic 0 accepts any "FIC?" version (version-agnostic assets);
        // the icon cache passes its exact magic so stale captures re-render.
        // a_makeGlow additionally builds the silhouette halo sprite (item
        // icons only — slot silhouettes never glow).
        static bool LoadFicTexture(const std::string& a_path, Icon& a_out,
                                   std::uint32_t a_exactMagic = 0,
                                   bool a_makeGlow = false);

        // main.cpp installs this: item -> capture orientation (category preset
        // + user ini override). The def is part of the cache key, so editing a
        // def naturally triggers a re-capture.
        using DefResolver = std::function<IconDef(RE::TESBoundObject*)>;
        void SetDefResolver(DefResolver a_resolver) { m_resolver = std::move(a_resolver); }
        [[nodiscard]] IconDef ResolveDef(RE::TESBoundObject* a_obj) const;

        // Enqueue an item for capture (no-op if cached or already queued).
        void QueueCapture(RE::TESBoundObject* a_obj);

        // Prefetch (B): queue a capture WITHOUT touching the GPU — items
        // already in the disk pak are skipped by index (their texture uploads
        // lazily when a grid actually draws them). a_evictAfter (mass
        // precache) keeps only the pak copy after capturing: VRAM stays flat.
        void Prefetch(RE::TESBoundObject* a_obj, bool a_evictAfter = false);

        // Precache (C): queue every nameable inventory form in the load
        // order (weapons/armor/potions/books/misc/...). Returns queued count.
        size_t PrecacheAll();
        void   CancelPrecache();   // drop the queue (current capture finishes)

        // Editor pin: keep this item's model loaded between captures so a
        // rotation edit re-captures every frame (live editing). While pinned,
        // captures replace a single in-memory slot and skip the disk write —
        // the FINAL sprite is flushed to disk when the pin moves/clears.
        void SetPin(RE::TESBoundObject* a_obj);

        [[nodiscard]] bool        IsBusy() const { return m_pendingBusy || !m_queue.empty(); }
        [[nodiscard]] const Icon* Get(RE::TESBoundObject* a_obj) const;   // resolves def internally
        [[nodiscard]] size_t      CachedCount() const { return m_icons.size(); }
        [[nodiscard]] size_t      QueuedCount() const { return m_queue.size() + (m_pendingBusy ? 1 : 0); }

        // Per-frame hooks, called from GridInventoryMenu::PostDisplay:
        void PreRender();   // BEFORE ItemPreview::Render — request the pending item
        void PostRender();  // AFTER  ItemPreview::Render — harvest the capture

        void Clear();  // release all cached textures

        // Save-load boundary. Clear() is far too heavy here (it would drop every
        // texture and trigger a re-capture storm on each load); the ONLY thing a
        // revert invalidates is work still queued against forms the engine is
        // about to destroy and re-create. Cached textures are keyed by model
        // slot, so they stay valid across the load.
        void OnRevert();

        // ICON CACHE reset (settings): drop every texture AND the disk pak —
        // for users who install retexture mods after icons were captured.
        // Call OUTSIDE the ImGui frame only (UIRoot::Tick): the current draw
        // list may still reference the SRVs being released.
        // The LOW-POLY pak is authored content, not cache — never touched.
        void ResetDiskCache();

        // GI47: preset icon bundle. Export copies our capture pak next to the
        // preset ini; import APPENDS the bundle's records behind our own --
        // the scanner's last-one-wins rule then gives the preset every shared
        // key while reader-only keys (extra mods) survive untouched.
        // MergePak drops the live textures so tiles reload lazily from the
        // merged pak: NEVER call it inside the ImGui frame (UIRoot::Tick only).
        bool ExportPakTo(const char* a_path);
        bool MergePak(const char* a_path);

        // Icon style (two-pak): OFF = realistic auto-captures (default).
        // ON = the tool-authored GridInventory_icons_lowpoly.pak wins per
        // model slot; items it doesn't cover fall back to realistic, so the
        // grid always looks complete while the low-poly set is in progress.
        // The auto-capture pipeline never writes the low-poly pak.
        void SetLowPolyStyle(bool a_on);
        [[nodiscard]] bool LowPolyStyle() const { return m_lowPoly; }

        // INSPECT mode (C key = vanilla "Item Zoom"): one item is captured
        // every frame at a mouse-driven rotation and drawn large, so the player
        // can read model detail the icon can't show (dragon-claw glyphs).
        //
        // FULLY SEPARATE from the icon cache: the capture lands in its own
        // texture slot, keyed by nothing, never persisted, and the item's
        // m_icons entry is never read, written or dropped. An inspect
        // therefore CANNOT change or lose the tile's icon — the earlier
        // rotation-key sharing is what caused exactly that class of bug.
        void SetInspect(RE::TESBoundObject* a_obj, float a_rx, float a_ry, float a_rz);
        void SetInspectRot(float a_rx, float a_ry, float a_rz);
        void ClearInspect();
        [[nodiscard]] bool IsInspecting() const { return m_inspect != nullptr; }
        [[nodiscard]] const Icon* InspectIcon() const
        {
            return m_inspectValid ? &m_inspectIcon : nullptr;
        }
        // Frees the retired inspect texture. Releases an SRV -> call from
        // UIRoot::Tick (outside the ImGui frame) only: ClearInspect can run
        // mid-frame, when this frame's draw list still references it.
        void ProcessDeferredRelease();

    private:
        IconCache() = default;

        // Phase 3: PostRender's front half — timeouts / soft-skip / capture &
        // model / rotation gates. kReady = harvest the capture this frame.
        enum class GateResult : std::uint8_t
        {
            kNotReady,    // wait: try again next frame (pending stays busy)
            kAbandoned,   // gave up / requeued: pending released
            kReady,
        };
        GateResult CheckPendingGates();

        // giveUp MECHANISM (callers own the policy of when): warn, unload,
        // release the pending slot, and escalate repeat offenders to the
        // PERSISTED permanent-fail list.
        void GiveUpPending(const char* a_why);

        // Capture request size: generous (~512px capture region) — the stored
        // icon is ALPHA-TRIMMED to the model's true pixel bounds afterwards,
        // so clipping is structurally impossible and files stay tight.
        static constexpr float kIconRequestSize = 290.0f;
        // inspect: engine item-scale multiplier (the capture rect auto-grows
        // to the projected bound, clamped to kTexSize, so this stays modest)
        // and a matching minimum capture box for the enlarged model
        static constexpr float kInspectModelScale  = 2.0f;
        static constexpr float kInspectRequestSize = 560.0f;

        static void ReleaseIcon(Icon& a_icon);   // free SRV/tex (+ glow)
        static constexpr int   kTimeoutFrames   = 180;

        // Cache key = MODEL HASH + quantised rotation hash: items sharing one
        // GND nif (enchant variants, keys, notes — 80% of all records) share
        // ONE capture. Items with alternate textures keep per-form keys.
        [[nodiscard]] std::uint64_t KeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const;
        // pre-model-key pak entries (FormID-based) stay readable via this
        [[nodiscard]] std::uint64_t LegacyKeyFor(RE::TESBoundObject* a_obj, const IconDef& a_def) const;

        // Disk cache: an item is captured once EVER (per def); later sessions
        // load the pixels straight from disk — no engine renders at all.
        bool LoadFromDisk(std::uint64_t a_key);
        // low-poly pak entry for this model slot -> m_lpIcons (read-only pak)
        bool LoadLowPolyFromDisk(std::uint32_t a_slot);
        static void SaveToDisk(std::uint64_t a_key, int a_w, int a_h, std::uint32_t a_fmt,
                               const std::vector<std::uint8_t>& a_pixels);

        struct Pending
        {
            RE::TESBoundObject* obj = nullptr;
            // The queue outlives a menu close, and a RUNTIME-CREATED form (a
            // brewed potion, a player enchantment) is destroyed and re-made with
            // a new FormID when a save loads -- so `obj` can go stale while an
            // entry waits its turn. Keep the id to re-validate before any use.
            RE::FormID          id = 0;
            std::uint64_t       key = 0;
            bool                evict = false;   // precache: pak-only, free the GPU copy
            float               boost = 0.0f;    // B4: clip-boost carried across requeues
        };

        DefResolver                            m_resolver;
        std::unordered_map<std::uint64_t, Icon> m_icons;
        // low-poly style: tool-authored sprites, keyed by model slot alone
        // (rotation-independent — hand-drawn art has a fixed composition).
        // m_lpTried = slots probed against the pak this session, hit or miss
        // (Get() lazy-loads; without it every uncovered tile would re-read
        // the pak index every frame)
        std::unordered_map<std::uint32_t, Icon> m_lpIcons;
        std::unordered_set<std::uint32_t>       m_lpTried;
        bool                                    m_lowPoly = false;
        std::deque<Pending>                    m_queue;
        std::unordered_set<std::uint64_t>      m_queued;    // membership for m_queue
        std::unordered_map<std::uint64_t, int> m_attempts;  // soft-skip retry counts
        // permanently skipped this session: items whose capture keeps failing
        // (e.g. mod items with no inventory model) — without this the visible
        // grid re-queues them every frame and the caching spinner never ends
        std::unordered_set<std::uint64_t>      m_failed;
        bool                                   m_failLoaded = false;

        void EnsureFailLoaded();               // lazy read of the persisted list
        void PersistFail(std::uint64_t a_key); // append one permanently-failed key
        Pending                                m_pending;
        RE::TESBoundObject*                    m_pin = nullptr;   // editor selection
        RE::TESBoundObject*                    m_inspect = nullptr;   // C-key overlay
        IconDef                                m_inspectDef{};        // live drag rotation
        Icon                                   m_inspectIcon{};       // its OWN texture
        bool                                   m_inspectValid = false;
        bool                                   m_inspectRetire = false;   // free on Tick
        bool                                   m_pendingInspect = false;  // in-flight kind
        bool                                   m_pendingBusy = false;
        int                                    m_frames = 0;
        std::uint32_t                          m_stampBefore = 0;

        // live-edit slot: latest completed capture of the pinned item (drawn
        // as a fallback while newer keys are still in flight -> no flicker)
        std::uint64_t             m_pinLastKey = 0;
        std::vector<std::uint8_t> m_pinSprite;   // pixels for the deferred disk write
        int                       m_pinW = 0;
        int                       m_pinH = 0;
        std::uint32_t             m_pinFmt = 0;
    };
}
