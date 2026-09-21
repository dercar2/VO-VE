#include "vove/core/directory_model.hpp"

#include "vove/core/catalog_visibility.hpp"

#include "utf8proc.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace vove::core {
namespace {

enum class CatalogGroup { directory, preview, other };

int compare_natural_names(const std::string_view lhs, const std::string_view rhs) noexcept {
    const auto digit = [](const char value) { return value >= '0' && value <= '9'; };
    std::size_t left{};
    std::size_t right{};
    int padding_order{};
    while (left < lhs.size() && right < rhs.size()) {
        if (digit(lhs[left]) && digit(rhs[right])) {
            auto left_end = left;
            auto right_end = right;
            while (left_end < lhs.size() && digit(lhs[left_end])) {
                ++left_end;
            }
            while (right_end < rhs.size() && digit(rhs[right_end])) {
                ++right_end;
            }
            auto left_significant = left;
            auto right_significant = right;
            while (left_significant < left_end && lhs[left_significant] == '0') {
                ++left_significant;
            }
            while (right_significant < right_end && rhs[right_significant] == '0') {
                ++right_significant;
            }
            const auto left_digits = left_end - left_significant;
            const auto right_digits = right_end - right_significant;
            if (left_digits != right_digits) {
                return left_digits < right_digits ? -1 : 1;
            }
            // Compare arbitrary-length integers without conversion or temporary allocations.
            const auto number_order = lhs.substr(left_significant, left_digits)
                                          .compare(rhs.substr(right_significant, right_digits));
            if (number_order != 0) {
                return number_order;
            }
            const auto left_width = left_end - left;
            const auto right_width = right_end - right;
            if (padding_order == 0 && left_width != right_width) {
                padding_order = left_width < right_width ? -1 : 1;
            }
            left = left_end;
            right = right_end;
        } else {
            const auto left_byte = static_cast<unsigned char>(lhs[left]);
            const auto right_byte = static_cast<unsigned char>(rhs[right]);
            if (left_byte != right_byte) {
                return left_byte < right_byte ? -1 : 1;
            }
            ++left;
            ++right;
        }
    }
    if (left != lhs.size() || right != rhs.size()) {
        return left == lhs.size() ? -1 : 1;
    }
    // Padding only breaks a tie after the complete name has been compared naturally.
    return padding_order;
}

CatalogGroup catalog_group(const DirectoryEntry &entry) noexcept {
    if (entry.kind == EntryKind::directory) {
        return CatalogGroup::directory;
    }
    const auto dot = entry.name_utf8.find_last_of('.');
    if (dot == std::string::npos || dot == 0) {
        return CatalogGroup::other;
    }
    const auto suffix = std::string_view(entry.name_utf8).substr(dot + 1);
    std::array<char, 10> lower{};
    if (suffix.empty() || suffix.size() > lower.size()) {
        return CatalogGroup::other;
    }
    for (std::size_t index = 0; index < lower.size() && index < suffix.size(); ++index) {
        const auto value = suffix[index];
        lower[index] =
            value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    }
    // A filename hint for stable catalog ordering, not a substitute for decoder signature checks.
    static constexpr std::array<std::string_view, 49> extensions{
        "af",   "afdesign", "afphoto", "afpub", "aftemplate", "ai",   "arw",  "avif", "bmp",  "cdr",
        "cr2",  "cr3",      "dng",     "dxf",   "eps",        "gif",  "heic", "heif", "hpgl", "ico",
        "idml", "indd",     "indt",    "jfif",  "jpe",        "jpeg", "jpg",  "jxl",  "kra",  "nef",
        "nrw",  "ora",      "orf",     "pdf",   "pef",        "plt",  "png",  "ps",   "psb",  "psd",
        "raf",  "rw2",      "srw",     "svg",   "svgz",       "tif",  "tiff", "webp", "xcf"};
    static_assert(std::ranges::is_sorted(extensions));
    return std::binary_search(extensions.begin(), extensions.end(),
                              std::string_view(lower.data(), suffix.size()))
               ? CatalogGroup::preview
               : CatalogGroup::other;
}

} // namespace

DirectoryModel::Generation DirectoryModel::begin_generation() {
    ++generation_;
    loading_ = true;
    entries_.clear();
    visible_indices_.clear();
    return generation_;
}

std::size_t
DirectoryModel::matching_count(const std::span<const DirectoryEntry> entries) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(), [this](const DirectoryEntry &entry) {
            return visible_in_preview_catalog(entry) && matches_filter(entry);
        }));
}

bool DirectoryModel::append_reorders_visible_rows(
    const std::span<const DirectoryEntry> entries) const noexcept {
    if (visible_indices_.empty()) {
        return false;
    }
    const auto last_group = catalog_group(entries_[visible_indices_.back()]);
    return std::ranges::any_of(entries, [this, last_group](const DirectoryEntry &entry) {
        return visible_in_preview_catalog(entry) && matches_filter(entry) &&
               catalog_group(entry) < last_group;
    });
}

bool DirectoryModel::append_batch(const Generation generation,
                                  const std::span<const DirectoryEntry> entries) {
    if (!loading_ || generation != generation_) {
        return false;
    }

    entries_.reserve(entries_.size() + entries.size());
    visible_indices_.reserve(visible_indices_.size() + entries.size());
    auto last_group = visible_indices_.empty() ? CatalogGroup::directory
                                               : catalog_group(entries_[visible_indices_.back()]);
    bool regroup{};
    for (const auto &entry : entries) {
        if (!visible_in_preview_catalog(entry)) {
            continue;
        }
        entries_.push_back(entry);
        if (matches_filter(entries_.back())) {
            visible_indices_.push_back(entries_.size() - 1);
            const auto group = catalog_group(entry);
            regroup = regroup || group < last_group;
            last_group = group;
        }
    }
    if (regroup) {
        partition_visible_groups();
    }
    return true;
}

bool DirectoryModel::replace_generation(const Generation generation,
                                        std::vector<DirectoryEntry> entries) {
    if (!loading_ || generation != generation_) {
        return false;
    }
    std::erase_if(entries,
                  [](const DirectoryEntry &entry) { return !visible_in_preview_catalog(entry); });
    entries_ = std::move(entries);
    visible_indices_.clear();
    visible_indices_.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (matches_filter(entries_[index])) {
            visible_indices_.push_back(index);
        }
    }
    partition_visible_groups();
    return true;
}

bool DirectoryModel::finish_generation(const Generation generation) {
    if (!loading_ || generation != generation_) {
        return false;
    }
    loading_ = false;
    sort_visible_indices();
    return true;
}

bool DirectoryModel::abort_generation(const Generation generation) {
    if (!loading_ || generation != generation_) {
        return false;
    }
    loading_ = false;
    return true;
}

void DirectoryModel::set_filter_key(std::string filter_key_utf8) {
    if (filter_key_utf8_ == filter_key_utf8) {
        return;
    }
    filter_key_utf8_ = std::move(filter_key_utf8);
    rebuild_visible_indices();
}

void DirectoryModel::set_filter_case(const FilterCase filter_case) {
    if (filter_case_ == filter_case) {
        return;
    }
    filter_case_ = filter_case;
    rebuild_visible_indices();
}

void DirectoryModel::set_sort(const SortField field, const SortDirection direction) {
    if (sort_field_ == field && sort_direction_ == direction) {
        return;
    }
    sort_field_ = field;
    sort_direction_ = direction;
    if (!loading_) {
        sort_visible_indices();
    }
}

void DirectoryModel::set_sort_field(const SortField field) {
    if (sort_field_ == field) {
        return;
    }
    sort_field_ = field;
    if (!loading_) {
        sort_visible_indices();
    }
}

void DirectoryModel::set_sort_direction(const SortDirection direction) {
    if (sort_direction_ == direction) {
        return;
    }
    sort_direction_ = direction;
    if (!loading_) {
        sort_visible_indices();
    }
}

DirectoryModel::Generation DirectoryModel::generation() const noexcept {
    return generation_;
}

bool DirectoryModel::loading() const noexcept {
    return loading_;
}

std::size_t DirectoryModel::total_size() const noexcept {
    return entries_.size();
}

std::size_t DirectoryModel::visible_size() const noexcept {
    return visible_indices_.size();
}

const DirectoryEntry &DirectoryModel::visible_at(const std::size_t index) const {
    if (index >= visible_indices_.size()) {
        throw std::out_of_range("DirectoryModel visible index is out of range");
    }
    return entries_[visible_indices_[index]];
}

const std::vector<DirectoryEntry> &DirectoryModel::entries() const noexcept {
    return entries_;
}

const std::vector<std::size_t> &DirectoryModel::visible_indices() const noexcept {
    return visible_indices_;
}

std::string_view DirectoryModel::filter_key() const noexcept {
    return filter_key_utf8_;
}

FilterCase DirectoryModel::filter_case() const noexcept {
    return filter_case_;
}

SortField DirectoryModel::sort_field() const noexcept {
    return sort_field_;
}

SortDirection DirectoryModel::sort_direction() const noexcept {
    return sort_direction_;
}

bool DirectoryModel::matches_filter(const DirectoryEntry &entry) const noexcept {
    if (filter_key_utf8_.empty()) {
        return true;
    }
    if (filter_case_ == FilterCase::sensitive) {
        return entry.name_utf8.find(filter_key_utf8_) != std::string::npos;
    }
    return entry.search_key_utf8.find(filter_key_utf8_) != std::string::npos;
}

void DirectoryModel::rebuild_visible_indices() {
    visible_indices_.clear();
    visible_indices_.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (matches_filter(entries_[index])) {
            visible_indices_.push_back(index);
        }
    }
    if (loading_) {
        partition_visible_groups();
    } else {
        sort_visible_indices();
    }
}

std::array<std::size_t, 2> DirectoryModel::partition_visible_groups() {
    const auto first_file = std::stable_partition(
        visible_indices_.begin(), visible_indices_.end(),
        [this](const std::size_t index) { return entries_[index].kind == EntryKind::directory; });
    const auto first_other =
        std::stable_partition(first_file, visible_indices_.end(), [this](const std::size_t index) {
            return catalog_group(entries_[index]) == CatalogGroup::preview;
        });
    return {static_cast<std::size_t>(first_file - visible_indices_.begin()),
            static_cast<std::size_t>(first_other - visible_indices_.begin())};
}

void DirectoryModel::sort_visible_indices() {
    const auto extension = [](const std::string &name) -> std::string_view {
        const auto dot = name.find_last_of('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) {
            return {};
        }
        return std::string_view(name).substr(dot + 1);
    };
    const auto compare = [](const auto &lhs, const auto &rhs) {
        if (lhs < rhs) {
            return -1;
        }
        if (rhs < lhs) {
            return 1;
        }
        return 0;
    };
    const auto less = [this, &compare, &extension](const std::size_t lhs_index,
                                                   const std::size_t rhs_index) {
        const auto &lhs = entries_[lhs_index];
        const auto &rhs = entries_[rhs_index];

        int key_order{};
        switch (sort_field_) {
        case SortField::name:
            key_order = compare_natural_names(lhs.search_key_utf8, rhs.search_key_utf8);
            break;
        case SortField::extension:
            key_order = extension(lhs.search_key_utf8).compare(extension(rhs.search_key_utf8));
            break;
        case SortField::modified:
            key_order = compare(lhs.modified_unix_ns, rhs.modified_unix_ns);
            break;
        case SortField::size:
            key_order = compare(lhs.size_bytes, rhs.size_bytes);
            break;
        }
        if (key_order != 0) {
            return sort_direction_ == SortDirection::ascending ? key_order < 0 : key_order > 0;
        }
        if (sort_field_ != SortField::name) {
            const auto name_order = compare_natural_names(lhs.search_key_utf8, rhs.search_key_utf8);
            if (name_order != 0) {
                return sort_direction_ == SortDirection::ascending ? name_order < 0 : name_order > 0;
            }
        }
        return sort_direction_ == SortDirection::ascending ? lhs.id < rhs.id : lhs.id > rhs.id;
    };

    // Classify once per entry, not on every comparison in large catalogs.
    const auto boundaries = partition_visible_groups();
    auto first = visible_indices_.begin();
    for (const auto end : {boundaries[0], boundaries[1], visible_indices_.size()}) {
        auto last = visible_indices_.begin() + static_cast<std::ptrdiff_t>(end);
        std::stable_sort(first, last, less);
        first = last;
    }
}

namespace {

struct DecodedCodepoint {
    std::size_t next;
    std::uint32_t codepoint;
};

std::optional<DecodedCodepoint> decode_utf8(const std::string_view text, const std::size_t start) {
    const auto first = static_cast<std::uint8_t>(text[start]);
    if (first < 0x80U) {
        return DecodedCodepoint{start + 1, first};
    }

    std::size_t length{};
    std::uint32_t value{};
    std::uint32_t minimum{};
    if ((first & 0xE0U) == 0xC0U) {
        length = 2;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return std::nullopt;
    }

    if (start + length > text.size()) {
        return std::nullopt;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<std::uint8_t>(text[start + offset]);
        if ((continuation & 0xC0U) != 0x80U) {
            return std::nullopt;
        }
        value = (value << 6U) | (continuation & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
        return std::nullopt;
    }

    return DecodedCodepoint{start + length, value};
}

std::uint32_t simple_lower(const std::uint32_t codepoint) {
    if (codepoint >= U'A' && codepoint <= U'Z') {
        return codepoint + 0x20U;
    }
    if ((codepoint >= 0x00C0U && codepoint <= 0x00D6U) ||
        (codepoint >= 0x00D8U && codepoint <= 0x00DEU)) {
        return codepoint + 0x20U;
    }
    if (codepoint >= 0x0391U && codepoint <= 0x03ABU && codepoint != 0x03A2U) {
        return codepoint + 0x20U;
    }
    if (codepoint >= 0x0400U && codepoint <= 0x040FU) {
        return codepoint + 0x50U;
    }
    if (codepoint >= 0x0410U && codepoint <= 0x042FU) {
        return codepoint + 0x20U;
    }
    return codepoint;
}

void append_utf8(std::string &output, const std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

} // namespace

std::string make_search_key(const std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }

    if (utf8.size() <= static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
        utf8proc_uint8_t *normalized = nullptr;
        constexpr auto options =
            static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT |
                                           UTF8PROC_CASEFOLD | UTF8PROC_IGNORE);
        const auto length =
            utf8proc_map(reinterpret_cast<const utf8proc_uint8_t *>(utf8.data()),
                         static_cast<utf8proc_ssize_t>(utf8.size()), &normalized, options);
        if (length >= 0 && normalized != nullptr) {
            std::string result(reinterpret_cast<const char *>(normalized),
                               static_cast<std::size_t>(length));
            std::free(normalized);
            return result;
        }
        std::free(normalized);
    }

    // Filesystems can expose byte sequences that are not valid UTF-8. Keep those
    // entries searchable instead of dropping them from the catalog.
    std::string result;
    result.reserve(utf8.size());
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        const auto start = offset;
        const auto decoded = decode_utf8(utf8, start);
        if (!decoded.has_value()) {
            result.push_back(utf8[start]);
            offset = start + 1;
            continue;
        }
        offset = decoded->next;
        append_utf8(result, simple_lower(decoded->codepoint));
    }
    return result;
}

std::string make_ascii_search_key(const std::string_view utf8) {
    return make_search_key(utf8);
}

} // namespace vove::core
