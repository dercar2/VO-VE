#include "format_style.hpp"

namespace vove::ui {

QColor format_color(const QString &suffix, const QColor &background) {
    const bool light = background.lightnessF() > 0.5;
    const auto extension = suffix.toLower();
    if (extension == QStringLiteral("idml") || extension == QStringLiteral("indd") ||
        extension == QStringLiteral("indt")) {
        return QColor(QLatin1String(light ? "#E52B70" : "#FF72A1"));
    }
    if (extension == QStringLiteral("afdesign")) {
        return QColor(QLatin1String(light ? "#0086DF" : "#54B5FF"));
    }
    if (extension == QStringLiteral("afphoto")) {
        return QColor(QLatin1String(light ? "#9746DD" : "#CA8BFA"));
    }
    if (extension == QStringLiteral("afpub") || extension == QStringLiteral("af") ||
        extension == QStringLiteral("aftemplate")) {
        return QColor(QLatin1String(light ? "#E65A21" : "#FF975C"));
    }
    if (extension == QStringLiteral("ai")) {
        return QColor(QLatin1String(light ? "#EE6500" : "#FF9A00"));
    }
    if (extension == QStringLiteral("psd") || extension == QStringLiteral("psb")) {
        return QColor(QLatin1String(light ? "#0086DF" : "#31A8FF"));
    }
    if (extension == QStringLiteral("cdr")) {
        return QColor(QLatin1String(light ? "#009735" : "#00A651"));
    }
    if (extension == QStringLiteral("kra") || extension == QStringLiteral("ora")) {
        return QColor(QLatin1String(light ? "#BF35BC" : "#E591DF"));
    }
    if (extension == QStringLiteral("xcf")) {
        return QColor(QLatin1String(light ? "#5A8500" : "#A8D45F"));
    }
    if (extension == QStringLiteral("dxf")) {
        return QColor(QLatin1String(light ? "#006FC7" : "#67B9F2"));
    }
    if (extension == QStringLiteral("pdf")) {
        return QColor(QLatin1String(light ? "#F01936" : "#FF6B6B"));
    }
    if (extension == QStringLiteral("eps") || extension == QStringLiteral("ps")) {
        return QColor(QLatin1String(light ? "#D67600" : "#F5A623"));
    }
    if (extension == QStringLiteral("jpg") || extension == QStringLiteral("jpeg")) {
        return QColor(QLatin1String(light ? "#008EAE" : "#52C7E8"));
    }
    if (extension == QStringLiteral("png")) {
        return QColor(QLatin1String(light ? "#00996A" : "#48D6B2"));
    }
    if (extension == QStringLiteral("bmp")) {
        return QColor(QLatin1String(light ? "#007DDF" : "#7CC7FF"));
    }
    if (extension == QStringLiteral("gif")) {
        return QColor(QLatin1String(light ? "#B42CDF" : "#D08AF9"));
    }
    if (extension == QStringLiteral("tif") || extension == QStringLiteral("tiff")) {
        return QColor(QLatin1String(light ? "#8455EE" : "#A99AFF"));
    }
    if (extension == QStringLiteral("webp")) {
        return QColor(QLatin1String(light ? "#00998E" : "#35D4C7"));
    }
    if (extension == QStringLiteral("ico")) {
        return QColor(QLatin1String(light ? "#697D88" : "#B0BEC8"));
    }
    if (extension == QStringLiteral("svg") || extension == QStringLiteral("svgz") ||
        extension == QStringLiteral("plt") || extension == QStringLiteral("hpgl")) {
        return QColor(QLatin1String(light ? "#5B9000" : "#9AD95D"));
    }
    if (extension == QStringLiteral("raw") || extension == QStringLiteral("cr2") ||
        extension == QStringLiteral("cr3") || extension == QStringLiteral("nef") ||
        extension == QStringLiteral("arw") || extension == QStringLiteral("dng") ||
        extension == QStringLiteral("orf") || extension == QStringLiteral("raf") ||
        extension == QStringLiteral("rw2")) {
        return QColor(QLatin1String(light ? "#A27A00" : "#F2D056"));
    }
    if (extension == QStringLiteral("heif") || extension == QStringLiteral("heic")) {
        return QColor(QLatin1String(light ? "#DC2B94" : "#F38AC3"));
    }
    if (extension == QStringLiteral("avif")) {
        return QColor(QLatin1String(light ? "#F04129" : "#FB817A"));
    }
    if (extension == QStringLiteral("jxl")) {
        return QColor(QLatin1String(light ? "#426FEC" : "#7FA7FF"));
    }
    return QColor(QLatin1String(light ? "#697580" : "#AEB8BD"));
}

} // namespace vove::ui
