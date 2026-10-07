#pragma once

#include <atomic>
#include <vector>

#include "game_types.hpp"

struct GameApi {
    void* scene_system = nullptr;
    std::uint8_t* engine_c_var = nullptr;

    void(__fastcall* get_handle_value)(void*, TextureHandle*, std::uint32_t, void*) = nullptr;
    void(__fastcall* build_frustum)(Frustum*, const Vec3*, const Vec3*, float, float, float, float, float, float, float,
                                    float) = nullptr;

    ConVar* viewport_scale = nullptr;
    ConVar* fsr_upsample = nullptr;
};

extern GameApi game;
extern std::atomic_bool is_enabled;

std::vector<std::uint8_t*> find_patterns(const char* module, const char* pattern);
std::uint8_t* find_pattern(const char* module, const char* pattern);
std::uint8_t* relative(std::uint8_t* address, int offset);
bool is_executable(const void* address) noexcept;
bool readable(const void* address, std::size_t bytes) noexcept;

bool initialize_game();
void apply_game_settings();
