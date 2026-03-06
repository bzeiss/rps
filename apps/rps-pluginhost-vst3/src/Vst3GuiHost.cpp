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
    if (!newSize) return;

    uint32_t w = static_cast<uint32_t>(newSize->right - newSize->left);
    uint32_t h = static_cast<uint32_t>(newSize->bottom - newSize->top);
    spdlog::info("onPluginRequestResize: {}x{}", w, h);

    if (!m_canResize) {
        m_window.setMinimumSize(w, h);
        m_window.setMaximumSize(w, h);
    }

    // JUCE pattern: set recursiveResize to suppress the user-drag handler
    // during programmatic resize.
    m_inPluginResize = true;
    m_window.resize(w, h);
    m_inPluginResize = false;

    // Per VST3 spec: host must call onSize() after resizeView().
    // Guard against plugin calling resizeView() again from within onSize().
    if (m_view && !m_inOnSize) {
        m_inOnSize = true;
        m_view->onSize(newSize);
        m_inOnSize = false;
    }
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

    auto initResult = m_component->initialize(&s_hostApp);
    if (initResult != kResultOk) {
        spdlog::error("  IComponent::initialize() returned {} for {}", initResult, m_pluginName);
        throw std::runtime_error("IComponent::initialize() failed (result=" +
                                 std::to_string(initResult) + ") for " + m_pluginName);
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
                auto ctrlInitResult = m_controller->initialize(&s_hostApp);
                spdlog::info("  IEditController::initialize() = {}", ctrlInitResult);
                if (ctrlInitResult < 0) {
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

    // 5c. Connect component and controller via IConnectionPoint.
    //     Direct connection is needed during initialization — JUCE plugins
    //     require messages like 'JuceVST3EditController' to flow during
    //     createView(). After view creation, we switch to ConnectionStub
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
    // Use SEH to catch access violations in buggy plugins.
    // Only catch fatal exceptions — NOT C++ exceptions (0xE06D7363) which
    // should propagate normally through try/catch.
    __try {
#endif
        m_view = owned(m_controller->createView(ViewType::kEditor));
#ifdef _WIN32
    } __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
               GetExceptionCode() == EXCEPTION_STACK_OVERFLOW
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        DWORD code = GetExceptionCode();
        spdlog::error("  createView() caused SEH exception: 0x{:08X}", code);
        spdlog::default_logger()->flush();
        char hexBuf[32];
        snprintf(hexBuf, sizeof(hexBuf), "0x%08lX", code);
        throw std::runtime_error(std::string("createView() crashed with SEH exception ") +
                                 hexBuf + " for " + m_pluginName);
    }
#endif

    if (!m_view) {
        throw std::runtime_error("IEditController::createView(kEditor) returned nullptr for " + m_pluginName);
    }
    spdlog::info("  Editor view created");

    // 6b. Switch to ConnectionStub for the event loop.
    //     Direct connection during the event loop can cause async recursive
    //     message bouncing. The stub satisfies the connection contract while
    //     safely dropping messages.
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

#ifdef _WIN32
    // Apply WS_CLIPCHILDREN to prevent drawing over plugin's child HWND during resize.
    // Without this, Windows BitBlts old content from the parent over the plugin area
    // before the plugin repaints, causing visual artifacts.
    {
        HWND hwnd = static_cast<HWND>(m_window.getNativeHandle());
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        SetWindowLong(hwnd, GWL_STYLE, style | WS_CLIPCHILDREN);
    }
#endif

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

    void* nativeHandle = m_window.getNativeHandle();

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
    std::function<void(std::vector<rps::ipc::ParameterValueUpdate>)> paramChangeCb) {
    spdlog::info("Vst3GuiHost::runEventLoop() starting");

    auto resizeHandler = [this](uint32_t newWidth, uint32_t newHeight) {
        if (!m_canResize || !m_view) return;

        // Skip during plugin-initiated resizes (resizeView) —
        // onPluginRequestResize handles onSize() itself.
        if (m_inPluginResize) return;

        spdlog::debug("Window resized to {}x{}, syncing with VST3 view...", newWidth, newHeight);

        // Tell the plugin its new size. The window is already at this size.
        // Do NOT call checkSizeConstraint + m_window.resize() here:
        // SDL3's WM_WINDOWPOSCHANGING handler blocks programmatic resizes
        // during the modal drag loop (adds SWP_NOSIZE), so m_window.resize()
        // would silently fail, but we'd pass the wrong size to onSize().
        ViewRect rect{0, 0, static_cast<int32>(newWidth), static_cast<int32>(newHeight)};
        m_view->onSize(&rect);
    };
    m_window.setResizeCallback(resizeHandler);

    // Parameter polling at ~20Hz (matches CLAP host)
    auto lastParamPoll = std::chrono::steady_clock::now();
    constexpr auto kParamPollInterval = std::chrono::milliseconds(50);

    // Event loop — blocks until window is closed
    while (m_window.pollEvents()) {
        // Parameter polling
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
    }

    spdlog::info("Vst3GuiHost::runEventLoop() ended");

    if (closedCb) {
        closedCb("user");
    }
}

void Vst3GuiHost::requestClose() {
    m_window.requestClose();
}

// --- VST3 String128 to UTF-8 helper ---
namespace {
std::string vst3String128ToUtf8(const Steinberg::Vst::String128& str128) {
    // String128 is char16_t[128] — convert to UTF-8
    std::string result;
    for (int i = 0; i < 128 && str128[i] != 0; ++i) {
        char16_t ch = str128[i];
        if (ch < 0x80) {
            result += static_cast<char>(ch);
        } else if (ch < 0x800) {
            result += static_cast<char>(0xC0 | (ch >> 6));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (ch >> 12));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        }
    }
    return result;
}
} // anonymous namespace

std::vector<rps::ipc::PluginParameterInfo> Vst3GuiHost::getParameters() {
    std::vector<rps::ipc::PluginParameterInfo> result;
    if (!m_controller) {
        spdlog::info("getParameters: no IEditController available");
        return result;
    }

    int32 count = m_controller->getParameterCount();
    spdlog::info("getParameters: plugin has {} parameters", count);
    result.reserve(count);
    m_cachedParams.clear();
    m_cachedParams.reserve(count);

    for (int32 i = 0; i < count; ++i) {
        Steinberg::Vst::ParameterInfo info{};
        if (m_controller->getParameterInfo(i, info) != kResultTrue) {
            spdlog::warn("  getParameterInfo({}) failed, skipping", i);
            continue;
        }

        rps::ipc::PluginParameterInfo p;
        p.id = std::to_string(info.id);
        p.index = static_cast<uint32_t>(i);
        p.name = vst3String128ToUtf8(info.title);
        p.module = vst3String128ToUtf8(info.units);

        // VST3 values are normalized [0,1] — convert to plain scale
        double normValue = m_controller->getParamNormalized(info.id);
        double plainValue = m_controller->normalizedParamToPlain(info.id, normValue);
        p.currentValue = plainValue;

        // Min/max: convert 0.0 and 1.0 from normalized to plain
        p.minValue = m_controller->normalizedParamToPlain(info.id, 0.0);
        p.maxValue = m_controller->normalizedParamToPlain(info.id, 1.0);
        p.defaultValue = m_controller->normalizedParamToPlain(info.id, info.defaultNormalizedValue);

        // Get display text
        Steinberg::Vst::String128 displayStr{};
        if (m_controller->getParamStringByValue(info.id, normValue, displayStr) == kResultTrue) {
            p.displayText = vst3String128ToUtf8(displayStr);
        }

        // Map VST3 flags to universal flags
        uint32_t flags = 0;
        if (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly)
            flags |= rps::ipc::kParamFlagReadOnly;
        if (info.flags & Steinberg::Vst::ParameterInfo::kIsHidden)
            flags |= rps::ipc::kParamFlagHidden;
        if (info.flags & Steinberg::Vst::ParameterInfo::kIsBypass)
            flags |= rps::ipc::kParamFlagBypass;
        if (info.flags & Steinberg::Vst::ParameterInfo::kIsList) {
            flags |= rps::ipc::kParamFlagEnum;
            flags |= rps::ipc::kParamFlagStepped;
        }
        if (info.stepCount > 0)
            flags |= rps::ipc::kParamFlagStepped;
        p.flags = flags;

        result.push_back(p);

        // Cache for polling
        m_cachedParams.push_back({p.id, info.id, plainValue});
    }

    return result;
}

std::vector<rps::ipc::ParameterValueUpdate> Vst3GuiHost::pollParameterChanges() {
    std::vector<rps::ipc::ParameterValueUpdate> updates;
    if (!m_controller) {
        return updates;
    }

    for (size_t i = 0; i < m_cachedParams.size(); ++i) {
        auto& cached = m_cachedParams[i];
        double normValue = m_controller->getParamNormalized(cached.paramId);
        double plainValue = m_controller->normalizedParamToPlain(cached.paramId, normValue);

        // Compare with cached value (epsilon for floating-point noise)
        if (std::abs(plainValue - cached.lastValue) > 1e-9) {
            rps::ipc::ParameterValueUpdate u;
            u.paramId = cached.id;
            u.value = plainValue;

            // Get updated display text
            Steinberg::Vst::String128 displayStr{};
            if (m_controller->getParamStringByValue(cached.paramId, normValue, displayStr) == kResultTrue) {
                u.displayText = vst3String128ToUtf8(displayStr);
            }

            cached.lastValue = plainValue;
            updates.push_back(std::move(u));
        }
    }

    return updates;
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
