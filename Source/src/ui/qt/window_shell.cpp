#include "window_shell.hpp"
#include "add_favorite_button.hpp"
#include "catalog_tool_button.hpp"
#include "create_folder_button.hpp"
#include "drag_views.hpp"
#include "format_style.hpp"
#include "interface_font.hpp"
#include "main_menu_dialog.hpp"
#include "navigation_button.hpp"
#include "page_button.hpp"
#include "preview_modified_label.hpp"
#include "preview_name_label.hpp"
#include "scrollbar_style.hpp"
#include "smooth_scroll.hpp"
#include "theme_switch.hpp"
#include "thumbnail_scale.hpp"
#include "toolbar_fields.hpp"
#include "translation_manager.hpp"
#include "window_controls.hpp"
#include "window_frame.hpp"
#include "window_placement.hpp"
#include "zoom_preview.hpp"
#include "vove/core/directory_model.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPolygonF>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace vove::ui {
namespace {
constexpr int favoriteKindRole = Qt::UserRole + 1;
constexpr int trashFavoriteKind = 1;
bool is_trash_favorite(const QListWidgetItem *item) {
    return item && item->data(favoriteKindRole).toInt() == trashFavoriteKind;
}

struct ThemeColors {
    QString window;
    QString base;
    QString panel;
    QString tile;
    QString input;
    QString text;
    QString muted;
    QString border;
    QString hover;
    QColor folderFrame;
    QColor folderText;
    QColor fileText;
    QColor pageBackground;
    QColor pageText;
    QColor pageBorder;
};

QIcon directory_picker_icon(const QColor &color, const qreal dpr) {
    QPixmap pixmap(QSize(qCeil(20 * dpr), qCeil(20 * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(QPolygonF{QPointF(7, 4), QPointF(13, 10), QPointF(7, 16)});
    return QIcon(pixmap);
}

[[nodiscard]] QString global_search_tooltip() {
    return QCoreApplication::translate(
        "MainWindow", "Search approved work folders with the system filename index");
}

QIcon authored_logo(const AppTheme theme) {
    const std::array resources{":/artwork/logo-north.png", ":/artwork/logo-vanilla.png",
                               ":/artwork/logo-breeze.png", ":/artwork/logo-twilight.png"};
    return QIcon(QString::fromLatin1(resources.at(static_cast<std::size_t>(theme))));
}

QFrame *toolbar_separator(QWidget *parent) {
    auto *separator = new QFrame(parent);
    separator->setObjectName(QStringLiteral("toolbarSeparator"));
    separator->setFrameShape(QFrame::VLine);
    separator->setFixedWidth(7);
    return separator;
}

ThemeColors theme_colors(const AppTheme theme) {
    switch (theme) {
    case AppTheme::white:
        return {QStringLiteral("#F2F2F2"), QStringLiteral("#F2F2F2"), QStringLiteral("#F2F2F2"),
                QStringLiteral("#F7F8F8"), QStringLiteral("#F2F2F2"), QStringLiteral("#202426"),
                QStringLiteral("#626A6E"), QStringLiteral("#5B5B5B"), QStringLiteral("#E7E9EA"),
                QColor("#B2B2B2"),         QColor("#5B5B5B"),         QColor("#606060"),
                QColor("#B2B2B2"),         QColor("#606060"),         QColor("#606060")};
    case AppTheme::vanilla:
        return {QStringLiteral("#F1E4BB"), QStringLiteral("#F1E4BB"), QStringLiteral("#F1E4BB"),
                QStringLiteral("#E0CEAA"), QStringLiteral("#F1E4BB"), QStringLiteral("#30525B"),
                QStringLiteral("#617774"), QStringLiteral("#2F525B"), QStringLiteral("#E0CEAA"),
                QColor("#5BA9B3"),         QColor("#A67C52"),         QColor("#2F525B"),
                QColor("#A67C52"),         QColor("#F1E4BB"),         QColor("#003244")};
    case AppTheme::blue:
        return {QStringLiteral("#213840"), QStringLiteral("#213840"), QStringLiteral("#213840"),
                QStringLiteral("#013145"), QStringLiteral("#213840"), QStringLiteral("#F1E4BA"),
                QStringLiteral("#5DA9B5"), QStringLiteral("#F1E4BB"), QStringLiteral("#2E525C"),
                QColor("#F1E4BB"),         QColor("#5EA5B6"),         QColor("#F1E4BB"),
                QColor("#013145"),         QColor("#F1E4BB"),         QColor("#FF7900")};
    case AppTheme::dark_gray:
        return {QStringLiteral("#333333"), QStringLiteral("#333333"), QStringLiteral("#333333"),
                QStringLiteral("#1F2123"), QStringLiteral("#333333"), QStringLiteral("#ECEFF1"),
                QStringLiteral("#A8B0B4"), QStringLiteral("#B2B2B2"), QStringLiteral("#383B3D"),
                QColor("#5B5B5B"),         QColor("#F2F2F2"),         QColor("#B2B2B2"),
                QColor("#606060"),         QColor("#B2B2B2"),         QColor("#B2B2B2")};
    }
    return theme_colors(AppTheme::blue);
}

QString theme_style_sheet(const ThemeColors &colors) {
    auto style = QStringLiteral(R"(
QMainWindow, QWidget#centralRoot {
    background: %WINDOW%;
    color: %TEXT%;
}
QDialog[mainMenuDialog="true"] {
    background: %WINDOW%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
}
QDialog[mainMenuDialog="true"] QLabel#mainMenuDialogTitle {
    border: 0;
    border-bottom: 1px solid %BORDER%;
    padding-bottom: 8px;
}
QDialog[mainMenuDialog="true"] QPushButton[mainMenuCloseButton="true"] {
    background: %INPUT%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 3px;
    padding: 4px 14px;
}
QDialog[mainMenuDialog="true"] QPushButton[mainMenuCloseButton="true"]:enabled:hover,
QDialog[mainMenuDialog="true"] QPushButton[mainMenuCloseButton="true"]:enabled:pressed {
    background: #FF7900;
    color: #182126;
    border-color: #FF7900;
}
QDialog[mainMenuDialog="true"] QPushButton[mainMenuCloseButton="true"]:focus {
    border-color: #FF7900;
}
QDialog[mainMenuDialog="true"] QPushButton[mainMenuCloseButton="true"]:disabled {
    color: %MUTED%;
}
QWidget#topBar, QWidget#rightTop, QWidget#previewPanel {
    background: %PANEL%;
}
QWidget#topBar {
    border-bottom: 1px solid %BORDER%;
}
QFrame#toolbarSeparator {
    color: %BORDER%;
    border: 0;
    border-left: 1px solid %BORDER%;
    margin: 5px 3px;
}
QFrame#navigationPreviewSeparator {
    background: %BORDER%;
    border: 0;
}
QFrame#favoritesHeader, QFrame#directoryHeader {
    background: %PANEL%;
    border: 0;
    border-bottom: 1px solid %BORDER%;
}
QFrame#favoritesHeader[pinDropActive="true"] {
    background: %HOVER%;
    border-bottom-color: #F5A623;
}
QLineEdit#pathEdit {
    background: %BASE%;
    color: %MUTED%;
    border-radius: 4px;
}
QLineEdit, QComboBox {
    background: %INPUT%;
    color: %TEXT%;
    border: 1px solid %BORDER%;
    border-radius: 2px;
    padding: 1px 5px;
    min-height: 20px;
}
QLineEdit:focus, QComboBox:focus {
    border-color: #F5A623;
}
QComboBox QAbstractItemView {
    background: %INPUT%;
    color: %TEXT%;
    selection-background-color: #F5A623;
    selection-color: #182126;
}
QListView#catalogGrid, QListWidget#favoritesView, QListWidget#trashFavoriteView,
QTreeView#directoryTree {
    background: %BASE%;
    color: %TEXT%;
    border: 0;
    outline: 0;
}
QListWidget#favoritesView, QListWidget#trashFavoriteView, QTreeView#directoryTree {
    color: %MUTED%;
}
QFrame#trashFavoriteDock {
    background: %BASE%;
    border: 0;
}
QListWidget#favoritesView::item, QListWidget#trashFavoriteView::item,
QTreeView#directoryTree::item {
    min-height: 24px;
}
QListWidget#favoritesView::item, QListWidget#trashFavoriteView::item {
    padding-left: 8px;
    padding-right: 8px;
}
QListWidget#favoritesView::item:selected, QListWidget#trashFavoriteView::item:selected,
QTreeView#directoryTree::item:selected {
    background: #F5A623;
    color: #182126;
}
QLabel#sectionTitle, QLabel#previewHint, QTextEdit#previewName {
    color: %TEXT%;
}
QLabel#previewHint, QLabel#previewSupport {
    color: %MUTED%;
}
QToolButton#logo {
    border: 0;
    padding: 0;
}
QToolButton#logo::menu-indicator {
    image: none;
    width: 0;
}
QToolButton {
    background: transparent;
    color: %TEXT%;
    border: 1px solid transparent;
    border-radius: 3px;
}
QToolButton:hover {
    background: %HOVER%;
    border-color: %BORDER%;
}
QToolButton:pressed {
    background: #F5A623;
    color: #182126;
}
QToolButton:disabled {
    color: %MUTED%;
}
QSplitter::handle {
    background: transparent;
}
QSplitter#mainSplitter, QSplitter#placesSplitter {
    background: %BORDER%;
}
QStatusBar {
    background: %PANEL%;
    color: %TEXT%;
    border-top: 1px solid %BORDER%;
}
QStatusBar::item {
    border: 0;
}
)");
    style.replace(QStringLiteral("%WINDOW%"), colors.window);
    style.replace(QStringLiteral("%BASE%"), colors.base);
    style.replace(QStringLiteral("%PANEL%"), colors.panel);
    style.replace(QStringLiteral("%INPUT%"), colors.input);
    style.replace(QStringLiteral("%TEXT%"), colors.text);
    style.replace(QStringLiteral("%MUTED%"), colors.muted);
    style.replace(QStringLiteral("%BORDER%"), colors.border);
    style.replace(QStringLiteral("%HOVER%"), colors.hover);
    return style;
}

double splitter_share(const QSplitter *splitter, const double fallback) {
    const auto sizes = splitter->sizes();
    if (sizes.size() != 2 || sizes[0] + sizes[1] <= 0) {
        return fallback;
    }
    return static_cast<double>(sizes[0]) / static_cast<double>(sizes[0] + sizes[1]);
}

} // namespace

void WindowShell::build_ui() {
    window_.setObjectName(QStringLiteral("mainWindow"));
    window_.setWindowTitle(QStringLiteral("VO-VE"));
    window_.setMinimumSize(980, 620);
    window_.resize(1440, 900);

    auto *central = new QWidget(&window_);
    central->setObjectName(QStringLiteral("centralRoot"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *top_bar = new QWidget(central);
    top_bar->setObjectName(QStringLiteral("topBar"));
    top_bar->setLayoutDirection(Qt::LeftToRight);
    auto *title_layout = new QHBoxLayout(top_bar);
    title_layout->setDirection(QBoxLayout::LeftToRight);
    title_layout->setContentsMargins(0, 0, 3, 0);
    title_layout->setSpacing(0);
    auto *toolbar_content = new QWidget(top_bar);
    widgets_.toolbarContent = toolbar_content;
    toolbar_content->setLayoutDirection(QApplication::layoutDirection());
    toolbar_content->setObjectName(QStringLiteral("toolbarContent"));
    toolbar_content->setProperty("windowDragArea", true);
    auto *tools = new QHBoxLayout(toolbar_content);
    tools->setContentsMargins(6, 1, 0, 1);
    tools->setSpacing(2);

    widgets_.backButton = new NavigationButton(NavigationDirection::back,
                                       QCoreApplication::translate("MainWindow", "Back"), top_bar);
    widgets_.backButton->setObjectName(QStringLiteral("backButton"));
    widgets_.forwardButton =
        new NavigationButton(NavigationDirection::forward,
                             QCoreApplication::translate("MainWindow", "Forward"), top_bar);
    widgets_.forwardButton->setObjectName(QStringLiteral("forwardButton"));
    widgets_.historyButton = new NavigationButton(
        NavigationDirection::history,
        QCoreApplication::translate("MainWindow", "Navigation history"), top_bar);
    widgets_.historyButton->setObjectName(QStringLiteral("historyButton"));
    widgets_.historyMenu = new QMenu(widgets_.historyButton);
    widgets_.historyButton->setMenu(widgets_.historyMenu);
    widgets_.historyButton->setPopupMode(QToolButton::InstantPopup);
    widgets_.upButton =
        new NavigationButton(NavigationDirection::up,
                             QCoreApplication::translate("MainWindow", "Up one level"), top_bar);
    widgets_.upButton->setObjectName(QStringLiteral("upButton"));
    widgets_.createFolderAction =
        new QAction(create_folder_icon(theme_),
                    QCoreApplication::translate("MainWindow", "Create Folder"), &window_);
    widgets_.createFolderAction->setObjectName(QStringLiteral("createFolderAction"));
    widgets_.createFolderAction->setShortcut(QKeySequence(Qt::Key_F7));
    widgets_.createFolderAction->setShortcutVisibleInContextMenu(true);
    window_.addAction(widgets_.createFolderAction);
    widgets_.createFolderButton = new CreateFolderButton(top_bar);
    widgets_.createFolderButton->setObjectName(QStringLiteral("createFolderButton"));
    widgets_.createFolderButton->setDefaultAction(widgets_.createFolderAction);

    widgets_.filterEdit = new SearchField(top_bar);
    widgets_.filterEdit->setObjectName(QStringLiteral("catalogFilter"));
    widgets_.filterEdit->setPlaceholderText(QCoreApplication::translate("MainWindow", "Filter by name"));

    widgets_.globalSearchEdit = new SearchField(top_bar);
    widgets_.globalSearchEdit->setObjectName(QStringLiteral("globalSearch"));
    widgets_.globalSearchEdit->setPlaceholderText(
        QCoreApplication::translate("MainWindow", "Global search"));
    widgets_.globalSearchEdit->setToolTip(global_search_tooltip());

    const auto sort_controls = create_sort_controls(top_bar);
    widgets_.sortControls = sort_controls.widget;
    widgets_.sortField = sort_controls.field;
    widgets_.sortField->setObjectName(QStringLiteral("sortField"));
    widgets_.sortField->setToolTip(QCoreApplication::translate("MainWindow", "Sort field"));
    widgets_.sortField->addItem(QCoreApplication::translate("MainWindow", "Name"),
                        static_cast<int>(core::SortField::name));
    widgets_.sortField->addItem(QCoreApplication::translate("MainWindow", "Extension"),
                        static_cast<int>(core::SortField::extension));
    widgets_.sortField->addItem(QCoreApplication::translate("MainWindow", "Date"),
                        static_cast<int>(core::SortField::modified));
    widgets_.sortField->addItem(QCoreApplication::translate("MainWindow", "Size"),
                        static_cast<int>(core::SortField::size));

    widgets_.sortDirection = sort_controls.direction;
    widgets_.sortDirection->setObjectName(QStringLiteral("sortDirection"));
    widgets_.sortDirection->setToolTip(QCoreApplication::translate("MainWindow", "Sort direction"));

    const auto thumbnail_scale = create_thumbnail_scale(top_bar);
    widgets_.smallerThumbnailsButton = thumbnail_scale.smaller;
    widgets_.sizeSlider = thumbnail_scale.slider;
    widgets_.sizeSlider->setValue(160);
    widgets_.largerThumbnailsButton = thumbnail_scale.larger;

    widgets_.logoButton = new QToolButton(top_bar);
    widgets_.logoButton->setObjectName(QStringLiteral("logo"));
    widgets_.logoButton->setIcon(authored_logo(theme_));
    widgets_.logoButton->setIconSize(QSize(104, 20));
    widgets_.logoButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    widgets_.logoButton->setFixedSize(108, 28);
    widgets_.logoButton->setAutoRaise(true);
    widgets_.logoButton->setFocusPolicy(Qt::StrongFocus);
    widgets_.mainMenu = new QMenu(widgets_.logoButton);
    widgets_.mainMenu->setObjectName(QStringLiteral("mainMenu"));
    widgets_.languageMenu = widgets_.mainMenu->addMenu(QString());
    widgets_.languageMenu->setObjectName(QStringLiteral("languageMenu"));
    widgets_.languageMenu->menuAction()->setObjectName(QStringLiteral("languageMenuAction"));
    widgets_.languageActions = new QActionGroup(&window_);
    widgets_.languageActions->setExclusive(true);
    for (const auto language : {AppLanguage::russian, AppLanguage::english,
                                AppLanguage::chinese_simplified, AppLanguage::arabic}) {
        auto *action = widgets_.languageMenu->addAction(language_native_name(language));
        action->setObjectName(QStringLiteral("language_%1").arg(language_code(language)));
        action->setData(static_cast<int>(language));
        action->setCheckable(true);
        action->setEnabled(translations_->available(language));
        widgets_.languageActions->addAction(action);
    }
    widgets_.settingsAction = widgets_.mainMenu->addAction(QString());
    widgets_.settingsAction->setObjectName(QStringLiteral("settingsAction"));
    widgets_.componentsAction = widgets_.mainMenu->addAction(QString());
    widgets_.componentsAction->setObjectName(QStringLiteral("componentsAction"));
    widgets_.updateAction = widgets_.mainMenu->addAction(QString());
    widgets_.updateAction->setObjectName(QStringLiteral("updateAction"));
    widgets_.unfinishedOperationAction = widgets_.mainMenu->addAction(QString());
    widgets_.unfinishedOperationAction->setObjectName(QStringLiteral("unfinishedOperationAction"));
    widgets_.mainMenu->addSeparator();
    widgets_.aboutAction = widgets_.mainMenu->addAction(QString());
    widgets_.aboutAction->setObjectName(QStringLiteral("aboutAction"));
    widgets_.donateAction = widgets_.mainMenu->addAction(QString());
    widgets_.donateAction->setObjectName(QStringLiteral("donateAction"));
    widgets_.reportIssueAction = widgets_.mainMenu->addAction(QString());
    widgets_.reportIssueAction->setObjectName(QStringLiteral("reportIssueAction"));

    // The recovery catalog remains an internal action until its final Stage F placement is
    // approved.
    widgets_.trashAction = new QAction(QCoreApplication::translate("MainWindow", "VO-VE Trash…"), &window_);
    widgets_.trashAction->setObjectName(QStringLiteral("trashAction"));
    widgets_.logoButton->setMenu(widgets_.mainMenu);
    widgets_.logoButton->setPopupMode(QToolButton::InstantPopup);

    tools->addWidget(widgets_.backButton);
    tools->addWidget(widgets_.forwardButton);
    tools->addWidget(widgets_.historyButton);
    tools->addWidget(widgets_.upButton);
    tools->addWidget(widgets_.createFolderButton);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(widgets_.filterEdit);
    tools->addWidget(widgets_.globalSearchEdit);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(widgets_.sortControls);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(thumbnail_scale.widget);
    tools->addStretch(1);
    tools->addWidget(toolbar_separator(top_bar));
    auto *catalog_tools = new QWidget(top_bar);
    catalog_tools->setObjectName(QStringLiteral("catalogTools"));
    catalog_tools->setLayoutDirection(Qt::LeftToRight);
    auto *catalog_tools_layout = new QHBoxLayout(catalog_tools);
    catalog_tools_layout->setContentsMargins(0, 0, 0, 0);
    catalog_tools_layout->setSpacing(3);
    widgets_.recursiveViewButton = new CatalogToolButton(CatalogToolKind::recursive_view, catalog_tools);
    widgets_.recursiveViewButton->setObjectName(QStringLiteral("recursiveViewButton"));
    widgets_.recursiveViewButton->setCheckable(true);
    widgets_.refreshButton = new CatalogToolButton(CatalogToolKind::refresh, catalog_tools);
    widgets_.refreshButton->setObjectName(QStringLiteral("refreshButton"));
    catalog_tools_layout->addWidget(widgets_.recursiveViewButton);
    catalog_tools_layout->addWidget(widgets_.refreshButton);
    tools->addWidget(catalog_tools);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(widgets_.logoButton);
    widgets_.windowControls = new WindowControls(&window_, top_bar);
    title_layout->addWidget(toolbar_content, 1);
    title_layout->addWidget(toolbar_separator(top_bar));
    title_layout->addWidget(widgets_.windowControls);
    new WindowFrame(&window_, top_bar, widgets_.windowControls->maximize_button());
    root->addWidget(top_bar);

    widgets_.mainSplitter = new QSplitter(Qt::Horizontal, central);
    widgets_.mainSplitter->setObjectName(QStringLiteral("mainSplitter"));
    widgets_.mainSplitter->setChildrenCollapsible(false);
    widgets_.mainSplitter->setHandleWidth(1);

    auto *catalog_view = new CatalogView(widgets_.mainSplitter);
    widgets_.listView = catalog_view;
    widgets_.listView->verticalScrollBar()->setProperty("voveOuterRailShared", true);
    widgets_.listView->setObjectName(QStringLiteral("catalogGrid"));
    widgets_.listView->setViewMode(QListView::IconMode);
    widgets_.listView->setFlow(QListView::LeftToRight);
    widgets_.listView->setWrapping(true);
    widgets_.listView->setResizeMode(QListView::Adjust);
    widgets_.listView->setMovement(QListView::Static);
    widgets_.listView->setUniformItemSizes(true);
    widgets_.listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    widgets_.listView->setDragEnabled(true);
    widgets_.listView->setAcceptDrops(true);
    widgets_.listView->viewport()->setAcceptDrops(true);
    widgets_.listView->setDragDropMode(QAbstractItemView::DragDrop);
    widgets_.listView->setDefaultDropAction(Qt::CopyAction);
    widgets_.listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    widgets_.listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    widgets_.listView->setSpacing(8);
    widgets_.listView->setContextMenuPolicy(Qt::CustomContextMenu);
    widgets_.globalSearchMessage = new QLabel(widgets_.listView->viewport());
    widgets_.globalSearchMessage->setObjectName(QStringLiteral("globalSearchMessage"));
    widgets_.globalSearchMessage->setAlignment(Qt::AlignCenter);
    widgets_.globalSearchMessage->setWordWrap(true);
    widgets_.globalSearchMessage->setTextFormat(Qt::RichText);
    widgets_.globalSearchMessage->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                                  Qt::TextSelectableByMouse);
    widgets_.globalSearchMessage->setOpenExternalLinks(true);
    widgets_.globalSearchMessage->hide();

    widgets_.renameAction = new QAction(QCoreApplication::translate("MainWindow", "Rename"), &window_);
    widgets_.renameAction->setObjectName(QStringLiteral("renameAction"));
    widgets_.renameAction->setShortcut(QKeySequence(Qt::Key_F2));
    widgets_.renameAction->setShortcutVisibleInContextMenu(true);
    widgets_.renameAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    widgets_.renameAction->setEnabled(false);
    widgets_.listView->addAction(widgets_.renameAction);
    widgets_.deleteAction = new QAction(QCoreApplication::translate("MainWindow", "Delete…"), &window_);
    widgets_.deleteAction->setObjectName(QStringLiteral("deleteAction"));
    widgets_.deleteAction->setShortcut(QKeySequence(Qt::Key_Delete));
    widgets_.deleteAction->setShortcutVisibleInContextMenu(true);
    widgets_.deleteAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    widgets_.deleteAction->setEnabled(false);
    widgets_.listView->addAction(widgets_.deleteAction);
    widgets_.copyNamesAction = new QAction(QCoreApplication::translate("MainWindow", "Copy name"), &window_);
    widgets_.copyNamesAction->setObjectName(QStringLiteral("copyNamesAction"));
    widgets_.cancelPreviewAttemptAction =
        new QAction(QCoreApplication::translate("MainWindow", "Cancel preview attempt"), &window_);
    widgets_.cancelPreviewAttemptAction->setObjectName(QStringLiteral("cancelPreviewAttemptAction"));
    widgets_.cancelPreviewAttemptAction->setShortcut(QKeySequence(Qt::Key_Escape));
    widgets_.cancelPreviewAttemptAction->setShortcutVisibleInContextMenu(true);
    widgets_.cancelPreviewAttemptAction->setEnabled(false);
    window_.addAction(widgets_.cancelPreviewAttemptAction);
    widgets_.copyPathsAction =
        new QAction(QCoreApplication::translate("MainWindow", "Copy full path"), &window_);
    widgets_.copyPathsAction->setObjectName(QStringLiteral("copyPathsAction"));
    widgets_.copyPathsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    widgets_.copyPathsAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    widgets_.copyPathsAction->setShortcutVisibleInContextMenu(true);
    widgets_.copyPathsAction->setEnabled(false);
    widgets_.listView->addAction(widgets_.copyPathsAction);
    widgets_.copyObjectsAction = new QAction(QCoreApplication::translate("MainWindow", "Copy"), &window_);
    widgets_.copyObjectsAction->setObjectName(QStringLiteral("copyObjectsAction"));
    widgets_.copyObjectsAction->setShortcut(QKeySequence::Copy);
    widgets_.copyObjectsAction->setShortcutVisibleInContextMenu(true);
    widgets_.copyObjectsAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    widgets_.copyObjectsAction->setEnabled(false);
    widgets_.listView->addAction(widgets_.copyObjectsAction);
    widgets_.pasteObjectsAction = new QAction(QCoreApplication::translate("MainWindow", "Paste"), &window_);
    widgets_.pasteObjectsAction->setObjectName(QStringLiteral("pasteObjectsAction"));
    widgets_.pasteObjectsAction->setShortcut(QKeySequence::Paste);
    widgets_.pasteObjectsAction->setShortcutVisibleInContextMenu(true);
    widgets_.pasteObjectsAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    widgets_.pasteObjectsAction->setEnabled(false);
    widgets_.listView->addAction(widgets_.pasteObjectsAction);

    auto *right_column = new QWidget(widgets_.mainSplitter);
    right_column->setObjectName(QStringLiteral("rightColumn"));
    auto *right_layout = new QVBoxLayout(right_column);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);

    auto *right_top = new QWidget(right_column);
    right_top->setObjectName(QStringLiteral("rightTop"));
    auto *right_top_layout = new QVBoxLayout(right_top);
    right_top_layout->setContentsMargins(0, 0, 0, 0);
    right_top_layout->setSpacing(0);

    widgets_.placesSplitter = new QSplitter(Qt::Horizontal, right_top);
    widgets_.placesSplitter->setObjectName(QStringLiteral("placesSplitter"));
    widgets_.placesSplitter->setChildrenCollapsible(false);
    widgets_.placesSplitter->setHandleWidth(1);

    auto *favorites_panel = new QWidget(widgets_.placesSplitter);
    auto *favorites_layout = new QVBoxLayout(favorites_panel);
    favorites_layout->setContentsMargins(0, 0, 0, 0);
    favorites_layout->setSpacing(0);
    widgets_.favoritesHeader = new QFrame(favorites_panel);
    widgets_.favoritesHeader->setObjectName(QStringLiteral("favoritesHeader"));
    widgets_.favoritesHeader->setAcceptDrops(true);
    auto *favorites_header_layout = new QHBoxLayout(widgets_.favoritesHeader);
    favorites_header_layout->setContentsMargins(8, 3, 8, 4);
    widgets_.favoritesTitle =
        new QLabel(QCoreApplication::translate("MainWindow", "Favorites"), favorites_panel);
    widgets_.favoritesTitle->setObjectName(QStringLiteral("sectionTitle"));
    widgets_.favoritesTitle->setAttribute(Qt::WA_TransparentForMouseEvents);
    favorites_header_layout->addWidget(widgets_.favoritesTitle);
    favorites_header_layout->addStretch(1);
    widgets_.addFavoriteButton = new AddFavoriteButton(widgets_.favoritesHeader);
    widgets_.addFavoriteButton->setObjectName(QStringLiteral("addFavoriteButton"));
    favorites_header_layout->addWidget(widgets_.addFavoriteButton);
    auto *favorites_body = new QWidget(favorites_panel);
    favorites_body->setObjectName(QStringLiteral("favoritesBody"));
    auto *favorites_body_layout = new QHBoxLayout(favorites_body);
    favorites_body_layout->setContentsMargins(0, 0, 0, 0);
    favorites_body_layout->setSpacing(0);
    auto *favorites_stack = new QWidget(favorites_body);
    auto *favorites_stack_layout = new QVBoxLayout(favorites_stack);
    favorites_stack_layout->setContentsMargins(0, 0, 0, 0);
    favorites_stack_layout->setSpacing(0);

    widgets_.favorites = new FavoritesView(favorites_stack);
    widgets_.favorites->setObjectName(QStringLiteral("favoritesView"));
    widgets_.favorites->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    widgets_.favorites->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    widgets_.favorites->setContextMenuPolicy(Qt::CustomContextMenu);
    widgets_.favorites->setAcceptDrops(true);
    widgets_.favorites->viewport()->setAcceptDrops(true);
    widgets_.favorites->setDragDropMode(QAbstractItemView::DropOnly);
    widgets_.favorites->setDefaultDropAction(Qt::CopyAction);
    widgets_.trashFavoriteDock = new QFrame(favorites_stack);
    widgets_.trashFavoriteDock->setObjectName(QStringLiteral("trashFavoriteDock"));
    auto *trash_layout = new QVBoxLayout(widgets_.trashFavoriteDock);
    trash_layout->setContentsMargins(0, 5, 0, 0);
    trash_layout->setSpacing(0);
    widgets_.trashFavorite = new FavoritesView(widgets_.trashFavoriteDock);
    widgets_.trashFavorite->setObjectName(QStringLiteral("trashFavoriteView"));
    widgets_.trashFavorite->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    widgets_.trashFavorite->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    widgets_.trashFavorite->setContextMenuPolicy(Qt::CustomContextMenu);
    widgets_.trashFavorite->setAcceptDrops(true);
    widgets_.trashFavorite->viewport()->setAcceptDrops(true);
    widgets_.trashFavorite->setDragDropMode(QAbstractItemView::DropOnly);
    widgets_.trashFavorite->setDefaultDropAction(Qt::CopyAction);
    widgets_.trashFavorite->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    trash_layout->addWidget(widgets_.trashFavorite);
    favorites_stack_layout->addWidget(widgets_.favorites, 1);
    favorites_stack_layout->addWidget(widgets_.trashFavoriteDock);

    widgets_.favoritesScrollBar = new QScrollBar(Qt::Vertical, favorites_body);
    widgets_.favoritesScrollBar->setObjectName(QStringLiteral("favoritesScrollBar"));
    widgets_.favoritesScrollBar->setProperty("voveOuterRailShared", true);
    widgets_.favoritesScrollBar->setFocusPolicy(Qt::NoFocus);
    widgets_.favoritesScrollBar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto *favorites_internal_scroll = widgets_.favorites->verticalScrollBar();
    const auto sync_favorites_scrollbar = [scrollbar = widgets_.favoritesScrollBar, favorites_internal_scroll](const int minimum,
                                                                            const int maximum) {
        scrollbar->setRange(minimum, maximum);
        scrollbar->setSingleStep(favorites_internal_scroll->singleStep());
        scrollbar->setPageStep(favorites_internal_scroll->pageStep());
        scrollbar->setValue(favorites_internal_scroll->value());
    };
    connect(favorites_internal_scroll, &QScrollBar::rangeChanged, widgets_.favoritesScrollBar,
            sync_favorites_scrollbar);
    connect(favorites_internal_scroll, &QScrollBar::valueChanged, widgets_.favoritesScrollBar,
            &QScrollBar::setValue);
    connect(widgets_.favoritesScrollBar, &QScrollBar::valueChanged, favorites_internal_scroll,
            &QScrollBar::setValue);
    sync_favorites_scrollbar(favorites_internal_scroll->minimum(),
                             favorites_internal_scroll->maximum());

    favorites_body_layout->addWidget(favorites_stack, 1);
    favorites_body_layout->addWidget(widgets_.favoritesScrollBar);
    favorites_layout->addWidget(widgets_.favoritesHeader);
    favorites_layout->addWidget(favorites_body, 1);

    auto *tree_panel = new QWidget(widgets_.placesSplitter);
    auto *tree_layout = new QVBoxLayout(tree_panel);
    tree_layout->setContentsMargins(0, 0, 0, 0);
    tree_layout->setSpacing(0);
    widgets_.directoryHeader = new QFrame(tree_panel);
    widgets_.directoryHeader->setObjectName(QStringLiteral("directoryHeader"));
    auto *directory_header_layout = new QHBoxLayout(widgets_.directoryHeader);
    const auto scroll_extent = window_.style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    directory_header_layout->setContentsMargins(scroll_extent, 3, scroll_extent, 4);
    widgets_.pathEdit = new AddressField(widgets_.directoryHeader);
    widgets_.pathEdit->setObjectName(QStringLiteral("pathEdit"));
    widgets_.pathEdit->setClearButtonEnabled(false);
    widgets_.pathEdit->setPlaceholderText(
        QCoreApplication::translate("MainWindow", "Local or network path"));

    widgets_.chooseFolderAction = widgets_.pathEdit->addAction(QIcon(), QLineEdit::TrailingPosition);
    widgets_.chooseFolderAction->setObjectName(QStringLiteral("chooseFolderAction"));
    directory_header_layout->addWidget(widgets_.pathEdit);

    widgets_.directoryTree = new DirectoryTreeView(tree_panel);
    widgets_.directoryTree->setObjectName(QStringLiteral("directoryTree"));
    widgets_.directoryTree->setHeaderHidden(true);
    widgets_.directoryTree->setAnimated(false);
    widgets_.directoryTree->setUniformRowHeights(true);
    widgets_.directoryTree->setIndentation(scroll_extent);
    widgets_.directoryTree->setDragEnabled(true);
    widgets_.directoryTree->setAcceptDrops(true);
    widgets_.directoryTree->viewport()->setAcceptDrops(true);
    widgets_.directoryTree->setDragDropMode(QAbstractItemView::DragDrop);
    widgets_.directoryTree->setDefaultDropAction(Qt::CopyAction);
    widgets_.directoryTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    widgets_.directoryTree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    tree_layout->addWidget(widgets_.directoryHeader);
    tree_layout->addWidget(widgets_.directoryTree, 1);

    right_top_layout->addWidget(widgets_.placesSplitter);

    install_smooth_scroll(*widgets_.listView, 180);
    install_smooth_scroll(*widgets_.favorites);
    install_smooth_scroll(*widgets_.directoryTree);

    widgets_.previewPanel = new QWidget(right_column);
    widgets_.previewPanel->setObjectName(QStringLiteral("previewPanel"));
    widgets_.previewPanel->setAcceptDrops(true);
    auto *preview_layout = new QVBoxLayout(widgets_.previewPanel);
    preview_layout->setContentsMargins(16, 12, 16, 12);
    preview_layout->setSpacing(8);
    widgets_.previewCanvas =
        new ZoomPreview(QCoreApplication::translate("MainWindow", "Select a file"), widgets_.previewPanel);
    widgets_.previewCanvas->setObjectName(QStringLiteral("previewHint"));
    widgets_.previewCanvas->setAlignment(Qt::AlignCenter);
    widgets_.previewCanvas->setWordWrap(true);
    widgets_.previewCanvas->setTextInteractionFlags(Qt::TextSelectableByMouse);
    widgets_.previewCanvas->setMinimumHeight(180);
    widgets_.previewCanvas->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    widgets_.previewCanvas->setAcceptDrops(true);

    widgets_.pageNavigation = new QWidget(widgets_.previewPanel);
    widgets_.pageNavigation->setObjectName(QStringLiteral("pageNavigation"));
    auto *page_navigation_layout = new QHBoxLayout(widgets_.pageNavigation);
    page_navigation_layout->setContentsMargins(0, 0, 0, 0);
    page_navigation_layout->setSpacing(4);
    widgets_.previousPageButton =
        new PageButton(PageDirection::previous,
                       QCoreApplication::translate("MainWindow", "Previous page"), widgets_.pageNavigation);
    widgets_.previousPageButton->setObjectName(QStringLiteral("previousPage"));
    widgets_.nextPageButton =
        new PageButton(PageDirection::next, QCoreApplication::translate("MainWindow", "Next page"),
                       widgets_.pageNavigation);
    widgets_.nextPageButton->setObjectName(QStringLiteral("nextPage"));
    widgets_.pageIndicator = new QLabel(widgets_.pageNavigation);
    widgets_.pageIndicator->setObjectName(QStringLiteral("pageIndicator"));
    widgets_.pageIndicator->setAlignment(Qt::AlignCenter);
    widgets_.pageIndicator->setMinimumWidth(
        widgets_.pageIndicator->fontMetrics().horizontalAdvance(QStringLiteral("0000 / 0000")));
    page_navigation_layout->addStretch(1);
    page_navigation_layout->addWidget(widgets_.previousPageButton);
    page_navigation_layout->addWidget(widgets_.pageIndicator);
    page_navigation_layout->addWidget(widgets_.nextPageButton);
    page_navigation_layout->addStretch(1);
    widgets_.pageNavigation->hide();

    widgets_.passwordButton = new QToolButton(widgets_.previewPanel);
    widgets_.passwordButton->setObjectName(QStringLiteral("documentPassword"));
    widgets_.passwordButton->setText(QCoreApplication::translate("MainWindow", "Enter password"));
    widgets_.passwordButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    widgets_.passwordButton->setAccessibleName(
        QCoreApplication::translate("MainWindow", "Enter document password"));
    widgets_.passwordButton->hide();

    auto *preview_caption = new QWidget(widgets_.previewPanel);
    preview_caption->setObjectName(QStringLiteral("previewCaption"));
    preview_caption->setLayoutDirection(Qt::LeftToRight);
    auto *preview_footer = new QHBoxLayout(preview_caption);
    preview_footer->setContentsMargins(0, 0, 0, 0);
    preview_footer->setSpacing(6);
    widgets_.previewName = new PreviewNameLabel(preview_caption);
    widgets_.previewFormat = new QLabel(preview_caption);
    widgets_.previewFormat->setObjectName(QStringLiteral("previewFormat"));
    widgets_.previewFormat->setTextFormat(Qt::PlainText);
    widgets_.previewFormat->setAlignment(Qt::AlignRight | Qt::AlignAbsolute | Qt::AlignBottom);
    widgets_.previewFormat->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    widgets_.previewFormat->hide();
    widgets_.previewSupport = new QLabel(widgets_.previewPanel);
    widgets_.previewSupport->setObjectName(QStringLiteral("previewSupport"));
    widgets_.previewSupport->setWordWrap(true);
    widgets_.previewSupport->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    widgets_.previewSupport->hide();
    preview_footer->addWidget(widgets_.previewName, 1, Qt::AlignBottom);
    preview_footer->addWidget(widgets_.previewFormat);
    preview_layout->addWidget(widgets_.pageNavigation);
    preview_layout->addWidget(widgets_.previewCanvas, 1);
    preview_layout->addWidget(widgets_.passwordButton, 0, Qt::AlignHCenter);
    preview_layout->addWidget(preview_caption);
    preview_layout->addWidget(widgets_.previewSupport);
    widgets_.previewModified = new PreviewModifiedLabel(widgets_.previewPanel);
    preview_layout->addWidget(widgets_.previewModified);

    right_layout->addWidget(right_top, 1);
    auto *navigation_separator = new QFrame(right_column);
    navigation_separator->setObjectName(QStringLiteral("navigationPreviewSeparator"));
    navigation_separator->setFixedHeight(1);
    right_layout->addWidget(navigation_separator);
    right_layout->addWidget(widgets_.previewPanel, 3);

    widgets_.mainSplitter->addWidget(widgets_.listView);
    widgets_.mainSplitter->addWidget(right_column);
    widgets_.mainSplitter->setStretchFactor(0, 3);
    widgets_.mainSplitter->setStretchFactor(1, 2);
    widgets_.placesSplitter->setStretchFactor(0, 3);
    widgets_.placesSplitter->setStretchFactor(1, 7);
    root->addWidget(widgets_.mainSplitter, 1);

    widgets_.status = new QLabel(&window_);
    widgets_.status->setObjectName(QStringLiteral("catalogStatus"));
    widgets_.status->setMinimumWidth(240);
    widgets_.status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    window_.statusBar()->setSizeGripEnabled(false);
    window_.statusBar()->addWidget(widgets_.status, 1);

    widgets_.themeButtons = new QButtonGroup(&window_);
    window_.statusBar()->addPermanentWidget(create_theme_switch(widgets_.themeButtons, &window_));
    window_.setCentralWidget(central);
}

void WindowShell::retranslate() {
    if (widgets_.logoButton == nullptr) {
        return;
    }
    const auto base_font = qApp->font();
    widgets_.filterEdit->setFont(base_font);
    widgets_.globalSearchEdit->setFont(base_font);
    widgets_.sortField->setFont(base_font);
    widgets_.favoritesTitle->setFont(base_font);
    widgets_.favorites->setFont(base_font);
    widgets_.trashFavorite->setFont(base_font);
    widgets_.directoryTree->setFont(base_font);
    widgets_.pathEdit->setFont(base_font);
    widgets_.listView->setFont(base_font);
    // IconMode's automatic wheel step grows with the tile. Keep it tied to readable text instead.
    widgets_.listView->verticalScrollBar()->setSingleStep(2 * widgets_.listView->fontMetrics().height());
    const auto header_height =
        std::max({widgets_.pathEdit->sizeHint().height(), widgets_.favoritesTitle->sizeHint().height(),
                  widgets_.addFavoriteButton->height()}) +
        7;
    widgets_.favoritesHeader->setFixedHeight(header_height);
    widgets_.directoryHeader->setFixedHeight(header_height);
    const auto trash_row_height = std::max(24, widgets_.trashFavorite->sizeHintForRow(0));
    widgets_.trashFavorite->setFixedHeight(trash_row_height);
    widgets_.chooseFolderAction->setText(QCoreApplication::translate("MainWindow", "Choose folder"));
    widgets_.chooseFolderAction->setToolTip(widgets_.chooseFolderAction->text());
    const auto add_favorite_text =
        QCoreApplication::translate("MainWindow", "Add current folder to Favorites");
    widgets_.addFavoriteButton->setToolTip(add_favorite_text);
    widgets_.addFavoriteButton->setAccessibleName(add_favorite_text);
    auto preview_name_font = base_font;
    preview_name_font.setPointSizeF(base_font.pointSizeF() * 1.8);
    widgets_.previewName->setFont(preview_name_font);
    preview_name_font.setPointSizeF(preview_name_font.pointSizeF() * 2.0);
    widgets_.previewFormat->setFont(preview_name_font);
    auto status_font = base_font;
    status_font.setPointSizeF(base_font.pointSizeF() * 0.9);
    widgets_.status->setFont(status_font);
    widgets_.previewModified->setFont(status_font);
    widgets_.previewCanvas->set_indicator_font(widgets_.previewModified->font());
    widgets_.previewModified->refresh();

    widgets_.logoButton->setToolTip(ui_text(UiTextId::main_menu));
    widgets_.logoButton->setAccessibleName(ui_text(UiTextId::main_menu));
    const auto set_tool_text = [](QToolButton *button, const QString &text) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    };
    set_tool_text(widgets_.backButton, QCoreApplication::translate("MainWindow", "Back"));
    set_tool_text(widgets_.forwardButton, QCoreApplication::translate("MainWindow", "Forward"));
    set_tool_text(widgets_.historyButton, QCoreApplication::translate("MainWindow", "Navigation history"));
    set_tool_text(widgets_.upButton, QCoreApplication::translate("MainWindow", "Up one level"));
    widgets_.createFolderAction->setText(QCoreApplication::translate("MainWindow", "Create Folder"));
    widgets_.languageMenu->setTitle(ui_text(UiTextId::language));
    widgets_.settingsAction->setText(ui_text(UiTextId::settings));
    widgets_.unfinishedOperationAction->setText(
        QCoreApplication::translate("TrashRecoveryDialog", "Unfinished operation…"));
    widgets_.updateAction->setText(ui_text(UiTextId::check_updates));
    widgets_.aboutAction->setText(ui_text(UiTextId::about));
    widgets_.componentsAction->setText(ui_text(UiTextId::components));
    widgets_.donateAction->setText(ui_text(UiTextId::donate));
    widgets_.reportIssueAction->setText(ui_text(UiTextId::report_issue));
    for (auto *action : widgets_.languageActions->actions()) {
        const auto action_language = static_cast<AppLanguage>(action->data().toInt());
        action->setEnabled(translations_->available(action_language));
        action->setChecked(action->data().toInt() == static_cast<int>(language_));
    }

    widgets_.filterEdit->setPlaceholderText(ui_text(UiTextId::local_filter));
    widgets_.filterEdit->setAccessibleName(ui_text(UiTextId::local_filter));
    widgets_.filterEdit->setToolTip(ui_text(UiTextId::local_filter));
    widgets_.globalSearchEdit->setPlaceholderText(ui_text(UiTextId::global_search));
    widgets_.globalSearchEdit->setAccessibleName(ui_text(UiTextId::global_search));
    widgets_.globalSearchEdit->setToolTip(global_search_tooltip());
    widgets_.filterEdit->fit_placeholder();
    widgets_.globalSearchEdit->fit_placeholder();
    widgets_.sortField->setToolTip(ui_text(UiTextId::sort_field));
    widgets_.sortField->setAccessibleName(ui_text(UiTextId::sort_field));
    const auto direction_text =
        ui_text(UiTextId::sort_direction) + QStringLiteral(": ") +
        ui_text(widgets_.sortDirection->isChecked() ? UiTextId::descending : UiTextId::ascending);
    widgets_.sortDirection->setToolTip(direction_text);
    widgets_.sortDirection->setAccessibleName(direction_text);
    const QSignalBlocker field_blocker(widgets_.sortField);
    widgets_.sortField->setItemText(0, ui_text(UiTextId::sort_name));
    widgets_.sortField->setItemText(1, ui_text(UiTextId::sort_extension));
    widgets_.sortField->setItemText(2, ui_text(UiTextId::sort_date));
    widgets_.sortField->setItemText(3, ui_text(UiTextId::sort_size));
    fit_sort_controls({widgets_.sortControls, widgets_.sortField, widgets_.sortDirection});
    widgets_.sizeSlider->setToolTip(ui_text(UiTextId::thumbnail_size));
    widgets_.sizeSlider->setAccessibleName(ui_text(UiTextId::thumbnail_size));
    widgets_.smallerThumbnailsButton->setToolTip(ui_text(UiTextId::smaller_thumbnails));
    widgets_.smallerThumbnailsButton->setAccessibleName(ui_text(UiTextId::smaller_thumbnails));
    widgets_.largerThumbnailsButton->setToolTip(ui_text(UiTextId::larger_thumbnails));
    widgets_.largerThumbnailsButton->setAccessibleName(ui_text(UiTextId::larger_thumbnails));
    widgets_.pathEdit->setPlaceholderText(ui_text(UiTextId::local_or_network_path));
    widgets_.pathEdit->setAccessibleName(ui_text(UiTextId::local_or_network_path));
    widgets_.favoritesTitle->setText(QCoreApplication::translate("MainWindow", "Favorites"));
    for (int row{}; row < widgets_.favorites->count(); ++row) {
        if (is_trash_favorite(widgets_.favorites->item(row))) {
            widgets_.favorites->item(row)->setText(QCoreApplication::translate("MainWindow", "Trash"));
            widgets_.favorites->item(row)->setToolTip(
                QCoreApplication::translate("MainWindow", "VO-VE Trash"));
        }
    }
    if (is_trash_favorite(widgets_.trashFavorite->item(0))) {
        widgets_.trashFavorite->item(0)->setText(QCoreApplication::translate("MainWindow", "Trash"));
        widgets_.trashFavorite->item(0)->setToolTip(
            QCoreApplication::translate("MainWindow", "VO-VE Trash"));
    }
    widgets_.trashAction->setText(QCoreApplication::translate("MainWindow", "VO-VE Trash…"));
    set_tool_text(widgets_.previousPageButton, QCoreApplication::translate("MainWindow", "Previous page"));
    set_tool_text(widgets_.nextPageButton, QCoreApplication::translate("MainWindow", "Next page"));
    widgets_.passwordButton->setText(QCoreApplication::translate("MainWindow", "Enter password"));
    widgets_.passwordButton->setAccessibleName(
        QCoreApplication::translate("MainWindow", "Enter document password"));
    const std::array theme_ids{UiTextId::theme_north, UiTextId::theme_vanilla,
                               UiTextId::theme_breeze, UiTextId::theme_twilight};
    for (int index = 0; index < static_cast<int>(theme_ids.size()); ++index) {
        if (auto *button = widgets_.themeButtons->button(index); button != nullptr) {
            const auto name = ui_text(theme_ids[static_cast<std::size_t>(index)]);
            button->setToolTip(name);
            button->setAccessibleName(name);
        }
    }
    widgets_.copyNamesAction->setText(QCoreApplication::translate("MainWindow", "Copy name"));
    widgets_.cancelPreviewAttemptAction->setText(
        QCoreApplication::translate("MainWindow", "Cancel preview attempt"));
    widgets_.copyPathsAction->setText(QCoreApplication::translate("MainWindow", "Copy full path"));
    widgets_.copyObjectsAction->setText(QCoreApplication::translate("MainWindow", "Copy"));
    widgets_.pasteObjectsAction->setText(QCoreApplication::translate("MainWindow", "Paste"));
    widgets_.listView->viewport()->update();
}
void WindowShell::apply_artwork_icons() {
    const auto horizontal_rotation = window_.layoutDirection() == Qt::RightToLeft ? 180 : 0;
    for (auto *button : {widgets_.backButton, widgets_.forwardButton, widgets_.historyButton, widgets_.upButton}) {
        button->set_theme(theme_);
    }
    widgets_.previousPageButton->set_theme(theme_);
    widgets_.nextPageButton->set_theme(theme_);
    widgets_.recursiveViewButton->set_theme(theme_);
    widgets_.refreshButton->set_theme(theme_);
    widgets_.logoButton->setIcon(authored_logo(theme_));
    apply_thumbnail_scale_theme({widgets_.sizeSlider->parentWidget(), widgets_.smallerThumbnailsButton, widgets_.sizeSlider,
                                 widgets_.largerThumbnailsButton},
                                theme_);
    widgets_.filterEdit->set_theme(theme_);
    widgets_.globalSearchEdit->set_theme(theme_);
    widgets_.pathEdit->set_theme(theme_);
    apply_sort_controls_theme({widgets_.sortControls, widgets_.sortField, widgets_.sortDirection}, theme_);
    widgets_.createFolderAction->setIcon(create_folder_icon(theme_));
    widgets_.chooseFolderAction->setIcon(directory_picker_icon(
        qApp->palette().color(QPalette::PlaceholderText), window_.devicePixelRatioF()));
    widgets_.backButton->setProperty("artworkRotation", horizontal_rotation);
    widgets_.forwardButton->setProperty("artworkRotation", horizontal_rotation);
    widgets_.previousPageButton->setProperty("artworkRotation", horizontal_rotation);
    widgets_.nextPageButton->setProperty("artworkRotation", horizontal_rotation);
    widgets_.historyButton->setProperty("artworkRotation", 180);
}
void WindowShell::apply_theme(const AppTheme theme) {
    theme_ = theme;
    apply_scrollbar_theme(theme);
    const auto colors = theme_colors(theme_);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(colors.window));
    palette.setColor(QPalette::WindowText, QColor(colors.text));
    palette.setColor(QPalette::Base, QColor(colors.base));
    palette.setColor(QPalette::AlternateBase, QColor(colors.tile));
    palette.setColor(QPalette::Text, QColor(colors.text));
    palette.setColor(QPalette::Button, QColor(colors.panel));
    palette.setColor(QPalette::ButtonText, QColor(colors.text));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#F5A623")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#182126")));
    palette.setColor(QPalette::Mid, QColor(colors.border));
    palette.setColor(QPalette::PlaceholderText, QColor(colors.muted));
    qApp->setPalette(palette);
    qApp->setStyleSheet(theme_style_sheet(colors));
    // Installing the first style sheet restores platform popup fonts in Qt.
    apply_interface_font(language_);
    widgets_.favorites->set_drop_target_theme(QColor(colors.hover), QColor(QStringLiteral("#F5A623")));
    widgets_.trashFavorite->set_drop_target_theme(QColor(colors.hover), QColor(QStringLiteral("#F5A623")));
    widgets_.previewFormat->setStyleSheet(
        QStringLiteral("font-weight: 700; color: %1;")
            .arg(format_color(widgets_.previewFormat->text(), QColor(colors.panel)).name()));
    apply_artwork_icons();
    widgets_.windowControls->set_theme(theme_);
    if (auto *button = widgets_.themeButtons->button(static_cast<int>(theme_)); button != nullptr) {
        button->setChecked(true);
    }
    if (callbacks_.style_changed) callbacks_.style_changed(style());
    widgets_.listView->viewport()->update();
}
WindowShell::WindowShell(QMainWindow &window, WindowShellCallbacks callbacks,
                         const QString &translation_directory)
    : QObject(&window), window_(window), callbacks_(std::move(callbacks)),
      translations_(std::make_unique<TranslationManager>(translation_directory)) {
    install_main_menu_dialog_frames(&window_);
    build_ui();
    window_.installEventFilter(this);
    widgets_.previewModified->installEventFilter(this);
    connect_presentation();
    retranslate();
}

WindowShell::~WindowShell() = default;

bool WindowShell::eventFilter(QObject *watched, QEvent *event) {
    if (watched == &window_ &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowActivate)) {
        QTimer::singleShot(0, widgets_.previewModified,
            [label = widgets_.previewModified] { label->refresh(); });
    }
    if (watched == widgets_.previewModified &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
         event->type() == QEvent::Polish)) {
        QTimer::singleShot(0, widgets_.previewModified,
            [label = widgets_.previewModified, canvas = widgets_.previewCanvas] {
                canvas->set_indicator_font(label->font());
            });
    }
    return QObject::eventFilter(watched, event);
}

void WindowShell::changed() {
    if (callbacks_.preferences_changed) callbacks_.preferences_changed();
}

void WindowShell::connect_presentation() {
    const auto &w = widgets_;
    connect(w.languageActions, &QActionGroup::triggered, this, [this](QAction *action) {
        static_cast<void>(apply_language(static_cast<AppLanguage>(action->data().toInt()), true));
    });
    connect(w.themeButtons, &QButtonGroup::idClicked, this, [this](int id) {
        apply_theme(static_cast<AppTheme>(id));
        changed();
    });
    connect(w.mainSplitter, &QSplitter::splitterMoved, this, [this] {
        mainShare_ = splitter_share(widgets_.mainSplitter, mainShare_);
        changed();
    });
    connect(w.placesSplitter, &QSplitter::splitterMoved, this, [this] {
        favoritesShare_ = splitter_share(widgets_.placesSplitter, favoritesShare_);
        changed();
    });
    connect(w.filterEdit, &QLineEdit::textChanged, this, [this] { changed(); });
    connect(w.sortField, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(w.sortDirection, &QToolButton::toggled, this, [this] {
        const auto text = ui_text(UiTextId::sort_direction) + QStringLiteral(": ") +
            ui_text(widgets_.sortDirection->isChecked() ? UiTextId::descending : UiTextId::ascending);
        widgets_.sortDirection->setToolTip(text);
        widgets_.sortDirection->setAccessibleName(text);
        fit_sort_controls({widgets_.sortControls, widgets_.sortField, widgets_.sortDirection});
        changed();
    });
    connect(w.sizeSlider, &QSlider::valueChanged, this, [this] { changed(); });
    const auto smaller = [slider = w.sizeSlider] { slider->setValue(slider->value() - slider->pageStep()); };
    const auto larger = [slider = w.sizeSlider] { slider->setValue(slider->value() + slider->pageStep()); };
    connect(w.smallerThumbnailsButton, &QToolButton::clicked, this, smaller);
    connect(w.largerThumbnailsButton, &QToolButton::clicked, this, larger);
    const auto shortcut = [this](const char *name, const QKeySequence &keys) {
        auto *result = new QShortcut(keys, &window_);
        result->setObjectName(QString::fromLatin1(name));
        result->setContext(Qt::WindowShortcut);
        return result;
    };
    widgets_.refreshShortcut = shortcut("refreshShortcut", QKeySequence::Refresh);
    widgets_.backShortcut = shortcut("backShortcut", QKeySequence(Qt::ALT | Qt::Key_Left));
    widgets_.forwardShortcut = shortcut("forwardShortcut", QKeySequence(Qt::ALT | Qt::Key_Right));
    widgets_.upShortcut = shortcut("upShortcut", QKeySequence(Qt::ALT | Qt::Key_Up));
    connect(shortcut("smallerThumbnailsShortcut", QKeySequence::ZoomOut), &QShortcut::activated, this, smaller);
    connect(shortcut("largerThumbnailsShortcut", QKeySequence::ZoomIn), &QShortcut::activated, this, larger);
    connect(shortcut("focusPathShortcut", QKeySequence(Qt::CTRL | Qt::Key_L)), &QShortcut::activated, this,
        [field = w.pathEdit] { field->setFocus(Qt::ShortcutFocusReason); field->selectAll(); });
    connect(shortcut("focusFilterShortcut", QKeySequence::Find), &QShortcut::activated, this,
        [field = w.filterEdit] { field->setFocus(Qt::ShortcutFocusReason); field->selectAll(); });
    connect(shortcut("focusGlobalSearchShortcut", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F)),
        &QShortcut::activated, this,
        [field = w.globalSearchEdit] { field->setFocus(Qt::ShortcutFocusReason); field->selectAll(); });
    connect(shortcut("mainMenuShortcut", QKeySequence(Qt::Key_F10)), &QShortcut::activated, this,
        [button = w.logoButton] { button->setFocus(Qt::ShortcutFocusReason); button->showMenu(); });
    w.directoryTree->addAction(w.pasteObjectsAction);
    w.favorites->addAction(w.pasteObjectsAction);
}

bool WindowShell::language_available(const AppLanguage language) const {
    return translations_->available(language);
}

bool WindowShell::apply_language(const AppLanguage language, const bool persist) {
    if (!translations_->activate(language)) {
        for (auto *action : widgets_.languageActions->actions()) {
            action->setChecked(action->data().toInt() == static_cast<int>(language_));
        }
        return false;
    }
    language_ = language;
    QLocale::setDefault(app_language_locale(language_));
    QApplication::setLayoutDirection(app_language_direction(language_));
    widgets_.toolbarContent->setLayoutDirection(app_language_direction(language_));
    apply_interface_font(language_);
    window_.setProperty("appLanguage", language_code(language_));
    retranslate();
    apply_artwork_icons();
    widgets_.windowControls->set_theme(theme_);
    if (callbacks_.language_changed) callbacks_.language_changed(language_);
    if (persist) changed();
    return true;
}

ViewerStyle WindowShell::style() const {
    const auto colors = theme_colors(theme_);
    return {.theme = theme_, .folder_frame = colors.folderFrame, .folder_text = colors.folderText,
            .file_text = colors.fileText, .page_background = colors.pageBackground,
            .page_text = colors.pageText, .page_border = colors.pageBorder,
            .muted = QColor(colors.muted), .hover = QColor(colors.hover), .panel = QColor(colors.panel)};
}

void WindowShell::apply_splitter_ratios(const double main_share, const double favorites_share) {
    mainShare_ = std::clamp(main_share, 0.40, 0.80);
    favoritesShare_ = std::clamp(favorites_share, 0.20, 0.55);
    widgets_.mainSplitter->setSizes({static_cast<int>(std::lround(mainShare_ * 1000.0)),
                                    static_cast<int>(std::lround((1.0 - mainShare_) * 1000.0))});
    widgets_.placesSplitter->setSizes({static_cast<int>(std::lround(favoritesShare_ * 1000.0)),
                                      static_cast<int>(std::lround((1.0 - favoritesShare_) * 1000.0))});
}

void WindowShell::restore_presentation(const WindowSettingsSnapshot &snapshot) {
    static_cast<void>(apply_language(language_available(snapshot.language) ? snapshot.language : AppLanguage::english));
    if (!snapshot.geometry.isEmpty()) {
        window_.restoreGeometry(snapshot.geometry);
        if (!window_.windowState().testAnyFlags(Qt::WindowMaximized | Qt::WindowFullScreen)) {
            QList<QRect> available_screens;
            for (const auto *screen : QApplication::screens()) {
                if (screen) available_screens.push_back(screen->availableGeometry());
            }
            window_.setGeometry(visible_window_geometry(window_.geometry(), available_screens));
        }
    }
    apply_splitter_ratios(snapshot.main_share, snapshot.favorites_share);
    widgets_.sizeSlider->setValue(snapshot.thumbnail_extent);
    widgets_.filterEdit->setText(snapshot.filter);
    widgets_.sortField->setCurrentIndex(std::max(0, widgets_.sortField->findData(snapshot.sort_field)));
    widgets_.sortDirection->setChecked(snapshot.sort_direction == static_cast<int>(core::SortDirection::descending));
    apply_theme(snapshot.theme);
}

WindowSettingsSnapshot WindowShell::capture_presentation(WindowSettingsSnapshot snapshot) const {
    snapshot.geometry = window_.saveGeometry();
    snapshot.main_share = splitter_share(widgets_.mainSplitter, mainShare_);
    snapshot.favorites_share = splitter_share(widgets_.placesSplitter, favoritesShare_);
    snapshot.thumbnail_extent = widgets_.sizeSlider->value();
    snapshot.filter = widgets_.filterEdit->text();
    snapshot.sort_field = widgets_.sortField->currentData().toInt();
    snapshot.sort_direction = static_cast<int>(widgets_.sortDirection->isChecked()
        ? core::SortDirection::descending : core::SortDirection::ascending);
    snapshot.theme = theme_;
    snapshot.language = language_;
    return snapshot;
}

} // namespace vove::ui
