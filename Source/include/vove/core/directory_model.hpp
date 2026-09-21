#pragma once

#include "vove/core/directory_entry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vove::core {

enum class SortDirection : std::uint8_t {
    ascending,
    descending,
};

enum class SortField : std::uint8_t {
    name,
    extension,
    modified,
    size,
};

enum class FilterCase : std::uint8_t {
    insensitive,
    sensitive,
};

class DirectoryModel {
  public:
    using Generation = std::uint64_t;

    // The model is intentionally single-writer. Background listers publish batches
    // to the UI thread, which applies them only when the generation still matches.
    [[nodiscard]] Generation begin_generation();
    [[nodiscard]] std::size_t
    matching_count(std::span<const DirectoryEntry> entries) const noexcept;
    [[nodiscard]] bool
    append_reorders_visible_rows(std::span<const DirectoryEntry> entries) const noexcept;
    [[nodiscard]] bool append_batch(Generation generation, std::span<const DirectoryEntry> entries);
    [[nodiscard]] bool replace_generation(Generation generation,
                                          std::vector<DirectoryEntry> entries);
    [[nodiscard]] bool finish_generation(Generation generation);
    [[nodiscard]] bool abort_generation(Generation generation);

    void set_filter_key(std::string filter_key_utf8);
    void set_sort(SortField field, SortDirection direction);
    void set_sort_field(SortField field);
    void set_sort_direction(SortDirection direction);
    void set_filter_case(FilterCase filter_case);

    [[nodiscard]] Generation generation() const noexcept;
    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] std::size_t total_size() const noexcept;
    [[nodiscard]] std::size_t visible_size() const noexcept;
    [[nodiscard]] const DirectoryEntry &visible_at(std::size_t index) const;
    [[nodiscard]] const std::vector<DirectoryEntry> &entries() const noexcept;
    [[nodiscard]] const std::vector<std::size_t> &visible_indices() const noexcept;
    [[nodiscard]] std::string_view filter_key() const noexcept;

    [[nodiscard]] FilterCase filter_case() const noexcept;
    [[nodiscard]] SortField sort_field() const noexcept;
    [[nodiscard]] SortDirection sort_direction() const noexcept;

  private:
    [[nodiscard]] bool matches_filter(const DirectoryEntry &entry) const noexcept;
    void rebuild_visible_indices();
    std::array<std::size_t, 2> partition_visible_groups();
    void sort_visible_indices();

    Generation generation_{};
    bool loading_{false};
    SortField sort_field_{SortField::name};
    SortDirection sort_direction_{SortDirection::ascending};
    std::string filter_key_utf8_;
    FilterCase filter_case_{FilterCase::insensitive};
    std::vector<DirectoryEntry> entries_;
    std::vector<std::size_t> visible_indices_;
};

// The production key performs deterministic Unicode NFKC case folding without
// importing a UI toolkit into the core. Invalid UTF-8 falls back to conservative
// byte-preserving simple lowercasing. The old helper name remains as a Gate A alias.
[[nodiscard]] std::string make_search_key(std::string_view utf8);
[[nodiscard]] std::string make_ascii_search_key(std::string_view utf8);

} // namespace vove::core
