#pragma once

#include "vove/fileops/file_operation.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace vove::fileops {

enum class BatchDateTimeSource : std::uint8_t {
    modified,
    current,
};

enum class BatchDateFormat : std::uint8_t {
    yyyy_mm_dd,
    yyyymmdd,
    dd_mm_yyyy,
};

enum class BatchTimeFormat : std::uint8_t {
    hh_mm,
    hhmm,
    hh_mm_ss,
};

enum class BatchExtensionMode : std::uint8_t {
    preserve,
    replace,
};

enum class BatchRenameIssue : std::uint8_t {
    none,
    empty_selection,
    mixed_directories,
    invalid_source,
    invalid_template,
    unknown_token,
    counter_overflow,
    invalid_name,
    duplicate_destination,
    occupied_destination,
};

struct BatchRenameSource {
    std::filesystem::path path;
    std::uint64_t size_bytes{};
    std::int64_t modified_unix_ns{};
    std::string source_revision_utf8;
    OperationObjectKind object_kind{OperationObjectKind::regular_file};
};

struct BatchRenameOptions {
    std::string name_template_utf8{"{name}_{counter}"};
    std::uint64_t counter_start{1};
    std::uint64_t counter_step{1};
    std::uint8_t counter_padding{1};
    BatchDateTimeSource date_time_source{BatchDateTimeSource::modified};
    BatchDateFormat date_format{BatchDateFormat::yyyy_mm_dd};
    BatchTimeFormat time_format{BatchTimeFormat::hh_mm};
    BatchExtensionMode extension_mode{BatchExtensionMode::preserve};
    std::string replacement_extension_utf8;
    std::int64_t current_unix_ns{};
};

struct BatchRenameRow {
    std::filesystem::path source;
    std::filesystem::path destination;
    BatchRenameIssue issue{BatchRenameIssue::none};
    std::string detail_utf8;

    [[nodiscard]] bool valid() const noexcept {
        return issue == BatchRenameIssue::none;
    }
};

struct BatchRenamePlan {
    BatchRenameIssue issue{BatchRenameIssue::none};
    std::string detail_utf8;
    std::vector<BatchRenameRow> rows;

    [[nodiscard]] bool valid() const noexcept {
        return issue == BatchRenameIssue::none;
    }
};

// `directory_names` is the caller's current catalog snapshot. Planning deliberately performs no
// filesystem I/O so live preview remains responsive on SMB; execution revalidates physically.
[[nodiscard]] BatchRenamePlan
plan_batch_rename(const std::vector<BatchRenameSource> &sources,
                  const std::vector<std::filesystem::path> &directory_names,
                  const BatchRenameOptions &options);

} // namespace vove::fileops
