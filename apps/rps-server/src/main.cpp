#include <rps/server/RpsServiceImpl.hpp>
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <boost/program_options.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <csignal>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static std::atomic<grpc::Server*> g_server{nullptr};
static std::atomic<rps::server::RpsServiceImpl*> g_service{nullptr};

static void signalHandler(int sig) {
    spdlog::info("Received signal {}, shutting down...", sig);
    // Stop scan engine FIRST — kills all scanner children immediately
    auto* svc = g_service.load();
    if (svc) svc->stopScan();
    // Then shut down gRPC with a deadline so it doesn't block forever
    auto* srv = g_server.load();
    if (srv) {
        std::thread([srv]() {
            auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
            srv->Shutdown(deadline);
        }).detach();
    }
}

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;

    po::options_description desc("RPS Server Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("port,p", po::value<int>()->default_value(50051), "gRPC listen port")
        ("db", po::value<std::string>()->default_value("rps-plugins.db"), "Path to the SQLite database file")
        ("scanner-bin,b", po::value<std::string>()->default_value(
#ifdef _WIN32
            "rps-pluginscanner.exe"
#else
            "rps-pluginscanner"
#endif
        ), "Path to the scanner binary")
        ("log", po::value<std::string>()->default_value("rps-server.log"), "Log file path")
        ("log-level", po::value<std::string>()->default_value("info"), "Log level: trace, debug, info, warn, error");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << desc << "\n";
        return 0;
    }

    // --- Setup logging ---
    std::string logFile = vm["log"].as<std::string>();
    std::string logLevel = vm["log-level"].as<std::string>();

    try {
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile, true);
        auto logger = std::make_shared<spdlog::logger>("rps", spdlog::sinks_init_list{consoleSink, fileSink});
        spdlog::set_default_logger(logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        if (logLevel == "trace") spdlog::set_level(spdlog::level::trace);
        else if (logLevel == "debug") spdlog::set_level(spdlog::level::debug);
        else if (logLevel == "info") spdlog::set_level(spdlog::level::info);
        else if (logLevel == "warn") spdlog::set_level(spdlog::level::warn);
        else if (logLevel == "error") spdlog::set_level(spdlog::level::err);
        else spdlog::set_level(spdlog::level::info);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log init failed: " << ex.what() << std::endl;
        return 1;
    }

#ifdef _WIN32
    // Kill all child scanner processes when this process exits (Ctrl+C, taskkill, etc.)
    HANDLE hJob = CreateJobObject(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        AssignProcessToJobObject(hJob, GetCurrentProcess());
    }
#endif

    // --- Resolve scanner binary ---
    std::string scannerBin = vm["scanner-bin"].as<std::string>();
    fs::path scannerPath(scannerBin);
    if (!scannerPath.is_absolute()) {
        fs::path exePath = boost::dll::program_location();
        scannerPath = exePath.parent_path() / scannerPath;
        if (!fs::exists(scannerPath)) {
            scannerPath = exePath.parent_path().parent_path() / "rps-pluginscanner" / scannerPath.filename();
        }
    }

    std::string dbPath = vm["db"].as<std::string>();
    int port = vm["port"].as<int>();
    std::string serverAddress = "0.0.0.0:" + std::to_string(port);

    spdlog::info("RPS Server starting on {}", serverAddress);
    spdlog::info("Database: {}", dbPath);
    spdlog::info("Scanner: {}", scannerPath.string());
    spdlog::info("Log file: {}", logFile);
    spdlog::info("Log level: {}", logLevel);

    // --- Build and start gRPC server ---
    rps::server::RpsServiceImpl service(dbPath, scannerPath.string());

    grpc::ServerBuilder builder;
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    if (!server) {
        spdlog::error("Failed to start gRPC server on {}", serverAddress);
        return 1;
    }

    service.setServer(server.get());
    g_server.store(server.get());
    g_service.store(&service);

    // Handle SIGINT/SIGTERM for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    spdlog::info("RPS Server listening on {}", serverAddress);
    server->Wait();

    spdlog::info("RPS Server stopped.");
    return 0;
}
