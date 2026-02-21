#include <rps/orchestrator/db/DatabaseManager.hpp>
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <boost/json.hpp>

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
            path TEXT UNIQUE NOT NULL,
            name TEXT,
            vendor TEXT,
            version TEXT,
            uid TEXT,
            description TEXT,
            url TEXT,
            category TEXT,
            num_inputs INTEGER DEFAULT 0,
            num_outputs INTEGER DEFAULT 0,
            status TEXT NOT NULL,
            error_message TEXT,
            scan_time_ms INTEGER DEFAULT 0,
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
}

void DatabaseManager::upsertPluginResult(const boost::filesystem::path& pluginPath, const rps::ipc::ScanResult& result, int64_t scanTimeMs) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    sqlite3_stmt* stmt = nullptr;
    
    // 1. Upsert into plugins table
    const std::string upsertPluginSql = R"(
        INSERT INTO plugins (path, name, vendor, version, uid, description, url, category, num_inputs, num_outputs, status, error_message, scan_time_ms, last_scanned)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'SUCCESS', NULL, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(path) DO UPDATE SET
            name=excluded.name,
            vendor=excluded.vendor,
            version=excluded.version,
            uid=excluded.uid,
            description=excluded.description,
            url=excluded.url,
            category=excluded.category,
            num_inputs=excluded.num_inputs,
            num_outputs=excluded.num_outputs,
            status=excluded.status,
            error_message=excluded.error_message,
            scan_time_ms=excluded.scan_time_ms,
            last_scanned=excluded.last_scanned;
    )";

    if (sqlite3_prepare_v2(m_db, upsertPluginSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare plugin upsert statement.\n";
        return;
    }

    std::string pathStr = pluginPath.string();
    sqlite3_bind_text(stmt, 1, pathStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, result.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, result.vendor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, result.version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, result.uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, result.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, result.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, result.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 9, result.numInputs);
    sqlite3_bind_int(stmt, 10, result.numOutputs);
    sqlite3_bind_int64(stmt, 11, scanTimeMs);

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

void DatabaseManager::recordPluginFailure(const boost::filesystem::path& pluginPath, const std::string& errorMsg, int64_t scanTimeMs) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    sqlite3_stmt* stmt = nullptr;
    
    const std::string upsertFailureSql = R"(
        INSERT INTO plugins (path, status, error_message, scan_time_ms, last_scanned)
        VALUES (?, 'ERROR', ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(path) DO UPDATE SET
            status=excluded.status,
            error_message=excluded.error_message,
            scan_time_ms=excluded.scan_time_ms,
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

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to execute plugin failure update.\n";
    }
    sqlite3_finalize(stmt);
}

} // namespace rps::orchestrator::db
