#include <rps/engine/ScanEngine.hpp>
#include <rps/engine/db/DatabaseManager.hpp>
#include <rps/core/PluginDiscovery.hpp>
#include <rps/core/FormatTraits.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <iostream>
#include <set>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace rps::engine {

ScanEngine::ScanEngine() {}

bool ScanEngine::isScanning() const {
    return m_scanning.load();
}

std::vector<std::string> ScanEngine::availableFormats() const {
    rps::core::FormatRegistry reg;
    std::vector<std::string> names;
    for (const auto& t : reg.getAllTraits()) {
        names.push_back(t->getName());
    }
    return names;
}

ScanSummary ScanEngine::runScan(const ScanConfig& config, ScanObserver* observer) {
    namespace fs = boost::filesystem;

    // Prevent concurrent scans
    bool expected = false;
    if (!m_scanning.compare_exchange_strong(expected, true)) {
        return {}; // Already scanning
    }

    // RAII guard to reset m_scanning on exit
    struct ScanGuard {
        std::atomic<bool>& flag;
        ~ScanGuard() { flag.store(false); }
    } guard{m_scanning};

    ScanSummary summary;
    auto startTime = std::chrono::steady_clock::now();

    // --- Parse formats ---
    rps::core::FormatRegistry formatRegistry;
    auto formatsToScan = formatRegistry.parseFormats(config.formats);
    if (formatsToScan.empty()) {
        std::cerr << "No valid formats specified to scan.\n";
        return summary;
    }

    // --- Discover plugins ---
    std::vector<fs::path> pluginsToScan;

    if (!config.scanDirs.empty()) {
        auto found = rps::core::PluginDiscovery::findPlugins(config.scanDirs, formatsToScan);
        pluginsToScan.insert(pluginsToScan.end(), found.begin(), found.end());
    }

    if (!config.singlePlugin.empty()) {
        fs::path p(config.singlePlugin);
        if (p.string() == "CRASH_ME" || p.string() == "HANG_ME" || fs::exists(p)) {
            pluginsToScan.push_back(p);
        } else {
            std::cerr << "Warning: Single scan file does not exist: " << p << "\n";
        }
    }

    // If neither scan dirs nor single plugin, use OS default directories
    if (pluginsToScan.empty() && config.scanDirs.empty() && config.singlePlugin.empty()) {
        std::vector<std::string> dirStrings;
        for (const auto* traits : formatsToScan) {
            auto defaultDirs = traits->getDefaultPaths();
            for (const auto& d : defaultDirs) {
                auto dStr = d.string();
                if (std::find(dirStrings.begin(), dirStrings.end(), dStr) == dirStrings.end()) {
                    dirStrings.push_back(dStr);
                }
            }
        }

        if (!dirStrings.empty()) {
            auto found = rps::core::PluginDiscovery::findPlugins(dirStrings, formatsToScan);
            pluginsToScan.insert(pluginsToScan.end(), found.begin(), found.end());
        }
    }

    if (pluginsToScan.empty()) {
        std::cerr << "No plugins found to scan.\n";
        return summary;
    }

    // --- Apply filter ---
    if (!config.filter.empty()) {
        std::vector<fs::path> filtered;
        for (const auto& p : pluginsToScan) {
            std::string filename = p.filename().string();
            auto it = std::search(
                filename.begin(), filename.end(),
                config.filter.begin(), config.filter.end(),
                [](char ch1, char ch2) { return std::tolower(ch1) == std::tolower(ch2); }
            );
            if (it != filename.end()) {
                filtered.push_back(p);
            }
        }
        pluginsToScan = std::move(filtered);
    }

    // --- Apply limit ---
    if (config.limit > 0 && pluginsToScan.size() > config.limit) {
        pluginsToScan.resize(config.limit);
    }

    // --- Resolve scanner binary ---
    fs::path scannerPath(config.scannerBin);
    if (!scannerPath.is_absolute()) {
        fs::path exePath = boost::dll::program_location();
        scannerPath = exePath.parent_path() / scannerPath;
        if (!fs::exists(scannerPath)) {
            scannerPath = exePath.parent_path().parent_path() / "rps-pluginscanner" / scannerPath.filename();
        }
    }

    // --- Database setup ---
    db::DatabaseManager db(config.dbPath);
    db.initializeSchema();

    // --- Collect format names ---
    std::vector<std::string> formatNames;
    for (const auto* traits : formatsToScan) {
        formatNames.push_back(traits->getName());
    }

    // --- Scan mode logic ---
    bool singleScan = !config.singlePlugin.empty();

    if (singleScan) {
        // No DB clearing, no stale pruning, no cache comparison — always scan.
    } else if (config.mode == "full") {
        db.clearPluginsByFormats(formatNames);
    } else {
        // Incremental mode
        std::set<std::string> discoveredPaths;
        for (const auto& p : pluginsToScan) {
            discoveredPaths.insert(p.string());
        }

        size_t staleRemoved = db.removeStaleEntries(discoveredPaths, formatNames);
        (void)staleRemoved;

        auto cache = db.loadPluginCache(formatNames);
        auto skippedCache = db.loadSkippedCache(formatNames);
        auto blockedCache = db.loadBlockedCache(formatNames);
        std::vector<fs::path> filtered;

        for (const auto& p : pluginsToScan) {
            std::string pathStr = p.string();
            std::string currentMtime = db::DatabaseManager::getFileMtime(p);

            // Check if previously blocked and file unchanged
            auto blockIt = blockedCache.find(pathStr);
            if (blockIt != blockedCache.end()) {
                if (currentMtime == blockIt->second) {
                    summary.skippedBlocked++;
                    continue;
                }
            }

            // Check if previously skipped and file unchanged
            auto skipIt = skippedCache.find(pathStr);
            if (skipIt != skippedCache.end()) {
                if (currentMtime == skipIt->second) {
                    summary.skippedUnchanged++;
                    continue;
                }
            }

            auto it = cache.find(pathStr);
            if (it == cache.end()) {
                filtered.push_back(p);
                continue;
            }

            const auto& entry = it->second;
            if (currentMtime != entry.fileMtime) {
                filtered.push_back(p);
                continue;
            }

            summary.skippedUnchanged++;
        }

        pluginsToScan = std::move(filtered);

        if (pluginsToScan.empty()) {
            auto endTime = std::chrono::steady_clock::now();
            summary.totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
            if (observer) {
                observer->onScanStarted(0, config.jobs);
                observer->onScanCompleted(0, 0, 0, 0, 0, summary.totalMs, {});
            }
            return summary;
        }
    }

    // --- Build job list ---
    size_t totalPlugins = pluginsToScan.size();
    std::vector<ScanJob> jobs;
    for (size_t i = 0; i < pluginsToScan.size(); ++i) {
        std::string fmt;
        for (const auto* traits : formatsToScan) {
            if (traits->isPluginPath(pluginsToScan[i])) {
                fmt = traits->getName();
                break;
            }
        }
        jobs.push_back({ pluginsToScan[i], scannerPath.string(), config.timeoutMs, config.verbose,
                          i, totalPlugins, config.retries, 0, fmt });
    }

    // --- Notify observer ---
    if (observer) {
        observer->onScanStarted(totalPlugins, config.jobs);
    }

    // --- Run ---
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_pool = std::make_unique<ProcessPool>(config.jobs, &db, observer);
    }
    
    m_pool->runJobs(jobs);

    auto endTime = std::chrono::steady_clock::now();
    summary.totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    auto s = m_pool->stats();
    summary.success = s.success;
    summary.fail = s.fail;
    summary.crash = s.crash;
    summary.timeout = s.timeout;
    summary.skipped += s.skipped;
    summary.failures = m_pool->failures();

    if (observer) {
        observer->onScanCompleted(s.success, s.fail, s.crash, s.timeout, s.skipped,
                                   summary.totalMs, summary.failures);
    }

    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_pool.reset();
    }

    return summary;
}

void ScanEngine::stop() {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (m_pool) {
        m_pool->stop();
    }
}

} // namespace rps::engine
