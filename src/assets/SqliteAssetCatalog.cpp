#include "assets/SqliteAssetCatalog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

namespace Iridium {

    namespace {

        [[noreturn]] void throwDatabaseError(sqlite3* database, std::string_view operation) {
            throw std::runtime_error(std::string(operation) + ": " +
                (database != nullptr ? sqlite3_errmsg(database) : "SQLite unavailable"));
        }

        void check(int result, sqlite3* database, std::string_view operation) {
            if (result != SQLITE_OK) throwDatabaseError(database, operation);
        }

        void execute(sqlite3* database, const char* sql) {
            char* error = nullptr;
            const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
            if (result == SQLITE_OK) return;
            const std::string message = error != nullptr
                ? std::string(error) : std::string(sqlite3_errmsg(database));
            sqlite3_free(error);
            throw std::runtime_error(message);
        }

        bool tryExecute(sqlite3* database, const char* sql) {
            char* error = nullptr;
            const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
            sqlite3_free(error);
            return result == SQLITE_OK;
        }

        class Statement {
        public:
            Statement(sqlite3* database, const std::string& sql) : m_database(database) {
                check(sqlite3_prepare_v2(database, sql.c_str(), -1, &m_statement, nullptr),
                    database, "prepare catalog statement");
            }
            ~Statement() {
                if (m_statement != nullptr) sqlite3_finalize(m_statement);
            }

            Statement(const Statement&) = delete;
            Statement& operator=(const Statement&) = delete;

            [[nodiscard]] sqlite3_stmt* get() const noexcept { return m_statement; }

            void bindText(int index, std::string_view value) {
                check(sqlite3_bind_text(m_statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT),
                    m_database, "bind catalog text");
            }
            void bindInteger(int index, int64_t value) {
                check(sqlite3_bind_int64(m_statement, index, value),
                    m_database, "bind catalog integer");
            }
            void bindNull(int index) {
                check(sqlite3_bind_null(m_statement, index),
                    m_database, "bind catalog null");
            }
            [[nodiscard]] bool stepRow() {
                const int result = sqlite3_step(m_statement);
                if (result == SQLITE_ROW) return true;
                if (result == SQLITE_DONE) return false;
                throwDatabaseError(m_database, "step catalog statement");
            }
            void stepDone() {
                if (sqlite3_step(m_statement) != SQLITE_DONE) {
                    throwDatabaseError(m_database, "execute catalog statement");
                }
            }
            void reset() {
                check(sqlite3_reset(m_statement), m_database, "reset catalog statement");
                check(sqlite3_clear_bindings(m_statement), m_database,
                    "clear catalog bindings");
            }

        private:
            sqlite3* m_database = nullptr;
            sqlite3_stmt* m_statement = nullptr;
        };

        std::string columnText(sqlite3_stmt* statement, int column) {
            const unsigned char* value = sqlite3_column_text(statement, column);
            return value == nullptr ? "" : reinterpret_cast<const char*>(value);
        }

        std::string normalizedSearchText(const AssetCatalogRecord& record) {
            std::string result = record.displayName + " " + record.sourcePath + " " +
                record.sourceKey + " " + record.assetType;
            for (const std::string& tag : record.tags) result += " " + tag;
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return result;
        }

        std::string tagsToJson(const std::vector<std::string>& tags) {
            return nlohmann::json(tags).dump();
        }

        std::vector<std::string> tagsFromJson(const std::string& text) {
            const auto parsed = nlohmann::json::parse(text, nullptr, false);
            if (!parsed.is_array()) return {};
            std::vector<std::string> result;
            for (const auto& tag : parsed) {
                if (tag.is_string()) result.push_back(tag.get<std::string>());
            }
            return result;
        }

        AssetCatalogRecord readRecord(sqlite3_stmt* statement) {
            AssetCatalogRecord record;
            const auto guid = AssetGuid::parse(columnText(statement, 0));
            if (!guid) throw std::runtime_error("Catalog contains an invalid asset GUID.");
            record.guid = *guid;
            if (sqlite3_column_type(statement, 1) != SQLITE_NULL) {
                const auto parent = AssetGuid::parse(columnText(statement, 1));
                if (!parent) throw std::runtime_error("Catalog contains an invalid parent GUID.");
                record.parentGuid = *parent;
            }
            record.assetType = columnText(statement, 2);
            record.assetRoot = columnText(statement, 3);
            record.sourcePath = columnText(statement, 4);
            record.metadataPath = columnText(statement, 5);
            record.sourceKey = columnText(statement, 6);
            record.displayName = columnText(statement, 7);
            record.importerId = columnText(statement, 8);
            record.importerVersion = static_cast<uint32_t>(
                sqlite3_column_int64(statement, 9));
            record.status = static_cast<AssetCatalogStatus>(
                sqlite3_column_int(statement, 10));
            record.tags = tagsFromJson(columnText(statement, 11));
            record.diagnosticSummary = columnText(statement, 12);
            return record;
        }

        constexpr const char* kSelectedColumns =
            "guid,parent_guid,asset_type,asset_root,source_path,metadata_path,"
            "source_key,display_name,importer_id,importer_version,status,tags_json,"
            "diagnostic_summary";

        std::string ftsQuery(std::string_view text) {
            std::string result;
            std::string token;
            auto flush = [&]() {
                if (token.empty()) return;
                if (!result.empty()) result.push_back(' ');
                result += token;
                result.push_back('*');
                token.clear();
            };
            for (const unsigned char character : text) {
                if (std::isalnum(character) || character == '_') {
                    token.push_back(static_cast<char>(std::tolower(character)));
                } else {
                    flush();
                }
            }
            flush();
            return result;
        }

        std::string whereClause(const AssetCatalogQuery& query, bool useFullTextSearch) {
            std::string where = " WHERE 1=1";
            if (!query.text.empty()) {
                where += useFullTextSearch
                    ? " AND asset_search MATCH ?"
                    : " AND search_text LIKE ? ESCAPE '\\'";
            }
            if (query.sourceDirectory) {
                where +=
                    " AND source_path LIKE ? ESCAPE '\\'"
                    " AND instr(substr(source_path,length(?) + 2),'/')=0";
            }
            if (!query.includeSubassets) {
                where += " AND parent_guid IS NULL";
            }
            if (query.assetType) where += " AND asset_type=?";
            if (query.status) where += " AND status=?";
            return where;
        }

        std::string escapeLike(std::string value) {
            std::string result;
            result.reserve(value.size());
            for (const char character : value) {
                if (character == '\\' || character == '%' || character == '_') {
                    result.push_back('\\');
                }
                result.push_back(character);
            }
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char item) { return static_cast<char>(std::tolower(item)); });
            return result;
        }

        int bindQueryFilters(Statement& statement, const AssetCatalogQuery& query,
            bool useFullTextSearch) {
            int binding = 1;
            if (!query.text.empty()) {
                statement.bindText(binding++, useFullTextSearch
                    ? ftsQuery(query.text)
                    : "%" + escapeLike(query.text) + "%");
            }
            if (query.sourceDirectory) {
                statement.bindText(binding++,
                    escapeLike(*query.sourceDirectory) + "/%");
                statement.bindText(binding++,
                    *query.sourceDirectory);
            }
            if (query.assetType) statement.bindText(binding++, *query.assetType);
            if (query.status) {
                statement.bindInteger(binding++, static_cast<int64_t>(*query.status));
            }
            return binding;
        }

        class SqliteAssetCatalog final : public AssetCatalog {
        public:
            explicit SqliteAssetCatalog(const std::filesystem::path& databasePath) {
                const std::string path = databasePath.generic_string();
                if (path != ":memory:" && !databasePath.parent_path().empty()) {
                    std::error_code error;
                    std::filesystem::create_directories(databasePath.parent_path(), error);
                    if (error) {
                        throw std::runtime_error("Could not create catalog directory: " +
                            error.message());
                    }
                }
                const int result = sqlite3_open_v2(path.c_str(), &m_database,
                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                    nullptr);
                if (result != SQLITE_OK) throwDatabaseError(m_database, "open asset catalog");
                sqlite3_busy_timeout(m_database, 5000);
                initialize();
            }

            ~SqliteAssetCatalog() override {
                if (m_database != nullptr) sqlite3_close(m_database);
            }

            void rebuild(
                std::span<const AssetCatalogRecord> records,
                std::span<const std::string>
                    sourceDirectories) override {
                std::set<std::string> directorySet;
                directorySet.insert(
                    sourceDirectories.begin(),
                    sourceDirectories.end());
                for (const AssetCatalogRecord& record : records) {
                    if (record.parentGuid) continue;
                    std::filesystem::path directory =
                        std::filesystem::path(record.sourcePath).parent_path();
                    while (!directory.empty() && directory != ".") {
                        directorySet.insert(directory.generic_string());
                        directory = directory.parent_path();
                    }
                }
                std::vector<std::string> directories(
                    directorySet.begin(), directorySet.end());

                std::lock_guard lock(m_mutex);
                execute(m_database, "BEGIN IMMEDIATE TRANSACTION");
                try {
                    execute(m_database, "DELETE FROM assets");
                    Statement insert(m_database,
                        "INSERT INTO assets("
                        "guid,parent_guid,asset_type,asset_root,source_path,metadata_path,"
                        "source_key,display_name,importer_id,importer_version,status,"
                        "tags_json,diagnostic_summary,search_text"
                        ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                    for (const AssetCatalogRecord& record : records) {
                        insert.bindText(1, record.guid.toString());
                        if (record.parentGuid) {
                            insert.bindText(2, record.parentGuid->toString());
                        } else {
                            insert.bindNull(2);
                        }
                        insert.bindText(3, record.assetType);
                        insert.bindText(4, record.assetRoot);
                        insert.bindText(5, record.sourcePath);
                        insert.bindText(6, record.metadataPath);
                        insert.bindText(7, record.sourceKey);
                        insert.bindText(8, record.displayName);
                        insert.bindText(9, record.importerId);
                        insert.bindInteger(10, record.importerVersion);
                        insert.bindInteger(11, static_cast<int64_t>(record.status));
                        insert.bindText(12, tagsToJson(record.tags));
                        insert.bindText(13, record.diagnosticSummary);
                        insert.bindText(14, normalizedSearchText(record));
                        insert.stepDone();
                        insert.reset();
                    }
                    if (m_fullTextSearch) {
                        execute(m_database, "DELETE FROM temp.asset_search");
                        execute(m_database,
                            "INSERT INTO temp.asset_search(rowid,search_text) "
                            "SELECT row_id,search_text FROM assets");
                    }
                    execute(m_database, "COMMIT");
                    m_directories = std::move(directories);
                } catch (...) {
                    sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
                    throw;
                }
            }

            std::vector<AssetCatalogRecord> recordsForGuid(
                const AssetGuid& guid) const override {
                std::lock_guard lock(m_mutex);
                Statement statement(m_database, std::string("SELECT ") + kSelectedColumns +
                    " FROM assets WHERE guid=? ORDER BY asset_root,source_path,source_key");
                statement.bindText(1, guid.toString());
                std::vector<AssetCatalogRecord> result;
                while (statement.stepRow()) result.push_back(readRecord(statement.get()));
                return result;
            }

            std::vector<AssetCatalogRecord>
                recordsForSourceRoot(
                    const AssetGuid& rootGuid)
                    const override {
                std::lock_guard lock(m_mutex);
                Statement statement(m_database,
                    std::string("SELECT ") +
                    kSelectedColumns +
                    " FROM assets WHERE guid=? OR parent_guid=? "
                    "ORDER BY CASE WHEN parent_guid IS NULL THEN 0 ELSE 1 END,"
                    "asset_type,source_key,guid");
                const std::string guid =
                    rootGuid.toString();
                statement.bindText(1, guid);
                statement.bindText(2, guid);
                std::vector<AssetCatalogRecord>
                    result;
                while (statement.stepRow()) {
                    result.push_back(
                        readRecord(
                            statement.get()));
                }
                return result;
            }

            AssetCatalogQueryPage query(const AssetCatalogQuery& query) const override {
                std::lock_guard lock(m_mutex);
                const bool useFullTextSearch =
                    m_fullTextSearch && !ftsQuery(query.text).empty();
                const std::string where = whereClause(query, useFullTextSearch);
                const std::string from = useFullTextSearch
                    ? " FROM temp.asset_search CROSS JOIN assets "
                      "ON assets.row_id=asset_search.rowid"
                    : " FROM assets";

                AssetCatalogQueryPage result;
                if (query.calculateTotalMatches) {
                    Statement count(m_database, "SELECT COUNT(*)" + from + where);
                    bindQueryFilters(count, query, useFullTextSearch);
                    if (!count.stepRow()) {
                        throw std::runtime_error("Catalog count returned no row.");
                    }
                    result.totalMatches = static_cast<uint64_t>(
                        sqlite3_column_int64(count.get(), 0));
                }

                Statement select(m_database, std::string("SELECT ") + kSelectedColumns +
                    from + where +
                    " ORDER BY display_name COLLATE NOCASE,guid,source_key LIMIT ? OFFSET ?");
                int binding = bindQueryFilters(select, query, useFullTextSearch);
                select.bindInteger(binding++, query.limit);
                select.bindInteger(binding, query.offset);
                while (select.stepRow()) result.records.push_back(readRecord(select.get()));
                return result;
            }

            uint64_t recordCount() const override {
                std::lock_guard lock(m_mutex);
                Statement statement(m_database, "SELECT COUNT(*) FROM assets");
                if (!statement.stepRow()) return 0;
                return static_cast<uint64_t>(sqlite3_column_int64(statement.get(), 0));
            }

            std::vector<std::string>
                sourceDirectories() const override {
                std::lock_guard lock(m_mutex);
                return m_directories;
            }

        private:
            void initialize() {
                execute(m_database, "PRAGMA journal_mode=WAL");
                execute(m_database, "PRAGMA synchronous=NORMAL");
                execute(m_database, "PRAGMA cache_size=-65536");
                execute(m_database, "PRAGMA mmap_size=268435456");
                execute(m_database, "PRAGMA temp_store=MEMORY");
                execute(m_database,
                    "CREATE TABLE IF NOT EXISTS catalog_schema("
                    "version INTEGER NOT NULL)");
                Statement schemaCount(m_database,
                    "SELECT COUNT(*) FROM catalog_schema");
                if (!schemaCount.stepRow()) {
                    throw std::runtime_error("Catalog schema query returned no row.");
                }
                if (sqlite3_column_int64(schemaCount.get(), 0) == 0) {
                    execute(m_database, "INSERT INTO catalog_schema(version) VALUES(1)");
                }
                Statement version(m_database, "SELECT version FROM catalog_schema LIMIT 1");
                if (!version.stepRow() || sqlite3_column_int(version.get(), 0) != 1) {
                    throw std::runtime_error("Unsupported asset catalog schema.");
                }
                execute(m_database,
                    "CREATE TABLE IF NOT EXISTS assets("
                    "row_id INTEGER PRIMARY KEY,"
                    "guid TEXT NOT NULL,"
                    "parent_guid TEXT,"
                    "asset_type TEXT NOT NULL,"
                    "asset_root TEXT NOT NULL,"
                    "source_path TEXT NOT NULL,"
                    "metadata_path TEXT NOT NULL,"
                    "source_key TEXT NOT NULL,"
                    "display_name TEXT NOT NULL,"
                    "importer_id TEXT NOT NULL,"
                    "importer_version INTEGER NOT NULL,"
                    "status INTEGER NOT NULL,"
                    "tags_json TEXT NOT NULL,"
                    "diagnostic_summary TEXT NOT NULL,"
                    "search_text TEXT NOT NULL)");
                execute(m_database,
                    "CREATE INDEX IF NOT EXISTS assets_guid ON assets(guid)");
                execute(m_database,
                    "CREATE INDEX IF NOT EXISTS assets_type_status "
                    "ON assets(asset_type,status)");
                execute(m_database,
                    "CREATE INDEX IF NOT EXISTS assets_source "
                    "ON assets(asset_root,source_path,source_key)");
                m_fullTextSearch = tryExecute(m_database,
                    "CREATE VIRTUAL TABLE IF NOT EXISTS temp.asset_search "
                    "USING fts5(search_text,tokenize='unicode61')");
                if (!m_fullTextSearch) {
                    m_fullTextSearch = tryExecute(m_database,
                        "CREATE VIRTUAL TABLE IF NOT EXISTS temp.asset_search "
                        "USING fts4(search_text,tokenize=unicode61)");
                }
            }

            sqlite3* m_database = nullptr;
            bool m_fullTextSearch = false;
            mutable std::mutex m_mutex;
            std::vector<std::string> m_directories;
        };

    } // namespace

    const char* assetCatalogStatusName(AssetCatalogStatus status) noexcept {
        switch (status) {
        case AssetCatalogStatus::Ready: return "ready";
        case AssetCatalogStatus::MissingSource: return "missing-source";
        case AssetCatalogStatus::DuplicateGuid: return "duplicate-guid";
        }
        return "unknown";
    }

    std::unique_ptr<AssetCatalog> createSqliteAssetCatalog(
        const std::filesystem::path& databasePath) {
        return std::make_unique<SqliteAssetCatalog>(databasePath);
    }

} // namespace Iridium
