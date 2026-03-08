#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#endif

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <memory>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <rps/ipc/Connection.hpp>
#include <rps/core/LoggingInit.hpp>
#include <spdlog/spdlog.h>
#include <scanner.pb.h>
#include <rps/scanner/IPluginFormatScanner.hpp>
#include <rps/scanner/ClapScanner.hpp>
#include <rps/scanner/Vst3Scanner.hpp>
#include <rps/scanner/AaxScanner.hpp>
#ifdef RPS_VST2_ENABLED
#include <rps/scanner/Vst2Scanner.hpp>
#endif
#ifdef __linux__
#include <rps/scanner/Lv2Scanner.hpp>
#include <rps/scanner/LadspaScanner.hpp>
#endif
#ifdef __APPLE__
#include <rps/scanner/AuScanner.hpp>
#endif
#include <rps/core/FormatTraits.hpp>

// g_verbose is referenced by individual scanner implementations (extern bool g_verbose).
// It's set based on the RPS_PLUGINSCANNER_LOGLEVEL environment variable.
bool g_verbose = false;

namespace rps::scanner {
std::vector<std::unique_ptr<IPluginFormatScanner>> ScannerFactory::createAllScanners() {
    std::vector<std::unique_ptr<IPluginFormatScanner>> scanners;
    scanners.push_back(std::make_unique<ClapScanner>());
    scanners.push_back(std::make_unique<Vst3Scanner>());
    scanners.push_back(std::make_unique<AaxScanner>());
#ifdef RPS_VST2_ENABLED
    scanners.push_back(std::make_unique<Vst2Scanner>());
#endif
#ifdef __linux__
    scanners.push_back(std::make_unique<Lv2Scanner>());
    scanners.push_back(std::make_unique<LadspaScanner>());
#endif
#ifdef __APPLE__
    scanners.push_back(std::make_unique<AuScanner>());
#endif
    return scanners;
}
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Suppress Windows error/crash dialog boxes from plugin DLLs.
    // This prevents pop-ups for access violations, missing DLLs, etc.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // Initialize COM — many VST3 plugins (e.g. Waves WaveShell) require COM
    // to be initialized before calling GetPluginFactory().
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif

    namespace po = boost::program_options;
    namespace fs = boost::filesystem;
    
    po::options_description desc("Scanner Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("ipc-id,i", po::value<std::string>(), "IPC connection handle ID")
        ("plugin-path,p", po::value<std::string>(), "Path to plugin to scan")
        ("worker-id,w", po::value<int>()->default_value(0), "Worker ID (for log file naming)");
        
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing command line: " << e.what() << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    if (!vm.count("ipc-id")) {
        std::cerr << "Error: --ipc-id is required.\n";
        return 1;
    }

    int workerId = vm["worker-id"].as<int>();

    // Initialize logging from RPS_PLUGINSCANNER_LOGGING / RPS_PLUGINSCANNER_LOGLEVEL
    std::string logFileName = "rps-pluginscanner.worker_" + std::to_string(workerId) + ".log";
    rps::core::initLogging("PLUGINSCANNER", logFileName);

    // Set g_verbose based on the effective spdlog level (used by scanner implementations)
    g_verbose = (spdlog::default_logger()->level() <= spdlog::level::debug);

    std::string ipcId = vm["ipc-id"].as<std::string>();
    std::string pluginPathStr = vm.count("plugin-path") ? vm["plugin-path"].as<std::string>() : "unknown";
    fs::path pluginPath(pluginPathStr);

    try {
        spdlog::info("{}: Connecting to IPC queue...", pluginPath.filename().string());
        // 1. Connect to Orchestrator IPC Queue
        auto connection = rps::ipc::MessageQueueConnection::createClient(ipcId);

        spdlog::info("{}: Waiting for ScanCommand...", pluginPath.filename().string());
        // 2. Wait for the ScanCommand (protobuf over MQ)
        rps::scanner::ScanCommand scanCmd;
        if (!connection->receiveProto(scanCmd, 5000)) {
            spdlog::error("Failed to receive ScanCommand from Orchestrator.");
            std::cerr << "Failed to receive ScanCommand from Orchestrator.\n";
            return 1;
        }

        std::string reqFormat = scanCmd.format();
        pluginPathStr = scanCmd.plugin_path();
        pluginPath = boost::filesystem::path(pluginPathStr);

        spdlog::info("{}: Finding {} scanner...", pluginPath.filename().string(), reqFormat);
        // 3. Find appropriate scanner
        auto scanners = rps::scanner::ScannerFactory::createAllScanners();
        rps::scanner::IPluginFormatScanner* activeScanner = nullptr;

        // Ensure we only try to parse the plugin with a scanner that natively handles its format.
        rps::core::FormatRegistry formatRegistry;

        for (auto& s : scanners) {
            auto formatName = s->getFormatName();
            if (formatName == reqFormat) {
                const auto* traits = formatRegistry.getTraits(formatName);
                if (traits && traits->isPluginPath(pluginPath)) {
                    if (s->canHandle(pluginPath)) {
                        activeScanner = s.get();
                        break;
                    }
                }
            }
        }

        if (!activeScanner && pluginPathStr != "CRASH_ME" && pluginPathStr != "HANG_ME") {
            spdlog::error("{}: No scanner handles format '{}'", pluginPath.filename().string(), reqFormat);
            rps::scanner::ScannerEvent evtMsg;
            auto* err = evtMsg.mutable_error();
            err->set_error("Unsupported Format");
            err->set_details("No scanner handles: " + pluginPathStr);
            connection->sendProto(evtMsg);
            return 1;
        }

        // 4. Progress Callback closure
        auto progressCb = [&connection](int percentage, const std::string& status) {
            rps::scanner::ScannerEvent evtMsg;
            auto* prog = evtMsg.mutable_progress();
            prog->set_status(status);
            prog->set_progress_percentage(percentage);
            connection->sendProto(evtMsg);
        };

        // --- CRASH TEST SIMULATION ---
        if (pluginPathStr == "CRASH_ME") {
            progressCb(10, "Simulating crash...");
            spdlog::error("Scanner triggering intentional crash!");
            std::cerr << "Scanner triggering intentional crash!\n";
            int* ptr = nullptr;
            *ptr = 42; // Segmentation fault
        }
        
        if (pluginPathStr == "HANG_ME") {
            progressCb(10, "Simulating hang...");
            spdlog::error("Scanner triggering intentional hang!");
            std::cerr << "Scanner triggering intentional hang!\n";
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        // -----------------------------

        // 5. Execute Scan
        spdlog::info("{}: Starting scan with {} scanner...",
                     pluginPath.filename().string(), activeScanner->getFormatName());
        if (activeScanner) {
            try {
                auto t0 = std::chrono::steady_clock::now();
                auto result = activeScanner->scan(pluginPath, progressCb);
                auto t1 = std::chrono::steady_clock::now();

                auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                spdlog::debug("{}: scan() returned in {}ms ({} params)",
                              pluginPath.filename().string(), scanMs, result.parameters.size());

                // Convert C++ struct → proto at the IPC boundary
                rps::scanner::ScannerEvent evtMsg;
                auto* pbRes = evtMsg.mutable_scan_result();
                pbRes->set_name(result.name);
                pbRes->set_vendor(result.vendor);
                pbRes->set_version(result.version);
                pbRes->set_uid(result.uid);
                pbRes->set_description(result.description);
                pbRes->set_url(result.url);
                pbRes->set_category(result.category);
                pbRes->set_format(result.format);
                pbRes->set_scan_method(result.scanMethod);
                pbRes->set_num_inputs(result.numInputs);
                pbRes->set_num_outputs(result.numOutputs);
                for (const auto& p : result.parameters) {
                    auto* pp = pbRes->add_parameters();
                    pp->set_id(p.id);
                    pp->set_name(p.name);
                    pp->set_default_value(p.defaultValue);
                }
                for (const auto& [k, v] : result.extraData) {
                    (*pbRes->mutable_extra_data())[k] = v;
                }

                auto t2 = std::chrono::steady_clock::now();
                bool sent = connection->sendProto(evtMsg);
                auto t3 = std::chrono::steady_clock::now();

                auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
                spdlog::debug("{}: IPC sendProto {} in {}ms",
                              pluginPath.filename().string(), sent ? "OK" : "FAILED", sendMs);
            } catch (const std::exception& scanErr) {
                std::string what = scanErr.what();
                spdlog::error("{}: Scan error: {}", pluginPath.filename().string(), what);
                std::cerr << "Scanner Fatal Error: " << what << "\n";
                rps::scanner::ScannerEvent evtMsg;
                auto* err = evtMsg.mutable_error();
                err->set_error("Scan Error");
                err->set_details(what);
                connection->sendProto(evtMsg);
            }
        }

        // Give IPC time to flush
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    } catch (const std::exception& e) {
        spdlog::error("Scanner fatal error: {}", e.what());
        std::cerr << "Scanner Fatal Error: " << e.what() << "\n";
        // Fall through to _exit below
    }

    // Terminate immediately. All IPC data has been sent; nothing left to clean up.
    // On Windows, _exit() still calls ExitProcess() which triggers DLL_PROCESS_DETACH
    // for every loaded plugin DLL -- many plugins hang there for 30-60+ seconds.
    // TerminateProcess bypasses DLL_PROCESS_DETACH entirely.
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#else
    _exit(0);
#endif
}
