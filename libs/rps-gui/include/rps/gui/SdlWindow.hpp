#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <functional>

struct SDL_Window;

namespace rps::gui {

/// Callback for window resize events. Arguments: new width, new height.
using ResizeCallback = std::function<void(uint32_t, uint32_t)>;

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

    // Non-copyable, non-movable
    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;

    /// Create the SDL3 window with the given title and initial size.
    /// @param resizable If false, the window cannot be resized by the user.
    /// @throws std::runtime_error on failure.
    void create(const std::string& title, uint32_t width, uint32_t height, bool resizable = true);

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
    /// If a resize callback is provided, it will be called when the window is resized.
    bool pollEvents(ResizeCallback resizeCb = nullptr);

    /// Post a quit event from another thread, causing pollEvents() to return false.
    void requestClose();

    /// Get the current window size.
    void getSize(uint32_t& width, uint32_t& height) const;

private:
    SDL_Window* m_window = nullptr;
    std::atomic<bool> m_closeRequested{false};
    ResizeCallback m_resizeCb;
};

} // namespace rps::gui
