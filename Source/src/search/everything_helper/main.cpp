#ifdef _WIN32

#define EVERYTHINGUSERAPI
#include "Everything.h"
#include "Everything_IPC.h"

#include "../search_protocol.hpp"
#include "vove/search/global_search.hpp"

#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using vove::search::SearchBatch;
using vove::search::SearchRequest;
using vove::search::SearchStatus;

constexpr DWORD nameRequestFlags = EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_PATH;

DWORD indexed_request_flags() {
    // Unindexed requested properties make Everything read the source paths, which can stall
    // the entire reply on an offline share. Older providers return FALSE for these capabilities.
    DWORD flags = nameRequestFlags;
    if (Everything_IsFileInfoIndexed(EVERYTHING_IPC_FILE_INFO_FILE_SIZE) != FALSE) {
        flags |= EVERYTHING_REQUEST_SIZE;
    }
    if (Everything_IsFileInfoIndexed(EVERYTHING_IPC_FILE_INFO_DATE_MODIFIED) != FALSE) {
        flags |= EVERYTHING_REQUEST_DATE_MODIFIED;
    }
    return flags;
}

void use_binary_stdio() {
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
}

std::wstring wide_from_utf8(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}

std::string utf8_from_wide(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring literal_search_text(const std::string &query_utf8) {
    auto query = wide_from_utf8(query_utf8);
    std::ranges::replace(query, L'"', L' ');
    return query;
}

std::wstring build_expression(const std::vector<std::string> &roots,
                              const std::string &query_utf8) {
    std::wstring expression = L"<";
    bool first = true;
    for (const auto &root_utf8 : roots) {
        const auto root = wide_from_utf8(root_utf8);
        if (root.empty()) {
            return {};
        }
        if (!first) {
            expression += L" | ";
        }
        first = false;
        // Everything 1.4 has no ancestor: function. path: narrows IPC work; the
        // boundary check below remains authoritative and rejects similarly named roots.
        expression += L"path:\"";
        expression += root;
        expression += L"\"";
    }
    // Everything 1.4 treats '*' and '?' as wildcards even inside quotes. The VO-VE field is
    // literal text, so pin that setting independently of the user's Everything preferences.
    expression += L"> nowildcards:\"";
    expression += literal_search_text(query_utf8);
    expression += L"\"";
    return expression;
}

std::wstring scope_probe_expression(const std::string &root_utf8) {
    auto root = wide_from_utf8(root_utf8);
    if (root.empty()) {
        return {};
    }
    if (root.back() != L'\\' && root.back() != L'/') {
        root.push_back(L'\\');
    }
    return L"path:\"" + root + L"\"";
}

enum class ScopeProbe : std::uint8_t {
    indexed,
    not_indexed,
    provider_error,
    query_error,
};

ScopeProbe probe_scope(const std::string &root_utf8) {
    const auto expression = scope_probe_expression(root_utf8);
    if (expression.empty()) {
        return ScopeProbe::not_indexed;
    }
    Everything_SetOffset(0);
    Everything_SetMax(1);
    Everything_SetRequestFlags(nameRequestFlags);
    Everything_SetSearchW(expression.c_str());
    if (Everything_QueryW(TRUE) == FALSE) {
        return Everything_GetLastError() == EVERYTHING_ERROR_IPC ? ScopeProbe::provider_error
                                                                 : ScopeProbe::query_error;
    }
    if (Everything_GetNumResults() != 0U) {
        return ScopeProbe::indexed;
    }
    return ScopeProbe::not_indexed;
}

std::wstring result_path(const DWORD index) {
    std::vector<wchar_t> buffer(1024U);
    auto length =
        Everything_GetResultFullPathNameW(index, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length >= buffer.size()) {
        if (length >= 32'768U) {
            return {};
        }
        buffer.resize(static_cast<std::size_t>(length) + 1U);
        length = Everything_GetResultFullPathNameW(index, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
    }
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    return {buffer.data(), length};
}

std::int64_t unix_ns(const FILETIME time) noexcept {
    constexpr std::uint64_t epochDifference100ns = 116'444'736'000'000'000ULL;
    const auto ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
                       static_cast<std::uint64_t>(time.dwLowDateTime);
    if (ticks < epochDifference100ns) {
        return 0;
    }
    const auto unix_ticks = ticks - epochDifference100ns;
    if (unix_ticks > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 100)) {
        return 0;
    }
    return static_cast<std::int64_t>(unix_ticks * 100U);
}

vove::core::DirectoryEntry make_entry(const DWORD index, const std::uint64_t id,
                                      const std::span<const std::string> roots) {
    const auto path_wide = result_path(index);
    const auto encoded_path = utf8_from_wide(path_wide);
    if (encoded_path.empty() || !vove::search::path_is_within_allowed_roots(encoded_path, roots)) {
        return {};
    }

    LARGE_INTEGER indexed_size{};
    FILETIME indexed_modified{};
    std::uint64_t size{};
    std::int64_t modified{};
    if (Everything_GetResultSize(index, &indexed_size) != FALSE && indexed_size.QuadPart >= 0) {
        size = static_cast<std::uint64_t>(indexed_size.QuadPart);
    }
    if (Everything_GetResultDateModified(index, &indexed_modified) != FALSE) {
        modified = unix_ns(indexed_modified);
    }
    // Search must remain an index-only operation. Missing optional metadata is represented by
    // zero instead of touching a possibly disconnected SMB path.

    const auto separator = encoded_path.find_last_of("/\\");
    const auto name =
        separator == std::string::npos ? encoded_path : encoded_path.substr(separator + 1U);
    if (name.empty()) {
        return {};
    }
    return {.id = id,
            .kind = Everything_IsFolderResult(index) != FALSE ? vove::core::EntryKind::directory
                                                              : vove::core::EntryKind::file,
            .state = vove::core::EntryState::metadata_ready,
            .name_utf8 = name,
            .search_key_utf8 = {},
            .path_utf8 = encoded_path,
            .source_revision_utf8 = {},
            .size_bytes = size,
            .modified_unix_ns = modified};
}

bool send_batch(const SearchBatch &batch) {
    try {
        return vove::search::protocol::write_stream_frame(
            std::cout, vove::search::protocol::encode_batch_payload(batch));
    } catch (...) {
        return false;
    }
}

int fail(const std::uint64_t generation, const SearchStatus status, std::string message) {
    SearchBatch batch;
    batch.generation = generation;
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
    const auto expression = build_expression(roots, request.query_utf8);
    if (expression.empty()) {
        return fail(request.generation, SearchStatus::invalid_request, "query is not valid UTF-8");
    }

    struct EverythingCleanup {
        ~EverythingCleanup() {
            Everything_CleanUp();
        }
    } cleanup;

    Everything_SetMatchPath(FALSE);
    Everything_SetMatchCase(FALSE);
    Everything_SetMatchWholeWord(FALSE);
    Everything_SetRegex(FALSE);
    Everything_SetSort(EVERYTHING_SORT_NAME_ASCENDING);
    Everything_SetRequestFlags(indexed_request_flags());
    Everything_SetSearchW(expression.c_str());

    std::uint64_t accepted{};
    std::uint64_t raw_offset{};
    std::uint64_t raw_total{};
    bool truncated{};
    SearchBatch batch;
    batch.generation = request.generation;
    while (accepted < request.maximum_results) {
        Everything_SetOffset(static_cast<DWORD>(raw_offset));
        Everything_SetMax(static_cast<DWORD>(vove::search::kSearchBatchSize));
        if (Everything_QueryW(TRUE) == FALSE) {
            const auto error = Everything_GetLastError();
            return fail(request.generation,
                        error == EVERYTHING_ERROR_IPC ? SearchStatus::provider_unavailable
                                                      : SearchStatus::io_error,
                        error == EVERYTHING_ERROR_IPC ? "Everything IPC is unavailable"
                                                      : "Everything query failed");
        }
        const auto count = Everything_GetNumResults();
        raw_total = Everything_GetTotResults();
        if (count == 0U) {
            break;
        }
        DWORD index{};
        for (; index < count && accepted < request.maximum_results; ++index) {
            auto entry = make_entry(index, accepted + 1U, roots);
            if (entry.path_utf8.empty()) {
                continue;
            }
            batch.entries.push_back(std::move(entry));
            ++accepted;
            if (batch.entries.size() == vove::search::kSearchBatchSize) {
                batch.total_matches = 0;
                if (!send_batch(batch)) {
                    return 4;
                }
                batch.entries.clear();
            }
        }
        if (accepted >= request.maximum_results &&
            (index < count || raw_offset + count < raw_total)) {
            truncated = true;
        }
        raw_offset += count;
        if (accepted >= request.maximum_results || raw_offset >= raw_total) {
            break;
        }
    }

    // A disconnected or empty root must never block matches from another root. Probe scopes only
    // after a completely empty query, and only through Everything IPC. No filesystem calls belong
    // on the global-search path.
    if (accepted == 0U && raw_total == 0U) {
        bool indexed_scope{};
        for (const auto &root : roots) {
            switch (probe_scope(root)) {
            case ScopeProbe::indexed:
                indexed_scope = true;
                break;
            case ScopeProbe::not_indexed:
                break;
            case ScopeProbe::provider_error:
                return fail(request.generation, SearchStatus::provider_unavailable,
                            "Everything IPC is unavailable");
            case ScopeProbe::query_error:
                return fail(request.generation, SearchStatus::io_error,
                            "Everything scope query failed");
            }
        }
        if (!indexed_scope) {
            return fail(request.generation, SearchStatus::scope_not_indexed, roots.front());
        }
    }

    batch.total_matches = accepted;
    batch.is_final = true;
    batch.truncated = truncated;
    if (!send_batch(batch)) {
        return 4;
    }
    return 0;
}

} // namespace

int main() {
    try {
        use_binary_stdio();
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
