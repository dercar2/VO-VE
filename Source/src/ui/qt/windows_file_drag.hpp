#pragma once

#include <QMimeData>
#include <QUrl>
#include <QWindowsMimeConverter>
#include <QtCore/qt_windows.h>

#include <algorithm>
#include <cstring>

namespace vove::ui::detail {

// Qt suppresses implicit text/URL conversions only for a single-format file drag.
// Keep private routing data accessible locally and export it separately through OLE.
class WindowsFileDragMimeData final : public QMimeData {
  public:
    static bool supports(const QMimeData &source) {
        const auto urls = source.urls();
        const auto formats = source.formats();
        return !urls.isEmpty() &&
               std::all_of(urls.cbegin(), urls.cend(), [](const QUrl &url) {
                   return url.isLocalFile();
               }) &&
               std::all_of(formats.cbegin(), formats.cend(), [](const QString &format) {
                   return format == QStringLiteral("text/uri-list") ||
                          format == QStringLiteral("application/x-vove-entry-set-v1") ||
                          format == QStringLiteral("application/x-vove-directory-tree-drag");
               });
    }

    explicit WindowsFileDragMimeData(const QMimeData &source) {
        setUrls(source.urls());
        for (const auto &format : source.formats()) {
            if (format != QStringLiteral("text/uri-list")) setData(format, source.data(format));
        }
    }

    QStringList formats() const override { return {QStringLiteral("text/uri-list")}; }
    bool hasFormat(const QString &format) const override {
        return QMimeData::formats().contains(format);
    }
    QStringList routing_formats() const {
        auto result = QMimeData::formats();
        result.removeAll(QStringLiteral("text/uri-list"));
        return result;
    }
};

class WindowsFileDragMetadata final : public QWindowsMimeConverter {
  public:
    explicit WindowsFileDragMetadata(const WindowsFileDragMimeData *source) : source_(source) {
        for (const auto &format : source->routing_formats()) {
            const auto id = registerMimeType(format);
            const auto bytes = source->data(format);
            if (id != 0 && !bytes.isEmpty())
                formats_.push_back({static_cast<CLIPFORMAT>(id), bytes});
        }
    }

    bool ready() const { return formats_.size() == source_->routing_formats().size(); }

    bool canConvertFromMime(const FORMATETC &format, const QMimeData *source) const override {
        return source == source_ && format.dwAspect == DVASPECT_CONTENT && format.lindex == -1 &&
               (format.tymed & TYMED_HGLOBAL) != 0 && payload(format.cfFormat) != nullptr;
    }

    bool convertFromMime(const FORMATETC &format, const QMimeData *source,
                         STGMEDIUM *medium) const override {
        if (medium == nullptr || !canConvertFromMime(format, source)) return false;
        const auto &bytes = *payload(format.cfFormat);
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
        if (memory == nullptr) return false;
        auto *destination = GlobalLock(memory);
        if (destination == nullptr) {
            GlobalFree(memory);
            return false;
        }
        std::memcpy(destination, bytes.constData(), static_cast<std::size_t>(bytes.size()));
        GlobalUnlock(memory);
        *medium = {};
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        return true;
    }

    QList<FORMATETC> formatsForMime(const QString &mime, const QMimeData *source) const override {
        QList<FORMATETC> result;
        if (source == source_ && mime == QStringLiteral("text/uri-list")) {
            for (const auto &format : formats_) {
                result.push_back({format.id, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
            }
        }
        return result;
    }

    bool canConvertToMime(const QString &, IDataObject *) const override { return false; }
    QVariant convertToMime(const QString &, IDataObject *, QMetaType) const override { return {}; }
    QString mimeForFormat(const FORMATETC &) const override { return {}; }

  private:
    struct Format {
        CLIPFORMAT id;
        QByteArray bytes;
    };
    const QByteArray *payload(const CLIPFORMAT id) const {
        for (const auto &format : formats_) {
            if (format.id == id && !format.bytes.isEmpty()) return &format.bytes;
        }
        return nullptr;
    }
    const WindowsFileDragMimeData *source_;
    QList<Format> formats_;
};

} // namespace vove::ui::detail
