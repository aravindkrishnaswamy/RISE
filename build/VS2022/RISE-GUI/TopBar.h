//////////////////////////////////////////////////////////////////////
//
//  TopBar.h - The workspace's persistent 44 pt top bar (design brief
//    "Top bar"): scene identity (left), refinement-status cluster
//    (center: pause/resume + restart + status readout + progress
//    strip + integrator chip), Save (right).
//
//  Mirrors the macOS TopBar.swift + RefinementStatusFormatter.swift.
//  Native menu bar (MainWindow::createMenuBar) covers File / Edit /
//  Render / View — this in-window strip is always visible regardless
//  of which menu is open.
//
//////////////////////////////////////////////////////////////////////

#ifndef TOPBAR_H
#define TOPBAR_H

#include <QWidget>
#include <QString>

class QLabel;
class QToolButton;
class QPushButton;
class QTimer;
class RenderEngine;
class ViewportBridge;
class ChatPanel;
class TopBarLogoSwatch;
class TopBarProgressStrip;
class QFrame;

class TopBar : public QWidget
{
    Q_OBJECT

public:
    explicit TopBar(QWidget* parent = nullptr);

    /// Wires the render-state / progress signals this bar reads.
    /// Call once; RenderEngine is expected to outlive the TopBar
    /// (MainWindow owns both for the app's lifetime).
    void setEngine(RenderEngine* engine);

    /// The interactive viewport bridge, or nullptr while no scene is
    /// loaded (torn down between scene loads).  Starts/stops the
    /// ~2 Hz refinement-status poll and gates the pause/resume/
    /// restart buttons.  Safe to call repeatedly with a new pointer
    /// (e.g. on every scene load/clear).
    void setViewportBridge(ViewportBridge* bridge);

    void setLoadedFilePath(const QString& path);

    /// P1-2 fix (mirrors Mac's canUseSceneTransport): the panel whose
    /// isChatRenderOutstanding() feeds updateControlsEnabled(), so the
    /// pause/resume/restart cluster goes disabled while a chat-driven
    /// render owns the scene even though nothing routes through
    /// RenderEngine::stateChanged for that transition.  Borrowed;
    /// MainWindow owns both for the app's lifetime.  Call once; safe to
    /// call with nullptr (treated as "no chat render can be outstanding").
    void setChatPanel(ChatPanel* chatPanel);

signals:
    /// User clicked the Save pill.  MainWindow performs the actual
    /// round-trip save (it owns the SceneEditor refresh + error
    /// dialog plumbing shared with ViewportProperties' own Save
    /// button) — this bar has no Save-As affordance, matching the
    /// macOS TopBar.
    void saveClicked();

public slots:
    /// Phase 6.5 dirty-state mirror — connect to
    /// ViewportBridge::dirtyChanged.  Also settable directly (e.g.
    /// reset to false right after a fresh scene load).
    void setSceneEditsDirty(bool dirty);

    /// Mirrors the retired ControlsWidget's elapsed/remaining-time
    /// display — surfaced here as the readout's tooltip so the
    /// info isn't dropped by the redesign.
    void updateElapsedTime(double seconds);
    void updateRemainingTime(double seconds, bool hasEstimate);

    /// P1-2 fix: connected to ChatPanel::chatRenderOutstandingChanged --
    /// re-runs updateControlsEnabled() so the pause/resume/restart
    /// cluster reacts immediately to a chat-driven render starting or
    /// finishing, not just to the next ~2 Hz poll tick.
    void onChatRenderOutstandingChanged();

private slots:
    void onPauseResumeClicked();
    void onRestartClicked();
    void pollRefinementState();
    void onEngineStateChanged(int newState);
    void onEngineProgress(double fraction, const QString& title);
    /// P1-3 fix: RenderEngine::errorOccurred was previously surfaced
    /// only as a transient 5s status-bar message (MainWindow.cpp) --
    /// never in this persistent readout, so a failure that happened
    /// while the user wasn't looking at the status bar left no trace.
    /// Stores the message; updateReadout()/updateReadoutTooltip() check
    /// engine state == Error and short-circuit before the normal
    /// refinement-status computation.
    void onEngineError(const QString& message);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void updateSceneIdentity();
    void updateReadout();
    void updateReadoutTooltip();
    void updateIntegratorChip();
    void updateControlsEnabled();

    RenderEngine*   m_engine = nullptr;
    ViewportBridge* m_bridge = nullptr;
    ChatPanel*      m_chatPanel = nullptr;
    QTimer*         m_pollTimer = nullptr;

    // Left: scene identity ------------------------------------------
    TopBarLogoSwatch* m_logoSwatch = nullptr;
    QLabel* m_sceneFileLabel = nullptr;
    QLabel* m_dirtyDotLabel = nullptr;
    QLabel* m_dirtyTextLabel = nullptr;
    QString m_loadedFilePath;
    bool    m_dirty = false;

    // Center: render-status cluster -----------------------------------
    QToolButton* m_pauseResumeBtn = nullptr;
    QToolButton* m_restartBtn = nullptr;
    QLabel*      m_statusRowLabel = nullptr;
    QLabel*      m_statusTagLabel = nullptr;
    QWidget*     m_readoutContainer = nullptr;
    TopBarProgressStrip* m_progressStrip = nullptr;
    QLabel*      m_integratorChip = nullptr;
    // P2 fix: the hairline divider between the readout and the
    // integrator chip -- hidden alongside the chip (updateIntegratorChip)
    // when there's no resolved integrator to show, instead of always
    // rendering next to a misleading static "AUTO" placeholder.
    QFrame*      m_integratorSep = nullptr;

    // Right: save -------------------------------------------------------
    QPushButton* m_saveBtn = nullptr;

    // Mirrors RenderViewModel's refinement-status state (design brief A2).
    int          m_refinementPhase = -1;         // -1 no controller, 0 Idle .. 4 Paused
    unsigned int m_refinementScaleDivisor = 1;
    bool         m_isRefinementPaused = false;
    int          m_engineState = 0;              // RenderEngine::Idle
    double       m_productionProgress = 0.0;
    double       m_lastNonPausedFraction = 0.0;

    // Elapsed/remaining mirrors — surfaced via the readout's tooltip.
    double m_lastElapsed = 0.0;
    double m_lastRemaining = 0.0;
    bool   m_haveRemainingEstimate = false;

    // P1-3 fix: the last RenderEngine::errorOccurred message, shown as
    // the readout's text/label/tooltip whenever m_engineState == Error.
    // Not cleared on a state change away from Error -- it's simply
    // unread while the state check gating its use is false, and a
    // fresh errorOccurred always arrives together with the next Error
    // transition, so a stale string can never be shown.
    QString m_lastErrorMessage;

    // P2 fix: RenderEngine::progressUpdated's title param, previously
    // discarded by onEngineProgress.  Surfaced in the readout tooltip
    // (mirrors Mac's TopBar.swift progressTitle use) when there's no
    // higher-priority error message to show instead.
    QString m_progressTitle;
};

#endif // TOPBAR_H
