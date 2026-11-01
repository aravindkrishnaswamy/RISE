//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.cpp - Single-entity inspector implementation.
//    See header for the macOS PropertiesPanel.swift cross-reference.
//
//////////////////////////////////////////////////////////////////////

#include "ViewportProperties.h"
#include "ViewportBridge.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QGuiApplication>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QSize>
#include <QToolButton>
#include <QComboBox>
#include <QColorDialog>
#include <QMenu>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

namespace {

// Single-numeric ValueKind values (Bool=0, UInt=1, Double=2,
// DoubleVec3=3, ...).  Vector / string / reference kinds get no
// scrub handle -- scrubbing a vector is ambiguous and keyboard
// entry is the natural input for non-numeric fields.
inline bool isScrubbableKind(int kind)
{
    return kind == 1 /* UInt */ || kind == 2 /* Double */;
}

inline bool isAngularField(const QString& name)
{
    return name == QLatin1String("theta")
        || name == QLatin1String("phi")
        || name == QLatin1String("fov")
        || name == QLatin1String("pitch")
        || name == QLatin1String("yaw")
        || name == QLatin1String("roll")
        || name == QLatin1String("aperture_rotation")
        || name == QLatin1String("tilt_x")
        || name == QLatin1String("tilt_y");
}

inline double scrubRate(const QString& name, double value)
{
    if (isAngularField(name)) return 0.5;
    return std::max(std::abs(value), 1e-3) * 0.005;
}

inline QString formatScrubbed(double v, int kind)
{
    if (!std::isfinite(v)) return QStringLiteral("0");
    if (std::abs(v) < 1e-15) v = 0.0;
    if (kind == 1 /* UInt */) {
        const double clamped = std::min(std::max(std::round(v), 0.0), static_cast<double>(UINT32_MAX));
        return QString::number(static_cast<qulonglong>(clamped));
    }
    return QString::asprintf("%.6g", v);
}

// doc 88 S4b (Tier-1 param sliders): the first whitespace-delimited token,
// SKIPPING token 0, that looks and parses as a number, as (its position and
// length within `line`, its parsed value).  Returns false when the line has
// no such token -- the caller then has nothing to drive and falls back to
// the range minimum / skips the commit.  Mirrors PropertiesPanel.swift's
// private `ParamSliderCell.numericToken` (~line 889).
//
// Two deliberate departures from "just try toDouble() on every token":
//
// 1. Token 0 is SKIPPED.  `hasRange` (the sole gate that routes a row
//    through the slider block in buildPropertyRow below, rather than
//    straight to the plain text well) is set only for an expression
//    painter's `param[i]` rows.  The row's VALUE line is `<paramName>
//    <value> [min ... max ... step ... label ...]` -- so token 0 is the
//    param's OWN NAME (e.g. `wob` or `ring_scale`), never a value the
//    slider should scrub.  A parameter literally named `4` (or, see point
//    2, `inf`/`nan`) would otherwise have its name token silently
//    overwritten by a scrub commit.
//
// 2. A token must LOOK numeric (optional leading sign, then a digit or
//    `.`) before `QString::toDouble()` is even tried.  `toDouble()`
//    accepts "inf"/"nan"-shaped input in some locales, so without this
//    guard a param named exactly `inf`/`nan` (rare, but not disallowed by
//    the scene grammar) could still be misread as a numeric value by a
//    naive scan -- the token-0 skip above only rules out the FIRST token,
//    not a same-shaped later one on a multi-token value line.
//
//    QChar::isNumber() (not isDigit()) deliberately: Swift's guard is
//    `Character.isNumber`, which spans Unicode categories Nd+Nl+No, and
//    QChar::isNumber() matches exactly those -- isDigit() is Nd only.
//    The category set decides whether a malformed token BAILS the scan
//    (looks numeric, fails to parse) or is silently skipped, so the two
//    platforms must classify identically or the same malformed line would
//    rewrite a DIFFERENT token on Windows than on macOS.
bool numericToken(const QString& line, int* outPos, int* outLen, double* outValue, bool skipFirstToken = true)
{
    auto looksNumeric = [](const QString& tok) {
        int i = 0;
        if (i < tok.size() && (tok[i] == QLatin1Char('+') || tok[i] == QLatin1Char('-'))) ++i;
        if (i + 1 < tok.size() && tok[i] == QLatin1Char('0')
            && (tok[i + 1] == QLatin1Char('x') || tok[i + 1] == QLatin1Char('X'))) {
            return false;
        }
        if (i >= tok.size()) return false;
        return tok[i].isNumber() || tok[i] == QLatin1Char('.');
    };

    const int n = line.size();
    int i = 0;
    int tokenIndex = 0;
    while (i < n) {
        while (i < n && line[i].isSpace()) ++i;
        if (i >= n) return false;
        int j = i;
        while (j < n && !line[j].isSpace()) ++j;
        const QString tok = line.mid(i, j - i);
        if (!skipFirstToken) {
            if (tokenIndex != 0 || !looksNumeric(tok)) return false;
            bool ok = false;
            const double v = tok.toDouble(&ok);
            if (!ok || !std::isfinite(v)) return false;
            int k = j;
            while (k < n && line[k].isSpace()) ++k;
            if (k != n) return false;
            if (outPos) *outPos = i;
            if (outLen) *outLen = j - i;
            if (outValue) *outValue = v;
            return true;
        }
        if (tokenIndex == 1) {
            // Token 1 in `<paramName> <value> [min ...]` is strictly the value
            // token.  If it does not look or parse as a finite decimal number,
            // bail out with false immediately rather than falling through to
            // scan later tokens (where `min`'s bound token would otherwise match).
            if (!looksNumeric(tok)) return false;
            bool ok = false;
            const double v = tok.toDouble(&ok);
            if (!ok || !std::isfinite(v)) return false;
            if (outPos) *outPos = i;
            if (outLen) *outLen = j - i;
            if (outValue) *outValue = v;
            return true;
        }
        tokenIndex += 1;
        i = j;
    }
    return false;
}

int defStageIndex(const QString& rowName)
{
    if (!rowName.startsWith(QLatin1String("def[")) || !rowName.endsWith(QLatin1Char(']'))) {
        return -1;
    }
    const QString inner = rowName.mid(4, rowName.size() - 5);
    if (inner.isEmpty()) return -1;
    bool ok = false;
    const int idx = inner.toInt(&ok);
    return (ok && idx >= 0) ? idx : -1;
}

// Shared well chrome (Theme::bgWell fill + hairline border) -- mirrors
// the comp's typed value cells and PropertiesPanel.swift's wellBackground().
QString wellStyleSheet()
{
    return QStringLiteral("background-color: %1; border: 1px solid %2; border-radius: %3px;")
        .arg(Theme::hex(Theme::bgWell), Theme::hex(Theme::borderLight))
        .arg(Theme::radiusSmall);
}

QString lineEditStyleSheet(const QColor& textColor)
{
    return QStringLiteral("QLineEdit { background: transparent; border: none; color: %1; }")
        .arg(Theme::hex(textColor));
}

// Click-and-drag chevron handle for scrubbing numeric property values.
class ScrubHandle : public QLabel
{
public:
    using CommitFn = std::function<void(const QString&, const QString&)>;
    using BracketFn = std::function<void()>;

    ScrubHandle(QLineEdit* target,
                QString name,
                int kind,
                CommitFn commit,
                BracketFn beginBracket,
                BracketFn endBracket,
                std::shared_ptr<bool> ownerAlive = nullptr,
                QWidget* parent = nullptr)
        : QLabel(parent)
        , m_target(target)
        , m_name(std::move(name))
        , m_kind(kind)
        , m_commit(std::move(commit))
        , m_beginBracket(std::move(beginBracket))
        , m_endBracket(std::move(endBracket))
        , m_ownerAlive(std::move(ownerAlive))
    {
        // Glyph is painted in paintEvent -- two stacked chevron icons
        // (bundled-SVG "chevron-up"/"chevron-down", Theme::icon), mirroring
        // PropertiesPanel.swift:1141's ScrubHandle
        // (`Image(systemName: "chevron.up.chevron.down")`).  Drawing the
        // pixmaps directly rather than parenting two QLabels keeps this a
        // single lightweight widget for the drag hit-target, same as the
        // hand-drawn-triangle version this replaces.
        setFixedSize(16, 18);
        setCursor(Qt::SizeVerCursor);
        setToolTip(QObject::tr(
            "Drag up/down to change.  Shift = fine, Alt = coarse."));
    }

    ~ScrubHandle() override
    {
        if (m_dragging && m_endBracket && (!m_ownerAlive || *m_ownerAlive)) {
            m_endBracket();
        }
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !m_target || (m_ownerAlive && !*m_ownerAlive)) {
            QLabel::mousePressEvent(e);
            return;
        }
        bool ok = false;
        m_startValue = m_target->text().toDouble(&ok);
        if (!ok) m_startValue = 0;
        m_startY = e->globalPosition().y();
        m_dragging = true;
        if (m_beginBracket) m_beginBracket();
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging || (m_ownerAlive && !*m_ownerAlive)) {
            QLabel::mouseMoveEvent(e);
            return;
        }
        if (!m_target) {
            m_dragging = false;
            if (m_endBracket) m_endBracket();
            return;
        }
        const double dy = m_startY - e->globalPosition().y();

        double rate = scrubRate(m_name, m_startValue);
        const Qt::KeyboardModifiers mods = e->modifiers();
        if (mods & Qt::ShiftModifier) rate *= 0.25;
        if (mods & Qt::AltModifier)   rate *= 4.0;

        const double newValue = m_startValue + dy * rate;
        const QString formatted = formatScrubbed(newValue, m_kind);
        m_target->setText(formatted);
        if (m_commit) m_commit(m_name, formatted);
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (m_dragging && e->button() == Qt::LeftButton) {
            m_dragging = false;
            if (m_endBracket && (!m_ownerAlive || *m_ownerAlive)) m_endBracket();
            e->accept();
        } else {
            QLabel::mouseReleaseEvent(e);
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const qreal dpr = devicePixelRatioF();
        const int w = width();
        const int h = height();
        const int glyphSize = 9;
        const int half = h / 2;

        const QPixmap up = Theme::iconPixmap(QStringLiteral("chevron-up"), glyphSize, Theme::textDim, dpr);
        const QPixmap down = Theme::iconPixmap(QStringLiteral("chevron-down"), glyphSize, Theme::textDim, dpr);
        p.drawPixmap(QRect((w - glyphSize) / 2, (half - glyphSize) / 2, glyphSize, glyphSize), up);
        p.drawPixmap(QRect((w - glyphSize) / 2, half + (half - glyphSize) / 2, glyphSize, glyphSize), down);
    }

private:
    QPointer<QLineEdit> m_target;
    QString    m_name;
    int        m_kind     = 0;
    CommitFn   m_commit;
    BracketFn  m_beginBracket;
    BracketFn  m_endBracket;
    std::shared_ptr<bool> m_ownerAlive;
    double     m_startValue = 0;
    double     m_startY     = 0;
    bool       m_dragging   = false;
};

// Bool-kind value cell: a pill toggle mirroring the comp's rounded
// switch (Theme::accent when on, Theme::fillTrough when off).  No
// Q_OBJECT -- a pure click target reporting through a std::function,
// same pattern as ScrubHandle above.
class BoolPill : public QWidget
{
public:
    using CommitFn = std::function<void(bool)>;

    BoolPill(bool initialOn, bool editable, CommitFn commit, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_on(initialOn)
        , m_editable(editable)
        , m_commit(std::move(commit))
    {
        setFixedSize(32, 18);
        if (m_editable) setCursor(Qt::PointingHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (m_editable && e->button() == Qt::LeftButton) {
            m_on = !m_on;
            update();
            if (m_commit) m_commit(m_on);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_on ? Theme::accent : Theme::fillTrough);
        p.drawRoundedRect(rect(), 9, 9);
        const int knobD = 14;
        const int x = m_on ? (width() - knobD - 2) : 2;
        p.setBrush(QColor(0x0d, 0x11, 0x16));
        p.drawEllipse(QRect(x, 2, knobD, knobD));
    }

private:
    bool     m_on;
    bool     m_editable;
    CommitFn m_commit;
};

// A row that reports a plain left-click via a std::function callback.
// No Q_OBJECT / signals -- same pattern as ScrubHandle/BoolPill above
// (and OutlinerWidget.cpp's ClickableRow), used here for the
// "Advanced" disclosure toggle row.
class ClickableRow : public QWidget
{
public:
    using ClickFn = std::function<void()>;

    explicit ClickableRow(ClickFn onClick, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_onClick(std::move(onClick))
    {
    }

protected:
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && m_onClick) {
            m_onClick();
        }
        QWidget::mouseReleaseEvent(e);
    }

private:
    ClickFn m_onClick;
};

static constexpr int kMaxBasicRows = 8;

// Progressive disclosure (two levels max): the first kMaxBasicRows
// rows show inline as "Basic"; the rest collapse under one "Advanced"
// toggle.  Priority for the Basic set: editable rows with quick-pick
// presets first (the ones most worth a click), then whichever other
// rows come next in descriptor order, until the cap is reached.  Both
// groups are then rendered back in their ORIGINAL descriptor order --
// the priority pass only decides membership, not display order.
void splitBasicAdvanced(const QVector<ViewportProperty>& rows,
                         QVector<ViewportProperty>& basic,
                         QVector<ViewportProperty>& advanced)
{
    basic.clear();
    advanced.clear();
    if (rows.size() <= kMaxBasicRows) {
        basic = rows;
        return;
    }

    QSet<QString> selected;
    for (const ViewportProperty& p : rows) {
        if (selected.size() >= kMaxBasicRows) break;
        if (p.editable && !p.presets.isEmpty()) selected.insert(p.name);
    }
    if (selected.size() < kMaxBasicRows) {
        for (const ViewportProperty& p : rows) {
            if (selected.size() >= kMaxBasicRows) break;
            if (!selected.contains(p.name)) selected.insert(p.name);
        }
    }
    for (const ViewportProperty& p : rows) {
        if (selected.contains(p.name)) basic.append(p);
        else advanced.append(p);
    }
}

}  // namespace

// ============================================================
// Category display metadata
// ============================================================

QString ViewportProperties::categoryTitle(Category cat)
{
    switch (cat) {
    case Category::Camera:       return tr("Cameras");
    case Category::Rasterizer:   return tr("Rasterizer");
    case Category::Object:       return tr("Objects");
    case Category::Light:        return tr("Lights");
    case Category::Film:         return tr("Output Settings");
    case Category::Material:     return tr("Materials");
    case Category::Medium:       return tr("Media");
    case Category::Animation:    return tr("Animation");
    case Category::SceneVariant: return tr("Variants");
    case Category::Painter:      return tr("Painters");
    case Category::Geometry:     return tr("Geometry");
    case Category::None:
    default:                     return tr("Scene");
    }
}

// Bundled-SVG icon system (Theme::iconPixmap/icon -- see Theme.h).  This
// used to return raw Unicode glyphs (◉ ◆ ☀ ◐ ⚙ ▦ ≈ ▶ ⧉ ▧), mirroring
// PropertiesPanel.swift's categoryGlyph(_:).  On macOS those glyphs are
// SF-adjacent and render fine; on Windows several of them (notably ☀ and
// ▶) fall back through IBM Plex -> Segoe UI Symbol -> Segoe UI Emoji,
// landing as full-color emoji glyphs inside a monochrome chip -- a
// visible polish bug that doesn't reproduce on Mac.  Returns an empty
// string for categories with no dedicated icon (None); the caller falls
// back to the "•" text glyph in that case, same as before.
QString ViewportProperties::categoryIconName(Category cat)
{
    switch (cat) {
	case Category::Camera:       return QStringLiteral("camera");
    case Category::Object:       return QStringLiteral("box");
    case Category::Light:        return QStringLiteral("lightbulb");
    case Category::Material:     return QStringLiteral("circle-dot");
    case Category::Rasterizer:   return QStringLiteral("cog");
    case Category::Film:         return QStringLiteral("film");
    case Category::Medium:       return QStringLiteral("waves");
    case Category::Animation:    return QStringLiteral("play");
    case Category::SceneVariant: return QStringLiteral("layers");
	case Category::Painter:      return QStringLiteral("paintbrush");
	case Category::Geometry:     return QStringLiteral("scale-3d");
    case Category::None:
    default:                     return QString();   // no dedicated icon -- "•" text fallback
    }
}

// ============================================================
// Construction
// ============================================================

ViewportProperties::ViewportProperties(ViewportBridge* bridge, QWidget* parent)
    : QWidget(parent)
    , m_bridge(bridge)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);

    auto* body = new QWidget(m_scroll);
    m_bodyLayout = new QVBoxLayout(body);
    m_bodyLayout->setContentsMargins(14, 12, 14, 12);
    m_bodyLayout->setSpacing(13);

    // ---- Entity header: icon chip + name + meta -----------------------
    auto* headerRow = new QWidget(body);
    auto* headerRowLayout = new QHBoxLayout(headerRow);
    headerRowLayout->setContentsMargins(0, 0, 0, 0);
    headerRowLayout->setSpacing(9);

    m_iconChip = new QLabel(headerRow);
    m_iconChip->setFixedSize(26, 26);
    m_iconChip->setAlignment(Qt::AlignCenter);
    m_iconChip->setFont(Theme::sans(12));
    // Accent-tinted bg/border/color stylesheet is set in restyleTheme()
    // (LIVE THEME-SWITCH CONTRACT, Theme.h) -- persistent chrome that
    // rebuildEntityHeader() never revisits (it only touches this
    // label's pixmap/text).
    headerRowLayout->addWidget(m_iconChip);

    auto* nameCol = new QWidget(headerRow);
    auto* nameColLayout = new QVBoxLayout(nameCol);
    nameColLayout->setContentsMargins(0, 0, 0, 0);
    nameColLayout->setSpacing(1);

    // Title row: entity name + (when available) the "Reveal in scene
    // file" ⌗ line chip, side by side -- mirrors PropertiesPanel.swift's
    // HStack(headerTitle, SourceLineChip).
    auto* titleRow = new QWidget(nameCol);
    auto* titleRowLayout = new QHBoxLayout(titleRow);
    titleRowLayout->setContentsMargins(0, 0, 0, 0);
    titleRowLayout->setSpacing(6);
    m_nameLabel = new QLabel(titleRow);
    m_nameLabel->setFont(Theme::sans(12, QFont::DemiBold));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    titleRowLayout->addWidget(m_nameLabel);

    m_sourceLineChip = new QToolButton(titleRow);
    m_sourceLineChip->setFont(Theme::mono(10));
    m_sourceLineChip->setCursor(Qt::PointingHandCursor);
    m_sourceLineChip->setToolTip(tr("Reveal in scene file"));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    m_sourceLineChip->hide();
    connect(m_sourceLineChip, &QToolButton::clicked, this, [this]() {
        emit revealRequested(m_currentSelectionCat, m_currentSelectionName);
    });
    titleRowLayout->addWidget(m_sourceLineChip);
    titleRowLayout->addStretch(1);
    nameColLayout->addWidget(titleRow);

    m_metaLabel = new QLabel(nameCol);
    m_metaLabel->setFont(Theme::mono(10));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    nameColLayout->addWidget(m_metaLabel);
    headerRowLayout->addWidget(nameCol);
    headerRowLayout->addStretch(1);

    m_bodyLayout->addWidget(headerRow);

    // ---- Camera-only affordances ---------------------------------------
    m_cameraAffordances = new QWidget(body);
    auto* camLayout = new QVBoxLayout(m_cameraAffordances);
    camLayout->setContentsMargins(0, 0, 0, 0);
    camLayout->setSpacing(6);

    m_useInViewportBtn = new QToolButton(m_cameraAffordances);
    m_useInViewportBtn->setText(tr("Use in viewport"));
    m_useInViewportBtn->setFont(Theme::sans(11));
    m_useInViewportBtn->setCursor(Qt::PointingHandCursor);
    // Stylesheet set in restyleTheme() -- persistent chrome.
    connect(m_useInViewportBtn, &QToolButton::clicked, this, &ViewportProperties::onUseInViewportClicked);
    camLayout->addWidget(m_useInViewportBtn);

    m_addCameraBtn = new QToolButton(m_cameraAffordances);
    // Bundled-SVG "circle-plus" icon beside the label, replacing the
    // "+ " text prefix -- mirrors PropertiesPanel.swift:331's
    // `Image(systemName: "plus.circle")` + Text("Add Camera") pairing.
    // Icon + stylesheet set in restyleTheme() -- persistent chrome.
    m_addCameraBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_addCameraBtn->setIconSize(QSize(10, 10));
    m_addCameraBtn->setText(tr("Add Camera"));
    m_addCameraBtn->setFont(Theme::mono(10));
    m_addCameraBtn->setCursor(Qt::PointingHandCursor);
    connect(m_addCameraBtn, &QToolButton::clicked, this, &ViewportProperties::onAddCameraClicked);
    camLayout->addWidget(m_addCameraBtn);

    m_bodyLayout->addWidget(m_cameraAffordances);

    // ---- Empty-selection message ----------------------------------------
    m_emptyMessage = new QLabel(body);
    m_emptyMessage->setFont(Theme::sans(11));
    m_emptyMessage->setWordWrap(true);
    // Stylesheet set in restyleTheme() -- persistent chrome (only the
    // text/visibility are touched by rebuildPropertyRows()).
    m_bodyLayout->addWidget(m_emptyMessage);

    // ---- Basic property rows --------------------------------------------
    auto* basicContainer = new QWidget(body);
    m_basicLayout = new QVBoxLayout(basicContainer);
    m_basicLayout->setContentsMargins(0, 0, 0, 0);
    m_basicLayout->setSpacing(9);
    m_bodyLayout->addWidget(basicContainer);

    // ---- Advanced disclosure ---------------------------------------------
    m_advancedToggleRow = new ClickableRow([this]() { onAdvancedToggleClicked(); }, body);
    m_advancedToggleRow->setCursor(Qt::PointingHandCursor);
    auto* advToggleLayout = new QHBoxLayout(m_advancedToggleRow);
    advToggleLayout->setContentsMargins(0, 0, 0, 0);
    advToggleLayout->setSpacing(6);
    m_advancedLabel = new QLabel(tr("Advanced"), m_advancedToggleRow);
    m_advancedLabel->setFont(Theme::sans(11));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    advToggleLayout->addWidget(m_advancedLabel);
    m_advancedArrowLabel = new QLabel(m_advancedToggleRow);
    m_advancedArrowLabel->setFixedSize(10, 10);
    m_advancedArrowLabel->setAlignment(Qt::AlignCenter);
    advToggleLayout->addWidget(m_advancedArrowLabel);
    advToggleLayout->addStretch(1);
    m_advancedCountLabel = new QLabel(m_advancedToggleRow);
    m_advancedCountLabel->setFont(Theme::mono(9));
    // Stylesheet set in restyleTheme() -- persistent chrome (only the
    // text is touched by updateAdvancedToggleLabel()).
    advToggleLayout->addWidget(m_advancedCountLabel);
    m_bodyLayout->addWidget(m_advancedToggleRow);

    m_advancedContainer = new QWidget(body);
    m_advancedLayout = new QVBoxLayout(m_advancedContainer);
    m_advancedLayout->setContentsMargins(0, 0, 0, 0);
    m_advancedLayout->setSpacing(9);
    m_bodyLayout->addWidget(m_advancedContainer);

    m_bodyLayout->addStretch(1);

    m_scroll->setWidget(body);
    root->addWidget(m_scroll, 1);

    // ---- Footer: Save / Save As... ----------------------------------------
    // Style set in restyleTheme() -- persistent chrome.
    m_footerSeparator = new QFrame(this);
    m_footerSeparator->setFrameShape(QFrame::HLine);
    root->addWidget(m_footerSeparator);

    auto* footer = new QWidget(this);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(14, 8, 14, 8);
    footerLayout->setSpacing(8);

    m_saveButton = new QToolButton(footer);
    m_saveButton->setText(tr("Save"));
    m_saveButton->setFont(Theme::sans(11, QFont::DemiBold));
    m_saveButton->setEnabled(false);
    m_saveButton->setToolTip(tr("No changes to save"));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    connect(m_saveButton, &QToolButton::clicked,
            this, [this]() { performSceneSave(/*useLoadedPath=*/true); });
    footerLayout->addWidget(m_saveButton);

    m_saveAsButton = new QToolButton(footer);
    m_saveAsButton->setText(tr("Save As\xE2\x80\xA6"));
    m_saveAsButton->setFont(Theme::sans(11));
    m_saveAsButton->setEnabled(false);
    m_saveAsButton->setToolTip(tr("No changes to save"));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    connect(m_saveAsButton, &QToolButton::clicked,
            this, [this]() { performSceneSave(/*useLoadedPath=*/false); });
    footerLayout->addWidget(m_saveAsButton);

    footerLayout->addStretch(1);

    m_refreshButton = new QToolButton(footer);
    // Tinted SVG icon (mirrors PropertiesPanel.swift's SF `arrow.clockwise`
    // refresh affordance).  Icon set in restyleTheme() -- persistent
    // chrome (QToolButton::setIcon bakes a QIcon; unlike bindIconLabel
    // there's no self-updating filter for it).
    m_refreshButton->setIconSize(QSize(12, 12));
    m_refreshButton->setToolTip(tr("Refresh from the live scene"));
    m_refreshButton->setStyleSheet(QStringLiteral("QToolButton { border: none; }"));
    connect(m_refreshButton, &QToolButton::clicked, this, &ViewportProperties::refresh);
    footerLayout->addWidget(m_refreshButton);

    root->addWidget(footer);

    setAutoFillBackground(true);
    // Palette Window fill set in restyleTheme() -- persistent chrome.

    // The dirtyChanged signal is emitted via QueuedConnection from
    // the bridge's C-trampoline (see ViewportBridge ctor), so a
    // direct Qt::AutoConnection here delivers on the GUI thread.
    if (m_bridge) {
        connect(m_bridge, &ViewportBridge::dirtyChanged,
                this, &ViewportProperties::onDirtyChanged);
    }

    setMinimumWidth(260);

    // LIVE THEME-SWITCH CONTRACT (Theme.h): run once at construction so
    // every persistent-chrome site above (which no longer sets its own
    // stylesheet/icon inline) actually gets styled -- restyleTheme() is
    // the single source of truth for all of it, not a redundant re-
    // application. See restyleTheme()'s doc comment below for the full
    // persistent-chrome/dynamic-content split.
    m_themeReady = true;
    restyleTheme();

    // Initial pull so the panel renders immediately.
    refresh();
}

ViewportProperties::~ViewportProperties()
{
    if (m_rowAlive) *m_rowAlive = false;
    endScrubBracket();
}

void ViewportProperties::beginScrubBracket()
{
    m_scrubbing = true;
    if (!m_scrubBracketOpen && m_bridge) {
        m_scrubBracketOpen = true;
        m_bridge->beginPropertyScrub();
    }
}

void ViewportProperties::endScrubBracket()
{
    m_scrubbing = false;
    if (m_scrubBracketOpen) {
        m_scrubBracketOpen = false;
        if (m_bridge) m_bridge->endPropertyScrub();
    }
}

// ============================================================
// LIVE THEME-SWITCH CONTRACT (Theme.h)
// ============================================================

void ViewportProperties::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void ViewportProperties::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();
    // Persistent chrome only -- every widget below is a MEMBER created
    // once in the constructor and never recreated, so its stylesheet/
    // icon must be re-applied explicitly on a theme switch (the
    // constructor no longer sets any of this inline; this is the sole
    // place it happens).  Idempotent, creates no widgets.
    //
    // Deliberately does NOT touch the Basic/Advanced property rows
    // (rebuildPropertyRows()/buildPropertyRow()) or the entity-header's
    // icon PIXMAP (rebuildEntityHeader() -> Theme::bindIconLabel, which
    // self-updates on QEvent::PaletteChange via its own installed event
    // filter -- see Theme.h). Those rows already read every Theme::
    // token live at build time via wellStyleSheet()/lineEditStyleSheet()
    // (this file's anonymous-namespace helpers) -- rebuildPropertyRows()
    // would need to run again for a WAITING (not yet visible) frame to
    // pick up new colors, and it already does: refresh() rides every
    // ViewportBridge::imageUpdated frame during a live render, and fires
    // immediately on the next selection change, edit, or explicit
    // Refresh click regardless.  A SYNCHRONOUS rebuildPropertyRows()
    // call from here would violate the contract's "must not create
    // widgets" rule (it deleteLater()s and recreates every row); the
    // idle-viewport case (switch with no pending frame) is instead
    // covered by the QUEUED rebuild at the END of this method -- see
    // the comment there.  BoolPill/ScrubHandle (both in the anonymous namespace
    // above) paint their own colors straight from Theme:: tokens in
    // paintEvent(), so they repaint correctly on Qt's own post-palette-
    // change update() with no extra code (contract item 3).

    if (m_iconChip) {
        m_iconChip->setStyleSheet(QStringLiteral(
            "background-color: %1; border: 1px solid %2; border-radius: 6px; color: %3;")
            .arg(Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.15 * 255))),
                 Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.3 * 255))),
                 Theme::hex(Theme::accentLight)));
    }

    if (m_sourceLineChip) {
        m_sourceLineChip->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 2px 6px; }"
            "QToolButton:hover { color: %4; border-color: %5; }")
            .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::borderHairline))
            .arg(Theme::radiusSmall)
            .arg(Theme::hex(Theme::textSecondary), Theme::hex(Theme::borderStrong)));
    }

    // Entity name + category subtitle (2026-07-23 round-2 review fix:
    // these were baked once at construction and survived a live switch
    // with stale colors -- the exact defect class this contract exists
    // to prevent).
    if (m_nameLabel) {
        m_nameLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_metaLabel) {
        m_metaLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }

    if (m_useInViewportBtn) {
        m_useInViewportBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 7px 0; }"
            "QToolButton:disabled { color: %4; border-color: %5; }")
            .arg(Theme::hex(Theme::accentLight),
                 Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.35 * 255))))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
    }

    if (m_addCameraBtn) {
        m_addCameraBtn->setIcon(Theme::icon("circle-plus", 10, Theme::textDim));
        m_addCameraBtn->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: none; padding: 2px 0; }")
            .arg(Theme::hex(Theme::textDim)));
    }

    if (m_emptyMessage) {
        m_emptyMessage->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }

    if (m_advancedLabel) {
        m_advancedLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    }
    if (m_advancedCountLabel) {
        m_advancedCountLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDisabled)));
    }

    if (m_footerSeparator) {
        m_footerSeparator->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderHairline)));
    }

    if (m_saveButton) {
        // Disabled-state background: Theme::fillTrough (a theme-aware
        // white/black-alpha fill token), NOT the raw Theme::whiteAlpha()
        // this used to bake directly -- whiteAlpha() is a "stays white
        // regardless of theme" primitive (Theme.h), which is wrong here:
        // in Light mode a white overlay on an already-light disabled
        // button reads as almost no dimming at all. fillTrough resolves
        // to whiteAlpha(26) in Dark and blackAlpha(20) in Light, so the
        // disabled fill actually darkens/mutes in both modes.
        //
        // Enabled-state fill/text: Theme::accent / Theme::textOnAccent,
        // NOT a hardcoded "#0d1116 on Theme::textPrimary" fill (P1 fix,
        // 2026-07-23 review) -- in Light mode textPrimary is near-black
        // (~0x1a1b1e) and the hardcoded text was ALSO near-black
        // (0x0d1116), giving ~1.1:1 contrast -- the button was there but
        // functionally invisible.  Diverges from the Mac counterpart
        // (PropertiesPanel.swift:1075-1080), which fills with
        // Theme.textPrimary too and has the SAME light-mode defect --
        // Windows uses the accent/textOnAccent idiom instead (the same
        // one ProposalCard's Apply button already uses), which stays
        // legible in both modes since Theme::textOnAccent is chosen for
        // contrast against Theme::accent specifically (see Theme.h/
        // Theme.cpp's LightPalette doc on textOnAccent).
        m_saveButton->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; background-color: %2; border: none; border-radius: %3px; padding: 5px 11px; }"
            "QToolButton:disabled { color: %4; background-color: %5; }")
            .arg(Theme::hex(Theme::textOnAccent), Theme::hex(Theme::accent))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::textDisabled), Theme::rgba(Theme::fillTrough)));
    }

    if (m_saveAsButton) {
        m_saveAsButton->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 5px 11px; }"
            "QToolButton:disabled { color: %4; border-color: %5; }")
            .arg(Theme::hex(Theme::textTertiary), Theme::hex(Theme::borderStrong))
            .arg(Theme::radiusMedium)
            .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
    }

    if (m_refreshButton) {
        m_refreshButton->setIcon(Theme::icon(QStringLiteral("refresh-cw"), 12, Theme::textDim));
    }

    if (m_scroll) {
        // Slim themed scrollbars (Task A): applied directly to the
        // QScrollArea's own scrollbar widgets, not the scroll area
        // itself, so the QSS can't leak into any other selector. Bakes
        // token colors, so re-applied on every restyleTheme() call.
        if (QScrollBar* vbar = m_scroll->verticalScrollBar()) {
            vbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
        if (QScrollBar* hbar = m_scroll->horizontalScrollBar()) {
            hbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
    }

    {
        // setAutoFillBackground(true) was set once in the constructor
        // (structural, not token-dependent); the fill color itself is
        // re-applied here every time, matching MainWindow::restyleTheme's
        // m_leftPanel/m_rightPanel pattern.
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        setPalette(pal);
    }

    // Idle-viewport queued rebuild (P2 fix, 2026-07-23 review; blessed by
    // Theme.h's LIVE THEME-SWITCH CONTRACT point 5): the long comment
    // above explains why this method deliberately skips forcing an
    // IMMEDIATE rebuildPropertyRows() call -- a pending render frame or
    // the next selection/edit/Refresh click self-heals it.  But a theme
    // switch landing while the viewport is completely IDLE (no
    // imageUpdated frame coming) leaves these rows baked with the OLD
    // palette's colors until the next real interaction.  QTimer::
    // singleShot(0, ...) defers the rebuild OUT of this synchronous
    // palette-change cascade -- restyleTheme() itself must not create
    // widgets (contract point 2) -- and onto the next event-loop turn,
    // where it's safe.  Uses `pullFreshSnapshot=false`
    // (rebuildPropertyRows()'s header doc) to replay from the
    // controller's ALREADY-cached snapshot rather than risking a fresh
    // RefreshProperties round-trip from inside a deferred callback;
    // skips entirely while a ScrubHandle drag is in flight (same
    // m_scrubbing guard refresh() uses) or once the bridge has since
    // gone away (rebuildPropertyRows() itself no-ops then too, but the
    // early return here also skips the otherwise-harmless
    // rebuildEntityHeader() call for a torn-down panel).
    // rebuildEntityHeader() is cheap regardless -- it only reads already-
    // cached members (m_currentSelectionCat/Name), no bridge call at all.
    QTimer::singleShot(0, this, [this]() {
        if (m_scrubbing || m_contextMenuOpen || !m_bridge) return;
        rebuildEntityHeader();
        rebuildPropertyRows(/*pullFreshSnapshot=*/false);
    });
}

// ============================================================
// Phase 6.5: scene-file save.  Footer buttons drive these.
// ============================================================

void ViewportProperties::onDirtyChanged(bool hasUnsavedChanges)
{
    if (!m_saveButton || !m_saveAsButton || !m_bridge) return;
    const QString loaded = m_bridge->loadedFilePath();
    m_saveButton->setEnabled(hasUnsavedChanges && !loaded.isEmpty());
    m_saveAsButton->setEnabled(hasUnsavedChanges);

    if (!hasUnsavedChanges) {
        m_saveButton->setToolTip(tr("No changes to save"));
        m_saveAsButton->setToolTip(tr("No changes to save"));
    } else {
        m_saveButton->setToolTip(loaded.isEmpty()
            ? tr("Use Save As\xE2\x80\xA6 (no loaded path)")
            : tr("Save scene to %1").arg(loaded));
        m_saveAsButton->setToolTip(tr("Save scene to a chosen path\xE2\x80\xA6"));
    }
}

void ViewportProperties::performSceneSave(bool useLoadedPath)
{
    if (!m_bridge) return;

    QString target;
    if (useLoadedPath) {
        target = m_bridge->loadedFilePath();
    }
    // The scene is gaining a NEW path via the picker below whenever
    // `target` is still empty at this point -- true for the Save As
    // button (useLoadedPath=false unconditionally lands here) AND for a
    // defensive in-place Save with no loaded path (start-screen untitled
    // scene; the Save button is normally disabled then, see
    // onDirtyChanged, but this keeps the flag correct regardless of
    // caller).  Computed from `target`, not `useLoadedPath` directly, so
    // it can't drift from which branch actually ran.
    const bool wasSaveAs = target.isEmpty();
    if (target.isEmpty()) {
        const QString loaded = m_bridge->loadedFilePath();
        QString dir;
        QString suggestedName = QStringLiteral("untitled.RISEscene");
        if (!loaded.isEmpty()) {
            QFileInfo fi(loaded);
            dir = fi.absolutePath();
            suggestedName = fi.fileName();
        }
        const QString picked = QFileDialog::getSaveFileName(
            this,
            tr("Save Scene As"),
            dir.isEmpty()
                ? suggestedName
                : (dir + QLatin1Char('/') + suggestedName),
            tr("RISE Scene Files (*.RISEscene);;All Files (*)"));
        if (picked.isEmpty()) return;   // user cancelled
        target = picked;
    }

    QString errMsg;
    const ViewportBridge::SaveStatus status =
        m_bridge->saveSceneTo(target, errMsg);
    switch (status) {
    case ViewportBridge::SaveStatus::Saved:
    case ViewportBridge::SaveStatus::NoOp:
        emit sceneSavedToPath(target, wasSaveAs);
        break;
    case ViewportBridge::SaveStatus::Refused:
        QMessageBox::warning(
            this,
            tr("Save Refused"),
            errMsg.isEmpty()
                ? tr("The save engine declined to write this file.")
                : errMsg);
        break;
    case ViewportBridge::SaveStatus::Failed:
        QMessageBox::warning(
            this,
            tr("Save Failed"),
            errMsg.isEmpty()
                ? tr("An I/O error occurred while saving the file.")
                : errMsg);
        break;
    case ViewportBridge::SaveStatus::Error:
        QMessageBox::warning(
            this,
            tr("Save Failed"),
            tr("Unexpected save state (%1).").arg(errMsg));
        break;
    }
}

// ============================================================
// Entity header + property rows
// ============================================================

void ViewportProperties::rebuildEntityHeader()
{
    // Tinted SVG icon, Theme::accentLight -- matches the chip's fixed
    // accent-tinted background/border (set once at construction, see the
    // ctor below) rather than a per-category color; PropertiesPanel.swift's
    // entityHeader uses the same fixed Theme.accentLight tint regardless of
    // category, so this mirrors Mac exactly rather than inventing a new
    // per-category coloring the design doesn't otherwise use here (the
    // outliner's RND/CAM/LGT/... tag chips are the per-category-colored
    // affordance; this header icon is deliberately uniform).
    const QString iconName = categoryIconName(m_currentSelectionCat);
    if (iconName.isEmpty()) {
        m_iconChip->setText(QString::fromUtf8("\xE2\x80\xA2"));   // • -- no dedicated icon (None)
    } else {
        // Theme::bindIconLabel keeps this pixmap correct across a
        // mixed-DPI monitor drag (plain setPixmap(iconPixmap(...,
        // devicePixelRatioF())) captures DPR once at construction and
        // goes stale) -- this label is also re-rendered repeatedly across
        // the widget's lifetime as the selection changes, which
        // bindIconLabel's re-bind path handles cleanly (no stacked event
        // filters).
        Theme::bindIconLabel(m_iconChip, iconName, 14, []{ return Theme::accentLight; });
    }

    const QString title = !m_currentSelectionName.isEmpty()
        ? m_currentSelectionName
        : (m_currentSelectionCat != Category::None ? categoryTitle(m_currentSelectionCat) : tr("Scene"));
    m_nameLabel->setText(title);

    QString meta;
    if (m_currentSelectionCat == Category::None) {
        meta = tr("Nothing selected");
    } else if (!m_currentSelectionName.isEmpty()) {
        meta = categoryTitle(m_currentSelectionCat);
    } else {
        meta = tr("No entity selected");
    }
    m_metaLabel->setText(meta);

    // "Reveal in scene file": hidden entirely unless a location was
    // resolved for the CURRENT selection (see refresh()'s fetch gate).
    if (m_sourceLineChip) {
        if (m_sourceLineKnown) {
            m_sourceLineChip->setText(QString::fromUtf8("\xE2\x8C\x97 L%1").arg(m_sourceLine));   // ⌗ L<n>
            m_sourceLineChip->show();
        } else {
            m_sourceLineChip->hide();
        }
    }

    const bool isCamera = (m_currentSelectionCat == Category::Camera);
    m_cameraAffordances->setVisible(isCamera);
    if (isCamera && m_useInViewportBtn) {
        m_useInViewportBtn->setEnabled(!m_currentSelectionName.isEmpty());
    }
}

void ViewportProperties::clearPropertyRows()
{
    if (m_rowAlive) *m_rowAlive = false;
    m_rowAlive = std::make_shared<bool>(true);
    endScrubBracket();

    auto clearInto = [](QVBoxLayout* layout) {
        if (!layout) return;
        QLayoutItem* item;
        while ((item = layout->takeAt(0)) != nullptr) {
            if (QWidget* widget = item->widget()) widget->deleteLater();
            delete item;
        }
    };
    clearInto(m_basicLayout);
    clearInto(m_advancedLayout);
    m_fields.clear();
    m_readOnly.clear();
    m_lastValue.clear();
}

void ViewportProperties::updateAdvancedToggleLabel(int advancedCount)
{
    // Bundled-SVG chevron, replacing the ▾/▸ Unicode triangles (same
    // Windows font-fallback rationale as categoryIconName above).
    // Deliberate Windows-side exception: Mac renders ▾/▸ as text, Windows
    // uses the chevron SVGs for cross-panel consistency with the
    // chat/log disclosure chevrons (approved by the orchestrating
    // session, 2026-07-23).
    // Theme::bindIconLabel keeps this pixmap correct across a mixed-DPI
    // monitor drag (plain setPixmap(iconPixmap(..., devicePixelRatioF()))
    // captures DPR once at construction and goes stale) -- this label is
    // also re-rendered repeatedly across the widget's lifetime as the
    // advanced section is toggled, which bindIconLabel's re-bind path
    // handles cleanly (no stacked event filters).
    Theme::bindIconLabel(m_advancedArrowLabel,
        m_advancedExpanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
        9, []{ return Theme::textFaint; });
    m_advancedCountLabel->setText(tr("%1 more").arg(advancedCount));
}

void ViewportProperties::onAdvancedToggleClicked()
{
    m_advancedExpanded = !m_advancedExpanded;
    m_advancedContainer->setVisible(m_advancedExpanded);
    updateAdvancedToggleLabel(m_advancedLayout ? m_advancedLayout->count() : 0);
}

void ViewportProperties::rebuildPropertyRows(bool pullFreshSnapshot)
{
    if (!m_bridge) return;
    clearPropertyRows();

    if (m_sceneEditable) {
        quint64 uuid = 0, rev = 0;
        if (m_bridge->getSceneTextVersion(&uuid, &rev) && rev != m_lastSceneRevision) {
            m_lastSceneRevision = rev;
            m_painterThumbCache.clear();
        }
    }

    // Force a refresh so the controller's per-category snapshot for the
    // PRIMARY selection is up to date before we read it -- SKIPPED when
    // `pullFreshSnapshot` is false (see this method's header doc): that
    // path deliberately reuses whatever `propertySnapshotFor` last has
    // cached on the controller side rather than paying a fresh
    // RefreshProperties round-trip.
    if (pullFreshSnapshot) {
        (void)m_bridge->propertySnapshot();
    }

    const QVector<ViewportProperty> props = m_bridge->propertySnapshotFor(m_currentSelectionCat);

    // doc 88 S10: a painter's own preview swatch, at the top of its row
    // group -- mirrors PropertiesPanel.swift's painterThumbnailHeader.
    if (m_currentSelectionCat == Category::Painter && !m_currentSelectionName.isEmpty()) {
        auto* headerWrap = new QWidget;
        auto* headerCol = new QVBoxLayout(headerWrap);
        headerCol->setContentsMargins(0, 0, 0, 4);
        headerCol->setSpacing(6);

        auto* swatchRow = new QWidget;
        auto* swatchRowLayout = new QHBoxLayout(swatchRow);
        swatchRowLayout->setContentsMargins(0, 0, 0, 0);
        swatchRowLayout->setSpacing(10);
        swatchRowLayout->addWidget(createPainterSwatchLabel(m_currentSelectionName, -1, 56));

        auto* textCol = new QWidget;
        auto* textColLayout = new QVBoxLayout(textCol);
        textColLayout->setContentsMargins(0, 0, 0, 0);
        textColLayout->setSpacing(2);
        auto* titleLbl = new QLabel(tr("Preview"));
        titleLbl->setFont(Theme::sans(10, true));
        titleLbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        auto* subLbl = new QLabel(tr("Evaluated over a synthetic UV/world patch, not a scene render."));
        subLbl->setFont(Theme::sans(9));
        subLbl->setWordWrap(true);
        subLbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        textColLayout->addWidget(titleLbl);
        textColLayout->addWidget(subLbl);
        swatchRowLayout->addWidget(textCol, 1);
        headerCol->addWidget(swatchRow);

        bool isRamp = false;
        for (const ViewportProperty& p : props) {
            if (p.name == QStringLiteral("chunk_type") && p.value == QStringLiteral("ramp_painter")) {
                isRamp = true;
                break;
            }
        }
        if (isRamp) {
            auto* gradLbl = new QLabel(tr("Gradient"));
            gradLbl->setFont(Theme::sans(10, true));
            gradLbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
            headerCol->addWidget(gradLbl);

            auto* stripLbl = new QLabel;
            stripLbl->setFixedSize(220, 20);
            stripLbl->setStyleSheet(QStringLiteral(
                "background-color: %1; border: 1px solid %2; border-radius: 4px;")
                .arg(Theme::rgba(Theme::textDim, 0.08), Theme::hex(Theme::borderHairline)));
            const QPixmap stripPx = fetchRampStripThumbnail(m_currentSelectionName, 220, 20);
            if (!stripPx.isNull()) {
                stripLbl->setPixmap(stripPx.scaled(220, 20, Qt::IgnoreAspectRatio, Qt::FastTransformation));
            }
            headerCol->addWidget(stripLbl);
        }

        m_basicLayout->addWidget(headerWrap);
    }

    // Material Editor Slice 5 (MATERIAL_EDITOR.md §4): a material's 3D
    // shaderball preview swatch at the top of its inspector row group --
    // mirrors PropertiesPanel.swift's materialThumbnailHeader.
    if (m_currentSelectionCat == Category::Material && !m_currentSelectionName.isEmpty()) {
        auto* headerWrap = new QWidget;
        auto* headerCol = new QVBoxLayout(headerWrap);
        headerCol->setContentsMargins(0, 0, 0, 4);
        headerCol->setSpacing(6);

        auto* swatchRow = new QWidget;
        auto* swatchRowLayout = new QHBoxLayout(swatchRow);
        swatchRowLayout->setContentsMargins(0, 0, 0, 0);
        swatchRowLayout->setSpacing(10);
        swatchRowLayout->addWidget(createMaterialSwatchLabel(m_currentSelectionName, 56));

        auto* textCol = new QWidget;
        auto* textColLayout = new QVBoxLayout(textCol);
        textColLayout->setContentsMargins(0, 0, 0, 0);
        textColLayout->setSpacing(2);
        auto* titleLbl = new QLabel(tr("Shaderball Preview"));
        titleLbl->setFont(Theme::sans(10, true));
        titleLbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        auto* subLbl = new QLabel(tr("Evaluated over a 3D studio sphere with spectral quadrature and checker backdrop."));
        subLbl->setFont(Theme::sans(9));
        subLbl->setWordWrap(true);
        subLbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        textColLayout->addWidget(titleLbl);
        textColLayout->addWidget(subLbl);
        swatchRowLayout->addWidget(textCol, 1);
        headerCol->addWidget(swatchRow);

        m_basicLayout->addWidget(headerWrap);
    }

    if (props.isEmpty()) {
        QString msg;
        switch (m_currentSelectionCat) {
        case Category::Animation:
            msg = tr("Selecting activates this animation path.");
            break;
        case Category::SceneVariant:
            msg = tr("Selecting activates this scene variant \xE2\x80\x94 the scene re-derives with it active.");
            break;
        case Category::None:
            msg = tr("Select an item in the outliner.");
            break;
        default:
            msg = tr("Select an entity in the outliner to see its properties.");
            break;
        }
        m_emptyMessage->setText(msg);
        m_emptyMessage->setVisible(true);
        m_advancedToggleRow->setVisible(false);
        m_advancedContainer->setVisible(false);
        return;
    }

    m_emptyMessage->setVisible(false);

    QVector<ViewportProperty> basic, advanced;
    splitBasicAdvanced(props, basic, advanced);

    for (const ViewportProperty& p : basic) buildPropertyRow(p, m_basicLayout);
    for (const ViewportProperty& p : advanced) buildPropertyRow(p, m_advancedLayout);

    m_advancedToggleRow->setVisible(!advanced.isEmpty());
    updateAdvancedToggleLabel(advanced.size());
    m_advancedContainer->setVisible(!advanced.isEmpty() && m_advancedExpanded);
}

void ViewportProperties::buildPropertyRow(const ViewportProperty& p, QVBoxLayout* into)
{
    const std::shared_ptr<bool> rowAlive = m_rowAlive;
    auto* row = new QWidget;
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);
    rowLayout->setAlignment(Qt::AlignTop);

    // Source traceability: right-clicking a property row reveals THIS
    // param's exact `role value` span in the Scene-file editor.  Shown
    // only when the current entity resolves to a scene-file chunk
    // (m_sourceLineKnown) -- the same gate as the whole-entity ⌗ chip.
    // Captures the param name by value; the current category/name come
    // from the members at right-click time (the row is rebuilt on any
    // selection change, so they always match this row's entity).
    row->setContextMenuPolicy(Qt::CustomContextMenu);
    const QString paramName = p.name;
    // The synthetic leading identity row ("chunk_type") has no param span of
    // its own -- it's the chunk's keyword, not a descriptor parameter -- so
    // reveal the WHOLE chunk (empty param, per ResolveSourceSpan's documented
    // convention) instead of name-matching "chunk_type" against a real
    // descriptor param.  Without this, right-clicking the identity row on a
    // chunk that also declares a genuine `type` parameter (sdf3d_painter,
    // polynomial_function2d_painter) used to jump to that param's line
    // instead of the chunk.
    const bool isIdentityRow = (paramName == QStringLiteral("chunk_type"));
    const QString revealParamName = isIdentityRow ? QString() : paramName;
    connect(row, &QWidget::customContextMenuRequested, this,
            [this, row, paramName, revealParamName, isIdentityRow](const QPoint& pos) {
                if (!m_sourceLineKnown) return;
                QMenu menu(this);
                // Label matches the action: the identity row reveals the
                // whole chunk, not a param named "chunk_type".
                QAction* reveal = menu.addAction(isIdentityRow
                    ? tr("Reveal chunk in Scene File")
                    : tr("Reveal \xE2\x80\x9C%1\xE2\x80\x9D in Scene File").arg(paramName));   // “<param>”
                connect(reveal, &QAction::triggered, this, [this, revealParamName]() {
                    emit revealParamRequested(m_currentSelectionCat, m_currentSelectionName, revealParamName);
                });
                m_contextMenuOpen = true;
                menu.exec(row->mapToGlobal(pos));
                m_contextMenuOpen = false;
            });

    if (m_currentSelectionCat == Category::Painter) {
        const int stageIdx = defStageIndex(p.name);
        if (stageIdx >= 0 && !m_currentSelectionName.isEmpty()) {
            rowLayout->addWidget(createPainterSwatchLabel(m_currentSelectionName, stageIdx, 28));
        }
    }

    auto* label = new QLabel(p.name);
    label->setFont(Theme::sans(11));
    label->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    label->setFixedWidth(82);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    rowLayout->addWidget(label);

    auto* cellCol = new QWidget;
    auto* cellColLayout = new QVBoxLayout(cellCol);
    cellColLayout->setContentsMargins(0, 0, 0, 0);
    cellColLayout->setSpacing(4);

    // doc 88 S4b (Tier-1 param sliders): a row that carries an authored
    // numeric range gets a QSlider ABOVE its ordinary value cell -- never
    // INSTEAD of it.  Mirrors PropertiesPanel.swift's PropertyRowView.valueCell
    // (~line 758) + ParamSliderCell (~line 806): the range is a presentation
    // hint, not a validation rule -- the scene language accepts values
    // outside it, an expression `param` line carries metadata (the param's
    // own name, `label`, `step`, ...) the slider does not touch, and the
    // value cell added by the branches below remains the only way to reach
    // either.  So: slider for the common gesture, field for everything the
    // slider cannot say.
    if (p.editable && p.hasRange) {
        // (S5) `p` is assembled on the GUI bridge from multiple separate
        // lock acquisitions (doc 88 S4b's snapshot construction), not one
        // atomic read -- a live scene mutation can swap the underlying
        // snapshot in between two of them, tearing `rangeMin` from one
        // snapshot and `rangeMax` from a different (older or newer) one.
        // Mirrors PropertiesPanel.swift's ParamSliderCell `hasValidRange`
        // comment (~line 814): compute the validity ONCE, here, into a
        // local -- never inline the comparison at each use site, where a
        // second torn re-read could disagree with the first.  Unlike
        // Swift's `Range` (a hard trap on an inverted bound), QSlider::
        // setRange won't crash on rangeMax <= rangeMin, but it WILL
        // silently clamp every position to the same endpoint and produce
        // an inert slider -- skip the block entirely instead.
        const bool hasValidRange = p.rangeMax > p.rangeMin;
        if (hasValidRange) {
            const double span = p.rangeMax - p.rangeMin;

            // QSlider is integer-only -- map the authored [rangeMin,
            // rangeMax] onto an integer tick count.  A discrete rangeStep
            // gets enough ticks to hit every authored step; "continuous"
            // (rangeStep == 0) gets 1000 subdivisions -- fine enough
            // granularity that no tick reads as a jump.  DELIBERATE
            // platform divergence: the Mac ParamSliderCell's continuous
            // fallback is an absolute 0.0001 step (SwiftUI sliders run on
            // doubles), which an integer QSlider cannot express for wide
            // spans (span 1e6 would need 1e10 ticks) -- a span-relative
            // subdivision is the sanctioned Qt equivalent.
            // Both bounds and the step are AUTHORED metadata, so `span /
            // step` is arbitrary -- a step tiny relative to the span
            // (`min 0 max 1e6 step 0.0001`) would overflow the int tick
            // cast into a garbage slider range.  Past 100k ticks a step
            // grid is indistinguishable from continuous anyway, so such
            // a row falls back to the continuous 1000-subdivision path.
            int ticks;
            double step;
            const double authoredTicks =
                (p.rangeStep > 0) ? (span / p.rangeStep) : 0.0;
            if (p.rangeStep > 0 && p.rangeStep <= span && authoredTicks <= 100000.0) {
                step = p.rangeStep;
                // ceil, not round: when the step doesn't evenly divide the
                // span, rounding DOWN would leave rangeMax unreachable from
                // the slider (min 0 max 10 step 2.9 -> 3 ticks tops out at
                // 8.7).  An extra tick past the span is fine -- tickToValue
                // below clamps it, so the top tick lands exactly on
                // rangeMax.  The 1e-9 slack keeps an evenly-dividing step
                // from gaining a spurious extra tick to FP error.
                ticks = std::max(1, static_cast<int>(std::ceil(authoredTicks - 1e-9)));
            } else {
                ticks = 1000;
                step = span / 1000.0;
            }

            // Tick -> value.  Clamped to rangeMax: a rangeStep that
            // doesn't divide the span evenly would otherwise let the last
            // tick overshoot the authored maximum.
            auto tickToValue = [rangeMin = p.rangeMin, rangeMax = p.rangeMax, step](int t) {
                return std::min(rangeMin + static_cast<double>(t) * step, rangeMax);
            };

            // Initial value: the row's own current numeric token, else
            // rangeMin (mirrors Swift's `.onAppear` fallback).  NOT
            // clamped -- an out-of-range authored value is displayed
            // honestly in the label below.  Clamped separately, below,
            // for the slider's own initial POSITION only.
            const bool skipFirstToken =
                p.name == QStringLiteral("param")
                || p.name.startsWith(QStringLiteral("param["));
            const int valueKind = p.kind;
            double initValue = p.rangeMin;
            {
                int tokPos = 0, tokLen = 0;
                double tokValue = 0.0;
                if (numericToken(p.value, &tokPos, &tokLen, &tokValue, skipFirstToken)) initValue = tokValue;
            }
            double clampedInit = initValue;
            if (clampedInit < p.rangeMin) clampedInit = p.rangeMin;
            if (clampedInit > p.rangeMax) clampedInit = p.rangeMax;
            const int initTick = static_cast<int>(std::llround((clampedInit - p.rangeMin) / step));

            auto* sliderRow = new QWidget;
            auto* sliderRowLayout = new QHBoxLayout(sliderRow);
            sliderRowLayout->setContentsMargins(0, 0, 0, 0);
            sliderRowLayout->setSpacing(6);

            auto* slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, ticks);

            auto* valueLabel = new QLabel(formatScrubbed(initValue, valueKind));
            valueLabel->setFont(Theme::mono(10));
            valueLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
            valueLabel->setFixedWidth(46);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            // Set the initial position BEFORE connecting any signals below
            // -- QSlider::setValue() emits valueChanged(), and construction
            // must not fire a spurious commit.
            slider->setValue(initTick);

            sliderRowLayout->addWidget(slider, 1);
            sliderRowLayout->addWidget(valueLabel);
            cellColLayout->addWidget(sliderRow);

            const QString capturedLine = p.value;
            const QString capturedName = p.name;
            auto movedFromInit = std::make_shared<bool>(false);

            // Commit: re-derives the token position from the CAPTURED
            // original line (not anything mutated since) -- mirrors
            // PropertiesPanel.swift's ParamSliderCell.commit() (~line
            // 856).  Guards exactly like that commit(): if the captured
            // line has no numeric token to drive, there's nothing to
            // rewrite, so return without touching the scene.  Each commit
            // is a real CST edit + re-derive + undo entry -- that is why
            // this fires on release (or a discrete groove-click/keyboard
            // gesture) and NEVER per `valueChanged` tick during an actual
            // drag, which would otherwise push one per frame.
            auto commitTick = [this, rowAlive, slider, capturedLine, capturedName, tickToValue, initTick, initValue, valueLabel, skipFirstToken, valueKind, movedFromInit](int tick) {
                if (!rowAlive || !*rowAlive) return;
                // The initial POSITION is a quantized approximation of the
                // row's actual value (which need not sit on the tick grid,
                // or even inside the range).  Releasing on that same tick
                // WITHOUT having moved away during the drag means the user
                // never moved -- committing would silently snap the authored
                // value to the grid (actual 10 on a step-2.9 grid would
                // commit as 8.7).  If the user DID move away and back to
                // `initTick` when `initValue` was off-grid or out-of-range,
                // `movedFromInit` is true and we commit the tick's value.
                if (tick == initTick && !*movedFromInit) {
                    valueLabel->setText(formatScrubbed(initValue, valueKind));
                    return;
                }
                int tokPos = 0, tokLen = 0;
                double unused = 0.0;
                if (!numericToken(capturedLine, &tokPos, &tokLen, &unused, skipFirstToken)) {
                    valueLabel->setText(formatScrubbed(initValue, valueKind));
                    *movedFromInit = false;
                    QSignalBlocker b(slider);
                    slider->setValue(initTick);
                    return;
                }
                QString newLine = capturedLine;
                newLine.replace(tokPos, tokLen, formatScrubbed(tickToValue(tick), valueKind));
                if (newLine == capturedLine) {
                    // Rewrite is byte-identical -- restore the display and
                    // thumb to the initial state so the UI stays in sync.
                    valueLabel->setText(formatScrubbed(initValue, valueKind));
                    *movedFromInit = false;
                    QSignalBlocker b(slider);
                    slider->setValue(initTick);
                    return;
                }
                commitEdit(capturedName, newLine);
            };

            connect(slider, &QSlider::sliderPressed, this, [this, rowAlive, movedFromInit]() {
                if (!rowAlive || !*rowAlive) return;
                // Blocks the per-frame refresh() rebuild that would
                // deleteLater() this very slider mid-drag -- same guard
                // ScrubHandle uses (see the beginPropertyScrub bracket in
                // the ScrubHandle wiring later in this method).
                // No beginPropertyScrub(): unlike ScrubHandle, there are
                // no mid-drag commits to coalesce here -- this slider
                // commits exactly once per gesture, on release.
                *movedFromInit = false;
                m_scrubbing = true;
            });

            connect(slider, &QSlider::valueChanged, this,
                    [rowAlive, slider, valueLabel, tickToValue, commitTick, initTick, initValue, valueKind, movedFromInit](int tick) {
                if (!rowAlive || !*rowAlive) return;
                if (tick != initTick) *movedFromInit = true;
                valueLabel->setText(formatScrubbed((tick == initTick && !*movedFromInit) ? initValue : tickToValue(tick), valueKind));
                // A groove-click (page step) or keyboard arrow never sets
                // isSliderDown() -- treat those as a complete one-shot
                // gesture and commit right here.  An actual drag also
                // fires this per tick, but isSliderDown() stays true for
                // its whole duration, so this branch is skipped until
                // sliderReleased below.  Relies on the initial setValue()
                // above running BEFORE this connect() -- otherwise that
                // very first setValue would land here as a "commit".
                // A click-and-HELD groove press (or held arrow key) can't
                // stack auto-repeat commits: the first commit's refresh()
                // rebuilds the rows, and this slider's deferred delete is
                // processed on the next event-loop pass -- long before
                // QAbstractSlider's 500ms repeat timer (or a keyboard
                // auto-repeat aimed at the now-unfocused replacement)
                // could fire a second one.
                if (!slider->isSliderDown()) commitTick(tick);
            });

            connect(slider, &QSlider::sliderReleased, this, [this, rowAlive, slider, commitTick]() {
                if (!rowAlive || !*rowAlive) return;
                m_scrubbing = false;
                commitTick(slider->value());
            });
            // A swallowed release (mouse grab stolen by a modal dialog
            // mid-drag) would leave m_scrubbing stuck true -- recovered by
            // the no-button self-heal at the top of refresh(), not here: a
            // QWidget never sees QEvent::UngrabMouse (that event is a
            // QGraphicsItem/QQuickItem mechanism), and a destructor net a
            // la ScrubHandle's can't work for this flag, because a stuck
            // flag is precisely what blocks the rebuild that would destroy
            // the slider.  The aborted drag's in-flight value is DROPPED,
            // deliberately: this slider commits only completed gestures,
            // and silently landing a half-drag value because a dialog
            // interrupted it would be worse than reverting.  (ScrubHandle
            // differs -- it commits per move tick, so its aborted drags
            // keep the last-dragged value; that's inherent to its design,
            // not a bar this slider should match.)
        }
    }

    if (!p.editable) {
        // Read-only well.
        auto* wellWidget = new QWidget;
        wellWidget->setStyleSheet(wellStyleSheet());
        auto* wellLayout = new QHBoxLayout(wellWidget);
        wellLayout->setContentsMargins(8, 5, 8, 5);
        wellLayout->setSpacing(6);

        if (p.kind == 9 /* Reference */) {
            if (p.targetsPainter && !p.value.isEmpty() && p.value != QStringLiteral("none")
                && !p.value.startsWith(QLatin1Char('('))) {
                wellLayout->addWidget(createPainterSwatchLabel(p.value, -1, 18));
            } else {
                auto* dot = new QLabel;
                dot->setFixedSize(16, 16);
                dot->setStyleSheet(QStringLiteral(
                    "background-color: %1; border-radius: 4px;").arg(Theme::rgba(Theme::purple, 0.5)));
                wellLayout->addWidget(dot);
            }
        }

        auto* valueLbl = new QLabel(p.value);
        valueLbl->setFont(Theme::mono(11));
        valueLbl->setWordWrap(true);
        valueLbl->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textMuted)));
        wellLayout->addWidget(valueLbl, 1);

        if (!p.unitLabel.isEmpty()) {
            auto* unit = new QLabel(p.unitLabel);
            unit->setFont(Theme::mono(9));
            unit->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
            wellLayout->addWidget(unit);
        }
        auto* badge = new QLabel(tr("read-only"));
        badge->setFont(Theme::mono(9));
        badge->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textGhost)));
        wellLayout->addWidget(badge);

        cellColLayout->addWidget(wellWidget);
        m_readOnly.insert(p.name, valueLbl);
    } else if (p.kind == 0 /* Bool */) {
        const bool isOn = p.value.trimmed().compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
            || p.value.trimmed() == QLatin1String("1");
        const QString name = p.name;
        auto* pill = new BoolPill(isOn, /*editable=*/true, [this, name](bool newVal) {
            commitEdit(name, newVal ? QStringLiteral("true") : QStringLiteral("false"));
        });
        auto* pillRow = new QWidget;
        auto* pillRowLayout = new QHBoxLayout(pillRow);
        pillRowLayout->setContentsMargins(0, 0, 0, 0);
        pillRowLayout->addWidget(pill);
        pillRowLayout->addStretch(1);
        cellColLayout->addWidget(pillRow);
    } else if (p.kind == 3 /* DoubleVec3 */) {
        // Three tinted wells (X red / Y green / Z blue) -- the wire
        // format is three space-separated %.6g doubles (see
        // CameraIntrospection.cpp's FormatVec3/FormatPoint3); joining
        // the three edited fields back with spaces round-trips exactly.
        auto* vecRow = new QWidget;
        auto* vecLayout = new QHBoxLayout(vecRow);
        vecLayout->setContentsMargins(0, 0, 0, 0);
        vecLayout->setSpacing(4);

        QStringList comps = p.value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        while (comps.size() < 3) comps.append(QStringLiteral("0"));

        const QColor tint[3] = { Theme::error, Theme::successLight, Theme::accentLight };
        auto edits = std::make_shared<QVector<QLineEdit*>>();
        for (int i = 0; i < 3; ++i) {
            auto* wellWidget = new QWidget;
            wellWidget->setStyleSheet(wellStyleSheet());
            auto* wellLayout = new QHBoxLayout(wellWidget);
            wellLayout->setContentsMargins(8, 5, 8, 5);
            auto* edit = new QLineEdit(comps.value(i, QStringLiteral("0")));
            edit->setFont(Theme::mono(11));
            edit->setFrame(false);
            edit->setStyleSheet(lineEditStyleSheet(tint[i]));
            edit->setEnabled(p.editable);
            wellLayout->addWidget(edit);
            vecLayout->addWidget(wellWidget, 1);
            edits->append(edit);
        }
        const QString name = p.name;
        for (QLineEdit* edit : *edits) {
            connect(edit, &QLineEdit::editingFinished, this, [this, rowAlive, edits, name]() {
                if (!rowAlive || !*rowAlive) return;
                QStringList parts;
                for (QLineEdit* e : *edits) parts << e->text();
                commitEdit(name, parts.join(QLatin1Char(' ')));
            });
        }
        cellColLayout->addWidget(vecRow);
    } else if (p.kind == 8 /* Enum */ && !p.presets.isEmpty() && p.editable) {
        // GUI redesign 2026-07-22 (typed-editor parity with the Mac
        // EnumChipCell): an Enum row with a pick list renders a proper
        // combo box, not a free-text well -- the descriptor's enumValues
        // arrive as presets (CstIntrospection folds them), so the combo
        // IS the closed vocabulary.
        auto* combo = new QComboBox;
        combo->setFont(Theme::mono(11));
        int cur = -1;
        for (const ViewportPropertyPreset& preset : p.presets) {
            combo->addItem(preset.label, preset.value);
            if (preset.value == p.value) cur = combo->count() - 1;
        }
        if (cur >= 0) {
            combo->setCurrentIndex(cur);
        } else if (!p.value.isEmpty()) {
            // The current value isn't one of the descriptor's enum values
            // (an older scene / hand-edit).  setEditText is a no-op on a
            // non-editable combo, so insert the raw value as a leading item
            // and select it -- the user sees the honest current value and
            // can still switch to a canonical one.
            combo->insertItem(0, p.value, p.value);
            combo->setCurrentIndex(0);
        }
        const QString name = p.name;
        connect(combo, QOverload<int>::of(&QComboBox::activated), this,
                [this, combo, name](int idx) {
            commitEdit(name, combo->itemData(idx).toString());
        });
        auto* comboRow = new QWidget;
        auto* comboRowLayout = new QHBoxLayout(comboRow);
        comboRowLayout->setContentsMargins(0, 0, 0, 0);
        comboRowLayout->addWidget(combo, 1);
        cellColLayout->addWidget(comboRow);
    } else {
        auto* wellWidget = new QWidget;
        wellWidget->setStyleSheet(wellStyleSheet());
        auto* fieldRowLayout = new QHBoxLayout(wellWidget);
        fieldRowLayout->setContentsMargins(6, 3, 6, 3);
        fieldRowLayout->setSpacing(4);

        auto* edit = new QLineEdit(p.value);
        edit->setObjectName(p.name);
        edit->setFont(Theme::mono(11));
        edit->setFrame(false);
        edit->setStyleSheet(lineEditStyleSheet(Theme::textPrimary));
        connect(edit, &QLineEdit::editingFinished,
                this, &ViewportProperties::onLineEditFinished);

        if (p.kind == 9 /* Reference */) {
            if (p.targetsPainter && !p.value.isEmpty() && p.value != QStringLiteral("none")
                && !p.value.startsWith(QLatin1Char('('))) {
                fieldRowLayout->addWidget(createPainterSwatchLabel(p.value, -1, 18));
            } else {
                auto* dot = new QLabel;
                dot->setFixedSize(16, 16);
                dot->setStyleSheet(QStringLiteral(
                    "background-color: %1; border-radius: 4px;").arg(Theme::rgba(Theme::purple, 0.5)));
                fieldRowLayout->addWidget(dot);
            }

            // Jump-to-definition (GUI redesign 2026-07-22): right-click a
            // reference value -> "Jump to Definition".  Resolution happens
            // at menu-open time against the live managers (bridge->core),
            // so a dangling reference simply shows no extra item.  The
            // standard edit menu (cut/copy/paste) is preserved.
            edit->setContextMenuPolicy(Qt::CustomContextMenu);
            const int rowIndex = p.index;
            connect(edit, &QLineEdit::customContextMenuRequested, this,
                    [this, edit, rowIndex](const QPoint& pos) {
                QMenu* menu = edit->createStandardContextMenu();
                // Reparent to the panel: refresh() is suppressed below while
                // the menu is modal, but reparenting also means the menu
                // outlives its origin line-edit belt-and-suspenders.
                menu->setParent(this, Qt::Popup);
                ViewportBridge::Category jumpCat = ViewportBridge::Category::None;
                QString jumpName;
                QAction* jump = nullptr;
                if (m_bridge && m_bridge->propertyJumpTargetFor(
                        m_currentSelectionCat, rowIndex, &jumpCat, &jumpName)) {
                    menu->addSeparator();
                    jump = menu->addAction(
                        tr("Jump to Definition of \"%1\"").arg(jumpName));
                }
                const QPoint global = edit->mapToGlobal(pos);
                m_contextMenuOpen = true;   // suppress the per-frame rebuild that would delete `edit`
                QAction* chosen = menu->exec(global);
                m_contextMenuOpen = false;
                menu->deleteLater();
                // Run the jump AFTER m_contextMenuOpen is cleared -- QAction::triggered
                // fires inside menu->exec() while m_contextMenuOpen is still true,
                // which would otherwise cause refresh() to early-return!
                if (chosen && chosen == jump && m_bridge && m_bridge->setSelection(jumpCat, jumpName)) {
                    refresh();
                }
            });
        }

        if (isScrubbableKind(p.kind)) {
            auto* handle = new ScrubHandle(
                edit, p.name, p.kind,
                [this, rowAlive](const QString& n, const QString& v) {
                    // A2: don't gate on the bool return -- a per-drag-
                    // tick callback can't afford a full refresh()
                    // (rebuildPropertyRows() would deleteLater() the
                    // very ScrubHandle whose mouseMoveEvent is on the
                    // stack; see the m_scrubbing guard in refresh()),
                    // so re-pull this property's LAST-KNOWN CACHED
                    // value and cache THAT rather than the submitted
                    // string.  Harmless: m_lastValue's only consumer
                    // is onLineEditFinished, unreachable mid-drag; the
                    // drag-end path (endBracket below) does a full
                    // refresh() that re-seeds from the genuinely live
                    // snapshot.
                    if (!rowAlive || !*rowAlive || !m_bridge) return;
                    m_bridge->setPropertyForCategory(m_currentSelectionCat, n, v);
                    const QVector<ViewportProperty> live =
                        m_bridge->propertySnapshotFor(m_currentSelectionCat);
                    for (const ViewportProperty& lp : live) {
                        if (lp.name == n) {
                            m_lastValue.insert(n, lp.value);
                            break;
                        }
                    }
                },
                [this, rowAlive]() {
                    if (!rowAlive || !*rowAlive) return;
                    beginScrubBracket();
                },
                [this, rowAlive]() {
                    if (!rowAlive || !*rowAlive) return;
                    endScrubBracket();
                    refresh();
                },
                rowAlive);
            fieldRowLayout->addWidget(handle);
        }
        fieldRowLayout->addWidget(edit, 1);

        if (!p.unitLabel.isEmpty()) {
            auto* unit = new QLabel(p.unitLabel);
            unit->setFont(Theme::mono(9));
            unit->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
            fieldRowLayout->addWidget(unit);
        }

        if (p.kind == 7 /* Filename */) {
            auto* browseBtn = new QToolButton;
            // Bundled-SVG "folder" icon, replacing the "…" ellipsis --
            // mirrors PropertiesPanel.swift:940's FilenameCell browse
            // button (`Image(systemName: "folder")`).
            browseBtn->setIcon(Theme::icon("folder", 10, Theme::textDim));
            browseBtn->setIconSize(QSize(10, 10));
            browseBtn->setToolTip(tr("Browse\xE2\x80\xA6"));
            // `color` is dead here now that the button is icon-only (the
            // icon carries its own tint via Theme::icon above); border
            // suppression is the only thing this rule still does.
            browseBtn->setStyleSheet(QStringLiteral("QToolButton { border: none; }"));
            const QString name = p.name;
            connect(browseBtn, &QToolButton::clicked, this, [this, edit, name]() {
                const QString picked = QFileDialog::getOpenFileName(this, tr("Choose File"));
                if (picked.isEmpty()) return;
                edit->setText(picked);
                commitEdit(name, picked);
            });
            fieldRowLayout->addWidget(browseBtn);
        } else if (!p.presets.isEmpty()) {
            auto* presetButton = new QToolButton;
            // Bundled-SVG "list" icon, replacing the "⋮" glyph -- mirrors
            // PropertiesPanel.swift:725's PresetMenu label
            // (`Image(systemName: "list.bullet")`).
            presetButton->setIcon(Theme::icon("list", 10, Theme::textDim));
            presetButton->setIconSize(QSize(10, 10));
            presetButton->setToolTip(tr("Quick-pick presets"));
            presetButton->setPopupMode(QToolButton::InstantPopup);
            // `color` is dead here now that the button is icon-only (the
            // icon carries its own tint via Theme::icon above); border
            // suppression is the only thing this rule still does.
            presetButton->setStyleSheet(QStringLiteral("QToolButton { border: none; }"));
            auto* menu = new QMenu(presetButton);
            const QString propName = p.name;
            for (const ViewportPropertyPreset& preset : p.presets) {
                const QString lbl = preset.label;
                const QString val = preset.value;
                QAction* action = menu->addAction(lbl);
                connect(action, &QAction::triggered, this,
                        [this, propName, val]() { commitEdit(propName, val); });
            }
            presetButton->setMenu(menu);
            fieldRowLayout->addWidget(presetButton);
        }

        cellColLayout->addWidget(wellWidget);
        m_fields.insert(p.name, edit);
        m_lastValue.insert(p.name, p.value);

        if (p.kind == 9 /* Reference */ && p.editable && p.inlinePainterKind != 0) {
            const QString slotParam = p.name;

            if (p.inlinePainterRefCount > 1) {
                auto* sharedRow = new QWidget;
                auto* sharedLayout = new QHBoxLayout(sharedRow);
                sharedLayout->setContentsMargins(0, 0, 0, 0);
                sharedLayout->setSpacing(6);

                auto* sharedBadge = new QLabel(tr("Shared (%1)").arg(p.inlinePainterRefCount));
                sharedBadge->setFont(Theme::mono(9));
                sharedBadge->setStyleSheet(QStringLiteral(
                    "color: %1; background-color: %2; border: 1px solid %3; border-radius: 3px; padding: 1px 5px;")
                    .arg(Theme::hex(Theme::textDim), Theme::rgba(Theme::textDim, 0.12), Theme::hex(Theme::borderHairline)));
                sharedBadge->setToolTip(tr("Referenced by %1 slots in the scene. Editing updates all of them unless you Fork first.")
                    .arg(p.inlinePainterRefCount));
                sharedLayout->addWidget(sharedBadge);

                auto* forkBtn = new QToolButton;
                forkBtn->setText(tr("Fork"));
                forkBtn->setFont(Theme::sans(10, true));
                forkBtn->setCursor(Qt::PointingHandCursor);
                forkBtn->setToolTip(tr("Duplicate this constant painter and rebind only this slot to the new copy"));
                forkBtn->setStyleSheet(QStringLiteral(
                    "QToolButton { color: %1; background-color: %2; border: 1px solid %3; border-radius: 3px; padding: 1px 6px; }")
                    .arg(Theme::hex(Theme::accentLight), Theme::rgba(Theme::accent, 0.15), Theme::rgba(Theme::accent, 0.35)));
                connect(forkBtn, &QToolButton::clicked, this, [this, slotParam]() {
                    forkInlinePainter(slotParam);
                });
                sharedLayout->addWidget(forkBtn);
                sharedLayout->addStretch(1);
                cellColLayout->addWidget(sharedRow);
            }

            if (p.inlinePainterKind == 1 /* UniformColor */) {
                auto* vecRow = new QWidget;
                auto* vecLayout = new QHBoxLayout(vecRow);
                vecLayout->setContentsMargins(0, 0, 0, 0);
                vecLayout->setSpacing(4);

                QStringList comps = p.inlinePainterValue.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                while (comps.size() < 3) comps.append(QStringLiteral("0"));
                const double r0 = std::clamp(comps[0].toDouble(), 0.0, 1.0);
                const double g0 = std::clamp(comps[1].toDouble(), 0.0, 1.0);
                const double b0 = std::clamp(comps[2].toDouble(), 0.0, 1.0);

                auto* colorBtn = new QToolButton;
                colorBtn->setFixedSize(22, 22);
                colorBtn->setCursor(Qt::PointingHandCursor);
                colorBtn->setToolTip(tr("Pick colour"));
                const QColor initColor = QColor::fromRgbF(
                    static_cast<float>(r0), static_cast<float>(g0), static_cast<float>(b0));
                colorBtn->setStyleSheet(QStringLiteral(
                    "QToolButton { background-color: %1; border: 1px solid %2; border-radius: 4px; }")
                    .arg(Theme::hex(initColor), Theme::hex(Theme::borderLight)));
                const QColor tint[3] = { Theme::error, Theme::successLight, Theme::accentLight };
                auto edits = std::make_shared<QVector<QLineEdit*>>();
                auto lastCommittedColor = std::make_shared<QString>(comps.mid(0, 3).join(QLatin1Char(' ')).trimmed());
                connect(colorBtn, &QToolButton::clicked, this, [this, slotParam, edits, initColor, lastCommittedColor]() {
                    QColor seedColor = initColor;
                    if (edits->size() == 3) {
                        const double cr = std::clamp((*edits)[0]->text().toDouble(), 0.0, 1.0);
                        const double cg = std::clamp((*edits)[1]->text().toDouble(), 0.0, 1.0);
                        const double cb = std::clamp((*edits)[2]->text().toDouble(), 0.0, 1.0);
                        seedColor = QColor::fromRgbF(
                            static_cast<float>(cr), static_cast<float>(cg), static_cast<float>(cb));
                    }
                    m_contextMenuOpen = true;
                    const QColor picked = QColorDialog::getColor(seedColor, this, tr("Select Painter Colour"));
                    m_contextMenuOpen = false;
                    if (!picked.isValid()) return;
                    const QString formatted = QStringLiteral("%1 %2 %3")
                        .arg(formatScrubbed(picked.redF(), 2),
                             formatScrubbed(picked.greenF(), 2),
                             formatScrubbed(picked.blueF(), 2));
                    if (formatted == *lastCommittedColor) return;
                    *lastCommittedColor = formatted;
                    commitInlinePainterEdit(slotParam, formatted);
                });
                vecLayout->addWidget(colorBtn);

                for (int i = 0; i < 3; ++i) {
                    auto* subWell = new QWidget;
                    subWell->setStyleSheet(wellStyleSheet());
                    auto* subLayout = new QHBoxLayout(subWell);
                    subLayout->setContentsMargins(6, 3, 6, 3);
                    auto* subEdit = new QLineEdit(comps.value(i, QStringLiteral("0")));
                    subEdit->setFont(Theme::mono(11));
                    subEdit->setFrame(false);
                    subEdit->setStyleSheet(lineEditStyleSheet(tint[i]));
                    subLayout->addWidget(subEdit);
                    vecLayout->addWidget(subWell, 1);
                    edits->append(subEdit);
                }
                for (QLineEdit* subEdit : *edits) {
                    connect(subEdit, &QLineEdit::editingFinished, this, [this, rowAlive, edits, slotParam, lastCommittedColor]() {
                        if (!rowAlive || !*rowAlive) return;
                        QStringList parts;
                        for (QLineEdit* e : *edits) parts << e->text().trimmed();
                        const QString joined = parts.join(QLatin1Char(' ')).trimmed();
                        if (joined.isEmpty() || joined == *lastCommittedColor) return;
                        *lastCommittedColor = joined;
                        commitInlinePainterEdit(slotParam, joined);
                    });
                }
                cellColLayout->addWidget(vecRow);
            } else if (p.inlinePainterKind == 2 /* ScalarValue */) {
                const bool hasValidRange = (p.rangeMax > p.rangeMin);
                const double sMin = p.rangeMin;
                const double sMax = p.rangeMax;
                const double span = sMax - sMin;
                bool okInit = false;
                double initVal = p.inlinePainterValue.trimmed().toDouble(&okInit);
                if (!okInit || !std::isfinite(initVal)) initVal = hasValidRange ? sMin : 0.0;

                auto* scalarWell = new QWidget;
                scalarWell->setStyleSheet(wellStyleSheet());
                auto* scalarLayout = new QHBoxLayout(scalarWell);
                scalarLayout->setContentsMargins(6, 3, 6, 3);
                scalarLayout->setSpacing(4);

                const QString initScalarText = p.inlinePainterValue.trimmed();
                auto lastCommittedScalar = std::make_shared<QString>(initScalarText);
                auto* scalarEdit = new QLineEdit(initScalarText);
                scalarEdit->setFont(Theme::mono(11));
                scalarEdit->setFrame(false);
                scalarEdit->setStyleSheet(lineEditStyleSheet(Theme::textPrimary));

                QSlider* slider = nullptr;
                QLabel* valueLabel = nullptr;
                int ticks = 1000;
                double step = 0.001;

                if (hasValidRange) {
                    const double authoredTicks =
                        (p.rangeStep > 0) ? (span / p.rangeStep) : 0.0;
                    if (p.rangeStep > 0 && p.rangeStep <= span && authoredTicks <= 100000.0) {
                        step = p.rangeStep;
                        ticks = std::max(1, static_cast<int>(std::ceil(authoredTicks - 1e-9)));
                    } else {
                        ticks = 1000;
                        step = span / 1000.0;
                    }
                    auto tickToVal = [sMin, sMax, step, ticks](int tick) {
                        if (tick <= 0) return sMin;
                        if (tick >= ticks) return sMax;
                        return std::min(std::max(sMin + static_cast<double>(tick) * step, sMin), sMax);
                    };
                    const double clamped = std::min(std::max(initVal, sMin), sMax);
                    const int initTick = std::min(std::max(
                        static_cast<int>(std::llround((clamped - sMin) / step)), 0), ticks);

                    auto* sliderRow = new QWidget;
                    auto* sliderRowLayout = new QHBoxLayout(sliderRow);
                    sliderRowLayout->setContentsMargins(0, 0, 0, 0);
                    sliderRowLayout->setSpacing(6);

                    slider = new QSlider(Qt::Horizontal);
                    slider->setRange(0, ticks);
                    slider->setValue(initTick);

                    valueLabel = new QLabel(formatScrubbed(initVal, 2));
                    valueLabel->setFont(Theme::mono(10));
                    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    valueLabel->setMinimumWidth(44);
                    valueLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));

                    sliderRowLayout->addWidget(slider, 1);
                    sliderRowLayout->addWidget(valueLabel);
                    cellColLayout->addWidget(sliderRow);

                    auto movedFromInit = std::make_shared<bool>(false);
                    auto commitTick = [this, rowAlive, slotParam, tickToVal, initTick, initVal, valueLabel, scalarEdit, movedFromInit, lastCommittedScalar](int tick) {
                        if (!rowAlive || !*rowAlive) return;
                        if (tick == initTick && !*movedFromInit) {
                            valueLabel->setText(formatScrubbed(initVal, 2));
                            return;
                        }
                        const QString formatted = formatScrubbed(tickToVal(tick), 2);
                        scalarEdit->setText(formatted);
                        if (formatted == *lastCommittedScalar) return;
                        *lastCommittedScalar = formatted;
                        commitInlinePainterEdit(slotParam, formatted);
                    };
                    connect(slider, &QSlider::sliderPressed, this, [this, rowAlive, movedFromInit]() {
                        if (!rowAlive || !*rowAlive) return;
                        *movedFromInit = false;
                        m_scrubbing = true;
                    });
                    connect(slider, &QSlider::valueChanged, this,
                            [rowAlive, slider, valueLabel, scalarEdit, tickToVal, commitTick, initTick, initVal, movedFromInit](int tick) {
                        if (!rowAlive || !*rowAlive) return;
                        if (tick != initTick) *movedFromInit = true;
                        const QString formatted = formatScrubbed(
                            (tick == initTick && !*movedFromInit) ? initVal : tickToVal(tick), 2);
                        valueLabel->setText(formatted);
                        scalarEdit->setText(formatted);
                        if (!slider->isSliderDown()) commitTick(tick);
                    });
                    connect(slider, &QSlider::sliderReleased, this, [this, rowAlive, slider, commitTick]() {
                        if (!rowAlive || !*rowAlive) return;
                        m_scrubbing = false;
                        commitTick(slider->value());
                    });
                }

                const QPointer<QLineEdit> guardedScalarEdit(scalarEdit);
                auto* handle = new ScrubHandle(
                    scalarEdit, slotParam, 2 /* Double */,
                    [rowAlive, slider, valueLabel, sMin, sMax, step, ticks, hasValidRange](const QString&, const QString& v) {
                        if (!rowAlive || !*rowAlive) return;
                        if (!hasValidRange || !slider || !valueLabel || step <= 0.0) return;
                        bool ok = false;
                        const double dv = v.toDouble(&ok);
                        if (!ok || !std::isfinite(dv)) return;
                        valueLabel->setText(v);
                        const double clamped = std::min(std::max(dv, sMin), sMax);
                        const int tick = std::min(std::max(
                            static_cast<int>(std::llround((clamped - sMin) / step)), 0), ticks);
                        const QSignalBlocker blocker(slider);
                        slider->setValue(tick);
                    },
                    [this, rowAlive]() {
                        if (!rowAlive || !*rowAlive) return;
                        beginScrubBracket();
                    },
                    [this, rowAlive, guardedScalarEdit, slotParam, lastCommittedScalar]() {
                        if (!rowAlive || !*rowAlive) return;
                        endScrubBracket();
                        const QString val = guardedScalarEdit ? guardedScalarEdit->text().trimmed() : QString();
                        if (!val.isEmpty() && val != *lastCommittedScalar) {
                            *lastCommittedScalar = val;
                            commitInlinePainterEdit(slotParam, val);
                        } else {
                            m_painterThumbCache.clear();
                            refresh();
                        }
                    },
                    rowAlive);
                scalarLayout->addWidget(handle);
                scalarLayout->addWidget(scalarEdit, 1);

                connect(scalarEdit, &QLineEdit::editingFinished, this,
                        [this, rowAlive, scalarEdit, slotParam, lastCommittedScalar]() {
                    if (!rowAlive || !*rowAlive || m_scrubbing) return;
                    const QString val = scalarEdit->text().trimmed();
                    if (val.isEmpty() || val == *lastCommittedScalar) return;
                    *lastCommittedScalar = val;
                    commitInlinePainterEdit(slotParam, val);
                });
                cellColLayout->addWidget(scalarWell);
            }
        }
    }

    if (!p.description.isEmpty()) {
        auto* desc = new QLabel(p.description);
        desc->setWordWrap(true);
        desc->setFont(Theme::mono(9));
        desc->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textGhost)));
        cellColLayout->addWidget(desc);
    }

    rowLayout->addWidget(cellCol, 1);
    into->addWidget(row);
}

void ViewportProperties::refresh()
{
    if (!m_bridge || !m_rowAlive || !*m_rowAlive) return;

    // doc 88 S4b: self-heal a swallowed drag release.  m_scrubbing is set
    // by a param-slider / ScrubHandle press and normally cleared on
    // release -- but a mouse grab stolen mid-drag (a modal dialog opening,
    // e.g. on render completion) can swallow the release event, leaving
    // the flag stuck true and this panel frozen (the gate below skips
    // every rebuild) until the next completed scrub gesture.  The flag
    // being set with NO mouse button held means the gesture is over,
    // however it ended, so it is safe to resume rebuilding.  A stuck
    // ScrubHandle's endPropertyScrub bracket is closed by its destructor
    // net when the rebuild below destroys it.
    if (m_scrubbing && QGuiApplication::mouseButtons() == Qt::NoButton)
        m_scrubbing = false;

    // While a ScrubHandle drag is in flight, do NOT rebuild the rows --
    // doing so would deleteLater() the handle whose mouseMoveEvent put
    // setProperty() -> imageUpdated -> here on the stack, killing the
    // mouse grab.  endPropertyScrub calls refresh() once the user lets
    // go to re-sync canonical values.
    if (m_scrubbing || m_contextMenuOpen) return;   // don't rebuild rows under an open menu / active scrub

    m_currentSelectionCat = m_bridge->selectionCategory();
    m_currentSelectionName = m_bridge->selectionName();

    const QString key = QStringLiteral("%1|%2")
        .arg(static_cast<int>(m_currentSelectionCat)).arg(m_currentSelectionName);
    const bool keyChanged = (key != m_lastEntityKey);
    if (keyChanged) {
        m_lastEntityKey = key;
        m_advancedExpanded = false;
    }

    // "Reveal in scene file": fetch on selection change, and ALSO
    // re-arm when a selection made mid-render becomes fetchable once
    // the render finishes (mirrors PropertiesPanel.swift's review-P2
    // re-arm fix: the gate below failed then, and nothing re-triggered
    // for the SAME still-selected entity).  The `!m_sourceLineKnown`
    // re-arm never fires per-frame on entities whose line IS known.
    // Gated on m_sceneEditable: calling the bridge while a render owns
    // the scene would wedge on the controller's commit mutex (same
    // caveat as every other scene-text bridge call).
    if (keyChanged || (!m_sourceLineKnown && m_sceneEditable)) {
        if (m_sceneEditable && !m_currentSelectionName.isEmpty()) {
            quint64 offset = 0;
            quint32 line = 0;
            m_sourceLineKnown = m_bridge->getEntitySourceLocation(
                m_currentSelectionCat, m_currentSelectionName, &offset, &line);
            m_sourceLine = m_sourceLineKnown ? line : 0;
        } else {
            m_sourceLineKnown = false;
            m_sourceLine = 0;
        }
    }

    rebuildEntityHeader();
    rebuildPropertyRows();
}

void ViewportProperties::setSceneEditable(bool editable)
{
    m_sceneEditable = editable;
}

void ViewportProperties::commitEdit(const QString& name, const QString& value)
{
    if (!m_bridge) return;
    // A2 contract (see SceneEditController::SetProperty doc comments):
    // `false` does not always mean "nothing happened", so refresh()
    // unconditionally rather than gating on the return value.
    m_bridge->setPropertyForCategory(m_currentSelectionCat, name, value);
    m_painterThumbCache.clear();
    refresh();
}

void ViewportProperties::commitInlinePainterEdit(const QString& slotParam, const QString& value)
{
    if (!m_bridge) return;
    m_bridge->setReferenceSlotPainterValue(m_currentSelectionCat, slotParam, value);
    m_painterThumbCache.clear();
    refresh();
}

void ViewportProperties::forkInlinePainter(const QString& slotParam)
{
    if (!m_bridge) return;
    m_bridge->forkReferenceSlotPainter(m_currentSelectionCat, slotParam);
    m_painterThumbCache.clear();
    refresh();
}

QPixmap ViewportProperties::fetchPainterThumbnail(const QString& painterName, int defIndex, int sizePt)
{
    if (!m_bridge || painterName.isEmpty()) return QPixmap();
    const int px = std::max(8, sizePt * 2);
    const QString key = QStringLiteral("%1|%2|%3x%3|%4")
        .arg(painterName).arg(defIndex).arg(px).arg(m_lastSceneRevision);
    const auto it = m_painterThumbCache.constFind(key);
    if (it != m_painterThumbCache.constEnd()) return it.value();

    QByteArray rgba;
    if (!m_bridge->painterPreview(painterName, defIndex, static_cast<unsigned int>(px),
                                   static_cast<unsigned int>(px), rgba)
        || rgba.size() != px * px * 4) {
        return QPixmap();
    }
    const QImage img(reinterpret_cast<const uchar*>(rgba.constData()), px, px, px * 4, QImage::Format_RGBA8888);
    const QPixmap pix = QPixmap::fromImage(img.copy());
    m_painterThumbCache.insert(key, pix);
    return pix;
}

QPixmap ViewportProperties::fetchRampStripThumbnail(const QString& painterName, int widthPt, int heightPt)
{
    if (!m_bridge || painterName.isEmpty()) return QPixmap();
    const int pxW = std::max(8, widthPt * 2);
    const int pxH = std::max(1, heightPt * 2);
    const QString key = QStringLiteral("%1|-2|%2x%3|%4")
        .arg(painterName).arg(pxW).arg(pxH).arg(m_lastSceneRevision);
    const auto it = m_painterThumbCache.constFind(key);
    if (it != m_painterThumbCache.constEnd()) return it.value();

    QByteArray rgba;
    if (!m_bridge->rampStripPreview(painterName, static_cast<unsigned int>(pxW),
                                     static_cast<unsigned int>(pxH), rgba)
        || rgba.size() != pxW * pxH * 4) {
        return QPixmap();
    }
    const QImage img(reinterpret_cast<const uchar*>(rgba.constData()), pxW, pxH, pxW * 4, QImage::Format_RGBA8888);
    const QPixmap pix = QPixmap::fromImage(img.copy());
    m_painterThumbCache.insert(key, pix);
    return pix;
}

QPixmap ViewportProperties::fetchMaterialThumbnail(const QString& materialName, int sizePt)
{
    if (!m_bridge || materialName.isEmpty()) return QPixmap();
    const int px = std::max(8, sizePt * 2);
    const QString key = QStringLiteral("%1|-3|%2x%2|%3")
        .arg(materialName).arg(px).arg(m_lastSceneRevision);
    const auto it = m_painterThumbCache.constFind(key);
    if (it != m_painterThumbCache.constEnd()) return it.value();

    QByteArray rgba;
    if (!m_bridge->materialPreview(materialName, static_cast<unsigned int>(px),
                                    static_cast<unsigned int>(px), rgba)
        || rgba.size() != px * px * 4) {
        return QPixmap();
    }
    const QImage img(reinterpret_cast<const uchar*>(rgba.constData()), px, px, px * 4, QImage::Format_RGBA8888);
    const QPixmap pix = QPixmap::fromImage(img.copy());
    m_painterThumbCache.insert(key, pix);
    return pix;
}

QLabel* ViewportProperties::createPainterSwatchLabel(const QString& painterName, int defIndex, int sizePt, QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedSize(sizePt, sizePt);
    lbl->setStyleSheet(QStringLiteral(
        "background-color: %1; border: 1px solid %2; border-radius: 4px;")
        .arg(Theme::rgba(Theme::textDim, 0.08), Theme::hex(Theme::borderHairline)));
    const QPixmap px = fetchPainterThumbnail(painterName, defIndex, sizePt);
    if (!px.isNull()) {
        lbl->setPixmap(px.scaled(sizePt, sizePt, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    }
    return lbl;
}

QLabel* ViewportProperties::createMaterialSwatchLabel(const QString& materialName, int sizePt, QWidget* parent)
{
    auto* lbl = new QLabel(parent);
    lbl->setFixedSize(sizePt, sizePt);
    lbl->setStyleSheet(QStringLiteral(
        "background-color: %1; border: 1px solid %2; border-radius: 4px;")
        .arg(Theme::rgba(Theme::textDim, 0.08), Theme::hex(Theme::borderHairline)));
    const QPixmap px = fetchMaterialThumbnail(materialName, sizePt);
    if (!px.isNull()) {
        lbl->setPixmap(px.scaled(sizePt, sizePt, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    }
    return lbl;
}

void ViewportProperties::onLineEditFinished()
{
    if (!m_rowAlive || !*m_rowAlive) return;
    auto* edit = qobject_cast<QLineEdit*>(sender());
    if (!edit || !m_bridge) return;
    const QString name = edit->objectName();
    if (m_fields.value(name) != edit) return;
    const QString val  = edit->text();
    if (val == m_lastValue.value(name)) return;
    commitEdit(name, val);
}

void ViewportProperties::onUseInViewportClicked()
{
    if (!m_bridge || m_currentSelectionName.isEmpty()) return;
    m_bridge->setSelection(Category::Camera, m_currentSelectionName);
    refresh();
}

void ViewportProperties::onAddCameraClicked()
{
    if (!m_bridge) return;

    // Default proposed name = "<active>_copy"
    const QString activeName = m_bridge->activeNameForCategory(Category::Camera);
    const QString defaultProposal = activeName.isEmpty()
        ? QStringLiteral("camera_copy")
        : (activeName + QStringLiteral("_copy"));

    bool ok = false;
    QString proposed = QInputDialog::getText(
        this,
        tr("Add Camera"),
        tr("Cloning the current camera.  Pick a name for the new camera.\n\n"
           "\xE2\x80\xA2 The clone is in-memory only \xE2\x80\x94 saving the .RISEscene file\n"
           "  from the editor does not yet emit added cameras.\n"
           "\xE2\x80\xA2 Duplicate names get a numeric suffix appended."),
        QLineEdit::Normal,
        defaultProposal,
        &ok);
    if (!ok) return;
    proposed = proposed.trimmed();

    const QString chosenName = m_bridge->addCameraFromActive(proposed);
    if (chosenName.isEmpty()) {
        QMessageBox::warning(
            this,
            tr("Couldn't add camera"),
            tr("The current camera could not be cloned.  See RISE_Log.txt for details."));
        return;
    }

    m_bridge->setSelection(Category::Camera, chosenName);
    refresh();

    if (!m_addCameraCaveatShown) {
        m_addCameraCaveatShown = true;
        QMessageBox::information(
            this,
            tr("New camera \"%1\" added").arg(chosenName),
            tr("Heads up \xE2\x80\x94 added cameras are kept in memory only until the\n"
               "scene-text round-trip lands.  Save your scene file from a\n"
               "text editor to preserve them across reloads."));
    }
}
