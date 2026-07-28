//////////////////////////////////////////////////////////////////////
//
//  TopBar.h - The workspace's persistent 44 pt top bar (design brief
//    "Top bar"): scene identity (left), refinement-status cluster
//    (center: restart + status readout + progress strip + integrator
//    chip), free-standing render-mode combo + X-ray chips (P1 fix,
//    2026-07-23 review -- their own bordered chips beside the status
//    cluster, not boxed inside it), render transport + Cancel + Save
//    (right).  The refinement pause/resume button was removed from the
//    center cluster -- see the tombstone comment on onRestartClicked's
//    declaration below.
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
class QComboBox;
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
    /// User clicked the save-state chip while it's in the active
    /// (dirty) "Save" state -- a save request.  MainWindow performs
    /// the actual round-trip save (it owns the SceneEditor refresh +
    /// error dialog plumbing shared with ViewportProperties' own Save
    /// button) — this bar has no Save-As affordance, matching the
    /// macOS TopBar.  Explicit-save-only (user decision 2026-07-12):
    /// UI edits never write the .RISEscene to disk automatically, so
    /// this is the ONLY way a save gets triggered from this bar.
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
    /// Explicit regional-final request from the dropdown adjacent to Render.
    void renderRegionClicked();

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
    /// User picked an item in the render-mode combo (P1, docs/gui/
    /// RENDER_MODES.md §5).  Looks up the picked item's wire name and
    /// calls ViewportBridge::setViewportRenderMode -- the set CAN fail
    /// (render-owns-scene, unknown mode, skeleton mode), so this always
    /// re-reads the effective mode afterward via refreshRenderModeCombo()
    /// rather than trusting the click.  Never re-entrant with the
    /// programmatic sync in refreshRenderModeCombo() -- that method
    /// wraps its setCurrentIndex in a QSignalBlocker, so this slot only
    /// ever fires from an actual user pick.
    void onRenderModeComboChanged(int index);
    /// User clicked the X-Ray toggle (docs/gui/RENDER_MODES.md "X-ray
    /// axis").  Same click-then-reread discipline as
    /// onRenderModeComboChanged: the set CAN fail (render-owns-scene,
    /// skeleton mode), so this always calls refreshRenderModeCombo()
    /// afterward to snap the button back to the effective value rather
    /// than trusting the click.  Connected to QToolButton::clicked (not
    /// toggled), so this never fires from refreshRenderModeCombo()'s own
    /// programmatic setChecked() -- matching m_renderModeCombo's
    /// QSignalBlocker-guarded sync not re-entering onRenderModeComboChanged.
    void onXrayButtonToggled(bool checked);
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
    /// LIVE THEME-SWITCH CONTRACT (Theme.h): calls the base implementation
    /// first, then restyleTheme() on QEvent::PaletteChange.
    void changeEvent(QEvent* e) override;

private:
    /// LIVE THEME-SWITCH CONTRACT (Theme.h) reference-pattern follower:
    /// re-applies every one of TopBar's token-dependent styling sites
    /// (static chrome stylesheets/palettes/icon tints, PLUS re-invoking
    /// the existing update*() methods whose bodies already recompute
    /// their styling from live Theme:: tokens -- see restyleTheme()'s
    /// own doc comment in TopBar.cpp for the full site inventory).
    /// Called once at the end of the constructor and again from
    /// changeEvent() on every QEvent::PaletteChange. Idempotent, creates
    /// no widgets.
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // See MainWindow.h for the full rationale; uniform across every
    // changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    void updateSceneIdentity();
    void updateReadout();
    void updateReadoutTooltip();
    void updateIntegratorChip();
    void updateControlsEnabled();
    /// Explicit-save-only (user decision 2026-07-12): recomputes the
    /// right-side save-state chip's visibility/text/color/enabled state
    /// from m_loadedFilePath / m_dirty.  Dirty -> an ACTIVE bordered
    /// "Save" pill; clean -> a passive "✓ saved" label; no loaded path
    /// -> hidden.
    void updateSaveChip();
    /// Recomputes the render-transport pill's text/style/enabled state
    /// from m_engineState, the engine's isProductionRenderPaused(), and
    /// m_canStartProductionRender.  Called from updateControlsEnabled()
    /// (see that method's call sites) so the pill refreshes at every
    /// engine-state change, pause toggle, and chat/bridge transition --
    /// the same set of triggers the pause/restart buttons already use.
    void updateTransportButton();
    void updateRegionRenderButton();
    /// P1 (docs/gui/RENDER_MODES.md §5): re-read the ACTIVE mode from the
    /// bridge and resync m_renderModeCombo's current index (QSignalBlocker-
    /// guarded so the programmatic sync never re-fires
    /// onRenderModeComboChanged), plus its enabled state.  The combo's
    /// item list itself is populated ONCE at construction (the mode set
    /// is fixed-registry, docs/gui/RENDER_MODES.md decision 2) --
    /// this only ever moves the current-index pointer.  Called from
    /// updateControlsEnabled() (its single call site), which is itself
    /// called from setViewportBridge (scene load/reload/close -- directly,
    /// AND indirectly via that method's own pollRefinementState() call
    /// while attaching a bridge), every render-owns-scene transition, AND
    /// the 500ms pollRefinementState tick -- the LAST one is the safety
    /// net for resets this bridge has no dedicated signal for
    /// (SceneEditController resets to "preview" on every whole-scene
    /// rebind, including scene_variant switches and CST full re-derives
    /// that reuse the SAME ViewportBridge/controller instance -- see
    /// SceneEditController::RebindEditorToJob's doc).
    ///
    /// ALSO resyncs m_xrayBtn's enabled + checked state (docs/gui/
    /// RENDER_MODES.md "X-ray axis") in the same pass, since it shares
    /// the identical render-owns-scene gate plus the identical
    /// controller-side reset-on-rebind hazard the mode combo already
    /// guards against -- one poll-driven self-heal for both controls
    /// rather than a second bespoke one.  P2a review fix: m_xrayBtn is
    /// ADDITIONALLY disabled while the ACTIVE mode is a BeautyVariant row
    /// (deep_reflect/direct) -- those drive a separate ephemeral pipeline
    /// that never reads the x-ray flag, so the toggle would silently no-op
    /// (registry-driven via each combo item's stashed isVariant flag,
    /// Qt::UserRole + 1).
    void refreshRenderModeCombo();

    RenderEngine*   m_engine = nullptr;
    ViewportBridge* m_bridge = nullptr;
    ChatPanel*      m_chatPanel = nullptr;
    QTimer*         m_pollTimer = nullptr;

    // Left: scene identity ------------------------------------------
    TopBarLogoSwatch* m_logoSwatch = nullptr;
    // Promoted from a ctor-local to a member so restyleTheme() can
    // re-apply their baked Theme:: colors on a live theme switch --
    // neither is ever restyled by any other code path.
    QLabel* m_wordmarkLabel = nullptr;
    QFrame* m_identitySep = nullptr;
    QLabel* m_sceneFileLabel = nullptr;
    QLabel* m_dirtyDotLabel = nullptr;
    QLabel* m_dirtyTextLabel = nullptr;
    QString m_loadedFilePath;
    bool    m_dirty = false;

    // Center: render-status cluster -----------------------------------
    // (m_pauseResumeBtn -- the refinement pause/resume tool button --
    // was removed by user request; see the tombstone comment in the
    // `private slots:` section above.)
    // Promoted from a ctor-local to a member so restyleTheme() can
    // re-apply its objectName-scoped QSS (bgWell/borderLight) on a live
    // theme switch.
    QWidget*     m_cluster = nullptr;
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

    // P1 (docs/gui/RENDER_MODES.md §5): compact viewport render-mode
    // combo -- "the viewport chrome ... Windows: TopBar combobox" per
    // §5's GUI bullet.  P1 fix (2026-07-23 review): a FREE-STANDING chip
    // parented to the top bar itself, right of the #topBarCluster status
    // card -- NOT inside that card's own layout.  Mac's TopBar.swift
    // renderStatusCluster boxes only restart + readout + integrator
    // chip; the mode combo and X-ray toggle are separate viewport-chrome
    // chips there too (ContentView.swift:574-629).  Populated ONCE at
    // construction from the controller-independent registry
    // (ViewportBridge::viewportRenderModes, a pure/static function); its
    // enabled state and current index are resynced by
    // refreshRenderModeCombo() at every scene load/reload/close and on
    // the 500ms refinement poll (session-only, no QSettings persistence
    // -- every fresh scene load reads back "preview" from the
    // controller's own reset, never assumed locally).
    QComboBox*   m_renderModeCombo = nullptr;

    // P1 (docs/gui/RENDER_MODES.md "X-ray axis"): compact checkable
    // toggle, right of the render-mode combo -- an orthogonal boolean
    // that composes with EVERY render mode, including the shaded
    // preview (default ON; user decision 2026-07-17), not just the four
    // data modes (normals/depth/facets/wireframe).  Same free-standing-
    // chip placement as m_renderModeCombo above (P1 fix, 2026-07-23
    // review) -- its own bordered chip in the top bar's main row, not
    // boxed inside #topBarCluster.  Enabled state and checked state are
    // entirely owned by refreshRenderModeCombo() (same treatment as
    // m_renderModeCombo above); no separate construction-time item list
    // since it's a single toggle, not a registry-backed combo.
    QToolButton* m_xrayBtn = nullptr;

    // Right: render transport (Render -> Pause -> Resume, + Cancel) -------
    // One slot that morphs with the production render's lifecycle -- see
    // updateTransportButton().  Placed before the save-state chip.
    QPushButton* m_transportBtn = nullptr;
    // Drives the Mac-mirrored 40% opacity on the disabled "Render" pill
    // (a plain QSS :disabled rule can't express the Mac's whole-pill
    // `.opacity(0.4)`, which dims background AND text together) --
    // installed once in the constructor, toggled by updateTransportButton.
    QGraphicsOpacityEffect* m_transportOpacity = nullptr;
    QToolButton* m_regionRenderBtn = nullptr;
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
