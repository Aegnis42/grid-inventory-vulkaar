#pragma once

#include "ui/ItemDef.h"

#include <imgui.h>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace FUI
{
    // IconDef (capture orientation: rx/ry/rz/scale) is an alias of the ONE
    // shared FUI::ItemDef (Phase 2 D2) — see ui/ItemDef.h.

    // Ported from ModExplorerMenu (Modex) by patchulidev — Item3DPreview.
    // https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
    //
    // Captures the engine's own Inventory3DManager render (the vanilla
    // inventory preview pipeline) off the backbuffer during the UI render
    // pass, and exposes it to ImGui as a shader resource view. This is the
    // only path that renders skinned meshes (bows, spell tome pages).
    class ItemPreview
    {
    public:
        static constexpr unsigned int kTexSize = 2048;

        // Margin applied to prevent clipping capture bounds
        static constexpr float kSafetyMargin = 1.5f;

        // Fallback model scale when Request() is passed a negative scale
        static constexpr float kDefaultModelScale = 0.85f;

        // Background painted behind the model before capture: chroma magenta,
        // keyed to transparency by IconCache so tall/wide icons can overflow
        // their footprint without covering neighbouring cells. Symmetric in
        // R/B, so BGRA-vs-RGBA channel order never matters.
        static constexpr float kCaptureBg[4] = { 1.0f, 0.0f, 1.0f, 1.0f };

        static ItemPreview* GetSingleton();

        void Begin();
        void End();
        void Render();

        void Request(RE::TESBoundObject* a_item, ImVec2 a_screenPos, ImVec2 a_screenSize,
                     float a_modelScale = -1.0f, float a_offsetX = 0.0f, float a_offsetY = 0.0f,
                     const IconDef* a_def = nullptr);

        // Called from the game-update hook (BEFORE the frame renders): applies
        // the def orientation and parks the model as soon as it lands, so the
        // engine's own on-screen draw never shows it at the default position.
        void Tick();

        ImVec2 GetCapturedSize() const { return m_lastCapturedSize; }

        // Full safety-margin region around the model centre (kSafetyMargin x
        // the inner box). IconCache stores THIS so content that outgrows the
        // inner box (rotation diagonals) never bakes in clipped; the tile
        // draw compensates by kSafetyMargin.
        void GetMarginUV(ImVec2& a_uv0, ImVec2& a_uv1) const;

        bool IsRunning() const { return m_running; }

        // IconCache support: raw capture texture and a monotonically
        // increasing stamp (bumped on every completed capture).
        ID3D11Texture2D*    GetTexture() const { return m_dstTex; }
        std::uint32_t       GetCaptureStamp() const { return m_captureStamp; }

        // The loadedModels entry matching m_current (async loads land late, so
        // back() can be a stale previous item). Null until render-ready.
        RE::NiAVObject* FindCurrentModel() const;

        // True when the model node's rotation matches the requested def.
        // On the landing frame the engine stomps the node with its own
        // default pose AFTER our UpdateParking ran — a capture accepted that
        // frame bakes the wrong orientation into the cache permanently.
        bool RotationApplied() const;

        void UnloadCurrent();  // unload AFTER the model landed (actually removes)
        // End3D+Begin3D recovery when loadedModels fills up (7 cap). Returns
        // false when DEFERRED: End3D derefs every entry's spModel, so a
        // not-yet-landed entry (null spModel, async load in flight) is a
        // guaranteed engine CTD — callers must retry on a later frame.
        bool ResetScene();

        // The engine ALSO draws loadedModels on screen every frame (that draw
        // IS the vanilla right-pane preview). Park the model so it projects
        // at this screen point — put it under an opaque UI window to hide it.
        void SetParkPos(ImVec2 a_screen) { m_parkPos = a_screen; m_hasPark = true; }

        // Caching-card support (translucent skins can't hide the parked model
        // behind a window): how big the capture region is, and whether ANY
        // engine model is currently on screen.
        ImVec2 CaptureCover() const { return m_captureSize; }
        int    SceneModelCount() const;

        // Clipped-capture retry: some records render far larger than the
        // standard box (engine-side per-item scale invisible on the node).
        // The capture consumer detects content touching the box edge and
        // requests a bigger box for the next attempt. Resets on item change.
        void BoostCapture(float a_px) { m_captureBoost = (std::max)(m_captureBoost, a_px); }

        // INSPECT mode (C key): the sprite is drawn several times larger than a
        // tile, so the engine must render the model bigger or the capture has
        // too few pixels to read fine detail (dragon-claw glyphs). Multiplier
        // on the engine's own item scale, re-applied every frame because the
        // engine recomputes it; 0 restores the saved value.
        void SetInspectScale(float a_mul) { m_inspectScale = a_mul; }
        // B4: the boost in effect right now — a soft-skip requeue stashes it
        // so the retry doesn't restart the 1.6x ladder from zero
        float CaptureBoost() const { return m_captureBoost; }

    private:
        ItemPreview() = default;

        bool Initialize();
        void Shutdown();
        void UpdateParking();   // apply def + move model under the park point
        // Engine scene teardown, retried via main-thread tasks until no model
        // load is in flight (see ResetScene note). Aborts when a_session no
        // longer matches (the menu reopened; the new session pairs its own
        // End3D).
        void TeardownWhenIdle(std::uint32_t a_session, int a_tries);

        bool                m_initialized = false;
        bool                m_running     = false;
        bool                m_requested   = false;
        RE::TESBoundObject* m_current     = nullptr;
        std::uint32_t       m_session     = 0;   // bumped by Begin(); guards deferred teardown

        ImVec2 m_capturePos       = ImVec2(0.0f, 0.0f);
        ImVec2 m_captureSize      = ImVec2(0.0f, 0.0f);  // full rect including safety margin
        ImVec2 m_innerSize        = ImVec2(0.0f, 0.0f);  // requested box (no margin) — capture size derives from it
        ImVec2 m_lastCapturedSize = ImVec2(0.0f, 0.0f);  // actual captured pixels after backbuffer clamp
        ImVec2 m_modelInTexture   = ImVec2(0.0f, 0.0f);  // model's centre position in captured-texture pixels

        // Per-request offset overrides applied at Render
        float m_overrideOffsetX   = 0.0f;
        float m_overrideOffsetY   = 0.0f;
        bool  m_hasOverrideOffset = false;

        ImVec2  m_parkPos = ImVec2(0.0f, 0.0f);
        bool    m_hasPark = false;
        float   m_inspectScale  = 0.0f;   // 0 = off
        float   m_savedItemScale = 0.0f;  // engine value to restore
        bool    m_hasSavedScale = false;
        float   m_savedNodeScale = 1.0f;  // model node scale to restore
        bool    m_nodeScaled = false;
        float   m_captureBoost = 0.0f;   // min capture-box side requested by BoostCapture
        IconDef m_def;   // rotation only — scale is a crop-region zoom (Request param)

        ID3D11Texture2D*          m_dstTex     = nullptr;
        ID3D11ShaderResourceView* m_dstSRV     = nullptr;
        ID3D11Texture2D*          m_scratchTex = nullptr;
        std::uint32_t             m_captureStamp = 0;
    };
}
