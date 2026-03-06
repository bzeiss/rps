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
}

void SdlWindow::create(const std::string& title, uint32_t width, uint32_t height,
                        bool resizable, bool enableSidebar) {
    m_sidebarEnabled = enableSidebar;

    // If sidebar is enabled, make the window wider to accommodate it
    uint32_t totalWidth = width;
    if (m_sidebarEnabled) {
        totalWidth += getSidebarWidth();
    }

    Uint32 flags = 0;
    if (resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    m_window = SDL_CreateWindow(
        title.c_str(),
        static_cast<int>(totalWidth),
        static_cast<int>(height),
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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Docking features
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
    ImGui_ImplSDLRenderer3_Init(m_renderer);

    m_imguiInitialized = true;
    spdlog::info("Dear ImGui initialized for sidebar");
}

void SdlWindow::shutdownImGui() {
    if (!m_imguiInitialized) return;

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_imguiInitialized = false;
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
        uint32_t totalWidth = width;
        if (m_sidebarEnabled) {
            totalWidth += getSidebarWidth();
        }
        SDL_SetWindowSize(m_window, static_cast<int>(totalWidth), static_cast<int>(height));
    }
}

void SdlWindow::setMinimumSize(uint32_t width, uint32_t height) {
    if (m_window) {
        uint32_t totalWidth = width;
        if (m_sidebarEnabled) {
            totalWidth += getSidebarWidth();
        }
        SDL_SetWindowMinimumSize(m_window, static_cast<int>(totalWidth), static_cast<int>(height));
    }
}

void SdlWindow::setMaximumSize(uint32_t width, uint32_t height) {
    if (m_window) {
        uint32_t totalWidth = width;
        if (m_sidebarEnabled) {
            totalWidth += getSidebarWidth();
        }
        SDL_SetWindowMaximumSize(m_window, static_cast<int>(totalWidth), static_cast<int>(height));
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
            // Feed events to ImGui if toolbar active
            if (m_imguiInitialized) {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    return false;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    return false;
                default:
                    break;
            }
        } while (SDL_PollEvent(&event));
    }

    // Render sidebar if enabled
    if (m_imguiInitialized && m_renderer) {
        renderSidebar();
    }

    return true;
}

void SdlWindow::renderSidebar() {
    // Start ImGui frame
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Get window size
    int winW = 0, winH = 0;
    SDL_GetWindowSize(m_window, &winW, &winH);

    // Current effective sidebar width
    uint32_t effectiveWidth = getSidebarWidth();

    // --- Splitter: only active when sidebar is expanded ---
    if (!m_sidebarCollapsed) {
        ImGuiIO& io = ImGui::GetIO();
        float splitterX = static_cast<float>(effectiveWidth);
        const float splitterHalfW = 4.0f;

        bool hovered = (io.MousePos.x >= splitterX - splitterHalfW &&
                        io.MousePos.x <= splitterX + splitterHalfW &&
                        io.MousePos.y >= 0.0f &&
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
        // ===== COLLAPSED: narrow vertical strip with expand button =====
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(kCollapsedStripWidth), static_cast<float>(winH)));
        ImGui::Begin("##SidebarStrip", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Center the expand button vertically
        float buttonSize = 30.0f;
        float yCenter = static_cast<float>(winH) * 0.5f - buttonSize * 0.5f;
        ImGui::SetCursorPosY(yCenter);
        ImGui::SetCursorPosX((static_cast<float>(kCollapsedStripWidth) - buttonSize) * 0.5f);

        if (ImGui::Button("<<", ImVec2(buttonSize, buttonSize))) {
            toggleSidebar(false); // expand
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Show Presets");
        }

        ImGui::End();
    } else {
        // ===== EXPANDED: full preset browser sidebar =====
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(m_sidebarWidth), static_cast<float>(winH)));
        ImGui::Begin("##Sidebar", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header row: "PRESETS" left, ">>" collapse button right-aligned
        ImGui::TextDisabled("PRESETS");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        if (ImGui::Button(">>", ImVec2(24, 0))) {
            toggleSidebar(true); // collapse
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hide Presets");
        }
        ImGui::Separator();
        ImGui::Spacing();

        // Search bar: stretch to full sidebar content width
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##Filter", "Search...", m_presetFilter, sizeof(m_presetFilter));
        ImGui::Spacing();

        ImGui::BeginChild("PresetListScroll", ImVec2(0, 0), true);

        if (m_presets.empty()) {
            ImGui::TextDisabled("No presets found.");
        } else {
            std::string filterLower = m_presetFilter;
            std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

            // Build filtered index list
            std::vector<int> filteredIndices;
            filteredIndices.reserve(m_presets.size());

            for (int i = 0; i < static_cast<int>(m_presets.size()); ++i) {
                const auto& preset = m_presets[static_cast<size_t>(i)];

                if (!filterLower.empty()) {
                    std::string nameLower = preset.name;
                    std::string catLower = preset.category;
                    std::string authorLower = preset.creator;
                    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                    std::transform(catLower.begin(), catLower.end(), catLower.begin(), ::tolower);
                    std::transform(authorLower.begin(), authorLower.end(), authorLower.begin(), ::tolower);

                    if (nameLower.find(filterLower) == std::string::npos &&
                        catLower.find(filterLower) == std::string::npos &&
                        authorLower.find(filterLower) == std::string::npos) {
                        continue;
                    }
                }
                filteredIndices.push_back(i);
            }

            if (ImGui::BeginTable("PresetsTable", 3,
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY)) {

                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                // Sort by column specs
                if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                    if (sortSpecs->SpecsDirty || true) {
                        const auto& presets = m_presets;
                        std::stable_sort(filteredIndices.begin(), filteredIndices.end(),
                            [&presets, sortSpecs](int lhs, int rhs) {
                                for (int n = 0; n < sortSpecs->SpecsCount; ++n) {
                                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[n];
                                    int cmp = 0;
                                    switch (spec.ColumnIndex) {
                                        case 0:
                                            cmp = presets[static_cast<size_t>(lhs)].name.compare(
                                                  presets[static_cast<size_t>(rhs)].name);
                                            break;
                                        case 1:
                                            cmp = presets[static_cast<size_t>(lhs)].category.compare(
                                                  presets[static_cast<size_t>(rhs)].category);
                                            break;
                                        case 2:
                                            cmp = presets[static_cast<size_t>(lhs)].creator.compare(
                                                  presets[static_cast<size_t>(rhs)].creator);
                                            break;
                                    }
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
                    const auto& preset = m_presets[static_cast<size_t>(idx)];

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();

                    const bool isSelected = (m_selectedPresetIndex == idx);

                    ImGui::PushID(idx);
                    if (ImGui::Selectable(preset.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                        m_selectedPresetIndex = idx;
                        if (m_presetSelectedCb) {
                            m_presetSelectedCb(preset.id);
                        }
                    }
                    ImGui::PopID();

                    if (isSelected && ImGui::IsWindowAppearing()) {
                        ImGui::SetScrollHereY();
                    }

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(preset.category.c_str());

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(preset.creator.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
        ImGui::End();
    }

    // Render ImGui
    ImGui::Render();

    // Draw the sidebar background
    SDL_FRect sidebarFillRect{0.0f, 0.0f, static_cast<float>(getSidebarWidth()), static_cast<float>(winH)};
    SDL_SetRenderDrawColor(m_renderer, 30, 30, 36, 255);
    SDL_RenderFillRect(m_renderer, &sidebarFillRect);

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
    if (m_sidebarEnabled && !m_sidebarCollapsed && m_prevWinW > 0) {
        int xDelta = curX - m_prevWinX; // negative = dragged left (window grew)
        int wDelta = curW - m_prevWinW; // positive = window grew

        // Left-edge drag: X moves and width changes by the opposite amount
        // (within a small tolerance for rounding)
        if (xDelta != 0 && wDelta != 0 && std::abs(xDelta + wDelta) <= 2) {
            leftEdgeDragged = true;

            // Absorb width delta into sidebar width
            int newSidebarW = static_cast<int>(m_sidebarWidth) + wDelta;
            if (newSidebarW < static_cast<int>(kCollapsedStripWidth)) {
                newSidebarW = static_cast<int>(kCollapsedStripWidth);
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

    if (leftEdgeDragged) {
        // Left-edge drag: do NOT resize the plugin, only sidebar absorbed the change.
        // But we still need to update the child HWND X-offset.
        if (m_resizeCb) {
            uint32_t effSidebar = getSidebarWidth();
            uint32_t pluginWidth = (width > effSidebar) ? width - effSidebar : 0;
            m_resizeCb(pluginWidth, height);
        }
    } else {
        // Normal resize (right/top/bottom edge or programmatic)
        if (m_resizeCb) {
            uint32_t effSidebar = getSidebarWidth();
            uint32_t pluginWidth = width;
            if (m_sidebarEnabled && width > effSidebar) {
                pluginWidth -= effSidebar;
            }
            m_resizeCb(pluginWidth, height);
        }
    }
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

    SDL_SetWindowPosition(m_window, newWinX, winY);
    SDL_SetWindowSize(m_window, newWinW, winH);

    // Update tracking state BEFORE handleResize so it doesn't falsely detect left-edge drag
    m_prevWinX = newWinX;
    m_prevWinW = newWinW;

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
        uint32_t effSidebar = getSidebarWidth();
        if (m_sidebarEnabled && width > effSidebar) {
            width -= effSidebar;
        }
    }
}

void SdlWindow::setPresets(const std::vector<rps::ipc::PresetInfo>& presets) {
    m_presets = presets;
    m_selectedPresetIndex = -1;
    std::memset(m_presetFilter, 0, sizeof(m_presetFilter));
    spdlog::info("SdlWindow: {} presets loaded into sidebar", m_presets.size());
}

void SdlWindow::setPresetSelectedCallback(PresetSelectedCallback cb) {
    m_presetSelectedCb = std::move(cb);
}

} // namespace rps::gui
