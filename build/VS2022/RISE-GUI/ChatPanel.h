//////////////////////////////////////////////////////////////////////
//
//  ChatPanel.h - Windows Qt LLM chat panel for live scene editing.
//
//  Thin UI/IO driver around RISE::Agent::AgentChatLoop.  The loop owns
//  transcript/tool-call state; this widget performs HTTPS requests and
//  executes model-requested tools through ViewportBridge::agentHandleLine.
//
//////////////////////////////////////////////////////////////////////

#ifndef CHATPANEL_H
#define CHATPANEL_H

#include <QWidget>
#include <QHash>
#include <QElapsedTimer>
#include <memory>
#include <vector>

#include "Agent/AgentChatCodecs.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTextEdit;
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
    void setSceneEditable(bool editable);
    bool isBusy() const { return m_busy; }

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

private slots:
    void sendMessage();
    void providerChanged(int index);
    void modelEditingFinished();
    void networkFinished();
    void retryLastRequest();

    // Eval-harness E1: the "Record chat trajectories" checkbox (default ON).
    void recordTrajectoriesToggled(bool on);

    // P1-2: fires on a ~250ms QTimer while a chat-driven `render` tool
    // call has an async job outstanding; each tick is a fast
    // render_wait(timeoutMs:0) poll-once through agentHandleLine.
    void pollOutstandingRender();

private:
    enum class Provider {
        OpenAI = 0,
        Anthropic = 1,
        Gemini = 2
    };

    Provider currentProvider() const;
    QString defaultModelFor(Provider provider) const;
    QString envKeyFor(Provider provider) const;
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

    //! Review-round P2 (E1): the scene path is the trajectory<->document
    //! correlator; MainWindow sets it at scene load, clears it at teardown.
    //! Takes effect at the NEXT startTrajectory (no mid-session restart).
    void setScenePath(const QString& path) { m_scenePath = path; }
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

    std::unique_ptr<RISE::Agent::AgentChatLoop> m_loop;
    ViewportBridge* m_bridge = nullptr;   // borrowed; owned by MainWindow
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;

    // Eval-harness E1: trajectory recording UI + timing.
    QCheckBox* m_recordTrajectoriesCheck = nullptr;
    bool m_recordTrajectories = true;
    QString m_scenePath;   // scene-identity for the session record (may be empty)
    QElapsedTimer m_httpTimer;   //!< started at post(), read at networkFinished()

    QComboBox* m_providerCombo = nullptr;
    QLineEdit* m_modelEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QTextEdit* m_transcript = nullptr;
    QLineEdit* m_inputEdit = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QPushButton* m_resetBtn = nullptr;
    QPushButton* m_retryBtn = nullptr;
    QLabel* m_statusLabel = nullptr;

    bool m_busy = false;
    bool m_stopRequested = false;
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
    Provider m_appliedProvider = Provider::OpenAI;
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
    // single-slot agent-render worker.
    quint64 m_outstandingRenderJobId = 0;
    // Lazily created on the first async render tool call; reused
    // (started/stopped) for every subsequent one.
    QTimer* m_renderPollTimer = nullptr;
};

#endif // CHATPANEL_H
