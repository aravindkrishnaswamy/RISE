//////////////////////////////////////////////////////////////////////
//
//  NamedViewsPanel.h - Right-panel "Named Views" section (Windows Qt6).
//
//    Session/UI camera view bookmarks mirroring macOS
//    NamedViewsPanel.swift (docs/gui/CAMERA_CONTROLS_UX.md §3).
//    Capture the current view (free-fly pose if active, else the active
//    camera), click a row to restore it non-destructively into the
//    transient viewport pose, or use per-row actions to Update, Promote
//    to a scene camera, or Delete.  All bookmark state lives in the C++
//    SceneEditController's NamedViewStore via ViewportBridge.
//
//////////////////////////////////////////////////////////////////////

#ifndef NAMEDVIEWSPANEL_H
#define NAMEDVIEWSPANEL_H

#include <QWidget>
#include <QStringList>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class ViewportBridge;

class NamedViewsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit NamedViewsPanel(QWidget* parent = nullptr);

    /// Borrows the bridge; pass nullptr on scene teardown.  Hides the
    /// whole section while no bridge is attached (mirrors OutlinerWidget
    /// and EnvironmentPanel).
    void setBridge(ViewportBridge* bridge);

public slots:
    /// Re-read namedViewNames() when scene editing is unlocked and
    /// rebuild the row list only when the list actually changed.
    /// Persistent capture widgets (m_captureEdit / m_captureBtn) are
    /// never destroyed by reload(), so typing a bookmark name is never
    /// interrupted by per-frame ViewportBridge::imageUpdated ticks.
    void reload();

    /// Pushed by MainWindow::updateMenuActionStates with the same
    /// bridgeInteractingEnabled gate used by OutlinerWidget,
    /// EnvironmentPanel, and ViewportProperties so synchronous bridge
    /// calls here never contend with a production or chat-owned render.
    void setSceneEditable(bool editable);

signals:
    /// Emitted after promoteNamedView() creates and activates a new
    /// scene camera so MainWindow can refresh OutlinerWidget and
    /// ViewportProperties immediately.
    void cameraPromoted();

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): hook QEvent::PaletteChange
    // and re-apply token-dependent styling via restyleTheme().
    void changeEvent(QEvent* e) override;

private slots:
    void onCaptureClicked();

private:
    void rebuildRows();
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    ViewportBridge* m_bridge = nullptr;
    bool            m_expanded = true;
    bool            m_sceneEditable = false;
    bool            m_rowsBuilt = false;
    QStringList     m_names;

    // Persistent chrome + capture controls (built once in ctor).
    QHBoxLayout* m_headerLayout = nullptr;
    QLabel*      m_chevronLabel = nullptr;
    QLabel*      m_titleLabel = nullptr;
    QLabel*      m_countLabel = nullptr;

    QWidget*     m_contentWidget = nullptr;
    QLineEdit*   m_captureEdit = nullptr;
    QPushButton* m_captureBtn = nullptr;
    QLabel*      m_emptyLabel = nullptr;
    QWidget*     m_rowsContainer = nullptr;
    QVBoxLayout* m_rowsLayout = nullptr;
};

#endif // NAMEDVIEWSPANEL_H
