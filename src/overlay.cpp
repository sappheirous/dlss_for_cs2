#include "overlay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <Windows.h>
#include <dxgi1_2.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <spdlog/spdlog.h>
#include <wrl/client.h>

#include "dlss.hpp"
#include "d3d11_proxy.hpp"
#include "dx11_state.hpp"
#include "game.hpp"
#include "hooks.hpp"
#include "input.hpp"
#include "ngx.hpp"
#include "overlay_panel.hpp"
#include "settings.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
using Microsoft::WRL::ComPtr;
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using SetFullscreenState = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);

Present o_present{};
Present1 o_present1{};
ResizeBuffers o_resize{};
SetFullscreenState o_fullscreen{};

thread_local bool inside_present = false;
std::recursive_mutex overlay_mutex;

// Identity only: keeping a reference can prevent the engine from replacing a flip-model chain.
IUnknown* current_swap_chain = nullptr;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ImGuiContext* gui = nullptr;
HWND window = nullptr;

std::mutex window_routes_mutex;
std::unordered_map<HWND, WNDPROC> window_routes;

Settings draft;
bool menu_was_drawn = false;
UINT drain_input_message = 0;

class FrameTimer {
public:
    void update() {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - previous_).count();
        const bool first = previous_ == std::chrono::steady_clock::time_point{};
        previous_ = now;
        if (first || seconds > 1.0) {
            elapsed_ = 0.0;
            frames_ = 0;
            frame_ms = 0.0;
            return;
        }

        elapsed_ += seconds;
        ++frames_;
        if (elapsed_ < 0.5) return;

        frame_ms = elapsed_ * 1000.0 / frames_;
        elapsed_ = 0.0;
        frames_ = 0;
    }

    double frame_ms = 0.0;

private:
    std::chrono::steady_clock::time_point previous_{};
    double elapsed_ = 0.0;
    unsigned frames_ = 0;
};

FrameTimer frame_timer;
OverlayPanel panel;
std::string gpu_name = "Unknown GPU";

struct GuiInput {
    HWND hwnd;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
};

std::mutex pending_input_mutex;
std::deque<GuiInput> pending_input;

class GuiContextScope {
public:
    explicit GuiContextScope(ImGuiContext* gui_context) noexcept : previous_(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(gui_context);
    }

    ~GuiContextScope() { ImGui::SetCurrentContext(previous_); }

    GuiContextScope(const GuiContextScope&) = delete;
    GuiContextScope& operator=(const GuiContextScope&) = delete;
    GuiContextScope(GuiContextScope&&) = delete;
    GuiContextScope& operator=(GuiContextScope&&) = delete;

    void forget(ImGuiContext* gui_context) noexcept {
        if (previous_ == gui_context) previous_ = nullptr;
    }

private:
    ImGuiContext* previous_;
};

bool is_gui_input(UINT message) {
    return (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
           message == WM_MOUSELEAVE || message == WM_NCMOUSELEAVE || message == WM_NCMOUSEMOVE ||
           message == WM_KILLFOCUS || message == WM_SETFOCUS;
}

std::optional<GuiInput> decode_raw_keyboard(HWND hwnd, LPARAM lparam) {
    RAWINPUT raw{};
    UINT bytes = sizeof(raw);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &bytes, sizeof(RAWINPUTHEADER)) ==
            static_cast<UINT>(-1) ||
        raw.header.dwType != RIM_TYPEKEYBOARD || raw.data.keyboard.VKey >= 255)
        return std::nullopt;

    const auto& keyboard = raw.data.keyboard;
    LPARAM key_data = 1 | (static_cast<LPARAM>(keyboard.MakeCode) << 16);
    if (keyboard.Flags & RI_KEY_E0) key_data |= 1LL << 24;

    return GuiInput{hwnd, static_cast<UINT>(keyboard.Flags & RI_KEY_BREAK ? WM_KEYUP : WM_KEYDOWN), keyboard.VKey,
                    key_data};
}

void queue_gui_input(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        const auto keyboard = decode_raw_keyboard(hwnd, lparam);
        if (!keyboard) return;

        message = keyboard->message;
        wparam = keyboard->wparam;
        lparam = keyboard->lparam;
    }

    if (!is_gui_input(message)) return;

    std::scoped_lock lock(pending_input_mutex);
    if (!pending_input.empty() && message == WM_MOUSEMOVE && pending_input.back().hwnd == hwnd &&
        pending_input.back().message == WM_MOUSEMOVE)
        pending_input.back() = {hwnd, message, wparam, lparam};
    else {
        if (pending_input.size() >= 512) pending_input.pop_front();
        pending_input.push_back({hwnd, message, wparam, lparam});
    }
}

void drain_gui_input(HWND hwnd) {
    std::deque<GuiInput> messages;
    {
        std::scoped_lock lock(pending_input_mutex);
        messages.swap(pending_input);
    }

    if (!menu_open()) {
        if (!messages.empty()) ImGui::GetIO().AddFocusEvent(false);
        return;
    }

    for (const auto& input : messages)
        if (input.hwnd == hwnd) ImGui_ImplWin32_WndProcHandler(hwnd, input.message, input.wparam, input.lparam);
}

WNDPROC previous_window_proc(HWND hwnd) {
    std::scoped_lock lock(window_routes_mutex);
    const auto route = window_routes.find(hwnd);
    return route == window_routes.end() ? nullptr : route->second;
}

IUnknown* swap_chain_identity(IDXGISwapChain* swap_chain) {
    ComPtr<IUnknown> identity;
    return SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&identity))) ? identity.Get() : nullptr;
}

LRESULT CALLBACK hk_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto previous = previous_window_proc(hwnd);
    if (message == WM_DESTROY && !renderer().display_change_active()) set_menu_open(false);

    {
        // DXGI/Win32 calls on the render thread can synchronously send window messages.
        // Never make the window thread wait for that thread's overlay lock.
        std::unique_lock lock(overlay_mutex, std::try_to_lock);
        if (lock.owns_lock() && hwnd == window && gui) {
            const GuiContextScope gui_context(gui);
            drain_gui_input(hwnd);

            const bool gui_input =
                menu_open() || message == WM_KILLFOCUS || message == WM_SETFOCUS || message == WM_DESTROY;
            const auto handled = gui_input ? ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam) : 0;

            if (menu_open() && message == WM_INPUT) {
                if (const auto keyboard = decode_raw_keyboard(hwnd, lparam))
                    ImGui_ImplWin32_WndProcHandler(hwnd, keyboard->message, keyboard->wparam, keyboard->lparam);
            }

            if (menu_open()) {
                // Do not let SDL replace the cursor selected by the ImGui Win32 backend.
                if (message == WM_SETCURSOR && handled) return handled;
                if (message == WM_INPUT) return DefWindowProcW(hwnd, message, wparam, lparam);
                if (is_gui_input(message) && message != WM_KILLFOCUS && message != WM_SETFOCUS) return 0;
            }
        } else if (menu_open() || message == WM_KILLFOCUS || message == WM_SETFOCUS) {
            // Retain events while rendering owns ImGui; the window thread delivers them later.
            // RAWINPUT handles are decoded now because their lifetime ends with this callback.
            queue_gui_input(hwnd, message, wparam, lparam);

            if (menu_open() && message == WM_INPUT) return DefWindowProcW(hwnd, message, wparam, lparam);
            if (menu_open() && is_gui_input(message)) return 0;
        }
    }

    if (message == drain_input_message) return 0;
    return previous ? CallWindowProcW(previous, hwnd, message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
}

void release_overlay() {
    {
        std::scoped_lock lock(pending_input_mutex);
        pending_input.clear();
    }

    if (window && IsWindow(window) &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &hk_window_proc)
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous_window_proc(window)));

    if (gui) {
        GuiContextScope gui_context(gui);
        gui_context.forget(gui);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(gui);
        gui = nullptr;
    }

    context.Reset();
    device.Reset();
    current_swap_chain = nullptr;
    window = nullptr;
    menu_was_drawn = false;
    frame_timer = {};
}

bool attach(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    DWORD process = 0;
    if (FAILED(swap_chain->GetDesc(&description)) || !description.OutputWindow ||
        !IsWindowVisible(description.OutputWindow) || !GetWindowThreadProcessId(description.OutputWindow, &process) ||
        process != GetCurrentProcessId())
        return false;

    const auto identity = swap_chain_identity(swap_chain);
    if (!identity) return false;

    ComPtr<ID3D11Device> candidate;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&candidate)))) return false;

    renderer().set_display_size(description.BufferDesc.Width, description.BufferDesc.Height);
    if (current_swap_chain == identity && window == description.OutputWindow && device == candidate) return true;

    release_overlay();
    device = candidate;
    device->GetImmediateContext(&context);
    window = description.OutputWindow;
    current_swap_chain = identity;

    gpu_name = "Unknown GPU";
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(candidate.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        std::array<char, 512> name{};
        if (WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, name.data(), static_cast<int>(name.size()),
                                nullptr, nullptr))
            gpu_name = name.data();
    }

    const GuiContextScope gui_context(nullptr);
    gui = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    OverlayPanel::apply_style(ImGui_ImplWin32_GetDpiScaleForHwnd(window));

    const bool win32_ready = ImGui_ImplWin32_Init(window);
    const bool dx11_ready = win32_ready && ImGui_ImplDX11_Init(device.Get(), context.Get());
    if (!dx11_ready) {
        // Only initialized backends may be shut down.
        if (win32_ready) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(gui);
        gui = nullptr;
        release_overlay();
        return false;
    }

    SetLastError(0);
    const auto previous_proc = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (previous_proc && previous_proc != &hk_window_proc) {
        std::scoped_lock lock(window_routes_mutex);
        window_routes[window] = previous_proc;
    }

    const auto replaced_proc =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hk_window_proc)));
    if (!replaced_proc) {
        release_overlay();
        return false;
    }

    if (replaced_proc != &hk_window_proc) {
        std::scoped_lock lock(window_routes_mutex);
        window_routes[window] = replaced_proc;
    }

    draft = store().requested().value;
    spdlog::info("ImGui attached to swap chain {}, HWND {}", static_cast<void*>(swap_chain),
                 static_cast<void*>(window));
    return true;
}

void draw_menu(UINT output_width, UINT output_height) {
    bool open = menu_open();
    const auto previous_settings = draft;
    const auto version = dlss_version();
    const auto input = renderer().input_resolution();
    panel.draw(draft, open,
               {store().status(), store().active(), is_enabled.load(), version, gpu_name, output_width, output_height,
                input[0], input[1]});
    if (draft != previous_settings) store().submit(draft);
    if (!open) set_menu_open(false);
}

void render(IDXGISwapChain* swap_chain, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) return;

    std::scoped_lock lock(overlay_mutex);
    if (renderer().display_change_active() || !attach(swap_chain)) return;

    frame_timer.update();
    panel.sample(frame_timer.frame_ms, is_enabled ? renderer().upscale_time_ms() : std::nullopt);

    if (!menu_open()) {
        menu_was_drawn = false;
        return;
    }

    if (!menu_was_drawn) draft = store().requested().value;
    menu_was_drawn = true;

    // These references expire before Present. No backbuffer survives between overlay frames.
    ComPtr<ID3D11Texture2D> buffer;
    ComPtr<ID3D11RenderTargetView> render_target;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
        FAILED(device->CreateRenderTargetView(buffer.Get(), nullptr, &render_target)))
        return;

    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    ContextStateGuard state(context.Get());
    if (!state.ready()) return;

    const GuiContextScope gui_context(gui);
    auto& io = ImGui::GetIO();
    io.AddFocusEvent(true);
    io.MouseDrawCursor = false;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Borderless windows can stretch a smaller backbuffer to the desktop client area.
    // Keep input in client coordinates and scale the drawing to the actual backbuffer.
    if (io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f)
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(description.Width) / io.DisplaySize.x,
                                            static_cast<float>(description.Height) / io.DisplaySize.y);

    ImGui::NewFrame();
    draw_menu(description.Width, description.Height);
    ImGui::Render();

    auto target = render_target.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

struct PresentGuard {
    PresentGuard() { inside_present = true; }
    ~PresentGuard() { inside_present = false; }
};

void try_render(IDXGISwapChain* swap_chain, UINT flags) noexcept {
    try {
        render(swap_chain, flags);
    } catch (const std::exception& error) {
        is_enabled = false;
        set_menu_open(false);
        store().fail(error.what());
    }
}

HRESULT handle_present_result(HRESULT result) {
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        std::scoped_lock lock(overlay_mutex);
        release_overlay();
        set_menu_open(false);
        is_enabled = false;
        store().fail("DX11 device lost", static_cast<std::uint32_t>(result));
    }

    return result;
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* swap_chain, UINT interval, UINT flags) {
    if (inside_present) return o_present(swap_chain, interval, flags);

    PresentGuard guard;
    try_render(swap_chain, flags);
    return handle_present_result(o_present(swap_chain, interval, flags));
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* swap_chain, UINT interval, UINT flags,
                                      const DXGI_PRESENT_PARAMETERS* parameters) {
    if (inside_present) return o_present1(swap_chain, interval, flags, parameters);

    PresentGuard guard;
    try_render(swap_chain, flags);
    return handle_present_result(o_present1(swap_chain, interval, flags, parameters));
}

struct DisplayChangeGuard {
    DisplayChangeGuard() { renderer().begin_display_change(); }

    ~DisplayChangeGuard() { renderer().end_display_change(); }
};

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* swap_chain, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                    UINT flags) {
    DisplayChangeGuard change;
    spdlog::info("ResizeBuffers begin: chain {}, {}x{}, format {}", static_cast<void*>(swap_chain), width, height,
                 static_cast<int>(format));
    const auto result = o_resize(swap_chain, count, width, height, format, flags);
    spdlog::info("ResizeBuffers end: 0x{:08X}", static_cast<std::uint32_t>(result));
    return result;
}

HRESULT STDMETHODCALLTYPE hk_fullscreen(IDXGISwapChain* swap_chain, BOOL fullscreen, IDXGIOutput* output) {
    DisplayChangeGuard change;
    spdlog::info("SetFullscreenState begin: chain {}, fullscreen {}", static_cast<void*>(swap_chain),
                 fullscreen != FALSE);
    const auto result = o_fullscreen(swap_chain, fullscreen, output);
    spdlog::info("SetFullscreenState end: 0x{:08X}", static_cast<std::uint32_t>(result));
    return result;
}
}  // namespace

bool initialize_overlay() {
    drain_input_message = RegisterWindowMessageW(L"CS2-DLSS.DrainGuiInput");
    if (!drain_input_message) return false;

    // A hidden, temporary DX11 window supplies DXGI method addresses. It never renders gameplay.
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW description{};
    description.lpfnWndProc = DefWindowProcW;
    description.hInstance = instance;
    description.lpszClassName = L"CS2DLSSBootstrap";
    if (!RegisterClassW(&description)) return false;

    const auto dummy_window =
        CreateWindowW(description.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, instance, nullptr);

    DXGI_SWAP_CHAIN_DESC swap_description{};
    swap_description.BufferDesc.Width = 16;
    swap_description.BufferDesc.Height = 16;
    swap_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_description.SampleDesc.Count = 1;
    swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_description.BufferCount = 1;
    swap_description.OutputWindow = dummy_window;
    swap_description.Windowed = TRUE;

    ComPtr<IDXGISwapChain> dummy;
    ComPtr<ID3D11Device> dummy_device;
    HRESULT result =
        create_d3d11_device_and_swap_chain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &swap_description, &dummy, &dummy_device, nullptr, nullptr);
    if (FAILED(result))
        result =
            create_d3d11_device_and_swap_chain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &swap_description, &dummy, &dummy_device, nullptr, nullptr);

    bool ready = false;
    static auto hooks = std::make_unique<HookSet>();
    if (SUCCEEDED(result)) {
        auto table = *reinterpret_cast<void***>(dummy.Get());
        ComPtr<IDXGISwapChain1> dummy1;
        const bool has_present1 = SUCCEEDED(dummy.As(&dummy1));
        ready = hooks->create(table[8], &hk_present, o_present) && hooks->create(table[13], &hk_resize, o_resize) &&
                hooks->create(table[10], &hk_fullscreen, o_fullscreen);
        if (ready && has_present1)
            ready = hooks->create((*reinterpret_cast<void***>(dummy1.Get()))[22], &hk_present1, o_present1);
        ready = ready && hooks->enable();
    }

    if (!ready) hooks->reset();

    dummy.Reset();
    dummy_device.Reset();
    if (dummy_window) DestroyWindow(dummy_window);
    UnregisterClassW(description.lpszClassName, instance);
    return ready;
}

void pump_overlay_input() {
    HWND target = nullptr;
    {
        std::scoped_lock lock(pending_input_mutex);
        while (!pending_input.empty() && !IsWindow(pending_input.front().hwnd)) pending_input.pop_front();
        if (!pending_input.empty()) target = pending_input.front().hwnd;
    }

    // Bootstrap retries at 10 ms, so a busy window callback never reposts in a polling loop.
    if (target && drain_input_message) PostMessageW(target, drain_input_message, 0, 0);
}
