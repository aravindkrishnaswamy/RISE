//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.h - Right-side accordion for the interactive
//    viewport.  Four sections (Cameras / Rasterizer / Objects /
//    Lights) each list the scene's entities; clicking a row activates
//    it on the C++ side and shows its read-only/edit properties below.
//    Mirrors the macOS PropertiesPanel.swift and Android ViewportPane.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTPROPERTIES_H
#define VIEWPORTPROPERTIES_H

#include <QWidget>
#include <QVector>
#include <QHash>

#include "ViewportBridge.h"   // for ViewportBridge::PanelMode / Category

class QVBoxLayout;
class QLineEdit;
class QLabel;
class QScrollArea;
class QComboBox;
class QToolButton;
class QFrame;

namespace ViewportPropertiesInternal {
    struct AccordionSectionWidgets {
        QWidget*     container = nullptr;   ///< wrapper holding header + body + properties
        QToolButton* toggle    = nullptr;   ///< collapsible header (checkable, arrow indicator)
        QFrame*      body      = nullptr;   ///< holds combo + properties
        QComboBox*   combo     = nullptr;   ///< entity selector (dropdown for every section)
        QFrame*      propsFrame= nullptr;   ///< container for property rows when this section is selected
        QVBoxLayout* propsLayout = nullptr;
    };
}

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
    void clearPropertyRows();

    /// Re-pull each section's entity list from the bridge.  Called
    /// when the scene epoch advances (scene reload, structural mutation).
    void rebuildEntityLists();

    /// Update every section's expanded/collapsed state and selected
    /// row to reflect the bridge's current selection tuple.
    void syncAccordionFromSelection();

    /// Rebuild the property-row list under whichever section is
    /// currently expanded.  Property rows are entity-specific.
    void rebuildPropertyRows();

    using Category = ViewportBridge::Category;
    using SectionWidgets = ViewportPropertiesInternal::AccordionSectionWidgets;

    ViewportBridge*                 m_bridge = nullptr;
    QLabel*                         m_headerLabel = nullptr;
    QVBoxLayout*                    m_listLayout = nullptr;
    QScrollArea*                    m_scroll = nullptr;
    QHash<int, SectionWidgets>      m_sections;     // keyed by Category int
    QHash<QString, QLineEdit*>      m_fields;       // editable rows by parameter name
    QHash<QString, QLabel*>         m_readOnly;     // read-only rows by parameter name
    QHash<QString, QString>         m_lastValue;    // last canonicalized value
    Category                        m_currentSelectionCat = Category::None;
    QString                         m_currentSelectionName;
    unsigned int                    m_lastEpoch = 0;
};

#endif // VIEWPORTPROPERTIES_H
