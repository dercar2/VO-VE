#pragma once

#include <QCursor>
#include <QLabel>
#include <QPointF>
#include <QRectF>

namespace vove::ui {

class ZoomPreview final : public QLabel {
  public:
    explicit ZoomPreview(QWidget *parent = nullptr);
    explicit ZoomPreview(const QString &text, QWidget *parent = nullptr);

    // QLabel's content setters are not virtual: keep the owning pointer typed as ZoomPreview.
    void setPixmap(const QPixmap &pixmap);
    void setText(const QString &text);
    void setMovie(QMovie *movie);
    void clear();
    // Raw decoded pixels; fit uses the widget DPR, not the image's metadata DPR.
    // Include source identity/revision and page in the key; a better raster for that key keeps zoom.
    void set_preview(const QImage &image, const QString &stable_key = {});
    void set_indicator_font(const QFont &font);
    void reset_zoom();

    [[nodiscard]] qreal zoom_factor() const { return zoom_; }
    [[nodiscard]] QPointF pan_offset() const { return pan_; }
    [[nodiscard]] QRectF image_rect() const;

  protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    [[nodiscard]] QRectF image_area() const;
    [[nodiscard]] QSizeF fitted_size() const;
    [[nodiscard]] bool can_pan() const;
    void zoom_at(qreal factor, const QPointF &anchor);
    void clamp_pan();
    void stop_pan();
    void refresh_indicator();
    void refresh_cursor();

    QLabel *indicator_{};
    QPixmap source_;
    qreal zoom_{1.0};
    QPointF pan_;
    QPointF dragPosition_;
    qint64 imageKey_{};
    QString sourceKey_;
    bool panning_{};
    bool ownsCursor_{};
    bool hadExplicitCursor_{};
    QCursor previousCursor_;
};

} // namespace vove::ui
