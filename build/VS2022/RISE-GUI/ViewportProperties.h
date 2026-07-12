//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.h - RISE UI redesign, right panel single-entity
//    inspector for the interactive viewport.
//
//  Shows the SCENE's currently selected entity (whatever
//  ViewportBridge's PRIMARY selection is -- set either by clicking a
//  row in OutlinerWidget above this panel, or by picking an object
//  directly in the 3D viewport) with a descriptor-driven property
//  list, split into an always-visible "Basic" set and a collapsed
//  "Advanced" disclosure once a section has more than
//  kMaxBasicRows properties.  Mirrors the macOS PropertiesPanel.swift.
//
//  Pre-redesign this file also owned the nine-section stacked
//  accordion (Cameras / Rasterizer / Objects / ...) -- that navigation
//  role moved to OutlinerWidget; this file keeps the property-row
//  rendering / editing machinery (kind-specific value cells, scrub-to-
//  change numeric fields, preset quick-picks, in-flight-drag re-entry
//  guard) and restyles it to the redesign's token set (Theme.h).
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTPROPERTIES_H
#define VIEWPORTPROPERTIES_H

#include <QWidget>
#include <QVector>
#include <QHash>
#include <QString>

#include "ViewportBridge.h"   // for ViewportBridge::Category / PanelMode

class QVBoxLayout;
class QLineEdit;
class QLabel;
class QScrollArea;
class QToolButton;
class QFrame;

class ViewportProperties : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportProperties(ViewportBridge* bridge, QWidget* parent = nullptr);

public slots:
    /// Re-snapshot from the live entity.  Called on each render frame
    /// (camera state may have just changed via drag), on tool /
    /// selection change, and by OutlinerWidget::selectionActivated.
    void refresh();

signals:
    /// Fired after a successful in-place / Save-As round-trip save.
    /// MainWindow connects to re-anchor RenderEngine::loadedFilePath
    /// and refresh the SceneEditor text pane.  Not emitted on NoOp.
    void sceneSavedToPath(const QString& path);

private slots:
    void onLineEditFinished();

    /// Phase 6.5: react to the bridge's dirty-state transition by
    /// flipping the Save / Save As... buttons' enable state and
    /// tooltip text.
    void onDirtyChanged(bool hasUnsavedChanges);

private:
    using Category = ViewportBridge::Category;

    void performSceneSave(bool useLoadedPath);
    void clearPropertyRows();

    /// Rebuild the entity-header row (icon chip + name + meta line)
    /// from the bridge's current primary selection.
    void rebuildEntityHeader();

    /// Rebuild the Basic/Advanced property row lists for the current
    /// primary selection.  Membership for the Basic set (when a
    /// section exceeds kMaxBasicRows rows): editable rows with quick-
    /// pick presets first, then remaining descriptor-order rows until
    /// the cap; DISPLAY order for both groups stays original descriptor
    /// order (the priority pass only decides membership).
    void rebuildPropertyRows();

    /// Build one label + value-cell row for `p`, appended to `into`.
    void buildPropertyRow(const ViewportProperty& p, QVBoxLayout* into);

    /// Commits an edited value through setPropertyForCategory against
    /// the CURRENT primary selection category, then refresh()es.
    void commitEdit(const QString& name, const QString& value);

    void updateAdvancedToggleLabel(int advancedCount);
    void onAdvancedToggleClicked();

    void onUseInViewportClicked();
    void onAddCameraClicked();

    static QString categoryTitle(Category cat);
    static QString categoryGlyph(Category cat);

    ViewportBridge* m_bridge = nullptr;

    // ---- Entity header ----------------------------------------------
    QLabel* m_iconChip  = nullptr;
    QLabel* m_nameLabel = nullptr;
    QLabel* m_metaLabel = nullptr;

    // ---- Camera-only affordances ("Use in viewport" / "Add Camera") -
    QWidget*     m_cameraAffordances = nullptr;
    QToolButton* m_useInViewportBtn  = nullptr;
    QToolButton* m_addCameraBtn      = nullptr;

    // ---- Property body: Basic (always shown) + Advanced disclosure --
    QVBoxLayout* m_basicLayout    = nullptr;
    QWidget*     m_advancedToggleRow   = nullptr;
    QLabel*      m_advancedArrowLabel  = nullptr;
    QLabel*      m_advancedCountLabel  = nullptr;
    QWidget*     m_advancedContainer   = nullptr;
    QVBoxLayout* m_advancedLayout      = nullptr;
    bool         m_advancedExpanded    = false;
    QLabel*      m_emptyMessage        = nullptr;

    QVBoxLayout* m_bodyLayout = nullptr;   // header + camera affordances + basic/advanced, inside the scroll
    QScrollArea* m_scroll     = nullptr;

    // ---- Footer: Save / Save As... -----------------------------------
    QToolButton* m_saveButton   = nullptr;
    QToolButton* m_saveAsButton = nullptr;

    QHash<QString, QLineEdit*> m_fields;     // editable rows by parameter name
    QHash<QString, QLabel*>    m_readOnly;   // read-only rows by parameter name
    QHash<QString, QString>    m_lastValue;  // last canonicalized value

    Category m_currentSelectionCat = Category::None;
    QString  m_currentSelectionName;
    // "<cat>|<name>" -- resets the Advanced disclosure whenever the
    // selected entity's identity changes, matching PropertiesPanel.swift.
    QString  m_lastEntityKey;

    /// Tracks whether we've already surfaced the in-memory-only caveat
    /// in this session, so the alert fires exactly once per session.
    bool m_addCameraCaveatShown = false;

    // Re-entry guard: a ScrubHandle drag drives setProperty -> re-render
    // -> imageUpdated -> refresh().  Rebuilding the property rows would
    // delete the very ScrubHandle whose mouseMoveEvent is on the stack,
    // killing the implicit mouse grab and freezing the drag at the
    // first move.  The ScrubHandle's begin/end bracket flips this on
    // and off; refresh() short-circuits while it is set.
    bool m_scrubbing = false;
};

#endif // VIEWPORTPROPERTIES_H
