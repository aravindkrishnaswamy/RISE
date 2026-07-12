//////////////////////////////////////////////////////////////////////
//
//  ChatPanel.cpp - Windows Qt chat panel implementation.
//
//////////////////////////////////////////////////////////////////////

#include "ChatPanel.h"

#include "ViewportBridge.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <ctime>

#include "Agent/AgentChatLoop.h"
#include "Agent/ChatTrajectory.h"

using RISE::Agent::AgentChatLoop;
using RISE::Agent::AnthropicChatCodec;
using RISE::Agent::ChatErrorKind;
using RISE::Agent::ChatProvider;
using RISE::Agent::ChatStepResult;
using RISE::Agent::ChatToolCall;
using RISE::Agent::GeminiChatCodec;
using RISE::Agent::OpenAIChatCodec;

namespace
{
    QString toQString(const std::string& s)
    {
        return QString::fromUtf8(s.c_str(), static_cast<int>(s.size()));
    }

    std::string toStdString(const QString& s)
    {
        const QByteArray utf8 = s.toUtf8();
        return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
    }

    QString roleLabel(RISE::Agent::ChatTranscriptEntry::Role role)
    {
        switch (role) {
        case RISE::Agent::ChatTranscriptEntry::Role::User:        return "You";
        case RISE::Agent::ChatTranscriptEntry::Role::Assistant:   return "Assistant";
        case RISE::Agent::ChatTranscriptEntry::Role::ToolResults: return "Tools";
        }
        return "Chat";
    }

    // Build one JSON-RPC 2.0 request line (used for the driver-internal
    // render_wait / render_cancel polls, which are never seen by the LLM).
    QString buildJsonRpcLine(const QString& method, const QJsonObject& params)
    {
        const QJsonObject root{
            { "jsonrpc", "2.0" },
            { "id", 0 },
            { "method", method },
            { "params", params }
        };
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    // P1-2: parse a JSON-RPC request line and set params.async = true,
    // re-serializing.  Returns an empty string on any parse failure; the
    // caller refuses the tool call honestly (it must never fall back to
    // the synchronous line, which would block the GUI thread).
    QString injectAsyncTrue(const QString& jsonRpcLine)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(jsonRpcLine.toUtf8());
        if (!doc.isObject()) return QString();
        QJsonObject obj = doc.object();
        QJsonObject params = obj.value("params").toObject();
        params["async"] = true;
        obj["params"] = params;
        return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    }

    // Wrap a `result` object as a plain JSON-RPC success envelope.  The
    // `id` is never inspected by AgentChatCodecs (branches only on
    // `error` vs `result`), so a fixed placeholder is honest here.
    QString makeSyntheticResponseLine(const QJsonObject& result)
    {
        const QJsonObject root{
            { "jsonrpc", "2.0" },
            { "id", 0 },
            { "result", result }
        };
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    // Build a synthetic render-shaped result (the same field set
    // RenderResultJson emits in AgentRpc.cpp) for the honest-failure
    // paths in the async render path, where only ok/message are known.
    QString makeSyntheticRenderResultLine(bool ok, const QString& message)
    {
        const QJsonObject result{
            { "ok", ok },
            { "width", 0 },
            { "height", 0 },
            { "meanR", 0.0 },
            { "meanG", 0.0 },
            { "meanB", 0.0 },
            { "integrator", QString() },
            { "previewWidth", 0 },
            { "previewHeight", 0 },
            { "cameraOverridden", false },
            { "message", message },
            { "renderJobId", 0 }
        };
        return makeSyntheticResponseLine(result);
    }
}

ChatPanel::ChatPanel(QWidget* parent)
    : QWidget(parent)
    , m_loop(new AgentChatLoop())
{
    m_network = new QNetworkAccessManager(this);
    // P2-6: mirror the Mac driver's URLRequest.timeoutInterval = 300 --
    // without this a stalled connection blocks the turn (and the render-
    // wait poll queued behind it) indefinitely.
    m_network->setTransferTimeout(300000);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 8);
    outer->setSpacing(6);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("Chat");
    title->setStyleSheet("font-weight: bold; color: gray;");
    header->addWidget(title);
    header->addStretch();
    m_retryBtn = new QPushButton("Retry");
    m_retryBtn->setToolTip("Retry the last request (after an HTTP/network error)");
    m_retryBtn->setEnabled(false);
    header->addWidget(m_retryBtn);
    m_resetBtn = new QPushButton("Reset");
    m_resetBtn->setToolTip("Clear the conversation");
    header->addWidget(m_resetBtn);
    outer->addLayout(header);

    auto* providerRow = new QHBoxLayout();
    m_providerCombo = new QComboBox();
    m_providerCombo->addItem("OpenAI");
    m_providerCombo->addItem("Anthropic");
    m_providerCombo->addItem("Gemini");
    m_providerCombo->addItem("Grok (xAI)");
    m_providerCombo->addItem("Local (Ollama)");
    m_modelEdit = new QLineEdit(defaultModelFor(Provider::OpenAI));
    m_modelEdit->setPlaceholderText("model");
    providerRow->addWidget(m_providerCombo);
    providerRow->addWidget(m_modelEdit, 1);
    outer->addLayout(providerRow);

    m_apiKeyEdit = new QLineEdit();
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText("API key");
    m_apiKeyEdit->setText(envKeyFor(Provider::OpenAI));
    outer->addWidget(m_apiKeyEdit);

    m_transcript = new QTextEdit();
    m_transcript->setReadOnly(true);
    m_transcript->setMinimumHeight(160);
    m_transcript->setPlaceholderText("Describe a scene change, for example: make the orange objects red.");
    outer->addWidget(m_transcript);

    auto* inputRow = new QHBoxLayout();
    m_inputEdit = new QLineEdit();
    m_inputEdit->setPlaceholderText("Ask for a scene change...");
    m_sendBtn = new QPushButton("Send");
    m_stopBtn = new QPushButton("Stop");
    inputRow->addWidget(m_inputEdit, 1);
    inputRow->addWidget(m_sendBtn);
    inputRow->addWidget(m_stopBtn);
    outer->addLayout(inputRow);

    // Eval-harness E1: trajectory recording toggle (default ON).  The
    // redaction pass in the core is unconditional; this only controls
    // whether a per-session JSONL file is written under
    // trajectoryDirectory() (see that method's doc for the resolved path).
    {
        QSettings settings;
        m_recordTrajectories =
            settings.value("agentChat/recordTrajectories", true).toBool();
    }
    m_recordTrajectoriesCheck = new QCheckBox("Record chat trajectories");
    m_recordTrajectoriesCheck->setChecked(m_recordTrajectories);
    m_recordTrajectoriesCheck->setToolTip(
        "Write a per-session JSONL log under " + trajectoryDirectory() +
        " (system prompt, each request/response, tool calls, tokens, timings).  "
        "API-key-shaped content is always redacted regardless of this setting.  "
        "Set RISE_TRAJECTORY_DIR to record elsewhere.");
    outer->addWidget(m_recordTrajectoriesCheck);

    m_statusLabel = new QLabel();
    m_statusLabel->setStyleSheet("color: gray;");
    m_statusLabel->setWordWrap(true);
    outer->addWidget(m_statusLabel);

    connect(m_recordTrajectoriesCheck, &QCheckBox::toggled,
            this, &ChatPanel::recordTrajectoriesToggled);
    connect(m_providerCombo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &ChatPanel::providerChanged);
    connect(m_modelEdit, &QLineEdit::editingFinished,
            this, &ChatPanel::modelEditingFinished);
    connect(m_inputEdit, &QLineEdit::returnPressed,
            this, &ChatPanel::sendMessage);
    connect(m_inputEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { updateButtonStates(); });
    connect(m_sendBtn, &QPushButton::clicked,
            this, &ChatPanel::sendMessage);
    connect(m_stopBtn, &QPushButton::clicked,
            this, &ChatPanel::requestStop);
    connect(m_resetBtn, &QPushButton::clicked,
            this, &ChatPanel::resetConversation);
    connect(m_retryBtn, &QPushButton::clicked,
            this, &ChatPanel::retryLastRequest);

    applyProviderToLoop(false);
    updateButtonStates();
}

ChatPanel::~ChatPanel()
{
    cancelOutstandingRender();
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void ChatPanel::setViewportBridge(ViewportBridge* bridge)
{
    if (!bridge) {
        // Cancel any outstanding turn / async chat-render job against the
        // OLD bridge (still current here) BEFORE dropping it below --
        // render_cancel must reach the controller that owns the job.
        requestStop();
    }
    m_bridge = bridge;
    m_sceneEditable = (bridge != nullptr);
    if (m_bridge) {
        fetchSkillIndex();
        // Eval-harness E1: a freshly-bound scene starts a NEW trajectory
        // file (skills are set above, so the session record captures the
        // full system prompt on the first user message).
        startTrajectory();
    } else {
        m_loop->Reset();
        // Detach so no file lingers between scenes.
        startTrajectory();
        refreshTranscript();
    }
    updateButtonStates();
}

void ChatPanel::setSceneEditable(bool editable)
{
    m_sceneEditable = editable && m_bridge != nullptr;
    if (!m_sceneEditable && m_busy) {
        requestStop();
    }
    updateButtonStates();
}

void ChatPanel::requestStop()
{
    if (!m_busy) return;
    m_stopRequested = true;
    if (m_outstandingRenderJobId != 0) {
        // P1-2: suspended in the render_wait poll, not blocked on an HTTP
        // reply -- there is no networkFinished() callback coming to end
        // the turn, so do it here directly.
        cancelActiveTurn("Stopped.");
        return;
    }
    if (m_reply) {
        m_reply->abort();
        return;
    }
    finishBusy("Stopped.");
}

void ChatPanel::resetConversation()
{
    requestStop();
    m_loop->Reset();
    // Eval-harness E1: Reset() closed the session ("reset" summary); start
    // a fresh file for the post-reset conversation (or detach if off).
    startTrajectory();
    clearErrorAffordances();
    refreshTranscript();
    updateButtonStates();
}

void ChatPanel::productionRenderStarting()
{
    // P2-4: called at the TOP of MainWindow::onRender/onRenderAnimation,
    // BEFORE the production rasterizer runs -- see the header doc for why
    // this explicit gate must stay even though onStateChanged also
    // disables the panel.
    if (!m_busy) return;
    m_stopRequested = true;
    if (m_reply) {
        // Detach immediately (rather than letting networkFinished() notice
        // m_stopRequested and report "Stopped.") so the notice text below
        // is the one that lands, not a race with the aborted reply's own
        // completion.  Not leaked: abort() still causes finished() to fire
        // later, and networkFinished()'s own `reply != m_reply` mismatch
        // branch (now true, since m_reply is null) calls reply->deleteLater()
        // for us -- the same cleanup path an already-superseded reply uses.
        m_reply->abort();
        m_reply = nullptr;
    }
    cancelActiveTurn("Turn cancelled -- a production render started.");
}

void ChatPanel::sendMessage()
{
    if (m_busy || !m_bridge || !m_sceneEditable) return;
    const QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty()) return;
    if (providerRequiresApiKey(m_appliedProvider) && m_apiKeyEdit->text().trimmed().isEmpty()) {
        refreshTranscript("Enter an API key before sending.");
        return;
    }

    clearErrorAffordances();
    m_inputEdit->clear();
    m_loop->AddUserMessage(toStdString(text));
    m_stopRequested = false;
    m_busy = true;
    refreshTranscript("Thinking...");
    updateButtonStates();
    runNextStep();
}

void ChatPanel::retryLastRequest()
{
    // P2-5: re-issue the SAME conversation via BuildRequest.  Nothing was
    // recorded for an Http-kind ProviderError, so this is safe without a
    // new user message.
    if (!m_retryAvailable || m_busy || !m_bridge || !m_sceneEditable) return;
    clearErrorAffordances();
    m_stopRequested = false;
    m_busy = true;
    refreshTranscript("Thinking...");
    updateButtonStates();
    runNextStep();
}

void ChatPanel::providerChanged(int)
{
    applyProviderChangeWithConfirmation(true);
}

void ChatPanel::modelEditingFinished()
{
    applyProviderChangeWithConfirmation(false);
}

void ChatPanel::networkFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    if (reply != m_reply) {
        reply->deleteLater();
        return;
    }

    m_reply = nullptr;
    const QByteArray body = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString networkError = reply->errorString();
    reply->deleteLater();

    if (m_stopRequested || netError == QNetworkReply::OperationCanceledError) {
        finishBusy("Stopped.");
        return;
    }
    if (status == 0 && netError != QNetworkReply::NoError) {
        // P2-5: a transport-level failure never reached the provider, so
        // nothing was recorded -- retriable exactly like an Http-kind
        // ProviderError.
        m_retryAvailable = true;
        finishBusy("Network error: " + networkError);
        return;
    }

    const long httpStatus = status == 0 ? 0 : status;
    const std::string bodyStd(body.constData(), static_cast<std::size_t>(body.size()));
    // Eval-harness E1: record the LLM round (status/body/latency) BEFORE
    // handling it (no-op when recording is off).  The loop strips auth
    // headers from its cached request; the writer redacts every line.
    const qint64 elapsedMs = m_httpTimer.isValid() ? m_httpTimer.elapsed() : 0;
    m_loop->RecordHttpRound(httpStatus, bodyStd, static_cast<int64_t>(elapsedMs));
    const ChatStepResult step = m_loop->HandleResponse(httpStatus, bodyStd);
    refreshTranscript(step.assistantDisplayText.empty() ? QString() : "Running tools...");

    if (step.kind == ChatStepResult::Kind::ProviderError) {
        handleProviderError(step);
        return;
    }

    if (step.kind == ChatStepResult::Kind::FinalText) {
        finishBusy();
        return;
    }

    // P1-2: hand the turn's tool calls to the suspendable state machine
    // instead of running them in one synchronous loop.
    m_pendingToolCalls.assign(step.toolCalls.begin(), step.toolCalls.end());
    processNextToolCall();
}

ChatPanel::Provider ChatPanel::currentProvider() const
{
    return static_cast<Provider>(m_providerCombo->currentIndex());
}

QString ChatPanel::defaultModelFor(Provider provider) const
{
    switch (provider) {
    case Provider::Anthropic:
        return QString::fromUtf8(AnthropicChatCodec().DefaultModelId());
    case Provider::Gemini:
        return QString::fromUtf8(GeminiChatCodec().DefaultModelId());
    case Provider::OpenAI:
        return QString::fromUtf8(OpenAIChatCodec().DefaultModelId());
    case Provider::XAI: {
        // xAI rides the parameterized OpenAIChatCodec (no standalone
        // XAIChatCodec type -- see AgentChatLoop.cpp's MakeCodec, which
        // this mirrors for display purposes since there is no bridge
        // accessor onto the loop's already-constructed codec config).
        OpenAIChatCodec::Config cfg;
        cfg.providerName   = "xai";
        cfg.baseUrl        = "https://api.x.ai/v1/chat/completions";
        cfg.defaultModelId = "grok-4.5";
        cfg.requiresAuth   = true;
        return QString::fromUtf8(OpenAIChatCodec(cfg).DefaultModelId());
    }
    case Provider::Local: {
        OpenAIChatCodec::Config cfg;
        cfg.providerName   = "local";
        cfg.baseUrl        = "http://127.0.0.1:11434/v1/chat/completions";
        cfg.defaultModelId = "qwen3:32b";
        cfg.requiresAuth   = false;
        return QString::fromUtf8(OpenAIChatCodec(cfg).DefaultModelId());
    }
    }
    // Every enum case is handled explicitly above -- reaching here means a
    // new Provider value was added without a matching case (the switch is
    // NOT exhaustive-checked by MSVC without a default, unlike Swift's
    // enum, so this is the loud backstop).  Log and fall back to OpenAI
    // rather than silently mis-defaulting.
    qWarning("ChatPanel::defaultModelFor: unhandled Provider %d -- falling back to OpenAI",
              static_cast<int>(provider));
    Q_ASSERT(false && "ChatPanel::defaultModelFor: unhandled Provider");
    return QString::fromUtf8(OpenAIChatCodec().DefaultModelId());
}

QString ChatPanel::envKeyFor(Provider provider) const
{
    switch (provider) {
    case Provider::Anthropic:
        return qEnvironmentVariable("ANTHROPIC_API_KEY");
    case Provider::Gemini: {
        const QString gemini = qEnvironmentVariable("GEMINI_API_KEY");
        return gemini.isEmpty() ? qEnvironmentVariable("GOOGLE_API_KEY") : gemini;
    }
    case Provider::OpenAI:
        return qEnvironmentVariable("OPENAI_API_KEY");
    case Provider::XAI:
        return qEnvironmentVariable("XAI_API_KEY");
    case Provider::Local:
        // Keyless by design (OpenAIChatCodec::Config::requiresAuth=false
        // for this provider) -- no env var to consult.  An empty return
        // here is the correct/expected value, not a miss.
        return QString();
    }
    qWarning("ChatPanel::envKeyFor: unhandled Provider %d -- falling back to OpenAI",
              static_cast<int>(provider));
    Q_ASSERT(false && "ChatPanel::envKeyFor: unhandled Provider");
    return qEnvironmentVariable("OPENAI_API_KEY");
}

bool ChatPanel::providerRequiresApiKey(Provider provider)
{
    return provider != Provider::Local;
}

QString ChatPanel::localResolvedEndpoint()
{
    const QString envUrl = qEnvironmentVariable("RISE_LOCAL_LLM_BASE_URL");
    return envUrl.isEmpty() ? QStringLiteral("http://127.0.0.1:11434/v1/chat/completions")
                             : envUrl;
}

void ChatPanel::applyProviderToLoop(bool resetModelToDefault)
{
    const Provider oldProvider = m_appliedProvider;
    const Provider newProvider = currentProvider();

    // P1-1: keys are per-provider and must never cross providers.  Stash
    // whatever is currently in the field under the OLD applied provider's
    // slot (a harmless no-op rewrite of the same slot when oldProvider ==
    // newProvider, e.g. a model-only change), then repopulate the field
    // from the NEW provider's slot, falling back to its env var when that
    // slot is still empty.  This guarantees the field -- and therefore
    // every fetch/step path that reads it -- always carries only the
    // CURRENTLY APPLIED provider's key.
    m_keysByProvider[static_cast<int>(oldProvider)] = m_apiKeyEdit->text();

    if (resetModelToDefault || m_modelEdit->text().trimmed().isEmpty()) {
        m_modelEdit->setText(defaultModelFor(newProvider));
    }

    QString newKey = m_keysByProvider.value(static_cast<int>(newProvider));
    if (newKey.trimmed().isEmpty()) {
        newKey = envKeyFor(newProvider);
    }
    m_apiKeyEdit->setText(newKey);

    // No-key affordance for Provider::Local (keyless-by-design): there is
    // no per-provider settings popover on this platform to swap in
    // dedicated endpoint UI, so reuse the existing key field's placeholder
    // to show where the chat is pointing plus a hint that a server has to
    // actually be running -- the field itself stays enabled (a local
    // server started with --api-key still accepts an optional key here).
    m_apiKeyEdit->setPlaceholderText(
        providerRequiresApiKey(newProvider)
            ? QStringLiteral("API key")
            : QStringLiteral("No key needed -- ") + localResolvedEndpoint() +
                  QStringLiteral(" (requires a running local server, e.g. "
                                 "`ollama serve`; override with RISE_LOCAL_LLM_BASE_URL)"));

    ChatProvider chatProvider = ChatProvider::OpenAI;
    if (newProvider == Provider::Anthropic) {
        chatProvider = ChatProvider::Anthropic;
    } else if (newProvider == Provider::Gemini) {
        chatProvider = ChatProvider::Gemini;
    } else if (newProvider == Provider::XAI) {
        chatProvider = ChatProvider::XAI;
    } else if (newProvider == Provider::Local) {
        chatProvider = ChatProvider::Local;
    } else if (newProvider != Provider::OpenAI) {
        // Same loud backstop as defaultModelFor/envKeyFor: a new Provider
        // value reached here without a matching branch.  Falls back to
        // the OpenAI default above rather than silently mis-routing.
        qWarning("ChatPanel::applyProviderToLoop: unhandled Provider %d -- falling back to OpenAI",
                  static_cast<int>(newProvider));
        Q_ASSERT(false && "ChatPanel::applyProviderToLoop: unhandled Provider");
    }
    m_loop->SetProvider(chatProvider, toStdString(m_modelEdit->text().trimmed()));
    // Eval-harness E1: SetProvider closed the old session ("provider_switch"
    // summary to the old file); begin a new file for the new provider.
    startTrajectory();
    refreshTranscript();

    m_appliedProvider = newProvider;
    m_appliedModel = m_modelEdit->text().trimmed();
    clearErrorAffordances();
}

void ChatPanel::applyProviderChangeWithConfirmation(bool resetModelToDefault)
{
    // The combo/model field are disabled while busy (updateButtonStates),
    // so this signal should not be reachable mid-turn -- guard anyway
    // rather than pop a modal dialog over a live turn.
    if (m_busy) return;

    const Provider provider = currentProvider();
    const QString model = m_modelEdit->text().trimmed();

    // P1-3: a focus-out / combo signal that didn't actually change
    // anything from the currently-applied provider+model is a no-op --
    // SetProvider() always resets the transcript, so treating "regained
    // then lost focus with the same text" as a change would silently wipe
    // the conversation on every stray click.
    if (provider == m_appliedProvider && model == m_appliedModel) {
        return;
    }

    if (m_loop->TranscriptSize() > 0) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Switch provider or model?"),
            tr("Conversation history cannot cross providers or models -- "
               "applying resets the chat. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            revertProviderModelWidgets();
            return;
        }
    }

    applyProviderToLoop(resetModelToDefault);
}

void ChatPanel::revertProviderModelWidgets()
{
    // Restore the widgets to the currently-applied provider/model WITHOUT
    // re-entering providerChanged (setCurrentIndex would otherwise re-fire
    // currentIndexChanged); QLineEdit::setText does not itself emit
    // editingFinished, so no blocker is needed for the model field.
    {
        const QSignalBlocker blocker(m_providerCombo);
        m_providerCombo->setCurrentIndex(static_cast<int>(m_appliedProvider));
    }
    m_modelEdit->setText(m_appliedModel);
}

void ChatPanel::refreshTranscript(const QString& statusLine)
{
    QString text;
    for (std::size_t i = 0; i < m_loop->TranscriptSize(); ++i) {
        const auto& entry = m_loop->TranscriptAt(i);
        QString body = toQString(entry.displayText).trimmed();
        if (body.isEmpty()) {
            body = entry.role == RISE::Agent::ChatTranscriptEntry::Role::ToolResults
                ? "[tool results]"
                : "[no text]";
        }
        text += roleLabel(entry.role) + ":\n" + body + "\n\n";
    }
    if (!statusLine.isEmpty()) {
        text += statusLine + "\n";
    }
    m_transcript->setPlainText(text.trimmed());
    if (QScrollBar* bar = m_transcript->verticalScrollBar()) {
        bar->setValue(bar->maximum());
    }
    m_statusLabel->setText(statusLine);
}

void ChatPanel::updateButtonStates()
{
    const bool canSend = !m_busy && m_bridge && m_sceneEditable
        && !m_inputEdit->text().trimmed().isEmpty();
    m_sendBtn->setEnabled(canSend);
    m_stopBtn->setEnabled(m_busy);
    m_resetBtn->setEnabled(!m_busy && m_loop->TranscriptSize() > 0);
    m_providerCombo->setEnabled(!m_busy);
    m_modelEdit->setEnabled(!m_busy);
    m_apiKeyEdit->setEnabled(!m_busy);
    m_inputEdit->setEnabled(!m_busy && m_bridge && m_sceneEditable);
    m_retryBtn->setEnabled(m_retryAvailable && !m_busy && m_bridge && m_sceneEditable);
}

void ChatPanel::runNextStep()
{
    if (m_stopRequested) {
        finishBusy("Stopped.");
        return;
    }
    if (!m_bridge || !m_sceneEditable) {
        finishBusy("Chat is disabled while the scene is not editable.");
        return;
    }

    // P1-1: the field always carries only m_appliedProvider's key (see
    // applyProviderToLoop) -- since providerChanged/modelEditingFinished
    // cannot change m_appliedProvider without going back through that
    // function, reading the live field text here is reading the CURRENTLY
    // APPLIED provider's key, never a stale/foreign one.
    const RISE::Agent::ChatHttpRequest req =
        m_loop->BuildRequest(toStdString(m_apiKeyEdit->text().trimmed()));
    if (req.url.empty()) {
        finishBusy();
        return;
    }

    QNetworkRequest netReq(QUrl(toQString(req.url)));
    for (const auto& header : req.headers) {
        netReq.setRawHeader(
            QByteArray(header.first.c_str(), static_cast<int>(header.first.size())),
            QByteArray(header.second.c_str(), static_cast<int>(header.second.size())));
    }

    // Eval-harness E1: measure the round-trip latency for the llm record.
    m_httpTimer.start();
    m_reply = m_network->post(
        netReq,
        QByteArray(req.body.c_str(), static_cast<int>(req.body.size())));
    connect(m_reply, &QNetworkReply::finished, this, &ChatPanel::networkFinished);
}

void ChatPanel::finishBusy(const QString& statusLine)
{
    m_busy = false;
    m_stopRequested = false;
    refreshTranscript(statusLine);
    updateButtonStates();
}

void ChatPanel::fetchSkillIndex()
{
    if (!m_bridge) return;
    const QString line =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"read_skill\",\"params\":{}}";
    m_loop->SetSkillIndex(toStdString(renderSkillIndex(m_bridge->agentHandleLine(line))));
}

QString ChatPanel::trajectoryDirectory() const
{
    // Dev-workflow override: running the GUI straight out of the repo and
    // wanting files under evals/runs/gui there.
    const QString dirOverride = qEnvironmentVariable("RISE_TRAJECTORY_DIR");
    if (!dirOverride.isEmpty()) return dirOverride;

    // QDir::currentPath() is nondeterministic for a GUI app (whatever the
    // shell/shortcut/Explorer launched it from happened to be) and can be
    // unwritable outright under Program Files -- either way "evals/runs/gui"
    // relative to it silently never got created and every trajectory line
    // vanished into a sink that opened nothing (recording toggle said ON;
    // nothing was written -- see MakeTrajectoryFileSink's loud-once-failure
    // log, which is the ONLY place that ever surfaced this before the fix).
    // AppDataLocation already folds in the organizationName/applicationName
    // set on QCoreApplication in main.cpp ("RISE"/"RISE"), so this resolves
    // to a deterministic, always-writable per-user directory.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return base + "/trajectories/gui";
}

void ChatPanel::startTrajectory()
{
    // Detach when recording is off or no scene is bound.
    if (!m_recordTrajectories || !m_bridge) {
        m_loop->SetTrajectorySink(std::function<void(const std::string&)>());
        return;
    }
    const std::string dir = toStdString(trajectoryDirectory());

    // Rotate BEFORE creating the new file (keep ~50 newest / ~200 MB).
    RISE::Agent::PruneTrajectoryDir(dir, 50, 200LL * 1024 * 1024);

    const std::string traceId = RISE::Agent::MakeTrajectoryTraceId();
    char ts[32];
    std::time_t nowT = std::time(nullptr);
    std::tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &nowT);
#else
    gmtime_r(&nowT, &tmv);
#endif
    std::strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tmv);
    const std::string path =
        dir + "/" + std::string(ts) + "-" + traceId.substr(0, 8) + ".jsonl";

    RISE::Agent::ChatTrajectoryConfig cfg;
    cfg.traceId = traceId;
    cfg.scenePath = toStdString(m_scenePath);
    cfg.sceneHeadVersion = -1;   // best-effort on the GUI; the headless runner populates it precisely
    m_loop->SetTrajectorySink(RISE::Agent::MakeTrajectoryFileSink(path), cfg);
}

void ChatPanel::recordTrajectoriesToggled(bool on)
{
    if (on == m_recordTrajectories) return;
    m_recordTrajectories = on;
    QSettings settings;
    settings.setValue("agentChat/recordTrajectories", on);
    if (on) {
        startTrajectory();
    } else {
        m_loop->FinishTrajectory("toggle_off");
        m_loop->SetTrajectorySink(std::function<void(const std::string&)>());
    }
}

QString ChatPanel::renderSkillIndex(const QString& rpcResponse) const
{
    const QJsonDocument doc = QJsonDocument::fromJson(rpcResponse.toUtf8());
    const QJsonObject root = doc.object();
    const QJsonObject result = root.value("result").toObject();
    const QJsonArray skills = result.value("skills").toArray();
    QStringList lines;
    for (const QJsonValue& v : skills) {
        const QJsonObject skill = v.toObject();
        const QString name = skill.value("name").toString();
        const QString hook = skill.value("hook").toString();
        if (!name.isEmpty()) {
            lines << (hook.isEmpty() ? name : name + " -- " + hook);
        }
    }
    return lines.join("\n");
}

QString ChatPanel::cancelledToolResultJson(int rpcId, const QString& message) const
{
    const QJsonObject err{
        { "code", -32000 },
        { "message", message }
    };
    const QJsonObject root{
        { "jsonrpc", "2.0" },
        { "id", rpcId },
        { "error", err }
    };
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void ChatPanel::clearErrorAffordances()
{
    m_retryAvailable = false;
    updateButtonStates();
}

void ChatPanel::handleProviderError(const RISE::Agent::ChatStepResult& step)
{
    // P2-5: per-kind reactions honoring AgentChatLoop.h's documented
    // ChatErrorKind contract (mirrors the Mac handleProviderError).
    m_retryAvailable = false;
    switch (step.errorKind) {
    case ChatErrorKind::Http:
        // The one retriable kind -- nothing was recorded for this step.
        m_retryAvailable = true;
        finishBusy("HTTP error: " + toQString(step.errorMessage));
        break;

    case ChatErrorKind::Refusal:
        finishBusy("The model declined this request: " + toQString(step.errorMessage));
        break;

    case ChatErrorKind::MaxTokens:
        // Truncated reply was discarded; a verbatim retry would likely
        // truncate again -- recovery is a new, narrower message, not Retry.
        finishBusy("The reply hit the output-token limit and was discarded. "
                   "Continue with a new message.");
        break;

    case ChatErrorKind::IterationCap:
        // The per-turn tool-round cap tripped; it resets on the next
        // user message, not on Retry.
        finishBusy("Stopped after too many tool rounds in one turn. "
                   "Send a new message to continue.");
        break;

    case ChatErrorKind::Misuse:
        // Driver-contract violation -- should not happen.  Log the
        // loop's own diagnostic to stderr (never the response body) and
        // show a generic message.
        qWarning("RISE agent chat: driver misuse: %s", step.errorMessage.c_str());
        finishBusy("Internal chat error. Reset the conversation if it persists.");
        break;

    case ChatErrorKind::Parse:
    case ChatErrorKind::Provider:
    case ChatErrorKind::None:
    default:
        finishBusy("Provider error: " + toQString(step.errorMessage));
        break;
    }
}

void ChatPanel::processNextToolCall()
{
    if (!m_pendingToolCalls.empty()) {
        if (m_stopRequested || !m_sceneEditable || !m_bridge) {
            const ChatToolCall call = m_pendingToolCalls.front();
            m_pendingToolCalls.erase(m_pendingToolCalls.begin());
            const int rpcId = m_nextRpcId++;
            m_loop->AddToolResult(call, toStdString(
                cancelledToolResultJson(rpcId, "tool call cancelled: scene is not editable")));
            processNextToolCall();
            return;
        }

        const ChatToolCall call = m_pendingToolCalls.front();
        m_pendingToolCalls.erase(m_pendingToolCalls.begin());
        const int rpcId = m_nextRpcId++;
        const std::string line = m_loop->ToolCallToJsonRpcLine(call, rpcId);

        if (call.name == "render") {
            // P1-2: suspends here -- resumes back into processNextToolCall
            // from pollOutstandingRender() once the async job completes.
            startAsyncRenderToolCall(call, line);
            return;
        }

        // Every other verb keeps the existing synchronous contract: fast
        // CST reads/edits with no render-duration cost to amortize.
        const QString response = m_bridge->agentHandleLine(toQString(line));
        m_loop->AddToolResult(call, toStdString(response));
        processNextToolCall();
        return;
    }

    refreshTranscript("Thinking...");
    if (m_stopRequested || !m_sceneEditable) {
        finishBusy("Stopped.");
        return;
    }
    runNextStep();
}

void ChatPanel::startAsyncRenderToolCall(const ChatToolCall& call, const std::string& submitLine)
{
    m_activeRenderCall = call;
    m_activeRenderPending = true;

    // Upgrade the already-built synchronous-shaped JSON-RPC line to carry
    // {"async":true} -- the LLM-authored params (width/height/camera/
    // samples/...) pass through unchanged.  A parse failure here should
    // not happen (ToolCallToJsonRpcLine always emits well-formed JSON) --
    // but the fallback must NEVER be the original synchronous line: that
    // would silently re-block the GUI thread for the render's whole
    // duration, the exact failure mode this async path exists to prevent.
    // Refuse honestly instead; the model can retry.
    const QString asyncLine = injectAsyncTrue(toQString(submitLine));
    if (asyncLine.isEmpty()) {
        m_activeRenderPending = false;
        m_loop->AddToolResult(m_activeRenderCall,
            toStdString(makeSyntheticRenderResultLine(
                false, "internal: could not stage the async render submit")));
        processNextToolCall();
        return;
    }

    const QString submitResponse = m_bridge->agentHandleLine(asyncLine);
    const QJsonDocument submitDoc = QJsonDocument::fromJson(submitResponse.toUtf8());
    if (!submitDoc.isObject()) {
        // Malformed response line -- should not happen (HandleLine always
        // emits valid JSON-RPC) -- fail honestly rather than guess.
        m_activeRenderPending = false;
        m_loop->AddToolResult(m_activeRenderCall, toStdString(submitResponse));
        processNextToolCall();
        return;
    }
    const QJsonObject submitObj = submitDoc.object();
    if (submitObj.contains("error")) {
        // Refused outright (no controller attached) -- already a
        // well-formed JSON-RPC error envelope; surface it verbatim.
        m_activeRenderPending = false;
        m_loop->AddToolResult(m_activeRenderCall, toStdString(submitResponse));
        processNextToolCall();
        return;
    }
    const QJsonObject result = submitObj.value("result").toObject();
    const QString status = result.value("status").toString();
    if (status != "submitted") {
        // "refused" -- the single-slot worker is busy or an editor
        // transaction is open.  Deliver an HONEST ok:false result in the
        // synchronous `render` shape so the model sees a clean failure.
        const QString message = result.contains("message")
            ? result.value("message").toString() : QStringLiteral("render refused");
        m_activeRenderPending = false;
        m_loop->AddToolResult(m_activeRenderCall,
            toStdString(makeSyntheticRenderResultLine(false, message)));
        processNextToolCall();
        return;
    }
    if (!result.contains("renderJobId")) {
        m_activeRenderPending = false;
        m_loop->AddToolResult(m_activeRenderCall, toStdString(makeSyntheticRenderResultLine(
            false, "render accepted but no renderJobId was returned")));
        processNextToolCall();
        return;
    }

    m_outstandingRenderJobId = static_cast<quint64>(result.value("renderJobId").toDouble());

    if (!m_renderPollTimer) {
        m_renderPollTimer = new QTimer(this);
        m_renderPollTimer->setInterval(250);
        connect(m_renderPollTimer, &QTimer::timeout, this, &ChatPanel::pollOutstandingRender);
    }
    m_renderPollTimer->start();
}

void ChatPanel::pollOutstandingRender()
{
    if (m_outstandingRenderJobId == 0) {
        if (m_renderPollTimer) m_renderPollTimer->stop();
        return;
    }
    if (m_stopRequested || !m_bridge || !m_sceneEditable) {
        // requestStop()/setSceneEditable(false)/productionRenderStarting()
        // already cancel+drain synchronously when they fire; this guard
        // only covers a tick that raced one of them.  Don't act on a job
        // we've already cancelled.
        if (m_renderPollTimer) m_renderPollTimer->stop();
        return;
    }

    // render_wait(timeoutMs:0) is a poll-ONCE call -- a single mutex-
    // guarded status read, never a multi-second block -- so this tick
    // stays fast regardless of how long the render itself is taking.
    const QString waitLine = buildJsonRpcLine("render_wait", QJsonObject{
        { "renderJobId", static_cast<double>(m_outstandingRenderJobId) },
        { "timeoutMs", 0 }
    });
    const QString waitResponse = m_bridge->agentHandleLine(waitLine);
    const QJsonDocument waitDoc = QJsonDocument::fromJson(waitResponse.toUtf8());
    if (!waitDoc.isObject()) {
        return; // transient parse hiccup -- keep polling
    }
    const QJsonObject waitObj = waitDoc.object();
    if (waitObj.contains("error")) {
        // A verb-level error envelope (e.g. no live session) can never
        // resolve by polling again -- without this branch the panel would
        // poll forever and sit busy until a manual Stop.  Surface it
        // verbatim, matching the submit path's error branch.
        if (m_renderPollTimer) m_renderPollTimer->stop();
        m_outstandingRenderJobId = 0;
        m_activeRenderPending = false;
        m_loop->AddToolResult(m_activeRenderCall, toStdString(waitResponse));
        processNextToolCall();
        return;
    }
    const QJsonObject waitResult = waitObj.value("result").toObject();
    if (!waitResult.value("completed").toBool(false)) {
        return; // not done yet -- keep polling
    }

    if (m_renderPollTimer) m_renderPollTimer->stop();
    m_outstandingRenderJobId = 0;
    m_activeRenderPending = false;

    // waitResult's `result` sub-object (present iff this session cached
    // that job's stats) carries the EXACT synchronous-render shape.
    QString responseLine;
    if (waitResult.contains("result")) {
        responseLine = makeSyntheticResponseLine(waitResult.value("result").toObject());
    } else {
        responseLine = makeSyntheticRenderResultLine(
            false, "render completed but no cached result was found");
    }
    m_loop->AddToolResult(m_activeRenderCall, toStdString(responseLine));
    processNextToolCall();
}

void ChatPanel::cancelOutstandingRender()
{
    if (m_renderPollTimer) {
        m_renderPollTimer->stop();
    }
    if (m_outstandingRenderJobId == 0) return;
    const quint64 jobId = m_outstandingRenderJobId;
    m_outstandingRenderJobId = 0;
    if (!m_bridge) return;
    // Fire-and-forget: render_cancel only trips the cancel flag and
    // returns immediately (it does not block for the render's duration --
    // see AgentRpc.h). The response is intentionally discarded.
    const QString line = buildJsonRpcLine("render_cancel", QJsonObject{
        { "renderJobId", static_cast<double>(jobId) }
    });
    m_bridge->agentHandleLine(line);
}

void ChatPanel::drainPendingToolCallsAsCancelled()
{
    for (const auto& call : m_pendingToolCalls) {
        const int rpcId = m_nextRpcId++;
        m_loop->AddToolResult(call, toStdString(
            cancelledToolResultJson(rpcId, "tool call cancelled: scene is not editable")));
    }
    m_pendingToolCalls.clear();
}

void ChatPanel::cancelActiveTurn(const QString& statusLine)
{
    cancelOutstandingRender();
    if (m_activeRenderPending) {
        const int rpcId = m_nextRpcId++;
        m_loop->AddToolResult(m_activeRenderCall, toStdString(
            cancelledToolResultJson(rpcId, "tool call cancelled: scene is not editable")));
        m_activeRenderPending = false;
    }
    drainPendingToolCallsAsCancelled();
    finishBusy(statusLine);
}
