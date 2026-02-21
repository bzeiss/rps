#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <boost/program_options.hpp>
#include <boost/process/v1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <rps/ipc/Connection.hpp>

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    namespace bp = boost::process::v1;

    
    po::options_description desc("Orchestrator Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("scan,s", po::value<std::string>(), "Path to plugin to scan")
        ("scanner-bin,b", po::value<std::string>()->default_value("rps-pluginscanner.exe"), "Path to the scanner binary")
        ("timeout,t", po::value<int>()->default_value(10000), "Timeout in milliseconds for the scanner to respond");
        
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

    if (!vm.count("scan")) {
        std::cerr << "Error: --scan <plugin_path> is required.\n";
        return 1;
    }

    std::string pluginPath = vm["scan"].as<std::string>();
    std::string scannerBin = vm["scanner-bin"].as<std::string>();
    int timeoutMs = vm["timeout"].as<int>();

    std::cout << "RPS Plugin Scan Orchestrator starting...\n";
    std::cout << "Target plugin: " << pluginPath << "\n";

    // 1. Generate unique IPC connection ID
    auto uuid = boost::uuids::random_generator()();
    std::string ipcId = "rps_ipc_" + boost::uuids::to_string(uuid);

    std::cout << "Initializing IPC Server: " << ipcId << "\n";

    try {
        // 2. Create the IPC Server Connection
        auto connection = rps::ipc::MessageQueueConnection::createServer(ipcId);

        // 3. Spawn the scanner process
        std::cout << "Spawning scanner process...\n";
        bp::child scannerProc(scannerBin, "--ipc-id", ipcId, "--plugin-path", pluginPath);

        // 4. Send the Scan Request
        rps::ipc::Message reqMsg;
        reqMsg.type = rps::ipc::MessageType::ScanRequest;
        reqMsg.payload = rps::ipc::ScanRequest{ pluginPath, "unknown", false };
        
        std::cout << "Sending ScanRequest...\n";
        if (!connection->sendMessage(reqMsg)) {
            std::cerr << "Failed to send ScanRequest to worker.\n";
            scannerProc.terminate();
            return 1;
        }

        // 5. Listen Loop
        bool done = false;
        auto startTime = std::chrono::steady_clock::now();
        auto lastResponseTime = startTime;

        while (!done) {
            // Wait up to 100ms for a message
            auto maybeMsg = connection->receiveMessage(100);
            auto now = std::chrono::steady_clock::now();
            
            if (maybeMsg.has_value()) {
                lastResponseTime = now; // Reset timeout watchdog
                const auto& msg = maybeMsg.value();
                if (msg.type == rps::ipc::MessageType::ProgressEvent) {
                    auto evt = std::get<rps::ipc::ProgressEvent>(msg.payload);
                    std::cout << "[Scanner Progress] " << evt.progressPercentage << "% - " << evt.status << "\n";
                } 
                else if (msg.type == rps::ipc::MessageType::ScanResult) {
                    auto res = std::get<rps::ipc::ScanResult>(msg.payload);
                    std::cout << "[Scanner Result] Success!\n";
                    std::cout << "  Name: " << res.name << "\n";
                    std::cout << "  Vendor: " << res.vendor << "\n";
                    std::cout << "  Version: " << res.version << "\n";
                    std::cout << "  I/O: " << res.numInputs << " in / " << res.numOutputs << " out\n";
                    done = true;
                }
                else if (msg.type == rps::ipc::MessageType::ErrorMessage) {
                    auto err = std::get<rps::ipc::ErrorMessage>(msg.payload);
                    std::cerr << "[Scanner Error] " << err.error << " - " << err.details << "\n";
                    done = true;
                }
            } else {
                // Check if process died
                if (!scannerProc.running()) {
                    std::cerr << "[Orchestrator] Scanner process terminated unexpectedly (exit code: " 
                              << scannerProc.exit_code() << ")\n";
                    done = true;
                } 
                // Check for watchdog timeout
                else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastResponseTime).count() > timeoutMs) {
                    std::cerr << "[Orchestrator] Scanner process timed out (hung plugin?). Terminating it.\n";
                    scannerProc.terminate();
                    done = true;
                }
            }
        }

        if (scannerProc.running()) {
            scannerProc.wait();
        }

    } catch (const std::exception& e) {
        std::cerr << "[Orchestrator Fatal Error] " << e.what() << "\n";
        return 1;
    }

    std::cout << "Orchestrator shutdown complete.\n";
    return 0;
}

