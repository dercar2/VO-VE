#pragma once

#include "vove/catalog/catalog_session.hpp"

#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <optional>

class QLabel;

namespace vove::ui {

class OperationNotice;

struct TrashStatusSummary {
    bool available{};
    std::uint64_t total_items{};
    std::uint64_t total_bytes{};
};

// Values only: no model, coordinator, backend commands or root-window access.
struct CatalogStatusSnapshot {
    catalog::CatalogSessionUpdate update;
    bool trash_view{};
    bool global_search{};
    bool recursive_view{};
    std::uint64_t visible_items{};
    std::uint64_t total_items{};
    std::uint64_t visible_directories{};
    QString search_status;
    QStringList search_roots;
    std::optional<TrashStatusSummary> trash; // Missing means still loading.
};

struct OperationStatusActivity {
    bool active{}; // Presentation only; FileCommands remains the busy-state authority.
    bool recovery_pending{};
    bool journal_blocked{};
};

struct DragStatusMessage {
    bool transfer{};
    bool move{};
    int count{};
    QString message;
};

// Integration/removal manifest (original MainWindow.cpp line numbers):
// - Replace 454-477, 1677-1705, 8642-8867 and the 2362-2368 pulse timer with
//   this owner (the shell supplies the theme accent; fixed/blend helpers live here).
//   Delete operationStatus_, dragStatus_, selectedName_/selectedColorSummary_
//   presentation copies, operationStatusPulseTimer_/Phase_ and their helpers there.
// - Catalog publishes CatalogStatusSnapshot on catalog/mode/filter/search/Trash
//   changes, using TrashController's summary. Keep update_catalog_tools in Catalog,
//   not in a forwarding update_status. Catalog clears messages explicitly with
//   setOperationMessage({}) at the original navigation/refresh clear points.
// - Selection publishes selection_status_text(); Preview publishes selectedName()
//   and selectedColorSummary(). No formatting callback into MainWindow is needed.
// - FileCommands status_changed -> setOperationMessage; activity_changed ->
//   setOperationActivity({mutation_in_flight(), recovery_pending(), journal_blocked()}).
//   Recovery is the old OR of directory/trash/file/delete/batch recovery flags.
//   Activity MUST NOT resend FileCommands' cached status: Catalog may have cleared
//   it. Non-operation notices use setOperationMessage too, without backend control.
// - DropRouter optional<DragStatusMessage> -> setDragStatus (nullopt clears).
//   Mirror dragStatusActive()
//   to the legacy window diagnostic if needed; never use diagnostics as authority.
// - FileCommands file_in_use -> notifyFileInUse (replaces 5978-5982). Pass the
//   shell's OperationNotice, if used; its existing fade/dismiss lifecycle is retained.
// - Theme -> setThemeColors(style.muted, #0080B3 for white/vanilla, otherwise
//   #F5A623); delete the old operation_status_accent helper, do not forward it.
//   Retranslate -> setInterfaceFont(qApp->font()), then retranslate(). Delete only
//   status_->setFont from retranslate_shell; other preview fonts stay with Preview.
//   Owners republish translated operation/selection/search/selected-color strings.
// - Remove direct writes to these presentation strings; send values here instead.
//   Backend operation/recovery state and catalog/action gating stay in their owners.
class StatusPresenter final : public QObject {
  public:
    explicit StatusPresenter(QLabel &status, OperationNotice *notice = nullptr,
                             QObject *parent = nullptr);
    ~StatusPresenter() override;

    void setCatalogSnapshot(CatalogStatusSnapshot snapshot);
    void setSelectionSummary(QString summary);
    void setSelectedFile(QString name, QString color_summary = {});
    void setOperationMessage(QString message);
    void setOperationActivity(OperationStatusActivity activity);
    void setDragStatus(std::optional<DragStatusMessage> message);
    void showDragStatus(const DragStatusMessage &message);
    void clearDragStatus();
    [[nodiscard]] bool dragStatusActive() const noexcept;
    [[nodiscard]] const QString &operationMessage() const noexcept;

    // Exact legacy accents: light themes #0080B3; blue/dark #F5A623.
    // Technical #FF7900 and drag #ff414c/#00b7ff are intentionally fixed.
    void setThemeColors(QColor muted, QColor operation_accent);
    void setInterfaceFont(const QFont &base_font);
    void retranslate();
    void notifyFileInUse();

  private:
#ifdef VOVE_UI_TEST_HOOKS
    friend struct StatusPresenterTestAccess;
#endif
    void render();
    void updatePulse();
    void advancePulse();
    void applyColor();
    [[nodiscard]] QString catalogText() const;
    [[nodiscard]] QString withSelectionAndOperation(QString text, bool include_color) const;

    QPointer<QLabel> status_;
    QPointer<OperationNotice> notice_;
    CatalogStatusSnapshot catalog_;
    OperationStatusActivity activity_;
    QString selectionSummary_;
    QString operationStatus_;
    QString dragStatus_;
    std::optional<DragStatusMessage> dragMessage_;
    QString selectedName_;
    QString selectedColorSummary_;
    QColor muted_{QStringLiteral("#5DA9B5")};
    QColor operationAccent_{QStringLiteral("#F5A623")};
    QTimer operationStatusPulseTimer_;
    int operationStatusPulsePhase_{};
    bool technicalStatusActive_{};
};

} // namespace vove::ui
