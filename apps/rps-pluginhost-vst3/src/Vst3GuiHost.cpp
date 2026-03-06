#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "Vst3GuiHost.hpp"

// Suppress warnings from VST3 SDK headers
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunnecessary-virtual-specifier"
#endif

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/common/memorystream.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace rps::scanner {

// ---------------------------------------------------------------------------
// IPlugFrame implementation — handles plugin-initiated resizes
// ---------------------------------------------------------------------------
class Vst3PlugFrame : public U::ImplementsNonDestroyable<U::Directly<IPlugFrame>> {
public:
    explicit Vst3PlugFrame(Vst3GuiHost& host) : m_host(host) {}

    tresult PLUGIN_API resizeView(IPlugView* /*view*/, ViewRect* newSize) override {
        if (!newSize) return kInvalidArgument;

        spdlog::info("IPlugFrame::resizeView({}x{})",
                     newSize->right - newSize->left,
                     newSize->bottom - newSize->top);
        m_host.onPluginRequestResize(newSize);
        return kResultTrue;
    }

private:
    Vst3GuiHost& m_host;
};

// ---------------------------------------------------------------------------
// IComponentHandler stub — satisfies controllers that require one
// ---------------------------------------------------------------------------
class Vst3ComponentHandler : public U::ImplementsNonDestroyable<U::Directly<IComponentHandler>> {
public:
    tresult PLUGIN_API beginEdit(ParamID id) override {
        spdlog::debug("beginEdit({})", id);
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        spdlog::debug("performEdit({}, {})", id, value);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID id) override {
        spdlog::debug("endEdit({})", id);
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        spdlog::debug("restartComponent({})", flags);
        return kResultOk;
    }
};

// ---------------------------------------------------------------------------
// ConnectionStub — accepts IConnectionPoint registration but drops messages.
// JUCE-based plugins require IConnectionPoint to be connected before
// createView() works, but forwarding messages causes async recursion
// through JUCE's internal message queue. The stub satisfies the connection
// requirement without causing recursion.
// ---------------------------------------------------------------------------
class ConnectionStub : public U::ImplementsNonDestroyable<U::Directly<IConnectionPoint>> {
public:
    tresult PLUGIN_API connect(IConnectionPoint* /*other*/) override {
        return kResultTrue;
    }
    tresult PLUGIN_API disconnect(IConnectionPoint* /*other*/) override {
        return kResultTrue;
    }
    tresult PLUGIN_API notify(IMessage* message) override {
        if (message) {
            spdlog::debug("ConnectionStub::notify(id='{}')", message->getMessageID() ? message->getMessageID() : "null");
        }
        return kResultOk;
    }
};

static ConnectionStub s_connectionStub;

// Static instances — lifetime tied to the process
static Vst3ComponentHandler s_componentHandler;
static Steinberg::Vst::HostApplication s_hostApp;

// ---------------------------------------------------------------------------
// Vst3GuiHost implementation
// ---------------------------------------------------------------------------

Vst3GuiHost::Vst3GuiHost() = default;

Vst3GuiHost::~Vst3GuiHost() {
    cleanup();
}

void Vst3GuiHost::cleanup() {
    spdlog::debug("Vst3GuiHost::cleanup()");

    if (m_view) {
        spdlog::debug("  removing view");
        m_view->setFrame(nullptr);
        m_view->removed();
        m_view = nullptr;
    }

    // Disconnect IConnectionPoint stubs
    {
        FUnknownPtr<IConnectionPoint> componentCP(m_component);
        FUnknownPtr<IConnectionPoint> controllerCP(m_controller);
        if (componentCP) componentCP->disconnect(&s_connectionStub);
        if (controllerCP) controllerCP->disconnect(&s_connectionStub);
    }

#ifdef _WIN32
    if (m_pluginHwnd) {
        DestroyWindow(m_pluginHwnd);
        m_pluginHwnd = nullptr;
    }
#endif

    // Deactivate before terminating
    if (m_component) {
        m_component->setActive(false);
    }

    if (m_controller) {
        spdlog::debug("  releasing controller");
        m_controller->terminate();
        m_controller = nullptr;
    }

    if (m_component) {
        spdlog::debug("  releasing component");
        m_component->terminate();
        m_component = nullptr;
    }

    if (m_module) {
        spdlog::debug("  releasing module");
        m_module.reset();
    }
}

void Vst3GuiHost::onPluginRequestResize(ViewRect* newSize) {
    if (!newSize || m_inResize) return;
    m_inResize = true;

    uint32_t w = static_cast<uint32_t>(newSize->right - newSize->left);
    uint32_t h = static_cast<uint32_t>(newSize->bottom - newSize->top);
    spdlog::info("onPluginRequestResize: {}x{}", w, h);

    // Update window constraints to allow the new size
    // (canResize=false means the HOST shouldn't offer edge-drag resizing,
    //  but the plugin can still request resizes via IPlugFrame::resizeView)
    m_window.setMinimumSize(w, h);
    m_window.setMaximumSize(w, h);

    // Resize the window to fit the plugin's requested size
    m_window.resize(w, h);

#ifdef _WIN32
    // Resize the child HWND to match
    if (m_pluginHwnd) {
        SetWindowPos(m_pluginHwnd, nullptr, 0, 0, static_cast<int>(w), static_cast<int>(h),
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
#endif

    // Per VST3 spec: host must call onSize() after resizeView()
    if (m_view) {
        m_view->onSize(newSize);
    }

    m_inResize = false;
}

rps::gui::IPluginGuiHost::OpenResult Vst3GuiHost::open(const boost::filesystem::path& pluginPath) {
    spdlog::info("Vst3GuiHost::open({})", pluginPath.string());

    // 1. Load the VST3 module
    spdlog::info("  Step 1: Loading VST3 module...");
    std::string error;
    m_module = VST3::Hosting::Module::create(pluginPath.string(), error);
    if (!m_module) {
        throw std::runtime_error("Failed to load VST3 module: " + pluginPath.string() + " (" + error + ")");
    }
    spdlog::info("  Module loaded: {}", m_module->getName());

    // 2. Get factory and enumerate plugins
    spdlog::info("  Step 2: Enumerating plugin classes...");
    auto& factory = m_module->getFactory();
    auto classInfos = factory.classInfos();
    spdlog::info("  Factory has {} class(es)", classInfos.size());

    // 3. Find the first Audio Effect class
    const VST3::Hosting::ClassInfo* audioEffectInfo = nullptr;
    for (auto& ci : classInfos) {
        spdlog::info("    Class: '{}' category='{}' vendor='{}'",
                     ci.name(), ci.category(), ci.vendor());
        if (ci.category() == kVstAudioEffectClass && !audioEffectInfo) {
            audioEffectInfo = &ci;
        }
    }
    if (!audioEffectInfo) {
        throw std::runtime_error("No VST3 Audio Effect class found in " + pluginPath.string());
    }
    m_pluginName = audioEffectInfo->name();
    spdlog::info("  Using plugin: '{}'", m_pluginName);

    // 4. Create IComponent
    spdlog::info("  Step 3: Creating IComponent...");
    m_component = factory.createInstance<IComponent>(audioEffectInfo->ID());
    if (!m_component) {
        throw std::runtime_error("Failed to create IComponent for " + m_pluginName);
    }

    if (m_component->initialize(&s_hostApp) != kResultOk) {
        throw std::runtime_error("IComponent::initialize() failed for " + m_pluginName);
    }
    spdlog::info("  IComponent created and initialized");

    // 5. Get IEditController — either from the component itself or separately
    spdlog::info("  Step 4: Getting IEditController...");
    auto qiResult = m_component->queryInterface(IEditController::iid, reinterpret_cast<void**>(&m_controller));
    spdlog::info("  queryInterface(IEditController) = {}", qiResult);

    if (qiResult != kResultTrue || !m_controller) {
        m_controller = nullptr;  // ensure clean state
        // Component and Controller are separate — get the controller CID
        TUID controllerTUID;
        memset(controllerTUID, 0, sizeof(TUID));
        auto gcResult = m_component->getControllerClassId(controllerTUID);
        FUID controllerCID = FUID::fromTUID(controllerTUID);
        spdlog::info("  getControllerClassId() = {}, CID valid = {}", gcResult, controllerCID.isValid());

        if (gcResult == kResultTrue && controllerCID.isValid()) {
            char cidStr[64] = {};
            controllerCID.toRegistryString(cidStr);
            spdlog::info("  Controller CID: {}", cidStr);
            VST3::UID controllerUID = VST3::UID::fromTUID(controllerCID.toTUID());
            m_controller = factory.createInstance<IEditController>(controllerUID);
            spdlog::info("  createInstance<IEditController> = {}", m_controller != nullptr);
            if (m_controller) {
                auto initResult = m_controller->initialize(&s_hostApp);
                spdlog::info("  IEditController::initialize() = {}", initResult);
                if (initResult != kResultOk) {
                    spdlog::warn("  IEditController::initialize() failed (non-fatal for some plugins)");
                }
                spdlog::info("  Separate IEditController created");
            }
        } else {
            spdlog::warn("  getControllerClassId failed or returned invalid CID");
        }
    } else {
        spdlog::info("  IEditController obtained from IComponent (same object)");
    }

    if (!m_controller) {
        throw std::runtime_error("No IEditController available for " + m_pluginName);
    }

    // 5b. Set component handler so the controller can report parameter changes
    m_controller->setComponentHandler(&s_componentHandler);

    // 5c. Connect component and controller via IConnectionPoint
    //     JUCE plugins require messages to flow during createView().
    //     We connect them directly, then disconnect after view creation
    //     to prevent async recursive message bouncing during the event loop.
    FUnknownPtr<IConnectionPoint> componentCP(m_component);
    FUnknownPtr<IConnectionPoint> controllerCP(m_controller);
    if (componentCP && controllerCP) {
        componentCP->connect(controllerCP);
        controllerCP->connect(componentCP);
        spdlog::info("  Component and controller connected (direct)");
    }

    // 5d. Sync component state to controller
    //     Some plugins crash during createView() without this.
    {
        auto* stream = new MemoryStream();
        if (m_component->getState(stream) == kResultTrue) {
            stream->seek(0, IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(stream);
            spdlog::info("  Component state synced to controller ({} bytes)", stream->getSize());
        } else {
            spdlog::info("  getState() not supported (non-fatal)");
        }
        stream->release();
    }

    // 6. Create the editor view
    spdlog::info("  Step 5: Creating editor view...");
    spdlog::default_logger()->flush();

#ifdef _WIN32
    // Use SEH to catch access violations in buggy plugins
    __try {
#endif
        m_view = owned(m_controller->createView(ViewType::kEditor));
#ifdef _WIN32
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        spdlog::error("  createView() caused SEH exception: 0x{:08X}", code);
        spdlog::default_logger()->flush();
        throw std::runtime_error("createView() crashed with SEH exception 0x" +
                                 std::to_string(code) + " for " + m_pluginName);
    }
#endif

    if (!m_view) {
        throw std::runtime_error("IEditController::createView(kEditor) returned nullptr for " + m_pluginName);
    }
    spdlog::info("  Editor view created");

    // 6b. Disconnect the direct connection to prevent async recursion during event loop.
    //     Reconnect via stub so the connection state is valid but messages are dropped.
    if (componentCP && controllerCP) {
        componentCP->disconnect(controllerCP);
        controllerCP->disconnect(componentCP);
        componentCP->connect(&s_connectionStub);
        controllerCP->connect(&s_connectionStub);
        spdlog::info("  Switched to ConnectionStub (async-safe)");
    }

    // 7. Check platform type support
#ifdef _WIN32
    FIDString platformType = kPlatformTypeHWND;
#elif defined(__APPLE__)
    FIDString platformType = kPlatformTypeNSView;
#else
    FIDString platformType = kPlatformTypeX11EmbedWindowID;
#endif

    if (m_view->isPlatformTypeSupported(platformType) != kResultTrue) {
        throw std::runtime_error(std::string("Plugin does not support platform type: ") + platformType);
    }
    spdlog::info("  Platform type '{}' is supported", platformType);

    // 8. Get initial size
    spdlog::info("  Step 6: Getting initial size...");
    ViewRect rect{};
    if (m_view->getSize(&rect) != kResultTrue) {
        spdlog::warn("  getSize() failed, using defaults 800x600");
        rect = {0, 0, 800, 600};
    }
    uint32_t w = static_cast<uint32_t>(rect.right - rect.left);
    uint32_t h = static_cast<uint32_t>(rect.bottom - rect.top);
    spdlog::info("  Initial size: {}x{}", w, h);

    // 9. Check resize support
    m_canResize = (m_view->canResize() == kResultTrue);
    spdlog::info("  canResize = {}", m_canResize);

    // 10. Create SDL3 window
    spdlog::info("  Step 7: Creating SDL3 window...");
    m_window.create(m_pluginName, w, h, m_canResize, false /* no presets in phase 1 */);
    spdlog::info("  SDL3 window created");

    // Set size constraints
    if (m_canResize) {
        ViewRect minRect{0, 0, 1, 1};
        if (m_view->checkSizeConstraint(&minRect) == kResultTrue) {
            uint32_t minW = static_cast<uint32_t>(minRect.right - minRect.left);
            uint32_t minH = static_cast<uint32_t>(minRect.bottom - minRect.top);
            m_window.setMinimumSize(minW, minH);
            spdlog::info("  Minimum size: {}x{}", minW, minH);
        } else {
            m_window.setMinimumSize(w, h);
        }
    } else {
        m_window.setMinimumSize(w, h);
        m_window.setMaximumSize(w, h);
    }

    // 11. Set IPlugFrame and attach view to the window
    spdlog::info("  Step 8: Attaching view to window...");
    static Vst3PlugFrame plugFrame(*this);
    m_view->setFrame(&plugFrame);

#ifdef _WIN32
    // Create a dedicated child HWND for the plugin's view.
    // JUCE's VST3 wrapper subclasses its parent HWND's WndProc — if we pass
    // SDL's HWND directly, JUCE's hooks conflict with SDL's internal message
    // processing, causing re-entrant DispatchMessage and stack overflow.
    {
        static bool classRegistered = false;
        static const wchar_t* className = L"RPS_Vst3PluginChild";
        if (!classRegistered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.style = CS_DBLCLKS;
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
            wc.lpszClassName = className;
            RegisterClassExW(&wc);
            classRegistered = true;
        }

        HWND parentHwnd = static_cast<HWND>(m_window.getNativeHandle());
        m_pluginHwnd = CreateWindowExW(
            0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, static_cast<int>(w), static_cast<int>(h),
            parentHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        if (!m_pluginHwnd) {
            throw std::runtime_error("Failed to create child HWND for plugin view");
        }
        spdlog::info("  Created child HWND {:p} (parent {:p})",
                     static_cast<void*>(m_pluginHwnd), static_cast<void*>(parentHwnd));

        // Disable IMM (Input Method Manager) for the plugin window.
        // Stack trace shows JUCE's WndProc and IMM32.dll enter infinite
        // recursion on WM_IME_* messages: JUCE handles IMM msg → triggers
        // another IMM msg → JUCE handles again → stack overflow.
        ImmAssociateContext(m_pluginHwnd, nullptr);
    }
    void* nativeHandle = static_cast<void*>(m_pluginHwnd);
#else
    void* nativeHandle = m_window.getNativeHandle();
#endif

    if (m_view->attached(nativeHandle, platformType) != kResultTrue) {
        throw std::runtime_error("IPlugView::attached() failed for " + m_pluginName);
    }
    spdlog::info("  View attached to native window at {:p}", nativeHandle);

    // 12. Re-query size after attachment — plugins may call resizeView() during
    //     attached(), changing their size (e.g. Roland XV-5080: 400x100 → 1877x247).
    //     Use the post-attach size for onSize() and the return value.
    ViewRect postAttachRect{};
    if (m_view->getSize(&postAttachRect) == kResultTrue) {
        uint32_t postW = static_cast<uint32_t>(postAttachRect.right - postAttachRect.left);
        uint32_t postH = static_cast<uint32_t>(postAttachRect.bottom - postAttachRect.top);
        if (postW != w || postH != h) {
            spdlog::info("  Size changed after attach: {}x{} -> {}x{}", w, h, postW, postH);
            w = postW;
            h = postH;
            rect = postAttachRect;
        }
    }
    if (m_view->onSize(&rect) != kResultTrue) {
        spdlog::warn("  onSize() returned error (non-fatal)");
    }

    // 13. Initial child HWND positioning for sidebar
    m_window.repositionChildHwnd(w, h);

    // 14. Activate the audio processor (AFTER view attachment)
    //     Some plugins (e.g. UAD) tie their GUI state to the processing state.
    //     Done after view creation to avoid interfering with createView().
    {
        m_processor = m_component;
        if (m_processor) {
            ProcessSetup setup{};
            setup.processMode = kRealtime;
            setup.symbolicSampleSize = kSample32;
            setup.maxSamplesPerBlock = 512;
            setup.sampleRate = 44100.0;
            m_processor->setupProcessing(setup);
        }
        m_component->setActive(true);
        spdlog::info("  Audio processor activated");
    }

    return OpenResult{m_pluginName, w, h};
}

void Vst3GuiHost::runEventLoop(
    std::function<void(const std::string& reason)> closedCb,
    std::function<void(std::vector<rps::ipc::ParameterValueUpdate>)> /*paramChangeCb*/) {
    spdlog::info("Vst3GuiHost::runEventLoop() starting");

    auto resizeHandler = [this](uint32_t newWidth, uint32_t newHeight) {
        if (!m_canResize || !m_view || m_inResize) return;
        m_inResize = true;

        spdlog::debug("Window resized to {}x{}, syncing with VST3 view...", newWidth, newHeight);

        ViewRect rect{0, 0, static_cast<int32>(newWidth), static_cast<int32>(newHeight)};

        // Let the plugin adjust to valid dimensions
        m_view->checkSizeConstraint(&rect);
        uint32_t adjustedW = static_cast<uint32_t>(rect.right - rect.left);
        uint32_t adjustedH = static_cast<uint32_t>(rect.bottom - rect.top);

        // Tell the plugin its new size
        m_view->onSize(&rect);

#ifdef _WIN32
        // Resize the child HWND to match
        if (m_pluginHwnd) {
            SetWindowPos(m_pluginHwnd, nullptr, 0, 0,
                         static_cast<int>(adjustedW), static_cast<int>(adjustedH),
                         SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        }
#endif

        // If the plugin adjusted the size, resize the window to match
        if (adjustedW != newWidth || adjustedH != newHeight) {
            spdlog::debug("  checkSizeConstraint: {}x{} -> {}x{}", newWidth, newHeight, adjustedW, adjustedH);
            m_window.resize(adjustedW, adjustedH);
        }

        m_inResize = false;

        // Child HWND repositioning is handled automatically by SdlWindow::handleResize()
    };
    m_window.setResizeCallback(resizeHandler);

    // Event loop — blocks until window is closed
    while (m_window.pollEvents()) {
        // Phase 2 will add parameter polling here
    }

    spdlog::info("Vst3GuiHost::runEventLoop() ended");

    if (closedCb) {
        closedCb("user");
    }
}

void Vst3GuiHost::requestClose() {
    m_window.requestClose();
}

// --- Phase 2+ stubs ---

std::vector<rps::ipc::PluginParameterInfo> Vst3GuiHost::getParameters() {
    return {};  // Phase 2
}

std::vector<rps::ipc::ParameterValueUpdate> Vst3GuiHost::pollParameterChanges() {
    return {};  // Phase 2
}

rps::ipc::GetStateResponse Vst3GuiHost::saveState() {
    return rps::ipc::GetStateResponse{{}, false, "Not implemented"};  // Phase 2
}

rps::ipc::SetStateResponse Vst3GuiHost::loadState(const std::vector<uint8_t>& /*stateData*/) {
    return rps::ipc::SetStateResponse{false, "Not implemented"};  // Phase 2
}

std::vector<rps::ipc::PresetInfo> Vst3GuiHost::getPresets() {
    return {};  // Phase 3
}

rps::ipc::LoadPresetResponse Vst3GuiHost::loadPreset(const std::string& /*presetId*/) {
    return rps::ipc::LoadPresetResponse{false, "Not implemented"};  // Phase 3
}

} // namespace rps::scanner
