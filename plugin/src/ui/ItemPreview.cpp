#include "ui/ItemPreview.h"
#include "game/Inv3D.h"

#include <d3d11_1.h>

// Ported from ModExplorerMenu (Modex) by patchulidev — Item3DPreview.cpp.
// https://github.com/patchulidev/ModExplorerMenu (GPL-3.0 with Modding Exception)
// Changes vs upstream: CommonLibSSE-NG (main) accessors, REL wrappers for
// Begin3D/End3D/Load/Unload (FUI::Inv3D), theme lookups replaced by constants.

namespace FUI
{
    namespace
    {
        // End3D walks loadedModels and derefs each entry's spModel — an entry
        // whose async load has not landed yet (null spModel) or a still-queued
        // load task makes that a null deref inside the engine (observed CTD
        // during icon-editing capture churn). The scene must never be torn
        // down while this returns true.
        bool LoadInFlight(RE::Inventory3DManager* a_mgr)
        {
            auto& rt = a_mgr->GetRuntimeData();
            if (rt.loadTask) return true;
            for (auto& lm : rt.loadedModels) {
                if (!lm.spModel) return true;
            }
            return false;
        }
    }

    ItemPreview* ItemPreview::GetSingleton()
    {
        static ItemPreview singleton;
        return std::addressof(singleton);
    }

    void ItemPreview::GetMarginUV(ImVec2& a_uv0, ImVec2& a_uv1) const
    {
        const float kTex = static_cast<float>(kTexSize);
        const float effW = (std::min)(m_captureSize.x, m_lastCapturedSize.x);
        const float effH = (std::min)(m_captureSize.y, m_lastCapturedSize.y);
        const float startX = m_modelInTexture.x - effW * 0.5f;
        const float startY = m_modelInTexture.y - effH * 0.5f;
        a_uv0 = ImVec2(startX / kTex, startY / kTex);
        a_uv1 = ImVec2((startX + effW) / kTex, (startY + effH) / kTex);
    }

    bool ItemPreview::Initialize()
    {
        if (m_initialized) return true;

        auto* data = RE::BSGraphics::Renderer::GetRendererData();
        if (!data) return false;
        auto* device = reinterpret_cast<ID3D11Device*>(data->forwarder);
        if (!device) return false;

        // Source is the on-screen swap chain. Get the underlying texture.
        auto* rtv = reinterpret_cast<ID3D11RenderTargetView*>(data->renderWindows[0].renderView);
        if (!rtv) return false;

        ID3D11Resource* srcRes = nullptr;
        rtv->GetResource(&srcRes);
        if (!srcRes) return false;

        ID3D11Texture2D* srcTex = nullptr;
        srcRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
        srcRes->Release();
        if (!srcTex) return false;

        D3D11_TEXTURE2D_DESC srcDesc = {};
        srcTex->GetDesc(&srcDesc);
        srcTex->Release();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width            = kTexSize;
        desc.Height           = kTexSize;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = srcDesc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_dstTex))) return false;

        if (FAILED(device->CreateShaderResourceView(m_dstTex, nullptr, &m_dstSRV))) {
            Shutdown();
            return false;
        }

        // Scratch holds the FULL backbuffer: the capture draw paints the
        // model to the live frame without clipping to the capture rect, so a
        // rect-only restore leaves any overspill visible (oversized items
        // peeking past the caching card for the capture frame). Full
        // save/restore reverts every pixel we touched.
        D3D11_TEXTURE2D_DESC sdesc = srcDesc;
        sdesc.MipLevels      = 1;
        sdesc.ArraySize      = 1;
        sdesc.Usage          = D3D11_USAGE_DEFAULT;
        sdesc.BindFlags      = 0;
        sdesc.CPUAccessFlags = 0;
        sdesc.MiscFlags      = 0;
        if (FAILED(device->CreateTexture2D(&sdesc, nullptr, &m_scratchTex))) {
            Shutdown();
            return false;
        }

        m_initialized = true;
        return true;
    }

    void ItemPreview::Shutdown()
    {
        auto release = [](auto*& p) {
            if (p) {
                p->Release();
                p = nullptr;
            }
        };

        release(m_dstSRV);
        release(m_dstTex);
        release(m_scratchTex);
        m_initialized = false;
    }

    void ItemPreview::Begin()
    {
        if (m_running) return;

        // Default park point BEFORE anything loads: screen centre (the UI
        // window spawns there). Off-screen is impossible — the capture reads
        // the on-screen backbuffer, so the model must be on screen, hidden
        // behind the opaque window. Once the window draws, SetParkPos tracks
        // its actual centre every frame (and persists across opens).
        if (!m_hasPark) {
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            if (sz.width > 0 && sz.height > 0) {
                m_parkPos = ImVec2(static_cast<float>(sz.width) * 0.5f,
                                   static_cast<float>(sz.height) * 0.5f);
                m_hasPark = true;
            }
        }

        if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
            Inv3D::Begin3D(mgr, RE::INTERFACE_LIGHT_SCHEME::kInventory);
            m_running = true;
            ++m_session;   // cancels any teardown still deferred from the last close
            SKSE::log::info("[PREVIEW] Begin3D");
        } else {
            SKSE::log::warn("[PREVIEW] Begin: Inventory3DManager null");
        }
    }

    void ItemPreview::End()
    {
        const bool wasRunning = m_running;
        m_running   = false;
        m_requested = false;
        m_current   = nullptr;
        Shutdown();
        // Engine teardown may have to wait for an in-flight model load (menu
        // closed mid-capture) — End3D right now would be the null-spModel CTD.
        if (wasRunning) {
            TeardownWhenIdle(m_session, 0);
        }
    }

    void ItemPreview::TeardownWhenIdle(std::uint32_t a_session, int a_tries)
    {
        // the menu reopened: the NEW session owns the scene now and its own
        // End() pairs the teardown — this stale one must not fire
        if (a_session != m_session || m_running) return;
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        if (!mgr) return;
        if (LoadInFlight(mgr)) {
            if (a_tries >= 300) {
                // a load that never lands: leave the scene untouched (next
                // open/close cycle pairs End3D) rather than risk the CTD
                SKSE::log::warn("[PREVIEW] End: load stuck in flight, teardown skipped");
                return;
            }
            SKSE::GetTaskInterface()->AddTask([this, a_session, a_tries]() {
                TeardownWhenIdle(a_session, a_tries + 1);
            });
            return;
        }
        Inv3D::Unload(mgr);
        Inv3D::End3D(mgr);
        if (a_tries > 0) {
            SKSE::log::info("[PREVIEW] End3D (deferred {} tasks)", a_tries);
        }
    }

    RE::NiAVObject* ItemPreview::FindCurrentModel() const
    {
        if (!m_current) return nullptr;
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        if (!mgr) return nullptr;
        for (auto& lm : mgr->GetRuntimeData().loadedModels) {
            if ((lm.itemBase == m_current || lm.modelObj == m_current) && lm.spModel) {
                return lm.spModel.get();
            }
        }

        // Same-nif fallback: the engine DEDUPES loads that share a model file
        // (enchanted weapon variants etc.) — no new entry is created and the
        // existing one keeps the first requester as itemBase, so the exact
        // match above never fires (this stalled 3 items for 2s each). Same
        // nif = identical visual, so capturing that entry is exact.
        const auto* mdl = skyrim_cast<RE::TESModel*>(m_current);
        const char* path = mdl ? mdl->GetModel() : nullptr;
        if (path && *path) {
            for (auto& lm : mgr->GetRuntimeData().loadedModels) {
                if (!lm.spModel) continue;
                // BUGFIX (precache "model=false" cascade): the dedup entry
                // keeps the FIRST requester in itemBase — the old fallback
                // compared modelObj only, which is often null, so same-nif
                // variants never matched and every one timed out. Check both.
                for (RE::TESForm* src : { lm.itemBase,
                         static_cast<RE::TESForm*>(lm.modelObj) }) {
                    const auto* mdl2 = src ? skyrim_cast<RE::TESModel*>(src) : nullptr;
                    if (mdl2 && mdl2->GetModel() &&
                        _stricmp(mdl2->GetModel(), path) == 0) {
                        return lm.spModel.get();
                    }
                }
            }
        }
        return nullptr;
    }

    bool ItemPreview::RotationApplied() const
    {
        auto* model = FindCurrentModel();
        if (!model) return false;
        constexpr float kDeg = 0.017453292f;
        RE::NiMatrix3 want;
        want.SetEulerAnglesXYZ(m_def.rx * kDeg, m_def.ry * kDeg, m_def.rz * kDeg);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (std::fabs(want.entry[r][c] - model->local.rotate.entry[r][c]) > 0.02f) {
                    return false;
                }
            }
        }
        return true;
    }

    void ItemPreview::UnloadCurrent()
    {
        if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
            Inv3D::Unload(mgr);
        }
        m_current = nullptr;
    }

    bool ItemPreview::ResetScene()
    {
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        if (!mgr || !m_running) return false;
        if (LoadInFlight(mgr)) {
            return false;   // deferred — caller retries once the load lands
        }
        Inv3D::Unload(mgr);
        Inv3D::End3D(mgr);
        Inv3D::Begin3D(mgr, RE::INTERFACE_LIGHT_SCHEME::kInventory);
        m_current = nullptr;
        SKSE::log::info("[PREVIEW] scene reset (loadedModels was full)");
        return true;
    }

    void ItemPreview::Tick()
    {
        if (!m_running) return;
        UpdateParking();
    }

    int ItemPreview::SceneModelCount() const
    {
        if (!m_running) return 0;
        auto* mgr = RE::Inventory3DManager::GetSingleton();
        return mgr ? static_cast<int>(mgr->GetRuntimeData().loadedModels.size()) : 0;
    }

    void ItemPreview::UpdateParking()
    {
        // INSPECT zoom: push the engine's own item scale (and the node scale)
        // so the capture has real pixels to trim. Runs BEFORE the model check
        // so the restore path can never be skipped. Whichever of the two the
        // engine honours wins; if it overwrites both, the overlay just draws
        // the normal-resolution sprite upscaled (still functional).
        if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
            if (m_inspectScale > 0.0f) {
                if (!m_hasSavedScale) {
                    m_savedItemScale = mgr->itemScale;
                    m_hasSavedScale = true;
                }
                mgr->itemScale = m_savedItemScale * m_inspectScale;
            } else if (m_hasSavedScale) {
                mgr->itemScale = m_savedItemScale;
                m_hasSavedScale = false;
            }
        }

        auto* model = FindCurrentModel();
        if (!model || model->worldBound.radius <= 0.0f) return;
        // Node scale is SAVED and RESTORED, never left behind: the engine keeps
        // one node per nif and the same-nif fast path retargets it without a
        // reload, so a stray 2x would leak into later captures of that model
        // (enchant variants) — those aren't pinned, so the oversized sprite
        // would have been written to the pak permanently.
        if (m_inspectScale > 0.0f) {
            if (!m_nodeScaled) {
                m_savedNodeScale = model->local.scale;
                m_nodeScaled = true;
            }
            model->local.scale = m_savedNodeScale * m_inspectScale;
        } else if (m_nodeScaled) {
            model->local.scale = m_savedNodeScale;
            m_nodeScaled = false;
        }

        // Rotation lives on the node (the engine leaves it alone). Scale is
        // NOT applied to the engine at all — Modex's own approach: the def
        // scale shrinks/grows the CROP REGION (Request's modelScale param),
        // and the fixed-size tile stretch does the zoom. Pure 2D, no engine
        // state to fight (node/itemScale writes kept getting overwritten).
        constexpr float kDeg = 0.017453292f;
        model->local.rotate.SetEulerAnglesXYZ(m_def.rx * kDeg, m_def.ry * kDeg, m_def.rz * kDeg);
        bool dirty = true;

        if (m_hasPark) {
            auto* scn = RE::UI3DSceneManager::GetSingleton();
            const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
            if (scn && sz.width > 0 && sz.height > 0) {
                const auto& vf = scn->viewFrustum;
                const auto& t  = model->local.translate;
                const float world_minx   = -vf.fLeft * t.y;
                const float world_minz   = -vf.fBottom * t.y;
                const float world_width  = -vf.fRight * t.y - world_minx;
                const float world_height = -vf.fTop * t.y - world_minz;
                const float ratio_x = world_width / static_cast<float>(sz.width);
                const float ratio_y = world_height / static_cast<float>(sz.height);
                if (ratio_x != 0.0f && ratio_y != 0.0f) {
                    if (dirty) {   // rotation moved the bound centre: refresh first
                        RE::NiUpdateData ud;
                        model->Update(ud);
                        dirty = false;
                    }
                    const auto& c = model->worldBound.center;
                    const float model_sx = -(c.x + world_minx) / ratio_x;
                    const float model_sy = -(c.z + world_minz) / ratio_y;
                    const float dsx = m_parkPos.x - model_sx;
                    const float dsy = m_parkPos.y - model_sy;
                    if (std::fabs(dsx) > 0.5f || std::fabs(dsy) > 0.5f) {
                        // inverse of the projection: dworld = -dscreen * ratio
                        model->local.translate.x += -dsx * ratio_x;
                        model->local.translate.z += -dsy * ratio_y;
                        dirty = true;
                    }
                }
            }
        }

        if (dirty) {
            RE::NiUpdateData ud;
            model->Update(ud);
        }
    }

    void ItemPreview::Request(RE::TESBoundObject* a_item, ImVec2 a_screenPos, ImVec2 a_screenSize,
                              float a_modelScale, float a_offsetX, float a_offsetY,
                              const IconDef* a_def)
    {
        if (!m_running || a_item == nullptr) return;
        // model-less leveled-item stubs CTD inside the engine's load task —
        // last-ditch guard behind IconCache's queue-side filter
        if (a_item->Is(RE::FormType::LeveledItem)) return;

        if (a_item != m_current) m_captureBoost = 0.0f;

        // Same-nif fast path: enchanted variants share one model file, and the
        // engine DEDUPES such loads anyway — worse, a dedup onto an entry a
        // previous Unload detached renders EMPTY and the capture times out
        // (the precache "2/sec, all skipped" cascade). If the incoming item
        // uses the SAME nif as the currently loaded one, keep the model and
        // just retarget: capture accepts within a frame or two.
        if (a_item != m_current && m_current) {
            const auto* mdlNew = skyrim_cast<RE::TESModel*>(a_item);
            const auto* mdlCur = skyrim_cast<RE::TESModel*>(m_current);
            if (mdlNew && mdlCur && mdlNew->GetModel() && mdlCur->GetModel() &&
                mdlNew->GetModel()[0] != '\0' &&
                _stricmp(mdlNew->GetModel(), mdlCur->GetModel()) == 0 &&
                FindCurrentModel() != nullptr) {
                m_current = a_item;
                m_def = a_def ? *a_def : IconDef{};
            }
        }

        if (a_item != m_current) {
            if (auto* mgr = RE::Inventory3DManager::GetSingleton()) {
                // The 7-slot loadedModels array fills up with late-landing
                // async loads (Unload before landing is a no-op); when near
                // capacity a fresh Load silently fails — reset the scene first.
                if (mgr->GetRuntimeData().loadedModels.size() >= 5) {
                    ResetScene();
                }

                // Birth position: point the manager's itemPos at the park
                // point BEFORE loading, so the model never spends a single
                // frame at the engine's default on-screen spot.
                if (m_hasPark) {
                    if (auto* scn = RE::UI3DSceneManager::GetSingleton()) {
                        const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
                        float ty = mgr->itemPos.y;
                        if (ty > -1.0f) ty = -500.0f;   // uninitialised → vanilla-ish depth
                        const auto& vf = scn->viewFrustum;
                        const float world_minx = -vf.fLeft * ty;
                        const float world_minz = -vf.fBottom * ty;
                        const float ww = -vf.fRight * ty - world_minx;
                        const float wh = -vf.fTop * ty - world_minz;
                        if (sz.width > 0 && sz.height > 0 && ww != 0.0f && wh != 0.0f) {
                            const float rw = ww / static_cast<float>(sz.width);
                            const float rh = wh / static_cast<float>(sz.height);
                            RE::NiPoint3 p;
                            p.x = -m_parkPos.x * rw - world_minx;
                            p.z = -m_parkPos.y * rh - world_minz;
                            p.y = ty;
                            mgr->itemPos = p;
                            mgr->itemPosCopy = p;
                        }
                    }
                }

                Inv3D::Unload(mgr);
                Inv3D::Load(mgr, a_item, nullptr);
                SKSE::log::info("[PREVIEW] load '{}'", a_item->GetName());
            }
            m_current = a_item;
            m_def = a_def ? *a_def : IconDef{};
        } else if (a_def && (a_def->rx != m_def.rx || a_def->ry != m_def.ry ||
                             a_def->rz != m_def.rz || a_def->scale != m_def.scale)) {
            // SAME item, new def (live editing): re-apply without reloading —
            // rotation is absolute and scale is base-anchored, so this is safe
            m_def = *a_def;
        }

        const float modelScale = (a_modelScale >= 0.0f) ? a_modelScale : kDefaultModelScale;
        const float expand     = (modelScale > 0.0f) ? (1.0f / modelScale) : 1.0f;

        m_capturePos  = a_screenPos;
        m_innerSize   = ImVec2((std::max)(4.0f, a_screenSize.x * expand),
                               (std::max)(4.0f, a_screenSize.y * expand));
        m_captureSize = ImVec2(m_innerSize.x * kSafetyMargin, m_innerSize.y * kSafetyMargin);
        if (m_captureBoost > (std::max)(m_captureSize.x, m_captureSize.y)) {
            const float grown = (std::min)(m_captureBoost, static_cast<float>(kTexSize));
            m_captureSize = ImVec2(grown, grown);
            m_innerSize   = ImVec2(grown / kSafetyMargin, grown / kSafetyMargin);
        }
        m_requested   = true;

        m_hasOverrideOffset = true;
        m_overrideOffsetX   = a_offsetX;
        m_overrideOffsetY   = a_offsetY;
    }

    void ItemPreview::Render()
    {
        static int s_frame = 0;
        ++s_frame;

        const bool req = m_requested;
        m_requested = false;
        if (!m_running || !req) return;
        if (!m_initialized && !Initialize()) {
            if (s_frame % 120 == 0) SKSE::log::warn("[PREVIEW] Initialize failing");
            return;
        }

        auto* inv = RE::Inventory3DManager::GetSingleton();
        if (!inv) return;

        // Self-heal: if something (e.g. the vanilla menu's deferred teardown)
        // wiped our loaded model, reload the current item.
        {
            auto& rt = inv->GetRuntimeData();
            static int s_lastReload = 0;
            if (m_current && rt.loadedModels.empty() && s_frame - s_lastReload > 60) {
                Inv3D::Load(inv, m_current, nullptr);
                s_lastReload = s_frame;
                SKSE::log::info("[PREVIEW] self-heal reload '{}'", m_current->GetName());
            }
            if (s_frame % 120 == 0) {
                const float r = (!rt.loadedModels.empty() && rt.loadedModels.back().spModel)
                    ? rt.loadedModels.back().spModel->worldBound.radius : -1.0f;
                SKSE::log::info("[PREVIEW] state: models={} backRadius={:.1f} cur='{}'",
                    rt.loadedModels.size(), r, m_current ? m_current->GetName() : "-");
            }
        }

        // Capture backbuffer in place without translation (upstream issue #48):
        // recentre the capture rect on the model's projected screen position.
        // Parking normally ran already in Tick() (game-update hook, before the
        // frame rendered); run it again here as a safety net for the first
        // frame after landing, then recentre on the (parked) model.
        UpdateParking();
        {
            auto* scn0 = RE::UI3DSceneManager::GetSingleton();
            auto& runtime0 = inv->GetRuntimeData();
            if (scn0 && !runtime0.loadedModels.empty()) {
                // Prefer the entry matching the requested item — back() can be
                // a stale previous model while async loads are still landing.
                auto* spModel0 = FindCurrentModel();
                if (!spModel0) spModel0 = runtime0.loadedModels.back().spModel.get();
                if (spModel0 && spModel0->worldBound.radius > 0.0f) {
                    const auto& vf = scn0->viewFrustum;
                    const auto& t  = spModel0->local.translate;
                    const float world_minx   = -vf.fLeft * t.y;
                    const float world_minz   = -vf.fBottom * t.y;
                    const float world_width  = -vf.fRight * t.y - world_minx;
                    const float world_height = -vf.fTop * t.y - world_minz;

                    const auto sz = RE::BSGraphics::Renderer::GetScreenSize();
                    if (sz.width > 0 && sz.height > 0) {
                        const float ratio_x = world_width / static_cast<float>(sz.width);
                        const float ratio_y = world_height / static_cast<float>(sz.height);
                        if (ratio_x != 0.0f && ratio_y != 0.0f) {
                            // Some records make the engine render far larger
                            // than the standard box (e.g. Moth Priest Robes) —
                            // grow the capture rect to the projected bound so
                            // the alpha-trim never bakes in clipping. The trim
                            // stores only real pixels, so a larger box costs
                            // nothing for normal-sized items.
                            const float r = spModel0->worldBound.radius;
                            const float projW = 2.0f * r / std::fabs(ratio_x);
                            const float projH = 2.0f * r / std::fabs(ratio_y);
                            const float need  = (std::max)(projW, projH) * 1.05f;
                            if (need > m_captureSize.x || need > m_captureSize.y) {
                                const float grown = (std::min)(
                                    (std::max)({ need, m_captureSize.x, m_captureSize.y }),
                                    static_cast<float>(kTexSize));
                                m_captureSize = ImVec2(grown, grown);
                                m_innerSize   = ImVec2(grown / kSafetyMargin,
                                                       grown / kSafetyMargin);
                            }
                            const auto& c = spModel0->worldBound.center;
                            const float model_sx = -(c.x + world_minx) / ratio_x;
                            const float model_sy = -(c.z + world_minz) / ratio_y;
                            m_capturePos = ImVec2(
                                model_sx - m_captureSize.x * 0.5f,
                                model_sy - m_captureSize.y * 0.5f);
                        }
                    }
                }
            }
        }

        auto* data = RE::BSGraphics::Renderer::GetRendererData();
        if (!data) return;
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data->context);
        auto* rtv     = reinterpret_cast<ID3D11RenderTargetView*>(data->renderWindows[0].renderView);
        if (!context || !m_dstTex || !m_scratchTex || !rtv) return;

        ID3D11Resource* srcRes = nullptr;
        rtv->GetResource(&srcRes);
        if (!srcRes) return;
        ID3D11Texture2D* srcTex = nullptr;
        srcRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex));
        srcRes->Release();
        if (!srcTex) return;

        // Compute the clamped backbuffer rect once. Save/clear/capture/restore
        // all operate on this single box.
        const auto screenSize = RE::BSGraphics::Renderer::GetScreenSize();
        int left   = static_cast<int>(m_capturePos.x);
        int top    = static_cast<int>(m_capturePos.y);
        int width  = static_cast<int>(m_captureSize.x);
        int height = static_cast<int>(m_captureSize.y);
        if (left < 0) { width += left; left = 0; }
        if (top < 0)  { height += top; top = 0; }
        if (width <= 0 || height <= 0) { srcTex->Release(); return; }

        const int maxW = static_cast<int>(screenSize.width) - left;
        const int maxH = static_cast<int>(screenSize.height) - top;
        if (width > maxW)  width = maxW;
        if (height > maxH) height = maxH;
        if (width > static_cast<int>(kTexSize))  width = static_cast<int>(kTexSize);
        if (height > static_cast<int>(kTexSize)) height = static_cast<int>(kTexSize);
        if (width <= 0 || height <= 0) { srcTex->Release(); return; }

        D3D11_BOX box = {};
        box.left   = static_cast<UINT>(left);
        box.top    = static_cast<UINT>(top);
        box.front  = 0;
        box.right  = static_cast<UINT>(left + width);
        box.bottom = static_cast<UINT>(top + height);
        box.back   = 1;

        // Step 1 (save): stash the WHOLE backbuffer for restoration — the
        // model draw is not confined to the capture rect.
        context->CopyResource(m_scratchTex, srcTex);

        // Step 2 (clear): paint a solid background into that rect so the
        // capture catches model + background.
        {
            ID3D11DeviceContext1* ctx1 = nullptr;
            if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                    reinterpret_cast<void**>(&ctx1))) && ctx1) {
                D3D11_RECT rect = { left, top, left + width, top + height };
                ctx1->ClearView(rtv, kCaptureBg, &rect, 1);
                ctx1->Release();
            }
        }

        // Step 3 (render): engine paints the model to the backbuffer. Any
        // overspill beyond the capture rect is erased by the full-frame
        // restore in Step 5, so nothing is ever visible on screen.
        inv->Render();

        // Step 4 (capture): copy backbuffer rect → top-left of our texture.
        context->CopySubresourceRegion(m_dstTex, 0, 0, 0, 0, srcTex, 0, &box);

        // Diagnostic probe (first few captures with a loaded model): read the
        // captured rect back and count pixels that differ from the painted
        // background — proves whether inv->Render() actually drew anything.
        // Compiled out by default: a GPU readback stall on the render thread.
#ifdef GI_CAPTURE_DIAG
        {
            static int s_probes = 0;
            const auto& rt = inv->GetRuntimeData();
            const bool modelReady = !rt.loadedModels.empty() && rt.loadedModels.back().spModel &&
                                    rt.loadedModels.back().spModel->worldBound.radius > 0.0f;
            if (s_probes < 3 && modelReady) {
                ++s_probes;
                auto* data2 = RE::BSGraphics::Renderer::GetRendererData();
                auto* dev = data2 ? reinterpret_cast<ID3D11Device*>(data2->forwarder) : nullptr;
                if (dev) {
                    D3D11_TEXTURE2D_DESC sd = {};
                    srcTex->GetDesc(&sd);
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width = static_cast<UINT>(width);
                    td.Height = static_cast<UINT>(height);
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = sd.Format;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_STAGING;
                    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    ID3D11Texture2D* staging = nullptr;
                    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging))) {
                        context->CopySubresourceRegion(staging, 0, 0, 0, 0, srcTex, 0, &box);
                        D3D11_MAPPED_SUBRESOURCE map = {};
                        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) {
                            int diff = 0;
                            const int bgR = static_cast<int>(kCaptureBg[0] * 255.0f);
                            const int bgG = static_cast<int>(kCaptureBg[1] * 255.0f);
                            const int bgB = static_cast<int>(kCaptureBg[2] * 255.0f);
                            for (int y = 0; y < height; y += 4) {
                                const auto* row = static_cast<const std::uint8_t*>(map.pData) +
                                                  static_cast<size_t>(y) * map.RowPitch;
                                for (int x = 0; x < width; x += 4) {
                                    const int b0 = row[x * 4 + 0], b1 = row[x * 4 + 1], b2 = row[x * 4 + 2];
                                    if (std::abs(b0 - bgR) > 12 && std::abs(b0 - bgB) > 12) { ++diff; continue; }
                                    if (std::abs(b1 - bgG) > 12) { ++diff; continue; }
                                    if (std::abs(b2 - bgR) > 12 && std::abs(b2 - bgB) > 12) ++diff;
                                }
                            }
                            context->Unmap(staging, 0);
                            SKSE::log::info(
                                "[PREVIEW] probe: rect=({},{}) {}x{} nonBg(sampled)={} radius={:.1f}",
                                left, top, width, height, diff,
                                rt.loadedModels.back().spModel->worldBound.radius);
                        }
                        staging->Release();
                    }
                }
            }
        }
#endif

        // Step 5 (restore): write the whole saved frame back — erases the
        // clear rect AND every model pixel, including overspill beyond the
        // capture rect, so nothing is ever visible on screen.
        context->CopyResource(srcTex, m_scratchTex);

        srcTex->Release();

        m_lastCapturedSize = ImVec2(static_cast<float>(width), static_cast<float>(height));

        // Where the model's projected centre lands inside the texture
        const float model_cx = m_capturePos.x + m_captureSize.x * 0.5f;
        const float model_cy = m_capturePos.y + m_captureSize.y * 0.5f;
        m_modelInTexture = ImVec2(model_cx - static_cast<float>(left),
                                  model_cy - static_cast<float>(top));

        if (m_hasOverrideOffset) {
            m_modelInTexture.x -= m_overrideOffsetX;
            m_modelInTexture.y -= m_overrideOffsetY;
        }

        ++m_captureStamp;
    }
}
