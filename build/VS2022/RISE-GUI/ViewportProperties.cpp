//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.cpp - Right-side accordion implementation.
//    Four sections, single selection, descriptor-driven property
//    rows under the expanded section.  See header for layout and
//    macOS / Android counterparts.
//
//////////////////////////////////////////////////////////////////////

#include "ViewportProperties.h"
#include "ViewportBridge.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLabel>
#include <QComboBox>
#include <QScrollArea>
#include <QFrame>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QMenu>

#include <cmath>
#include <functional>
#include <utility>

using SectionWidgets = ViewportPropertiesInternal::AccordionSectionWidgets;

namespace {

// Single-numeric ValueKind values (Bool=0, UInt=1, Double=2,
// DoubleVec3=3, ...).  Vector / string / reference kinds get no
// scrub handle — scrubbing a vector is ambiguous and keyboard
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

struct AccordionSectionDef {
    ViewportBridge::Category category;
    const char*              title;
};

// Top-down order: Cameras first (most-used), Rasterizer second
// (scene-global), Objects third (long lists), Lights last.
static const AccordionSectionDef kSectionDefs[] = {
    { ViewportBridge::Category::Camera,     "Cameras"    },
    { ViewportBridge::Category::Rasterizer, "Rasterizer" },
    { ViewportBridge::Category::Object,     "Objects"    },
    { ViewportBridge::Category::Light,      "Lights"     },
};

}  // namespace

ViewportProperties::ViewportProperties(ViewportBridge* bridge, QWidget* parent)
    : QWidget(parent)
    , m_bridge(bridge)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_headerLabel = new QLabel(tr("Scene"), this);
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
    m_listLayout->setSpacing(4);

    // Build each accordion section.  Each section is a
    // QToolButton header (toggleable arrow indicator) over a body
    // QFrame holding a list + a properties container.
    for (const auto& def : kSectionDefs) {
        SectionWidgets w;
        w.container = new QWidget(listHolder);
        auto* col = new QVBoxLayout(w.container);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(0);

        w.toggle = new QToolButton(w.container);
        w.toggle->setText(QString::fromUtf8(def.title));
        w.toggle->setCheckable(true);
        w.toggle->setChecked(false);
        w.toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        w.toggle->setArrowType(Qt::RightArrow);
        w.toggle->setStyleSheet(
            "QToolButton { font-weight: bold; padding: 4px; border: none; text-align: left; }"
            "QToolButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }");
        w.toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        col->addWidget(w.toggle);

        w.body = new QFrame(w.container);
        w.body->setVisible(false);
        auto* bodyLayout = new QVBoxLayout(w.body);
        bodyLayout->setContentsMargins(8, 4, 0, 4);
        bodyLayout->setSpacing(4);

        // Every section uses a dropdown combo so the panel surface is
        // visually consistent across categories.  Rasterizers list 8
        // standard types; Objects can run into the hundreds; Cameras
        // and Lights are usually a handful but pick the same widget
        // for parity.
        const ViewportBridge::Category cat = def.category;

        w.combo = new QComboBox(w.body);
        w.combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        w.combo->setStyleSheet(
            "QComboBox { padding: 2px 6px; border: 1px solid palette(mid); border-radius: 3px; }");
        bodyLayout->addWidget(w.combo);

        w.propsFrame = new QFrame(w.body);
        w.propsLayout = new QVBoxLayout(w.propsFrame);
        w.propsLayout->setContentsMargins(0, 4, 0, 0);
        w.propsLayout->setSpacing(6);
        bodyLayout->addWidget(w.propsFrame);

        col->addWidget(w.body);
        m_listLayout->addWidget(w.container);

        const int catInt = static_cast<int>(def.category);
        m_sections.insert(catInt, w);

        // Toggle: clicking the header expands/collapses the section.
        // Single-selection rule means expanding section X collapses
        // every other section, which we achieve by routing the
        // toggle through the bridge: setting an empty-name selection
        // for that category opens it and closes the others.  When
        // the user collapses the active section, we set Category::None.
        connect(w.toggle, &QToolButton::toggled, this,
                [this, cat](bool checked) {
                    if (!m_bridge) return;
                    if (checked) {
                        m_bridge->setSelection(cat, QString());
                    } else if (m_currentSelectionCat == cat) {
                        m_bridge->setSelection(ViewportBridge::Category::None, QString());
                    }
                    refresh();
                });

        // Combo activation: routes a (category, name) selection
        // through the bridge.  `activated` only fires on user choice
        // (not programmatic `setCurrentIndex`), so the re-entry guard
        // via m_currentSelectionName below is belt-and-suspenders for
        // pathological cases (e.g. a duplicate-named entity).
        QComboBox* comboPtr = w.combo;
        connect(comboPtr, &QComboBox::activated, this,
                [this, cat, comboPtr](int /*index*/) {
                    if (!m_bridge || !comboPtr) return;
                    const QString name = comboPtr->currentText();
                    if (name.isEmpty()) return;
                    if (m_currentSelectionCat == cat && m_currentSelectionName == name) return;
                    m_bridge->setSelection(cat, name);
                    refresh();
                });
    }

    m_listLayout->addStretch(1);

    m_scroll->setWidget(listHolder);
    root->addWidget(m_scroll, 1);

    setMinimumWidth(260);

    // Initial pull so the panel renders immediately.
    refresh();
}

void ViewportProperties::clearPropertyRows()
{
    // Tear down rows from every section's propsFrame.  Rows are
    // entity-specific and shouldn't bleed across selection changes.
    for (auto it = m_sections.begin(); it != m_sections.end(); ++it) {
        SectionWidgets& w = it.value();
        QLayoutItem* item;
        while ((item = w.propsLayout->takeAt(0)) != nullptr) {
            if (QWidget* widget = item->widget()) {
                widget->deleteLater();
            }
            delete item;
        }
    }
    m_fields.clear();
    m_readOnly.clear();
    m_lastValue.clear();
}

void ViewportProperties::rebuildEntityLists()
{
    if (!m_bridge) return;

    for (auto it = m_sections.begin(); it != m_sections.end(); ++it) {
        const Category cat = static_cast<Category>(it.key());
        SectionWidgets& w = it.value();
        if (!w.combo) continue;
        const QStringList names = m_bridge->categoryEntities(cat);

        // Block activate signals during repopulation.  Without the
        // block, `clear()` followed by `addItems()` could fire
        // change callbacks that round-trip a bogus setSelection and
        // stomp on the user's actual choice.
        const QSignalBlocker blocker(w.combo);
        w.combo->clear();
        for (const QString& n : names) {
            w.combo->addItem(n);
        }
    }
}

void ViewportProperties::syncAccordionFromSelection()
{
    if (!m_bridge) return;

    const Category cat = m_bridge->selectionCategory();
    const QString name = m_bridge->selectionName();
    m_currentSelectionCat = cat;
    m_currentSelectionName = name;

    // Single-selection / single-expansion rule: the section
    // matching `cat` is expanded; all others are collapsed.
    for (auto it = m_sections.begin(); it != m_sections.end(); ++it) {
        SectionWidgets& w = it.value();
        const Category sectionCat = static_cast<Category>(it.key());
        const bool open = (sectionCat == cat);

        const QSignalBlocker toggleBlock(w.toggle);
        w.toggle->setChecked(open);
        w.toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        w.body->setVisible(open);

        // Highlight the matching entry.  When this section is the
        // expanded one, prefer the user's explicit selection name;
        // otherwise fall back to the scene's active entity for this
        // category so the dropdown shows e.g. the active camera /
        // rasterizer on first load instead of being empty.
        if (w.combo) {
            const QSignalBlocker comboBlock(w.combo);
            QString display = (open && !name.isEmpty())
                                  ? name
                                  : m_bridge->activeNameForCategory(sectionCat);
            if (!display.isEmpty()) {
                const int idx = w.combo->findText(display, Qt::MatchExactly);
                w.combo->setCurrentIndex(idx);     // setCurrentIndex(-1) is fine when not found
            } else {
                w.combo->setCurrentIndex(-1);
            }
        }
    }
}

void ViewportProperties::rebuildPropertyRows()
{
    if (!m_bridge) return;
    clearPropertyRows();

    const ViewportBridge::PanelMode mode = m_bridge->panelMode();
    if (mode == ViewportBridge::PanelMode::None) return;

    // The properties live under whichever section is currently
    // expanded.  Map PanelMode → Category (they share numeric values
    // but the explicit switch makes the intent obvious).
    Category propsCat = Category::None;
    switch (mode) {
        case ViewportBridge::PanelMode::Camera:     propsCat = Category::Camera;     break;
        case ViewportBridge::PanelMode::Rasterizer: propsCat = Category::Rasterizer; break;
        case ViewportBridge::PanelMode::Object:     propsCat = Category::Object;     break;
        case ViewportBridge::PanelMode::Light:      propsCat = Category::Light;      break;
        default: return;
    }
    auto sectionIt = m_sections.find(static_cast<int>(propsCat));
    if (sectionIt == m_sections.end()) return;
    SectionWidgets& section = sectionIt.value();
    QVBoxLayout* propsLayout = section.propsLayout;
    if (!propsLayout) return;

    const QVector<ViewportProperty> props = m_bridge->propertySnapshot();
    for (const ViewportProperty& p : props) {
        if (p.editable) {
            auto* container = new QWidget;
            auto* col = new QVBoxLayout(container);
            col->setContentsMargins(0, 0, 0, 0);
            col->setSpacing(2);

            auto* label = new QLabel(p.name);
            QFont f = label->font();
            f.setBold(true);
            label->setFont(f);
            label->setStyleSheet("color: palette(window-text);");

            auto* edit = new QLineEdit;
            edit->setObjectName(p.name);
            edit->setText(p.value);
            connect(edit, &QLineEdit::editingFinished,
                    this, &ViewportProperties::onLineEditFinished);

            col->addWidget(label);

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
                    presetButton->setText(QStringLiteral("⋮"));
                    presetButton->setToolTip(tr("Quick-pick presets"));
                    presetButton->setPopupMode(QToolButton::InstantPopup);
                    auto* menu = new QMenu(presetButton);
                    const QString propName = p.name;
                    for (const ViewportPropertyPreset& preset : p.presets) {
                        const QString lbl = preset.label;
                        const QString val = preset.value;
                        QAction* action = menu->addAction(lbl);
                        connect(action, &QAction::triggered, this,
                                [this, propName, val]() {
                                    if (!m_bridge) return;
                                    if (m_bridge->setProperty(propName, val)) {
                                        m_lastValue.insert(propName, val);
                                        refresh();
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
            propsLayout->addWidget(container);
            m_fields.insert(p.name, edit);
            m_lastValue.insert(p.name, p.value);
        } else {
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

            auto* lbl = new QLabel(p.value);
            lbl->setStyleSheet("color: palette(placeholder-text); font-family: monospace;");
            lbl->setWordWrap(true);
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
            propsLayout->addWidget(container);
            m_readOnly.insert(p.name, lbl);
        }
    }
}

void ViewportProperties::refresh()
{
    if (!m_bridge) return;

    m_headerLabel->setText(m_bridge->panelHeader());

    // Re-pull entity lists when the scene epoch advances.  Cheap to
    // pull on every refresh too — small lists — but the epoch gate
    // keeps the C-API chatter down on busy frames.
    const unsigned int epoch = m_bridge->sceneEpoch();
    if (epoch != m_lastEpoch) {
        m_lastEpoch = epoch;
        rebuildEntityLists();
    }

    syncAccordionFromSelection();
    rebuildPropertyRows();
}

void ViewportProperties::onLineEditFinished()
{
    auto* edit = qobject_cast<QLineEdit*>(sender());
    if (!edit || !m_bridge) return;
    const QString name = edit->objectName();
    const QString val  = edit->text();
    if (val == m_lastValue.value(name)) return;
    if (m_bridge->setProperty(name, val)) {
        m_lastValue.insert(name, val);
        refresh();
    }
}
