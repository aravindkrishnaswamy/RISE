//////////////////////////////////////////////////////////////////////
//
//  EnvironmentPanel.h - RISE UI redesign, right-panel "Environment"
//    section: the scene's image-based-lighting (IBL) environment as a
//    first-class, always-present control group (NOT a per-selection
//    inspector, since the environment is a scene-level singleton -- an
//    hdr/exr painter bound via radiance_* on the active rasterizer,
//    invisible to the Lights category).  Mirrors the macOS
//    EnvironmentPanel.swift.
//
//    States (all decided in reload() from ViewportBridge::environmentInfo):
//      * no scene / no active rasterizer -> the whole widget hides
//        (setVisible(false))
//      * procedural sky (hosek) installed -> read-only note
//      * no environment                  -> "Add HDRI..." (when editable)
//      * environment bound               -> file (Swap...), intensity,
//        rotation X/Y/Z (degrees), "Show in background", Remove
//
//    Every edit routes through ViewportBridge -> SceneEditController,
//    which applies it LIVE (viewport re-renders) and mirrors it into the
//    CST so a save keeps it.  Edit-enablement gates on the SAME
//    bridgeInteractingEnabled term MainWindow pushes to OutlinerWidget /
//    ViewportProperties via setSceneEditable (mirrors Mac's
//    isSceneEditableForAgents).  See docs/gui/ENVIRONMENT_SECTION.md.
//
//////////////////////////////////////////////////////////////////////

#ifndef ENVIRONMENTPANEL_H
#define ENVIRONMENTPANEL_H

#include <QWidget>
#include <QPointer>
#include <QString>

#include <functional>

#include "ViewportBridge.h"   // for ViewportBridge / EnvironmentInfo

class QVBoxLayout;
class QLineEdit;

class EnvironmentPanel : public QWidget
{
    Q_OBJECT

public:
    explicit EnvironmentPanel(QWidget* parent = nullptr);

    /// Borrows the bridge; safe to call again with a new pointer (or
    /// nullptr) on scene reload/teardown.  Mirrors OutlinerWidget::setBridge.
    void setBridge(ViewportBridge* bridge);

public slots:
    /// Re-read environmentInfo() and rebuild.  Called on scene load
    /// (setBridge), on every preview frame (rides
    /// ViewportBridge::imageUpdated -- cheap, mirrors OutlinerWidget), and
    /// after any edit this widget itself makes.  Skips the rebuild while
    /// one of this panel's own numeric fields has focus so a live frame
    /// can't blow away an in-progress edit (Windows equivalent of the Mac
    /// panel's focusedFieldIsActive buffer guard).
    void reload();

    /// Pushed by MainWindow::updateMenuActionStates with the SAME
    /// bridgeInteractingEnabled term that gates OutlinerWidget /
    /// ViewportProperties (mirrors Mac's isSceneEditableForAgents) -- a
    /// production or chat-driven render owning the controller's commit
    /// mutex disables every mutating control here, since each
    /// ViewportBridge setEnvironment* call takes that mutex.
    void setSceneEditable(bool editable);

signals:
    /// Emitted after a successful mutating environment edit.  MainWindow
    /// forwards it to OutlinerWidget::refresh + ViewportProperties::refresh
    /// so the other panels follow immediately (an Add/Remove changes the
    /// Painter list; a live edit re-renders anyway) -- mirrors the Mac
    /// panel's refreshTrigger bump.
    void environmentEdited();

    /// Source traceability: a per-row context menu ("Reveal ... in Scene
    /// File") was invoked.  MainWindow connects this to revealSourceSpan(),
    /// which highlights the backing param's EXACT span: the HDRI file lives
    /// on the bound PAINTER chunk (category Painter, name = m_env.painterName,
    /// param "file"); intensity/rotation/background are radiance_* params on
    /// the ACTIVE RASTERIZER chunk (category Rasterizer, empty name).
    /// `category` is a raw int using ViewportBridge::Category numbering
    /// (Rasterizer = 2, Painter = 10), matching MainWindow::revealSourceSpan.
    void revealEnvParamRequested(int category, const QString& name, const QString& param);

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): hook QEvent::PaletteChange
    // here and call restyleTheme() -- mirrors MainWindow::changeEvent,
    // the contract's reference implementation.
    void changeEvent(QEvent* e) override;

private:
    /// True iff editing is currently allowed: an editable bound
    /// environment AND the scene isn't wedged by an in-flight render.
    bool canEdit() const;

    // LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies this panel's
    // PERSISTENT-CHROME token-dependent styling -- just the outer
    // widget's palette fill + border-bottom stylesheet, set once in the
    // constructor and never revisited by rebuild().  Does NOT
    // SYNCHRONOUSLY touch rebuild()'s own content (header/rows): see the
    // doc comment at this method's definition for why that's safe to
    // leave alone -- P2 fix (2026-07-23 review, LIVE THEME-SWITCH
    // CONTRACT point 5) instead QUEUES a rebuild() via
    // QTimer::singleShot(0, ...) for the idle-viewport case nothing else
    // would self-heal (rebuild() here reads only cached members, no
    // bridge round-trip, so this is unconditional -- no scrubbing-style
    // guard needed).  Called once at the end of the constructor and
    // again from changeEvent() on QEvent::PaletteChange.  Idempotent,
    // creates no widgets SYNCHRONOUSLY (the queued follow-up runs on the
    // next event-loop turn, outside this call).
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // See MainWindow.h for the full rationale; uniform across every
    // changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    void rebuild();
    bool anyFieldFocused() const;

    /// Re-read + rebuild + notify after a bridge mutator.  On a refused
    /// add (ok == false with a non-empty message) surfaces the core's
    /// diagnostic via QMessageBox::warning, mirroring how OutlinerWidget /
    /// MainWindow surface removeEntity refusals.
    void finishEdit(bool ok, const QString& failureMessage);

    /// Commit all three rotation axes together (radiance_orient is one
    /// vec3): a change to any one field re-sends X/Y/Z.
    void commitRotation();

    /// Present a QFileDialog restricted to HDR / EXR and invoke `chosen`
    /// with the picked absolute path (the picker is the guard against a
    /// non-existent file).
    void pickFile(const std::function<void(const QString&)>& chosen);

    /// Install a custom context menu on `target` that emits
    /// revealEnvParamRequested(category, name, param) when its single
    /// "Reveal ... in Scene File" item is chosen -- the Windows/Qt analogue
    /// of the Mac panel's per-row `.contextMenu { revealButton(...) }`.
    /// `label` is the already-translated menu-item text.  `category` is a
    /// raw int (ViewportBridge::Category numbering).
    void installRevealMenu(QWidget* target, const QString& label,
                            int category, const QString& name, const QString& param);

    ViewportBridge* m_bridge = nullptr;
    QVBoxLayout*    m_rootLayout = nullptr;

    bool m_expanded      = true;
    bool m_sceneEditable = false;

    // Last snapshot pulled from the bridge; m_hasInfo mirrors
    // environmentInfo()'s return (false == no scene / no active rasterizer).
    EnvironmentInfo m_env;
    bool            m_hasInfo = false;

    // Editable numeric fields for the bound state.  QPointer so the
    // focus-guard check in reload() stays safe even after a rebuild has
    // deleteLater()'d the previous set.
    QPointer<QLineEdit> m_scaleEdit;
    QPointer<QLineEdit> m_orientXEdit;
    QPointer<QLineEdit> m_orientYEdit;
    QPointer<QLineEdit> m_orientZEdit;
};

#endif // ENVIRONMENTPANEL_H
