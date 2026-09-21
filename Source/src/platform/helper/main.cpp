#include "../catalog_protocol.hpp"
#include "../directory_enumerator.hpp"

#include "vove/platform/read_only_source.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifdef __linux__
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace {

void use_binary_stdio() {
#ifdef _WIN32
    static_cast<void>(_setmode(_fileno(stdin), _O_BINARY));
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
#endif
}

bool arm_parent_death() {
#ifdef __linux__
    return prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && getppid() != 1;
#else
    return true;
#endif
}

vove::catalog::CatalogError catalog_error(const vove::platform::SourceOpenError &source) {
    using Source = vove::platform::SourceOpenErrorKind;
    using Target = vove::catalog::CatalogErrorKind;
    Target kind = Target::io_error;
    switch (source.kind) {
    case Source::not_found:
        kind = Target::not_found;
        break;
    case Source::access_denied:
        kind = Target::permission_denied;
        break;
    case Source::authentication_failed:
        kind = Target::authentication_required;
        break;
    case Source::timed_out:
        kind = Target::timed_out;
        break;
    case Source::disconnected:
        kind = Target::network_disconnected;
        break;
    case Source::none:
    case Source::invalid_path:
    case Source::not_regular_file:
    case Source::too_large:
    case Source::io_error:
        break;
    }
    return {.kind = kind, .message_utf8 = source.detail, .platform_code = source.platform_code};
}

} // namespace

int main() {
    using namespace vove;
    use_binary_stdio();
    if (!arm_parent_death()) {
        return 6;
    }

    try {
        std::vector<std::byte> request_payload;
        std::string protocol_error;
        if (!platform::detail::read_stream_frame(std::cin, request_payload, protocol_error)) {
            return EXIT_FAILURE;
        }

        catalog::CatalogRequest request;
        if (!platform::detail::decode_request_payload(request_payload, request, protocol_error)) {
            return 2;
        }

        // Recursive roots are validated by the no-follow enumerator, not a following revision probe.
        const auto directory_revision = request.recursive ? platform::DirectoryRevisionResult{}
                                                         : platform::query_directory_revision(request.path);
        if (!request.recursive && !directory_revision) {
            catalog::CatalogBatch batch;
            batch.generation = request.generation;
            batch.is_final = true;
            batch.error = catalog_error(directory_revision.error);
            return platform::detail::write_stream_frame(
                       std::cout, platform::detail::encode_batch_payload(batch))
                       ? EXIT_SUCCESS
                       : 3;
        }

        std::vector<core::DirectoryEntry> entries;
        entries.reserve(catalog::kCatalogBatchSize);
        bool transport_ok = true;
        std::uint32_t directories_visited{};
        const auto send = [&](const bool is_final, const bool truncated,
                              catalog::CatalogError error = {}, const bool root_failed = false) {
            catalog::CatalogBatch batch;
            batch.generation = request.generation;
            batch.entries = std::move(entries);
            batch.directory_revision_utf8 = directory_revision.revision_utf8;
            batch.is_final = is_final;
            batch.truncated = truncated;
            batch.error = std::move(error);
            batch.directories_visited = directories_visited;
            batch.root_failed = root_failed;
            const auto payload = platform::detail::encode_batch_payload(batch);
            transport_ok = platform::detail::write_stream_frame(std::cout, payload);
            entries.clear();
            entries.reserve(catalog::kCatalogBatchSize);
            return transport_ok;
        };

        if (!request.exact_entry_path.empty()) {
            const auto parent = request.exact_entry_path.lexically_normal().parent_path();
            if (parent != request.path.lexically_normal()) {
                return 2;
            }
            auto queried = platform::detail::query_entry(request.exact_entry_path);
            if (queried.error) {
                return send(true, false, std::move(queried.error)) ? EXIT_SUCCESS : 3;
            }
            if (!queried.entry) {
                return 4;
            }
            entries.push_back(std::move(*queried.entry));
            return send(true, false) ? EXIT_SUCCESS : 3;
        }

        bool first_recursive_entry = request.recursive;
        const auto result =
            platform::detail::enumerate_directory(request, [&](core::DirectoryEntry entry) {
                entries.push_back(std::move(entry));
                if (first_recursive_entry) {
                    first_recursive_entry = false;
                    return send(false, false);
                }
                return entries.size() < catalog::kCatalogBatchSize || send(false, false);
            }, [&](const std::uint32_t visited) {
                directories_visited = visited;
                return send(false, false);
            });
        if (!transport_ok) {
            return 3;
        }
        if (result.error.kind == catalog::CatalogErrorKind::cancelled) {
            return 3;
        }
        directories_visited = result.directories_visited;
        return send(true, result.truncated, result.error, result.root_failed) ? EXIT_SUCCESS : 3;
    } catch (const std::exception &) {
        return 4;
    } catch (...) {
        return 5;
    }
}
