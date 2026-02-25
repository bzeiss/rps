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
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <rps/ipc/Connection.hpp>
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
#include <rps/core/FormatTraits.hpp>

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
        ("verbose,v", "Enable verbose/debug output");
        
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

    g_verbose = vm.count("verbose") > 0;

    std::string ipcId = vm["ipc-id"].as<std::string>();
    std::string pluginPathStr = vm.count("plugin-path") ? vm["plugin-path"].as<std::string>() : "unknown";
    fs::path pluginPath(pluginPathStr);

    // Stage logging: flushed immediately so orchestrator captures output even on crash
    auto logStage = [&](const std::string& stage) {
        if (g_verbose) {
            std::cerr << "[scanner] " << pluginPath.filename().string() << ": " << stage << std::endl;
        }
    };

    try {
        logStage("Connecting to IPC queue...");
        // 1. Connect to Orchestrator IPC Queue
        auto connection = rps::ipc::MessageQueueConnection::createClient(ipcId);

        logStage("Waiting for ScanRequest...");
        // 2. Wait for the ScanRequest
        auto maybeMsg = connection->receiveMessage(5000); // 5 sec timeout
        if (!maybeMsg.has_value() || maybeMsg.value().type != rps::ipc::MessageType::ScanRequest) {
            std::cerr << "Failed to receive ScanRequest from Orchestrator.\n";
            return 1;
        }

        auto req = std::get<rps::ipc::ScanRequest>(maybeMsg.value().payload);

        logStage("Finding appropriate scanner...");
        // 3. Find appropriate scanner
        auto scanners = rps::scanner::ScannerFactory::createAllScanners();
        rps::scanner::IPluginFormatScanner* activeScanner = nullptr;

        // Ensure we only try to parse the plugin with a scanner that natively handles its format.
        // E.g. don't try to parse a .vst3 file with a ClapScanner.
        rps::core::FormatRegistry formatRegistry;

        for (auto& s : scanners) {
            auto formatName = s->getFormatName();
            if (formatName == req.format) {
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
            rps::ipc::Message errMsg;
            errMsg.type = rps::ipc::MessageType::ErrorMessage;
            errMsg.payload = rps::ipc::ErrorMessage{"Unsupported Format", "No scanner handles: " + pluginPathStr};
            connection->sendMessage(errMsg);
            return 1;
        }

        // 4. Progress Callback closure
        auto progressCb = [&connection](int percentage, const std::string& status) {
            rps::ipc::Message progMsg;
            progMsg.type = rps::ipc::MessageType::ProgressEvent;
            progMsg.payload = rps::ipc::ProgressEvent{status, percentage};
            connection->sendMessage(progMsg);
        };

        // --- CRASH TEST SIMULATION ---
        if (pluginPathStr == "CRASH_ME") {
            progressCb(10, "Simulating crash...");
            std::cerr << "Scanner triggering intentional crash!\n";
            int* ptr = nullptr;
            *ptr = 42; // Segmentation fault
        }
        
        if (pluginPathStr == "HANG_ME") {
            progressCb(10, "Simulating hang...");
            std::cerr << "Scanner triggering intentional hang!\n";
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        // -----------------------------

        // 5. Execute Scan
        logStage("Starting scan with " + activeScanner->getFormatName() + " scanner...");
        if (activeScanner) {
            try {
                auto t0 = std::chrono::steady_clock::now();
                auto result = activeScanner->scan(pluginPath, progressCb);
                auto t1 = std::chrono::steady_clock::now();

                if (g_verbose) {
                    auto scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                    std::cerr << "[scanner] " << pluginPath.filename().string()
                              << ": scan() returned in " << scanMs << "ms"
                              << " (" << result.parameters.size() << " params)" << std::endl;
                }

                rps::ipc::Message resMsg;
                resMsg.type = rps::ipc::MessageType::ScanResult;
                resMsg.payload = result;

                auto t2 = std::chrono::steady_clock::now();
                bool sent = connection->sendMessage(resMsg);
                auto t3 = std::chrono::steady_clock::now();

                if (g_verbose) {
                    auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
                    std::cerr << "[scanner] " << pluginPath.filename().string()
                              << ": IPC sendMessage " << (sent ? "OK" : "FAILED")
                              << " in " << sendMs << "ms" << std::endl;
                }
            } catch (const std::exception& scanErr) {
                std::string what = scanErr.what();
                std::cerr << "Scanner Fatal Error: " << what << "\n";
                rps::ipc::Message errMsg;
                errMsg.type = rps::ipc::MessageType::ErrorMessage;
                errMsg.payload = rps::ipc::ErrorMessage{"Scan Error", what};
                connection->sendMessage(errMsg);
            }
        }

        // Give IPC time to flush
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    } catch (const std::exception& e) {
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


