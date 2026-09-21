#pragma once

#include "drag_views.hpp"
#include "vove/catalog/catalog_types.hpp"

#include <QCollator>
#include <QHash>
#include <QPersistentModelIndex>
#include <QStringList>
#include <QTimer>

#include <deque>

namespace vove::ui {

class LazyDirectoryTreeModel final : public DirectoryTreeModel {
  public:
    // The source is exclusive to this model and must outlive it.
    explicit LazyDirectoryTreeModel(catalog::DirectorySource &source, QObject *parent = nullptr);
    ~LazyDirectoryTreeModel() override;
    static constexpr int ErrorRole = PathRole + 1;

    // Drive letters on Windows; / elsewhere (mounted volumes retain their real hierarchy).
    // Lists roots only, without checking readiness or inspecting their contents.
    [[nodiscard]] static QStringList detected_roots();
    // Additive and lexical only; does not change the retained navigation chain.
    // Accepts drive, UNC share, and / roots. Invalid paths and non-roots are ignored.
    void add_roots(const QStringList &roots);

    // Lexical only: retains this current ancestor chain, releasing the previous chain.
    [[nodiscard]] QModelIndex ensure_path(const QString &path);
    void load_children(const QModelIndex &index);
    void refresh(const QModelIndex &index);
    // Reuse the open catalog's completed listing; never scan its children again for the tree.
    void reconcile_children(const QModelIndex &index,
                             const QList<QPair<QString, QString>> &directories, bool complete);
    // Connect expanded to load_children and collapsed to cancel (also cancels descendants).
    void cancel(const QModelIndex &index);
    void poll();

    [[nodiscard]] bool hasChildren(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

  private:
    struct Node;

    [[nodiscard]] Node *node(const QModelIndex &index) const;
    Node *add_node(const QString &path, Node *parent, bool retained);
    void update_spelling(Node *item, const QString &path);
    void start_next();
    void cancel_requests(const QModelIndex &index, bool descendants);
    void forget_subtree(Node *item);
    void merge_batch(Node *parent, const catalog::CatalogBatch &batch);
    void finish_request(Node *parent, bool complete);
    void finish_children(Node *parent, bool complete, catalog::RequestGeneration generation,
                         bool truncated);

    catalog::DirectorySource &source_;
    QTimer timer_;
    QCollator collator_;
    QHash<QString, QPersistentModelIndex> paths_;
    QList<QPersistentModelIndex> retainedChain_;
    std::deque<QPersistentModelIndex> pending_;
    QPersistentModelIndex active_;
    catalog::RequestGeneration activeGeneration_{};
    bool activeTruncated_{};
    bool polling_{};
};

} // namespace vove::ui
