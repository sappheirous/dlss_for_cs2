#pragma once

#include <cstdint>
#include <string>

#include <d3d11.h>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

struct NgxApi {
    std::uint32_t(__cdecl* get_capability_parameters)(NVSDK_NGX_Parameter**) = nullptr;
    std::uint32_t(__cdecl* allocate_parameters)(NVSDK_NGX_Parameter**) = nullptr;
    std::uint32_t(__cdecl* destroy_parameters)(NVSDK_NGX_Parameter*) = nullptr;
    std::uint32_t(__cdecl* create_feature)(ID3D11DeviceContext*, int, NVSDK_NGX_Parameter*,
                                           NVSDK_NGX_Handle**) = nullptr;
    std::uint32_t(__cdecl* evaluate_feature)(ID3D11DeviceContext*, NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*,
                                             void*) = nullptr;
    std::uint32_t(__cdecl* release_feature)(NVSDK_NGX_Handle*) = nullptr;

    void(__cdecl* set_ui)(NVSDK_NGX_Parameter*, const char*, std::uint32_t) = nullptr;
    void(__cdecl* set_i)(NVSDK_NGX_Parameter*, const char*, int) = nullptr;
    void(__cdecl* set_f)(NVSDK_NGX_Parameter*, const char*, float) = nullptr;
    void(__cdecl* set_resource)(NVSDK_NGX_Parameter*, const char*, ID3D11Resource*) = nullptr;
    std::uint32_t(__cdecl* get_i)(NVSDK_NGX_Parameter*, const char*, int*) = nullptr;
    std::uint32_t(__cdecl* get_ui)(NVSDK_NGX_Parameter*, const char*, std::uint32_t*) = nullptr;
    std::uint32_t(__cdecl* get_pointer)(NVSDK_NGX_Parameter*, const char*, void**) = nullptr;
};

extern NgxApi ngx;
bool bind_ngx();
std::string dlss_version();

constexpr bool ngx_failed(std::uint32_t result) noexcept { return (result & 0xFFF00000u) == 0xBAD00000u; }
