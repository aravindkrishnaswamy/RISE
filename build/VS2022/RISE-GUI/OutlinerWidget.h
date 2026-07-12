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

signals:
    /// Fired after this widget calls a bridge selection/expansion
    /// mutator -- MainWindow forwards this to
    /// ViewportProperties::refresh() so the single-entity inspector
    /// follows the outliner's pick immediately rather than waiting for
    /// the next imageUpdated-driven refresh.
    void selectionActivated();

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
};

#endif // OUTLINERWIDGET_H
