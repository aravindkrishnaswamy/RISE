//////////////////////////////////////////////////////////////////////
//
//  OutlinerWidget.h - RISE UI redesign, right panel "OUTLINER".
//
//  87 section 5 step 4c: a REAL TREE MODEL over the AUTHORED graph.
//  This used to be a hand-rolled TWO-LEVEL list of QWidget rows in a
//  QVBoxLayout (category header + one flat row per entity, all torn
//  down and recreated on every refresh).  It is now a QTreeView over a
//  QAbstractItemModel that draws whatever depth the authored graph
//  has: an Object with parts nests its parts under it.  A category
//  whose entities have no hierarchy is N ROOTS WITH NO CHILDREN
//  (87 step 4a's design), so there is NO per-category branch anywhere
//  below -- the flat categories render through the identical code path
//  the hierarchical one does.
//
//  Level 0 of the model is the eleven category headers (arrow +
//  3-letter tag chip + name + "+" + count); level 1+ is that
//  category's authored tree.  Category expansion and selection route
//  through the SAME ViewportBridge calls the pre-redesign
//  ViewportProperties accordion used (isSectionExpanded /
//  setSelection / collapseSection) -- the bridge stays the single
//  source of truth for "what's expanded" and "what's selected", so
//  viewport click-to-pick and outliner clicks stay in sync
//  automatically.  PER-NODE expansion is the one piece of state the
//  bridge does not model, and is held here (see m_expandedPaths).
//
//  Mirrors the macOS OutlinerView.swift (step 4b), which reaches the
//  same tree through RISEViewportBridge -categoryTree:.
//
//////////////////////////////////////////////////////////////////////

#ifndef OUTLINERWIDGET_H
#define OUTLINERWIDGET_H

#include <QWidget>
#include <QSet>
#include <QString>

#include "ViewportBridge.h"   // for ViewportBridge::Category, SceneTree

class QLabel;
class QPoint;
class QTreeView;

class OutlinerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinerWidget(QWidget* parent = nullptr);

    /// Borrows the bridge; safe to call again with a new pointer (or
    /// nullptr) on scene reload/teardown.
    void setBridge(ViewportBridge* bridge);

public slots:
    /// Re-pull the per-category AUTHORED TREES (epoch-gated) + the
    /// current expansion/selection state from the bridge and refresh
    /// the model.  Called on scene load, on every preview frame (cheap
    /// -- mirrors ViewportProperties::refresh's epoch gate), and after
    /// any selection change this widget itself triggers.
    ///
    /// A DRAG-TO-REPARENT gesture, when one is added, must bump the
    /// scene epoch itself: re-parenting changes the TREE without
    /// changing any category's entity LIST, so nothing else in the
    /// pipeline would notice and the epoch gate below would keep the
    /// old shape on screen.  SceneEditController.h says the same thing
    /// at ReadTree.
    void refresh();

    /// "Reveal in scene file" (item 3): pushed by MainWindow::
    /// updateMenuActionStates with the SAME bridgeInteractingEnabled
    /// term that gates undo/redo (mirrors Mac's
    /// isSceneEditableForAgents) -- gates each entity row's context-menu
    /// item so ViewportBridge::getEntitySourceLocation() is never
    /// called while a production or chat-driven render owns the
    /// controller's commit mutex.  Entity-creation slice: the SAME term
    /// also gates each category header's "+" add-entity button and the
    /// entity row context menu's Duplicate/Delete items, since all three
    /// mutating bridge calls take the same commit mutex.
    void setSceneEditable(bool editable);

signals:
    /// Fired after this widget calls a bridge selection/expansion
    /// mutator -- MainWindow forwards this to
    /// ViewportProperties::refresh() so the single-entity inspector
    /// follows the outliner's pick immediately rather than waiting for
    /// the next imageUpdated-driven refresh.
    void selectionActivated();

    /// "Reveal in scene file" context-menu item was clicked on an
    /// entity row.  MainWindow connects this to its own
    /// revealEntityInSceneText(), the SAME slot the properties panel's
    /// ⌗ chip drives.
    void revealRequested(ViewportBridge::Category category, const QString& name);

    /// Entity-creation slice: a category header's "+" menu picked a
    /// template.  MainWindow owns the actual instantiate + select-new-
    /// entity + failure-alert sequence (mirrors macOS RenderViewModel.
    /// addEntity) -- this widget only reports the user's pick, the same
    /// division of labor `revealRequested` already uses.
    void addEntityRequested(ViewportBridge::Category category, unsigned int templateIndex);

    /// An entity row's context-menu "Duplicate" was clicked.  MainWindow
    /// performs the duplicate + re-select (mirrors macOS
    /// RenderViewModel.duplicateSelectedOrNamed).
    void duplicateRequested(ViewportBridge::Category category, const QString& name);

    /// An entity row's context-menu "Delete" was clicked.  MainWindow owns
    /// the confirm dialog + gate re-check + remove (mirrors macOS
    /// RenderViewModel.removeEntity) -- this widget does NOT confirm
    /// itself, so there is exactly one place that dialog can appear.
    void deleteRequested(ViewportBridge::Category category, const QString& name);

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): hook QEvent::PaletteChange
    // here and call restyleTheme() -- mirrors MainWindow::changeEvent,
    // the contract's reference implementation.
    void changeEvent(QEvent* e) override;

private:
    using Category = ViewportBridge::Category;

    /// The QAbstractItemModel behind the QTreeView, defined out-of-line
    /// in OutlinerWidget.cpp.
    ///
    /// NOTHING FROM SceneEditController IS STORED IN IT.  The model's
    /// backing store is a flat row table it OWNS, and a QModelIndex's
    /// internalId() is an index into THAT table -- never a controller
    /// TreeNodeHandle, which 87 step 4a settled may not survive an
    /// event-loop turn (an incremental edit republishes the category's
    /// tree and refuses every outstanding handle; for Geometry that
    /// happens on every tick of a radius drag).  The table is rebuilt
    /// from scratch out of a fresh `categoryTree()` read whenever the
    /// shape changes, and every index minted before that rebuild dies
    /// with the model reset that publishes it.  Selection is BY NAME,
    /// which is what survives a republish.
    class TreeModel;

    void applyExpansionState();
    void toggleCategory(Category cat);
    void selectChild(Category cat, const QString& name);
    void toggleNodeExpansion(const QString& pathKey);
    void showAddMenu(Category cat, const QPoint& globalPos);
    void showRowContextMenu(Category cat, const QString& name, const QPoint& globalPos);

    // LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies this widget's
    // PERSISTENT-CHROME token-dependent styling (this widget's own
    // palette + border-bottom stylesheet, the "Scene" title label, the
    // entity-count label, the tree view's palette and its scrollbars).
    //
    // Unlike the pre-4c version, this needs NO queued rebuild -- this
    // panel is contract point 5's named exemption, which point 5 itself
    // now spells out.  Point 5's hazard is a theme switch while the
    // viewport is IDLE with nothing left to re-trigger the panel; that is
    // closed here because this function ends with an explicit
    // m_tree->viewport()->update() and is driven by QEvent::PaletteChange,
    // which contract point 1 guarantees reaches every nested widget.  A
    // repaint is then the whole fix, because the outliner's rows are no
    // longer widgets at all -- a QStyledItemDelegate paints them and reads
    // every token LIVE on each paint -- and a repaint creates no widgets
    // (contract point 2).  Called once at the end of the constructor and
    // again from changeEvent() on QEvent::PaletteChange.  Idempotent.
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // See MainWindow.h for the full rationale; uniform across every
    // changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    ViewportBridge* m_bridge = nullptr;
    QTreeView*      m_tree = nullptr;
    TreeModel*      m_model = nullptr;
    QLabel*         m_countLabel = nullptr;
    // "Scene" title label -- promoted from a constructor-local (was
    // `title`) so restyleTheme() can re-apply its token-dependent
    // stylesheet on a live theme switch; it's persistent chrome.
    QLabel*         m_titleLabel = nullptr;

    // categoryTree() results are pulled into the model, epoch-gated like
    // ViewportProperties::rebuildEntityLists.  m_treesPulled covers the
    // "epoch is legitimately 0 on first load" case the pre-4c code
    // covered with `m_entitiesByCategory.isEmpty()`.
    unsigned int m_lastEpoch = 0;
    bool         m_treesPulled = false;

    // Per-NODE disclosure state, keyed by tree PATH (87 section 5 step
    // 4), NOT by name.
    //
    //  - A NAME KEY WOULD COLLIDE.  A name is unique only within one
    //    manager, and this widget draws eleven categories side by side
    //    -- a Material and an Object may both be called `glass`, and
    //    Category::Painter is itself a UNION of two managers with
    //    independent contents.  One QSet keyed by bare name would tie
    //    unrelated rows' disclosure state together.  It also breaks the
    //    moment a node is re-parented, since the same name then means a
    //    different position in the tree.
    //  - A PATH KEY IS WHAT SURVIVES A REFRESH, which is the entire
    //    point: the model's row table is thrown away and rebuilt on
    //    every shape change (and QTreeView clears its own expanded set
    //    on a model reset), so any key derived from a POSITION -- a
    //    node index, a row number, a QModelIndex -- names a different
    //    node afterwards.  Ancestry is stable across a rebuild for
    //    exactly the same reason selection is: it is made of names.
    //
    // Not pruned when a node disappears, and deliberately not cleared by
    // setBridge() either, so it accumulates across document loads.  That is
    // bounded by USER ACTIONS, not by scene size: the only insert is
    // toggleNodeExpansion(), i.e. one entry per node the user has manually
    // disclosed and not collapsed again, over the life of the process.  A
    // session that opened a hundred scenes and expanded twenty nodes in
    // each holds two thousand short strings.  Pruning would cost the
    // property that makes the key a PATH in the first place: re-creating
    // the same node under the same parent -- an undo, a reload, a scene
    // re-opened later -- restores its disclosure state, and keys from a
    // different document are inert strings that no row ever asks about.
    QSet<QString> m_expandedPaths;

    // "Reveal in scene file" (item 3): see setSceneEditable's doc.
    // Pushed into the model, which gates the "+" tint and the row
    // context menu's items.
    bool m_sceneEditable = false;
};

#endif // OUTLINERWIDGET_H
