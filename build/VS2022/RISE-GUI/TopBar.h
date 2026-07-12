//////////////////////////////////////////////////////////////////////
//
//  TopBar.h - The workspace's persistent 44 pt top bar (design brief
//    "Top bar"): scene identity (left), refinement-status cluster
//    (center: restart + status readout + progress strip + integrator
//    chip), render transport + Cancel + Save (right).  The refinement
//    pause/resume button was removed from the center cluster -- see
//    the tombstone comment on onRestartClicked's declaration below.
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
class QGraphicsOpacityEffect;

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
    /// ~2 Hz refinement-status poll and gates the restart button.
    /// Safe to call repeatedly with a new pointer
    /// (e.g. on every scene load/clear).
    void setViewportBridge(ViewportBridge* bridge);

    void setLoadedFilePath(const QString& path);

    /// P1-2 fix (mirrors Mac's canUseSceneTransport): the panel whose
    /// isChatRenderOutstanding() feeds updateControlsEnabled(), so the
    /// restart control goes disabled while a chat-driven
    /// render owns the scene even though nothing routes through
    /// RenderEngine::stateChanged for that transition.  Borrowed;
    /// MainWindow owns both for the app's lifetime.  Call once; safe to
    /// call with nullptr (treated as "no chat render can be outstanding").
    void setChatPanel(ChatPanel* chatPanel);

signals:
    /// User clicked the save-state chip while it's in the suspended
    /// (amber "not saved") state -- a retry request.  MainWindow
    /// performs the actual round-trip save (it owns the SceneEditor
    /// refresh + error dialog plumbing shared with ViewportProperties'
    /// own Save button) — this bar has no Save-As affordance, matching
    /// the macOS TopBar.  Named `saveClicked` for back-compat with the
    /// existing MainWindow wiring; CST <-> scene-file live sync + auto-
    /// save (item 2) means edits now auto-save to disk on a debounce,
    /// so in practice this only fires from the suspended-state click.
    void saveClicked();

    /// The render-transport pill was clicked while idle (no render in
    /// flight) -- a request to start a production render.  MainWindow
    /// connects this to the SAME slot the Render menu action / Ctrl+R
    /// shortcut use (onRender), so the pre-render bookkeeping (stopping
    /// the viewport, cancelling an outstanding chat turn, advancing
    /// scene time) lives in exactly one place.  Pause/resume and Cancel,
    /// by contrast, are handled locally by this bar via the engine's
    /// setProductionRenderPaused / cancelRender accessors -- no signal
    /// needed for those (see onRenderTransportClicked / onCancelClicked).
    void renderTransportClicked();

public slots:
    /// Mirrors the Render menu's "&Render" action's enable predicate
    /// (MainWindow::updateMenuActionStates's `canRender`) -- pushed in
    /// from MainWindow rather than re-derived here so the two can never
    /// hand-copy-drift apart.  Only meaningful for the idle "Render"
    /// pill state; the Pause/Resume/Cancelling states have their own
    /// unconditional enable rules (see updateTransportButton).
    void setCanStartProductionRender(bool canStart);
    /// Phase 6.5 dirty-state mirror — connect to
    /// ViewportBridge::dirtyChanged.  Also settable directly (e.g.
    /// reset to false right after a fresh scene load).
    void setSceneEditsDirty(bool dirty);

    /// CST <-> scene-file live sync + auto-save (item 2): true once an
    /// auto-save attempt has been refused or I/O-failed (e.g. the save
    /// engine detects the file changed on disk externally).  Flips the
    /// save-state chip to the amber "not saved" state, which doubles
    /// as the one-click retry (see saveClicked() above).  Cleared by a
    /// manual save attempt (successful or not) and by a fresh scene
    /// load/bridge teardown.
    void setAutoSaveSuspended(bool suspended);

    /// Mirrors the retired ControlsWidget's elapsed/remaining-time
    /// display — surfaced here as the readout's tooltip so the
    /// info isn't dropped by the redesign.
    void updateElapsedTime(double seconds);
    void updateRemainingTime(double seconds, bool hasEstimate);

    /// P1-2 fix: connected to ChatPanel::chatRenderOutstandingChanged --
    /// re-runs updateControlsEnabled() so the restart control
    /// reacts immediately to a chat-driven render starting or
    /// finishing, not just to the next ~2 Hz poll tick.
    void onChatRenderOutstandingChanged();

private slots:
    // (The center-cluster refinement pause/resume button and its click
    // handler, onPauseResumeClicked, were removed by user request --
    // interactive refinement restarts on every edit anyway, so pausing
    // it was a niche control.  Production pause lives on the transport
    // pill below; onRestartClicked remains.  Mirrors TopBar.swift.)
    void onRestartClicked();
    /// Click handler for the render-transport pill (right side).  While
    /// Rendering, toggles production pause directly via the engine;
    /// while idle/SceneLoaded/Completed/Cancelled, click-time rechecks
    /// m_canStartProductionRender and emits renderTransportClicked();
    /// while Cancelling, the button is disabled so this is unreachable
    /// but still guards defensively.
    void onRenderTransportClicked();
    /// Click handler for the Cancel pill (right side, beside Pause/
    /// Resume while Rendering).  Calls RenderEngine::cancelRender()
    /// directly -- the exact same single-line body as MainWindow::
    /// onCancel() (wired to the Render menu's Cancel action / Ctrl+.)
    /// -- so there is exactly one cancel code path, not a duplicated
    /// one.  Works while paused too: the pause gate observes cancel.
    void onCancelClicked();
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
    /// CST <-> scene-file live sync + auto-save (item 2): recomputes
    /// the right-side save-state chip's visibility/text/color/enabled
    /// state from m_loadedFilePath / m_dirty / m_autoSaveSuspended.
    void updateSaveChip();
    /// Recomputes the render-transport pill's text/style/enabled state
    /// from m_engineState, the engine's isProductionRenderPaused(), and
    /// m_canStartProductionRender.  Called from updateControlsEnabled()
    /// (see that method's call sites) so the pill refreshes at every
    /// engine-state change, pause toggle, and chat/bridge transition --
    /// the same set of triggers the pause/restart buttons already use.
    void updateTransportButton();

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
    // CST <-> scene-file live sync + auto-save (item 2) — see
    // setAutoSaveSuspended's doc.
    bool    m_autoSaveSuspended = false;

    // Center: render-status cluster -----------------------------------
    // (m_pauseResumeBtn -- the refinement pause/resume tool button --
    // was removed by user request; see the tombstone comment in the
    // `private slots:` section above.)
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

    // Right: render transport (Render -> Pause -> Resume, + Cancel) -------
    // One slot that morphs with the production render's lifecycle -- see
    // updateTransportButton().  Placed before the save-state chip.
    QPushButton* m_transportBtn = nullptr;
    // Drives the Mac-mirrored 40% opacity on the disabled "Render" pill
    // (a plain QSS :disabled rule can't express the Mac's whole-pill
    // `.opacity(0.4)`, which dims background AND text together) --
    // installed once in the constructor, toggled by updateTransportButton.
    QGraphicsOpacityEffect* m_transportOpacity = nullptr;
    // Mirrors the Render menu action's `canRender` predicate -- pushed in
    // by MainWindow via setCanStartProductionRender() (see that slot's
    // doc); only consulted while idle (SceneLoaded/Completed/Cancelled).
    bool m_canStartProductionRender = false;
    // Cancel pill, shown beside m_transportBtn only while Rendering
    // (mirrors TopBar.swift's cancelPill).  Error-tinted outline; see
    // updateTransportButton() and onCancelClicked().
    QPushButton* m_cancelBtn = nullptr;

    // Right: save -------------------------------------------------------
    QPushButton* m_saveBtn = nullptr;

    // Mirrors RenderViewModel's refinement-status state (design brief A2).
    // (m_isRefinementPaused was removed alongside m_pauseResumeBtn -- its
    // only readers were that button's icon and click handler; the phase-4
    // "Paused" readout below still reads m_refinementPhase directly from
    // the poll, so no status-display capability was lost.)
    int          m_refinementPhase = -1;         // -1 no controller, 0 Idle .. 4 Paused
    unsigned int m_refinementScaleDivisor = 1;
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
