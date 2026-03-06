#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <rps/ipc/Messages.hpp>
#include <boost/filesystem.hpp>

// Forward declare sqlite3
struct sqlite3;

namespace rps::engine::db {

class DatabaseManager {
public:
    // Opens or creates the SQLite database at the specified path
    DatabaseManager(const boost::filesystem::path& dbPath);
    ~DatabaseManager();

    // Initialize the schema if it doesn't exist
    void initializeSchema();

    // Get the raw SQLite3 handle (for direct queries)
    sqlite3* rawDb() { return m_db; }

    // Insert or update a plugin scan result
    void upsertPluginResult(const boost::filesystem::path& pluginPath, const rps::ipc::ScanResult& result,
                            int64_t scanTimeMs, const std::string& fileMtime = "", const std::string& fileHash = "");

    // Record a failed scan
    void recordPluginFailure(const boost::filesystem::path& pluginPath, const std::string& errorMsg,
                             int64_t scanTimeMs, const std::string& fileMtime = "", const std::string& fileHash = "");

    // Insert AAX plugin variants from extraData into aax_plugins table.
    // Called automatically by upsertPluginResult when format is "aax".
    void upsertAaxPluginVariants(int64_t pluginId, const rps::ipc::ScanResult& result);

    // Insert VST3 class entries from extraData into vst3_classes table.
    // Called automatically by upsertPluginResult when format is "vst3".
    void upsertVst3Classes(int64_t pluginId, const rps::ipc::ScanResult& result);

    // Insert AU plugin entries from extraData into au_plugins table.
    // Called automatically by upsertPluginResult when format is "au".
    void upsertAuPlugins(int64_t pluginId, const rps::ipc::ScanResult& result);

    // Record a skipped plugin (not scannable, e.g. empty bundle)
    void recordPluginSkip(const boost::filesystem::path& pluginPath, const std::string& format,
                          const std::string& reason, const std::string& fileMtime = "");

    // Record a blocked plugin (exhausted all retries or timed out)
    void recordPluginBlocked(const boost::filesystem::path& pluginPath, const std::string& format,
                             const std::string& reason, const std::string& fileMtime = "");

    // --- Incremental scan support ---

    struct PluginCacheEntry {
        std::string fileMtime;
        std::string fileHash;
        std::string status;
    };

    // Load plugin entries from DB for incremental comparison (filtered by formats)
    std::map<std::string, PluginCacheEntry> loadPluginCache(const std::vector<std::string>& formats = {});

    // Load skipped plugin paths from plugins_skipped for incremental comparison (filtered by formats)
    std::map<std::string, std::string> loadSkippedCache(const std::vector<std::string>& formats = {});

    // Load blocked plugin paths from plugins_blocked for incremental comparison (filtered by formats)
    std::map<std::string, std::string> loadBlockedCache(const std::vector<std::string>& formats = {});

    // Delete plugins and parameters for the specified formats only (for full scan mode)
    void clearPluginsByFormats(const std::vector<std::string>& formats);

    // Remove DB entries for plugins no longer present on disk (filtered by formats). Returns count removed.
    size_t removeStaleEntries(const std::set<std::string>& validPaths, const std::vector<std::string>& formats = {});

    // Compute CRC32 hash of a file (hex string)
    static std::string computeFileHash(const boost::filesystem::path& filePath);

    // Get file modification time as string
    static std::string getFileMtime(const boost::filesystem::path& filePath);

private:
    sqlite3* m_db = nullptr;
    std::mutex m_dbMutex;
    
    void executeQuery(const std::string& query);
};

} // namespace rps::engine::db
