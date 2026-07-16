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
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPointer>
#include <QPolygonF>
#include <QToolButton>
#include <QMenu>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QSet>
#include <QStringList>

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
    if (kind == 1 /* UInt */) {
        const long long n = std::max<long long>(
            0, static_cast<long long>(std::llround(v)));
        return QString::number(n);
    }
    return QString::asprintf("%.6g", v);
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
                QWidget* parent = nullptr)
        : QLabel(parent)
        , m_target(target)
        , m_name(std::move(name))
        , m_kind(kind)
        , m_commit(std::move(commit))
        , m_beginBracket(std::move(beginBracket))
        , m_endBracket(std::move(endBracket))
    {
        // Glyph is painted in paintEvent -- we draw two filled triangles
        // rather than rely on a Unicode arrow glyph being present in the
        // system font.
        setFixedSize(16, 18);
        setCursor(Qt::SizeVerCursor);
        setToolTip(QObject::tr(
            "Drag up/down to change.  Shift = fine, Alt = coarse."));
    }

    ~ScrubHandle() override
    {
        if (m_dragging && m_endBracket) m_endBracket();
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !m_target) {
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
        if (!m_dragging) {
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
            if (m_endBracket) m_endBracket();
            e->accept();
        } else {
            QLabel::mouseReleaseEvent(e);
        }
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::textDim);

        const qreal w  = width();
        const qreal h  = height();
        const qreal pad = 2.0;
        const qreal triH = (h - 2 * pad - 1.0) * 0.5;   // 1 px gap between

        QPolygonF up;
        up << QPointF(pad,          pad + triH)
           << QPointF(w - pad,      pad + triH)
           << QPointF(w * 0.5,      pad);
        p.drawPolygon(up);

        QPolygonF down;
        down << QPointF(pad,        h - pad - triH)
             << QPointF(w - pad,    h - pad - triH)
             << QPointF(w * 0.5,    h - pad);
        p.drawPolygon(down);
    }

private:
    QPointer<QLineEdit> m_target;
    QString    m_name;
    int        m_kind     = 0;
    CommitFn   m_commit;
    BracketFn  m_beginBracket;
    BracketFn  m_endBracket;
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
    case Category::None:
    default:                     return tr("Scene");
    }
}

QString ViewportProperties::categoryGlyph(Category cat)
{
    switch (cat) {
    case Category::Camera:       return QString::fromUtf8("\xE2\x97\x89");   // ◉
    case Category::Object:       return QString::fromUtf8("\xE2\x97\x86");   // ◆
    case Category::Light:        return QString::fromUtf8("\xE2\x98\x80");   // ☀
    case Category::Material:     return QString::fromUtf8("\xE2\x97\x90");   // ◐
    case Category::Rasterizer:   return QString::fromUtf8("\xE2\x9A\x99");   // ⚙
    case Category::Film:         return QString::fromUtf8("\xE2\x96\xA6");   // ▦
    case Category::Medium:       return QString::fromUtf8("\xE2\x89\x88");   // ≈
    case Category::Animation:    return QString::fromUtf8("\xE2\x96\xB6");   // ▶
    case Category::SceneVariant: return QString::fromUtf8("\xE2\xA7\x89");   // ⧉
    case Category::Painter:      return QString::fromUtf8("\xE2\x96\xA7");   // ▧
    case Category::None:
    default:                     return QString::fromUtf8("\xE2\x80\xA2");   // •
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
    m_iconChip->setStyleSheet(QStringLiteral(
        "background-color: %1; border: 1px solid %2; border-radius: 6px; color: %3;")
        .arg(Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.15 * 255))),
             Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.3 * 255))),
             Theme::hex(Theme::accentLight)));
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
    m_nameLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    titleRowLayout->addWidget(m_nameLabel);

    m_sourceLineChip = new QToolButton(titleRow);
    m_sourceLineChip->setFont(Theme::mono(10));
    m_sourceLineChip->setCursor(Qt::PointingHandCursor);
    m_sourceLineChip->setToolTip(tr("Reveal in scene file"));
    m_sourceLineChip->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 2px 6px; }"
        "QToolButton:hover { color: %4; border-color: %5; }")
        .arg(Theme::hex(Theme::textDim), Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusSmall)
        .arg(Theme::hex(Theme::textSecondary), Theme::hex(Theme::borderStrong)));
    m_sourceLineChip->hide();
    connect(m_sourceLineChip, &QToolButton::clicked, this, [this]() {
        emit revealRequested(m_currentSelectionCat, m_currentSelectionName);
    });
    titleRowLayout->addWidget(m_sourceLineChip);
    titleRowLayout->addStretch(1);
    nameColLayout->addWidget(titleRow);

    m_metaLabel = new QLabel(nameCol);
    m_metaLabel->setFont(Theme::mono(10));
    m_metaLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
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
    m_useInViewportBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 7px 0; }"
        "QToolButton:disabled { color: %4; border-color: %5; }")
        .arg(Theme::hex(Theme::accentLight),
             Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.35 * 255))))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
    connect(m_useInViewportBtn, &QToolButton::clicked, this, &ViewportProperties::onUseInViewportClicked);
    camLayout->addWidget(m_useInViewportBtn);

    m_addCameraBtn = new QToolButton(m_cameraAffordances);
    m_addCameraBtn->setText(QStringLiteral("+ ") + tr("Add Camera"));
    m_addCameraBtn->setFont(Theme::mono(10));
    m_addCameraBtn->setCursor(Qt::PointingHandCursor);
    m_addCameraBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: none; padding: 2px 0; }")
        .arg(Theme::hex(Theme::textDim)));
    connect(m_addCameraBtn, &QToolButton::clicked, this, &ViewportProperties::onAddCameraClicked);
    camLayout->addWidget(m_addCameraBtn);

    m_bodyLayout->addWidget(m_cameraAffordances);

    // ---- Empty-selection message ----------------------------------------
    m_emptyMessage = new QLabel(body);
    m_emptyMessage->setFont(Theme::sans(11));
    m_emptyMessage->setWordWrap(true);
    m_emptyMessage->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
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
    auto* advLabel = new QLabel(tr("Advanced"), m_advancedToggleRow);
    advLabel->setFont(Theme::sans(11));
    advLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    advToggleLayout->addWidget(advLabel);
    m_advancedArrowLabel = new QLabel(m_advancedToggleRow);
    m_advancedArrowLabel->setFont(Theme::sans(9));
    m_advancedArrowLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    advToggleLayout->addWidget(m_advancedArrowLabel);
    advToggleLayout->addStretch(1);
    m_advancedCountLabel = new QLabel(m_advancedToggleRow);
    m_advancedCountLabel->setFont(Theme::mono(9));
    m_advancedCountLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDisabled)));
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
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderHairline)));
    root->addWidget(sep);

    auto* footer = new QWidget(this);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(14, 8, 14, 8);
    footerLayout->setSpacing(8);

    m_saveButton = new QToolButton(footer);
    m_saveButton->setText(tr("Save"));
    m_saveButton->setFont(Theme::sans(11, QFont::DemiBold));
    m_saveButton->setEnabled(false);
    m_saveButton->setToolTip(tr("No changes to save"));
    m_saveButton->setStyleSheet(QStringLiteral(
        "QToolButton { color: #0d1116; background-color: %1; border: none; border-radius: %2px; padding: 5px 11px; }"
        "QToolButton:disabled { color: %3; background-color: %4; }")
        .arg(Theme::hex(Theme::textPrimary))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::textDisabled), Theme::rgba(Theme::whiteAlpha(int(0.08 * 255)))));
    connect(m_saveButton, &QToolButton::clicked,
            this, [this]() { performSceneSave(/*useLoadedPath=*/true); });
    footerLayout->addWidget(m_saveButton);

    m_saveAsButton = new QToolButton(footer);
    m_saveAsButton->setText(tr("Save As\xE2\x80\xA6"));
    m_saveAsButton->setFont(Theme::sans(11));
    m_saveAsButton->setEnabled(false);
    m_saveAsButton->setToolTip(tr("No changes to save"));
    m_saveAsButton->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: %3px; padding: 5px 11px; }"
        "QToolButton:disabled { color: %4; border-color: %5; }")
        .arg(Theme::hex(Theme::textTertiary), Theme::hex(Theme::borderStrong))
        .arg(Theme::radiusMedium)
        .arg(Theme::hex(Theme::textDisabled), Theme::hex(Theme::borderLight)));
    connect(m_saveAsButton, &QToolButton::clicked,
            this, [this]() { performSceneSave(/*useLoadedPath=*/false); });
    footerLayout->addWidget(m_saveAsButton);

    footerLayout->addStretch(1);

    auto* refreshBtn = new QToolButton(footer);
    refreshBtn->setText(QString::fromUtf8("\xE2\x86\xBB"));   // ↻
    refreshBtn->setToolTip(tr("Refresh from the live scene"));
    refreshBtn->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; }")
        .arg(Theme::hex(Theme::textDim)));
    connect(refreshBtn, &QToolButton::clicked, this, &ViewportProperties::refresh);
    footerLayout->addWidget(refreshBtn);

    root->addWidget(footer);

    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        setPalette(pal);
    }

    // The dirtyChanged signal is emitted via QueuedConnection from
    // the bridge's C-trampoline (see ViewportBridge ctor), so a
    // direct Qt::AutoConnection here delivers on the GUI thread.
    if (m_bridge) {
        connect(m_bridge, &ViewportBridge::dirtyChanged,
                this, &ViewportProperties::onDirtyChanged);
    }

    setMinimumWidth(260);

    // Initial pull so the panel renders immediately.
    refresh();
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
        emit sceneSavedToPath(target, wasSaveAs);
        break;
    case ViewportBridge::SaveStatus::NoOp:
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
    m_iconChip->setText(categoryGlyph(m_currentSelectionCat));

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
    m_advancedArrowLabel->setText(m_advancedExpanded
        ? QString::fromUtf8("\xE2\x96\xBE")    // ▾
        : QString::fromUtf8("\xE2\x96\xB8"));  // ▸
    m_advancedCountLabel->setText(tr("%1 more").arg(advancedCount));
}

void ViewportProperties::onAdvancedToggleClicked()
{
    m_advancedExpanded = !m_advancedExpanded;
    m_advancedContainer->setVisible(m_advancedExpanded);
    updateAdvancedToggleLabel(m_advancedLayout ? m_advancedLayout->count() : 0);
}

void ViewportProperties::rebuildPropertyRows()
{
    if (!m_bridge) return;
    clearPropertyRows();

    // Force a refresh so the controller's per-category snapshot for the
    // PRIMARY selection is up to date before we read it.
    (void)m_bridge->propertySnapshot();

    const QVector<ViewportProperty> props = m_bridge->propertySnapshotFor(m_currentSelectionCat);

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
    connect(row, &QWidget::customContextMenuRequested, this,
            [this, row, paramName](const QPoint& pos) {
                if (!m_sourceLineKnown) return;
                QMenu menu(this);
                QAction* reveal = menu.addAction(
                    tr("Reveal \xE2\x80\x9C%1\xE2\x80\x9D in Scene File").arg(paramName));   // “<param>”
                connect(reveal, &QAction::triggered, this, [this, paramName]() {
                    emit revealParamRequested(m_currentSelectionCat, m_currentSelectionName, paramName);
                });
                menu.exec(row->mapToGlobal(pos));
            });

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

    if (!p.editable) {
        // Read-only well.
        auto* wellWidget = new QWidget;
        wellWidget->setStyleSheet(wellStyleSheet());
        auto* wellLayout = new QHBoxLayout(wellWidget);
        wellLayout->setContentsMargins(8, 5, 8, 5);
        wellLayout->setSpacing(6);

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
            connect(edit, &QLineEdit::editingFinished, this, [this, edits, name]() {
                QStringList parts;
                for (QLineEdit* e : *edits) parts << e->text();
                commitEdit(name, parts.join(QLatin1Char(' ')));
            });
        }
        cellColLayout->addWidget(vecRow);
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

        if (isScrubbableKind(p.kind)) {
            auto* handle = new ScrubHandle(
                edit, p.name, p.kind,
                [this](const QString& n, const QString& v) {
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
                    if (!m_bridge) return;
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
                [this]() {
                    m_scrubbing = true;
                    if (m_bridge) m_bridge->beginPropertyScrub();
                },
                [this]() {
                    m_scrubbing = false;
                    if (m_bridge) m_bridge->endPropertyScrub();
                    refresh();
                });
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
            browseBtn->setText(QString::fromUtf8("\xE2\x80\xA6"));   // …
            browseBtn->setToolTip(tr("Browse\xE2\x80\xA6"));
            browseBtn->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; }")
                .arg(Theme::hex(Theme::textDim)));
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
            presetButton->setText(QStringLiteral("\xE2\x8B\xAE"));   // ⋮
            presetButton->setToolTip(tr("Quick-pick presets"));
            presetButton->setPopupMode(QToolButton::InstantPopup);
            presetButton->setStyleSheet(QStringLiteral("QToolButton { color: %1; border: none; }")
                .arg(Theme::hex(Theme::textDim)));
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
    if (!m_bridge) return;
    // While a ScrubHandle drag is in flight, do NOT rebuild the rows --
    // doing so would deleteLater() the handle whose mouseMoveEvent put
    // setProperty() -> imageUpdated -> here on the stack, killing the
    // mouse grab.  endPropertyScrub calls refresh() once the user lets
    // go to re-sync canonical values.
    if (m_scrubbing) return;

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
    refresh();
}

void ViewportProperties::onLineEditFinished()
{
    auto* edit = qobject_cast<QLineEdit*>(sender());
    if (!edit || !m_bridge) return;
    const QString name = edit->objectName();
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
