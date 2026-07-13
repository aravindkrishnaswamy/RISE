//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.h - Toolbar for the interactive viewport, restyled
//    (RISE UI redesign, design brief "Viewport toolbar") into grouped
//    tool buttons + a right-hand cluster of status chips: active-camera
//    (read-only, now with an honest lens summary when the primary
//    selection verifiably is the active camera), REGION (3-state:
//    off/armed/active), EV (click opens an exposure-slider popup), and
//    EDR (mirrors View > HDR Preview).  Mirrors the macOS
//    ContentView.swift's viewport toolbar row.
//
//  Discoverability redesign (2026-07, mirrors ViewportToolbar.swift):
//  the PREVIOUS design put one button per CATEGORY (Select / Camera /
//  ObjectTransform), with the camera/transform sub-tools (orbit, pan,
//  zoom, roll; translate, rotate, scale) hidden behind a right-click
//  flyout menu.  User testing found nobody discovered the flyout, so
//  heavily-used sub-tools were effectively invisible.  Every tool is
//  now its OWN always-visible, labeled button, grouped into three
//  visually separated clusters:
//
//    Select  — Select
//    Camera  — Orbit, Pan, Zoom, Roll
//    Object  — Move, Rotate, Scale   (gizmo overlay renders in
//               ViewportWidget when an Object tool is active)
//
//  `ViewportBridge::lastSubToolForCategory` (the "remember the last-
//  used sub-tool per category" controller-side memory the old flyout
//  UI queried on every poll to decide which icon to show on a
//  collapsed slot) is no longer called from this file — there is no
//  collapsed slot left to populate.  The bridge method itself is
//  untouched and may still be meaningful controller-side state; it is
//  simply dead from this toolbar's point of view now.
//
//////////////////////////////////////////////////////////////////////

#ifndef VIEWPORTTOOLBAR_H
#define VIEWPORTTOOLBAR_H

#include <QHash>
#include <QSlider>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include "ViewportBridge.h"

class QLabel;
class QTimer;
class QMenu;

// QSlider subclass that emits `resetRequested` on a double-click
// anywhere in the widget rect.  Lives here (rather than MainWindow.h,
// where it was declared pre-slice-B) because the actual slider WIDGET
// now lives inside ViewportToolbar's EV chip popup; MainWindow still
// owns the business logic (RenderEngine::setViewExposureEV) via
// exposureSlider()'s signals.  Users double-click the track to snap
// back to 0 EV.
class ExposureSlider : public QSlider
{
    Q_OBJECT
public:
    explicit ExposureSlider(QWidget* parent = nullptr) : QSlider(Qt::Horizontal, parent) {}
signals:
    void resetRequested();
protected:
    void mouseDoubleClickEvent(QMouseEvent* /*e*/) override { emit resetRequested(); }
};

class ViewportToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportToolbar(QWidget* parent = nullptr);

    /// Borrows the bridge for the active-camera chip (incl. its lens
    /// summary), and the REGION chip's 3-state poll.  Safe to call
    /// multiple times (e.g., on scene reload that swaps the bridge).
    void setBridge(ViewportBridge* bridge);

    ViewportTool currentTool() const { return m_current; }

    /// The EV chip's popup slider.  MainWindow connects its
    /// valueChanged/resetRequested signals to the RenderEngine-facing
    /// slots (onExposureSliderChanged/onExposureResetRequested) each
    /// time a new ViewportToolbar is built -- the widget resets to a
    /// fresh instance (and 0 EV) per scene load, matching the rest of
    /// the per-scene viewport chrome's lifetime.
    ExposureSlider* exposureSlider() const { return m_exposureSlider; }

    static constexpr int kExposureSliderMin = -60;  //  -6.0 EV
    static constexpr int kExposureSliderMax =  60;  //  +6.0 EV

    /// True while the REGION chip is armed (next viewport drag draws
    /// the region) -- ViewportWidget reads this via
    /// setRegionArmed()'s mirrored state, not this getter directly;
    /// exposed for completeness / tests.
    bool isRegionArmed() const { return m_regionArmed; }

    /// Round-2 P1: the toolbar's undo/redo buttons drive the same
    /// scene-transport path the Edit menu gates on
    /// canUseSceneTransport-equivalent state (production render OR
    /// outstanding chat-driven render).  MainWindow::
    /// updateMenuActionStates calls this with the SAME term it uses
    /// for the Edit-menu actions, so the two affordances can't drift.
    /// The rest of the toolbar (tools / camera / region / EV chips)
    /// deliberately stays on the broader production-only enable.
    void setUndoRedoEnabled(bool enabled);

signals:
    void toolChanged(ViewportTool t);
    void undoClicked();
    void redoClicked();

    /// EDR chip clicked -- MainWindow forwards this to
    /// `m_hdrToggleAction->toggle()` so both the menu item and this
    /// chip stay a single source of truth.
    void edrToggleClicked();

    /// REGION chip's armed state changed (off<->armed) -- ViewportWidget
    /// listens so it knows whether the next drag should draw a region
    /// box instead of routing pointer events to the bridge.
    void regionArmedChanged(bool armed);

public slots:
    /// Reflect the View > HDR Preview action's checked state on the
    /// EDR chip.
    void setEdrChecked(bool checked);
    /// Reflect HDR *availability* (independent of checked state) --
    /// the chip is disabled entirely when no HDR-capable display is
    /// active, matching the menu action.
    void setEdrEnabled(bool enabled);

    /// Enable/disable the EV chip -- mirrors the old exposure slider's
    /// HDR interlock: exposure is meaningless (and double-maps the
    /// signal) once the OS compositor's HDR tone map is in the loop.
    void setEvEnabled(bool enabled);

    /// Cancel an in-progress REGION arm without drawing a box --
    /// wired to ViewportWidget's Escape handler and to
    /// MainWindow::onRender/onRenderAnimation's productionRenderStarting-
    /// style gate (a production render must not leave the toolbar
    /// showing "armed" for a drag that can no longer land).
    void cancelRegionArm();

private slots:
    void onToolButtonClicked();
    void onRegionChipClicked();
    void onEdrChipClicked();
    void pollState();

private:
    /// One always-visible tool button.  Holds the tool and a pointer to
    /// the QToolButton that renders it; `refreshAllToolButtons()` syncs
    /// every button's checked state to `m_current` on every tool change.
    struct ToolButtonEntry {
        ViewportTool  tool;
        QToolButton*  button;
    };

    QToolButton* makeToolButton(ViewportTool t);
    void         refreshAllToolButtons();
    void         applyToolSelection(ViewportTool t);
    QIcon        iconForTool(ViewportTool t) const;
    QString      labelForTool(ViewportTool t) const;
    /// Per-tool tooltip — every button gets its own descriptive text
    /// now that there's no category-level flyout to describe instead.
    QString      tooltipForTool(ViewportTool t) const;
    QVector<ViewportTool> subToolsForCategory(ViewportBridge::ToolCategory cat) const;

    void updateRegionChip();
    void updateCameraChip();
    /// Lens summary appended to the camera chip, e.g. "50mm · f/4" or
    /// "62° FOV · f/8" — mirrors ContentView.swift's cameraLensSummary.
    /// Returns an empty string (name-only fallback) unless the PANEL's
    /// current selection verifiably echoes `activeCameraName` (same
    /// honesty gate as the Mac chip — see the .cpp for the full
    /// rationale on why this can't just trust `activeNameForCategory`
    /// alone).
    QString cameraLensSummary(const QString& activeCameraName) const;
    void updateEvChipLabel(int sliderValue);

    QVector<ToolButtonEntry> m_toolButtons;
    ViewportTool     m_current = ViewportTool::Select;
    ViewportBridge*  m_bridge  = nullptr;   // borrowed; outlives the toolbar

    // ---- Right-hand status cluster --------------------------------------
    QLabel*      m_cameraChip = nullptr;
    // Round-2 P1: member pointers so setUndoRedoEnabled can gate them
    // (they were construction-time locals before, un-disable-able).
    QToolButton* m_undoBtn = nullptr;
    QToolButton* m_redoBtn = nullptr;

    QToolButton* m_regionChip = nullptr;
    bool         m_regionArmed = false;

    QToolButton*     m_evChip = nullptr;
    QMenu*           m_evMenu = nullptr;
    ExposureSlider*  m_exposureSlider = nullptr;
    QLabel*          m_evValueLabel = nullptr;

    QToolButton* m_edrChip = nullptr;
    bool         m_edrChecked = false;
    bool         m_edrAvailable = false;

    QTimer* m_pollTimer = nullptr;   // 500ms: active-camera name + REGION 3-state
};

#endif // VIEWPORTTOOLBAR_H
