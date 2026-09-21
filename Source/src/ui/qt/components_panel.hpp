#pragma once

#include <QString>
#include <QUrl>
#include <QWidget>

#include <vector>

class QEvent;
class QScrollArea;

namespace vove::ui {

struct ComponentSpec {
    QString name;
    QString purpose;
    QUrl url;
    bool installed{};
    bool required{};
};

class ComponentsPanel final : public QWidget {
  public:
    explicit ComponentsPanel(QWidget *parent = nullptr);

    void set_components(const std::vector<ComponentSpec> &components);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override;
    [[nodiscard]] int heightForWidth(int width) const override;

  protected:
    void changeEvent(QEvent *event) override;

  private:
    QScrollArea *scrollArea_{};
};

} // namespace vove::ui
