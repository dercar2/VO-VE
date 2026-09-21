#pragma once

#include "runtime.hpp"

namespace vove::worker {

inline constexpr std::string_view kCdrWorkerBuildId = "vove-embedded-documents-2";

[[nodiscard]] WorkerResult execute_cdr_job_native(NativeJobIo io, const WorkerJob &job);

} // namespace vove::worker
