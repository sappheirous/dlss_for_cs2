#pragma once

#include <cstddef>
#include <cstdint>

#include <Windows.h>
#include <d3d11.h>

struct Vec3 {
    float x, y, z;
};

struct Matrix4 {
    float m[4][4];
};

struct RenderViewport {
    int version;
    int top_left_x;
    int top_left_y;
    int width;
    int height;
    float min_z;
    float max_z;
};

struct alignas(16) Frustum {
    Vec3 origin;
    Vec3 view;

    float fov_x;
    float aspect;
    float near_plane;
    float far_plane;
    float width;
    float height;
    std::uint8_t pad0[368 - 48];

    float clip_bottom_left_x;
    float clip_bottom_left_y;
    float clip_top_right_x;
    float clip_top_right_y;
    std::uint8_t pad1[648 - 384];

    Matrix4 view_projection;
    Matrix4 inv_view_projection;
    Matrix4 rev_z_projection;
    Matrix4 inv_rev_z_projection;
    Matrix4 rev_z_view_projection;
    Matrix4 inv_rev_z_view_projection;
    std::uint8_t pad2[1044 - 1032];

    float jitter_x;
    float jitter_y;
    std::uint8_t pad3[0x420 - 1052];
};
static_assert(sizeof(Frustum) == 0x420);
static_assert(offsetof(Frustum, view_projection) == 0x288);
static_assert(offsetof(Frustum, jitter_x) == 0x414);
static_assert(offsetof(Frustum, jitter_y) == 0x418);

struct TextureDx11 {
    std::uint8_t pad[0x10];
    ID3D11ShaderResourceView* view_unorm;
    ID3D11ShaderResourceView* view_srgb;
};
static_assert(offsetof(TextureDx11, view_unorm) == 0x10);
static_assert(offsetof(TextureDx11, view_srgb) == 0x18);

struct TextureBinding {
    TextureDx11* data;
};

struct TextureHandle {
    TextureBinding* texture;
    std::int64_t index;
};

struct LayerListElement {
    void* layer;
    std::uint16_t prev;
    std::uint16_t next;
};

struct LayerList {
    std::uint16_t size;
    std::uint16_t capacity;
    LayerListElement* data;
    std::uint16_t head;
    std::uint16_t tail;
};

struct UpscaleLayersContext {
    void* view;
    std::uint8_t pad0[0x8];
    RenderViewport viewport;
    std::uint8_t pad1[0x4];
    const int* target_size;
    void* attributes;
    std::int64_t color_target;
    std::uint8_t pad2[0x10];
    std::int64_t depth_target;
};
static_assert(offsetof(UpscaleLayersContext, depth_target) == 0x58);

struct FsrUpscaleRenderer {
    std::uint8_t pad[0x58];
    RenderViewport source_viewport;
    RenderViewport destination_viewport;
};

struct ConVar {
    const char* name;
    std::uint8_t pad[0x58 - 0x8];
    union {
        bool bool_value;
        float float_value;
        int int_value;
    };
};
static_assert(offsetof(ConVar, float_value) == 0x58);

struct ConVarNode {
    ConVar* con_var;
    std::uint8_t pad[0x8];
};

enum class Multisample : std::uint8_t { None = 0, X2, X4, X6, X8, X16 };

struct RenderDeviceConfig {
    std::uint32_t reserved[9];
    Multisample multisample;
    std::uint8_t flags[3];
};
static_assert(sizeof(RenderDeviceConfig) == 0x28);
static_assert(offsetof(RenderDeviceConfig, multisample) == 0x24);

namespace layer_offsets {
constexpr std::size_t viewport = 0x10;
constexpr std::size_t has_color_target = 0x58;
constexpr std::size_t color_target = 0x60;
constexpr std::size_t override_render_targets = 0xA0;
constexpr std::size_t depth_target = 0xA8;
constexpr std::size_t attributes = 0xE0;
constexpr std::size_t view = 0x6F0;
}  // namespace layer_offsets

constexpr std::size_t view_layers = 0x3480;
constexpr int add_view_index = 21;
constexpr int add_procedural_layer_index = 2;
constexpr int render_target_texture_index = 45;
constexpr int queue_upscaler_index = 173;
constexpr std::uint32_t upscale_input_hash = 0xE2A716A1u;

template <typename T>
T& field(void* base, std::size_t offset) {
    return *reinterpret_cast<T*>(reinterpret_cast<std::uint8_t*>(base) + offset);
}

template <typename R, typename... A>
R call_virtual(void* object, int index, A... args) {
    return (*reinterpret_cast<R (***)(void*, A...)>(object))[index](object, args...);
}
