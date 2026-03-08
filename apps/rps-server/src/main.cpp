#include <rps/server/RpsServiceImpl.hpp>
#include <rps/core/LoggingInit.hpp>
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4244)
#pragma warning(disable: 4245)
#endif
#include <boost/program_options.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
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
        ), "Path to the scanner binary");

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

    // --- Setup logging via environment variables ---
    rps::core::initLogging("SERVER", "rps-server.log");

    // Ensure RPS_LOG_DIR is set to an absolute path so child processes
    // (scanner, pluginhost) running in temp directories resolve it correctly.
    {
        std::string logDir;
        if (const char* envDir = std::getenv("RPS_LOG_DIR")) {
            // User provided — resolve relative paths against server CWD
            auto p = boost::filesystem::absolute(envDir);
            logDir = p.string();
        } else {
            // Default to server's CWD
            logDir = boost::filesystem::current_path().string();
        }
#ifdef _WIN32
        _putenv_s("RPS_LOG_DIR", logDir.c_str());
#else
        setenv("RPS_LOG_DIR", logDir.c_str(), 1);
#endif
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
        fs::path exeDir = boost::dll::program_location().parent_path();
        std::vector<fs::path> candidates = {
            fs::current_path() / scannerPath,
            exeDir / scannerPath
        };

        bool found = false;
        for (auto& c : candidates) {
            if (fs::exists(c) && fs::is_regular_file(c)) {
                scannerPath = fs::canonical(c);
                found = true;
                break;
            }
        }

        if (!found) {
            spdlog::error("Cannot find rps-pluginscanner binary: {}. Use --scanner-bin.", scannerBin);
            return 1;
        }
    }

    std::string dbPath = vm["db"].as<std::string>();
    int port = vm["port"].as<int>();
    std::string serverAddress = "0.0.0.0:" + std::to_string(port);

    spdlog::info("RPS Server starting on {}", serverAddress);
    spdlog::info("Database: {}", dbPath);
    spdlog::info("Scanner: {}", scannerPath.string());

    // --- Compute UDS path for local IPC ---
    std::string udsPath;
    {
        auto tempDir = boost::filesystem::temp_directory_path();
        udsPath = (tempDir / ("rps-server-" + std::to_string(port) + ".sock")).string();
        // Remove stale socket file from a previous crash
        boost::system::error_code ec;
        boost::filesystem::remove(udsPath, ec);
    }

    // --- Build and start gRPC server ---
    rps::server::RpsServiceImpl service(dbPath, scannerPath.string());

    // Try TCP + UDS dual-listen first, fall back to TCP-only if UDS fails
    grpc::ServerBuilder builder;
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
    // gRPC URI: "unix:" prefix (no "//") for cross-platform compatibility
    // with Windows drive letters (e.g. unix:C:\Users\...\rps-server.sock)
    std::string udsAddress = "unix:" + udsPath;
    builder.AddListeningPort(udsAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    auto server = builder.BuildAndStart();
    if (!server) {
        // UDS may have failed — retry with TCP only
        spdlog::warn("Dual-listen (TCP+UDS) failed, retrying TCP-only...");
        udsPath.clear();
        grpc::ServerBuilder fallbackBuilder;
        fallbackBuilder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
        fallbackBuilder.RegisterService(&service);
        server = fallbackBuilder.BuildAndStart();
        if (!server) {
            spdlog::error("Failed to start gRPC server on {}", serverAddress);
            return 1;
        }
    }

    service.setServer(server.get());
    g_server.store(server.get());
    g_service.store(&service);

    // Handle SIGINT/SIGTERM for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (!udsPath.empty()) {
        spdlog::info("RPS Server listening on {} and UDS: {}", serverAddress, udsPath);
    } else {
        spdlog::info("RPS Server listening on {} (UDS unavailable)", serverAddress);
    }
    server->Wait();

    // Clean up UDS socket file
    {
        boost::system::error_code ec;
        boost::filesystem::remove(udsPath, ec);
    }

    spdlog::info("RPS Server stopped.");
    return 0;
}
