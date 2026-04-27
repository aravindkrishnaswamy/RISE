//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.h - Tool palette + undo/redo for the interactive
//    viewport.  Eight tool buttons matching SceneEditController::Tool.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTTOOLBAR_H
#define VIEWPORTTOOLBAR_H

#include <QWidget>
#include <QToolButton>

#include "ViewportBridge.h"

class ViewportToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportToolbar(QWidget* parent = nullptr);

    ViewportTool currentTool() const { return m_current; }

signals:
    void toolChanged(ViewportTool t);
    void undoClicked();
    void redoClicked();

private slots:
    void onToolButtonClicked();

private:
    QToolButton* makeToolButton(ViewportTool t,
                                const QString& iconName,
                                const QString& label,
                                const QString& tooltip);

    QVector<QToolButton*> m_toolButtons;
    ViewportTool          m_current = ViewportTool::Select;
};

#endif // VIEWPORTTOOLBAR_H
