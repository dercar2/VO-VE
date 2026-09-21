#pragma once

#include "runtime.hpp"

namespace vove::worker {

inline constexpr std::string_view kXcfWorkerBuildId = "vove-kimageformats-xcf-2";

[[nodiscard]] WorkerResult execute_xcf_job_native(NativeJobIo io, const WorkerJob &job);

} // namespace vove::worker
