#include "trash_dialog.hpp"

#include "vove/fileops/trash_coordinator.hpp"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace vove::ui {

struct TrashCatalogLoadState {
    std::stop_source stop;
    std::mutex mutex;
    std::optional<fileops::TrashCatalogResult> result;
    std::atomic_bool failed{false};
    std::atomic_bool finished{false};
};

namespace {

struct TrashCatalogLoadJob {
    std::shared_ptr<TrashCatalogLoadState> state;
    std::filesystem::path manifest_directory;
    std::optional<std::vector<std::filesystem::path>> recovery_roots;
};

fileops::TrashCatalogResult failed_catalog() {
    fileops::TrashCatalogResult result;
    result.status = fileops::TrashCatalogStatus::io_error;
    result.error = std::make_error_code(std::errc::io_error);
    return result;
}

fileops::TrashCatalogResult cancelled_catalog() {
    fileops::TrashCatalogResult result;
    result.status = fileops::TrashCatalogStatus::cancelled;
    return result;
}

void finish_load(const std::shared_ptr<TrashCatalogLoadState> &state,
                 fileops::TrashCatalogResult result) noexcept {
    try {
        const std::scoped_lock lock(state->mutex);
        state->result = std::move(result);
    } catch (...) {
        state->failed.store(true, std::memory_order_release);
    }
    state->finished.store(true, std::memory_order_release);
}

void run_load(TrashCatalogLoadJob job) noexcept {
    try {
#ifdef VOVE_TEST_TRASH_BLOCKING_LOADER
        if (qEnvironmentVariableIsSet("VOVE_TEST_TRASH_BLOCKING_LOADER")) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
#endif
        if (job.state->stop.stop_requested()) {
            finish_load(job.state, cancelled_catalog());
            return;
        }
        auto roots =
            job.recovery_roots
                ? fileops::TrashRecoveryRootDiscovery{.roots = std::move(*job.recovery_roots),
                                                      .discovery_failures = 0}
                : fileops::trash_known_recovery_roots(job.manifest_directory);
        auto catalog = fileops::read_trash_catalog(job.manifest_directory, roots.roots,
                                                   job.state->stop.get_token());
        if (roots.discovery_failures >
            std::numeric_limits<std::size_t>::max() - catalog.unreadable_recovery_roots) {
            catalog.unreadable_recovery_roots = std::numeric_limits<std::size_t>::max();
        } else {
            catalog.unreadable_recovery_roots += roots.discovery_failures;
        }
        finish_load(job.state, std::move(catalog));
    } catch (...) {
        finish_load(job.state, failed_catalog());
    }
}

class TrashCatalogLoaderPool final {
  public:
    static TrashCatalogLoaderPool &instance() {
        // A blocked filesystem syscall cannot be joined portably during static destruction. The
        // bounded loader is intentionally process-lifetime; the OS retires its two threads.
        static auto *pool = new TrashCatalogLoaderPool;
        return *pool;
    }

    [[nodiscard]] bool submit(TrashCatalogLoadJob job) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            std::erase_if(queue_, [](const TrashCatalogLoadJob &queued) {
                return queued.state->stop.stop_requested();
            });
            if (workers_started_ == 0 || queue_.size() >= maximum_queue_size) {
                ++rejected_jobs_;
                return false;
            }
            queue_.push_back(std::move(job));
            completed_.notify_one();
            return true;
        } catch (...) {
            std::scoped_lock lock(mutex_);
            ++rejected_jobs_;
            return false;
        }
    }

#ifdef VOVE_TEST_TRASH_BLOCKING_LOADER
    [[nodiscard]] TrashCatalogLoaderStats stats() const {
        const std::scoped_lock lock(mutex_);
        return {.workers_started = workers_started_,
                .active_jobs = active_jobs_,
                .queued_jobs = queue_.size(),
                .rejected_jobs = rejected_jobs_};
    }
#endif

  private:
    static constexpr std::size_t worker_count = 2;
    static constexpr std::size_t maximum_queue_size = 8;

    TrashCatalogLoaderPool() noexcept {
        for (std::size_t index{}; index < worker_count; ++index) {
            try {
                std::thread([this] { worker_loop(); }).detach();
                ++workers_started_;
            } catch (...) {
                break;
            }
        }
    }

    void worker_loop() noexcept {
        for (;;) {
            TrashCatalogLoadJob job;
            {
                std::unique_lock lock(mutex_);
                completed_.wait(lock, [this] { return !queue_.empty(); });
                job = std::move(queue_.front());
                queue_.pop_front();
                ++active_jobs_;
            }
            run_load(std::move(job));
            {
                const std::scoped_lock lock(mutex_);
                --active_jobs_;
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable completed_;
    std::deque<TrashCatalogLoadJob> queue_;
    std::size_t workers_started_{};
    std::size_t active_jobs_{};
    std::size_t rejected_jobs_{};
};

} // namespace

#ifdef VOVE_TEST_TRASH_BLOCKING_LOADER
TrashCatalogLoaderStats trash_catalog_loader_stats_for_test() {
    return TrashCatalogLoaderPool::instance().stats();
}
#endif

TrashCatalogLoader::~TrashCatalogLoader() {
    cancel();
}

bool TrashCatalogLoader::start(std::filesystem::path manifest_directory,
                               std::optional<std::vector<std::filesystem::path>> recovery_roots) {
    cancel();
    try {
        state_ = std::make_shared<TrashCatalogLoadState>();
        if (!TrashCatalogLoaderPool::instance().submit(
                {.state = state_,
                 .manifest_directory = std::move(manifest_directory),
                 .recovery_roots = std::move(recovery_roots)})) {
            finish_load(state_, failed_catalog());
        }
        return true;
    } catch (...) {
        state_.reset();
        return false;
    }
}

std::optional<fileops::TrashCatalogResult> TrashCatalogLoader::poll() {
    if (state_ == nullptr || !state_->finished.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::optional<fileops::TrashCatalogResult> result;
    const auto failed = state_->failed.load(std::memory_order_acquire);
    {
        const std::scoped_lock lock(state_->mutex);
        result = std::move(state_->result);
    }
    state_.reset();
    if (failed || !result) {
        return failed_catalog();
    }
    return result;
}

bool TrashCatalogLoader::loading() const noexcept {
    return state_ != nullptr && !state_->finished.load(std::memory_order_acquire);
}

void TrashCatalogLoader::cancel() noexcept {
    if (state_ != nullptr) {
        state_->stop.request_stop();
        state_.reset();
    }
}

TrashCleanupPlan make_trash_cleanup_plan(const fileops::TrashCatalogResult &catalog,
                                         const bool remove_all, const int days,
                                         const std::int64_t now_unix_ns) noexcept {
    TrashCleanupPlan plan;
    if (!catalog.ok() || days < 1 || now_unix_ns <= 0) {
        return plan;
    }
    constexpr std::int64_t nanoseconds_per_day = 86'400'000'000'000LL;
    const auto age = days > std::numeric_limits<std::int64_t>::max() / nanoseconds_per_day
                         ? std::numeric_limits<std::int64_t>::max()
                         : static_cast<std::int64_t>(days) * nanoseconds_per_day;
    const auto cutoff = age >= now_unix_ns ? 0 : now_unix_ns - age;
    for (const auto &record : catalog.manifests) {
        const auto created = record.transaction.created_unix_ns;
        if (!remove_all && (created <= 0 || created > cutoff || created > now_unix_ns)) {
            continue;
        }
        plan.manifests.push_back(record.manifest_path);
        if (record.transaction.items.size() >
            std::numeric_limits<std::size_t>::max() - plan.total_items) {
            plan.total_items = std::numeric_limits<std::size_t>::max();
            plan.totals_saturated = true;
        } else {
            plan.total_items += record.transaction.items.size();
        }
        for (const auto &item : record.transaction.items) {
            if (item.current_snapshot.size_bytes >
                std::numeric_limits<std::uint64_t>::max() - plan.total_bytes) {
                plan.total_bytes = std::numeric_limits<std::uint64_t>::max();
                plan.totals_saturated = true;
            } else {
                plan.total_bytes += item.current_snapshot.size_bytes;
            }
        }
    }
    return plan;
}

TrashCleanupDialog::TrashCleanupDialog(const fileops::TrashCatalogResult &catalog, QWidget *parent)
    : QDialog(parent), catalog_(&catalog),
      nowUnixNs_(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count()) {
    setObjectName(QStringLiteral("trashCleanupDialog"));
    setWindowTitle(QCoreApplication::translate("TrashCleanupDialog", "Empty Trash"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    all_ = new QRadioButton(QCoreApplication::translate("TrashCleanupDialog", "Delete everything"),
                            this);
    all_->setObjectName(QStringLiteral("trashCleanupAll"));
    older_ = new QRadioButton(
        QCoreApplication::translate("TrashCleanupDialog", "Delete objects older than"), this);
    older_->setObjectName(QStringLiteral("trashCleanupOlder"));
    days_ = new QSpinBox(this);
    days_->setObjectName(QStringLiteral("trashCleanupDays"));
    days_->setRange(1, 3650);
    days_->setValue(60);
    days_->setSuffix(QCoreApplication::translate("TrashCleanupDialog", " days"));
    older_->setChecked(true);

    auto *age_row = new QHBoxLayout;
    age_row->addWidget(older_);
    age_row->addWidget(days_);
    age_row->addStretch(1);
    layout->addWidget(all_);
    layout->addLayout(age_row);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("trashCleanupSummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    if (catalog.unreadable_recovery_roots != 0) {
        auto *notice =
            new QLabel(QCoreApplication::translate(
                           "TrashCleanupDialog",
                           "Some Trash storage locations are unavailable and will not be changed."),
                       this);
        notice->setObjectName(QStringLiteral("trashCleanupUnavailableNotice"));
        notice->setWordWrap(true);
        layout->addWidget(notice);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)
        ->setText(QCoreApplication::translate("TrashCleanupDialog", "Cancel"));
    clear_ = buttons->addButton(QCoreApplication::translate("TrashCleanupDialog", "Empty"),
                                QDialogButtonBox::DestructiveRole);
    clear_->setObjectName(QStringLiteral("trashCleanupConfirm"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(clear_, &QPushButton::clicked, this, &QDialog::accept);
    connect(all_, &QRadioButton::toggled, this, [this] {
        days_->setEnabled(!all_->isChecked());
        update_summary();
    });
    connect(older_, &QRadioButton::toggled, this, [this] { update_summary(); });
    connect(days_, &QSpinBox::valueChanged, this, [this] { update_summary(); });
    update_summary();
    resize(520, sizeHint().height());
}

TrashCleanupPlan TrashCleanupDialog::plan() const {
    return catalog_ == nullptr
               ? TrashCleanupPlan{}
               : make_trash_cleanup_plan(*catalog_, all_->isChecked(), days_->value(), nowUnixNs_);
}

void TrashCleanupDialog::update_summary() {
    const auto selected = plan();
    summary_->setText(
        QCoreApplication::translate("TrashCleanupDialog",
                                    "Will be permanently deleted: %1 objects · %2%3")
            .arg(selected.total_items)
            .arg(QLocale().formattedDataSize(static_cast<qint64>(std::min<std::uint64_t>(
                selected.total_bytes,
                static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())))))
            .arg(selected.totals_saturated
                     ? QCoreApplication::translate("TrashCleanupDialog", " or more")
                     : QString{}));
    clear_->setEnabled(!selected.manifests.empty());
}

} // namespace vove::ui
