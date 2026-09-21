#pragma once

#include <QColor>
#include <QImage>
#include <QRegion>
#include <QSize>
#include <QStyledItemDelegate>

namespace vove::ui {

enum class AppTheme : int;

class DirectoryItemDelegate final : public QStyledItemDelegate {
  public:
    explicit DirectoryItemDelegate(QObject *parent = nullptr);

    void set_thumbnail_extent(int extent);
    [[nodiscard]] int thumbnail_extent() const noexcept;
    void set_placeholder_theme(AppTheme theme);
    void set_folder_colors(const QColor &frame, const QColor &text);
    void set_file_label_colors(const QColor &text, const QColor &page_background,
                               const QColor &page_text, const QColor &page_border);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem &option,
                                 const QModelIndex &index) const override;
    [[nodiscard]] QRegion item_hit_region(const QStyleOptionViewItem &option,
                                          const QModelIndex &index) const;
    [[nodiscard]] QRect name_rect(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const;

  private:
    [[nodiscard]] QRect thumbnail_rect(const QStyleOptionViewItem &option) const;
    [[nodiscard]] QRect preview_content_rect(const QStyleOptionViewItem &option,
                                             const QModelIndex &index) const;
    [[nodiscard]] QRect page_badge_rect(const QStyleOptionViewItem &option,
                                        const QModelIndex &index) const;
    [[nodiscard]] QRect status_badge_rect(const QStyleOptionViewItem &option,
                                          const QModelIndex &index) const;
    int extent_{160};
    QColor folderFrameColor_{QStringLiteral("#F1E4BB")};
    QColor folderTextColor_{QStringLiteral("#5EA5B6")};
    QColor fileTextColor_{QStringLiteral("#F1E4BB")};
    QColor pageBackgroundColor_{QStringLiteral("#013145")};
    QColor pageTextColor_{QStringLiteral("#F1E4BB")};
    QColor pageBorderColor_{QStringLiteral("#013145")};
    QImage folderPlaceholder_{QStringLiteral(":/artwork/empty-folder-breeze.png")};
    QImage filePlaceholder_{QStringLiteral(":/artwork/empty-file-breeze.png")};
};

} // namespace vove::ui
