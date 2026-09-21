#pragma once

#include "runtime.hpp"

namespace vove::worker {

inline constexpr std::string_view kSvgWorkerBuildId = "vove-stage10d-resvg-1";

[[nodiscard]] WorkerResult execute_svg_job_native(NativeJobIo io, const WorkerJob &job);

} // namespace vove::worker
