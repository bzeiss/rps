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

namespace rps::scanner {
std::vector<std::unique_ptr<IPluginFormatScanner>> ScannerFactory::createAllScanners() {
    std::vector<std::unique_ptr<IPluginFormatScanner>> scanners;
    scanners.push_back(std::make_unique<ClapScanner>());
    scanners.push_back(std::make_unique<Vst3Scanner>());
    // Add AU, LV2 here later
    return scanners;
}
}

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;
    
    po::options_description desc("Scanner Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("ipc-id,i", po::value<std::string>(), "IPC connection handle ID")
        ("plugin-path,p", po::value<std::string>(), "Path to plugin to scan");
        
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

    std::string ipcId = vm["ipc-id"].as<std::string>();
    std::string pluginPathStr = vm.count("plugin-path") ? vm["plugin-path"].as<std::string>() : "unknown";
    fs::path pluginPath(pluginPathStr);

    try {
        // 1. Connect to Orchestrator IPC Queue
        auto connection = rps::ipc::MessageQueueConnection::createClient(ipcId);

        // 2. Wait for the ScanRequest
        auto maybeMsg = connection->receiveMessage(5000); // 5 sec timeout
        if (!maybeMsg.has_value() || maybeMsg.value().type != rps::ipc::MessageType::ScanRequest) {
            std::cerr << "Failed to receive ScanRequest from Orchestrator.\n";
            return 1;
        }

        auto req = std::get<rps::ipc::ScanRequest>(maybeMsg.value().payload);

        // 3. Find appropriate scanner
        auto scanners = rps::scanner::ScannerFactory::createAllScanners();
        rps::scanner::IPluginFormatScanner* activeScanner = nullptr;

        for (auto& s : scanners) {
            if (s->canHandle(pluginPath)) {
                activeScanner = s.get();
                break;
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
        if (activeScanner) {
            auto result = activeScanner->scan(pluginPath, progressCb);
            
            rps::ipc::Message resMsg;
            resMsg.type = rps::ipc::MessageType::ScanResult;
            resMsg.payload = result;
            connection->sendMessage(resMsg);
        }

        // Give IPC time to flush
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    } catch (const std::exception& e) {
        std::cerr << "Scanner Fatal Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}


