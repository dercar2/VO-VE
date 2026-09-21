#pragma once
#include "vove/preview/thumbnail_types.hpp"

#include "vove/catalog/catalog_types.hpp"
#include "vove/core/directory_model.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QItemSelection>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include <filesystem>
#include <vector>

class QMimeData;

namespace vove::ui {

class DirectoryListModel final : public QAbstractListModel, public catalog::DirectorySink {
  public:
    enum Role : int {
        EntryIdRole = Qt::UserRole + 1,
        EntryKindRole,
        EntryPathRole,
        EntrySizeRole,
        EntryModifiedRole,
        ThumbnailRole,
        PreviewStatusRole,
        PageCountRole,
        SourceColorModelRole,
        SourceColorProfileRole,
        SourceProfileFingerprintRole,
        PreviewProvenanceRole,
        StartupDiagnosticRole,
    };
    static constexpr int VerificationPendingStatus = -1;

    struct PreviewRecord {
        qulonglong source_size{};
        qint64 modified_unix_ns{};
        QString source_revision;
        QImage image;
        int status{};
        std::uint16_t presentation_edge{};
        std::uint32_t page_count{};
        QString source_color_model;
        QString source_color_profile;
        QString source_profile_fingerprint;
        int provenance{};
        preview::WorkerStartupDiagnostic startup_diagnostic{};
    };

    struct Snapshot {
        core::DirectoryModel model;
        QHash<qulonglong, PreviewRecord> previews;
        QHash<qulonglong, QString> tooltip_paths;
        bool valid{};
    };

    explicit DirectoryListModel(QObject *parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex &index) const override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData *mimeData(const QModelIndexList &indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    [[nodiscard]] bool decode_internal_drag(const QMimeData *mime,
                                            QList<core::DirectoryEntry> &entries) const;

    void begin_catalog() override;
    void append_catalog(std::vector<core::DirectoryEntry> entries) override;
    void replace_catalog(std::vector<core::DirectoryEntry> entries) override;
    void finish_catalog() override;
    void abort_catalog() override;
    [[nodiscard]] bool last_replacement_changed() const noexcept {
        return lastReplacementChanged_;
    }

    void set_filter(const QString &text);
    void set_sort(core::SortField field, core::SortDirection direction);
    void set_drag_and_edit_enabled(bool enabled, bool allow_edit = true);
    void set_tooltip_paths(QHash<qulonglong, QString> paths);
    void set_preview(qulonglong id, qulonglong source_size, qint64 modified_unix_ns,
                     QString source_revision, QImage image, int status,
                     std::uint16_t presentation_edge, std::uint32_t page_count,
                     QString source_color_model, QString source_color_profile,
                     QString source_profile_fingerprint = {}, int provenance = 0,
                     preview::WorkerStartupDiagnostic startup_diagnostic = {});
    void clear_preview(qulonglong id);
    [[nodiscard]] QList<qulonglong> clear_previews_with_status(int status);
    void clear_previews();
    void clear_preview_images();
    void mark_preview_verification_pending(qulonglong id);
    void mark_previews_verification_pending();
    void mark_previews_offline();
    [[nodiscard]] Snapshot take_snapshot();
    void restore_snapshot(Snapshot snapshot);

    [[nodiscard]] std::size_t total_size() const noexcept;
    [[nodiscard]] std::size_t visible_size() const noexcept;
    [[nodiscard]] std::size_t visible_directories() const noexcept;
    [[nodiscard]] std::size_t visible_files() const noexcept;
    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] const core::DirectoryEntry *entry_at(const QModelIndex &index) const noexcept;
    [[nodiscard]] const core::DirectoryEntry *entry_for_id(qulonglong id) const noexcept;
    [[nodiscard]] int row_for_id(qulonglong id) const noexcept;
    [[nodiscard]] QHash<qulonglong, int> rows_for_ids(const QSet<qulonglong> &ids) const;
    [[nodiscard]] QItemSelection selection_for_ids(const QSet<qulonglong> &ids) const;
    [[nodiscard]] QList<QPair<QString, QString>> directory_entries() const;
    [[nodiscard]] std::vector<std::filesystem::path> all_entry_names(const QString &parent = {}) const;

  private:
    void prune_previews();
    void mark_previews_status(int status);
    void rebuild_row_index();

    core::DirectoryModel model_;
    QByteArray dragSecret_;
    core::DirectoryModel::Generation generation_{};
    QHash<qulonglong, PreviewRecord> previews_;
    QHash<qulonglong, QString> tooltipPaths_;
    QHash<qulonglong, int> rowsById_;
    bool lastReplacementChanged_{true};
    bool dragAndEditEnabled_{true};
    bool allowEdit_{true};
};

} // namespace vove::ui
