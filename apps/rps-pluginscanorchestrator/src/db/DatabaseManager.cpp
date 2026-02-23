#include <rps/orchestrator/db/DatabaseManager.hpp>
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <boost/json.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <fstream>

namespace rps::orchestrator::db {

DatabaseManager::DatabaseManager(const boost::filesystem::path& dbPath) {
    if (sqlite3_open(dbPath.string().c_str(), &m_db) != SQLITE_OK) {
        throw std::runtime_error("Failed to open SQLite database: " + std::string(sqlite3_errmsg(m_db)));
    }
}

DatabaseManager::~DatabaseManager() {
    if (m_db) {
        sqlite3_close(m_db);
    }
}

void DatabaseManager::executeQuery(const std::string& query) {
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, query.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::string errStr = errMsg ? errMsg : "Unknown SQLite error";
        if (errMsg) sqlite3_free(errMsg);
        throw std::runtime_error("SQLite query execution failed: " + errStr + " (Query: " + query + ")");
    }
}

void DatabaseManager::initializeSchema() {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    const std::string createTablePlugins = R"(
        CREATE TABLE IF NOT EXISTS plugins (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            format TEXT,
            path TEXT UNIQUE NOT NULL,
            name TEXT,
            uid TEXT,
            vendor TEXT,
            version TEXT,
            description TEXT,
            url TEXT,
            category TEXT,
            num_inputs INTEGER DEFAULT 0,
            num_outputs INTEGER DEFAULT 0,
            status TEXT NOT NULL,
            error_message TEXT,
            scan_time_ms INTEGER DEFAULT 0,
            file_mtime TEXT,
            file_hash TEXT,
            last_scanned TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createTableParameters = R"(
        CREATE TABLE IF NOT EXISTS parameters (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plugin_id INTEGER NOT NULL,
            param_index INTEGER NOT NULL,
            name TEXT NOT NULL,
            default_value REAL,
            FOREIGN KEY (plugin_id) REFERENCES plugins(id) ON DELETE CASCADE
        );
    )";

    executeQuery(createTablePlugins);
    executeQuery(createTableParameters);

    // Migrate existing databases: add columns if they don't exist.
    // SQLite ALTER TABLE ADD COLUMN is safe if column already exists (we catch the error).
    auto tryAddColumn = [this](const std::string& sql) {
        char* errMsg = nullptr;
        sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);
        if (errMsg) sqlite3_free(errMsg); // Ignore "duplicate column" errors
    };
    tryAddColumn("ALTER TABLE plugins ADD COLUMN format TEXT;");
    tryAddColumn("ALTER TABLE plugins ADD COLUMN file_mtime TEXT;");
    tryAddColumn("ALTER TABLE plugins ADD COLUMN file_hash TEXT;");
}

void DatabaseManager::upsertPluginResult(const boost::filesystem::path& pluginPath, const rps::ipc::ScanResult& result,
                                          int64_t scanTimeMs, const std::string& fileMtime, const std::string& fileHash) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    sqlite3_stmt* stmt = nullptr;
    
    // 1. Upsert into plugins table
    const std::string upsertPluginSql = R"(
        INSERT INTO plugins (format, path, name, uid, vendor, version, description, url, category, num_inputs, num_outputs, status, error_message, scan_time_ms, file_mtime, file_hash, last_scanned)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'SUCCESS', NULL, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(path) DO UPDATE SET
            format=excluded.format,
            name=excluded.name,
            uid=excluded.uid,
            vendor=excluded.vendor,
            version=excluded.version,
            description=excluded.description,
            url=excluded.url,
            category=excluded.category,
            num_inputs=excluded.num_inputs,
            num_outputs=excluded.num_outputs,
            status=excluded.status,
            error_message=excluded.error_message,
            scan_time_ms=excluded.scan_time_ms,
            file_mtime=excluded.file_mtime,
            file_hash=excluded.file_hash,
            last_scanned=excluded.last_scanned;
    )";

    if (sqlite3_prepare_v2(m_db, upsertPluginSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare plugin upsert statement.\n";
        return;
    }

    std::string pathStr = pluginPath.string();
    sqlite3_bind_text(stmt, 1, result.format.c_str(), -1, SQLITE_TRANSIENT);     // format
    sqlite3_bind_text(stmt, 2, pathStr.c_str(), -1, SQLITE_TRANSIENT);            // path
    sqlite3_bind_text(stmt, 3, result.name.c_str(), -1, SQLITE_TRANSIENT);        // name
    sqlite3_bind_text(stmt, 4, result.uid.c_str(), -1, SQLITE_TRANSIENT);         // uid
    sqlite3_bind_text(stmt, 5, result.vendor.c_str(), -1, SQLITE_TRANSIENT);      // vendor
    sqlite3_bind_text(stmt, 6, result.version.c_str(), -1, SQLITE_TRANSIENT);     // version
    sqlite3_bind_text(stmt, 7, result.description.c_str(), -1, SQLITE_TRANSIENT); // description
    sqlite3_bind_text(stmt, 8, result.url.c_str(), -1, SQLITE_TRANSIENT);         // url
    sqlite3_bind_text(stmt, 9, result.category.c_str(), -1, SQLITE_TRANSIENT);    // category
    sqlite3_bind_int(stmt, 10, result.numInputs);                                 // num_inputs
    sqlite3_bind_int(stmt, 11, result.numOutputs);                                // num_outputs
    // 'SUCCESS' and NULL are literals in the SQL                                  // status, error_message
    sqlite3_bind_int64(stmt, 12, scanTimeMs);                                     // scan_time_ms
    sqlite3_bind_text(stmt, 13, fileMtime.c_str(), -1, SQLITE_TRANSIENT);         // file_mtime
    sqlite3_bind_text(stmt, 14, fileHash.c_str(), -1, SQLITE_TRANSIENT);          // file_hash

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to execute plugin upsert.\n";
    }
    sqlite3_finalize(stmt);

    // 2. Get the plugin ID we just inserted/updated
    sqlite3_int64 pluginId = sqlite3_last_insert_rowid(m_db);

    // If it was an update, last_insert_rowid() might not return the correct ID. 
    // Let's fetch it explicitly.
    const std::string selectIdSql = "SELECT id FROM plugins WHERE path = ?";
    if (sqlite3_prepare_v2(m_db, selectIdSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathStr.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            pluginId = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    // 3. Clear old parameters and insert new ones
    const std::string deleteParamsSql = "DELETE FROM parameters WHERE plugin_id = ?";
    if (sqlite3_prepare_v2(m_db, deleteParamsSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, pluginId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    const std::string insertParamSql = R"(
        INSERT INTO parameters (plugin_id, param_index, name, default_value)
        VALUES (?, ?, ?, ?)
    )";

    if (sqlite3_prepare_v2(m_db, insertParamSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto& param : result.parameters) {
            sqlite3_bind_int64(stmt, 1, pluginId);
            sqlite3_bind_int(stmt, 2, param.id);
            sqlite3_bind_text(stmt, 3, param.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 4, param.defaultValue);
            
            sqlite3_step(stmt);
            sqlite3_reset(stmt); // Reset for next iteration
        }
        sqlite3_finalize(stmt);
    }
}

void DatabaseManager::recordPluginFailure(const boost::filesystem::path& pluginPath, const std::string& errorMsg,
                                           int64_t scanTimeMs, const std::string& fileMtime, const std::string& fileHash) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    sqlite3_stmt* stmt = nullptr;
    
    const std::string upsertFailureSql = R"(
        INSERT INTO plugins (path, status, error_message, scan_time_ms, file_mtime, file_hash, last_scanned)
        VALUES (?, 'ERROR', ?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(path) DO UPDATE SET
            status=excluded.status,
            error_message=excluded.error_message,
            scan_time_ms=excluded.scan_time_ms,
            file_mtime=excluded.file_mtime,
            file_hash=excluded.file_hash,
            last_scanned=excluded.last_scanned;
    )";

    if (sqlite3_prepare_v2(m_db, upsertFailureSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare plugin failure statement.\n";
        return;
    }

    std::string pathStr = pluginPath.string();
    sqlite3_bind_text(stmt, 1, pathStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, errorMsg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, scanTimeMs);
    sqlite3_bind_text(stmt, 4, fileMtime.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, fileHash.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to execute plugin failure update.\n";
    }
    sqlite3_finalize(stmt);
}

std::map<std::string, DatabaseManager::PluginCacheEntry> DatabaseManager::loadPluginCache() {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    std::map<std::string, PluginCacheEntry> cache;

    const std::string sql = "SELECT path, file_mtime, file_hash, status FROM plugins";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return cache;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PluginCacheEntry entry;
        std::string path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* mt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* fh = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        entry.fileMtime = mt ? mt : "";
        entry.fileHash = fh ? fh : "";
        entry.status = st ? st : "";
        cache[path] = entry;
    }
    sqlite3_finalize(stmt);
    return cache;
}

void DatabaseManager::clearAllPlugins() {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    executeQuery("DELETE FROM parameters;");
    executeQuery("DELETE FROM plugins;");
}

size_t DatabaseManager::removeStaleEntries(const std::set<std::string>& validPaths) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    size_t removed = 0;

    // Collect all paths in DB
    std::vector<std::string> dbPaths;
    const std::string sql = "SELECT path FROM plugins";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            dbPaths.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);
    }

    for (const auto& p : dbPaths) {
        if (validPaths.find(p) == validPaths.end()) {
            // Path no longer on disk — remove
            const std::string delParams = "DELETE FROM parameters WHERE plugin_id IN (SELECT id FROM plugins WHERE path = ?)";
            if (sqlite3_prepare_v2(m_db, delParams.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            const std::string delPlugin = "DELETE FROM plugins WHERE path = ?";
            if (sqlite3_prepare_v2(m_db, delPlugin.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, p.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            removed++;
        }
    }
    return removed;
}

std::string DatabaseManager::computeFileHash(const boost::filesystem::path& filePath) {
    std::ifstream file(filePath.string(), std::ios::binary);
    if (!file.is_open()) return "";

    boost::crc_32_type crc;
    char buf[8192];
    while (file.read(buf, sizeof(buf))) {
        crc.process_bytes(buf, file.gcount());
    }
    if (file.gcount() > 0) {
        crc.process_bytes(buf, file.gcount());
    }

    std::ostringstream oss;
    oss << std::hex << std::uppercase << crc.checksum();
    return oss.str();
}

std::string DatabaseManager::getFileMtime(const boost::filesystem::path& filePath) {
    try {
        auto mtime = boost::filesystem::last_write_time(filePath);
        return std::to_string(mtime);
    } catch (...) {
        return "";
    }
}

} // namespace rps::orchestrator::db
