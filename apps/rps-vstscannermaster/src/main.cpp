#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstring>
#include <chrono>
#include <mutex>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <rps/engine/ScanEngine.hpp>
#include <rps/engine/ScanObserver.hpp>
#include <rps/engine/db/DatabaseManager.hpp>
#include <rps/core/FormatTraits.hpp>
#include <rps/core/PluginDiscovery.hpp>
#include <sqlite3.h>
#include <algorithm>
#include <set>

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Dual-output helper: writes to both an ostream (stdout) and a log stream.
// When showProgress is false, only the log stream receives output.
// ---------------------------------------------------------------------------
class DualOut {
public:
    DualOut(bool showProgress, std::ostream& log)
        : m_show(showProgress), m_log(log) {}

    template <typename T>
    DualOut& operator<<(const T& v) {
        if (m_show) std::cout << v;
        m_log << v;
        return *this;
    }
    // Handle manipulators like std::endl
    DualOut& operator<<(std::ostream& (*manip)(std::ostream&)) {
        if (m_show) manip(std::cout);
        manip(m_log);
        return *this;
    }

private:
    bool m_show;
    std::ostream& m_log;
};

// ---------------------------------------------------------------------------
// Progress observer — Steinberg-compatible output
// ---------------------------------------------------------------------------
class ProgressObserver : public rps::engine::ScanObserver {
public:
    ProgressObserver(bool showProgress, std::ostream& logStream,
                     size_t totalAllPlugins, size_t cachedOffset)
        : m_out(showProgress, logStream)
        , m_log(logStream)
        , m_totalAll(totalAllPlugins)
        , m_offset(cachedOffset) {}

    void onScanStarted(size_t /*totalPlugins*/, size_t /*workerCount*/) override {}

    void onPluginStarted(size_t /*workerId*/, size_t pluginIndex, size_t /*totalPlugins*/,
                          const std::string& pluginPath) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t displayIdx = m_offset + pluginIndex + 1;
        auto name = fs::path(pluginPath).stem().string();
        m_out << "Scanning: " << name << " (" << displayIdx << "/" << m_totalAll << ")" << std::endl;
    }

    void onPluginProgress(size_t, const std::string&, int, const std::string&) override {}
    void onPluginSlowWarning(size_t /*workerId*/, const std::string& pluginPath, int64_t elapsedMs) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto name = fs::path(pluginPath).stem().string();
        m_log << "  SLOW: " << name << " still scanning after " << (elapsedMs / 1000) << "s" << std::endl;
    }

    void onPluginCompleted(size_t /*workerId*/, const std::string& pluginPath,
                            rps::engine::ScanOutcome outcome, int64_t elapsedMs,
                            const rps::ipc::ScanResult* result,
                            const std::string* errorMessage) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Use result->name if available, otherwise path stem
        std::string name = (result && !result->name.empty())
                           ? result->name
                           : fs::path(pluginPath).stem().string();

        switch (outcome) {
            case rps::engine::ScanOutcome::Success:
                m_out << "Scanning: " << name << " OK (" << elapsedMs << " ms)" << std::endl;
                if (elapsedMs >= 3000) {
                    m_slowPlugins.push_back({elapsedMs, resolveDisplayPath(pluginPath)});
                }
                break;
            case rps::engine::ScanOutcome::Fail:
                m_out << "Scanning: " << name << " FAILED" << std::endl;
                if (errorMessage) m_log << "  Detail: " << *errorMessage << std::endl;
                break;
            case rps::engine::ScanOutcome::Crash:
                m_out << "Scanning: " << name << " CRASHED" << std::endl;
                if (errorMessage) m_log << "  Detail: " << *errorMessage << std::endl;
                m_blockedPaths.push_back(resolveDisplayPath(pluginPath));
                break;
            case rps::engine::ScanOutcome::Timeout:
                m_out << "Scanning: " << name << " TIMEOUT" << std::endl;
                if (errorMessage) m_log << "  Detail: " << *errorMessage << std::endl;
                m_blockedPaths.push_back(resolveDisplayPath(pluginPath));
                break;
            case rps::engine::ScanOutcome::Skipped:
                break;
        }
    }

    void onWorkerStderrLine(size_t, const std::string&, const std::string&) override {}
    void onWorkerStderrDump(size_t /*workerId*/, const std::string& pluginPath,
                            const std::vector<std::string>& lines) override {
        if (lines.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto name = fs::path(pluginPath).stem().string();
        m_log << "  stderr [" << name << "]:" << std::endl;
        for (const auto& line : lines)
            m_log << "    " << line << std::endl;
    }
    void onWorkerStdoutLine(size_t, const std::string&, const std::string&) override {}
    void onWorkerStdoutDump(size_t /*workerId*/, const std::string& pluginPath,
                            const std::vector<std::string>& lines) override {
        if (lines.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto name = fs::path(pluginPath).stem().string();
        m_log << "  stdout [" << name << "]:" << std::endl;
        for (const auto& line : lines)
            m_log << "    " << line << std::endl;
    }
    void onWorkerForceKill(size_t /*workerId*/, const std::string& pluginPath) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto name = fs::path(pluginPath).stem().string();
        m_log << "  FORCE KILL: " << name << " did not respond to terminate, using TerminateProcess" << std::endl;
    }
    void onPluginRetry(size_t /*workerId*/, const std::string& pluginPath,
                       size_t attempt, size_t /*maxAttempts*/,
                       const std::string& reason) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto name = fs::path(pluginPath).stem().string();
        m_log << "  Retry #" << attempt << " for " << name << ": " << reason << std::endl;
    }
    void onMonitorReport(const std::vector<std::pair<size_t, std::pair<std::string, int64_t>>>& report) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [workerId, info] : report) {
            auto name = fs::path(info.first).stem().string();
            m_log << "  [Worker #" << workerId << "] " << name
                  << " still scanning (" << (info.second / 1000) << "s)" << std::endl;
        }
    }
    void onScanCompleted(size_t, size_t, size_t, size_t, size_t, int64_t,
                          const std::vector<std::pair<std::string, std::string>>&) override {}

    const std::vector<std::string>& blockedPaths() const { return m_blockedPaths; }
    const std::vector<std::pair<int64_t, std::string>>& slowPlugins() const { return m_slowPlugins; }

private:
    static std::string resolveDisplayPath(const std::string& pluginPath) {
        fs::path p(pluginPath);
        fs::path resolved = p;
        if (fs::is_directory(p)) {
#ifdef _WIN32
            fs::path candidate = p / "Contents" / "x86_64-win" / (p.stem().string() + ".vst3");
            if (fs::exists(candidate)) resolved = candidate;
#elif defined(__APPLE__)
            fs::path candidate = p / "Contents" / "MacOS" / p.stem().string();
            if (fs::exists(candidate)) resolved = candidate;
#else
            fs::path candidate = p / "Contents" / "x86_64-linux" / (p.stem().string() + ".so");
            if (fs::exists(candidate)) resolved = candidate;
#endif
        }
        std::string result = resolved.string();
        for (auto& c : result) { if (c == '\\') c = '/'; }
        return result;
    }

    DualOut m_out;
    std::ostream& m_log;
    size_t m_totalAll;
    size_t m_offset;
    std::mutex m_mutex;
    std::vector<std::string> m_blockedPaths;
    std::vector<std::pair<int64_t, std::string>> m_slowPlugins; // ms, display path
};

// ---------------------------------------------------------------------------
// XML helper — escape special characters for XML content
// ---------------------------------------------------------------------------
static std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c; break;
        }
    }
    return out;
}

static std::string xmlElement(const std::string& tag, const std::string& value) {
    if (value.empty()) return "<" + tag + " />";
    return "<" + tag + ">" + xmlEscape(value) + "</" + tag + ">";
}

// ---------------------------------------------------------------------------
// Resolve plugin path to the inner binary (for bundles) and use forward slashes
// Original vstscannermaster uses paths like:
//   C:/Program Files/Common Files/VST3/Foo.vst3/Contents/x86_64-win/Foo.vst3
// ---------------------------------------------------------------------------
static std::string resolvePluginDisplayPath(const std::string& pluginPath) {
    fs::path p(pluginPath);
    fs::path resolved = p;

    // If it's a bundle directory, resolve to the inner binary
    if (fs::is_directory(p)) {
#ifdef _WIN32
        fs::path candidate = p / "Contents" / "x86_64-win" / (p.stem().string() + ".vst3");
        if (fs::exists(candidate)) resolved = candidate;
#elif defined(__APPLE__)
        fs::path candidate = p / "Contents" / "MacOS" / p.stem().string();
        if (fs::exists(candidate)) resolved = candidate;
#else
        fs::path candidate = p / "Contents" / "x86_64-linux" / (p.stem().string() + ".so");
        if (fs::exists(candidate)) resolved = candidate;
#endif
    }

    // Convert to forward slashes (Steinberg convention)
    std::string result = resolved.string();
    for (auto& c : result) {
        if (c == '\\') c = '/';
    }
    return result;
}

// ---------------------------------------------------------------------------
// Get executable timestamp as string (platform-specific)
// On Windows: FILETIME (100-ns intervals since 1601-01-01)
// On other platforms: seconds since epoch
// ---------------------------------------------------------------------------
static std::string getExecutableTimestamp(const fs::path& pluginPath) {
    fs::path execPath = pluginPath;
    if (fs::is_directory(pluginPath)) {
#ifdef _WIN32
        fs::path candidate = pluginPath / "Contents" / "x86_64-win" / (pluginPath.stem().string() + ".vst3");
        if (fs::exists(candidate)) execPath = candidate;
#elif defined(__APPLE__)
        fs::path candidate = pluginPath / "Contents" / "MacOS" / pluginPath.stem().string();
        if (fs::exists(candidate)) execPath = candidate;
#else
        fs::path candidate = pluginPath / "Contents" / "x86_64-linux" / (pluginPath.stem().string() + ".so");
        if (fs::exists(candidate)) execPath = candidate;
#endif
    }
    if (!fs::exists(execPath) || !fs::is_regular_file(execPath)) {
        execPath = pluginPath;
    }

#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(execPath.wstring().c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER uli;
        uli.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        uli.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        return std::to_string(uli.QuadPart);
    }
#endif
    try {
        auto mtime = fs::last_write_time(execPath);
        return std::to_string(static_cast<int64_t>(mtime));
    } catch (...) {}
    return "0";
}

// ---------------------------------------------------------------------------
// Get architecture flag
// ---------------------------------------------------------------------------
static int getArchitectureFlag() {
#ifdef _WIN32
#if defined(_M_X64) || defined(__x86_64__)
    return 2; // x86_64
#elif defined(_M_ARM64) || defined(__aarch64__)
    return 4; // ARM64
#else
    return 1; // x86
#endif
#elif defined(__APPLE__)
#ifdef __aarch64__
    return 4; // ARM64
#else
    return 2; // x86_64
#endif
#else
    return 2; // x86_64 Linux default
#endif
}

// ---------------------------------------------------------------------------
// Data structures for DB query results
// ---------------------------------------------------------------------------
struct Vst3ClassInfo {
    int64_t dbId = 0;
    int classIndex = 0;
    std::string name;
    std::string uid;
    std::string category;       // sub-categories (pipe-delimited)
    std::string classCategory;  // "Audio Module Class", "Component Controller Class", etc.
    int64_t cardinality = 0;
    int64_t classFlags = 0;
    std::string subCategories;
    std::string sdkVersion;
    std::string vendor;
    std::string version;
    std::vector<std::string> compatUids; // old UIDs for compatibility
};

struct Vst3PluginInfo {
    int64_t pluginId = 0;
    std::string path;
    std::string vendor;
    std::string url;
    std::string email;   // from extraData vst3_factory_email (stored in plugins table desc/extraData)
    int32_t flags = 0;
    std::vector<Vst3ClassInfo> classes;
};

// ---------------------------------------------------------------------------
// Read successful VST3 plugins + classes from DB
// ---------------------------------------------------------------------------
static std::vector<Vst3PluginInfo> readPluginsFromDb(const std::string& dbPath) {
    std::vector<Vst3PluginInfo> plugins;

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        std::cerr << "Failed to open database: " << dbPath << std::endl;
        return plugins;
    }

    // Read successful VST3 plugins
    sqlite3_stmt* stmt = nullptr;
    const std::string selectPlugins = R"(
        SELECT id, path, vendor, url, factory_email, factory_flags
        FROM plugins
        WHERE format = 'vst3' AND status = 'SUCCESS'
        ORDER BY path
    )";

    if (sqlite3_prepare_v2(db, selectPlugins.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Vst3PluginInfo info;
            info.pluginId = sqlite3_column_int64(stmt, 0);
            info.path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            info.vendor = sqlite3_column_text(stmt, 2) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) : "";
            info.url = sqlite3_column_text(stmt, 3) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "";
            info.email = sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "";
            info.flags = sqlite3_column_int(stmt, 5);
            plugins.push_back(info);
        }
        sqlite3_finalize(stmt);
    }

    // Read classes for each plugin
    const std::string selectClasses = R"(
        SELECT id, class_index, name, uid, category, class_category,
               cardinality, class_flags, sub_categories, sdk_version, vendor, version
        FROM vst3_classes
        WHERE plugin_id = ?
        ORDER BY class_index
    )";

    // Resolve compat UIDs by matching new_uid to the class's CID.
    // The Plugin Compatibility Class declares compat entries with (new_uid, old_uid),
    // but Steinberg's XML puts them under the Audio Module Class whose CID matches new_uid.
    const std::string selectCompat = R"(
        SELECT old_uid FROM vst3_compat_uids
        WHERE class_id IN (SELECT id FROM vst3_classes WHERE plugin_id = ?)
          AND new_uid = ?
    )";

    for (auto& plugin : plugins) {
        if (sqlite3_prepare_v2(db, selectClasses.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, plugin.pluginId);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                Vst3ClassInfo cls;
                cls.dbId = sqlite3_column_int64(stmt, 0);
                cls.classIndex = sqlite3_column_int(stmt, 1);
                cls.name = sqlite3_column_text(stmt, 2) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)) : "";
                cls.uid = sqlite3_column_text(stmt, 3) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)) : "";
                cls.category = sqlite3_column_text(stmt, 4) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)) : "";
                cls.classCategory = sqlite3_column_text(stmt, 5) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)) : "";
                cls.cardinality = sqlite3_column_int64(stmt, 6);
                cls.classFlags = sqlite3_column_int64(stmt, 7);
                cls.subCategories = sqlite3_column_text(stmt, 8) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)) : "";
                cls.sdkVersion = sqlite3_column_text(stmt, 9) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)) : "";
                cls.vendor = sqlite3_column_text(stmt, 10) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10)) : "";
                cls.version = sqlite3_column_text(stmt, 11) ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11)) : "";
                plugin.classes.push_back(cls);
            }
            sqlite3_finalize(stmt);
        }

        // Read compat UIDs for each class — match by CID (new_uid) not by class_id
        for (auto& cls : plugin.classes) {
            if (sqlite3_prepare_v2(db, selectCompat.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, plugin.pluginId);
                sqlite3_bind_text(stmt, 2, cls.uid.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (sqlite3_column_text(stmt, 0))
                        cls.compatUids.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
                }
                sqlite3_finalize(stmt);
            }
        }

    }

    sqlite3_close(db);
    return plugins;
}

// ---------------------------------------------------------------------------
// Read blocked plugin paths from DB
// ---------------------------------------------------------------------------
static std::vector<std::string> readBlockedFromDb(const std::string& dbPath) {
    std::vector<std::string> paths;

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) return paths;

    sqlite3_stmt* stmt = nullptr;
    const std::string sql = "SELECT path FROM plugins_blocked WHERE format = 'vst3' ORDER BY path";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sqlite3_column_text(stmt, 0)) {
                std::string p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                for (auto& c : p) { if (c == '\\') c = '/'; }
                paths.push_back(p);
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return paths;
}

// ---------------------------------------------------------------------------
// Write vst3plugins.xml
// ---------------------------------------------------------------------------
static void writePluginsXml(const fs::path& outputPath, const std::vector<Vst3PluginInfo>& plugins) {
    std::ofstream ofs(outputPath.string(), std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to write: " << outputPath << std::endl;
        return;
    }

    int archFlag = getArchitectureFlag();

    ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ofs << "<plugins>";

    for (auto& plugin : plugins) {
        ofs << "<plugin>";
        std::string displayPath = resolvePluginDisplayPath(plugin.path);
        ofs << xmlElement("path", displayPath);
        ofs << xmlElement("vendor", plugin.vendor);
        ofs << xmlElement("url", plugin.url);
        ofs << xmlElement("email", plugin.email);
        ofs << "<flags>" << plugin.flags << "</flags>";
        ofs << "<codesigned>false</codesigned>";
        ofs << "<architectures>" << archFlag << "</architectures>";

        std::string timestamp = getExecutableTimestamp(fs::path(plugin.path));
        ofs << "<timestamps>";
        ofs << "<executable>" << timestamp << "</executable>";
        ofs << "</timestamps>";

        for (auto& cls : plugin.classes) {
            ofs << "<class>";
            ofs << xmlElement("cid", cls.uid);
            ofs << "<cardinality>" << cls.cardinality << "</cardinality>";
            ofs << xmlElement("category", cls.classCategory);
            ofs << xmlElement("name", cls.name);
            ofs << "<classFlags>" << cls.classFlags << "</classFlags>";
            ofs << xmlElement("subCategories", cls.subCategories);
            ofs << xmlElement("vendor", cls.vendor);
            ofs << xmlElement("version", cls.version);
            ofs << xmlElement("sdkVersion", cls.sdkVersion);

            // Only add compatibility UIDs to the Audio Module Class
            if (cls.classCategory == "Audio Module Class" && !cls.compatUids.empty()) {
                ofs << "<compatibility>";
                for (auto& uid : cls.compatUids) {
                    ofs << "<compatUID>" << xmlEscape(uid) << "</compatUID>";
                }
                ofs << "</compatibility>";
            }

            ofs << "</class>";
        }

        ofs << "</plugin>";
    }

    ofs << "</plugins>";
}

// ---------------------------------------------------------------------------
// Write vst3blocklist.xml
// ---------------------------------------------------------------------------
static void writeBlocklistXml(const fs::path& outputPath, const std::vector<std::string>& blockedPaths) {
    std::ofstream ofs(outputPath.string(), std::ios::binary);
    if (!ofs) {
        std::cerr << "Failed to write: " << outputPath << std::endl;
        return;
    }

    ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ofs << "<plugins>";
    for (auto& path : blockedPaths) {
        ofs << "<plugin>";
        ofs << "<path>" << xmlEscape(path) << "</path>";
        ofs << "</plugin>";
    }
    ofs << "</plugins>";
}

// ---------------------------------------------------------------------------
// Write vst3allowlist.xml (preserve existing or create empty)
// ---------------------------------------------------------------------------
static void writeAllowlistXml(const fs::path& outputPath) {
    if (fs::exists(outputPath)) return; // Preserve user-managed allowlist
    std::ofstream ofs(outputPath.string(), std::ios::binary);
    if (!ofs) return;
    ofs << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    ofs << "<pluginpaths />";
}

// ---------------------------------------------------------------------------
// Write cacheVersion
// ---------------------------------------------------------------------------
static void writeCacheVersion(const fs::path& outputPath) {
    std::ofstream ofs(outputPath.string(), std::ios::binary);
    if (!ofs) return;
    const char data[] = { 0x04, 0x00, 0x00, 0x00 };
    ofs.write(data, sizeof(data));
}

// ---------------------------------------------------------------------------
// Read cached plugin names from DB (path -> name) for CACHED display
// ---------------------------------------------------------------------------
static std::map<std::string, std::string> readCachedPluginNames(const std::string& dbPath) {
    std::map<std::string, std::string> names;
    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) return names;

    sqlite3_stmt* stmt = nullptr;
    const std::string sql = "SELECT path, name FROM plugins WHERE format = 'vst3' AND status = 'SUCCESS'";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            auto* path = sqlite3_column_text(stmt, 0);
            auto* name = sqlite3_column_text(stmt, 1);
            if (path && name)
                names[reinterpret_cast<const char*>(path)] = reinterpret_cast<const char*>(name);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return names;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // vstscannermaster uses single-dash arguments (e.g. -prefPath, -rescan).
    // boost::program_options by default uses -- for long options.
    // We configure it to accept both single and double dash.
    namespace po = boost::program_options;

    po::options_description desc("vstscannermaster options");
    desc.add_options()
        ("help,h", "Show help")
        ("prefPath", po::value<std::string>(), "Cache output directory (required)")
        ("licenceLevel", po::value<int>()->default_value(0), "Licence level (accepted but ignored)")
        ("licenseLevel", po::value<int>(), "Alias for licenceLevel")
        ("hostName", po::value<std::string>()->default_value(""), "Host name")
        ("progress", "Show progress output")
        ("rescan", "Force full rescan")
        ("timeout", po::value<int>()->default_value(120), "Per-plugin timeout in seconds")
        ("recheckPath", po::value<std::string>(), "Rescan a single plugin path")
        ("scanner-bin", po::value<std::string>()->default_value(
#ifdef _WIN32
            "rps-pluginscanner.exe"
#else
            "rps-pluginscanner"
#endif
        ), "Path to the scanner binary")
        ("jobs,j", po::value<size_t>()->default_value(6), "Parallel scanner workers")
        ("verbose,v", "Verbose output");

    // Allow single-dash long options (e.g. -prefPath instead of --prefPath)
    po::command_line_parser parser(argc, argv);
    parser.options(desc);
    parser.style(po::command_line_style::default_style | po::command_line_style::allow_long_disguise);

    po::variables_map vm;
    try {
        po::store(parser.run(), vm);
        po::notify(vm);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    if (vm.count("help")) {
        std::cout << "Usage: vstscannermaster -prefPath <dir> [options]\n\n" << desc << std::endl;
        return 0;
    }

    if (!vm.count("prefPath")) {
        std::cerr << "Error: -prefPath is required.\n";
        std::cerr << "Usage: vstscannermaster -prefPath <dir> [options]\n";
        return 1;
    }

    std::string prefPath = vm["prefPath"].as<std::string>();
    bool showProgress = vm.count("progress") > 0;
    bool rescan = vm.count("rescan") > 0;
    bool verbose = vm.count("verbose") > 0;
    int timeoutSecs = vm["timeout"].as<int>();
    if (timeoutSecs <= 0) timeoutSecs = 120; // Cubase passes 0 (no timeout) — enforce 120s default
    size_t jobs = vm["jobs"].as<size_t>();
    if (jobs == 0) jobs = 1;

    // Ensure output directory exists
    fs::path outputDir(prefPath);
    if (!fs::exists(outputDir)) {
        fs::create_directories(outputDir);
    }

    fs::path dbPath = outputDir / "rps-cache.db";
    fs::path logPath = outputDir / "vstscannermaster.log";

    // Open the log file early so the observer can write to it
    std::ofstream logFile(logPath.string());

#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    DWORD outMode = 0, errMode = 0;
    if (hOut != INVALID_HANDLE_VALUE) { GetConsoleMode(hOut, &outMode); SetConsoleMode(hOut, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
    if (hErr != INVALID_HANDLE_VALUE) { GetConsoleMode(hErr, &errMode); SetConsoleMode(hErr, errMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }

    // Create a Job Object that kills all child processes when this process exits.
    // On Ctrl+C (or any abnormal exit), the OS closes our handle to the job object,
    // triggering JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE which terminates all children.
    // On Windows 8+, child processes automatically inherit the parent's job.
    HANDLE hJob = CreateJobObject(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        AssignProcessToJobObject(hJob, GetCurrentProcess());
        // Do NOT close hJob — it must stay open for the process lifetime.
    }
#else
    // On POSIX, Ctrl+C sends SIGINT to the entire foreground process group,
    // which includes children. No extra setup needed.
#endif

    auto overallStart = std::chrono::steady_clock::now();

    // --- Discover all VST3 plugins (same logic ScanEngine uses) ---
    rps::core::FormatRegistry formatRegistry;
    auto vst3Traits = formatRegistry.getTraits("vst3");
    std::vector<const rps::core::IFormatTraits*> vst3Only = { vst3Traits };

    std::vector<std::string> dirStrings;
    auto defaultDirs = vst3Traits->getDefaultPaths();
    for (const auto& d : defaultDirs) dirStrings.push_back(d.string());

    auto allDiscovered = rps::core::PluginDiscovery::findPlugins(dirStrings, vst3Only);
    std::sort(allDiscovered.begin(), allDiscovered.end());
    size_t totalPlugins = allDiscovered.size();

    // --- Determine cached vs needs-scanning ---
    // Read DB cache to identify unchanged plugins (incremental mode only)
    std::map<std::string, std::string> cachedNames; // path -> name
    std::vector<fs::path> cachedPaths;
    std::set<std::string> cachedSet;

    if (!rescan && fs::exists(dbPath)) {
        rps::engine::db::DatabaseManager tmpDb(dbPath);
        tmpDb.initializeSchema();
        auto cache = tmpDb.loadPluginCache({"vst3"});
        auto skippedCache = tmpDb.loadSkippedCache({"vst3"});
        auto blockedCache = tmpDb.loadBlockedCache({"vst3"});
        auto nameMap = readCachedPluginNames(dbPath.string());

        for (const auto& p : allDiscovered) {
            std::string pathStr = p.string();
            std::string currentMtime = rps::engine::db::DatabaseManager::getFileMtime(p);

            // Check success cache
            auto it = cache.find(pathStr);
            if (it != cache.end() && currentMtime == it->second.fileMtime) {
                cachedSet.insert(pathStr);
                cachedPaths.push_back(p);
                auto nameIt = nameMap.find(pathStr);
                cachedNames[pathStr] = (nameIt != nameMap.end()) ? nameIt->second : p.stem().string();
                continue;
            }
            // Check skipped cache
            auto skipIt = skippedCache.find(pathStr);
            if (skipIt != skippedCache.end() && currentMtime == skipIt->second) {
                cachedSet.insert(pathStr);
                cachedPaths.push_back(p);
                cachedNames[pathStr] = p.stem().string();
                continue;
            }
            // Check blocked cache
            auto blockIt = blockedCache.find(pathStr);
            if (blockIt != blockedCache.end() && currentMtime == blockIt->second) {
                cachedSet.insert(pathStr);
                cachedPaths.push_back(p);
                cachedNames[pathStr] = p.stem().string();
                continue;
            }
        }
    }

    // DualOut writes to both stdout (if -progress) and log file
    DualOut out(showProgress, logFile);

    out << "Initialising" << std::endl;
    out << "Starting scan of " << totalPlugins << " plugins" << std::endl;

    // --- Phase 1: Show cached plugins ---
    size_t cachedCount = cachedPaths.size();
    size_t displayIdx = 0;
    for (const auto& p : cachedPaths) {
        displayIdx++;
        std::string name = cachedNames[p.string()];
        out << "Scanning: " << name << " (" << displayIdx << "/" << totalPlugins << ")" << std::endl;
        out << "Scanning: " << name << " OK (CACHED)" << std::endl;
    }

    // --- Phase 2: Run ScanEngine for non-cached plugins ---
    rps::engine::ScanConfig config;
    config.formats = "vst3";
    config.mode = rescan ? "full" : "incremental";
    config.dbPath = dbPath.string();
    config.timeoutMs = timeoutSecs * 1000;
    config.jobs = jobs;
    config.verbose = verbose;
    config.scannerBin = vm["scanner-bin"].as<std::string>();

    if (vm.count("recheckPath")) {
        config.singlePlugin = vm["recheckPath"].as<std::string>();
    }

    ProgressObserver observer(showProgress, logFile, totalPlugins, cachedCount);
    rps::engine::ScanEngine engine;
    auto summary = engine.runScan(config, &observer);

    auto overallEnd = std::chrono::steady_clock::now();
    int64_t overallMs = std::chrono::duration_cast<std::chrono::milliseconds>(overallEnd - overallStart).count();
    int64_t avgMs = totalPlugins > 0 ? overallMs / static_cast<int64_t>(totalPlugins) : 0;

    // --- Phase 3: Write cache files ---
    out << "Updating cache files" << std::endl;

    auto plugins = readPluginsFromDb(dbPath.string());
    auto blockedFromDb = readBlockedFromDb(dbPath.string());

    // Merge observer's blocked paths with DB blocked paths
    auto& observerBlocked = observer.blockedPaths();
    std::vector<std::string> allBlocked = blockedFromDb;
    for (auto& p : observerBlocked) {
        bool found = false;
        for (auto& existing : allBlocked) {
            if (existing == p) { found = true; break; }
        }
        if (!found) allBlocked.push_back(p);
    }

    writePluginsXml(outputDir / "vst3plugins.xml", plugins);
    writeBlocklistXml(outputDir / "vst3blocklist.xml", allBlocked);
    writeAllowlistXml(outputDir / "vst3allowlist.xml");
    writeCacheVersion(outputDir / "cacheVersion");

    // --- Phase 4: Summary ---
    out << "Finished " << totalPlugins << " plugins, (" << overallMs << " ms - average " << avgMs << " ms)" << std::endl;
    out << "vstscannermaster OK" << std::endl;

    // Log file ends here (mirrors -progress output up to "vstscannermaster OK")
    logFile.flush();

    // --- Phase 5: Blocked + slow plugins (stdout only, not in log) ---
    if (showProgress) {
        if (!allBlocked.empty()) {
            std::cout << std::endl;
            std::cout << "Blocked plugins:" << std::endl;
            for (auto& bp : allBlocked) {
                std::cout << " - " << bp << std::endl;
            }
        }

        auto& slowPlugins = observer.slowPlugins();
        if (!slowPlugins.empty()) {
            std::cout << std::endl;
            std::cout << "Slow plugins:" << std::endl;
            // Sort by time ascending
            auto sorted = slowPlugins;
            std::sort(sorted.begin(), sorted.end());
            for (auto& [ms, path] : sorted) {
                std::cout << " - " << ms << "ms: " << path << std::endl;
            }
        }
    }

    // --- Without -progress: dump vst3plugins.xml to stdout ---
    if (!showProgress) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        fs::path xmlPath = outputDir / "vst3plugins.xml";
        std::ifstream xmlIn(xmlPath.string(), std::ios::binary);
        if (xmlIn) {
            std::cout << xmlIn.rdbuf();
        }
    }

    return 0;
}
