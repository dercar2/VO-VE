#include "vove/catalog/catalog_session.hpp"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <iterator>

namespace vove::catalog {

namespace {

constexpr std::array retryDelays{std::chrono::seconds(1), std::chrono::seconds(3),
                                 std::chrono::seconds(10)};

} // namespace

CatalogSession::CatalogSession(DirectorySource &source, DirectorySink &sink)
    : source_(source), sink_(sink) {}

CatalogSession::~CatalogSession() {
    if (activeGeneration_ != 0) {
        source_.cancel(activeGeneration_);
    }
}

void CatalogSession::open(std::filesystem::path path, const std::size_t maximum_entries,
                          const bool recursive, const bool preserve_visible_entries) {
    if (path.empty()) {
        throw std::invalid_argument("CatalogSession requires a non-empty path");
    }
    if (activeGeneration_ != 0) {
        source_.cancel(activeGeneration_);
    }
    finish_sink_if_open();

    path_ = std::move(path);
    maximumEntries_ = maximum_entries;
    recursive_ = recursive;
    retryIndex_ = 0;
    retryAt_ = {};
    lastError_ = {};
    truncated_ = false;
    if (!preserve_visible_entries) {
        sink_.begin_catalog();
        sinkOpen_ = true;
    }
    begin_request(preserve_visible_entries);
}

void CatalogSession::refresh() {
    if (path_.empty()) {
        return;
    }
    if (activeGeneration_ != 0) {
        source_.cancel(activeGeneration_);
    }
    finish_sink_if_open();

    retryIndex_ = 0;
    retryAt_ = {};
    lastError_ = {};
    truncated_ = false;
    begin_request(true);
}

void CatalogSession::suspend() {
    if (activeGeneration_ != 0) {
        source_.cancel(activeGeneration_);
        activeGeneration_ = 0;
    }
    abort_sink_if_open();
    stagedEntries_.clear();
    stageUntilSuccess_ = false;
    retryIndex_ = 0;
    retryAt_ = {};
    lastError_ = {};
    truncated_ = false;
    state_ = CatalogSessionState::unavailable;
}

void CatalogSession::begin_request(const bool preserve_visible_entries) {
    activeGeneration_ = ++nextGeneration_;
    stageUntilSuccess_ = preserve_visible_entries;
    stagedEntries_.clear();
    state_ = CatalogSessionState::loading;
    directoriesVisited_ = 0;
    CatalogRequest request{
        .generation = activeGeneration_, .path = path_, .maximum_entries = maximumEntries_,
        .recursive = recursive_};
    if (recursive_) {
        request.maximum_entries = recursive_entry_limit(request);
    }
    source_.submit(std::move(request));
}

void CatalogSession::finish_sink_if_open() {
    if (!sinkOpen_) {
        return;
    }
    sink_.finish_catalog();
    sinkOpen_ = false;
}

void CatalogSession::abort_sink_if_open() {
    if (!sinkOpen_) {
        return;
    }
    sink_.abort_catalog();
    sinkOpen_ = false;
}

CatalogSessionUpdate CatalogSession::poll(const std::chrono::steady_clock::time_point now) {
    auto update = make_update();

    if (state_ == CatalogSessionState::waiting_retry) {
        if (now < retryAt_) {
            update.retry_in = std::chrono::duration_cast<std::chrono::milliseconds>(retryAt_ - now);
            return update;
        }
        begin_request(true);
        update = make_update();
    }

    if (state_ != CatalogSessionState::loading) {
        return update;
    }

    std::optional<CatalogBatch> batch;
    while (auto candidate = source_.poll()) {
        if (candidate->generation == activeGeneration_) {
            batch = std::move(candidate);
            break;
        }
    }
    if (!batch) {
        return update;
    }

    const bool successfulResult = !batch->error || (recursive_ && !batch->root_failed);
    directoriesVisited_ = std::max(directoriesVisited_, batch->directories_visited);
    update.directories_visited = directoriesVisited_;
    update.entries_received = batch->entries.size();
    if (recursive_ && stageUntilSuccess_ && !batch->entries.empty()) {
        sink_.begin_catalog();
        sinkOpen_ = true;
        stageUntilSuccess_ = false;
    }
    if (stageUntilSuccess_) {
        stagedEntries_.insert(stagedEntries_.end(), std::make_move_iterator(batch->entries.begin()),
                              std::make_move_iterator(batch->entries.end()));
    } else if (!batch->entries.empty()) {
        sink_.append_catalog(std::move(batch->entries));
        update.model_changed = true;
    }
    truncated_ = truncated_ || batch->truncated || (recursive_ && bool(batch->error));
    update.truncated = truncated_;

    if (!batch->is_final) {
        return update;
    }

    if (successfulResult) {
        if (stageUntilSuccess_) {
            // An incomplete empty refresh is not evidence that the old files disappeared.
            if (!recursive_ || !truncated_) {
                sink_.replace_catalog(std::move(stagedEntries_));
                update.model_changed = true;
            }
            stageUntilSuccess_ = false;
        } else {
            finish_sink_if_open();
            update.model_changed = true;
        }
        lastError_ = {};
        state_ = CatalogSessionState::ready;
        update.state = state_;
        update.error = {};
        return update;
    }

    if (stageUntilSuccess_) {
        stagedEntries_.clear();
    } else {
        abort_sink_if_open();
        update.model_changed = true;
    }
    lastError_ = std::move(batch->error);
    update.error = lastError_;

    if (should_retry(lastError_) && retryIndex_ < retryDelays.size()) {
        const auto delay = retryDelays[retryIndex_++];
        retryAt_ = now + delay;
        state_ = CatalogSessionState::waiting_retry;
        update.state = state_;
        update.retry_in = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
        return update;
    }

    switch (lastError_.kind) {
    case CatalogErrorKind::network_disconnected:
        state_ = CatalogSessionState::network_disconnected;
        break;
    case CatalogErrorKind::timed_out:
        state_ = CatalogSessionState::timed_out;
        break;
    case CatalogErrorKind::authentication_required:
        state_ = CatalogSessionState::authentication_required;
        break;
    case CatalogErrorKind::permission_denied:
        state_ = CatalogSessionState::permission_denied;
        break;
    default:
        state_ = CatalogSessionState::unavailable;
        break;
    }
    update.state = state_;
    return update;
}

CatalogSessionUpdate CatalogSession::make_update() const {
    CatalogSessionUpdate update;
    update.state = state_;
    update.error = lastError_;
    update.truncated = truncated_;
    update.directories_visited = directoriesVisited_;
    return update;
}

bool CatalogSession::should_retry(const CatalogError &error) const noexcept {
    return !recursive_ && (error.kind == CatalogErrorKind::network_disconnected ||
           error.kind == CatalogErrorKind::timed_out || error.kind == CatalogErrorKind::io_error);
}

CatalogSessionState CatalogSession::state() const noexcept {
    return state_;
}

const CatalogError &CatalogSession::last_error() const noexcept {
    return lastError_;
}

const std::filesystem::path &CatalogSession::path() const noexcept {
    return path_;
}

} // namespace vove::catalog
