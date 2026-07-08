//////////////////////////////////////////////////////////////////////
//
//  ChatPanel.cpp - Windows Qt chat panel implementation.
//
//////////////////////////////////////////////////////////////////////

#include "ChatPanel.h"

#include "ViewportBridge.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include "Agent/AgentChatLoop.h"

using RISE::Agent::AgentChatLoop;
using RISE::Agent::AnthropicChatCodec;
using RISE::Agent::ChatErrorKind;
using RISE::Agent::ChatProvider;
using RISE::Agent::ChatStepResult;
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
}

ChatPanel::ChatPanel(QWidget* parent)
    : QWidget(parent)
    , m_loop(new AgentChatLoop())
{
    m_network = new QNetworkAccessManager(this);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 8);
    outer->setSpacing(6);

    auto* header = new QHBoxLayout();
    auto* title = new QLabel("Chat");
    title->setStyleSheet("font-weight: bold; color: gray;");
    header->addWidget(title);
    header->addStretch();
    m_resetBtn = new QPushButton("Reset");
    m_resetBtn->setToolTip("Clear the conversation");
    header->addWidget(m_resetBtn);
    outer->addLayout(header);

    auto* providerRow = new QHBoxLayout();
    m_providerCombo = new QComboBox();
    m_providerCombo->addItem("OpenAI");
    m_providerCombo->addItem("Anthropic");
    m_providerCombo->addItem("Gemini");
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

    m_statusLabel = new QLabel();
    m_statusLabel->setStyleSheet("color: gray;");
    m_statusLabel->setWordWrap(true);
    outer->addWidget(m_statusLabel);

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

    applyProviderToLoop(false);
    updateButtonStates();
}

ChatPanel::~ChatPanel()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void ChatPanel::setViewportBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_sceneEditable = (bridge != nullptr);
    if (m_bridge) {
        fetchSkillIndex();
    } else {
        requestStop();
        m_loop->Reset();
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
    refreshTranscript();
    updateButtonStates();
}

void ChatPanel::sendMessage()
{
    if (m_busy || !m_bridge || !m_sceneEditable) return;
    const QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty()) return;
    if (m_apiKeyEdit->text().trimmed().isEmpty()) {
        refreshTranscript("Enter an API key before sending.");
        return;
    }

    m_inputEdit->clear();
    m_loop->AddUserMessage(toStdString(text));
    m_stopRequested = false;
    m_busy = true;
    refreshTranscript("Thinking...");
    updateButtonStates();
    runNextStep();
}

void ChatPanel::providerChanged(int)
{
    applyProviderToLoop(true);
}

void ChatPanel::modelEditingFinished()
{
    applyProviderToLoop(false);
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
        finishBusy("Network error: " + networkError);
        return;
    }

    const long httpStatus = status == 0 ? 0 : status;
    const ChatStepResult step =
        m_loop->HandleResponse(httpStatus, std::string(body.constData(), static_cast<std::size_t>(body.size())));
    refreshTranscript(step.assistantDisplayText.empty() ? QString() : "Running tools...");

    if (step.kind == ChatStepResult::Kind::ProviderError) {
        const QString prefix =
            step.errorKind == ChatErrorKind::Http ? "HTTP error: " : "Provider error: ";
        finishBusy(prefix + toQString(step.errorMessage));
        return;
    }

    if (step.kind == ChatStepResult::Kind::FinalText) {
        finishBusy();
        return;
    }

    for (const auto& call : step.toolCalls) {
        const int rpcId = m_nextRpcId++;
        if (m_stopRequested || !m_sceneEditable || !m_bridge) {
            m_loop->AddToolResult(call, toStdString(
                cancelledToolResultJson(rpcId, "tool call cancelled: scene is not editable")));
            continue;
        }

        const std::string line = m_loop->ToolCallToJsonRpcLine(call, rpcId);
        const QString response = m_bridge->agentHandleLine(toQString(line));
        m_loop->AddToolResult(call, toStdString(response));
    }

    refreshTranscript("Thinking...");
    if (m_stopRequested || !m_sceneEditable) {
        finishBusy("Stopped.");
        return;
    }
    runNextStep();
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
    default:
        return QString::fromUtf8(OpenAIChatCodec().DefaultModelId());
    }
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
    default:
        return qEnvironmentVariable("OPENAI_API_KEY");
    }
}

void ChatPanel::applyProviderToLoop(bool resetModelToDefault)
{
    const Provider provider = currentProvider();
    if (resetModelToDefault || m_modelEdit->text().trimmed().isEmpty()) {
        m_modelEdit->setText(defaultModelFor(provider));
    }
    if (m_apiKeyEdit->text().trimmed().isEmpty()) {
        m_apiKeyEdit->setText(envKeyFor(provider));
    }

    ChatProvider chatProvider = ChatProvider::OpenAI;
    if (provider == Provider::Anthropic) {
        chatProvider = ChatProvider::Anthropic;
    } else if (provider == Provider::Gemini) {
        chatProvider = ChatProvider::Gemini;
    }
    m_loop->SetProvider(chatProvider, toStdString(m_modelEdit->text().trimmed()));
    refreshTranscript();
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
