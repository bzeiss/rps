#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <rps/core/PluginDiscovery.hpp>
#include <rps/core/FormatTraits.hpp>
#include <rps/orchestrator/ProcessPool.hpp>
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
        ("timeout,t", po::value<int>()->default_value(10000), "Timeout in milliseconds for the scanner to respond")
        ("jobs,j", po::value<size_t>()->default_value(std::thread::hardware_concurrency()), "Number of parallel workers")
        ("formats,f", po::value<std::string>()->default_value("all"), "Comma-separated list of formats to scan (e.g. vst3,clap) or 'all'")
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

    if (numWorkers == 0) numWorkers = 1;

    std::cout << "RPS Plugin Scan Orchestrator starting...\n";
    std::cout << "Discovered " << pluginsToScan.size() << " plugins.\n";
    std::cout << "Using scanner binary: " << scannerPath.string() << "\n";
    std::cout << "Starting process pool with " << numWorkers << " workers (timeout: " << timeoutMs << "ms)...\n";
    std::cout << "--------------------------------------------------------\n";

    std::vector<rps::orchestrator::ScanJob> jobs;
    for (const auto& p : pluginsToScan) {
        jobs.push_back({ p, scannerPath.string(), timeoutMs });
    }

    std::string dbPath = vm["db"].as<std::string>();
    std::cout << "Output database: " << dbPath << "\n";

    rps::orchestrator::db::DatabaseManager db(dbPath);
    db.initializeSchema();

    rps::orchestrator::ProcessPool pool(numWorkers, &db);
    pool.runJobs(jobs);

    std::cout << "--------------------------------------------------------\n";
    std::cout << "Orchestrator shutdown complete.\n";
    return 0;
}

