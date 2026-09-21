#pragma once

#include <cstdint>

namespace vove::preview {

using JobId = std::uint64_t;
using RequestGeneration = std::uint64_t;
using SourceId = std::uint64_t;

inline constexpr std::uint32_t kDefaultCanonicalEdge = 512;
inline constexpr std::uint32_t kMaximumCanonicalEdge = 4'096;

enum class ThumbnailPriority : std::uint8_t {
    visible_selected = 0,
    visible = 1,
    folder = 2,
    nearby = 3,
    rest = 4,
};

enum class PreviewWorker : std::uint8_t {
    none = 0,
    raster = 1,
    document = 2,
    cdr = 3,
    svg = 4,
    xcf = 5,
};
enum class WorkerStartupStage : std::uint8_t {
    none = 0,
    configuration = 1,
    launch = 2,
    handshake = 3,
    compatibility = 4,
    containment = 5,
};

// Fixed identities and numeric codes only: no executable paths or process text.
struct WorkerStartupDiagnostic {
    WorkerStartupStage stage{WorkerStartupStage::none};
    PreviewWorker worker{PreviewWorker::none};
    std::uint32_t system_error{};
    std::uint32_t exit_code{};

    [[nodiscard]] constexpr bool present() const noexcept {
        return stage != WorkerStartupStage::none;
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        if (stage > WorkerStartupStage::containment || worker > PreviewWorker::xcf) {
            return false;
        }
        return present() ? worker != PreviewWorker::none
                         : worker == PreviewWorker::none && system_error == 0 && exit_code == 0;
    }
    friend bool operator==(const WorkerStartupDiagnostic &,
                           const WorkerStartupDiagnostic &) = default;
};

struct ThumbnailRequest {
    JobId job_id{};
    RequestGeneration generation{};
    SourceId source_id{};
    std::uint32_t canonical_edge{kDefaultCanonicalEdge};
    ThumbnailPriority priority{ThumbnailPriority::rest};
};

} // namespace vove::preview
