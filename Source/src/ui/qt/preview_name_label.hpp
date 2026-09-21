#pragma once

#include <QTextEdit>

namespace vove::ui {

class PreviewNameLabel final : public QTextEdit {
  public:
    explicit PreviewNameLabel(QWidget *parent = nullptr);
    void set_full_text(const QString &text);
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void update_scroll_policy();
};

} // namespace vove::ui
