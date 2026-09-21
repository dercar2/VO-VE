#pragma once

#include "runtime.hpp"

namespace vove::worker {

inline constexpr std::string_view kRasterWorkerBuildId = "vove-raster-jpeg-3";

[[nodiscard]] WorkerResult execute_raster_job_native(NativeJobIo io, const WorkerJob &job);

} // namespace vove::worker
