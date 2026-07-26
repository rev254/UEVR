#pragma once

#include <mutex>
#include <vector>
#include <wrl.h>

#include "../../Mod.hpp"

#include <SafetyHook.hpp>

#include <sdk/RHICommandList.hpp>
#include <sdk/StereoStuff.hpp>

namespace sdk {
class FRenderTargetPool;
class FPooledRenderTargetDesc;
}

class RenderTargetPoolHook : public ModComponent {
public:
    RenderTargetPoolHook();
    void on_pre_engine_tick(sdk::UGameEngine* engine, float delta) override;
    void activate() {
        m_wants_activate = true;
    }

    IPooledRenderTarget* get_render_target(const std::wstring& name) {
        std::scoped_lock _{m_mutex};
        if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
            return it->second;
        }

        return nullptr;
    }

    // Diagnostic: lists every render target name UEVR has actually observed going
    // through the pool. Added to confirm/correct the "SceneVelocity" name guess used
    // for AFW's native motion-vector fallback (Respawn's engine fork has renamed
    // things elsewhere, e.g. the RsActor-prefixed classes found in Jedi Survivor's
    // pawn hierarchy) - if the expected name isn't present, this list shows what
    // actually is.
    std::vector<std::wstring> get_seen_render_target_names() {
        std::scoped_lock _{m_mutex};
        return std::vector<std::wstring>(m_seen_names.begin(), m_seen_names.end());
    }

    template<typename T>
    Microsoft::WRL::ComPtr<T> get_texture(const std::wstring& name) {
        std::scoped_lock _{m_mutex};
        if (auto it = m_render_targets.find(name); it != m_render_targets.end()) {
            const auto& rt = it->second;
            const auto& tex = rt->item.texture.texture;

            if (tex == nullptr) {
                return nullptr;
            }

            auto native_resource = (T*)tex->get_native_resource();

            if (native_resource == nullptr) {
                return nullptr;
            }

            return native_resource;
        }

        return nullptr;
    }

private:
    bool hook();

    // Stuff past name param is added in newer UE versions.
    static bool find_free_element_hook(
        sdk::FRenderTargetPool* pool, 
        sdk::FRHICommandListBase* cmd_list,
        sdk::FPooledRenderTargetDesc* desc,
        TRefCountPtr<IPooledRenderTarget>* out,
        const wchar_t* name,
        // these arent uintptrs, just defending against future changes to the size of the params
        uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10);

    static bool find_free_element_hook_ue5(
        sdk::FRenderTargetPool* pool,
        sdk::FPooledRenderTargetDesc* desc,
        TRefCountPtr<IPooledRenderTarget>* out,
        const wchar_t* name,
        // these arent uintptrs, just defending against future changes to the size of the params
        uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10
    );

    void on_post_find_free_element(sdk::FRenderTargetPool* pool, 
        sdk::FPooledRenderTargetDesc* desc, 
        TRefCountPtr<IPooledRenderTarget>* out, 
        const wchar_t* name
    );

    bool m_attempted_hook{false};
    bool m_hooked{false};
    bool m_wants_activate{false};

    std::recursive_mutex m_mutex{};
    SafetyHookInline m_find_free_element_hook{};
    std::unordered_map<std::wstring, IPooledRenderTarget*> m_render_targets{};
    std::unordered_set<std::wstring> m_seen_names{};
};