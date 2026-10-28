#pragma once

#include "window_settings.hpp"

#include <QColor>
#include <QObject>

#include <functional>
#include <memory>

class QAction;
class QActionGroup;
class QButtonGroup;
class QComboBox;
class QLabel;
class QEvent;
class QMainWindow;
class QMenu;
class QScrollBar;
class QShortcut;
class QSlider;
class QSplitter;
class QToolButton;
class QWidget;

namespace vove::ui {

class AddressField;
class CatalogToolButton;
class CatalogView;
class DirectoryTreeView;
class FavoritesView;
class NavigationButton;
class PageButton;
class PreviewModifiedLabel;
class PreviewNameLabel;
class SearchField;
class TranslationManager;
class WindowControls;
class ZoomPreview;

// Non-owning widget handles. Qt parent ownership stays with the target QMainWindow.
// Models, delegates, controller state and command implementations are deliberately absent.
struct ViewerWidgets {
    QWidget *toolbarContent{};
    CatalogView *listView{};
    FavoritesView *favorites{};
    QScrollBar *favoritesScrollBar{};
    FavoritesView *trashFavorite{};
    QLabel *favoritesTitle{};
    DirectoryTreeView *directoryTree{};
    QSplitter *mainSplitter{};
    QSplitter *placesSplitter{};
    AddressField *pathEdit{};
    QAction *chooseFolderAction{};
    QWidget *favoritesHeader{};
    QToolButton *addFavoriteButton{};
    QWidget *trashFavoriteDock{};
    QWidget *directoryHeader{};
    SearchField *filterEdit{};
    SearchField *globalSearchEdit{};
    QWidget *sortControls{};
    QComboBox *sortField{};
    QToolButton *sortDirection{};
    QSlider *sizeSlider{};
    QWidget *previewPanel{};
    ZoomPreview *previewCanvas{};
    QLabel *globalSearchMessage{};
    QWidget *pageNavigation{};
    PageButton *previousPageButton{};
    PageButton *nextPageButton{};
    QToolButton *passwordButton{};
    QLabel *pageIndicator{};
    PreviewNameLabel *previewName{};
    PreviewModifiedLabel *previewModified{};
    QLabel *previewFormat{};
    QLabel *previewSupport{};
    QLabel *status{};
    NavigationButton *backButton{};
    NavigationButton *forwardButton{};
    NavigationButton *historyButton{};
    NavigationButton *upButton{};
    QToolButton *createFolderButton{};
    QToolButton *smallerThumbnailsButton{};
    QToolButton *largerThumbnailsButton{};
    CatalogToolButton *recursiveViewButton{};
    CatalogToolButton *refreshButton{};
    QToolButton *logoButton{};
    WindowControls *windowControls{};
    QMenu *historyMenu{};
    QMenu *mainMenu{};
    QMenu *languageMenu{};
    QAction *createFolderAction{};
    QAction *renameAction{};
    QAction *deleteAction{};
    QAction *copyNamesAction{};
    QAction *cancelPreviewAttemptAction{};
    QAction *copyPathsAction{};
    QAction *copyObjectsAction{};
    QAction *pasteObjectsAction{};
    QAction *trashAction{};
    QAction *settingsAction{};
    QAction *unfinishedOperationAction{};
    QAction *updateAction{};
    QAction *aboutAction{};
    QAction *componentsAction{};
    QAction *donateAction{};
    QAction *reportIssueAction{};
    QActionGroup *languageActions{};
    QButtonGroup *themeButtons{};
    QShortcut *refreshShortcut{};
    QShortcut *backShortcut{};
    QShortcut *forwardShortcut{};
    QShortcut *upShortcut{};
};

struct ViewerStyle {
    AppTheme theme{AppTheme::blue};
    QColor folder_frame;
    QColor folder_text;
    QColor file_text;
    QColor page_background;
    QColor page_text;
    QColor page_border;
    QColor muted;
    QColor hover;
    QColor panel;
};

struct WindowShellCallbacks {
    std::function<void()> preferences_changed;
    std::function<void(AppLanguage)> language_changed;
    std::function<void(const ViewerStyle &)> style_changed;
};

class WindowShell final : public QObject {
  public:
    explicit WindowShell(QMainWindow &window, WindowShellCallbacks callbacks = {},
                         const QString &translation_directory = {});
    ~WindowShell() override;
    [[nodiscard]] const ViewerWidgets &widgets() const noexcept { return widgets_; }
    [[nodiscard]] AppTheme theme() const noexcept { return theme_; }
    [[nodiscard]] AppLanguage language() const noexcept { return language_; }
    [[nodiscard]] bool language_available(AppLanguage language) const;
    [[nodiscard]] ViewerStyle style() const;
    void apply_theme(AppTheme theme);
    [[nodiscard]] bool apply_language(AppLanguage language, bool persist = false);
    void retranslate();
    void apply_artwork_icons();
    void apply_splitter_ratios(double main_share, double favorites_share);
    void restore_presentation(const WindowSettingsSnapshot &snapshot);
    [[nodiscard]] WindowSettingsSnapshot capture_presentation(WindowSettingsSnapshot snapshot = {}) const;

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void build_ui();
    void connect_presentation();
    void changed();

    QMainWindow &window_;
    ViewerWidgets widgets_;
    WindowShellCallbacks callbacks_;
    std::unique_ptr<TranslationManager> translations_;
    AppTheme theme_{AppTheme::blue};
    AppLanguage language_{AppLanguage::english};
    double mainShare_{0.60};
    double favoritesShare_{0.30};
};

} // namespace vove::ui
