#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <cctype>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <rps/core/PluginDiscovery.hpp>
#include <rps/core/FormatTraits.hpp>
#include <rps/orchestrator/ProcessPool.hpp>
#include <rps/orchestrator/ConsoleScanObserver.hpp>
#include <rps/orchestrator/db/DatabaseManager.hpp>

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    namespace fs = boost::filesystem;
    
    po::options_description desc("Orchestrator Options");
    desc.add_options()
        ("help,h", "Produce help message")
        ("scan-dir,d", po::value<std::vector<std::string>>()->multitoken(), "Directories to recursively scan for plugins")
        ("scan,s", po::value<std::string>(), "Single file to scan")
        ("scanner-bin,b", po::value<std::string>()->default_value("rps-pluginscanner.exe"), "Path to the scanner binary")
        ("timeout,t", po::value<int>()->default_value(300000), "Timeout in ms per plugin (0 = no timeout, default: 5 min)")
        ("jobs,j", po::value<size_t>()->default_value(std::thread::hardware_concurrency()), "Number of parallel workers")
        ("formats,f", po::value<std::string>()->default_value("all"), "Comma-separated list of formats to scan (e.g. vst3,clap) or 'all'")
        ("filter", po::value<std::string>(), "Only scan plugins whose filename contains this string")
        ("limit,l", po::value<size_t>()->default_value(0), "Maximum number of plugins to scan (0 = unlimited)")
        ("verbose,v", "Enable verbose scanner output (plugin debug logs)")
        ("retries,r", po::value<size_t>()->default_value(3), "Number of retries for failed plugins (0 = no retries)")
        ("db", po::value<std::string>()->default_value("rps-plugins.db"), "Path to the output SQLite database file");
        
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

#ifdef _WIN32
    // Enable ANSI/VT100 escape code processing so plugin output renders correctly
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD outMode = 0, errMode = 0;
    if (hOut != INVALID_HANDLE_VALUE) { GetConsoleMode(hOut, &outMode); SetConsoleMode(hOut, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
    if (hErr != INVALID_HANDLE_VALUE) { GetConsoleMode(hErr, &errMode); SetConsoleMode(hErr, errMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
#endif

    bool verbose = vm.count("verbose") > 0;

    rps::core::FormatRegistry formatRegistry;
    std::string formatListStr = vm["formats"].as<std::string>();
    auto formatsToScan = formatRegistry.parseFormats(formatListStr);

    if (formatsToScan.empty()) {
        std::cerr << "No valid formats specified to scan.\n";
        return 1;
    }

    std::vector<fs::path> pluginsToScan;

    if (vm.count("scan-dir")) {
        auto dirs = vm["scan-dir"].as<std::vector<std::string>>();
        auto found = rps::core::PluginDiscovery::findPlugins(dirs, formatsToScan);
        pluginsToScan.insert(pluginsToScan.end(), found.begin(), found.end());
    }

    if (vm.count("scan")) {
        fs::path p(vm["scan"].as<std::string>());
        // If it's a dummy test string like CRASH_ME, allow it through
        if (p.string() == "CRASH_ME" || p.string() == "HANG_ME" || fs::exists(p)) {
            pluginsToScan.push_back(p);
        } else {
            std::cerr << "Warning: Single scan file does not exist: " << p << "\n";
        }
    }

    // If neither --scan-dir nor --scan was provided, fallback to OS default directories for the requested formats
    if (pluginsToScan.empty() && !vm.count("scan-dir") && !vm.count("scan")) {
        std::cout << "No paths provided. Using OS default plugin directories for requested formats...\n";
        
        std::vector<std::string> dirStrings;
        for (const auto* traits : formatsToScan) {
            auto defaultDirs = traits->getDefaultPaths();
            for (const auto& d : defaultDirs) {
                // Only add if not already in list to avoid duplicates
                auto dStr = d.string();
                if (std::find(dirStrings.begin(), dirStrings.end(), dStr) == dirStrings.end()) {
                    dirStrings.push_back(dStr);
                }
            }
        }
        
        for (const auto& d : dirStrings) {
            std::cout << "  -> " << d << "\n";
        }
        
        if (!dirStrings.empty()) {
            auto found = rps::core::PluginDiscovery::findPlugins(dirStrings, formatsToScan);
            pluginsToScan.insert(pluginsToScan.end(), found.begin(), found.end());
        }
    }

    if (pluginsToScan.empty()) {
        std::cerr << "No plugins found to scan. Provide --scan-dir or --scan, or ensure plugins exist in standard OS locations.\n";
        return 1;
    }

    // Apply filter if provided
    if (vm.count("filter")) {
        std::string filterStr = vm["filter"].as<std::string>();
        std::vector<fs::path> filtered;
        for (const auto& p : pluginsToScan) {
            std::string filename = p.filename().string();
            // Case-insensitive substring match
            auto it = std::search(
                filename.begin(), filename.end(),
                filterStr.begin(), filterStr.end(),
                [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
            );
            if (it != filename.end()) {
                filtered.push_back(p);
            }
        }
        pluginsToScan = std::move(filtered);
        std::cout << "Applied filter '" << filterStr << "': " << pluginsToScan.size() << " plugins match.\n";
    }

    // Apply limit if provided
    size_t limit = vm["limit"].as<size_t>();
    if (limit > 0 && pluginsToScan.size() > limit) {
        std::cout << "Limiting scan to " << limit << " plugins (out of " << pluginsToScan.size() << ").\n";
        pluginsToScan.resize(limit);
    }

    std::string scannerBin = vm["scanner-bin"].as<std::string>();
    
    // Resolve scanner binary path relative to the orchestrator executable if it's just a filename
    fs::path scannerPath(scannerBin);
    if (!scannerPath.is_absolute()) {
        fs::path exePath = boost::dll::program_location();
        scannerPath = exePath.parent_path() / scannerPath;
        if (!fs::exists(scannerPath)) {
            // Fallback for CMake build directory structure if running from root
            scannerPath = exePath.parent_path().parent_path() / "rps-pluginscanner" / scannerPath.filename();
        }
    }
    
    int timeoutMs = vm["timeout"].as<int>();
    size_t numWorkers = vm["jobs"].as<size_t>();
    size_t maxRetries = vm["retries"].as<size_t>();

    if (numWorkers == 0) numWorkers = 1;

    std::cout << "RPS Plugin Scan Orchestrator starting...\n";
    std::cout << "Discovered " << pluginsToScan.size() << " plugins.\n";
    std::cout << "Using scanner binary: " << scannerPath.string() << "\n";

    size_t totalPlugins = pluginsToScan.size();
    std::vector<rps::orchestrator::ScanJob> jobs;
    for (size_t i = 0; i < pluginsToScan.size(); ++i) {
        jobs.push_back({ pluginsToScan[i], scannerPath.string(), timeoutMs, verbose, i, totalPlugins, maxRetries, 0 });
    }

    rps::orchestrator::db::DatabaseManager db(vm["db"].as<std::string>());
    db.initializeSchema();

    rps::orchestrator::ConsoleScanObserver observer(verbose);
    rps::orchestrator::ProcessPool pool(numWorkers, &db, &observer);
    
    auto startTime = std::chrono::steady_clock::now();
    std::cout << "Starting process pool with " << numWorkers << " workers (timeout: " << timeoutMs << "ms, retries: " << maxRetries << ")...\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "Output database: " << vm["db"].as<std::string>() << "\n";

    pool.runJobs(jobs);

    auto endTime = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    auto s = pool.stats();
    observer.onScanCompleted(s.success, s.fail, s.crash, s.timeout, elapsedMs, pool.failures());
    return 0;
}

