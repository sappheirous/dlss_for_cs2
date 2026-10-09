#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <type_traits>

#include <d3d11.h>
#include <wrl/client.h>

#include "game_types.hpp"
#include "gpu_timer.hpp"
#include "ngx.hpp"
#include "rcas.hpp"
#include "settings.hpp"
#include "upscale_fallback.hpp"

using Microsoft::WRL::ComPtr;

struct ViewMatrices {
    Matrix4 view_projection;
    Matrix4 inv_view_projection;
};

struct ViewIdentity {
    std::uint64_t name_hash;
    std::uint64_t key;
    std::uint32_t subkey;

    bool operator==(const ViewIdentity&) const = default;
};

struct CameraState {
    Vec3 origin;
    Vec3 angles;
    float fov_x;
    float aspect;
    float near_plane;
    float far_plane;
};

struct ViewRecord {
    void* view;
    ViewIdentity identity;
    std::uint32_t frame;
    std::int64_t depth_target;

    float jitter_ndc_x;
    float jitter_ndc_y;

    ViewMatrices current;
    CameraState camera;
};

struct LayerRecord {
    void* layer;
    TextureBinding* color_texture;
    RenderViewport source_viewport;
};

struct Dispatch {
    RenderViewport source_viewport;
    RenderViewport destination_viewport;

    ID3D11ShaderResourceView* color_view;
    ID3D11ShaderResourceView* output_view;
    ID3D11ShaderResourceView* depth_view;

    ViewMatrices matrices;
    CameraState camera;
    ViewIdentity identity;
    std::uint32_t frame;

    float jitter_x;
    float jitter_y;
    bool has_matrices;
    bool release;
    bool fallback;

    Settings settings;
    std::uint64_t display_generation;
};

static_assert(std::is_trivially_copyable_v<Dispatch>);

struct ShaderConstants {
    Matrix4 reprojection;

    float viewport[4];
    float params[4];
    float jitter[4];
};

static_assert(sizeof(ShaderConstants) == 112);
static_assert(offsetof(ShaderConstants, viewport) == 64);
static_assert(offsetof(ShaderConstants, params) == 80);
static_assert(offsetof(ShaderConstants, jitter) == 96);

struct EvaluationHistory {
    ViewMatrices matrices;
    CameraState camera;
    ViewIdentity identity;
    std::uint32_t frame;
    std::chrono::steady_clock::time_point time;
};

class RenderExecutor {
public:
    virtual void reserved0() {}
    virtual void reserved1() {}
    virtual void reserved2() {}
    virtual void reserved3() {}

    virtual void execute(ID3D11DeviceContext* device_context, const Dispatch* dispatch);

    virtual void reserved5() {}
    virtual void reserved6() {}
    virtual void reserved7() {}
};

class LayerRenderer {
public:
    virtual void render(void* view, void* render_context, void* scene_layer);
    virtual void destroy(int) {}
    virtual bool reserved2() { return false; }
};

class DlssRenderer {
public:
    bool build_jittered_frustum(const char* debug_name, const Frustum* frustum, Frustum* jittered_frustum);
    void on_add_view(void* view, const char* debug_name, const void* view_id, const Frustum* frustum,
                     float jitter_ndc_x, float jitter_ndc_y);
    void on_finish_rendering_views();
    void on_add_upscale_layers(const UpscaleLayersContext* context);
    bool on_upscale_render(FsrUpscaleRenderer* renderer, void* render_context, void* scene_layer, bool native_rendered);
    void on_native_render(void* render_context, void* scene_layer);
    void execute(ID3D11DeviceContext* device_context, const Dispatch& dispatch);

    RenderExecutor executor;
    LayerRenderer layer_renderer;

    void prepare_retry();
    void begin_display_change();
    void end_display_change();
    void set_display_size(std::uint32_t width, std::uint32_t height) noexcept;
    bool display_change_active() const noexcept;
    std::optional<float> upscale_time_ms() const noexcept;
    std::array<std::uint32_t, 2> input_resolution() const noexcept;

private:
    bool main_viewport(const RenderViewport& viewport, float scale = 1.0f) const noexcept;
    bool queue_dispatch(void* render_context, void* scene_layer, const RenderViewport& source,
                        const RenderViewport& destination, ID3D11ShaderResourceView* color_view,
                        ID3D11ShaderResourceView* output_view, bool fallback = false);
    bool initialize_ngx();
    bool select_resolution(const Dispatch& dispatch, std::uint32_t& width, std::uint32_t& height, int& quality);
    bool execute_dlss(ID3D11DeviceContext* device_context, const Dispatch& dispatch);
    bool evaluate(ID3D11DeviceContext* device_context, const Dispatch& dispatch);
    bool create_feature(ID3D11DeviceContext* device_context, const Dispatch& dispatch,
                        const D3D11_TEXTURE2D_DESC& color_desc, const D3D11_TEXTURE2D_DESC& output_desc,
                        bool direct_color, bool direct_output);
    bool create_resources(ID3D11DeviceContext* device_context, const Dispatch& dispatch,
                          const D3D11_TEXTURE2D_DESC& color_desc, const D3D11_TEXTURE2D_DESC& output_desc);
    bool dispatch_motion_vectors(ID3D11DeviceContext* device_context, const Dispatch& dispatch);
    void release_feature();
    void release_parameters();
    void fail(const char* message, std::uint32_t result = 0);

    std::mutex views_mutex_;
    std::array<ViewRecord, 32> views_ = {};
    std::size_t next_view_ = 0;
    std::array<LayerRecord, 8> layers_ = {};
    std::size_t next_layer_ = 0;
    std::uint32_t frame_ = 0;
    std::optional<std::uint32_t> jitter_frame_;
    std::uint64_t jitter_generation_ = 0;
    std::array<float, 2> frame_jitter_{};

    std::atomic_bool retry_requested_{false};
    std::atomic_uint32_t display_changes_{0};
    std::atomic_uint64_t display_generation_{0};
    std::atomic_uint64_t resource_generation_{0};
    std::atomic_bool unavailable_ = false;
    std::atomic_bool feature_created_ = false;
    std::atomic_int last_output_height_ = 0;
    std::atomic_int last_render_width_ = 0;
    std::atomic_int last_render_height_ = 0;
    std::atomic_uint64_t display_size_ = 0;
    std::atomic_uint64_t input_size_ = 0;
    std::atomic_uint64_t main_view_hash_ = 0;
    std::atomic_uint32_t jitter_index_{0};

    bool reset_ = true;
    bool direct_color_ = false;
    bool direct_output_ = false;
    std::optional<EvaluationHistory> evaluation_history_;

    NVSDK_NGX_Parameter* parameters_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    std::uint32_t(__cdecl* optimal_settings_)(NVSDK_NGX_Parameter*) = nullptr;

    Settings feature_settings_;
    RenderViewport feature_source_viewport_ = {};
    RenderViewport feature_destination_viewport_ = {};
    D3D11_TEXTURE2D_DESC feature_color_desc_ = {};
    D3D11_TEXTURE2D_DESC feature_output_desc_ = {};

    ComPtr<ID3D11Device> resource_device_;
    UpscaleFallback fallback_;
    Rcas rcas_;
    std::atomic_bool fallback_ready_ = false;
    GpuTimer upscale_timer_;
    ComPtr<ID3D11Texture2D> output_texture_;
    ComPtr<ID3D11ShaderResourceView> output_view_;
    ComPtr<ID3D11Texture2D> color_copy_texture_;
    ComPtr<ID3D11Texture2D> depth_texture_;
    ComPtr<ID3D11Texture2D> motion_vectors_texture_;
    ComPtr<ID3D11UnorderedAccessView> depth_view_;
    ComPtr<ID3D11UnorderedAccessView> motion_vectors_view_;

    ComPtr<ID3D11ComputeShader> motion_vectors_shader_;
    ComPtr<ID3D11Buffer> shader_constants_;
};

DlssRenderer& renderer();
