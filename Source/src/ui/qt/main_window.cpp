#include "main_window.hpp"
#include "preview_name_label.hpp"
#include "operation_notice.hpp"
#include "zoom_preview.hpp"
#include "catalog_tool_button.hpp"
#include "worker_startup_text.hpp"
#include "components_panel.hpp"
#include "about_dialog.hpp"
#include "add_favorite_button.hpp"
#include "window_controls.hpp"
#include "window_frame.hpp"
#include "main_menu_dialog.hpp"
#include "navigation_button.hpp"
#include "page_button.hpp"
#include "scrollbar_style.hpp"
#include "smooth_scroll.hpp"
#include "startup_registration.hpp"
#include "drag_views.hpp"
#include "lazy_directory_tree_model.hpp"
#include "interface_font.hpp"
#include "create_folder_button.hpp"
#include "toolbar_fields.hpp"
#include "preview_modified_label.hpp"

#include "batch_rename_dialog.hpp"
#include "rename_dialog.hpp"
#include "default_application_name.hpp"
#include "format_style.hpp"
#include "external_component_detection.hpp"
#include "release_configuration.hpp"
#include "theme_switch.hpp"
#include "thumbnail_scale.hpp"
#include "trash_dialog.hpp"
#include "diagnostic_sink.hpp"
#include "update_dialog.hpp"
#include "window_placement.hpp"
#include "vove/core/directory_entry.hpp"
#include "vove/color/color_transform.h"
#include "vove/fileops/current_operation_journal.hpp"
#include "vove/fileops/directory_transfer_manifest.hpp"
#include "vove/fileops/trash_catalog.hpp"

#include <QAbstractItemView>
#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFrame>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollBar>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QTreeWidget>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vove::ui {

bool trusted_internal_drop_source(const QObject *source, const QObject *catalog,
                                  const QObject *catalog_viewport) noexcept {
    return source != nullptr && (source == catalog || source == catalog_viewport);
}

namespace {

constexpr int treePathRole = DirectoryTreeModel::PathRole;
constexpr int favoriteKindRole = Qt::UserRole + 1;
constexpr int regularFavoriteKind = 0;
constexpr int trashFavoriteKind = 1;
constexpr auto directoryTransferStateKey = "operations/directoryTransferStateV1";
constexpr auto directoryTransferStateMagic = "VOVE-DIRECTORY-STATE-1\n";
constexpr qsizetype directoryTransferStateDigestBytes = 32;
constexpr qsizetype directoryTransferStateMaximumBytes = qsizetype{2} * 1'024 * 1'024;

bool same_directory_path(const QString &left, const QString &right) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir::fromNativeSeparators(left))
               .compare(QDir::cleanPath(QDir::fromNativeSeparators(right)), sensitivity) == 0;
}

bool is_trash_favorite(const QListWidgetItem *item) {
    return item != nullptr && item->data(favoriteKindRole).toInt() == trashFavoriteKind;
}

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

[[nodiscard]] QString external_open_helper_override() {
#ifdef VOVE_UI_TEST_HOOKS
    return QString::fromUtf8(qgetenv("VOVE_TEST_OPEN_HELPER_OVERRIDE"));
#else
    return {};
#endif
}

[[nodiscard]] QString global_search_tooltip() {
    return QCoreApplication::translate(
        "MainWindow", "Search approved work folders with the system filename index");
}

[[nodiscard]] QString global_search_unavailable_message() {
#ifdef Q_OS_WIN
    return QCoreApplication::translate(
        "MainWindow",
        "<b>Everything is unavailable.</b><br>"
        "Make sure it is installed, running, and available to VO-VE with the same permissions. "
        "<a href=\"https://www.voidtools.com/downloads/\">Install Everything</a>");
#else
    return QCoreApplication::translate(
        "MainWindow",
        "<b>plocate is unavailable.</b><br>"
        "Install plocate and update its filename index. It searches only mounted resources "
        "present in that index. <a href=\"https://plocate.sesse.net/\">About plocate</a>");
#endif
}

[[nodiscard]] QString global_search_scope_message(const QString &root_detail) {
#ifdef Q_OS_WIN
    return QCoreApplication::translate(
               "MainWindow", "<b>The work folder is not included in the Everything index.</b><br>"
                             "Add it to the folders indexed by Everything and retry the search.%1")
        .arg(root_detail);
#else
    return QCoreApplication::translate(
               "MainWindow",
               "<b>The work folder is absent from the plocate index.</b><br>"
               "Make sure the resource is mounted and included by updatedb, then refresh the "
               "index.%1")
        .arg(root_detail);
#endif
}

QByteArray seal_directory_transfer_payload(const QByteArray &payload) {
    const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    auto sealed = QByteArray(directoryTransferStateMagic) + digest + payload;
    return sealed.size() <= directoryTransferStateMaximumBytes ? sealed : QByteArray{};
}

QByteArray seal_directory_transfer_state(const QJsonObject &state) {
    return seal_directory_transfer_payload(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

std::optional<QJsonObject> open_directory_transfer_state(const QByteArray &sealed) {
    const QByteArray magic(directoryTransferStateMagic);
    if (sealed.size() > directoryTransferStateMaximumBytes || !sealed.startsWith(magic) ||
        sealed.size() <= magic.size() + directoryTransferStateDigestBytes) {
        return std::nullopt;
    }
    const auto digest = sealed.mid(magic.size(), directoryTransferStateDigestBytes);
    const auto payload = sealed.mid(magic.size() + directoryTransferStateDigestBytes);
    if (QCryptographicHash::hash(payload, QCryptographicHash::Sha256) != digest) {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    auto state = document.object();
    if (state.value(QStringLiteral("schema")).toInt() != 1) {
        return std::nullopt;
    }
    return state;
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

std::filesystem::path native_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(
        std::string(utf8.constData(), static_cast<std::size_t>(utf8.size())));
#endif
}

std::optional<QStringList> external_drop_paths(const QMimeData *mime) {
    constexpr qsizetype maximum_paths = 1'024;
    constexpr qsizetype maximum_path_units = 32'767;
    constexpr qsizetype maximum_total_units = qsizetype{1'024} * 1'024;
    constexpr qsizetype maximum_mime_bytes = qsizetype{2} * 1'024 * 1'024;
    if (mime == nullptr || !mime->hasUrls()) {
        return std::nullopt;
    }
    const auto encoded_urls = mime->data(QStringLiteral("text/uri-list"));
    if (encoded_urls.isEmpty() || encoded_urls.size() > maximum_mime_bytes ||
        encoded_urls.contains('\0')) {
        return std::nullopt;
    }
    const auto urls = mime->urls();
    if (urls.isEmpty() || urls.size() > maximum_paths) {
        return std::nullopt;
    }
    QStringList paths;
    QSet<QString> seen;
    qsizetype total_units{};
#ifdef Q_OS_WIN
    constexpr auto path_case = Qt::CaseInsensitive;
#else
    constexpr auto path_case = Qt::CaseSensitive;
#endif
    for (const auto &url : urls) {
        if (!url.isValid() || !url.isLocalFile() ||
            url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) != 0 ||
            !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
            return std::nullopt;
        }
        auto path = QDir::cleanPath(QDir::fromNativeSeparators(url.toLocalFile()));
        if (path.isEmpty() || path.contains(QChar::Null) || path.size() > maximum_path_units ||
            !QDir::isAbsolutePath(path)) {
            return std::nullopt;
        }
        total_units += path.size();
        if (total_units > maximum_total_units) {
            return std::nullopt;
        }
        auto key = path;
        if (path_case == Qt::CaseInsensitive) {
            key = key.toCaseFolded();
        }
        if (!seen.contains(key)) {
            seen.insert(key);
            paths.push_back(std::move(path));
        }
    }
    return paths.isEmpty() ? std::nullopt : std::optional<QStringList>(std::move(paths));
}

bool external_drop_requests_move(const QDropEvent &drop) {
    return (drop.modifiers() & Qt::ShiftModifier) != 0;
}

QString display_path(const std::filesystem::path &path) {
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.native());
#else
    const auto text = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char *>(text.data()),
                             static_cast<qsizetype>(text.size()));
#endif
}

std::string path_utf8(const std::filesystem::path &path) {
#ifdef Q_OS_WIN
    return display_path(path).toUtf8().toStdString();
#else
    const auto text = path.u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
#endif
}

qulonglong trash_entry_id(const std::uint64_t operation_id, const std::size_t item_index) noexcept {
    auto value = operation_id ^ (0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(item_index) +
                                 (operation_id << 6U) + (operation_id >> 2U));
    return static_cast<qulonglong>(value == 0 ? item_index + 1U : value);
}

QString transfer_path_key(const std::filesystem::path &path) {
    auto key = QDir::cleanPath(QDir::fromNativeSeparators(display_path(path)));
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

#ifdef Q_OS_WIN
bool is_unc_path(const QString &path) {
    return QDir::fromNativeSeparators(path).startsWith(QStringLiteral("//"));
}
#endif

std::filesystem::path current_journal_path(const QString &override_path) {
    if (!override_path.isEmpty()) {
        return native_path(override_path);
    }
    const auto state_root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return native_path(
        QDir(state_root).filePath(QString::fromLatin1(fileops::kCurrentOperationJournalFilename)));
}

std::filesystem::path trash_manifest_directory(const std::filesystem::path &journal_path) {
    return journal_path.parent_path() / "trash";
}

bool same_entry_snapshot(const core::DirectoryEntry &left,
                         const core::DirectoryEntry &right) noexcept {
    return left.kind == right.kind && left.path_utf8 == right.path_utf8 &&
           left.size_bytes == right.size_bytes && left.modified_unix_ns == right.modified_unix_ns &&
           left.source_revision_utf8 == right.source_revision_utf8;
}

bool same_entry_snapshots(const QList<core::DirectoryEntry> &left,
                          const QList<core::DirectoryEntry> &right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (qsizetype index{}; index < left.size(); ++index) {
        if (!same_entry_snapshot(left[index], right[index])) {
            return false;
        }
    }
    return true;
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

QColor operation_status_accent(const AppTheme theme) {
    switch (theme) {
    case AppTheme::white:
    case AppTheme::vanilla:
        return QColor(QStringLiteral("#0080B3"));
    case AppTheme::blue:
    case AppTheme::dark_gray:
        return QColor(QStringLiteral("#F5A623"));
    }
    return QColor(QStringLiteral("#F5A623"));
}

QColor technical_status_accent() {
    return QColor(QStringLiteral("#FF7900"));
}

QColor blend_colors(const QColor &base, const QColor &accent, const qreal amount) {
    const auto bounded = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    return QColor::fromRgbF(base.redF() + (accent.redF() - base.redF()) * bounded,
                            base.greenF() + (accent.greenF() - base.greenF()) * bounded,
                            base.blueF() + (accent.blueF() - base.blueF()) * bounded);
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

QString preview_status_text(const preview::helper_protocol::ResponseStatus status) {
    using Status = preview::helper_protocol::ResponseStatus;
    switch (status) {
    case Status::unsupported:
        return QCoreApplication::translate("MainWindow", "This format is not supported yet");
    case Status::malformed:
        return QCoreApplication::translate("MainWindow", "The file is damaged");
    case Status::disconnected:
    case Status::source_unavailable:
    case Status::source_changed:
    case Status::not_found:
        return QCoreApplication::translate("MainWindow", "The file is currently unavailable");
    case Status::authentication_failed:
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow", "Cannot access the file");
    case Status::too_large:
        return QCoreApplication::translate("MainWindow", "Preview exceeds image processing limits");
    case Status::pdf_input_too_large:
        return QCoreApplication::translate("MainWindow",
                                           "PDF file exceeds the 2 GiB viewing limit");
    case Status::memory_limit:
        return QCoreApplication::translate("MainWindow", "Preview memory limit exceeded");
    case Status::timed_out:
    case Status::processing_timed_out:
        return QCoreApplication::translate("MainWindow", "Preview processing time limit exceeded");
    case Status::cancelled:
        return QCoreApplication::translate("MainWindow", "Preview cancelled");
    case Status::queue_busy:
        return QCoreApplication::translate("MainWindow", "Preview is waiting for the queue");
    case Status::color_profile_required:
        return QCoreApplication::translate("MainWindow",
                                           "CMYK: no color profile is specified in the file");
    case Status::password_required:
        return QCoreApplication::translate("MainWindow", "The document is password-protected");
    case Status::document_password_incorrect:
        return QCoreApplication::translate("MainWindow", "Incorrect password");
    case Status::ghostscript_required:
        return ghostscript_available(false)
                   ? QCoreApplication::translate(
                         "MainWindow",
                         "Ghostscript is installed, but it could not be started for this preview")
                   : QCoreApplication::translate(
                         "MainWindow",
                         "Ghostscript is not installed\n\nIt is required to preview PS, EPS, "
                         "and legacy AI files without an embedded thumbnail");
    case Status::embedded_preview_unavailable:
        return QCoreApplication::translate(
            "MainWindow",
            "Embedded preview is unavailable\n\nThe file does not contain a recognized embedded "
            "thumbnail");
    case Status::internal_error:
        return QCoreApplication::translate("MainWindow", "Failed to generate preview");
    case Status::worker_start_failed:
        return worker_startup_message();
    case Status::success_cached:
    case Status::success_offline_cached:
    case Status::success_decoded:
        return QCoreApplication::translate("MainWindow", "Preparing preview…");
    }
    return QCoreApplication::translate("MainWindow", "Failed to generate preview");
}

QString localized_color_model(QString model) {
    if (model == QStringLiteral("MIXED")) {
        return QCoreApplication::translate("MainWindow", "Mixed");
    }
    if (model == QStringLiteral("LAB")) {
        return QStringLiteral("Lab");
    }
    return model;
}

QString color_summary(QString model, const QString &profile) {
    if (model == QStringLiteral("CMYK") &&
        profile == QLatin1StringView(color::kUnprofiledCmykApproximation)) {
        return QCoreApplication::translate("MainWindow",
                                           "CMYK without profile · approximate colors");
    }
    model = localized_color_model(std::move(model));
    if (profile.isEmpty()) {
        return model;
    }
    return model.isEmpty() ? profile : model + QStringLiteral(": ") + profile;
}

QString operation_status_text(const fileops::OperationStatus status) {
    using Status = fileops::OperationStatus;
    switch (status) {
    case Status::success:
        return QCoreApplication::translate("MainWindow", "Item renamed");
    case Status::invalid_request:
        return QCoreApplication::translate("MainWindow", "Invalid item name");
    case Status::not_found:
        return QCoreApplication::translate("MainWindow", "The source item is no longer present");
    case Status::conflict:
        return QCoreApplication::translate("MainWindow", "An item with this name already exists");
    case Status::source_changed:
        return QCoreApplication::translate("MainWindow",
                                           "The item changed. Refresh the folder and try again");
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow", "No permission to rename");
    case Status::authentication_required:
        return QCoreApplication::translate("MainWindow", "Network folder sign-in required");
    case Status::disconnected:
        return QCoreApplication::translate("MainWindow",
                                           "Connection to the network folder was lost");
    case Status::timed_out:
        return QCoreApplication::translate(
            "MainWindow", "The network folder did not respond. State was verified");
    case Status::unsupported:
        return QCoreApplication::translate("MainWindow", "This item cannot be renamed safely");
    case Status::unknown_outcome:
        return QCoreApplication::translate(
            "MainWindow", "The outcome is unclear. The operation was not repeated");
    case Status::io_error:
        return QCoreApplication::translate("MainWindow", "Failed to rename the item");
    case Status::cross_device:
        return QCoreApplication::translate("MainWindow", "The item is on another drive");
    case Status::file_in_use:
        return QCoreApplication::translate("OperationNotice", "File is in use by another application");
    }
    return QCoreApplication::translate("MainWindow", "Failed to rename the item");
}

bool ambiguous_rename_status(const fileops::OperationStatus status) {
    using Status = fileops::OperationStatus;
    return status == Status::disconnected || status == Status::timed_out ||
           status == Status::unknown_outcome || status == Status::io_error;
}

QString trash_operation_error_text(const fileops::OperationStatus status) {
    using Status = fileops::OperationStatus;
    switch (status) {
    case Status::success:
        return {};
    case Status::invalid_request:
        return QCoreApplication::translate("MainWindow", "Invalid Trash operation request");
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow", "No permission for this Trash operation");
    case Status::timed_out:
        return QCoreApplication::translate("MainWindow", "The operation timed out");
    case Status::unsupported:
        return QCoreApplication::translate("MainWindow", "This Trash operation is not supported");
    case Status::io_error:
        return QCoreApplication::translate("MainWindow", "Trash operation I/O error");
    default:
        return operation_status_text(status);
    }
}

bool confirms_no_committed_rename(const fileops::OperationResult &result) {
    return ambiguous_rename_status(result.status) && result.source_matches_expected &&
           !result.destination_present;
}

std::uint64_t next_directory_transfer_request_id() noexcept {
    static std::atomic_uint64_t next{
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    auto value = next.fetch_add(1U, std::memory_order_relaxed);
    if (value == 0U) {
        value = next.fetch_add(1U, std::memory_order_relaxed);
    }
    return value;
}

QString directory_transfer_error_text(const fileops::BasicDirectoryTransferStatus status) {
    using Status = fileops::BasicDirectoryTransferStatus;
    switch (status) {
    case Status::success:
        return {};
    case Status::move_pending_publication:
        return QCoreApplication::translate("MainWindow",
                                           "Move is waiting to be published: free the occupied "
                                           "name in the destination folder and choose Continue");
    case Status::recovery_required:
        return QCoreApplication::translate("MainWindow", "An incomplete operation was found");
    case Status::unknown_outcome:
        return QCoreApplication::translate("MainWindow", "The publication outcome is unknown");
    case Status::invalid_request:
        return QCoreApplication::translate(
            "MainWindow", "This folder cannot be transferred to the selected location");
    case Status::not_found:
        return QCoreApplication::translate("MainWindow",
                                           "The source or destination folder was not found");
    case Status::conflict:
        return QCoreApplication::translate(
            "MainWindow",
            "Name conflict: an item with this name already exists in the destination folder");
    case Status::source_changed:
        return QCoreApplication::translate("MainWindow",
                                           "The source folder changed during transfer");
    case Status::staging_changed:
        return QCoreApplication::translate("MainWindow", "Temporary transfer data was modified");
    case Status::permission_denied:
        return QCoreApplication::translate("MainWindow",
                                           "Insufficient permissions to transfer the folder");
    case Status::file_in_use:
        return QCoreApplication::translate("OperationNotice", "File is in use by another application");
    case Status::authentication_required:
        return QCoreApplication::translate("MainWindow", "The network folder requires sign-in");
    case Status::disconnected:
        return QCoreApplication::translate("MainWindow",
                                           "Connection to the network folder was lost");
    case Status::timed_out:
        return QCoreApplication::translate("MainWindow",
                                           "The network folder did not respond in time");
    case Status::unsupported:
        return QCoreApplication::translate(
            "MainWindow", "This network resource does not support safe folder transfer");
    case Status::io_error:
        return QCoreApplication::translate("MainWindow", "Failed to transfer the folder");
    }
    return QCoreApplication::translate("MainWindow", "Failed to transfer the folder");
}

fileops::FileTransferServiceOptions
directory_transfer_service_options(fileops::FileTransferServiceOptions options) {
    if (options.idle_timeout == std::chrono::seconds(15)) {
        options.idle_timeout = std::chrono::minutes(30);
    }
    return options;
}

[[nodiscard]] QByteArray application_icon_png(const QString &path) {
    QFileIconProvider provider;
    const auto pixmap = provider.icon(QFileInfo(path)).pixmap(QSize(32, 32));
    if (pixmap.isNull()) {
        return {};
    }
    QByteArray encoded;
    QBuffer buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !pixmap.save(&buffer, "PNG")) {
        return {};
    }
    return encoded;
}

[[nodiscard]] QIcon stored_application_icon(const QByteArray &encoded) {
    QPixmap pixmap;
    if (!encoded.isEmpty() && pixmap.loadFromData(encoded, "PNG")) {
        return QIcon(pixmap);
    }
    return {};
}

} // namespace

#ifdef VOVE_UI_TEST_HOOKS
QByteArray seal_directory_transfer_payload_for_test(const QByteArray &payload) {
    return seal_directory_transfer_payload(payload);
}
#endif

MainWindow::MainWindow(QString initial_path, QWidget *parent, QString preview_helper_override,
                       fileops::FileOperationServiceOptions file_operation_options,
                       const QString &batch_journal_override,
                       fileops::FileTransferServiceOptions file_transfer_options,
                       QString global_search_helper_override)
    : QMainWindow(parent), delegate_(this),
      previewClient_(this, 12'000, std::move(preview_helper_override)),
      folderMosaicController_(previewClient_),
      globalSearchClient_(this, std::move(global_search_helper_override)),
      session_(source_, listModel_), directoryMonitor_(this),
      currentOperationJournalPath_(current_journal_path(batch_journal_override)),
      trashManifestDirectory_(trash_manifest_directory(currentOperationJournalPath_)),
      fileOperationService_(file_operation_options),
      directoryTransferService_(directory_transfer_service_options(file_transfer_options)),
      batchRenameCoordinator_({.file_operations = file_operation_options,
                               .journal_path = currentOperationJournalPath_}),
      permanentDeleteCoordinator_({.file_operations = file_operation_options,
                                   .journal_path = currentOperationJournalPath_}),
      fileTransferCoordinator_({.file_operations = file_operation_options,
                                .file_transfers = file_transfer_options,
                                .journal_path = currentOperationJournalPath_}),
      objectTransferQueue_({.file_operations = file_operation_options,
                            .file_transfers = directory_transfer_service_options(file_transfer_options),
                            .journal_path = currentOperationJournalPath_}),
      trashCoordinator_({.file_operations = std::move(file_operation_options),
                         .journal_path = currentOperationJournalPath_,
                         .manifest_directory = trashManifestDirectory_}),
      externalOpenCoordinator_(external_open_helper_override()) {
    install_main_menu_dialog_frames(this);
    build_ui();
    installEventFilter(this);
    directoryMonitor_.refresh_requested = [this] { return refresh_catalog_automatically(); };
    load_settings();
    static_cast<void>(refresh_current_operation_journal_state());

    if (initial_path.isEmpty()) {
        initial_path = currentPath_;
    }
    if (initial_path.isEmpty()) {
        initial_path = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    currentPath_.clear();
    open_path(initial_path, true);
    QTimer::singleShot(0, this, [this] { bind_monitor_profile(); });
    QTimer::singleShot(0, this, [this] { start_trash_catalog_load(false); });
}

MainWindow::~MainWindow() {
    directoryMonitor_.stop();
    // The model cancels outstanding reads while its DirectorySource is still alive.
    delete treeModel_;
    treeModel_ = nullptr;
    clear_transfer_drag_cursor();
    trashCatalogLoader_.cancel();
    trashCoordinator_.stop();
    objectTransferQueue_.stop();
    fileTransferCoordinator_.stop();
    directoryTransferService_.stop();
    permanentDeleteCoordinator_.stop();
    batchRenameCoordinator_.stop();
    fileOperationService_.stop();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (pendingNavigationRestore_ && listView_ != nullptr && event != nullptr &&
        (watched == listView_ || watched == listView_->viewport() ||
         watched == listView_->verticalScrollBar()) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel ||
         event->type() == QEvent::KeyPress || event->type() == QEvent::TouchBegin)) {
        pendingNavigationRestore_.reset();
    }
    if (watched == this && event != nullptr && previewModified_ != nullptr &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowActivate)) {
        QTimer::singleShot(0, previewModified_, [this] { previewModified_->refresh(); });
    }
    if (watched == this && event != nullptr &&
        (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
         event->type() == QEvent::WindowStateChange)) {
        QTimer::singleShot(0, this, [this] { update_directory_monitor_visibility(); });
    }
    if (watched == previewModified_ && event != nullptr && previewCanvas_ != nullptr &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
         event->type() == QEvent::Polish)) {
        QTimer::singleShot(0, previewModified_, [this] {
            previewCanvas_->set_indicator_font(previewModified_->font());
        });
    }
    if (watched == previewCanvas_ && event != nullptr &&
        (event->type() == QEvent::Resize || event->type() == QEvent::DevicePixelRatioChange) &&
        previewTimer_ != nullptr) {
        if (selectedEntryId_ != 0 && listView_ != nullptr) {
            update_selected_preview(listView_->currentIndex());
        }
        previewTimer_->start();
    }
    const auto is_regular_favorites_viewport =
        favorites_ != nullptr && watched == favorites_->viewport();
    const auto is_trash_favorite_viewport =
        trashFavorite_ != nullptr && watched == trashFavorite_->viewport();
    const auto is_favorites_viewport = is_regular_favorites_viewport || is_trash_favorite_viewport;
    const auto is_favorites_pin_header = favoritesHeader_ != nullptr && watched == favoritesHeader_;
    auto *favorite_view = is_regular_favorites_viewport ? favorites_
                          : is_trash_favorite_viewport  ? trashFavorite_
                                                        : nullptr;
    const auto set_favorite_drop_target = [this](FavoritesView *view, QListWidgetItem *item) {
        favorites_->set_drop_target_item(view == favorites_ ? item : nullptr);
        trashFavorite_->set_drop_target_item(view == trashFavorite_ ? item : nullptr);
    };
    const auto clear_favorite_drop_target = [this] {
        favorites_->clear_drop_target_item();
        trashFavorite_->clear_drop_target_item();
    };
    const auto is_catalog_viewport = listView_ != nullptr && watched == listView_->viewport();
    const auto is_directory_tree_viewport =
        directoryTree_ != nullptr && watched == directoryTree_->viewport();
    const auto is_preview_drop_target = (previewPanel_ != nullptr && watched == previewPanel_) ||
                                        (previewCanvas_ != nullptr && watched == previewCanvas_);
    if (trashViewActive_ && event != nullptr && (is_catalog_viewport || is_preview_drop_target) &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::Drop)) {
        clear_drag_status();
        clear_transfer_drag_cursor();
        event->ignore();
        return true;
    }
    if (is_preview_drop_target && event != nullptr && event->type() == QEvent::DragLeave) {
        clear_drag_status();
        event->accept();
        return true;
    }
    if (is_preview_drop_target && event != nullptr &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::Drop)) {
        auto *drop = static_cast<QDropEvent *>(event);
        const auto private_mime = listModel_.mimeTypes().value(1);
        const auto paths = drop->mimeData() != nullptr && !drop->mimeData()->hasFormat(private_mime)
                               ? external_drop_paths(drop->mimeData())
                               : std::nullopt;
        if (!paths || (drop->possibleActions() & Qt::CopyAction) == 0) {
            clear_drag_status();
            drop->ignore();
            return true;
        }
        if (event->type() != QEvent::Drop) {
            const auto item_count = static_cast<int>(paths->size());
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            show_drag_status(
                false, false, item_count,
                QCoreApplication::translate("MainWindow", "Open %n item(s)", nullptr, item_count));
            return true;
        }
        clear_drag_status();
        open_external_paths(*paths);
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    if (is_catalog_viewport && event != nullptr && event->type() == QEvent::Resize) {
        update_global_search_message_geometry();
    }
    if (is_catalog_viewport && (globalSearchActive_ || recursiveViewActive_) && event != nullptr &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::Drop)) {
        clear_drag_status();
        clear_transfer_drag_cursor();
        event->ignore();
        return true;
    }
    QList<core::DirectoryEntry> authenticated_entries;
    bool authenticated_internal_drag{};
    const auto recovery_blocks_new_operations = [this] {
        return property("batchRenameRecoveryPending").toBool() ||
               property("permanentDeleteRecoveryPending").toBool() ||
               property("trashRecoveryPending").toBool() ||
               property("fileTransferRecoveryPending").toBool() ||
               property("directoryTransferRecoveryPending").toBool() ||
               property("fileOperationJournalBlocked").toBool();
    };
    const auto show_recovery_drag_status = [this] {
        show_drag_status(
            false, false, 0,
            QCoreApplication::translate(
                "MainWindow", "An incomplete file operation must be resolved first. Press F5"));
    };
    if ((is_favorites_viewport || is_favorites_pin_header || is_catalog_viewport ||
         is_directory_tree_viewport) &&
        event != nullptr && event->type() == QEvent::DragLeave) {
        clear_transfer_drag_cursor();
        set_favorites_pin_drop_active(false);
        clear_favorite_drop_target();
        externalDragMime_ = nullptr;
        externalDragPaths_.clear();
        clear_drag_status();
        event->accept();
        return true;
    }
    if ((is_favorites_viewport || is_favorites_pin_header || is_catalog_viewport ||
         is_directory_tree_viewport) &&
        event != nullptr &&
        (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
         event->type() == QEvent::Drop)) {
        QDropEvent *drop{};
        QPoint position;
        if (event->type() == QEvent::DragEnter) {
            auto *drag = static_cast<QDragEnterEvent *>(event);
            drop = drag;
            position = drag->position().toPoint();
        } else if (event->type() == QEvent::DragMove) {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            drop = drag;
            position = drag->position().toPoint();
        } else {
            drop = static_cast<QDropEvent *>(event);
            position = drop->position().toPoint();
            clear_transfer_drag_cursor();
            clear_drag_status();
        }

        const auto private_mime = listModel_.mimeTypes().value(1);
        const auto has_private_mime =
            drop->mimeData() != nullptr && drop->mimeData()->hasFormat(private_mime);
        auto trusted_internal_source =
            trusted_internal_drop_source(drop->source(), listView_, listView_->viewport());
#ifdef VOVE_TEST_ALLOW_SYNTHETIC_INTERNAL_DRAG
        trusted_internal_source = trusted_internal_source || drop->source() == nullptr;
#endif
        authenticated_internal_drag =
            has_private_mime && trusted_internal_source &&
            listModel_.decode_internal_drag(drop->mimeData(), authenticated_entries);
        if (authenticated_internal_drag && globalSearchActive_) {
            authenticated_internal_drag = listView_->dragEnabled() &&
                std::ranges::all_of(authenticated_entries, [this](const auto &entry) {
                    const auto *current = listModel_.entry_for_id(entry.id);
                    return current && same_entry_snapshot(*current, entry);
                });
        }
        if (!has_private_mime) {
            const auto tree_mime = QString::fromLatin1(directoryTreeDragMime);
            const auto has_tree_mime =
                drop->mimeData() != nullptr && drop->mimeData()->hasFormat(tree_mime);
            auto trusted_tree_source = trusted_internal_drop_source(drop->source(), directoryTree_,
                                                                    directoryTree_->viewport());
#ifdef VOVE_TEST_ALLOW_SYNTHETIC_INTERNAL_DRAG
            trusted_tree_source = trusted_tree_source || drop->source() == nullptr;
#endif
            const auto authenticated_tree_drag = has_tree_mime && trusted_tree_source;
            if (has_tree_mime && !authenticated_tree_drag) {
                // Do not let an external process spoof the tree-only routing contract.
                set_favorites_pin_drop_active(false);
                clear_favorite_drop_target();
                clear_drag_status();
                clear_transfer_drag_cursor();
                drop->ignore();
                return true;
            }
            if (event->type() == QEvent::DragEnter) {
                externalDragMime_ = nullptr;
                externalDragPaths_.clear();
                if (auto parsed = external_drop_paths(drop->mimeData())) {
                    externalDragMime_ = drop->mimeData();
                    externalDragPaths_ = std::move(*parsed);
                }
            }
            const auto *paths =
                externalDragMime_ == drop->mimeData() && !externalDragPaths_.isEmpty()
                    ? &externalDragPaths_
                    : nullptr;
            const auto blank_favorites =
                is_regular_favorites_viewport && favorite_view->itemAt(position) == nullptr;
            auto *favorite_target =
                is_favorites_viewport ? favorite_view->itemAt(position) : nullptr;
            const auto trash_target = is_trash_favorite(favorite_target);
            const auto pin_target =
                is_favorites_pin_header ||
                (is_regular_favorites_viewport && (blank_favorites || authenticated_tree_drag));
            const auto tree_target =
                is_directory_tree_viewport ? directoryTree_->indexAt(position) : QModelIndex{};
            const auto tree_target_path = tree_target.data(treePathRole).toString();
            const auto catalog_target_index =
                is_catalog_viewport ? listView_->indexAt(position) : QModelIndex{};
            const auto *catalog_target_entry = listModel_.entry_at(catalog_target_index);
            const auto catalog_target_path =
                catalog_target_entry != nullptr &&
                        catalog_target_entry->kind == core::EntryKind::directory
                    ? QString::fromUtf8(catalog_target_entry->path_utf8)
                    : QString{};
            const auto transfer_target = authenticated_tree_drag
                                             ? (trash_target || is_catalog_viewport)
                                             : (favorite_target != nullptr ||
                                                !tree_target_path.isEmpty() || is_catalog_viewport);
            const auto transfer_destination =
                is_catalog_viewport
                    ? (catalog_target_path.isEmpty() ? currentPath_ : catalog_target_path)
                : favorite_target != nullptr && !trash_target
                    ? favorite_target->data(Qt::UserRole).toString()
                : !tree_target_path.isEmpty() ? tree_target_path
                                              : currentPath_;
            const auto immediate_tree_pin = authenticated_tree_drag && pin_target;
            const auto drop_probe_available = !pendingExternalDirectoryDrop_ &&
                                              !pendingExternalTransferDrop_ &&
                                              !pendingDirectoryPreparation_ &&
                                              dropProbeSource_.idle();
            const auto admissible =
                paths != nullptr && (immediate_tree_pin || drop_probe_available) &&
                (pin_target || transfer_target) &&
                (!transfer_target ||
                 (!renameInFlight_ && !deleteInFlight_ && !transferInFlight_ &&
                  !createDirectoryInFlight_ && !fileOperationService_.busy() &&
                  !directoryTransferService_.busy() && !batchRenameCoordinator_.busy() &&
                  !permanentDeleteCoordinator_.busy() && !trashCoordinator_.busy() &&
                  !fileTransferCoordinator_.busy() && !recovery_blocks_new_operations())) &&
                (drop->possibleActions() & Qt::CopyAction) != 0;
            if (event->type() != QEvent::Drop) {
                set_favorites_pin_drop_active(admissible && pin_target);
                set_favorite_drop_target(admissible && transfer_target && favorite_target != nullptr
                                             ? favorite_view
                                             : nullptr,
                                         admissible && transfer_target ? favorite_target : nullptr);
                if (admissible) {
                    const auto move =
                        trash_target || (transfer_target && external_drop_requests_move(*drop));
                    update_transfer_drag_cursor(move);
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    show_drag_status(
                        transfer_target, move, static_cast<int>(paths->size()),
                        trash_target
                            ? QCoreApplication::translate("MainWindow", "Move to VO-VE Trash: %1")
                                  .arg(paths->size())
                        : pin_target ? (authenticated_tree_drag
                                            ? QCoreApplication::translate(
                                                  "MainWindow", "Add folders to Favorites: %1")
                                                  .arg(paths->size())
                                            : QCoreApplication::translate(
                                                  "MainWindow", "Checking folders for Favorites…"))
                                     : QString{});
                } else {
                    clear_transfer_drag_cursor();
                    drop->ignore();
                    if (paths != nullptr && transfer_target && recovery_blocks_new_operations()) {
                        show_recovery_drag_status();
                    } else {
                        clear_drag_status();
                    }
                }
                return true;
            }

            set_favorites_pin_drop_active(false);
            clear_favorite_drop_target();
            const auto transfer_kind = external_drop_requests_move(*drop)
                                           ? fileops::FileTransferKind::move
                                           : fileops::FileTransferKind::copy;
            auto accepted = false;
            if (admissible && pin_target && authenticated_tree_drag) {
                for (const auto &path : *paths) {
                    add_favorite(path);
                }
                accepted = true;
            } else if (admissible && pin_target) {
                accepted = begin_external_directory_drop(ExternalDirectoryDropIntent::pin, *paths);
            } else if (admissible) {
                accepted = begin_external_transfer_drop(*paths, transfer_destination, transfer_kind,
                                                        trash_target);
            }
            if (accepted) {
                drop->setDropAction(Qt::CopyAction);
                drop->accept();
            } else {
                drop->ignore();
            }
            externalDragMime_ = nullptr;
            externalDragPaths_.clear();
            return true;
        }
        if (has_private_mime && !authenticated_internal_drag) {
            // A private VO-VE payload is meaningful only with this model's in-process secret.
            set_favorites_pin_drop_active(false);
            clear_favorite_drop_target();
            clear_drag_status();
            clear_transfer_drag_cursor();
            drop->ignore();
            return true;
        }
        if (is_favorites_pin_header && authenticated_internal_drag) {
            clear_favorite_drop_target();
            const auto all_directories =
                !authenticated_entries.isEmpty() &&
                std::ranges::all_of(authenticated_entries, [](const auto &entry) {
                    return entry.kind == core::EntryKind::directory;
                });
            const auto available =
                all_directories && (drop->possibleActions() & Qt::CopyAction) != 0;
            if (event->type() != QEvent::Drop) {
                set_favorites_pin_drop_active(available);
                clear_transfer_drag_cursor();
                if (available) {
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    show_drag_status(
                        false, false, static_cast<int>(authenticated_entries.size()),
                        QCoreApplication::translate("MainWindow", "Add folders to Favorites: %1")
                            .arg(authenticated_entries.size()));
                } else {
                    clear_drag_status();
                    drop->ignore();
                }
                return true;
            }
            set_favorites_pin_drop_active(false);
            if (available) {
                if (globalSearchActive_) {
                    QStringList paths;
                    for (const auto &entry : authenticated_entries)
                        paths.push_back(QString::fromUtf8(entry.path_utf8));
                    if (!begin_external_directory_drop(ExternalDirectoryDropIntent::pin, paths)) {
                        drop->ignore();
                        return true;
                    }
                } else {
                    for (const auto &entry : authenticated_entries) {
                        add_favorite(QString::fromUtf8(entry.path_utf8));
                    }
                }
                drop->setDropAction(Qt::CopyAction);
                drop->accept();
            } else {
                drop->ignore();
            }
            return true;
        }
        if (event->type() == QEvent::DragEnter && authenticated_internal_drag &&
            !authenticated_entries.isEmpty() && (drop->possibleActions() & Qt::CopyAction) != 0) {
            clear_favorite_drop_target();
            if (recovery_blocks_new_operations()) {
                clear_transfer_drag_cursor();
                show_recovery_drag_status();
                drop->ignore();
                return true;
            }
            // Enter admits the payload, not the first pixel under the pointer. Otherwise
            // rejecting a blank entry point prevents Qt from delivering later folder hovers.
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            return true;
        }
        if (is_catalog_viewport) {
            const auto target_index = listView_->indexAt(position);
            const auto *target_entry = listModel_.entry_at(target_index);
            const auto destination = target_entry != nullptr && target_entry->kind == core::EntryKind::directory
                ? QString::fromUtf8(target_entry->path_utf8)
                : (!target_index.isValid() && !globalSearchActive_ ? currentPath_ : QString{});
            const auto available =
                !destination.isEmpty() && !authenticated_entries.isEmpty() && !renameInFlight_ && !deleteInFlight_ &&
                !transferInFlight_ && !createDirectoryInFlight_ && !fileOperationService_.busy() &&
                !directoryTransferService_.busy() && !batchRenameCoordinator_.busy() &&
                !permanentDeleteCoordinator_.busy() && !trashCoordinator_.busy() &&
                !fileTransferCoordinator_.busy() && !recovery_blocks_new_operations() &&
                (drop->possibleActions() & Qt::CopyAction) != 0;
            if (event->type() != QEvent::Drop) {
                if (available) {
                    update_transfer_drag_cursor((drop->modifiers() & Qt::ShiftModifier) != 0);
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    show_drag_status(true, (drop->modifiers() & Qt::ShiftModifier) != 0,
                                     static_cast<int>(authenticated_entries.size()));
                } else {
                    clear_transfer_drag_cursor();
                    if (authenticated_internal_drag && target_entry != nullptr &&
                        recovery_blocks_new_operations()) {
                        show_recovery_drag_status();
                    } else {
                        clear_drag_status();
                    }
                    drop->ignore();
                }
                return true;
            }
            const auto kind = (drop->modifiers() & Qt::ShiftModifier) != 0
                                  ? fileops::FileTransferKind::move
                                  : fileops::FileTransferKind::copy;
            const auto started =
                available && start_transfer_selection(authenticated_entries, destination, kind);
            if (started) {
                // The source model exposes only CopyAction so Qt never removes data on its own.
                // Shift changes the durable target-side operation and the visible status text.
                drop->setDropAction(Qt::CopyAction);
                drop->accept();
            } else {
                drop->ignore();
            }
            return true;
        }
        if (is_directory_tree_viewport) {
            const auto target_index = directoryTree_->indexAt(position);
            const auto destination = target_index.data(treePathRole).toString();
            const auto available =
                !destination.isEmpty() && !authenticated_entries.isEmpty() && !renameInFlight_ &&
                !deleteInFlight_ && !transferInFlight_ && !createDirectoryInFlight_ &&
                !fileOperationService_.busy() && !directoryTransferService_.busy() &&
                !batchRenameCoordinator_.busy() && !permanentDeleteCoordinator_.busy() &&
                !trashCoordinator_.busy() && !fileTransferCoordinator_.busy() &&
                !recovery_blocks_new_operations() &&
                (drop->possibleActions() & Qt::CopyAction) != 0;
            if (event->type() != QEvent::Drop) {
                if (available) {
                    const auto move = (drop->modifiers() & Qt::ShiftModifier) != 0;
                    update_transfer_drag_cursor(move);
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    show_drag_status(true, move, static_cast<int>(authenticated_entries.size()));
                } else {
                    clear_transfer_drag_cursor();
                    if (authenticated_internal_drag && !destination.isEmpty() &&
                        recovery_blocks_new_operations()) {
                        show_recovery_drag_status();
                    } else {
                        clear_drag_status();
                    }
                    drop->ignore();
                }
                return true;
            }
            const auto kind = (drop->modifiers() & Qt::ShiftModifier) != 0
                                  ? fileops::FileTransferKind::move
                                  : fileops::FileTransferKind::copy;
            const auto started =
                available && start_transfer_selection(authenticated_entries, destination, kind);
            if (started) {
                drop->setDropAction(Qt::CopyAction);
                drop->accept();
            } else {
                drop->ignore();
            }
            return true;
        }
    }
    if (favorite_view != nullptr && is_favorites_viewport && event != nullptr) {
        const auto requested_action =
            [this, favorite_view](const QList<core::DirectoryEntry> &entries,
                                  const QPoint &position, const Qt::DropActions possible,
                                  const Qt::KeyboardModifiers modifiers) {
                if (entries.isEmpty()) {
                    return Qt::IgnoreAction;
                }
                if (globalSearchActive_ && is_trash_favorite(favorite_view->itemAt(position)))
                    return Qt::IgnoreAction;
                if (favorite_view->itemAt(position) == nullptr) {
                    return favorite_view == favorites_ && (possible & Qt::CopyAction) != 0 &&
                                   std::ranges::all_of(entries,
                                                       [](const auto &entry) {
                                                           return entry.kind ==
                                                                  core::EntryKind::directory;
                                                       })
                               ? Qt::CopyAction
                               : Qt::IgnoreAction;
                }
                const auto all_trash_objects = std::ranges::all_of(entries, [](const auto &entry) {
                    return entry.kind == core::EntryKind::file ||
                           entry.kind == core::EntryKind::directory;
                });
                if (!all_trash_objects) {
                    return Qt::IgnoreAction;
                }
                static_cast<void>(modifiers);
                if (renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
                    createDirectoryInFlight_ || fileOperationService_.busy() ||
                    batchRenameCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
                    trashCoordinator_.busy() || fileTransferCoordinator_.busy() ||
                    directoryTransferService_.busy() ||
                    property("batchRenameRecoveryPending").toBool() ||
                    property("permanentDeleteRecoveryPending").toBool() ||
                    property("trashRecoveryPending").toBool() ||
                    property("fileTransferRecoveryPending").toBool() ||
                    property("directoryTransferRecoveryPending").toBool() ||
                    property("fileOperationJournalBlocked").toBool()) {
                    return Qt::IgnoreAction;
                }
                return (possible & Qt::CopyAction) != 0 ? Qt::CopyAction : Qt::IgnoreAction;
            };
        if (event->type() == QEvent::DragMove) {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            const auto action =
                authenticated_internal_drag
                    ? requested_action(authenticated_entries, drag->position().toPoint(),
                                       drag->possibleActions(), drag->modifiers())
                    : Qt::IgnoreAction;
            if (action != Qt::IgnoreAction) {
                auto *target = favorite_view->itemAt(drag->position().toPoint());
                const auto trash_target = is_trash_favorite(target);
                set_favorites_pin_drop_active(target == nullptr && favorite_view == favorites_);
                set_favorite_drop_target(target != nullptr ? favorite_view : nullptr, target);
                update_transfer_drag_cursor(
                    trash_target ||
                    (target != nullptr && (drag->modifiers() & Qt::ShiftModifier) != 0));
                drag->setDropAction(action);
                drag->accept();
                show_drag_status(
                    target != nullptr, trash_target || (drag->modifiers() & Qt::ShiftModifier) != 0,
                    static_cast<int>(authenticated_entries.size()),
                    trash_target
                        ? QCoreApplication::translate("MainWindow", "Move to VO-VE Trash: %1")
                              .arg(authenticated_entries.size())
                        : QCoreApplication::translate("MainWindow", "Add folders to Favorites: %1")
                              .arg(authenticated_entries.size()));
                return true;
            }
            set_favorites_pin_drop_active(false);
            clear_favorite_drop_target();
            clear_transfer_drag_cursor();
            if (authenticated_internal_drag &&
                favorite_view->itemAt(drag->position().toPoint()) != nullptr &&
                recovery_blocks_new_operations()) {
                show_recovery_drag_status();
            } else {
                clear_drag_status();
            }
            drag->ignore();
            return true;
        } else if (event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            set_favorites_pin_drop_active(false);
            clear_favorite_drop_target();
            if (!authenticated_internal_drag) {
                drop->ignore();
                return true;
            }
            auto *target = favorite_view->itemAt(drop->position().toPoint());
            const auto action = requested_action(authenticated_entries, drop->position().toPoint(),
                                                 drop->possibleActions(), drop->modifiers());
            if (target == nullptr && action == Qt::CopyAction &&
                std::ranges::all_of(authenticated_entries, [](const auto &entry) {
                    return entry.kind == core::EntryKind::directory;
                })) {
                if (globalSearchActive_) {
                    QStringList paths;
                    for (const auto &entry : authenticated_entries)
                        paths.push_back(QString::fromUtf8(entry.path_utf8));
                    if (!begin_external_directory_drop(ExternalDirectoryDropIntent::pin, paths)) {
                        drop->ignore();
                        return true;
                    }
                } else {
                    for (const auto &entry : authenticated_entries) {
                        add_favorite(QString::fromUtf8(entry.path_utf8));
                    }
                }
                drop->setDropAction(Qt::CopyAction);
                drop->accept();
                return true;
            }
            if (is_trash_favorite(target) && action != Qt::IgnoreAction &&
                std::ranges::all_of(authenticated_entries, [](const auto &entry) {
                    return entry.kind == core::EntryKind::file ||
                           entry.kind == core::EntryKind::directory;
                })) {
                if (prompt_move_entries_to_trash(authenticated_entries)) {
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    return true;
                }
                drop->ignore();
                return true;
            }
            if (target != nullptr && action != Qt::IgnoreAction &&
                std::ranges::all_of(authenticated_entries, [](const auto &entry) {
                    return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
                })) {
                const auto kind = (drop->modifiers() & Qt::ShiftModifier) != 0
                                      ? fileops::FileTransferKind::move
                                      : fileops::FileTransferKind::copy;
                if (start_transfer_selection(authenticated_entries,
                                        target->data(Qt::UserRole).toString(), kind)) {
                    // Qt sees a target-owned copy transport. The durable VO-VE coordinator alone
                    // performs the requested move and the source view never deletes independently.
                    drop->setDropAction(Qt::CopyAction);
                    drop->accept();
                    return true;
                }
            }
            drop->ignore();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::update_transfer_drag_cursor(const bool move) {
    if (set_local_drag_move_feedback(move)) {
        return;
    }
    if (move && !dragMoveCursorOverride_) {
        QApplication::setOverrideCursor(QCursor(Qt::DragMoveCursor));
        dragMoveCursorOverride_ = true;
    } else if (!move) {
        clear_transfer_drag_cursor();
    }
}

void MainWindow::clear_transfer_drag_cursor() {
    set_local_drag_move_feedback(false);
    if (dragMoveCursorOverride_) {
        QApplication::restoreOverrideCursor();
        dragMoveCursorOverride_ = false;
    }
}

void MainWindow::show_drag_status(const bool transfer, const bool move, const int count,
                                  const QString &message) {
    if (transfer) {
        const auto red = QStringLiteral("<span style=\"color:#ff414c\">%1</span>");
        dragStatus_ = move ? red.arg(QCoreApplication::translate("MainWindow", "Move: %n item(s)",
                                                                 nullptr, count)
                                         .toHtmlEscaped())
                           : QStringLiteral("<span style=\"color:#00b7ff\">%1</span> %2")
                                 .arg(QCoreApplication::translate("MainWindow", "Copy %n item(s)",
                                                                  nullptr, count)
                                          .toHtmlEscaped(),
                                      red.arg(QCoreApplication::translate(
                                                  "MainWindow", "(hold Shift to move instead)")
                                                  .toHtmlEscaped()));
    } else {
        dragStatus_ = message.toHtmlEscaped();
    }
    setProperty("dragStatusActive", true);
    update_status(lastUpdate_);
}

void MainWindow::clear_drag_status() {
    if (dragStatus_.isEmpty()) {
        return;
    }
    dragStatus_.clear();
    setProperty("dragStatusActive", false);
    update_status(lastUpdate_);
}

MainWindow::CurrentOperationJournalState MainWindow::refresh_current_operation_journal_state() {
    const auto loaded_operation =
        fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    const auto delete_recovery =
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        loaded_operation.kind == fileops::CurrentOperationKind::permanent_delete;
    const auto batch_recovery =
        loaded_operation.ok() &&
        (loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::legacy_untyped ||
         (loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
          loaded_operation.kind == fileops::CurrentOperationKind::batch_rename));
    const auto trash_recovery =
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded_operation.kind == fileops::CurrentOperationKind::trash_move ||
         loaded_operation.kind == fileops::CurrentOperationKind::trash_restore ||
         loaded_operation.kind == fileops::CurrentOperationKind::trash_purge);
    const auto transfer_recovery =
        loaded_operation.ok() &&
        loaded_operation.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        (loaded_operation.kind == fileops::CurrentOperationKind::file_transfer ||
         loaded_operation.kind == fileops::CurrentOperationKind::object_transfer);
    const auto journal_blocked =
        loaded_operation.status != fileops::DurableJournalStatus::not_found && !delete_recovery &&
        !batch_recovery && !trash_recovery && !transfer_recovery;
    setProperty("batchRenameRecoveryPending", batch_recovery);
    setProperty("permanentDeleteRecoveryPending", delete_recovery);
    setProperty("trashRecoveryPending", trash_recovery);
    setProperty("fileTransferRecoveryPending", transfer_recovery);
    const auto directory_transfer_recovery =
        directoryTransferStateCorrupt_ || !directoryTransferRecoveryManifest_.isEmpty() ||
        (directoryTransferRecoveryRequestId_ != 0U && !directoryTransferRecoverySource_.isEmpty() &&
         !directoryTransferRecoveryDestination_.isEmpty() &&
         !directoryTransferRecoverySourceRevision_.isEmpty());
    setProperty("directoryTransferRecoveryPending", directory_transfer_recovery);
    setProperty("fileOperationJournalBlocked", journal_blocked);

    CurrentOperationJournalState state = CurrentOperationJournalState::none;
    if (directory_transfer_recovery) {
        state = CurrentOperationJournalState::directory_transfer_recovery;
        operationStatus_ =
            directoryTransferStateCorrupt_
                ? QCoreApplication::translate(
                      "MainWindow", "Folder transfer state is corrupted; data was not changed and "
                                    "requires manual review")
                : QCoreApplication::translate(
                      "MainWindow", "Incomplete folder transfer. Press F5 to choose an action");
    } else if (trash_recovery) {
        state = CurrentOperationJournalState::trash_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete VO-VE Trash operation. Press F5 to recover");
    } else if (transfer_recovery) {
        state = CurrentOperationJournalState::transfer_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete copy or move. Press F5 to recover");
    } else if (delete_recovery) {
        state = CurrentOperationJournalState::delete_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete deletion. Press F5 to recover safely");
    } else if (batch_recovery) {
        state = CurrentOperationJournalState::batch_recovery;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Incomplete batch rename. Press F5 to recover");
    } else if (journal_blocked) {
        state = CurrentOperationJournalState::blocked;
        operationStatus_ =
            loaded_operation.status == fileops::DurableJournalStatus::corrupt
                ? QCoreApplication::translate(
                      "MainWindow",
                      "The current operation journal is corrupted. File commands are blocked")
                : QCoreApplication::translate(
                      "MainWindow", "The current operation journal was created by another version. "
                                    "File commands are blocked");
    }
    update_rename_action();
    return state;
}

void MainWindow::bind_monitor_profile() {
    auto *active_screen = windowHandle() != nullptr ? windowHandle()->screen() : screen();
    if (previewClient_.set_monitor_name(active_screen != nullptr ? active_screen->name()
                                                                 : QString{})) {
        monitor_profile_changed();
    }
    if (monitorProfileBound_ || windowHandle() == nullptr) {
        return;
    }
    monitorProfileBound_ = true;
    connect(windowHandle(), &QWindow::screenChanged, this, [this](QScreen *changed) {
        if (previewClient_.set_monitor_name(changed != nullptr ? changed->name() : QString{})) {
            monitor_profile_changed();
        }
    });
}

void MainWindow::monitor_profile_changed() {
    reset_selected_preview();
    listModel_.clear_preview_images();
    completedPreviewEdges_.clear();
    folderMosaicController_.invalidate_display_images();
    if (listView_ != nullptr && listView_->currentIndex().isValid()) {
        if (selectedPageIndex_ > 0 && selectedPageIndex_ < selectedPageCount_) {
            request_selected_page(selectedPageIndex_);
        } else {
            update_selected_preview(listView_->currentIndex());
            ensure_selected_preview();
        }
    }
    if (previewTimer_ != nullptr) {
        previewTimer_->start();
    }
}

void MainWindow::build_ui() {
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("VO-VE"));
    setMinimumSize(980, 620);
    resize(1440, 900);

    auto *central = new QWidget(this);
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
    toolbarContent_ = toolbar_content;
    toolbar_content->setLayoutDirection(QApplication::layoutDirection());
    toolbar_content->setObjectName(QStringLiteral("toolbarContent"));
    toolbar_content->setProperty("windowDragArea", true);
    auto *tools = new QHBoxLayout(toolbar_content);
    tools->setContentsMargins(6, 1, 0, 1);
    tools->setSpacing(2);

    backButton_ = new NavigationButton(NavigationDirection::back,
                                       QCoreApplication::translate("MainWindow", "Back"), top_bar);
    backButton_->setObjectName(QStringLiteral("backButton"));
    forwardButton_ =
        new NavigationButton(NavigationDirection::forward,
                             QCoreApplication::translate("MainWindow", "Forward"), top_bar);
    forwardButton_->setObjectName(QStringLiteral("forwardButton"));
    historyButton_ = new NavigationButton(
        NavigationDirection::history,
        QCoreApplication::translate("MainWindow", "Navigation history"), top_bar);
    historyButton_->setObjectName(QStringLiteral("historyButton"));
    historyMenu_ = new QMenu(historyButton_);
    historyButton_->setMenu(historyMenu_);
    historyButton_->setPopupMode(QToolButton::InstantPopup);
    upButton_ =
        new NavigationButton(NavigationDirection::up,
                             QCoreApplication::translate("MainWindow", "Up one level"), top_bar);
    upButton_->setObjectName(QStringLiteral("upButton"));
    createFolderAction_ =
        new QAction(create_folder_icon(theme_),
                    QCoreApplication::translate("MainWindow", "Create Folder"), this);
    createFolderAction_->setObjectName(QStringLiteral("createFolderAction"));
    createFolderAction_->setShortcut(QKeySequence(Qt::Key_F7));
    createFolderAction_->setShortcutVisibleInContextMenu(true);
    addAction(createFolderAction_);
    createFolderButton_ = new CreateFolderButton(top_bar);
    createFolderButton_->setObjectName(QStringLiteral("createFolderButton"));
    createFolderButton_->setDefaultAction(createFolderAction_);

    filterEdit_ = new SearchField(top_bar);
    filterEdit_->setObjectName(QStringLiteral("catalogFilter"));
    filterEdit_->setPlaceholderText(QCoreApplication::translate("MainWindow", "Filter by name"));

    globalSearchEdit_ = new SearchField(top_bar);
    globalSearchEdit_->setObjectName(QStringLiteral("globalSearch"));
    globalSearchEdit_->setPlaceholderText(
        QCoreApplication::translate("MainWindow", "Global search"));
    globalSearchEdit_->setToolTip(global_search_tooltip());

    const auto sort_controls = create_sort_controls(top_bar);
    sortControls_ = sort_controls.widget;
    sortField_ = sort_controls.field;
    sortField_->setObjectName(QStringLiteral("sortField"));
    sortField_->setToolTip(QCoreApplication::translate("MainWindow", "Sort field"));
    sortField_->addItem(QCoreApplication::translate("MainWindow", "Name"),
                        static_cast<int>(core::SortField::name));
    sortField_->addItem(QCoreApplication::translate("MainWindow", "Extension"),
                        static_cast<int>(core::SortField::extension));
    sortField_->addItem(QCoreApplication::translate("MainWindow", "Date"),
                        static_cast<int>(core::SortField::modified));
    sortField_->addItem(QCoreApplication::translate("MainWindow", "Size"),
                        static_cast<int>(core::SortField::size));

    sortDirection_ = sort_controls.direction;
    sortDirection_->setObjectName(QStringLiteral("sortDirection"));
    sortDirection_->setToolTip(QCoreApplication::translate("MainWindow", "Sort direction"));

    const auto thumbnail_scale = create_thumbnail_scale(top_bar);
    smallerThumbnailsButton_ = thumbnail_scale.smaller;
    sizeSlider_ = thumbnail_scale.slider;
    sizeSlider_->setValue(delegate_.thumbnail_extent());
    largerThumbnailsButton_ = thumbnail_scale.larger;

    logoButton_ = new QToolButton(top_bar);
    logoButton_->setObjectName(QStringLiteral("logo"));
    logoButton_->setIcon(authored_logo(theme_));
    logoButton_->setIconSize(QSize(104, 20));
    logoButton_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    logoButton_->setFixedSize(108, 28);
    logoButton_->setAutoRaise(true);
    logoButton_->setFocusPolicy(Qt::StrongFocus);
    mainMenu_ = new QMenu(logoButton_);
    mainMenu_->setObjectName(QStringLiteral("mainMenu"));
    languageMenu_ = mainMenu_->addMenu(QString());
    languageMenu_->setObjectName(QStringLiteral("languageMenu"));
    languageMenu_->menuAction()->setObjectName(QStringLiteral("languageMenuAction"));
    languageActions_ = new QActionGroup(this);
    languageActions_->setExclusive(true);
    for (const auto language : {AppLanguage::russian, AppLanguage::english,
                                AppLanguage::chinese_simplified, AppLanguage::arabic}) {
        auto *action = languageMenu_->addAction(language_native_name(language));
        action->setObjectName(QStringLiteral("language_%1").arg(language_code(language)));
        action->setData(static_cast<int>(language));
        action->setCheckable(true);
        action->setEnabled(translations_.available(language));
        languageActions_->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, language] { apply_language(language, true); });
    }
    settingsAction_ = mainMenu_->addAction(QString());
    settingsAction_->setObjectName(QStringLiteral("settingsAction"));
    componentsAction_ = mainMenu_->addAction(QString());
    componentsAction_->setObjectName(QStringLiteral("componentsAction"));
    updateAction_ = mainMenu_->addAction(QString());
    updateAction_->setObjectName(QStringLiteral("updateAction"));
    mainMenu_->addSeparator();
    aboutAction_ = mainMenu_->addAction(QString());
    aboutAction_->setObjectName(QStringLiteral("aboutAction"));
    donateAction_ = mainMenu_->addAction(QString());
    donateAction_->setObjectName(QStringLiteral("donateAction"));
    reportIssueAction_ = mainMenu_->addAction(QString());
    reportIssueAction_->setObjectName(QStringLiteral("reportIssueAction"));

    // The recovery catalog remains an internal action until its final Stage F placement is
    // approved.
    trashAction_ = new QAction(QCoreApplication::translate("MainWindow", "VO-VE Trash…"), this);
    trashAction_->setObjectName(QStringLiteral("trashAction"));
    logoButton_->setMenu(mainMenu_);
    logoButton_->setPopupMode(QToolButton::InstantPopup);

    tools->addWidget(backButton_);
    tools->addWidget(forwardButton_);
    tools->addWidget(historyButton_);
    tools->addWidget(upButton_);
    tools->addWidget(createFolderButton_);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(filterEdit_);
    tools->addWidget(globalSearchEdit_);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(sortControls_);
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
    recursiveViewButton_ = new CatalogToolButton(CatalogToolKind::recursive_view, catalog_tools);
    recursiveViewButton_->setObjectName(QStringLiteral("recursiveViewButton"));
    recursiveViewButton_->setCheckable(true);
    refreshButton_ = new CatalogToolButton(CatalogToolKind::refresh, catalog_tools);
    refreshButton_->setObjectName(QStringLiteral("refreshButton"));
    catalog_tools_layout->addWidget(recursiveViewButton_);
    catalog_tools_layout->addWidget(refreshButton_);
    tools->addWidget(catalog_tools);
    connect(recursiveViewButton_, &QToolButton::clicked, this, &MainWindow::set_recursive_view);
    connect(refreshButton_, &QToolButton::clicked, this, &MainWindow::refresh_catalog);
    tools->addWidget(toolbar_separator(top_bar));
    tools->addWidget(logoButton_);
    windowControls_ = new WindowControls(this, top_bar);
    title_layout->addWidget(toolbar_content, 1);
    title_layout->addWidget(toolbar_separator(top_bar));
    title_layout->addWidget(windowControls_);
    new WindowFrame(this, top_bar, windowControls_->maximize_button());
    root->addWidget(top_bar);

    mainSplitter_ = new QSplitter(Qt::Horizontal, central);
    mainSplitter_->setObjectName(QStringLiteral("mainSplitter"));
    mainSplitter_->setChildrenCollapsible(false);
    mainSplitter_->setHandleWidth(1);

    auto *catalog_view = new CatalogView(mainSplitter_);
    listView_ = catalog_view;
    listView_->verticalScrollBar()->setProperty("voveOuterRailShared", true);
    listView_->setObjectName(QStringLiteral("catalogGrid"));
    listView_->setModel(&listModel_);
    listView_->setItemDelegate(&delegate_);
    listView_->setViewMode(QListView::IconMode);
    listView_->setFlow(QListView::LeftToRight);
    listView_->setWrapping(true);
    listView_->setResizeMode(QListView::Adjust);
    listView_->setMovement(QListView::Static);
    listView_->setUniformItemSizes(true);
    listView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    listView_->setDragEnabled(true);
    listView_->setAcceptDrops(true);
    listView_->viewport()->setAcceptDrops(true);
    listView_->setDragDropMode(QAbstractItemView::DragDrop);
    listView_->setDefaultDropAction(Qt::CopyAction);
    listView_->viewport()->installEventFilter(this);
    listView_->installEventFilter(this);
    listView_->verticalScrollBar()->installEventFilter(this);
    listView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    listView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listView_->setSpacing(8);
    listView_->setContextMenuPolicy(Qt::CustomContextMenu);
    catalog_view->set_item_hit_test([this](const QModelIndex &index, const QPoint &point) {
        QStyleOptionViewItem option;
        option.initFrom(listView_);
        option.rect = listView_->visualRect(index);
        return delegate_.item_hit_region(option, index).contains(point);
    });
    globalSearchMessage_ = new QLabel(listView_->viewport());
    globalSearchMessage_->setObjectName(QStringLiteral("globalSearchMessage"));
    globalSearchMessage_->setAlignment(Qt::AlignCenter);
    globalSearchMessage_->setWordWrap(true);
    globalSearchMessage_->setTextFormat(Qt::RichText);
    globalSearchMessage_->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                                  Qt::TextSelectableByMouse);
    globalSearchMessage_->setOpenExternalLinks(true);
    globalSearchMessage_->hide();

    renameAction_ = new QAction(QCoreApplication::translate("MainWindow", "Rename"), this);
    renameAction_->setObjectName(QStringLiteral("renameAction"));
    renameAction_->setShortcut(QKeySequence(Qt::Key_F2));
    renameAction_->setShortcutVisibleInContextMenu(true);
    renameAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    renameAction_->setEnabled(false);
    listView_->addAction(renameAction_);
    catalog_view->set_rename_action(renameAction_,
                                    [this](const QModelIndex &index, const QPoint &point) {
                                        QStyleOptionViewItem option;
                                        option.initFrom(listView_);
                                        option.rect = listView_->visualRect(index);
                                        return delegate_.name_rect(option, index).contains(point);
                                    });
    deleteAction_ = new QAction(QCoreApplication::translate("MainWindow", "Delete…"), this);
    deleteAction_->setObjectName(QStringLiteral("deleteAction"));
    deleteAction_->setShortcut(QKeySequence(Qt::Key_Delete));
    deleteAction_->setShortcutVisibleInContextMenu(true);
    deleteAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    deleteAction_->setEnabled(false);
    listView_->addAction(deleteAction_);
    copyNamesAction_ = new QAction(QCoreApplication::translate("MainWindow", "Copy name"), this);
    copyNamesAction_->setObjectName(QStringLiteral("copyNamesAction"));
    cancelPreviewAttemptAction_ =
        new QAction(QCoreApplication::translate("MainWindow", "Cancel preview attempt"), this);
    cancelPreviewAttemptAction_->setObjectName(QStringLiteral("cancelPreviewAttemptAction"));
    cancelPreviewAttemptAction_->setShortcut(QKeySequence(Qt::Key_Escape));
    cancelPreviewAttemptAction_->setShortcutVisibleInContextMenu(true);
    cancelPreviewAttemptAction_->setEnabled(false);
    addAction(cancelPreviewAttemptAction_);
    connect(cancelPreviewAttemptAction_, &QAction::triggered, this,
            &MainWindow::cancel_forced_preview);
    copyPathsAction_ =
        new QAction(QCoreApplication::translate("MainWindow", "Copy full path"), this);
    copyPathsAction_->setObjectName(QStringLiteral("copyPathsAction"));
    copyPathsAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C));
    copyPathsAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    copyPathsAction_->setShortcutVisibleInContextMenu(true);
    copyPathsAction_->setEnabled(false);
    listView_->addAction(copyPathsAction_);
    copyObjectsAction_ = new QAction(QCoreApplication::translate("MainWindow", "Copy"), this);
    copyObjectsAction_->setObjectName(QStringLiteral("copyObjectsAction"));
    copyObjectsAction_->setShortcut(QKeySequence::Copy);
    copyObjectsAction_->setShortcutVisibleInContextMenu(true);
    copyObjectsAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    copyObjectsAction_->setEnabled(false);
    listView_->addAction(copyObjectsAction_);
    pasteObjectsAction_ = new QAction(QCoreApplication::translate("MainWindow", "Paste"), this);
    pasteObjectsAction_->setObjectName(QStringLiteral("pasteObjectsAction"));
    pasteObjectsAction_->setShortcut(QKeySequence::Paste);
    pasteObjectsAction_->setShortcutVisibleInContextMenu(true);
    pasteObjectsAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    pasteObjectsAction_->setEnabled(false);
    listView_->addAction(pasteObjectsAction_);

    auto *right_column = new QWidget(mainSplitter_);
    right_column->setObjectName(QStringLiteral("rightColumn"));
    auto *right_layout = new QVBoxLayout(right_column);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);

    auto *right_top = new QWidget(right_column);
    right_top->setObjectName(QStringLiteral("rightTop"));
    auto *right_top_layout = new QVBoxLayout(right_top);
    right_top_layout->setContentsMargins(0, 0, 0, 0);
    right_top_layout->setSpacing(0);

    placesSplitter_ = new QSplitter(Qt::Horizontal, right_top);
    placesSplitter_->setObjectName(QStringLiteral("placesSplitter"));
    placesSplitter_->setChildrenCollapsible(false);
    placesSplitter_->setHandleWidth(1);

    auto *favorites_panel = new QWidget(placesSplitter_);
    auto *favorites_layout = new QVBoxLayout(favorites_panel);
    favorites_layout->setContentsMargins(0, 0, 0, 0);
    favorites_layout->setSpacing(0);
    favoritesHeader_ = new QFrame(favorites_panel);
    favoritesHeader_->setObjectName(QStringLiteral("favoritesHeader"));
    favoritesHeader_->setAcceptDrops(true);
    favoritesHeader_->installEventFilter(this);
    auto *favorites_header_layout = new QHBoxLayout(favoritesHeader_);
    favorites_header_layout->setContentsMargins(8, 3, 8, 4);
    favoritesTitle_ =
        new QLabel(QCoreApplication::translate("MainWindow", "Favorites"), favorites_panel);
    favoritesTitle_->setObjectName(QStringLiteral("sectionTitle"));
    favoritesTitle_->setAttribute(Qt::WA_TransparentForMouseEvents);
    favorites_header_layout->addWidget(favoritesTitle_);
    favorites_header_layout->addStretch(1);
    addFavoriteButton_ = new AddFavoriteButton(favoritesHeader_);
    addFavoriteButton_->setObjectName(QStringLiteral("addFavoriteButton"));
    favorites_header_layout->addWidget(addFavoriteButton_);
    auto *favorites_body = new QWidget(favorites_panel);
    favorites_body->setObjectName(QStringLiteral("favoritesBody"));
    auto *favorites_body_layout = new QHBoxLayout(favorites_body);
    favorites_body_layout->setContentsMargins(0, 0, 0, 0);
    favorites_body_layout->setSpacing(0);
    auto *favorites_stack = new QWidget(favorites_body);
    auto *favorites_stack_layout = new QVBoxLayout(favorites_stack);
    favorites_stack_layout->setContentsMargins(0, 0, 0, 0);
    favorites_stack_layout->setSpacing(0);

    favorites_ = new FavoritesView(favorites_stack);
    favorites_->setObjectName(QStringLiteral("favoritesView"));
    favorites_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    favorites_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    favorites_->setContextMenuPolicy(Qt::CustomContextMenu);
    favorites_->setAcceptDrops(true);
    favorites_->viewport()->setAcceptDrops(true);
    favorites_->setDragDropMode(QAbstractItemView::DropOnly);
    favorites_->setDefaultDropAction(Qt::CopyAction);
    favorites_->viewport()->installEventFilter(this);
    trashFavoriteDock_ = new QFrame(favorites_stack);
    trashFavoriteDock_->setObjectName(QStringLiteral("trashFavoriteDock"));
    auto *trash_layout = new QVBoxLayout(trashFavoriteDock_);
    trash_layout->setContentsMargins(0, 5, 0, 0);
    trash_layout->setSpacing(0);
    trashFavorite_ = new FavoritesView(trashFavoriteDock_);
    trashFavorite_->setObjectName(QStringLiteral("trashFavoriteView"));
    trashFavorite_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    trashFavorite_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    trashFavorite_->setContextMenuPolicy(Qt::CustomContextMenu);
    trashFavorite_->setAcceptDrops(true);
    trashFavorite_->viewport()->setAcceptDrops(true);
    trashFavorite_->setDragDropMode(QAbstractItemView::DropOnly);
    trashFavorite_->setDefaultDropAction(Qt::CopyAction);
    trashFavorite_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    trashFavorite_->viewport()->installEventFilter(this);
    trash_layout->addWidget(trashFavorite_);
    favorites_stack_layout->addWidget(favorites_, 1);
    favorites_stack_layout->addWidget(trashFavoriteDock_);

    favoritesScrollBar_ = new QScrollBar(Qt::Vertical, favorites_body);
    favoritesScrollBar_->setObjectName(QStringLiteral("favoritesScrollBar"));
    favoritesScrollBar_->setProperty("voveOuterRailShared", true);
    favoritesScrollBar_->setFocusPolicy(Qt::NoFocus);
    favoritesScrollBar_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    auto *favorites_internal_scroll = favorites_->verticalScrollBar();
    const auto sync_favorites_scrollbar = [this, favorites_internal_scroll](const int minimum,
                                                                            const int maximum) {
        favoritesScrollBar_->setRange(minimum, maximum);
        favoritesScrollBar_->setSingleStep(favorites_internal_scroll->singleStep());
        favoritesScrollBar_->setPageStep(favorites_internal_scroll->pageStep());
        favoritesScrollBar_->setValue(favorites_internal_scroll->value());
    };
    connect(favorites_internal_scroll, &QScrollBar::rangeChanged, favoritesScrollBar_,
            sync_favorites_scrollbar);
    connect(favorites_internal_scroll, &QScrollBar::valueChanged, favoritesScrollBar_,
            &QScrollBar::setValue);
    connect(favoritesScrollBar_, &QScrollBar::valueChanged, favorites_internal_scroll,
            &QScrollBar::setValue);
    sync_favorites_scrollbar(favorites_internal_scroll->minimum(),
                             favorites_internal_scroll->maximum());

    favorites_body_layout->addWidget(favorites_stack, 1);
    favorites_body_layout->addWidget(favoritesScrollBar_);
    favorites_layout->addWidget(favoritesHeader_);
    favorites_layout->addWidget(favorites_body, 1);

    auto *tree_panel = new QWidget(placesSplitter_);
    auto *tree_layout = new QVBoxLayout(tree_panel);
    tree_layout->setContentsMargins(0, 0, 0, 0);
    tree_layout->setSpacing(0);
    directoryHeader_ = new QFrame(tree_panel);
    directoryHeader_->setObjectName(QStringLiteral("directoryHeader"));
    auto *directory_header_layout = new QHBoxLayout(directoryHeader_);
    const auto scroll_extent = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    directory_header_layout->setContentsMargins(scroll_extent, 3, scroll_extent, 4);
    pathEdit_ = new AddressField(directoryHeader_);
    pathEdit_->setObjectName(QStringLiteral("pathEdit"));
    pathEdit_->setClearButtonEnabled(false);
    pathEdit_->setPlaceholderText(
        QCoreApplication::translate("MainWindow", "Local or network path"));

    chooseFolderAction_ = pathEdit_->addAction(QIcon(), QLineEdit::TrailingPosition);
    chooseFolderAction_->setObjectName(QStringLiteral("chooseFolderAction"));
    directory_header_layout->addWidget(pathEdit_);

    treeModel_ = new LazyDirectoryTreeModel(treeSource_, this);
    treeModel_->add_roots(LazyDirectoryTreeModel::detected_roots());
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state == Qt::ApplicationActive && treeModel_ != nullptr) {
                    treeModel_->add_roots(LazyDirectoryTreeModel::detected_roots());
                    directoryMonitor_.request_recheck();
                }
            });
    directoryTree_ = new DirectoryTreeView(tree_panel);
    directoryTree_->setObjectName(QStringLiteral("directoryTree"));
    directoryTree_->setModel(treeModel_);
    directoryTree_->setHeaderHidden(true);
    directoryTree_->setAnimated(false);
    directoryTree_->setUniformRowHeights(true);
    directoryTree_->setIndentation(scroll_extent);
    directoryTree_->setDragEnabled(true);
    directoryTree_->setAcceptDrops(true);
    directoryTree_->viewport()->setAcceptDrops(true);
    directoryTree_->setDragDropMode(QAbstractItemView::DragDrop);
    directoryTree_->setDefaultDropAction(Qt::CopyAction);
    directoryTree_->viewport()->installEventFilter(this);
    directoryTree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    directoryTree_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    tree_layout->addWidget(directoryHeader_);
    tree_layout->addWidget(directoryTree_, 1);

    right_top_layout->addWidget(placesSplitter_);

    install_smooth_scroll(*listView_, 180);
    install_smooth_scroll(*favorites_);
    install_smooth_scroll(*directoryTree_);
    // Observe input before smooth scrolling consumes wheel events.
    listView_->installEventFilter(this);
    listView_->viewport()->installEventFilter(this);
    listView_->verticalScrollBar()->installEventFilter(this);

    previewPanel_ = new QWidget(right_column);
    previewPanel_->setObjectName(QStringLiteral("previewPanel"));
    previewPanel_->setAcceptDrops(true);
    previewPanel_->installEventFilter(this);
    auto *preview_layout = new QVBoxLayout(previewPanel_);
    preview_layout->setContentsMargins(16, 12, 16, 12);
    preview_layout->setSpacing(8);
    previewCanvas_ =
        new ZoomPreview(QCoreApplication::translate("MainWindow", "Select a file"), previewPanel_);
    previewCanvas_->setObjectName(QStringLiteral("previewHint"));
    previewCanvas_->setAlignment(Qt::AlignCenter);
    previewCanvas_->setWordWrap(true);
    previewCanvas_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    previewCanvas_->setMinimumHeight(180);
    previewCanvas_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    previewCanvas_->setAcceptDrops(true);
    previewCanvas_->installEventFilter(this);

    pageNavigation_ = new QWidget(previewPanel_);
    pageNavigation_->setObjectName(QStringLiteral("pageNavigation"));
    auto *page_navigation_layout = new QHBoxLayout(pageNavigation_);
    page_navigation_layout->setContentsMargins(0, 0, 0, 0);
    page_navigation_layout->setSpacing(4);
    previousPageButton_ =
        new PageButton(PageDirection::previous,
                       QCoreApplication::translate("MainWindow", "Previous page"), pageNavigation_);
    previousPageButton_->setObjectName(QStringLiteral("previousPage"));
    nextPageButton_ =
        new PageButton(PageDirection::next, QCoreApplication::translate("MainWindow", "Next page"),
                       pageNavigation_);
    nextPageButton_->setObjectName(QStringLiteral("nextPage"));
    pageIndicator_ = new QLabel(pageNavigation_);
    pageIndicator_->setObjectName(QStringLiteral("pageIndicator"));
    pageIndicator_->setAlignment(Qt::AlignCenter);
    pageIndicator_->setMinimumWidth(
        pageIndicator_->fontMetrics().horizontalAdvance(QStringLiteral("0000 / 0000")));
    page_navigation_layout->addStretch(1);
    page_navigation_layout->addWidget(previousPageButton_);
    page_navigation_layout->addWidget(pageIndicator_);
    page_navigation_layout->addWidget(nextPageButton_);
    page_navigation_layout->addStretch(1);
    pageNavigation_->hide();

    passwordButton_ = new QToolButton(previewPanel_);
    passwordButton_->setObjectName(QStringLiteral("documentPassword"));
    passwordButton_->setText(QCoreApplication::translate("MainWindow", "Enter password"));
    passwordButton_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    passwordButton_->setAccessibleName(
        QCoreApplication::translate("MainWindow", "Enter document password"));
    passwordButton_->hide();

    auto *preview_caption = new QWidget(previewPanel_);
    preview_caption->setObjectName(QStringLiteral("previewCaption"));
    preview_caption->setLayoutDirection(Qt::LeftToRight);
    auto *preview_footer = new QHBoxLayout(preview_caption);
    preview_footer->setContentsMargins(0, 0, 0, 0);
    preview_footer->setSpacing(6);
    previewName_ = new PreviewNameLabel(preview_caption);
    previewFormat_ = new QLabel(preview_caption);
    previewFormat_->setObjectName(QStringLiteral("previewFormat"));
    previewFormat_->setTextFormat(Qt::PlainText);
    previewFormat_->setAlignment(Qt::AlignRight | Qt::AlignAbsolute | Qt::AlignBottom);
    previewFormat_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    previewFormat_->hide();
    previewSupport_ = new QLabel(previewPanel_);
    previewSupport_->setObjectName(QStringLiteral("previewSupport"));
    previewSupport_->setWordWrap(true);
    previewSupport_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    previewSupport_->hide();
    preview_footer->addWidget(previewName_, 1, Qt::AlignBottom);
    preview_footer->addWidget(previewFormat_);
    preview_layout->addWidget(pageNavigation_);
    preview_layout->addWidget(previewCanvas_, 1);
    preview_layout->addWidget(passwordButton_, 0, Qt::AlignHCenter);
    preview_layout->addWidget(preview_caption);
    preview_layout->addWidget(previewSupport_);
    previewModified_ = new PreviewModifiedLabel(previewPanel_);
    previewModified_->installEventFilter(this);
    preview_layout->addWidget(previewModified_);

    right_layout->addWidget(right_top, 1);
    auto *navigation_separator = new QFrame(right_column);
    navigation_separator->setObjectName(QStringLiteral("navigationPreviewSeparator"));
    navigation_separator->setFixedHeight(1);
    right_layout->addWidget(navigation_separator);
    right_layout->addWidget(previewPanel_, 3);

    mainSplitter_->addWidget(listView_);
    mainSplitter_->addWidget(right_column);
    mainSplitter_->setStretchFactor(0, 3);
    mainSplitter_->setStretchFactor(1, 2);
    placesSplitter_->setStretchFactor(0, 3);
    placesSplitter_->setStretchFactor(1, 7);
    root->addWidget(mainSplitter_, 1);

    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("catalogStatus"));
    status_->setMinimumWidth(240);
    status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(status_, 1);

    operationStatusPulseTimer_ = new QTimer(this);
    operationStatusPulseTimer_->setInterval(80);
    operationStatusPulseTimer_->setTimerType(Qt::CoarseTimer);
    connect(operationStatusPulseTimer_, &QTimer::timeout, this, [this] {
        operationStatusPulsePhase_ = (operationStatusPulsePhase_ + 1) % 30;
        apply_operation_status_color();
    });

    themeButtons_ = new QButtonGroup(this);
    statusBar()->addPermanentWidget(create_theme_switch(themeButtons_, this));
    retranslate_shell();
    setCentralWidget(central);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(16);
    connect(pollTimer_, &QTimer::timeout, this, &MainWindow::poll_catalog);
    previewTimer_ = new QTimer(this);
    previewTimer_->setObjectName(QStringLiteral("previewDispatchTimer"));
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(40);
    connect(previewTimer_, &QTimer::timeout, this, &MainWindow::schedule_visible_previews);
    settingsTimer_ = new QTimer(this);
    settingsTimer_->setSingleShot(true);
    settingsTimer_->setInterval(250);
    connect(settingsTimer_, &QTimer::timeout, this, &MainWindow::save_settings);
    globalSearchTimer_ = new QTimer(this);
    globalSearchTimer_->setSingleShot(true);
    globalSearchTimer_->setInterval(250);
    connect(globalSearchTimer_, &QTimer::timeout, this, &MainWindow::start_global_search);
    transferProbeTimer_ = new QTimer(this);
    transferProbeTimer_->setInterval(16);
    connect(transferProbeTimer_, &QTimer::timeout, this, &MainWindow::poll_transfer_preparation);
    dropProbeTimer_ = new QTimer(this);
    dropProbeTimer_->setInterval(16);
    connect(dropProbeTimer_, &QTimer::timeout, this, &MainWindow::poll_external_drop);
    trashCatalogTimer_ = new QTimer(this);
    trashCatalogTimer_->setInterval(25);
    connect(trashCatalogTimer_, &QTimer::timeout, this, &MainWindow::poll_trash_catalog_loaders);
    previewClient_.set_reply_handler(
        [this](PreviewReply reply) { handle_preview_reply(std::move(reply)); });
    folderMosaicController_.set_reply_handler(
        [this](FolderMosaicReply reply) { handle_folder_mosaic(std::move(reply)); });
    globalSearchClient_.set_reply_handler(
        [this](search::SearchBatch batch) { handle_global_search_batch(std::move(batch)); });
    previewClient_.refresh_external_components([this](const bool changed) {
        if (changed) {
            retry_external_component_previews();
        }
    });

    connect(backButton_, &QToolButton::clicked, this, &MainWindow::go_back);
    connect(forwardButton_, &QToolButton::clicked, this, &MainWindow::go_forward);
    connect(historyMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuild_history_menu);
    connect(upButton_, &QToolButton::clicked, this, &MainWindow::go_up);
    connect(createFolderAction_, &QAction::triggered, this, &MainWindow::prompt_create_directory);
    connect(settingsAction_, &QAction::triggered, this, &MainWindow::show_settings_dialog);
    connect(updateAction_, &QAction::triggered, this, &MainWindow::show_update_dialog);
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::show_about_dialog);
    connect(componentsAction_, &QAction::triggered, this, &MainWindow::show_components_dialog);
    connect(donateAction_, &QAction::triggered, this, [this] { show_contact_dialog(true); });
    connect(reportIssueAction_, &QAction::triggered, this, [this] { show_contact_dialog(false); });
    connect(pathEdit_, &QLineEdit::returnPressed, this,
            [this] { open_path(pathEdit_->text(), true); });
    connect(chooseFolderAction_, &QAction::triggered, this, &MainWindow::choose_directory);
    connect(addFavoriteButton_, &QToolButton::clicked, this, &MainWindow::add_current_favorite);
    connect(filterEdit_, &QLineEdit::textChanged, this, &MainWindow::apply_filter);
    connect(filterEdit_, &QLineEdit::textChanged, this, [this] { schedule_settings_save(); });
    connect(globalSearchEdit_, &QLineEdit::textChanged, this, &MainWindow::schedule_global_search);
    connect(globalSearchEdit_, &QLineEdit::returnPressed, this, [this] {
        globalSearchTimer_->stop();
        start_global_search();
    });
    connect(sortField_, &QComboBox::currentIndexChanged, this, &MainWindow::apply_sort);
    connect(sortField_, &QComboBox::currentIndexChanged, this,
            [this] { schedule_settings_save(); });
    connect(sortDirection_, &QToolButton::toggled, this, &MainWindow::apply_sort);
    connect(sortDirection_, &QToolButton::toggled, this, [this] { schedule_settings_save(); });
    connect(sizeSlider_, &QSlider::valueChanged, this, &MainWindow::apply_thumbnail_extent);
    connect(sizeSlider_, &QSlider::valueChanged, this, [this] { schedule_settings_save(); });
    connect(smallerThumbnailsButton_, &QToolButton::clicked, this,
            [this] { sizeSlider_->setValue(sizeSlider_->value() - sizeSlider_->pageStep()); });
    connect(largerThumbnailsButton_, &QToolButton::clicked, this,
            [this] { sizeSlider_->setValue(sizeSlider_->value() + sizeSlider_->pageStep()); });
    connect(previousPageButton_, &QToolButton::clicked, this, [this] {
        if (selectedPageIndex_ > 0) {
            request_selected_page(selectedPageIndex_ - 1U);
        }
    });
    connect(nextPageButton_, &QToolButton::clicked, this, [this] {
        if (selectedPageIndex_ + 1U < selectedPageCount_) {
            request_selected_page(selectedPageIndex_ + 1U);
        }
    });
    connect(passwordButton_, &QToolButton::clicked, this,
            &MainWindow::prompt_for_document_password);
    const auto open_favorite = [this](QListWidgetItem *item) {
        if (is_trash_favorite(item)) {
            show_trash();
            return;
        }
        const auto path = item->data(Qt::UserRole).toString();
        if (trashViewActive_ || globalSearchActive_ || recursiveViewActive_ ||
            !same_directory_path(path, currentPath_)) {
            open_path(path, true);
        } else {
            update_location_selection();
        }
    };
    connect(favorites_, &QListWidget::itemClicked, this, open_favorite);
    connect(favorites_, &QListWidget::itemActivated, this, open_favorite);
    connect(trashFavorite_, &QListWidget::itemClicked, this, open_favorite);
    connect(trashFavorite_, &QListWidget::itemActivated, this, open_favorite);
    connect(
        favorites_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
            auto *item = favorites_->itemAt(position);
            if (is_trash_favorite(item)) {
                favorites_->setCurrentItem(item);
                QMenu menu(favorites_);
                auto *clear =
                    menu.addAction(QCoreApplication::translate("MainWindow", "Empty Trash…"));
                clear->setObjectName(QStringLiteral("emptyTrashAction"));
                clear->setEnabled(!deleteInFlight_ && !trashCoordinator_.busy());
                if (menu.exec(favorites_->viewport()->mapToGlobal(position)) == clear) {
                    show_trash_cleanup();
                }
                return;
            }
            QMenu menu(favorites_);
            if (item != nullptr) {
                favorites_->setCurrentItem(item);
            }
            auto *add =
                menu.addAction(QCoreApplication::translate("MainWindow", "Add current folder"));
            auto *rename =
                menu.addAction(QCoreApplication::translate("MainWindow", "Rename favorite"));
            rename->setObjectName(QStringLiteral("renameFavoriteAction"));
            menu.addSeparator();
            auto *move_up = menu.addAction(QCoreApplication::translate("MainWindow", "Move up"));
            move_up->setObjectName(QStringLiteral("moveFavoriteUpAction"));
            auto *move_down =
                menu.addAction(QCoreApplication::translate("MainWindow", "Move down"));
            move_down->setObjectName(QStringLiteral("moveFavoriteDownAction"));
            menu.addSeparator();
            auto *remove =
                menu.addAction(QCoreApplication::translate("MainWindow", "Remove from Favorites"));
            remove->setObjectName(QStringLiteral("removeFavoriteAction"));
            const auto row = item != nullptr ? favorites_->row(item) : -1;
            rename->setEnabled(item != nullptr);
            move_up->setEnabled(row > 0 && !is_trash_favorite(favorites_->item(row - 1)));
            move_down->setEnabled(row >= 0 && row + 1 < favorites_->count() &&
                                  !is_trash_favorite(favorites_->item(row + 1)));
            remove->setEnabled(item != nullptr);
            const auto *chosen = menu.exec(favorites_->viewport()->mapToGlobal(position));
            if (chosen == add) {
                add_current_favorite();
            } else if (chosen == rename) {
                rename_selected_favorite();
            } else if (chosen == move_up) {
                move_selected_favorite(-1);
            } else if (chosen == move_down) {
                move_selected_favorite(1);
            } else if (chosen == remove) {
                remove_selected_favorite();
            }
        });
    connect(trashFavorite_, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                auto *item = trashFavorite_->itemAt(position);
                if (!is_trash_favorite(item)) {
                    return;
                }
                trashFavorite_->setCurrentItem(item);
                QMenu menu(trashFavorite_);
                auto *clear =
                    menu.addAction(QCoreApplication::translate("MainWindow", "Empty Trash…"));
                clear->setObjectName(QStringLiteral("emptyTrashAction"));
                clear->setEnabled(!deleteInFlight_ && !trashCoordinator_.busy());
                if (menu.exec(trashFavorite_->viewport()->mapToGlobal(position)) == clear) {
                    show_trash_cleanup();
                }
            });
    const auto open_tree_location = [this](const QModelIndex &index) {
        const auto path = index.data(treePathRole).toString();
        if (!path.isEmpty() && !same_directory_path(path, currentPath_)) {
            open_path(path, true);
        } else {
            update_location_selection();
        }
    };
    connect(directoryTree_, &QTreeView::clicked, this, open_tree_location);
    connect(directoryTree_, &QTreeView::activated, this, open_tree_location);
    connect(directoryTree_, &QTreeView::expanded, this,
            [this](const QModelIndex &index) { treeModel_->load_children(index); });
    connect(directoryTree_, &QTreeView::collapsed, this,
            [this](const QModelIndex &index) { treeModel_->cancel(index); });
    connect(treeModel_, &QAbstractItemModel::rowsAboutToBeInserted, this, [this] {
        if (std::exchange(treeInsertBatchPending_, true)) {
            return;
        }
        const QPersistentModelIndex current(directoryTree_->currentIndex());
        const auto row = directoryTree_->visualRect(current);
        treeRevealCurrentAfterInsert_ =
            !row.isEmpty() && directoryTree_->viewport()->rect().intersects(row);
        QTimer::singleShot(0, this, [this, current] {
            treeInsertBatchPending_ = false;
            if (!std::exchange(treeRevealCurrentAfterInsert_, false) ||
                current != directoryTree_->currentIndex()) {
                return;
            }
            for (auto parent = current.parent(); parent.isValid(); parent = parent.parent()) {
                if (!directoryTree_->isExpanded(parent)) {
                    return;
                }
            }
            // Preserve the visible location only after Qt finishes inserting the batch.
            directoryTree_->scrollTo(current);
        });
    });
    connect(directoryTree_->verticalScrollBar(), &QScrollBar::actionTriggered, this,
            [this] { treeRevealCurrentAfterInsert_ = false; });
    connect(listView_, &QListView::doubleClicked, this, [this](const QModelIndex &index) {
        if (trashViewActive_) {
            return;
        }
        const auto *entry = listModel_.entry_at(index);
        if (entry != nullptr) {
            open_entry(*entry);
        }
    });
    connect(listView_, &QListView::customContextMenuRequested, this,
            &MainWindow::show_catalog_context_menu);
    connect(renameAction_, &QAction::triggered, this, &MainWindow::prompt_rename_selected);
    connect(deleteAction_, &QAction::triggered, this, &MainWindow::prompt_delete_selected);
    connect(trashAction_, &QAction::triggered, this, &MainWindow::show_trash);
    connect(copyNamesAction_, &QAction::triggered, this, &MainWindow::copy_selected_names);
    connect(copyPathsAction_, &QAction::triggered, this, &MainWindow::copy_selected_paths);
    connect(copyObjectsAction_, &QAction::triggered, this, &MainWindow::copy_selected_objects);
    connect(pasteObjectsAction_, &QAction::triggered, this, &MainWindow::paste_objects);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this,
            &MainWindow::update_clipboard_actions);
    directoryTree_->addAction(pasteObjectsAction_);
    favorites_->addAction(pasteObjectsAction_);
    connect(listView_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current, const QModelIndex &) { show_selection(current); });
    connect(listView_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
        update_rename_action();
        update_status(lastUpdate_);
    });
    connect(listView_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!previewTimer_->isActive()) {
            previewTimer_->start();
        }
    });
    // Preserve the dispatch deadline while catalog batches keep changing the model.
    connect(&listModel_, &QAbstractItemModel::rowsInserted, this,
            [this] { if (!previewTimer_->isActive()) previewTimer_->start(); });
    connect(&listModel_, &QAbstractItemModel::modelReset, this, [this] {
        previewModified_->set_modified_time(std::nullopt);
        if (!previewTimer_->isActive()) previewTimer_->start();
    });
    connect(&listModel_, &QAbstractItemModel::dataChanged, this,
            [this] { if (!previewTimer_->isActive()) previewTimer_->start(); });
    connect(&listModel_, &QAbstractItemModel::layoutChanged, this,
            [this] { if (!previewTimer_->isActive()) previewTimer_->start(); });
    connect(themeButtons_, &QButtonGroup::idClicked, this, [this](const int id) {
        apply_theme(static_cast<AppTheme>(id));
        schedule_settings_save();
    });
    connect(mainSplitter_, &QSplitter::splitterMoved, this, [this] {
        mainShare_ = splitter_share(mainSplitter_, mainShare_);
        schedule_settings_save();
    });
    connect(placesSplitter_, &QSplitter::splitterMoved, this, [this] {
        favoritesShare_ = splitter_share(placesSplitter_, favoritesShare_);
        schedule_settings_save();
    });

    auto *refresh_shortcut = new QShortcut(QKeySequence::Refresh, this);
    refresh_shortcut->setObjectName(QStringLiteral("refreshShortcut"));
    connect(refresh_shortcut, &QShortcut::activated, this, &MainWindow::refresh_catalog);

    const auto install_shortcut = [this](const char *name, const QKeySequence &keys,
                                         auto &&handler) {
        auto *shortcut = new QShortcut(keys, this);
        shortcut->setObjectName(QString::fromLatin1(name));
        shortcut->setContext(Qt::WindowShortcut);
        connect(shortcut, &QShortcut::activated, this, std::forward<decltype(handler)>(handler));
    };
    install_shortcut("focusPathShortcut", QKeySequence(Qt::CTRL | Qt::Key_L), [this] {
        pathEdit_->setFocus(Qt::ShortcutFocusReason);
        pathEdit_->selectAll();
    });
    install_shortcut("focusFilterShortcut", QKeySequence::Find, [this] {
        filterEdit_->setFocus(Qt::ShortcutFocusReason);
        filterEdit_->selectAll();
    });
    install_shortcut("focusGlobalSearchShortcut", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F),
                     [this] {
                         globalSearchEdit_->setFocus(Qt::ShortcutFocusReason);
                         globalSearchEdit_->selectAll();
                     });
    install_shortcut("backShortcut", QKeySequence(Qt::ALT | Qt::Key_Left), [this] { go_back(); });
    install_shortcut("forwardShortcut", QKeySequence(Qt::ALT | Qt::Key_Right),
                     [this] { go_forward(); });
    install_shortcut("upShortcut", QKeySequence(Qt::ALT | Qt::Key_Up), [this] { go_up(); });
    install_shortcut("smallerThumbnailsShortcut", QKeySequence::ZoomOut, [this] {
        sizeSlider_->setValue(sizeSlider_->value() - sizeSlider_->pageStep());
    });
    install_shortcut("largerThumbnailsShortcut", QKeySequence::ZoomIn, [this] {
        sizeSlider_->setValue(sizeSlider_->value() + sizeSlider_->pageStep());
    });
    install_shortcut("mainMenuShortcut", QKeySequence(Qt::Key_F10), [this] {
        logoButton_->setFocus(Qt::ShortcutFocusReason);
        logoButton_->showMenu();
    });
    apply_thumbnail_extent(sizeSlider_->value());
    apply_splitter_ratios();
    update_navigation();
}

void MainWindow::open_external_path(const QString &path, const bool select_file) {
    if (select_file) {
        open_external_paths({path});
        return;
    }
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
    if (path.isEmpty()) {
        return;
    }
    open_path(path, true);
}

void MainWindow::open_external_paths(const QStringList &paths) {
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
    if (paths.isEmpty()) {
        return;
    }

    QStringList normalized_paths;
    QSet<QString> seen;
    for (const auto &path : paths) {
        auto normalized =
            QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());
#ifdef Q_OS_WIN
        const auto key = normalized.toCaseFolded();
#else
        const auto &key = normalized;
#endif
        if (!normalized.isEmpty() && !seen.contains(key)) {
            seen.insert(key);
            normalized_paths.push_back(std::move(normalized));
        }
    }
    if (normalized_paths.isEmpty()) {
        return;
    }

    if (pendingExternalDirectoryDrop_ || pendingExternalTransferDrop_ ||
        pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        queuedExternalOpenPaths_ = std::move(normalized_paths);
        if (pendingExternalDirectoryDrop_ &&
            pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open) {
            dropProbeSource_.cancel(pendingExternalDirectoryDrop_->generation);
        }
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Wait for the current dragged-object check to finish");
        update_status(lastUpdate_);
        return;
    }

    if (normalized_paths.size() == 1) {
        if (!begin_external_directory_drop(ExternalDirectoryDropIntent::open, normalized_paths)) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Wait for the current dragged-object check to finish");
            update_status(lastUpdate_);
        }
        return;
    }

    open_external_items_in_parent(normalized_paths);
}

void MainWindow::open_external_items_in_parent(const QStringList &paths) {
    if (paths.isEmpty()) {
        return;
    }
    const auto parent = QFileInfo(paths.front()).absolutePath();
    if (parent.isEmpty()) {
        return;
    }
#ifdef Q_OS_WIN
    constexpr auto path_case = Qt::CaseInsensitive;
#else
    constexpr auto path_case = Qt::CaseSensitive;
#endif
    filterEdit_->clear();
    pendingRenamePaths_.clear();
    for (const auto &path : paths) {
        if (QFileInfo(path).absolutePath().compare(parent, path_case) == 0) {
            pendingRenamePaths_.push_back(path);
        }
    }
    if (pendingRenamePaths_.isEmpty()) {
        return;
    }
    pendingCatalogReveal_ = true;
    if (QDir::cleanPath(currentPath_).compare(QDir::cleanPath(parent), path_case) != 0 ||
        globalSearchActive_ || recursiveViewActive_) {
        open_path(parent, true);
    } else if (lastUpdate_.state == catalog::CatalogSessionState::ready) {
        restore_pending_rename_selection();
    }
}

void MainWindow::open_entry(const core::DirectoryEntry &entry) {
    const auto path = QString::fromUtf8(entry.path_utf8);
    if (entry.kind == core::EntryKind::directory) {
        open_path(path, true);
        return;
    }
    open_file_in_default_application(path);
}

void MainWindow::open_file_in_default_application(const QString &path) {
#ifdef VOVE_UI_TEST_HOOKS
    if (property("captureExternalOpen").toBool()) {
        setProperty("lastExternalOpenKind", QStringLiteral("default"));
        setProperty("lastExternalOpenPath", QDir::cleanPath(path));
    }
#endif
    externalOpenCoordinator_.open(path, [this](const ExternalOpenResult result) {
        switch (result) {
        case ExternalOpenResult::opened:
            operationStatus_.clear();
            break;
        case ExternalOpenResult::timed_out:
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Opening the file timed out");
            break;
        case ExternalOpenResult::helper_unavailable:
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "The file opening component is unavailable");
            break;
        case ExternalOpenResult::busy:
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Another file is being opened");
            break;
        case ExternalOpenResult::failed:
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "No application could open the selected file");
            break;
        }
        update_status(lastUpdate_);
    });
}

void MainWindow::open_file_with_application(const ExternalApplication &application,
                                            const QString &path) {
#ifdef VOVE_UI_TEST_HOOKS
    if (property("captureExternalOpen").toBool()) {
        setProperty("lastExternalOpenKind", QStringLiteral("saved"));
        setProperty("lastExternalOpenApplication", application.path);
        setProperty("lastExternalOpenPath", QDir::cleanPath(path));
        return;
    }
#endif
    QString error;
    if (!externalApplications_.launch(application, path, &error)) {
        operationStatus_ =
            error == QStringLiteral("application_unavailable")
                ? QCoreApplication::translate("MainWindow",
                                              "The selected application is no longer available")
                : QCoreApplication::translate("MainWindow",
                                              "The selected application could not be started");
        update_status(lastUpdate_);
    }
}

void MainWindow::reveal_file_in_file_manager(const QString &path) {
    externalOpenCoordinator_.reveal(path, [this](const ExternalOpenResult result) {
        switch (result) {
        case ExternalOpenResult::opened:
            operationStatus_.clear();
            break;
        case ExternalOpenResult::timed_out:
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "The file manager did not respond");
            break;
        case ExternalOpenResult::helper_unavailable:
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "The desktop integration component is unavailable");
            break;
        case ExternalOpenResult::busy:
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Another external action is still running");
            break;
        case ExternalOpenResult::failed:
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Could not show the selected file in the file manager");
            break;
        }
        update_status(lastUpdate_);
    });
}

void MainWindow::prompt_add_external_application(const QString &file_path) {
    const auto application = QFileDialog::getOpenFileName(
        this, QCoreApplication::translate("MainWindow", "Add Application"),
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation));
    if (application.isEmpty()) {
        return;
    }
    const auto result = externalApplications_.add(
        {.name = {}, .path = application, .icon_png = application_icon_png(application)});
    if (result == AddExternalApplicationResult::invalid) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The selected application cannot be used");
        update_status(lastUpdate_);
        return;
    }
    if (result == AddExternalApplicationResult::duplicate) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "This application is already in the list");
        update_status(lastUpdate_);
        return;
    }
    schedule_settings_save();
    open_file_with_application(externalApplications_.applications().back(), file_path);
}

void MainWindow::open_path(const QString &path, const bool add_to_history,
                          const QString &reveal_child) {
    auto normalized = path.trimmed();
    if (normalized.size() >= 2 && normalized.front() == QLatin1Char('"') &&
        normalized.back() == QLatin1Char('"')) {
        normalized = normalized.mid(1, normalized.size() - 2);
    }
    if (normalized.isEmpty()) {
        return;
    }
    if (add_to_history) {
        save_navigation_state();
    }
    leave_recursive_view(false);
    pendingNavigationRestore_.reset();
    if (trashViewActive_) {
        leave_trash_view();
    }
    normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalized));
    if (QDir::isRelativePath(normalized)) {
        normalized =
            QDir::cleanPath(QDir(currentPath_.isEmpty() ? QDir::currentPath() : currentPath_)
                                .absoluteFilePath(normalized));
    }

    if (globalSearchActive_ ||
        (globalSearchEdit_ != nullptr && !globalSearchEdit_->text().isEmpty())) {
        const QSignalBlocker blocker(globalSearchEdit_);
        globalSearchEdit_->clear();
        leave_global_search(false);
    }

    if (add_to_history) {
        NavigationEntry next{.path = normalized};
        for (auto it = history_.crbegin(); it != history_.crend(); ++it) {
            if (it->saved && same_directory_path(it->path, normalized)) {
                next.grid = it->grid;
                next.saved = true;
                break;
            }
        }
        while (history_.size() - 1 > historyIndex_) {
            history_.removeLast();
        }
        if (history_.isEmpty() || !same_directory_path(history_.back().path, normalized)) {
            history_.push_back(std::move(next));
        }
        if (history_.size() > 128) {
            history_.removeFirst();
        }
        historyIndex_ = static_cast<int>(history_.size()) - 1;
    }
    if (historyIndex_ >= 0 && historyIndex_ < history_.size() &&
        history_[historyIndex_].saved &&
        same_directory_path(history_[historyIndex_].path, normalized)) {
        pendingNavigationRestore_ = PendingNavigationRestore{
            .path = normalized, .grid = history_[historyIndex_].grid,
            .reveal_child = reveal_child};
    } else if (!reveal_child.isEmpty()) {
        pendingNavigationRestore_ = PendingNavigationRestore{
            .path = normalized, .grid = {}, .reveal_child = reveal_child};
    }

    const auto was_protected = previewRuntimeState_ != PreviewRuntimeState::online;
    restrictOfflineFallback_ = restrictOfflineFallback_ || was_protected;
    if (was_protected) {
        strictRevalidatedPreviews_.clear();
        strictRevalidatedPages_.clear();
    }
    currentPath_ = normalized;
    navigationCatalogReady_ = false;
    automaticRefreshInFlight_ = false;
    directoryMonitor_.set_directory(currentPath_);
    directoryMonitor_.scan_started();
    update_directory_monitor_visibility();
    schedule_settings_save();
    leave_preview_protected_state();
    begin_preview_generation();
    pathEdit_->setText(QDir::toNativeSeparators(currentPath_));
    selectedName_.clear();
    selectedColorSummary_.clear();
    previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Select a file"));
    previewName_->clear();
    previewName_->setToolTip({});
    previewFormat_->clear();
    previewFormat_->hide();
    previewModified_->set_modified_time(std::nullopt);
    session_.open(native_path(currentPath_));
    update_directory_tree();
    pollTimer_->setInterval(16);
    pollTimer_->start();
    update_navigation();
    catalog::CatalogSessionUpdate update;
    update.state = catalog::CatalogSessionState::loading;
    lastUpdate_ = update;
    update_status(lastUpdate_);
}

void MainWindow::refresh_catalog() {
    if (trashViewActive_) {
        const auto state = refresh_current_operation_journal_state();
        if (state == CurrentOperationJournalState::none && !trashCoordinator_.busy() &&
            !deleteInFlight_) {
            start_trash_catalog_load();
            return;
        }
        leave_trash_view();
        open_path(currentPath_, false);
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        batchRenameCoordinator_.busy() || fileTransferCoordinator_.busy() ||
        directoryTransferService_.busy() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Waiting for the file operation result…");
        update_status(lastUpdate_);
        return;
    }
    const auto journal_state = refresh_current_operation_journal_state();
    if (journal_state == CurrentOperationJournalState::blocked) {
        update_status(lastUpdate_);
        return;
    }
    if (journal_state == CurrentOperationJournalState::directory_transfer_recovery) {
        offer_directory_transfer_discard();
        return;
    }
    if (trashCoordinator_.owns_recovery()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Recovering VO-VE Trash operation…");
        update_status(lastUpdate_);
        if (!submit_trash_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start VO-VE Trash recovery");
            update_status(lastUpdate_);
        }
        return;
    }
    if (journal_state == CurrentOperationJournalState::transfer_recovery &&
        !fileTransferCoordinator_.owns_recovery()) {
        static_cast<void>(start_object_transfer({}, {}, fileops::FileTransferKind::copy, true));
        return;
    }
    if (fileTransferCoordinator_.owns_recovery()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Recovering copy or move…");
        update_status(lastUpdate_);
        if (!submit_file_transfer_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start transfer recovery");
            update_status(lastUpdate_);
        }
        return;
    }
    if (permanentDeleteCoordinator_.owns_recovery()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Recovering safe deletion…");
        update_status(lastUpdate_);
        if (!submit_delete_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start deletion recovery");
            update_status(lastUpdate_);
        }
        return;
    }
    if (batchRenameCoordinator_.owns_recovery()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Recovering batch rename…");
        update_status(lastUpdate_);
        if (!submit_batch_rename_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start name recovery");
            update_status(lastUpdate_);
        }
        return;
    }
    if (pendingRenameOperation_) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Checking rename result…");
        update_status(lastUpdate_);
        if (!submit_pending_rename_reconciliation()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to start result verification");
            update_status(lastUpdate_);
        }
        return;
    }
    if (globalSearchActive_) {
        start_global_search();
        return;
    }
    const auto was_protected = previewRuntimeState_ != PreviewRuntimeState::online;
    restrictOfflineFallback_ = restrictOfflineFallback_ || was_protected;
    if (was_protected) {
        strictRevalidatedPreviews_.clear();
        strictRevalidatedPages_.clear();
    }
    leave_preview_protected_state();
    begin_preview_generation(false);
    listModel_.mark_previews_verification_pending();
    if (listView_->currentIndex().isValid()) {
        update_selected_preview(listView_->currentIndex());
    }
    refresh_directory_sources();
    pollTimer_->setInterval(16);
    pollTimer_->start();
    catalog::CatalogSessionUpdate update;
    update.state = catalog::CatalogSessionState::loading;
    lastUpdate_ = update;
    update_status(lastUpdate_);
}

bool MainWindow::automatic_refresh_blocked() const {
    return trashViewActive_ || globalSearchActive_ || recursiveViewActive_ || !isVisible() || isMinimized() ||
           QApplication::activeModalWidget() != nullptr ||
           QApplication::activePopupWidget() != nullptr ||
           QApplication::mouseButtons() != Qt::NoButton || !dragStatus_.isEmpty() ||
           renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
           pendingTransferPreparation_ || pendingDirectoryTransferBatch_ ||
           pendingCreateDirectoryPreparation_ || pendingRenameOperation_ ||
           batchRenameCoordinator_.busy() || fileTransferCoordinator_.busy() ||
           directoryTransferService_.busy() || permanentDeleteCoordinator_.busy() ||
           trashCoordinator_.busy();
}

void MainWindow::update_directory_monitor_visibility() {
    directoryMonitor_.set_visible(isVisible() && !isMinimized() && !globalSearchActive_ &&
                                  !trashViewActive_ && !recursiveViewActive_);
}

bool MainWindow::refresh_catalog_automatically() {
    if (automatic_refresh_blocked() || !source_.idle() ||
        lastUpdate_.state == catalog::CatalogSessionState::loading ||
        lastUpdate_.state == catalog::CatalogSessionState::waiting_retry) {
        return false;
    }
    automaticRefreshInFlight_ = true;
    beforeAutomaticRefresh_ = lastUpdate_;
    directoryMonitor_.scan_started();
    session_.refresh();
    lastUpdate_.state = catalog::CatalogSessionState::loading;
    pollTimer_->setInterval(16);
    pollTimer_->start();
    return true;
}

void MainWindow::poll_catalog() {
    if (globalSearchActive_ || trashViewActive_) {
        return;
    }
    if (automaticRefreshInFlight_ && automatic_refresh_blocked()) {
        // Do not leave a bounded producer queue blocked behind a long user interaction.
        session_.suspend();
        pollTimer_->stop();
        lastUpdate_ = beforeAutomaticRefresh_;
        automaticRefreshInFlight_ = false;
        directoryMonitor_.scan_finished(true);
        directoryMonitor_.request_recheck();
        return;
    }
    const auto automatic = automaticRefreshInFlight_;
    std::optional<QSignalBlocker> selection_blocker;
    if (automatic) {
        selection_blocker.emplace(listView_->selectionModel());
    }
    const auto previous_state = lastUpdate_.state;
    const auto view_state = capture_grid_view_state();
    catalog::CatalogSessionUpdate update;
    std::size_t received{};
    bool model_changed{};
    for (int index = 0; index < 8; ++index) {
        auto next = session_.poll();
        received += next.entries_received;
        model_changed = model_changed || next.model_changed;
        const auto made_progress = next.entries_received != 0 || next.model_changed;
        update = std::move(next);
        if (!made_progress) {
            break;
        }
    }
    update.entries_received = received;
    update.model_changed = model_changed;
    if (automatic && update.state == catalog::CatalogSessionState::ready &&
        !listModel_.last_replacement_changed()) {
        update.model_changed = false;
    }
    if (update.model_changed) {
        restore_grid_view_state(view_state);
        const auto *modified_entry = listModel_.entry_at(listView_->currentIndex());
        previewModified_->set_modified_time(
            modified_entry != nullptr && modified_entry->kind == core::EntryKind::file
                ? std::optional{modified_entry->modified_unix_ns}
                : std::nullopt);
        if (automatic) {
            const auto current = listView_->currentIndex();
            const auto *entry = listModel_.entry_at(current);
            if (entry == nullptr || entry->id != view_state.current_id ||
                (selectedPreview_ && !same_source(*selectedPreview_, *entry))) {
                show_selection(current);
            }
            update_rename_action();
        }
        if (update.state == catalog::CatalogSessionState::ready) {
            if (!globalSearchRemovedPaths_.isEmpty()) {
                for (int row = 0; row < listModel_.rowCount(); ++row) {
                    const auto *entry = listModel_.entry_at(listModel_.index(row, 0));
                    if (entry && !entry->source_revision_utf8.empty())
                        allow_observed_global_search_path(QString::fromUtf8(entry->path_utf8));
                }
            }
            if (automatic && !recursiveViewActive_) {
                treeModel_->reconcile_children(treeModel_->ensure_path(currentPath_),
                                               listModel_.directory_entries(), !update.truncated);
            }
            if (!recursiveViewActive_) update_directory_tree();
            previewTimer_->start();
        }
        if (recursiveViewActive_ && !previewTimer_->isActive()) previewTimer_->start();
    }
    if (update.state == catalog::CatalogSessionState::ready) {
        if (!recursiveViewActive_) {
            navigationCatalogReady_ = true;
        }
        restore_navigation_state();
        restore_pending_rename_selection();
    }
    lastUpdate_ = update;
    if (recursiveViewActive_) update_rename_action();
    if (update.state == catalog::CatalogSessionState::network_disconnected ||
        update.state == catalog::CatalogSessionState::timed_out ||
        update.state == catalog::CatalogSessionState::waiting_retry) {
        const auto entered_offline =
            previous_state != catalog::CatalogSessionState::network_disconnected &&
            previous_state != catalog::CatalogSessionState::timed_out &&
            previous_state != catalog::CatalogSessionState::waiting_retry;
        if (entered_offline || previewRuntimeState_ != PreviewRuntimeState::offline) {
            enter_preview_offline_state(false);
        }
    } else if (update.state == catalog::CatalogSessionState::ready &&
               (previewRevalidationPending_ ||
                previewRuntimeState_ == PreviewRuntimeState::blocked)) {
        previewRevalidationPending_ = false;
        leave_preview_protected_state();
        begin_preview_generation(false);
        listModel_.mark_previews_verification_pending();
        if (listView_->currentIndex().isValid()) {
            update_selected_preview(listView_->currentIndex());
        }
        previewTimer_->start();
    } else if (update.state == catalog::CatalogSessionState::ready) {
        previewRuntimeState_ = PreviewRuntimeState::online;
        folderMosaicController_.set_offline(false);
        folderMosaicController_.set_allow_offline_fallback(!restrictOfflineFallback_);
    } else if (update.state == catalog::CatalogSessionState::authentication_required ||
               update.state == catalog::CatalogSessionState::permission_denied ||
               update.state == catalog::CatalogSessionState::unavailable) {
        enter_preview_blocked_state();
    }
    if (!automatic || update.state != catalog::CatalogSessionState::loading) {
        update_status(lastUpdate_);
    }

    if (update.state == catalog::CatalogSessionState::ready ||
        update.state == catalog::CatalogSessionState::network_disconnected ||
        update.state == catalog::CatalogSessionState::timed_out ||
        update.state == catalog::CatalogSessionState::authentication_required ||
        update.state == catalog::CatalogSessionState::permission_denied ||
        update.state == catalog::CatalogSessionState::unavailable) {
        pollTimer_->stop();
        directoryMonitor_.scan_finished(update.state == catalog::CatalogSessionState::ready);
        automaticRefreshInFlight_ = false;
    } else if (update.state == catalog::CatalogSessionState::waiting_retry) {
        pollTimer_->setInterval(100);
    } else {
        pollTimer_->setInterval(16);
    }
    update_clipboard_actions();
}

QList<core::DirectoryEntry> MainWindow::selected_entries_in_view_order() const {
    auto selected = listView_->selectionModel()->selectedRows();
    if (selected.isEmpty() && listView_->currentIndex().isValid()) {
        selected.push_back(listView_->currentIndex());
    }
    std::ranges::sort(selected, {}, [](const QModelIndex &index) { return index.row(); });
    QList<core::DirectoryEntry> entries;
    entries.reserve(selected.size());
    for (const auto &index : selected) {
        if (const auto *entry = listModel_.entry_at(index); entry != nullptr) {
            entries.push_back(*entry);
        }
    }
    return entries;
}

QString MainWindow::selection_status_text() const {
    if (listView_ == nullptr || listView_->selectionModel() == nullptr) {
        return {};
    }
    const auto selected = listView_->selectionModel()->selectedRows();
    if (selected.size() < 2) {
        return {};
    }
    qulonglong selected_directories{};
    qulonglong selected_files{};
    for (const auto &index : selected) {
        const auto *entry = listModel_.entry_at(index);
        if (entry == nullptr) {
            continue;
        }
        if (entry->kind == core::EntryKind::directory) {
            ++selected_directories;
        } else {
            ++selected_files;
        }
    }
    return QCoreApplication::translate("MainWindow", "Selected: %1 of %2 folders · %3 of %4 files")
        .arg(selected_directories)
        .arg(static_cast<qulonglong>(listModel_.visible_directories()))
        .arg(selected_files)
        .arg(static_cast<qulonglong>(listModel_.visible_files()));
}

void MainWindow::copy_selected_names() {
    QStringList values;
    for (const auto &entry : selected_entries_in_view_order()) {
        values.push_back(QString::fromUtf8(entry.name_utf8));
    }
    if (!values.isEmpty()) {
        QApplication::clipboard()->setText(values.join(QLatin1Char('\n')));
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Names copied: %1").arg(values.size());
        update_status(lastUpdate_);
    }
}

void MainWindow::copy_selected_paths() {
    QStringList values;
    for (const auto &entry : selected_entries_in_view_order()) {
        values.push_back(QDir::toNativeSeparators(QString::fromUtf8(entry.path_utf8)));
    }
    if (!values.isEmpty()) {
        QApplication::clipboard()->setText(values.join(QLatin1Char('\n')));
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Paths copied: %1").arg(values.size());
        update_status(lastUpdate_);
    }
}

void MainWindow::update_clipboard_actions() {
    if (!copyObjectsAction_ || !pasteObjectsAction_ || !listView_->selectionModel()) return;
    const auto entries = selected_entries_in_view_order();
    copyObjectsAction_->setEnabled(!trashViewActive_ &&
        !listView_->selectionModel()->selectedRows().isEmpty() &&
        entries.size() <= 1024 && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        }));
    copyPathsAction_->setEnabled(!trashViewActive_ &&
        !listView_->selectionModel()->selectedRows().isEmpty() && !entries.isEmpty());
    const auto *mime = QApplication::clipboard()->mimeData();
    pasteObjectsAction_->setEnabled(createFolderAction_ && createFolderAction_->isEnabled() &&
        lastUpdate_.state == catalog::CatalogSessionState::ready &&
        !pendingExternalDirectoryDrop_ && !pendingExternalTransferDrop_ &&
        !pendingDirectoryPreparation_ && dropProbeSource_.idle() &&
        !trashConfirmationInProgress_ && mime && mime->hasUrls());
}

void MainWindow::copy_selected_objects() {
    update_clipboard_actions();
    if (!copyObjectsAction_->isEnabled()) return;
    auto mime = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    QStringList names;
    for (const auto &entry : selected_entries_in_view_order()) {
        urls.push_back(QUrl::fromLocalFile(QString::fromUtf8(entry.path_utf8)));
        names.push_back(QString::fromUtf8(entry.name_utf8));
    }
    mime->setUrls(urls);
    mime->setText(names.join(QLatin1Char('\n')));
    // Public file URLs let the platform clipboard interoperate with file managers.
    // Do not export the private drag payload or transfer image bytes.
    if (!external_drop_paths(mime.get())) return;
    QApplication::clipboard()->setMimeData(mime.release());
}

void MainWindow::paste_objects() {
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }
    update_clipboard_actions();
    if (!pasteObjectsAction_->isEnabled()) return;
    const auto paths = external_drop_paths(QApplication::clipboard()->mimeData());
    if (!paths) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Clipboard does not contain valid file paths");
        update_status(lastUpdate_);
        return;
    }
    // Capture the destination now; asynchronous source checks must not follow later navigation.
    static_cast<void>(begin_external_transfer_drop(*paths, currentPath_,
        fileops::FileTransferKind::copy, false, true));
}

void MainWindow::prompt_transfer_selected(const fileops::FileTransferKind kind) {
    const auto entries = selected_entries_in_view_order();
    if (entries.isEmpty() || !std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        })) {
        return;
    }
    const auto destination = QFileDialog::getExistingDirectory(
        this,
        kind == fileops::FileTransferKind::copy
            ? QCoreApplication::translate("MainWindow", "Copy to Folder")
            : QCoreApplication::translate("MainWindow", "Move to Folder"),
        currentPath_, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!destination.isEmpty()) {
        static_cast<void>(start_transfer_selection(entries, destination, kind));
    }
}

bool MainWindow::start_transfer_selection(const QList<core::DirectoryEntry> &entries,
                                           const QString &destination, const fileops::FileTransferKind kind) {
    if (entries.isEmpty()) return false;
    if (globalSearchActive_) {
        if (!globalSearchReady_ || !listView_->dragEnabled() ||
            globalSearchQuery_ != globalSearchEdit_->text().trimmed() ||
            globalSearchRemovedPaths_.size() + entries.size() >
                static_cast<qsizetype>(search::kMaximumSearchResults))
            return false;
        QStringList paths;
        for (const auto &entry : entries) paths.push_back(QString::fromUtf8(entry.path_utf8));
        return begin_external_transfer_drop(paths, destination, kind, false, false, entries);
    }
    if (recursiveViewActive_) {
        if (lastUpdate_.state != catalog::CatalogSessionState::ready) return false;
        return start_object_transfer(entries, destination, kind);
    }
    if (std::ranges::all_of(entries, [](const auto &entry) { return entry.kind == core::EntryKind::file; })) {
        return start_file_transfer(entries, destination, kind);
    }
    if (std::ranges::all_of(entries, [](const auto &entry) { return entry.kind == core::EntryKind::directory; })) {
        return start_directory_transfer(entries, destination, kind);
    }
    return start_object_transfer(entries, destination, kind);
}

bool MainWindow::start_object_transfer(const QList<core::DirectoryEntry> &entries,
                                        const QString &destination, const fileops::FileTransferKind kind,
                                        const bool resume) {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileOperationService_.busy() || pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        objectTransferQueue_.busy() || fileTransferCoordinator_.busy() || directoryTransferService_.busy()) return false;
    if (!resume && refresh_current_operation_journal_state() != CurrentOperationJournalState::none) return false;
    folderMosaicController_.clear_pending();
    activeTransferKind_ = kind;
    set_transfer_in_flight(true);
    operationStatus_ = QCoreApplication::translate("MainWindow", "Checking the selected objects…");
    update_status(lastUpdate_);
    const QPointer<MainWindow> window(this);
    const auto progress = [window](fileops::FileTransferProgressUpdate value) {
        if (window) QMetaObject::invokeMethod(window, [window, value = std::move(value)] {
            if (window) window->handle_file_transfer_progress(value);
        }, Qt::QueuedConnection);
    };
    const auto conflict = [window](fileops::FileTransferConflict value, bool overwrite_allowed, bool copy_allowed) {
        if (window) QMetaObject::invokeMethod(window, [window, value = std::move(value), overwrite_allowed, copy_allowed] {
            if (window) window->handle_file_transfer_conflict(value, true, overwrite_allowed, copy_allowed);
        }, Qt::QueuedConnection);
    };
    const auto completion = [window](ObjectTransferResult result) {
        if (window) QMetaObject::invokeMethod(window, [window, result = std::move(result)] {
            if (!window) return;
            window->set_transfer_in_flight(false);
            window->notify_file_in_use(result.operation_status);
            static_cast<void>(window->refresh_current_operation_journal_state());
            if (result.success) {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Objects transferred: %1").arg(result.completed);
            } else if (result.recovery) {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Transfer paused. Check the network and press F5");
            } else if (result.cancelled) {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Transfer cancelled. Objects transferred: %1").arg(result.completed);
            } else {
                window->operationStatus_ = QCoreApplication::translate("MainWindow", "Cannot transfer the selected objects: %1").arg(result.detail);
            }
            window->setProperty("objectTransferDetail", result.detail);
            if (window->globalSearchActive_) window->catalogBeforeGlobalSearchDirty_ = true;
            for (const auto &path : result.published_paths)
                window->allow_observed_global_search_path(path);
            window->reconcile_global_search_move(result.moved_sources);
            window->begin_preview_generation();
            window->refresh_directory_sources();
            if (!window->globalSearchActive_) window->pollTimer_->start();
            window->update_status(window->lastUpdate_);
            if (window->globalSearchActive_ &&
                window->globalSearchQuery_ != window->globalSearchEdit_->text().trimmed())
                window->schedule_global_search();
        }, Qt::QueuedConnection);
    };
    const auto submitted = resume ? objectTransferQueue_.resume(progress, conflict, completion)
        : objectTransferQueue_.start(kind, {entries.begin(), entries.end()}, native_path(destination), progress, conflict, completion);
    if (!submitted) set_transfer_in_flight(false);
    return submitted;
}

bool MainWindow::start_file_transfer(const QList<core::DirectoryEntry> &entries,
                                     const QString &destination_directory,
                                     const fileops::FileTransferKind kind) {
    const auto all_regular_files =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file;
        });
    if (!all_regular_files || destination_directory.trimmed().isEmpty()) {
        return false;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return false;
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileOperationService_.busy() || pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        fileTransferCoordinator_.busy()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
        return false;
    }

    PendingTransferPreparation pending;
    pending.kind = kind;
    pending.entries = entries;
    pending.destination_directory =
        native_path(QDir::cleanPath(QDir::fromNativeSeparators(destination_directory.trimmed())));
    QSet<QString> probe_keys;
    const auto add_probe = [this, &pending, &probe_keys](const std::filesystem::path &path) {
        const auto normalized = path.lexically_normal();
        const auto key = transfer_path_key(normalized);
        if (normalized.empty() || probe_keys.contains(key)) {
            return;
        }
        probe_keys.insert(key);
        pending.probes.push_back({.generation = nextTransferProbeGeneration_++,
                                  .path = normalized,
                                  .revision_utf8 = {},
                                  .complete = false});
    };
    for (const auto &entry : entries) {
        add_probe(native_path(QString::fromUtf8(entry.path_utf8)).parent_path());
    }
    add_probe(pending.destination_directory);
    if (pending.probes.empty()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to determine the operation folders");
        update_status(lastUpdate_);
        return false;
    }

    const auto first_probe_generation = pending.probes.front().generation;
    const auto first_probe_path = pending.probes.front().path;
    pendingTransferPreparation_ = std::move(pending);
    set_transfer_in_flight(true);
    operationStatus_ =
        kind == fileops::FileTransferKind::copy
            ? QCoreApplication::translate("MainWindow", "Checking folders before copying…")
            : QCoreApplication::translate("MainWindow", "Checking folders before moving…");
    update_status(lastUpdate_);
    transferProbeSource_.submit(
        {.generation = first_probe_generation, .path = first_probe_path, .maximum_entries = 1});
    transferProbeTimer_->start();
    return true;
}

bool MainWindow::start_directory_transfer(const QList<core::DirectoryEntry> &entries,
                                          const QString &destination_directory,
                                          const fileops::FileTransferKind kind) {
    if (entries.isEmpty() || !std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::directory;
        })) {
        return false;
    }
    QStringList paths;
    std::vector<std::string> revisions;
    paths.reserve(entries.size());
    revisions.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        if (entry.source_revision_utf8.empty()) {
            return false;
        }
        paths.push_back(QString::fromUtf8(entry.path_utf8));
        revisions.push_back(entry.source_revision_utf8);
    }
    return start_directory_transfer_paths(std::move(paths), std::move(revisions),
                                          destination_directory, kind);
}

bool MainWindow::start_directory_transfer_paths(QStringList source_paths,
                                                std::vector<std::string> source_revisions_utf8,
                                                const QString &destination_directory,
                                                const fileops::FileTransferKind kind) {
    if (source_paths.isEmpty() ||
        static_cast<std::size_t>(source_paths.size()) > fileops::kMaximumDirectoryTransferRoots ||
        source_revisions_utf8.size() != static_cast<std::size_t>(source_paths.size()) ||
        std::ranges::any_of(source_revisions_utf8, &std::string::empty) ||
        destination_directory.trimmed().isEmpty() ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileOperationService_.busy() || directoryTransferService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        fileTransferCoordinator_.busy()) {
        operationStatus_ =
            static_cast<std::size_t>(source_paths.size()) > fileops::kMaximumDirectoryTransferRoots
                ? QCoreApplication::translate("MainWindow",
                                              "No more than %1 folders can be transferred at once")
                      .arg(fileops::kMaximumDirectoryTransferRoots)
                : QCoreApplication::translate("MainWindow",
                                              "Another file operation is still running");
        update_status(lastUpdate_);
        return false;
    }

    const auto destination =
        native_path(QDir::cleanPath(QDir::fromNativeSeparators(destination_directory.trimmed())));
    if (!destination.is_absolute()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to determine the destination folder");
        update_status(lastUpdate_);
        return false;
    }
    for (auto &path : source_paths) {
        path = QDir::cleanPath(QDir::fromNativeSeparators(path));
        if (!native_path(path).is_absolute()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to determine the source folder");
            update_status(lastUpdate_);
            return false;
        }
    }

    pendingDirectoryTransferBatch_ = PendingDirectoryTransferBatch{
        .kind = kind,
        .source_paths = std::move(source_paths),
        .source_revisions_utf8 = std::move(source_revisions_utf8),
        .destination_directory = destination,
    };
    // Folder mosaics may still be reading children of a directory selected for transfer.
    // Release those preview handles before the transfer engine validates and retires the source.
    folderMosaicController_.clear_pending();
    set_transfer_in_flight(true);
    return submit_next_directory_transfer();
}

bool MainWindow::submit_next_directory_transfer() {
    if (!pendingDirectoryTransferBatch_) {
        return false;
    }
    auto &batch = *pendingDirectoryTransferBatch_;
    if (batch.index >= batch.source_paths.size()) {
        const auto completed = batch.completed;
        const auto kind = batch.kind;
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            kind == fileops::FileTransferKind::move
                ? QCoreApplication::translate("MainWindow", "Folders moved: %1").arg(completed)
                : QCoreApplication::translate("MainWindow", "Folders copied: %1").arg(completed);
        begin_preview_generation();
        refresh_directory_sources();
        pollTimer_->setInterval(16);
        pollTimer_->start();
        update_status(lastUpdate_);
        return true;
    }

    const auto source = native_path(batch.source_paths.at(batch.index));
    const auto filename = source.filename();
    if (filename.empty()) {
        pendingDirectoryTransferBatch_.reset();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file-system root cannot be transferred");
        update_status(lastUpdate_);
        return false;
    }
    const auto operation_id = next_directory_transfer_request_id();
    const fileops::BasicDirectoryTransferStreamRequest request{
        .request_id = operation_id,
        .operation_id = operation_id,
        .command = batch.kind == fileops::FileTransferKind::move
                       ? fileops::BasicDirectoryTransferCommand::move
                       : fileops::BasicDirectoryTransferCommand::copy,
        .source = source,
        .destination = batch.destination_directory / filename,
        .manifest_path = {},
        .expected_source_revision_utf8 =
            batch.source_revisions_utf8.at(static_cast<std::size_t>(batch.index)),
    };
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_ = display_path(request.source);
    directoryTransferRecoveryDestination_ = display_path(request.destination);
    directoryTransferRecoverySourceRevision_ =
        QString::fromUtf8(request.expected_source_revision_utf8);
    directoryTransferRecoveryRequestId_ = request.request_id;
    directoryTransferCompletionKnown_ = false;
    setProperty("directoryTransferRecoveryPending", true);
    if (!persist_directory_transfer_recovery_state()) {
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to save folder transfer state");
        update_status(lastUpdate_);
        return false;
    }
    operationStatus_ = batch.kind == fileops::FileTransferKind::move
                           ? QCoreApplication::translate("MainWindow", "Moving folder %1 of %2…")
                                 .arg(batch.index + 1)
                                 .arg(batch.source_paths.size())
                           : QCoreApplication::translate("MainWindow", "Copying folder %1 of %2…")
                                 .arg(batch.index + 1)
                                 .arg(batch.source_paths.size());
    update_status(lastUpdate_);

    const QPointer<MainWindow> window(this);
    const auto completion_state = encode_directory_transfer_recovery_state(true);
    if (completion_state.isEmpty()) {
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder transfer state exceeds the safe limit");
        update_status(lastUpdate_);
        return false;
    }
    const auto submitted = directoryTransferService_.submit_directory(
        request,
        [window, completion_state](const fileops::BasicDirectoryTransferProgress &progress) {
            const auto sharedProgress =
                std::make_shared<fileops::BasicDirectoryTransferProgress>(progress);
            if (progress.phase == fileops::BasicDirectoryTransferPhase::completed) {
                QSettings settings;
                settings.setValue(QString::fromLatin1(directoryTransferStateKey), completion_state);
                settings.sync();
                if (settings.status() != QSettings::NoError) {
                    throw std::runtime_error("directory completion evidence was not persisted");
                }
            }
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, sharedProgress] {
                    if (!window.isNull()) {
                        window->handle_directory_transfer_progress(*sharedProgress);
                        if (sharedProgress->phase ==
                            fileops::BasicDirectoryTransferPhase::completed) {
                            static_cast<void>(window->record_directory_transfer_completion());
                        }
                    }
                },
                Qt::QueuedConnection);
        },
        [window](fileops::BasicDirectoryTransferResult result) mutable {
            if (!window.isNull()) {
                const auto sharedResult =
                    std::make_shared<fileops::BasicDirectoryTransferResult>(std::move(result));
                QMetaObject::invokeMethod(
                    window,
                    [window, sharedResult] {
                        if (!window.isNull()) {
                            window->handle_directory_transfer_result(*sharedResult);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        pendingDirectoryTransferBatch_.reset();
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to start folder transfer");
        update_status(lastUpdate_);
    }
    return submitted;
}

void MainWindow::poll_transfer_preparation() {
    if (pendingCreateDirectoryPreparation_) {
        poll_create_directory_preparation();
        return;
    }
    if (!pendingTransferPreparation_) {
        transferProbeTimer_->stop();
        return;
    }
    for (int index{}; index < 32; ++index) {
        auto batch = transferProbeSource_.poll();
        if (!batch) {
            break;
        }
        const auto target = std::ranges::find(pendingTransferPreparation_->probes,
                                              batch->generation, &TransferProbeTarget::generation);
        if (target == pendingTransferPreparation_->probes.end() || target->complete) {
            continue;
        }
        if (batch->error) {
            QString message;
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                message = QCoreApplication::translate("MainWindow",
                                                      "The folder requires network sign-in");
                break;
            case catalog::CatalogErrorKind::permission_denied:
                message = QCoreApplication::translate("MainWindow",
                                                      "Cannot access the destination folder");
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                message = QCoreApplication::translate(
                    "MainWindow", "Network unavailable. Transfer was not started");
                break;
            case catalog::CatalogErrorKind::not_found:
                message = QCoreApplication::translate("MainWindow", "Destination folder not found");
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                message = QCoreApplication::translate("MainWindow", "Failed to check the folder");
                break;
            }
            fail_transfer_preparation(message);
            return;
        }
        if (batch->is_final) {
            if (batch->directory_revision_utf8.empty()) {
                fail_transfer_preparation(
                    QCoreApplication::translate("MainWindow", "Failed to verify folder identity"));
                return;
            }
            target->revision_utf8 = std::move(batch->directory_revision_utf8);
            target->complete = true;
            const auto next = std::ranges::find(pendingTransferPreparation_->probes, false,
                                                &TransferProbeTarget::complete);
            if (next != pendingTransferPreparation_->probes.end()) {
                transferProbeSource_.submit(
                    {.generation = next->generation, .path = next->path, .maximum_entries = 1});
            }
        }
    }
    if (std::ranges::all_of(pendingTransferPreparation_->probes, &TransferProbeTarget::complete)) {
        launch_prepared_file_transfer();
    }
}

void MainWindow::poll_create_directory_preparation() {
    if (!pendingCreateDirectoryPreparation_) {
        transferProbeTimer_->stop();
        return;
    }
    for (int index{}; index < 32; ++index) {
        auto batch = transferProbeSource_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != pendingCreateDirectoryPreparation_->generation) {
            continue;
        }
        if (batch->error) {
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "The folder requires network sign-in");
                break;
            case catalog::CatalogErrorKind::permission_denied:
                operationStatus_ =
                    QCoreApplication::translate("MainWindow", "Cannot access the current folder");
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "Network unavailable. Folder was not created");
                break;
            case catalog::CatalogErrorKind::not_found:
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "The current folder no longer exists");
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                operationStatus_ =
                    QCoreApplication::translate("MainWindow", "Failed to check the current folder");
                break;
            }
            pendingCreateDirectoryPreparation_.reset();
            transferProbeTimer_->stop();
            set_create_directory_in_flight(false);
            update_status(lastUpdate_);
            return;
        }
        if (!batch->is_final) {
            continue;
        }
        if (batch->directory_revision_utf8.empty()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to verify the current folder");
            pendingCreateDirectoryPreparation_.reset();
            transferProbeTimer_->stop();
            set_create_directory_in_flight(false);
            update_status(lastUpdate_);
            return;
        }

        const auto destination = pendingCreateDirectoryPreparation_->destination;
        pendingCreateDirectoryPreparation_.reset();
        transferProbeTimer_->stop();
        static std::atomic_uint64_t next_operation{1};
        fileops::CreateDirectoryRequest request{
            .operation_id = next_operation.fetch_add(1, std::memory_order_relaxed),
            .destination = destination,
            .destination_parent_revision_utf8 = std::move(batch->directory_revision_utf8)};
        const auto destination_text = display_path(destination);
        operationStatus_ = QCoreApplication::translate("MainWindow", "Creating folder…");
        update_status(lastUpdate_);
        const QPointer<MainWindow> window(this);
        if (!fileOperationService_.submit_create_directory(
                std::move(request),
                [window, destination_text](fileops::OperationResult result) mutable {
                    if (!window.isNull()) {
                        QMetaObject::invokeMethod(
                            window,
                            [window, destination_text, result = std::move(result)]() mutable {
                                if (!window.isNull()) {
                                    window->handle_create_directory_result(result,
                                                                           destination_text);
                                }
                            },
                            Qt::QueuedConnection);
                    }
                })) {
            set_create_directory_in_flight(false);
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Another file operation is still running");
            update_status(lastUpdate_);
        }
        return;
    }
}

bool MainWindow::begin_external_directory_drop(const ExternalDirectoryDropIntent intent,
                                               QStringList paths,
                                               const fileops::FileTransferKind transfer_kind,
                                               QString transfer_destination) {
    if (paths.isEmpty() || pendingExternalDirectoryDrop_ || pendingExternalTransferDrop_ ||
        pendingDirectoryPreparation_ || !dropProbeSource_.idle() ||
        (intent == ExternalDirectoryDropIntent::open && paths.size() != 1) ||
        (intent == ExternalDirectoryDropIntent::transfer && transfer_destination.isEmpty())) {
        return false;
    }

    PendingExternalDirectoryDrop pending;
    pending.intent = intent;
    pending.paths = std::move(paths);
    pending.transfer_kind = transfer_kind;
    pending.transfer_destination = std::move(transfer_destination);
    pending.generation = nextDropProbeGeneration_++;
    const auto generation = pending.generation;
    const auto first_path = native_path(pending.paths.front());
    pendingExternalDirectoryDrop_ = std::move(pending);
    update_rename_action();
    operationStatus_ =
        intent == ExternalDirectoryDropIntent::pin
            ? QCoreApplication::translate("MainWindow", "Checking folders for Favorites…")
        : intent == ExternalDirectoryDropIntent::transfer
            ? (transfer_kind == fileops::FileTransferKind::move
                   ? QCoreApplication::translate("MainWindow", "Checking folders for moving…")
                   : QCoreApplication::translate("MainWindow", "Checking folders for copying…"))
            : QCoreApplication::translate("MainWindow", "Checking folder…");
    update_status(lastUpdate_);
    dropProbeSource_.submit({.generation = generation, .path = first_path, .maximum_entries = 1});
    dropProbeTimer_->start();
    return true;
}

bool MainWindow::begin_external_transfer_drop(QStringList paths,
                                              const QString &destination_directory,
                                              const fileops::FileTransferKind transfer_kind,
                                              const bool trash_target,
                                              const bool clipboard_copy,
                                              QList<core::DirectoryEntry> search_entries) {
    if (paths.isEmpty() ||
        static_cast<std::size_t>(paths.size()) > fileops::kMaximumFileTransferItems ||
        (!search_entries.isEmpty() && search_entries.size() != paths.size()) ||
        (!trash_target && destination_directory.trimmed().isEmpty()) ||
        pendingExternalDirectoryDrop_ || pendingExternalTransferDrop_ ||
        pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        return false;
    }

    PendingExternalTransferDrop pending;
    pending.paths = std::move(paths);
    pending.transfer_kind = transfer_kind;
    pending.transfer_destination = destination_directory;
    pending.trash_target = trash_target;
    pending.clipboard_copy = clipboard_copy;
    pending.search_entries = std::move(search_entries);
    pending.search_generation = pending.search_entries.isEmpty() ? 0 : activeGlobalSearchGeneration_;
    pending.search_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (const auto &path : std::as_const(pending.paths)) {
        const auto parent = native_path(path).parent_path();
        if (parent.empty()) {
            return false;
        }
    }

    pending.generation = nextDropProbeGeneration_++;
    const auto generation = pending.generation;
    const auto first_path = native_path(pending.paths.front());
    pendingExternalTransferDrop_ = std::move(pending);
    update_rename_action();
    operationStatus_ =
        trash_target
            ? QCoreApplication::translate("MainWindow", "Checking objects for VO-VE Trash…")
        : transfer_kind == fileops::FileTransferKind::move
            ? QCoreApplication::translate("MainWindow", "Checking objects before moving…")
            : QCoreApplication::translate("MainWindow", "Checking objects before copying…");
    update_status(lastUpdate_);
    dropProbeSource_.submit({.generation = generation,
                             .path = first_path.parent_path(),
                             .maximum_entries = 1,
                             .exact_entry_path = first_path});
    dropProbeTimer_->start();
    return true;
}

void MainWindow::poll_external_drop() {
    if (pendingDirectoryPreparation_) {
        poll_directory_preparation();
    } else if (pendingExternalTransferDrop_) {
        poll_external_transfer_drop();
    } else if (pendingExternalDirectoryDrop_) {
        poll_external_directory_drop();
    } else if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
        resume_queued_external_open();
    }
    update_rename_action();
}

void MainWindow::poll_external_directory_drop() {
    if (!pendingExternalDirectoryDrop_) {
        if (dropProbeSource_.idle()) {
            dropProbeTimer_->stop();
        }
        return;
    }
    for (int batch_index{}; batch_index < 32; ++batch_index) {
        auto batch = dropProbeSource_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != pendingExternalDirectoryDrop_->generation) {
            continue;
        }
        if (batch->error) {
            if (pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open &&
                batch->error.kind == catalog::CatalogErrorKind::cancelled &&
                !queuedExternalOpenPaths_.isEmpty()) {
                pendingExternalDirectoryDrop_.reset();
                if (dropProbeSource_.idle()) {
                    dropProbeTimer_->stop();
                    resume_queued_external_open();
                }
                return;
            }
            if (pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open &&
                batch->error.kind == catalog::CatalogErrorKind::io_error) {
                auto paths = std::move(pendingExternalDirectoryDrop_->paths);
                pendingExternalDirectoryDrop_.reset();
                if (dropProbeSource_.idle()) {
                    dropProbeTimer_->stop();
                }
                operationStatus_.clear();
                if (!resume_queued_external_open()) {
                    open_external_items_in_parent(paths);
                }
                return;
            }
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                fail_external_directory_drop(QCoreApplication::translate(
                    "MainWindow", "The folder requires network sign-in"));
                break;
            case catalog::CatalogErrorKind::permission_denied:
                fail_external_directory_drop(
                    QCoreApplication::translate("MainWindow", "Cannot access the folder"));
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                fail_external_directory_drop(QCoreApplication::translate(
                    "MainWindow", "The network folder did not respond"));
                break;
            case catalog::CatalogErrorKind::not_found:
                fail_external_directory_drop(
                    QCoreApplication::translate("MainWindow", "The dragged folder was not found"));
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                fail_external_directory_drop(
                    QCoreApplication::translate("MainWindow", "Only folders can be dragged here"));
                break;
            }
            return;
        }
        if (!batch->is_final) {
            continue;
        }
        if (batch->directory_revision_utf8.empty()) {
            fail_external_directory_drop(
                QCoreApplication::translate("MainWindow", "Only folders can be dragged here"));
            return;
        }

        pendingExternalDirectoryDrop_->confirmed_paths.push_back(
            pendingExternalDirectoryDrop_->paths.at(pendingExternalDirectoryDrop_->index));
        pendingExternalDirectoryDrop_->confirmed_revisions_utf8.push_back(
            std::move(batch->directory_revision_utf8));
        ++pendingExternalDirectoryDrop_->index;
        pendingExternalDirectoryDrop_->awaiting_worker_reap = true;
        break;
    }

    if (pendingExternalDirectoryDrop_ &&
        pendingExternalDirectoryDrop_->intent == ExternalDirectoryDropIntent::open &&
        !queuedExternalOpenPaths_.isEmpty() && dropProbeSource_.idle()) {
        pendingExternalDirectoryDrop_.reset();
        dropProbeTimer_->stop();
        resume_queued_external_open();
        return;
    }

    if (!pendingExternalDirectoryDrop_ || !pendingExternalDirectoryDrop_->awaiting_worker_reap ||
        !dropProbeSource_.idle()) {
        return;
    }
    pendingExternalDirectoryDrop_->awaiting_worker_reap = false;
    if (pendingExternalDirectoryDrop_->index < pendingExternalDirectoryDrop_->paths.size()) {
        const auto index = pendingExternalDirectoryDrop_->index;
        const auto generation = nextDropProbeGeneration_++;
        const auto next_path = native_path(pendingExternalDirectoryDrop_->paths.at(index));
        pendingExternalDirectoryDrop_->generation = generation;
        dropProbeSource_.submit(
            {.generation = generation, .path = next_path, .maximum_entries = 1});
        return;
    }

    auto completed = std::move(*pendingExternalDirectoryDrop_);
    pendingExternalDirectoryDrop_.reset();
    if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
    } else {
        dropProbeTimer_->start();
    }
    if (completed.intent == ExternalDirectoryDropIntent::open) {
        operationStatus_.clear();
        if (!resume_queued_external_open()) {
            open_path(completed.confirmed_paths.front(), true);
        }
    } else if (completed.intent == ExternalDirectoryDropIntent::pin) {
        for (const auto &path : completed.confirmed_paths) {
            add_favorite(path);
        }
        operationStatus_ = QCoreApplication::translate("MainWindow", "Added to Favorites: %1")
                               .arg(completed.confirmed_paths.size());
        update_status(lastUpdate_);
    } else if (!start_directory_transfer_paths(std::move(completed.confirmed_paths),
                                               std::move(completed.confirmed_revisions_utf8),
                                               completed.transfer_destination,
                                               completed.transfer_kind)) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to start folder transfer");
        update_status(lastUpdate_);
    }
    resume_queued_external_open();
}

void MainWindow::poll_external_transfer_drop() {
    if (!pendingExternalTransferDrop_) {
        return;
    }
    if (pendingExternalTransferDrop_->search_generation != 0 &&
        (!globalSearchActive_ || !globalSearchReady_ ||
         globalSearchQuery_ != globalSearchEdit_->text().trimmed() ||
         pendingExternalTransferDrop_->search_generation != activeGlobalSearchGeneration_)) {
        fail_external_transfer_drop(QCoreApplication::translate("MainWindow", "Transfer cancelled. Objects transferred: %1").arg(0));
        return;
    }
    if (pendingExternalTransferDrop_->search_generation != 0 &&
        std::chrono::steady_clock::now() >= pendingExternalTransferDrop_->search_deadline) {
        fail_external_transfer_drop(QCoreApplication::translate(
            "MainWindow", "A dragged object was not found or could not be checked"));
        return;
    }
    for (int batch_index{}; batch_index < 32; ++batch_index) {
        auto batch = dropProbeSource_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != pendingExternalTransferDrop_->generation) {
            continue;
        }
        if (batch->error) {
            switch (batch->error.kind) {
            case catalog::CatalogErrorKind::authentication_required:
                fail_external_transfer_drop(QCoreApplication::translate(
                    "MainWindow", "The source folder requires network sign-in"));
                break;
            case catalog::CatalogErrorKind::permission_denied:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "Cannot access the source folder"));
                break;
            case catalog::CatalogErrorKind::network_disconnected:
            case catalog::CatalogErrorKind::timed_out:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "The source folder did not respond"));
                break;
            case catalog::CatalogErrorKind::not_found:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "The source folder was not found"));
                break;
            case catalog::CatalogErrorKind::none:
            case catalog::CatalogErrorKind::io_error:
            case catalog::CatalogErrorKind::cancelled:
                fail_external_transfer_drop(
                    QCoreApplication::translate("MainWindow", "Failed to check dragged objects"));
                break;
            }
            return;
        }

        for (const auto &entry : batch->entries) {
            const auto expected_path = native_path(
                pendingExternalTransferDrop_->paths.at(pendingExternalTransferDrop_->index));
            if (transfer_path_key(native_path(QString::fromUtf8(entry.path_utf8))) ==
                transfer_path_key(expected_path)) {
                if (!pendingExternalTransferDrop_->search_entries.isEmpty() &&
                    (entry.kind != pendingExternalTransferDrop_->search_entries.at(
                         pendingExternalTransferDrop_->index).kind || entry.source_revision_utf8.empty())) {
                    fail_external_transfer_drop(QCoreApplication::translate(
                        "MainWindow", "A dragged object was not found or could not be checked"));
                    return;
                }
                pendingExternalTransferDrop_->confirmed_entries.push_back(entry);
            }
        }
        if (batch->is_final) {
            pendingExternalTransferDrop_->awaiting_worker_reap = true;
            break;
        }
    }

    if (!pendingExternalTransferDrop_ || !pendingExternalTransferDrop_->awaiting_worker_reap ||
        !dropProbeSource_.idle()) {
        return;
    }
    pendingExternalTransferDrop_->awaiting_worker_reap = false;
    if (pendingExternalTransferDrop_->confirmed_entries.size() !=
        pendingExternalTransferDrop_->index + 1) {
        fail_external_transfer_drop(QCoreApplication::translate(
            "MainWindow", "A dragged object was not found or could not be checked"));
        return;
    }
    ++pendingExternalTransferDrop_->index;
    if (pendingExternalTransferDrop_->index < pendingExternalTransferDrop_->paths.size()) {
        pendingExternalTransferDrop_->generation = nextDropProbeGeneration_++;
        const auto next_path = native_path(
            pendingExternalTransferDrop_->paths.at(pendingExternalTransferDrop_->index));
        dropProbeSource_.submit({.generation = pendingExternalTransferDrop_->generation,
                                 .path = next_path.parent_path(),
                                 .maximum_entries = 1,
                                 .exact_entry_path = next_path});
        return;
    }

    auto completed = std::move(*pendingExternalTransferDrop_);
    pendingExternalTransferDrop_.reset();
    dropProbeTimer_->stop();
    auto ordered_entries = std::move(completed.confirmed_entries);

    const auto all_trash_objects = std::ranges::all_of(ordered_entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
    });
    if (completed.trash_target) {
        if (!all_trash_objects) {
            fail_external_transfer_drop(QCoreApplication::translate(
                "MainWindow", "Only files and folders can be moved to VO-VE Trash"));
        } else {
            static_cast<void>(prompt_move_entries_to_trash(ordered_entries));
        }
        resume_queued_external_open();
        return;
    }
    const auto started = completed.clipboard_copy || !completed.search_entries.isEmpty()
        ? start_object_transfer(ordered_entries, completed.transfer_destination,
              completed.clipboard_copy ? fileops::FileTransferKind::copy : completed.transfer_kind)
        : start_transfer_selection(ordered_entries, completed.transfer_destination, completed.transfer_kind);
    if (!started) {
        fail_external_transfer_drop(
            QCoreApplication::translate("MainWindow", "Failed to start object transfer"));
    }
    resume_queued_external_open();
}

bool MainWindow::resume_queued_external_open() {
    if (queuedExternalOpenPaths_.isEmpty() || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        return false;
    }
    auto paths = std::move(queuedExternalOpenPaths_);
    queuedExternalOpenPaths_.clear();
    open_external_paths(paths);
    return true;
}

void MainWindow::fail_external_directory_drop(const QString &message) {
    if (pendingExternalDirectoryDrop_) {
        dropProbeSource_.cancel(pendingExternalDirectoryDrop_->generation);
    }
    pendingExternalDirectoryDrop_.reset();
    if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
    } else {
        dropProbeTimer_->start();
    }
    operationStatus_ = message;
    update_status(lastUpdate_);
    resume_queued_external_open();
}

void MainWindow::fail_external_transfer_drop(const QString &message) {
    if (pendingExternalTransferDrop_) {
        dropProbeSource_.cancel(pendingExternalTransferDrop_->generation);
    }
    pendingExternalTransferDrop_.reset();
    if (dropProbeSource_.idle()) {
        dropProbeTimer_->stop();
    } else {
        dropProbeTimer_->start();
    }
    operationStatus_ = message;
    update_status(lastUpdate_);
    resume_queued_external_open();
}

void MainWindow::fail_transfer_preparation(const QString &message) {
    if (pendingTransferPreparation_) {
        for (const auto &probe : pendingTransferPreparation_->probes) {
            if (!probe.complete) {
                transferProbeSource_.cancel(probe.generation);
            }
        }
    }
    pendingTransferPreparation_.reset();
    transferProbeTimer_->stop();
    set_transfer_in_flight(false);
    operationStatus_ = message;
    update_status(lastUpdate_);
}

void MainWindow::launch_prepared_file_transfer() {
    if (!pendingTransferPreparation_) {
        return;
    }
    auto pending = std::move(*pendingTransferPreparation_);
    pendingTransferPreparation_.reset();
    transferProbeTimer_->stop();
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        set_transfer_in_flight(false);
        update_status(lastUpdate_);
        return;
    }
    const auto revision_for = [&pending](const std::filesystem::path &path) -> std::string {
        const auto key = transfer_path_key(path.lexically_normal());
        const auto match = std::ranges::find_if(pending.probes, [&key](const auto &probe) {
            return transfer_path_key(probe.path) == key;
        });
        return match == pending.probes.end() ? std::string{} : match->revision_utf8;
    };

    std::vector<fileops::FileTransferSource> sources;
    sources.reserve(static_cast<std::size_t>(pending.entries.size()));
    QSet<QString> destinations;
    for (const auto &entry : pending.entries) {
        const auto source = native_path(QString::fromUtf8(entry.path_utf8));
        const auto destination = pending.destination_directory / source.filename();
        const auto destination_key = transfer_path_key(destination);
        if (source.empty() || destination.empty() || transfer_path_key(source) == destination_key ||
            destinations.contains(destination_key)) {
            fail_transfer_preparation(QCoreApplication::translate(
                "MainWindow", "Source and destination paths are identical or duplicated"));
            return;
        }
        destinations.insert(destination_key);
        sources.push_back(
            {.path = source,
             .destination = destination,
             .snapshot = {.size_bytes = entry.size_bytes,
                          .modified_unix_ns = entry.modified_unix_ns,
                          .source_revision_utf8 = entry.source_revision_utf8},
             .source_parent_revision_utf8 = revision_for(source.parent_path()),
             .destination_parent_revision_utf8 = revision_for(pending.destination_directory)});
    }
    if (std::ranges::any_of(sources, [](const auto &source) {
            return source.source_parent_revision_utf8.empty() ||
                   source.destination_parent_revision_utf8.empty();
        })) {
        fail_transfer_preparation(
            QCoreApplication::translate("MainWindow", "Failed to verify the operation folders"));
        return;
    }

    activeTransferKind_ = pending.kind;
    activeFileTransferSources_.clear();
    for (const auto &entry : pending.entries)
        activeFileTransferSources_.push_back(QString::fromUtf8(entry.path_utf8));
    operationStatus_ = pending.kind == fileops::FileTransferKind::copy
                           ? QCoreApplication::translate("MainWindow", "Copying: preparing…")
                           : QCoreApplication::translate("MainWindow", "Moving: preparing…");
    update_status(lastUpdate_);
    const QPointer<MainWindow> window(this);
    const auto submitted = fileTransferCoordinator_.start(
        pending.kind, std::move(sources),
        [window](fileops::FileTransferProgressUpdate progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_file_transfer_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::FileTransferResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_file_transfer_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::FileTransferConflict conflict) {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, conflict = std::move(conflict)] {
                        if (!window.isNull()) {
                            window->handle_file_transfer_conflict(conflict);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
    }
}

void MainWindow::show_catalog_context_menu(const QPoint &position) {
    if (trashViewActive_) {
        show_trash_context_menu(position);
        return;
    }
    const auto clicked = listView_->indexAt(position);
    if (!clicked.isValid()) {
        QMenu menu(listView_);
        update_clipboard_actions();
        menu.addAction(pasteObjectsAction_);
        if (!globalSearchActive_ && !recursiveViewActive_) {
            menu.addAction(createFolderAction_);
        }
        menu.exec(listView_->viewport()->mapToGlobal(position));
        return;
    }
    if (clicked.isValid() && !listView_->selectionModel()->isSelected(clicked)) {
        listView_->selectionModel()->setCurrentIndex(clicked, QItemSelectionModel::ClearAndSelect);
    }
    const auto entries = selected_entries_in_view_order();
    if (entries.isEmpty()) {
        return;
    }
    const bool all_deletable_objects = std::ranges::all_of(entries, [](const auto &entry) {
        return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
    });
    update_rename_action();
    copyNamesAction_->setText(entries.size() == 1
                                  ? QCoreApplication::translate("MainWindow", "Copy name")
                                  : QCoreApplication::translate("MainWindow", "Copy names"));
    copyPathsAction_->setText(entries.size() == 1
                                  ? QCoreApplication::translate("MainWindow", "Copy full path")
                                  : QCoreApplication::translate("MainWindow", "Copy full paths"));

    QMenu menu(listView_);
    if (entries.size() == 1) {
        if (entries.front().kind == core::EntryKind::file) {
            auto *force_preview =
                menu.addAction(QCoreApplication::translate("MainWindow", "Force preview"));
            force_preview->setObjectName(QStringLiteral("forcePreviewAttemptAction"));
            force_preview->setEnabled(can_force_preview(entries.front()));
            connect(force_preview, &QAction::triggered, this,
                    [this, id = static_cast<qulonglong>(entries.front().id)] {
                        this->force_preview(id);
                    });
            if (selectedPreviewExtended_) {
                menu.addAction(cancelPreviewAttemptAction_);
            }
            menu.addSeparator();
        }
        auto default_application =
            default_application_name(QString::fromUtf8(entries.front().path_utf8));
        default_application.replace(QLatin1Char('&'), QStringLiteral("&&"));
        const auto open_label =
            entries.front().kind == core::EntryKind::file
                ? QCoreApplication::translate("MainWindow", "Open in %1")
                      .arg(default_application.isEmpty()
                               ? QCoreApplication::translate("MainWindow", "default application")
                               : default_application)
                : QCoreApplication::translate("MainWindow", "Open");
        auto *open = menu.addAction(open_label);
        open->setObjectName(QStringLiteral("openDefaultAction"));
        connect(open, &QAction::triggered, this,
                [this, entry = entries.front()] { open_entry(entry); });
        if (entries.front().kind == core::EntryKind::file) {
            auto *open_with = menu.addMenu(QCoreApplication::translate("MainWindow", "Open with"));
            open_with->setObjectName(QStringLiteral("openWithMenu"));
            for (const auto &application : externalApplications_.applications()) {
                auto label = application.name;
                label.replace(QLatin1Char('&'), QStringLiteral("&&"));
                auto *action =
                    open_with->addAction(stored_application_icon(application.icon_png), label);
                action->setObjectName(QStringLiteral("openWithApplicationAction"));
                action->setData(application.path);
                connect(action, &QAction::triggered, this,
                        [this, application, path = QString::fromUtf8(entries.front().path_utf8)] {
                            open_file_with_application(application, path);
                        });
            }
            if (!externalApplications_.applications().isEmpty()) {
                open_with->addSeparator();
            }
            auto *add_application =
                open_with->addAction(QCoreApplication::translate("MainWindow", "Add Application…"));
            add_application->setObjectName(QStringLiteral("addExternalApplicationAction"));
            connect(add_application, &QAction::triggered, this,
                    [this, path = QString::fromUtf8(entries.front().path_utf8)] {
                        prompt_add_external_application(path);
                    });
            menu.addSeparator();
            auto *reveal = menu.addAction(
#ifdef _WIN32
                QCoreApplication::translate("MainWindow", "Show in File Explorer")
#else
                QCoreApplication::translate("MainWindow", "Show in File Manager")
#endif
            );
            reveal->setObjectName(QStringLiteral("revealInFileManagerAction"));
            connect(reveal, &QAction::triggered, this,
                    [this, path = QString::fromUtf8(entries.front().path_utf8)] {
                        reveal_file_in_file_manager(path);
                    });
        }
        if (globalSearchActive_ || recursiveViewActive_) {
            auto *show_in_catalog =
                menu.addAction(QCoreApplication::translate("MainWindow", "Show in Folder"));
            connect(show_in_catalog, &QAction::triggered, this,
                    [this, entry = entries.front()] { show_entry_in_catalog(entry); });
        }
        menu.addSeparator();
    }
    const auto recursive_ready = !recursiveViewActive_ ||
                                 lastUpdate_.state == catalog::CatalogSessionState::ready;
    if (all_deletable_objects && !globalSearchActive_ && recursive_ready) {
        menu.addAction(renameAction_);
        menu.addAction(deleteAction_);
        menu.addAction(QCoreApplication::translate("MainWindow", "Copy to Folder…"), this,
                       [this] { prompt_transfer_selected(fileops::FileTransferKind::copy); });
        menu.addAction(QCoreApplication::translate("MainWindow", "Move to Folder…"), this,
                       [this] { prompt_transfer_selected(fileops::FileTransferKind::move); });
        menu.addSeparator();
    }
    menu.addAction(copyObjectsAction_);
    menu.addAction(pasteObjectsAction_);
    menu.addSeparator();
    menu.addAction(copyNamesAction_);
    menu.addAction(copyPathsAction_);
    menu.exec(listView_->viewport()->mapToGlobal(position));
}

void MainWindow::prompt_create_directory() {
    if (createDirectoryInFlight_ || renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
        return;
    }
    const auto parent = native_path(currentPath_).lexically_normal();
    bool accepted{};
    const auto name =
        QInputDialog::getText(this, QCoreApplication::translate("MainWindow", "Create Folder"),
                              QCoreApplication::translate("MainWindow", "Folder name:"),
                              QLineEdit::Normal,
                              QCoreApplication::translate("MainWindow", "New folder"), &accepted)
            .trimmed();
    if (!accepted || name.isEmpty()) {
        return;
    }
    std::string validation;
    const auto filename = native_path(name);
    if (!fileops::valid_destination_filename(filename, validation)) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Invalid folder name");
        update_status(lastUpdate_);
        return;
    }
    if (createDirectoryInFlight_ || renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        native_path(currentPath_).lexically_normal() != parent ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The folder changed. Retry creating the folder");
        update_status(lastUpdate_);
        return;
    }

    const auto generation = nextTransferProbeGeneration_++;
    pendingCreateDirectoryPreparation_ = {.generation = generation,
                                          .destination = (parent / filename).lexically_normal()};
    set_create_directory_in_flight(true);
    operationStatus_ = QCoreApplication::translate("MainWindow", "Checking current folder…");
    update_status(lastUpdate_);
    transferProbeSource_.submit({.generation = generation, .path = parent, .maximum_entries = 1});
    transferProbeTimer_->start();
}

void MainWindow::prepare_directory_entries(
    QList<core::DirectoryEntry> entries, QList<qsizetype> indices,
    std::function<void(QList<core::DirectoryEntry>)> ready, const bool trash_target) {
    if (pendingDirectoryPreparation_ || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || !dropProbeSource_.idle()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
        return;
    }
    if (indices.isEmpty()) {
        ready(std::move(entries));
        return;
    }
    PendingDirectoryPreparation pending;
    pending.entries = std::move(entries);
    pending.indices = std::move(indices);
    pending.ready = std::move(ready);
    pending.trash_target = trash_target;
    pending.readers_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    pending.generation = nextDropProbeGeneration_++;
    const auto path = native_path(QString::fromUtf8(pending.entries.at(pending.indices.front()).path_utf8));
    const auto generation = pending.generation;
    pendingDirectoryPreparation_ = std::move(pending);
    renamePreviewSuspended_ = true;
    begin_preview_generation(false);
    previewTimer_->stop();
    set_rename_in_flight(true);
    operationStatus_ = trash_target
        ? QCoreApplication::translate("MainWindow", "VO-VE Trash: preparing…")
        : QCoreApplication::translate("MainWindow", "Batch rename: preparing…");
    update_status(lastUpdate_);
    dropProbeSource_.submit({.generation = generation, .path = path.parent_path(),
                             .maximum_entries = 1, .exact_entry_path = path});
    dropProbeTimer_->start();
}

void MainWindow::poll_directory_preparation() {
    if (!pendingDirectoryPreparation_) return;
    if ((!folderMosaicController_.readers_idle() || !previewClient_.readers_idle()) &&
        std::chrono::steady_clock::now() >= pendingDirectoryPreparation_->readers_deadline) {
        fail_directory_preparation();
        return;
    }
    for (int batch_index{}; batch_index < 32; ++batch_index) {
        auto batch = dropProbeSource_.poll();
        if (!batch) break;
        auto &pending = *pendingDirectoryPreparation_;
        if (batch->generation != pending.generation) continue;
        if (batch->error) {
            fail_directory_preparation();
            return;
        }
        auto &expected = pending.entries[pending.indices.at(pending.index)];
        for (const auto &entry : batch->entries) {
            // Directory enumeration metadata can lag behind the handle snapshot on Windows.
            // Refresh metadata only for the same selected native object; mutation checks stay strict.
            if (pending.received || entry.kind != core::EntryKind::directory ||
                entry.path_utf8 != expected.path_utf8 ||
                !fileops::same_object_identity(entry.source_revision_utf8, expected.source_revision_utf8)) {
                fail_directory_preparation();
                return;
            }
            expected = entry;
            pending.received = true;
        }
        if (batch->is_final) {
            if (!pending.received) {
                fail_directory_preparation();
                return;
            }
            pending.awaiting_worker_reap = true;
            break;
        }
    }
    if (!pendingDirectoryPreparation_ || !pendingDirectoryPreparation_->awaiting_worker_reap ||
        !dropProbeSource_.idle()) return;
    if (!folderMosaicController_.readers_idle() || !previewClient_.readers_idle()) return;
    auto &pending = *pendingDirectoryPreparation_;
    ++pending.index;
    if (pending.index < pending.indices.size()) {
        pending.generation = nextDropProbeGeneration_++;
        pending.received = false;
        pending.awaiting_worker_reap = false;
        const auto path = native_path(QString::fromUtf8(pending.entries.at(pending.indices.at(pending.index)).path_utf8));
        dropProbeSource_.submit({.generation = pending.generation, .path = path.parent_path(),
                                 .maximum_entries = 1, .exact_entry_path = path});
        return;
    }
    auto completed = std::move(pending);
    pendingDirectoryPreparation_.reset();
    dropProbeTimer_->stop();
    completed.ready(std::move(completed.entries));
    resume_queued_external_open();
}

void MainWindow::fail_directory_preparation() {
    const auto trash_target = pendingDirectoryPreparation_ && pendingDirectoryPreparation_->trash_target;
    if (pendingDirectoryPreparation_) dropProbeSource_.cancel(pendingDirectoryPreparation_->generation);
    pendingDirectoryPreparation_.reset();
    set_rename_in_flight(false);
    operationStatus_ = trash_target
        ? QCoreApplication::translate("MainWindow", "The file list changed. Retry deletion")
        : QCoreApplication::translate("MainWindow", "The file list changed. Retry renaming");
    update_status(lastUpdate_);
    if (dropProbeSource_.idle()) dropProbeTimer_->stop();
    else dropProbeTimer_->start();
    resume_queued_external_open();
}

void MainWindow::prompt_rename_selected() {
    const auto entries = selected_entries_in_view_order();
    if (entries.isEmpty() || !std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        })) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }
    if (entries.size() > 1) {
        prompt_batch_rename_selected(entries);
        return;
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        batchRenameCoordinator_.recovery_pending() || fileTransferCoordinator_.recovery_pending() ||
        trashCoordinator_.recovery_pending()) {
        return;
    }
    const auto &entry = entries.front();
    const auto source_path = QString::fromUtf8(entry.path_utf8);
    const QFileInfo source_info(source_path);
    const auto directory = entry.kind == core::EntryKind::directory;
    RenameDialog dialog(source_info.fileName(), this, directory);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const auto destination_name = dialog.file_name();
    if (destination_name.isEmpty() || destination_name == source_info.fileName()) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }
    const auto current_entries = selected_entries_in_view_order();
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        batchRenameCoordinator_.recovery_pending() || fileTransferCoordinator_.recovery_pending() ||
        trashCoordinator_.recovery_pending() || current_entries.size() != 1 ||
        current_entries.front().kind != entry.kind ||
        current_entries.front().path_utf8 != entry.path_utf8 ||
        current_entries.front().size_bytes != entry.size_bytes ||
        current_entries.front().modified_unix_ns != entry.modified_unix_ns ||
        current_entries.front().source_revision_utf8 != entry.source_revision_utf8) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file list changed. Retry renaming");
        update_status(lastUpdate_);
        return;
    }
    prepare_directory_entries(entries, directory ? QList<qsizetype>{0} : QList<qsizetype>{},
                           [this, destination_name](QList<core::DirectoryEntry> prepared) {
                               submit_single_rename(prepared.front(), destination_name);
                           });
}

void MainWindow::submit_single_rename(const core::DirectoryEntry &entry,
                                      const QString &destination_name) {
    const auto source_path = QString::fromUtf8(entry.path_utf8);
    const auto directory = entry.kind == core::EntryKind::directory;
    const auto destination_path = QFileInfo(source_path).dir().filePath(destination_name);
    static std::atomic_uint64_t next_operation{1};
    fileops::RenameRequest request{
        .operation_id = next_operation.fetch_add(1, std::memory_order_relaxed),
        .mode = !directory && native_path(source_path).extension() == native_path(destination_path).extension()
                    ? fileops::RenameMode::single_preserve_extension
                    : fileops::RenameMode::single_allow_extension_change,
        .object_kind = directory ? fileops::OperationObjectKind::directory
                                 : fileops::OperationObjectKind::regular_file,
        .source = native_path(source_path),
        .destination = native_path(destination_path),
        .expected_source = {.size_bytes = entry.size_bytes,
                            .modified_unix_ns = entry.modified_unix_ns,
                            .source_revision_utf8 = entry.source_revision_utf8},
        .source_parent_identity_utf8 = {},
        .destination_parent_identity_utf8 = {},
        .destination_anchor_path = {},
        .destination_anchor_identity_utf8 = {}};
    PendingRenameOperation operation{
        .request = request, .source_path = source_path, .destination_path = destination_path};
    auto submitted_request = operation.request;
    operationStatus_ = QCoreApplication::translate("MainWindow", "Renaming…");
    update_status(lastUpdate_);
    const QPointer<MainWindow> window(this);
    set_rename_in_flight(true);
    if (!fileOperationService_.submit_rename(
            std::move(submitted_request),
            [window, operation = std::move(operation)](fileops::OperationResult result) mutable {
                if (window.isNull()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result),
                     operation = std::move(operation)]() mutable {
                        if (!window.isNull()) {
                            window->handle_rename_result(result, std::move(operation));
                        }
                    },
                    Qt::QueuedConnection);
            })) {
        set_rename_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
    }
}

void MainWindow::prompt_batch_rename_selected(const QList<core::DirectoryEntry> &entries) {
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }
    if (entries.size() < 2 || renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
        createDirectoryInFlight_ || fileTransferCoordinator_.busy() ||
        fileOperationService_.busy() || pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy()) {
        return;
    }

    std::vector<fileops::BatchRenameSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back({.path = native_path(QString::fromUtf8(entry.path_utf8)),
                           .size_bytes = entry.size_bytes,
                           .modified_unix_ns = entry.modified_unix_ns,
                           .source_revision_utf8 = entry.source_revision_utf8,
                           .object_kind = entry.kind == core::EntryKind::directory
                                              ? fileops::OperationObjectKind::directory
                                              : fileops::OperationObjectKind::regular_file});
    }
    const auto parent = QFileInfo(QString::fromUtf8(entries.front().path_utf8)).absolutePath();
    if (!std::ranges::all_of(entries, [&](const auto &entry) {
            return same_directory_path(parent, QFileInfo(QString::fromUtf8(entry.path_utf8)).absolutePath());
        })) return;
    BatchRenameDialog dialog(sources, listModel_.all_entry_names(recursiveViewActive_ ? parent : QString{}), this);
    if (dialog.exec() != QDialog::Accepted || !dialog.plan().valid()) {
        return;
    }
    if (std::ranges::all_of(dialog.plan().rows, [](const auto &row) {
            return row.source == row.destination;
        })) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Names unchanged");
        update_status(lastUpdate_);
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }

    const auto current_entries = selected_entries_in_view_order();
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        batchRenameCoordinator_.recovery_pending() || fileTransferCoordinator_.recovery_pending() ||
        trashCoordinator_.recovery_pending() || !same_entry_snapshots(entries, current_entries)) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file list changed. Retry renaming");
        update_status(lastUpdate_);
        return;
    }

    QList<qsizetype> directory_indices;
    for (qsizetype index{}; index < entries.size(); ++index) {
        const auto &row = dialog.plan().rows.at(static_cast<std::size_t>(index));
        if (entries.at(index).kind == core::EntryKind::directory && row.source != row.destination) {
            directory_indices.push_back(index);
        }
    }
    prepare_directory_entries(entries, std::move(directory_indices),
                           [this, plan = dialog.plan()](QList<core::DirectoryEntry> prepared) {
                               submit_batch_rename(plan, prepared);
                           });
}

void MainWindow::submit_batch_rename(const fileops::BatchRenamePlan &plan,
                                     const QList<core::DirectoryEntry> &entries) {
    std::vector<fileops::BatchRenameSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back({.path = native_path(QString::fromUtf8(entry.path_utf8)),
                           .size_bytes = entry.size_bytes,
                           .modified_unix_ns = entry.modified_unix_ns,
                           .source_revision_utf8 = entry.source_revision_utf8,
                           .object_kind = entry.kind == core::EntryKind::directory
                                              ? fileops::OperationObjectKind::directory
                                              : fileops::OperationObjectKind::regular_file});
    }
    activeBatchSourcePaths_.clear();
    activeBatchSourcePaths_.reserve(entries.size());
    activeBatchUnchangedPaths_.clear();
    for (const auto &row : plan.rows) {
        if (row.source == row.destination) activeBatchUnchangedPaths_.push_back(display_path(row.source));
    }
    for (const auto &entry : entries) {
        activeBatchSourcePaths_.push_back(QString::fromUtf8(entry.path_utf8));
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Batch rename: preparing…");
    update_status(lastUpdate_);
    set_rename_in_flight(true);
    const QPointer<MainWindow> window(this);
    const auto submitted = batchRenameCoordinator_.start(
        plan, std::move(sources),
        [window](fileops::BatchRenameProgress progress) mutable {
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, progress = std::move(progress)]() mutable {
                    if (!window.isNull()) {
                        window->handle_batch_rename_progress(progress);
                    }
                },
                Qt::QueuedConnection);
        },
        [window](fileops::BatchRenameResult result) mutable {
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, result = std::move(result)]() mutable {
                    if (!window.isNull()) {
                        window->handle_batch_rename_result(result);
                    }
                },
                Qt::QueuedConnection);
        });
    if (!submitted) {
        activeBatchSourcePaths_.clear();
        set_rename_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
    }
}

bool MainWindow::prompt_move_entries_to_trash(const QList<core::DirectoryEntry> &entries) {
    const auto all_deletable_objects =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        });
    if (!all_deletable_objects) {
        return false;
    }
#ifdef Q_OS_WIN
    const auto all_local = std::ranges::all_of(entries, [](const auto &entry) {
        return entry.source_revision_utf8.starts_with("win-file128:") &&
               !is_unc_path(QString::fromUtf8(entry.path_utf8));
    });
    if (!all_local) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash is unavailable for this resource");
        update_status(lastUpdate_);
        return false;
    }
#endif
    if (trashConfirmationInProgress_ ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        directoryTransferService_.busy() || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        update_status(lastUpdate_);
        return false;
    }

    QList<qsizetype> directory_indices;
    for (qsizetype index{}; index < entries.size(); ++index) {
        if (entries.at(index).kind == core::EntryKind::directory) {
            directory_indices.push_back(index);
        }
    }
    if (!directory_indices.isEmpty()) {
        prepare_directory_entries(entries, std::move(directory_indices),
            [this](QList<core::DirectoryEntry> prepared) {
                set_rename_in_flight(false);
                static_cast<void>(confirm_and_start_trash_move(prepared));
            }, true);
        return true;
    }
    return confirm_and_start_trash_move(entries);
}

bool MainWindow::confirm_and_start_trash_move(const QList<core::DirectoryEntry> &entries) {
    if (entries.isEmpty() || trashConfirmationInProgress_) {
        return false;
    }

    std::uint64_t total_bytes{};
    bool contains_directory = false;
    for (const auto &entry : entries) {
        total_bytes += entry.kind == core::EntryKind::file ? entry.size_bytes : 0U;
        contains_directory = contains_directory || entry.kind == core::EntryKind::directory;
    }
    trashConfirmationInProgress_ = true;
    setProperty("trashConfirmationInProgress", true);
    update_rename_action();
    QMessageBox confirmation(
        QMessageBox::Question, QCoreApplication::translate("MainWindow", "Move to VO-VE Trash?"),
        QCoreApplication::translate("MainWindow", "Objects: %1 · folders: %2 · files: %3")
            .arg(entries.size())
            .arg(std::ranges::count_if(
                entries,
                [](const auto &entry) { return entry.kind == core::EntryKind::directory; }))
            .arg(std::ranges::count_if(
                entries, [](const auto &entry) { return entry.kind == core::EntryKind::file; })),
        QMessageBox::NoButton, this);
    confirmation.setObjectName(QStringLiteral("trashConfirmation"));
    if (!contains_directory) {
        confirmation.setInformativeText(
            QCoreApplication::translate("MainWindow", "Total size: %1")
                .arg(QLocale().formattedDataSize(static_cast<qint64>(total_bytes))));
    }
    auto *move = confirmation.addButton(QCoreApplication::translate("MainWindow", "Move to Trash"),
                                        QMessageBox::AcceptRole);
    move->setObjectName(QStringLiteral("moveToTrashConfirmButton"));
    confirmation.addButton(QCoreApplication::translate("MainWindow", "Cancel"),
                           QMessageBox::RejectRole);
    confirmation.exec();
    trashConfirmationInProgress_ = false;
    setProperty("trashConfirmationInProgress", false);
    update_rename_action();
    if (confirmation.clickedButton() != move ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        directoryTransferService_.busy() || pendingExternalDirectoryDrop_ ||
        pendingExternalTransferDrop_ || pendingDirectoryPreparation_ || !dropProbeSource_.idle()) {
        return false;
    }

    return submit_trash_move(entries);
}

bool MainWindow::submit_trash_move(const QList<core::DirectoryEntry> &entries) {
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none ||
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy()) {
        update_status(lastUpdate_);
        return false;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "VO-VE Trash: preparing…");
    set_delete_in_flight(true);
    update_status(lastUpdate_);
    previewTimer_->stop();
    begin_preview_generation();
    previewClient_.stop();

    std::vector<fileops::TrashSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back(
            {.path = native_path(QString::fromUtf8(entry.path_utf8)),
             .snapshot = {.size_bytes = entry.kind == core::EntryKind::file ? entry.size_bytes : 0U,
                          .modified_unix_ns = entry.modified_unix_ns,
                          .source_revision_utf8 = entry.source_revision_utf8},
             .kind = entry.kind == core::EntryKind::directory
                         ? fileops::TrashItemKind::directory
                         : fileops::TrashItemKind::regular_file,
             .payload_bytes = entry.kind == core::EntryKind::file ? entry.size_bytes : 0U,
             .directory_entries = {},
             .directory_security_descriptors_sddl_utf8 = {},
             .storage_identity_utf8 = {},
             .original_security_descriptor_sddl_utf8 = {},
             .restore_path = {}});
    }
    const QPointer<MainWindow> window(this);
    const auto submitted = trashCoordinator_.start(
        std::move(sources),
        [window](fileops::TrashProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::TrashResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_result(result, TrashOperationIntent::move);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
        previewTimer_->start(0);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
    }
    return submitted;
}

void MainWindow::prompt_delete_selected() {
    const auto entries = selected_entries_in_view_order();
    const auto all_deletable_objects =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        });
    if (!all_deletable_objects) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_ || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy()) {
        return;
    }

#ifdef Q_OS_WIN
    const auto is_remote = [](const auto &entry) {
        return entry.source_revision_utf8.starts_with("win-smb64:") ||
               is_unc_path(QString::fromUtf8(entry.path_utf8));
    };
    const auto is_local = [](const auto &entry) {
        return entry.source_revision_utf8.starts_with("win-file128:") &&
               !is_unc_path(QString::fromUtf8(entry.path_utf8));
    };
    const auto all_remote = std::ranges::all_of(entries, is_remote);
    const auto any_remote = std::ranges::any_of(entries, is_remote);
    const auto all_local = std::ranges::all_of(entries, is_local);
    const auto any_directory = std::ranges::any_of(
        entries, [](const auto &entry) { return entry.kind == core::EntryKind::directory; });
#else
    // POSIX uses the private same-parent VO-VE Trash for local and mounted network files.
    // The helper verifies the supplied identity and performs the rename inside the source
    // filesystem, so a mounted SMB source remains recoverable without a network copy.
    constexpr bool all_remote = false;
    constexpr bool any_remote = false;
    constexpr bool all_local = true;
    constexpr bool any_directory = false;
#endif
    std::uint64_t total_bytes{};
    for (const auto &entry : entries) {
        total_bytes += entry.size_bytes;
    }
    if (!all_remote) {
        if (any_remote) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Local and network files must be deleted in separate operations");
            update_status(lastUpdate_);
            return;
        }
        if (!all_local) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow",
                "The storage type could not be determined reliably. Deletion was not started");
            update_status(lastUpdate_);
            return;
        }
        static_cast<void>(prompt_move_entries_to_trash(entries));
        return;
    }
    if (any_directory) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash is unavailable for network folders on Windows");
        update_status(lastUpdate_);
        return;
    }

    QMessageBox confirmation(
        QMessageBox::Warning, QCoreApplication::translate("MainWindow", "Delete permanently?"),
        QCoreApplication::translate("MainWindow", "Delete files: %1").arg(entries.size()),
        QMessageBox::NoButton, this);
    confirmation.setObjectName(QStringLiteral("permanentDeleteConfirmation"));
    confirmation.setInformativeText(
        QCoreApplication::translate(
            "MainWindow",
            "Path: %1\nTotal size: %2\nTrash is not guaranteed on a network resource. "
            "This deletion cannot be undone.")
            .arg(QDir::toNativeSeparators(currentPath_))
            .arg(QLocale().formattedDataSize(static_cast<qint64>(total_bytes))));
    auto *acknowledge =
        new QCheckBox(QCoreApplication::translate(
                          "MainWindow", "I understand that the files will be deleted permanently"),
                      &confirmation);
    acknowledge->setObjectName(QStringLiteral("permanentDeleteAcknowledge"));
    confirmation.setCheckBox(acknowledge);
    auto *remove =
        confirmation.addButton(QCoreApplication::translate("MainWindow", "Delete permanently"),
                               QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("permanentDeleteConfirmButton"));
    remove->setEnabled(false);
    confirmation.addButton(QCoreApplication::translate("MainWindow", "Cancel"),
                           QMessageBox::RejectRole);
    connect(acknowledge, &QCheckBox::toggled, remove, &QPushButton::setEnabled);
    confirmation.exec();
    if (confirmation.clickedButton() != remove || !acknowledge->isChecked()) {
        return;
    }
    if (refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return;
    }

    const auto current_entries = selected_entries_in_view_order();
    if (!same_entry_snapshots(entries, current_entries) || renameInFlight_ || deleteInFlight_ ||
        transferInFlight_ || createDirectoryInFlight_ || fileTransferCoordinator_.busy() ||
        batchRenameCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy() || batchRenameCoordinator_.recovery_pending() ||
        fileTransferCoordinator_.recovery_pending()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "The file list changed. Retry deletion");
        update_status(lastUpdate_);
        return;
    }

    std::vector<fileops::PermanentDeleteSource> sources;
    sources.reserve(static_cast<std::size_t>(entries.size()));
    for (const auto &entry : entries) {
        sources.push_back({.path = native_path(QString::fromUtf8(entry.path_utf8)),
                           .snapshot = {.size_bytes = entry.size_bytes,
                                        .modified_unix_ns = entry.modified_unix_ns,
                                        .source_revision_utf8 = entry.source_revision_utf8}});
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Safe deletion: preparing…");
    set_delete_in_flight(true);
    update_status(lastUpdate_);
    const QPointer<MainWindow> window(this);
    const auto submitted = permanentDeleteCoordinator_.start(
        std::move(sources),
        [window](fileops::PermanentDeleteProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_delete_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::PermanentDeleteResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_delete_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
    }
}

void MainWindow::handle_rename_result(const fileops::OperationResult &result,
                                      PendingRenameOperation operation) {
    notify_file_in_use(result.status);
    operationStatus_ = operation_status_text(result.status);
    if (result.ok()) {
        refresh_after_rename(operation.destination_path);
    } else if (confirms_no_committed_rename(result)) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Rename failed");
        refresh_after_rename(operation.source_path);
    } else if (ambiguous_rename_status(result.status)) {
        const auto source_path = operation.source_path;
        pendingRenameOperation_ = std::move(operation);
        setProperty("pendingRenameReconciliation", true);
        begin_preview_generation(false);
        const auto source = index_for_path(source_path);
        if (source.isValid()) {
            listModel_.mark_preview_verification_pending(
                source.data(DirectoryListModel::EntryIdRole).toULongLong());
        }
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Rename result is unverified. Press F5 to check");
    }
    set_rename_in_flight(false);
    update_status(lastUpdate_);
}

void MainWindow::handle_create_directory_result(const fileops::OperationResult &result,
                                                const QString &destination) {
    set_create_directory_in_flight(false);
    if (result.ok()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Folder created");
        if (transfer_path_key(native_path(destination).parent_path()) ==
            transfer_path_key(native_path(currentPath_))) {
            refresh_after_rename(destination);
        }
    } else if (result.status == fileops::OperationStatus::conflict) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "A folder with this name already exists");
        refresh_directory_sources();
        pollTimer_->start();
    } else if (result.status == fileops::OperationStatus::source_changed) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The current folder changed. Refresh it and try again");
        refresh_directory_sources();
        pollTimer_->start();
    } else if (result.status == fileops::OperationStatus::permission_denied) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "No permission to create a folder");
    } else if (result.status == fileops::OperationStatus::authentication_required) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Network folder sign-in required");
    } else if (result.status == fileops::OperationStatus::invalid_request) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Invalid folder name");
    } else if (result.status == fileops::OperationStatus::unsupported) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Safe folder creation is not available on this system yet");
    } else if (result.evidence == fileops::OperationEvidence::committed) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder created, but the directory is currently unavailable");
    } else if (result.evidence == fileops::OperationEvidence::no_commit) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Folder was not created");
    } else {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder creation could not be verified. Refresh the folder");
    }
    update_status(lastUpdate_);
}

bool MainWindow::submit_pending_rename_reconciliation() {
    const auto pending = pendingRenameOperation_;
    if (!pending) {
        return false;
    }
    if (renameInFlight_ || fileOperationService_.busy()) {
        return false;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Checking rename result…");
    update_status(lastUpdate_);
    const QPointer<MainWindow> window(this);
    const auto request = pending->request;
    set_rename_in_flight(true);
    const auto submitted = fileOperationService_.submit_reconciliation(
        request, [window](fileops::OperationResult result) mutable {
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, result = std::move(result)]() mutable {
                    if (!window.isNull()) {
                        window->handle_rename_reconciliation_result(result);
                    }
                },
                Qt::QueuedConnection);
        });
    if (!submitted) {
        set_rename_in_flight(false);
    }
    return submitted;
}

void MainWindow::handle_rename_reconciliation_result(const fileops::OperationResult &result) {
    if (!pendingRenameOperation_) {
        return;
    }
    if (result.ok()) {
        auto destination = std::move(pendingRenameOperation_->destination_path);
        pendingRenameOperation_.reset();
        setProperty("pendingRenameReconciliation", false);
        operationStatus_ = operation_status_text(result.status);
        refresh_after_rename(destination);
    } else if (confirms_no_committed_rename(result)) {
        auto source = std::move(pendingRenameOperation_->source_path);
        pendingRenameOperation_.reset();
        setProperty("pendingRenameReconciliation", false);
        operationStatus_ = QCoreApplication::translate("MainWindow", "Rename failed");
        refresh_after_rename(source);
    } else if (result.status == fileops::OperationStatus::conflict ||
               result.status == fileops::OperationStatus::source_changed) {
        auto source = std::move(pendingRenameOperation_->source_path);
        pendingRenameOperation_.reset();
        setProperty("pendingRenameReconciliation", false);
        operationStatus_ = operation_status_text(result.status);
        refresh_after_rename(source);
    } else {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The result is still unverified. Check the network and press F5");
    }
    set_rename_in_flight(false);
    update_status(lastUpdate_);
}

void MainWindow::handle_batch_rename_progress(const fileops::BatchRenameProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::BatchTransactionPhase::prepared:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::BatchTransactionPhase::evacuating:
        phase = QCoreApplication::translate("MainWindow", "releasing names");
        break;
    case fileops::BatchTransactionPhase::rollback:
        phase = QCoreApplication::translate("MainWindow", "rolling back changes");
        break;
    case fileops::BatchTransactionPhase::commit_intent:
        phase = QCoreApplication::translate("MainWindow", "committing");
        break;
    case fileops::BatchTransactionPhase::publishing:
        phase = QCoreApplication::translate("MainWindow", "new names");
        break;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Batch rename: %1 · %2 of %3")
                           .arg(phase)
                           .arg(progress.completed)
                           .arg(progress.total);
    update_status(lastUpdate_);
}

void MainWindow::handle_batch_rename_result(const fileops::BatchRenameResult &result) {
    notify_file_in_use(result.operation_status);
#ifdef VOVE_UI_TEST_HOOKS
    setProperty("batchRenameDetail", QString::fromStdString(result.detail_utf8));
#endif
    const auto needs_recovery = batchRenameCoordinator_.recovery_pending();
    setProperty("batchRenameRecoveryPending", needs_recovery);
    if (result.ok()) {
        QStringList destinations;
        destinations.reserve(static_cast<qsizetype>(result.destinations.size()));
        for (const auto &path : result.destinations) {
            destinations.push_back(display_path(path));
        }
        operationStatus_ = QCoreApplication::translate("MainWindow", "Items renamed: %1")
                               .arg(result.destinations.size());
        destinations.append(activeBatchUnchangedPaths_);
        activeBatchSourcePaths_.clear();
        refresh_after_batch_rename(destinations);
    } else if (result.status == fileops::BatchRunStatus::rolled_back) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Rename cancelled; original names were restored");
        const auto sources = activeBatchSourcePaths_;
        activeBatchSourcePaths_.clear();
        refresh_after_batch_rename(sources);
    } else if (needs_recovery || result.status == fileops::BatchRunStatus::recovery_required ||
               result.status == fileops::BatchRunStatus::journal_error ||
               result.status == fileops::BatchRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Batch rename paused. Check the network and press F5");
        begin_preview_generation(false);
        listModel_.mark_previews_verification_pending();
    } else if (result.status == fileops::BatchRunStatus::unsupported) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Batch rename is unavailable on this system");
        activeBatchSourcePaths_.clear();
    } else {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Batch rename failed");
        activeBatchSourcePaths_.clear();
    }
    if (!needs_recovery) activeBatchUnchangedPaths_.clear();
    set_rename_in_flight(false);
    update_status(lastUpdate_);
}

void MainWindow::handle_delete_progress(const fileops::PermanentDeleteProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::PermanentDeletePhase::prepared:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::PermanentDeletePhase::evacuating:
        phase = QCoreApplication::translate("MainWindow", "checking items");
        break;
    case fileops::PermanentDeletePhase::rollback:
        phase = QCoreApplication::translate("MainWindow", "restoring original names");
        break;
    case fileops::PermanentDeletePhase::permanent_delete_intent:
        phase = QCoreApplication::translate("MainWindow", "recording intent");
        break;
    case fileops::PermanentDeletePhase::deleting:
        phase = QCoreApplication::translate("MainWindow", "deleting");
        break;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "Safe deletion: %1 · %2 of %3")
                           .arg(phase)
                           .arg(progress.completed)
                           .arg(progress.total);
    update_status(lastUpdate_);
}

void MainWindow::handle_delete_result(const fileops::PermanentDeleteResult &result) {
    notify_file_in_use(result.operation_status);
    const auto owns_recovery = permanentDeleteCoordinator_.owns_recovery();
    setProperty("permanentDeleteRecoveryPending", owns_recovery);
    if (result.ok()) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Files deleted: %1").arg(result.completed);
        selectedEntryId_ = 0;
        begin_preview_generation();
        refresh_directory_sources();
        pollTimer_->setInterval(16);
        pollTimer_->start();
    } else if (result.status == fileops::PermanentDeleteRunStatus::rolled_back) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Deletion cancelled; original names were restored");
        refresh_directory_sources();
        pollTimer_->start();
    } else if (owns_recovery ||
               result.status == fileops::PermanentDeleteRunStatus::recovery_required ||
               result.status == fileops::PermanentDeleteRunStatus::journal_error ||
               result.status == fileops::PermanentDeleteRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Deletion paused. Check the network and press F5");
        begin_preview_generation(false);
        listModel_.mark_previews_verification_pending();
    } else if (result.status == fileops::PermanentDeleteRunStatus::unsupported) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "This resource cannot be deleted safely using the selected method");
    } else {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Deletion failed");
    }
    set_delete_in_flight(false);
    update_status(lastUpdate_);
}

void MainWindow::handle_trash_progress(const fileops::TrashProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::TrashPhase::prepared:
    case fileops::TrashPhase::restore_prepared:
    case fileops::TrashPhase::purge_prepared:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::TrashPhase::moving:
        phase = QCoreApplication::translate("MainWindow", "Moving");
        break;
    case fileops::TrashPhase::rollback:
    case fileops::TrashPhase::rollback_container_remove_intent:
        phase = QCoreApplication::translate("MainWindow", "rolling back move");
        break;
    case fileops::TrashPhase::manifest_intent:
        phase = QCoreApplication::translate("MainWindow", "saving operation");
        break;
    case fileops::TrashPhase::published:
        phase = QCoreApplication::translate("MainWindow", "done");
        break;
    case fileops::TrashPhase::restoring:
        phase = QCoreApplication::translate("MainWindow", "restoring");
        break;
    case fileops::TrashPhase::restore_rollback:
        phase = QCoreApplication::translate("MainWindow", "rolling back restore");
        break;
    case fileops::TrashPhase::manifest_remove_intent:
    case fileops::TrashPhase::restore_container_remove_intent:
        phase = QCoreApplication::translate("MainWindow", "finishing restore");
        break;
    case fileops::TrashPhase::restored:
        phase = QCoreApplication::translate("MainWindow", "restored");
        break;
    case fileops::TrashPhase::purging:
        phase = QCoreApplication::translate("MainWindow", "permanent deletion");
        break;
    case fileops::TrashPhase::purge_manifest_remove_intent:
    case fileops::TrashPhase::purge_container_remove_intent:
        phase = QCoreApplication::translate("MainWindow", "cleaning records");
        break;
    case fileops::TrashPhase::purged:
        phase = QCoreApplication::translate("MainWindow", "deleted");
        break;
    }
    operationStatus_ = QCoreApplication::translate("MainWindow", "VO-VE Trash: %1 · %2 of %3")
                           .arg(phase)
                           .arg(progress.completed)
                           .arg(progress.total);
    update_status(lastUpdate_);
}

void MainWindow::handle_trash_result(const fileops::TrashResult &result,
                                     const TrashOperationIntent intent) {
    auto &diagnostic = DiagnosticSink::instance();
    if (diagnostic.active()) {
        diagnostic.record("trash.result",
                          {{"intent", std::to_string(static_cast<int>(intent))},
                           {"status", std::to_string(static_cast<int>(result.status))},
                           {"operation_status", std::to_string(static_cast<int>(result.operation_status))},
                           {"completed", std::to_string(result.completed)},
                           {"total", std::to_string(result.total)},
                           {"detail", result.detail_utf8}});
    }
    notify_file_in_use(result.operation_status);
    const auto restoring = intent == TrashOperationIntent::restore;
    const auto purging = intent == TrashOperationIntent::purge;
    const auto owns_recovery = trashCoordinator_.owns_recovery();
    setProperty("trashRecoveryPending", owns_recovery);
    if (result.ok()) {
        operationStatus_ =
            purging     ? QCoreApplication::translate("MainWindow", "Files permanently deleted: %1")
                              .arg(result.completed)
            : restoring ? QCoreApplication::translate("MainWindow", "Files restored: %1")
                              .arg(result.completed)
                        : QCoreApplication::translate("MainWindow", "Moved to VO-VE Trash: %1")
                              .arg(result.completed);
        selectedEntryId_ = 0;
        if (purging && trashPurgeTotal_ != 0) {
            ++trashPurgeCompleted_;
            set_delete_in_flight(false);
            if (!submit_next_trash_purge()) {
                trashPurgeQueue_.clear();
                trashPurgeTotal_ = 0;
                operationStatus_ = QCoreApplication::translate(
                    "MainWindow", "Failed to continue emptying VO-VE Trash");
                update_status(lastUpdate_);
            }
            return;
        }
        if (trashViewActive_) {
            start_trash_catalog_load(true);
        } else {
            begin_preview_generation();
            refresh_directory_sources();
            pollTimer_->setInterval(16);
            pollTimer_->start();
            start_trash_catalog_load(false);
        }
    } else if (result.status == fileops::TrashRunStatus::rolled_back) {
        operationStatus_ =
            restoring ? QCoreApplication::translate("MainWindow",
                                                    "Restore cancelled; Trash was not changed")
                      : QCoreApplication::translate(
                            "MainWindow", "Deletion cancelled; original names were restored");
        refresh_directory_sources();
        pollTimer_->start();
    } else if (owns_recovery || result.status == fileops::TrashRunStatus::recovery_required ||
               result.status == fileops::TrashRunStatus::journal_error ||
               result.status == fileops::TrashRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash operation paused. Press F5 to recover");
        if (result.operation_status != fileops::OperationStatus::success) {
            operationStatus_ += QStringLiteral(": ") + operation_status_text(result.operation_status);
        }
        begin_preview_generation(false);
        listModel_.mark_previews_verification_pending();
    } else if (result.status == fileops::TrashRunStatus::unsupported) {
#ifdef Q_OS_WIN
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash supports local files on a fixed NTFS drive");
#else
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "VO-VE Trash is unavailable for this file or filesystem");
#endif
    } else if (result.status == fileops::TrashRunStatus::insufficient_space) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Not enough free disk space for VO-VE Trash");
    } else if (result.status == fileops::TrashRunStatus::storage_unavailable) {
        operationStatus_ =
            result.completed == 0 && result.operation_status == fileops::OperationStatus::permission_denied
                ? QCoreApplication::translate("MainWindow",
                    "VO-VE Trash is unavailable on this resource. Originals were not changed")
                : QCoreApplication::translate("MainWindow", "Failed to check VO-VE Trash storage");
        if (!result.detail_utf8.empty()) {
            operationStatus_ += QStringLiteral(": ") +
                                QString::fromStdString(result.detail_utf8).left(512).simplified();
        }
    } else {
        operationStatus_ =
            purging ? QCoreApplication::translate("MainWindow", "Permanent deletion failed")
            : restoring
                ? QCoreApplication::translate("MainWindow", "Restore failed")
                : QCoreApplication::translate("MainWindow", "Files were not moved to Trash");
        const auto reason = trash_operation_error_text(result.operation_status);
        if (!reason.isEmpty()) {
            operationStatus_ += QStringLiteral(": ") + reason;
        }
        const auto detail = QString::fromStdString(result.detail_utf8).left(512).simplified();
        if (!detail.isEmpty()) {
            operationStatus_ += QStringLiteral(": ") + detail;
        }
    }
    if (!result.ok() && trashPurgeTotal_ != 0) {
        trashPurgeQueue_.clear();
        trashPurgeCompleted_ = 0;
        trashPurgeTotal_ = 0;
    }
    set_delete_in_flight(false);
    setProperty("trashOperationDetail", QString::fromStdString(result.detail_utf8));
    previewTimer_->start(0);
    update_status(lastUpdate_);
}

void MainWindow::handle_file_transfer_progress(
    const fileops::FileTransferProgressUpdate &progress) {
    QString phase;
    switch (progress.step) {
    case fileops::FileTransferStep::none:
        phase = QCoreApplication::translate("MainWindow", "preparing");
        break;
    case fileops::FileTransferStep::atomic_move:
    case fileops::FileTransferStep::publish_temp:
        phase = QCoreApplication::translate("MainWindow", "publishing");
        break;
    case fileops::FileTransferStep::stage_source:
        phase = QCoreApplication::translate("MainWindow", "committing source");
        break;
    case fileops::FileTransferStep::reserve_temp:
        phase = QCoreApplication::translate("MainWindow", "preparing space");
        break;
    case fileops::FileTransferStep::stream_temp:
        phase = QCoreApplication::translate("MainWindow", "transferring data");
        break;
    case fileops::FileTransferStep::delete_source:
        phase = QCoreApplication::translate("MainWindow", "finishing move");
        break;
    case fileops::FileTransferStep::restore_staged_source:
        phase = QCoreApplication::translate("MainWindow", "restoring source");
        break;
    case fileops::FileTransferStep::cleanup_temp:
        phase = QCoreApplication::translate("MainWindow", "cleaning incomplete file");
        break;
    case fileops::FileTransferStep::evacuate_overwrite_destination:
        phase = QCoreApplication::translate("MainWindow", "preparing overwrite");
        break;
    case fileops::FileTransferStep::restore_overwrite_destination:
        phase = QCoreApplication::translate("MainWindow", "restoring destination");
        break;
    case fileops::FileTransferStep::cleanup_overwrite_destination:
        phase = QCoreApplication::translate("MainWindow", "finishing overwrite");
        break;
    }
    auto detail = QCoreApplication::translate("MainWindow", "%1: %2 · %3 of %4")
                      .arg(activeTransferKind_ == fileops::FileTransferKind::copy
                               ? QCoreApplication::translate("MainWindow", "Copying")
                               : QCoreApplication::translate("MainWindow", "Moving"),
                           phase)
                      .arg(progress.completed)
                      .arg(progress.total);
    if (progress.bytes_total != 0) {
        detail += QStringLiteral(" · %1 / %2")
                      .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.bytes_written)))
                      .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.bytes_total)));
    }
    operationStatus_ = std::move(detail);
    update_status(lastUpdate_);
}

void MainWindow::handle_file_transfer_conflict(const fileops::FileTransferConflict &conflict,
                                               const bool object_queue, const bool overwrite_allowed,
                                               const bool copy_allowed) {
    if (conflict.items.empty()) {
        if (object_queue) objectTransferQueue_.resolve_conflict(fileops::FileTransferConflictDecision::cancel);
        else fileTransferCoordinator_.resolve_conflict(fileops::FileTransferConflictDecision::cancel);
        return;
    }
    QStringList names;
    names.reserve(static_cast<qsizetype>(conflict.items.size()));
    for (const auto &item : conflict.items) {
        names.push_back(display_path(item.destination.filename()));
    }
    const auto single = conflict.items.size() == 1U;
    auto *dialog = new QMessageBox(
        QMessageBox::Question, QCoreApplication::translate("MainWindow", "Name conflict"),
        single
            ? QCoreApplication::translate("MainWindow", "An object with this name already exists")
            : QCoreApplication::translate(
                  "MainWindow", "%1 already exist in the destination. What should be done?")
                  .arg(names.join(QStringLiteral(", "))),
        QMessageBox::NoButton, this);
    dialog->setObjectName(QStringLiteral("fileOverwriteConfirmation"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setWindowModality(Qt::WindowModal);
    if (single) {
        const auto &item = conflict.items.front();
        dialog->setInformativeText(
            QCoreApplication::translate("MainWindow", "Object: %1\nFrom: %2\nTo: %3")
                .arg(display_path(item.destination.filename()),
                     QDir::toNativeSeparators(display_path(item.source.parent_path())),
                     QDir::toNativeSeparators(display_path(item.destination.parent_path()))));
    } else {
        dialog->setInformativeText(
            QCoreApplication::translate("MainWindow", "The selected action applies to all items"));
    }
    auto *create_copy = dialog->addButton(QCoreApplication::translate("MainWindow", "Create copy"),
                                          QMessageBox::ActionRole);
    create_copy->setObjectName(QStringLiteral("fileCreateCopyConfirmButton"));
    create_copy->setEnabled(copy_allowed);
    if (!copy_allowed) dialog->setInformativeText(QCoreApplication::translate("MainWindow",
        "The destination changed during transfer. Cancel to choose a new copy name."));
    auto *overwrite = dialog->addButton(QCoreApplication::translate("MainWindow", "Overwrite"),
                                        QMessageBox::DestructiveRole);
    overwrite->setObjectName(QStringLiteral("fileOverwriteConfirmButton"));
    overwrite->setEnabled(overwrite_allowed);
    if (!overwrite_allowed) {
        dialog->setInformativeText(QCoreApplication::translate("MainWindow",
            "An existing folder cannot be replaced. Create a copy or cancel the entire selection."));
    }
    auto *cancel = dialog->addButton(QCoreApplication::translate("MainWindow", "Cancel"),
                                     QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("fileOverwriteCancelButton"));
    dialog->setDefaultButton(cancel);
    dialog->setEscapeButton(cancel);
    connect(dialog, &QDialog::finished, this, [this, dialog, create_copy, overwrite, object_queue] {
        auto decision = fileops::FileTransferConflictDecision::cancel;
        if (dialog->clickedButton() == create_copy) {
            decision = fileops::FileTransferConflictDecision::create_copy;
        } else if (dialog->clickedButton() == overwrite) {
            decision = fileops::FileTransferConflictDecision::overwrite;
        }
        if (object_queue) objectTransferQueue_.resolve_conflict(decision);
        else fileTransferCoordinator_.resolve_conflict(decision);
    });
    operationStatus_ = QCoreApplication::translate("MainWindow", "Waiting for conflict decision");
    update_status(lastUpdate_);
    dialog->open();
}

void MainWindow::handle_file_transfer_result(const fileops::FileTransferResult &result) {
    notify_file_in_use(result.operation_status);
    for (const auto &destination : result.destinations)
        allow_observed_global_search_path(display_path(destination));
    if (globalSearchActive_ && !result.destinations.empty()) catalogBeforeGlobalSearchDirty_ = true;
    if (activeTransferKind_ == fileops::FileTransferKind::move)
        reconcile_global_search_move(activeFileTransferSources_.first(static_cast<qsizetype>(
            std::min(result.completed, static_cast<std::size_t>(activeFileTransferSources_.size())))));
    const auto owns_recovery = fileTransferCoordinator_.owns_recovery();
    if (!owns_recovery) activeFileTransferSources_.clear();
    setProperty("fileTransferRecoveryPending", owns_recovery);
    if (result.ok()) {
        operationStatus_ = activeTransferKind_ == fileops::FileTransferKind::copy
                               ? QCoreApplication::translate("MainWindow", "Files copied: %1")
                                     .arg(result.completed)
                               : QCoreApplication::translate("MainWindow", "Files moved: %1")
                                     .arg(result.completed);
        QStringList visible_destinations;
        const auto current_key = transfer_path_key(native_path(currentPath_));
        for (const auto &destination : result.destinations) {
            if (transfer_path_key(destination.parent_path()) == current_key) {
                visible_destinations.push_back(display_path(destination));
            }
        }
        pendingRenamePaths_ = std::move(visible_destinations);
        selectedEntryId_ = 0;
        begin_preview_generation();
        refresh_directory_sources();
        pollTimer_->setInterval(16);
        pollTimer_->start();
    } else if (result.status == fileops::FileTransferRunStatus::cancelled && !owns_recovery) {
        operationStatus_ =
            activeTransferKind_ == fileops::FileTransferKind::copy
                ? QCoreApplication::translate("MainWindow", "Copy cancelled. Files copied: %1")
                      .arg(result.completed)
                : QCoreApplication::translate("MainWindow", "Move cancelled. Files moved: %1")
                      .arg(result.completed);
        refresh_directory_sources();
        pollTimer_->start();
    } else if (owns_recovery ||
               result.status == fileops::FileTransferRunStatus::recovery_required ||
               result.status == fileops::FileTransferRunStatus::journal_error ||
               result.status == fileops::FileTransferRunStatus::stopped) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Transfer paused. Check the network and press F5");
        begin_preview_generation(false);
        listModel_.mark_previews_verification_pending();
    } else if (result.status == fileops::FileTransferRunStatus::partial_failure) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow",
                                        "Files transferred: %1 of %2. The rest were not changed")
                .arg(result.completed)
                .arg(result.total);
        refresh_directory_sources();
        pollTimer_->start();
    } else if (result.status == fileops::FileTransferRunStatus::unsupported) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Transfer is unavailable for the selected resource");
    } else if (result.operation_status == fileops::OperationStatus::conflict) {
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "A file with this name already exists");
    } else if (result.operation_status == fileops::OperationStatus::source_changed) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "The source file changed. Retry the operation");
    } else if (result.operation_status == fileops::OperationStatus::permission_denied) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "No permission to write to the selected folder");
    } else {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Copy or move failed");
    }
    set_transfer_in_flight(false);
    update_status(lastUpdate_);
}

void MainWindow::notify_file_in_use(const fileops::OperationStatus status) {
    if (status != fileops::OperationStatus::file_in_use) return;
    if (operationNotice_ == nullptr) operationNotice_ = new OperationNotice(this);
    operationNotice_->show_file_in_use();
}

void MainWindow::handle_directory_transfer_progress(
    const fileops::BasicDirectoryTransferProgress &progress) {
    QString phase;
    switch (progress.phase) {
    case fileops::BasicDirectoryTransferPhase::enumerating:
        phase = QCoreApplication::translate("MainWindow", "counting contents");
        break;
    case fileops::BasicDirectoryTransferPhase::staging_ready:
        phase = QCoreApplication::translate("MainWindow", "space prepared");
        break;
    case fileops::BasicDirectoryTransferPhase::copying:
        phase = QCoreApplication::translate("MainWindow", "transferring files");
        break;
    case fileops::BasicDirectoryTransferPhase::verifying:
        phase = QCoreApplication::translate("MainWindow", "verifying copy");
        break;
    case fileops::BasicDirectoryTransferPhase::publishing:
        phase = QCoreApplication::translate("MainWindow", "publishing folder");
        break;
    case fileops::BasicDirectoryTransferPhase::retiring_source:
    case fileops::BasicDirectoryTransferPhase::deleting_source:
        phase = QCoreApplication::translate("MainWindow", "finishing move");
        break;
    case fileops::BasicDirectoryTransferPhase::completed:
        phase = QCoreApplication::translate("MainWindow", "done");
        break;
    }
    auto detail = QCoreApplication::translate("MainWindow", "Folder transfer: %1").arg(phase);
    if (progress.total_entries != 0U) {
        detail += QCoreApplication::translate("MainWindow", " · %1 of %2")
                      .arg(progress.completed_entries)
                      .arg(progress.total_entries);
    }
    if (progress.total_bytes != 0U) {
        detail +=
            QStringLiteral(" · %1 / %2")
                .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.completed_bytes)))
                .arg(QLocale().formattedDataSize(static_cast<qint64>(progress.total_bytes)));
    }
    operationStatus_ = std::move(detail);
    update_status(lastUpdate_);
}

bool MainWindow::record_directory_transfer_completion() {
    directoryTransferCompletionKnown_ = true;
    return persist_directory_transfer_recovery_state();
}

void MainWindow::handle_directory_transfer_result(
    const fileops::BasicDirectoryTransferResult &result) {
    if (result.status == fileops::BasicDirectoryTransferStatus::file_in_use)
        notify_file_in_use(fileops::OperationStatus::file_in_use);
    if (!directoryTransferDiscarding_ && (result.ok() ||
        (directoryTransferCompletionKnown_ && result.status == fileops::BasicDirectoryTransferStatus::not_found))) {
        allow_observed_global_search_path(directoryTransferRecoveryDestination_);
        if (pendingDirectoryTransferBatch_ && pendingDirectoryTransferBatch_->kind == fileops::FileTransferKind::move)
            reconcile_global_search_move({directoryTransferRecoverySource_});
        if (globalSearchActive_) catalogBeforeGlobalSearchDirty_ = true;
    }
    if (directoryTransferDiscarding_) {
        set_transfer_in_flight(false);
        if (result.ok() || result.status == fileops::BasicDirectoryTransferStatus::not_found) {
            pendingDirectoryTransferBatch_.reset();
            clear_directory_transfer_recovery_state();
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Incomplete data was safely deleted");
            refresh_directory_sources();
            pollTimer_->start();
            update_status(lastUpdate_);
            return;
        }
        if (!result.manifest_path.empty()) {
            directoryTransferRecoveryManifest_ = display_path(result.manifest_path);
        }
        setProperty("directoryTransferRecoveryPending", true);
        const auto choice_persisted = persist_directory_transfer_recovery_state();
        operationStatus_ =
            result.status == fileops::BasicDirectoryTransferStatus::recovery_required
                ? QCoreApplication::translate(
                      "MainWindow",
                      "Cannot delete safely: hidden data may be the only complete copy")
                : QCoreApplication::translate("MainWindow", "Incomplete data was not deleted: %1")
                      .arg(directory_transfer_error_text(result.status));
        operationStatus_ +=
            choice_persisted ? QCoreApplication::translate(
                                   "MainWindow", ". Your choice was saved; press F5 to retry")
                             : QCoreApplication::translate("MainWindow",
                                                           ". Failed to update the recovery state");
        update_status(lastUpdate_);
        return;
    }
    if (directoryTransferCompletionKnown_ &&
        result.status == fileops::BasicDirectoryTransferStatus::not_found) {
        if (advance_directory_transfer_batch_after_success()) {
            return;
        }
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Folder transfer completed and internal cleanup verified");
        begin_preview_generation();
        refresh_directory_sources();
        pollTimer_->start();
        update_status(lastUpdate_);
        return;
    }
    if (!directoryTransferCompletionKnown_ && pendingDirectoryTransferBatch_ &&
        result.status == fileops::BasicDirectoryTransferStatus::not_found) {
        set_transfer_in_flight(false);
        directoryTransferRecoveryManifest_.clear();
        directoryTransferDiscarding_ = false;
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Internal data has not been created yet. Retrying the "
                          "current folder without skipping the queue…");
        update_status(lastUpdate_);
        if (!submit_next_directory_transfer()) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Failed to retry the current folder; the queue was kept");
            static_cast<void>(persist_directory_transfer_recovery_state());
            update_status(lastUpdate_);
        }
        return;
    }
    if (result.recovery_available &&
        (!result.manifest_path.empty() || directoryTransferRecoveryRequestId_ != 0U)) {
        set_transfer_in_flight(false);
        if (!result.manifest_path.empty()) {
            directoryTransferRecoveryManifest_ = display_path(result.manifest_path);
        }
        setProperty("directoryTransferRecoveryPending", true);
        static_cast<void>(persist_directory_transfer_recovery_state());
        if (result.ok()) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Folder transferred. Press F5 to finish internal cleanup");
        } else {
            operationStatus_ = directory_transfer_error_text(result.status);
            const auto retryable =
                result.status == fileops::BasicDirectoryTransferStatus::disconnected ||
                result.status == fileops::BasicDirectoryTransferStatus::timed_out ||
                result.status == fileops::BasicDirectoryTransferStatus::io_error ||
                result.status == fileops::BasicDirectoryTransferStatus::unknown_outcome ||
                result.status == fileops::BasicDirectoryTransferStatus::recovery_required;
            operationStatus_ +=
                retryable
                    ? QCoreApplication::translate("MainWindow", ". Check the network and press F5")
                    : QCoreApplication::translate("MainWindow", ". Incomplete data was kept");
        }
        begin_preview_generation(false);
        listModel_.mark_previews_verification_pending();
        update_status(lastUpdate_);
        offer_directory_transfer_discard();
        return;
    }
    if (result.ok()) {
        if (advance_directory_transfer_batch_after_success()) {
            return;
        }
        clear_directory_transfer_recovery_state();
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Folder transfer recovered and completed");
        begin_preview_generation();
        refresh_directory_sources();
        pollTimer_->setInterval(16);
        pollTimer_->start();
        update_status(lastUpdate_);
        return;
    }

    set_transfer_in_flight(false);
    pendingDirectoryTransferBatch_.reset();
    clear_directory_transfer_recovery_state();
    operationStatus_ = directory_transfer_error_text(result.status);
    refresh_directory_sources();
    pollTimer_->start();
    update_status(lastUpdate_);
}

void MainWindow::show_trash() {
    save_navigation_state();
    pendingNavigationRestore_.reset();
    const auto operation_busy =
        renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || fileOperationService_.busy() ||
        pendingRenameOperation_.has_value() || batchRenameCoordinator_.busy() ||
        permanentDeleteCoordinator_.busy() || trashCoordinator_.busy() ||
        directoryTransferService_.busy() || pendingExternalDirectoryDrop_.has_value() ||
        pendingExternalTransferDrop_.has_value() || !dropProbeSource_.idle();
    if (operation_busy) {
        update_status(lastUpdate_);
        return;
    }
    leave_recursive_view(false);
    static_cast<void>(refresh_current_operation_journal_state());
    if (globalSearchActive_ || !globalSearchEdit_->text().isEmpty()) {
        const QSignalBlocker blocker(globalSearchEdit_);
        globalSearchEdit_->clear();
        leave_global_search(false);
    }
    directoryMonitor_.set_visible(false);
    directoryMonitor_.set_directory({});
    session_.suspend();
    pollTimer_->stop();
    automaticRefreshInFlight_ = false;
    begin_preview_generation();
    trashCatalog_.reset();
    trashManifestByEntry_.clear();
    listModel_.set_tooltip_paths({});
    listModel_.replace_catalog({});
    trashViewActive_ = true;
    setProperty("trashViewActive", true);
    globalSearchEdit_->setEnabled(false);
    listView_->setDragEnabled(false);
    listView_->setAcceptDrops(false);
    listView_->viewport()->setAcceptDrops(false);
    listView_->setDragDropMode(QAbstractItemView::NoDragDrop);
    listModel_.set_drag_and_edit_enabled(false);
    pathEdit_->setText(QCoreApplication::translate("MainWindow", "VO-VE Trash"));
    selectedEntryId_ = 0;
    selectedName_.clear();
    selectedColorSummary_.clear();
    previewCanvas_->setPixmap({});
    previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Select a file"));
    previewName_->clear();
    previewName_->setToolTip({});
    previewFormat_->clear();
    previewFormat_->hide();
    update_page_navigation(0, 0);
    update_location_selection();
    previewModified_->set_modified_time(std::nullopt);
    update_navigation();
    update_rename_action();
    start_trash_catalog_load(true);
}

void MainWindow::start_trash_catalog_load(const bool discover_recovery_roots) {
    const auto roots = discover_recovery_roots ? std::nullopt
                                               : std::optional<std::vector<std::filesystem::path>>{
                                                     std::vector<std::filesystem::path>{}};
    if (!trashCatalogLoader_.start(trashManifestDirectory_, roots)) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "Failed to read VO-VE Trash");
        update_status(lastUpdate_);
        return;
    }
    if (trashViewActive_ &&
        refresh_current_operation_journal_state() == CurrentOperationJournalState::none) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "VO-VE Trash: loading…");
        update_status(lastUpdate_);
    }
    trashCatalogTimer_->start();
}

void MainWindow::poll_trash_catalog_loaders() {
    if (auto catalog = trashCatalogLoader_.poll()) {
        update_trash_favorite_state(*catalog);
        if (trashViewActive_) {
            apply_trash_catalog(std::move(*catalog));
        } else {
            trashCatalog_ = std::move(*catalog);
        }
    }
    if (!trashCatalogLoader_.loading()) {
        trashCatalogTimer_->stop();
    }
}

void MainWindow::apply_trash_catalog(fileops::TrashCatalogResult catalog) {
    trashCatalog_ = std::move(catalog);
    if (!trashCatalog_->ok()) {
        listModel_.replace_catalog({});
        trashManifestByEntry_.clear();
        operationStatus_ = QCoreApplication::translate("MainWindow", "Failed to read VO-VE Trash");
        update_status(lastUpdate_);
        return;
    }

    std::vector<core::DirectoryEntry> entries;
    entries.reserve(trashCatalog_->total_items);
    QHash<qulonglong, QString> tooltips;
    trashManifestByEntry_.clear();
    for (const auto &record : trashCatalog_->manifests) {
        const auto deleted =
            QDateTime::fromMSecsSinceEpoch(record.transaction.created_unix_ns / 1'000'000);
        const auto deleted_text =
            deleted.isValid() ? QLocale().toString(deleted.toLocalTime(), QLocale::ShortFormat)
                              : QCoreApplication::translate("MainWindow", "unknown date");
        for (std::size_t index{}; index < record.transaction.items.size(); ++index) {
            const auto &item = record.transaction.items[index];
            if (item.location != fileops::TrashItemLocation::stored || item.current.empty()) {
                continue;
            }
            const auto id = trash_entry_id(record.transaction.operation_id, index);
            auto name = item.original.filename().u8string();
            if (name.empty()) {
                name = item.current.filename().u8string();
            }
            entries.push_back(
                {.id = id,
                 .kind = item.kind == fileops::TrashItemKind::directory ? core::EntryKind::directory
                                                                        : core::EntryKind::file,
                 .state = core::EntryState::metadata_ready,
                 .name_utf8 = reinterpret_cast<const char *>(name.c_str()),
                 .search_key_utf8 =
                     core::make_search_key(reinterpret_cast<const char *>(name.c_str())),
                 .path_utf8 = path_utf8(item.current),
                 .source_revision_utf8 = item.current_snapshot.source_revision_utf8,
                 .size_bytes = item.payload_bytes,
                 .modified_unix_ns = item.current_snapshot.modified_unix_ns});
            const auto original = QString::fromUtf8(path_utf8(item.original));
            tooltips.insert(static_cast<qulonglong>(id),
                            QCoreApplication::translate(
                                "MainWindow",
                                "%1\nDeleted: %2\nActions apply to the complete deletion operation")
                                .arg(QDir::toNativeSeparators(original), deleted_text));
            trashManifestByEntry_.insert(static_cast<qulonglong>(id),
                                         QString::fromUtf8(path_utf8(record.manifest_path)));
        }
    }

    begin_preview_generation();
    listModel_.replace_catalog(std::move(entries));
    listModel_.set_tooltip_paths(std::move(tooltips));
    listModel_.set_filter(filterEdit_->text());
    listModel_.set_sort(static_cast<core::SortField>(sortField_->currentData().toInt()),
                        sortDirection_->isChecked() ? core::SortDirection::descending
                                                    : core::SortDirection::ascending);
    if (refresh_current_operation_journal_state() == CurrentOperationJournalState::none) {
        operationStatus_.clear();
    }
    previewTimer_->start(0);
    update_status(lastUpdate_);
    update_rename_action();
    if (pendingTrashCleanup_) {
        pendingTrashCleanup_ = false;
        QTimer::singleShot(0, this, &MainWindow::show_trash_cleanup);
    }
}

void MainWindow::leave_trash_view() {
    if (!trashViewActive_) {
        return;
    }
    trashCatalogLoader_.cancel();
    trashViewActive_ = false;
    setProperty("trashViewActive", false);
    trashCatalog_.reset();
    trashManifestByEntry_.clear();
    listModel_.set_tooltip_paths({});
    listModel_.replace_catalog({});
    listModel_.set_drag_and_edit_enabled(true);
    globalSearchEdit_->setEnabled(true);
    listView_->setDragEnabled(true);
    listView_->setAcceptDrops(true);
    listView_->viewport()->setAcceptDrops(true);
    listView_->setDragDropMode(QAbstractItemView::DragDrop);
    filterEdit_->setEnabled(true);
}

void MainWindow::show_trash_context_menu(const QPoint &position) {
    const auto index = listView_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    listView_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    const auto id = index.data(DirectoryListModel::EntryIdRole).toULongLong();
    const auto found = trashManifestByEntry_.constFind(id);
    if (found == trashManifestByEntry_.cend()) {
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("trashItemContextMenu"));
    auto *restore = menu.addAction(QCoreApplication::translate("MainWindow", "Restore"));
    restore->setObjectName(QStringLiteral("trashRestoreAction"));
    auto *restore_to = menu.addAction(QCoreApplication::translate("MainWindow", "Restore to…"));
    restore_to->setObjectName(QStringLiteral("trashRestoreToAction"));
    auto *purge = menu.addAction(QCoreApplication::translate("MainWindow", "Delete permanently"));
    purge->setObjectName(QStringLiteral("trashDeletePermanentlyAction"));
    const auto enabled =
        refresh_current_operation_journal_state() == CurrentOperationJournalState::none &&
        !deleteInFlight_ && !trashCoordinator_.busy();
    restore->setEnabled(enabled);
    restore_to->setEnabled(enabled);
    purge->setEnabled(enabled);
    const auto selected = menu.exec(listView_->viewport()->mapToGlobal(position));
    const auto manifest = native_path(*found);
    if (selected == restore) {
        static_cast<void>(start_trash_manifest_operation(manifest, TrashAction::restore));
    } else if (selected == restore_to) {
        static_cast<void>(start_trash_manifest_operation(manifest, TrashAction::restore_to));
    } else if (selected == purge) {
        static_cast<void>(start_trash_manifest_operation(manifest, TrashAction::purge));
    }
}

void MainWindow::show_trash_cleanup() {
    if (!trashCatalog_ || !trashCatalog_->ok()) {
        pendingTrashCleanup_ = true;
        if (!trashViewActive_) {
            show_trash();
        } else if (!trashCatalogLoader_.loading()) {
            start_trash_catalog_load();
        }
        return;
    }
    if (deleteInFlight_ || trashCoordinator_.busy() || trashCatalog_->manifests.empty()) {
        return;
    }
    TrashCleanupDialog dialog(*trashCatalog_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    auto plan = dialog.plan();
    if (plan.manifests.empty()) {
        operationStatus_ = QCoreApplication::translate("MainWindow", "No matching Trash objects");
        update_status(lastUpdate_);
        return;
    }
    trashPurgeQueue_ = std::move(plan.manifests);
    trashPurgeCompleted_ = 0;
    trashPurgeTotal_ = trashPurgeQueue_.size();
    if (!submit_next_trash_purge()) {
        trashPurgeQueue_.clear();
        trashPurgeTotal_ = 0;
    }
}

bool MainWindow::submit_next_trash_purge() {
    if (trashPurgeQueue_.empty()) {
        const auto completed = trashPurgeCompleted_;
        trashPurgeCompleted_ = 0;
        trashPurgeTotal_ = 0;
        operationStatus_ = QCoreApplication::translate("MainWindow", "Trash operations deleted: %1")
                               .arg(static_cast<qulonglong>(completed));
        start_trash_catalog_load();
        return true;
    }
    const auto manifest = trashPurgeQueue_.front();
    trashPurgeQueue_.erase(trashPurgeQueue_.begin());
    return start_trash_manifest_operation(manifest, TrashAction::purge, false);
}

void MainWindow::update_trash_favorite_state(const fileops::TrashCatalogResult &catalog) {
    QList<QListWidgetItem *> trash_items;
    for (int row{}; row < favorites_->count(); ++row) {
        if (is_trash_favorite(favorites_->item(row))) {
            trash_items.push_back(favorites_->item(row));
            break;
        }
    }
    if (trashFavorite_ != nullptr && is_trash_favorite(trashFavorite_->item(0))) {
        trash_items.push_back(trashFavorite_->item(0));
    }
    if (trash_items.isEmpty()) {
        return;
    }
    auto worst = fileops::TrashQuotaState::normal;
    for (const auto &usage : catalog.storage_usage) {
        const auto state = fileops::classify_trash_quota(usage, trashMaximumBytes_);
        if (state == fileops::TrashQuotaState::exceeded) {
            worst = state;
            break;
        }
        if (state == fileops::TrashQuotaState::warning) {
            worst = state;
        }
    }
    const auto foreground =
        worst == fileops::TrashQuotaState::exceeded  ? QVariant(QColor(QStringLiteral("#FF3B30")))
        : worst == fileops::TrashQuotaState::warning ? QVariant(QColor(QStringLiteral("#F5A623")))
                                                     : QVariant{};
    const auto tooltip =
        QCoreApplication::translate("MainWindow", "Objects: %1 · %2")
            .arg(static_cast<qulonglong>(catalog.total_items))
            .arg(QLocale().formattedDataSize(static_cast<qint64>(catalog.total_bytes)));
    for (auto *trash_item : trash_items) {
        trash_item->setData(Qt::ForegroundRole, foreground);
        trash_item->setData(Qt::UserRole + 2, static_cast<int>(worst));
        trash_item->setToolTip(tooltip);
    }
}

bool MainWindow::start_trash_manifest_operation(const std::filesystem::path &manifest,
                                                const TrashAction action,
                                                const bool confirm_permanent_delete) {
    if (action == TrashAction::none || manifest.empty() || renameInFlight_ || deleteInFlight_ ||
        transferInFlight_ || createDirectoryInFlight_ || fileTransferCoordinator_.busy() ||
        fileOperationService_.busy() || pendingRenameOperation_.has_value() ||
        batchRenameCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy() || directoryTransferService_.busy() ||
        pendingExternalDirectoryDrop_.has_value() || pendingExternalTransferDrop_.has_value() ||
        !dropProbeSource_.idle() ||
        refresh_current_operation_journal_state() != CurrentOperationJournalState::none) {
        update_status(lastUpdate_);
        return false;
    }
    const auto intent =
        action == TrashAction::purge ? TrashOperationIntent::purge : TrashOperationIntent::restore;
    std::size_t operation_items{1};
    if (trashCatalog_) {
        const auto record =
            std::ranges::find_if(trashCatalog_->manifests, [&](const auto &candidate) {
                return candidate.manifest_path.lexically_normal() == manifest.lexically_normal();
            });
        if (record != trashCatalog_->manifests.end()) {
            operation_items = record->transaction.items.size();
        }
    }
    std::filesystem::path restore_destination;
    if (action == TrashAction::restore_to) {
        auto options = QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks;
#ifdef VOVE_UI_TEST_HOOKS
        options |= QFileDialog::DontUseNativeDialog;
#endif
        const auto chosen = QFileDialog::getExistingDirectory(
            this, QCoreApplication::translate("MainWindow", "Restore to…"), currentPath_, options);
        if (chosen.isEmpty()) {
            return false;
        }
        restore_destination = native_path(chosen);
    }
    if (intent == TrashOperationIntent::restore && operation_items > 1) {
        QMessageBox confirmation(
            QMessageBox::Question, QCoreApplication::translate("MainWindow", "Restore operation?"),
            QCoreApplication::translate(
                "MainWindow",
                "The selected object belongs to one deletion operation containing %1 objects. "
                "Restore the entire operation?")
                .arg(static_cast<qulonglong>(operation_items)),
            QMessageBox::Cancel, this);
        confirmation.setObjectName(QStringLiteral("trashRestoreConfirmation"));
        auto *confirm = confirmation.addButton(QCoreApplication::translate("MainWindow", "Restore"),
                                               QMessageBox::AcceptRole);
        confirm->setObjectName(QStringLiteral("trashRestoreConfirmButton"));
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != confirm) {
            return false;
        }
    }
    if (intent == TrashOperationIntent::purge && confirm_permanent_delete) {
        QMessageBox confirmation(
            QMessageBox::Warning, QCoreApplication::translate("MainWindow", "Delete permanently?"),
            QCoreApplication::translate(
                "MainWindow",
                "%1 objects from the selected deletion operation will be deleted and cannot be "
                "restored.")
                .arg(static_cast<qulonglong>(operation_items)),
            QMessageBox::Cancel, this);
        confirmation.setObjectName(QStringLiteral("trashPurgeConfirmation"));
        auto *confirm =
            confirmation.addButton(QCoreApplication::translate("MainWindow", "Delete permanently"),
                                   QMessageBox::DestructiveRole);
        confirm->setObjectName(QStringLiteral("trashPurgeConfirmButton"));
        confirmation.setDefaultButton(QMessageBox::Cancel);
        confirmation.exec();
        if (confirmation.clickedButton() != confirm) {
            return false;
        }
    }
    operationStatus_ =
        intent == TrashOperationIntent::purge
            ? QCoreApplication::translate("MainWindow",
                                          "VO-VE Trash: preparing permanent deletion…")
            : QCoreApplication::translate("MainWindow", "VO-VE Trash: preparing restore…");
    set_delete_in_flight(true);
    update_status(lastUpdate_);
    // A decoder can still hold the file briefly after its generation was cancelled. Stop the
    // isolated helper before moving or deleting it, and gate new preview work until completion.
    previewTimer_->stop();
    begin_preview_generation();
    previewClient_.stop();
    const QPointer<MainWindow> window(this);
    const auto progress = [window](fileops::TrashProgress progress) mutable {
        if (!window.isNull()) {
            QMetaObject::invokeMethod(
                window,
                [window, progress = std::move(progress)]() mutable {
                    if (!window.isNull()) {
                        window->handle_trash_progress(progress);
                    }
                },
                Qt::QueuedConnection);
        }
    };
    const auto completion = [window, intent](fileops::TrashResult result) mutable {
        if (!window.isNull()) {
            QMetaObject::invokeMethod(
                window,
                [window, intent, result = std::move(result)]() mutable {
                    if (!window.isNull()) {
                        window->handle_trash_result(result, intent);
                    }
                },
                Qt::QueuedConnection);
        }
    };
    const auto submitted = intent == TrashOperationIntent::purge
                               ? trashCoordinator_.purge(manifest, progress, completion)
                           : action == TrashAction::restore_to
                               ? trashCoordinator_.restore_to(
                                     manifest, std::move(restore_destination), progress, completion)
                               : trashCoordinator_.restore(manifest, progress, completion);
    if (!submitted) {
        set_delete_in_flight(false);
        previewTimer_->start(0);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Another file operation is still running");
        update_status(lastUpdate_);
    }
    return submitted;
}

bool MainWindow::submit_batch_rename_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || batchRenameCoordinator_.busy() ||
        !batchRenameCoordinator_.owns_recovery() || permanentDeleteCoordinator_.busy() ||
        trashCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    set_rename_in_flight(true);
    const QPointer<MainWindow> window(this);
    const auto submitted = batchRenameCoordinator_.resume(
        [window](fileops::BatchRenameProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_batch_rename_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::BatchRenameResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_batch_rename_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_rename_in_flight(false);
    }
    return submitted;
}

bool MainWindow::submit_delete_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        !permanentDeleteCoordinator_.owns_recovery() || batchRenameCoordinator_.busy() ||
        trashCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    set_delete_in_flight(true);
    const QPointer<MainWindow> window(this);
    const auto submitted = permanentDeleteCoordinator_.resume(
        [window](fileops::PermanentDeleteProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_delete_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::PermanentDeleteResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_delete_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
    }
    return submitted;
}

bool MainWindow::submit_trash_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || trashCoordinator_.busy() ||
        !trashCoordinator_.owns_recovery() || permanentDeleteCoordinator_.busy() ||
        batchRenameCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    const auto loaded = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    const auto restoring = loaded.ok() &&
                           loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
                           loaded.kind == fileops::CurrentOperationKind::trash_restore;
    const auto purging = loaded.ok() &&
                         loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
                         loaded.kind == fileops::CurrentOperationKind::trash_purge;
    const auto intent = purging     ? TrashOperationIntent::purge
                        : restoring ? TrashOperationIntent::restore
                                    : TrashOperationIntent::move;
    set_delete_in_flight(true);
    const QPointer<MainWindow> window(this);
    const auto submitted = trashCoordinator_.resume(
        [window](fileops::TrashProgress progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window, intent](fileops::TrashResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, intent, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_trash_result(result, intent);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_delete_in_flight(false);
    }
    return submitted;
}

bool MainWindow::submit_file_transfer_recovery() {
    if (renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_ ||
        fileTransferCoordinator_.busy() || !fileTransferCoordinator_.owns_recovery() ||
        permanentDeleteCoordinator_.busy() || batchRenameCoordinator_.busy() ||
        trashCoordinator_.busy() || fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    const auto loaded = fileops::CurrentOperationJournalStore(currentOperationJournalPath_).read();
    fileops::FileTransferTransaction transaction;
    std::string detail;
    if (loaded.ok() && loaded.encoding == fileops::CurrentOperationJournalEncoding::typed &&
        loaded.kind == fileops::CurrentOperationKind::file_transfer &&
        fileops::decode_file_transfer_transaction(loaded.payload, transaction, detail)) {
        activeTransferKind_ = transaction.kind;
        activeFileTransferSources_.clear();
        for (const auto &item : transaction.items)
            activeFileTransferSources_.push_back(display_path(item.source));
    }
    set_transfer_in_flight(true);
    const QPointer<MainWindow> window(this);
    const auto submitted = fileTransferCoordinator_.resume(
        [window](fileops::FileTransferProgressUpdate progress) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, progress = std::move(progress)]() mutable {
                        if (!window.isNull()) {
                            window->handle_file_transfer_progress(progress);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::FileTransferResult result) mutable {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, result = std::move(result)]() mutable {
                        if (!window.isNull()) {
                            window->handle_file_transfer_result(result);
                        }
                    },
                    Qt::QueuedConnection);
            }
        },
        [window](fileops::FileTransferConflict conflict) {
            if (!window.isNull()) {
                QMetaObject::invokeMethod(
                    window,
                    [window, conflict = std::move(conflict)] {
                        if (!window.isNull()) {
                            window->handle_file_transfer_conflict(conflict);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_transfer_in_flight(false);
    }
    return submitted;
}

void MainWindow::offer_directory_transfer_discard() {
    const auto recovery_ready = directoryTransferRecoveryRequestId_ != 0U &&
                                !directoryTransferRecoverySource_.isEmpty() &&
                                !directoryTransferRecoveryDestination_.isEmpty() &&
                                !directoryTransferRecoverySourceRevision_.isEmpty();
    if (!recovery_ready || directoryTransferStateCorrupt_ || transferInFlight_ ||
        directoryTransferService_.busy()) {
        return;
    }
    if (directoryTransferDiscarding_) {
        if (!submit_directory_transfer_recovery()) {
            operationStatus_ =
                QCoreApplication::translate("MainWindow", "Failed to continue safe deletion");
            update_status(lastUpdate_);
        }
        return;
    }
    QMessageBox decision(
        QMessageBox::Warning,
        QCoreApplication::translate("MainWindow", "Incomplete folder transfer"),
        QCoreApplication::translate("MainWindow",
                                    "Hidden data was kept. Continue the transfer now, leave it "
                                    "until the next launch, or delete it after another check?"),
        QMessageBox::NoButton, this);
    decision.setObjectName(QStringLiteral("directoryTransferRecoveryDecision"));
    auto *resume = decision.addButton(QCoreApplication::translate("MainWindow", "Continue"),
                                      QMessageBox::AcceptRole);
    auto *keep = decision.addButton(QCoreApplication::translate("MainWindow", "Continue later"),
                                    QMessageBox::RejectRole);
    auto *discard =
        decision.addButton(QCoreApplication::translate("MainWindow", "Delete incomplete data"),
                           QMessageBox::DestructiveRole);
    discard->setObjectName(QStringLiteral("directoryTransferDiscardButton"));
    resume->setObjectName(QStringLiteral("directoryTransferResumeButton"));
    keep->setObjectName(QStringLiteral("directoryTransferKeepButton"));
    decision.setDefaultButton(qobject_cast<QPushButton *>(resume));
    decision.exec();
    if (decision.clickedButton() == keep || decision.clickedButton() == nullptr) {
        return;
    }
    const auto discarding = discard != nullptr && decision.clickedButton() == discard;
    directoryTransferDiscarding_ = discarding;
    if (!persist_directory_transfer_recovery_state()) {
        directoryTransferDiscarding_ = false;
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Failed to save the selected action");
        update_status(lastUpdate_);
        return;
    }
    operationStatus_ =
        discarding
            ? QCoreApplication::translate("MainWindow", "Checking and deleting incomplete data…")
            : QCoreApplication::translate("MainWindow", "Recovering folder transfer…");
    update_status(lastUpdate_);
    if (!submit_directory_transfer_recovery()) {
        operationStatus_ =
            discarding
                ? QCoreApplication::translate("MainWindow", "Failed to start safe deletion")
                : QCoreApplication::translate("MainWindow", "Failed to start transfer recovery");
        update_status(lastUpdate_);
    }
}

bool MainWindow::submit_directory_transfer_recovery() {
    const auto exact_manifest = !directoryTransferRecoveryManifest_.isEmpty();
    const auto discoverable = directoryTransferRecoveryRequestId_ != 0U &&
                              !directoryTransferRecoverySource_.isEmpty() &&
                              !directoryTransferRecoveryDestination_.isEmpty() &&
                              !directoryTransferRecoverySourceRevision_.isEmpty();
    if ((!exact_manifest && !discoverable) || renameInFlight_ || deleteInFlight_ ||
        transferInFlight_ || createDirectoryInFlight_ || directoryTransferService_.busy() ||
        fileTransferCoordinator_.busy() || permanentDeleteCoordinator_.busy() ||
        batchRenameCoordinator_.busy() || trashCoordinator_.busy() ||
        fileOperationService_.busy() || pendingRenameOperation_) {
        return false;
    }
    const fileops::BasicDirectoryTransferStreamRequest request{
        .request_id = next_directory_transfer_request_id(),
        .operation_id = directoryTransferRecoveryRequestId_,
        .command =
            directoryTransferDiscarding_
                ? (exact_manifest ? fileops::BasicDirectoryTransferCommand::discard
                                  : fileops::BasicDirectoryTransferCommand::discover_and_discard)
                : (exact_manifest ? fileops::BasicDirectoryTransferCommand::resume
                                  : fileops::BasicDirectoryTransferCommand::discover_and_resume),
        .source = native_path(directoryTransferRecoverySource_),
        .destination = native_path(directoryTransferRecoveryDestination_),
        .manifest_path = exact_manifest ? native_path(directoryTransferRecoveryManifest_)
                                        : std::filesystem::path{},
        .expected_source_revision_utf8 =
            directoryTransferRecoverySourceRevision_.toUtf8().toStdString(),
    };
    set_transfer_in_flight(true);
    const QPointer<MainWindow> window(this);
    const auto completion_state = encode_directory_transfer_recovery_state(true);
    if (completion_state.isEmpty()) {
        set_transfer_in_flight(false);
        operationStatus_ =
            QCoreApplication::translate("MainWindow", "Recovery state exceeds the safe limit");
        update_status(lastUpdate_);
        return false;
    }
    const auto submitted = directoryTransferService_.submit_directory(
        request,
        [window, completion_state](const fileops::BasicDirectoryTransferProgress &progress) {
            const auto sharedProgress =
                std::make_shared<fileops::BasicDirectoryTransferProgress>(progress);
            if (progress.phase == fileops::BasicDirectoryTransferPhase::completed) {
                QSettings settings;
                settings.setValue(QString::fromLatin1(directoryTransferStateKey), completion_state);
                settings.sync();
                if (settings.status() != QSettings::NoError) {
                    throw std::runtime_error("directory completion evidence was not persisted");
                }
            }
            if (window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                window,
                [window, sharedProgress] {
                    if (!window.isNull()) {
                        window->handle_directory_transfer_progress(*sharedProgress);
                        if (sharedProgress->phase ==
                            fileops::BasicDirectoryTransferPhase::completed) {
                            static_cast<void>(window->record_directory_transfer_completion());
                        }
                    }
                },
                Qt::QueuedConnection);
        },
        [window](fileops::BasicDirectoryTransferResult result) mutable {
            if (!window.isNull()) {
                const auto sharedResult =
                    std::make_shared<fileops::BasicDirectoryTransferResult>(std::move(result));
                QMetaObject::invokeMethod(
                    window,
                    [window, sharedResult] {
                        if (!window.isNull()) {
                            window->handle_directory_transfer_result(*sharedResult);
                        }
                    },
                    Qt::QueuedConnection);
            }
        });
    if (!submitted) {
        set_transfer_in_flight(false);
    }
    return submitted;
}

bool MainWindow::advance_directory_transfer_batch_after_success() {
    if (!pendingDirectoryTransferBatch_) {
        return false;
    }
    auto &batch = *pendingDirectoryTransferBatch_;
    ++batch.completed;
    ++batch.index;
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_.clear();
    directoryTransferRecoveryDestination_.clear();
    directoryTransferRecoverySourceRevision_.clear();
    directoryTransferRecoveryRequestId_ = 0U;
    directoryTransferCompletionKnown_ = false;
    setProperty("directoryTransferRecoveryPending", false);
    if (batch.index < batch.source_paths.size()) {
        if (!submit_next_directory_transfer()) {
            set_transfer_in_flight(false);
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "Failed to continue batch folder transfer");
            update_status(lastUpdate_);
        }
        return true;
    }

    const auto completed = batch.completed;
    const auto kind = batch.kind;
    pendingDirectoryTransferBatch_.reset();
    clear_directory_transfer_recovery_state();
    set_transfer_in_flight(false);
    operationStatus_ =
        kind == fileops::FileTransferKind::move
            ? QCoreApplication::translate("MainWindow", "Folders moved: %1").arg(completed)
            : QCoreApplication::translate("MainWindow", "Folders copied: %1").arg(completed);
    begin_preview_generation();
    refresh_directory_sources();
    pollTimer_->setInterval(16);
    pollTimer_->start();
    update_status(lastUpdate_);
    return true;
}

QByteArray MainWindow::encode_directory_transfer_recovery_state(const bool completion_known) const {
    QJsonObject state{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("operationId"),
         QString::number(static_cast<qulonglong>(directoryTransferRecoveryRequestId_))},
        {QStringLiteral("source"), directoryTransferRecoverySource_},
        {QStringLiteral("destination"), directoryTransferRecoveryDestination_},
        {QStringLiteral("sourceRevision"), directoryTransferRecoverySourceRevision_},
        {QStringLiteral("manifest"), directoryTransferRecoveryManifest_},
        {QStringLiteral("completionKnown"), completion_known},
        {QStringLiteral("discarding"), directoryTransferDiscarding_},
    };
    if (pendingDirectoryTransferBatch_) {
        const auto &batch = *pendingDirectoryTransferBatch_;
        QJsonArray sources;
        QJsonArray revisions;
        for (const auto &source : batch.source_paths) {
            sources.push_back(source);
        }
        for (const auto &revision : batch.source_revisions_utf8) {
            revisions.push_back(QString::fromUtf8(revision));
        }
        state.insert(QStringLiteral("batch"),
                     QJsonObject{
                         {QStringLiteral("kind"), static_cast<int>(batch.kind)},
                         {QStringLiteral("sources"), sources},
                         {QStringLiteral("revisions"), revisions},
                         {QStringLiteral("destination"), display_path(batch.destination_directory)},
                         {QStringLiteral("index"), static_cast<qint64>(batch.index)},
                         {QStringLiteral("completed"), static_cast<qint64>(batch.completed)},
                     });
    }
    return seal_directory_transfer_state(state);
}

bool MainWindow::persist_directory_transfer_recovery_state() {
    if (directoryTransferStateCorrupt_) {
        return false;
    }
    QSettings settings;
    if (directoryTransferRecoveryRequestId_ == 0U || directoryTransferRecoverySource_.isEmpty() ||
        directoryTransferRecoveryDestination_.isEmpty() ||
        directoryTransferRecoverySourceRevision_.isEmpty()) {
        settings.remove(QString::fromLatin1(directoryTransferStateKey));
    } else {
        const auto state =
            encode_directory_transfer_recovery_state(directoryTransferCompletionKnown_);
        if (state.isEmpty()) {
            return false;
        }
        settings.setValue(QString::fromLatin1(directoryTransferStateKey), state);
    }
    settings.remove(QStringLiteral("operations/directoryTransferRecoveryRequestId"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoverySource"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoveryDestination"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoverySourceRevision"));
    settings.remove(QStringLiteral("operations/directoryTransferCompletionKnown"));
    settings.remove(QStringLiteral("operations/directoryTransferDiscarding"));
    settings.remove(QStringLiteral("operations/directoryTransferRecoveryManifest"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchKind"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchSources"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchRevisions"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchDestination"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchIndex"));
    settings.remove(QStringLiteral("operations/directoryTransferBatchCompleted"));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

void MainWindow::clear_directory_transfer_recovery_state() {
    directoryTransferRecoveryManifest_.clear();
    directoryTransferRecoverySource_.clear();
    directoryTransferRecoveryDestination_.clear();
    directoryTransferRecoverySourceRevision_.clear();
    directoryTransferRecoveryRequestId_ = 0U;
    directoryTransferCompletionKnown_ = false;
    directoryTransferDiscarding_ = false;
    directoryTransferStateCorrupt_ = false;
    setProperty("directoryTransferRecoveryPending", false);
    static_cast<void>(persist_directory_transfer_recovery_state());
}

void MainWindow::set_rename_in_flight(const bool in_flight) {
    renameInFlight_ = in_flight;
    if (!in_flight && renamePreviewSuspended_) {
        renamePreviewSuspended_ = false;
        previewTimer_->start();
    }
    setProperty("renameInFlight", in_flight);
    update_rename_action();
    update_operation_status_pulse();
}

void MainWindow::set_delete_in_flight(const bool in_flight) {
    deleteInFlight_ = in_flight;
    setProperty("deleteInFlight", in_flight);
    update_rename_action();
    update_operation_status_pulse();
}

void MainWindow::set_transfer_in_flight(const bool in_flight) {
    transferInFlight_ = in_flight;
    setProperty("fileTransferInFlight", in_flight);
    update_rename_action();
    update_operation_status_pulse();
}

void MainWindow::set_create_directory_in_flight(const bool in_flight) {
    createDirectoryInFlight_ = in_flight;
    setProperty("createDirectoryInFlight", in_flight);
    update_rename_action();
    update_operation_status_pulse();
}

void MainWindow::update_rename_action() {
    const auto entries = selected_entries_in_view_order();
    const auto recursive_ready = !recursiveViewActive_ ||
                                 lastUpdate_.state == catalog::CatalogSessionState::ready;
    const auto same_parent = entries.size() < 2 || std::ranges::all_of(entries, [&](const auto &entry) {
        return same_directory_path(QFileInfo(QString::fromUtf8(entry.path_utf8)).absolutePath(),
            QFileInfo(QString::fromUtf8(entries.front().path_utf8)).absolutePath());
    });
    const auto all_deletable_objects =
        !entries.isEmpty() && std::ranges::all_of(entries, [](const auto &entry) {
            return entry.kind == core::EntryKind::file || entry.kind == core::EntryKind::directory;
        });
    renameAction_->setText(entries.size() > 1
                               ? QCoreApplication::translate("MainWindow", "Batch rename")
                               : QCoreApplication::translate("MainWindow", "Rename"));
    renameAction_->setToolTip(same_parent ? QString{} : QCoreApplication::translate(
        "MainWindow", "Batch renaming requires items from one folder"));
    renameAction_->setEnabled(
        all_deletable_objects && recursive_ready && same_parent && !trashViewActive_ && !globalSearchActive_ && !renameInFlight_ &&
        !deleteInFlight_ && !trashConfirmationInProgress_ && !transferInFlight_ &&
        !createDirectoryInFlight_ && !fileOperationService_.busy() &&
        !pendingRenameOperation_.has_value() && !batchRenameCoordinator_.busy() &&
        !permanentDeleteCoordinator_.busy() && !trashCoordinator_.busy() &&
        !fileTransferCoordinator_.busy() && !directoryTransferService_.busy() &&
        !batchRenameCoordinator_.recovery_pending() &&
        !fileTransferCoordinator_.recovery_pending() && !trashCoordinator_.recovery_pending() &&
        !pendingExternalDirectoryDrop_.has_value() && !pendingExternalTransferDrop_.has_value() &&
        dropProbeSource_.idle() && !property("directoryTransferRecoveryPending").toBool() &&
        !property("fileOperationJournalBlocked").toBool());
    deleteAction_->setText(
        entries.size() > 1 ? QCoreApplication::translate("MainWindow", "Delete Objects…")
        : entries.size() == 1 && entries.front().kind == core::EntryKind::directory
            ? QCoreApplication::translate("MainWindow", "Delete Folder…")
            : QCoreApplication::translate("MainWindow", "Delete…"));
    deleteAction_->setEnabled(
        all_deletable_objects && recursive_ready && !trashViewActive_ && !globalSearchActive_ && !renameInFlight_ &&
        !deleteInFlight_ && !trashConfirmationInProgress_ && !transferInFlight_ &&
        !createDirectoryInFlight_ && !fileOperationService_.busy() &&
        !pendingRenameOperation_.has_value() && !batchRenameCoordinator_.busy() &&
        !permanentDeleteCoordinator_.busy() && !trashCoordinator_.busy() &&
        !fileTransferCoordinator_.busy() && !directoryTransferService_.busy() &&
        !batchRenameCoordinator_.recovery_pending() &&
        !fileTransferCoordinator_.recovery_pending() && !trashCoordinator_.recovery_pending() &&
        !pendingExternalDirectoryDrop_.has_value() && !pendingExternalTransferDrop_.has_value() &&
        dropProbeSource_.idle() && !property("directoryTransferRecoveryPending").toBool() &&
        !property("fileOperationJournalBlocked").toBool());
    if (trashAction_ != nullptr) {
        // Recovery state keeps the catalog readable; only an actively mutating operation blocks it.
        trashAction_->setEnabled(
            !renameInFlight_ && !deleteInFlight_ && !transferInFlight_ &&
            !createDirectoryInFlight_ && !fileOperationService_.busy() &&
            !pendingRenameOperation_.has_value() && !batchRenameCoordinator_.busy() &&
            !permanentDeleteCoordinator_.busy() && !trashCoordinator_.busy() &&
            !fileTransferCoordinator_.busy() && !directoryTransferService_.busy() &&
            !pendingExternalDirectoryDrop_.has_value() &&
            !pendingExternalTransferDrop_.has_value() && dropProbeSource_.idle());
    }
    if (createFolderAction_ != nullptr) {
        createFolderAction_->setEnabled(
            !trashViewActive_ && !globalSearchActive_ && !recursiveViewActive_ && !renameInFlight_ && !deleteInFlight_ &&
            !transferInFlight_ && !createDirectoryInFlight_ && !fileOperationService_.busy() &&
            !pendingRenameOperation_.has_value() && !batchRenameCoordinator_.busy() &&
            !permanentDeleteCoordinator_.busy() && !trashCoordinator_.busy() &&
            !fileTransferCoordinator_.busy() && !directoryTransferService_.busy() &&
            !batchRenameCoordinator_.recovery_pending() &&
            !permanentDeleteCoordinator_.recovery_pending() &&
            !fileTransferCoordinator_.recovery_pending() && !trashCoordinator_.recovery_pending() &&
            !property("directoryTransferRecoveryPending").toBool() &&
            !property("fileOperationJournalBlocked").toBool());
    }
    update_clipboard_actions();
}

void MainWindow::refresh_after_rename(const QString &selection_path) {
    pendingRenamePaths_ = {QDir::cleanPath(QDir::fromNativeSeparators(selection_path))};
    begin_preview_generation();
    refresh_directory_sources();
    pollTimer_->setInterval(16);
    pollTimer_->start();
}

void MainWindow::refresh_after_batch_rename(const QStringList &selection_paths) {
    pendingRenamePaths_.clear();
    pendingRenamePaths_.reserve(selection_paths.size());
    for (const auto &path : selection_paths) {
        pendingRenamePaths_.push_back(QDir::cleanPath(QDir::fromNativeSeparators(path)));
    }
    begin_preview_generation();
    refresh_directory_sources();
    pollTimer_->setInterval(16);
    pollTimer_->start();
}

QModelIndex MainWindow::index_for_path(const QString &path) const {
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (int row = 0; row < listModel_.rowCount(); ++row) {
        const auto index = listModel_.index(row, 0);
        if (QDir::cleanPath(QDir::fromNativeSeparators(
                index.data(DirectoryListModel::EntryPathRole).toString())) == normalized) {
            return index;
        }
    }
    return {};
}

void MainWindow::restore_pending_rename_selection() {
    if (pendingRenamePaths_.isEmpty()) {
        return;
    }
    QItemSelection selection;
    QModelIndex first;
    for (const auto &path : std::as_const(pendingRenamePaths_)) {
        const auto renamed = index_for_path(path);
        if (!renamed.isValid()) {
            continue;
        }
        selection.select(renamed, renamed);
        if (!first.isValid()) {
            first = renamed;
        }
    }
    if (!first.isValid()) {
        pendingRenamePaths_.clear();
        if (std::exchange(pendingCatalogReveal_, false)) {
            operationStatus_ = QCoreApplication::translate(
                "MainWindow", "The file can no longer be found. It may have been "
                              "moved or deleted after the search.");
            update_status(lastUpdate_);
        }
        return;
    }
    listView_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    listView_->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    listView_->scrollTo(first, QAbstractItemView::EnsureVisible);
    pendingRenamePaths_.clear();
    pendingCatalogReveal_ = false;
}

void MainWindow::apply_filter() {
    pendingNavigationRestore_.reset();
    if (globalSearchActive_) {
        update_status(lastUpdate_);
        return;
    }
    listModel_.set_filter(filterEdit_->text());
    update_status(lastUpdate_);
}

QStringList MainWindow::allowed_search_roots() const {
    return searchRoots_;
}

void MainWindow::apply_language(const AppLanguage language, const bool persist) {
    if (!translations_.activate(language)) {
        for (auto *action : languageActions_->actions()) {
            action->setChecked(action->data().toInt() == static_cast<int>(language_));
        }
        return;
    }
    language_ = language;
    QLocale::setDefault(app_language_locale(language_));
    QApplication::setLayoutDirection(app_language_direction(language_));
    toolbarContent_->setLayoutDirection(app_language_direction(language_));
    apply_interface_font(language_);
    setProperty("appLanguage", language_code(language_));
    operationStatus_.clear();
    retranslate_shell();
    apply_artwork_icons();
    windowControls_->set_theme(theme_);
    if (persist) {
        schedule_settings_save();
    }
}

void MainWindow::retranslate_shell() {
    if (logoButton_ == nullptr) {
        return;
    }
    const auto base_font = qApp->font();
    filterEdit_->setFont(base_font);
    globalSearchEdit_->setFont(base_font);
    sortField_->setFont(base_font);
    favoritesTitle_->setFont(base_font);
    favorites_->setFont(base_font);
    trashFavorite_->setFont(base_font);
    directoryTree_->setFont(base_font);
    pathEdit_->setFont(base_font);
    listView_->setFont(base_font);
    // IconMode's automatic wheel step grows with the tile. Keep it tied to readable text instead.
    listView_->verticalScrollBar()->setSingleStep(2 * listView_->fontMetrics().height());
    const auto header_height =
        std::max({pathEdit_->sizeHint().height(), favoritesTitle_->sizeHint().height(),
                  addFavoriteButton_->height()}) +
        7;
    favoritesHeader_->setFixedHeight(header_height);
    directoryHeader_->setFixedHeight(header_height);
    const auto trash_row_height = std::max(24, trashFavorite_->sizeHintForRow(0));
    trashFavorite_->setFixedHeight(trash_row_height);
    chooseFolderAction_->setText(QCoreApplication::translate("MainWindow", "Choose folder"));
    chooseFolderAction_->setToolTip(chooseFolderAction_->text());
    const auto add_favorite_text =
        QCoreApplication::translate("MainWindow", "Add current folder to Favorites");
    addFavoriteButton_->setToolTip(add_favorite_text);
    addFavoriteButton_->setAccessibleName(add_favorite_text);
    auto preview_name_font = base_font;
    preview_name_font.setPointSizeF(base_font.pointSizeF() * 1.8);
    previewName_->setFont(preview_name_font);
    preview_name_font.setPointSizeF(preview_name_font.pointSizeF() * 2.0);
    previewFormat_->setFont(preview_name_font);
    auto status_font = base_font;
    status_font.setPointSizeF(base_font.pointSizeF() * 0.9);
    status_->setFont(status_font);
    previewModified_->setFont(status_font);
    previewCanvas_->set_indicator_font(previewModified_->font());
    previewModified_->refresh();

    logoButton_->setToolTip(ui_text(UiTextId::main_menu));
    logoButton_->setAccessibleName(ui_text(UiTextId::main_menu));
    const auto set_tool_text = [](QToolButton *button, const QString &text) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    };
    set_tool_text(backButton_, QCoreApplication::translate("MainWindow", "Back"));
    update_catalog_tools();
    set_tool_text(forwardButton_, QCoreApplication::translate("MainWindow", "Forward"));
    set_tool_text(historyButton_, QCoreApplication::translate("MainWindow", "Navigation history"));
    set_tool_text(upButton_, QCoreApplication::translate("MainWindow", "Up one level"));
    createFolderAction_->setText(QCoreApplication::translate("MainWindow", "Create Folder"));
    languageMenu_->setTitle(ui_text(UiTextId::language));
    settingsAction_->setText(ui_text(UiTextId::settings));
    updateAction_->setText(ui_text(UiTextId::check_updates));
    aboutAction_->setText(ui_text(UiTextId::about));
    componentsAction_->setText(ui_text(UiTextId::components));
    donateAction_->setText(ui_text(UiTextId::donate));
    reportIssueAction_->setText(ui_text(UiTextId::report_issue));
    for (auto *action : languageActions_->actions()) {
        const auto action_language = static_cast<AppLanguage>(action->data().toInt());
        action->setEnabled(translations_.available(action_language));
        action->setChecked(action->data().toInt() == static_cast<int>(language_));
    }

    filterEdit_->setPlaceholderText(ui_text(UiTextId::local_filter));
    filterEdit_->setAccessibleName(ui_text(UiTextId::local_filter));
    filterEdit_->setToolTip(ui_text(UiTextId::local_filter));
    globalSearchEdit_->setPlaceholderText(ui_text(UiTextId::global_search));
    globalSearchEdit_->setAccessibleName(ui_text(UiTextId::global_search));
    globalSearchEdit_->setToolTip(global_search_tooltip());
    filterEdit_->fit_placeholder();
    globalSearchEdit_->fit_placeholder();
    sortField_->setToolTip(ui_text(UiTextId::sort_field));
    sortField_->setAccessibleName(ui_text(UiTextId::sort_field));
    const auto direction_text =
        ui_text(UiTextId::sort_direction) + QStringLiteral(": ") +
        ui_text(sortDirection_->isChecked() ? UiTextId::descending : UiTextId::ascending);
    sortDirection_->setToolTip(direction_text);
    sortDirection_->setAccessibleName(direction_text);
    const QSignalBlocker field_blocker(sortField_);
    sortField_->setItemText(0, ui_text(UiTextId::sort_name));
    sortField_->setItemText(1, ui_text(UiTextId::sort_extension));
    sortField_->setItemText(2, ui_text(UiTextId::sort_date));
    sortField_->setItemText(3, ui_text(UiTextId::sort_size));
    fit_sort_controls({sortControls_, sortField_, sortDirection_});
    sizeSlider_->setToolTip(ui_text(UiTextId::thumbnail_size));
    sizeSlider_->setAccessibleName(ui_text(UiTextId::thumbnail_size));
    smallerThumbnailsButton_->setToolTip(ui_text(UiTextId::smaller_thumbnails));
    smallerThumbnailsButton_->setAccessibleName(ui_text(UiTextId::smaller_thumbnails));
    largerThumbnailsButton_->setToolTip(ui_text(UiTextId::larger_thumbnails));
    largerThumbnailsButton_->setAccessibleName(ui_text(UiTextId::larger_thumbnails));
    pathEdit_->setPlaceholderText(ui_text(UiTextId::local_or_network_path));
    pathEdit_->setAccessibleName(ui_text(UiTextId::local_or_network_path));
    favoritesTitle_->setText(QCoreApplication::translate("MainWindow", "Favorites"));
    for (int row{}; row < favorites_->count(); ++row) {
        if (is_trash_favorite(favorites_->item(row))) {
            favorites_->item(row)->setText(QCoreApplication::translate("MainWindow", "Trash"));
            favorites_->item(row)->setToolTip(
                QCoreApplication::translate("MainWindow", "VO-VE Trash"));
        }
    }
    if (is_trash_favorite(trashFavorite_->item(0))) {
        trashFavorite_->item(0)->setText(QCoreApplication::translate("MainWindow", "Trash"));
        trashFavorite_->item(0)->setToolTip(
            QCoreApplication::translate("MainWindow", "VO-VE Trash"));
    }
    trashAction_->setText(QCoreApplication::translate("MainWindow", "VO-VE Trash…"));
    set_tool_text(previousPageButton_, QCoreApplication::translate("MainWindow", "Previous page"));
    set_tool_text(nextPageButton_, QCoreApplication::translate("MainWindow", "Next page"));
    passwordButton_->setText(QCoreApplication::translate("MainWindow", "Enter password"));
    passwordButton_->setAccessibleName(
        QCoreApplication::translate("MainWindow", "Enter document password"));
    const auto current_index = listView_->currentIndex();
    const auto *current_entry = listModel_.entry_at(current_index);
    if (current_entry == nullptr) {
        previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Select a file"));
    } else if (current_entry->kind == core::EntryKind::directory) {
        previewCanvas_->setText(QCoreApplication::translate(
            "MainWindow", "Folder\n\nA mosaic will appear after thumbnails are connected"));
        update_preview_support_label(false);
    } else {
        selectedColorSummary_ = color_summary(
            current_index.data(DirectoryListModel::SourceColorModelRole).toString(),
            current_index.data(DirectoryListModel::SourceColorProfileRole).toString());
        if (selectedPageIndex_ == 0U) {
            update_selected_preview(current_index);
        } else {
            const auto preview_ready = !previewCanvas_->pixmap().isNull();
            update_preview_support_label(preview_ready);
            if (!preview_ready) {
                request_selected_page(selectedPageIndex_);
            }
        }
    }

    const std::array theme_ids{UiTextId::theme_north, UiTextId::theme_vanilla,
                               UiTextId::theme_breeze, UiTextId::theme_twilight};
    for (int index = 0; index < static_cast<int>(theme_ids.size()); ++index) {
        if (auto *button = themeButtons_->button(index); button != nullptr) {
            const auto name = ui_text(theme_ids[static_cast<std::size_t>(index)]);
            button->setToolTip(name);
            button->setAccessibleName(name);
        }
    }
    update_rename_action();
    copyNamesAction_->setText(QCoreApplication::translate("MainWindow", "Copy name"));
    cancelPreviewAttemptAction_->setText(
        QCoreApplication::translate("MainWindow", "Cancel preview attempt"));
    copyPathsAction_->setText(QCoreApplication::translate("MainWindow", "Copy full path"));
    copyObjectsAction_->setText(QCoreApplication::translate("MainWindow", "Copy"));
    pasteObjectsAction_->setText(QCoreApplication::translate("MainWindow", "Paste"));
    if (globalSearchActive_) {
        start_global_search();
    } else {
        update_status(lastUpdate_);
    }
    listView_->viewport()->update();
}

void MainWindow::show_update_dialog() {
    const auto version = QCoreApplication::applicationVersion().isEmpty()
                             ? updates::compiled_application_version()
                             : QCoreApplication::applicationVersion();
    auto configuration = updates::compiled_release_configuration();
#ifdef VOVE_UI_TEST_HOOKS
    if (property("disableUpdateNetwork").toBool()) {
        configuration.manifest_url = QUrl{};
        configuration.public_key.clear();
        configuration.releases_api_url = QUrl{};
    }
#endif
    updates::UpdateDialog dialog(std::move(configuration), version, this);
    dialog.exec();
}

void MainWindow::show_about_dialog() {
    const auto version = QCoreApplication::applicationVersion().isEmpty()
                             ? updates::compiled_application_version()
                             : QCoreApplication::applicationVersion();
    AboutDialog dialog(version, this);
    dialog.exec();
}

void MainWindow::show_contact_dialog(const bool support) {
    QDialog dialog(this);
    dialog.setObjectName(support ? QStringLiteral("donateDialog")
                                 : QStringLiteral("reportIssueDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(ui_text(support ? UiTextId::donate : UiTextId::report_issue));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    const auto make_text = [&dialog](const QString &name) {
        auto *text = new QTextBrowser(&dialog);
        text->setObjectName(name);
        text->setFrameShape(QFrame::NoFrame);
        text->setOpenLinks(false);
        text->setOpenExternalLinks(false);
        text->document()->setDefaultFont(dialog.font());
        auto text_palette = text->palette();
        text_palette.setColor(QPalette::Base, dialog.palette().color(QPalette::Window));
        text_palette.setColor(QPalette::Text, dialog.palette().color(QPalette::WindowText));
        text->setPalette(text_palette);
        return text;
    };
    QTextBrowser *message{};
    if (support) {
        message = make_text(QStringLiteral("contactMessage"));
        message->setPlainText(
            QCoreApplication::translate("ProjectContactDialog",
                                        "Send us a few words of thanks - that would mean a lot!") +
            QStringLiteral("\n\n") +
            QCoreApplication::translate(
                "ProjectContactDialog",
                "And if you would like to support us financially, cash only. And only in rubles.") +
            QStringLiteral("\n\n") +
            QCoreApplication::translate(
                "ProjectContactDialog",
                "We can also accept a good bottle of vodka, such as: Chistye Rosy (rye), "
                "Belvedere and Belvedere Single Estate Rye (Lake Bartężek or Smogóry Forest), "
                "Chopin Potato Vodka, Polba, Black Cow, Hangar 1, Luksusowa, Beluga "
                "(Noble / Transatlantic Racing), Spelta, Tito’s Handmade Vodka, Ocean Organic "
                "Vodka, Boyd & Blair, Potocki, Koval Millet or Koval Rye, Reyka, Chopin Rye, "
                "and perhaps others we have yet to discover?"));
        message->setMinimumHeight(4 * message->fontMetrics().height());
        layout->addWidget(message);
    }

    QStringList emails{QStringLiteral("dercar@ya.com")};
    if (support) {
        emails.append(QStringLiteral("Rubilaxik@ya.com"));
    }
    QStringList links;
    for (const auto &email : emails) {
        links.append(QStringLiteral("<a href=\"mailto:%1\" style=\"color: %2;\">%1</a>")
                         .arg(email.toHtmlEscaped(),
                              dialog.palette().color(QPalette::WindowText).name()));
    }
    auto *contact = new QLabel(&dialog);
    contact->setObjectName(QStringLiteral("contactEmail"));
    contact->setTextFormat(Qt::RichText);
    contact->setText(links.join(QStringLiteral(" | ")));
    contact->setLayoutDirection(Qt::LeftToRight);
    contact->setTextInteractionFlags(Qt::TextBrowserInteraction);
    contact->setOpenExternalLinks(false);
    layout->addWidget(contact);

    auto *error = make_text(QStringLiteral("contactError"));
    error->setMinimumHeight(2 * error->fontMetrics().height());
    error->setMaximumHeight(4 * error->fontMetrics().height());
    error->hide();
    layout->addWidget(error);
    connect(contact, &QLabel::linkActivated, &dialog, [emails, error](const QString &href) {
        const QUrl mailto(href);
        const auto email = std::find_if(emails.cbegin(), emails.cend(), [&](const QString &address) {
            return mailto == QUrl(QStringLiteral("mailto:") + address);
        });
        if (email == emails.cend()) {
            return;
        }
        const auto opened = QDesktopServices::openUrl(mailto);
        error->setPlainText(opened
                                ? QString{}
                                : QCoreApplication::translate(
                                      "ProjectContactDialog",
                                      "Could not open the mail application. You can write to %1.")
                                      .arg(*email));
        error->setVisible(!opened);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.ensurePolished();
    const auto available = screen()->availableGeometry();
    const auto width = std::min(600, available.width() - 40);
    auto height = dialog.sizeHint().height();
    if (message != nullptr) {
        const auto margins = layout->contentsMargins();
        QTextDocument measurement;
        measurement.setDefaultFont(message->document()->defaultFont());
        measurement.setPlainText(message->toPlainText());
        measurement.setTextWidth(width - margins.left() - margins.right());
        const auto *message_item = layout->itemAt(layout->indexOf(message));
        const auto layout_height = layout->hasHeightForWidth()
                                       ? layout->totalHeightForWidth(width)
                                       : layout->totalSizeHint().height();
        // Preserve every polished layout row (including the shared title), replacing only
        // QTextBrowser's generic size hint with the height of its wrapped document.
        height = layout_height - message_item->sizeHint().height() +
                 std::max(message_item->minimumSize().height(),
                          qCeil(measurement.size().height()) +
                              2 * message->fontMetrics().height());
    }
    dialog.resize(width, std::min(height, available.height() - 40));
    dialog.exec();
}

void MainWindow::show_components_dialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("componentsDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(ui_text(UiTextId::components_title));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);
    auto *components = new ComponentsPanel(&dialog);
    layout->addWidget(components, 1);

    const QPointer<QDialog> dialog_pointer{&dialog};
    const QPointer<ComponentsPanel> components_pointer{components};
    const auto populate = [components_pointer, dialog_pointer](const bool refresh) {
        if (components_pointer.isNull() || dialog_pointer.isNull()) {
            return;
        }
        auto *components = components_pointer.data();
        std::vector<ComponentSpec> specs;
        specs.push_back(
            {QStringLiteral("LINE Seed JP"), ui_text(UiTextId::line_seed_purpose),
             QUrl(QStringLiteral("https://raw.githubusercontent.com/google/fonts/main/ofl/"
                                 "lineseedjp/LINESeedJP-Regular.ttf")),
             line_seed_jp_available(refresh), false});
        specs.push_back({QStringLiteral("Ghostscript"), ui_text(UiTextId::ghostscript_purpose),
                         QUrl(QStringLiteral("https://ghostscript.com/releases/gsdnld.html")),
                         ghostscript_available(false), true});
#ifndef Q_OS_WIN
        specs.push_back({QStringLiteral("bubblewrap"), ui_text(UiTextId::bubblewrap_purpose),
                         QUrl(QStringLiteral("https://github.com/containers/bubblewrap")),
                         bubblewrap_available(), true});
#endif
#ifdef Q_OS_WIN
        specs.push_back({QStringLiteral("Everything"), ui_text(UiTextId::everything_purpose),
                         QUrl(QStringLiteral("https://www.voidtools.com/downloads/")),
                         everything_available(refresh), false});
#else
        specs.push_back({QStringLiteral("plocate"), ui_text(UiTextId::plocate_purpose),
                         QUrl(QStringLiteral("https://plocate.sesse.net/")),
                         plocate_available(refresh), false});
#endif
        components->set_components(specs);
    };
    populate(false);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, Qt::Horizontal, &dialog);
    auto *check_again = new QPushButton(ui_text(UiTextId::check_again), &dialog);
    check_again->setObjectName(QStringLiteral("checkComponentsAgain"));
    buttons->addButton(check_again, QDialogButtonBox::ActionRole);
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    const QPointer<QPushButton> check_pointer{check_again};
    connect(check_again, &QPushButton::clicked, &dialog,
            [this, dialog_pointer, check_pointer, populate] {
                if (!check_pointer.isNull()) {
                    check_pointer->setEnabled(false);
                }
                previewClient_.refresh_external_components(
                    [this, dialog_pointer, check_pointer, populate](const bool changed) {
                        if (changed) {
                            retry_external_component_previews();
                        }
                        if (dialog_pointer.isNull()) {
                            return;
                        }
                        populate(true);
                        if (!check_pointer.isNull()) {
                            check_pointer->setEnabled(true);
                        }
                    });
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.ensurePolished();
    layout->activate();
    const auto available = screen()->availableGeometry();
    const auto width = std::min(800, available.width() - 40);
    const auto margins = layout->contentsMargins();
    const auto content_width = width - margins.left() - margins.right();
    auto natural_height = margins.top() + margins.bottom() +
                          layout->spacing() * (layout->count() - 1);
    for (int index = 0; index < layout->count(); ++index) {
        const auto *item = layout->itemAt(index);
        // The panel's inner QScrollArea hides its natural height from QWidgetItem.
        natural_height += item->widget() == components
                              ? components->heightForWidth(content_width)
                              : (item->hasHeightForWidth() ? item->heightForWidth(content_width)
                                                           : item->sizeHint().height());
    }
    dialog.resize(width, std::min(available.height() - 60, natural_height));
    check_again->click();
    dialog.exec();
}

void MainWindow::retry_external_component_previews() {
    using Status = preview::helper_protocol::ResponseStatus;
    const auto component_missing = [](const Status status) {
        return status == Status::ghostscript_required;
    };
    if (selectedPreview_ && component_missing(selectedPreview_->status)) {
        reset_selected_preview();
        previewTimer_->start(0);
    }
    auto retry_ids =
        listModel_.clear_previews_with_status(static_cast<int>(Status::ghostscript_required));
    for (const auto id : retry_ids) {
        completedPreviewEdges_.remove(id);
    }
    folderMosaicController_.invalidate_display_images();
    if (!retry_ids.isEmpty() || listModel_.visible_directories() != 0) {
        update_selected_preview(listView_->currentIndex());
        previewTimer_->start(0);
    }
}

void MainWindow::show_settings_dialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("settingsDialog"));
    dialog.setProperty("mainMenuDialog", true);
    dialog.setWindowTitle(ui_text(UiTextId::settings_title));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setObjectName(QStringLiteral("settingsScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(12, 8, 12, 8);
    content_layout->setSpacing(12);
    content_layout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    const auto add_section_break = [content_layout, content] {
        content_layout->addSpacing(4);
        auto *separator = new QFrame(content);
        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setProperty("settingsSectionSeparator", true);
        content_layout->addWidget(separator);
        content_layout->addSpacing(4);
    };
    const auto configure_compact_list = [&dialog](QListWidget *list) {
        const auto update_height = [list] {
            const auto row_height =
                std::max(list->sizeHintForRow(0), list->fontMetrics().lineSpacing() + 4);
            const auto visible_rows = std::clamp(list->count(), 2, 8);
            const auto height = visible_rows * row_height + 2 * list->frameWidth();
            list->setMinimumHeight(height);
            list->setMaximumHeight(height);
            list->setProperty("compactVisibleRows", visible_rows);
        };
        QObject::connect(list->model(), &QAbstractItemModel::rowsInserted, &dialog,
                         [update_height] { update_height(); });
        QObject::connect(list->model(), &QAbstractItemModel::rowsRemoved, &dialog,
                         [update_height] { update_height(); });
        update_height();
    };
    auto *title = new QLabel(ui_text(UiTextId::search_roots_title), &dialog);
    auto title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    content_layout->addWidget(title);
    auto *explanation = new QLabel(ui_text(UiTextId::search_roots_description), &dialog);
    explanation->setWordWrap(true);
    content_layout->addWidget(explanation);

    auto *roots = new QListWidget(&dialog);
    roots->setObjectName(QStringLiteral("searchRootsList"));
    roots->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &path : std::as_const(searchRoots_)) {
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(path), roots);
        item->setData(Qt::UserRole, path);
    }
    configure_compact_list(roots);
    roots->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    roots->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    content_layout->addWidget(roots);

    auto *path_row = new QHBoxLayout;
    auto *path = new QLineEdit(&dialog);
    path->setObjectName(QStringLiteral("searchRootPath"));
    path->setPlaceholderText(ui_text(UiTextId::local_or_unc_path));
    auto *browse = new QPushButton(ui_text(UiTextId::browse), &dialog);
    auto *add = new QPushButton(ui_text(UiTextId::add), &dialog);
    auto *remove = new QPushButton(ui_text(UiTextId::remove), &dialog);
    path_row->addWidget(path, 1);
    path_row->addWidget(browse);
    path_row->addWidget(add);
    path_row->addWidget(remove);
    content_layout->addLayout(path_row);

    add_section_break();
    auto *applications_title = new QLabel(ui_text(UiTextId::external_applications_title), &dialog);
    auto applications_title_font = applications_title->font();
    applications_title_font.setBold(true);
    applications_title->setFont(applications_title_font);
    content_layout->addWidget(applications_title);
    auto *applications_explanation =
        new QLabel(ui_text(UiTextId::external_applications_description), &dialog);
    applications_explanation->setWordWrap(true);
    content_layout->addWidget(applications_explanation);
    auto *applications = new QListWidget(&dialog);
    applications->setObjectName(QStringLiteral("externalApplicationsList"));
    applications->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto &application : externalApplications_.applications()) {
        auto *item = new QListWidgetItem(application.name, applications);
        item->setData(Qt::UserRole, application.path);
        item->setToolTip(QDir::toNativeSeparators(application.path));
    }
    configure_compact_list(applications);
    content_layout->addWidget(applications);
    auto *remove_application = new QPushButton(ui_text(UiTextId::remove), &dialog);
    remove_application->setObjectName(QStringLiteral("removeExternalApplication"));
    remove_application->setEnabled(false);
    content_layout->addWidget(remove_application, 0, Qt::AlignTrailing);

    add_section_break();
    auto *startup_title = new QLabel(ui_text(UiTextId::startup_title), &dialog);
    auto startup_title_font = startup_title->font();
    startup_title_font.setBold(true);
    startup_title->setFont(startup_title_font);
    content_layout->addWidget(startup_title);
    auto *start_with_system = new QCheckBox(ui_text(UiTextId::start_with_system), &dialog);
    start_with_system->setObjectName(QStringLiteral("startWithSystem"));
    start_with_system->setChecked(startup_registration_enabled());
    content_layout->addWidget(start_with_system);

    add_section_break();
    auto *cache_title = new QLabel(ui_text(UiTextId::preview_cache), &dialog);
    auto cache_title_font = cache_title->font();
    cache_title_font.setBold(true);
    cache_title->setFont(cache_title_font);
    content_layout->addWidget(cache_title);
    auto *cache_row = new QHBoxLayout;
    auto *cache_occupied = new QLabel(ui_text(UiTextId::cache_occupied), &dialog);
    auto *cache_usage = new QLabel(ui_text(UiTextId::calculating), &dialog);
    cache_usage->setObjectName(QStringLiteral("previewCacheUsage"));
    auto *cache_limit_label = new QLabel(ui_text(UiTextId::cache_limit), &dialog);
    auto *cache_limit = new QComboBox(&dialog);
    cache_limit->setObjectName(QStringLiteral("previewCacheLimit"));
    constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
    for (const auto gib : {1ULL, 2ULL, 5ULL, 10ULL, 20ULL, 50ULL}) {
        cache_limit->addItem(QStringLiteral("%1 GiB").arg(gib),
                             QVariant::fromValue<qulonglong>(gib * gibibyte));
    }
    cache_limit->setCurrentIndex(
        std::max(0, cache_limit->findData(QVariant::fromValue<qulonglong>(cacheMaximumBytes_))));
    auto *clear_cache = new QPushButton(ui_text(UiTextId::clear_cache), &dialog);
    clear_cache->setObjectName(QStringLiteral("clearPreviewCache"));
    cache_row->addWidget(cache_occupied);
    cache_row->addWidget(cache_usage, 1);
    cache_row->addWidget(cache_limit_label);
    cache_row->addWidget(cache_limit);
    cache_row->addWidget(clear_cache);
    content_layout->addLayout(cache_row);
    auto *cache_error = new QLabel(&dialog);
    cache_error->setObjectName(QStringLiteral("previewCacheError"));
    cache_error->setStyleSheet(QStringLiteral("color: #c62828;"));
    cache_error->hide();
    content_layout->addWidget(cache_error);

    add_section_break();
    auto *trash_title =
        new QLabel(QCoreApplication::translate("MainWindow", "VO-VE Trash"), &dialog);
    auto trash_title_font = trash_title->font();
    trash_title_font.setBold(true);
    trash_title->setFont(trash_title_font);
    content_layout->addWidget(trash_title);
    auto *trash_row = new QHBoxLayout;
    auto *trash_limit_label = new QLabel(
        QCoreApplication::translate("MainWindow", "Maximum per storage device"), &dialog);
    auto *trash_limit = new QComboBox(&dialog);
    trash_limit->setObjectName(QStringLiteral("trashMaximumBytes"));
    for (const auto gib : {10ULL, 20ULL, 50ULL, 100ULL, 200ULL}) {
        trash_limit->addItem(QStringLiteral("%1 GiB").arg(gib),
                             QVariant::fromValue<qulonglong>(gib * gibibyte));
    }
    trash_limit->setCurrentIndex(
        std::max(0, trash_limit->findData(QVariant::fromValue<qulonglong>(trashMaximumBytes_))));
    auto *empty_trash =
        new QPushButton(QCoreApplication::translate("MainWindow", "Empty Trash…"), &dialog);
    empty_trash->setObjectName(QStringLiteral("emptyTrashFromSettings"));
    trash_row->addWidget(trash_limit_label);
    trash_row->addWidget(trash_limit);
    trash_row->addStretch(1);
    trash_row->addWidget(empty_trash);
    content_layout->addLayout(trash_row);
    auto *trash_explanation = new QLabel(
        QCoreApplication::translate(
            "MainWindow",
            "The effective limit is the smaller of this value and 10% of the storage capacity. "
            "Trash is emptied only manually."),
        &dialog);
    trash_explanation->setWordWrap(true);
    content_layout->addWidget(trash_explanation);
    connect(empty_trash, &QPushButton::clicked, &dialog, [this, &dialog] {
        dialog.accept();
        QTimer::singleShot(0, this, &MainWindow::show_trash_cleanup);
    });

    const QPointer<QLabel> cache_usage_pointer{cache_usage};
    const QPointer<QLabel> cache_error_pointer{cache_error};
    const QPointer<QPushButton> clear_cache_pointer{clear_cache};
    const auto show_cache_result = [cache_usage_pointer, cache_error_pointer,
                                    clear_cache_pointer](const CacheReply reply) {
        if (cache_usage_pointer.isNull()) {
            return;
        }
        if (reply.success) {
            cache_usage_pointer->setText(
                QLocale().formattedDataSize(static_cast<qint64>(reply.bytes)));
            if (!cache_error_pointer.isNull()) {
                cache_error_pointer->hide();
            }
        } else {
            cache_usage_pointer->setText(QStringLiteral("—"));
            if (!cache_error_pointer.isNull()) {
                cache_error_pointer->setText(ui_text(UiTextId::cache_clear_failed));
                cache_error_pointer->show();
            }
        }
        if (!clear_cache_pointer.isNull()) {
            clear_cache_pointer->setEnabled(true);
        }
    };
    if (previewClient_.inspect_cache(show_cache_result) == 0) {
        show_cache_result({});
    }
    connect(clear_cache, &QPushButton::clicked, &dialog,
            [this, cache_usage_pointer, clear_cache_pointer, show_cache_result] {
                if (!clear_cache_pointer.isNull()) {
                    clear_cache_pointer->setEnabled(false);
                }
                if (!cache_usage_pointer.isNull()) {
                    cache_usage_pointer->setText(ui_text(UiTextId::calculating));
                }
                const auto request = previewClient_.clear_cache(
                    [this, cache_usage_pointer, show_cache_result](const CacheReply reply) {
                        show_cache_result(reply);
                        if (!reply.success) {
                            return;
                        }
                        listModel_.clear_previews();
                        completedPreviewEdges_.clear();
                        folderMosaicController_.invalidate_display_images();
                        begin_preview_generation(true);
                        if (!cache_usage_pointer.isNull()) {
                            cache_usage_pointer->setText(ui_text(UiTextId::calculating));
                        }
                        QTimer::singleShot(2000, this, [this, show_cache_result] {
                            if (previewClient_.inspect_cache(show_cache_result) == 0) {
                                show_cache_result({});
                            }
                        });
                    });
                if (request == 0) {
                    show_cache_result({});
                }
            });

    const auto add_path = [roots, path] {
        const auto candidate = QDir::cleanPath(QDir::fromNativeSeparators(path->text().trimmed()));
        if (candidate.isEmpty() || !native_path(candidate).is_absolute()) {
            return;
        }
#ifdef Q_OS_WIN
        constexpr auto path_case = Qt::CaseInsensitive;
#else
        constexpr auto path_case = Qt::CaseSensitive;
#endif
        for (int index = 0; index < roots->count(); ++index) {
            if (roots->item(index)->data(Qt::UserRole).toString().compare(candidate, path_case) ==
                0) {
                roots->setCurrentRow(index);
                path->clear();
                return;
            }
        }
        auto *item = new QListWidgetItem(QDir::toNativeSeparators(candidate), roots);
        item->setData(Qt::UserRole, candidate);
        roots->setCurrentItem(item);
        path->clear();
    };
    connect(path, &QLineEdit::returnPressed, &dialog, add_path);
    connect(add, &QPushButton::clicked, &dialog, add_path);
    connect(browse, &QPushButton::clicked, &dialog, [this, path] {
        const auto selected = QFileDialog::getExistingDirectory(
            this, QCoreApplication::translate("MainWindow", "Allow Global Search in Folder"),
            currentPath_, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!selected.isEmpty()) {
            path->setText(selected);
        }
    });
    connect(remove, &QPushButton::clicked, &dialog, [roots] {
        const auto selected = roots->selectedItems();
        for (auto *item : selected) {
            delete roots->takeItem(roots->row(item));
        }
    });
    connect(applications, &QListWidget::itemSelectionChanged, &dialog,
            [applications, remove_application] {
                remove_application->setEnabled(!applications->selectedItems().isEmpty());
            });
    connect(remove_application, &QPushButton::clicked, &dialog, [applications] {
        const auto selected = applications->selectedItems();
        for (auto *item : selected) {
            delete applications->takeItem(applications->row(item));
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close,
                                         Qt::Horizontal, &dialog);
    buttons->setContentsMargins(12, 0, 12, 0);
    auto *check_updates = new QPushButton(ui_text(UiTextId::check_updates), &dialog);
    check_updates->setObjectName(QStringLiteral("settingsCheckUpdates"));
    check_updates->setAutoDefault(false);
    buttons->addButton(check_updates, QDialogButtonBox::ActionRole);
    connect(check_updates, &QPushButton::clicked, this, &MainWindow::show_update_dialog);
    buttons->button(QDialogButtonBox::Save)->setText(ui_text(UiTextId::save));
    buttons->button(QDialogButtonBox::Close)->setText(ui_text(UiTextId::close));
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [&dialog, start_with_system] {
                QString error;
                if (!set_startup_registration_enabled(start_with_system->isChecked(), &error)) {
                    QMessageBox message(QMessageBox::Warning, ui_text(UiTextId::settings_title),
                                        ui_text(UiTextId::startup_change_failed),
                                        QMessageBox::Ok, &dialog);
                    message.setInformativeText(error);
                    message.exec();
                    return;
                }
                dialog.accept();
            });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto fit_dialog_to_content = [&dialog, content, content_layout, buttons, layout] {
        content_layout->activate();
        const auto available = dialog.screen()->availableGeometry().size();
        const auto margins = layout->contentsMargins();
        const auto natural_height = content->sizeHint().height() + buttons->sizeHint().height() +
                                    margins.top() + margins.bottom() + layout->spacing();
        const auto width = std::min(700, std::max(360, available.width() - 40));
        const auto height_limit = std::max(240, available.height() - 80);
        dialog.resize(width, std::min(height_limit, std::max(360, natural_height)));
    };
    const auto schedule_dialog_fit = [&dialog, fit_dialog_to_content] {
        QTimer::singleShot(0, &dialog, fit_dialog_to_content);
    };
    connect(roots->model(), &QAbstractItemModel::rowsInserted, &dialog,
            [schedule_dialog_fit] { schedule_dialog_fit(); });
    connect(roots->model(), &QAbstractItemModel::rowsRemoved, &dialog,
            [schedule_dialog_fit] { schedule_dialog_fit(); });
    connect(applications->model(), &QAbstractItemModel::rowsInserted, &dialog,
            [schedule_dialog_fit] { schedule_dialog_fit(); });
    connect(applications->model(), &QAbstractItemModel::rowsRemoved, &dialog,
            [schedule_dialog_fit] { schedule_dialog_fit(); });
    fit_dialog_to_content();
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    std::vector<std::string> encoded;
    encoded.reserve(static_cast<std::size_t>(roots->count()));
    for (int index = 0; index < roots->count(); ++index) {
        encoded.push_back(roots->item(index)->data(Qt::UserRole).toString().toUtf8().toStdString());
    }
    searchRoots_.clear();
    for (const auto &root : search::normalize_allowed_roots(encoded)) {
        searchRoots_.push_back(QString::fromUtf8(root));
    }
    cacheMaximumBytes_ = cache_limit->currentData().toULongLong();
    previewClient_.set_cache_maximum_bytes(cacheMaximumBytes_);
    trashMaximumBytes_ = trash_limit->currentData().toULongLong();
    trashCoordinator_.set_maximum_bytes(trashMaximumBytes_);
    if (trashCatalog_) {
        update_trash_favorite_state(*trashCatalog_);
    }
    const auto saved_applications = externalApplications_.applications();
    for (const auto &application : saved_applications) {
        bool retained{};
        for (int index = 0; index < applications->count(); ++index) {
            if (applications->item(index)->data(Qt::UserRole).toString() == application.path) {
                retained = true;
                break;
            }
        }
        if (!retained) {
            static_cast<void>(externalApplications_.remove(application.path));
        }
    }
    schedule_settings_save();
    if (globalSearchActive_ && !globalSearchEdit_->text().trimmed().isEmpty()) {
        start_global_search();
    }
}

void MainWindow::show_entry_in_catalog(const core::DirectoryEntry &entry) {
    const auto path = QString::fromUtf8(entry.path_utf8);
    const auto parent = QFileInfo(path).absolutePath();
    if (parent.isEmpty()) {
        return;
    }
    {
        const QSignalBlocker blocker(globalSearchEdit_);
        globalSearchEdit_->clear();
    }
    leave_global_search(false);
    filterEdit_->clear();
    pendingRenamePaths_ = {QDir::cleanPath(QDir::fromNativeSeparators(path))};
    pendingCatalogReveal_ = true;
    open_path(parent, true);
}

void MainWindow::schedule_global_search() {
    if (globalSearchActive_) update_catalog_tools();
    if (globalSearchEdit_->text().trimmed().isEmpty()) {
        globalSearchTimer_->stop();
        if (globalSearchActive_) {
            leave_global_search(true);
        }
        return;
    }
    globalSearchTimer_->start();
}

void MainWindow::start_global_search() {
    if (transferInFlight_ || deleteInFlight_ || renameInFlight_ || createDirectoryInFlight_) {
        if (!globalSearchEdit_->text().trimmed().isEmpty()) globalSearchTimer_->start();
        return;
    }
    if (pendingExternalTransferDrop_ && pendingExternalTransferDrop_->search_generation != 0)
        fail_external_transfer_drop(QCoreApplication::translate("MainWindow", "Transfer cancelled. Objects transferred: %1").arg(0));
    const auto query = globalSearchEdit_->text().trimmed();
    if (query.isEmpty()) {
        if (globalSearchActive_) {
            leave_global_search(true);
        }
        return;
    }

    if (recursiveViewActive_) leave_recursive_view(true);
    globalSearchClient_.cancel();
    activeGlobalSearchGeneration_ = 0;
    globalSearchQuery_ = query;
    globalSearchReady_ = false;
    operationStatus_.clear();
    const auto roots = allowed_search_roots();
    if (!globalSearchActive_) {
        directoryMonitor_.set_visible(false);
        directoryMonitor_.set_directory({});
        automaticRefreshInFlight_ = false;
        session_.suspend();
        pollTimer_->stop();
        catalogBeforeGlobalSearch_ = take_catalog_view_state();
        catalogBeforeGlobalSearchDirty_ = false;
        globalSearchActive_ = true;
        setProperty("globalSearchActive", true);
        filterEdit_->setEnabled(false);
        listView_->setDragEnabled(false);
        listView_->setAcceptDrops(false);
        listView_->viewport()->setAcceptDrops(false);
        listView_->setDragDropMode(QAbstractItemView::DragOnly);
        listModel_.set_filter({});
    }

    leave_preview_protected_state();
    begin_preview_generation();
    listModel_.begin_catalog();
    update_catalog_tools();
    selectedEntryId_ = 0;
    selectedName_.clear();
    selectedColorSummary_.clear();
    previewCanvas_->setPixmap({});
    previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Select a file"));
    previewName_->clear();
    previewName_->setToolTip({});
    previewFormat_->clear();
    previewFormat_->hide();
    update_page_navigation(0, 0);
    globalSearchResultCount_ = 0;
    previewModified_->set_modified_time(std::nullopt);
    globalSearchTruncated_ = false;
    globalSearchStatus_ = QCoreApplication::translate("MainWindow", "Global search…");
    show_global_search_message(
        QCoreApplication::translate("MainWindow", "Searching the system filename index…"));
    update_rename_action();
    update_status(lastUpdate_);
    if (roots.isEmpty()) {
        if (listModel_.loading()) {
            listModel_.finish_catalog();
        }
        globalSearchStatus_ =
            QCoreApplication::translate("MainWindow", "Global search · scope not configured");
        show_global_search_message(QCoreApplication::translate(
            "MainWindow", "Set work folders in <b>VO-VE → Settings…</b>."));
        update_status(lastUpdate_);
        return;
    }
    activeGlobalSearchGeneration_ = globalSearchClient_.start(query, roots);
}

void MainWindow::handle_global_search_batch(search::SearchBatch batch) {
    if (!globalSearchActive_ || globalSearchReady_ || batch.generation != activeGlobalSearchGeneration_) {
        return;
    }

    if (batch.status != search::SearchStatus::success) {
        if (listModel_.loading()) {
            listModel_.abort_catalog();
        }
        previewTimer_->start();
        globalSearchStatus_ =
            batch.status == search::SearchStatus::timed_out
                ? QCoreApplication::translate("MainWindow", "Global search timed out")
                : QCoreApplication::translate("MainWindow", "Global search is unavailable");
        switch (batch.status) {
        case search::SearchStatus::provider_unavailable:
            show_global_search_message(global_search_unavailable_message());
            break;
        case search::SearchStatus::scope_not_indexed: {
            auto root = QString::fromUtf8(batch.message_utf8).toHtmlEscaped();
            const auto root_detail =
                root.isEmpty() ? QString{} : QStringLiteral("<br><code>%1</code>").arg(root);
            show_global_search_message(global_search_scope_message(root_detail));
            break;
        }
        case search::SearchStatus::timed_out:
            show_global_search_message(QCoreApplication::translate(
                "MainWindow", "The filename index did not respond in time. Retry the search."));
            break;
        case search::SearchStatus::invalid_request:
            show_global_search_message(QCoreApplication::translate(
                "MainWindow", "Enter search text and select an available search scope."));
            break;
        case search::SearchStatus::io_error:
            show_global_search_message(QCoreApplication::translate(
                "MainWindow", "Failed to get results from the filename index."));
            break;
        case search::SearchStatus::cancelled:
            show_global_search_message(
                QCoreApplication::translate("MainWindow", "Search cancelled."));
            break;
        case search::SearchStatus::success:
            break;
        }
        update_status(lastUpdate_);
        return;
    }

    std::erase_if(batch.entries, [this](const auto &entry) {
        return removed_global_search_path(entry.path_utf8);
    });
    globalSearchResultCount_ += static_cast<std::uint64_t>(batch.entries.size());
    if (!batch.entries.empty()) {
        listModel_.append_catalog(std::move(batch.entries));
        globalSearchMessage_->hide();
    }
    globalSearchTruncated_ = globalSearchTruncated_ || batch.truncated;
    if (batch.is_final) {
        if (listModel_.loading()) {
            listModel_.finish_catalog();
        }
        globalSearchReady_ = true;
        globalSearchResultCount_ = listModel_.total_size();
        globalSearchStatus_ = QCoreApplication::translate("MainWindow", "Global search · %1 found")
                                  .arg(static_cast<qulonglong>(globalSearchResultCount_));
        if (batch.provider_index_modified_unix_ns > 0) {
            const auto modified =
                QDateTime::fromMSecsSinceEpoch(batch.provider_index_modified_unix_ns / 1'000'000);
            globalSearchStatus_ +=
                QCoreApplication::translate("MainWindow", " · index %1")
                    .arg(QLocale().toString(modified.toLocalTime(), QLocale::ShortFormat));
        }
        if (globalSearchTruncated_) {
            globalSearchStatus_ += QCoreApplication::translate("MainWindow", " · results limited");
        }
        if (globalSearchResultCount_ == 0) {
            show_global_search_message(QCoreApplication::translate("MainWindow", "Nothing found"));
        }
    } else {
        globalSearchStatus_ = QCoreApplication::translate("MainWindow", "Global search… · %1 found")
                                  .arg(static_cast<qulonglong>(globalSearchResultCount_));
    }
    previewTimer_->start();
    update_catalog_tools();
    update_status(lastUpdate_);
}

bool MainWindow::removed_global_search_path(const std::string &path) const {
    if (globalSearchRemovedPaths_.isEmpty()) return false;
    auto current = native_path(QString::fromUtf8(path)).lexically_normal();
    if (globalSearchObservedPaths_.contains(transfer_path_key(current))) return false;
    while (!current.empty()) {
        if (globalSearchRemovedPaths_.contains(transfer_path_key(current))) return true;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return false;
}

void MainWindow::allow_observed_global_search_path(const QString &path) {
    if (!removed_global_search_path(path.toUtf8().toStdString())) return;
    auto current = native_path(path).lexically_normal();
    while (!current.empty()) {
        if (globalSearchObservedPaths_.size() == static_cast<qsizetype>(search::kMaximumSearchResults)) break;
        // A recreated folder proves only its own existence, not any former children in the index.
        globalSearchObservedPaths_.insert(transfer_path_key(current));
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}

void MainWindow::reconcile_global_search_move(const QStringList &paths) {
    if (paths.isEmpty()) return;
    // The queue reports acknowledged moves, including recovery; never infer them from the index.
    QSet<QString> moved;
    for (const auto &path : paths) {
        if (globalSearchRemovedPaths_.size() == static_cast<qsizetype>(search::kMaximumSearchResults)) break;
        const auto key = transfer_path_key(native_path(path));
        globalSearchRemovedPaths_.insert(key);
        moved.insert(key);
    }
    for (auto it = globalSearchObservedPaths_.begin(); it != globalSearchObservedPaths_.end();) {
        auto candidate = *it;
        bool removed = false;
        while (!candidate.isEmpty()) {
            if (moved.contains(candidate)) {
                removed = true;
                break;
            }
            const auto slash = candidate.lastIndexOf(QLatin1Char('/'));
            if (slash < 0) break;
            candidate.truncate(slash);
        }
        if (removed) it = globalSearchObservedPaths_.erase(it);
        else ++it;
    }
    if (!globalSearchActive_) return;
    auto grid = capture_grid_view_state();
    std::vector<core::DirectoryEntry> remaining;
    for (int row = 0; row < listModel_.rowCount(); ++row) {
        const auto *entry = listModel_.entry_at(listModel_.index(row, 0));
        if (entry && !removed_global_search_path(entry->path_utf8)) remaining.push_back(*entry);
    }
    listModel_.replace_catalog(std::move(remaining));
    restore_grid_view_state(grid);
    if (!listModel_.entry_for_id(selectedEntryId_)) reset_selected_preview();
    globalSearchResultCount_ = listModel_.total_size();
    globalSearchStatus_ = QCoreApplication::translate("MainWindow", "Global search · %1 found")
        .arg(static_cast<qulonglong>(globalSearchResultCount_));
    if (globalSearchTruncated_)
        globalSearchStatus_ += QCoreApplication::translate("MainWindow", " · results limited");
    if (globalSearchResultCount_ == 0)
        show_global_search_message(QCoreApplication::translate("MainWindow", "Nothing found"));
}

void MainWindow::leave_global_search(const bool restore_catalog) {
    if (pendingExternalTransferDrop_ && pendingExternalTransferDrop_->search_generation != 0)
        fail_external_transfer_drop(QCoreApplication::translate("MainWindow", "Transfer cancelled. Objects transferred: %1").arg(0));
    globalSearchTimer_->stop();
    globalSearchClient_.cancel();
    activeGlobalSearchGeneration_ = 0;
    globalSearchResultCount_ = 0;
    globalSearchTruncated_ = false;
    globalSearchStatus_.clear();
    if (listModel_.loading()) {
        listModel_.abort_catalog();
    }
    globalSearchActive_ = false;
    globalSearchReady_ = false;
    globalSearchQuery_.clear();
    directoryMonitor_.set_directory(currentPath_);
    update_directory_monitor_visibility();
    setProperty("globalSearchActive", false);
    globalSearchMessage_->hide();
    filterEdit_->setEnabled(true);
    listView_->setDragEnabled(true);
    listView_->setAcceptDrops(true);
    listView_->viewport()->setAcceptDrops(true);
    listView_->setDragDropMode(QAbstractItemView::DragDrop);
    listModel_.set_filter(filterEdit_->text());
    update_rename_action();

    auto saved = std::exchange(catalogBeforeGlobalSearch_, std::nullopt);
    const auto catalog_dirty = std::exchange(catalogBeforeGlobalSearchDirty_, false);
    if (!restore_catalog || currentPath_.isEmpty() || !saved.has_value()) {
        return;
    }
    restore_catalog_view_state(std::move(*saved));
    const auto resume_catalog = catalog_dirty || lastUpdate_.state == catalog::CatalogSessionState::loading ||
                                lastUpdate_.state == catalog::CatalogSessionState::waiting_retry;
    if (resume_catalog) {
        refresh_directory_sources();
        lastUpdate_.state = catalog::CatalogSessionState::loading;
        lastUpdate_.truncated = false;
        lastUpdate_.error = {};
        lastUpdate_.retry_in.reset();
        pollTimer_->setInterval(16);
        pollTimer_->start();
    }
    previewTimer_->start();
    update_status(lastUpdate_);
}

MainWindow::CatalogViewState MainWindow::take_catalog_view_state() {
    CatalogViewState saved;
    saved.grid = capture_grid_view_state();
    saved.update = lastUpdate_;
    saved.preview_runtime = previewRuntimeState_;
    saved.restrict_offline_fallback = restrictOfflineFallback_;
    saved.preview_revalidation_pending = previewRevalidationPending_;
    saved.offline_previews = std::exchange(offlineEligiblePreviews_, {});
    saved.offline_pages = std::exchange(offlineEligiblePages_, {});
    saved.revalidated_previews = std::exchange(strictRevalidatedPreviews_, {});
    saved.revalidated_pages = std::exchange(strictRevalidatedPages_, {});
    saved.catalog = listModel_.take_snapshot();
    return saved;
}

void MainWindow::restore_catalog_view_state(CatalogViewState state) {
    auto *saved = &state;
    begin_preview_generation(false);
    listModel_.restore_snapshot(std::move(saved->catalog));
    listModel_.set_filter(filterEdit_->text());
    listModel_.set_sort(static_cast<core::SortField>(sortField_->currentData().toInt()),
                        sortDirection_->isChecked() ? core::SortDirection::descending
                                                    : core::SortDirection::ascending);
    offlineEligiblePreviews_ = std::move(saved->offline_previews);
    offlineEligiblePages_ = std::move(saved->offline_pages);
    strictRevalidatedPreviews_ = std::move(saved->revalidated_previews);
    strictRevalidatedPages_ = std::move(saved->revalidated_pages);
    previewRuntimeState_ = saved->preview_runtime;
    restrictOfflineFallback_ = saved->restrict_offline_fallback;
    previewRevalidationPending_ = saved->preview_revalidation_pending;
    const auto protected_preview = previewRuntimeState_ != PreviewRuntimeState::online;
    setProperty("previewRuntimeState",
                previewRuntimeState_ == PreviewRuntimeState::offline   ? QStringLiteral("offline")
                : previewRuntimeState_ == PreviewRuntimeState::blocked ? QStringLiteral("blocked")
                                                                       : QStringLiteral("online"));
    folderMosaicController_.set_offline(protected_preview);
    folderMosaicController_.set_allow_offline_fallback(!protected_preview &&
                                                       !restrictOfflineFallback_);
    selectedName_.clear();
    selectedColorSummary_.clear();
    previewCanvas_->setPixmap({});
    previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Select a file"));
    restore_grid_view_state(saved->grid);
    update_directory_tree();
    lastUpdate_ = saved->update;
    lastUpdate_.entries_received = 0;
    lastUpdate_.model_changed = false;
    previewTimer_->start();
}

void MainWindow::set_recursive_view(const bool enabled) {
    if (enabled == recursiveViewActive_) return;
    if (!enabled) {
        leave_recursive_view(true);
        return;
    }
    if (trashViewActive_ || globalSearchActive_ || currentPath_.isEmpty() ||
        lastUpdate_.state != catalog::CatalogSessionState::ready ||
        !createFolderAction_->isEnabled()) {
        update_catalog_tools();
        return;
    }
    save_navigation_state();
    pendingNavigationRestore_.reset();
    directoryMonitor_.set_visible(false);
    directoryMonitor_.set_directory({});
    automaticRefreshInFlight_ = false;
    session_.suspend();
    pollTimer_->stop();
    catalogBeforeRecursiveView_ = take_catalog_view_state();
    recursiveViewActive_ = true;
    navigationCatalogReady_ = false;
    setProperty("recursiveViewActive", true);
    static_cast<CatalogView *>(listView_)->set_recursive_mode(true);
    operationStatus_.clear();
    leave_preview_protected_state();
    begin_preview_generation();
    session_.open(native_path(currentPath_), catalog::kRecursiveCatalogMaximumEntries, true);
    show_selection({});
    lastUpdate_ = {};
    pollTimer_->setInterval(16);
    pollTimer_->start();
    update_rename_action();
    update_status(lastUpdate_);
}

void MainWindow::leave_recursive_view(const bool restore_catalog) {
    if (!recursiveViewActive_) return;
    session_.suspend();
    pollTimer_->stop();
    recursiveViewActive_ = false;
    setProperty("recursiveViewActive", false);
    static_cast<CatalogView *>(listView_)->set_recursive_mode(false);
    auto saved = std::exchange(catalogBeforeRecursiveView_, std::nullopt);
    pendingNavigationRestore_.reset();
    if (restore_catalog && saved) {
        restore_catalog_view_state(std::move(*saved));
        navigationCatalogReady_ = true;
        directoryMonitor_.set_directory(currentPath_);
        update_directory_monitor_visibility();
        // Revalidate the physical folder after file operations performed in the combined view.
        session_.open(native_path(currentPath_), 100'000, false, true);
        directoryMonitor_.scan_started();
        lastUpdate_ = {};
        pollTimer_->setInterval(16);
        pollTimer_->start();
        update_rename_action();
        update_status(lastUpdate_);
    }
    update_catalog_tools();
}

void MainWindow::update_catalog_tools() {
    if (recursiveViewButton_ == nullptr || createFolderAction_ == nullptr) return;
    const auto busy = renameInFlight_ || deleteInFlight_ || transferInFlight_ ||
                      createDirectoryInFlight_ || pendingExternalDirectoryDrop_ ||
                      pendingExternalTransferDrop_ || trashConfirmationInProgress_;
    recursiveViewButton_->setChecked(recursiveViewActive_);
    recursiveViewButton_->setEnabled(!busy && !trashViewActive_ && !globalSearchActive_ &&
        (recursiveViewActive_ || (lastUpdate_.state == catalog::CatalogSessionState::ready &&
                                 createFolderAction_->isEnabled())));
    const auto eye_text = recursiveViewActive_
        ? QCoreApplication::translate("MainWindow", "Return to ordinary folder view")
        : QCoreApplication::translate("MainWindow", "Show files from all subfolders");
    recursiveViewButton_->setToolTip(eye_text);
    recursiveViewButton_->setAccessibleName(eye_text);
    const auto recovery = property("directoryTransferRecoveryPending").toBool() ||
        property("fileTransferRecoveryPending").toBool() || property("trashRecoveryPending").toBool() ||
        property("permanentDeleteRecoveryPending").toBool() || property("batchRenameRecoveryPending").toBool();
    const auto refresh_text = recovery
        ? QCoreApplication::translate("MainWindow", "Resume interrupted operation (F5)")
        : QCoreApplication::translate("MainWindow", "Refresh folder and previews (F5)");
    refreshButton_->setToolTip(refresh_text);
    refreshButton_->setAccessibleName(refresh_text);
    if (globalSearchActive_) {
        const auto ready = globalSearchReady_ && !busy && !recovery &&
            globalSearchQuery_ == globalSearchEdit_->text().trimmed() &&
            !property("fileOperationJournalBlocked").toBool() &&
            globalSearchRemovedPaths_.size() < static_cast<qsizetype>(search::kMaximumSearchResults);
        listView_->setDragEnabled(ready);
        listModel_.set_drag_and_edit_enabled(ready, false);
    } else if (recursiveViewActive_) {
        const auto ready = lastUpdate_.state == catalog::CatalogSessionState::ready;
        listView_->setDragEnabled(ready);
        listModel_.set_drag_and_edit_enabled(ready);
    } else if (!trashViewActive_ && !globalSearchActive_) {
        listView_->setDragEnabled(true);
        listModel_.set_drag_and_edit_enabled(true);
    }
}

void MainWindow::show_global_search_message(const QString &message) {
    globalSearchMessage_->setText(message);
    update_global_search_message_geometry();
    globalSearchMessage_->show();
    globalSearchMessage_->raise();
}

void MainWindow::update_global_search_message_geometry() {
    if (globalSearchMessage_ == nullptr || listView_ == nullptr) {
        return;
    }
    const auto bounds = listView_->viewport()->rect();
    const auto horizontal = std::max(16, bounds.width() / 8);
    const auto vertical = std::max(16, bounds.height() / 5);
    globalSearchMessage_->setGeometry(
        bounds.adjusted(horizontal, vertical, -horizontal, -vertical));
}

void MainWindow::apply_sort() {
    const auto field = static_cast<core::SortField>(sortField_->currentData().toInt());
    const auto direction = sortDirection_->isChecked() ? core::SortDirection::descending
                                                       : core::SortDirection::ascending;
    const auto direction_text =
        ui_text(UiTextId::sort_direction) + QStringLiteral(": ") +
        ui_text(sortDirection_->isChecked() ? UiTextId::descending : UiTextId::ascending);
    sortDirection_->setToolTip(direction_text);
    sortDirection_->setAccessibleName(direction_text);
    const auto view_state = capture_grid_view_state();
    {
        const QSignalBlocker selection_blocker(listView_->selectionModel());
        listModel_.set_sort(field, direction);
        restore_grid_view_state(view_state);
    }
    const auto *entry = listModel_.entry_at(listView_->currentIndex());
    previewModified_->set_modified_time(entry != nullptr && entry->kind == core::EntryKind::file
                                           ? std::optional{entry->modified_unix_ns}
                                           : std::nullopt);
    update_rename_action();
    update_status(lastUpdate_);
}

void MainWindow::apply_thumbnail_extent(const int extent) {
    const auto changed = delegate_.thumbnail_extent() != std::clamp(extent, 64, 320);
    delegate_.set_thumbnail_extent(extent);
    sizeSlider_->setToolTip(QCoreApplication::translate("MainWindow", "Thumbnail size: %1")
                                .arg(delegate_.thumbnail_extent()));
    listView_->setGridSize(delegate_.sizeHint({}, {}));
    listView_->doItemsLayout();
    if (changed) {
        begin_preview_generation();
        previewTimer_->start();
    }
}

void MainWindow::begin_preview_generation(const bool clear_previews) {
    reset_selected_preview(clear_previews);
    const auto previous_preview = previewGeneration_;
    const auto previous_viewport = viewportGeneration_;
    ++previewGeneration_;
    ++viewportGeneration_;
    if (previewGeneration_ == 0) {
        previewGeneration_ = 1;
    }
    if (viewportGeneration_ == 0) {
        viewportGeneration_ = 1;
    }
    previewClient_.cancel_generation(previous_viewport);
    if (previous_preview != previous_viewport) {
        previewClient_.cancel_generation(previous_preview);
    }
    folderMosaicController_.reset(previewGeneration_);
    pendingPreviewEdges_.clear();
    completedPreviewEdges_.clear();
    if (clear_previews) {
        offlineEligiblePreviews_.clear();
        offlineEligiblePages_.clear();
        listModel_.clear_previews();
    }
}

bool MainWindow::offline_cache_only() const noexcept {
    return previewRuntimeState_ == PreviewRuntimeState::offline;
}

bool MainWindow::preview_failure_can_disconnect(const core::DirectoryEntry &entry) const {
#ifdef VOVE_UI_TEST_HOOKS
    if (property("testRemotePreviewSource").toBool()) {
        return true;
    }
#endif
#ifdef Q_OS_WIN
    const auto path = QDir::toNativeSeparators(QString::fromUtf8(entry.path_utf8));
    if (!is_unc_path(path) && path.size() >= 3 && path.at(1) == QLatin1Char(':')) {
        const auto root = path.left(3).toStdWString();
        if (GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
            return false;
        }
    }
#else
    Q_UNUSED(entry);
#endif
    return true;
}

void MainWindow::enter_preview_offline_state(const bool refresh_catalog) {
    if (previewRuntimeState_ == PreviewRuntimeState::offline) {
        return;
    }
    begin_preview_generation(false);
    previewRuntimeState_ = PreviewRuntimeState::offline;
    restrictOfflineFallback_ = true;
    strictRevalidatedPreviews_.clear();
    strictRevalidatedPages_.clear();
    setProperty("previewRuntimeState", QStringLiteral("offline"));
    folderMosaicController_.set_offline(true);
    folderMosaicController_.set_allow_offline_fallback(false);
    previewRevalidationPending_ = true;
    listModel_.mark_previews_offline();
    if (listView_->currentIndex().isValid()) {
        update_selected_preview(listView_->currentIndex());
    }
    if (refresh_catalog && !globalSearchActive_ && !recursiveViewActive_) {
        refresh_directory_sources();
        pollTimer_->setInterval(100);
        pollTimer_->start();
    }
    previewTimer_->start();
}

void MainWindow::enter_preview_blocked_state() {
    if (previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    begin_preview_generation(false);
    previewRuntimeState_ = PreviewRuntimeState::blocked;
    setProperty("previewRuntimeState", QStringLiteral("blocked"));
    folderMosaicController_.set_offline(true);
    folderMosaicController_.set_allow_offline_fallback(false);
    previewRevalidationPending_ = false;
    restrictOfflineFallback_ = true;
    strictRevalidatedPreviews_.clear();
    strictRevalidatedPages_.clear();
    offlineEligiblePreviews_.clear();
    offlineEligiblePages_.clear();
    listModel_.clear_previews();
    selectedPreview_.reset();
    previewCanvas_->clear();
    passwordButton_->hide();
    update_page_navigation(0, 0);
    update_preview_support_label(false);
}

void MainWindow::leave_preview_protected_state() {
    previewRuntimeState_ = PreviewRuntimeState::online;
    setProperty("previewRuntimeState", QStringLiteral("online"));
    folderMosaicController_.set_offline(false);
    folderMosaicController_.set_allow_offline_fallback(!restrictOfflineFallback_);
}

bool MainWindow::offline_cache_allowed(const core::DirectoryEntry &entry,
                                       const std::uint32_t page_index) const noexcept {
    const auto found = offlineEligiblePreviews_.constFind(static_cast<qulonglong>(entry.id));
    const auto pages = offlineEligiblePages_.constFind(static_cast<qulonglong>(entry.id));
    return found != offlineEligiblePreviews_.cend() && same_source(*found, entry) &&
           pages != offlineEligiblePages_.cend() && pages->contains(page_index);
}

void MainWindow::begin_viewport_generation() {
    const auto previous = viewportGeneration_;
    ++viewportGeneration_;
    if (viewportGeneration_ == 0) {
        viewportGeneration_ = 1;
    }
    previewClient_.cancel_generation(previous);
    folderMosaicController_.clear_pending();
    pendingPreviewEdges_.clear();
    previewTimer_->start();
}

bool MainWindow::same_source(const PreviewEdgeState &state,
                             const core::DirectoryEntry &entry) noexcept {
    return state.source_size == entry.size_bytes &&
           state.modified_unix_ns == entry.modified_unix_ns &&
           state.source_revision == QString::fromUtf8(entry.source_revision_utf8);
}

bool MainWindow::same_source(const PreviewReply &reply,
                             const core::DirectoryEntry &entry) noexcept {
    return reply.source_size == entry.size_bytes &&
           reply.modified_unix_ns == entry.modified_unix_ns &&
           reply.source_revision == QString::fromUtf8(entry.source_revision_utf8);
}

std::uint16_t MainWindow::current_preview_edge(const QHash<qulonglong, PreviewEdgeState> &states,
                                               const core::DirectoryEntry &entry) const noexcept {
    const auto found = states.constFind(static_cast<qulonglong>(entry.id));
    return found != states.cend() && same_source(*found, entry) ? found->edge : 0;
}

bool MainWindow::request_preview(const QModelIndex &index, const std::uint16_t edge,
                                 const preview::ThumbnailPriority priority) {
    if (deleteInFlight_ || renamePreviewSuspended_ || previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return false;
    }
    const auto *entry = listModel_.entry_at(index);
    if (entry == nullptr || entry->kind != core::EntryKind::file) {
        return false;
    }
    const auto id = static_cast<qulonglong>(entry->id);
    const auto known_offline = offline_cache_only();
    if (known_offline && !offline_cache_allowed(*entry)) {
        return false;
    }
    if (current_preview_edge(pendingPreviewEdges_, *entry) >= edge ||
        current_preview_edge(completedPreviewEdges_, *entry) >= edge) {
        return false;
    }
    const auto request_id = previewClient_.request_preview(
        id, viewportGeneration_, QString::fromUtf8(entry->path_utf8), entry->size_bytes,
        entry->modified_unix_ns, QString::fromUtf8(entry->source_revision_utf8), edge, priority, {},
        0, documentPasswords_.value(QString::fromUtf8(entry->path_utf8)), known_offline,
        !restrictOfflineFallback_ || current_preview_edge(strictRevalidatedPreviews_, *entry) != 0);
    if (request_id != 0) {
        pendingPreviewEdges_.insert(
            id, {.source_size = entry->size_bytes,
                 .modified_unix_ns = entry->modified_unix_ns,
                 .source_revision = QString::fromUtf8(entry->source_revision_utf8),
                 .edge = std::max(current_preview_edge(pendingPreviewEdges_, *entry), edge)});
        return true;
    }
    return false;
}

void MainWindow::schedule_visible_previews() {
    if (deleteInFlight_ || renamePreviewSuspended_) {
        return;
    }
    ensure_selected_preview();
    if (listModel_.rowCount() == 0 || !listView_->isVisible()) {
        return;
    }
    const auto viewport = listView_->viewport()->rect().adjusted(-32, -32, 32, 32);
    // Locate the first visible row even when the top-left point falls in tile spacing.
    int first{};
    int end = listModel_.rowCount();
    while (first < end) {
        const int middle = first + (end - first) / 2;
        if (listView_->visualRect(listModel_.index(middle, 0)).bottom() < viewport.top()) {
            first = middle + 1;
        } else {
            end = middle;
        }
    }
    const auto last = std::min(listModel_.rowCount() - 1, first + 127);
    std::size_t requested{};
    QList<QModelIndex> visible_folders;
    QList<QModelIndex> visible_files;
    int first_visible{-1};
    int last_visible{-1};
    for (auto row = first; row <= last; ++row) {
        const auto index = listModel_.index(row, 0);
        if (!listView_->visualRect(index).intersects(viewport)) {
            continue;
        }
        first_visible = first_visible < 0 ? row : first_visible;
        last_visible = row;
        const auto *entry = listModel_.entry_at(index);
        if (entry == nullptr) {
            continue;
        }
        if (entry->kind == core::EntryKind::directory) {
            visible_folders.push_back(index);
            continue;
        }
        visible_files.push_back(index);
    }
    const auto edge = static_cast<std::uint16_t>(delegate_.thumbnail_extent());
    QSet<qulonglong> retained_entries;
    if (first_visible >= 0) {
        for (int row = std::max(0, first_visible - 24);
             row <= std::min(listModel_.rowCount() - 1, last_visible + 48); ++row) {
            if (const auto *entry = listModel_.entry_at(listModel_.index(row, 0))) {
                retained_entries.insert(static_cast<qulonglong>(entry->id));
            }
        }
    }
    previewClient_.retain_generation_entries(viewportGeneration_, retained_entries);
    for (const auto &index : visible_files) {
        if (requested >= 96) {
            break;
        }
        if (request_preview(index, edge, preview::ThumbnailPriority::visible)) {
            ++requested;
        }
    }
    if (offline_cache_only()) {
        folderMosaicController_.clear_pending();
    } else {
        folderMosaicController_.discard_queued();
        for (const auto &index : visible_folders) {
            const auto *entry = listModel_.entry_at(index);
            if (entry != nullptr) {
                folderMosaicController_.enqueue(
                    static_cast<qulonglong>(entry->id), QString::fromUtf8(entry->path_utf8),
                    entry->size_bytes, entry->modified_unix_ns,
                    QString::fromUtf8(entry->source_revision_utf8), edge);
            }
        }
    }
    // Speculative files must not age ahead of the visible folder mosaics in the shared queue.
    if (listModel_.loading() || !folderMosaicController_.idle()) {
        return;
    }
    const auto request_background_row =
        [this, edge, &requested](const int row, const preview::ThumbnailPriority priority,
                                 const std::size_t limit) {
            if (row < 0 || row >= listModel_.rowCount() || requested >= limit) {
                return;
            }
            if (request_preview(listModel_.index(row, 0), edge, priority)) {
                ++requested;
            }
        };
    if (first_visible >= 0 && last_visible >= first_visible) {
        const auto visible_requested = requested;
        for (int distance = 1; distance <= 24 && requested < visible_requested + 32; ++distance) {
            request_background_row(first_visible - distance, preview::ThumbnailPriority::nearby,
                                   visible_requested + 32);
            request_background_row(last_visible + distance, preview::ThumbnailPriority::nearby,
                                   visible_requested + 32);
        }
        const auto nearby_requested = requested;
        for (int distance = 25; distance <= 48 && requested < nearby_requested + 16; ++distance) {
            request_background_row(last_visible + distance, preview::ThumbnailPriority::rest,
                                   nearby_requested + 16);
        }
    }
}

void MainWindow::handle_preview_reply(PreviewReply reply) {
    if (reply.generation != viewportGeneration_) {
        return;
    }
    const auto pending = pendingPreviewEdges_.constFind(reply.entry_id);
    if (pending != pendingPreviewEdges_.cend() && pending->source_size == reply.source_size &&
        pending->modified_unix_ns == reply.modified_unix_ns &&
        pending->source_revision == reply.source_revision &&
        pending->edge <= reply.presentation_edge) {
        pendingPreviewEdges_.remove(reply.entry_id);
    }

    const auto *entry = listModel_.entry_for_id(reply.entry_id);
    if (entry == nullptr) {
        return;
    }
    if (!same_source(reply, *entry)) {
        const auto completed = completedPreviewEdges_.constFind(reply.entry_id);
        if (completed != completedPreviewEdges_.cend() &&
            completed->source_size == reply.source_size &&
            completed->modified_unix_ns == reply.modified_unix_ns &&
            completed->source_revision == reply.source_revision) {
            completedPreviewEdges_.remove(reply.entry_id);
        }
        previewTimer_->start();
        return;
    }
    using Status = preview::helper_protocol::ResponseStatus;
    if (reply.status == Status::queue_busy || reply.status == Status::cancelled) {
        previewTimer_->start();
        return;
    }
    listModel_.set_preview(
        reply.entry_id, reply.source_size, reply.modified_unix_ns, reply.source_revision,
        std::move(reply.image), static_cast<int>(reply.status), reply.presentation_edge,
        reply.page_count, std::move(reply.source_color_model),
        std::move(reply.source_color_profile), std::move(reply.source_profile_fingerprint),
        static_cast<int>(reply.provenance), reply.startup_diagnostic);

    if (reply.status == Status::success_cached || reply.status == Status::success_decoded) {
        strictRevalidatedPreviews_.insert(
            reply.entry_id,
            {.source_size = reply.source_size,
             .modified_unix_ns = reply.modified_unix_ns,
             .source_revision = reply.source_revision,
             .edge = std::max(current_preview_edge(strictRevalidatedPreviews_, *entry),
                              reply.presentation_edge)});
        strictRevalidatedPages_[reply.entry_id].insert(0);
        offlineEligiblePreviews_.insert(
            reply.entry_id,
            {.source_size = reply.source_size,
             .modified_unix_ns = reply.modified_unix_ns,
             .source_revision = reply.source_revision,
             .edge = std::max(current_preview_edge(offlineEligiblePreviews_, *entry),
                              reply.presentation_edge)});
        offlineEligiblePages_[reply.entry_id].insert(0);
    } else if (reply.status == Status::source_changed || reply.status == Status::not_found ||
               reply.status == Status::permission_denied ||
               reply.status == Status::authentication_failed) {
        offlineEligiblePreviews_.remove(reply.entry_id);
        offlineEligiblePages_.remove(reply.entry_id);
        strictRevalidatedPreviews_.remove(reply.entry_id);
        strictRevalidatedPages_.remove(reply.entry_id);
    }

    if (reply.status == Status::disconnected || reply.status == Status::timed_out ||
        reply.status == Status::source_unavailable ||
        reply.status == Status::success_offline_cached) {
        if (!globalSearchActive_ && !recursiveViewActive_ && preview_failure_can_disconnect(*entry)) {
            enter_preview_offline_state();
            return;
        }
    }
    if (reply.status == Status::permission_denied ||
        reply.status == Status::authentication_failed) {
        if (!globalSearchActive_ && !recursiveViewActive_) {
            enter_preview_blocked_state();
            return;
        }
    }
    completedPreviewEdges_.insert(
        reply.entry_id, {.source_size = reply.source_size,
                         .modified_unix_ns = reply.modified_unix_ns,
                         .source_revision = reply.source_revision,
                         .edge = std::max(current_preview_edge(completedPreviewEdges_, *entry),
                                          reply.presentation_edge)});
    if (selectedPageIndex_ == 0 &&
        listView_->currentIndex().data(DirectoryListModel::EntryIdRole).toULongLong() ==
            reply.entry_id) {
        update_selected_preview(listView_->currentIndex());
    }
    previewTimer_->start();
}

void MainWindow::handle_folder_mosaic(FolderMosaicReply reply) {
    if (reply.app_generation != previewGeneration_) {
        return;
    }
    previewTimer_->start();
    const auto status = reply.status;
    listModel_.set_preview(reply.folder_id, reply.source_size, reply.modified_unix_ns,
                           reply.source_revision, std::move(reply.image), static_cast<int>(status),
                           reply.presentation_edge, 0, {}, {});
    if (status == preview::helper_protocol::ResponseStatus::disconnected ||
        status == preview::helper_protocol::ResponseStatus::timed_out ||
        status == preview::helper_protocol::ResponseStatus::source_unavailable ||
        status == preview::helper_protocol::ResponseStatus::success_offline_cached) {
        const auto *entry = listModel_.entry_for_id(reply.folder_id);
        if (!globalSearchActive_ && !recursiveViewActive_ && entry != nullptr && preview_failure_can_disconnect(*entry)) {
            enter_preview_offline_state();
        }
    } else if (status == preview::helper_protocol::ResponseStatus::authentication_failed) {
        // A denied child folder does not revoke access to its parent or siblings.
        // Its own pixels were invalidated by set_preview above; only authentication
        // failure retains the catalog-wide protection policy.
        if (!globalSearchActive_ && !recursiveViewActive_) {
            enter_preview_blocked_state();
        }
    }
}

void MainWindow::go_back() {
    if (trashViewActive_) {
        open_path(currentPath_, false);
        return;
    }
    if (historyIndex_ <= 0) {
        return;
    }
    open_history(historyIndex_ - 1);
}

void MainWindow::go_forward() {
    if (historyIndex_ + 1 >= history_.size()) {
        return;
    }
    open_history(historyIndex_ + 1);
}

void MainWindow::go_up() {
    const auto parent = QFileInfo(currentPath_).dir().absolutePath();
    if (!parent.isEmpty() && parent != currentPath_) {
        open_path(parent, true, currentPath_);
    }
}

void MainWindow::rebuild_history_menu() {
    historyMenu_->clear();
    for (int index = static_cast<int>(history_.size()) - 1; index >= 0; --index) {
        auto *action = historyMenu_->addAction(QDir::toNativeSeparators(history_[index].path));
        action->setCheckable(true);
        action->setChecked(index == historyIndex_);
        action->setData(index);
        connect(action, &QAction::triggered, this, [this, index] {
            open_history(index);
        });
    }
}

void MainWindow::update_navigation() {
    backButton_->setEnabled(trashViewActive_ || historyIndex_ > 0);
    forwardButton_->setEnabled(!trashViewActive_ && historyIndex_ >= 0 &&
                               historyIndex_ + 1 < history_.size());
    historyButton_->setEnabled(!history_.isEmpty());
    upButton_->setEnabled(!trashViewActive_);
}

bool MainWindow::operation_status_is_active() const noexcept {
    return renameInFlight_ || deleteInFlight_ || transferInFlight_ || createDirectoryInFlight_;
}

void MainWindow::apply_operation_status_color() {
    if (status_ == nullptr) {
        return;
    }
    const auto colors = theme_colors(theme_);
    auto color = QColor(colors.muted);
    if (status_->property("technicalStatusActive").toBool()) {
        color = technical_status_accent();
    } else if (status_->property("operationPulseActive").toBool()) {
        constexpr auto pi = 3.14159265358979323846;
        const auto phase = static_cast<qreal>(operationStatusPulsePhase_) / 30.0;
        const auto wave = 0.5 - 0.5 * std::cos(phase * 2.0 * pi);
        color = blend_colors(color, operation_status_accent(theme_), 0.72 + 0.28 * wave);
    }
    auto palette = status_->palette();
    palette.setColor(QPalette::WindowText, color);
    status_->setPalette(palette);
    status_->setProperty("operationPulseColor", color.name(QColor::HexRgb));
}

void MainWindow::update_operation_status_pulse() {
    if (status_ == nullptr || operationStatusPulseTimer_ == nullptr) {
        return;
    }
    const auto active = dragStatus_.isEmpty() &&
                        !status_->property("technicalStatusActive").toBool() &&
                        !operationStatus_.isEmpty() && operation_status_is_active();
    status_->setProperty("operationPulseActive", active);
    status_->setProperty("operationPulseAccent",
                         operation_status_accent(theme_).name(QColor::HexRgb));
    if (active) {
        if (!operationStatusPulseTimer_->isActive()) {
            operationStatusPulsePhase_ = 0;
            operationStatusPulseTimer_->start();
        }
    } else {
        operationStatusPulseTimer_->stop();
        operationStatusPulsePhase_ = 0;
    }
    apply_operation_status_color();
}

void MainWindow::update_status(const catalog::CatalogSessionUpdate &update) {
    update_catalog_tools();
    if (!dragStatus_.isEmpty()) {
        status_->setTextFormat(Qt::RichText);
        status_->setToolTip({});
        status_->setText(dragStatus_);
        update_operation_status_pulse();
        return;
    }
    status_->setTextFormat(Qt::PlainText);
    status_->setProperty("catalogErrorKind", static_cast<int>(update.error.kind));
    status_->setProperty("catalogPlatformCode",
                         QVariant::fromValue<qlonglong>(update.error.platform_code));
    const auto recovery_pending = property("directoryTransferRecoveryPending").toBool() ||
                                  property("trashRecoveryPending").toBool() ||
                                  property("fileTransferRecoveryPending").toBool() ||
                                  property("permanentDeleteRecoveryPending").toBool() ||
                                  property("batchRenameRecoveryPending").toBool() ||
                                  property("fileOperationJournalBlocked").toBool();
    const auto catalog_technical = !trashViewActive_ && !globalSearchActive_ &&
        ((update.state != catalog::CatalogSessionState::loading &&
          update.state != catalog::CatalogSessionState::ready) ||
         (recursiveViewActive_ && update.truncated));
    status_->setProperty("technicalStatusActive", recovery_pending || catalog_technical);
    status_->setProperty("technicalStatusAccent", technical_status_accent().name(QColor::HexRgb));
    const auto visible = static_cast<qulonglong>(listModel_.visible_size());
    const auto total = static_cast<qulonglong>(listModel_.total_size());
    const auto directories = static_cast<qulonglong>(listModel_.visible_directories());
    const auto files = visible - directories;
    const auto counts = QCoreApplication::translate("MainWindow", "Folders: %1 · files: %2")
                            .arg(directories)
                            .arg(files);
    const auto selection_summary = selection_status_text();
    const auto error = QString::fromUtf8(update.error.message_utf8);
    status_->setToolTip(error);

    QString text;
    if (trashViewActive_) {
        status_->setProperty("catalogErrorKind", 0);
        status_->setProperty("catalogPlatformCode", QVariant::fromValue<qlonglong>(0));
        status_->setToolTip({});
        if (!trashCatalog_) {
            text = QCoreApplication::translate("MainWindow", "VO-VE Trash: loading…");
        } else if (!trashCatalog_->ok()) {
            text = QCoreApplication::translate("MainWindow", "VO-VE Trash is unavailable");
        } else {
            text = QCoreApplication::translate("MainWindow", "VO-VE Trash: %1 objects · %2")
                       .arg(static_cast<qulonglong>(trashCatalog_->total_items))
                       .arg(QLocale().formattedDataSize(
                           static_cast<qint64>(trashCatalog_->total_bytes)));
            if (visible != total) {
                text += QCoreApplication::translate("MainWindow", " · showing %1 of %2")
                            .arg(visible)
                            .arg(total);
            }
        }
        if (!selection_summary.isEmpty()) {
            text = selection_summary;
        } else if (!selectedName_.isEmpty()) {
            text += QStringLiteral(" · %1").arg(selectedName_);
        }
        if (!operationStatus_.isEmpty()) {
            text += QStringLiteral(" · %1").arg(operationStatus_);
        }
        status_->setText(text);
        update_operation_status_pulse();
        return;
    }
    if (globalSearchActive_) {
        status_->setProperty("catalogErrorKind", 0);
        status_->setProperty("catalogPlatformCode", QVariant::fromValue<qlonglong>(0));
        QStringList native_roots;
        native_roots.reserve(searchRoots_.size());
        for (const auto &root : std::as_const(searchRoots_)) {
            native_roots.push_back(QDir::toNativeSeparators(root));
        }
        status_->setToolTip(
            native_roots.isEmpty()
                ? QCoreApplication::translate("MainWindow", "Global search scope is not configured")
                : QCoreApplication::translate("MainWindow", "Global search roots:\n%1")
                      .arg(native_roots.join(QLatin1Char('\n'))));
        text = globalSearchStatus_.isEmpty()
                   ? QCoreApplication::translate("MainWindow", "Global search…")
                   : globalSearchStatus_;
        if (!selection_summary.isEmpty()) {
            text = selection_summary;
        } else if (!selectedName_.isEmpty()) {
            text += QStringLiteral(" · %1").arg(selectedName_);
        }
        if (selection_summary.isEmpty() && !selectedColorSummary_.isEmpty()) {
            text += QStringLiteral(" · %1").arg(selectedColorSummary_);
        }
        if (!operationStatus_.isEmpty()) text += QStringLiteral(" · %1").arg(operationStatus_);
        status_->setText(text);
        update_operation_status_pulse();
        return;
    }
    switch (update.state) {
    case catalog::CatalogSessionState::loading:
        text = total == 0
                   ? QCoreApplication::translate("MainWindow", "Loading folder…")
                   : QCoreApplication::translate("MainWindow", "Refreshing… · %1").arg(counts);
        break;
    case catalog::CatalogSessionState::waiting_retry: {
        const auto milliseconds = update.retry_in.value_or(std::chrono::milliseconds(0)).count();
        const auto seconds = std::max<std::int64_t>(1, (milliseconds + 999) / 1000);
        text = QCoreApplication::translate("MainWindow", "Connection lost. Retrying in %1 s · %2")
                   .arg(seconds)
                   .arg(counts);
        break;
    }
    case catalog::CatalogSessionState::network_disconnected:
        text = QCoreApplication::translate("MainWindow",
                                           "Network folder unavailable. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::timed_out:
        text = QCoreApplication::translate(
                   "MainWindow", "Network folder is not responding. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::authentication_required:
        text = QCoreApplication::translate(
                   "MainWindow", "Network folder sign-in required. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::permission_denied:
        text = QCoreApplication::translate("MainWindow",
                                           "Folder access denied. Press F5 to retry · %1")
                   .arg(counts);
        break;
    case catalog::CatalogSessionState::unavailable:
        text =
            QCoreApplication::translate("MainWindow", "Folder unavailable. Press F5 to retry · %1")
                .arg(counts);
        break;
    case catalog::CatalogSessionState::ready:
        if (total == 0) {
            text = QCoreApplication::translate("MainWindow", "Folder is empty");
        } else if (visible == 0) {
            text = QCoreApplication::translate("MainWindow", "No items match the filter");
        } else {
            text = counts;
            if (visible != total) {
                text += QCoreApplication::translate("MainWindow", " · showing %1 of %2")
                            .arg(visible)
                            .arg(total);
            }
            if (update.truncated) {
                text += QCoreApplication::translate("MainWindow", " · results limited");
            }
        }
        break;
    }
    if (!selection_summary.isEmpty()) {
        text = selection_summary;
    } else if (!selectedName_.isEmpty()) {
        text += QStringLiteral(" · %1").arg(selectedName_);
    }
    if (selection_summary.isEmpty() && !selectedColorSummary_.isEmpty()) {
        text += QStringLiteral(" · %1").arg(selectedColorSummary_);
    }
    if (!operationStatus_.isEmpty()) {
        text += QStringLiteral(" · %1").arg(operationStatus_);
    }
    if (recursiveViewActive_) {
        const auto mode = update.state == catalog::CatalogSessionState::loading
            ? QCoreApplication::translate("MainWindow", "Reading subfolders…")
            : QCoreApplication::translate("MainWindow", "All subfolders");
        text = mode + QStringLiteral(" · ") + text;
        if (update.truncated || (update.state != catalog::CatalogSessionState::ready &&
                                 update.state != catalog::CatalogSessionState::loading)) {
            text += QCoreApplication::translate("MainWindow", " · View incomplete");
        }
    }
    status_->setText(text);
    update_operation_status_pulse();
}

void MainWindow::show_selection(const QModelIndex &index) {
    reset_selected_preview();
    const auto *entry = listModel_.entry_at(index);
    previewModified_->set_modified_time(entry != nullptr && entry->kind == core::EntryKind::file
                                           ? std::optional{entry->modified_unix_ns}
                                           : std::nullopt);
    if (entry == nullptr) {
        selectedEntryId_ = 0;
        update_page_navigation(0, 0);
        selectedName_.clear();
        selectedColorSummary_.clear();
        previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Select a file"));
        previewName_->clear();
        previewName_->setToolTip({});
        previewFormat_->clear();
        previewFormat_->hide();
        update_preview_support_label(false);
        passwordButton_->hide();
        update_status(lastUpdate_);
        return;
    }

    const auto file_name = QString::fromUtf8(entry->name_utf8);
    const QFileInfo file_info(file_name);
    const auto base_name = file_info.completeBaseName();
    // An unusually long suffix belongs in the wrapped literal name, not an unbounded format badge.
    const auto suffix = entry->kind != core::EntryKind::directory && !base_name.isEmpty()
                                && file_info.suffix().size() <= 16
                            ? file_info.suffix().toUpper()
                            : QString{};
    selectedName_ = file_name;
    previewName_->set_full_text(suffix.isEmpty() ? file_name : base_name);
    previewName_->setToolTip(QString::fromUtf8(entry->path_utf8));
    if (entry->kind == core::EntryKind::directory) {
        selectedEntryId_ = 0;
        update_page_navigation(0, 0);
        previewCanvas_->setText(QCoreApplication::translate(
            "MainWindow", "Folder\n\nA mosaic will appear after thumbnails are connected"));
        previewFormat_->clear();
        previewFormat_->hide();
        update_preview_support_label(false);
        selectedColorSummary_.clear();
        passwordButton_->hide();
    } else {
        selectedEntryId_ = static_cast<qulonglong>(entry->id);
        selectedPageIndex_ = 0;
        selectedPageCount_ = 0;
        previewFormat_->setText(suffix);
        previewFormat_->setVisible(!suffix.isEmpty());
        previewFormat_->setStyleSheet(
            QStringLiteral("font-weight: 700; color: %1;")
                .arg(format_color(suffix, previewFormat_->palette().window().color()).name()));
        update_preview_support_label(false);
        update_selected_preview(index);
        ensure_selected_preview();
    }
    update_status(lastUpdate_);
}

void MainWindow::update_selected_preview(const QModelIndex &index) {
    previewCanvas_->setToolTip({});
    const auto *entry = listModel_.entry_at(index);
    if (entry == nullptr || entry->id != selectedEntryId_) {
        return;
    }
    const auto current = selectedPreview_ && same_source(*selectedPreview_, *entry) &&
                         selectedPreview_->page_index == selectedPageIndex_;
    const auto image = current ? selectedPreview_->image : QImage{};
    previewCanvas_->setProperty("previewSourceSize", image.size());
    previewCanvas_->setProperty("previewRequestedEdge",
                                current ? selectedPreview_->presentation_edge : 0);
    if (!image.isNull()) {
        const auto key = QString::fromUtf8(entry->path_utf8) + QChar::Null +
                         QString::fromUtf8(entry->source_revision_utf8) + QChar::Null +
                         QString::number(entry->size_bytes) + QLatin1Char(':') +
                         QString::number(entry->modified_unix_ns) + QLatin1Char(':') +
                         QString::number(selectedPageIndex_);
        previewCanvas_->set_preview(image, key);
        passwordButton_->hide();
        update_preview_support_label(true);
    } else {
        const auto status = current ? QVariant(static_cast<int>(selectedPreview_->status))
                                    : index.data(DirectoryListModel::PreviewStatusRole);
        using Status = preview::helper_protocol::ResponseStatus;
        if (selectedPreviewExtended_) {
            previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Force preview…"));
            passwordButton_->hide();
        } else if (!status.isValid() ||
                   status.toInt() == static_cast<int>(Status::success_cached) ||
                   status.toInt() == static_cast<int>(Status::success_decoded) ||
                   status.toInt() == static_cast<int>(Status::success_offline_cached)) {
            previewCanvas_->setText(
                QCoreApplication::translate("MainWindow", "Preparing preview…"));
            passwordButton_->hide();
        } else {
            const auto response_status =
                static_cast<preview::helper_protocol::ResponseStatus>(status.toInt());
            previewCanvas_->setText(selected_preview_status_text(response_status));
            if (response_status == Status::worker_start_failed) {
                previewCanvas_->setToolTip(
                    current ? worker_startup_tooltip(selectedPreview_->startup_diagnostic)
                            : index.data(DirectoryListModel::StartupDiagnosticRole).toString());
            }
            update_document_password_action(response_status);
        }
        update_preview_support_label(false);
    }
    const auto page_count =
        current && selectedPreview_->page_count != 0 ? selectedPreview_->page_count
        : selectedPageCount_ != 0                    ? selectedPageCount_
                                  : index.data(DirectoryListModel::PageCountRole).toUInt();
    update_page_navigation(selectedPageIndex_, page_count);
    const auto model = current ? selectedPreview_->source_color_model
                               : index.data(DirectoryListModel::SourceColorModelRole).toString();
    const auto profile = current
                             ? selectedPreview_->source_color_profile
                             : index.data(DirectoryListModel::SourceColorProfileRole).toString();
    selectedColorSummary_ = color_summary(model, profile);
    update_status(lastUpdate_);
}

void MainWindow::update_preview_support_label(const bool preview_ready) {
    const auto row = listModel_.row_for_id(selectedEntryId_);
    const auto embedded =
        selectedPreview_
            ? selectedPreview_->provenance ==
                  preview::helper_protocol::PreviewProvenance::embedded_preview
            : row >= 0 && listModel_.index(row, 0)
                                  .data(DirectoryListModel::PreviewProvenanceRole)
                                  .toInt() ==
                              static_cast<int>(
                                  preview::helper_protocol::PreviewProvenance::embedded_preview);
    const auto offline =
        offline_cache_only() ||
        (selectedPreview_
             ? selectedPreview_->status ==
                   preview::helper_protocol::ResponseStatus::success_offline_cached
             : row >= 0 &&
                   listModel_.index(row, 0).data(DirectoryListModel::PreviewStatusRole).toInt() ==
                       static_cast<int>(
                           preview::helper_protocol::ResponseStatus::success_offline_cached));
    const auto checking =
        !selectedPreview_ && row >= 0 &&
        listModel_.index(row, 0).data(DirectoryListModel::PreviewStatusRole).toInt() ==
            DirectoryListModel::VerificationPendingStatus;
    const auto profile =
        selectedPreview_
            ? selectedPreview_->source_color_profile
            : listModel_.index(row, 0).data(DirectoryListModel::SourceColorProfileRole).toString();
    const auto approximate = profile == QLatin1StringView(color::kUnprofiledCmykApproximation);
    previewSupport_->setVisible(preview_ready && (checking || offline || embedded || approximate));
    previewSupport_->setText(
        preview_ready && checking   ? QCoreApplication::translate("MainWindow", "Checking…")
        : preview_ready && offline  ? QCoreApplication::translate("MainWindow", "Cache · offline")
        : preview_ready && embedded ? QCoreApplication::translate("MainWindow", "Embedded preview")
                                    : QString{});
    if (preview_ready && approximate) {
        const auto warning =
            QCoreApplication::translate("MainWindow", "CMYK without profile · approximate colors");
        previewSupport_->setText(previewSupport_->text().isEmpty()
                                     ? warning
                                     : previewSupport_->text() + QStringLiteral(" · ") + warning);
    }
}

void MainWindow::reset_selected_preview(const bool clear_image) {
    selectedPreviewExtended_ = false;
    if (cancelPreviewAttemptAction_ != nullptr) {
        cancelPreviewAttemptAction_->setEnabled(false);
    }
    previewClient_.cancel_generation(selectedPreviewGeneration_);
    constexpr auto generation_bit = 1ULL << 62U;
    selectedPreviewGeneration_ =
        generation_bit | ((selectedPreviewGeneration_ + 1U) & (generation_bit - 1U));
    selectedPreviewRequest_ = 0;
    selectedPreviewPendingEdge_ = 0;
    if (clear_image) {
        selectedPreview_.reset();
        if (previewCanvas_ != nullptr) {
            previewCanvas_->clear();
            previewCanvas_->setToolTip({});
            previewCanvas_->setProperty("previewSourceSize", QSize{});
            previewCanvas_->setProperty("previewRequestedEdge", 0);
        }
    }
}

std::uint16_t MainWindow::selected_preview_edge() const {
    const auto size = previewCanvas_->contentsRect().size();
    const auto physical_edge =
        std::ceil(std::max(size.width(), size.height()) * previewCanvas_->devicePixelRatioF());
    return static_cast<std::uint16_t>(
        std::clamp(physical_edge, 512.0,
                   static_cast<double>(preview::helper_protocol::kMaximumCanonicalEdge)));
}

bool MainWindow::can_force_preview(const core::DirectoryEntry &entry) const {
    if (entry.kind != core::EntryKind::file || selectedPreviewExtended_ ||
        previewRuntimeState_ != PreviewRuntimeState::online) {
        return false;
    }
    const auto retryable = [](const int status) {
        using Status = preview::helper_protocol::ResponseStatus;
        return status == static_cast<int>(Status::memory_limit) ||
               status == static_cast<int>(Status::worker_start_failed) ||
               status == static_cast<int>(Status::timed_out) ||
               status == static_cast<int>(Status::processing_timed_out);
    };
    if (selectedPreview_ && selectedPreview_->entry_id == entry.id &&
        same_source(*selectedPreview_, entry) &&
        selectedPreview_->page_index == selectedPageIndex_) {
        if (retryable(static_cast<int>(selectedPreview_->status)) ||
            selectedPreview_->status == preview::helper_protocol::ResponseStatus::cancelled) {
            return true;
        }
        if (selectedPageIndex_ != 0 || selectedPreview_->image.isNull()) {
            return false;
        }
    }
    const auto row = listModel_.row_for_id(static_cast<qulonglong>(entry.id));
    const auto status = listModel_.index(row, 0).data(DirectoryListModel::PreviewStatusRole);
    return status.isValid() && retryable(status.toInt());
}

void MainWindow::force_preview(const qulonglong entry_id) {
    const auto *entry = listModel_.entry_for_id(entry_id);
    if (entry == nullptr || !can_force_preview(*entry)) {
        return;
    }
    if (selectedEntryId_ != entry_id) {
        const auto index = listModel_.index(listModel_.row_for_id(entry_id), 0);
        listView_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect);
    }
    reset_selected_preview();
    passwordButton_->hide();
    previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Force preview…"));
    ensure_selected_preview(true);
}

void MainWindow::cancel_forced_preview() {
    if (!selectedPreviewExtended_) {
        return;
    }
    const auto *entry = listModel_.entry_for_id(selectedEntryId_);
    reset_selected_preview();
    if (entry == nullptr) {
        return;
    }
    // Keep a terminal result for this selection so an idle/resize tick cannot restart it.
    selectedPreview_ =
        PreviewReply{.entry_id = selectedEntryId_,
                     .generation = selectedPreviewGeneration_,
                     .source_size = entry->size_bytes,
                     .modified_unix_ns = entry->modified_unix_ns,
                     .source_revision = QString::fromUtf8(entry->source_revision_utf8),
                     .presentation_edge = preview::helper_protocol::kMaximumCanonicalEdge,
                     .status = preview::helper_protocol::ResponseStatus::cancelled,
                     .image = {},
                     .source_color_model = {},
                     .source_color_profile = {},
                     .source_profile_fingerprint = {},
                     .page_index = selectedPageIndex_,
                     .page_count = selectedPageCount_};
    update_selected_preview(listView_->currentIndex());
}

void MainWindow::ensure_selected_preview(const bool extended_limits) {
    if (deleteInFlight_ || renamePreviewSuspended_ || selectedEntryId_ == 0 || previewCanvas_ == nullptr ||
        previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    const auto *entry = listModel_.entry_for_id(selectedEntryId_);
    if (entry == nullptr || entry->kind != core::EntryKind::file) {
        return;
    }
    if (selectedPreview_ && !same_source(*selectedPreview_, *entry)) {
        reset_selected_preview();
        update_page_navigation(0, 0);
        update_selected_preview(listView_->currentIndex());
    }
    const auto edge =
        extended_limits ? preview::helper_protocol::kMaximumCanonicalEdge : selected_preview_edge();
    if (selectedPreviewExtended_ && selectedPreviewRequest_ != 0) {
        return;
    }
    if (selectedPreviewRequest_ != 0 && selectedPreviewPendingEdge_ >= edge) {
        return;
    }
    if (selectedPreview_ && selectedPreview_->generation == selectedPreviewGeneration_ &&
        selectedPreview_->page_index == selectedPageIndex_ &&
        selectedPreview_->presentation_edge >= edge) {
        return;
    }
    const auto known_offline = offline_cache_only();
    if (known_offline && !offline_cache_allowed(*entry, selectedPageIndex_)) {
        previewCanvas_->setPixmap({});
        previewCanvas_->setText(
            QCoreApplication::translate("MainWindow", "No verified offline copy of this page"));
        return;
    }

    if (selectedPreviewRequest_ != 0) {
        reset_selected_preview(false);
    }
    selectedPreviewPendingEdge_ = edge;
    selectedPreviewRequest_ = previewClient_.request_preview(
        selectedEntryId_, selectedPreviewGeneration_, QString::fromUtf8(entry->path_utf8),
        entry->size_bytes, entry->modified_unix_ns, QString::fromUtf8(entry->source_revision_utf8),
        edge, preview::ThumbnailPriority::visible_selected,
        [this](PreviewReply reply) { handle_selected_page_reply(std::move(reply)); },
        selectedPageIndex_, documentPasswords_.value(QString::fromUtf8(entry->path_utf8)),
        known_offline,
        !restrictOfflineFallback_ ||
            (current_preview_edge(strictRevalidatedPreviews_, *entry) != 0 &&
             strictRevalidatedPages_.value(selectedEntryId_).contains(selectedPageIndex_)),
        true, extended_limits);
    selectedPreviewExtended_ = extended_limits && selectedPreviewRequest_ != 0;
    cancelPreviewAttemptAction_->setEnabled(selectedPreviewExtended_);
    if (selectedPreviewRequest_ == 0) {
        previewCanvas_->setText(
            QCoreApplication::translate("MainWindow", "Failed to queue the page"));
    }
}

void MainWindow::request_selected_page(const std::uint32_t page_index) {
    if (selectedEntryId_ == 0 || page_index >= selectedPageCount_ ||
        previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    if (page_index != selectedPageIndex_) {
        reset_selected_preview();
    }
    selectedPageIndex_ = page_index;
    update_page_navigation(page_index, selectedPageCount_);
    update_selected_preview(listView_->currentIndex());
    ensure_selected_preview();
    if (selectedPreviewRequest_ != 0 && (!selectedPreview_ || selectedPreview_->image.isNull())) {
        previewCanvas_->setPixmap({});
        previewCanvas_->setText(
            QCoreApplication::translate("MainWindow", "Preparing page %1…").arg(page_index + 1U));
    }
}

void MainWindow::handle_selected_page_reply(PreviewReply reply) {
    if (reply.generation != selectedPreviewGeneration_ || reply.entry_id != selectedEntryId_ ||
        reply.page_index != selectedPageIndex_ || reply.request_id != selectedPreviewRequest_) {
        return;
    }
    const bool extended_attempt = selectedPreviewExtended_;
    selectedPreviewExtended_ = false;
    cancelPreviewAttemptAction_->setEnabled(false);
    selectedPreviewRequest_ = 0;
    selectedPreviewPendingEdge_ = 0;
    const auto *entry = listModel_.entry_for_id(reply.entry_id);
    if (entry == nullptr) {
        return;
    }
    if (!same_source(reply, *entry)) {
        reset_selected_preview();
        selectedPageIndex_ = 0;
        selectedPageCount_ = 0;
        update_selected_preview(listView_->currentIndex());
        previewTimer_->start();
        return;
    }

    using Status = preview::helper_protocol::ResponseStatus;
    const auto offline_reply = reply.status == Status::success_offline_cached;
    if (reply.status == Status::disconnected || reply.status == Status::timed_out ||
        reply.status == Status::source_unavailable) {
        if (!globalSearchActive_ && !recursiveViewActive_ && preview_failure_can_disconnect(*entry)) {
            enter_preview_offline_state();
            return;
        }
    }
    if (reply.status == Status::permission_denied ||
        reply.status == Status::authentication_failed) {
        enter_preview_blocked_state();
        return;
    }
    if ((reply.status == Status::success_cached || reply.status == Status::success_offline_cached ||
         reply.status == Status::success_decoded) &&
        !reply.image.isNull()) {
        selectedColorSummary_ = color_summary(reply.source_color_model, reply.source_color_profile);
        passwordButton_->hide();
        if (reply.status != Status::success_offline_cached) {
            strictRevalidatedPreviews_.insert(
                reply.entry_id,
                {.source_size = reply.source_size,
                 .modified_unix_ns = reply.modified_unix_ns,
                 .source_revision = reply.source_revision,
                 .edge = std::max(current_preview_edge(strictRevalidatedPreviews_, *entry),
                                  reply.presentation_edge)});
            strictRevalidatedPages_[reply.entry_id].insert(reply.page_index);
            offlineEligiblePreviews_.insert(
                reply.entry_id,
                {.source_size = reply.source_size,
                 .modified_unix_ns = reply.modified_unix_ns,
                 .source_revision = reply.source_revision,
                 .edge = std::max(current_preview_edge(offlineEligiblePreviews_, *entry),
                                  reply.presentation_edge)});
            offlineEligiblePages_[reply.entry_id].insert(reply.page_index);
        }
    } else {
        if (reply.status == Status::source_changed || reply.status == Status::not_found ||
            reply.status == Status::permission_denied ||
            reply.status == Status::authentication_failed) {
            offlineEligiblePreviews_.remove(reply.entry_id);
            offlineEligiblePages_.remove(reply.entry_id);
            strictRevalidatedPreviews_.remove(reply.entry_id);
            strictRevalidatedPages_.remove(reply.entry_id);
        }
        previewCanvas_->setPixmap({});
        previewCanvas_->setText(selected_preview_status_text(reply.status));
        update_document_password_action(reply.status);
    }
    if (!extended_attempt &&
        (reply.status == Status::cancelled || reply.status == Status::queue_busy)) {
        previewTimer_->start();
        return;
    }
    selectedPreview_ = std::move(reply);
    if (extended_attempt && selectedPreview_->page_index == 0 &&
        !selectedPreview_->image.isNull()) {
        auto tile = *selectedPreview_;
        tile.generation = viewportGeneration_;
        tile.presentation_edge = static_cast<std::uint16_t>(delegate_.thumbnail_extent());
        tile.image = tile.image.scaled(tile.presentation_edge, tile.presentation_edge,
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
        handle_preview_reply(std::move(tile));
    }
    if (offline_reply && !globalSearchActive_ && !recursiveViewActive_ && preview_failure_can_disconnect(*entry)) {
        enter_preview_offline_state();
    }
    update_selected_preview(listView_->currentIndex());
}

void MainWindow::prompt_for_document_password() {
    const auto *entry = listModel_.entry_for_id(selectedEntryId_);
    if (entry == nullptr || entry->kind != core::EntryKind::file ||
        previewRuntimeState_ == PreviewRuntimeState::blocked) {
        return;
    }
    bool accepted{};
    const auto password =
        QInputDialog::getText(this, QCoreApplication::translate("MainWindow", "Document password"),
                              QCoreApplication::translate("MainWindow", "Enter password:"),
                              QLineEdit::Password, {}, &accepted, Qt::Dialog, Qt::ImhHiddenText);
    if (!accepted || password.isEmpty()) {
        return;
    }

    const auto path = QString::fromUtf8(entry->path_utf8);
    documentPasswords_.insert(path, password);
    begin_viewport_generation();
    reset_selected_preview();
    completedPreviewEdges_.remove(selectedEntryId_);
    selectedPageIndex_ = 0;
    selectedPageCount_ = 0;
    update_page_navigation(0, 0);
    previewCanvas_->setPixmap({});
    previewCanvas_->setText(QCoreApplication::translate("MainWindow", "Checking password…"));
    passwordButton_->hide();
    const auto row = listModel_.row_for_id(selectedEntryId_);
    ensure_selected_preview();
    if (row < 0 || selectedPreviewRequest_ == 0) {
        previewCanvas_->setText(
            QCoreApplication::translate("MainWindow", "Failed to verify the password"));
    }
}

void MainWindow::update_document_password_action(
    const preview::helper_protocol::ResponseStatus status) {
    const auto wrong_password =
        status == preview::helper_protocol::ResponseStatus::document_password_incorrect;
    const auto visible =
        previewRuntimeState_ != PreviewRuntimeState::blocked &&
        (status == preview::helper_protocol::ResponseStatus::password_required || wrong_password);
    passwordButton_->setVisible(visible);
    passwordButton_->setText(wrong_password
                                 ? QCoreApplication::translate("MainWindow", "Change password")
                                 : QCoreApplication::translate("MainWindow", "Enter password"));
}

QString MainWindow::selected_preview_status_text(
    const preview::helper_protocol::ResponseStatus status) const {
    return preview_status_text(status);
}

void MainWindow::update_page_navigation(const std::uint32_t page_index,
                                        const std::uint32_t page_count) {
    selectedPageIndex_ = page_index;
    selectedPageCount_ = page_count;
    const auto multiple_pages = page_count > 1;
    pageNavigation_->setVisible(multiple_pages);
    pageIndicator_->setText(multiple_pages
                                ? QStringLiteral("%1 / %2").arg(page_index + 1U).arg(page_count)
                                : QString{});
    previousPageButton_->setEnabled(multiple_pages && page_index > 0);
    nextPageButton_->setEnabled(multiple_pages && page_index + 1U < page_count);
}

void MainWindow::add_favorite(const QString &path) {
    if (path.isEmpty()) {
        return;
    }
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    constexpr auto path_case = Qt::CaseInsensitive;
#else
    constexpr auto path_case = Qt::CaseSensitive;
#endif
    for (int index = 0; index < favorites_->count(); ++index) {
        if (favorites_->item(index)->data(Qt::UserRole).toString().compare(normalized, path_case) ==
            0) {
            return;
        }
    }
    auto *item = new QListWidgetItem(QFileInfo(normalized).fileName());
    if (item->text().isEmpty()) {
        item->setText(QDir::toNativeSeparators(normalized));
    }
    item->setData(Qt::UserRole, normalized);
    item->setData(favoriteKindRole, regularFavoriteKind);
    item->setToolTip(QDir::toNativeSeparators(normalized));
    auto insertion_row = favorites_->count();
    for (int row{}; row < favorites_->count(); ++row) {
        if (is_trash_favorite(favorites_->item(row))) {
            insertion_row = row;
            break;
        }
    }
    favorites_->insertItem(insertion_row, item);
    update_location_selection();
    schedule_settings_save();
}

void MainWindow::add_trash_favorite() {
    bool service_item_exists{};
    for (int row{}; row < favorites_->count(); ++row) {
        if (is_trash_favorite(favorites_->item(row))) {
            service_item_exists = true;
            break;
        }
    }
    if (!service_item_exists) {
        auto *item = new QListWidgetItem(QCoreApplication::translate("MainWindow", "Trash"));
        item->setData(favoriteKindRole, trashFavoriteKind);
        item->setToolTip(QCoreApplication::translate("MainWindow", "VO-VE Trash"));
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        favorites_->addItem(item);
        favorites_->setRowHidden(favorites_->row(item), true);
    }
    if (trashFavorite_ != nullptr && trashFavorite_->count() == 0) {
        auto *pinned = new QListWidgetItem(QCoreApplication::translate("MainWindow", "Trash"));
        pinned->setData(favoriteKindRole, trashFavoriteKind);
        pinned->setToolTip(QCoreApplication::translate("MainWindow", "VO-VE Trash"));
        pinned->setFlags(pinned->flags() & ~Qt::ItemIsEditable);
        trashFavorite_->addItem(pinned);
        trashFavorite_->setFixedHeight(std::max(24, trashFavorite_->sizeHintForRow(0)));
    }
}

void MainWindow::add_current_favorite() {
    add_favorite(currentPath_);
}

void MainWindow::set_favorites_pin_drop_active(const bool active) {
    if (favoritesHeader_ == nullptr ||
        favoritesHeader_->property("pinDropActive").toBool() == active) {
        return;
    }
    favoritesHeader_->setProperty("pinDropActive", active);
    favoritesHeader_->style()->unpolish(favoritesHeader_);
    favoritesHeader_->style()->polish(favoritesHeader_);
    favoritesHeader_->update();
}

void MainWindow::rename_selected_favorite() {
    auto *item = favorites_->currentItem();
    if (item == nullptr || is_trash_favorite(item)) {
        return;
    }
    bool accepted{};
    const auto name =
        QInputDialog::getText(this, QCoreApplication::translate("MainWindow", "Rename favorite"),
                              QCoreApplication::translate("MainWindow", "Name"), QLineEdit::Normal,
                              item->text(), &accepted)
            .trimmed();
    if (!accepted || name.isEmpty() || name == item->text()) {
        return;
    }
    item->setText(name);
    schedule_settings_save();
}

void MainWindow::move_selected_favorite(const int offset) {
    const auto row = favorites_->currentRow();
    const auto destination = row + offset;
    if (row < 0 || destination < 0 || destination >= favorites_->count() ||
        is_trash_favorite(favorites_->item(row)) ||
        is_trash_favorite(favorites_->item(destination))) {
        return;
    }
    auto *item = favorites_->takeItem(row);
    favorites_->insertItem(destination, item);
    favorites_->setCurrentItem(item);
    update_location_selection();
    schedule_settings_save();
}

void MainWindow::remove_selected_favorite() {
    if (is_trash_favorite(favorites_->currentItem())) {
        return;
    }
    delete favorites_->takeItem(favorites_->currentRow());
    update_location_selection();
    schedule_settings_save();
}

void MainWindow::refresh_directory_sources() {
    if (globalSearchActive_) {
        // Refreshing the suspended local session would replace search results with its old folder.
        treeModel_->refresh(treeModel_->ensure_path(currentPath_));
        return;
    }
    automaticRefreshInFlight_ = false;
    if (recursiveViewActive_ && !pendingNavigationRestore_) {
        auto grid = capture_grid_view_state();
        if (grid.selected_ids.size() > 512) grid.selected_ids.resize(512);
        pendingNavigationRestore_ = PendingNavigationRestore{currentPath_, std::move(grid), {}};
    }
    if (!recursiveViewActive_) directoryMonitor_.scan_started();
    session_.refresh();
    if (recursiveViewActive_) {
        lastUpdate_.state = catalog::CatalogSessionState::loading;
        lastUpdate_.truncated = false;
        lastUpdate_.error = {};
        update_rename_action();
        update_catalog_tools();
    }
    if (!recursiveViewActive_) treeModel_->refresh(treeModel_->ensure_path(currentPath_));
}

void MainWindow::update_directory_tree() {
    if (currentPath_.isEmpty()) {
        return;
    }
    if (same_directory_path(directoryTree_->property("locationPath").toString(), currentPath_)) {
        update_location_selection();
        return;
    }
    directoryTree_->setProperty("locationPath", currentPath_);
    const auto current = treeModel_->ensure_path(currentPath_);
    treeModel_->refresh(current);
    for (auto parent = current.parent(); parent.isValid(); parent = parent.parent()) {
        directoryTree_->expand(parent);
    }
    directoryTree_->expand(current);
    update_location_selection();
    directoryTree_->scrollTo(current);
}

void MainWindow::update_location_selection() {
    if (treeModel_ == nullptr || favorites_ == nullptr || currentPath_.isEmpty()) {
        return;
    }
    QListWidgetItem *active_favorite = nullptr;
    if (!trashViewActive_) {
        for (int row = 0; row < favorites_->count(); ++row) {
            auto *item = favorites_->item(row);
            if (same_directory_path(item->data(Qt::UserRole).toString(), currentPath_)) {
                active_favorite = item;
                break;
            }
        }
    }
    favorites_->setCurrentItem(active_favorite);
    favorites_->clearSelection();
    trashFavorite_->clearSelection();
    if (trashViewActive_) {
        auto *trash_item = trashFavorite_->item(0);
        trashFavorite_->setCurrentItem(trash_item);
        if (trash_item != nullptr) {
            trash_item->setSelected(true);
        }
    } else if (active_favorite != nullptr) {
        active_favorite->setSelected(true);
    }
    if (trashViewActive_) {
        directoryTree_->selectionModel()->clearSelection();
        directoryTree_->selectionModel()->setCurrentIndex({}, QItemSelectionModel::Clear);
        return;
    }
    const auto current = treeModel_->ensure_path(currentPath_);
    directoryTree_->selectionModel()->setCurrentIndex(
        current, active_favorite == nullptr ? QItemSelectionModel::ClearAndSelect
                                            : QItemSelectionModel::Clear);
}

void MainWindow::choose_directory() {
    auto options = QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks;
#ifdef VOVE_UI_TEST_HOOKS
    options |= QFileDialog::DontUseNativeDialog;
#endif
    const auto path =
        QFileDialog::getExistingDirectory(this, chooseFolderAction_->text(), currentPath_, options);
    if (!path.isEmpty()) {
        open_path(path, true);
    }
}

void MainWindow::save_navigation_state() {
    if (trashViewActive_ || globalSearchActive_ || recursiveViewActive_ || historyIndex_ < 0 ||
        historyIndex_ >= history_.size() || !navigationCatalogReady_ ||
        !same_directory_path(history_[historyIndex_].path, currentPath_)) {
        return;
    }
    auto state = capture_grid_view_state();
    if (state.selected_ids.size() > 512) {
        state.selected_ids.clear();
        if (state.current_id != 0) {
            state.selected_ids.push_back(state.current_id);
        }
    }
    history_[historyIndex_].grid = std::move(state);
    history_[historyIndex_].saved = true;
}

void MainWindow::open_history(const int index) {
    if (index < 0 || index >= history_.size()) {
        return;
    }
    save_navigation_state();
    historyIndex_ = index;
    open_path(history_[index].path, false);
}

void MainWindow::restore_navigation_state() {
    if (!pendingNavigationRestore_) {
        return;
    }
    auto pending = std::move(*pendingNavigationRestore_);
    pendingNavigationRestore_.reset();
    if (!same_directory_path(pending.path, currentPath_) || trashViewActive_ || globalSearchActive_) {
        return;
    }
    QModelIndex child;
    if (!pending.reveal_child.isEmpty()) {
        for (int row = 0; row < listModel_.rowCount(); ++row) {
            const auto index = listModel_.index(row, 0);
            const auto *entry = listModel_.entry_at(index);
            if (entry != nullptr && entry->kind == core::EntryKind::directory &&
                same_directory_path(QString::fromUtf8(entry->path_utf8), pending.reveal_child)) {
                child = index;
                pending.grid.current_id = entry->id;
                if (std::ranges::find(pending.grid.selected_ids, entry->id) == pending.grid.selected_ids.end()) {
                    if (pending.grid.selected_ids.size() >= 512) pending.grid.selected_ids.pop_back();
                    pending.grid.selected_ids.push_back(entry->id);
                }
                break;
            }
        }
    }
    {
        const QSignalBlocker blocker(listView_->selectionModel());
        restore_grid_view_state(pending.grid);
        if (child.isValid() &&
            !listView_->viewport()->rect().intersects(listView_->visualRect(child))) {
            listView_->scrollTo(child, QAbstractItemView::EnsureVisible);
        }
    }
    show_selection(listView_->currentIndex());
    update_rename_action();
}

MainWindow::GridViewState MainWindow::capture_grid_view_state() const {
    GridViewState state;
    state.scroll_value = listView_->verticalScrollBar()->value();
    state.current_id =
        listView_->currentIndex().data(DirectoryListModel::EntryIdRole).toULongLong();
    const auto selected = listView_->selectionModel()->selectedIndexes();
    state.selected_ids.reserve(static_cast<std::size_t>(selected.size()));
    for (const auto &index : selected) {
        state.selected_ids.push_back(index.data(DirectoryListModel::EntryIdRole).toULongLong());
    }

    // The viewport corner is often in a tile's gutter, so indexAt(1, 1) is not an anchor.
    int first = 0;
    int last = listModel_.rowCount();
    while (first < last) {
        const auto middle = first + (last - first) / 2;
        if (listView_->visualRect(listModel_.index(middle, 0)).bottom() < 0) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    const auto top = listModel_.index(first, 0);
    if (top.isValid()) {
        state.top_id = top.data(DirectoryListModel::EntryIdRole).toULongLong();
        state.top_offset = listView_->visualRect(top).top();
        state.has_top = true;
    }
    return state;
}

void MainWindow::restore_grid_view_state(const GridViewState &state) {
    auto *selection = listView_->selectionModel();
    QSet<qulonglong> selected_ids;
    selected_ids.reserve(static_cast<qsizetype>(state.selected_ids.size()));
    for (const auto id : state.selected_ids) {
        selected_ids.insert(static_cast<qulonglong>(id));
    }
    const auto selected_ranges = listModel_.selection_for_ids(selected_ids);
    selection->select(selected_ranges, QItemSelectionModel::ClearAndSelect);
    const auto current_row = listModel_.row_for_id(state.current_id);
    if (current_row >= 0) {
        selection->setCurrentIndex(listModel_.index(current_row, 0), QItemSelectionModel::NoUpdate);
    } else if (!selected_ranges.isEmpty()) {
        selection->setCurrentIndex(selected_ranges.front().topLeft(),
                                   QItemSelectionModel::NoUpdate);
    } else {
        selection->setCurrentIndex({}, QItemSelectionModel::NoUpdate);
    }

    if (!state.has_top) {
        listView_->verticalScrollBar()->setValue(state.scroll_value);
        return;
    }
    const QSet<qulonglong> top_id{static_cast<qulonglong>(state.top_id)};
    const auto top_row =
        listModel_.rows_for_ids(top_id).value(static_cast<qulonglong>(state.top_id), -1);
    if (top_row < 0) {
        listView_->verticalScrollBar()->setValue(state.scroll_value);
        return;
    }
    listView_->doItemsLayout();
    const auto top = listModel_.index(top_row, 0);
    listView_->scrollTo(top, QAbstractItemView::PositionAtTop);
    listView_->verticalScrollBar()->setValue(listView_->verticalScrollBar()->value() -
                                             state.top_offset + listView_->visualRect(top).top());
}

void MainWindow::apply_splitter_ratios() {
    mainShare_ = std::clamp(mainShare_, 0.40, 0.80);
    favoritesShare_ = std::clamp(favoritesShare_, 0.20, 0.55);
    mainSplitter_->setSizes({static_cast<int>(std::lround(mainShare_ * 1000.0)),
                             static_cast<int>(std::lround((1.0 - mainShare_) * 1000.0))});
    placesSplitter_->setSizes({static_cast<int>(std::lround(favoritesShare_ * 1000.0)),
                               static_cast<int>(std::lround((1.0 - favoritesShare_) * 1000.0))});
}

void MainWindow::apply_artwork_icons() {
    const auto horizontal_rotation = layoutDirection() == Qt::RightToLeft ? 180 : 0;
    for (auto *button : {backButton_, forwardButton_, historyButton_, upButton_}) {
        button->set_theme(theme_);
    }
    previousPageButton_->set_theme(theme_);
    nextPageButton_->set_theme(theme_);
    recursiveViewButton_->set_theme(theme_);
    refreshButton_->set_theme(theme_);
    logoButton_->setIcon(authored_logo(theme_));
    apply_thumbnail_scale_theme({sizeSlider_->parentWidget(), smallerThumbnailsButton_, sizeSlider_,
                                 largerThumbnailsButton_},
                                theme_);
    filterEdit_->set_theme(theme_);
    globalSearchEdit_->set_theme(theme_);
    pathEdit_->set_theme(theme_);
    apply_sort_controls_theme({sortControls_, sortField_, sortDirection_}, theme_);
    createFolderAction_->setIcon(create_folder_icon(theme_));
    chooseFolderAction_->setIcon(directory_picker_icon(
        qApp->palette().color(QPalette::PlaceholderText), devicePixelRatioF()));
    backButton_->setProperty("artworkRotation", horizontal_rotation);
    forwardButton_->setProperty("artworkRotation", horizontal_rotation);
    previousPageButton_->setProperty("artworkRotation", horizontal_rotation);
    nextPageButton_->setProperty("artworkRotation", horizontal_rotation);
    historyButton_->setProperty("artworkRotation", 180);
}

void MainWindow::apply_theme(const AppTheme theme) {
    theme_ = theme;
    apply_scrollbar_theme(theme);
    const auto colors = theme_colors(theme_);
    delegate_.set_placeholder_theme(theme_);
    delegate_.set_folder_colors(colors.folderFrame, colors.folderText);
    delegate_.set_file_label_colors(colors.fileText, colors.pageBackground, colors.pageText,
                                    colors.pageBorder);
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
    favorites_->set_drop_target_theme(QColor(colors.hover), QColor(QStringLiteral("#F5A623")));
    trashFavorite_->set_drop_target_theme(QColor(colors.hover), QColor(QStringLiteral("#F5A623")));
    previewFormat_->setStyleSheet(
        QStringLiteral("font-weight: 700; color: %1;")
            .arg(format_color(previewFormat_->text(), QColor(colors.panel)).name()));
    apply_artwork_icons();
    windowControls_->set_theme(theme_);
    if (treeModel_ != nullptr && directoryTree_ != nullptr) {
        update_directory_tree();
    }
    if (auto *button = themeButtons_->button(static_cast<int>(theme_)); button != nullptr) {
        button->setChecked(true);
    }
    update_operation_status_pulse();
    listView_->viewport()->update();
}

void MainWindow::load_settings() {
    QSettings settings;
    externalApplications_.load(settings);
    const auto requested_language =
        settings.contains(QStringLiteral("appearance/language"))
            ? app_language_from_code(
                  settings.value(QStringLiteral("appearance/language")).toString())
            : app_language_from_locale(QLocale());
    apply_language(translations_.available(requested_language) ? requested_language
                                                               : AppLanguage::english,
                   false);
    const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
        if (!windowState().testAnyFlags(Qt::WindowMaximized | Qt::WindowFullScreen)) {
            QList<QRect> available_screens;
            const auto screens = QApplication::screens();
            available_screens.reserve(screens.size());
            for (const auto *screen : screens) {
                if (screen != nullptr) {
                    available_screens.push_back(screen->availableGeometry());
                }
            }
            setGeometry(visible_window_geometry(this->geometry(), available_screens));
        }
    }
    mainShare_ = settings.value(QStringLiteral("window/mainShare"), 0.60).toDouble();
    favoritesShare_ = settings.value(QStringLiteral("window/favoritesShare"), 0.30).toDouble();
    apply_splitter_ratios();

    sizeSlider_->setValue(settings.value(QStringLiteral("catalog/thumbnailExtent"), 160).toInt());
    filterEdit_->setText(settings.value(QStringLiteral("catalog/filter")).toString());
    const auto sort_field =
        settings.value(QStringLiteral("catalog/sortField"), static_cast<int>(core::SortField::name))
            .toInt();
    const auto sort_direction = settings
                                    .value(QStringLiteral("catalog/sortDirection"),
                                           static_cast<int>(core::SortDirection::ascending))
                                    .toInt();
    sortField_->setCurrentIndex(std::max(0, sortField_->findData(sort_field)));
    sortDirection_->setChecked(sort_direction == static_cast<int>(core::SortDirection::descending));
    apply_sort();

    const auto theme_id = std::clamp(
        settings.value(QStringLiteral("appearance/theme"), static_cast<int>(AppTheme::blue))
            .toInt(),
        static_cast<int>(AppTheme::white), static_cast<int>(AppTheme::dark_gray));
    apply_theme(static_cast<AppTheme>(theme_id));
    currentPath_ = settings.value(QStringLiteral("catalog/lastPath")).toString();
    const auto sealed_state =
        settings.value(QString::fromLatin1(directoryTransferStateKey)).toByteArray();
    const auto decoded_state = open_directory_transfer_state(sealed_state);
    directoryTransferStateCorrupt_ = !sealed_state.isEmpty() && !decoded_state;
    if (decoded_state) {
        const auto &state = *decoded_state;
        bool operation_ok{};
        const auto operation_id =
            state.value(QStringLiteral("operationId")).toString().toULongLong(&operation_ok);
        const auto source = state.value(QStringLiteral("source")).toString();
        const auto destination = state.value(QStringLiteral("destination")).toString();
        const auto revision = state.value(QStringLiteral("sourceRevision")).toString();
        const auto manifest = state.value(QStringLiteral("manifest")).toString();
        auto state_valid = operation_ok && operation_id != 0U &&
                           native_path(source).is_absolute() &&
                           native_path(destination).is_absolute() && !revision.isEmpty() &&
                           (manifest.isEmpty() || native_path(manifest).is_absolute()) &&
                           state.value(QStringLiteral("completionKnown")).isBool() &&
                           state.value(QStringLiteral("discarding")).isBool();
        std::optional<PendingDirectoryTransferBatch> restored_batch;
        if (state_valid && state.contains(QStringLiteral("batch"))) {
            const auto batch = state.value(QStringLiteral("batch")).toObject();
            const auto sources_json = batch.value(QStringLiteral("sources")).toArray();
            const auto revisions_json = batch.value(QStringLiteral("revisions")).toArray();
            const auto batch_kind = batch.value(QStringLiteral("kind")).toInt(-1);
            const auto batch_index = batch.value(QStringLiteral("index")).toInteger(-1);
            const auto batch_completed = batch.value(QStringLiteral("completed")).toInteger(-1);
            const auto batch_destination_text =
                batch.value(QStringLiteral("destination")).toString();
            const auto batch_destination = native_path(batch_destination_text);
            const auto kind_valid =
                batch_kind == static_cast<int>(fileops::FileTransferKind::copy) ||
                batch_kind == static_cast<int>(fileops::FileTransferKind::move);
            state_valid = kind_valid && !sources_json.isEmpty() &&
                          static_cast<std::size_t>(sources_json.size()) <=
                              fileops::kMaximumDirectoryTransferRoots &&
                          sources_json.size() == revisions_json.size() &&
                          batch_destination.is_absolute() && batch_index >= 0 &&
                          batch_index < sources_json.size() && batch_completed >= 0 &&
                          batch_completed <= batch_index;
            QStringList batch_sources;
            std::vector<std::string> batch_revisions;
            batch_sources.reserve(sources_json.size());
            batch_revisions.reserve(static_cast<std::size_t>(revisions_json.size()));
            for (qsizetype index{}; state_valid && index < sources_json.size(); ++index) {
                const auto source_value = sources_json.at(index);
                const auto revision_value = revisions_json.at(index);
                const auto source_text = source_value.toString();
                const auto revision_text = revision_value.toString();
                const auto source_path = native_path(source_text);
                state_valid = source_value.isString() && revision_value.isString() &&
                              source_path.is_absolute() && !source_path.filename().empty() &&
                              !revision_text.isEmpty();
                batch_sources.push_back(source_text);
                batch_revisions.push_back(revision_text.toUtf8().toStdString());
            }
            if (state_valid) {
                const auto current_source = native_path(batch_sources.at(batch_index));
                state_valid =
                    current_source.lexically_normal() == native_path(source).lexically_normal() &&
                    (batch_destination / current_source.filename()).lexically_normal() ==
                        native_path(destination).lexically_normal() &&
                    QString::fromUtf8(batch_revisions.at(static_cast<std::size_t>(batch_index))) ==
                        revision;
            }
            if (state_valid) {
                restored_batch = PendingDirectoryTransferBatch{
                    .kind = static_cast<fileops::FileTransferKind>(batch_kind),
                    .source_paths = batch_sources,
                    .source_revisions_utf8 = std::move(batch_revisions),
                    .destination_directory = batch_destination,
                    .index = static_cast<qsizetype>(batch_index),
                    .completed = static_cast<qsizetype>(batch_completed),
                };
            }
        }
        if (state_valid) {
            directoryTransferRecoveryRequestId_ = operation_id;
            directoryTransferRecoverySource_ = source;
            directoryTransferRecoveryDestination_ = destination;
            directoryTransferRecoverySourceRevision_ = revision;
            directoryTransferRecoveryManifest_ = manifest;
            directoryTransferCompletionKnown_ =
                state.value(QStringLiteral("completionKnown")).toBool();
            directoryTransferDiscarding_ = state.value(QStringLiteral("discarding")).toBool();
            pendingDirectoryTransferBatch_ = std::move(restored_batch);
        } else {
            directoryTransferStateCorrupt_ = true;
        }
    }

    const auto favorites = settings.value(QStringLiteral("catalog/favorites")).toStringList();
    const auto favorite_labels =
        settings.value(QStringLiteral("catalog/favoriteLabels")).toStringList();
    for (qsizetype index{}; index < favorites.size(); ++index) {
        const auto previous_count = favorites_->count();
        add_favorite(favorites.at(index));
        if (favorites_->count() > previous_count && index < favorite_labels.size() &&
            !favorite_labels.at(index).trimmed().isEmpty()) {
            favorites_->item(favorites_->count() - 1)->setText(favorite_labels.at(index));
        }
    }
    if (favorites_->count() == 0) {
        add_favorite(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
    }
    add_trash_favorite();
    const auto configured_roots =
        settings.value(QStringLiteral("search/allowedRoots")).toStringList();
    std::vector<std::string> encoded_roots;
    encoded_roots.reserve(static_cast<std::size_t>(configured_roots.size()));
    for (const auto &root : configured_roots) {
        encoded_roots.push_back(root.toUtf8().toStdString());
    }
    searchRoots_.clear();
    for (const auto &root : search::normalize_allowed_roots(encoded_roots)) {
        searchRoots_.push_back(QString::fromUtf8(root));
    }
    // The retired CMYK fallback must not remain active invisibly after upgrading.
    settings.remove(QStringLiteral("color/fallbackCmykProfile"));
    cacheMaximumBytes_ = settings
                             .value(QStringLiteral("cache/maximumBytes"),
                                    static_cast<qulonglong>(cacheMaximumBytes_))
                             .toULongLong();
    if (cacheMaximumBytes_ == 0) {
        cacheMaximumBytes_ = 10ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    previewClient_.set_cache_maximum_bytes(cacheMaximumBytes_);
    trashMaximumBytes_ = settings
                             .value(QStringLiteral("trash/maximumBytes"),
                                    static_cast<qulonglong>(trashMaximumBytes_))
                             .toULongLong();
    if (trashMaximumBytes_ == 0) {
        trashMaximumBytes_ = 50ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    trashCoordinator_.set_maximum_bytes(trashMaximumBytes_);
}

void MainWindow::save_settings() {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/mainShare"),
                      splitter_share(mainSplitter_, mainShare_));
    settings.setValue(QStringLiteral("window/favoritesShare"),
                      splitter_share(placesSplitter_, favoritesShare_));
    settings.setValue(QStringLiteral("catalog/thumbnailExtent"), sizeSlider_->value());
    settings.setValue(QStringLiteral("catalog/filter"), filterEdit_->text());
    settings.setValue(QStringLiteral("catalog/sortField"), sortField_->currentData());
    settings.setValue(QStringLiteral("catalog/sortDirection"),
                      static_cast<int>(sortDirection_->isChecked()
                                           ? core::SortDirection::descending
                                           : core::SortDirection::ascending));
    settings.setValue(QStringLiteral("catalog/lastPath"), currentPath_);
    settings.setValue(QStringLiteral("appearance/theme"), static_cast<int>(theme_));
    settings.setValue(QStringLiteral("appearance/language"), language_code(language_));
    QStringList favorites;
    QStringList favorite_labels;
    for (int index = 0; index < favorites_->count(); ++index) {
        if (is_trash_favorite(favorites_->item(index))) {
            continue;
        }
        favorites.push_back(favorites_->item(index)->data(Qt::UserRole).toString());
        favorite_labels.push_back(favorites_->item(index)->text());
    }
    settings.setValue(QStringLiteral("catalog/favorites"), favorites);
    settings.setValue(QStringLiteral("catalog/favoriteLabels"), favorite_labels);
    settings.setValue(QStringLiteral("search/allowedRoots"), searchRoots_);
    settings.setValue(QStringLiteral("cache/maximumBytes"),
                      static_cast<qulonglong>(cacheMaximumBytes_));
    settings.setValue(QStringLiteral("trash/maximumBytes"),
                      static_cast<qulonglong>(trashMaximumBytes_));
    externalApplications_.save(settings);
    settings.sync();
    static_cast<void>(persist_directory_transfer_recovery_state());
}

void MainWindow::schedule_settings_save() {
    if (settingsTimer_ != nullptr) {
        settingsTimer_->start();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (directoryTransferService_.busy() || transferInFlight_) {
        operationStatus_ = QCoreApplication::translate(
            "MainWindow", "Wait for the current folder transfer to finish");
        update_status(lastUpdate_);
        event->ignore();
        return;
    }
    save_settings();
    QMainWindow::closeEvent(event);
}

} // namespace vove::ui
