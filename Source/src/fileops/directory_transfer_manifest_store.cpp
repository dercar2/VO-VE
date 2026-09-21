#include "vove/fileops/directory_transfer_manifest_store.hpp"

#include "vove/fileops/current_operation_lease.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vove::fileops {
namespace {

DirectoryTransferManifestStoreStatus map_status(const DurableJournalStatus status) noexcept {
    switch (status) {
    case DurableJournalStatus::success:
        return DirectoryTransferManifestStoreStatus::success;
    case DurableJournalStatus::not_found:
        return DirectoryTransferManifestStoreStatus::not_found;
    case DurableJournalStatus::corrupt:
        return DirectoryTransferManifestStoreStatus::corrupt;
    case DurableJournalStatus::payload_mismatch:
        return DirectoryTransferManifestStoreStatus::payload_mismatch;
    case DurableJournalStatus::payload_too_large:
        return DirectoryTransferManifestStoreStatus::payload_too_large;
    case DurableJournalStatus::io_error:
        return DirectoryTransferManifestStoreStatus::io_error;
    }
    return DirectoryTransferManifestStoreStatus::io_error;
}

DirectoryTransferManifestStoreResult map_result(const DurableJournalResult &result) {
    return {.status = map_status(result.status), .error = result.error, .detail_utf8 = {}};
}

DirectoryTransferManifestStoreResult invalid_manifest(std::string detail_utf8) {
    return {.status = DirectoryTransferManifestStoreStatus::invalid_manifest,
            .error = {},
            .detail_utf8 = std::move(detail_utf8)};
}

DirectoryTransferManifestStoreResult allocation_failure() {
    return {.status = DirectoryTransferManifestStoreStatus::io_error,
            .error = std::make_error_code(std::errc::not_enough_memory),
            .detail_utf8 = "directory-transfer manifest allocation failed"};
}

DirectoryTransferManifestStoreResult lease_failure(const std::error_code &error) {
    return {.status = DirectoryTransferManifestStoreStatus::io_error,
            .error = error ? error : std::make_error_code(std::errc::device_or_resource_busy),
            .detail_utf8 = "directory-transfer manifest store is busy"};
}

DirectoryTransferManifestStoreResult invalid_lease() {
    return {.status = DirectoryTransferManifestStoreStatus::io_error,
            .error = std::make_error_code(std::errc::operation_not_permitted),
            .detail_utf8 = "directory-transfer manifest lease protects another path"};
}

DirectoryTransferManifestStoreResult
as_result(const DirectoryTransferManifestStoreReadResult &result) {
    return {.status = result.status, .error = result.error, .detail_utf8 = result.detail_utf8};
}

std::optional<std::vector<std::byte>>
encode_checked(const DirectoryTransferManifest &manifest,
               DirectoryTransferManifestStoreResult &failure) {
    std::string detail;
    if (!valid_directory_transfer_manifest(manifest, detail)) {
        failure = invalid_manifest(std::move(detail));
        return std::nullopt;
    }
    try {
        return encode_directory_transfer_manifest(manifest);
    } catch (const std::length_error &error) {
        failure = {.status = DirectoryTransferManifestStoreStatus::payload_too_large,
                   .error = {},
                   .detail_utf8 = error.what()};
    } catch (const std::bad_alloc &) {
        failure = allocation_failure();
    } catch (const std::exception &error) {
        failure = invalid_manifest(error.what());
    }
    return std::nullopt;
}

DirectoryTransferManifestStoreReadResult decode_generation(const DurableJournalReadResult &loaded) {
    DirectoryTransferManifestStoreReadResult result;
    result.status = map_status(loaded.status);
    result.error = loaded.error;
    result.source = loaded.source;
    if (!loaded.ok()) {
        return result;
    }

    std::string detail;
    const auto decode_status =
        decode_directory_transfer_manifest_status(loaded.payload, result.manifest, detail);
    if (decode_status != DirectoryTransferManifestDecodeStatus::success) {
        result.status = decode_status == DirectoryTransferManifestDecodeStatus::incompatible_version
                            ? DirectoryTransferManifestStoreStatus::incompatible_version
                            : DirectoryTransferManifestStoreStatus::corrupt;
        result.source = DurableJournalReadSource::none;
        result.detail_utf8 = std::move(detail);
        return result;
    }
    result.encoded_size = loaded.payload.size();
    const auto digest_begin =
        loaded.payload.end() - static_cast<std::ptrdiff_t>(kDirectoryTransferManifestDigestBytes);
    std::transform(digest_begin, loaded.payload.end(), result.digest.begin(),
                   [](const std::byte value) { return std::to_integer<std::uint8_t>(value); });
    return result;
}

std::optional<std::vector<std::byte>>
reencode(const DirectoryTransferManifestStoreReadResult &generation,
         DirectoryTransferManifestStoreResult &failure) {
    if (!generation.ok()) {
        failure = as_result(generation);
        return std::nullopt;
    }
    try {
        return encode_checked(generation.manifest, failure);
    } catch (const std::bad_alloc &) {
        failure = allocation_failure();
        return std::nullopt;
    }
}

struct GenerationSet {
    DirectoryTransferManifestStoreReadResult primary;
    DirectoryTransferManifestStoreReadResult previous;
    bool temporary_present{};
    std::error_code error;
};

bool path_present(const std::filesystem::path &path, std::error_code &error) {
    error.clear();
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
        return false;
    }
    return !error && status.type() != std::filesystem::file_type::not_found;
}

GenerationSet inspect_generations(const DurableJournalStore &durable) {
    GenerationSet result;
    result.primary = decode_generation(durable.read_primary_generation());
    result.previous = decode_generation(durable.read_previous_generation());
    result.temporary_present = path_present(durable.temporary_path(), result.error);
    return result;
}

struct MatchSummary {
    std::size_t exact_count{};
    bool different{};
    bool corrupt{};
    bool incompatible{};
    std::error_code error;
    std::string detail_utf8;
};

void classify_generation(const DirectoryTransferManifestStoreReadResult &generation,
                         const std::span<const std::byte> expected, MatchSummary &summary) {
    switch (generation.status) {
    case DirectoryTransferManifestStoreStatus::success: {
        DirectoryTransferManifestStoreResult failure;
        const auto actual = reencode(generation, failure);
        if (!actual) {
            summary.error = failure.error;
            summary.corrupt = !failure.error;
            summary.detail_utf8 = failure.detail_utf8;
        } else if (std::ranges::equal(*actual, expected)) {
            ++summary.exact_count;
        } else {
            summary.different = true;
        }
        break;
    }
    case DirectoryTransferManifestStoreStatus::not_found:
        break;
    case DirectoryTransferManifestStoreStatus::corrupt:
    case DirectoryTransferManifestStoreStatus::invalid_manifest:
        summary.corrupt = true;
        if (summary.detail_utf8.empty()) {
            summary.detail_utf8 = generation.detail_utf8;
        }
        break;
    case DirectoryTransferManifestStoreStatus::incompatible_version:
        summary.incompatible = true;
        if (summary.detail_utf8.empty()) {
            summary.detail_utf8 = generation.detail_utf8;
        }
        break;
    case DirectoryTransferManifestStoreStatus::io_error:
        if (!summary.error) {
            summary.error = generation.error;
        }
        break;
    case DirectoryTransferManifestStoreStatus::payload_mismatch:
        summary.different = true;
        break;
    case DirectoryTransferManifestStoreStatus::payload_too_large:
        summary.corrupt = true;
        break;
    }
}

MatchSummary classify(const GenerationSet &generations, const std::span<const std::byte> expected) {
    MatchSummary result;
    if (generations.error) {
        result.error = generations.error;
        return result;
    }
    classify_generation(generations.primary, expected, result);
    classify_generation(generations.previous, expected, result);
    return result;
}

DirectoryTransferManifestStoreResult classification_failure(const MatchSummary &summary) {
    if (summary.error) {
        return {.status = DirectoryTransferManifestStoreStatus::io_error,
                .error = summary.error,
                .detail_utf8 = summary.detail_utf8};
    }
    if (summary.different) {
        return {.status = DirectoryTransferManifestStoreStatus::payload_mismatch,
                .error = {},
                .detail_utf8 = "directory-transfer manifest generations disagree"};
    }
    if (summary.incompatible) {
        return {.status = DirectoryTransferManifestStoreStatus::incompatible_version,
                .error = {},
                .detail_utf8 = summary.detail_utf8.empty()
                                   ? "directory-transfer manifest version is incompatible"
                                   : summary.detail_utf8};
    }
    return {.status = DirectoryTransferManifestStoreStatus::corrupt,
            .error = {},
            .detail_utf8 = summary.detail_utf8.empty()
                               ? "directory-transfer manifest generations are corrupt"
                               : summary.detail_utf8};
}

DirectoryTransferManifestStoreReadResult read_consistent(const DurableJournalStore &durable) {
    const auto generations = inspect_generations(durable);
    if (generations.error) {
        DirectoryTransferManifestStoreReadResult result;
        result.status = DirectoryTransferManifestStoreStatus::io_error;
        result.error = generations.error;
        return result;
    }

    if (generations.primary.ok() && generations.previous.ok()) {
        DirectoryTransferManifestStoreResult first_failure;
        DirectoryTransferManifestStoreResult second_failure;
        const auto first = reencode(generations.primary, first_failure);
        const auto second = reencode(generations.previous, second_failure);
        if (!first || !second) {
            DirectoryTransferManifestStoreReadResult result;
            const auto &failure = !first ? first_failure : second_failure;
            result.status = failure.error ? DirectoryTransferManifestStoreStatus::io_error
                                          : DirectoryTransferManifestStoreStatus::corrupt;
            result.error = failure.error;
            result.detail_utf8 = failure.detail_utf8;
            return result;
        }
        if (*first != *second) {
            DirectoryTransferManifestStoreReadResult result;
            result.status = DirectoryTransferManifestStoreStatus::payload_mismatch;
            result.detail_utf8 = "directory-transfer manifest generations disagree";
            return result;
        }
        return generations.primary;
    }
    if (generations.primary.status == DirectoryTransferManifestStoreStatus::incompatible_version ||
        generations.previous.status == DirectoryTransferManifestStoreStatus::incompatible_version) {
        if (generations.primary.status == DirectoryTransferManifestStoreStatus::io_error) {
            return generations.primary;
        }
        if (generations.previous.status == DirectoryTransferManifestStoreStatus::io_error) {
            return generations.previous;
        }
        return generations.primary.status ==
                       DirectoryTransferManifestStoreStatus::incompatible_version
                   ? generations.primary
                   : generations.previous;
    }
    if (generations.primary.ok()) {
        return generations.primary;
    }
    if (generations.previous.ok()) {
        return generations.previous;
    }

    DirectoryTransferManifestStoreReadResult result;
    const auto select_error = [&]() -> const DirectoryTransferManifestStoreReadResult * {
        if (generations.primary.status == DirectoryTransferManifestStoreStatus::io_error) {
            return &generations.primary;
        }
        if (generations.previous.status == DirectoryTransferManifestStoreStatus::io_error) {
            return &generations.previous;
        }
        if (generations.primary.status ==
            DirectoryTransferManifestStoreStatus::incompatible_version) {
            return &generations.primary;
        }
        if (generations.previous.status ==
            DirectoryTransferManifestStoreStatus::incompatible_version) {
            return &generations.previous;
        }
        if (generations.primary.status == DirectoryTransferManifestStoreStatus::corrupt) {
            return &generations.primary;
        }
        if (generations.previous.status == DirectoryTransferManifestStoreStatus::corrupt) {
            return &generations.previous;
        }
        return nullptr;
    };
    if (const auto *failure = select_error()) {
        return *failure;
    }
    result.status = DirectoryTransferManifestStoreStatus::not_found;
    return result;
}

} // namespace

DirectoryTransferManifestStore::DirectoryTransferManifestStore(std::filesystem::path path)
    : durable_(std::move(path), kMaximumDirectoryTransferManifestBytes) {
    static_assert(kMaximumDirectoryTransferManifestBytes ==
                  kAbsoluteMaximumDurableJournalPayloadBytes);
}

DirectoryTransferManifestStoreResult
DirectoryTransferManifestStore::persist_immutable(const DirectoryTransferManifest &manifest) const {
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(durable_.primary_path(), lease_error);
    if (!lease.owns_lock()) {
        return lease_failure(lease_error);
    }
    return persist_immutable_locked(manifest, lease);
}

DirectoryTransferManifestStoreResult
DirectoryTransferManifestStore::persist_immutable_locked(const DirectoryTransferManifest &manifest,
                                                         const CurrentOperationLease &lease) const {
    if (!lease.protects(durable_.primary_path())) {
        return invalid_lease();
    }
    DirectoryTransferManifestStoreResult failure;
    std::optional<std::vector<std::byte>> encoded;
    try {
        encoded = encode_checked(manifest, failure);
    } catch (const std::bad_alloc &) {
        return allocation_failure();
    }
    if (!encoded) {
        return failure;
    }

    for (std::size_t attempt{}; attempt <= 2U; ++attempt) {
        const auto generations = inspect_generations(durable_);
        const auto summary = classify(generations, *encoded);
        if (summary.error || summary.different || summary.incompatible ||
            (summary.corrupt && summary.exact_count == 0U)) {
            return classification_failure(summary);
        }
        if (summary.exact_count == 2U && !summary.corrupt && !generations.temporary_present) {
            return {.status = DirectoryTransferManifestStoreStatus::success,
                    .error = {},
                    .detail_utf8 = {}};
        }
        if (attempt == 2U) {
            return {.status = DirectoryTransferManifestStoreStatus::corrupt,
                    .error = {},
                    .detail_utf8 = "directory-transfer manifest redundancy could not be repaired"};
        }
        const auto written = durable_.write(*encoded);
        if (!written.ok()) {
            return map_result(written);
        }
    }
    return {.status = DirectoryTransferManifestStoreStatus::corrupt,
            .error = {},
            .detail_utf8 = "directory-transfer manifest publish did not converge"};
}

DirectoryTransferManifestStoreReadResult DirectoryTransferManifestStore::read() const {
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(durable_.primary_path(), lease_error);
    if (!lease.owns_lock()) {
        DirectoryTransferManifestStoreReadResult result;
        const auto failure = lease_failure(lease_error);
        result.status = failure.status;
        result.error = failure.error;
        result.detail_utf8 = failure.detail_utf8;
        return result;
    }
    return read_locked(lease);
}

DirectoryTransferManifestStoreReadResult
DirectoryTransferManifestStore::read_locked(const CurrentOperationLease &lease) const {
    if (!lease.protects(durable_.primary_path())) {
        DirectoryTransferManifestStoreReadResult result;
        const auto failure = invalid_lease();
        result.status = failure.status;
        result.error = failure.error;
        result.detail_utf8 = failure.detail_utf8;
        return result;
    }
    return read_consistent(durable_);
}

DirectoryTransferManifestStoreResult
DirectoryTransferManifestStore::remove_if_matches(const DirectoryTransferManifest &manifest) const {
    std::error_code lease_error;
    auto lease = CurrentOperationLease::try_acquire(durable_.primary_path(), lease_error);
    if (!lease.owns_lock()) {
        return lease_failure(lease_error);
    }
    return remove_if_matches_locked(manifest, lease);
}

DirectoryTransferManifestStoreResult
DirectoryTransferManifestStore::remove_if_matches_locked(const DirectoryTransferManifest &manifest,
                                                         const CurrentOperationLease &lease) const {
    if (!lease.protects(durable_.primary_path())) {
        return invalid_lease();
    }
    DirectoryTransferManifestStoreResult failure;
    std::optional<std::vector<std::byte>> encoded;
    try {
        encoded = encode_checked(manifest, failure);
    } catch (const std::bad_alloc &) {
        return allocation_failure();
    }
    if (!encoded) {
        return failure;
    }

    const auto generations = inspect_generations(durable_);
    const auto summary = classify(generations, *encoded);
    if (summary.error || summary.different || summary.incompatible || summary.corrupt) {
        return classification_failure(summary);
    }
    if (generations.temporary_present) {
        return {.status = DirectoryTransferManifestStoreStatus::payload_mismatch,
                .error = {},
                .detail_utf8 = "directory-transfer manifest has an uncommitted temporary record"};
    }
    if (summary.exact_count == 0U) {
        return {.status = DirectoryTransferManifestStoreStatus::success,
                .error = {},
                .detail_utf8 = {}};
    }

    const auto removed = durable_.remove_if_payload_matches(*encoded);
    if (!removed.ok()) {
        return map_result(removed);
    }
    const auto after = inspect_generations(durable_);
    if (after.error) {
        return {.status = DirectoryTransferManifestStoreStatus::io_error,
                .error = after.error,
                .detail_utf8 = {}};
    }
    if (after.primary.status != DirectoryTransferManifestStoreStatus::not_found ||
        after.previous.status != DirectoryTransferManifestStoreStatus::not_found ||
        after.temporary_present) {
        return {.status = DirectoryTransferManifestStoreStatus::payload_mismatch,
                .error = {},
                .detail_utf8 = "directory-transfer manifest changed while it was removed"};
    }
    return {
        .status = DirectoryTransferManifestStoreStatus::success, .error = {}, .detail_utf8 = {}};
}

const std::filesystem::path &DirectoryTransferManifestStore::path() const noexcept {
    return durable_.primary_path();
}

std::size_t DirectoryTransferManifestStore::maximum_payload_bytes() const noexcept {
    return durable_.maximum_payload_bytes();
}

} // namespace vove::fileops
