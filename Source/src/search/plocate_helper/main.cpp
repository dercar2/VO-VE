#ifndef _WIN32

#include "../search_protocol.hpp"
#include "vove/core/directory_model.hpp"
#include "vove/search/global_search.hpp"

#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <clocale>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using vove::search::SearchBatch;
using vove::search::SearchRequest;
using vove::search::SearchStatus;

constexpr std::size_t maximumRawResults = 1'000'000;
constexpr std::size_t maximumProviderPathBytes = 1U << 16U;
constexpr std::size_t maximumProviderDiagnosticBytes = 4096U;

std::string environment_value(const char *name) {
    const auto *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
}

std::optional<std::string> find_executable(std::string name) {
    if (name.empty()) {
        name = "plocate";
    }
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? std::optional<std::string>(std::move(name))
                                               : std::nullopt;
    }
    const auto path = environment_value("PATH");
    std::size_t offset{};
    while (offset <= path.size()) {
        const auto separator = path.find(':', offset);
        const auto directory = path.substr(offset, separator - offset);
        const auto candidate =
            (directory.empty() ? std::filesystem::path(".") : std::filesystem::path(directory)) /
            name;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
        if (separator == std::string::npos) {
            break;
        }
        offset = separator + 1U;
    }
    return std::nullopt;
}

std::int64_t timestamp_ns(const struct stat &metadata) noexcept {
    constexpr auto billion = std::int64_t{1'000'000'000};
    if (metadata.st_mtim.tv_sec > std::numeric_limits<std::int64_t>::max() / billion ||
        metadata.st_mtim.tv_sec < std::numeric_limits<std::int64_t>::min() / billion) {
        return 0;
    }
    return static_cast<std::int64_t>(metadata.st_mtim.tv_sec) * billion +
           static_cast<std::int64_t>(metadata.st_mtim.tv_nsec);
}

std::int64_t database_timestamp(const std::string &database) noexcept {
    struct stat metadata{};
    return stat(database.c_str(), &metadata) == 0 ? timestamp_ns(metadata) : 0;
}

std::optional<std::string> select_utf8_locale() {
    const auto try_locale = [](const char *name) -> std::optional<std::string> {
        const auto *resolved = std::setlocale(LC_CTYPE, name);
        if (resolved == nullptr) {
            return std::nullopt;
        }
        auto normalized = std::string(resolved);
        std::ranges::transform(normalized, normalized.begin(), [](const char character) {
            const auto value = static_cast<unsigned char>(character);
            return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
        });
        if (normalized.find("utf-8") == std::string::npos &&
            normalized.find("utf8") == std::string::npos) {
            return std::nullopt;
        }
        return std::string(resolved);
    };
    if (auto inherited = try_locale("")) {
        return inherited;
    }
    for (const auto *candidate : {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8"}) {
        if (auto selected = try_locale(candidate)) {
            return selected;
        }
    }
    return std::nullopt;
}

std::string escape_glob_literal(const std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const auto character : text) {
        if (character == '\\' || character == '*' || character == '?' || character == '[' ||
            character == ']') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string descendant_pattern(const std::string &root) {
    auto pattern = escape_glob_literal(root);
    if (!pattern.ends_with('/')) {
        pattern.push_back('/');
    }
    pattern.push_back('*');
    return pattern;
}

bool name_contains_query(const std::string &path, const std::string &query) {
    const auto name = std::filesystem::path(path).filename().string();
    const auto folded_name = vove::core::make_search_key(name);
    const auto folded_query = vove::core::make_search_key(query);
    return !folded_query.empty() && folded_name.find(folded_query) != std::string::npos;
}

struct ProviderRun {
    int exit_code{};
    bool stopped_early{};
    std::string diagnostic;
};

bool provider_completed(const ProviderRun &run) noexcept {
    // plocate uses exit code 1 both for an empty result and for an error. A normal no-match run
    // has no diagnostic; actual provider errors report one on stderr.
    return run.stopped_early || run.exit_code == 0 ||
           (run.exit_code == 1 && run.diagnostic.empty());
}

ProviderRun run_plocate(const std::string &executable, const std::string &database,
                        const std::string &utf8_locale, const std::span<const std::string> patterns,
                        const std::size_t raw_limit,
                        const std::function<bool(std::string)> &accept_path) {
    int output_pipe[2]{};
    int error_pipe[2]{};
    if (pipe(output_pipe) != 0) {
        return {.exit_code = 126, .stopped_early = false, .diagnostic = {}};
    }
    if (pipe(error_pipe) != 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return {.exit_code = 126, .stopped_early = false, .diagnostic = {}};
    }
    const auto child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return {.exit_code = 126, .stopped_early = false, .diagnostic = {}};
    }
    if (child == 0) {
        const auto parent = getppid();
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) {
            _exit(126);
        }
        if (setenv("LC_ALL", utf8_locale.c_str(), 1) != 0) {
            _exit(126);
        }
        close(output_pipe[0]);
        close(error_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 || dup2(error_pipe[1], STDERR_FILENO) < 0) {
            _exit(126);
        }
        close(output_pipe[1]);
        close(error_pipe[1]);
        std::vector<std::string> arguments{executable,   "--ignore-case", "--null",
                                           "--existing", "--literal",     "--database",
                                           database,     "--limit",       std::to_string(raw_limit),
                                           "--"};
        arguments.insert(arguments.end(), patterns.begin(), patterns.end());
        std::vector<char *> argv;
        argv.reserve(arguments.size() + 1U);
        for (auto &argument : arguments) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    close(output_pipe[1]);
    close(error_pipe[1]);
    ProviderRun result;
    std::string current;
    current.reserve(1024U);
    std::size_t raw_count{};
    bool output_open = true;
    bool error_open = true;
    bool current_too_large{};
    const auto close_output = [&] {
        if (output_open) {
            close(output_pipe[0]);
            output_open = false;
        }
    };
    const auto close_error = [&] {
        if (error_open) {
            close(error_pipe[0]);
            error_open = false;
        }
    };
    while (output_open || error_open) {
        pollfd descriptors[2]{{output_open ? output_pipe[0] : -1, POLLIN, 0},
                              {error_open ? error_pipe[0] : -1, POLLIN, 0}};
        const auto ready = poll(descriptors, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            result.exit_code = 126;
            break;
        }
        if (output_open &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            if ((descriptors[0].revents & POLLNVAL) != 0) {
                result.exit_code = 126;
                close_output();
                continue;
            }
            char buffer[8192]{};
            const auto count = read(output_pipe[0], buffer, sizeof(buffer));
            if (count == 0) {
                close_output();
            } else if (count < 0) {
                if (errno != EINTR) {
                    result.exit_code = 126;
                    close_output();
                }
            } else {
                for (ssize_t index = 0; index < count; ++index) {
                    if (buffer[index] != '\0') {
                        if (current.size() < maximumProviderPathBytes) {
                            current.push_back(buffer[index]);
                        } else {
                            current_too_large = true;
                        }
                        continue;
                    }
                    ++raw_count;
                    if (!current_too_large && !current.empty() &&
                        !accept_path(std::move(current))) {
                        result.stopped_early = true;
                        break;
                    }
                    current.clear();
                    current_too_large = false;
                    if (raw_count >= raw_limit) {
                        result.stopped_early = true;
                        break;
                    }
                }
            }
            if (result.stopped_early) {
                break;
            }
        }
        if (error_open && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            if ((descriptors[1].revents & POLLNVAL) != 0) {
                result.exit_code = 126;
                close_error();
                continue;
            }
            char buffer[1024]{};
            const auto count = read(error_pipe[0], buffer, sizeof(buffer));
            if (count == 0) {
                close_error();
            } else if (count < 0) {
                if (errno != EINTR) {
                    result.exit_code = 126;
                    close_error();
                }
            } else if (result.diagnostic.size() < maximumProviderDiagnosticBytes) {
                const auto available = maximumProviderDiagnosticBytes - result.diagnostic.size();
                result.diagnostic.append(
                    buffer, std::min<std::size_t>(static_cast<std::size_t>(count), available));
            }
        }
    }
    if (!result.stopped_early && (!current.empty() || current_too_large)) {
        result.exit_code = 126;
    }
    if (result.stopped_early) {
        static_cast<void>(kill(child, SIGTERM));
    }
    close_output();
    close_error();
    int status{};
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (result.exit_code == 0) {
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (!result.stopped_early) {
            result.exit_code = 126;
        }
    }
    return result;
}

bool directory_has_entries(const std::string &root, bool &available) {
    std::error_code error;
    const std::filesystem::directory_iterator iterator(std::filesystem::path(root), error);
    available = !error;
    return available && iterator != std::filesystem::directory_iterator{};
}

enum class ScopeProbe : std::uint8_t {
    indexed,
    empty,
    not_indexed,
    unavailable,
    provider_error,
};

ScopeProbe probe_scope(const std::string &executable, const std::string &database,
                       const std::string &utf8_locale, const std::string &root) {
    bool found{};
    const std::vector<std::string> patterns{descendant_pattern(root)};
    const auto run =
        run_plocate(executable, database, utf8_locale, patterns, 1U, [&](std::string path) {
            struct stat metadata{};
            if (lstat(path.c_str(), &metadata) == 0 &&
                (S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode)) &&
                vove::search::path_is_within_allowed_roots(path, std::span(&root, 1U))) {
                found = true;
                return false;
            }
            return true;
        });
    if (!provider_completed(run)) {
        return ScopeProbe::provider_error;
    }
    if (found) {
        return ScopeProbe::indexed;
    }
    bool available{};
    const auto has_entries = directory_has_entries(root, available);
    if (!available) {
        return ScopeProbe::unavailable;
    }
    return has_entries ? ScopeProbe::not_indexed : ScopeProbe::empty;
}

std::optional<vove::core::DirectoryEntry> make_entry(const std::string &path,
                                                     const std::uint64_t id) {
    struct stat metadata{};
    if (lstat(path.c_str(), &metadata) != 0 ||
        (!S_ISREG(metadata.st_mode) && !S_ISDIR(metadata.st_mode))) {
        return std::nullopt;
    }
    const auto name = std::filesystem::path(path).filename().string();
    if (name.empty()) {
        return std::nullopt;
    }
    return vove::core::DirectoryEntry{
        .id = id,
        .kind = S_ISDIR(metadata.st_mode) ? vove::core::EntryKind::directory
                                          : vove::core::EntryKind::file,
        .state = vove::core::EntryState::metadata_ready,
        .name_utf8 = name,
        .search_key_utf8 = {},
        .path_utf8 = path,
        .source_revision_utf8 = {},
        .size_bytes = S_ISREG(metadata.st_mode) && metadata.st_size > 0
                          ? static_cast<std::uint64_t>(metadata.st_size)
                          : 0U,
        .modified_unix_ns = timestamp_ns(metadata)};
}

bool send_batch(const SearchBatch &batch) {
    try {
        return vove::search::protocol::write_stream_frame(
            std::cout, vove::search::protocol::encode_batch_payload(batch));
    } catch (...) {
        return false;
    }
}

int fail(const std::uint64_t generation, const SearchStatus status, std::string message,
         const std::int64_t index_timestamp = 0) {
    SearchBatch batch;
    batch.generation = generation;
    batch.provider_index_modified_unix_ns = index_timestamp;
    batch.status = status;
    batch.message_utf8 = std::move(message);
    batch.is_final = true;
    static_cast<void>(send_batch(batch));
    return status == SearchStatus::provider_unavailable ? 3 : 2;
}

int run(const SearchRequest &request) {
    const auto roots = vove::search::normalize_allowed_roots(request.allowed_roots_utf8);
    if (request.query_utf8.empty() || roots.empty() || request.maximum_results == 0U) {
        return fail(request.generation, SearchStatus::invalid_request,
                    "query and allowed roots are required");
    }
    const auto executable = find_executable(environment_value("VOVE_PLOCATE_EXECUTABLE"));
    if (!executable) {
        return fail(request.generation, SearchStatus::provider_unavailable,
                    "plocate executable is unavailable");
    }
    auto database = environment_value("VOVE_PLOCATE_DATABASE");
    if (database.empty()) {
        database = "/var/lib/plocate/plocate.db";
    }
    const auto index_timestamp = database_timestamp(database);
    if (index_timestamp == 0) {
        return fail(request.generation, SearchStatus::provider_unavailable,
                    "plocate database is unavailable");
    }
    const auto utf8_locale = select_utf8_locale();
    if (!utf8_locale) {
        return fail(request.generation, SearchStatus::provider_unavailable,
                    "a UTF-8 locale is unavailable", index_timestamp);
    }
    std::vector<std::string> indexed_roots;
    indexed_roots.reserve(roots.size());
    std::optional<std::string> first_not_indexed;
    std::optional<std::string> first_unavailable;
    for (const auto &root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::path(root), error) || error) {
            if (!first_unavailable) {
                first_unavailable = root;
            }
            continue;
        }
        switch (probe_scope(*executable, database, *utf8_locale, root)) {
        case ScopeProbe::indexed:
        case ScopeProbe::empty:
            indexed_roots.push_back(root);
            break;
        case ScopeProbe::not_indexed:
            if (!first_not_indexed) {
                first_not_indexed = root;
            }
            break;
        case ScopeProbe::unavailable:
            if (!first_unavailable) {
                first_unavailable = root;
            }
            break;
        case ScopeProbe::provider_error:
            return fail(request.generation, SearchStatus::provider_unavailable,
                        "plocate query failed", index_timestamp);
        }
    }
    if (indexed_roots.empty()) {
        if (first_not_indexed) {
            return fail(request.generation, SearchStatus::scope_not_indexed, *first_not_indexed,
                        index_timestamp);
        }
        return fail(request.generation, SearchStatus::io_error,
                    "allowed root is unavailable: " + first_unavailable.value_or(roots.front()),
                    index_timestamp);
    }

    SearchBatch batch;
    batch.generation = request.generation;
    batch.provider_index_modified_unix_ns = index_timestamp;
    std::unordered_set<std::string> emitted;
    std::uint64_t accepted{};
    bool truncated{};
    bool output_failed{};
    for (const auto &root : indexed_roots) {
        const auto raw_limit = std::min(
            maximumRawResults, std::max<std::size_t>(4096U, request.maximum_results * 32U + 1U));
        const std::vector<std::string> patterns{descendant_pattern(root),
                                                escape_glob_literal(request.query_utf8)};
        const auto provider = run_plocate(
            *executable, database, *utf8_locale, patterns, raw_limit, [&](std::string path) {
                if (!vove::search::path_is_within_allowed_roots(path, std::span(&root, 1U)) ||
                    !name_contains_query(path, request.query_utf8) || emitted.contains(path)) {
                    return true;
                }
                auto entry = make_entry(path, accepted + 1U);
                if (!entry) {
                    return true;
                }
                if (accepted >= request.maximum_results) {
                    truncated = true;
                    return false;
                }
                emitted.insert(path);
                batch.entries.push_back(std::move(*entry));
                ++accepted;
                if (batch.entries.size() == vove::search::kSearchBatchSize) {
                    if (!send_batch(batch)) {
                        output_failed = true;
                        return false;
                    }
                    batch.entries.clear();
                }
                return true;
            });
        if (!provider_completed(provider)) {
            return fail(request.generation, SearchStatus::provider_unavailable,
                        "plocate query failed", index_timestamp);
        }
        if (output_failed) {
            return 4;
        }
        if (provider.stopped_early && accepted < request.maximum_results) {
            truncated = true;
        }
        if (truncated || accepted >= request.maximum_results) {
            break;
        }
    }

    batch.total_matches = accepted;
    batch.is_final = true;
    batch.truncated = truncated;
    return send_batch(batch) ? 0 : 4;
}

} // namespace

int main() {
    try {
        std::ios::sync_with_stdio(false);
        std::vector<std::byte> payload;
        std::string error;
        SearchRequest request;
        if (!vove::search::protocol::read_stream_frame(std::cin, payload, error) ||
            !vove::search::protocol::decode_request_payload(payload, request, error)) {
            return fail(request.generation, SearchStatus::invalid_request, std::move(error));
        }
        return run(request);
    } catch (...) {
        return 5;
    }
}

#else
int main() {
    return 1;
}
#endif
