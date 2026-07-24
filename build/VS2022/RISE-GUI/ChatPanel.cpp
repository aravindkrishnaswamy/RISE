//////////////////////////////////////////////////////////////////////
//
//  ChatPanel.cpp - Windows Qt chat panel implementation.
//
//////////////////////////////////////////////////////////////////////

#include "ChatPanel.h"

#include "ViewportBridge.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <utility>

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

    // GUI stage 3 (Windows parity, chat-display enrichment): cap a
    // tool-call detail payload (args JSON or a raw result line) to
    // ~4KB for the expandable detail view -- mirrors
    // ChatViewModel.cappedForDetail (Swift).  QString::size() counts
    // UTF-16 code units, not Characters/graphemes the way Swift's
    // String.count does, so a cut could in principle land inside a
    // surrogate pair for an astral codepoint; accepted per the task
    // brief ("QString::fromUtf8 then truncate by QString size is
    // safe") since this truncates an already-decoded QString that is
    // only ever displayed, never re-encoded onto the wire -- a split
    // surrogate renders as one replacement glyph at worst, not a crash
    // or a corrupted request.
    QString cappedForDetail(const QString& s, int limit = 4096)
    {
        if (s.size() <= limit) return s;
        return s.left(limit) + QStringLiteral("...[truncated]");
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

namespace {

// Agent autonomy selector (2026-07 GUI composer chips): QSettings key,
// shared between the constructor's read and setAutonomyLevel()'s
// write.  Same key name as the Mac client's UserDefaults key
// ("agentAutonomyLevel") -- not load-bearing (each platform's settings
// store is separate), just a nice cross-platform naming consistency.
const char* const kAutonomyLevelSettingsKey = "agentAutonomyLevel";

// Returns `c` with its alpha channel replaced by `alphaFraction` (0..1) --
// a small live-token-friendly stand-in for the QColor(r,g,b,alpha)
// reconstructions scattered through this file's styling code, so an
// alpha-blended fill is always computed from whatever Theme:: token value
// is CURRENT at call time (never a value frozen at some earlier point).
QColor withAlpha(const QColor& c, double alphaFraction)
{
    QColor result = c;
    result.setAlphaF(alphaFraction);
    return result;
}

// LIVE THEME-SWITCH CONTRACT (Theme.h): factored out of makePillButton
// so ChatPanel::restyleTheme() can re-apply the SAME formula to
// m_retryBtn/m_resetBtn from the CURRENT token values without
// duplicating (and risking drifting) the stylesheet string.
QString pillButtonStyleSheet()
{
    return QStringLiteral(
        "QPushButton { color: %1; border: 1px solid %2; border-radius: 6px; padding: 3px 10px; background: transparent; }"
        "QPushButton:disabled { color: %3; border-color: %4; }"
        "QPushButton:hover:!disabled { border-color: %5; }")
        .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderStrong),
             Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight),
             Theme::hex(Theme::borderHover));
}

// Small bordered pill button shared by the header's Retry/Reset and the
// provider/model chips below -- mirrors the comp's hairline-bordered
// affordances (composerFooter's model chip, errorAffordances' pill
// buttons on the Mac side).
QPushButton* makePillButton(const QString& text, QWidget* parent)
{
    auto* btn = new QPushButton(text, parent);
    btn->setFont(Theme::sans(11));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);
    btn->setStyleSheet(pillButtonStyleSheet());
    return btn;
}

// LIVE THEME-SWITCH CONTRACT: factored out of the constructor's m_sendBtn
// setup so restyleTheme() can re-apply it. The enabled background is a
// live 16%-alpha tint of Theme::accent (computed via withAlpha() above,
// never a frozen literal); the disabled background used to be a bare
// Theme::whiteAlpha(6%) -- always white regardless of mode, wrong in
// Light -- now Theme::fillHover, the theme-aware "faint inactive fill"
// token (whiteAlpha(20) ~8% in Dark, blackAlpha(15) ~6% in Light, i.e.
// the same visual weight the old hardcoded 6% white aimed for in Dark).
QString sendButtonStyleSheet()
{
    return QStringLiteral(
        "QPushButton { color: %1; background-color: %2; border: none; border-radius: 7px; padding: 6px 12px; }"
        "QPushButton:disabled { color: %3; background-color: %4; }")
        .arg(Theme::hex(Theme::accentLight), Theme::rgba(withAlpha(Theme::accent, 0.16)),
             Theme::hex(Theme::textDisabled), Theme::rgba(Theme::fillHover));
}

// Bundled-SVG-icon disclosure chevron shared by buildThinkingRow's and
// buildToolRow's checkable QToolButtons -- replaces the native Fusion
// QStyle::PE_IndicatorArrow* triangle (setArrowType) with the same
// Lucide glyph pair the icon system ships (chevron-right / chevron-
// down), tinted to whatever token color the row's text uses.  Mirrors
// the Mac client's SF chevron.right/chevron.down swap on these two
// rows (ChatPanel.swift:556 thinking row, :595 tool row).
//
// One QIcon carries BOTH states (QIcon::Off = chevron-right, QIcon::On
// = chevron-down) so Qt's standard checkable-button icon lookup
// (QCommonStyle::drawControl(CE_ToolButtonLabel): State_On -> QIcon::On)
// swaps the glyph automatically as the button's checked state flips --
// the toggled() handlers below only need to keep updating the text /
// detail-visibility they already touched, not poke the icon.
QIcon disclosureChevronIcon(const QColor& color, int sizePx)
{
    QIcon result;
    static const qreal kDprs[] = { 1.0, 1.25, 1.5, 2.0 };
    for (qreal dpr : kDprs) {
        const QPixmap collapsed = Theme::iconPixmap(QStringLiteral("chevron-right"), sizePx, color, dpr);
        if (!collapsed.isNull()) result.addPixmap(collapsed, QIcon::Normal, QIcon::Off);
        const QPixmap expanded = Theme::iconPixmap(QStringLiteral("chevron-down"), sizePx, color, dpr);
        if (!expanded.isNull()) result.addPixmap(expanded, QIcon::Normal, QIcon::On);
    }
    return result;
}

// Phase-3 crispness, task B: slim, theme-aware scrollbars for the
// transcript QScrollArea and the tool-detail QTextEdit boxes inside
// buildToolRow -- replaces the stock Fusion scrollbar (a thick, boxy
// bar that doesn't match the comp's overlay-thin feel).  A SHARED
// string so both call sites (ChatPanel::restyleTheme() for the
// transcript scroll area, and buildToolRow() -- rebuilt per-row on
// every transcript refresh -- for each detail QTextEdit) read the
// SAME live tokens at build time rather than risking the two drifting
// apart: Theme::fillActive for the resting handle fill (already
// theme-aware -- whiteAlpha in Dark, blackAlpha in Light, see
// Theme.cpp's DarkPalette/LightPalette) and Theme::borderStrong for
// the hover fill (a hairline stronger in both palettes -- the same
// pair ViewportProperties' ScrubHandle-adjacent BoolPill-style
// widgets lean on for "resting fill / stronger hover fill").  No
// arrow buttons (add-line/sub-line height/width 0), transparent
// track, a slim 8px bar with a rounded 4px handle -- unobtrusive,
// overlay-thin, matching the Mac client's native NSScroller rather
// than a heavy desktop-chrome scrollbar.  Sibling helper:
// Theme::scrollBarStyleSheet() (Theme.cpp) serves the OTHER panels and
// deliberately uses a stronger resting token (borderStrong) because
// those surfaces include pure-white light-mode wells where fillActive
// is near-invisible; this transcript variant sits on bgPanel where
// fillActive reads correctly.  If you tweak the geometry (widths,
// radii, zero-size buttons), change BOTH helpers.  The caller APPENDS this
// (rather than replacing) onto its own QScrollArea/QTextEdit
// stylesheet string and sets it directly on that one widget, so the
// rule stays scoped to that widget's own stylesheet cascade and can't
// leak into any other panel's scrollbars.
QString transcriptScrollBarStyleSheet()
{
    return QStringLiteral(
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 0px; }"
        "QScrollBar::handle:vertical { background: %1; border-radius: 4px; min-height: 24px; }"
        "QScrollBar::handle:vertical:hover { background: %2; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: transparent; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QScrollBar:horizontal { background: transparent; height: 8px; margin: 0px; }"
        "QScrollBar::handle:horizontal { background: %1; border-radius: 4px; min-width: 24px; }"
        "QScrollBar::handle:horizontal:hover { background: %2; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: transparent; border: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }")
        .arg(Theme::rgba(Theme::fillActive), Theme::rgba(Theme::borderStrong));
}

// Phase-3 crispness, task A: the user chat bubble's uneven-rounded
// background.  Mirrors ChatPanel.swift:496-498's
//   .clipShape(UnevenRoundedRectangle(
//       topLeadingRadius: 12, bottomLeadingRadius: 12,
//       bottomTrailingRadius: 4, topTrailingRadius: 12))
// -- SwiftUI resolves leading/trailing against layout direction, and
// this app (like the Mac client) is fixed LTR, so topLeading/
// topTrailing/bottomLeading/bottomTrailing map 1:1 onto Qt's
// top-left/top-right/bottom-left/bottom-right: topLeft 12, topRight
// 12, bottomLeft 12, bottomRight 4.  The bottomRight corner is the
// small one -- the "tail" -- which reads correctly because the Mac
// bubble (like this one) is right-aligned in its row, so the small
// corner sits at the bubble's inner/reading-direction edge, not out
// at the panel's outer edge.  A plain QLabel + QSS `border-radius`
// can only express a single UNIFORM radius (Qt stylesheets have no
// per-corner border-radius-* longhand), hence this small QLabel
// subclass: paintEvent fills a hand-built QPainterPath with the
// per-corner radii above, then falls through to QLabel::paintEvent
// for the text.  Follows this file's existing no-Q_OBJECT
// file-local-widget convention (see ScrubHandle/BoolPill in
// ViewportProperties.cpp, ClickableRow in OutlinerWidget.cpp).
//
// Theme::bgBubbleUser is read LIVE in paintEvent (LIVE THEME-SWITCH
// CONTRACT point 3, Theme.h) rather than baked once at construction,
// so a theme switch repaints the correct fill the instant Qt's own
// post-palette-change update() fires -- BEFORE restyleTheme()'s
// queued rebuildTranscriptWidgets() even runs, avoiding a stale-color
// flash while that queued rebuild is pending.  The text color is set
// once via a QSS `color:` at construction time; that's fine per the
// SAME contract point, because -- unlike the background fill -- there
// is no live-paint alternative for QLabel's own text color, and this
// whole row is torn down and rebuilt wholesale by
// rebuildTranscriptWidgets() on every theme switch anyway (queued
// from restyleTheme()), the same mechanism every other transcript row
// already relies on for its baked colors.
class BubbleLabel : public QLabel
{
public:
    explicit BubbleLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(text, parent)
    {
        // Background is painted by paintEvent below -- QSS only carries
        // the text color plus a transparent background so QLabel's own
        // stock rect-fill (from border-radius/background-color) doesn't
        // fight the custom uneven shape.
        setStyleSheet(QStringLiteral("QLabel { background: transparent; color: %1; }")
            .arg(Theme::hex(Theme::textPrimary)));
        setContentsMargins(13, 10, 13, 10);
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        // The fill painter must END before QLabel::paintEvent runs --
        // QLabel constructs its own QPainter on this widget for the text,
        // and Qt allows only one active painter per paint device (a live
        // one here would make the base's begin() fail and silently skip
        // the text). Hence the explicit brace scope.
        {
            QPainter p(this);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::bgBubbleUser);

            const QRectF r(rect());
            constexpr qreal tl = 12.0, tr = 12.0, br = 4.0, bl = 12.0;

            QPainterPath path;
            path.moveTo(r.left() + tl, r.top());
            path.lineTo(r.right() - tr, r.top());
            path.arcTo(r.right() - 2 * tr, r.top(), 2 * tr, 2 * tr, 90, -90);
            path.lineTo(r.right(), r.bottom() - br);
            path.arcTo(r.right() - 2 * br, r.bottom() - 2 * br, 2 * br, 2 * br, 0, -90);
            path.lineTo(r.left() + bl, r.bottom());
            path.arcTo(r.left(), r.bottom() - 2 * bl, 2 * bl, 2 * bl, -90, -90);
            path.lineTo(r.left(), r.top() + tl);
            path.arcTo(r.left(), r.top(), 2 * tl, 2 * tl, 180, -90);
            path.closeSubpath();

            p.drawPath(path);
        }
        QLabel::paintEvent(e);
    }
};

// Recursively empties `layout`, deleteLater()-ing every widget it
// directly owned and destroying every nested QLayout it wrapped (e.g.
// a right-align QHBoxLayout added via addLayout()).  takeAt() only
// detaches the QLayoutItem -- it neither deletes the widget nor the
// nested layout, so both must be handled explicitly here.
void clearQLayoutRecursive(QLayout* layout)
{
    if (!layout) return;
    QLayoutItem* item;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) {
            w->deleteLater();
        } else if (QLayout* child = item->layout()) {
            clearQLayoutRecursive(child);
            delete child;
        }
        delete item;
    }
}

} // namespace

ChatPanel::ChatPanel(QWidget* parent)
    : QWidget(parent)
    , m_loop(new AgentChatLoop())
{
    m_network = new QNetworkAccessManager(this);
    // P2-6: mirror the Mac driver's URLRequest.timeoutInterval = 300 --
    // without this a stalled connection blocks the turn (and the render-
    // wait poll queued behind it) indefinitely.
    m_network->setTransferTimeout(300000);

    // Window fill: styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT
    // (Theme.h). setAutoFillBackground(true) is structural and stays here.
    setAutoFillBackground(true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 14, 14);
    outer->setSpacing(9);

    auto* header = new QHBoxLayout();
    header->addStretch();
    m_retryBtn = makePillButton(QStringLiteral("\xE2\x86\xBB Retry"), this);
    m_retryBtn->setToolTip("Retry the last request (after an HTTP/network error)");
    m_retryBtn->setEnabled(false);
    header->addWidget(m_retryBtn);
    m_resetBtn = makePillButton(QStringLiteral("\xE2\x8C\xAB Reset"), this);
    m_resetBtn->setToolTip("Clear the conversation");
    header->addWidget(m_resetBtn);
    outer->addLayout(header);

    auto* providerRow = new QHBoxLayout();
    providerRow->setSpacing(6);
    m_providerCombo = new QComboBox();
    m_providerCombo->addItem("OpenAI");
    m_providerCombo->addItem("Anthropic");
    m_providerCombo->addItem("Gemini");
    m_providerCombo->addItem("Grok (xAI)");
    m_providerCombo->addItem("Local (Ollama)");
    // Default provider = Local (Ollama) with the "opencoder" model (user
    // decision 2026-07-16) -- keyless out-of-box; combo order matches the
    // Provider enum 1:1.
    m_providerCombo->setCurrentIndex(static_cast<int>(Provider::Local));
    m_providerCombo->setFont(Theme::mono(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_modelEdit = new QLineEdit(defaultModelFor(Provider::Local));
    m_modelEdit->setPlaceholderText("model");
    m_modelEdit->setFont(Theme::mono(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    providerRow->addWidget(m_providerCombo);
    providerRow->addWidget(m_modelEdit, 1);
    outer->addLayout(providerRow);

    m_apiKeyEdit = new QLineEdit();
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText("API key");
    m_apiKeyEdit->setFont(Theme::mono(10));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_apiKeyEdit->setText(envKeyFor(Provider::Local));
    outer->addWidget(m_apiKeyEdit);

    // Secure-MCP slice 5c (Windows parity, RISE UI redesign): pending/
    // recently-resolved proposals column -- above the transcript,
    // mirrors ChatPanel.swift's ProposalsPanel placement.
    m_proposalsContainer = new QWidget(this);
    m_proposalsLayout = new QVBoxLayout(m_proposalsContainer);
    m_proposalsLayout->setContentsMargins(0, 0, 0, 0);
    m_proposalsLayout->setSpacing(6);
    m_proposalsContainer->setVisible(false);
    outer->addWidget(m_proposalsContainer);

    m_proposalsPollTimer = new QTimer(this);
    m_proposalsPollTimer->setInterval(1000);
    connect(m_proposalsPollTimer, &QTimer::timeout, this, &ChatPanel::refreshProposals);

    // Transcript -- scrollable column of per-entry widgets restyled to
    // the comp (bubbles / plain narration / trace chips); see
    // rebuildTranscriptWidgets().
    m_transcriptContent = new QWidget();
    m_transcriptLayout = new QVBoxLayout(m_transcriptContent);
    m_transcriptLayout->setContentsMargins(2, 2, 2, 2);
    m_transcriptLayout->setSpacing(15);

    m_transcriptScroll = new QScrollArea();
    m_transcriptScroll->setWidget(m_transcriptContent);
    m_transcriptScroll->setWidgetResizable(true);
    m_transcriptScroll->setFrameShape(QFrame::NoFrame);
    m_transcriptScroll->setMinimumHeight(160);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_transcriptContent->setAutoFillBackground(true);
    // Palette fill: styled in restyleTheme() -- LIVE THEME-SWITCH
    // CONTRACT (Theme.h).
    outer->addWidget(m_transcriptScroll, 1);

    // Composer -- input well + accent Send pill, per the comp.
    m_composerWell = new QWidget(this);
    m_composerWell->setObjectName(QStringLiteral("chatComposerWell"));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    auto* inputRow = new QHBoxLayout(m_composerWell);
    inputRow->setContentsMargins(12, 6, 6, 6);
    inputRow->setSpacing(8);

    m_inputEdit = new QLineEdit();
    m_inputEdit->setPlaceholderText(QStringLiteral("Ask the agent \xE2\x80\x94 reads, edits, validates, renders\xE2\x80\xA6"));
    m_inputEdit->setFont(Theme::sans(12));
    m_inputEdit->setFrame(false);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    inputRow->addWidget(m_inputEdit, 1);

    m_sendBtn = new QPushButton(QStringLiteral("Send"));
    m_sendBtn->setFont(Theme::sans(11, QFont::DemiBold));
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setFlat(true);
    m_sendBtn->setStyleSheet(sendButtonStyleSheet());
    inputRow->addWidget(m_sendBtn);

    m_stopBtn = new QPushButton(QStringLiteral("Stop"));
    m_stopBtn->setFont(Theme::sans(11, QFont::DemiBold));
    m_stopBtn->setCursor(Qt::PointingHandCursor);
    m_stopBtn->setFlat(true);
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    inputRow->addWidget(m_stopBtn);

    outer->addWidget(m_composerWell);

    // ---- Agent autonomy selector (2026-07 GUI composer chips) ---------
    // "Read" / "Propose" / "Apply" -- mirrors the design comp's L0/L1/L2
    // chips (ChatPanel.swift's composerFooter autonomySelector).  Placed
    // in its own row just below the composer, matching the Mac layout's
    // "footer strip beneath the input" positioning.
    auto* autonomyRow = new QHBoxLayout();
    autonomyRow->setSpacing(10);
    autonomyRow->addStretch(1);
    m_autonomyReadBtn = makeAutonomyChip(tr("Read"), AutonomyLevel::Read,
        tr("Read: agent can read the scene and render, never edit."));
    m_autonomyProposeBtn = makeAutonomyChip(tr("Propose"), AutonomyLevel::Propose,
        tr("Propose: edits are staged as proposals for your review."));
    m_autonomyApplyBtn = makeAutonomyChip(tr("Apply"), AutonomyLevel::Apply,
        tr("Apply: edits apply directly — still one undo step each."));
    autonomyRow->addWidget(m_autonomyReadBtn);
    autonomyRow->addWidget(m_autonomyProposeBtn);
    autonomyRow->addWidget(m_autonomyApplyBtn);
    outer->addLayout(autonomyRow);

    // Restore the persisted choice (default Apply -- see AutonomyLevel's
    // header doc for why Propose is NOT the default).  Reads the raw int
    // and validates the range rather than trusting an out-of-range value
    // from a hand-edited or corrupted settings file.
    {
        QSettings settings;
        const int storedRaw = settings.value(QLatin1String(kAutonomyLevelSettingsKey),
            static_cast<int>(AutonomyLevel::Apply)).toInt();
        m_autonomyLevel = (storedRaw == static_cast<int>(AutonomyLevel::Read)
                            || storedRaw == static_cast<int>(AutonomyLevel::Propose)
                            || storedRaw == static_cast<int>(AutonomyLevel::Apply))
            ? static_cast<AutonomyLevel>(storedRaw)
            : AutonomyLevel::Apply;
    }
    updateAutonomyChipsStyle();

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

    // GUI stage 3 (Windows parity, chat-display enrichment): the
    // transcript verbosity toggle -- mirrors the Mac ChatSettingsView
    // "Detailed transcript" toggle (@AppStorage "agentChatDetailedTranscript"),
    // persisted under this platform's own QSettings key
    // ("agentChat/detailedTranscript", default false) -- not shared
    // storage with the Mac client, just naming parity.  Off (default):
    // reasoning rows append collapsed and tool-call detail always stays
    // collapsed regardless of this setting.  On: reasoning rows append
    // pre-expanded.
    {
        QSettings settings;
        m_detailedTranscript =
            settings.value("agentChat/detailedTranscript", false).toBool();
    }
    m_detailedTranscriptCheck = new QCheckBox("Detailed transcript (auto-expand thinking)");
    m_detailedTranscriptCheck->setChecked(m_detailedTranscript);
    m_detailedTranscriptCheck->setToolTip(
        "Off (default): reasoning is collapsed to a word-count summary and "
        "tool-call detail (args + result) stays collapsed behind a chevron. "
        "On: reasoning starts expanded. Tool-call detail always starts "
        "collapsed either way -- click its chevron to inspect one call.");
    outer->addWidget(m_detailedTranscriptCheck);

    m_statusLabel = new QLabel();
    m_statusLabel->setFont(Theme::sans(11));
    // Styled in restyleTheme() -- LIVE THEME-SWITCH CONTRACT (Theme.h).
    m_statusLabel->setWordWrap(true);
    outer->addWidget(m_statusLabel);

    connect(m_recordTrajectoriesCheck, &QCheckBox::toggled,
            this, &ChatPanel::recordTrajectoriesToggled);
    connect(m_detailedTranscriptCheck, &QCheckBox::toggled,
            this, &ChatPanel::detailedTranscriptToggled);
    connect(m_providerCombo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &ChatPanel::providerChanged);
    connect(m_modelEdit, &QLineEdit::editingFinished,
            this, &ChatPanel::modelEditingFinished);
    // Start screen readiness (spec §6): agentConfigured() reads the
    // applied provider + the key field's live text.  The key field
    // covers every keystroke; applyProviderToLoop() (below) emits
    // explicitly for the applied-provider transition itself, since
    // QLineEdit::setText is a no-op-and-no-signal when the incoming key
    // happens to match what's already shown (e.g. switching between two
    // providers that both currently have an empty stashed key) -- that
    // case must still be able to flip agentConfigured() (Local is
    // keyless; every other provider isn't).
    connect(m_apiKeyEdit, &QLineEdit::textChanged,
            this, [this](const QString&) { emit agentConfiguredChanged(); });
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

    // LIVE THEME-SWITCH CONTRACT (Theme.h): re-applies every token-
    // dependent style above from the CURRENT Theme:: values (a harmless
    // redundant re-write at construction time, since the widgets above
    // were already built from those same CURRENT values) and installs
    // this as the single source of truth changeEvent() re-invokes on
    // every QEvent::PaletteChange.
    m_themeReady = true;
    restyleTheme();
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

// ============================================================
// LIVE THEME-SWITCH CONTRACT (Theme.h) -- see MainWindow::restyleTheme
// for the reference implementation this mirrors.
// ============================================================

void ChatPanel::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void ChatPanel::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();
    // See the header doc for the full list of what this covers and what
    // it deliberately leaves to other widgets' own contract compliance
    // (ProposalCard instances, native-palette-driven QCheckBoxes).

    {
        // setPalette() is an explicit per-widget override (set in the
        // constructor via setAutoFillBackground(true)), so it does NOT
        // automatically track QApplication::setPalette() -- re-applied
        // here from the current token, same reasoning as every
        // QPalette::Window fill in MainWindow::restyleTheme().
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        setPalette(pal);
    }

    if (m_retryBtn) m_retryBtn->setStyleSheet(pillButtonStyleSheet());
    if (m_resetBtn) m_resetBtn->setStyleSheet(pillButtonStyleSheet());

    if (m_providerCombo) {
        m_providerCombo->setStyleSheet(QStringLiteral(
            "QComboBox { color: %1; background: transparent; border: 1px solid %2; border-radius: 5px; padding: 3px 8px; }")
            .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderLight)));
    }
    if (m_modelEdit) {
        m_modelEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { color: %1; background: transparent; border: 1px solid %2; border-radius: 5px; padding: 3px 8px; }")
            .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderLight)));
    }
    if (m_apiKeyEdit) {
        m_apiKeyEdit->setStyleSheet(QStringLiteral(
            "QLineEdit { color: %1; background: %2; border: 1px solid %3; border-radius: 6px; padding: 4px 8px; }")
            .arg(Theme::hex(Theme::textSecondary), Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight)));
    }

    if (m_transcriptScroll) {
        // Task B (Phase-3 crispness): slim themed scrollbar, appended
        // onto the QScrollArea's own background rule -- see
        // transcriptScrollBarStyleSheet()'s doc for why this is scoped
        // to just this widget and shares its live tokens with
        // buildToolRow()'s detail QTextEdit boxes below.
        m_transcriptScroll->setStyleSheet(QStringLiteral("QScrollArea { background-color: %1; }")
            .arg(Theme::hex(Theme::bgPanel)) + transcriptScrollBarStyleSheet());
    }
    if (m_transcriptContent) {
        QPalette pal = m_transcriptContent->palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        m_transcriptContent->setPalette(pal);
    }

    if (m_composerWell) {
        m_composerWell->setStyleSheet(QStringLiteral(
            "#chatComposerWell { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
            .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight))
            .arg(Theme::radiusCard));
    }
    if (m_inputEdit) {
        m_inputEdit->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; color: %1; border: none; }")
            .arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_sendBtn) m_sendBtn->setStyleSheet(sendButtonStyleSheet());
    if (m_stopBtn) {
        m_stopBtn->setStyleSheet(QStringLiteral(
            "QPushButton { color: %1; background: transparent; border: 1px solid %2; border-radius: 7px; padding: 6px 12px; }")
            .arg(Theme::hex(Theme::error), Theme::hex(Theme::borderStrong)));
    }

    // Agent autonomy selector: already reads live tokens (Theme::accentLight
    // / Theme::textDim) from m_autonomyLevel -- just re-invoke.
    updateAutonomyChipsStyle();

    if (m_statusLabel) {
        m_statusLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }

    // Secure-MCP proposals column: each live ProposalCard restyles itself
    // (its own restyleTheme()/changeEvent -- see ProposalCard.cpp), but
    // the "-> joins the shared undo history" note labels are parented
    // directly by this panel and need their own re-apply.
    for (QLabel* note : std::as_const(m_proposalNoteLabels)) {
        if (note) note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }

    // Transcript rows are torn down and rebuilt wholesale from m_loop's
    // transcript data (see rebuildTranscriptWidgets' doc) -- the simplest
    // way to refresh every row's baked colors plus the buildThinkingRow/
    // buildToolRow disclosure chevrons' Theme::iconPixmap tints, which
    // are baked at row-build time rather than read live in a paintEvent.
    // QUEUED per Theme.h's LIVE THEME-SWITCH CONTRACT point 5 (2026-07-23
    // round-2 review fix): a long conversation's transcript is unbounded,
    // and rebuilding it synchronously inside the PaletteChange cascade
    // both violates the "creates no widgets" rule (contract point 2) and
    // stutters the GUI thread on a theme flip.  The 3-arg context-object
    // form auto-cancels if this panel is destroyed first.
    QTimer::singleShot(0, this, [this]() {
        rebuildTranscriptWidgets();
    });
}

// ============================================================
// Agent autonomy selector (2026-07 GUI composer chips)
// ============================================================

QPushButton* ChatPanel::makeAutonomyChip(const QString& title, AutonomyLevel level, const QString& help)
{
    auto* btn = new QPushButton(title, this);
    btn->setFont(Theme::mono(10));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFlat(true);
    btn->setToolTip(help);
    connect(btn, &QPushButton::clicked, this, [this, level]() { setAutonomyLevel(level); });
    return btn;
}

void ChatPanel::updateAutonomyChipsStyle()
{
    auto style = [](QPushButton* btn, bool active) {
        if (!btn) return;
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; padding: 0; color: %1; font-weight: %2; }")
            .arg(active ? Theme::hex(Theme::accentLight) : Theme::hex(Theme::textDim))
            .arg(active ? QStringLiteral("600") : QStringLiteral("400")));
    };
    style(m_autonomyReadBtn,    m_autonomyLevel == AutonomyLevel::Read);
    style(m_autonomyProposeBtn, m_autonomyLevel == AutonomyLevel::Propose);
    style(m_autonomyApplyBtn,   m_autonomyLevel == AutonomyLevel::Apply);
}

// DEFAULT IS Apply, matching today's actual behaviour byte-for-byte:
// the in-app chat's own tool calls have ALWAYS committed directly
// (Owner authority + Commit autonomy) -- the existing "staged
// proposal" path (ProposalCard / onProposalApplyClicked etc.) is fed
// ONLY by proposals an EXTERNAL MCP client staged, never by this
// in-app chat driver.  So Propose is a genuinely NEW behaviour for the
// in-app chat, not a re-labeling of something already true by default
// -- defaulting to it would silently change what happens when a user
// who has never touched this control sends a message, which the
// feature brief explicitly rules out.  See the header's AutonomyLevel
// doc for the numeric-value mirror of ViewportBridge::AgentAutonomyLevel.
void ChatPanel::setAutonomyLevel(AutonomyLevel level)
{
    m_autonomyLevel = level;
    QSettings settings;
    settings.setValue(QLatin1String(kAutonomyLevelSettingsKey), static_cast<int>(level));
    if (m_bridge) {
        m_bridge->setAgentAutonomyLevel(static_cast<ViewportBridge::AgentAutonomyLevel>(level));
    }
    updateAutonomyChipsStyle();
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
    m_sceneEditableExternal = (bridge != nullptr);
    recomputeSceneEditable();
    if (m_bridge) {
        // Agent autonomy selector: a fresh bridge's dispatchers default
        // to Apply (see ViewportBridge.h's doc) until told otherwise --
        // apply THIS panel's persisted choice immediately so a scene
        // switch never silently resets a user's Read/Propose selection
        // back to Apply.
        m_bridge->setAgentAutonomyLevel(static_cast<ViewportBridge::AgentAutonomyLevel>(m_autonomyLevel));
        fetchSkillIndex();
        // Secure-MCP slice 5c (Windows parity): a freshly-opened scene
        // never inherits a stale proposals list from whatever was
        // showing for the previous scene.
        m_resolvedProposalObservedAt.clear();
        refreshProposals();
        m_proposalsPollTimer->start();
        // Eval-harness E1: a freshly-bound scene starts a NEW trajectory
        // file (skills are set above, so the session record captures the
        // full system prompt on the first user message).
        startTrajectory();

        // Start-screen create path (spec §7): consume the pending first
        // prompt EXACTLY ONCE, and only here -- after the fresh bridge is
        // attached and the chat state reset above, the same point a typed
        // message could first be sent.  Stashing-then-consuming (vs
        // sending directly from the start screen) is what makes the
        // handoff immune to the slow-load race: however long the scene
        // load took, the prompt can neither fire early (no bridge until
        // now) nor double-fire (cleared before send).  Mirrors macOS
        // ChatViewModel.sceneOpened's tail.
        if (!m_pendingFirstPrompt.isEmpty()) {
            const QString prompt = m_pendingFirstPrompt;
            m_pendingFirstPrompt.clear();
            m_inputEdit->setText(prompt);
            sendMessage();
        }
    } else {
        m_proposalsPollTimer->stop();
        m_resolvedProposalObservedAt.clear();
        rebuildProposalsUI({});
        m_loop->Reset();
        // Detach so no file lingers between scenes.
        startTrajectory();
        refreshTranscript();
    }
    updateButtonStates();
}

void ChatPanel::setSceneEditable(bool editable)
{
    // Only the EXTERNAL half of the gate lives here -- MainWindow calls
    // this off production-render state, and requestStop()'s
    // "cancel the turn because the scene just became unsafe" behavior
    // must fire ONLY for that transition, never merely because our own
    // outstanding chat-render job flipped (recomputeSceneEditable()
    // handles that half without touching m_busy/requestStop -- see its
    // doc).
    m_sceneEditableExternal = editable && m_bridge != nullptr;
    if (!m_sceneEditableExternal && m_busy) {
        requestStop();
    }
    recomputeSceneEditable();
}

void ChatPanel::recomputeSceneEditable()
{
    // P1-2 fix: the effective gate is now the AND of two independent
    // inputs -- mirrors Mac's canUseSceneTransport, which also ANDs the
    // production-render gate with "no chat-driven render outstanding".
    // Deliberately does NOT call requestStop() here even when this
    // flips true->false: that happens when OUR OWN render tool call
    // just went outstanding (see setOutstandingRenderJobId), and
    // cancelling the turn that just started its own render would be
    // self-defeating. setSceneEditable() above is the only path that
    // may cancel the turn, and only for the external-gate transition.
    m_sceneEditable = m_sceneEditableExternal && !isChatRenderOutstanding();
    // Reflect the new gate on every live proposal card immediately --
    // don't wait for the next 1s poll tick, matching the Mac card's live
    // `applyRejectDisabled` binding.
    updateProposalCardsEnabled();
    updateButtonStates();
}

void ChatPanel::setOutstandingRenderJobId(quint64 jobId)
{
    if (m_outstandingRenderJobId == jobId) return;
    m_outstandingRenderJobId = jobId;
    recomputeSceneEditable();
    emit chatRenderOutstandingChanged();
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
        cfg.defaultModelId = "opencoder";
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

bool ChatPanel::agentConfigured() const
{
    // Start screen readiness (spec §6): the SAME condition sendMessage()
    // gates a send on, inverted to a positive "ready" reading -- reusing
    // it rather than a second source of truth.  m_appliedProvider (not
    // the combo's possibly-mid-edit selection) matches what an actual
    // send would evaluate right now.
    return !providerRequiresApiKey(m_appliedProvider) || !m_apiKeyEdit->text().trimmed().isEmpty();
}

QString ChatPanel::currentProviderDisplayName() const
{
    return m_providerCombo ? m_providerCombo->itemText(static_cast<int>(m_appliedProvider)) : QString();
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

    // Start screen readiness (spec §6): the applied provider just changed
    // (or was (re)confirmed) -- explicit emit because the key-field
    // textChanged connection above can miss this transition (setText is a
    // no-op-and-no-signal when the incoming key equals what's already
    // shown).
    emit agentConfiguredChanged();
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
    rebuildTranscriptWidgets();
    m_statusLabel->setText(statusLine);
}

void ChatPanel::clearLayout(QVBoxLayout* layout)
{
    clearQLayoutRecursive(layout);
}

// ============================================================
// GUI stage 3 (Windows parity, chat-display enrichment): collapsible
// row builders -- see the header doc on each for the shared contract.
// ============================================================

QWidget* ChatPanel::buildThinkingRow(const QString& reasoningText, bool expandedByDefault)
{
    auto* container = new QWidget(m_transcriptContent);
    auto* vlayout = new QVBoxLayout(container);
    vlayout->setContentsMargins(0, 0, 0, 0);
    vlayout->setSpacing(4);

    // Word count for the collapsed label -- QString::simplified()
    // collapses every run of whitespace (space/tab/newline) to a
    // single space and trims the ends, so counting spaces + 1 gives
    // the word count without pulling in QRegularExpression.
    const QString simplified = reasoningText.simplified();
    const int wordCount = simplified.isEmpty() ? 0 : simplified.count(QLatin1Char(' ')) + 1;
    const QString collapsedLabel = (wordCount == 1)
        ? tr("Thinking (1 word)")
        : tr("Thinking (%1 words)").arg(wordCount);
    const QString expandedLabel = tr("Thinking");

    auto* chevron = new QToolButton(container);
    chevron->setCheckable(true);
    chevron->setChecked(expandedByDefault);
    chevron->setAutoRaise(true);
    chevron->setCursor(Qt::PointingHandCursor);
    chevron->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    chevron->setIcon(disclosureChevronIcon(Theme::textDim, 9));
    chevron->setIconSize(QSize(9, 9));
    chevron->setText(expandedByDefault ? expandedLabel : collapsedLabel);
    QFont chevronFont = Theme::sans(11);
    chevronFont.setItalic(true);
    chevron->setFont(chevronFont);
    chevron->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: none; background: transparent; padding: 0; }")
        .arg(Theme::hex(Theme::textDim)));
    vlayout->addWidget(chevron);

    auto* detail = new QLabel(reasoningText, container);
    detail->setFont(Theme::sans(11));
    detail->setWordWrap(true);
    detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    detail->setContentsMargins(13, 0, 0, 0);
    detail->setVisible(expandedByDefault);
    vlayout->addWidget(detail);

    // Both widgets are children of `container`; when it (or an
    // ancestor) is torn down by clearLayout(), chevron dies with it and
    // Qt disconnects this lambda automatically -- no dangling capture.
    connect(chevron, &QToolButton::toggled, container,
            [chevron, detail, collapsedLabel, expandedLabel](bool checked) {
        // Icon swap is automatic (see disclosureChevronIcon's doc) --
        // only the text label and detail visibility need updating here.
        chevron->setText(checked ? expandedLabel : collapsedLabel);
        detail->setVisible(checked);
    });

    return container;
}

QWidget* ChatPanel::buildToolRow(const QString& headerText, const QString& detailText,
                                  bool hasDetail, bool startExpanded,
                                  QLabel** outLabel, QTextEdit** outDetailEdit,
                                  QToolButton** outChevron)
{
    auto* container = new QWidget(m_transcriptContent);
    auto* vlayout = new QVBoxLayout(container);
    vlayout->setContentsMargins(0, 0, 0, 0);
    vlayout->setSpacing(5);

    // The hairline-bordered chip -- same look as the pre-feature trace
    // chip when hasDetail is false (chevron hidden), per the "rows
    // without detail render exactly as today" contract.
    auto* chip = new QWidget(container);
    chip->setStyleSheet(QStringLiteral("border: 1px solid %1; border-radius: 7px;")
        .arg(Theme::hex(Theme::borderHairline)));
    auto* chipLayout = new QHBoxLayout(chip);
    chipLayout->setContentsMargins(10, 6, 10, 6);
    chipLayout->setSpacing(7);

    auto* check = new QLabel(QStringLiteral("\xE2\x9C\x93"), chip);
    check->setFont(Theme::mono(10));
    check->setStyleSheet(QStringLiteral("color: %1; border: none;").arg(Theme::hex(Theme::success)));
    chipLayout->addWidget(check);

    auto* label = new QLabel(headerText, chip);
    label->setFont(Theme::mono(10));
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color: %1; border: none;").arg(Theme::hex(Theme::textMuted)));
    chipLayout->addWidget(label, 1);

    auto* chevron = new QToolButton(chip);
    chevron->setCheckable(true);
    chevron->setChecked(startExpanded);
    chevron->setAutoRaise(true);
    chevron->setCursor(Qt::PointingHandCursor);
    chevron->setFixedSize(16, 16);
    chevron->setIcon(disclosureChevronIcon(Theme::textMuted, 10));
    chevron->setIconSize(QSize(10, 10));
    chevron->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    chevron->setVisible(hasDetail);
    chevron->setEnabled(hasDetail);
    chipLayout->addWidget(chevron);

    vlayout->addWidget(chip);

    auto* detail = new QTextEdit(container);
    detail->setReadOnly(true);
    detail->setPlainText(detailText);
    detail->setFont(Theme::mono(9));
    detail->setFixedHeight(120);
    // Task B (Phase-3 crispness): same slim themed scrollbar as the
    // transcript QScrollArea (transcriptScrollBarStyleSheet(), shared
    // helper so both read the same live tokens at build time) -- this
    // row is rebuilt per-call by rebuildTranscriptWidgets(), so the
    // tokens baked in here are already current on every theme switch.
    detail->setStyleSheet(QStringLiteral(
        "QTextEdit { color: %1; background-color: %2; border: none; border-radius: 6px; }")
        .arg(Theme::hex(Theme::textMuted), Theme::hex(Theme::bgPanel)) + transcriptScrollBarStyleSheet());
    detail->setVisible(hasDetail && startExpanded);
    vlayout->addWidget(detail);

    // Same auto-disconnect-on-destroy reasoning as buildThinkingRow's
    // connection above -- chevron and detail are both children of
    // `container`, so they share its lifetime.
    connect(chevron, &QToolButton::toggled, container, [detail](bool checked) {
        // Icon swap is automatic (see disclosureChevronIcon's doc) --
        // only detail visibility needs updating here.
        if (detail) detail->setVisible(checked);
    });

    if (outLabel) *outLabel = label;
    if (outDetailEdit) *outDetailEdit = detail;
    if (outChevron) *outChevron = chevron;
    return container;
}

void ChatPanel::updatePendingToolRow(const RISE::Agent::ChatToolCall& call,
                                      const std::string& responseLine)
{
    // QPointer GUARD: every OTHER tool call's dispatch-to-result path is
    // fully synchronous C++ (no event-loop turn in between), so the row
    // built at dispatch time is still exactly what it was. `render`
    // calls are the one exception -- they suspend across the async
    // submit/poll state machine (startAsyncRenderToolCall /
    // pollOutstandingRender), a genuine event-loop gap during which a
    // Stop / production-render-start / provider-switch can call
    // finishBusy()/refreshTranscript(), which clearLayout()s (i.e.
    // deleteLater()s) every live transcript widget, this row included.
    // A raw pointer would dangle the moment that deferred deletion
    // actually runs; QPointer auto-nulls instead, so this check is a
    // real guard against a stale write, not decoration.
    if (!m_pendingToolRowLabel) {
        m_pendingToolRowDetail = nullptr;
        m_pendingToolRowChevron = nullptr;
        return;
    }

    // The SAME deterministic one-line outcome summary
    // FlushPendingToolResults uses for the wire-side
    // ChatToolDisplaySummary::outcomeLine, so this row and the eventual
    // authoritative rebuild (from m_loop's transcript) never disagree.
    const QString outcome = toQString(AgentChatLoop::ToolOutcomeLineForDisplay(call, responseLine));
    m_pendingToolRowLabel->setText(
        QStringLiteral("\xE2\x86\x92 ") + toQString(call.name) + QStringLiteral("  ") + outcome);

    if (m_pendingToolRowDetail) {
        m_pendingToolRowDetail->setPlainText(
            QStringLiteral("Args:\n") + cappedForDetail(toQString(call.argsJson))
            + QStringLiteral("\n\nResult:\n") + cappedForDetail(toQString(responseLine)));
    }
    if (m_pendingToolRowChevron) {
        m_pendingToolRowChevron->setVisible(true);
        m_pendingToolRowChevron->setEnabled(true);
    }

    m_pendingToolRowLabel = nullptr;
    m_pendingToolRowDetail = nullptr;
    m_pendingToolRowChevron = nullptr;
}

void ChatPanel::rebuildTranscriptWidgets()
{
    clearLayout(m_transcriptLayout);

    using Role = RISE::Agent::ChatTranscriptEntry::Role;

    if (m_loop->TranscriptSize() == 0) {
        auto* placeholder = new QLabel(QStringLiteral(
            "Describe a scene change \xE2\x80\x94 e.g. \xE2\x80\x9Cmake the orange objects red\xE2\x80\x9D."));
        placeholder->setFont(Theme::sans(12));
        placeholder->setWordWrap(true);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        m_transcriptLayout->addWidget(placeholder);
    }

    for (std::size_t i = 0; i < m_loop->TranscriptSize(); ++i) {
        const auto& entry = m_loop->TranscriptAt(i);
        const QString body = toQString(entry.displayText).trimmed();

        if (entry.role == Role::User) {
            // Right-aligned bubble -- Theme::bgBubbleUser fill, uneven
            // corners 12/12/4/12 (topLeft/topRight/bottomLeft/
            // bottomRight) via BubbleLabel's custom paintEvent -- see
            // that class's doc (cites ChatPanel.swift:496-498) for the
            // exact corner mapping.  Padding (13px horizontal, 10px
            // vertical) is baked into BubbleLabel's own contents
            // margins, matching the Mac bubble's `.padding(.horizontal,
            // 13).padding(.vertical, 10)`.
            auto* bubble = new BubbleLabel(body.isEmpty() ? tr("[no text]") : body);
            bubble->setFont(Theme::sans(12));
            bubble->setWordWrap(true);
            bubble->setMaximumWidth(320);
            auto* row = new QHBoxLayout();
            row->addStretch(1);
            row->addWidget(bubble, 0, Qt::AlignRight);
            m_transcriptLayout->addLayout(row);
        } else if (entry.role == Role::Assistant) {
            // GUI stage 3 (Windows parity): the model's reasoning for
            // THIS turn, if the provider exposed any (Anthropic
            // `thinking` blocks; OpenAI-family `reasoning`/
            // `reasoning_content`; "" on Gemini and a plain gpt-family
            // turn) -- rendered BEFORE the assistant's own narration,
            // mirroring ChatViewModel.driveTurn's reasoning-first
            // ordering.  entry.reasoningText is recorded onto this SAME
            // Assistant transcript entry by HandleResponse regardless of
            // whether the turn ended in tool calls or final text (see
            // AgentChatLoop.cpp), so this one spot covers both paths.
            if (!entry.reasoningText.empty()) {
                m_transcriptLayout->addWidget(
                    buildThinkingRow(toQString(entry.reasoningText), m_detailedTranscript));
            }
            auto* lbl = new QLabel(body.isEmpty() ? tr("[no text]") : body);
            lbl->setFont(Theme::sans(12));
            lbl->setWordWrap(true);
            lbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textSecondary)));
            m_transcriptLayout->addWidget(lbl);
        } else {
            // ToolResults -- one expandable trace chip PER CALL, built
            // from entry.toolSummaries (name/outcomeLine/argsJson/
            // resultJson, populated by FlushPendingToolResults) rather
            // than parsing entry.displayText: stage 1 (AgentChatLoop.cpp)
            // reformatted displayText to "name -> outcome (dot) name ->
            // outcome" and it no longer carries a "[tool results: ...]"
            // wrapper to strip.
            if (entry.toolSummaries.empty()) {
                // Defensive fallback -- every ToolResults entry gets
                // toolSummaries from FlushPendingToolResults, so this
                // should not happen; kept so an unexpected empty entry
                // still renders something instead of silently vanishing.
                const QString label = body.isEmpty() ? tr("tool results") : body;
                m_transcriptLayout->addWidget(buildToolRow(label, QString(), false, false));
            } else {
                for (const auto& summary : entry.toolSummaries) {
                    const QString header = QStringLiteral("\xE2\x86\x92 ") + toQString(summary.name)
                        + QStringLiteral("  ") + toQString(summary.outcomeLine);
                    const QString detail = QStringLiteral("Args:\n")
                        + cappedForDetail(toQString(summary.argsJson))
                        + QStringLiteral("\n\nResult:\n")
                        + cappedForDetail(toQString(summary.resultJson));
                    // Collapsed by default regardless of the "Detailed
                    // transcript" setting -- that toggle only seeds
                    // thinking rows (see buildThinkingRow's call site
                    // above); tool-call detail always starts collapsed.
                    m_transcriptLayout->addWidget(buildToolRow(header, detail, true, false));
                }
            }
        }
    }

    m_transcriptLayout->addStretch(1);

    // Auto-scroll to bottom once the new widgets have been laid out.
    QTimer::singleShot(0, this, [this]() {
        if (m_transcriptScroll) {
            if (QScrollBar* bar = m_transcriptScroll->verticalScrollBar()) {
                bar->setValue(bar->maximum());
            }
        }
    });
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

// GUI stage 3 (Windows parity, chat-display enrichment): persist +
// re-render.  NOTE the difference from the Mac panel: this panel's
// rebuildTranscriptWidgets() fully tears down and rebuilds every
// transcript widget on each refresh (see clearLayout()), so toggling
// this ALSO re-seeds every thinking row currently on screen from the
// new setting -- not just rows appended after the toggle, unlike the
// Mac panel (whose SwiftUI rows are per-Entry @State, seeded once at
// append time). A user's manually expanded/collapsed rows already do
// not survive any refresh on this panel regardless of this toggle --
// a pre-existing property of the full-rebuild design, not introduced
// here.
void ChatPanel::detailedTranscriptToggled(bool on)
{
    if (on == m_detailedTranscript) return;
    m_detailedTranscript = on;
    QSettings settings;
    settings.setValue("agentChat/detailedTranscript", on);
    refreshTranscript(m_statusLabel ? m_statusLabel->text() : QString());
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
    // ChatErrorKind contract.  Implements the SAME per-kind status text
    // and Retry-availability as the Mac handleProviderError, but is NOT
    // full parity with it: the Mac side also tracks consecutiveHttp400s
    // across Http-kind errors and, at >= 2 in a row, offers a
    // conversation-Reset affordance ("the conversation may carry
    // content the provider no longer accepts") on the theory that
    // repeated 400s indicate the transcript itself is poisoned and a
    // verbatim Retry will just fail again.  This Windows port has no
    // such counter or reset offer -- every Http error is treated
    // identically regardless of how many preceded it in the same
    // conversation.  Deliberately out of scope for this pass; port the
    // counter separately if the poison-scoping UX is needed here too.
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

        // GUI stage 3 (Windows parity): a live dispatch-time row --
        // "-> name" with no detail yet (the chevron stays hidden until
        // updatePendingToolRow reveals it once a result exists) --
        // mirrors ChatViewModel.driveTurn appending its `.toolActivity`
        // entry before executing the call.  Inserted just before the
        // trailing stretch item rebuildTranscriptWidgets() always
        // leaves last, so it lands at the bottom of the transcript
        // without a full rebuild.  m_pendingToolRow* are consumed (and
        // reset to null) by updatePendingToolRow() below, or by the
        // async render path's own AddToolResult call sites.
        {
            QLabel* label = nullptr;
            QTextEdit* detailEdit = nullptr;
            QToolButton* chevron = nullptr;
            QWidget* row = buildToolRow(
                QStringLiteral("\xE2\x86\x92 ") + toQString(call.name),
                QString(), /*hasDetail=*/false, /*startExpanded=*/false,
                &label, &detailEdit, &chevron);
            const int insertIndex = m_transcriptLayout->count() > 0
                ? m_transcriptLayout->count() - 1 : 0;
            m_transcriptLayout->insertWidget(insertIndex, row);
            m_pendingToolRowLabel = label;
            m_pendingToolRowDetail = detailEdit;
            m_pendingToolRowChevron = chevron;
            QTimer::singleShot(0, this, [this]() {
                if (m_transcriptScroll) {
                    if (QScrollBar* bar = m_transcriptScroll->verticalScrollBar()) {
                        bar->setValue(bar->maximum());
                    }
                }
            });
        }

        if (call.name == "render") {
            // P1-2: suspends here -- resumes back into processNextToolCall
            // from pollOutstandingRender() once the async job completes.
            // GUI stage 3: the row created above stays live across the
            // suspension via m_pendingToolRow* (QPointer-guarded --
            // see updatePendingToolRow); startAsyncRenderToolCall /
            // pollOutstandingRender update it at each of their
            // AddToolResult call sites.
            startAsyncRenderToolCall(call, line);
            return;
        }

        // Every other verb keeps the existing synchronous contract: fast
        // CST reads/edits with no render-duration cost to amortize.
        // Agent autonomy selector: routes to whichever dispatcher
        // matches the composer's CURRENT level (see ViewportBridge.h's
        // agentHandleToolCall doc) -- Read refuses edit verbs, Propose
        // stages them, Apply commits them directly (today's behaviour).
        // `render` is excluded from this routing on purpose (handled in
        // the `if` branch above via the level-independent agentHandleLine).
        const QString response = m_bridge->agentHandleToolCall(toQString(line));
        updatePendingToolRow(call, toStdString(response));
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
        const std::string resultLine = toStdString(makeSyntheticRenderResultLine(
            false, "internal: could not stage the async render submit"));
        updatePendingToolRow(m_activeRenderCall, resultLine);
        m_loop->AddToolResult(m_activeRenderCall, resultLine);
        processNextToolCall();
        return;
    }

    const QString submitResponse = m_bridge->agentHandleLine(asyncLine);
    const QJsonDocument submitDoc = QJsonDocument::fromJson(submitResponse.toUtf8());
    if (!submitDoc.isObject()) {
        // Malformed response line -- should not happen (HandleLine always
        // emits valid JSON-RPC) -- fail honestly rather than guess.
        m_activeRenderPending = false;
        updatePendingToolRow(m_activeRenderCall, toStdString(submitResponse));
        m_loop->AddToolResult(m_activeRenderCall, toStdString(submitResponse));
        processNextToolCall();
        return;
    }
    const QJsonObject submitObj = submitDoc.object();
    if (submitObj.contains("error")) {
        // Refused outright (no controller attached) -- already a
        // well-formed JSON-RPC error envelope; surface it verbatim.
        m_activeRenderPending = false;
        updatePendingToolRow(m_activeRenderCall, toStdString(submitResponse));
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
        const std::string resultLine = toStdString(makeSyntheticRenderResultLine(false, message));
        updatePendingToolRow(m_activeRenderCall, resultLine);
        m_loop->AddToolResult(m_activeRenderCall, resultLine);
        processNextToolCall();
        return;
    }
    if (!result.contains("renderJobId")) {
        m_activeRenderPending = false;
        const std::string resultLine = toStdString(makeSyntheticRenderResultLine(
            false, "render accepted but no renderJobId was returned"));
        updatePendingToolRow(m_activeRenderCall, resultLine);
        m_loop->AddToolResult(m_activeRenderCall, resultLine);
        processNextToolCall();
        return;
    }

    setOutstandingRenderJobId(static_cast<quint64>(result.value("renderJobId").toDouble()));

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
        setOutstandingRenderJobId(0);
        m_activeRenderPending = false;
        updatePendingToolRow(m_activeRenderCall, toStdString(waitResponse));
        m_loop->AddToolResult(m_activeRenderCall, toStdString(waitResponse));
        processNextToolCall();
        return;
    }
    const QJsonObject waitResult = waitObj.value("result").toObject();
    if (!waitResult.value("completed").toBool(false)) {
        return; // not done yet -- keep polling
    }

    if (m_renderPollTimer) m_renderPollTimer->stop();
    setOutstandingRenderJobId(0);
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
    updatePendingToolRow(m_activeRenderCall, toStdString(responseLine));
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
    setOutstandingRenderJobId(0);
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

// ============================================================
// Secure-MCP slice 5c (Windows parity) -- proposals panel.
//
// Polls list_proposals over the SAME in-process agentHandleLine
// transport the tool-call loop and the raw JSON-RPC debug panel use
// (not any external hosted server -- Windows has no MCP-hosting UI in
// this slice).  Mirrors ChatViewModel.refreshProposals /
// resolveProposal / the ProposalsPanel linger behaviour.
// ============================================================

void ChatPanel::refreshProposals()
{
    // Field fix (2026-07-11, found on macOS first): a render --
    // production, or this chat's own outstanding render job -- holds
    // the controller's mMutex for its whole duration, and
    // list_proposals serializes on that same mutex.  This poll runs
    // synchronously on the GUI thread, so one tick during a render
    // wedges the entire UI until the render finishes.  Skipping is
    // lossless: StageProposal takes the same mutex, so the queue
    // cannot change while we're gated -- the first allowed tick
    // re-syncs.  m_sceneEditable already folds in BOTH the production
    // render state and isChatRenderOutstanding.
    if (!m_sceneEditable) return;
    if (!m_bridge) {
        m_resolvedProposalObservedAt.clear();
        rebuildProposalsUI({});
        return;
    }

    const QString line = QStringLiteral("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"list_proposals\"}");
    const QString response = m_bridge->agentHandleLine(line);
    const QJsonDocument doc = QJsonDocument::fromJson(response.toUtf8());
    if (!doc.isObject()) {
        rebuildProposalsUI({});
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonObject result = root.value("result").toObject();
    const QJsonArray proposals = result.value("proposals").toArray();

    const QDateTime now = QDateTime::currentDateTime();
    QVector<ProposalEntry> next;
    QMap<quint64, QDateTime> nextObserved;
    for (const QJsonValue& v : proposals) {
        if (!v.isObject()) continue;
        const QJsonObject p = v.toObject();
        if (!p.contains(QLatin1String("id"))) continue;

        ProposalEntry e;
        e.id = static_cast<quint64>(p.value(QLatin1String("id")).toDouble());
        e.kind = p.value(QLatin1String("kind")).toString();
        e.status = p.value(QLatin1String("status")).toString();
        e.sessionLabel = p.value(QLatin1String("sessionLabel")).toString();
        e.target = p.value(QLatin1String("target")).toString();
        e.entityKind = p.value(QLatin1String("entityKind")).toString();
        e.param = p.value(QLatin1String("param")).toString();
        e.value = p.value(QLatin1String("value")).toString();
        e.chunkText = p.value(QLatin1String("chunkText")).toString();
        e.truncated = p.value(QLatin1String("truncated")).toBool(false);

        if (e.status != QLatin1String("pending")) {
            // Carry forward (or start) the observed-resolved timestamp;
            // drop it from the panel once it has lingered long enough --
            // mirrors ChatViewModel's resolvedProposalLingerSeconds.
            const QDateTime observedAt = m_resolvedProposalObservedAt.value(e.id, now);
            if (observedAt.msecsTo(now) > static_cast<qint64>(kResolvedProposalLingerSeconds * 1000)) {
                continue;
            }
            nextObserved.insert(e.id, observedAt);
        }

        next.append(e);
    }

    m_resolvedProposalObservedAt = nextObserved;
    rebuildProposalsUI(next);
}

void ChatPanel::rebuildProposalsUI(const QVector<ProposalEntry>& proposals)
{
    clearLayout(m_proposalsLayout);
    m_currentProposalCards.clear();
    m_proposalNoteLabels.clear();

    // The scene file the proposal targets -- read from the SAME
    // loadedFilePath the TopBar uses for the window title, so this is
    // never a fabricated label.
    const QString sceneFileName = m_bridge
        ? QFileInfo(m_bridge->loadedFilePath()).fileName()
        : QString();

    for (const ProposalEntry& p : proposals) {
        auto* card = new ProposalCard(p, sceneFileName, m_proposalsContainer);
        card->setActionsEnabled(m_sceneEditable);
        connect(card, &ProposalCard::applyClicked, this, &ChatPanel::onProposalApplyClicked);
        connect(card, &ProposalCard::rejectClicked, this, &ChatPanel::onProposalRejectClicked);
        connect(card, &ProposalCard::undoClicked, this, &ChatPanel::onProposalUndoClicked);
        m_proposalsLayout->addWidget(card);
        m_currentProposalCards.append(card);

        if (p.status == QLatin1String("pending")) {
            auto* note = new QLabel(QStringLiteral(
                "\xE2\x86\xBA joins the shared undo history \xE2\x80\x94 one step"));
            note->setFont(Theme::mono(10));
            note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
            m_proposalsLayout->addWidget(note);
            m_proposalNoteLabels.append(note);
        }
    }

    m_proposalsContainer->setVisible(!proposals.isEmpty());
}

void ChatPanel::updateProposalCardsEnabled()
{
    for (ProposalCard* card : std::as_const(m_currentProposalCards)) {
        if (card) card->setActionsEnabled(m_sceneEditable);
    }
}

void ChatPanel::onProposalApplyClicked(quint64 proposalId)
{
    // P1-2-equivalent gate: the SAME predicate that disables the card's
    // Apply/Reject buttons (ProposalCard::setActionsEnabled) -- belt and
    // suspenders against a click that raced a state change between paint
    // and dispatch, matching ChatViewModel.resolveProposal's guard.
    if (!m_bridge || !m_sceneEditable) return;
    const QString line = QStringLiteral(
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"resolve_proposal\","
        "\"params\":{\"proposalId\":%1,\"approve\":true}}").arg(proposalId);
    m_bridge->agentHandleLine(line);
    refreshProposals();
}

void ChatPanel::onProposalRejectClicked(quint64 proposalId)
{
    if (!m_bridge || !m_sceneEditable) return;
    const QString line = QStringLiteral(
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"resolve_proposal\","
        "\"params\":{\"proposalId\":%1,\"approve\":false}}").arg(proposalId);
    m_bridge->agentHandleLine(line);
    refreshProposals();
}

void ChatPanel::onProposalUndoClicked()
{
    // Joins the shared undo history -- the SAME ViewportBridge::undo()
    // the toolbar's undo button and the Edit menu's Undo action drive.
    // Round-2 P1: same belt-and-suspenders gate as Apply/Reject -- the
    // card disables the button via setActionsEnabled, but a click
    // racing a state flip must still refuse rather than drive undo
    // underneath an in-flight render.
    if (!m_bridge || !m_sceneEditable) return;
    m_bridge->undo();
}
