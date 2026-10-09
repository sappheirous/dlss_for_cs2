#include "dlss.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "dx11_state.hpp"
#include "game.hpp"
#include "motion_vectors.generated.h"

namespace {
struct TextureView {
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC description{};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    UINT mip = 0;
    UINT mip_levels = 0;
};

bool read_texture_view(ID3D11ShaderResourceView* view, TextureView& result) {
    if (!view) return false;

    view->GetDesc(&result.view);
    if (result.view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;

    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (FAILED(resource.As(&result.texture))) return false;

    result.texture->GetDesc(&result.description);
    result.mip_levels = result.description.MipLevels;
    result.mip = result.view.Texture2D.MostDetailedMip;
    if (result.description.SampleDesc.Count != 1 || result.description.ArraySize != 1 ||
        result.mip >= result.description.MipLevels)
        return false;

    result.description.Width = std::max(1u, result.description.Width >> result.mip);
    result.description.Height = std::max(1u, result.description.Height >> result.mip);
    result.description.MipLevels = 1;

    return true;
}

bool multisampled_view(ID3D11ShaderResourceView* view) {
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    view->GetDesc(&description);
    return description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS ||
           description.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
}

bool valid_viewport(const RenderViewport& viewport, const D3D11_TEXTURE2D_DESC& texture) {
    return viewport.top_left_x >= 0 && viewport.top_left_y >= 0 && viewport.width > 0 && viewport.height > 0 &&
           static_cast<std::uint64_t>(viewport.top_left_x) + viewport.width <= texture.Width &&
           static_cast<std::uint64_t>(viewport.top_left_y) + viewport.height <= texture.Height;
}

bool matches_render_scale(const Dispatch& dispatch) {
    const float scale = render_scale(dispatch.settings);
    return std::abs(dispatch.source_viewport.width - dispatch.destination_viewport.width * scale) <= 2.0f &&
           std::abs(dispatch.source_viewport.height - dispatch.destination_viewport.height * scale) <= 2.0f;
}

bool same_viewport(const RenderViewport& left, const RenderViewport& right) noexcept {
    // The leading version/tag is not part of the reconstruction rectangle.
    return left.top_left_x == right.top_left_x && left.top_left_y == right.top_left_y && left.width == right.width &&
           left.height == right.height && left.min_z == right.min_z && left.max_z == right.max_z;
}

bool valid_matrix(const Matrix4& matrix) noexcept {
    for (const auto& row : matrix.m)
        for (const float value : row)
            if (!std::isfinite(value)) return false;

    return true;
}

bool valid_camera(const CameraState& camera) noexcept {
    for (const float value :
         std::array{camera.origin.x, camera.origin.y, camera.origin.z, camera.angles.x, camera.angles.y,
                    camera.angles.z, camera.fov_x, camera.aspect, camera.near_plane, camera.far_plane})
        if (!std::isfinite(value)) return false;

    return camera.fov_x > 0.0f && camera.fov_x < 180.0f && camera.aspect > 0.0f;
}

bool camera_cut(const CameraState& current, const CameraState& previous) noexcept {
    const float x = current.origin.x - previous.origin.x;
    const float y = current.origin.y - previous.origin.y;
    const float z = current.origin.z - previous.origin.z;
    // Large discontinuities should discard history rather than drag it across the new view.
    if (x * x + y * y + z * z > 128.0f * 128.0f) return true;

    const auto angle_changed = [](float current_angle, float previous_angle) {
        return std::abs(std::remainder(current_angle - previous_angle, 360.0f)) > 45.0f;
    };
    return angle_changed(current.angles.x, previous.angles.x) || angle_changed(current.angles.y, previous.angles.y) ||
           angle_changed(current.angles.z, previous.angles.z) || std::abs(current.fov_x - previous.fov_x) > 1.0f ||
           std::abs(current.aspect - previous.aspect) > 0.001f || current.near_plane != previous.near_plane ||
           current.far_plane != previous.far_plane;
}

DXGI_FORMAT typed_color_format(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return format;
    }
}

ID3D11ShaderResourceView* resource_view(const TextureBinding* binding) {
    const auto* texture = binding ? binding->data : nullptr;
    return texture ? (texture->view_unorm ? texture->view_unorm : texture->view_srgb) : nullptr;
}

float halton(std::uint32_t index, std::uint32_t base) {
    float fraction = 1.0f;
    float result = 0.0f;

    for (; index > 0; index /= base) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
    }

    return result;
}

Matrix4 multiply(const Matrix4& left, const Matrix4& right) noexcept {
    Matrix4 result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column) {
            double value = 0.0;
            for (int index = 0; index < 4; ++index)
                value += static_cast<double>(left.m[row][index]) * right.m[index][column];
            result.m[row][column] = static_cast<float>(value);
        }

    return result;
}

void remove_jitter(Matrix4& projection, Matrix4& inverse, float jitter_x, float jitter_y) noexcept {
    if (jitter_x == 0.0f && jitter_y == 0.0f) return;

    for (int column = 0; column < 4; ++column) {
        projection.m[0][column] = static_cast<float>(static_cast<double>(projection.m[0][column]) -
                                                     static_cast<double>(projection.m[3][column]) * jitter_x);
        projection.m[1][column] = static_cast<float>(static_cast<double>(projection.m[1][column]) -
                                                     static_cast<double>(projection.m[3][column]) * jitter_y);
    }
    for (auto& row : inverse.m)
        row[3] = static_cast<float>(static_cast<double>(row[3]) + static_cast<double>(row[0]) * jitter_x +
                                    static_cast<double>(row[1]) * jitter_y);
}

}  // namespace

DlssRenderer& renderer() {
    static auto renderer = std::make_unique<DlssRenderer>();
    return *renderer;
}

std::uint64_t hash_name(const char* name) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (; *name; name++) hash = (hash ^ static_cast<std::uint8_t>(*name)) * 0x100000001B3ull;
    return hash;
}

void RenderExecutor::execute(ID3D11DeviceContext* device_context, const Dispatch* dispatch) {
    if (device_context && dispatch) renderer().execute(device_context, *dispatch);
}

void LayerRenderer::render(void*, void* render_context, void* scene_layer) {
    renderer().on_native_render(render_context, scene_layer);
}

bool DlssRenderer::build_jittered_frustum(const char* debug_name, const Frustum* frustum, Frustum* jittered_frustum) {
    const int render_width = last_render_width_;
    const int render_height = last_render_height_;
    const int output_height = last_output_height_;

    if (!is_enabled || !debug_name || !frustum || unavailable_ || !feature_created_ || retry_requested_ ||
        display_change_active() || display_generation_ != resource_generation_ || render_width <= 0 ||
        render_height <= 0 || output_height <= 0 || hash_name(debug_name) != main_view_hash_)
        return false;

    const float ratio = static_cast<float>(output_height) / static_cast<float>(render_height);
    const std::uint32_t phase_count = static_cast<std::uint32_t>(std::ceil(8.0f * ratio * ratio));
    std::array<float, 2> jitter{};
    {
        std::scoped_lock lock(views_mutex_);
        const auto generation = display_generation_.load();
        if (!jitter_frame_ || *jitter_frame_ != frame_ || jitter_generation_ != generation) {
            const std::uint32_t phase = jitter_index_++ % phase_count + 1;
            frame_jitter_ = {halton(phase, 2) - 0.5f, halton(phase, 3) - 0.5f};
            jitter_frame_ = frame_;
            jitter_generation_ = generation;
        }

        jitter = frame_jitter_;
    }

    *jittered_frustum = *frustum;
    jittered_frustum->jitter_x = 2.0f * jitter[0] / static_cast<float>(render_width);
    jittered_frustum->jitter_y = -2.0f * jitter[1] / static_cast<float>(render_height);

    game.build_frustum(jittered_frustum, &frustum->origin, &frustum->view, frustum->near_plane, frustum->far_plane,
                       frustum->fov_x, frustum->aspect, frustum->clip_bottom_left_x, frustum->clip_bottom_left_y,
                       frustum->clip_top_right_x, frustum->clip_top_right_y);

    return true;
}

void DlssRenderer::on_add_view(void* view, const char* debug_name, const void* view_id, const Frustum* frustum,
                               float jitter_ndc_x, float jitter_ndc_y) {
    if (!view || !debug_name || !frustum || !readable(view_id, 12)) return;

    // The main world pass uses standard depth: clear 1, LESS_EQUAL, and the primary projection.
    ViewMatrices matrices = {frustum->view_projection, frustum->inv_view_projection};
    remove_jitter(matrices.view_projection, matrices.inv_view_projection, frustum->jitter_x, frustum->jitter_y);
    ViewIdentity identity{hash_name(debug_name)};
    // AddView consumes an eight-byte key and four-byte subkey, not the transient view pointer.
    std::memcpy(&identity.key, view_id, sizeof(identity.key));
    std::memcpy(&identity.subkey, static_cast<const std::byte*>(view_id) + 8, sizeof(identity.subkey));

    std::scoped_lock lock(views_mutex_);

    ViewRecord& record = views_[next_view_++ % views_.size()];
    record.view = view;
    record.identity = identity;
    record.frame = frame_;
    record.depth_target = -1;
    record.jitter_ndc_x = jitter_ndc_x;
    record.jitter_ndc_y = jitter_ndc_y;
    record.current = matrices;
    record.camera = {frustum->origin, frustum->view,       frustum->fov_x,
                     frustum->aspect, frustum->near_plane, frustum->far_plane};
}

void DlssRenderer::on_finish_rendering_views() {
    std::scoped_lock lock(views_mutex_);
    frame_++;
}

void DlssRenderer::on_add_upscale_layers(const UpscaleLayersContext* context) {
    if (!context || !context->view || unavailable_ || !game.viewport_scale || !game.fsr_upsample ||
        !main_viewport(context->viewport, game.viewport_scale->float_value))
        return;

    bool main_view = false;
    {
        std::scoped_lock lock(views_mutex_);

        for (std::size_t i = 1; i <= views_.size(); i++) {
            ViewRecord& record = views_[(next_view_ - i) % views_.size()];
            if (record.view != context->view) continue;

            record.depth_target = context->depth_target;

            main_view_hash_ = record.identity.name_hash;
            main_view = true;
            break;
        }
    }

    if (!main_view || context->color_target == -1) return;

    const float scale = game.viewport_scale->float_value;
    const int fsr_upsample = game.fsr_upsample->int_value;

    if (!is_enabled && !feature_created_) return;

    // Replace the FSR layer at render time; do not append a second upscale layer.
    if (scale < 1.0f && fsr_upsample != 0) return;

    TextureHandle color = {};
    RenderViewport destination = context->viewport;

    if (scale < 1.0f) {
        auto& game_layers = field<LayerList>(context->view, view_layers);
        if (game_layers.tail == 0xFFFF || !game_layers.data || !game_layers.data[game_layers.tail].layer) return;

        void* upscale_layer = game_layers.data[game_layers.tail].layer;
        game.get_handle_value(&field<std::uint8_t>(upscale_layer, layer_offsets::attributes), &color,
                              upscale_input_hash, nullptr);
        if (!color.texture) return;

        destination = field<RenderViewport>(upscale_layer, layer_offsets::viewport);
    }

    void* scene_layer = call_virtual<void*>(
        context->view, add_procedural_layer_index, static_cast<const char*>(scale < 1.0f ? "DLSS" : "DLAA"),
        static_cast<const RenderViewport*>(&destination), static_cast<void*>(&layer_renderer), false);
    if (!scene_layer) return;

    field<std::int64_t>(scene_layer, layer_offsets::depth_target) = -1;
    field<std::int32_t>(scene_layer, layer_offsets::has_color_target) = 1;
    field<std::int64_t>(scene_layer, layer_offsets::color_target) = context->color_target;
    field<std::uint8_t>(scene_layer, layer_offsets::override_render_targets) = 1;

    std::scoped_lock lock(views_mutex_);
    layers_[next_layer_++ % layers_.size()] = {scene_layer, color.texture, context->viewport};
}

bool DlssRenderer::on_upscale_render(FsrUpscaleRenderer* renderer, void* render_context, void* scene_layer,
                                     bool native_rendered) {
    if (!renderer || !render_context || !scene_layer || unavailable_ ||
        (!native_rendered && (!is_enabled || !fallback_ready_ || display_change_active())))
        return false;

    const RenderViewport& source = renderer->source_viewport;
    const RenderViewport& destination = renderer->destination_viewport;
    if (source.width <= 0 || source.height <= 0 ||
        (source.width == destination.width && source.height == destination.height))
        return false;

    void* view = field<void*>(scene_layer, layer_offsets::view);

    TextureHandle color = {};
    game.get_handle_value(&field<std::uint8_t>(scene_layer, layer_offsets::attributes), &color, upscale_input_hash,
                          nullptr);

    TextureHandle output = {};
    if (field<std::int32_t>(scene_layer, layer_offsets::has_color_target) && view)
        call_virtual<void>(view, render_target_texture_index, &output,
                           field<std::int64_t>(scene_layer, layer_offsets::color_target));

    return queue_dispatch(render_context, scene_layer, source, destination, resource_view(color.texture),
                          resource_view(output.texture), !native_rendered);
}

void DlssRenderer::on_native_render(void* render_context, void* scene_layer) {
    if (!render_context || !scene_layer || unavailable_) return;

    const RenderViewport& viewport = field<RenderViewport>(scene_layer, layer_offsets::viewport);
    void* view = field<void*>(scene_layer, layer_offsets::view);
    if (viewport.width <= 0 || viewport.height <= 0 || !view) return;

    TextureHandle target = {};
    call_virtual<void>(view, render_target_texture_index, &target,
                       field<std::int64_t>(scene_layer, layer_offsets::color_target));

    const auto target_view = resource_view(target.texture);

    LayerRecord layer_record = {};
    {
        std::scoped_lock lock(views_mutex_);

        for (std::size_t i = 1; i <= layers_.size(); i++) {
            const LayerRecord& candidate = layers_[(next_layer_ - i) % layers_.size()];
            if (candidate.layer == scene_layer) {
                layer_record = candidate;
                break;
            }
        }
    }

    if (!layer_record.color_texture) {
        queue_dispatch(render_context, scene_layer, viewport, viewport, target_view, target_view);
        return;
    }

    queue_dispatch(render_context, scene_layer, layer_record.source_viewport, viewport,
                   resource_view(layer_record.color_texture), target_view);
}

bool DlssRenderer::queue_dispatch(void* render_context, void* scene_layer, const RenderViewport& source,
                                  const RenderViewport& destination, ID3D11ShaderResourceView* color_view,
                                  ID3D11ShaderResourceView* output_view, bool fallback) {
    // Panorama and other auxiliary views can share full-sized textures with the main camera.
    if (!main_viewport(destination)) return false;

    const bool enabled = is_enabled;
    void* view = field<void*>(scene_layer, layer_offsets::view);

    ViewRecord record = {};
    bool has_record = false;
    {
        std::scoped_lock lock(views_mutex_);

        for (std::size_t i = 1; i <= views_.size() && view; i++) {
            const ViewRecord& candidate = views_[(next_view_ - i) % views_.size()];
            if (candidate.view != view) continue;

            record = candidate;
            has_record = true;
            break;
        }
    }

    if (!has_record || record.depth_target == -1 || record.identity.name_hash != main_view_hash_) return false;

    last_render_width_ = source.width;
    last_render_height_ = source.height;
    last_output_height_ = destination.height;

    TextureHandle depth = {};
    call_virtual<void>(view, render_target_texture_index, &depth, record.depth_target);

    if (fallback) {
        TextureView color, output;
        const auto depth_view = resource_view(depth.texture);
        if (!enabled || !depth_view || multisampled_view(depth_view) || !read_texture_view(color_view, color) ||
            !read_texture_view(output_view, output) || !(output.description.BindFlags & D3D11_BIND_RENDER_TARGET) ||
            color.texture == output.texture || !valid_viewport(source, color.description) ||
            !valid_viewport(destination, output.description))
            return false;
    }

    Dispatch dispatch = {};
    dispatch.source_viewport = source;
    dispatch.destination_viewport = destination;
    dispatch.color_view = color_view;
    dispatch.output_view = output_view;
    dispatch.depth_view = resource_view(depth.texture);
    dispatch.release = !enabled;
    dispatch.fallback = fallback;
    dispatch.settings = store().active();
    dispatch.display_generation = display_generation_;

    dispatch.matrices = record.current;
    dispatch.camera = record.camera;
    dispatch.identity = record.identity;
    dispatch.frame = record.frame;
    dispatch.jitter_x = record.jitter_ndc_x * static_cast<float>(source.width) * 0.5f;
    dispatch.jitter_y = record.jitter_ndc_y * static_cast<float>(source.height) * -0.5f;
    dispatch.has_matrices = true;

    if (!enabled && !feature_created_) return false;

    call_virtual<void>(render_context, queue_upscaler_index, static_cast<void*>(&executor),
                       static_cast<const void*>(&dispatch), static_cast<int>(sizeof(dispatch)));
    return true;
}

void DlssRenderer::prepare_retry() {
    unavailable_ = false;
    feature_created_ = false;
    retry_requested_ = true;
}

void DlssRenderer::begin_display_change() {
    display_changes_.fetch_add(1);
    display_generation_.fetch_add(1);
    feature_created_ = false;
}

void DlssRenderer::end_display_change() { display_changes_.fetch_sub(1); }

void DlssRenderer::set_display_size(std::uint32_t width, std::uint32_t height) noexcept {
    display_size_ = (static_cast<std::uint64_t>(width) << 32) | height;
}

bool DlssRenderer::main_viewport(const RenderViewport& viewport, float scale) const noexcept {
    const auto size = display_size_.load();
    const auto width = static_cast<std::uint32_t>(size >> 32);
    const auto height = static_cast<std::uint32_t>(size);
    if (!width || !height || viewport.top_left_x != 0 || viewport.top_left_y != 0) return false;

    return (std::abs(viewport.width - width * scale) <= 2.0f && std::abs(viewport.height - height * scale) <= 2.0f) ||
           (static_cast<std::uint32_t>(viewport.width) == width &&
            static_cast<std::uint32_t>(viewport.height) == height);
}

bool DlssRenderer::display_change_active() const noexcept { return display_changes_.load() != 0; }

std::optional<float> DlssRenderer::upscale_time_ms() const noexcept {
    if (!is_enabled || !feature_created_ || display_change_active()) return std::nullopt;

    return upscale_timer_.milliseconds();
}

std::array<std::uint32_t, 2> DlssRenderer::input_resolution() const noexcept {
    if (!is_enabled || !feature_created_ || display_change_active()) return {};

    const auto size = input_size_.load();
    return {static_cast<std::uint32_t>(size >> 32), static_cast<std::uint32_t>(size)};
}

void DlssRenderer::fail(const char* message, std::uint32_t result) {
    is_enabled = false;
    unavailable_ = true;
    release_feature();
    release_parameters();
    store().fail(message, result);
}

void DlssRenderer::execute(ID3D11DeviceContext* device_context, const Dispatch& dispatch) {
    try {
        if (display_change_active() || dispatch.display_generation != display_generation_ ||
            !main_viewport(dispatch.destination_viewport))
            return;

        ContextStateGuard state(device_context);
        if (!state.ready()) {
            fail("DLSS requires an immediate D3D11.1 context with state isolation");
            return;
        }

        if (resource_generation_ != dispatch.display_generation) {
            release_feature();
            resource_generation_ = dispatch.display_generation;
            jitter_index_ = 0;
            spdlog::info("DLSS display history reset, generation {}", resource_generation_.load());
        }

        ComPtr<ID3D11Device> current_device;
        device_context->GetDevice(&current_device);
        if (current_device != resource_device_) {
            release_feature();
            release_parameters();
            motion_vectors_shader_.Reset();
            shader_constants_.Reset();
            fallback_ = {};
            rcas_ = {};
            fallback_ready_ = false;
            resource_device_ = current_device;
        }

        if (!fallback_ready_) fallback_ready_ = fallback_.initialize(current_device.Get());

        bool rendered = false;
        try {
            rendered = execute_dlss(device_context, dispatch);
        } catch (const std::exception& error) {
            fail(error.what());
        }

        if (!rendered) {
            reset_ = true;
            evaluation_history_.reset();
        }

        if (!rendered && dispatch.fallback &&
            (!fallback_ready_ || !fallback_.render(device_context, dispatch.color_view, dispatch.output_view,
                                                   dispatch.source_viewport, dispatch.destination_viewport)))
            fail("Could not render the DLSS fallback");
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

bool DlssRenderer::execute_dlss(ID3D11DeviceContext* device_context, const Dispatch& dispatch) {
    if (dispatch.release || !is_enabled || store().has_failed()) {
        release_feature();
        release_parameters();
        return false;
    }

    // Settings may change before an already-built scene reaches the render queue.
    const auto active = store().active();
    if (dispatch.settings.enabled != active.enabled || dispatch.settings.quality != active.quality ||
        dispatch.settings.preset != active.preset)
        return false;
    if (!dispatch.color_view || !dispatch.output_view || !dispatch.depth_view || !dispatch.has_matrices) return false;

    if (!matches_render_scale(dispatch)) {
        feature_created_ = false;
        return false;
    }

    if (retry_requested_.exchange(false)) {
        release_feature();
        jitter_index_ = 0;
    }

    if (!parameters_ && !initialize_ngx()) return false;

    return evaluate(device_context, dispatch);
}

bool DlssRenderer::evaluate(ID3D11DeviceContext* device_context, const Dispatch& dispatch) {
    // Frames queued before the main-thread MSAA change retain their native resolve.
    if (multisampled_view(dispatch.color_view) || multisampled_view(dispatch.output_view) ||
        multisampled_view(dispatch.depth_view)) {
        feature_created_ = false;
        return false;
    }

    TextureView color;
    TextureView output;
    TextureView depth;
    if (!read_texture_view(dispatch.color_view, color) || !read_texture_view(dispatch.output_view, output) ||
        !read_texture_view(dispatch.depth_view, depth)) {
        fail("DLSS requires single-sample Texture2D views with a valid mip level");
        return false;
    }

    const auto& color_desc = color.description;
    const auto& output_desc = output.description;

    const RenderViewport& source = dispatch.source_viewport;
    const RenderViewport& destination = dispatch.destination_viewport;
    if (!valid_viewport(source, color_desc) || !valid_viewport(source, depth.description) ||
        !valid_viewport(destination, output_desc)) {
        fail("Game supplied a DLSS viewport outside the selected color, depth, or output mip");
        return false;
    }

    auto input_desc = color_desc;
    input_desc.Format = typed_color_format(color.view.Format);

    const bool direct_color = color.mip == 0 && color.mip_levels == 1 && color_desc.Format == input_desc.Format;
    const bool direct_output =
        output.mip == 0 && output.mip_levels == 1 && (output_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) &&
        output_desc.Format == typed_color_format(output.view.Format) && color.texture != output.texture;

    if (!feature_ || feature_settings_.quality != dispatch.settings.quality ||
        feature_settings_.preset != dispatch.settings.preset || direct_color_ != direct_color ||
        direct_output_ != direct_output || !same_viewport(feature_source_viewport_, source) ||
        !same_viewport(feature_destination_viewport_, destination) || feature_color_desc_.Width != color_desc.Width ||
        feature_color_desc_.Height != color_desc.Height || feature_color_desc_.Format != input_desc.Format ||
        feature_output_desc_.Width != output_desc.Width || feature_output_desc_.Height != output_desc.Height ||
        feature_output_desc_.Format != output_desc.Format) {
        release_feature();

        if (!create_feature(device_context, dispatch, input_desc, output_desc, direct_color, direct_output))
            return false;
    }

    if (!valid_camera(dispatch.camera) || !std::isfinite(dispatch.jitter_x) || !std::isfinite(dispatch.jitter_y) ||
        !std::isfinite(source.min_z) || !std::isfinite(source.max_z) || source.min_z < 0.0f || source.max_z > 1.0f ||
        source.max_z <= source.min_z)
        return false;

    const auto now = std::chrono::steady_clock::now();
    if (!evaluation_history_ || dispatch.identity != evaluation_history_->identity ||
        dispatch.frame - evaluation_history_->frame != 1 ||
        now - evaluation_history_->time > std::chrono::milliseconds(500) ||
        camera_cut(dispatch.camera, evaluation_history_->camera))
        reset_ = true;

    if (!dispatch_motion_vectors(device_context, dispatch)) return false;

    // NGX takes a resource rather than an SRV; nonzero mips and format reinterpretation need a copy.
    if (!direct_color_) {
        const D3D11_BOX source_box = {static_cast<UINT>(source.top_left_x),
                                      static_cast<UINT>(source.top_left_y),
                                      0,
                                      static_cast<UINT>(source.top_left_x + source.width),
                                      static_cast<UINT>(source.top_left_y + source.height),
                                      1};
        device_context->CopySubresourceRegion(color_copy_texture_.Get(), 0, 0, 0, 0, color.texture.Get(), color.mip,
                                              &source_box);
    }

    ngx.set_resource(parameters_, "Color", direct_color_ ? color.texture.Get() : color_copy_texture_.Get());
    ngx.set_resource(parameters_, "Output", direct_output_ ? output.texture.Get() : output_texture_.Get());
    ngx.set_f(parameters_, "Jitter.Offset.X", dispatch.jitter_x);
    ngx.set_f(parameters_, "Jitter.Offset.Y", dispatch.jitter_y);
    ngx.set_i(parameters_, "Reset", reset_ ? 1 : 0);

    std::uint32_t result = 0;
    {
        const auto measurement = upscale_timer_.measure(device_context);
        result = ngx.evaluate_feature(device_context, feature_, parameters_, nullptr);
    }

    if (ngx_failed(result)) {
        fail("NGX EvaluateFeature failed", result);
        return false;
    }

    reset_ = false;
    evaluation_history_ = EvaluationHistory{dispatch.matrices, dispatch.camera, dispatch.identity, dispatch.frame,
                                            std::chrono::steady_clock::now()};
    feature_created_ = true;

    if (dispatch.settings.sharpness > 0.0f) {
        if (!rcas_.render(device_context, direct_output_ ? output.texture.Get() : output_texture_.Get(),
                          direct_output_ ? nullptr : output_view_.Get(), direct_output_ ? output.mip : 0,
                          output.texture.Get(), output.mip, destination, typed_color_format(output.view.Format),
                          dispatch.settings.sharpness)) {
            fail("Could not apply RCAS sharpening");
            return false;
        }
    } else if (!direct_output_) {
        const D3D11_BOX box = {0, 0, 0, static_cast<UINT>(destination.width), static_cast<UINT>(destination.height), 1};
        device_context->CopySubresourceRegion(
            output.texture.Get(), output.mip, static_cast<UINT>(destination.top_left_x),
            static_cast<UINT>(destination.top_left_y), 0, output_texture_.Get(), 0, &box);
    }

    input_size_ = (static_cast<std::uint64_t>(source.width) << 32) | static_cast<std::uint32_t>(source.height);
    return true;
}

bool DlssRenderer::dispatch_motion_vectors(ID3D11DeviceContext* device_context, const Dispatch& dispatch) {
    const auto& inverse = dispatch.matrices.inv_view_projection;
    if (!valid_matrix(inverse)) return false;

    Matrix4 reprojection{};
    if (reset_) {
        for (int index = 0; index < 4; ++index) reprojection.m[index][index] = 1.0f;
    } else {
        const auto& previous = evaluation_history_->matrices.view_projection;
        if (!valid_matrix(previous)) return false;
        reprojection = multiply(previous, inverse);
    }

    const RenderViewport& source = dispatch.source_viewport;
    const float jitter_x = 2.0f * dispatch.jitter_x / static_cast<float>(source.width);
    const float jitter_y = -2.0f * dispatch.jitter_y / static_cast<float>(source.height);
    // BuildFrustum applies a clip-space translation. Fold its inverse into the CPU product.
    for (auto& row : reprojection.m)
        row[3] = static_cast<float>(static_cast<double>(row[3]) - static_cast<double>(row[0]) * jitter_x -
                                    static_cast<double>(row[1]) * jitter_y);
    if (!valid_matrix(reprojection)) return false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!dispatch.depth_view ||
        FAILED(device_context->Map(shader_constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        fail("Could not map motion-vector constants or acquire depth");
        return false;
    }

    const ShaderConstants constants = {reprojection,
                                       {static_cast<float>(source.top_left_x), static_cast<float>(source.top_left_y),
                                        static_cast<float>(source.width), static_cast<float>(source.height)},
                                       {reset_ ? 0.0f : 1.0f, source.min_z, source.max_z, 0.0f},
                                       {jitter_x, jitter_y, 0.0f, 0.0f}};

    std::memcpy(mapped.pData, &constants, sizeof(constants));
    device_context->Unmap(shader_constants_.Get(), 0);

    ID3D11Buffer* constant_buffer = shader_constants_.Get();
    ID3D11ShaderResourceView* source_depth_view = dispatch.depth_view;
    ID3D11UnorderedAccessView* unordered_views[2] = {motion_vectors_view_.Get(), depth_view_.Get()};

    device_context->CSSetShader(motion_vectors_shader_.Get(), nullptr, 0);
    device_context->CSSetConstantBuffers(0, 1, &constant_buffer);
    device_context->CSSetShaderResources(0, 1, &source_depth_view);
    device_context->CSSetUnorderedAccessViews(0, 2, unordered_views, nullptr);
    device_context->Dispatch(static_cast<UINT>((source.width + 7) / 8), static_cast<UINT>((source.height + 7) / 8), 1);

    ID3D11Buffer* null_constants = nullptr;
    ID3D11ShaderResourceView* null_resource = nullptr;
    ID3D11UnorderedAccessView* null_views[2] = {};

    device_context->CSSetUnorderedAccessViews(0, 2, null_views, nullptr);
    device_context->CSSetShaderResources(0, 1, &null_resource);
    device_context->CSSetConstantBuffers(0, 1, &null_constants);
    device_context->CSSetShader(nullptr, nullptr, 0);

    return true;
}

bool DlssRenderer::initialize_ngx() {
    NVSDK_NGX_Parameter* capabilities = nullptr;
    const std::uint32_t capabilities_result = ngx.get_capability_parameters(&capabilities);
    if (ngx_failed(capabilities_result) || !capabilities) {
        if (capabilities) ngx.destroy_parameters(capabilities);
        fail("NGX is not initialized; launch CS2 in DX11 with -dlss", capabilities_result);
        return false;
    }

    int available = 0, init_result = 0;
    ngx.get_i(capabilities, "SuperSampling.Available", &available);
    ngx.get_i(capabilities, "SuperSampling.FeatureInitResult", &init_result);
    void* optimal_settings = nullptr;
    ngx.get_pointer(capabilities, "DLSSOptimalSettingsCallback", &optimal_settings);
    optimal_settings_ = reinterpret_cast<decltype(optimal_settings_)>(optimal_settings);
    ngx.destroy_parameters(capabilities);

    if (!available) {
        fail("Super Resolution is unavailable; check GPU and nvngx_dlss.dll", static_cast<std::uint32_t>(init_result));
        return false;
    }

    if (!optimal_settings_) {
        fail("DLSS runtime does not support render-scale queries; update nvngx_dlss.dll");
        return false;
    }

    const std::uint32_t allocate_result = ngx.allocate_parameters(&parameters_);
    if (ngx_failed(allocate_result) || !parameters_) {
        fail("Could not allocate NGX parameters", allocate_result);
        return false;
    }

    return true;
}

bool DlssRenderer::select_resolution(const Dispatch& dispatch, std::uint32_t& width, std::uint32_t& height,
                                     int& quality) {
    const auto& source = dispatch.source_viewport;
    const auto& destination = dispatch.destination_viewport;

    // Create at the runtime's recommended size; Evaluate uses the selected render subrectangle.
    quality = std::to_underlying(dispatch.settings.quality);
    ngx.set_ui(parameters_, "Width", static_cast<std::uint32_t>(destination.width));
    ngx.set_ui(parameters_, "Height", static_cast<std::uint32_t>(destination.height));
    ngx.set_i(parameters_, "PerfQualityValue", quality);
    ngx.set_i(parameters_, "RTXValue", 0);
    if (ngx_failed(optimal_settings_(parameters_))) {
        fail("Could not query the selected DLSS quality mode");
        return false;
    }

    width = height = 0;
    ngx.get_ui(parameters_, "OutWidth", &width);
    ngx.get_ui(parameters_, "OutHeight", &height);
    if (!width || !height) {
        fail("DLSS runtime returned empty render dimensions");
        return false;
    }

    auto min_width = width, max_width = width, min_height = height, max_height = height;
    ngx.get_ui(parameters_, "DLSS.Get.Dynamic.Min.Render.Width", &min_width);
    ngx.get_ui(parameters_, "DLSS.Get.Dynamic.Max.Render.Width", &max_width);
    ngx.get_ui(parameters_, "DLSS.Get.Dynamic.Min.Render.Height", &min_height);
    ngx.get_ui(parameters_, "DLSS.Get.Dynamic.Max.Render.Height", &max_height);
    if (static_cast<std::uint32_t>(source.width) >= min_width &&
        static_cast<std::uint32_t>(source.width) <= max_width &&
        static_cast<std::uint32_t>(source.height) >= min_height &&
        static_cast<std::uint32_t>(source.height) <= max_height)
        return true;

    fail("Selected render scale is unsupported by the DLSS runtime; update nvngx_dlss.dll");
    return false;
}

bool DlssRenderer::create_resources(ID3D11DeviceContext* device_context, const Dispatch& dispatch,
                                    const D3D11_TEXTURE2D_DESC& color_desc, const D3D11_TEXTURE2D_DESC& output_desc) {
    ComPtr<ID3D11Device> device;
    device_context->GetDevice(device.GetAddressOf());
    if (!device) return false;

    if (!motion_vectors_shader_ &&
        FAILED(device->CreateComputeShader(motion_vectors_shader, sizeof(motion_vectors_shader), nullptr,
                                           motion_vectors_shader_.GetAddressOf())))
        return false;

    if (!shader_constants_) {
        D3D11_BUFFER_DESC buffer_desc = {};
        buffer_desc.ByteWidth = sizeof(ShaderConstants);
        buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (FAILED(device->CreateBuffer(&buffer_desc, nullptr, shader_constants_.GetAddressOf()))) return false;
    }

    const auto output_format = typed_color_format(output_desc.Format);

    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = static_cast<UINT>(dispatch.destination_viewport.width);
    texture_desc.Height = static_cast<UINT>(dispatch.destination_viewport.height);
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = output_format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    if (!direct_output_ &&
        (FAILED(device->CreateTexture2D(&texture_desc, nullptr, output_texture_.ReleaseAndGetAddressOf())) ||
         FAILED(
             device->CreateShaderResourceView(output_texture_.Get(), nullptr, output_view_.ReleaseAndGetAddressOf()))))
        return false;

    D3D11_TEXTURE2D_DESC color_copy_desc = color_desc;
    color_copy_desc.Width = static_cast<UINT>(dispatch.source_viewport.width);
    color_copy_desc.Height = static_cast<UINT>(dispatch.source_viewport.height);
    color_copy_desc.MipLevels = 1;
    color_copy_desc.ArraySize = 1;
    color_copy_desc.Usage = D3D11_USAGE_DEFAULT;
    color_copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    color_copy_desc.CPUAccessFlags = 0;
    color_copy_desc.MiscFlags = 0;

    if (!direct_color_ &&
        FAILED(device->CreateTexture2D(&color_copy_desc, nullptr, color_copy_texture_.ReleaseAndGetAddressOf())))
        return false;

    texture_desc.Width = static_cast<UINT>(dispatch.source_viewport.width);
    texture_desc.Height = static_cast<UINT>(dispatch.source_viewport.height);
    texture_desc.Format = DXGI_FORMAT_R32_FLOAT;

    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, depth_texture_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateUnorderedAccessView(depth_texture_.Get(), nullptr, depth_view_.ReleaseAndGetAddressOf())))
        return false;

    texture_desc.Format = DXGI_FORMAT_R16G16_FLOAT;

    if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, motion_vectors_texture_.ReleaseAndGetAddressOf())) ||
        FAILED(device->CreateUnorderedAccessView(motion_vectors_texture_.Get(), nullptr,
                                                 motion_vectors_view_.ReleaseAndGetAddressOf())))
        return false;

    return true;
}

bool DlssRenderer::create_feature(ID3D11DeviceContext* device_context, const Dispatch& dispatch,
                                  const D3D11_TEXTURE2D_DESC& color_desc, const D3D11_TEXTURE2D_DESC& output_desc,
                                  bool direct_color, bool direct_output) {
    direct_color_ = direct_color;
    direct_output_ = direct_output;
    if (!create_resources(device_context, dispatch, color_desc, output_desc)) {
        fail("Could not create DLSS GPU resources");
        return false;
    }

    const RenderViewport& source = dispatch.source_viewport;
    const RenderViewport& destination = dispatch.destination_viewport;
    constexpr int motion_vectors_low_res = 2;
    const auto preset = requested_preset(dispatch.settings);

    ngx.set_i(parameters_, "DLSS.Feature.Create.Flags", motion_vectors_low_res);
    const bool output_subrect = direct_output_ && (destination.top_left_x != 0 || destination.top_left_y != 0 ||
                                                   static_cast<UINT>(destination.width) != output_desc.Width ||
                                                   static_cast<UINT>(destination.height) != output_desc.Height);
    ngx.set_i(parameters_, "DLSS.Enable.Output.Subrects", output_subrect ? 1 : 0);
    for (const auto* name :
         std::array{"DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
                    "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
                    "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"})
        ngx.set_ui(parameters_, name, std::to_underlying(preset));

    std::uint32_t width = 0, height = 0;
    int quality = 0;
    if (!select_resolution(dispatch, width, height, quality)) return false;

    ngx.set_ui(parameters_, "Width", width);
    ngx.set_ui(parameters_, "Height", height);
    ngx.set_ui(parameters_, "OutWidth", static_cast<std::uint32_t>(destination.width));
    ngx.set_ui(parameters_, "OutHeight", static_cast<std::uint32_t>(destination.height));
    ngx.set_i(parameters_, "PerfQualityValue", quality);

    const std::uint32_t result = ngx.create_feature(device_context, 1, parameters_, &feature_);

    if (ngx_failed(result) || !feature_) {
        feature_ = nullptr;
        fail("Could not create DLSS feature", result);
        return false;
    }

    ngx.set_resource(parameters_, "Depth", depth_texture_.Get());
    ngx.set_resource(parameters_, "MotionVectors", motion_vectors_texture_.Get());
    ngx.set_f(parameters_, "Sharpness", 0.0f);
    ngx.set_f(parameters_, "MV.Scale.X", 1.0f);
    ngx.set_f(parameters_, "MV.Scale.Y", 1.0f);
    ngx.set_ui(parameters_, "DLSS.Input.Color.Subrect.Base.X", direct_color_ ? source.top_left_x : 0);
    ngx.set_ui(parameters_, "DLSS.Input.Color.Subrect.Base.Y", direct_color_ ? source.top_left_y : 0);
    ngx.set_ui(parameters_, "DLSS.Input.Depth.Subrect.Base.X", 0);
    ngx.set_ui(parameters_, "DLSS.Input.Depth.Subrect.Base.Y", 0);
    ngx.set_ui(parameters_, "DLSS.Input.MV.Subrect.Base.X", 0);
    ngx.set_ui(parameters_, "DLSS.Input.MV.Subrect.Base.Y", 0);
    ngx.set_ui(parameters_, "DLSS.Output.Subrect.Base.X", direct_output_ ? destination.top_left_x : 0);
    ngx.set_ui(parameters_, "DLSS.Output.Subrect.Base.Y", direct_output_ ? destination.top_left_y : 0);
    ngx.set_ui(parameters_, "DLSS.Render.Subrect.Dimensions.Width", static_cast<std::uint32_t>(source.width));
    ngx.set_ui(parameters_, "DLSS.Render.Subrect.Dimensions.Height", static_cast<std::uint32_t>(source.height));
    ngx.set_f(parameters_, "DLSS.Pre.Exposure", 1.0f);
    ngx.set_f(parameters_, "DLSS.Exposure.Scale", 1.0f);

    spdlog::info("DLSS feature created: {}x{} -> {}x{}, quality {}, requested preset {} (selection {})", source.width,
                 source.height, destination.width, destination.height, quality_name(dispatch.settings.quality),
                 preset_name(preset), preset_name(dispatch.settings.preset));

    feature_settings_ = dispatch.settings;
    feature_source_viewport_ = source;
    feature_destination_viewport_ = destination;
    feature_color_desc_ = color_desc;
    feature_output_desc_ = output_desc;
    reset_ = true;
    feature_created_ = true;

    return true;
}

void DlssRenderer::release_feature() {
    input_size_ = 0;
    upscale_timer_.reset();

    if (feature_) {
        ngx.release_feature(feature_);
        feature_ = nullptr;
    }

    if (parameters_) {
        ngx.set_resource(parameters_, "Color", nullptr);
        ngx.set_resource(parameters_, "Output", nullptr);
        ngx.set_resource(parameters_, "Depth", nullptr);
        ngx.set_resource(parameters_, "MotionVectors", nullptr);
    }

    output_view_.Reset();
    output_texture_.Reset();
    rcas_ = {};
    color_copy_texture_.Reset();
    depth_texture_.Reset();
    motion_vectors_texture_.Reset();
    depth_view_.Reset();
    motion_vectors_view_.Reset();

    feature_color_desc_ = {};
    feature_output_desc_ = {};
    feature_created_ = false;
    direct_color_ = false;
    direct_output_ = false;
    reset_ = true;
    evaluation_history_.reset();
}

void DlssRenderer::release_parameters() {
    if (parameters_) {
        ngx.destroy_parameters(parameters_);
        parameters_ = nullptr;
    }

    optimal_settings_ = nullptr;
}
