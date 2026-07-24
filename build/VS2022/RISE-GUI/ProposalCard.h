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
#include <QVector>
#include <QtGlobal>

class QEvent;
class QFrame;
class QLabel;
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

protected:
    // LIVE THEME-SWITCH CONTRACT (Theme.h): ProposalCard instances are
    // created dynamically -- one per pending/resolved proposal, torn
    // down and recreated wholesale by ChatPanel::rebuildProposalsUI on
    // its ~1s list_proposals poll cadence -- but a card already on
    // screen when the theme switches must restyle IMMEDIATELY rather
    // than wait out that poll interval, so each instance follows the
    // contract itself instead of relying on ChatPanel's next rebuild.
    void changeEvent(QEvent* e) override;

private:
    void buildHeader(QVBoxLayout* root);
    void buildBody(QVBoxLayout* root);
    void buildFooter(QVBoxLayout* root);
    // Returns the QLabel it created (rather than a fixed QWidget*) so
    // buildBody() can retain it for restyleTheme() -- see m_diffAddition-
    // Lines / m_diffRemovalLines below.
    QLabel* makeDiffLine(const QString& sign, const QString& text, bool isAddition);
    // Same reasoning: out-params hand back the dot + text QLabel inside
    // the row so restyleTheme() can re-apply their color without
    // rebuilding the row.
    QWidget* makeBlockLabelRow(const QString& text, QWidget** outDot, QLabel** outLabel) const;

    int plusCount() const;
    int minusCount() const;

    // LIVE THEME-SWITCH CONTRACT: re-applies every one of THIS card's
    // own token-dependent styling sites (outer card border/background,
    // the two hairline separators, the header's title/file/count-chip
    // labels, the body's block-label-row dot+text and every diff line,
    // the footer's status label / Apply-Reject-Undo buttons / footer
    // background) from the CURRENT Theme:: token values.  Every widget
    // below is promoted to a member specifically so this can re-style
    // them WITHOUT creating/destroying any widget -- buildHeader/
    // buildBody/buildFooter run exactly once, from the constructor, and
    // m_proposal is immutable for the rest of this instance's lifetime,
    // so a rebuild would accomplish nothing a direct re-style doesn't.
    // Called once at the end of the constructor and again from
    // changeEvent() on QEvent::PaletteChange.  Idempotent, creates no
    // widgets.
    void restyleTheme();

    // LIVE THEME-SWITCH CONTRACT point 4 (Theme.h) -- re-entrancy guard.
    // See MainWindow.h for the full rationale; uniform across every
    // changeEvent()-overriding class.
    bool m_themeReady = false;
    int  m_themeEpochSeen = -1;

    ProposalEntry m_proposal;
    QString       m_sceneFileName;

    // ---- Header (buildHeader) ----
    QLabel* m_titleLabel = nullptr;
    QLabel* m_fileLabel  = nullptr;   // null when m_sceneFileName is empty
    QLabel* m_minusChip  = nullptr;   // null when minusCount() == 0
    QLabel* m_plusChip   = nullptr;   // null when plusCount() == 0

    QFrame* m_sep1 = nullptr;
    QFrame* m_sep2 = nullptr;

    // ---- Body (buildBody) -- kind-dependent: m_proposal.kind is fixed
    // for this instance's lifetime, so exactly one of the block-label-
    // row pair / the diff-line vectors is ever populated; the rest stay
    // null/empty and restyleTheme() no-ops on them via null/empty checks.
    QWidget* m_blockDot   = nullptr;
    QLabel*  m_blockLabel = nullptr;
    QVector<QLabel*> m_diffAdditionLines;   // "+" lines (successLight text / success-tint bg)
    QVector<QLabel*> m_diffRemovalLines;    // "\xE2\x88\x92" lines (error text / errorStrong-tint bg)
    QLabel* m_moreLabel = nullptr;          // insert_chunk's "\xE2\x80\xA6 N more", if truncated

    // ---- Footer (buildFooter) -- status-dependent: m_proposal.status is
    // likewise fixed, so at most one non-null path below is ever live.
    QPushButton*  m_applyBtn  = nullptr;
    QPushButton*  m_rejectBtn = nullptr;
    // Round-2 P1: the applied-state Undo button must be disable-able by
    // setActionsEnabled just like Apply/Reject -- it drives the same
    // scene-mutating ViewportBridge::undo() path.
    QPushButton*  m_undoBtn   = nullptr;
    // The single-line status text for the applied/rejected/conflict
    // footers ("Applied", "Rejected", "Conflict") -- which color formula
    // restyleTheme() re-applies is recorded once at build time via the
    // enum below, since the three statuses use three different tokens.
    QLabel* m_footerStatusLabel = nullptr;
    enum class FooterStatusColor { None, Success, Faint, Warn };
    FooterStatusColor m_footerStatusColor = FooterStatusColor::None;
    QWidget* m_footerWidget = nullptr;
};

#endif // PROPOSALCARD_H
