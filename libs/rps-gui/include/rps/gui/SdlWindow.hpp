#pragma once

#include <rps.pb.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <functional>
#include <set>
#include <map>

struct SDL_Window;
struct SDL_Renderer;
typedef struct SDL_GLContextState *SDL_GLContext;

namespace rps::gui {

/// Callback for window resize events. Arguments: new width, new height.
using ResizeCallback = std::function<void(uint32_t, uint32_t)>;

/// Callback when a preset is selected from the toolbar dropdown.
using PresetSelectedCallback = std::function<void(const std::string& presetId)>;

/// Toolbar button toggle callbacks.
struct ToolbarCallbacks {
    std::function<void(bool)> onBypassChanged;
    std::function<void(bool)> onDeltaChanged;
};

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

    /// Destroy the SDL window and renderer, releasing all GPU resources.
    /// Safe to call multiple times. After this, create() can be called again.
    void destroy();

    /// Hide the SDL window without destroying it. Used for CLAP GUI hide/show cycling.
    void hide();

    /// Show a previously hidden SDL window.
    void show();

    /// Get the current window size (excluding sidebar and toolbar).
    void getSize(uint32_t& width, uint32_t& height) const;

    // --- Toolbar API ---

    /// Toolbar height in pixels.
    static constexpr uint32_t kToolbarHeight = 28;

    /// Get the toolbar height (0 if sidebar/toolbar is disabled).
    uint32_t getToolbarHeight() const {
        return m_sidebarEnabled ? kToolbarHeight : 0;
    }

    /// Set callbacks for toolbar button toggles.
    void setToolbarCallbacks(ToolbarCallbacks cb);

    // --- Sidebar / Preset API ---

    /// Get the sidebar width in pixels (0 if sidebar is disabled or collapsed).
    uint32_t getSidebarWidth() const {
        if (!m_sidebarEnabled) return 0;
        return m_sidebarCollapsed ? 0 : m_sidebarWidth;
    }

    /// Set available presets for the sidebar list.
    void setPresets(const rps::v1::PresetList& presets);

    /// Set callback for when a preset is selected from the sidebar.
    void setPresetSelectedCallback(PresetSelectedCallback cb);

    /// Reposition the first child HWND to account for the sidebar offset and toolbar.
    /// Called automatically by handleResize() on Windows. Format hosts do NOT need
    /// to manage child HWND positioning themselves.
    /// Repositions the plugin child window below the toolbar.
    /// Returns the actual child window dimensions (which may differ from pluginW/H on Linux).
    std::pair<uint32_t, uint32_t> repositionChildHwnd(uint32_t pluginW, uint32_t pluginH);


    /// Check if a plugin child window has been detected (set by repositionChildHwnd).
    bool hasPluginChild() const { return m_x11PluginChild != 0; }

    /// Get the underlying SDL_Window pointer (for diagnostics).
    SDL_Window* sdlWindow() const { return m_window; }

private:
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    // Rendering context
    SDL_GLContext m_glContext = nullptr;
    std::atomic<bool> m_closeRequested{false};
    ResizeCallback m_resizeCb;

    // Sidebar state
    bool m_sidebarEnabled = false;
    bool m_xembedSent = false;  // Whether XEmbed activation was sent to child

    // X11 plugin child tracking (on Linux, plugin embeds into SDL window)
    void* m_x11Display = nullptr;        // Cached Display* for per-frame use
    unsigned long m_x11PluginChild = 0;  // Plugin's child window XID
    bool m_mouseInPluginArea = false;    // Track mouse enter/leave for plugin area

    bool m_sidebarCollapsed = true;
    uint32_t m_sidebarWidth = 260;
    bool m_imguiInitialized = false;
    bool m_splitterDragging = false;
    bool m_inResizeRender = false;  // Re-entrancy guard for renderDuringResize
    bool m_inProgrammaticResize = false; // Skip left-edge detection during sidebar toggle
    int m_prevWinX = 0;    // Track window X position for left-edge drag detection
    int m_prevWinW = 0;    // Track window width for left-edge drag detection
    uint32_t m_resizeSerial = 0;       // Incremented by resize() for each programmatic resize
    uint32_t m_lastHandledSerial = 0;  // Last serial seen by handleResize()

    rps::v1::PresetList m_presets;
    int m_selectedPresetIndex = -1;
    char m_presetFilter[256] = {};
    PresetSelectedCallback m_presetSelectedCb;

    // Toolbar state
    bool m_bypassActive = false;
    bool m_deltaActive = false;
    ToolbarCallbacks m_toolbarCallbacks;

    // Category tree filter state
    bool m_allCategoriesSelected = true;
    std::set<std::string> m_selectedCategories;  // Full category paths that are checked

    void initImGui();
    void shutdownImGui();
    void renderToolbar(int winW, int winH);
    void renderSidebar();
    void toggleSidebar(bool collapse);
    void enforceChildPosition();  // Per-frame child reposition (Linux X11)
};

} // namespace rps::gui
