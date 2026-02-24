#include <rps/engine/db/DatabaseManager.hpp>
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <boost/json.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <fstream>

namespace rps::engine::db {

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

    const std::string createTableAaxPlugins = R"(
        CREATE TABLE IF NOT EXISTS aax_plugins (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plugin_id INTEGER NOT NULL,
            plugin_index INTEGER NOT NULL,
            manufacturer_id TEXT,
            manufacturer_id_num INTEGER,
            product_id TEXT,
            product_id_num INTEGER,
            aax_plugin_id TEXT,
            aax_plugin_id_num INTEGER,
            effect_id TEXT,
            plugin_type INTEGER,
            stem_format_input INTEGER,
            stem_format_output INTEGER,
            stem_format_sidechain INTEGER,
            FOREIGN KEY (plugin_id) REFERENCES plugins(id) ON DELETE CASCADE
        );
    )";

    const std::string createTablePluginsSkipped = R"(
        CREATE TABLE IF NOT EXISTS plugins_skipped (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            format TEXT,
            reason TEXT,
            file_mtime TEXT,
            last_scanned TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createTablePluginsBlocked = R"(
        CREATE TABLE IF NOT EXISTS plugins_blocked (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT UNIQUE NOT NULL,
            format TEXT,
            reason TEXT,
            file_mtime TEXT,
            last_scanned TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createTableVst3Classes = R"(
        CREATE TABLE IF NOT EXISTS vst3_classes (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            plugin_id INTEGER NOT NULL,
            class_index INTEGER NOT NULL,
            name TEXT,
            uid TEXT,
            category TEXT,
            vendor TEXT,
            version TEXT,
            FOREIGN KEY (plugin_id) REFERENCES plugins(id) ON DELETE CASCADE
        );
    )";

    executeQuery(createTablePlugins);
    executeQuery(createTableParameters);
    executeQuery(createTableAaxPlugins);
    executeQuery(createTableVst3Classes);
    executeQuery(createTablePluginsSkipped);
    executeQuery(createTablePluginsBlocked);

    // Enable WAL mode for better concurrent write performance.
    // WAL allows readers and writers to operate simultaneously.
    // PRAGMA returns a result row, so we use sqlite3_exec directly.
    {
        char* errMsg = nullptr;
        sqlite3_exec(m_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
        if (errMsg) sqlite3_free(errMsg);
    }

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

    // Wrap the entire operation in a single transaction. Without this, each
    // INSERT is an implicit transaction with its own fsync — 30-50s for a
    // plugin with thousands of parameters. With an explicit transaction,
    // all writes share a single fsync at COMMIT.
    executeQuery("BEGIN TRANSACTION;");

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

    // If this is an AAX plugin, store variant data in aax_plugins table
    if (result.format == "aax" && !result.extraData.empty()) {
        upsertAaxPluginVariants(pluginId, result);
    }

    // If this is a VST3 plugin, store class entries in vst3_classes table
    if (result.format == "vst3" && !result.extraData.empty()) {
        upsertVst3Classes(pluginId, result);
    }

    executeQuery("COMMIT;");
}

void DatabaseManager::upsertAaxPluginVariants(int64_t pluginId, const rps::ipc::ScanResult& result) {
    // Delete old AAX variants for this plugin
    sqlite3_stmt* stmt = nullptr;
    const std::string deleteOldSql = "DELETE FROM aax_plugins WHERE plugin_id = ?";
    if (sqlite3_prepare_v2(m_db, deleteOldSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, pluginId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Parse variant count from extraData
    auto it = result.extraData.find("aax_variant_count");
    if (it == result.extraData.end()) return;
    int variantCount = std::stoi(it->second);

    const std::string insertSql = R"(
        INSERT INTO aax_plugins (plugin_id, plugin_index, manufacturer_id, manufacturer_id_num,
            product_id, product_id_num, aax_plugin_id, aax_plugin_id_num,
            effect_id, plugin_type, stem_format_input, stem_format_output, stem_format_sidechain)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    if (sqlite3_prepare_v2(m_db, insertSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare AAX variant insert.\n";
        return;
    }

    for (int i = 0; i < variantCount; ++i) {
        std::string prefix = "aax_v" + std::to_string(i) + "_";
        auto getStr = [&](const std::string& key) -> std::string {
            auto found = result.extraData.find(prefix + key);
            return (found != result.extraData.end()) ? found->second : "";
        };
        auto getInt = [&](const std::string& key) -> int64_t {
            auto found = result.extraData.find(prefix + key);
            if (found != result.extraData.end() && !found->second.empty()) {
                try { return std::stoll(found->second); } catch (...) {}
            }
            return 0;
        };

        sqlite3_bind_int64(stmt, 1, pluginId);
        sqlite3_bind_int(stmt, 2, i + 1);

        std::string mfgId = getStr("manufacturer_id");
        sqlite3_bind_text(stmt, 3, mfgId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, getInt("manufacturer_id_num"));

        std::string prodId = getStr("product_id");
        sqlite3_bind_text(stmt, 5, prodId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 6, getInt("product_id_num"));

        std::string plgId = getStr("plugin_id");
        sqlite3_bind_text(stmt, 7, plgId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 8, getInt("plugin_id_num"));

        std::string effectId = getStr("effect_id");
        sqlite3_bind_text(stmt, 9, effectId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 10, getInt("plugin_type"));
        sqlite3_bind_int64(stmt, 11, getInt("stem_format_input"));
        sqlite3_bind_int64(stmt, 12, getInt("stem_format_output"));
        sqlite3_bind_int64(stmt, 13, getInt("stem_format_sidechain"));

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::upsertVst3Classes(int64_t pluginId, const rps::ipc::ScanResult& result) {
    // Delete old VST3 classes for this plugin
    sqlite3_stmt* stmt = nullptr;
    const std::string deleteOldSql = "DELETE FROM vst3_classes WHERE plugin_id = ?";
    if (sqlite3_prepare_v2(m_db, deleteOldSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, pluginId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    auto it = result.extraData.find("vst3_class_count");
    if (it == result.extraData.end()) return;
    int classCount = std::stoi(it->second);

    const std::string insertSql = R"(
        INSERT INTO vst3_classes (plugin_id, class_index, name, uid, category, vendor, version)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )";

    if (sqlite3_prepare_v2(m_db, insertSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare VST3 class insert.\n";
        return;
    }

    for (int i = 0; i < classCount; ++i) {
        std::string prefix = "vst3_c" + std::to_string(i) + "_";
        auto getStr = [&](const std::string& key) -> std::string {
            auto found = result.extraData.find(prefix + key);
            return (found != result.extraData.end()) ? found->second : "";
        };

        sqlite3_bind_int64(stmt, 1, pluginId);
        sqlite3_bind_int(stmt, 2, i);

        std::string name = getStr("name");
        std::string uid = getStr("uid");
        std::string category = getStr("category");
        std::string vendor = getStr("vendor");
        std::string version = getStr("version");

        sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, uid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, vendor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, version.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::recordPluginFailure(const boost::filesystem::path& pluginPath, const std::string& errorMsg,
                                           int64_t scanTimeMs, const std::string& fileMtime, const std::string& fileHash) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    executeQuery("BEGIN TRANSACTION;");
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
    executeQuery("COMMIT;");
}

void DatabaseManager::recordPluginSkip(const boost::filesystem::path& pluginPath, const std::string& format,
                                        const std::string& reason, const std::string& fileMtime) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    executeQuery("BEGIN TRANSACTION;");
    sqlite3_stmt* stmt = nullptr;

    const std::string upsertSql = R"(
        INSERT INTO plugins_skipped (path, format, reason, file_mtime, last_scanned)
        VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(path) DO UPDATE SET
            format=excluded.format,
            reason=excluded.reason,
            file_mtime=excluded.file_mtime,
            last_scanned=excluded.last_scanned;
    )";

    if (sqlite3_prepare_v2(m_db, upsertSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare plugin skip statement.\n";
        executeQuery("ROLLBACK;");
        return;
    }

    std::string pathStr = pluginPath.string();
    sqlite3_bind_text(stmt, 1, pathStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, format.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, fileMtime.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to execute plugin skip upsert.\n";
    }
    sqlite3_finalize(stmt);
    executeQuery("COMMIT;");
}

std::map<std::string, std::string> DatabaseManager::loadSkippedCache(const std::vector<std::string>& formats) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    std::map<std::string, std::string> cache;

    std::string sql = "SELECT path, file_mtime FROM plugins_skipped";
    if (!formats.empty()) {
        std::string inClause;
        for (size_t i = 0; i < formats.size(); ++i) {
            if (i > 0) inClause += ",";
            inClause += "'" + formats[i] + "'";
        }
        sql += " WHERE format IN (" + inClause + ")";
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return cache;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* mt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        cache[path] = mt ? mt : "";
    }
    sqlite3_finalize(stmt);
    return cache;
}

void DatabaseManager::recordPluginBlocked(const boost::filesystem::path& pluginPath, const std::string& format,
                                           const std::string& reason, const std::string& fileMtime) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    executeQuery("BEGIN TRANSACTION;");
    sqlite3_stmt* stmt = nullptr;

    const std::string upsertSql = R"(
        INSERT INTO plugins_blocked (path, format, reason, file_mtime, last_scanned)
        VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(path) DO UPDATE SET
            format=excluded.format,
            reason=excluded.reason,
            file_mtime=excluded.file_mtime,
            last_scanned=excluded.last_scanned;
    )";

    if (sqlite3_prepare_v2(m_db, upsertSql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare plugin blocked statement.\n";
        executeQuery("ROLLBACK;");
        return;
    }

    std::string pathStr = pluginPath.string();
    sqlite3_bind_text(stmt, 1, pathStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, format.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, fileMtime.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to execute plugin blocked upsert.\n";
    }
    sqlite3_finalize(stmt);
    executeQuery("COMMIT;");
}

std::map<std::string, std::string> DatabaseManager::loadBlockedCache(const std::vector<std::string>& formats) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    std::map<std::string, std::string> cache;

    std::string sql = "SELECT path, file_mtime FROM plugins_blocked";
    if (!formats.empty()) {
        std::string inClause;
        for (size_t i = 0; i < formats.size(); ++i) {
            if (i > 0) inClause += ",";
            inClause += "'" + formats[i] + "'";
        }
        sql += " WHERE format IN (" + inClause + ")";
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return cache;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* mt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        cache[path] = mt ? mt : "";
    }
    sqlite3_finalize(stmt);
    return cache;
}

std::map<std::string, DatabaseManager::PluginCacheEntry> DatabaseManager::loadPluginCache(const std::vector<std::string>& formats) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    std::map<std::string, PluginCacheEntry> cache;

    std::string sql = "SELECT path, file_mtime, file_hash, status FROM plugins";
    if (!formats.empty()) {
        std::string inClause;
        for (size_t i = 0; i < formats.size(); ++i) {
            if (i > 0) inClause += ",";
            inClause += "'" + formats[i] + "'";
        }
        sql += " WHERE format IN (" + inClause + ")";
    }
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

void DatabaseManager::clearPluginsByFormats(const std::vector<std::string>& formats) {
    std::lock_guard<std::mutex> lock(m_dbMutex);

    if (formats.empty()) return;

    // Build IN clause: ('vst2','vst3',...)
    std::string inClause;
    for (size_t i = 0; i < formats.size(); ++i) {
        if (i > 0) inClause += ",";
        inClause += "'" + formats[i] + "'";
    }

    executeQuery("BEGIN TRANSACTION;");
    // Delete child rows for matching plugins first (FK)
    executeQuery("DELETE FROM aax_plugins WHERE plugin_id IN (SELECT id FROM plugins WHERE format IN (" + inClause + "));");
    executeQuery("DELETE FROM vst3_classes WHERE plugin_id IN (SELECT id FROM plugins WHERE format IN (" + inClause + "));");
    executeQuery("DELETE FROM parameters WHERE plugin_id IN (SELECT id FROM plugins WHERE format IN (" + inClause + "));");
    executeQuery("DELETE FROM plugins WHERE format IN (" + inClause + ");");
    executeQuery("DELETE FROM plugins_skipped WHERE format IN (" + inClause + ");");
    executeQuery("DELETE FROM plugins_blocked WHERE format IN (" + inClause + ");");
    executeQuery("COMMIT;");
}

size_t DatabaseManager::removeStaleEntries(const std::set<std::string>& validPaths, const std::vector<std::string>& formats) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    size_t removed = 0;

    // Collect paths in DB (filtered by formats if provided)
    std::vector<std::string> dbPaths;
    std::string sql = "SELECT path FROM plugins";
    if (!formats.empty()) {
        std::string inClause;
        for (size_t i = 0; i < formats.size(); ++i) {
            if (i > 0) inClause += ",";
            inClause += "'" + formats[i] + "'";
        }
        sql += " WHERE format IN (" + inClause + ")";
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            dbPaths.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        }
        sqlite3_finalize(stmt);
    }

    // Also collect stale skipped and blocked paths
    auto collectAuxPaths = [&](const std::string& table) {
        std::vector<std::string> paths;
        std::string auxSql = "SELECT path FROM " + table;
        if (!formats.empty()) {
            std::string inClause;
            for (size_t i = 0; i < formats.size(); ++i) {
                if (i > 0) inClause += ",";
                inClause += "'" + formats[i] + "'";
            }
            auxSql += " WHERE format IN (" + inClause + ")";
        }
        if (sqlite3_prepare_v2(m_db, auxSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                paths.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            }
            sqlite3_finalize(stmt);
        }
        return paths;
    };
    auto dbSkippedPaths = collectAuxPaths("plugins_skipped");
    auto dbBlockedPaths = collectAuxPaths("plugins_blocked");

    // Prepare statements once, reuse in loop
    sqlite3_stmt* stmtDelAax = nullptr;
    sqlite3_stmt* stmtDelParams = nullptr;
    sqlite3_stmt* stmtDelPlugin = nullptr;
    sqlite3_stmt* stmtDelSkip = nullptr;
    sqlite3_stmt* stmtDelBlock = nullptr;
    sqlite3_stmt* stmtDelVst3 = nullptr;
    sqlite3_prepare_v2(m_db, "DELETE FROM aax_plugins WHERE plugin_id IN (SELECT id FROM plugins WHERE path = ?)", -1, &stmtDelAax, nullptr);
    sqlite3_prepare_v2(m_db, "DELETE FROM vst3_classes WHERE plugin_id IN (SELECT id FROM plugins WHERE path = ?)", -1, &stmtDelVst3, nullptr);
    sqlite3_prepare_v2(m_db, "DELETE FROM parameters WHERE plugin_id IN (SELECT id FROM plugins WHERE path = ?)", -1, &stmtDelParams, nullptr);
    sqlite3_prepare_v2(m_db, "DELETE FROM plugins WHERE path = ?", -1, &stmtDelPlugin, nullptr);
    sqlite3_prepare_v2(m_db, "DELETE FROM plugins_skipped WHERE path = ?", -1, &stmtDelSkip, nullptr);
    sqlite3_prepare_v2(m_db, "DELETE FROM plugins_blocked WHERE path = ?", -1, &stmtDelBlock, nullptr);

    auto deleteByPath = [&](sqlite3_stmt* s, const std::string& p) {
        if (s) {
            sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(s);
            sqlite3_reset(s);
        }
    };

    executeQuery("BEGIN TRANSACTION;");
    for (const auto& p : dbPaths) {
        if (validPaths.find(p) == validPaths.end()) {
            deleteByPath(stmtDelAax, p);
            deleteByPath(stmtDelVst3, p);
            deleteByPath(stmtDelParams, p);
            deleteByPath(stmtDelPlugin, p);
            removed++;
        }
    }
    for (const auto& p : dbSkippedPaths) {
        if (validPaths.find(p) == validPaths.end()) {
            deleteByPath(stmtDelSkip, p);
            removed++;
        }
    }
    for (const auto& p : dbBlockedPaths) {
        if (validPaths.find(p) == validPaths.end()) {
            deleteByPath(stmtDelBlock, p);
            removed++;
        }
    }
    executeQuery("COMMIT;");

    if (stmtDelAax) sqlite3_finalize(stmtDelAax);
    if (stmtDelVst3) sqlite3_finalize(stmtDelVst3);
    if (stmtDelParams) sqlite3_finalize(stmtDelParams);
    if (stmtDelPlugin) sqlite3_finalize(stmtDelPlugin);
    if (stmtDelSkip) sqlite3_finalize(stmtDelSkip);
    if (stmtDelBlock) sqlite3_finalize(stmtDelBlock);

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

} // namespace rps::engine::db
