//////////////////////////////////////////////////////////////////////
//
//  ProposalCard.h - RISE UI redesign, left panel Agent tab "DIFF CARD".
//
//  Renders one pending/resolved agent-proposed edit (Secure-MCP slice
//  5c's list_proposals wire shape, AgentRpc.h) as the bordered diff
//  card from the approved design comp.  Mirrors the macOS
//  ProposalCard.swift (ProposalDiffCard / ProposalsPanel).
//
//  ChatPanel owns the JSON-RPC polling (list_proposals) and JSON
//  parsing; this widget is presentation + the Apply/Reject/Undo
//  affordances, which it reports back via signals so ChatPanel can
//  route them through resolve_proposal / ViewportBridge::undo.
//
//////////////////////////////////////////////////////////////////////

#ifndef PROPOSALCARD_H
#define PROPOSALCARD_H

#include <QWidget>
#include <QString>
#include <QtGlobal>

class QPushButton;
class QVBoxLayout;

/// One entry of list_proposals' wire shape -- Windows-side mirror of
/// the macOS ChatViewModel.ProposalEntry.  Plain data struct; empty
/// string / false for fields that don't apply to a given `kind`.
struct ProposalEntry {
    quint64 id = 0;
    QString kind;            // "param_edit" | "insert_chunk" | "remove_chunk"
    QString sessionLabel;
    QString status;          // "pending" | "applied" | "rejected" | "conflict"
    QString target;
    QString entityKind;
    QString param;
    QString value;
    QString chunkText;
    bool    truncated = false;
};

class ProposalCard : public QWidget
{
    Q_OBJECT

public:
    explicit ProposalCard(const ProposalEntry& proposal, const QString& sceneFileName,
                           QWidget* parent = nullptr);

    quint64 proposalId() const { return m_proposal.id; }
    QString status() const { return m_proposal.status; }

    /// Mirrors the Mac card's `applyRejectDisabled` -- disabled while
    /// the scene isn't editable (a production render in flight, a
    /// chat turn outstanding, etc).  No-op on the resolved footers
    /// (they have no action buttons to gate).
    void setActionsEnabled(bool enabled);

signals:
    void applyClicked(quint64 proposalId);
    void rejectClicked(quint64 proposalId);
    /// Joins the shared undo history (ViewportBridge::undo(), the SAME
    /// Edit-menu undo TopBar/ViewportToolbar drive) -- matches the
    /// comp's "one undo step" note.
    void undoClicked();

private:
    void buildHeader(QVBoxLayout* root);
    void buildBody(QVBoxLayout* root);
    void buildFooter(QVBoxLayout* root);
    QWidget* makeDiffLine(const QString& sign, const QString& text, bool isAddition) const;
    QWidget* makeBlockLabelRow(const QString& text) const;

    int plusCount() const;
    int minusCount() const;

    ProposalEntry m_proposal;
    QString       m_sceneFileName;
    QPushButton*  m_applyBtn  = nullptr;
    QPushButton*  m_rejectBtn = nullptr;
    // Round-2 P1: the applied-state Undo button must be disable-able by
    // setActionsEnabled just like Apply/Reject -- it drives the same
    // scene-mutating ViewportBridge::undo() path.
    QPushButton*  m_undoBtn   = nullptr;
};

#endif // PROPOSALCARD_H
