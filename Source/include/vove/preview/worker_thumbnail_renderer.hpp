#pragma once

#include "vove/preview/thumbnail_pipeline.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace vove::worker {
struct SupervisorResult;
}

namespace vove::preview {

[[nodiscard]] WorkerStartupDiagnostic
classify_worker_startup_failure(const worker::SupervisorResult &result,
                                PreviewWorker worker) noexcept;

struct WorkerThumbnailRendererOptions {
    std::filesystem::path worker_executable;
    std::string expected_build_id;
    std::filesystem::path document_worker_executable;
    std::string document_expected_build_id;
    std::filesystem::path cdr_worker_executable;
    std::string cdr_expected_build_id;
    std::filesystem::path svg_worker_executable;
    std::string svg_expected_build_id;
    std::string svg_font_environment_fingerprint;
    std::filesystem::path xcf_worker_executable;
    std::string xcf_expected_build_id;
    std::filesystem::path ghostscript_executable;
    std::filesystem::path fallback_cmyk_profile;
    std::chrono::milliseconds timeout{10'000};
    bool force_bounded_fingerprint{};
    std::chrono::milliseconds selected_document_timeout{30'000};
};

struct WorkerThumbnailBackend {
    ThumbnailSourceResolver resolve_source;
    ThumbnailRenderer render;
    ThumbnailColorPolicyResolver resolve_color_policy;
};

[[nodiscard]] WorkerThumbnailBackend
make_worker_thumbnail_renderer(WorkerThumbnailRendererOptions options);

} // namespace vove::preview
