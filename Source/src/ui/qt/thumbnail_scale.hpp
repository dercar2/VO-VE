#pragma once

class QSlider;
class QToolButton;
class QWidget;

namespace vove::ui {

enum class AppTheme : int;

struct ThumbnailScaleControls {
    QWidget *widget;
    QToolButton *smaller;
    QSlider *slider;
    QToolButton *larger;
};

ThumbnailScaleControls create_thumbnail_scale(QWidget *parent);
void apply_thumbnail_scale_theme(const ThumbnailScaleControls &controls, AppTheme theme);

} // namespace vove::ui
