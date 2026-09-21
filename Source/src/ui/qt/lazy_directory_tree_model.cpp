#include "lazy_directory_tree_model.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QScopedValueRollback>
#include <QStringList>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <atomic>
#include <filesystem>
#include <utility>

namespace vove::ui {
namespace {

constexpr std::size_t kMaximumPendingRequests = 32;
constexpr int kMaximumBatchesPerPoll = 4;
std::atomic<catalog::RequestGeneration> nextGeneration{};

bool drive_path(const QString &path) {
    return path.size() >= 3 && path.at(0).toUpper() >= QLatin1Char('A') &&
           path.at(0).toUpper() <= QLatin1Char('Z') && path.at(1) == QLatin1Char(':') &&
           (path.at(2) == QLatin1Char('/') || path.at(2) == QLatin1Char('\\'));
}

QStringList ancestor_paths(QString path) {
    if (path.isEmpty() || path.contains(QChar::Null)) {
        return {};
    }
    path = QDir::fromNativeSeparators(path);
    if (drive_path(path) || path.startsWith(QStringLiteral("\\\\")) ||
        (path.startsWith(QStringLiteral("//")) && !path.startsWith(QStringLiteral("///")))) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    }
    if (path.startsWith(QStringLiteral("//?/UNC/"), Qt::CaseInsensitive)) {
        path = QStringLiteral("//") + path.mid(8);
    } else if (path.startsWith(QStringLiteral("//?/")) && drive_path(path.mid(4))) {
        path = path.mid(4);
    }

    QString root;
    QStringList parts;
    if (drive_path(path)) {
        root = path.left(1).toUpper() + QStringLiteral(":/");
        parts = path.mid(3).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    } else if (path.startsWith(QStringLiteral("//")) &&
               !path.startsWith(QStringLiteral("///"))) {
        parts = path.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() < 2 || parts.at(0) == QStringLiteral(".") ||
            parts.at(0) == QStringLiteral("..") || parts.at(0) == QStringLiteral("?") ||
            parts.at(1) == QStringLiteral(".") || parts.at(1) == QStringLiteral("..")) {
            return {};
        }
        root = QStringLiteral("//") + parts.at(0) + QLatin1Char('/') + parts.at(1);
        parts = parts.mid(2);
    } else if (path.startsWith(QLatin1Char('/'))) {
        root = QStringLiteral("/");
        parts = path.mid(1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    } else {
        return {};
    }

    // Resolve dot components only within the root; UNC parents never escape the share.
    QStringList components;
    for (const auto &part : parts) {
        if (part == QStringLiteral("..")) {
            if (!components.isEmpty()) {
                components.removeLast();
            }
        } else if (part != QStringLiteral(".")) {
            components.push_back(part);
        }
    }
    QStringList result{root};
    auto current = root;
    for (const auto &part : components) {
        if (!current.endsWith(QLatin1Char('/'))) {
            current += QLatin1Char('/');
        }
        current += part;
        result.push_back(current);
    }
    return result;
}

QString path_key(const QString &path) {
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return path;
#endif
}

std::filesystem::path native_path(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}

bool in_branch(QModelIndex candidate, const QModelIndex &root) {
    while (candidate.isValid()) {
        if (candidate == root) {
            return true;
        }
        candidate = candidate.parent();
    }
    return false;
}

} // namespace

struct LazyDirectoryTreeModel::Node final : QStandardItem {
    enum class State { unknown, queued, loading, loaded, failed };

    Node(const QString &path_value, const bool root, const bool retained_value)
        : QStandardItem(root ? QDir::toNativeSeparators(path_value)
                             : path_value.mid(path_value.lastIndexOf(QLatin1Char('/')) + 1)),
          path(path_value), retained(retained_value) {
        setData(path, PathRole);
        setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    }

    QVariant data(const int role) const override {
        if (role != Qt::ToolTipRole && role != ErrorRole) {
            return QStandardItem::data(role);
        }
        QString error;
        if (state == State::failed) {
            error = incomplete
                        ? QCoreApplication::translate(
                              "MainWindow", "Folder listing is incomplete. Collapse and expand to retry.")
                        : QCoreApplication::translate(
                              "MainWindow", "Folder is unavailable. Collapse and expand to retry.");
        }
        if (role == ErrorRole) {
            return error.isEmpty() ? QVariant{} : QVariant(error);
        }
        const auto native = QDir::toNativeSeparators(path);
        return error.isEmpty() ? native : native + QLatin1Char('\n') + error;
    }

    QString path;
    State state{State::unknown};
    catalog::RequestGeneration seenGeneration{};
    bool retained{};
    bool incomplete{};
};

LazyDirectoryTreeModel::LazyDirectoryTreeModel(catalog::DirectorySource &source, QObject *parent)
    : DirectoryTreeModel(parent), source_(source), timer_(this) {
    setColumnCount(1);
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
    collator_.setNumericMode(true);
    timer_.setInterval(16);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
}

LazyDirectoryTreeModel::~LazyDirectoryTreeModel() {
    timer_.stop();
    if (activeGeneration_ != 0) {
        source_.cancel(activeGeneration_);
    }
}

QStringList LazyDirectoryTreeModel::detected_roots() {
#ifdef Q_OS_WIN
    // The bitmask includes mapped drives, without resolving or contacting their shares.
    const auto drives = GetLogicalDrives();
    QStringList roots;
    for (int letter = 0; letter < 26; ++letter) {
        if ((drives & (DWORD{1} << letter)) != 0) {
            roots.push_back(QString(QChar(u'A' + letter)) + QStringLiteral(":/"));
        }
    }
    return roots;
#else
    return {QStringLiteral("/")};
#endif
}

void LazyDirectoryTreeModel::add_roots(const QStringList &roots) {
    for (const auto &path : roots) {
        const auto ancestors = ancestor_paths(path);
        if (ancestors.size() == 1) {
            add_node(ancestors.front(), nullptr, false);
        }
    }
}

LazyDirectoryTreeModel::Node *LazyDirectoryTreeModel::node(const QModelIndex &index) const {
    if (!index.isValid() || index.model() != this || index.column() != 0) {
        return nullptr;
    }
    return dynamic_cast<Node *>(itemFromIndex(index));
}

LazyDirectoryTreeModel::Node *LazyDirectoryTreeModel::add_node(const QString &path, Node *parent,
                                                              const bool retained) {
    const auto key = path_key(path);
    const auto found = paths_.constFind(key);
    if (found != paths_.cend()) {
        if (auto *existing = node(found.value())) {
            existing->retained = existing->retained || retained;
            return existing;
        }
    }
    auto *item = new Node(path, parent == nullptr, retained);
    auto *container = parent == nullptr ? invisibleRootItem() : parent;
    int first = 0;
    int last = container->rowCount();
    while (first < last) {
        const int middle = first + (last - first) / 2;
        const auto existing = container->child(middle)->text();
        const int comparison = collator_.compare(existing, item->text());
        if (comparison < 0 || (comparison == 0 && existing < item->text())) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    container->insertRow(first, item);
    paths_.insert(key, QPersistentModelIndex(item->index()));
    return item;
}

void LazyDirectoryTreeModel::update_spelling(Node *item, const QString &path) {
    if (item->path == path) {
        return;
    }
    // Case-only renames keep lookup keys, items, and persistent indexes unchanged.
    Q_ASSERT(path_key(item->path) == path_key(path));
    item->path = path;
    item->setData(path, PathRole);
    item->setText(item->parent() == nullptr
                      ? QDir::toNativeSeparators(path)
                      : path.mid(path.lastIndexOf(QLatin1Char('/')) + 1));
    auto prefix = path;
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix += QLatin1Char('/');
    }
    for (int row = 0; row < item->rowCount(); ++row) {
        if (auto *child = dynamic_cast<Node *>(item->child(row))) {
            const auto name = child->path.mid(child->path.lastIndexOf(QLatin1Char('/')) + 1);
            update_spelling(child, prefix + name);
        }
    }
    const auto index = item->index();
    emit dataChanged(index, index, {Qt::DisplayRole, PathRole, Qt::ToolTipRole});
}

QModelIndex LazyDirectoryTreeModel::ensure_path(const QString &path) {
    const auto ancestors = ancestor_paths(path);
    if (ancestors.isEmpty()) {
        return {};
    }
    if (!retainedChain_.isEmpty()) {
        if (auto *current = node(retainedChain_.back());
            current != nullptr && path_key(current->path) == path_key(ancestors.back())) {
            return current->index();
        }
    }
    for (const auto &index : retainedChain_) {
        if (auto *previous = node(index)) {
            previous->retained = false;
        }
    }
    retainedChain_.clear();
    Node *current = nullptr;
    for (const auto &ancestor : ancestors) {
        current = add_node(ancestor, current, true);
        retainedChain_.push_back(QPersistentModelIndex(current->index()));
    }
    return current == nullptr ? QModelIndex{} : current->index();
}

bool LazyDirectoryTreeModel::hasChildren(const QModelIndex &parent) const {
    if (!parent.isValid()) {
        return rowCount() != 0;
    }
    const auto *item = node(parent);
    return item != nullptr && (item->rowCount() != 0 || item->state != Node::State::loaded);
}

bool LazyDirectoryTreeModel::canFetchMore(const QModelIndex &parent) const {
    const auto *item = node(parent);
    return item != nullptr && item->state == Node::State::unknown;
}

void LazyDirectoryTreeModel::fetchMore(const QModelIndex &parent) {
    if (canFetchMore(parent)) {
        load_children(parent);
    }
}

void LazyDirectoryTreeModel::load_children(const QModelIndex &index) {
    auto *item = node(index);
    if (item == nullptr || (item->state != Node::State::unknown &&
                            item->state != Node::State::failed)) {
        return;
    }
    if (pending_.size() == kMaximumPendingRequests) {
        // A newer explicit navigation wins; evicted work needs a new expansion/refresh.
        if (auto *evicted = node(pending_.front())) {
            evicted->state = Node::State::failed;
            evicted->incomplete = true;
        }
        pending_.pop_front();
    }
    item->state = Node::State::queued;
    pending_.emplace_back(index);
    start_next();
}

void LazyDirectoryTreeModel::start_next() {
    if (activeGeneration_ != 0) {
        return;
    }
    while (!pending_.empty()) {
        const auto index = pending_.front();
        pending_.pop_front();
        auto *item = node(index);
        if (item == nullptr || item->state != Node::State::queued) {
            continue;
        }
        active_ = index;
        activeGeneration_ = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
        activeTruncated_ = false;
        item->state = Node::State::loading;
        timer_.start();
        // DirectoryService replaces its previous request, so only one may be active.
        source_.submit({.generation = activeGeneration_, .path = native_path(item->path)});
        emit dataChanged(index, index, {Qt::ToolTipRole, ErrorRole});
        return;
    }
    timer_.stop();
}

void LazyDirectoryTreeModel::cancel_requests(const QModelIndex &index, const bool descendants) {
    const auto matches = [&](const QModelIndex &candidate) {
        return descendants ? in_branch(candidate, index) : candidate == index;
    };
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!it->isValid() || matches(*it)) {
            if (auto *item = node(*it)) {
                item->state = Node::State::unknown;
            }
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    if (activeGeneration_ != 0 && (!active_.isValid() || matches(active_))) {
        if (auto *item = node(active_)) {
            item->state = Node::State::unknown;
        }
        const auto generation = std::exchange(activeGeneration_, 0);
        active_ = QPersistentModelIndex{};
        source_.cancel(generation);
    }
}

void LazyDirectoryTreeModel::cancel(const QModelIndex &index) {
    if (node(index) == nullptr) {
        return;
    }
    cancel_requests(index, true);
    start_next();
}

void LazyDirectoryTreeModel::refresh(const QModelIndex &index) {
    if (auto *item = node(index)) {
        cancel_requests(index, false);
        item->state = Node::State::unknown;
        load_children(index);
    }
}

void LazyDirectoryTreeModel::forget_subtree(Node *item) {
    paths_.remove(path_key(item->path));
    for (int row = 0; row < item->rowCount(); ++row) {
        if (auto *child = dynamic_cast<Node *>(item->child(row))) {
            forget_subtree(child);
        }
    }
}

void LazyDirectoryTreeModel::reconcile_children(
    const QModelIndex &index, const QList<QPair<QString, QString>> &directories,
    const bool complete) {
    auto *parent = node(index);
    if (parent == nullptr) {
        return;
    }
    cancel_requests(index, false);
    catalog::CatalogBatch batch;
    batch.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    batch.entries.reserve(static_cast<std::size_t>(directories.size()));
    for (const auto &[name, path] : directories) {
        core::DirectoryEntry entry;
        entry.kind = core::EntryKind::directory;
        entry.name_utf8 = name.toUtf8().toStdString();
        entry.path_utf8 = path.toUtf8().toStdString();
        batch.entries.push_back(std::move(entry));
    }
    merge_batch(parent, batch);
    finish_children(parent, complete, batch.generation, !complete);
    start_next();
}

void LazyDirectoryTreeModel::merge_batch(Node *parent, const catalog::CatalogBatch &batch) {
    for (const auto &entry : batch.entries) {
        if (entry.kind != core::EntryKind::directory) {
            continue;
        }
        const auto chain = ancestor_paths(QString::fromUtf8(entry.path_utf8));
        if (chain.size() < 2 || path_key(chain.at(chain.size() - 2)) != path_key(parent->path)) {
            continue;
        }
        auto child_path = parent->path;
        if (!child_path.endsWith(QLatin1Char('/'))) {
            child_path += QLatin1Char('/');
        }
        // An earlier request may still carry the old parent spelling in its entry paths.
        child_path += chain.back().mid(chain.back().lastIndexOf(QLatin1Char('/')) + 1);
        auto *child = add_node(child_path, parent, false);
        update_spelling(child, child_path);
        child->seenGeneration = batch.generation;
    }
}

void LazyDirectoryTreeModel::finish_request(Node *parent, const bool complete) {
    const auto generation = std::exchange(activeGeneration_, 0);
    active_ = QPersistentModelIndex{};
    finish_children(parent, complete, generation, activeTruncated_);
}

void LazyDirectoryTreeModel::finish_children(Node *parent, const bool complete,
                                             const catalog::RequestGeneration generation,
                                             const bool truncated) {
    parent->state = complete ? Node::State::loaded : Node::State::failed;
    parent->incomplete = truncated;
    if (complete) {
        // Missing children are removed only after success; navigation placeholders survive.
        for (int row = parent->rowCount() - 1; row >= 0; --row) {
            auto *child = dynamic_cast<Node *>(parent->child(row));
            if (child != nullptr && !child->retained && child->seenGeneration != generation) {
                cancel_requests(child->index(), true);
                forget_subtree(child);
                parent->removeRow(row);
            }
        }
    }
    const auto index = parent->index();
    emit dataChanged(index, index);
}

void LazyDirectoryTreeModel::poll() {
    if (polling_) {
        return;
    }
    const QScopedValueRollback guard(polling_, true);
    if (activeGeneration_ != 0 && !active_.isValid()) {
        source_.cancel(std::exchange(activeGeneration_, 0));
    }
    if (activeGeneration_ == 0) {
        start_next();
    }
    for (int count = 0; activeGeneration_ != 0 && count < kMaximumBatchesPerPoll; ++count) {
        auto batch = source_.poll();
        if (!batch) {
            break;
        }
        if (batch->generation != activeGeneration_) {
            continue;
        }
        auto *parent = node(active_);
        if (parent == nullptr) {
            source_.cancel(std::exchange(activeGeneration_, 0));
            break;
        }
        merge_batch(parent, *batch);
        activeTruncated_ = activeTruncated_ || batch->truncated;
        if (batch->is_final || batch->error) {
            finish_request(parent, !batch->error && !activeTruncated_);
            break;
        }
    }
    start_next();
}

} // namespace vove::ui
