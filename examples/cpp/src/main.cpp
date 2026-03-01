#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "WindowsProcessJob.hpp"
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

#include <csignal>

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <mutex>

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

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wdll-attribute-on-redeclaration"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include <grpcpp/grpcpp.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include "rps.grpc.pb.h"
#include "rps.pb.h"

namespace po = boost::program_options;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// ANSI helpers
// ---------------------------------------------------------------------------
namespace ansi {
    const char* reset   = "\033[0m";
    const char* bold    = "\033[1m";
    const char* dim     = "\033[2m";
    const char* red     = "\033[31m";
    const char* green   = "\033[32m";
    const char* yellow  = "\033[33m";
    const char* blue    = "\033[34m";
    const char* cyan    = "\033[36m";
    const char* clear_line = "\033[2K";
    const char* cursor_up = "\033[A";
    const char* hide_cursor = "\033[?25l";
    const char* show_cursor = "\033[?25h";

    void move_up(int n) {
        for (int i = 0; i < n; ++i) std::cout << cursor_up;
    }
}

// ---------------------------------------------------------------------------
// Signal handling for clean Ctrl+C
// ---------------------------------------------------------------------------
static std::atomic<bool> g_interrupted{false};

// Forward-declare signal setup.
static void installSignalHandlers();
#ifdef _WIN32
static bool processDebugEnabled() {
    const char* v = std::getenv("RPS_DEBUG_PROCESS_LIFECYCLE");
    return v && std::string(v) == "1";
}
#endif

// ---------------------------------------------------------------------------
// Server process manager
// ---------------------------------------------------------------------------
class ServerManager {
public:
    ServerManager(const std::string& bin, int port, const std::string& db,
                  const std::string& logLevel)
        : m_bin(bin), m_port(port), m_db(db), m_logLevel(logLevel) {}

    ~ServerManager() { stop(); }

    bool start(int timeoutSec = 10) {
        std::string cmd = m_bin
            + " --port " + std::to_string(m_port)
            + " --db " + m_db
            + " --log-level " + m_logLevel;

#ifdef _WIN32
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()),
                            nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return false;
        }
        m_processHandle = pi.hProcess;
        CloseHandle(pi.hThread);
        if (processDebugEnabled()) {
            std::cerr << "[rps] spawned server process\n";
        }

        // Windows process ownership model:
        // parent/child does not imply lifetime ownership. Attach rps-server
        // to a Job Object so parent exit tears down the process tree.
        std::string jobErr;
        if (!m_windowsJob.attach(m_processHandle, &jobErr)) {
            TerminateProcess(m_processHandle, 1);
            CloseHandle(m_processHandle);
            m_processHandle = nullptr;
            m_windowsJob.close();
            std::cerr << "Error: " << jobErr << "\n";
            return false;
        }
#else
        // Fork + exec on POSIX
        m_pid = fork();
        if (m_pid == 0) {
            // Suppress stdout/stderr — server logs to file already
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execlp(m_bin.c_str(), m_bin.c_str(),
                   "--port", std::to_string(m_port).c_str(),
                   "--db", m_db.c_str(),
                   "--log-level", m_logLevel.c_str(),
                   nullptr);
            _exit(1);
        }
        if (m_pid < 0) return false;
#endif
        m_running = true;

        // Wait for server to accept connections
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
        while (std::chrono::steady_clock::now() < deadline) {
            auto channel = grpc::CreateChannel(address(), grpc::InsecureChannelCredentials());
            if (channel->WaitForConnected(
                    std::chrono::system_clock::now() + std::chrono::milliseconds(500))) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return false;
    }

    void stop(bool inHurry = false) {
        if (!m_running) return;
        m_running = false;

        if (!inHurry) {
            // Try graceful shutdown via gRPC
            try {
                auto channel = grpc::CreateChannel(address(), grpc::InsecureChannelCredentials());
                auto stub = rps::v1::RpsService::NewStub(channel);
                grpc::ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
                rps::v1::ShutdownRequest req;
                rps::v1::ShutdownResponse resp;
                stub->Shutdown(&ctx, req, &resp);
            } catch (...) {}
        }

#ifdef _WIN32
        if (m_processHandle) {
            if (processDebugEnabled()) {
                std::cerr << "[rps] stopping server process\n";
            }
            if (inHurry) {
                TerminateProcess(m_processHandle, 0);
            } else {
                WaitForSingleObject(m_processHandle, 3000);
                TerminateProcess(m_processHandle, 0);
            }
            CloseHandle(m_processHandle);
            m_processHandle = nullptr;
        }
        m_windowsJob.close();
#else
        if (m_pid > 0) {
            int status;
            kill(m_pid, SIGTERM);
            auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(inHurry ? 1 : 3);
            while (std::chrono::steady_clock::now() < deadline) {
                if (waitpid(m_pid, &status, WNOHANG) != 0) { m_pid = -1; return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            kill(m_pid, SIGKILL);
            waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
#endif
    }

    std::string address() const {
        return "localhost:" + std::to_string(m_port);
    }

private:
    std::string m_bin;
    int m_port;
    std::string m_db;
    std::string m_logLevel;
    bool m_running = false;
#ifdef _WIN32
    HANDLE m_processHandle = nullptr;
    WindowsProcessJob m_windowsJob;
#else
    pid_t m_pid = -1;
#endif
};

static void signalHandlerFunc(int /*sig*/) {
    g_interrupted = true;
}

static void installSignalHandlers() {
    std::signal(SIGINT, signalHandlerFunc);
    std::signal(SIGTERM, signalHandlerFunc);
}

// ---------------------------------------------------------------------------
// Console TUI state
// ---------------------------------------------------------------------------
struct WorkerState {
    std::string pluginFilename;
    int percentage = 0;
    std::string stage;
    bool idle = true;
    std::string lastResult;
};

struct ScanState {
    uint32_t totalPlugins = 0;
    uint32_t workerCount = 0;
    uint32_t skippedUnchanged = 0;
    uint32_t skippedBlocked = 0;
    uint32_t completed = 0;
    uint32_t success = 0;
    uint32_t fail = 0;
    uint32_t crash = 0;
    uint32_t timeout = 0;
    uint32_t skipped = 0;
    std::map<uint32_t, WorkerState> workers;
    std::vector<std::string> recent;
    bool finished = false;
    int64_t totalMs = 0;
    std::vector<std::pair<std::string, std::string>> failures;
    std::string mode;
    std::string formats;
    std::mutex mtx;

    static constexpr size_t MAX_RECENT = 12;

    void addRecent(const std::string& line) {
        recent.push_back(line);
        if (recent.size() > MAX_RECENT)
            recent.erase(recent.begin(), recent.begin() + (recent.size() - MAX_RECENT));
    }
};

// ---------------------------------------------------------------------------
// Format helpers
// ---------------------------------------------------------------------------
std::string formatDuration(int64_t ms) {
    int64_t sec = ms / 1000;
    int64_t min = sec / 60;
    sec %= 60;
    std::ostringstream oss;
    if (min > 0) oss << min << "m ";
    oss << sec << "." << std::setw(1) << std::setfill('0') << (ms % 1000) / 100 << "s";
    return oss.str();
}

std::string progressBar(int pct, int width) {
    int filled = width * pct / 100;
    std::string bar;
    for (int i = 0; i < width; ++i)
        bar += (i < filled) ? "━" : "╌";
    return bar;
}

// ---------------------------------------------------------------------------
// Render TUI
// ---------------------------------------------------------------------------
int g_lastLines = 0;

void render(ScanState& state) {
    std::lock_guard<std::mutex> lock(state.mtx);

    // Move cursor up to overwrite previous frame
    if (g_lastLines > 0) {
        ansi::move_up(g_lastLines);
    }

    int lines = 0;
    auto printLine = [&](const std::string& s) {
        std::cout << ansi::clear_line << s << "\n";
        ++lines;
    };

    // Header
    uint32_t total = state.totalPlugins;
    uint32_t done = state.completed;
    int pct = (total > 0) ? static_cast<int>(done * 100 / total) : 0;

    std::ostringstream hdr;
    hdr << ansi::bold << ansi::blue << "RPS Scanner" << ansi::reset
        << "  " << state.mode << " (" << state.formats << ")"
        << "  Workers: " << state.workerCount;
    printLine(hdr.str());

    std::ostringstream prog;
    prog << " " << progressBar(pct, 40) << " " << done << "/" << total << " (" << pct << "%)";
    if (state.skippedUnchanged > 0)
        prog << "  [unchanged: " << state.skippedUnchanged << "]";
    printLine(prog.str());

    std::ostringstream stats;
    stats << " " << ansi::green << "✓" << state.success << ansi::reset
          << "  " << ansi::red << "✗" << state.fail << ansi::reset
          << "  " << ansi::red << "💥" << state.crash << ansi::reset
          << "  " << ansi::yellow << "⏱" << state.timeout << ansi::reset
          << "  " << ansi::dim << "⊘" << state.skipped << ansi::reset;
    printLine(stats.str());

    printLine("");

    // Workers
    for (auto& [wid, ws] : state.workers) {
        std::ostringstream wl;
        if (ws.idle) {
            wl << ansi::dim << " #" << wid << "  idle";
            if (!ws.lastResult.empty())
                wl << "  " << ws.lastResult;
            wl << ansi::reset;
        } else {
            wl << ansi::bold << " #" << wid << ansi::reset
               << "  " << ansi::cyan << progressBar(ws.percentage, 15) << ansi::reset
               << " " << std::setw(3) << ws.percentage << "%"
               << "  " << ws.pluginFilename;
            if (!ws.stage.empty())
                wl << " " << ansi::dim << "(" << ws.stage << ")" << ansi::reset;
        }
        printLine(wl.str());
    }

    printLine("");

    // Recent results
    size_t recentStart = 0;
    size_t maxRecent = 10;
    if (state.recent.size() > maxRecent)
        recentStart = state.recent.size() - maxRecent;
    for (size_t i = recentStart; i < state.recent.size(); ++i) {
        printLine(" " + state.recent[i]);
    }

    // Pad remaining lines from last frame
    while (lines < g_lastLines) {
        printLine("");
    }

    std::cout << std::flush;
    g_lastLines = lines;
}

// ---------------------------------------------------------------------------
// Process events
// ---------------------------------------------------------------------------
void processEvent(const rps::v1::ScanEvent& event, ScanState& state) {
    std::lock_guard<std::mutex> lock(state.mtx);

    if (event.has_scan_started()) {
        auto& e = event.scan_started();
        state.totalPlugins = e.total_plugins();
        state.workerCount = e.worker_count();
        state.skippedUnchanged = e.skipped_unchanged();
        state.skippedBlocked = e.skipped_blocked();
        for (uint32_t i = 1; i <= e.worker_count(); ++i)
            state.workers.emplace(i, WorkerState{});
    }
    else if (event.has_plugin_started()) {
        auto& e = event.plugin_started();
        auto& ws = state.workers[e.worker_id()];
        ws.pluginFilename = e.plugin_filename();
        ws.percentage = 0;
        ws.stage = "Starting...";
        ws.idle = false;
    }
    else if (event.has_plugin_progress()) {
        auto& e = event.plugin_progress();
        auto it = state.workers.find(e.worker_id());
        if (it != state.workers.end()) {
            it->second.percentage = e.percentage();
            it->second.stage = e.stage();
        }
    }
    else if (event.has_plugin_completed()) {
        auto& e = event.plugin_completed();
        state.completed++;
        auto it = state.workers.find(e.worker_id());

        std::string line;
        switch (e.outcome()) {
            case rps::v1::OUTCOME_SUCCESS:
                state.success++;
                line = std::string(ansi::green) + "✓" + ansi::reset + " " + e.plugin_filename()
                     + " → " + e.plugin_name() + " v" + e.plugin_version()
                     + " (" + std::to_string(e.elapsed_ms()) + "ms)";
                if (it != state.workers.end())
                    it->second.lastResult = std::string("✓ ") + e.plugin_name();
                break;
            case rps::v1::OUTCOME_FAIL:
                state.fail++;
                line = std::string(ansi::red) + "✗" + ansi::reset + " " + e.plugin_filename()
                     + " → " + e.error_message()
                     + " (" + std::to_string(e.elapsed_ms()) + "ms)";
                if (it != state.workers.end())
                    it->second.lastResult = "✗ FAIL";
                break;
            case rps::v1::OUTCOME_CRASH:
                state.crash++;
                line = std::string(ansi::red) + "💥" + ansi::reset + " " + e.plugin_filename()
                     + " → " + e.error_message()
                     + " (" + std::to_string(e.elapsed_ms()) + "ms)";
                if (it != state.workers.end())
                    it->second.lastResult = "💥 CRASH";
                break;
            case rps::v1::OUTCOME_TIMEOUT:
                state.timeout++;
                line = std::string(ansi::yellow) + "⏱" + ansi::reset + " " + e.plugin_filename()
                     + " → TIMEOUT (" + std::to_string(e.elapsed_ms()) + "ms)";
                if (it != state.workers.end())
                    it->second.lastResult = "⏱ TIMEOUT";
                break;
            case rps::v1::OUTCOME_SKIPPED:
                state.skipped++;
                line = std::string(ansi::dim) + "⊘" + ansi::reset + " " + e.plugin_filename()
                     + " → " + e.error_message();
                if (it != state.workers.end())
                    it->second.lastResult = "⊘ SKIP";
                break;
            default:
                line = "? " + e.plugin_filename();
                break;
        }

        state.addRecent(line);
        if (it != state.workers.end()) {
            it->second.idle = true;
            it->second.percentage = 0;
            it->second.stage.clear();
            it->second.pluginFilename.clear();
        }
    }
    else if (event.has_plugin_retry()) {
        auto& e = event.plugin_retry();
        std::string filename = fs::path(e.plugin_path()).filename().string();
        state.addRecent(std::string(ansi::yellow) + "↻" + ansi::reset + " " + filename
                        + " retry " + std::to_string(e.attempt()) + "/" + std::to_string(e.max_retries())
                        + ": " + e.reason());
    }
    else if (event.has_scan_completed()) {
        auto& e = event.scan_completed();
        state.success = e.success();
        state.fail = e.fail();
        state.crash = e.crash();
        state.timeout = e.timeout();
        state.skipped = e.skipped();
        state.totalMs = e.total_ms();
        state.failures.clear();
        for (auto& f : e.failures())
            state.failures.emplace_back(f.path(), f.reason());
        state.finished = true;
    }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
int cmdScan(const std::string& serverAddr, const po::variables_map& vm,
            ServerManager* /*mgr*/) {
    auto channel = grpc::CreateChannel(serverAddr, grpc::InsecureChannelCredentials());
    auto stub = rps::v1::RpsService::NewStub(channel);

    // Build request
    rps::v1::StartScanRequest req;
    req.set_mode(vm["mode"].as<std::string>());
    req.set_formats(vm["formats"].as<std::string>());
    req.set_jobs(vm["jobs"].as<int>());
    req.set_retries(vm["retries"].as<int>());
    req.set_timeout_ms(vm["timeout"].as<int>());
    req.set_limit(vm["limit"].as<int>());
    req.set_verbose(vm.count("verbose") > 0);

    if (vm.count("filter"))
        req.set_filter(vm["filter"].as<std::string>());
    if (vm.count("scan"))
        req.set_single_plugin(vm["scan"].as<std::string>());
    if (vm.count("scan-dir")) {
        for (auto& d : vm["scan-dir"].as<std::vector<std::string>>())
            req.add_scan_dirs(d);
    }

    grpc::ClientContext ctx;
    auto stream = stub->StartScan(&ctx, req);

    ScanState state;
    state.mode = req.mode();
    state.formats = req.formats();

    // Consumer thread
    std::thread consumer([&] {
        rps::v1::ScanEvent event;
        while (stream->Read(&event)) {
            processEvent(event, state);
        }
        std::lock_guard<std::mutex> lock(state.mtx);
        state.finished = true;
    });

    // Render loop
    std::cout << ansi::hide_cursor;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(state.mtx);
            if (state.finished) break;
        }
        if (g_interrupted) {
            ctx.TryCancel();
            break;
        }
        render(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    render(state);  // Final
    std::cout << ansi::show_cursor;

    consumer.join();

    auto status = stream->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::OK) {
        std::cerr << "gRPC error: " << status.error_message() << "\n";
    }

    // Summary
    std::cout << "\n" << ansi::bold << "Scan complete" << ansi::reset
              << " in " << formatDuration(state.totalMs) << "\n";
    std::cout << "  " << ansi::green << "✓ " << state.success << ansi::reset
              << "  " << ansi::red << "✗ " << state.fail << ansi::reset
              << "  💥 " << state.crash
              << "  ⏱ " << state.timeout
              << "  ⊘ " << state.skipped << "\n";

    if (!state.failures.empty()) {
        std::cout << "\n" << ansi::red << "Failed plugins (" << state.failures.size() << "):" << ansi::reset << "\n";
        for (auto& [path, reason] : state.failures) {
            std::cout << "  " << path << "\n    -> " << reason << "\n";
        }
    }

    return (state.fail + state.crash > 0) ? 1 : 0;
}

int cmdStatus(const std::string& serverAddr) {
    auto channel = grpc::CreateChannel(serverAddr, grpc::InsecureChannelCredentials());
    auto stub = rps::v1::RpsService::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    rps::v1::GetStatusRequest req;
    rps::v1::GetStatusResponse resp;
    auto status = stub->GetStatus(&ctx, req, &resp);
    if (!status.ok()) {
        std::cerr << "Error: " << status.error_message() << "\n";
        return 1;
    }

    std::cout << "Server: " << serverAddr << "\n"
              << "State:  " << (resp.state() == rps::v1::GetStatusResponse::SCANNING ? "SCANNING" : "IDLE") << "\n"
              << "Uptime: " << formatDuration(resp.uptime_ms()) << "\n"
              << "DB:     " << resp.db_path() << "\n";
    return 0;
}

int cmdShutdown(const std::string& serverAddr) {
    auto channel = grpc::CreateChannel(serverAddr, grpc::InsecureChannelCredentials());
    auto stub = rps::v1::RpsService::NewStub(channel);

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    rps::v1::ShutdownRequest req;
    rps::v1::ShutdownResponse resp;
    auto status = stub->Shutdown(&ctx, req, &resp);
    if (!status.ok()) {
        std::cerr << "Error: " << status.error_message() << "\n";
        return 1;
    }
    std::cout << "Shutdown request sent.\n";
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Enable ANSI escape codes on Windows
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
#endif

    po::options_description general("General");
    general.add_options()
        ("help,h", "Produce help message")
        ("server", po::value<std::string>(), "Connect to existing server (host:port)")
        ("server-bin", po::value<std::string>(), "Path to rps-server binary")
        ("port", po::value<int>()->default_value(50051), "Server port")
        ("db", po::value<std::string>()->default_value("rps-plugins.db"), "Database file")
        ("command", po::value<std::string>()->default_value("scan"), "Command: scan, status, shutdown")
    ;

    po::options_description scan_opts("Scan options");
    scan_opts.add_options()
        ("formats,f", po::value<std::string>()->default_value("all"), "Formats to scan")
        ("mode,m", po::value<std::string>()->default_value("incremental"), "Scan mode: full|incremental")
        ("jobs,j", po::value<int>()->default_value(6), "Parallel workers")
        ("retries,r", po::value<int>()->default_value(3), "Max retries per plugin")
        ("timeout,t", po::value<int>()->default_value(120000), "Per-plugin timeout (ms)")
        ("limit,l", po::value<int>()->default_value(0), "Max plugins (0 = unlimited)")
        ("filter", po::value<std::string>(), "Filename filter")
        ("scan,s", po::value<std::string>(), "Single plugin to scan")
        ("scan-dir,d", po::value<std::vector<std::string>>(), "Directories to scan")
        ("verbose,v", "Enable verbose output")
    ;

    po::options_description all("RPS Example Client (C++)");
    all.add(general).add(scan_opts);

    po::positional_options_description positional;
    positional.add("command", 1);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv)
                      .options(all)
                      .positional(positional)
                      .run(),
                  vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    if (vm.count("help")) {
        std::cout << all << "\n";
        std::cout << "Commands:\n"
                  << "  scan      Start a plugin scan with TUI display (default)\n"
                  << "  status    Query server status\n"
                  << "  shutdown  Shut down the server\n";
        return 0;
    }

    std::string command = vm["command"].as<std::string>();

    // Determine server address
    std::string serverAddr;
    std::unique_ptr<ServerManager> mgr;

    if (vm.count("server")) {
        serverAddr = vm["server"].as<std::string>();
    } else {
        // Auto-spawn server
        std::string serverBin;
        if (vm.count("server-bin")) {
            serverBin = vm["server-bin"].as<std::string>();
        } else {
            // Predictable lookup: check CWD and the executable's own directory
            std::string binaryName = "rps-server";
#ifdef _WIN32
            binaryName += ".exe";
#endif
            fs::path exeDir = fs::canonical(fs::path(argv[0])).parent_path();
            std::vector<fs::path> candidates = {
                fs::current_path() / binaryName,
                exeDir / binaryName
            };

            for (auto& c : candidates) {
                if (fs::exists(c) && fs::is_regular_file(c)) {
                    serverBin = fs::canonical(c).string();
                    break;
                }
            }
        }

        if (serverBin.empty()) {
            std::cerr << "Error: Cannot find rps-server binary. Use --server-bin or --server.\n";
            return 1;
        }

        std::string scannerName = "rps-pluginscanner";
#ifdef _WIN32
        scannerName += ".exe";
#endif
        fs::path scannerBin = fs::path(serverBin).parent_path() / scannerName;
        if (!fs::exists(scannerBin) || !fs::is_regular_file(scannerBin)) {
            std::cerr << "Error: Cannot find " << scannerName << " alongside rps-server (" << serverBin << ").\n";
            return 1;
        }

        std::string logLevel = vm.count("verbose") ? "debug" : "info";
        mgr = std::make_unique<ServerManager>(
            serverBin, vm["port"].as<int>(), vm["db"].as<std::string>(), logLevel);

        std::cout << "Starting rps-server (" << serverBin << ")...\n";
        if (!mgr->start()) {
            std::cerr << "Error: Failed to start rps-server.\n";
            return 1;
        }
        serverAddr = mgr->address();
    }

    // Install signal handlers so Ctrl+C triggers fast server shutdown
    installSignalHandlers();

    int rc = 0;
    if (command == "scan") {
        rc = cmdScan(serverAddr, vm, mgr.get());
    } else if (command == "status") {
        rc = cmdStatus(serverAddr);
    } else if (command == "shutdown") {
        rc = cmdShutdown(serverAddr);
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        rc = 1;
    }

    // ServerManager destructor handles shutdown
    return rc;
}



