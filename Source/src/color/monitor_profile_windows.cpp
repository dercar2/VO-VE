#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "vove/color/color_transform.h"
#include "vove/color/monitor_profile.h"

#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace vove::color {
namespace {

class DisplayDc final {
  public:
    explicit DisplayDc(const wchar_t *device)
        : value_(CreateDCW(L"DISPLAY", device, nullptr, nullptr)) {}
    ~DisplayDc() {
        if (value_ != nullptr) {
            DeleteDC(value_);
        }
    }
    [[nodiscard]] HDC get() const noexcept {
        return value_;
    }

  private:
    HDC value_{};
};

} // namespace

std::vector<std::byte> load_monitor_icc(const std::string_view display_name_utf8) {
    std::wstring display_name;
    if (!display_name_utf8.empty()) {
        const auto required =
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, display_name_utf8.data(),
                                static_cast<int>(display_name_utf8.size()), nullptr, 0);
        if (required <= 0) {
            return {};
        }
        display_name.resize(static_cast<std::size_t>(required));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, display_name_utf8.data(),
                                static_cast<int>(display_name_utf8.size()), display_name.data(),
                                required) != required) {
            return {};
        }
    }
    const DisplayDc display(display_name.empty() ? nullptr : display_name.c_str());
    if (display.get() == nullptr) {
        return {};
    }
    DWORD characters = 32'768;
    std::vector<wchar_t> path(characters);
    if (GetICMProfileW(display.get(), &characters, path.data()) == FALSE || characters == 0) {
        return {};
    }

    std::ifstream stream(path.data(), std::ios::binary | std::ios::ate);
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
    return stream && stream.gcount() == static_cast<std::streamsize>(profile.size())
               ? profile
               : std::vector<std::byte>{};
}

std::vector<std::byte> load_primary_monitor_icc() {
    return load_monitor_icc({});
}

} // namespace vove::color
