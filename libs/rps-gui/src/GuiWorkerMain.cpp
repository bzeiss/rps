#include <rps/gui/GuiWorkerMain.hpp>
#include <rps/gui/SdlWindow.hpp>
#include <rps/ipc/Connection.hpp>
#include <rps/ipc/Messages.hpp>
#include <SDL3/SDL.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#endif
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <iostream>
#include <thread>
#include <chrono>

namespace po = boost::program_options;

namespace rps::gui {

int GuiWorkerMain::run(int argc, char* argv[], std::unique_ptr<IPluginGuiHost> host) {
    // Parse command-line arguments
    po::options_description desc("Plugin GUI Host Worker");
    desc.add_options()
        ("ipc-id", po::value<std::string>(), "IPC queue identifier")
        ("plugin-path", po::value<std::string>(), "Path to plugin binary")
        ("format", po::value<std::string>()->default_value("clap"), "Plugin format")
        ("help,h", "Show help");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing arguments: " << e.what() << "\n";
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    if (!vm.count("ipc-id") || !vm.count("plugin-path")) {
        std::cerr << "Error: --ipc-id and --plugin-path are required.\n";
        return 1;
    }

    const auto ipcId = vm["ipc-id"].as<std::string>();
    const auto pluginPath = vm["plugin-path"].as<std::string>();
    const auto format = vm["format"].as<std::string>();

    // Set up logging — both console and file for crash diagnostics
    try {
        auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            "rps-pluginhost-" + format + ".log", true);
        auto logger = std::make_shared<spdlog::logger>(
            "pluginhost", spdlog::sinks_init_list{consoleSink, fileSink});
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);  // Flush every message for crash diagnostics
        spdlog::set_default_logger(logger);
    } catch (...) {
        // Fall back to default logger
        spdlog::set_level(spdlog::level::debug);
    }

    spdlog::info("=== rps-pluginhost-{} starting ===", format);
    spdlog::info("  ipc-id: {}", ipcId);
    spdlog::info("  plugin-path: {}", pluginPath);

    // Connect to IPC queue
    spdlog::info("Connecting to IPC queue...");
    auto connection = rps::ipc::MessageQueueConnection::createClient(ipcId);
    if (!connection) {
        spdlog::error("Failed to connect to IPC queue: {}", ipcId);
        return 1;
    }
    spdlog::info("IPC connected");

    // Initialize SDL3 video subsystem
    spdlog::info("Initializing SDL3...");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        spdlog::error("SDL_Init failed: {}", SDL_GetError());

        // Send error back via IPC
        rps::ipc::Message errMsg;
        errMsg.type = rps::ipc::MessageType::GuiClosedEvent;
        errMsg.payload = rps::ipc::GuiClosedEvent{"crash"};
        connection->sendMessage(errMsg);
        return 1;
    }
    spdlog::info("SDL3 initialized");

    try {
        // Open the plugin GUI
        spdlog::info("Opening plugin GUI...");
        auto result = host->open(boost::filesystem::path(pluginPath));
        spdlog::info("Plugin GUI opened: '{}' ({}x{})", result.name, result.width, result.height);

        // Send GuiOpenedEvent
        rps::ipc::Message openedMsg;
        openedMsg.type = rps::ipc::MessageType::GuiOpenedEvent;
        openedMsg.payload = rps::ipc::GuiOpenedEvent{result.name, result.width, result.height};
        connection->sendMessage(openedMsg);

        // Query and send full parameter list
        auto params = host->getParameters();
        if (!params.empty()) {
            spdlog::info("Sending ParameterListEvent ({} params)", params.size());
            rps::ipc::Message paramListMsg;
            paramListMsg.type = rps::ipc::MessageType::ParameterListEvent;
            paramListMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
            connection->sendMessage(paramListMsg);
        }

        // Run the GUI event loop — this blocks until the window is closed
        // We also need to poll IPC for CloseGuiRequest
        std::atomic<bool> ipcClosed{false};

        // IPC listener thread: watches for CloseGuiRequest and state commands
        std::thread ipcThread([&]() {
            while (!ipcClosed.load(std::memory_order_relaxed)) {
                auto msg = connection->receiveMessage(200);
                if (!msg) continue;

                if (msg->type == rps::ipc::MessageType::CloseGuiRequest) {
                    spdlog::info("Received CloseGuiRequest via IPC");
                    host->requestClose();
                    break;
                }

                if (msg->type == rps::ipc::MessageType::GetStateRequest) {
                    spdlog::info("Received GetStateRequest via IPC");
                    auto resp = host->saveState();
                    rps::ipc::Message respMsg;
                    respMsg.type = rps::ipc::MessageType::GetStateResponse;
                    respMsg.payload = std::move(resp);
                    connection->sendMessage(respMsg);
                    continue;
                }

                if (msg->type == rps::ipc::MessageType::SetStateRequest) {
                    spdlog::info("Received SetStateRequest via IPC");
                    auto& req = std::get<rps::ipc::SetStateRequest>(msg->payload);
                    auto resp = host->loadState(req.stateData);
                    rps::ipc::Message respMsg;
                    respMsg.type = rps::ipc::MessageType::SetStateResponse;
                    respMsg.payload = std::move(resp);
                    connection->sendMessage(respMsg);

                    // After successful state load, re-send parameter list
                    if (resp.success) {
                        auto params = host->getParameters();
                        spdlog::info("Re-sending ParameterListEvent ({} params) after state restore", params.size());
                        rps::ipc::Message paramMsg;
                        paramMsg.type = rps::ipc::MessageType::ParameterListEvent;
                        paramMsg.payload = rps::ipc::ParameterListEvent{std::move(params)};
                        connection->sendMessage(paramMsg);
                    }
                    continue;
                }
            }
        });

        spdlog::info("Entering GUI event loop (with parameter polling)...");
        std::string closeReason;

        // Parameter change callback: sends delta updates over IPC
        auto paramChangeCb = [&connection](std::vector<rps::ipc::ParameterValueUpdate> changes) {
            rps::ipc::Message msg;
            msg.type = rps::ipc::MessageType::ParameterValuesEvent;
            msg.payload = rps::ipc::ParameterValuesEvent{std::move(changes)};
            connection->sendMessage(msg);
        };

        host->runEventLoop(
            [&](const std::string& reason) {
                closeReason = reason;
            },
            paramChangeCb
        );

        spdlog::info("GUI event loop exited (reason: {})", closeReason.empty() ? "user" : closeReason);

        ipcClosed.store(true, std::memory_order_relaxed);
        if (ipcThread.joinable()) {
            ipcThread.join();
        }

        // Send GuiClosedEvent
        rps::ipc::Message closedMsg;
        closedMsg.type = rps::ipc::MessageType::GuiClosedEvent;
        closedMsg.payload = rps::ipc::GuiClosedEvent{closeReason.empty() ? "user" : closeReason};
        connection->sendMessage(closedMsg);

    } catch (const std::exception& e) {
        spdlog::error("Plugin GUI error: {}", e.what());

        // Send error event
        rps::ipc::Message errMsg;
        errMsg.type = rps::ipc::MessageType::GuiClosedEvent;
        errMsg.payload = rps::ipc::GuiClosedEvent{"crash"};
        connection->sendMessage(errMsg);
    }

    spdlog::info("=== rps-pluginhost shutting down ===");
    SDL_Quit();
    return 0;
}

} // namespace rps::gui
