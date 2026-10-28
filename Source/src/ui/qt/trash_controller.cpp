#include "trash_controller.hpp"

#include "catalog_controller.hpp"
#include "drag_views.hpp"
#include "file_command_controller.hpp"
#include "places_controller.hpp"
#include "vove/fileops/trash_catalog.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QItemSelectionModel>
#include <QListView>
#include <QLocale>
#include <QMenu>

#include <utility>

namespace vove::ui {
namespace {
std::filesystem::path native_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
std::string path_utf8(const std::filesystem::path &path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native()).toUtf8().toStdString();
#else
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
#endif
}
qulonglong entry_id(const std::uint64_t operation, const std::size_t index) noexcept {
    const auto value = operation ^ (0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(index) +
                                    (operation << 6U) + (operation >> 2U));
    return static_cast<qulonglong>(value == 0 ? index + 1U : value);
}
} // namespace

TrashController::TrashController(CatalogController &catalog, FileCommandController &commands,
                                 TrashViews views, TrashCallbacks callbacks, QObject *parent)
    : QObject(parent), catalog_(catalog), commands_(commands), views_(views),
      callbacks_(std::move(callbacks)) {
    timer_.setInterval(40);
    connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
}
TrashController::~TrashController() { timer_.stop(); loader_.cancel(); }
void TrashController::message(const QString &text) {
    if (callbacks_.message) callbacks_.message(text);
    if (callbacks_.changed) callbacks_.changed();
}
void TrashController::show() {
    if (!commands_.availability().trash_catalog_allowed()) {
        if (callbacks_.changed) callbacks_.changed();
        return;
    }
    static_cast<void>(commands_.refresh_current_operation_journal_state());
    result_.reset();
    manifests_.clear();
    catalog_.enter_trash();
    load();
}
void TrashController::leave() {
    timer_.stop();
    loader_.cancel();
    result_.reset();
    manifests_.clear();
}
void TrashController::load(const bool discover) {
    const auto roots = discover ? std::nullopt
        : std::optional<std::vector<std::filesystem::path>>{std::vector<std::filesystem::path>{}};
    if (!loader_.start(commands_.trash_manifest_directory(), roots)) {
        message(QCoreApplication::translate("MainWindow", "Failed to read VO-VE Trash"));
        return;
    }
    if (catalog_.trash_active() &&
        commands_.refresh_current_operation_journal_state() == CurrentOperationJournalState::none)
        message(QCoreApplication::translate("MainWindow", "VO-VE Trash: loading…"));
    timer_.start();
}
void TrashController::poll() {
    if (auto result = loader_.poll()) {
        if (catalog_.trash_active()) apply(std::move(*result));
        else { result_ = std::move(*result); update_favorite(); }
    }
    if (!loader_.loading()) timer_.stop();
}
void TrashController::apply(fileops::TrashCatalogResult result) {
    result_ = std::move(result);
    update_favorite();
    manifests_.clear();
    if (!result_->ok()) {
        catalog_.replace_trash_entries({}, {});
        message(QCoreApplication::translate("MainWindow", "Failed to read VO-VE Trash"));
        return;
    }
    std::vector<core::DirectoryEntry> entries;
    entries.reserve(result_->total_items);
    QHash<qulonglong, QString> tooltips;
    for (const auto &record : result_->manifests) {
        const auto deleted = QDateTime::fromMSecsSinceEpoch(record.transaction.created_unix_ns / 1'000'000);
        const auto date = deleted.isValid() ? QLocale().toString(deleted.toLocalTime(), QLocale::ShortFormat)
            : QCoreApplication::translate("MainWindow", "unknown date");
        for (std::size_t index{}; index < record.transaction.items.size(); ++index) {
            const auto &item = record.transaction.items[index];
            if (item.location != fileops::TrashItemLocation::stored || item.current.empty()) continue;
            const auto id = entry_id(record.transaction.operation_id, index);
            auto name = item.original.filename().u8string();
            if (name.empty()) name = item.current.filename().u8string();
            entries.push_back({.id = id,
                .kind = item.kind == fileops::TrashItemKind::directory ? core::EntryKind::directory : core::EntryKind::file,
                .state = core::EntryState::metadata_ready,
                .name_utf8 = reinterpret_cast<const char *>(name.c_str()),
                .search_key_utf8 = core::make_search_key(reinterpret_cast<const char *>(name.c_str())),
                .path_utf8 = path_utf8(item.current),
                .source_revision_utf8 = item.current_snapshot.source_revision_utf8,
                .size_bytes = item.payload_bytes, .modified_unix_ns = item.current_snapshot.modified_unix_ns});
            tooltips.insert(id, QCoreApplication::translate("MainWindow",
                "%1\nDeleted: %2\nActions apply to the complete deletion operation")
                .arg(QDir::toNativeSeparators(QString::fromUtf8(path_utf8(item.original))), date));
            manifests_.insert(id, QString::fromUtf8(path_utf8(record.manifest_path)));
        }
    }
    catalog_.replace_trash_entries(std::move(entries), std::move(tooltips));
    if (commands_.refresh_current_operation_journal_state() == CurrentOperationJournalState::none) message({});
    if (callbacks_.changed) callbacks_.changed();
    if (std::exchange(pendingCleanup_, false)) QTimer::singleShot(0, this, [this] { cleanup(); });
}
void TrashController::show_context_menu(const QPoint &position) {
    const auto index = views_.grid->indexAt(position);
    if (!index.isValid()) return;
    views_.grid->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    const auto found = manifests_.constFind(index.data(DirectoryListModel::EntryIdRole).toULongLong());
    if (found == manifests_.cend()) return;
    // Own the identifier across the modal menu; catalog callbacks may replace its hash.
    const auto manifest = native_path(*found);
    QMenu menu(views_.dialog_owner);
    menu.setObjectName(QStringLiteral("trashItemContextMenu"));
    auto *restore = menu.addAction(QCoreApplication::translate("MainWindow", "Restore"));
    restore->setObjectName(QStringLiteral("trashRestoreAction"));
    auto *restore_to = menu.addAction(QCoreApplication::translate("MainWindow", "Restore to…"));
    restore_to->setObjectName(QStringLiteral("trashRestoreToAction"));
    auto *purge = menu.addAction(QCoreApplication::translate("MainWindow", "Delete permanently"));
    purge->setObjectName(QStringLiteral("trashDeletePermanentlyAction"));
    const bool allowed = commands_.refresh_current_operation_journal_state() == CurrentOperationJournalState::none && !commands_.busy();
    restore->setEnabled(allowed);
    restore_to->setEnabled(allowed);
    purge->setEnabled(allowed);
    const auto chosen = menu.exec(views_.grid->viewport()->mapToGlobal(position));
    if (chosen == restore) static_cast<void>(commands_.start_trash_manifest_operation(manifest, TrashAction::restore));
    else if (chosen == restore_to) static_cast<void>(commands_.start_trash_manifest_operation(manifest, TrashAction::restore_to));
    else if (chosen == purge) static_cast<void>(commands_.start_trash_manifest_operation(manifest, TrashAction::purge));
}
bool TrashController::can_cleanup() const {
    return commands_.trash_cleanup_allowed();
}
void TrashController::cleanup() {
    if (!result_ || !result_->ok()) {
        pendingCleanup_ = true;
        if (!catalog_.trash_active()) show();
        else if (!loader_.loading()) load();
        return;
    }
    if (!can_cleanup()) return;
    TrashCleanupDialog dialog(*result_, views_.dialog_owner);
    if (dialog.exec() != QDialog::Accepted) return;
    auto plan = dialog.plan();
    if (plan.manifests.empty()) {
        message(QCoreApplication::translate("MainWindow", "No matching Trash objects"));
        return;
    }
    static_cast<void>(commands_.start_trash_purge(std::move(plan.manifests)));
}
void TrashController::set_maximum_bytes(const std::uint64_t maximum) { maximumBytes_ = maximum; update_favorite(); }
std::size_t TrashController::operation_items(const std::filesystem::path &manifest) const {
    if (result_) for (const auto &record : result_->manifests)
        if (record.manifest_path == manifest) return record.transaction.items.size();
    return 0;
}
std::optional<TrashStatusSummary> TrashController::status_summary() const {
    if (!result_) return {};
    return TrashStatusSummary{result_->ok(), result_->total_items, result_->total_bytes};
}
void TrashController::update_favorite() {
    if (!result_) return;
    QList<QListWidgetItem *> items;
    for (int row = 0; row < views_.favorites->count(); ++row) {
        if (PlacesController::is_trash_favorite(views_.favorites->item(row))) {
            items.push_back(views_.favorites->item(row));
            break;
        }
    }
    if (views_.trash_favorite && PlacesController::is_trash_favorite(views_.trash_favorite->item(0)))
        items.push_back(views_.trash_favorite->item(0));
    auto worst = fileops::TrashQuotaState::normal;
    for (const auto &usage : result_->storage_usage) {
        const auto state = fileops::classify_trash_quota(usage, maximumBytes_);
        if (state == fileops::TrashQuotaState::exceeded) { worst = state; break; }
        if (state == fileops::TrashQuotaState::warning) worst = state;
    }
    const auto foreground = worst == fileops::TrashQuotaState::exceeded ? QVariant(QColor(QStringLiteral("#FF3B30")))
        : worst == fileops::TrashQuotaState::warning ? QVariant(QColor(QStringLiteral("#F5A623"))) : QVariant{};
    const auto tooltip = QCoreApplication::translate("MainWindow", "Objects: %1 · %2")
        .arg(static_cast<qulonglong>(result_->total_items)).arg(QLocale().formattedDataSize(static_cast<qint64>(result_->total_bytes)));
    for (auto *item : items) {
        item->setData(Qt::ForegroundRole, foreground);
        item->setData(Qt::UserRole + 2, static_cast<int>(worst));
        item->setToolTip(tooltip);
    }
}
} // namespace vove::ui
