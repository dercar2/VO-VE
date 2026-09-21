#include "vove/color/monitor_profile.h"

#include "vove/color/color_transform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vove::color {
namespace {

struct XDisplay;
using XWindow = unsigned long;
using XAtom = unsigned long;

struct LibraryCloser {
    void operator()(void *library) const noexcept {
        if (library != nullptr) {
            dlclose(library);
        }
    }
};

struct DisplayCloser {
    using Close = int (*)(XDisplay *);
    Close close{};

    void operator()(XDisplay *display) const noexcept {
        if (display != nullptr && close != nullptr) {
            close(display);
        }
    }
};

struct PropertyCloser {
    using Free = int (*)(void *);
    Free free{};

    void operator()(unsigned char *property) const noexcept {
        if (property != nullptr && free != nullptr) {
            free(property);
        }
    }
};

[[nodiscard]] bool valid_icc_header(const std::vector<std::byte> &profile) noexcept {
    constexpr std::size_t minimum_icc_bytes = 128;
    constexpr std::size_t signature_offset = 36;
    if (profile.size() < minimum_icc_bytes || profile.size() > kMaximumIccProfileBytes) {
        return false;
    }
    const auto byte = [&profile](const std::size_t index) {
        return std::to_integer<unsigned char>(profile[index]);
    };
    const auto declared = (static_cast<std::uint32_t>(byte(0)) << 24U) |
                          (static_cast<std::uint32_t>(byte(1)) << 16U) |
                          (static_cast<std::uint32_t>(byte(2)) << 8U) |
                          static_cast<std::uint32_t>(byte(3));
    constexpr std::array<unsigned char, 4> signature{'a', 'c', 's', 'p'};
    return declared >= minimum_icc_bytes && declared <= profile.size() &&
           std::equal(signature.begin(), signature.end(),
                      reinterpret_cast<const unsigned char *>(profile.data()) + signature_offset);
}

[[nodiscard]] std::vector<std::byte> read_profile_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const auto length = stream.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > kMaximumIccProfileBytes ||
        static_cast<std::uint64_t>(length) > std::numeric_limits<std::size_t>::max()) {
        return {};
    }
    std::vector<std::byte> profile(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(profile.data()),
                static_cast<std::streamsize>(profile.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(profile.size()) ||
        !valid_icc_header(profile)) {
        return {};
    }
    return profile;
}

template <typename Function>
[[nodiscard]] Function load_symbol(void *library, const char *name) noexcept {
    void *symbol = dlsym(library, name);
    Function function{};
    static_assert(sizeof(function) == sizeof(symbol));
    std::memcpy(&function, &symbol, sizeof(function));
    return function;
}

[[nodiscard]] std::optional<unsigned int> monitor_index(const std::string_view monitor_key) {
    if (monitor_key.empty()) {
        return 0U;
    }
    unsigned int index{};
    const auto *begin = monitor_key.data();
    const auto *end = begin + monitor_key.size();
    const auto result = std::from_chars(begin, end, index);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return index;
}

[[nodiscard]] std::vector<std::byte> load_environment_profile(const std::string &name) {
    const auto *path = std::getenv(name.c_str());
    if (path == nullptr || path[0] == '\0') {
        return {};
    }
    return read_profile_file(std::filesystem::path(path));
}

[[nodiscard]] std::vector<std::byte> load_x11_profile(const unsigned int monitor) {
    using OpenDisplay = XDisplay *(*)(const char *);
    using CloseDisplay = int (*)(XDisplay *);
    using DefaultRootWindow = XWindow (*)(XDisplay *);
    using InternAtom = XAtom (*)(XDisplay *, const char *, int);
    using GetWindowProperty = int (*)(XDisplay *, XWindow, XAtom, long, long, int, XAtom, XAtom *,
                                      int *, unsigned long *, unsigned long *, unsigned char **);
    using Free = int (*)(void *);

    const std::unique_ptr<void, LibraryCloser> library(
        dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL));
    if (!library) {
        return {};
    }
    const auto open_display = load_symbol<OpenDisplay>(library.get(), "XOpenDisplay");
    const auto close_display = load_symbol<CloseDisplay>(library.get(), "XCloseDisplay");
    const auto default_root = load_symbol<DefaultRootWindow>(library.get(), "XDefaultRootWindow");
    const auto intern_atom = load_symbol<InternAtom>(library.get(), "XInternAtom");
    const auto get_property = load_symbol<GetWindowProperty>(library.get(), "XGetWindowProperty");
    const auto free_property = load_symbol<Free>(library.get(), "XFree");
    if (open_display == nullptr || close_display == nullptr || default_root == nullptr ||
        intern_atom == nullptr || get_property == nullptr || free_property == nullptr) {
        return {};
    }

    const std::unique_ptr<XDisplay, DisplayCloser> display(open_display(nullptr),
                                                           DisplayCloser{close_display});
    if (!display) {
        return {};
    }
    const auto property_name = monitor == 0U
                                   ? std::string{"_ICC_PROFILE"}
                                   : std::string{"_ICC_PROFILE_"} + std::to_string(monitor);
    const auto property_atom = intern_atom(display.get(), property_name.c_str(), 1);
    if (property_atom == 0) {
        return {};
    }

    XAtom actual_type{};
    int actual_format{};
    unsigned long item_count{};
    unsigned long bytes_after{};
    unsigned char *raw_property{};
    constexpr XAtom any_property_type = 0;
    constexpr long offset = 0;
    constexpr long maximum_units = static_cast<long>((kMaximumIccProfileBytes + 3U) / 4U);
    constexpr int success = 0;
    if (get_property(display.get(), default_root(display.get()), property_atom, offset,
                     maximum_units, 0, any_property_type, &actual_type, &actual_format, &item_count,
                     &bytes_after, &raw_property) != success) {
        return {};
    }
    const std::unique_ptr<unsigned char, PropertyCloser> property(raw_property,
                                                                  PropertyCloser{free_property});
    if (!property || actual_type == 0 || actual_format != 8 || bytes_after != 0 ||
        item_count < 128 || item_count > kMaximumIccProfileBytes) {
        return {};
    }
    std::vector<std::byte> profile(item_count);
    std::memcpy(profile.data(), property.get(), item_count);
    return valid_icc_header(profile) ? profile : std::vector<std::byte>{};
}

} // namespace

std::vector<std::byte> load_primary_monitor_icc() {
    return load_monitor_icc("0");
}

std::vector<std::byte> load_monitor_icc(const std::string_view monitor_key) {
    const auto index = monitor_index(monitor_key);
    if (index.has_value()) {
        if (auto profile = load_environment_profile("ICC_PROFILE_" + std::to_string(*index));
            !profile.empty()) {
            return profile;
        }
    }
    if (auto profile = load_environment_profile("ICC_PROFILE"); !profile.empty()) {
        return profile;
    }
    return index.has_value() ? load_x11_profile(*index) : std::vector<std::byte>{};
}

} // namespace vove::color
