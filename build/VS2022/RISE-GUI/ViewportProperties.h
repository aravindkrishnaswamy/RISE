//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.h - Right-side properties panel — descriptor-
//    driven introspection of the active editable entity (camera).
//    Mirrors the macOS PropertiesPanel.swift.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTPROPERTIES_H
#define VIEWPORTPROPERTIES_H

#include <QWidget>
#include <QVector>
#include <QHash>

#include "ViewportBridge.h"   // for ViewportBridge::PanelMode

class QVBoxLayout;
class QLineEdit;
class QLabel;
class QScrollArea;

class ViewportProperties : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportProperties(ViewportBridge* bridge, QWidget* parent = nullptr);

public slots:
    /// Re-snapshot from the live entity.  Called on each render frame
    /// (camera state may have just changed via drag) and on tool /
    /// selection change (panel mode may have just flipped).
    void refresh();

private slots:
    void onLineEditFinished();

private:
    void clearRows();   // teardown when panel mode changes

    ViewportBridge*             m_bridge = nullptr;
    QLabel*                     m_headerLabel = nullptr;
    QLabel*                     m_emptyMessage = nullptr;
    QVBoxLayout*                m_listLayout = nullptr;
    QScrollArea*                m_scroll = nullptr;
    QHash<QString, QLineEdit*>  m_fields;       // editable rows by parameter name
    QHash<QString, QLabel*>     m_readOnly;     // read-only rows by parameter name
    QHash<QString, QString>     m_lastValue;    // last canonicalized value (used to skip mid-edit overwrites)
    ViewportBridge::PanelMode   m_currentMode = ViewportBridge::PanelMode::None;
};

#endif // VIEWPORTPROPERTIES_H
