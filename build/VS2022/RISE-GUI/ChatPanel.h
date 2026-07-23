//////////////////////////////////////////////////////////////////////
//
//  ChatPanel.h - Windows Qt LLM chat panel for live scene editing.
//
//  Thin UI/IO driver around RISE::Agent::AgentChatLoop.  The loop owns
//  transcript/tool-call state; this widget performs HTTPS requests and
//  executes model-requested tools through ViewportBridge::agentHandleToolCall
//  (autonomy-routed); agentHandleLine is administrative-only (proposals,
//  render submit/poll, skill index).
//
//////////////////////////////////////////////////////////////////////

#ifndef CHATPANEL_H
#define CHATPANEL_H

#include <QWidget>
#include <QHash>
#include <QMap>
#include <QDateTime>
#include <QVector>
#include <QElapsedTimer>
#include <QPointer>
#include <memory>
#include <vector>

#include "Agent/AgentChatCodecs.h"
#include "ProposalCard.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QScrollArea;
class QTextEdit;
class QToolButton;
class QVBoxLayout;
class QTimer;
class ViewportBridge;

namespace RISE {
    namespace Agent {
        class AgentChatLoop;
    }
}

class ChatPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ChatPanel(QWidget* parent = nullptr);
    ~ChatPanel() override;

    void setViewportBridge(ViewportBridge* bridge);

    //! Review-round P2 (E1): the scene path is the trajectory<->document
    //! correlator; MainWindow sets it at scene load, clears it at teardown.
    //! Takes effect at the NEXT startTrajectory (no mid-session restart).
    //! PUBLIC: called cross-class from MainWindow, like setViewportBridge
    //! (the closing verifier caught the original private placement -- a
    //! certain MSVC C2248 on the next Windows pass).
    void setScenePath(const QString& path) { m_scenePath = path; }
    void setSceneEditable(bool editable);
    bool isBusy() const { return m_busy; }

    // ---- Agent autonomy selector (2026-07 GUI composer chips) -------
    // Mirrors the macOS ChatViewModel's autonomyLevel / setAutonomyLevel
    // plus RISEViewportBridge's RISEAgentAutonomyLevel.  A LOCAL enum
    // (not `ViewportBridge::AgentAutonomyLevel` directly) so this header
    // doesn't need to pull in ViewportBridge.h -- ChatPanel.h only
    // forward-declares `class ViewportBridge` — matching every other
    // cross-widget type boundary in this class; numeric values are
    // identical to ViewportBridge::AgentAutonomyLevel and cast 1:1 at
    // the boundary in setAutonomyLevel()'s .cpp implementation.
    enum class AutonomyLevel : int {
        Read    = 0,
        Propose = 1,
        Apply   = 2
    };

    /// The composer's current autonomy level for the CHAT AGENT's OWN
    /// tool calls (routed through ViewportBridge::agentHandleToolCall —
    /// see that method's doc for the exact per-level verb behaviour).
    AutonomyLevel autonomyLevel() const { return m_autonomyLevel; }

    /// Change the composer's autonomy level: persist it to QSettings
    /// ("agentAutonomyLevel") and push it onto the live bridge (if any)
    /// immediately, so an in-flight session reflects the new choice on
    /// its very next tool call.  Also refreshes the three chip buttons'
    /// active/inactive styling.
    void setAutonomyLevel(AutonomyLevel level);

    // P1-2 fix (mirrors Mac's canUseSceneTransport / TopBar.swift:103-113):
    // true while a chat-driven `render` tool call has an async job
    // outstanding on the controller's single-slot agent-render worker.
    // While true, the scene is not safe for any OTHER scene-transport
    // action (undo/redo, refinement pause/resume/restart, proposal
    // Apply/Reject) to touch concurrently -- consulted by
    // MainWindow::updateMenuActionStates and TopBar::updateControlsEnabled
    // via the chatRenderOutstandingChanged() signal below.
    bool isChatRenderOutstanding() const { return m_outstandingRenderJobId != 0; }

    // ---- Start screen readiness (docs/gui/START_SCREEN.md §6) --------
    // Reuses the EXACT gate sendMessage() checks before allowing a send
    // (providerRequiresApiKey(m_appliedProvider) && key empty) rather than
    // inventing a second source of truth -- a keyless provider (Local/
    // Ollama) is always configured; every other provider needs a
    // resolvable key (Keychain-equivalent: in-memory/env-var on this
    // platform).  Reads the APPLIED provider/key (not necessarily what
    // the combo is mid-editing to -- see applyProviderChangeWithConfirmation),
    // matching what an actual send would do right now.
    bool agentConfigured() const;

    /// The applied provider's display label ("OpenAI", "Anthropic", ...),
    /// for the start screen's provider chip.  Empty if the combo hasn't
    /// been built yet (never true after construction).
    QString currentProviderDisplayName() const;

    /// Start-screen create path (spec §7): stash a prompt to be submitted
    /// EXACTLY ONCE, the next time setViewportBridge() attaches a fresh
    /// (non-null) bridge -- mirrors macOS ChatViewModel.pendingFirstPrompt,
    /// consumed at the tail of sceneOpened.  MainWindow calls this BEFORE
    /// loading the starter scene so a slow load can neither drop nor
    /// double-send it.  Passing an empty string clears a stale stash (used
    /// on a load failure, since setViewportBridge(non-null) never runs
    /// then).
    void setPendingFirstPrompt(const QString& prompt) { m_pendingFirstPrompt = prompt; }

public slots:
    void requestStop();
    void resetConversation();

    // P2-4: called at the TOP of MainWindow::onRender / onRenderAnimation,
    // BEFORE the production render is kicked off.  Cancels any in-flight
    // turn (including an outstanding async chat-render job) so a tool
    // call can never resume against Scene state the production rasterizer
    // is reading off-main.  Mirrors the Mac 80d7e0f8 gate -- keep the call
    // sites even though onStateChanged's setSceneEditable(false) also
    // disables this panel, since that disable only lands on the NEXT
    // event-loop turn, which is too late for a turn already suspended in
    // an HTTP await or a render_wait poll.
    void productionRenderStarting();

signals:
    // P1-2 fix: fired whenever isChatRenderOutstanding() transitions
    // (both directions -- job submitted, and job resolved/cancelled).
    // MainWindow relays this into updateMenuActionStates() (undo/redo,
    // pause/resume/restart) and TopBar's own control-enable refresh, so
    // neither surface stays stale across a chat-driven render that runs
    // entirely on this panel's async poll timer rather than through the
    // usual RenderEngine::stateChanged path.
    void chatRenderOutstandingChanged();

    /// Start screen readiness (spec §6): fired whenever agentConfigured()
    /// may have changed (provider applied, API-key field edited) so the
    /// Create column can update live without polling.
    void agentConfiguredChanged();

private slots:
    void sendMessage();
    void providerChanged(int index);
    void modelEditingFinished();
    void networkFinished();
    void retryLastRequest();

    // Eval-harness E1: the "Record chat trajectories" checkbox (default ON).
    void recordTrajectoriesToggled(bool on);

    // GUI stage 3 (Windows parity, chat-display enrichment): the
    // "Detailed transcript" checkbox (default OFF) -- mirrors the Mac
    // ChatSettingsView toggle.  See detailedTranscriptToggled()'s .cpp
    // doc for the persistence + re-render contract.
    void detailedTranscriptToggled(bool on);

    // P1-2: fires on a ~250ms QTimer while a chat-driven `render` tool
    // call has an async job outstanding; each tick is a fast
    // render_wait(timeoutMs:0) poll-once through agentHandleLine.
    void pollOutstandingRender();

    // Secure-MCP slice 5c (Windows parity, RISE UI redesign): poll
    // list_proposals over the same in-process agentHandleLine transport
    // the tool-call loop uses, and route Apply/Reject/Undo clicks from
    // the resulting ProposalCard widgets.  Mirrors the macOS
    // ChatViewModel.refreshProposals / resolveProposal.
    void refreshProposals();
    void onProposalApplyClicked(quint64 proposalId);
    void onProposalRejectClicked(quint64 proposalId);
    void onProposalUndoClicked();

private:
    enum class Provider {
        OpenAI = 0,
        Anthropic = 1,
        Gemini = 2,
        XAI = 3,
        Local = 4
    };

    Provider currentProvider() const;
    QString defaultModelFor(Provider provider) const;
    QString envKeyFor(Provider provider) const;

    // ---- Agent autonomy selector (2026-07 GUI composer chips) -------
    QPushButton* makeAutonomyChip(const QString& title, AutonomyLevel level, const QString& help);
    /// Refreshes the three chips' active/inactive styling from
    /// m_autonomyLevel.  Called by setAutonomyLevel() and once at
    /// construction.
    void updateAutonomyChipsStyle();

    // False only for Provider::Local (keyless-by-design local/Ollama-style
    // server -- OpenAIChatCodec::Config::requiresAuth=false).  Gates the
    // "Enter an API key before sending" prompt in sendMessage() and the
    // key-field placeholder in applyProviderToLoop().
    static bool providerRequiresApiKey(Provider provider);
    // Provider::Local only: the endpoint shown in the key field's
    // placeholder in place of a key prompt.  Mirrors MakeCodec's
    // RISE_LOCAL_LLM_BASE_URL override / Ollama-default fallback
    // (src/Library/Agent/AgentChatLoop.cpp) for DISPLAY purposes only --
    // there is no bridge accessor onto the loop's resolved codec config.
    static QString localResolvedEndpoint();
    void applyProviderToLoop(bool resetModelToDefault);
    // P1-3: shared no-op / confirm-then-apply gate for providerChanged and
    // modelEditingFinished (both used to call applyProviderToLoop
    // unconditionally, silently resetting the transcript on every stray
    // focus-out).
    void applyProviderChangeWithConfirmation(bool resetModelToDefault);
    void revertProviderModelWidgets();
    void refreshTranscript(const QString& statusLine = QString());
    void updateButtonStates();
    void runNextStep();
    void finishBusy(const QString& statusLine = QString());
    void fetchSkillIndex();
    // Eval-harness E1: (re)attach the per-session trajectory file for the
    // current scene, or detach when recording is off / no scene is bound.
    void startTrajectory();
    // The resolved trajectory directory: RISE_TRAJECTORY_DIR verbatim when
    // set + non-empty, else QStandardPaths::AppDataLocation + "/trajectories/gui"
    // (a deterministic, per-user-writable location -- QDir::currentPath()
    // is nondeterministic for a GUI app and unwritable under Program Files).
    QString trajectoryDirectory() const;
    QString renderSkillIndex(const QString& rpcResponse) const;
    QString cancelledToolResultJson(int rpcId, const QString& message) const;
    void clearErrorAffordances();
    void handleProviderError(const RISE::Agent::ChatStepResult& step);

    // P1-2: the tool-call loop's state machine.  networkFinished() used to
    // run a single synchronous `for` loop over step.toolCalls; that loop
    // is now split across processNextToolCall() (one call at a time,
    // still synchronous for non-render verbs) with a `render` call
    // suspending into startAsyncRenderToolCall()/pollOutstandingRender()
    // and resuming back into processNextToolCall() on completion --
    // mirroring how networkFinished() already resumes runNextStep()
    // across the HTTP boundary.
    void processNextToolCall();
    void startAsyncRenderToolCall(const RISE::Agent::ChatToolCall& call,
                                   const std::string& submitLine);
    void cancelOutstandingRender();
    void drainPendingToolCallsAsCancelled();
    void cancelActiveTurn(const QString& statusLine);

    // Rebuild the scrollable transcript from m_loop's transcript entries,
    // classifying each by Role into a bubble (User), plain narration
    // text (Assistant), or a trace chip (ToolResults) -- mirrors the
    // macOS ChatTranscriptRow.  The busy/"Thinking..."/error status line
    // stays a separate small label below the transcript (m_statusLabel),
    // matching the previous QTextEdit-based layout's two-area split.
    void rebuildTranscriptWidgets();
    void clearLayout(QVBoxLayout* layout);

    // GUI stage 3 (Windows parity, chat-display enrichment) -----------
    // Row builders shared by rebuildTranscriptWidgets() (the full,
    // authoritative rebuild from m_loop's transcript) and
    // processNextToolCall()'s live dispatch-time row (an ephemeral
    // widget inserted directly into the already-built layout, ahead of
    // the trailing stretch, until the next full rebuild supersedes it).

    // A collapsed-by-default (or, when `expandedByDefault`, pre-
    // expanded) disclosure row for one turn's reasoning text: a flat,
    // checkable QToolButton chevron reading "Thinking (N words)",
    // toggling a hidden word-wrapped, selectable QLabel with the full
    // text.  Parented to m_transcriptContent; the caller owns nothing
    // (Qt parent/child cleans it up on the next clearLayout()).
    QWidget* buildThinkingRow(const QString& reasoningText, bool expandedByDefault);

    // One tool-call "trace chip": a checkmark + `headerText`, hairline-
    // bordered like the pre-existing chip, plus a chevron toggling a
    // hidden read-only QTextEdit body (`detailText`) -- collapsed by
    // default (`startExpanded` exists for interface symmetry with
    // buildThinkingRow; every call site below passes false, matching
    // the "tool detail always starts collapsed" contract).  When
    // `hasDetail` is false the chevron is hidden/disabled and the row
    // reads exactly like the plain pre-feature chip -- the dispatch-
    // time state, before a result exists to show.  The three optional
    // out-params let a caller (processNextToolCall's dispatch site)
    // retain pointers for a later in-place update; pass nullptr when
    // the row will never be updated (the full-rebuild path, where the
    // whole ChatToolDisplaySummary is already known up front).
    QWidget* buildToolRow(const QString& headerText, const QString& detailText,
                          bool hasDetail, bool startExpanded,
                          QLabel** outLabel = nullptr, QTextEdit** outDetailEdit = nullptr,
                          QToolButton** outChevron = nullptr);

    // Update the live dispatch-time row (m_pendingToolRow*, set by
    // processNextToolCall's buildToolRow call) now that `call`'s result
    // line has returned: relabels it "-> name  <outcome>" via
    // AgentChatLoop::ToolOutcomeLineForDisplay, fills in the args+result
    // detail, and reveals the chevron.  QPointer-guarded (see the .cpp
    // doc) so a transcript clear that races an in-flight async render
    // tool call is a safe no-op rather than a dangling-pointer write.
    void updatePendingToolRow(const RISE::Agent::ChatToolCall& call,
                              const std::string& responseLine);

    // Secure-MCP slice 5c (Windows parity): rebuild the proposals
    // column from a freshly-parsed list_proposals response.
    void rebuildProposalsUI(const QVector<ProposalEntry>& proposals);
    // Re-applies the current m_sceneEditable gate to every live
    // ProposalCard's Apply/Reject buttons without waiting for the next
    // poll tick -- called from setSceneEditable() so the panel reacts
    // immediately to a production render starting/ending, matching the
    // Mac card's live `applyRejectDisabled` binding.
    void updateProposalCardsEnabled();

    // P1-2 fix: single source of truth for m_sceneEditable, which is now
    // derived from TWO independent inputs -- the externally-supplied
    // production-render gate (m_sceneEditableExternal, set only by
    // setSceneEditable) AND this panel's own isChatRenderOutstanding().
    // setSceneEditable() only fires when MainWindow's render state
    // changes; it never re-runs merely because m_outstandingRenderJobId
    // flips, so setOutstandingRenderJobId() below calls back into this
    // whenever that happens too -- otherwise Apply/Reject/undo/etc.
    // would stay stale for the lifetime of a chat-driven render.
    void recomputeSceneEditable();
    // Single mutator for m_outstandingRenderJobId: recomputes
    // m_sceneEditable and emits chatRenderOutstandingChanged() exactly
    // when the outstanding/not-outstanding state actually flips, so no
    // call site can update the job id without also refreshing the
    // dependent UI (a bare assignment previously left this out at three
    // of four sites).
    void setOutstandingRenderJobId(quint64 jobId);

    std::unique_ptr<RISE::Agent::AgentChatLoop> m_loop;
    ViewportBridge* m_bridge = nullptr;   // borrowed; owned by MainWindow
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;

    // Eval-harness E1: trajectory recording UI + timing.
    QCheckBox* m_recordTrajectoriesCheck = nullptr;
    bool m_recordTrajectories = true;
    QString m_scenePath;   // scene-identity for the session record (may be empty)
    QElapsedTimer m_httpTimer;   //!< started at post(), read at networkFinished()

    // GUI stage 3 (Windows parity, chat-display enrichment): the
    // transcript verbosity toggle -- QSettings key "agentChat/detailedTranscript",
    // default false.  Read by rebuildTranscriptWidgets() to seed each
    // thinking row's initial expanded state; see detailedTranscriptToggled()'s
    // .cpp doc for why toggling it re-seeds every row currently on
    // screen (not just future ones, unlike the Mac panel).
    QCheckBox* m_detailedTranscriptCheck = nullptr;
    bool m_detailedTranscript = false;

    // GUI stage 3: the live tool-dispatch row's widgets, set by
    // processNextToolCall() at dispatch time and consumed (then reset
    // to null) by updatePendingToolRow() once that call's result lands.
    // QPointer, not raw pointers -- see updatePendingToolRow's header
    // doc for the lifetime story. At most one tool call is ever
    // in-flight at a time (processNextToolCall drains m_pendingToolCalls
    // one at a time, synchronously except for the `render` verb's async
    // submit/poll suspension), so a single set suffices -- no per-call
    // container needed.
    QPointer<QLabel>      m_pendingToolRowLabel;
    QPointer<QTextEdit>   m_pendingToolRowDetail;
    QPointer<QToolButton> m_pendingToolRowChevron;

    QComboBox* m_providerCombo = nullptr;
    QLineEdit* m_modelEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;

    // ---- Agent autonomy selector (2026-07 GUI composer chips) -------
    // Default Apply -- matches today's actual behaviour byte-for-byte
    // (see setAutonomyLevel's .cpp doc for why Propose is NOT the
    // default).  Overwritten from QSettings ("agentAutonomyLevel") in
    // the constructor if a valid persisted value exists.
    AutonomyLevel m_autonomyLevel = AutonomyLevel::Apply;
    QPushButton*  m_autonomyReadBtn    = nullptr;
    QPushButton*  m_autonomyProposeBtn = nullptr;
    QPushButton*  m_autonomyApplyBtn   = nullptr;

    // Transcript -- restyled (RISE UI redesign) from a single QTextEdit
    // into a scrollable column of per-entry widgets (bubbles / plain
    // text / trace chips) so each row can carry its own alignment,
    // background, and font per the design comp.  m_transcriptLayout is
    // torn down and rebuilt wholesale on every refreshTranscript() call,
    // same rebuild granularity the old setPlainText() had.
    QScrollArea* m_transcriptScroll = nullptr;
    QWidget*     m_transcriptContent = nullptr;
    QVBoxLayout* m_transcriptLayout = nullptr;

    // Secure-MCP slice 5c (Windows parity, RISE UI redesign): the
    // pending/recently-resolved proposals column, shown above the
    // transcript.  Polled on a 1s QTimer via the same in-process
    // agentHandleLine transport the tool-call loop uses.
    QWidget*     m_proposalsContainer = nullptr;
    QVBoxLayout* m_proposalsLayout = nullptr;
    QTimer*      m_proposalsPollTimer = nullptr;
    QVector<ProposalCard*> m_currentProposalCards;   // children of m_proposalsLayout; not separately owned
    // proposalId -> the moment it was first observed leaving "pending" --
    // drives the "show briefly, then drop" behaviour for resolved
    // entries, mirroring ChatViewModel.resolvedProposalObservedAt.
    QMap<quint64, QDateTime> m_resolvedProposalObservedAt;
    static constexpr double kResolvedProposalLingerSeconds = 4.0;

    QLineEdit* m_inputEdit = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QPushButton* m_resetBtn = nullptr;
    QPushButton* m_retryBtn = nullptr;
    QLabel* m_statusLabel = nullptr;

    bool m_busy = false;
    bool m_stopRequested = false;
    // The EXTERNALLY-supplied half of the editable gate -- set only by
    // setSceneEditable(), which MainWindow drives off production-render
    // state.  m_sceneEditable (below) is the effective gate every other
    // member reads: m_sceneEditableExternal && !isChatRenderOutstanding(),
    // recomputed by recomputeSceneEditable() whenever either input
    // changes.
    bool m_sceneEditableExternal = false;
    bool m_sceneEditable = false;
    int m_nextRpcId = 1;

    // P1-1: per-provider in-memory API key storage (never persisted to
    // disk).  Keyed by static_cast<int>(Provider).  On a provider switch,
    // applyProviderToLoop() stashes the field's current text under the
    // OLD provider's slot before repopulating the field from the NEW
    // provider's slot (falling back to its env var) -- a key typed for
    // one provider must never be sent under another provider's auth
    // header.
    QHash<int, QString> m_keysByProvider;
    // The provider/model the loop is CURRENTLY configured for (as of the
    // last applyProviderToLoop() call) -- distinct from whatever the combo
    // / model field widgets are showing mid-edit.  Used by
    // applyProviderChangeWithConfirmation() to detect a genuine change
    // (vs. a stray focus-out that didn't alter anything) and as the
    // "old provider" key when stashing.
    Provider m_appliedProvider = Provider::Local;   // default: keyless local (opencoder), user decision 2026-07-16
    QString m_appliedModel;

    // P2-5: set on an Http-kind (or transport-level) ProviderError; lets
    // the user re-issue the exact same conversation without typing a new
    // message.  Cleared on the next send, retry, reset, or provider/model
    // switch.
    bool m_retryAvailable = false;

    // P1-2: remaining tool calls of the CURRENT assistant turn still to be
    // dispatched -- processNextToolCall() pops the front one at a time.
    // Non-empty only while networkFinished()'s ToolCalls branch is being
    // drained (across a possible render suspension).
    std::vector<RISE::Agent::ChatToolCall> m_pendingToolCalls;

    // The `render` tool call currently suspended in the async submit/poll
    // state machine (valid only while m_activeRenderPending is true).
    RISE::Agent::ChatToolCall m_activeRenderCall;
    bool m_activeRenderPending = false;
    // 0 = no async chat-render job outstanding on the controller's
    // single-slot agent-render worker.  Never assign this directly --
    // go through setOutstandingRenderJobId() so isChatRenderOutstanding()
    // consumers (recomputeSceneEditable, MainWindow, TopBar) stay
    // synchronized with every transition.
    quint64 m_outstandingRenderJobId = 0;
    // Lazily created on the first async render tool call; reused
    // (started/stopped) for every subsequent one.
    QTimer* m_renderPollTimer = nullptr;

    // Start-screen create path: see setPendingFirstPrompt()'s doc.  Valid
    // only between a stash and its consumption in setViewportBridge();
    // empty the rest of the time.
    QString m_pendingFirstPrompt;
};

#endif // CHATPANEL_H
