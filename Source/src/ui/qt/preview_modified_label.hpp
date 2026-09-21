#pragma once

#include <QLabel>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <optional>

namespace vove::ui {

class PreviewModifiedLabel final : public QLabel {
  public:
    using Clock = std::function<qint64()>;

    explicit PreviewModifiedLabel(QWidget *parent = nullptr, Clock clock = {});
    void set_modified_time(std::optional<std::int64_t> modified_unix_ns);
    void refresh();

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    Clock clock_;
    QTimer timer_;
    std::optional<std::int64_t> modifiedUnixNs_;
};

} // namespace vove::ui
