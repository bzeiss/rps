#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "ClapGuiHost.hpp"

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>

namespace rps::scanner {

// ---------------------------------------------------------------------------
// CLAP Host Callbacks — implements the host side for GUI hosting
// ---------------------------------------------------------------------------
namespace {

// Forward declaration — we retrieve ClapGuiHost* from clap_host::host_data
ClapGuiHost* getHostFromClap(const clap_host* host) {
    return static_cast<ClapGuiHost*>(host->host_data);
}

// Host gui extension callbacks
void hostGuiResizeHintsChanged(const clap_host*) {
    spdlog::debug("hostGuiResizeHintsChanged called");
}

bool hostGuiRequestResize(const clap_host* host, uint32_t width, uint32_t height) {
    spdlog::info("hostGuiRequestResize: {}x{}", width, height);
    auto* self = getHostFromClap(host);
    if (self) {
        self->onPluginRequestResize(width, height);
    }
    return true;
}

bool hostGuiRequestShow(const clap_host*) {
    spdlog::debug("hostGuiRequestShow called");
    return true;
}

bool hostGuiRequestHide(const clap_host*) {
    spdlog::debug("hostGuiRequestHide called");
    return true;
}

void hostGuiClosed(const clap_host*, bool was_destroyed) {
    spdlog::info("hostGuiClosed called (was_destroyed={})", was_destroyed);
}

static clap_host_gui_t s_hostGui = {
    .resize_hints_changed = hostGuiResizeHintsChanged,
    .request_resize = hostGuiRequestResize,
    .request_show = hostGuiRequestShow,
    .request_hide = hostGuiRequestHide,
    .closed = hostGuiClosed,
};

// Core host callbacks
const void* hostGetExtension(const clap_host* /*host*/, const char* extensionId) {
    spdlog::debug("hostGetExtension: {}", extensionId);
    if (strcmp(extensionId, CLAP_EXT_GUI) == 0) {
        return &s_hostGui;
    }
    return nullptr;
}

void hostRequestRestart(const clap_host*) {
    spdlog::debug("hostRequestRestart called");
}
void hostRequestProcess(const clap_host*) {
    spdlog::debug("hostRequestProcess called");
}
void hostRequestCallback(const clap_host*) {
    spdlog::debug("hostRequestCallback called");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ClapGuiHost implementation
// ---------------------------------------------------------------------------

ClapGuiHost::ClapGuiHost() = default;

ClapGuiHost::~ClapGuiHost() {
    cleanup();
}

void ClapGuiHost::cleanup() {
    spdlog::debug("ClapGuiHost::cleanup()");
    if (m_gui && m_plugin && m_guiCreated) {
        spdlog::debug("  destroying GUI");
        m_gui->destroy(m_plugin);
        m_guiCreated = false;
    }

    if (m_plugin) {
        spdlog::debug("  deactivating and destroying plugin");
        m_plugin->deactivate(m_plugin);
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
    }

    if (m_entry) {
        spdlog::debug("  deinit entry");
        m_entry->deinit();
        m_entry = nullptr;
    }

    if (m_libHandle) {
        spdlog::debug("  unloading library");
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(m_libHandle));
#else
        dlclose(m_libHandle);
#endif
        m_libHandle = nullptr;
    }
}

void ClapGuiHost::onPluginRequestResize(uint32_t width, uint32_t height) {
    spdlog::info("onPluginRequestResize: resizing SDL window to {}x{}", width, height);
    m_window.resize(width, height);
}

rps::gui::IPluginGuiHost::OpenResult ClapGuiHost::open(const boost::filesystem::path& pluginPath) {
    spdlog::info("ClapGuiHost::open({})", pluginPath.string());

    // 1. Load the CLAP DLL
    spdlog::info("  Step 1: Loading CLAP DLL...");
#ifdef _WIN32
    HMODULE hLib = LoadLibraryW(pluginPath.c_str());
    if (!hLib) {
        DWORD err = GetLastError();
        throw std::runtime_error("Failed to load CLAP DLL: " + pluginPath.string()
                                 + " (Win32 error: " + std::to_string(err) + ")");
    }
    m_libHandle = static_cast<void*>(hLib);
    spdlog::info("  DLL loaded at {:p}", m_libHandle);

    auto* procAddr = reinterpret_cast<void*>(GetProcAddress(hLib, "clap_entry"));
    if (!procAddr) {
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }
    m_entry = reinterpret_cast<const clap_plugin_entry*>(procAddr);
    spdlog::info("  clap_entry found at {:p}", static_cast<const void*>(m_entry));
#else
    void* handle = dlopen(pluginPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load CLAP library: ") + dlerror());
    }
    m_libHandle = handle;
    spdlog::info("  DLL loaded at {:p}", m_libHandle);

    void* symAddr = dlsym(handle, "clap_entry");
    if (!symAddr) {
        throw std::runtime_error("Library does not export 'clap_entry'. Not a valid CLAP plugin.");
    }
    m_entry = reinterpret_cast<const clap_plugin_entry*>(symAddr);
    spdlog::info("  clap_entry found at {:p}", static_cast<const void*>(m_entry));
#endif

    // 2. Init entry and get factory
    spdlog::info("  Step 2: Checking CLAP version and initializing entry...");
    spdlog::info("  Plugin CLAP version: {}.{}.{}", m_entry->clap_version.major,
                 m_entry->clap_version.minor, m_entry->clap_version.revision);

    if (!clap_version_is_compatible(m_entry->clap_version)) {
        throw std::runtime_error("Incompatible CLAP version.");
    }

    spdlog::info("  Calling entry->init()...");
    if (!m_entry->init(pluginPath.string().c_str())) {
        throw std::runtime_error("Failed to initialize CLAP plugin entry.");
    }
    spdlog::info("  entry->init() succeeded");

    spdlog::info("  Step 3: Getting plugin factory...");
    const auto* factoryPtr = m_entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factoryPtr) {
        throw std::runtime_error("CLAP library does not provide a plugin factory.");
    }

    auto* factory = static_cast<const clap_plugin_factory*>(factoryPtr);
    uint32_t numPlugins = factory->get_plugin_count(factory);
    spdlog::info("  Factory has {} plugin(s)", numPlugins);
    if (numPlugins == 0) {
        throw std::runtime_error("CLAP library contains no plugins.");
    }

    // 3. Get the first plugin descriptor
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    if (!desc) {
        throw std::runtime_error("Failed to get CLAP plugin descriptor.");
    }

    m_pluginName = desc->name ? desc->name : "Unknown Plugin";
    spdlog::info("  Plugin: '{}' (id: {}, vendor: {})", m_pluginName,
                 desc->id ? desc->id : "?", desc->vendor ? desc->vendor : "?");

    // 4. Create the host
    spdlog::info("  Step 4: Creating CLAP host...");
    static clap_host_t s_host{};
    s_host.clap_version = CLAP_VERSION;
    s_host.name = "rps-pluginhost-clap";
    s_host.vendor = "rps";
    s_host.url = "";
    s_host.version = "1.0.0";
    s_host.get_extension = hostGetExtension;
    s_host.request_restart = hostRequestRestart;
    s_host.request_process = hostRequestProcess;
    s_host.request_callback = hostRequestCallback;
    s_host.host_data = this;

    // 5. Create and init the plugin
    spdlog::info("  Step 5: Creating plugin instance...");
    m_plugin = factory->create_plugin(factory, &s_host, desc->id);
    if (!m_plugin) {
        throw std::runtime_error("Failed to create CLAP plugin instance.");
    }
    spdlog::info("  Plugin instance created at {:p}", static_cast<const void*>(m_plugin));

    spdlog::info("  Calling plugin->init()...");
    if (!m_plugin->init(m_plugin)) {
        throw std::runtime_error("CLAP plugin init() failed.");
    }
    spdlog::info("  plugin->init() succeeded");

    // 5b. Query params extension (optional — used for parameter streaming)
    m_params = static_cast<const clap_plugin_params*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS));
    if (m_params) {
        spdlog::info("  Params extension found (count={} get_info={} get_value={} value_to_text={})",
                     m_params->count != nullptr, m_params->get_info != nullptr,
                     m_params->get_value != nullptr, m_params->value_to_text != nullptr);
    } else {
        spdlog::info("  No params extension — parameter streaming disabled");
    }

    // 5c. Query state extension (optional — used for state save/restore)
    m_state = static_cast<const clap_plugin_state*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_STATE));
    if (m_state) {
        spdlog::info("  State extension found (save={} load={})",
                     m_state->save != nullptr, m_state->load != nullptr);
    } else {
        spdlog::info("  No state extension — state save/restore disabled");
    }

    // 6. Query GUI extension
    spdlog::info("  Step 6: Querying CLAP_EXT_GUI...");
    m_gui = static_cast<const clap_plugin_gui*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
    if (!m_gui) {
        throw std::runtime_error("Plugin does not support CLAP GUI extension.");
    }
    spdlog::info("  GUI extension found at {:p}", static_cast<const void*>(m_gui));

    // Log which function pointers are available
    spdlog::info("  GUI ext: is_api_supported={}, create={}, destroy={}, set_scale={}, "
                 "get_size={}, can_resize={}, get_resize_hints={}, adjust_size={}, "
                 "set_size={}, set_parent={}, show={}, hide={}",
                 m_gui->is_api_supported != nullptr, m_gui->create != nullptr,
                 m_gui->destroy != nullptr, m_gui->set_scale != nullptr,
                 m_gui->get_size != nullptr, m_gui->can_resize != nullptr,
                 m_gui->get_resize_hints != nullptr, m_gui->adjust_size != nullptr,
                 m_gui->set_size != nullptr, m_gui->set_parent != nullptr,
                 m_gui->show != nullptr, m_gui->hide != nullptr);

    // 7. Check API support and create GUI
#ifdef _WIN32
    const char* windowApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    const char* windowApi = CLAP_WINDOW_API_COCOA;
#else
    const char* windowApi = CLAP_WINDOW_API_X11;
#endif

    spdlog::info("  Step 7: Checking API support for '{}'...", windowApi);
    if (!m_gui->is_api_supported(m_plugin, windowApi, false)) {
        throw std::runtime_error(std::string("Plugin does not support embedded ") + windowApi + " GUI.");
    }
    spdlog::info("  API '{}' is supported", windowApi);

    spdlog::info("  Calling gui->create()...");
    if (!m_gui->create(m_plugin, windowApi, false)) {
        throw std::runtime_error("clap_plugin_gui->create() failed.");
    }
    m_guiCreated = true;
    spdlog::info("  GUI created successfully");

    // 8. Query whether the plugin supports resizing
    spdlog::info("  Step 8: Querying can_resize...");
    m_canResize = m_gui->can_resize && m_gui->can_resize(m_plugin);
    spdlog::info("  can_resize = {}", m_canResize);

    // 9. Get initial size
    spdlog::info("  Step 9: Getting initial size...");
    uint32_t w = 800, h = 600;
    if (!m_gui->get_size(m_plugin, &w, &h)) {
        spdlog::warn("  get_size failed, using defaults 800x600");
        w = 800;
        h = 600;
    }
    spdlog::info("  Initial size: {}x{}", w, h);

    // 10. Create SDL3 window
    spdlog::info("  Step 10: Creating SDL3 window (resizable={})...", m_canResize);
    m_window.create(m_pluginName, w, h, m_canResize);
    spdlog::info("  SDL3 window created");

    if (m_canResize && m_gui->adjust_size) {
        // Discover the plugin's minimum size by requesting a very small size.
        // adjust_size will snap to the nearest valid dimensions.
        uint32_t minW = 1, minH = 1;
        if (m_gui->adjust_size(m_plugin, &minW, &minH)) {
            spdlog::info("  Plugin minimum size: {}x{}", minW, minH);
            m_window.setMinimumSize(minW, minH);
        } else {
            m_window.setMinimumSize(w, h);
        }
    } else if (!m_canResize) {
        // Non-resizable: lock both min and max to the initial size
        m_window.setMinimumSize(w, h);
        m_window.setMaximumSize(w, h);
    }

    // 11. Set parent (embed plugin GUI into SDL window)
    spdlog::info("  Step 11: Setting parent window...");
    void* nativeHandle = m_window.getNativeHandle();
    spdlog::info("  Native handle: {:p}", nativeHandle);

    clap_window_t clapWindow{};
    clapWindow.api = windowApi;
#ifdef _WIN32
    clapWindow.win32 = nativeHandle;
#elif defined(__APPLE__)
    clapWindow.cocoa = nativeHandle;
#else
    clapWindow.x11 = reinterpret_cast<unsigned long>(nativeHandle);
#endif

    if (!m_gui->set_parent(m_plugin, &clapWindow)) {
        throw std::runtime_error("clap_plugin_gui->set_parent() failed.");
    }
    spdlog::info("  set_parent() succeeded");

    // 11b. Explicitly set size — some plugins require this before show()
    spdlog::info("  Step 11b: Calling set_size({}x{})...", w, h);
    if (m_gui->set_size) {
        if (!m_gui->set_size(m_plugin, w, h)) {
            spdlog::warn("  set_size() returned false (non-fatal)");
        } else {
            spdlog::info("  set_size() succeeded");
        }
    }

    // 12. Show
    spdlog::info("  Step 12: Showing GUI...");
    if (!m_gui->show(m_plugin)) {
        // Some plugins return false from show() but still render correctly.
        // Log a warning but don't treat as fatal.
        spdlog::warn("  show() returned false — plugin may still render (continuing)");
    } else {
        spdlog::info("  show() succeeded — plugin GUI is now visible");
    }

    return OpenResult{m_pluginName, w, h};
}

void ClapGuiHost::runEventLoop(
    std::function<void(const std::string& reason)> closedCb,
    std::function<void(std::vector<rps::ipc::ParameterValueUpdate>)> paramChangeCb) {
    spdlog::info("ClapGuiHost::runEventLoop() starting");

    auto resizeHandler = [this](uint32_t newWidth, uint32_t newHeight) {
        if (!m_canResize || !m_gui || !m_plugin) return;

        spdlog::debug("Window resized to {}x{}, syncing with plugin...", newWidth, newHeight);

        uint32_t adjustedW = newWidth;
        uint32_t adjustedH = newHeight;

        if (m_gui->adjust_size) {
            m_gui->adjust_size(m_plugin, &adjustedW, &adjustedH);
            spdlog::debug("  adjust_size: {}x{} -> {}x{}", newWidth, newHeight, adjustedW, adjustedH);
        }

        m_gui->set_size(m_plugin, adjustedW, adjustedH);

        if (adjustedW != newWidth || adjustedH != newHeight) {
            spdlog::debug("  SDL window resize correction to {}x{}", adjustedW, adjustedH);
            m_window.resize(adjustedW, adjustedH);
        }
    };
    // Register resize handler via event watcher for live resize during drag
    m_window.setResizeCallback(resizeHandler);

    auto lastParamPoll = std::chrono::steady_clock::now();
    constexpr auto kParamPollInterval = std::chrono::milliseconds(50);

    while (m_window.pollEvents()) {
        // Parameter polling at ~20Hz
        if (paramChangeCb) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastParamPoll >= kParamPollInterval) {
                auto changes = pollParameterChanges();
                if (!changes.empty()) {
                    paramChangeCb(std::move(changes));
                }
                lastParamPoll = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    spdlog::info("ClapGuiHost::runEventLoop() ended");

    if (closedCb) {
        closedCb("user");
    }
}

void ClapGuiHost::requestClose() {
    spdlog::info("ClapGuiHost::requestClose()");
    m_window.requestClose();
}

std::vector<rps::ipc::PluginParameterInfo> ClapGuiHost::getParameters() {
    std::vector<rps::ipc::PluginParameterInfo> result;
    if (!m_params || !m_params->count || !m_params->get_info) {
        spdlog::info("getParameters: params extension not available");
        return result;
    }

    uint32_t count = m_params->count(m_plugin);
    spdlog::info("getParameters: plugin has {} parameters", count);
    result.reserve(count);
    m_cachedParams.clear();
    m_cachedParams.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!m_params->get_info(m_plugin, i, &info)) {
            spdlog::warn("  get_info({}) failed, skipping", i);
            continue;
        }

        rps::ipc::PluginParameterInfo p;
        p.id = std::to_string(info.id);
        p.index = i;
        p.name = info.name;
        p.module = info.module;
        p.minValue = info.min_value;
        p.maxValue = info.max_value;
        p.defaultValue = info.default_value;

        // Get current value
        double val = info.default_value;
        if (m_params->get_value) {
            m_params->get_value(m_plugin, info.id, &val);
        }
        p.currentValue = val;

        // Get display text
        if (m_params->value_to_text) {
            char buf[256] = {};
            if (m_params->value_to_text(m_plugin, info.id, val, buf, sizeof(buf))) {
                p.displayText = buf;
            }
        }

        // Map CLAP flags to universal flags
        uint32_t flags = 0;
        if (info.flags & CLAP_PARAM_IS_STEPPED)  flags |= rps::ipc::kParamFlagStepped;
        if (info.flags & CLAP_PARAM_IS_HIDDEN)   flags |= rps::ipc::kParamFlagHidden;
        if (info.flags & CLAP_PARAM_IS_READONLY) flags |= rps::ipc::kParamFlagReadOnly;
        if (info.flags & CLAP_PARAM_IS_BYPASS)   flags |= rps::ipc::kParamFlagBypass;
        if (info.flags & CLAP_PARAM_IS_ENUM)     flags |= rps::ipc::kParamFlagEnum;
        p.flags = flags;

        result.push_back(p);

        // Cache for polling
        m_cachedParams.push_back({p.id, val});
    }

    return result;
}

std::vector<rps::ipc::ParameterValueUpdate> ClapGuiHost::pollParameterChanges() {
    std::vector<rps::ipc::ParameterValueUpdate> updates;
    if (!m_params || !m_params->count || !m_params->get_info || !m_params->get_value) {
        return updates;
    }

    uint32_t count = m_params->count(m_plugin);

    // If the parameter count changed (e.g. preset load), reinitialize
    if (count != static_cast<uint32_t>(m_cachedParams.size())) {
        spdlog::info("pollParameterChanges: param count changed ({} -> {}), full rescan needed",
                     m_cachedParams.size(), count);
        m_cachedParams.clear();
        return updates; // The caller should call getParameters() again
    }

    for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!m_params->get_info(m_plugin, i, &info)) {
            continue;
        }

        double val = 0.0;
        if (!m_params->get_value(m_plugin, info.id, &val)) {
            continue;
        }

        // Compare with cached value (epsilon for floating-point noise)
        if (i < m_cachedParams.size() && std::abs(val - m_cachedParams[i].lastValue) > 1e-9) {
            rps::ipc::ParameterValueUpdate u;
            u.paramId = m_cachedParams[i].id;
            u.value = val;

            // Get updated display text
            if (m_params->value_to_text) {
                char buf[256] = {};
                if (m_params->value_to_text(m_plugin, info.id, val, buf, sizeof(buf))) {
                    u.displayText = buf;
                }
            }

            m_cachedParams[i].lastValue = val;
            updates.push_back(std::move(u));
        }
    }

    return updates;
}

rps::ipc::GetStateResponse ClapGuiHost::saveState() {
    rps::ipc::GetStateResponse resp;

    if (!m_state || !m_state->save) {
        resp.success = false;
        resp.error = "Plugin does not support state extension";
        spdlog::warn("saveState: state extension not available");
        return resp;
    }

    // Memory-backed output stream
    std::vector<uint8_t> buffer;
    clap_ostream_t ostream{};
    ostream.ctx = &buffer;
    ostream.write = [](const clap_ostream_t* stream, const void* data, uint64_t size) -> int64_t {
        auto* buf = static_cast<std::vector<uint8_t>*>(stream->ctx);
        auto* bytes = static_cast<const uint8_t*>(data);
        buf->insert(buf->end(), bytes, bytes + size);
        return static_cast<int64_t>(size);
    };

    spdlog::info("saveState: saving plugin state...");
    if (!m_state->save(m_plugin, &ostream)) {
        resp.success = false;
        resp.error = "Plugin save() returned false";
        spdlog::error("saveState: plugin save() failed");
        return resp;
    }

    resp.stateData = std::move(buffer);
    resp.success = true;
    spdlog::info("saveState: saved {} bytes", resp.stateData.size());
    return resp;
}

rps::ipc::SetStateResponse ClapGuiHost::loadState(const std::vector<uint8_t>& stateData) {
    rps::ipc::SetStateResponse resp;

    if (!m_state || !m_state->load) {
        resp.success = false;
        resp.error = "Plugin does not support state extension";
        spdlog::warn("loadState: state extension not available");
        return resp;
    }

    // Memory-backed input stream
    struct ReadCtx {
        const std::vector<uint8_t>* data;
        size_t pos = 0;
    };
    ReadCtx readCtx{&stateData, 0};

    clap_istream_t istream{};
    istream.ctx = &readCtx;
    istream.read = [](const clap_istream_t* stream, void* buffer, uint64_t size) -> int64_t {
        auto* ctx = static_cast<ReadCtx*>(stream->ctx);
        size_t remaining = ctx->data->size() - ctx->pos;
        size_t toRead = std::min(static_cast<size_t>(size), remaining);
        if (toRead == 0) return 0; // EOF
        std::memcpy(buffer, ctx->data->data() + ctx->pos, toRead);
        ctx->pos += toRead;
        return static_cast<int64_t>(toRead);
    };

    spdlog::info("loadState: loading {} bytes...", stateData.size());
    if (!m_state->load(m_plugin, &istream)) {
        resp.success = false;
        resp.error = "Plugin load() returned false";
        spdlog::error("loadState: plugin load() failed");
        return resp;
    }

    // Clear cached params so next poll re-queries everything
    m_cachedParams.clear();

    resp.success = true;
    spdlog::info("loadState: state restored successfully");
    return resp;
}

} // namespace rps::scanner
