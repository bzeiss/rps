#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef __linux__
#include <X11/Xlib.h>
// Xlib.h pollutes the global namespace with macros like Status, Bool, None, etc.
// These clash with protobuf/abseil headers. Undefine them after extracting what we need.
#undef Status
#undef Bool
#undef None
#undef Above
#undef Below
#endif

#include <rps/gui/SdlWindow.hpp>
#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <spdlog/spdlog.h>

namespace rps::gui {

namespace {
// Free function for SDL_AddEventWatch — fires during Windows modal resize loop.
bool sdlEventWatcher(void* userdata, SDL_Event* event) {
    auto* self = static_cast<SdlWindow*>(userdata);
    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        self->handleResize(
            static_cast<uint32_t>(event->window.data1),
            static_cast<uint32_t>(event->window.data2)
        );
    }
    return true;
}
} // anonymous namespace

SdlWindow::SdlWindow() = default;

SdlWindow::~SdlWindow() {
    destroy();
}

void SdlWindow::destroy() {
    shutdownImGui();
    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_window) {
        SDL_RemoveEventWatch(sdlEventWatcher, this);
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    // Flush any stale quit/close events so they don't affect the next window
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}

void SdlWindow::hide() {
    if (m_window) {
        SDL_HideWindow(m_window);
    }
}

void SdlWindow::show() {
    if (m_window) {
        SDL_ShowWindow(m_window);
    }
}

void SdlWindow::create(const std::string& title, uint32_t width, uint32_t height,
                        bool resizable, bool enableSidebar) {
    // Flush any stale events from a previous window
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    m_sidebarEnabled = enableSidebar;

    // If sidebar is enabled, make the window wider/taller to accommodate sidebar + toolbar
    uint32_t totalWidth = width;
    uint32_t totalHeight = height;
    if (m_sidebarEnabled) {
        totalWidth += getSidebarWidth();
        totalHeight += kToolbarHeight;
    }

    // Don't set SDL_WINDOW_RESIZABLE — resizing is driven by the plugin's own
    // resize control (request_resize → m_window.resize()). SDL_SetWindowSize
    // still works programmatically without this flag.
    Uint32 flags = 0;
    (void)resizable;

#ifdef _WIN32
    // Prevent SDL from painting black over the entire client area (including
    // plugin child HWNDs) during WM_ERASEBKGND. SDL3's default handler calls
    // FillRect(GetDC(hwnd), &client_rect, black_brush), which overwrites plugin
    // content on every resize. Must be set before SDL_CreateWindow.
    SDL_SetHint(SDL_HINT_WINDOWS_ERASE_BACKGROUND_MODE, "never");

    // Preserve existing pixel content during SDL's internal programmatic resizes.
    // SDL3 defaults copybits_flag to SWP_NOCOPYBITS, which tells Windows to
    // discard old pixel content when SetWindowPos is called (causes flash).
    // Setting this hint makes SDL use 0 instead, preserving the rendered
    // plugin content during resize transitions.
    SDL_SetHint("SDL_WINDOW_RETAIN_CONTENT", "1");
#endif


    m_window = SDL_CreateWindow(
        title.c_str(),
        static_cast<int>(totalWidth),
        static_cast<int>(totalHeight),
        flags
    );
    if (!m_window) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    // Register event watcher for live resize on Windows.
    SDL_AddEventWatch(sdlEventWatcher, this);

    // Create renderer + ImGui if sidebar enabled
    if (m_sidebarEnabled) {
        m_renderer = SDL_CreateRenderer(m_window, nullptr);
        if (!m_renderer) {
            spdlog::warn("Failed to create SDL_Renderer: {} — sidebar disabled", SDL_GetError());
            m_sidebarEnabled = false;
        } else {
            initImGui();
        }
    }

    // Initialize position tracking for left-edge drag detection
    SDL_GetWindowPosition(m_window, &m_prevWinX, nullptr);
    SDL_GetWindowSize(m_window, &m_prevWinW, nullptr);
}

void SdlWindow::initImGui() {
    if (m_imguiInitialized) return;

    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifndef __linux__
    // Multi-viewport creates additional platform windows. On Linux/X11, this
    // can interfere with mouse event routing to the plugin's child window
    // (grabs the pointer during drags). Safe on Windows/macOS.
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
#endif
    // Disable imgui.ini saving
    io.IniFilename = nullptr;

    // Style: dark theme with slight customization
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameRounding = 3.0f;
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);

    // Make the sidebar background darker
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);

    ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
    ImGui_ImplSDLRenderer3_Init(m_renderer); // Initialize Dear ImGui
    m_imguiInitialized = true;
    if (m_sidebarEnabled) {
        spdlog::info("Dear ImGui initialized for sidebar");
    }


}

void SdlWindow::shutdownImGui() {
    if (!m_imguiInitialized) return;

    if (m_imguiInitialized) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_imguiInitialized = false;
    }



    if (m_renderer) {
        // This block was already present, but the user's snippet cut it off.
        // It should remain here.
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
    auto xwindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (xwindow) {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(xwindow));
    }
    throw std::runtime_error("SdlWindow: no supported window handle found on Linux (X11 required)");
#else
    throw std::runtime_error("SdlWindow: unsupported platform");
#endif
}

void SdlWindow::resize(uint32_t width, uint32_t height) {
    if (!m_window) return;

    uint32_t totalWidth = width;
    uint32_t totalHeight = height;
    if (m_sidebarEnabled) {
        totalWidth += getSidebarWidth();
        totalHeight += kToolbarHeight;
    }

    spdlog::info("SdlWindow::resize: {}x{} -> SDL total {}x{}", width, height, totalWidth, totalHeight);
    SDL_SetWindowSize(m_window, static_cast<int>(totalWidth), static_cast<int>(totalHeight));
}

void SdlWindow::setMinimumSize(uint32_t width, uint32_t height) {
    if (m_window) {
        uint32_t totalWidth = width;
        uint32_t totalHeight = height;
        if (m_sidebarEnabled) {
            totalWidth += getSidebarWidth();
            totalHeight += kToolbarHeight;
        }
        SDL_SetWindowMinimumSize(m_window, static_cast<int>(totalWidth), static_cast<int>(totalHeight));
    }
}

void SdlWindow::setMaximumSize(uint32_t width, uint32_t height) {
    if (m_window) {
        uint32_t totalWidth = width;
        uint32_t totalHeight = height;
        if (m_sidebarEnabled) {
            totalWidth += getSidebarWidth();
            totalHeight += kToolbarHeight;
        }
        SDL_SetWindowMaximumSize(m_window, static_cast<int>(totalWidth), static_cast<int>(totalHeight));
    }
}

bool SdlWindow::pollEvents(ResizeCallback /*resizeCb*/) {
    if (m_closeRequested.load(std::memory_order_relaxed)) {
        return false;
    }

    // Block on the OS message queue for up to 16ms (~60Hz).
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, 16)) {
        do {
            bool inPluginArea = false;

#ifdef __linux__
            // Check if this mouse event is in the plugin child area.
            // If so, don't pass it to ImGui — the child window receives
            // native X11 events directly.
            if (m_sidebarEnabled &&
                (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                 event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                 event.type == SDL_EVENT_MOUSE_MOTION ||
                 event.type == SDL_EVENT_MOUSE_WHEEL)) {

                float my = (event.type == SDL_EVENT_MOUSE_MOTION) ? event.motion.y
                         : (event.type == SDL_EVENT_MOUSE_WHEEL) ? event.wheel.mouse_y
                         : event.button.y;
                float toolbarH = static_cast<float>(getToolbarHeight());
                float sidebarW = static_cast<float>(getSidebarWidth());
                float mx = (event.type == SDL_EVENT_MOUSE_MOTION) ? event.motion.x
                         : (event.type == SDL_EVENT_MOUSE_WHEEL) ? event.wheel.mouse_x
                         : event.button.x;

                if (my > toolbarH && mx > sidebarW) {
                    inPluginArea = true;
                    m_mouseInPluginArea = true;
                } else {
                    m_mouseInPluginArea = false;
                }
            }

#endif // __linux__

            // Feed events to ImGui if toolbar active — but NOT mouse events
            // in the plugin area (those go to the child via native X11)
            if (m_imguiInitialized && !inPluginArea) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return false;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    return false;
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    m_lastResizeTime = std::chrono::steady_clock::now();
                    break;
                default:
                    break;
            }
        } while (SDL_PollEvent(&event));
    }

    // Render sidebar if enabled — suppress for 200ms after last resize to avoid
    // SDL renderer painting over the plugin's area during drag resize
    bool recentlyResized = (std::chrono::steady_clock::now() - m_lastResizeTime)
                           < std::chrono::milliseconds(200);
    if (m_imguiInitialized && m_renderer && !recentlyResized) {
        renderSidebar();
    }

    return true;
}

void SdlWindow::renderToolbar(int winW, int winH) {
    (void)winH;
    float toolbarH = static_cast<float>(kToolbarHeight);
    float sidebarW = static_cast<float>(getSidebarWidth());
    float toolbarX = sidebarW;
    float toolbarW = static_cast<float>(winW) - sidebarW;

    ImGui::SetNextWindowPos(ImVec2(toolbarX, 0));
    ImGui::SetNextWindowSize(ImVec2(toolbarW, toolbarH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::Begin("##Toolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNav);

    float btnH = toolbarH - 8.0f; // button height with padding

    // --- Left: Presets toggle (at left edge of toolbar = left edge of plugin area) ---
    if (m_sidebarCollapsed) {
        if (ImGui::Button("<< Presets", ImVec2(0, btnH))) {
            toggleSidebar(false);
        }
    } else {
        if (ImGui::Button(">> Presets", ImVec2(0, btnH))) {
            toggleSidebar(true);
        }
    }

    // --- Right side buttons ---
    // Calculate right-aligned positions
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImVec2 levelsSize = ImGui::CalcTextSize("Output Levels >>");
    ImVec2 bypassSize = ImGui::CalcTextSize("Bypass");
    ImVec2 deltaSize = ImGui::CalcTextSize("Delta");
    float btnPadX = ImGui::GetStyle().FramePadding.x * 2.0f;

    // Right-aligned positions are relative to toolbar width (not window width)
    float rightEdge = toolbarW - ImGui::GetStyle().WindowPadding.x;
    float levelsW = levelsSize.x + btnPadX;
    float bypassW = bypassSize.x + btnPadX;
    float deltaW = deltaSize.x + btnPadX;

    float levelsX = rightEdge - levelsW;
    float bypassX = levelsX - spacing - bypassW;
    float deltaX = bypassX - spacing - deltaW;

    // Delta toggle
    ImGui::SameLine();
    ImGui::SetCursorPosX(deltaX);
    bool deltaWasActive = m_deltaActive;
    if (deltaWasActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.55f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.65f, 0.15f, 1.0f));
    }
    if (ImGui::Button("Delta", ImVec2(deltaW, btnH))) {
        m_deltaActive = !m_deltaActive;
        if (m_toolbarCallbacks.onDeltaChanged) {
            m_toolbarCallbacks.onDeltaChanged(m_deltaActive);
        }
    }
    if (deltaWasActive) {
        ImGui::PopStyleColor(2);
    }

    // Bypass toggle
    ImGui::SameLine();
    ImGui::SetCursorPosX(bypassX);
    bool bypassWasActive = m_bypassActive;
    if (bypassWasActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.30f, 0.30f, 1.0f));
    }
    if (ImGui::Button("Bypass", ImVec2(bypassW, btnH))) {
        m_bypassActive = !m_bypassActive;
        if (m_toolbarCallbacks.onBypassChanged) {
            m_toolbarCallbacks.onBypassChanged(m_bypassActive);
        }
    }
    if (bypassWasActive) {
        ImGui::PopStyleColor(2);
    }

    // Output Levels placeholder
    ImGui::SameLine();
    ImGui::SetCursorPosX(levelsX);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.22f, 1.0f));
    ImGui::Button("Output Levels >>", ImVec2(levelsW, btnH));
    ImGui::PopStyleColor();

    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
}

void SdlWindow::renderSidebar() {
    // Start ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Get window size
    int winW = 0, winH = 0;
    SDL_GetWindowSize(m_window, &winW, &winH);

    // Render toolbar first (always visible)
    renderToolbar(winW, winH);

    // Current effective sidebar width
    uint32_t effectiveWidth = getSidebarWidth();

    // Sidebar takes full window height (toolbar is beside it, not above)
    float sidebarY = 0.0f;
    float sidebarH = static_cast<float>(winH);

    // --- Splitter: only active when sidebar is expanded ---
    if (!m_sidebarCollapsed) {
        ImGuiIO& io = ImGui::GetIO();
        float splitterX = static_cast<float>(effectiveWidth);
        const float splitterHalfW = 4.0f;

        bool hovered = (io.MousePos.x >= splitterX - splitterHalfW &&
                        io.MousePos.x <= splitterX + splitterHalfW &&
                        io.MousePos.y >= sidebarY &&
                        io.MousePos.y <= static_cast<float>(winH));

        if (hovered || m_splitterDragging) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_splitterDragging = true;
        }
        if (m_splitterDragging) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                float delta = io.MouseDelta.x;
                if (delta != 0.0f) {
                    int newWidth = static_cast<int>(m_sidebarWidth) + static_cast<int>(delta);
                    if (newWidth < 150) newWidth = 150;
                    if (newWidth > winW - 100) newWidth = winW - 100;

                    if (static_cast<uint32_t>(newWidth) != m_sidebarWidth) {
                        m_sidebarWidth = static_cast<uint32_t>(newWidth);
                        handleResize(static_cast<uint32_t>(winW), static_cast<uint32_t>(winH));
                    }
                }
            } else {
                m_splitterDragging = false;
            }
        }
    }

    if (m_sidebarCollapsed) {
        // ===== COLLAPSED: no sidebar panel, only toolbar button =====
    } else {
        // ===== EXPANDED: full preset browser sidebar =====
        ImGui::SetNextWindowPos(ImVec2(0, sidebarY));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(m_sidebarWidth), sidebarH));
        ImGui::Begin("##Sidebar", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header row: "PRESETS"
        ImGui::TextDisabled("PRESETS");
        ImGui::Separator();
        ImGui::Spacing();

        // --- Category tree dropdown ---
        // Build category tree from preset data
        struct CatNode {
            std::string fullPath;
            int count = 0;  // Number of presets at this exact category
            std::map<std::string, CatNode> children;

            // Recursive total: this node + all descendants
            int totalCount() const {
                int total = count;
                for (const auto& [_, c] : children) total += c.totalCount();
                return total;
            }
        };
        CatNode catRoot;
        for (int pi = 0; pi < m_presets.presets_size(); ++pi) {
            const auto& preset = m_presets.presets(pi);
            if (preset.category().empty()) continue;
            // Split by '|' (VST3) or '/' (CLAP)
            std::string cat = preset.category();
            CatNode* node = &catRoot;
            std::string pathSoFar;
            size_t start = 0;
            while (start < cat.size()) {
                size_t sepPipe = cat.find('|', start);
                size_t sepSlash = cat.find('/', start);
                size_t sep = std::min(sepPipe, sepSlash);
                std::string part = (sep == std::string::npos) ? cat.substr(start) : cat.substr(start, sep - start);
                if (!part.empty()) {
                    if (!pathSoFar.empty()) pathSoFar += '|';
                    pathSoFar += part;
                    auto& child = node->children[part];
                    child.fullPath = pathSoFar;
                    node = &child;
                }
                start = (sep == std::string::npos) ? cat.size() : sep + 1;
            }
            // Increment count on the leaf node
            node->count++;
        }

        // Determine preview text for the combobox
        std::string comboPreview;
        if (m_allCategoriesSelected) {
            comboPreview = "All";
        } else if (m_selectedCategories.empty()) {
            comboPreview = "None";
        } else if (m_selectedCategories.size() == 1) {
            comboPreview = *m_selectedCategories.begin();
        } else {
            comboPreview = std::to_string(m_selectedCategories.size()) + " selected";
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        // Allow the combo popup to grow tall for deep category trees
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, static_cast<float>(winH) * 0.8f));
        if (ImGui::BeginCombo("##CatFilter", comboPreview.c_str())) {
            // "All" toggle at the top
            bool allChecked = m_allCategoriesSelected;
            if (ImGui::Checkbox("All", &allChecked)) {
                m_allCategoriesSelected = allChecked;
                if (allChecked) {
                    m_selectedCategories.clear();
                }
            }
            ImGui::Separator();

            if (catRoot.children.empty()) {
                ImGui::TextDisabled("No categories available");
            } else {
                // Recursive tree rendering lambda
                std::function<void(const std::map<std::string, CatNode>&)> renderTree;
                renderTree = [&](const std::map<std::string, CatNode>& children) {
                    for (const auto& [label, child] : children) {
                        bool isSelected = m_selectedCategories.contains(child.fullPath);
                        int total = child.totalCount();
                        std::string displayLabel = label + " (" + std::to_string(total) + ")";

                        if (child.children.empty()) {
                            // Leaf node: just a checkbox
                            bool checked = isSelected;
                            if (ImGui::Checkbox(displayLabel.c_str(), &checked)) {
                                m_allCategoriesSelected = false;
                                if (checked) {
                                    m_selectedCategories.insert(child.fullPath);
                                } else {
                                    m_selectedCategories.erase(child.fullPath);
                                }
                            }
                        } else {
                            // Branch node: tree node with checkbox
                            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
                            bool nodeOpen = ImGui::TreeNodeEx(("##tree_" + child.fullPath).c_str(), flags);

                            ImGui::SameLine();
                            bool checked = isSelected;
                            if (ImGui::Checkbox(displayLabel.c_str(), &checked)) {
                                m_allCategoriesSelected = false;
                                if (checked) {
                                    m_selectedCategories.insert(child.fullPath);
                                } else {
                                    m_selectedCategories.erase(child.fullPath);
                                }
                            }

                            if (nodeOpen) {
                                renderTree(child.children);
                                ImGui::TreePop();
                            }
                        }
                    }
                };
                renderTree(catRoot.children);
            }

            ImGui::EndCombo();
        }
        ImGui::Spacing();

        // Search bar
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##Filter", "Search...", m_presetFilter, sizeof(m_presetFilter));
        ImGui::Spacing();

        ImGui::BeginChild("PresetListScroll", ImVec2(0, 0), true);

        if (m_presets.presets_size() == 0) {
            ImGui::TextDisabled("No presets found.");
        } else {
            std::string filterLower = m_presetFilter;
            std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

            // Build filtered index list
            std::vector<int> filteredIndices;
            filteredIndices.reserve(m_presets.presets_size());

            for (int i = 0; i < m_presets.presets_size(); ++i) {
                const auto& preset = m_presets.presets(i);

                // Category filter
                if (!m_allCategoriesSelected) {
                    if (m_selectedCategories.empty()) {
                        // "None" selected — hide everything
                        continue;
                    }
                    // Check if preset's category matches any selected category
                    bool catMatch = false;
                    if (!preset.category().empty()) {
                        // Normalize to '|' separator to match tree paths
                        std::string cat = preset.category();
                        std::replace(cat.begin(), cat.end(), '/', '|');
                        // Check exact match
                        if (m_selectedCategories.contains(cat)) {
                            catMatch = true;
                        }
                        // Check parent path matches (e.g., if "Fx" is selected, "Fx|EQ" matches)
                        if (!catMatch) {
                            for (const auto& sel : m_selectedCategories) {
                                if (cat.starts_with(sel + "|")) {
                                    catMatch = true;
                                    break;
                                }
                            }
                        }
                    }
                    // Uncategorized presets don't match any specific category
                    if (!catMatch) continue;
                }

                // Text filter
                if (!filterLower.empty()) {
                    std::string nameLower = preset.name();
                    std::string authorLower = preset.creator();
                    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                    std::transform(authorLower.begin(), authorLower.end(), authorLower.begin(), ::tolower);

                    if (nameLower.find(filterLower) == std::string::npos &&
                        authorLower.find(filterLower) == std::string::npos) {
                        continue;
                    }
                }
                filteredIndices.push_back(i);
            }

            if (ImGui::BeginTable("PresetsTable", 1,
                ImGuiTableFlags_Sortable |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_ScrollY)) {

                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // Sort
                if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                    if (sortSpecs->SpecsDirty || true) {
                        const auto& presets = m_presets;
                        std::stable_sort(filteredIndices.begin(), filteredIndices.end(),
                            [&presets, sortSpecs](int lhs, int rhs) {
                                for (int n = 0; n < sortSpecs->SpecsCount; ++n) {
                                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[n];
                                    int cmp = presets.presets(lhs).name().compare(
                                              presets.presets(rhs).name());
                                    if (cmp != 0) {
                                        return (spec.SortDirection == ImGuiSortDirection_Ascending) ? cmp < 0 : cmp > 0;
                                    }
                                }
                                return lhs < rhs;
                            });
                        sortSpecs->SpecsDirty = false;
                    }
                }

                for (int idx : filteredIndices) {
                    const auto& preset = m_presets.presets(idx);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    const bool isSelected = (m_selectedPresetIndex == idx);

                    // Build display text: "Name" or "Name  — Author"
                    ImGui::PushID(idx);
                    if (ImGui::Selectable(preset.name().c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                        m_selectedPresetIndex = idx;
                        if (m_presetSelectedCb) {
                            m_presetSelectedCb(preset.id());
                        }
                    }

                    // Append dimmed author suffix on the same line
                    if (!preset.creator().empty()) {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                        ImGui::TextUnformatted(preset.creator().c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();

                    if (isSelected && ImGui::IsWindowAppearing()) {
                        ImGui::SetScrollHereY();
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::End();
    }

    // Render ImGui
    ImGui::Render();

    // Draw only the toolbar and sidebar backgrounds — do NOT clear the entire
    // window. SDL_RenderClear would overwrite the plugin's child window content
    // (the child renders into its own X11 window inside the parent).

    // Sidebar background (full height, left side)
    if (!m_sidebarCollapsed) {
        SDL_FRect sidebarFillRect{0.0f, 0.0f,
                                  static_cast<float>(getSidebarWidth()),
                                  static_cast<float>(winH)};
        SDL_SetRenderDrawColor(m_renderer, 30, 30, 36, 255);
        SDL_RenderFillRect(m_renderer, &sidebarFillRect);
    }

    // Toolbar background (top strip, to the right of sidebar)
    float tbX = static_cast<float>(getSidebarWidth());
    SDL_FRect toolbarFillRect{tbX, 0.0f, static_cast<float>(winW) - tbX, static_cast<float>(kToolbarHeight)};
    SDL_SetRenderDrawColor(m_renderer, 25, 25, 30, 255);
    SDL_RenderFillRect(m_renderer, &toolbarFillRect);

    // Render ImGui draw data
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), m_renderer);

    SDL_RenderPresent(m_renderer);

    // Update and Render additional Platform Windows (for Viewports if enabled)
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void SdlWindow::renderDuringResize() {
    if (m_inResizeRender) return;  // Prevent re-entrancy
    if (!m_imguiInitialized || !m_renderer) return;

    m_inResizeRender = true;
    renderSidebar();
    m_inResizeRender = false;
}

void SdlWindow::setResizeCallback(ResizeCallback cb) {
    m_resizeCb = std::move(cb);
}

void SdlWindow::handleResize(uint32_t width, uint32_t height) {
    if (!m_window) return;

    // Get current window position for left-edge drag detection
    int curX = 0;
    SDL_GetWindowPosition(m_window, &curX, nullptr);
    int curW = static_cast<int>(width);

    // Detect left-edge drag: window X changed AND width changed
    // If the right edge is fixed (X changed, width changed by opposite amount),
    // the user dragged the left edge.
    bool leftEdgeDragged = false;
    if (m_sidebarEnabled && !m_sidebarCollapsed && m_prevWinW > 0 && !m_inProgrammaticResize) {
        int xDelta = curX - m_prevWinX; // negative = dragged left (window grew)
        int wDelta = curW - m_prevWinW; // positive = window grew

        // Left-edge drag: X moves and width changes by the opposite amount
        // (within a small tolerance for rounding)
        if (xDelta != 0 && wDelta != 0 && std::abs(xDelta + wDelta) <= 2) {
            leftEdgeDragged = true;

            // Absorb width delta into sidebar width
            int newSidebarW = static_cast<int>(m_sidebarWidth) + wDelta;
            if (newSidebarW < 150) {
                newSidebarW = 150;
            }
            if (newSidebarW > curW - 100) {
                newSidebarW = curW - 100;
            }
            m_sidebarWidth = static_cast<uint32_t>(newSidebarW);

            spdlog::debug("Left-edge drag detected: xDelta={}, wDelta={}, sidebarW={}",
                          xDelta, wDelta, m_sidebarWidth);
        }
    }

    // Update tracking state
    m_prevWinX = curX;
    m_prevWinW = curW;

    uint32_t effSidebar = getSidebarWidth();
    uint32_t effToolbar = getToolbarHeight();
    uint32_t pluginWidth = width;
    uint32_t pluginHeight = height > effToolbar ? height - effToolbar : height;
    if (m_sidebarEnabled && pluginWidth > effSidebar) {
        pluginWidth -= effSidebar;
    }

    if (leftEdgeDragged) {
        // Left-edge drag: sidebar absorbed the change, but still update child HWND offset
        if (m_resizeCb) {
            m_resizeCb(pluginWidth, pluginHeight);
        }
    } else {
        // Normal resize (right/top/bottom edge or programmatic)
        if (m_resizeCb) {
            m_resizeCb(pluginWidth, pluginHeight);
        }
    }

    // Reposition the plugin's child window to account for sidebar + toolbar offset
    repositionChildHwnd(pluginWidth, pluginHeight);
}

std::pair<uint32_t, uint32_t> SdlWindow::repositionChildHwnd(uint32_t pluginW, uint32_t pluginH) {
    if (!m_window || !m_sidebarEnabled) return {pluginW, pluginH};

#ifdef _WIN32
    HWND parentHwnd = static_cast<HWND>(getNativeHandle());
    HWND child = GetWindow(parentHwnd, GW_CHILD);
    if (child) {
        int sidebarW = static_cast<int>(getSidebarWidth());
        int toolbarH = static_cast<int>(getToolbarHeight());
        SetWindowPos(child, nullptr, sidebarW, toolbarH,
                     static_cast<int>(pluginW), static_cast<int>(pluginH),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return {pluginW, pluginH};
#elif defined(__linux__)
    SDL_PropertiesID props = SDL_GetWindowProperties(m_window);
    if (!props) return {pluginW, pluginH};

    auto* display = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    auto parent = static_cast<Window>(
        SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (!display || !parent) return {pluginW, pluginH};

    Window rootRet, parentRet, *children = nullptr;
    unsigned int nChildren = 0;
    if (!XQueryTree(display, parent, &rootRet, &parentRet, &children, &nChildren)
        || !nChildren || !children) {
        if (children) XFree(children);
        return {pluginW, pluginH};
    }

    int sidebarW = static_cast<int>(getSidebarWidth());
    int toolbarH = static_cast<int>(getToolbarHeight());
    uint32_t actualW = pluginW, actualH = pluginH;

    for (unsigned int i = 0; i < nChildren; ++i) {
        Window cr; int cx, cy; unsigned int cw, ch, cb, cd;
        if (XGetGeometry(display, children[i], &cr, &cx, &cy, &cw, &ch, &cb, &cd)) {
            if (cw > actualW) actualW = cw;
            if (ch > actualH) actualH = ch;
            XMoveResizeWindow(display, children[i], sidebarW, toolbarH, cw, ch);
            XMapWindow(display, children[i]);
            m_x11PluginChild = children[i];
        }
    }
    XFree(children);

    if (actualW != pluginW || actualH != pluginH) {
        SDL_SetWindowSize(m_window,
            static_cast<int>(actualW) + sidebarW,
            static_cast<int>(actualH) + toolbarH);
    }
    XFlush(display);

    spdlog::debug("repositionChildHwnd: child at ({},{}) {}x{}", sidebarW, toolbarH, actualW, actualH);
    return {actualW, actualH};
#else
    (void)pluginW;
    (void)pluginH;
    return {pluginW, pluginH};
#endif
}

void SdlWindow::toggleSidebar(bool collapse) {
    if (!m_window || !m_sidebarEnabled) return;

    uint32_t oldEffective = getSidebarWidth();
    m_sidebarCollapsed = collapse;
    uint32_t newEffective = getSidebarWidth();

    int delta = static_cast<int>(newEffective) - static_cast<int>(oldEffective);

    // Get current window size and position
    int winW = 0, winH = 0;
    SDL_GetWindowSize(m_window, &winW, &winH);
    int winX = 0, winY = 0;
    SDL_GetWindowPosition(m_window, &winX, &winY);

    // Resize the OS window: grow/shrink by the sidebar delta
    int newWinW = winW + delta;
    if (newWinW < 200) newWinW = 200;

    // Shift the window position so the plugin content stays in the same screen location
    int newWinX = winX - delta;

    // CRITICAL: Update min/max constraints BEFORE resizing.
    // setMinimumSize/setMaximumSize were called during plugin init with the old
    // (collapsed) sidebar width baked in. If we don't adjust them, the OS will
    // enforce the old max constraint and block the window from growing.
    {
        int curMinW = 0, curMinH = 0;
        SDL_GetWindowMinimumSize(m_window, &curMinW, &curMinH);
        if (curMinW > 0) {
            SDL_SetWindowMinimumSize(m_window, curMinW + delta, curMinH);
        }
        int curMaxW = 0, curMaxH = 0;
        SDL_GetWindowMaximumSize(m_window, &curMaxW, &curMaxH);
        if (curMaxW > 0) {
            SDL_SetWindowMaximumSize(m_window, curMaxW + delta, curMaxH);
        }
    }

    // CRITICAL: Update tracking state BEFORE touching the window.
    // SDL_SetWindowSize fires SDL_EVENT_WINDOW_RESIZED synchronously via our
    // event watcher, which calls handleResize(). If m_prevWinX/m_prevWinW are
    // stale, handleResize() falsely detects a left-edge drag and absorbs the
    // sidebar width delta into m_sidebarWidth — shrinking the plugin area.
    m_prevWinX = newWinX;
    m_prevWinW = newWinW;

    // Guard so handleResize() skips left-edge detection entirely during toggle
    m_inProgrammaticResize = true;

    SDL_SetWindowPosition(m_window, newWinX, winY);
    SDL_SetWindowSize(m_window, newWinW, winH);

    m_inProgrammaticResize = false;

    // Trigger resize callback so plugin host updates the child HWND X-offset
    // Plugin width stays the same: (winW + delta) - newEffective == winW - oldEffective
    handleResize(static_cast<uint32_t>(newWinW), static_cast<uint32_t>(winH));

    spdlog::info("Sidebar toggled: collapsed={}, effective={}->{}px, window={}->{}px",
                 collapse, oldEffective, newEffective, winW, newWinW);
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
        // Return plugin area, not total
        if (m_sidebarEnabled) {
            uint32_t effSidebar = getSidebarWidth();
            if (width > effSidebar) {
                width -= effSidebar;
            }
            uint32_t effToolbar = getToolbarHeight();
            if (height > effToolbar) {
                height -= effToolbar;
            }
        }
    }
}

void SdlWindow::setPresets(const rps::v1::PresetList& presets) {
    m_presets = presets;
    m_selectedPresetIndex = -1;
    std::memset(m_presetFilter, 0, sizeof(m_presetFilter));
    spdlog::info("SdlWindow: {} presets loaded into sidebar", m_presets.presets_size());
}

void SdlWindow::setPresetSelectedCallback(PresetSelectedCallback cb) {
    m_presetSelectedCb = std::move(cb);
}

void SdlWindow::setToolbarCallbacks(ToolbarCallbacks cb) {
    m_toolbarCallbacks = std::move(cb);
}








} // namespace rps::gui
