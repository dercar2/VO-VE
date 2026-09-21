#pragma once

#include "runtime.hpp"

namespace vove::worker {

inline constexpr std::string_view kDocumentWorkerBuildId = "vove-stage5-mupdf-1";

[[nodiscard]] WorkerResult execute_document_job_native(NativeJobIo io, const WorkerJob &job);

} // namespace vove::worker
