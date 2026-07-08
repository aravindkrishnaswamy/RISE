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
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTextEdit;
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

private slots:
    void sendMessage();
    void providerChanged(int index);
    void modelEditingFinished();
    void networkFinished();

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
    void refreshTranscript(const QString& statusLine = QString());
    void updateButtonStates();
    void runNextStep();
    void finishBusy(const QString& statusLine = QString());
    void fetchSkillIndex();
    QString renderSkillIndex(const QString& rpcResponse) const;
    QString cancelledToolResultJson(int rpcId, const QString& message) const;

    std::unique_ptr<RISE::Agent::AgentChatLoop> m_loop;
    ViewportBridge* m_bridge = nullptr;   // borrowed; owned by MainWindow
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;

    QComboBox* m_providerCombo = nullptr;
    QLineEdit* m_modelEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QTextEdit* m_transcript = nullptr;
    QLineEdit* m_inputEdit = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QPushButton* m_stopBtn = nullptr;
    QPushButton* m_resetBtn = nullptr;
    QLabel* m_statusLabel = nullptr;

    bool m_busy = false;
    bool m_stopRequested = false;
    bool m_sceneEditable = false;
    int m_nextRpcId = 1;
};

#endif // CHATPANEL_H
