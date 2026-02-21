#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <boost/program_options.hpp>
#include <rps/ipc/Connection.hpp>

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    
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
    std::string pluginPath = vm.count("plugin-path") ? vm["plugin-path"].as<std::string>() : "unknown";

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

        // 3. Simulate Scanning Process with Progress Events
        rps::ipc::Message progMsg;
        progMsg.type = rps::ipc::MessageType::ProgressEvent;

        progMsg.payload = rps::ipc::ProgressEvent{"Loading DLL/SO into memory...", 10};
        connection->sendMessage(progMsg);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        // --- CRASH TEST SIMULATION ---
        if (pluginPath == "CRASH_ME") {
            std::cerr << "Scanner triggering intentional crash!\n";
            int* ptr = nullptr;
            *ptr = 42; // Segmentation fault
        }
        
        if (pluginPath == "HANG_ME") {
            std::cerr << "Scanner triggering intentional hang!\n";
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        // -----------------------------

        progMsg.payload = rps::ipc::ProgressEvent{"Instantiating plugin factory...", 40};
        connection->sendMessage(progMsg);

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        progMsg.payload = rps::ipc::ProgressEvent{"Extracting parameters...", 70};
        connection->sendMessage(progMsg);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 4. Send Dummy Success Result
        rps::ipc::Message resMsg;
        resMsg.type = rps::ipc::MessageType::ScanResult;
        
        rps::ipc::ScanResult res;
        res.name = "Dummy EQ";
        res.vendor = "RPS Audio";
        res.version = "1.0.0";
        res.numInputs = 2;
        res.numOutputs = 2;
        res.parameters.push_back({0, "Gain", 0.0});
        res.parameters.push_back({1, "Frequency", 1000.0});
        
        resMsg.payload = res;
        connection->sendMessage(resMsg);

        // Give IPC time to flush
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    } catch (const std::exception& e) {
        std::cerr << "Scanner Fatal Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

