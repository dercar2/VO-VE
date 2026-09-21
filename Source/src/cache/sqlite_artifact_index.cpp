#include "vove/cache/sqlite_artifact_index.hpp"

#include "vove/cache/cache_key.hpp"

#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace vove::cache {
namespace {

constexpr int kSchemaVersion = 6;
constexpr int kBusyTimeoutMs = 5000;
constexpr std::size_t kMaxSourceIdentityBytes = 32768;
constexpr std::size_t kMaxStableFileIdBytes = 4096;
constexpr std::size_t kMaxRelativePathBytes = 4096;
constexpr std::size_t kMaxOldestLimit = 100000;
constexpr std::size_t kMaxPublishBatch = 100000;

constexpr std::string_view kCreateTableSql = "CREATE TABLE artifacts("
                                             "digest_hex TEXT NOT NULL PRIMARY KEY,"
                                             "source_identity_utf8 TEXT NOT NULL,"
                                             "source_size_bytes INTEGER NOT NULL,"
                                             "source_modified_unix_ns INTEGER NOT NULL,"
                                             "source_stable_file_id TEXT,"
                                             "artifact_encoding INTEGER NOT NULL,"
                                             "artifact_width INTEGER NOT NULL,"
                                             "artifact_height INTEGER NOT NULL,"
                                             "payload_bytes INTEGER NOT NULL,"
                                             "payload_crc32 INTEGER NOT NULL,"
                                             "source_color_model INTEGER NOT NULL,"
                                             "preview_provenance INTEGER NOT NULL,"
                                             "page_count INTEGER NOT NULL,"
                                             "source_profile_name TEXT NOT NULL,"
                                             "source_profile_fingerprint TEXT NOT NULL,"
                                             "relative_artifact_path_utf8 TEXT NOT NULL,"
                                             "artifact_bytes INTEGER NOT NULL,"
                                             "last_access_unix_ns INTEGER NOT NULL"
                                             ") STRICT, WITHOUT ROWID";

constexpr std::string_view kCreateLruIndexSql =
    "CREATE INDEX artifacts_lru_idx ON artifacts(last_access_unix_ns, digest_hex)";

constexpr std::string_view kCreateLocatorTableSql =
    "CREATE TABLE artifact_locators("
    "locator_digest_hex TEXT NOT NULL PRIMARY KEY,"
    "artifact_digest_hex TEXT NOT NULL,"
    "binding_digest_hex TEXT NOT NULL"
    ") STRICT, WITHOUT ROWID";

constexpr std::string_view kCreateLocatorArtifactIndexSql =
    "CREATE INDEX artifact_locators_artifact_idx ON artifact_locators(artifact_digest_hex)";

constexpr std::string_view kSelectColumns =
    "digest_hex,source_identity_utf8,source_size_bytes,source_modified_unix_ns,"
    "source_stable_file_id,artifact_encoding,artifact_width,artifact_height,payload_bytes,"
    "payload_crc32,source_color_model,preview_provenance,page_count,source_profile_name,source_"
    "profile_"
    "fingerprint,"
    "relative_artifact_path_utf8,artifact_bytes,last_access_unix_ns";

constexpr std::string_view kUpsertSql =
    "INSERT INTO artifacts("
    "digest_hex,source_identity_utf8,source_size_bytes,source_modified_unix_ns,"
    "source_stable_file_id,artifact_encoding,artifact_width,artifact_height,payload_bytes,"
    "payload_crc32,source_color_model,preview_provenance,page_count,source_profile_name,source_"
    "profile_"
    "fingerprint,"
    "relative_artifact_path_utf8,artifact_bytes,last_access_unix_ns"
    ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(digest_hex) DO UPDATE SET "
    "source_identity_utf8=excluded.source_identity_utf8,"
    "source_size_bytes=excluded.source_size_bytes,"
    "source_modified_unix_ns=excluded.source_modified_unix_ns,"
    "source_stable_file_id=excluded.source_stable_file_id,"
    "artifact_encoding=excluded.artifact_encoding,"
    "artifact_width=excluded.artifact_width,"
    "artifact_height=excluded.artifact_height,"
    "payload_bytes=excluded.payload_bytes,"
    "payload_crc32=excluded.payload_crc32,"
    "source_color_model=excluded.source_color_model,"
    "preview_provenance=excluded.preview_provenance,"
    "page_count=excluded.page_count,"
    "source_profile_name=excluded.source_profile_name,"
    "source_profile_fingerprint=excluded.source_profile_fingerprint,"
    "relative_artifact_path_utf8=excluded.relative_artifact_path_utf8,"
    "artifact_bytes=excluded.artifact_bytes,"
    "last_access_unix_ns=excluded.last_access_unix_ns";

struct ExpectedColumn {
    std::string_view name;
    std::string_view type;
    bool not_null;
    int primary_key;
};

struct TextColumnSpec {
    int index;
    std::size_t maximum_bytes;
    bool allow_empty;
};

struct AggregateQuery {
    std::string_view operation;
    std::string_view sql;
};

constexpr std::array<ExpectedColumn, 18> kExpectedColumns{{
    {"digest_hex", "TEXT", true, 1},
    {"source_identity_utf8", "TEXT", true, 0},
    {"source_size_bytes", "INTEGER", true, 0},
    {"source_modified_unix_ns", "INTEGER", true, 0},
    {"source_stable_file_id", "TEXT", false, 0},
    {"artifact_encoding", "INTEGER", true, 0},
    {"artifact_width", "INTEGER", true, 0},
    {"artifact_height", "INTEGER", true, 0},
    {"payload_bytes", "INTEGER", true, 0},
    {"payload_crc32", "INTEGER", true, 0},
    {"source_color_model", "INTEGER", true, 0},
    {"preview_provenance", "INTEGER", true, 0},
    {"page_count", "INTEGER", true, 0},
    {"source_profile_name", "TEXT", true, 0},
    {"source_profile_fingerprint", "TEXT", true, 0},
    {"relative_artifact_path_utf8", "TEXT", true, 0},
    {"artifact_bytes", "INTEGER", true, 0},
    {"last_access_unix_ns", "INTEGER", true, 0},
}};

constexpr std::array<ExpectedColumn, 3> kExpectedLocatorColumns{{
    {"locator_digest_hex", "TEXT", true, 1},
    {"artifact_digest_hex", "TEXT", true, 0},
    {"binding_digest_hex", "TEXT", true, 0},
}};

class Statement {
  public:
    Statement() = default;
    ~Statement() {
        if (statement_ != nullptr) {
            static_cast<void>(sqlite3_finalize(statement_));
        }
    }

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    [[nodiscard]] int prepare(sqlite3 *database, const std::string_view sql) {
        return sqlite3_prepare_v3(database, sql.data(), static_cast<int>(sql.size()),
                                  SQLITE_PREPARE_PERSISTENT, &statement_, nullptr);
    }

    [[nodiscard]] sqlite3_stmt *get() const noexcept {
        return statement_;
    }

    void finalize() noexcept {
        if (statement_ != nullptr) {
            static_cast<void>(sqlite3_finalize(statement_));
            statement_ = nullptr;
        }
    }

  private:
    sqlite3_stmt *statement_{};
};

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] bool is_digest(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] bool valid_utf8(const std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const auto first = static_cast<unsigned char>(value[offset]);
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        if (first <= 0x7fU) {
            if (first == 0U) {
                return false;
            }
            continuation_count = 0;
            code_point = first;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (offset + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto byte = static_cast<unsigned char>(value[offset + index]);
            if ((byte & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3fU);
        }
        if ((continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U) ||
            (code_point >= 0xd800U && code_point <= 0xdfffU) || code_point > 0x10ffffU) {
            return false;
        }
        offset += continuation_count + 1;
    }
    return true;
}

[[nodiscard]] bool valid_text(const std::string_view value, const std::size_t maximum,
                              const bool allow_empty) {
    return value.size() <= maximum && (allow_empty || !value.empty()) && valid_utf8(value);
}

[[nodiscard]] std::string expected_relative_path(const std::string_view digest) {
    return std::string(digest.substr(0, 2)) + "/" + std::string(digest.substr(2, 2)) + "/" +
           std::string(digest) + ".vvt";
}

[[nodiscard]] std::optional<std::string> validate_record(const ArtifactIndexRecord &record) {
    if (!is_digest(record.digest_hex)) {
        return "digest must be exactly 64 lowercase hexadecimal characters";
    }
    if (!valid_text(record.source.source_identity_utf8, kMaxSourceIdentityBytes, false)) {
        return "source identity is empty, invalid UTF-8, or outside its size limit";
    }
    if (record.source.size_bytes >
        static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return "source size exceeds SQLite's signed integer range";
    }
    if (record.source.stable_file_id &&
        !valid_text(*record.source.stable_file_id, kMaxStableFileIdBytes, false)) {
        return "stable file ID is invalid or outside its size limit";
    }
    if (record.metadata.encoding != ArtifactEncoding::qoi_rgba8 || record.metadata.width == 0 ||
        record.metadata.height == 0 || record.metadata.width > kMaxArtifactDimension ||
        record.metadata.height > kMaxArtifactDimension ||
        record.metadata.payload_bytes > kMaxArtifactBytes - kVvt1HeaderBytes ||
        record.metadata.page_count == 0 || record.metadata.page_count > kMaxArtifactPageCount ||
        record.metadata.source_color_model > ColorModel::indexed ||
        record.metadata.provenance > PreviewProvenance::embedded_preview) {
        return "artifact metadata is outside VVT1 limits";
    }
    const auto expected_bytes =
        static_cast<std::uint64_t>(kVvt1HeaderBytes) + record.source_profile_name.size() +
        record.source_profile_fingerprint.size() + record.metadata.payload_bytes;
    if (record.artifact_bytes != expected_bytes || record.artifact_bytes > kMaxArtifactBytes) {
        return "artifact byte count does not match the VVT1 payload";
    }
    if (!valid_text(record.source_profile_name, kMaxSourceProfileNameBytes, true) ||
        !valid_text(record.source_profile_fingerprint, kMaxSourceProfileFingerprintBytes, true)) {
        return "color profile metadata is invalid or outside its size limit";
    }
    if (!valid_text(record.relative_artifact_path_utf8, kMaxRelativePathBytes, false) ||
        record.relative_artifact_path_utf8 != expected_relative_path(record.digest_hex)) {
        return "artifact path must be the canonical digest-sharded relative path";
    }
    if (record.last_access_unix_ns < 0) {
        return "last-access timestamp cannot be negative";
    }
    return std::nullopt;
}

[[nodiscard]] int bind_text(sqlite3_stmt *statement, const int parameter,
                            const std::string_view value) {
    // SQLITE_TRANSIENT is SQLite's documented sentinel, not an application pointer cast.
    return sqlite3_bind_text(statement, parameter, value.data(), static_cast<int>(value.size()),
                             SQLITE_TRANSIENT); // NOLINT(performance-no-int-to-ptr)
}

[[nodiscard]] int bind_record(sqlite3_stmt *statement, const ArtifactIndexRecord &record) {
    int result = bind_text(statement, 1, record.digest_hex);
    if (result == SQLITE_OK) {
        result = bind_text(statement, 2, record.source.source_identity_utf8);
    }
    if (result == SQLITE_OK) {
        result =
            sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(record.source.size_bytes));
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 4, record.source.modified_unix_ns);
    }
    if (result == SQLITE_OK) {
        result = record.source.stable_file_id
                     ? bind_text(statement, 5, *record.source.stable_file_id)
                     : sqlite3_bind_null(statement, 5);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 6, static_cast<int>(record.metadata.encoding));
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 7, record.metadata.width);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 8, record.metadata.height);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 9, record.metadata.payload_bytes);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 10, record.metadata.payload_crc32);
    }
    if (result == SQLITE_OK) {
        result =
            sqlite3_bind_int(statement, 11, static_cast<int>(record.metadata.source_color_model));
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 12, static_cast<int>(record.metadata.provenance));
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 13, record.metadata.page_count);
    }
    if (result == SQLITE_OK) {
        result = bind_text(statement, 14, record.source_profile_name);
    }
    if (result == SQLITE_OK) {
        result = bind_text(statement, 15, record.source_profile_fingerprint);
    }
    if (result == SQLITE_OK) {
        result = bind_text(statement, 16, record.relative_artifact_path_utf8);
    }
    if (result == SQLITE_OK) {
        result =
            sqlite3_bind_int64(statement, 17, static_cast<sqlite3_int64>(record.artifact_bytes));
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 18, record.last_access_unix_ns);
    }
    return result;
}

[[nodiscard]] std::optional<std::string> column_text(sqlite3_stmt *statement,
                                                     const TextColumnSpec spec) {
    if (sqlite3_column_type(statement, spec.index) != SQLITE_TEXT) {
        return std::nullopt;
    }
    const auto length = sqlite3_column_bytes(statement, spec.index);
    const auto *bytes = sqlite3_column_text(statement, spec.index);
    if (length < 0 || bytes == nullptr) {
        return std::nullopt;
    }
    std::string value(reinterpret_cast<const char *>(bytes), static_cast<std::size_t>(length));
    if (!valid_text(value, spec.maximum_bytes, spec.allow_empty)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<ArtifactIndexRecord> read_record(sqlite3_stmt *statement) {
    ArtifactIndexRecord record;
    auto digest = column_text(statement, {0, 64, false});
    auto source_identity = column_text(statement, {1, kMaxSourceIdentityBytes, false});
    if (!digest || !source_identity || sqlite3_column_type(statement, 2) != SQLITE_INTEGER ||
        sqlite3_column_type(statement, 3) != SQLITE_INTEGER) {
        return std::nullopt;
    }
    const auto source_size = sqlite3_column_int64(statement, 2);
    if (source_size < 0) {
        return std::nullopt;
    }
    record.digest_hex = std::move(*digest);
    record.source.source_identity_utf8 = std::move(*source_identity);
    record.source.size_bytes = static_cast<std::uint64_t>(source_size);
    record.source.modified_unix_ns = sqlite3_column_int64(statement, 3);

    if (sqlite3_column_type(statement, 4) == SQLITE_TEXT) {
        auto stable_id = column_text(statement, {4, kMaxStableFileIdBytes, false});
        if (!stable_id) {
            return std::nullopt;
        }
        record.source.stable_file_id = std::move(*stable_id);
    } else if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
        return std::nullopt;
    }

    for (int column = 5; column <= 12; ++column) {
        if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
            return std::nullopt;
        }
    }
    record.metadata.encoding = static_cast<ArtifactEncoding>(sqlite3_column_int(statement, 5));
    const auto width = sqlite3_column_int64(statement, 6);
    const auto height = sqlite3_column_int64(statement, 7);
    const auto payload_bytes = sqlite3_column_int64(statement, 8);
    const auto payload_crc32 = sqlite3_column_int64(statement, 9);
    if (width < 0 || height < 0 || payload_bytes < 0 || payload_crc32 < 0 ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max() ||
        payload_bytes > std::numeric_limits<std::uint32_t>::max() ||
        payload_crc32 > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    record.metadata.width = static_cast<std::uint32_t>(width);
    record.metadata.height = static_cast<std::uint32_t>(height);
    record.metadata.payload_bytes = static_cast<std::uint32_t>(payload_bytes);
    record.metadata.payload_crc32 = static_cast<std::uint32_t>(payload_crc32);
    record.metadata.source_color_model = static_cast<ColorModel>(sqlite3_column_int(statement, 10));
    record.metadata.provenance = static_cast<PreviewProvenance>(sqlite3_column_int(statement, 11));
    const auto page_count = sqlite3_column_int64(statement, 12);
    if (page_count < 0 || page_count > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    record.metadata.page_count = static_cast<std::uint32_t>(page_count);

    auto profile_name = column_text(statement, {13, kMaxSourceProfileNameBytes, true});
    auto profile_fingerprint =
        column_text(statement, {14, kMaxSourceProfileFingerprintBytes, true});
    auto relative_path = column_text(statement, {15, kMaxRelativePathBytes, false});
    if (!profile_name || !profile_fingerprint || !relative_path ||
        sqlite3_column_type(statement, 16) != SQLITE_INTEGER ||
        sqlite3_column_type(statement, 17) != SQLITE_INTEGER) {
        return std::nullopt;
    }
    const auto artifact_bytes = sqlite3_column_int64(statement, 16);
    if (artifact_bytes < 0) {
        return std::nullopt;
    }
    record.source_profile_name = std::move(*profile_name);
    record.source_profile_fingerprint = std::move(*profile_fingerprint);
    record.relative_artifact_path_utf8 = std::move(*relative_path);
    record.artifact_bytes = static_cast<std::uint64_t>(artifact_bytes);
    record.last_access_unix_ns = sqlite3_column_int64(statement, 17);
    if (validate_record(record)) {
        return std::nullopt;
    }
    return record;
}

[[nodiscard]] bool recoverable_sqlite_error(const int result, const std::string_view message) {
    const auto primary = result & 0xff;
    return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB || primary == SQLITE_SCHEMA ||
           primary == SQLITE_ERROR || message.find("malformed") != std::string_view::npos;
}

} // namespace

class SqliteArtifactIndex::Impl {
  public:
    explicit Impl(std::filesystem::path database_path) : database_path_(std::move(database_path)) {
        std::scoped_lock lock(mutex_);
        static_cast<void>(open_locked(true));
    }

    ~Impl() {
        std::scoped_lock lock(mutex_);
        close_locked();
    }

    [[nodiscard]] std::optional<ArtifactIndexRecord> find(const std::string_view digest_hex) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("find")) {
            return std::nullopt;
        }
        if (!is_digest(digest_hex)) {
            last_error_ = "digest must be exactly 64 lowercase hexadecimal characters";
            return std::nullopt;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            Statement statement;
            const auto sql = std::string("SELECT ") + std::string(kSelectColumns) +
                             " FROM artifacts WHERE digest_hex=?";
            auto result = statement.prepare(database_, sql);
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 1, digest_hex);
            }
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
                if (result == SQLITE_ROW) {
                    auto record = read_record(statement.get());
                    if (record) {
                        clear_error_locked();
                        return record;
                    }
                    result = SQLITE_CORRUPT;
                } else if (result == SQLITE_DONE) {
                    clear_error_locked();
                    return std::nullopt;
                }
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, "find", attempt)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool publish_many(const std::span<const ArtifactIndexRecord> records) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("publish")) {
            return false;
        }
        if (records.size() > kMaxPublishBatch) {
            last_error_ = "publish batch exceeds 100000 records";
            return false;
        }
        for (const auto &record : records) {
            if (const auto error = validate_record(record)) {
                last_error_ = *error;
                return false;
            }
        }
        if (records.empty()) {
            clear_error_locked();
            return true;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto result = publish_transaction_locked(records);
            if (result == SQLITE_OK) {
                clear_error_locked();
                return true;
            }
            if (!retry_after_recovery_locked(result, "publish", attempt)) {
                return false;
            }
        }
        return false;
    }

    TouchResult touch(const std::string_view digest_hex, const std::int64_t access_unix_ns) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("touch")) {
            return TouchResult::error;
        }
        if (!is_digest(digest_hex) || access_unix_ns < 0) {
            last_error_ = "touch requires a valid digest and non-negative timestamp";
            return TouchResult::error;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            Statement statement;
            auto result = statement.prepare(
                database_, "UPDATE artifacts SET last_access_unix_ns=? WHERE digest_hex=?");
            if (result == SQLITE_OK) {
                result = sqlite3_bind_int64(statement.get(), 1, access_unix_ns);
            }
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 2, digest_hex);
            }
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
            }
            if (result == SQLITE_DONE) {
                const auto changed = sqlite3_changes(database_) != 0;
                clear_error_locked();
                return changed ? TouchResult::updated : TouchResult::missing;
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, "touch", attempt)) {
                return TouchResult::error;
            }
        }
        return TouchResult::error;
    }

    bool erase(const std::string_view digest_hex) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("erase")) {
            return false;
        }
        if (!is_digest(digest_hex)) {
            last_error_ = "erase requires a valid digest";
            return false;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto result = execute_locked("BEGIN IMMEDIATE");
            Statement locator_statement;
            if (result == SQLITE_OK) {
                result = locator_statement.prepare(
                    database_, "DELETE FROM artifact_locators WHERE artifact_digest_hex=?");
            }
            if (result == SQLITE_OK) {
                result = bind_text(locator_statement.get(), 1, digest_hex);
            }
            if (result == SQLITE_OK) {
                result = sqlite3_step(locator_statement.get());
            }
            if (result == SQLITE_DONE) {
                Statement artifact_statement;
                result = artifact_statement.prepare(database_,
                                                    "DELETE FROM artifacts WHERE digest_hex=?");
                if (result == SQLITE_OK) {
                    result = bind_text(artifact_statement.get(), 1, digest_hex);
                }
                if (result == SQLITE_OK) {
                    result = sqlite3_step(artifact_statement.get());
                }
                const auto changed = result == SQLITE_DONE && sqlite3_changes(database_) != 0;
                if (result == SQLITE_DONE) {
                    result = execute_locked("COMMIT");
                }
                if (result == SQLITE_OK) {
                    clear_error_locked();
                    return changed;
                }
            }
            static_cast<void>(execute_locked("ROLLBACK"));
            locator_statement.finalize();
            if (!retry_after_recovery_locked(result, "erase", attempt)) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string>
    find_locator(const std::string_view locator_digest_hex) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("find_locator")) {
            return std::nullopt;
        }
        if (!is_digest(locator_digest_hex)) {
            last_error_ = "locator digest must be exactly 64 lowercase hexadecimal characters";
            return std::nullopt;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            Statement statement;
            auto result = statement.prepare(
                database_,
                "SELECT artifact_digest_hex,binding_digest_hex FROM artifact_locators "
                "WHERE locator_digest_hex=?");
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 1, locator_digest_hex);
            }
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
                if (result == SQLITE_ROW) {
                    const auto digest = column_text(statement.get(), {0, 64, false});
                    const auto binding = column_text(statement.get(), {1, 64, false});
                    if (digest && binding && is_digest(*digest) && is_digest(*binding) &&
                        *binding == hash_locator_binding(locator_digest_hex, *digest).hex()) {
                        clear_error_locked();
                        return digest;
                    }
                    result = SQLITE_CORRUPT;
                } else if (result == SQLITE_DONE) {
                    clear_error_locked();
                    return std::nullopt;
                }
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, "find_locator", attempt)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool publish_locator(const std::string_view locator_digest_hex,
                         const std::string_view artifact_digest_hex) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("publish_locator")) {
            return false;
        }
        if (!is_digest(locator_digest_hex) || !is_digest(artifact_digest_hex)) {
            last_error_ = "locator publication requires two valid digests";
            return false;
        }
        const auto binding_digest =
            hash_locator_binding(locator_digest_hex, artifact_digest_hex).hex();
        for (int attempt = 0; attempt < 2; ++attempt) {
            Statement statement;
            auto result = statement.prepare(
                database_,
                "INSERT INTO artifact_locators(locator_digest_hex,artifact_digest_hex,"
                "binding_digest_hex) "
                "SELECT ?,digest_hex,? FROM artifacts WHERE digest_hex=? "
                "ON CONFLICT(locator_digest_hex) DO UPDATE SET "
                "artifact_digest_hex=excluded.artifact_digest_hex,"
                "binding_digest_hex=excluded.binding_digest_hex");
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 1, locator_digest_hex);
            }
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 2, binding_digest);
            }
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 3, artifact_digest_hex);
            }
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
            }
            if (result == SQLITE_DONE) {
                if (sqlite3_changes(database_) == 0) {
                    last_error_ = "locator target artifact is absent from the index";
                    return false;
                }
                clear_error_locked();
                return true;
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, "publish_locator", attempt)) {
                return false;
            }
        }
        return false;
    }

    bool erase_locator(const std::string_view locator_digest_hex) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked("erase_locator")) {
            return false;
        }
        if (!is_digest(locator_digest_hex)) {
            last_error_ = "locator erase requires a valid digest";
            return false;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            Statement statement;
            auto result = statement.prepare(
                database_, "DELETE FROM artifact_locators WHERE locator_digest_hex=?");
            if (result == SQLITE_OK) {
                result = bind_text(statement.get(), 1, locator_digest_hex);
            }
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
            }
            if (result == SQLITE_DONE) {
                const auto changed = sqlite3_changes(database_) != 0;
                clear_error_locked();
                return changed;
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, "erase_locator", attempt)) {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t count() {
        return aggregate({"count", "SELECT count(*) FROM artifacts"});
    }

    [[nodiscard]] std::uint64_t total_bytes() {
        return aggregate({"total_bytes", "SELECT coalesce(sum(artifact_bytes),0) FROM artifacts"});
    }

    [[nodiscard]] std::vector<ArtifactIndexRecord> oldest(const std::size_t limit) {
        std::scoped_lock lock(mutex_);
        std::vector<ArtifactIndexRecord> records;
        if (!require_ready_locked("oldest")) {
            return records;
        }
        if (limit == 0) {
            clear_error_locked();
            return records;
        }
        if (limit > kMaxOldestLimit) {
            last_error_ = "oldest limit exceeds 100000 records";
            return records;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            records.clear();
            Statement statement;
            const auto sql = std::string("SELECT ") + std::string(kSelectColumns) +
                             " FROM artifacts ORDER BY last_access_unix_ns,digest_hex LIMIT ?";
            auto result = statement.prepare(database_, sql);
            if (result == SQLITE_OK) {
                result = sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(limit));
            }
            while (result == SQLITE_OK || result == SQLITE_ROW) {
                result = sqlite3_step(statement.get());
                if (result == SQLITE_ROW) {
                    auto record = read_record(statement.get());
                    if (!record) {
                        result = SQLITE_CORRUPT;
                        break;
                    }
                    records.push_back(std::move(*record));
                    continue;
                }
                break;
            }
            if (result == SQLITE_DONE) {
                clear_error_locked();
                return records;
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, "oldest", attempt)) {
                records.clear();
                return records;
            }
        }
        return records;
    }

    [[nodiscard]] bool ready() const {
        std::scoped_lock lock(mutex_);
        return ready_;
    }

    [[nodiscard]] bool recovered_corruption() const {
        std::scoped_lock lock(mutex_);
        return recovered_corruption_;
    }

    [[nodiscard]] std::string last_error() const {
        std::scoped_lock lock(mutex_);
        return last_error_;
    }

  private:
    [[nodiscard]] bool open_locked(const bool allow_recovery) {
        ready_ = false;
        std::error_code directory_error;
        const auto parent = database_path_.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directory_error);
        }
        if (directory_error) {
            last_error_ = "failed to create SQLite index directory: " + directory_error.message();
            return false;
        }

        const auto encoded_path = path_to_utf8(database_path_);
        auto result = sqlite3_open_v2(
            encoded_path.c_str(), &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (result != SQLITE_OK) {
            set_sqlite_error_locked("failed to open SQLite artifact index", result);
            close_locked();
            return false;
        }
        static_cast<void>(sqlite3_extended_result_codes(database_, 1));
        result = sqlite3_busy_timeout(database_, kBusyTimeoutMs);
        if (result == SQLITE_OK) {
            result = execute_locked("PRAGMA trusted_schema=OFF");
        }
        if (result == SQLITE_OK) {
            result = require_wal_locked();
        }
        if (result == SQLITE_OK) {
            // The index is disposable metadata rebuilt from validated VVT artifacts. Avoid one
            // durable media flush per thumbnail; corruption recovery deletes and rebuilds it.
            result = execute_locked("PRAGMA synchronous=OFF");
        }
        if (result != SQLITE_OK) {
            const auto message = current_sqlite_message_locked();
            if (allow_recovery && recoverable_sqlite_error(result, message)) {
                return recover_locked("SQLite configuration is corrupt or unreadable");
            }
            set_sqlite_error_locked("failed to configure SQLite artifact index", result);
            close_locked();
            return false;
        }

        int version{};
        result = query_integer_locked("PRAGMA user_version", version);
        bool table_exists{};
        if (result == SQLITE_OK) {
            result = query_exists_locked(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name='artifacts'",
                table_exists);
        }
        if (result != SQLITE_OK) {
            const auto message = current_sqlite_message_locked();
            if (allow_recovery && recoverable_sqlite_error(result, message)) {
                return recover_locked("SQLite schema metadata is corrupt");
            }
            set_sqlite_error_locked("failed to inspect SQLite artifact schema", result);
            close_locked();
            return false;
        }

        if (version == 0 && !table_exists) {
            result = create_schema_locked();
        } else if (version != kSchemaVersion || !table_exists || !schema_is_valid_locked(result)) {
            if (result != SQLITE_OK &&
                !recoverable_sqlite_error(result, current_sqlite_message_locked())) {
                set_sqlite_error_locked("failed to validate SQLite artifact schema", result);
                close_locked();
                return false;
            }
            if (allow_recovery) {
                return recover_locked("SQLite artifact schema is incompatible");
            }
            last_error_ = "new SQLite artifact index has an incompatible schema";
            close_locked();
            return false;
        }

        if (result != SQLITE_OK) {
            const auto message = current_sqlite_message_locked();
            if (allow_recovery && recoverable_sqlite_error(result, message)) {
                return recover_locked("SQLite artifact schema creation failed after corruption");
            }
            set_sqlite_error_locked("failed to create SQLite artifact schema", result);
            close_locked();
            return false;
        }
        ready_ = true;
        clear_error_locked();
        return true;
    }

    void close_locked() noexcept {
        ready_ = false;
        if (database_ != nullptr) {
            static_cast<void>(sqlite3_close_v2(database_));
            database_ = nullptr;
        }
    }

    [[nodiscard]] bool recover_locked(const std::string_view reason) {
        close_locked();
        for (const auto suffix : {std::string_view{}, std::string_view{"-wal"},
                                  std::string_view{"-shm"}, std::string_view{"-journal"}}) {
            auto candidate = database_path_;
            candidate += suffix;
            std::error_code remove_error;
            static_cast<void>(std::filesystem::remove(candidate, remove_error));
            if (remove_error) {
                last_error_ = std::string(reason) +
                              "; failed to remove local index file: " + remove_error.message();
                return false;
            }
        }
        recovered_corruption_ = true;
        return open_locked(false);
    }

    [[nodiscard]] int require_wal_locked() {
        Statement statement;
        auto result = statement.prepare(database_, "PRAGMA journal_mode=WAL");
        if (result != SQLITE_OK) {
            return result;
        }
        result = sqlite3_step(statement.get());
        if (result != SQLITE_ROW) {
            return result;
        }
        const auto mode = column_text(statement.get(), {0, 16, false});
        if (!mode || *mode != "wal") {
            last_error_ = "SQLite WAL mode is unavailable for the local cache index";
            return SQLITE_CANTOPEN;
        }
        return SQLITE_OK;
    }

    [[nodiscard]] int create_schema_locked() {
        auto result = execute_locked("BEGIN IMMEDIATE");
        if (result == SQLITE_OK) {
            result = execute_locked(kCreateTableSql);
        }
        if (result == SQLITE_OK) {
            result = execute_locked(kCreateLruIndexSql);
        }
        if (result == SQLITE_OK) {
            result = execute_locked(kCreateLocatorTableSql);
        }
        if (result == SQLITE_OK) {
            result = execute_locked(kCreateLocatorArtifactIndexSql);
        }
        if (result == SQLITE_OK) {
            result = execute_locked("PRAGMA user_version=6");
        }
        if (result == SQLITE_OK) {
            result = execute_locked("COMMIT");
        } else {
            static_cast<void>(execute_locked("ROLLBACK"));
        }
        return result;
    }

    [[nodiscard]] bool schema_is_valid_locked(int &result) {
        Statement statement;
        result = statement.prepare(database_, "PRAGMA table_info(artifacts)");
        if (result != SQLITE_OK) {
            return false;
        }
        std::size_t index{};
        while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            if (index >= kExpectedColumns.size()) {
                result = SQLITE_SCHEMA;
                return false;
            }
            auto name = column_text(statement.get(), {1, 64, false});
            auto type = column_text(statement.get(), {2, 16, false});
            const auto &expected = kExpectedColumns[index];
            if (!name || !type || *name != expected.name || *type != expected.type ||
                (sqlite3_column_int(statement.get(), 3) != 0) != expected.not_null ||
                sqlite3_column_int(statement.get(), 5) != expected.primary_key) {
                result = SQLITE_SCHEMA;
                return false;
            }
            ++index;
        }
        if (result != SQLITE_DONE || index != kExpectedColumns.size()) {
            if (result == SQLITE_DONE) {
                result = SQLITE_SCHEMA;
            }
            return false;
        }
        statement.finalize();
        Statement locator_statement;
        result = locator_statement.prepare(database_, "PRAGMA table_info(artifact_locators)");
        if (result != SQLITE_OK) {
            return false;
        }
        index = 0;
        while ((result = sqlite3_step(locator_statement.get())) == SQLITE_ROW) {
            if (index >= kExpectedLocatorColumns.size()) {
                result = SQLITE_SCHEMA;
                return false;
            }
            auto name = column_text(locator_statement.get(), {1, 64, false});
            auto type = column_text(locator_statement.get(), {2, 16, false});
            const auto &expected = kExpectedLocatorColumns[index];
            if (!name || !type || *name != expected.name || *type != expected.type ||
                (sqlite3_column_int(locator_statement.get(), 3) != 0) != expected.not_null ||
                sqlite3_column_int(locator_statement.get(), 5) != expected.primary_key) {
                result = SQLITE_SCHEMA;
                return false;
            }
            ++index;
        }
        if (result != SQLITE_DONE || index != kExpectedLocatorColumns.size()) {
            if (result == SQLITE_DONE) {
                result = SQLITE_SCHEMA;
            }
            return false;
        }
        bool lru_index_exists{};
        result = query_exists_locked(
            "SELECT 1 FROM sqlite_master WHERE type='index' AND name='artifacts_lru_idx' "
            "AND tbl_name='artifacts'",
            lru_index_exists);
        if (result != SQLITE_OK || !lru_index_exists) {
            if (result == SQLITE_OK) {
                result = SQLITE_SCHEMA;
            }
            return false;
        }
        bool locator_index_exists{};
        result = query_exists_locked(
            "SELECT 1 FROM sqlite_master WHERE type='index' "
            "AND name='artifact_locators_artifact_idx' AND tbl_name='artifact_locators'",
            locator_index_exists);
        if (result != SQLITE_OK || !locator_index_exists) {
            if (result == SQLITE_OK) {
                result = SQLITE_SCHEMA;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] int
    publish_transaction_locked(const std::span<const ArtifactIndexRecord> records) {
        auto result = execute_locked("BEGIN IMMEDIATE");
        Statement statement;
        if (result == SQLITE_OK) {
            result = statement.prepare(database_, kUpsertSql);
        }
        for (const auto &record : records) {
            if (result != SQLITE_OK) {
                break;
            }
            result = bind_record(statement.get(), record);
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
            }
            if (result == SQLITE_DONE) {
                result = sqlite3_reset(statement.get());
                if (result == SQLITE_OK) {
                    result = sqlite3_clear_bindings(statement.get());
                }
            }
        }
        if (result == SQLITE_OK) {
            result = execute_locked("COMMIT");
        } else {
            static_cast<void>(execute_locked("ROLLBACK"));
        }
        return result;
    }

    [[nodiscard]] std::uint64_t aggregate(const AggregateQuery query) {
        std::scoped_lock lock(mutex_);
        if (!require_ready_locked(query.operation)) {
            return 0;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            Statement statement;
            auto result = statement.prepare(database_, query.sql);
            if (result == SQLITE_OK) {
                result = sqlite3_step(statement.get());
            }
            if (result == SQLITE_ROW && sqlite3_column_type(statement.get(), 0) == SQLITE_INTEGER) {
                const auto value = sqlite3_column_int64(statement.get(), 0);
                if (value >= 0) {
                    clear_error_locked();
                    return static_cast<std::uint64_t>(value);
                }
                result = SQLITE_CORRUPT;
            }
            statement.finalize();
            if (!retry_after_recovery_locked(result, query.operation, attempt)) {
                return 0;
            }
        }
        return 0;
    }

    [[nodiscard]] int execute_locked(const std::string_view sql) {
        char *error_message{};
        const auto result =
            sqlite3_exec(database_, std::string(sql).c_str(), nullptr, nullptr, &error_message);
        if (error_message != nullptr) {
            sqlite3_free(error_message);
        }
        return result;
    }

    [[nodiscard]] int query_integer_locked(const std::string_view sql, int &value) {
        Statement statement;
        auto result = statement.prepare(database_, sql);
        if (result == SQLITE_OK) {
            result = sqlite3_step(statement.get());
        }
        if (result == SQLITE_ROW && sqlite3_column_type(statement.get(), 0) == SQLITE_INTEGER) {
            value = sqlite3_column_int(statement.get(), 0);
            return SQLITE_OK;
        }
        return result;
    }

    [[nodiscard]] int query_exists_locked(const std::string_view sql, bool &exists) {
        Statement statement;
        auto result = statement.prepare(database_, sql);
        if (result == SQLITE_OK) {
            result = sqlite3_step(statement.get());
        }
        if (result == SQLITE_ROW) {
            exists = true;
            return SQLITE_OK;
        }
        if (result == SQLITE_DONE) {
            exists = false;
            return SQLITE_OK;
        }
        return result;
    }

    [[nodiscard]] bool retry_after_recovery_locked(const int result,
                                                   const std::string_view operation,
                                                   const int attempt) {
        const auto message = current_sqlite_message_locked();
        if (attempt == 0 && recoverable_sqlite_error(result, message) &&
            recover_locked(std::string(operation) + " detected a corrupt SQLite index")) {
            return true;
        }
        set_sqlite_error_locked(std::string(operation) + " failed", result);
        return false;
    }

    [[nodiscard]] bool require_ready_locked(const std::string_view operation) {
        if (ready_ && database_ != nullptr) {
            return true;
        }
        if (last_error_.empty()) {
            last_error_ = std::string(operation) + ": SQLite artifact index is not ready";
        }
        return false;
    }

    [[nodiscard]] std::string current_sqlite_message_locked() const {
        return database_ != nullptr ? sqlite3_errmsg(database_) : "SQLite database is closed";
    }

    void set_sqlite_error_locked(const std::string_view prefix, const int result) {
        last_error_ = std::string(prefix) + " (" + std::to_string(result) +
                      "): " + current_sqlite_message_locked();
    }

    void clear_error_locked() {
        last_error_.clear();
    }

    std::filesystem::path database_path_;
    sqlite3 *database_{};
    mutable std::mutex mutex_;
    bool ready_{};
    bool recovered_corruption_{};
    std::string last_error_;
};

SqliteArtifactIndex::SqliteArtifactIndex(std::filesystem::path database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {}

SqliteArtifactIndex::~SqliteArtifactIndex() = default;

std::optional<ArtifactIndexRecord> SqliteArtifactIndex::find(const std::string_view digest_hex) {
    return impl_->find(digest_hex);
}

bool SqliteArtifactIndex::publish(const ArtifactIndexRecord &record) {
    return impl_->publish_many(std::span<const ArtifactIndexRecord>(&record, 1));
}

bool SqliteArtifactIndex::publish_many(const std::span<const ArtifactIndexRecord> records) {
    return impl_->publish_many(records);
}

TouchResult SqliteArtifactIndex::touch(const std::string_view digest_hex,
                                       const std::int64_t access_unix_ns) {
    return impl_->touch(digest_hex, access_unix_ns);
}

bool SqliteArtifactIndex::erase(const std::string_view digest_hex) {
    return impl_->erase(digest_hex);
}

std::optional<std::string>
SqliteArtifactIndex::find_locator(const std::string_view locator_digest_hex) {
    return impl_->find_locator(locator_digest_hex);
}

bool SqliteArtifactIndex::publish_locator(const std::string_view locator_digest_hex,
                                          const std::string_view artifact_digest_hex) {
    return impl_->publish_locator(locator_digest_hex, artifact_digest_hex);
}

bool SqliteArtifactIndex::erase_locator(const std::string_view locator_digest_hex) {
    return impl_->erase_locator(locator_digest_hex);
}

bool SqliteArtifactIndex::ready() const {
    return impl_->ready();
}

bool SqliteArtifactIndex::recovered_corruption() const {
    return impl_->recovered_corruption();
}

std::string SqliteArtifactIndex::last_error() const {
    return impl_->last_error();
}

std::uint64_t SqliteArtifactIndex::count() {
    return impl_->count();
}

std::uint64_t SqliteArtifactIndex::total_bytes() {
    return impl_->total_bytes();
}

std::vector<ArtifactIndexRecord> SqliteArtifactIndex::oldest(const std::size_t limit) {
    return impl_->oldest(limit);
}

} // namespace vove::cache
