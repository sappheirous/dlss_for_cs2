#include "ngx.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <mutex>
#include <vector>

#include <Windows.h>
#include <spdlog/spdlog.h>

namespace {
template <typename Function>
bool bind_export(HMODULE module, const char* name, Function& function) {
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (function) return true;

    spdlog::error("NGX export {} is missing from rendersystemdx11.dll", name);
    return false;
}
}  // namespace

NgxApi ngx;

std::string dlss_version() {
    static std::mutex mutex;
    static HMODULE previous_module = nullptr;
    static std::string version = "not loaded";
    std::scoped_lock lock(mutex);
    const auto module = GetModuleHandleW(L"nvngx_dlss.dll");
    if (module == previous_module) return version;

    previous_module = module;
    version = module ? "unavailable" : "not loaded";
    if (!module) return version;

    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return version;

    const auto size = GetFileVersionInfoSizeW(path.data(), nullptr);
    if (!size) return version;
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.data(), 0, size, data.data())) return version;

    VS_FIXEDFILEINFO* info = nullptr;
    UINT info_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) || info_size < sizeof(*info) ||
        info->dwSignature != 0xFEEF04BD)
        return version;

    version = std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                          HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return version;
}

bool bind_ngx() {
    const auto module = GetModuleHandleW(L"rendersystemdx11.dll");
    if (!module) return false;

    NgxApi api;
    if (!bind_export(module, "NVSDK_NGX_D3D11_GetCapabilityParameters", api.get_capability_parameters) ||
        !bind_export(module, "NVSDK_NGX_D3D11_AllocateParameters", api.allocate_parameters) ||
        !bind_export(module, "NVSDK_NGX_D3D11_DestroyParameters", api.destroy_parameters) ||
        !bind_export(module, "NVSDK_NGX_D3D11_CreateFeature", api.create_feature) ||
        !bind_export(module, "NVSDK_NGX_D3D11_EvaluateFeature_C", api.evaluate_feature) ||
        !bind_export(module, "NVSDK_NGX_D3D11_ReleaseFeature", api.release_feature) ||
        !bind_export(module, "NVSDK_NGX_Parameter_SetUI", api.set_ui) ||
        !bind_export(module, "NVSDK_NGX_Parameter_SetI", api.set_i) ||
        !bind_export(module, "NVSDK_NGX_Parameter_SetF", api.set_f) ||
        !bind_export(module, "NVSDK_NGX_Parameter_SetD3d11Resource", api.set_resource) ||
        !bind_export(module, "NVSDK_NGX_Parameter_GetI", api.get_i) ||
        !bind_export(module, "NVSDK_NGX_Parameter_GetUI", api.get_ui) ||
        !bind_export(module, "NVSDK_NGX_Parameter_GetVoidPointer", api.get_pointer))
        return false;

    ngx = api;
    return true;
}
