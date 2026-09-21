#include "vove/fileops/batch_rename.hpp"

#include "batch_rename_detail.hpp"

#include "vove/core/reserved_names.hpp"
#include "vove/fileops/file_operation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace vove::fileops {
namespace {

std::string path_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
    const auto *first = reinterpret_cast<const char8_t *>(text.data());
    return std::filesystem::path(std::u8string(first, first + text.size()));
}

std::string filename_utf8(const std::filesystem::path &path) {
    return path_utf8(path.filename());
}

} // namespace

std::string detail::batch_rename_collision_key(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto native = path.filename().native();
    const auto required =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(),
                      static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr, 0);
    if (required > 0) {
        std::wstring folded(static_cast<std::size_t>(required), L'\0');
        if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, native.data(),
                          static_cast<int>(native.size()), folded.data(), required, nullptr,
                          nullptr, 0) == required) {
            return path_utf8(std::filesystem::path(folded));
        }
    }
#endif
    auto key = filename_utf8(path);
    std::ranges::transform(key, key.begin(), [](const unsigned char value) {
        if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
            return static_cast<char>(value - static_cast<unsigned char>('A') +
                                     static_cast<unsigned char>('a'));
        }
        return static_cast<char>(value);
    });
    return key;
}

namespace {

using detail::batch_rename_collision_key;

std::optional<std::tm> local_time(const std::int64_t unix_ns) {
    using namespace std::chrono;
    const auto seconds = duration_cast<std::chrono::seconds>(nanoseconds(unix_ns));
    const auto raw = static_cast<std::time_t>(seconds.count());
    std::tm value{};
#ifdef _WIN32
    if (localtime_s(&value, &raw) != 0) {
        return std::nullopt;
    }
#else
    if (localtime_r(&raw, &value) == nullptr) {
        return std::nullopt;
    }
#endif
    return value;
}

struct PaddedNumber {
    std::uint64_t value{};
    unsigned int width{};
};

void append_padded(std::string &output, const PaddedNumber number) {
    std::array<char, 24> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), number.value);
    if (error != std::errc{}) {
        return;
    }
    const auto length = static_cast<unsigned int>(end - buffer.data());
    if (length < number.width) {
        output.append(number.width - length, '0');
    }
    output.append(buffer.data(), end);
}

std::string format_date(const std::tm &value, const BatchDateFormat format) {
    std::string output;
    output.reserve(10);
    const auto year = static_cast<unsigned int>(value.tm_year + 1900);
    const auto month = static_cast<unsigned int>(value.tm_mon + 1);
    const auto day = static_cast<unsigned int>(value.tm_mday);
    switch (format) {
    case BatchDateFormat::yyyy_mm_dd:
        append_padded(output, {year, 4});
        output.push_back('-');
        append_padded(output, {month, 2});
        output.push_back('-');
        append_padded(output, {day, 2});
        break;
    case BatchDateFormat::yyyymmdd:
        append_padded(output, {year, 4});
        append_padded(output, {month, 2});
        append_padded(output, {day, 2});
        break;
    case BatchDateFormat::dd_mm_yyyy:
        append_padded(output, {day, 2});
        output.push_back('-');
        append_padded(output, {month, 2});
        output.push_back('-');
        append_padded(output, {year, 4});
        break;
    }
    return output;
}

std::string format_time(const std::tm &value, const BatchTimeFormat format) {
    std::string output;
    output.reserve(8);
    const auto hour = static_cast<unsigned int>(value.tm_hour);
    const auto minute = static_cast<unsigned int>(value.tm_min);
    const auto second = static_cast<unsigned int>(value.tm_sec);
    append_padded(output, {hour, 2});
    if (format != BatchTimeFormat::hhmm) {
        output.push_back('-');
    }
    append_padded(output, {minute, 2});
    if (format == BatchTimeFormat::hh_mm_ss) {
        output.push_back('-');
        append_padded(output, {second, 2});
    }
    return output;
}

std::optional<std::uint64_t> counter_at(const BatchRenameOptions &options,
                                        const std::size_t index) {
    if (options.counter_step != 0 &&
        index > (std::numeric_limits<std::uint64_t>::max() - options.counter_start) /
                    options.counter_step) {
        return std::nullopt;
    }
    return options.counter_start + static_cast<std::uint64_t>(index) * options.counter_step;
}

struct ExpandedName {
    std::string value;
    BatchRenameIssue issue{BatchRenameIssue::none};
    std::string detail;
};

ExpandedName expand_name(const BatchRenameSource &source, const BatchRenameOptions &options,
                         const std::size_t index) {
    if (options.name_template_utf8.empty()) {
        return {{}, BatchRenameIssue::invalid_template, "name template is empty"};
    }
    if (options.counter_padding == 0 || options.counter_padding > 20) {
        return {{}, BatchRenameIssue::invalid_template, "counter padding must be between 1 and 20"};
    }
    if (options.counter_step == 0) {
        return {{}, BatchRenameIssue::invalid_template, "counter step must be at least one"};
    }
    const auto counter = counter_at(options, index);
    if (!counter.has_value()) {
        return {{}, BatchRenameIssue::counter_overflow, "counter value overflows"};
    }
    const auto timestamp = options.date_time_source == BatchDateTimeSource::modified
                               ? source.modified_unix_ns
                               : options.current_unix_ns;
    const auto time = local_time(timestamp);
    if (!time.has_value()) {
        return {{}, BatchRenameIssue::invalid_source, "date or time is outside supported range"};
    }

    const auto stem = path_utf8(source.object_kind == OperationObjectKind::directory
                                    ? source.path.filename()
                                    : source.path.filename().stem());
    std::string counter_text;
    append_padded(counter_text, {*counter, options.counter_padding});
    const auto date = format_date(*time, options.date_format);
    const auto clock = format_time(*time, options.time_format);

    std::string output;
    output.reserve(options.name_template_utf8.size() + stem.size() + 32);
    for (std::size_t offset{}; offset < options.name_template_utf8.size();) {
        const auto value = options.name_template_utf8[offset];
        if (value == '{' && offset + 1 < options.name_template_utf8.size() &&
            options.name_template_utf8[offset + 1] == '{') {
            output.push_back('{');
            offset += 2;
            continue;
        }
        if (value == '}' && offset + 1 < options.name_template_utf8.size() &&
            options.name_template_utf8[offset + 1] == '}') {
            output.push_back('}');
            offset += 2;
            continue;
        }
        if (value != '{') {
            if (value == '}') {
                return {{},
                        BatchRenameIssue::invalid_template,
                        "name template contains an unmatched closing brace"};
            }
            output.push_back(value);
            ++offset;
            continue;
        }
        const auto close = options.name_template_utf8.find('}', offset + 1);
        if (close == std::string::npos) {
            return {{},
                    BatchRenameIssue::invalid_template,
                    "name template contains an unmatched opening brace"};
        }
        const auto token =
            std::string_view(options.name_template_utf8).substr(offset + 1, close - offset - 1);
        if (token == "name") {
            output += stem;
        } else if (token == "counter") {
            output += counter_text;
        } else if (token == "date") {
            output += date;
        } else if (token == "time") {
            output += clock;
        } else {
            return {{}, BatchRenameIssue::unknown_token, "name template contains an unknown token"};
        }
        offset = close + 1;
    }
    return {std::move(output), BatchRenameIssue::none, {}};
}

std::string replacement_extension(const BatchRenameSource &source,
                                  const BatchRenameOptions &options) {
    if (source.object_kind == OperationObjectKind::directory) {
        return {};
    }
    if (options.extension_mode == BatchExtensionMode::preserve) {
        return path_utf8(source.path.extension());
    }
    if (options.replacement_extension_utf8.empty()) {
        return {};
    }
    return '.' + options.replacement_extension_utf8;
}

void mark_issue(BatchRenamePlan &plan, BatchRenameRow &row, const BatchRenameIssue issue,
                std::string detail) {
    if (row.issue == BatchRenameIssue::none) {
        row.issue = issue;
        row.detail_utf8 = detail;
    }
    if (plan.issue == BatchRenameIssue::none) {
        plan.issue = issue;
        plan.detail_utf8 = std::move(detail);
    }
}

} // namespace

BatchRenamePlan plan_batch_rename(const std::vector<BatchRenameSource> &sources,
                                  const std::vector<std::filesystem::path> &directory_names,
                                  const BatchRenameOptions &options) {
    BatchRenamePlan plan;
    plan.rows.reserve(sources.size());
    if (sources.empty()) {
        plan.issue = BatchRenameIssue::empty_selection;
        plan.detail_utf8 = "batch rename selection is empty";
        return plan;
    }

    const auto parent = sources.front().path.parent_path().lexically_normal();
    std::unordered_map<std::string, std::size_t> source_keys;
    std::unordered_map<std::string, std::size_t> source_identity_keys;
    std::unordered_map<std::string, bool> occupied_keys;
    for (const auto &name : directory_names) {
        occupied_keys.emplace(batch_rename_collision_key(name.filename()), true);
    }
    for (std::size_t index{}; index < sources.size(); ++index) {
        BatchRenameRow row;
        row.source = sources[index].path;
        if (!sources[index].path.is_absolute() || sources[index].path.filename().empty() ||
            core::is_internal_filename(sources[index].path.filename().u8string()) ||
            (sources[index].object_kind != OperationObjectKind::regular_file &&
             sources[index].object_kind != OperationObjectKind::directory) ||
            (sources[index].object_kind == OperationObjectKind::directory &&
             sources[index].size_bytes != 0)) {
            mark_issue(plan, row, BatchRenameIssue::invalid_source,
                       "batch rename source path is invalid");
        } else if (sources[index].path.parent_path().lexically_normal() != parent) {
            mark_issue(plan, row, BatchRenameIssue::mixed_directories,
                       "batch rename sources must share one directory");
        }
        const auto source_key = batch_rename_collision_key(sources[index].path.filename());
        if (!source_keys.emplace(source_key, index).second) {
            mark_issue(plan, row, BatchRenameIssue::invalid_source,
                       "batch rename contains the same source more than once");
        }
        const auto source_identity = stable_object_identity(sources[index].source_revision_utf8);
        if (source_identity.empty()) {
            mark_issue(plan, row, BatchRenameIssue::invalid_source,
                       "batch rename source has no stable identity");
        } else if (!source_identity_keys.emplace(source_identity, index).second) {
            mark_issue(plan, row, BatchRenameIssue::invalid_source,
                       "batch rename contains two paths to the same physical file");
        }

        const auto expanded = expand_name(sources[index], options, index);
        if (expanded.issue != BatchRenameIssue::none) {
            mark_issue(plan, row, expanded.issue, expanded.detail);
        } else {
            const auto generated =
                path_from_utf8(expanded.value + replacement_extension(sources[index], options));
            std::string detail;
            if (sources[index].object_kind == OperationObjectKind::regular_file &&
                options.extension_mode == BatchExtensionMode::replace &&
                options.replacement_extension_utf8.starts_with('.')) {
                mark_issue(plan, row, BatchRenameIssue::invalid_name,
                           "replacement extension must not start with a dot");
            } else if (!valid_destination_filename(generated, detail)) {
                mark_issue(plan, row, BatchRenameIssue::invalid_name, std::move(detail));
            } else if (core::is_internal_filename(generated.u8string())) {
                mark_issue(plan, row, BatchRenameIssue::invalid_name,
                           "batch destination uses a reserved VO-VE filename");
            } else {
                row.destination = sources[index].path.filename() == generated
                                      ? sources[index].path
                                      : parent / generated;
            }
        }
        plan.rows.push_back(std::move(row));
    }

    std::unordered_map<std::string, std::size_t> destination_keys;
    for (std::size_t index{}; index < plan.rows.size(); ++index) {
        auto &row = plan.rows[index];
        if (row.destination.empty()) {
            continue;
        }
        const auto key = batch_rename_collision_key(row.destination.filename());
        const auto [existing, inserted] = destination_keys.emplace(key, index);
        if (!inserted) {
            mark_issue(plan, row, BatchRenameIssue::duplicate_destination,
                       "two batch rows produce the same destination");
            mark_issue(plan, plan.rows[existing->second], BatchRenameIssue::duplicate_destination,
                       "two batch rows produce the same destination");
            continue;
        }
        if (occupied_keys.contains(key) && !source_keys.contains(key)) {
            mark_issue(plan, row, BatchRenameIssue::occupied_destination,
                       "batch destination is occupied by an unselected item");
        }
    }
    return plan;
}

} // namespace vove::fileops
