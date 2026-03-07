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
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstpresetkeys.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/common/memorystream.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <filesystem>
#include <fstream>
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
    m_pluginVendor = audioEffectInfo->vendor();
    {
        auto uid = audioEffectInfo->ID();
        m_componentFUID = Steinberg::FUID::fromTUID(uid.data());
    }
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
    m_window.create(m_pluginName, w, h, m_canResize, true /* enable sidebar */);
    spdlog::info("  SDL3 window created (sidebar enabled)");

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
    m_window.repositionChildHwnd(w, h);

    // 14. Populate sidebar with presets
    {
        auto presets = getPresets();
        if (!presets.empty()) {
            m_window.setPresets(presets);
            spdlog::info("  Sidebar: {} presets loaded", presets.size());
        }

        // Wire preset selection callback
        m_window.setPresetSelectedCallback([this](const std::string& presetId) {
            spdlog::info("Sidebar: preset selected: {}", presetId);
            auto resp = loadPreset(presetId);
            if (resp.success) {
                spdlog::info("Sidebar: preset loaded successfully");
                m_cachedParams.clear();  // Force full re-poll
            } else {
                spdlog::warn("Sidebar: preset load failed: {}", resp.error);
            }
        });
    }

    // 15. Activate the audio processor (AFTER view attachment)
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
    rps::ipc::GetStateResponse resp;

    if (!m_component) {
        resp.success = false;
        resp.error = "No IComponent available";
        return resp;
    }

    // 1. Save processor (IComponent) state
    auto* procStream = new MemoryStream();
    auto procResult = m_component->getState(procStream);
    if (procResult != kResultTrue) {
        procStream->release();
        resp.success = false;
        resp.error = "IComponent::getState() failed (result=" + std::to_string(procResult) + ")";
        spdlog::error("saveState: {}", resp.error);
        return resp;
    }
    int64 procSize = procStream->getSize();
    spdlog::info("saveState: processor state = {} bytes", procSize);

    // 2. Save controller (IEditController) state
    int64 ctrlSize = 0;
    MemoryStream* ctrlStream = nullptr;
    if (m_controller) {
        ctrlStream = new MemoryStream();
        if (m_controller->getState(ctrlStream) == kResultTrue) {
            ctrlSize = ctrlStream->getSize();
            spdlog::info("saveState: controller state = {} bytes", ctrlSize);
        } else {
            spdlog::info("saveState: controller getState() not supported (non-fatal)");
            ctrlStream->release();
            ctrlStream = nullptr;
        }
    }

    // 3. Pack into blob: [4-byte procSize LE] [procData] [ctrlData]
    uint32_t procSizeU32 = static_cast<uint32_t>(procSize);
    resp.stateData.resize(4 + procSize + ctrlSize);
    std::memcpy(resp.stateData.data(), &procSizeU32, 4);

    // Copy processor state
    procStream->seek(0, IBStream::kIBSeekSet, nullptr);
    procStream->read(resp.stateData.data() + 4, static_cast<int32>(procSize), nullptr);
    procStream->release();

    // Copy controller state
    if (ctrlStream && ctrlSize > 0) {
        ctrlStream->seek(0, IBStream::kIBSeekSet, nullptr);
        ctrlStream->read(resp.stateData.data() + 4 + procSize, static_cast<int32>(ctrlSize), nullptr);
        ctrlStream->release();
    }

    resp.success = true;
    spdlog::info("saveState: total {} bytes", resp.stateData.size());
    return resp;
}

rps::ipc::SetStateResponse Vst3GuiHost::loadState(const std::vector<uint8_t>& stateData) {
    rps::ipc::SetStateResponse resp;

    if (!m_component) {
        resp.success = false;
        resp.error = "No IComponent available";
        return resp;
    }

    if (stateData.size() < 4) {
        resp.success = false;
        resp.error = "State data too small";
        return resp;
    }

    // Unpack: [4-byte procSize LE] [procData] [ctrlData]
    uint32_t procSize = 0;
    std::memcpy(&procSize, stateData.data(), 4);

    if (4 + procSize > stateData.size()) {
        resp.success = false;
        resp.error = "Invalid state data: processor size exceeds total";
        return resp;
    }

    size_t ctrlSize = stateData.size() - 4 - procSize;
    spdlog::info("loadState: processor={} bytes, controller={} bytes", procSize, ctrlSize);

    // 1. Restore processor state
    {
        auto* stream = new MemoryStream(
            const_cast<void*>(static_cast<const void*>(stateData.data() + 4)),
            static_cast<int32>(procSize));
        auto result = m_component->setState(stream);
        if (result != kResultTrue) {
            stream->release();
            resp.success = false;
            resp.error = "IComponent::setState() failed (result=" + std::to_string(result) + ")";
            spdlog::error("loadState: {}", resp.error);
            return resp;
        }

        // 2. Sync controller with processor state
        if (m_controller) {
            stream->seek(0, IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(stream);
        }
        stream->release();
    }

    // 3. Restore controller's own state
    if (m_controller && ctrlSize > 0) {
        auto* stream = new MemoryStream(
            const_cast<void*>(static_cast<const void*>(stateData.data() + 4 + procSize)),
            static_cast<int32>(ctrlSize));
        m_controller->setState(stream);
        stream->release();
    }

    // Clear cached params so next poll re-queries everything
    m_cachedParams.clear();

    resp.success = true;
    spdlog::info("loadState: state restored successfully");
    return resp;
}

std::vector<rps::ipc::PresetInfo> Vst3GuiHost::getPresets() {
    m_presets.clear();
    uint32_t globalIndex = 0;

    // --- Source 1: IUnitInfo program lists ---
    if (m_controller) {
        FUnknownPtr<IUnitInfo> unitInfo(m_controller);
        if (unitInfo) {
            int32 listCount = unitInfo->getProgramListCount();
            spdlog::info("getPresets: {} program list(s)", listCount);

            for (int32 li = 0; li < listCount; ++li) {
                ProgramListInfo listInfo{};
                if (unitInfo->getProgramListInfo(li, listInfo) != kResultTrue) continue;

                std::string listName = vst3String128ToUtf8(listInfo.name);
                spdlog::info("  List '{}' (id={}, count={})", listName, listInfo.id, listInfo.programCount);

                for (int32 pi = 0; pi < listInfo.programCount; ++pi) {
                    String128 programName{};
                    if (unitInfo->getProgramName(listInfo.id, pi, programName) != kResultTrue) continue;

                    rps::ipc::PresetInfo preset;
                    preset.id = std::to_string(listInfo.id) + ":" + std::to_string(pi);
                    preset.name = vst3String128ToUtf8(programName);
                    preset.index = globalIndex++;
                    preset.flags = rps::ipc::kPresetFlagFactory;

                    String128 categoryStr{};
                    if (unitInfo->getProgramInfo(listInfo.id, pi,
                            Steinberg::Vst::PresetAttributes::kInstrument, categoryStr) == kResultTrue) {
                        preset.category = vst3String128ToUtf8(categoryStr);
                    }
                    m_presets.push_back(std::move(preset));
                }
            }
        } else {
            spdlog::info("getPresets: plugin does not support IUnitInfo");
        }
    }

    // --- Source 2: .vstpreset files on disk ---
    // VST3 preset root directories (can contain <Vendor>/<PluginName>/ subdirs)
    std::vector<std::filesystem::path> presetRoots;
#ifdef _WIN32
    if (auto* appdata = std::getenv("APPDATA")) {
        presetRoots.push_back(std::filesystem::path(appdata) / "VST3 Presets");
    }
    if (auto* userprofile = std::getenv("USERPROFILE")) {
        presetRoots.push_back(std::filesystem::path(userprofile) / "Documents" / "VST3 Presets");
    }
    if (auto* common = std::getenv("COMMONPROGRAMFILES")) {
        presetRoots.push_back(std::filesystem::path(common) / "VST3 Presets");
    }
#elif defined(__APPLE__)
    if (auto* home = std::getenv("HOME")) {
        presetRoots.push_back(std::filesystem::path(home) / "Library" / "Audio" / "Presets");
    }
    presetRoots.push_back("/Library/Audio/Presets");
#else
    if (auto* home = std::getenv("HOME")) {
        presetRoots.push_back(std::filesystem::path(home) / ".vst3" / "presets");
    }
#endif

    // Collect preset directories: try reported vendor first, then scan all vendors
    std::vector<std::filesystem::path> presetDirs;
    for (const auto& root : presetRoots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;

        // Try the direct vendor/plugin path first
        auto directPath = root / m_pluginVendor / m_pluginName;
        if (std::filesystem::exists(directPath, ec)) {
            presetDirs.push_back(directPath);
        }

        // Also scan ALL vendor subdirectories for our plugin name
        // (handles vendor name mismatches, e.g. ClassInfo says "Brainworx"
        // but presets are under "Plugin Alliance")
        for (const auto& vendorEntry : std::filesystem::directory_iterator(root, ec)) {
            if (!vendorEntry.is_directory()) continue;
            auto pluginDir = vendorEntry.path() / m_pluginName;
            if (std::filesystem::exists(pluginDir, ec) && pluginDir != directPath) {
                presetDirs.push_back(pluginDir);
            }
        }
    }

    for (const auto& dir : presetDirs) {
        std::error_code ec;
        spdlog::info("getPresets: scanning {}", dir.string());

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            if (ext != ".vstpreset" && ext != ".VSTPRESET") continue;

            rps::ipc::PresetInfo preset;
            preset.id = "file:" + entry.path().string();
            preset.name = entry.path().stem().string();
            preset.index = globalIndex++;

            // Derive category from relative subdirectory path
            auto relPath = std::filesystem::relative(entry.path().parent_path(), dir, ec);
            if (!ec && relPath != ".") {
                preset.category = relPath.string();
            }

            preset.flags = rps::ipc::kPresetFlagFactory;
            m_presets.push_back(std::move(preset));
        }
    }

    spdlog::info("getPresets: {} total preset(s) found", m_presets.size());
    return m_presets;
}

rps::ipc::LoadPresetResponse Vst3GuiHost::loadPreset(const std::string& presetId) {
    rps::ipc::LoadPresetResponse resp;

    if (!m_controller) {
        resp.success = false;
        resp.error = "No IEditController available";
        return resp;
    }

    // --- File-based preset (from .vstpreset file) ---
    if (presetId.starts_with("file:")) {
        auto filePath = presetId.substr(5);
        spdlog::info("loadPreset: loading .vstpreset file: {}", filePath);

        // Read entire file into memory
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file) {
            resp.success = false;
            resp.error = "Failed to open preset file: " + filePath;
            return resp;
        }
        auto fileSize = file.tellg();
        file.seekg(0);
        std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
        file.close();

        // Parse .vstpreset format:
        // Header: 'VST3' (4) + version (4) + classID (32 ASCII hex) + chunkListOffset (8)
        // Total header = 48 bytes
        if (fileData.size() < 48) {
            resp.success = false;
            resp.error = "Preset file too small";
            return resp;
        }

        // Check magic
        if (memcmp(fileData.data(), "VST3", 4) != 0) {
            resp.success = false;
            resp.error = "Not a .vstpreset file (bad magic)";
            return resp;
        }

        // Read chunk list offset (int64 at offset 40)
        int64_t chunkListOffset = 0;
        memcpy(&chunkListOffset, fileData.data() + 40, 8);

        if (chunkListOffset < 48 || static_cast<size_t>(chunkListOffset) >= fileData.size()) {
            resp.success = false;
            resp.error = "Invalid chunk list offset";
            return resp;
        }

        // Parse chunk list: 'List' (4) + entryCount (4)
        auto* chunkList = fileData.data() + chunkListOffset;
        auto remaining = fileData.size() - static_cast<size_t>(chunkListOffset);
        if (remaining < 8 || memcmp(chunkList, "List", 4) != 0) {
            resp.success = false;
            resp.error = "Invalid chunk list header";
            return resp;
        }

        int32_t entryCount = 0;
        memcpy(&entryCount, chunkList + 4, 4);

        struct ChunkEntry {
            char id[4];
            int64_t offset;
            int64_t size;
        };

        // Each entry: id(4) + offset(8) + size(8) = 20 bytes
        if (remaining < 8 + static_cast<size_t>(entryCount) * 20) {
            resp.success = false;
            resp.error = "Chunk list truncated";
            return resp;
        }

        // Find 'Comp' and 'Cont' chunks
        int64_t compOffset = -1, compSize = 0;
        int64_t contOffset = -1, contSize = 0;

        for (int32_t i = 0; i < entryCount; ++i) {
            auto* entry = chunkList + 8 + i * 20;
            ChunkEntry ce{};
            memcpy(ce.id, entry, 4);
            memcpy(&ce.offset, entry + 4, 8);
            memcpy(&ce.size, entry + 12, 8);

            if (memcmp(ce.id, "Comp", 4) == 0) {
                compOffset = ce.offset;
                compSize = ce.size;
            } else if (memcmp(ce.id, "Cont", 4) == 0) {
                contOffset = ce.offset;
                contSize = ce.size;
            }
        }

        spdlog::info("  Comp chunk: offset={}, size={}", compOffset, compSize);
        spdlog::info("  Cont chunk: offset={}, size={}", contOffset, contSize);

        // Load component state
        if (compOffset >= 0 && compSize > 0) {
            auto* stream = new MemoryStream(
                fileData.data() + compOffset, static_cast<int32>(compSize));
            auto result = m_component->setState(stream);
            spdlog::info("  IComponent::setState: {}", result);

            // Sync controller with component state
            stream->seek(0, IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(stream);
            stream->release();
        }

        // Load controller state
        if (contOffset >= 0 && contSize > 0) {
            auto* stream = new MemoryStream(
                fileData.data() + contOffset, static_cast<int32>(contSize));
            m_controller->setState(stream);
            stream->release();
        }

        m_cachedParams.clear();
        resp.success = true;
        spdlog::info("loadPreset: .vstpreset loaded successfully");
        return resp;
    }

    // --- Program list preset (listId:programIndex) ---

    // Parse presetId: "listId:programIndex"
    auto colonPos = presetId.find(':');
    if (colonPos == std::string::npos) {
        resp.success = false;
        resp.error = "Invalid preset ID format: " + presetId;
        return resp;
    }
    auto listId = static_cast<ProgramListID>(std::stoi(presetId.substr(0, colonPos)));
    auto programIndex = std::stoi(presetId.substr(colonPos + 1));

    spdlog::info("loadPreset: listId={}, programIndex={}", listId, programIndex);

    // Find the total program count and the owning unit for this list
    int32 totalProgramCount = 0;
    UnitID owningUnitId = kRootUnitId;
    {
        FUnknownPtr<IUnitInfo> unitInfo(m_controller);
        if (unitInfo) {
            int32 listCount = unitInfo->getProgramListCount();
            for (int32 li = 0; li < listCount; ++li) {
                ProgramListInfo pli{};
                if (unitInfo->getProgramListInfo(li, pli) == kResultTrue && pli.id == listId) {
                    totalProgramCount = pli.programCount;
                    break;
                }
            }
            // Find the unit that owns this program list
            int32 unitCount = unitInfo->getUnitCount();
            for (int32 ui = 0; ui < unitCount; ++ui) {
                UnitInfo uinfo{};
                if (unitInfo->getUnitInfo(ui, uinfo) == kResultTrue && uinfo.programListId == listId) {
                    owningUnitId = uinfo.id;
                    spdlog::info("  Program list {} belongs to unit {}", listId, owningUnitId);
                    break;
                }
            }
        }
    }

    // For single-preset plugins (only "Default"), loading it is a no-op success
    if (totalProgramCount <= 1 && programIndex == 0) {
        spdlog::info("loadPreset: single preset — already active");
        resp.success = true;
        return resp;
    }

    // Search all parameters, collecting candidates
    int32 paramCount = m_controller->getParameterCount();
    ParamID programChangeParamId = static_cast<ParamID>(-1);
    int32 programChangeStepCount = 0;

    // Strategy 1: Find a parameter with kIsProgramChange flag
    for (int32 i = 0; i < paramCount; ++i) {
        Steinberg::Vst::ParameterInfo info{};
        if (m_controller->getParameterInfo(i, info) != kResultTrue) continue;

        if (info.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange) {
            programChangeParamId = info.id;
            programChangeStepCount = info.stepCount;
            spdlog::info("  Strategy 1: kIsProgramChange param id={}, stepCount={}", info.id, info.stepCount);
            break;
        }
    }

    // Strategy 2: Find a stepped param in the owning unit with matching stepCount
    if (programChangeParamId == static_cast<ParamID>(-1) && totalProgramCount > 1) {
        for (int32 i = 0; i < paramCount; ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (m_controller->getParameterInfo(i, info) != kResultTrue) continue;

            if (info.unitId == owningUnitId &&
                info.stepCount == totalProgramCount - 1) {
                programChangeParamId = info.id;
                programChangeStepCount = info.stepCount;
                spdlog::info("  Strategy 2: unit-matched param id={}, stepCount={}, name='{}'",
                             info.id, info.stepCount, vst3String128ToUtf8(info.title));
                break;
            }
        }
    }

    // Strategy 3: Any stepped param with matching stepCount (regardless of unit)
    if (programChangeParamId == static_cast<ParamID>(-1) && totalProgramCount > 1) {
        for (int32 i = 0; i < paramCount; ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (m_controller->getParameterInfo(i, info) != kResultTrue) continue;

            if (info.stepCount == totalProgramCount - 1) {
                programChangeParamId = info.id;
                programChangeStepCount = info.stepCount;
                spdlog::info("  Strategy 3: stepCount-matched param id={}, name='{}'",
                             info.id, vst3String128ToUtf8(info.title));
                break;
            }
        }
    }

    if (programChangeParamId == static_cast<ParamID>(-1)) {
        // Log all stepped params for debugging
        spdlog::warn("loadPreset: no matching param found. Listing stepped params:");
        for (int32 i = 0; i < paramCount; ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (m_controller->getParameterInfo(i, info) != kResultTrue) continue;
            if (info.stepCount > 0) {
                spdlog::warn("  param id={} unitId={} stepCount={} flags=0x{:x} name='{}'",
                             info.id, info.unitId, info.stepCount, info.flags,
                             vst3String128ToUtf8(info.title));
            }
        }
        resp.success = false;
        resp.error = "No program change parameter found for this plugin";
        spdlog::warn("loadPreset: {}", resp.error);
        return resp;
    }

    // --- Apply the preset ---
    bool applied = false;

    // Strategy A: Use IProgramListData to load actual preset data (most reliable)
    // This gets the raw program data and loads it as state, which fully applies
    // the preset without needing an audio processing loop.
    {
        FUnknownPtr<IProgramListData> programListData(m_component);
        if (programListData) {
            if (programListData->programDataSupported(listId) == kResultTrue) {
                auto* stream = new MemoryStream();
                if (programListData->getProgramData(listId, programIndex, stream) == kResultTrue) {
                    int64 dataSize = stream->getSize();
                    spdlog::info("  IProgramListData::getProgramData: {} bytes", dataSize);

                    // Load into component (processor)
                    stream->seek(0, IBStream::kIBSeekSet, nullptr);
                    auto setResult = m_component->setState(stream);
                    spdlog::info("  IComponent::setState result: {}", setResult);

                    // Sync controller with the new processor state
                    stream->seek(0, IBStream::kIBSeekSet, nullptr);
                    m_controller->setComponentState(stream);

                    applied = true;
                } else {
                    spdlog::info("  IProgramListData::getProgramData failed for prog {}", programIndex);
                }
                stream->release();
            } else {
                spdlog::info("  IProgramListData: list {} not supported for program data", listId);
            }
        } else {
            spdlog::info("  Component does not implement IProgramListData");
        }
    }

    // Strategy B: Use IUnitInfo::setUnitProgramData if available
    if (!applied) {
        FUnknownPtr<IUnitInfo> unitInfo(m_controller);
        if (unitInfo) {
            auto* stream = new MemoryStream();
            // setUnitProgramData with listId and programIndex
            auto result = unitInfo->setUnitProgramData(listId, programIndex, stream);
            stream->release();
            if (result == kResultTrue) {
                spdlog::info("  IUnitInfo::setUnitProgramData succeeded");
                applied = true;
            } else {
                spdlog::info("  IUnitInfo::setUnitProgramData returned {}", result);
            }
        }
    }

    // Strategy C: Minimal audio process pass to deliver the parameter change
    // to the processor. Without audio processing, the processor never receives
    // the program change. We set up a minimal process, queue the parameter,
    // process one silent buffer, then save+sync the state.
    if (!applied && programChangeParamId != static_cast<ParamID>(-1) && m_processor) {
        ParamValue normValue = 0.0;
        if (programChangeStepCount > 0) {
            normValue = static_cast<ParamValue>(programIndex) /
                        static_cast<ParamValue>(programChangeStepCount);
        }

        spdlog::info("  Strategy C: minimal process pass, param {} = {}", programChangeParamId, normValue);

        // 1. Setup processing
        ProcessSetup setup{};
        setup.processMode = Steinberg::Vst::kOffline;
        setup.symbolicSampleSize = Steinberg::Vst::kSample32;
        setup.maxSamplesPerBlock = 32;
        setup.sampleRate = 44100.0;

        auto setupResult = m_processor->setupProcessing(setup);
        spdlog::info("    setupProcessing: {}", setupResult);

        if (setupResult == kResultOk || setupResult == kResultTrue) {
            // 2. Activate and start processing
            m_component->setActive(true);
            m_processor->setProcessing(true);

            // 3. Create parameter changes with the program change
            ParameterChanges inputChanges(1);
            int32 queueIndex = 0;
            auto* queue = inputChanges.addParameterData(programChangeParamId, queueIndex);
            if (queue) {
                int32 pointIndex = 0;
                queue->addPoint(0, normValue, pointIndex);
            }

            ParameterChanges outputChanges;

            // 4. Process one silent buffer (no audio needed)
            ProcessData data{};
            data.processMode = Steinberg::Vst::kOffline;
            data.symbolicSampleSize = Steinberg::Vst::kSample32;
            data.numSamples = 32;
            data.inputParameterChanges = &inputChanges;
            data.outputParameterChanges = &outputChanges;
            data.numInputs = 0;
            data.numOutputs = 0;
            data.inputs = nullptr;
            data.outputs = nullptr;

            auto processResult = m_processor->process(data);
            spdlog::info("    process: {}", processResult);

            // 5. Stop processing and deactivate
            m_processor->setProcessing(false);
            m_component->setActive(false);

            // 6. Save component state and sync to controller
            auto* stateStream = new MemoryStream();
            if (m_component->getState(stateStream) == kResultTrue) {
                stateStream->seek(0, IBStream::kIBSeekSet, nullptr);
                m_controller->setComponentState(stateStream);
                spdlog::info("    State synced to controller ({} bytes)", stateStream->getSize());
            }
            stateStream->release();

            applied = true;
        } else {
            spdlog::warn("    setupProcessing failed, falling back to setParamNormalized only");
            // Last resort: just set the parameter (won't fully work but better than nothing)
            m_controller->setParamNormalized(programChangeParamId, normValue);
            applied = true;
        }
    }

    if (!applied) {
        resp.success = false;
        resp.error = "No preset loading mechanism available for this plugin";
        return resp;
    }

    // Also update the program change parameter to reflect the selection
    if (programChangeParamId != static_cast<ParamID>(-1)) {
        ParamValue normValue = 0.0;
        if (programChangeStepCount > 0) {
            normValue = static_cast<ParamValue>(programIndex) /
                        static_cast<ParamValue>(programChangeStepCount);
        }
        m_controller->setParamNormalized(programChangeParamId, normValue);
    }

    // Clear cached params so next poll re-queries everything
    m_cachedParams.clear();

    resp.success = true;
    spdlog::info("loadPreset: program {} loaded", programIndex);
    return resp;
}

} // namespace rps::scanner
