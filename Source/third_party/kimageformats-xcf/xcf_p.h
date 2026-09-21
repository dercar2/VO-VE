/*
    xcf.cpp: A Qt 5 plug-in for reading GIMP XCF image files
    SPDX-FileCopyrightText: 2001 lignum Computing Inc. <allen@lignumcomputing.com>
    SPDX-FileCopyrightText: 2004 Melchior FRANZ <mfranz@kde.org>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#ifndef KIMG_XCF_P_H
#define KIMG_XCF_P_H

#include <QImageIOHandler>

class XCFHandler : public QImageIOHandler
{
public:
    XCFHandler();

    bool canRead() const override;
    bool read(QImage *image) override;
    bool write(const QImage &image) override;

    bool supportsOption(QImageIOHandler::ImageOption option) const override;
    QVariant option(QImageIOHandler::ImageOption option) const override;
    bool resourceLimitExceeded() const noexcept;
    bool unsupportedCompression() const noexcept;
    bool malformedTileData() const noexcept;

    static bool canRead(QIODevice *device);

private:
    /*!
     * \brief m_imageSize
     * Image size cache used by option()
     */
    QSize m_imageSize;
    bool m_resourceLimitExceeded{};
    bool m_unsupportedCompression{};
    bool m_malformedTileData{};
};

#endif // KIMG_XCF_P_H
