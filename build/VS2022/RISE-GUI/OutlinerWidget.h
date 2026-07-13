//////////////////////////////////////////////////////////////////////
//
//  OutlinerWidget.h - RISE UI redesign, right panel "OUTLINER".
//
//  Category tree above the single-entity ViewportProperties inspector:
//  one row per scene category (arrow + 3-letter tag chip + name +
//  count), expandable to list that category's entities.  Both header
//  and child clicks route through the SAME ViewportBridge selection /
//  expansion calls the pre-redesign ViewportProperties accordion used
//  (isSectionExpanded / setSelection / collapseSection) -- the bridge
//  stays the single source of truth for "what's expanded" and "what's
//  selected", so viewport click-to-pick and outliner clicks stay in
//  sync automatically.  Mirrors the macOS OutlinerView.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef OUTLINERWIDGET_H
#define OUTLINERWIDGET_H

#include <QWidget>
#include <QColor>
#include <QHash>
#include <QStringList>

#include "ViewportBridge.h"   // for ViewportBridge::Category

class QVBoxLayout;
class QScrollArea;
class QLabel;

class OutlinerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinerWidget(QWidget* parent = nullptr);

    /// Borrows the bridge; safe to call again with a new pointer (or
    /// nullptr) on scene reload/teardown.
    void setBridge(ViewportBridge* bridge);

public slots:
    /// Re-pull category entity lists (epoch-gated) + the current
    /// expansion/selection state from the bridge and rebuild the tree.
    /// Called on scene load, on every preview frame (cheap -- mirrors
    /// ViewportProperties::refresh's epoch gate), and after any
    /// selection change this widget itself triggers.
    void refresh();

    /// "Reveal in scene file" (item 3): pushed by MainWindow::
    /// updateMenuActionStates with the SAME bridgeInteractingEnabled
    /// term that gates undo/redo (mirrors Mac's
    /// isSceneEditableForAgents) -- gates each child row's context-menu
    /// item so ViewportBridge::getEntitySourceLocation() is never
    /// called while a production or chat-driven render owns the
    /// controller's commit mutex.  Entity-creation slice: the SAME term
    /// also gates each category header's "+" add-entity button and the
    /// child row context menu's Duplicate/Delete items, since all three
    /// mutating bridge calls take the same commit mutex.
    void setSceneEditable(bool editable);

signals:
    /// Fired after this widget calls a bridge selection/expansion
    /// mutator -- MainWindow forwards this to
    /// ViewportProperties::refresh() so the single-entity inspector
    /// follows the outliner's pick immediately rather than waiting for
    /// the next imageUpdated-driven refresh.
    void selectionActivated();

    /// "Reveal in scene file" context-menu item was clicked on a child
    /// row.  MainWindow connects this to its own
    /// revealEntityInSceneText(), the SAME slot the properties panel's
    /// ⌗ chip drives.
    void revealRequested(ViewportBridge::Category category, const QString& name);

    /// Entity-creation slice: a category header's "+" menu picked a
    /// template.  MainWindow owns the actual instantiate + select-new-
    /// entity + failure-alert sequence (mirrors macOS RenderViewModel.
    /// addEntity) -- this widget only reports the user's pick, the same
    /// division of labor `revealRequested` already uses.
    void addEntityRequested(ViewportBridge::Category category, unsigned int templateIndex);

    /// A child row's context-menu "Duplicate" was clicked.  MainWindow
    /// performs the duplicate + re-select (mirrors macOS
    /// RenderViewModel.duplicateSelectedOrNamed).
    void duplicateRequested(ViewportBridge::Category category, const QString& name);

    /// A child row's context-menu "Delete" was clicked.  MainWindow owns
    /// the confirm dialog + gate re-check + remove (mirrors macOS
    /// RenderViewModel.removeEntity) -- this widget does NOT confirm
    /// itself, so there is exactly one place that dialog can appear.
    void deleteRequested(ViewportBridge::Category category, const QString& name);

private:
    using Category = ViewportBridge::Category;

    struct CategoryDef {
        Category    category;
        const char* title;
        const char* tag;
        QColor      tagColor;
    };

    void rebuild();
    void toggleCategory(const CategoryDef& def);
    void selectChild(Category cat, const QString& name);

    ViewportBridge* m_bridge = nullptr;
    QScrollArea*    m_scroll = nullptr;
    QWidget*        m_listHolder = nullptr;
    QVBoxLayout*    m_listLayout = nullptr;
    QLabel*         m_countLabel = nullptr;

    // categoryEntities() results, epoch-gated like
    // ViewportProperties::rebuildEntityLists.
    QHash<int, QStringList> m_entitiesByCategory;
    unsigned int             m_lastEpoch = 0;

    // "Reveal in scene file" (item 3): see setSceneEditable's doc.
    // Consulted by rebuild() to compute each child row's canReveal.
    bool m_sceneEditable = false;
};

#endif // OUTLINERWIDGET_H
