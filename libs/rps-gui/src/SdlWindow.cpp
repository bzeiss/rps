#include <rps/gui/SdlWindow.hpp>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>

namespace rps::gui {

SdlWindow::SdlWindow() = default;

SdlWindow::~SdlWindow() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void SdlWindow::create(const std::string& title, uint32_t width, uint32_t height, bool resizable) {
    Uint32 flags = 0;
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(
        title.c_str(),
        static_cast<int>(width),
        static_cast<int>(height),
        flags
    );
    if (!m_window) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
}

void* SdlWindow::getNativeHandle() const {
    if (!m_window) {
        throw std::runtime_error("SdlWindow: window not created");
    }

    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    if (!props) {
        throw std::runtime_error("SdlWindow: failed to get window properties");
    }

#ifdef _WIN32
    void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) {
        throw std::runtime_error("SdlWindow: failed to get Win32 HWND");
    }
    return hwnd;
#elif defined(__APPLE__)
    void* nsview = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!nsview) {
        throw std::runtime_error("SdlWindow: failed to get Cocoa NSWindow");
    }
    return nsview;
#elif defined(__linux__)
    // Try X11 first, then Wayland
    void* xwindow = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, nullptr);
    if (xwindow) {
        return xwindow;
    }
    throw std::runtime_error("SdlWindow: no supported window handle found on Linux (X11 required)");
#else
    throw std::runtime_error("SdlWindow: unsupported platform");
#endif
}

void SdlWindow::resize(uint32_t width, uint32_t height) {
    if (m_window) {
        SDL_SetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
    }
}

void SdlWindow::setMinimumSize(uint32_t width, uint32_t height) {
    if (m_window) {
        SDL_SetWindowMinimumSize(m_window, static_cast<int>(width), static_cast<int>(height));
    }
}

bool SdlWindow::pollEvents(ResizeCallback resizeCb) {
    if (m_closeRequested.load(std::memory_order_relaxed)) {
        return false;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                return false;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                return false;
            case SDL_EVENT_WINDOW_RESIZED:
                if (resizeCb) {
                    resizeCb(static_cast<uint32_t>(event.window.data1),
                             static_cast<uint32_t>(event.window.data2));
                }
                break;
            default:
                break;
        }
    }
    return true;
}

void SdlWindow::requestClose() {
    m_closeRequested.store(true, std::memory_order_relaxed);
    // Push a quit event to wake up pollEvents() if blocking
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    event.common.timestamp = 0;
    SDL_PushEvent(&event);
}

void SdlWindow::getSize(uint32_t& width, uint32_t& height) const {
    if (m_window) {
        int w = 0, h = 0;
        SDL_GetWindowSize(m_window, &w, &h);
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
    }
}

} // namespace rps::gui
