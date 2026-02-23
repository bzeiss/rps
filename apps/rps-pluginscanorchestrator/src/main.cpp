#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <iostream>
#include <string>
#include <vector>
#include <set>
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
        ("timeout,t", po::value<int>()->default_value(120000), "Timeout in ms per plugin (0 = no timeout, default: 2 min)")
        ("jobs,j", po::value<size_t>()->default_value(6), "Number of parallel scanner workers (default: 6)")
        ("formats,f", po::value<std::string>()->default_value("all"), "Comma-separated list of formats to scan (e.g. vst3,clap) or 'all'")
        ("filter", po::value<std::string>(), "Only scan plugins whose filename contains this string")
        ("limit,l", po::value<size_t>()->default_value(0), "Maximum number of plugins to scan (0 = unlimited)")
        ("verbose,v", "Enable verbose scanner output (plugin debug logs)")
        ("retries,r", po::value<size_t>()->default_value(3), "Number of retries for failed plugins (0 = no retries)")
        ("db", po::value<std::string>()->default_value("rps-plugins.db"), "Path to the output SQLite database file")
        ("mode,m", po::value<std::string>()->default_value("incremental"), "Scan mode: 'full' (rescan everything) or 'incremental' (skip unchanged)");
        
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

    rps::orchestrator::db::DatabaseManager db(vm["db"].as<std::string>());
    db.initializeSchema();

    // --- Scan mode: full vs incremental ---
    std::string scanMode = vm["mode"].as<std::string>();
    if (scanMode != "full" && scanMode != "incremental") {
        std::cerr << "Invalid --mode value: '" << scanMode << "'. Use 'full' or 'incremental'.\n";
        return 1;
    }

    size_t skippedUnchanged = 0;
    auto startTime = std::chrono::steady_clock::now();

    // Collect format short names (used by both full and incremental mode)
    std::vector<std::string> formatNames;
    for (const auto* traits : formatsToScan) {
        formatNames.push_back(traits->getName());
    }

    if (scanMode == "full") {
        std::string fmtList;
        for (size_t i = 0; i < formatNames.size(); ++i) {
            if (i > 0) fmtList += ", ";
            fmtList += formatNames[i];
        }
        std::cout << "Mode: FULL -- clearing " << fmtList << " entries and rescanning.\n";
        db.clearPluginsByFormats(formatNames);
    } else {
        std::cout << "Mode: INCREMENTAL -- skipping unchanged plugins.\n";

        // Build set of all discovered paths (for stale entry removal)
        std::set<std::string> discoveredPaths;
        for (const auto& p : pluginsToScan) {
            discoveredPaths.insert(p.string());
        }

        // Remove stale entries (plugins in DB but no longer on disk) -- only for scanned formats
        size_t staleRemoved = db.removeStaleEntries(discoveredPaths, formatNames);
        if (staleRemoved > 0) {
            std::cout << "Removed " << staleRemoved << " stale database entries (plugins no longer on disk).\n";
        }

        // Load cache and filter out unchanged, skipped, and blocked plugins
        auto cache = db.loadPluginCache(formatNames);
        auto skippedCache = db.loadSkippedCache(formatNames);
        auto blockedCache = db.loadBlockedCache(formatNames);
        std::vector<fs::path> filtered;
        size_t blockedCount = 0;

        for (const auto& p : pluginsToScan) {
            std::string pathStr = p.string();
            std::string currentMtime = rps::orchestrator::db::DatabaseManager::getFileMtime(p);

            // Check if previously blocked (exhausted retries) and file unchanged
            auto blockIt = blockedCache.find(pathStr);
            if (blockIt != blockedCache.end()) {
                if (currentMtime == blockIt->second) {
                    blockedCount++;
                    continue;
                }
            }

            // Check if previously skipped (and file unchanged)
            auto skipIt = skippedCache.find(pathStr);
            if (skipIt != skippedCache.end()) {
                if (currentMtime == skipIt->second) {
                    skippedUnchanged++;
                    continue;
                }
            }

            auto it = cache.find(pathStr);
            if (it == cache.end()) {
                // Not in DB → must scan
                filtered.push_back(p);
                continue;
            }

            const auto& entry = it->second;

            // Compare file modification time — mtime is sufficient for change detection
            if (currentMtime != entry.fileMtime) {
                // mtime differs → rescan
                filtered.push_back(p);
                continue;
            }

            // Unchanged — skip
            skippedUnchanged++;
        }

        if (blockedCount > 0) {
            std::cout << "Skipping " << blockedCount << " blocked plugins (previously failed all retries).\n";
        }

        if (skippedUnchanged > 0) {
            std::cout << "Skipping " << skippedUnchanged << " unchanged plugins.\n";
        }

        pluginsToScan = std::move(filtered);

        if (pluginsToScan.empty()) {
            auto endTime = std::chrono::steady_clock::now();
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            std::cout << "All plugins are up-to-date. Nothing to scan. (" << elapsedMs << "ms)\n";
            return 0;
        }
    }

    // Build job list from the (possibly filtered) plugin list
    size_t totalPlugins = pluginsToScan.size();
    std::vector<rps::orchestrator::ScanJob> jobs;
    for (size_t i = 0; i < pluginsToScan.size(); ++i) {
        // Determine format from the plugin path
        std::string fmt;
        for (const auto* traits : formatsToScan) {
            if (traits->isPluginPath(pluginsToScan[i])) {
                fmt = traits->getName();
                break;
            }
        }
        jobs.push_back({ pluginsToScan[i], scannerPath.string(), timeoutMs, verbose, i, totalPlugins, maxRetries, 0, fmt });
    }

    rps::orchestrator::ConsoleScanObserver observer(verbose);
    rps::orchestrator::ProcessPool pool(numWorkers, &db, &observer);
    
    std::cout << "Scanning " << pluginsToScan.size() << " plugin(s).\n";
    std::cout << "Starting process pool with " << numWorkers << " workers (timeout: " << timeoutMs << "ms, retries: " << maxRetries << ")...\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "Output database: " << vm["db"].as<std::string>() << "\n";

    pool.runJobs(jobs);

    auto endTime = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    auto s = pool.stats();
    observer.onScanCompleted(s.success, s.fail, s.crash, s.timeout, s.skipped, elapsedMs, pool.failures());
    return 0;
}

