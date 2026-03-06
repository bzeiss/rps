#pragma once

#include <rps/ipc/Messages.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>

struct SDL_Window;
struct SDL_Renderer;

namespace rps::gui {

/// Callback for window resize events. Arguments: new width, new height.
using ResizeCallback = std::function<void(uint32_t, uint32_t)>;

/// Callback when a preset is selected from the toolbar dropdown.
using PresetSelectedCallback = std::function<void(const std::string& presetId)>;

/// RAII wrapper around an SDL3 window. Provides native handle retrieval
/// and event loop helpers for plugin GUI hosting.
class SdlWindow {
public:
    SdlWindow();
    ~SdlWindow();

    /// Set the callback for window resize events.
    /// This is called both during and after resize (including live drag on Windows).
    void setResizeCallback(ResizeCallback cb);

    /// Called by the SDL event watcher. Do not call directly.
    void handleResize(uint32_t width, uint32_t height);

    /// Called by the SDL event watcher during modal resize to keep sidebar responsive.
    void renderDuringResize();

    // Non-copyable, non-movable
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    /// Create the SDL3 window with the given title and initial size.
    /// @param resizable If false, the window cannot be resized by the user.
    /// @param enableSidebar If true, creates a renderer and ImGui context for a left sidebar.
    /// @throws std::runtime_error on failure.
    void create(const std::string& title, uint32_t width, uint32_t height,
                bool resizable = true, bool enableSidebar = false);

    /// Get the platform-native window handle (HWND on Win32, NSView* on macOS, X11 Window on Linux).
    /// @throws std::runtime_error if window not created.
    void* getNativeHandle() const;

    /// Resize the SDL3 window.
    void resize(uint32_t width, uint32_t height);

    /// Set minimum window size constraint.
    void setMinimumSize(uint32_t width, uint32_t height);

    /// Set maximum window size constraint.
    void setMaximumSize(uint32_t width, uint32_t height);

    /// Process pending SDL events. Returns false if quit/close was requested.
    bool pollEvents(ResizeCallback resizeCb = nullptr);

    /// Post a quit event from another thread, causing pollEvents() to return false.
    void requestClose();

    /// Get the current window size (excluding sidebar).
    void getSize(uint32_t& width, uint32_t& height) const;

    // --- Sidebar / Preset API ---

    /// Get the sidebar width in pixels (0 if sidebar is disabled).
    /// Returns the collapsed strip width when collapsed, or full sidebar width when expanded.
    uint32_t getSidebarWidth() const {
        if (!m_sidebarEnabled) return 0;
        return m_sidebarCollapsed ? kCollapsedStripWidth : m_sidebarWidth;
    }

    /// Set available presets for the sidebar list.
    void setPresets(const std::vector<rps::ipc::PresetInfo>& presets);

    /// Set callback for when a preset is selected from the sidebar.
    void setPresetSelectedCallback(PresetSelectedCallback cb);

private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    std::atomic<bool> m_closeRequested{false};
    ResizeCallback m_resizeCb;

    // Sidebar state
    static constexpr uint32_t kCollapsedStripWidth = 38;
    bool m_sidebarEnabled = false;
    bool m_sidebarCollapsed = true;
    uint32_t m_sidebarWidth = 260;
    bool m_imguiInitialized = false;
    bool m_splitterDragging = false;
    bool m_inResizeRender = false;  // Re-entrancy guard for renderDuringResize
    int m_prevWinX = 0;    // Track window X position for left-edge drag detection
    int m_prevWinW = 0;    // Track window width for left-edge drag detection
    std::vector<rps::ipc::PresetInfo> m_presets;
    int m_selectedPresetIndex = -1;
    char m_presetFilter[256] = {};
    PresetSelectedCallback m_presetSelectedCb;

    void initImGui();
    void shutdownImGui();
    void renderSidebar();
    void toggleSidebar(bool collapse);
};

} // namespace rps::gui
