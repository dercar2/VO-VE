#include "directory_item_delegate.hpp"

#include "directory_list_model.hpp"
#include "format_style.hpp"
#include "external_component_detection.hpp"
#include "vove/core/directory_entry.hpp"
#include "vove/preview/helper_protocol.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionViewItem>

#include <algorithm>
#include <array>

namespace vove::ui {

DirectoryItemDelegate::DirectoryItemDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void DirectoryItemDelegate::set_thumbnail_extent(const int extent) {
    extent_ = std::clamp(extent, 64, 320);
}

int DirectoryItemDelegate::thumbnail_extent() const noexcept {
    return extent_;
}

void DirectoryItemDelegate::set_placeholder_theme(const AppTheme theme) {
    const std::array names{"north", "vanilla", "breeze", "twilight"};
    const auto name = QString::fromLatin1(names.at(static_cast<std::size_t>(theme)));
    folderPlaceholder_.load(QStringLiteral(":/artwork/empty-folder-%1.png").arg(name));
    filePlaceholder_.load(QStringLiteral(":/artwork/empty-file-%1.png").arg(name));
}

void DirectoryItemDelegate::set_folder_colors(const QColor &frame, const QColor &text) {
    folderFrameColor_ = frame;
    folderTextColor_ = text;
}

void DirectoryItemDelegate::set_file_label_colors(const QColor &text, const QColor &page_background,
                                                  const QColor &page_text,
                                                  const QColor &page_border) {
    fileTextColor_ = text;
    pageBackgroundColor_ = page_background;
    pageTextColor_ = page_text;
    pageBorderColor_ = page_border;
}

QRect DirectoryItemDelegate::thumbnail_rect(const QStyleOptionViewItem &option) const {
    return {option.rect.x() + 16, option.rect.y() + 8, extent_, std::max(48, extent_ * 3 / 4)};
}

QRect DirectoryItemDelegate::page_badge_rect(const QStyleOptionViewItem &option,
                                             const QModelIndex &index) const {
    const auto page_count = index.data(DirectoryListModel::PageCountRole).toULongLong();
    if (page_count <= 1 || index.data(DirectoryListModel::EntryKindRole).toInt() ==
                               static_cast<int>(core::EntryKind::directory)) {
        return {};
    }
    auto count_font = option.font;
    if (count_font.pointSizeF() > 0) {
        count_font.setPointSizeF(count_font.pointSizeF() * 0.9);
    } else {
        count_font.setPixelSize(std::max(1, qRound(count_font.pixelSize() * 0.9)));
    }
    const auto text = QString::number(page_count);
    const auto thumbnail = thumbnail_rect(option);
    const auto height = std::min(QFontMetrics(option.font).height() + 2, extent_ / 2);
    const auto width =
        text.size() == 1
            ? height
            : std::min(
                  extent_ / 2,
                  std::max(height,
                           qCeil(QFontMetricsF(count_font).tightBoundingRect(text).width()) + 4));
    const QRect bounds(thumbnail.left(), thumbnail.bottom() + 9, extent_, height);
    const QRect badge(bounds.right() - width + 1, bounds.top(), width, height);
    return QStyle::visualRect(option.direction, bounds, badge);
}

QRect DirectoryItemDelegate::name_rect(const QStyleOptionViewItem &option,
                                       const QModelIndex &index) const {
    const auto thumbnail = thumbnail_rect(option);
    const QRect bounds(thumbnail.left(), thumbnail.bottom() + 9, extent_,
                       QFontMetrics(option.font).height() + 2);
    auto name = bounds;
    const auto badge = page_badge_rect(option, index);
    if (!badge.isEmpty()) {
        name.setWidth(std::max(0, extent_ - badge.width() - 8));
    }
    return QStyle::visualRect(option.direction, bounds, name);
}

QRect DirectoryItemDelegate::preview_content_rect(const QStyleOptionViewItem &option,
                                                  const QModelIndex &index) const {
    const auto thumbnail = thumbnail_rect(option);
    const auto image = index.data(DirectoryListModel::ThumbnailRole).value<QImage>();
    if (image.isNull()) {
        return thumbnail;
    }
    auto image_size = image.size();
    image_size.scale(thumbnail.size(), Qt::KeepAspectRatio);
    return {QPoint(thumbnail.center().x() - image_size.width() / 2,
                   thumbnail.center().y() - image_size.height() / 2),
            image_size};
}

QRegion DirectoryItemDelegate::item_hit_region(const QStyleOptionViewItem &option,
                                               const QModelIndex &index) const {
    QRegion hit(thumbnail_rect(option));
    hit += name_rect(option, index);
    const auto badge = page_badge_rect(option, index);
    if (!badge.isEmpty()) {
        hit += badge;
    }
    const auto status_badge = status_badge_rect(option, index);
    if (!status_badge.isEmpty()) {
        hit += status_badge;
    }
    const auto directory = index.data(DirectoryListModel::EntryKindRole).toInt() ==
                           static_cast<int>(core::EntryKind::directory);
    if (!directory && !QFileInfo(index.data(Qt::DisplayRole).toString()).suffix().isEmpty()) {
        const auto name = name_rect(option, index);
        const auto thumbnail = thumbnail_rect(option);
        hit += QRect(thumbnail.left(), name.bottom() + 1, extent_,
                     QFontMetrics(option.font).height() + 2);
    }
    return hit;
}

QRect DirectoryItemDelegate::status_badge_rect(const QStyleOptionViewItem &option,
                                               const QModelIndex &index) const {
    const auto image = index.data(DirectoryListModel::ThumbnailRole).value<QImage>();
    const auto raw_status = index.data(DirectoryListModel::PreviewStatusRole).toInt();
    const auto status = static_cast<preview::helper_protocol::ResponseStatus>(raw_status);
    if (image.isNull() ||
        (status != preview::helper_protocol::ResponseStatus::success_offline_cached &&
         raw_status != DirectoryListModel::VerificationPendingStatus)) {
        return {};
    }
    const auto text =
        raw_status == DirectoryListModel::VerificationPendingStatus
            ? QCoreApplication::translate("DirectoryItemDelegate", "Checking...")
            : QCoreApplication::translate("DirectoryItemDelegate", "Cached - offline");
    auto font = option.font;
    font.setBold(true);
    font.setPointSize(std::max(7, font.pointSize() - 1));
    const auto width = std::min(extent_ - 14, QFontMetrics(font).horizontalAdvance(text) + 14);
    const auto thumbnail = thumbnail_rect(option);
    return {thumbnail.right() - width - 7, thumbnail.bottom() - 27, width, 20};
}

void DirectoryItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setLayoutDirection(option.direction);

    const bool selected = option.state.testFlag(QStyle::State_Selected);
    painter->fillRect(option.rect, option.palette.base());

    const bool directory = index.data(DirectoryListModel::EntryKindRole).toInt() ==
                           static_cast<int>(core::EntryKind::directory);
    const auto thumbnail = thumbnail_rect(option);
    if (selected) {
        painter->fillRect(thumbnail, option.palette.alternateBase());
    }

    const auto image = index.data(DirectoryListModel::ThumbnailRole).value<QImage>();
    if (!image.isNull()) {
        const auto image_rect = preview_content_rect(option, index);
        // Folder mosaics already contain a white canvas for each individual artwork.
        if (!directory) {
            painter->fillRect(image_rect, Qt::white);
        }
        painter->drawImage(image_rect, image);
    } else {
        const auto status = static_cast<preview::helper_protocol::ResponseStatus>(
            index.data(DirectoryListModel::PreviewStatusRole).toInt());
        if (!directory &&
            (status == preview::helper_protocol::ResponseStatus::ghostscript_required ||
             status == preview::helper_protocol::ResponseStatus::embedded_preview_unavailable)) {
            auto message_font = option.font;
            message_font.setBold(true);
            painter->setFont(message_font);
            painter->setPen(option.palette.text().color());
            QString message;
            if (status == preview::helper_protocol::ResponseStatus::ghostscript_required) {
                message = ghostscript_available()
                              ? QCoreApplication::translate("DirectoryItemDelegate",
                                                            "Ghostscript\nrequired")
                              : QCoreApplication::translate("DirectoryItemDelegate",
                                                            "Ghostscript\nnot installed");
            } else {
                message =
                    QCoreApplication::translate("DirectoryItemDelegate", "No embedded\npreview");
            }
            painter->drawText(thumbnail.adjusted(10, 10, -10, -10),
                              Qt::AlignCenter | Qt::TextWordWrap, message);
        } else {
            const auto &placeholder = directory ? folderPlaceholder_ : filePlaceholder_;
            if (!placeholder.isNull()) {
                painter->drawImage(thumbnail, placeholder);
            }
        }
    }

    const auto raw_preview_status = index.data(DirectoryListModel::PreviewStatusRole).toInt();
    const auto preview_status =
        static_cast<preview::helper_protocol::ResponseStatus>(raw_preview_status);
    if (!image.isNull() &&
        (preview_status == preview::helper_protocol::ResponseStatus::success_offline_cached ||
         raw_preview_status == DirectoryListModel::VerificationPendingStatus)) {
        const auto text =
            raw_preview_status == DirectoryListModel::VerificationPendingStatus
                ? QCoreApplication::translate("DirectoryItemDelegate", "Checking...")
                : QCoreApplication::translate("DirectoryItemDelegate", "Cached - offline");
        auto offline_font = option.font;
        offline_font.setBold(true);
        offline_font.setPointSize(std::max(7, offline_font.pointSize() - 1));
        const auto offline_badge = status_badge_rect(option, index);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(QStringLiteral("#48545E")));
        painter->drawRoundedRect(offline_badge, 3, 3);
        painter->setPen(Qt::white);
        painter->setFont(offline_font);
        painter->drawText(offline_badge, Qt::AlignCenter, text);
    }
    painter->setFont(option.font);
    const auto file_name = index.data(Qt::DisplayRole).toString();
    const QFileInfo file_info(file_name);
    const auto suffix = directory ? QString{} : file_info.suffix();
    auto base_name = file_name;
    if (!suffix.isEmpty()) {
        base_name.chop(suffix.size() + 1);
    }
    const QFontMetrics metrics(option.font);
    auto extension_font = option.font;
    extension_font.setBold(true);
    const auto name_rect = this->name_rect(option, index);
    QRect extension_rect(thumbnail.left(), name_rect.bottom() + 1, extent_, metrics.height() + 2);
    const auto page_count = index.data(DirectoryListModel::PageCountRole).toULongLong();
    if (!directory && page_count > 1) {
        const auto text = QString::number(page_count);
        auto count_font = option.font;
        if (count_font.pointSizeF() > 0) {
            count_font.setPointSizeF(count_font.pointSizeF() * 0.9);
        } else {
            count_font.setPixelSize(std::max(1, qRound(count_font.pixelSize() * 0.9)));
        }
        auto glyphs = QFontMetricsF(count_font).tightBoundingRect(text);
        const auto page_badge = page_badge_rect(option, index);
        const auto badge_width = page_badge.width();
        const auto badge_height = page_badge.height();

        // Keep every digit inside the authored badge, including at the smallest grid size.
        const auto scale =
            std::min({qreal(1), (badge_width - 4) / std::max(qreal(1), glyphs.width()),
                      (badge_height - 4) / std::max(qreal(1), glyphs.height())});
        if (scale < 1) {
            if (count_font.pointSizeF() > 0) {
                count_font.setPointSizeF(count_font.pointSizeF() * scale);
            } else {
                count_font.setPixelSize(std::max(1, qFloor(count_font.pixelSize() * scale)));
            }
            glyphs = QFontMetricsF(count_font).tightBoundingRect(text);
        }
        painter->setPen(QPen(pageBorderColor_, 1.0));
        painter->setBrush(pageBackgroundColor_);
        painter->drawRect(QRectF(page_badge).adjusted(0.5, 0.5, -0.5, -0.5));
        painter->setFont(count_font);
        painter->setPen(pageTextColor_);
        painter->save();
        painter->setClipRect(page_badge.adjusted(2, 2, -2, -2), Qt::IntersectClip);
        painter->drawText(QRectF(page_badge).center() - glyphs.center(), text);
        painter->restore();
    }
    painter->setFont(option.font);
    painter->setPen(directory ? folderTextColor_ : fileTextColor_);
    painter->drawText(name_rect, Qt::AlignLeading | Qt::AlignVCenter,
                      metrics.elidedText(base_name, Qt::ElideMiddle, name_rect.width()));

    if (!suffix.isEmpty()) {
        painter->setFont(extension_font);
        painter->setPen(format_color(suffix, option.palette.base().color()));
        painter->drawText(extension_rect, Qt::AlignLeading | Qt::AlignVCenter, suffix.toUpper());
    }
    if (directory && !selected) {
        QPen border(folderFrameColor_);
        border.setWidthF(1.5);
        painter->setPen(border);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(thumbnail).adjusted(0.75, 0.75, -0.75, -0.75));
    }
    if (selected) {
        QPen border(QColor(QStringLiteral("#F5A623")));
        border.setWidth(2);
        painter->setPen(border);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(thumbnail.adjusted(0, 0, -1, -1));
    }
    painter->restore();
}

QSize DirectoryItemDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const {
    constexpr int horizontal_air = 48;
    constexpr int vertical_air_and_labels = 62;
    return {extent_ + horizontal_air, std::max(48, extent_ * 3 / 4) + vertical_air_and_labels};
}

} // namespace vove::ui
