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

namespace rps::orchestrator::db {

class DatabaseManager {
public:
    // Opens or creates the SQLite database at the specified path
    DatabaseManager(const boost::filesystem::path& dbPath);
    ~DatabaseManager();

    // Initialize the schema if it doesn't exist
    void initializeSchema();

    // Insert or update a plugin scan result
    void upsertPluginResult(const boost::filesystem::path& pluginPath, const rps::ipc::ScanResult& result,
                            int64_t scanTimeMs, const std::string& fileMtime = "", const std::string& fileHash = "");

    // Record a failed scan
    void recordPluginFailure(const boost::filesystem::path& pluginPath, const std::string& errorMsg,
                             int64_t scanTimeMs, const std::string& fileMtime = "", const std::string& fileHash = "");

    // --- Incremental scan support ---

    struct PluginCacheEntry {
        std::string fileMtime;
        std::string fileHash;
        std::string status;
    };

    // Load all plugin entries from DB for incremental comparison
    std::map<std::string, PluginCacheEntry> loadPluginCache();

    // Delete plugins and parameters for the specified formats only (for full scan mode)
    void clearPluginsByFormats(const std::vector<std::string>& formats);

    // Remove DB entries for plugins no longer present on disk. Returns count removed.
    size_t removeStaleEntries(const std::set<std::string>& validPaths);

    // Compute CRC32 hash of a file (hex string)
    static std::string computeFileHash(const boost::filesystem::path& filePath);

    // Get file modification time as string
    static std::string getFileMtime(const boost::filesystem::path& filePath);

private:
    sqlite3* m_db = nullptr;
    std::mutex m_dbMutex;
    
    void executeQuery(const std::string& query);
};

} // namespace rps::orchestrator::db
