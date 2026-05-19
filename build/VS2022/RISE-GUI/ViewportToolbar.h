//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.h - Photoshop-style toolbar for the interactive
//    viewport.  Three category slots (Select / Camera / Object
//    Transform); each multi-tool slot shows its last-used sub-tool's
//    icon and exposes the full list via a right-click context menu.
//    Mirrors the macOS ViewportToolbar.swift layout so the per-
//    platform feel is consistent.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTTOOLBAR_H
#define VIEWPORTTOOLBAR_H

#include <QHash>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include "ViewportBridge.h"

class ViewportToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportToolbar(QWidget* parent = nullptr);

    /// Borrows the bridge for `lastSubToolForCategory` lookups so a
    /// slot click reactivates the user's most recent sub-tool pick
    /// rather than the category default.  Safe to call multiple
    /// times (e.g., on scene reload that swaps the bridge).
    void setBridge(ViewportBridge* bridge);

    ViewportTool currentTool() const { return m_current; }

signals:
    void toolChanged(ViewportTool t);
    void undoClicked();
    void redoClicked();

private slots:
    void onSlotClicked();

private:
    /// One Photoshop-style slot.  Holds the category and a pointer to
    /// the QToolButton that renders it; updated by `refreshSlot()`
    /// whenever the current tool changes or the bridge's last-used
    /// memory shifts.
    struct Slot {
        ViewportBridge::ToolCategory category;
        QToolButton*                 button;
    };

    QToolButton* makeSlotButton(ViewportBridge::ToolCategory cat);
    void         refreshSlot(const Slot& s);
    void         refreshAllSlots();
    void         applyToolSelection(ViewportTool t);
    QIcon        iconForTool(ViewportTool t) const;
    QString      labelForTool(ViewportTool t) const;
    QString      tooltipForCategory(ViewportBridge::ToolCategory cat) const;
    QVector<ViewportTool> subToolsForCategory(ViewportBridge::ToolCategory cat) const;

    QVector<Slot>    m_slots;
    ViewportTool     m_current = ViewportTool::Select;
    ViewportBridge*  m_bridge  = nullptr;   // borrowed; outlives the toolbar
};

#endif // VIEWPORTTOOLBAR_H
