#include "create_folder_button.hpp"
#include "round_button_artwork.hpp"

#include <QIconEngine>
#include <QPainter>

#include <algorithm>
#include <array>

namespace vove::ui {
namespace {

struct Palette {
    QRgb mark;
    QRgb ring;
    QRgb active;
    QRgb disabled_mark;
    QRgb disabled_ring;
};

// Normal colors are authored; interaction colors follow the adjacent navigation controls.
constexpr std::array palettes{Palette{qRgb(211, 211, 211), qRgb(178, 178, 178), qRgb(237, 196, 95),
                                      qRgb(178, 178, 178), qRgb(178, 178, 178)},
                              Palette{qRgb(93, 165, 181), qRgb(142, 179, 193), qRgb(255, 127, 0),
                                      qRgb(186, 175, 145), qRgb(82, 106, 112)},
                              Palette{qRgb(241, 228, 187), qRgb(142, 179, 193), qRgb(255, 127, 0),
                                      qRgb(82, 106, 112), qRgb(82, 106, 112)},
                              Palette{qRgb(91, 91, 91), qRgb(178, 178, 178), qRgb(93, 165, 181),
                                      qRgb(91, 91, 91), qRgb(91, 91, 91)}};

const QPainterPath &plus_path() {
    static const auto path = [] {
        QPainterPath plus;
        plus.moveTo(6.391, -1.821);
        plus.lineTo(1.821, -1.821);
        plus.lineTo(1.821, -6.391);
        plus.lineTo(-1.821, -6.391);
        plus.lineTo(-1.821, -1.821);
        plus.lineTo(-6.391, -1.821);
        plus.lineTo(-6.391, 1.821);
        plus.lineTo(-1.821, 1.821);
        plus.lineTo(-1.821, 6.391);
        plus.lineTo(1.821, 6.391);
        plus.lineTo(1.821, 1.821);
        plus.lineTo(6.391, 1.821);
        plus.closeSubpath();
        return plus;
    }();
    return path;
}

class CreateFolderIcon final : public QIconEngine {
  public:
    explicit CreateFolderIcon(const AppTheme theme) : theme_(theme) {}

    QIconEngine *clone() const override {
        return new CreateFolderIcon(theme_);
    }

    void paint(QPainter *painter, const QRect &rect, const QIcon::Mode mode,
               QIcon::State) override {
        const auto &palette = palettes.at(static_cast<std::size_t>(theme_));
        const auto disabled = mode == QIcon::Disabled;
        const auto active = mode == QIcon::Active || mode == QIcon::Selected;
        const auto mark = disabled ? palette.disabled_mark : active ? palette.active : palette.mark;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(QRectF(rect).center());
        const auto scale = std::min(rect.width(), rect.height()) / 24.0;
        painter->scale(scale, scale);
        painter->fillPath(artwork::round_button_ring(),
                          QColor(disabled ? palette.disabled_ring : palette.ring));
        painter->fillPath(plus_path(), QColor(mark));
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, const QIcon::Mode mode, const QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint{}, size), mode, state);
        return result;
    }

  private:
    AppTheme theme_;
};

} // namespace

QIcon create_folder_icon(const AppTheme theme) {
    return QIcon(new CreateFolderIcon(theme));
}

CreateFolderButton::CreateFolderButton(QWidget *parent) : QToolButton(parent) {
    setFixedSize(28, 28);
    setIconSize(QSize(24, 24));
    setAutoRaise(true);
    setFocusPolicy(Qt::StrongFocus);
}

void CreateFolderButton::paintEvent(QPaintEvent *) {
    const auto active = underMouse() || isDown() ||
                        (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange));
    const auto mode = !isEnabled() ? QIcon::Disabled : active ? QIcon::Active : QIcon::Normal;
    QPainter painter(this);
    icon().paint(&painter, rect().adjusted(2, 2, -2, -2), Qt::AlignCenter, mode);
}

} // namespace vove::ui
