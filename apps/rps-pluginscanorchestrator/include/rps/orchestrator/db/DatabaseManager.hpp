#pragma once

#include <string>
#include <vector>
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
    void upsertPluginResult(const boost::filesystem::path& pluginPath, const rps::ipc::ScanResult& result, int64_t scanTimeMs);

    // Record a failed scan
    void recordPluginFailure(const boost::filesystem::path& pluginPath, const std::string& errorMsg, int64_t scanTimeMs);

private:
    sqlite3* m_db = nullptr;
    std::mutex m_dbMutex; // SQLite is thread-safe if compiled so, but better to enforce serialization on writes
    
    void executeQuery(const std::string& query);
};

} // namespace rps::orchestrator::db
