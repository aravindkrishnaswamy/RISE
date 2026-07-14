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

private:
    /// True iff editing is currently allowed: an editable bound
    /// environment AND the scene isn't wedged by an in-flight render.
    bool canEdit() const;

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
