//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportProperties.h"
#include "ViewportBridge.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QScrollArea>
#include <QFrame>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QMenu>

#include <cmath>
#include <functional>
#include <utility>

namespace {

// Single-numeric ValueKind values (Bool=0, UInt=1, Double=2,
// DoubleVec3=3, ...).  Vector / string / reference kinds get no
// scrub handle — scrubbing a vector is ambiguous and keyboard
// entry is the natural input for non-numeric fields.
inline bool isScrubbableKind(int kind)
{
    return kind == 1 /* UInt */ || kind == 2 /* Double */;
}

// Angular fields the camera descriptor surfaces.  These get a fixed
// 0.5°/px rate (matching the Orbit tool's sensitivity); other
// numeric fields use the proportional rate.  Without this split, a
// theta=30° row scrubs at 0.15°/px (100px = 15°) which is sluggish
// for typical orbit work.  `aperture_rotation` is a thinlens_camera
// angular field (polygon rotation in degrees) and follows the same
// convention.
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

// Per-pixel scrub rate.  Angular = 0.5/px (fixed).  Otherwise
// proportional to magnitude with a 1e-3 floor so values at zero
// can still scrub off zero.
inline double scrubRate(const QString& name, double value)
{
    if (isAngularField(name)) return 0.5;
    return std::max(std::abs(value), 1e-3) * 0.005;
}

// Format a scrubbed value back into the same textual form the C++
// FormatDouble / FormatUInt helpers produce (`%.6g` for doubles,
// integer for UInt clamped at zero).  Keeping the formatting in
// lockstep avoids round-trip drift across many small drags.
inline QString formatScrubbed(double v, int kind)
{
    if (kind == 1 /* UInt */) {
        const long long n = std::max<long long>(
            0, static_cast<long long>(std::llround(v)));
        return QString::number(n);
    }
    return QString::asprintf("%.6g", v);
}

// Click-and-drag chevron handle for scrubbing numeric property
// values.  Drag up = increase, drag down = decrease.  Rate scales
// with the current value's magnitude so a 50° FOV and a 0.08
// aperture both feel natural under the same gesture.
//
// Modifiers held while dragging:
//   • Shift — 0.25× rate (fine control)
//   • Alt   — 4×    rate (coarse control)
//
// No MOC required: the widget overrides only the mouse-event
// virtuals it inherits from QLabel and emits no Qt signals.  The
// commit callback is a captured std::function, invoked on every
// drag tick to drive live preview through the same setProperty
// path that keyboard editing uses.
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
        , m_target(target)        // QPointer auto-nulls on QObject destruction
        , m_name(std::move(name))
        , m_kind(kind)
        , m_commit(std::move(commit))
        , m_beginBracket(std::move(beginBracket))
        , m_endBracket(std::move(endBracket))
    {
        // Unicode up-down arrow — chevron is the same affordance
        // macOS uses for scrubbable numerics.  Fixed size keeps the
        // row alignment stable across different label widths.
        setText(QStringLiteral("↕"));
        setFixedSize(16, 18);
        setAlignment(Qt::AlignCenter);
        setCursor(Qt::SizeVerCursor);
        setStyleSheet(
            "color: palette(placeholder-text); "
            "font-size: 11px; font-weight: bold;");
        setToolTip(QObject::tr(
            "Drag up/down to change.  Shift = fine, Alt = coarse."));
    }

    ~ScrubHandle() override
    {
        // If we're destroyed mid-drag (e.g. a panel-mode flip in
        // ViewportProperties::clearRows() reaches the deleteLater
        // before the user releases the mouse), fire the End bracket
        // so the controller's mScrubInProgress flag clears.  The
        // render-thread watchdog (kScrubWatchdogMs) recovers if
        // this destructor never runs, but firing here is the
        // synchronous, predictable path.
        if (m_dragging && m_endBracket) m_endBracket();
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) {
            QLabel::mousePressEvent(e);
            return;
        }
        if (!m_target) {
            // Target line edit was destroyed (parent panel torn
            // down between rows being built and the user clicking).
            // Decline the gesture — pressing without a target
            // would dereference a null QPointer in mouseMoveEvent.
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
            // QPointer detected the QLineEdit was destroyed under
            // us mid-drag.  Bail cleanly: end the scrub bracket so
            // the controller doesn't keep mScrubInProgress=true,
            // and stop processing drag deltas (writing to a freed
            // QLineEdit would be a UAF — exactly the bug the
            // QPointer guards against).
            m_dragging = false;
            if (m_endBracket) m_endBracket();
            return;
        }
        // Drag up (smaller global Y) increases the value.
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

private:
    // QPointer auto-nulls when the target QLineEdit is destroyed,
    // so mouseMoveEvent's read sees `m_target == nullptr` instead
    // of dereferencing a freed line edit.  Without this, a panel
    // mode flip mid-drag (e.g. user picks a different camera tool
    // while still holding the chevron) would deleteLater the
    // QLineEdit while pending mouse events are still being routed
    // to this handle — the next move event would UAF on
    // m_target->text().
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

}  // namespace

ViewportProperties::ViewportProperties(ViewportBridge* bridge, QWidget* parent)
    : QWidget(parent)
    , m_bridge(bridge)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Header is set dynamically by refresh() based on the bridge's
    // current panel mode ("Camera", "Object: <name>", or empty).  We
    // always render the bar so the layout doesn't jump.
    m_headerLabel = new QLabel(QString(), this);
    m_headerLabel->setStyleSheet("padding: 4px 8px; font-weight: bold;");
    root->addWidget(m_headerLabel);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);

    auto* listHolder = new QWidget(m_scroll);
    m_listLayout = new QVBoxLayout(listHolder);
    m_listLayout->setContentsMargins(8, 8, 8, 8);
    m_listLayout->setSpacing(8);

    // Empty-mode placeholder.  Hidden when there are real rows.
    m_emptyMessage = new QLabel(
        tr("Pick an object or select a camera tool to inspect properties."),
        listHolder);
    m_emptyMessage->setWordWrap(true);
    m_emptyMessage->setStyleSheet("color: palette(placeholder-text); padding: 4px;");
    m_listLayout->addWidget(m_emptyMessage);

    m_listLayout->addStretch(1);

    m_scroll->setWidget(listHolder);
    root->addWidget(m_scroll, 1);

    setMinimumWidth(260);
}

void ViewportProperties::clearRows()
{
    // Tear down all dynamically-added rows; leave the empty-message
    // placeholder and the trailing stretch in place.  Iterate by
    // index from the end to avoid shifting.
    for (int i = m_listLayout->count() - 1; i >= 0; --i) {
        QLayoutItem* item = m_listLayout->itemAt(i);
        if (!item) continue;
        QWidget* w = item->widget();
        if (!w) continue;                 // stretch / spacers stay
        if (w == m_emptyMessage) continue; // keep placeholder

        m_listLayout->removeWidget(w);
        w->deleteLater();
    }
    m_fields.clear();
    m_readOnly.clear();
    m_lastValue.clear();
}

void ViewportProperties::refresh()
{
    if (!m_bridge) return;

    const ViewportBridge::PanelMode mode = m_bridge->panelMode();
    const QString header = m_bridge->panelHeader();

    // If the panel mode flipped (None ↔ Camera ↔ Object), tear down
    // all rows from the previous entity — the property set is
    // entity-specific and shouldn't bleed across.
    if (mode != m_currentMode) {
        clearRows();
        m_currentMode = mode;
    }

    m_headerLabel->setText(header);

    if (mode == ViewportBridge::PanelMode::None) {
        // Empty panel — show the placeholder, no rows.
        m_emptyMessage->setVisible(true);
        return;
    }
    m_emptyMessage->setVisible(false);

    const QVector<ViewportProperty> props = m_bridge->propertySnapshot();

    // Build any rows that don't exist yet; update existing rows in place.
    for (const ViewportProperty& p : props) {
        if (p.editable) {
            QLineEdit* edit = m_fields.value(p.name, nullptr);
            if (!edit) {
                // Create a new row
                auto* container = new QWidget;
                auto* col = new QVBoxLayout(container);
                col->setContentsMargins(0, 0, 0, 0);
                col->setSpacing(2);

                auto* label = new QLabel(p.name);
                QFont f = label->font();
                f.setBold(true);
                label->setFont(f);
                label->setStyleSheet("color: palette(window-text);");

                edit = new QLineEdit;
                edit->setObjectName(p.name);   // stash name for the slot
                edit->setText(p.value);
                connect(edit, &QLineEdit::editingFinished,
                        this, &ViewportProperties::onLineEditFinished);

                col->addWidget(label);

                // For single-numeric kinds (Double, UInt) wrap the
                // line edit in an HBox with a leading scrub handle.
                // The chevron is the click-and-drag affordance:
                // drag up to increase, down to decrease.  Other
                // kinds (vectors, strings, references) get the line
                // edit on its own — scrubbing a vector is
                // ambiguous and keyboard entry is the natural input
                // for non-numeric fields.
                // Field row layout: scrub handle (if numeric) +
                // line edit + unit suffix (if any) + presets menu
                // (if descriptor has any).  Quick-pick presets stay
                // alongside the line edit so users can type a custom
                // value or pick a preset; the unit label disambiguates
                // small numbers like "35" as "35 mm" so the user can
                // tell at a glance what unit the field is in.
                if (isScrubbableKind(p.kind) || !p.presets.isEmpty() || !p.unitLabel.isEmpty()) {
                    auto* fieldRow = new QHBoxLayout;
                    fieldRow->setContentsMargins(0, 0, 0, 0);
                    fieldRow->setSpacing(4);
                    if (isScrubbableKind(p.kind)) {
                        auto* handle = new ScrubHandle(
                            edit, p.name, p.kind,
                            [this](const QString& n, const QString& v) {
                                if (!m_bridge) return;
                                if (m_bridge->setProperty(n, v)) {
                                    m_lastValue.insert(n, v);
                                }
                            },
                            [this]() { if (m_bridge) m_bridge->beginPropertyScrub(); },
                            [this]() { if (m_bridge) m_bridge->endPropertyScrub();   });
                        fieldRow->addWidget(handle);
                    }
                    fieldRow->addWidget(edit, 1);
                    if (!p.unitLabel.isEmpty()) {
                        auto* unit = new QLabel(p.unitLabel);
                        QFont uf = unit->font();
                        uf.setPointSizeF(uf.pointSizeF() * 0.85);
                        unit->setFont(uf);
                        unit->setStyleSheet("color: palette(placeholder-text);");
                        fieldRow->addWidget(unit);
                    }
                    if (!p.presets.isEmpty()) {
                        auto* presetButton = new QToolButton;
                        presetButton->setText(QStringLiteral("⋮"));   // vertical ellipsis
                        presetButton->setToolTip(tr("Quick-pick presets"));
                        presetButton->setPopupMode(QToolButton::InstantPopup);
                        auto* menu = new QMenu(presetButton);
                        const QString propName = p.name;
                        for (const ViewportPropertyPreset& preset : p.presets) {
                            const QString label = preset.label;
                            const QString value = preset.value;
                            QAction* action = menu->addAction(label);
                            connect(action, &QAction::triggered, this,
                                    [this, propName, value]() {
                                        if (!m_bridge) return;
                                        if (m_bridge->setProperty(propName, value)) {
                                            m_lastValue.insert(propName, value);
                                        }
                                    });
                        }
                        presetButton->setMenu(menu);
                        fieldRow->addWidget(presetButton);
                    }
                    col->addLayout(fieldRow);
                } else {
                    col->addWidget(edit);
                }
                if (!p.description.isEmpty()) {
                    auto* desc = new QLabel(p.description);
                    desc->setWordWrap(true);
                    QFont df = desc->font();
                    df.setPointSizeF(df.pointSizeF() * 0.85);
                    desc->setFont(df);
                    desc->setStyleSheet("color: palette(placeholder-text);");
                    col->addWidget(desc);
                }
                m_listLayout->insertWidget(m_listLayout->count() - 1, container);
                m_fields.insert(p.name, edit);
                m_lastValue.insert(p.name, p.value);
            } else {
                // Update only when the field isn't being actively edited.
                if (!edit->hasFocus() && edit->text() != p.value) {
                    edit->setText(p.value);
                }
                m_lastValue.insert(p.name, p.value);
            }
        } else {
            QLabel* lbl = m_readOnly.value(p.name, nullptr);
            if (!lbl) {
                auto* container = new QWidget;
                auto* col = new QVBoxLayout(container);
                col->setContentsMargins(0, 0, 0, 0);
                col->setSpacing(2);

                auto* hdr = new QHBoxLayout;
                hdr->setContentsMargins(0, 0, 0, 0);
                auto* label = new QLabel(p.name);
                QFont f = label->font();
                f.setBold(true);
                label->setFont(f);
                hdr->addWidget(label);
                auto* badge = new QLabel(tr("read-only"));
                QFont bf = badge->font();
                bf.setPointSizeF(bf.pointSizeF() * 0.75);
                badge->setFont(bf);
                badge->setStyleSheet("color: palette(placeholder-text);");
                hdr->addStretch(1);
                hdr->addWidget(badge);
                col->addLayout(hdr);

                lbl = new QLabel(p.value);
                lbl->setStyleSheet("color: palette(placeholder-text); font-family: monospace;");
                col->addWidget(lbl);

                if (!p.description.isEmpty()) {
                    auto* desc = new QLabel(p.description);
                    desc->setWordWrap(true);
                    QFont df = desc->font();
                    df.setPointSizeF(df.pointSizeF() * 0.85);
                    desc->setFont(df);
                    desc->setStyleSheet("color: palette(placeholder-text);");
                    col->addWidget(desc);
                }

                m_listLayout->insertWidget(m_listLayout->count() - 1, container);
                m_readOnly.insert(p.name, lbl);
            } else {
                lbl->setText(p.value);
            }
        }
    }
}

void ViewportProperties::onLineEditFinished()
{
    auto* edit = qobject_cast<QLineEdit*>(sender());
    if (!edit || !m_bridge) return;
    const QString name = edit->objectName();
    const QString val  = edit->text();
    if (val == m_lastValue.value(name)) return;   // no change
    if (m_bridge->setProperty(name, val)) {
        m_lastValue.insert(name, val);
        // Re-snapshot so the field reflects the canonicalized value.
        refresh();
    }
}
