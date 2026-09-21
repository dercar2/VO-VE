#include "directory_list_model.hpp"
#include "worker_startup_text.hpp"

#include "vove/preview/helper_protocol.hpp"

#include <QDateTime>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QtGlobal>

#include <algorithm>

namespace vove::ui {
namespace {

constexpr auto internalDragMime = "application/x-vove-entry-set-v1";
constexpr quint32 internalDragMagic = 0x31445656U;
constexpr quint16 internalDragVersion = 1;
constexpr qsizetype maximumInternalDragBytes = qsizetype{1024} * 1024;
constexpr quint32 maximumInternalDragEntries = 10'000;

} // namespace

DirectoryListModel::DirectoryListModel(QObject *parent)
    : QAbstractListModel(parent), dragSecret_(QUuid::createUuid().toRfc4122()) {}

int DirectoryListModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(model_.visible_size());
}

QVariant DirectoryListModel::data(const QModelIndex &index, const int role) const {
    const auto *entry = entry_at(index);
    if (entry == nullptr) {
        return {};
    }

    switch (role) {
    case Qt::DisplayRole:
        return QString::fromUtf8(entry->name_utf8);
    case Qt::ToolTipRole: {
        const auto diagnostic = data(index, StartupDiagnosticRole).toString();
        if (!diagnostic.isEmpty())
            return worker_startup_message() + QStringLiteral("\n\n") + diagnostic;
        const auto tooltip = tooltipPaths_.constFind(static_cast<qulonglong>(entry->id));
        if (tooltip != tooltipPaths_.cend()) {
            return *tooltip;
        }
        return QString::fromUtf8(entry->path_utf8);
    }
    case EntryIdRole:
        return QVariant::fromValue<qulonglong>(entry->id);
    case EntryKindRole:
        return static_cast<int>(entry->kind);
    case EntryPathRole:
        return QString::fromUtf8(entry->path_utf8);
    case EntrySizeRole:
        return QVariant::fromValue<qulonglong>(entry->size_bytes);
    case EntryModifiedRole:
        return QDateTime::fromMSecsSinceEpoch(entry->modified_unix_ns / 1'000'000);
    case ThumbnailRole:
    case PreviewStatusRole:
    case PageCountRole:
    case SourceColorModelRole:
    case SourceColorProfileRole:
    case SourceProfileFingerprintRole:
    case PreviewProvenanceRole:
    case StartupDiagnosticRole: {
        const auto found = previews_.constFind(static_cast<qulonglong>(entry->id));
        if (found == previews_.cend() || found->source_size != entry->size_bytes ||
            found->modified_unix_ns != entry->modified_unix_ns ||
            found->source_revision != QString::fromUtf8(entry->source_revision_utf8)) {
            return {};
        }
        if (role == ThumbnailRole) {
            return QVariant::fromValue(found->image);
        }
        if (role == PreviewStatusRole) {
            return found->status;
        }
        if (role == StartupDiagnosticRole) {
            return found->status ==
                           static_cast<int>(
                               preview::helper_protocol::ResponseStatus::worker_start_failed)
                       ? worker_startup_tooltip(found->startup_diagnostic)
                       : QString{};
        }
        if (role == PageCountRole) {
            return QVariant::fromValue<qulonglong>(found->page_count);
        }
        if (role == SourceColorModelRole) {
            return found->source_color_model;
        }
        if (role == SourceColorProfileRole) {
            return found->source_color_profile;
        }
        if (role == PreviewProvenanceRole) {
            return found->provenance;
        }
        return found->source_profile_fingerprint;
    }
    default:
        return {};
    }
}

Qt::ItemFlags DirectoryListModel::flags(const QModelIndex &index) const {
    auto result = QAbstractListModel::flags(index);
    if (index.isValid() && dragAndEditEnabled_) {
        result |= Qt::ItemIsDragEnabled;
        if (const auto *entry = entry_at(index); allowEdit_ && entry && entry->kind == core::EntryKind::file) {
            result |= Qt::ItemIsEditable;
        }
    }
    return result;
}

QStringList DirectoryListModel::mimeTypes() const {
    return {QStringLiteral("text/uri-list"), QString::fromLatin1(internalDragMime)};
}

QMimeData *DirectoryListModel::mimeData(const QModelIndexList &indexes) const {
    if (!dragAndEditEnabled_) {
        return new QMimeData;
    }
    QList<QUrl> urls;
    QList<const core::DirectoryEntry *> entries;
    QSet<int> rows;
    for (const auto &index : indexes) {
        if (!index.isValid() || rows.contains(index.row())) {
            continue;
        }
        rows.insert(index.row());
        const auto *entry = entry_at(index);
        if (entry != nullptr) {
            urls.push_back(QUrl::fromLocalFile(QString::fromUtf8(entry->path_utf8)));
            entries.push_back(entry);
        }
    }
    auto *result = new QMimeData;
    result->setUrls(urls);
    QByteArray payload;
    QDataStream stream(&payload, QIODeviceBase::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << internalDragMagic << internalDragVersion << dragSecret_
           << static_cast<quint32>(entries.size());
    for (const auto *entry : entries) {
        stream << static_cast<quint64>(entry->id) << static_cast<quint8>(entry->kind)
               << QString::fromUtf8(entry->name_utf8) << QString::fromUtf8(entry->path_utf8)
               << static_cast<quint64>(entry->size_bytes)
               << static_cast<qint64>(entry->modified_unix_ns)
               << QString::fromUtf8(entry->source_revision_utf8);
    }
    if (stream.status() == QDataStream::Ok && payload.size() <= maximumInternalDragBytes) {
        result->setData(QString::fromLatin1(internalDragMime), payload);
    }
    return result;
}

bool DirectoryListModel::decode_internal_drag(const QMimeData *mime,
                                              QList<core::DirectoryEntry> &entries) const {
    entries.clear();
    if (mime == nullptr || !mime->hasFormat(QString::fromLatin1(internalDragMime))) {
        return false;
    }
    const auto payload = mime->data(QString::fromLatin1(internalDragMime));
    if (payload.isEmpty() || payload.size() > maximumInternalDragBytes) {
        return false;
    }
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic{};
    quint16 version{};
    QByteArray secret;
    quint32 count{};
    stream >> magic >> version >> secret >> count;
    if (stream.status() != QDataStream::Ok || magic != internalDragMagic ||
        version != internalDragVersion || secret != dragSecret_ || count == 0 ||
        count > maximumInternalDragEntries) {
        return false;
    }
    entries.reserve(static_cast<qsizetype>(count));
    for (quint32 index{}; index < count; ++index) {
        quint64 id{};
        quint8 kind{};
        QString name;
        QString path;
        quint64 size{};
        qint64 modified{};
        QString revision;
        stream >> id >> kind >> name >> path >> size >> modified >> revision;
        if (stream.status() != QDataStream::Ok ||
            kind > static_cast<quint8>(core::EntryKind::directory) || name.isEmpty() ||
            path.isEmpty()) {
            entries.clear();
            return false;
        }
        const auto name_utf8 = name.toUtf8().toStdString();
        entries.push_back({.id = static_cast<std::uint64_t>(id),
                           .kind = static_cast<core::EntryKind>(kind),
                           .name_utf8 = name_utf8,
                           .search_key_utf8 = core::make_search_key(name_utf8),
                           .path_utf8 = path.toUtf8().toStdString(),
                           .source_revision_utf8 = revision.toUtf8().toStdString(),
                           .size_bytes = static_cast<std::uint64_t>(size),
                           .modified_unix_ns = static_cast<std::int64_t>(modified)});
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd()) {
        entries.clear();
        return false;
    }
    return true;
}

Qt::DropActions DirectoryListModel::supportedDragActions() const {
    // External targets may copy these read-only source URLs. All moves remain owned by VO-VE's
    // journaled target-side coordinator rather than an arbitrary shell drop target.
    return dragAndEditEnabled_ ? Qt::CopyAction : Qt::IgnoreAction;
}

void DirectoryListModel::begin_catalog() {
    beginResetModel();
    dragSecret_ = QUuid::createUuid().toRfc4122();
    previews_.clear();
    tooltipPaths_.clear();
    rowsById_.clear();
    generation_ = model_.begin_generation();
    endResetModel();
}

void DirectoryListModel::append_catalog(std::vector<core::DirectoryEntry> entries) {
    if (entries.empty()) {
        return;
    }
    const auto matching = model_.matching_count(entries);
    const auto first = static_cast<int>(model_.visible_size());
    const auto resets_existing_file_rows = model_.append_reorders_visible_rows(entries);
    if (resets_existing_file_rows) {
        beginResetModel();
    } else if (matching != 0) {
        beginInsertRows({}, first, first + static_cast<int>(matching) - 1);
    }
    if (!model_.append_batch(generation_, entries)) {
        qFatal("DirectoryListModel rejected its current catalog generation");
    }
    rebuild_row_index();
    if (resets_existing_file_rows) {
        endResetModel();
    } else if (matching != 0) {
        endInsertRows();
    }
}

void DirectoryListModel::finish_catalog() {
    if (!model_.loading()) {
        return;
    }
    beginResetModel();
    if (!model_.finish_generation(generation_)) {
        qFatal("DirectoryListModel could not finish its current catalog generation");
    }
    rebuild_row_index();
    endResetModel();
}

void DirectoryListModel::abort_catalog() {
    if (!model_.loading()) {
        return;
    }
    if (!model_.abort_generation(generation_)) {
        qFatal("DirectoryListModel could not abort its current catalog generation");
    }
}

DirectoryListModel::Snapshot DirectoryListModel::take_snapshot() {
    beginResetModel();
    Snapshot snapshot{.model = std::move(model_),
                      .previews = std::move(previews_),
                      .tooltip_paths = std::move(tooltipPaths_),
                      .valid = true};
    model_ = {};
    generation_ = 0;
    rowsById_.clear();
    endResetModel();
    return snapshot;
}

void DirectoryListModel::restore_snapshot(Snapshot snapshot) {
    if (!snapshot.valid) {
        return;
    }
    beginResetModel();
    model_ = std::move(snapshot.model);
    previews_ = std::move(snapshot.previews);
    tooltipPaths_ = std::move(snapshot.tooltip_paths);
    generation_ = model_.generation();
    rebuild_row_index();
    endResetModel();
}

void DirectoryListModel::replace_catalog(std::vector<core::DirectoryEntry> entries) {
    const auto same_entry = [](const auto &left, const auto &right) {
        return left.id == right.id && left.kind == right.kind &&
               left.name_utf8 == right.name_utf8 && left.path_utf8 == right.path_utf8 &&
               left.source_revision_utf8 == right.source_revision_utf8 &&
               left.size_bytes == right.size_bytes &&
               left.modified_unix_ns == right.modified_unix_ns;
    };
    lastReplacementChanged_ = !std::ranges::equal(model_.entries(), entries, same_entry);
    if (!lastReplacementChanged_) {
        return;
    }
    core::DirectoryModel replacement;
    replacement.set_filter_case(model_.filter_case());
    replacement.set_filter_key(std::string(model_.filter_key()));
    replacement.set_sort(model_.sort_field(), model_.sort_direction());
    const auto replacement_generation = replacement.begin_generation();
    if (!replacement.replace_generation(replacement_generation, std::move(entries)) ||
        !replacement.finish_generation(replacement_generation)) {
        qFatal("DirectoryListModel could not replace its catalog generation");
    }

    const auto same_rows = [&] {
        if (model_.visible_size() != replacement.visible_size()) {
            return false;
        }
        for (std::size_t row = 0; row < model_.visible_size(); ++row) {
            if (model_.visible_at(row).id != replacement.visible_at(row).id) {
                return false;
            }
        }
        return true;
    }();

    if (same_rows) {
        model_ = std::move(replacement);
        generation_ = model_.generation();
        rebuild_row_index();
        prune_previews();
        if (rowCount() != 0) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
        }
        return;
    }

    beginResetModel();
    model_ = std::move(replacement);
    generation_ = model_.generation();
    rebuild_row_index();
    prune_previews();
    endResetModel();
}

void DirectoryListModel::set_filter(const QString &text) {
    beginResetModel();
    const auto utf8 = text.toUtf8().toStdString();
    model_.set_filter_case(core::FilterCase::insensitive);
    model_.set_filter_key(core::make_search_key(utf8));
    rebuild_row_index();
    endResetModel();
}

void DirectoryListModel::set_sort(const core::SortField field,
                                  const core::SortDirection direction) {
    beginResetModel();
    model_.set_sort(field, direction);
    rebuild_row_index();
    endResetModel();
}

void DirectoryListModel::set_drag_and_edit_enabled(const bool enabled, const bool allow_edit) {
    if (dragAndEditEnabled_ == enabled && allowEdit_ == allow_edit) {
        return;
    }
    dragAndEditEnabled_ = enabled;
    allowEdit_ = allow_edit;
    if (rowCount() != 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
    }
}

void DirectoryListModel::set_tooltip_paths(QHash<qulonglong, QString> paths) {
    tooltipPaths_ = std::move(paths);
    if (rowCount() != 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::ToolTipRole});
    }
}

void DirectoryListModel::set_preview(const qulonglong id, const qulonglong source_size,
                                     const qint64 modified_unix_ns, QString source_revision,
                                     QImage image, const int status,
                                     const std::uint16_t presentation_edge,
                                     const std::uint32_t page_count, QString source_color_model,
                                     QString source_color_profile,
                                     QString source_profile_fingerprint, const int provenance,
                                     preview::WorkerStartupDiagnostic startup_diagnostic) {
    const auto row = row_for_id(id);
    if (row < 0) {
        return;
    }
    const auto *entry = entry_at(index(row, 0));
    if (entry == nullptr || entry->size_bytes != source_size ||
        entry->modified_unix_ns != modified_unix_ns ||
        QString::fromUtf8(entry->source_revision_utf8) != source_revision) {
        return;
    }
    const auto response_status = static_cast<preview::helper_protocol::ResponseStatus>(status);
    if (response_status != preview::helper_protocol::ResponseStatus::worker_start_failed ||
        !startup_diagnostic.valid())
        startup_diagnostic = {};
    const auto invalidates_pixels =
        response_status == preview::helper_protocol::ResponseStatus::source_changed ||
        response_status == preview::helper_protocol::ResponseStatus::not_found ||
        response_status == preview::helper_protocol::ResponseStatus::permission_denied ||
        response_status == preview::helper_protocol::ResponseStatus::authentication_failed;
    const auto marks_preserved_pixels_offline =
        response_status == preview::helper_protocol::ResponseStatus::disconnected ||
        response_status == preview::helper_protocol::ResponseStatus::timed_out ||
        response_status == preview::helper_protocol::ResponseStatus::source_unavailable;
    const auto existing = previews_.constFind(id);
    if (image.isNull() && existing != previews_.cend() && existing->source_size == source_size &&
        existing->modified_unix_ns == modified_unix_ns &&
        existing->source_revision == source_revision && !existing->image.isNull()) {
        if (marks_preserved_pixels_offline) {
            auto preserved = *existing;
            preserved.status =
                static_cast<int>(preview::helper_protocol::ResponseStatus::success_offline_cached);
            previews_.insert(id, preserved);
            const auto changed = index(row, 0);
            emit dataChanged(changed, changed, {PreviewStatusRole});
            return;
        }
        if (!invalidates_pixels) {
            return;
        }
    }
    if (!invalidates_pixels && existing != previews_.cend() &&
        existing->source_size == source_size && existing->modified_unix_ns == modified_unix_ns &&
        existing->source_revision == source_revision &&
        existing->presentation_edge > presentation_edge && !existing->image.isNull()) {
        return;
    }
    previews_.insert(id, {.source_size = source_size,
                          .modified_unix_ns = modified_unix_ns,
                          .source_revision = std::move(source_revision),
                          .image = std::move(image),
                          .status = status,
                          .presentation_edge = presentation_edge,
                          .page_count = page_count,
                          .source_color_model = std::move(source_color_model),
                          .source_color_profile = std::move(source_color_profile),
                          .source_profile_fingerprint = std::move(source_profile_fingerprint),
                          .provenance = provenance,
                          .startup_diagnostic = startup_diagnostic});
    const auto changed = index(row, 0);
    emit dataChanged(changed, changed,
                     {ThumbnailRole, PreviewStatusRole, PageCountRole, SourceColorModelRole,
                      SourceColorProfileRole, SourceProfileFingerprintRole, PreviewProvenanceRole,
                      StartupDiagnosticRole, Qt::ToolTipRole});
}

void DirectoryListModel::clear_preview(const qulonglong id) {
    if (previews_.remove(id) == 0) {
        return;
    }
    const auto row = row_for_id(id);
    if (row >= 0) {
        const auto changed = index(row, 0);
        emit dataChanged(changed, changed,
                         {ThumbnailRole, PreviewStatusRole, PageCountRole, SourceColorModelRole,
                          SourceColorProfileRole, SourceProfileFingerprintRole,
                          PreviewProvenanceRole, StartupDiagnosticRole, Qt::ToolTipRole});
    }
}

QList<qulonglong> DirectoryListModel::clear_previews_with_status(const int status) {
    QList<qulonglong> removed;
    for (auto iterator = previews_.begin(); iterator != previews_.end();) {
        if (iterator->status == status) {
            removed.push_back(iterator.key());
            iterator = previews_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (!removed.isEmpty() && rowCount() != 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                         {ThumbnailRole, PreviewStatusRole, PageCountRole, SourceColorModelRole,
                          SourceColorProfileRole, SourceProfileFingerprintRole,
                          PreviewProvenanceRole, StartupDiagnosticRole, Qt::ToolTipRole});
    }
    return removed;
}

void DirectoryListModel::clear_previews() {
    if (previews_.isEmpty()) {
        return;
    }
    previews_.clear();
    if (rowCount() != 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                         {ThumbnailRole, PreviewStatusRole, PageCountRole, SourceColorModelRole,
                          SourceColorProfileRole, SourceProfileFingerprintRole,
                          PreviewProvenanceRole, StartupDiagnosticRole, Qt::ToolTipRole});
    }
}

void DirectoryListModel::clear_preview_images() {
    bool changed{};
    for (auto iterator = previews_.begin(); iterator != previews_.end(); ++iterator) {
        if (!iterator->image.isNull()) {
            iterator->image = {};
            changed = true;
        }
    }
    if (changed && rowCount() != 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {ThumbnailRole});
    }
}

void DirectoryListModel::mark_previews_verification_pending() {
    mark_previews_status(VerificationPendingStatus);
}

void DirectoryListModel::mark_preview_verification_pending(const qulonglong id) {
    const auto preview = previews_.find(id);
    if (preview == previews_.end() || preview->image.isNull() ||
        preview->status == VerificationPendingStatus) {
        return;
    }
    preview->status = VerificationPendingStatus;
    const auto row = row_for_id(id);
    if (row >= 0) {
        const auto changed = index(row, 0);
        emit dataChanged(changed, changed, {PreviewStatusRole});
    }
}

void DirectoryListModel::mark_previews_offline() {
    mark_previews_status(
        static_cast<int>(preview::helper_protocol::ResponseStatus::success_offline_cached));
}

void DirectoryListModel::mark_previews_status(const int status) {
    if (previews_.isEmpty()) {
        return;
    }
    bool changed{};
    for (auto iterator = previews_.begin(); iterator != previews_.end(); ++iterator) {
        if (!iterator->image.isNull() && iterator->status != status) {
            iterator->status = status;
            changed = true;
        }
    }
    if (changed && rowCount() != 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {PreviewStatusRole});
    }
}

void DirectoryListModel::prune_previews() {
    for (auto iterator = previews_.begin(); iterator != previews_.end();) {
        const auto row = row_for_id(iterator.key());
        const auto *entry = row < 0 ? nullptr : entry_at(index(row, 0));
        if (entry == nullptr || entry->size_bytes != iterator->source_size ||
            entry->modified_unix_ns != iterator->modified_unix_ns ||
            QString::fromUtf8(entry->source_revision_utf8) != iterator->source_revision) {
            iterator = previews_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

std::size_t DirectoryListModel::total_size() const noexcept {
    return model_.total_size();
}

std::size_t DirectoryListModel::visible_size() const noexcept {
    return model_.visible_size();
}

std::size_t DirectoryListModel::visible_directories() const noexcept {
    std::size_t count{};
    for (std::size_t index = 0; index < model_.visible_size(); ++index) {
        if (model_.visible_at(index).kind == core::EntryKind::directory) {
            ++count;
        }
    }
    return count;
}

std::size_t DirectoryListModel::visible_files() const noexcept {
    return model_.visible_size() - visible_directories();
}

bool DirectoryListModel::loading() const noexcept {
    return model_.loading();
}

const core::DirectoryEntry *DirectoryListModel::entry_at(const QModelIndex &index) const noexcept {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= model_.visible_size()) {
        return nullptr;
    }
    return &model_.visible_at(static_cast<std::size_t>(index.row()));
}

const core::DirectoryEntry *DirectoryListModel::entry_for_id(const qulonglong id) const noexcept {
    const auto row = row_for_id(id);
    return row < 0 ? nullptr : &model_.visible_at(static_cast<std::size_t>(row));
}

int DirectoryListModel::row_for_id(const qulonglong id) const noexcept {
    const auto found = rowsById_.constFind(id);
    return found == rowsById_.cend() ? -1 : found.value();
}

void DirectoryListModel::rebuild_row_index() {
    rowsById_.clear();
    rowsById_.reserve(static_cast<qsizetype>(model_.visible_size()));
    for (std::size_t row = 0; row < model_.visible_size(); ++row) {
        rowsById_.insert(static_cast<qulonglong>(model_.visible_at(row).id), static_cast<int>(row));
    }
}

QHash<qulonglong, int> DirectoryListModel::rows_for_ids(const QSet<qulonglong> &ids) const {
    QHash<qulonglong, int> result;
    result.reserve(ids.size());
    if (ids.isEmpty()) {
        return result;
    }
    for (std::size_t row = 0; row < model_.visible_size(); ++row) {
        const auto id = static_cast<qulonglong>(model_.visible_at(row).id);
        if (ids.contains(id)) {
            result.insert(id, static_cast<int>(row));
            if (result.size() == ids.size()) {
                break;
            }
        }
    }
    return result;
}

QItemSelection DirectoryListModel::selection_for_ids(const QSet<qulonglong> &ids) const {
    const auto mapped = rows_for_ids(ids);
    std::vector<int> rows;
    rows.reserve(static_cast<std::size_t>(mapped.size()));
    for (auto iterator = mapped.cbegin(); iterator != mapped.cend(); ++iterator) {
        rows.push_back(iterator.value());
    }
    std::ranges::sort(rows);

    QItemSelection selection;
    if (rows.empty()) {
        return selection;
    }
    auto first = rows.front();
    auto last = first;
    for (std::size_t offset = 1; offset < rows.size(); ++offset) {
        if (rows[offset] == last + 1) {
            last = rows[offset];
            continue;
        }
        selection.select(index(first, 0), index(last, 0));
        first = rows[offset];
        last = first;
    }
    selection.select(index(first, 0), index(last, 0));
    return selection;
}

QList<QPair<QString, QString>> DirectoryListModel::directory_entries() const {
    QList<QPair<QString, QString>> result;
    for (const auto &entry : model_.entries()) {
        if (entry.kind == core::EntryKind::directory) {
            result.emplaceBack(QString::fromUtf8(entry.name_utf8),
                               QString::fromUtf8(entry.path_utf8));
        }
    }
    std::ranges::sort(result, {}, [](const auto &entry) { return entry.first.toCaseFolded(); });
    return result;
}

std::vector<std::filesystem::path> DirectoryListModel::all_entry_names(const QString &parent) const {
    std::vector<std::filesystem::path> result;
    result.reserve(model_.entries().size());
    for (const auto &entry : model_.entries()) {
        if (!parent.isEmpty()) {
#ifdef Q_OS_WIN
            constexpr auto sensitivity = Qt::CaseInsensitive;
#else
            constexpr auto sensitivity = Qt::CaseSensitive;
#endif
            const auto entry_parent = QFileInfo(QString::fromUtf8(entry.path_utf8)).absolutePath();
            if (QDir::cleanPath(entry_parent).compare(QDir::cleanPath(parent), sensitivity) != 0)
                continue;
        }
#ifdef _WIN32
        result.emplace_back(QString::fromUtf8(entry.name_utf8).toStdWString());
#else
        result.emplace_back(entry.name_utf8);
#endif
    }
    return result;
}

} // namespace vove::ui
