//////////////////////////////////////////////////////////////////////
//
//  ViewportProperties.cpp - Right-side accordion implementation.
//    Five sections (Cameras / Rasterizer / Objects / Lights / Output
//    Settings), single selection, descriptor-driven property rows
//    under the expanded section.  See header for layout and macOS /
//    Android counterparts.
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
        // Glyph is painted in paintEvent — we draw two filled triangles
        // rather than rely on a Unicode arrow glyph being present in the
        // system font (Mac uses the SF Symbol `chevron.up.chevron.down`;
        // there is no portable Windows-font equivalent).
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
        // Two filled triangles, up over down, matching SF Symbol
        // `chevron.up.chevron.down`.  Geometry is in widget-local
        // coordinates with a 2 px border so the glyph never clips at
        // odd DPI scales.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QColor c = palette().color(QPalette::PlaceholderText);
        if (!c.isValid()) c = palette().color(QPalette::WindowText);
        p.setBrush(c);

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

struct AccordionSectionDef {
    ViewportBridge::Category category;
    const char*              title;
};

// Top-down order: Cameras first (most-used), Animation right under it
// (picking a named animation re-points the timeline, like activating a
// camera), Rasterizer (scene-global), Objects (long lists), Lights, then
// Output Settings (Film — last because it's a one-row global config the
// user typically touches once at the start of a session).  Animation has
// no editable properties — selecting a row just activates that animation
// (the section renders generically: combo populated from categoryEntities,
// pick routed through setSelection).
static const AccordionSectionDef kSectionDefs[] = {
    { ViewportBridge::Category::Camera,     "Cameras"         },
    { ViewportBridge::Category::Animation,  "Animation"       },
    { ViewportBridge::Category::Rasterizer, "Rasterizer"      },
    { ViewportBridge::Category::Object,     "Objects"         },
    { ViewportBridge::Category::Light,      "Lights"          },
    { ViewportBridge::Category::Material,   "Materials"       },
    { ViewportBridge::Category::Medium,     "Media"           },
    { ViewportBridge::Category::Film,       "Output Settings" },
};

}  // namespace

ViewportProperties::ViewportProperties(ViewportBridge* bridge, QWidget* parent)
    : QWidget(parent)
    , m_bridge(bridge)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Phase 6.5: header row with Save / Save As… buttons on the right.
    // Mirrors the macOS PropertiesPanel.swift header HStack.  The
    // buttons are stored as members so the dirtyChanged slot can
    // re-evaluate their enable state.
    auto* headerRow = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(headerRow);
    headerLayout->setContentsMargins(8, 4, 8, 4);
    headerLayout->setSpacing(4);

    m_headerLabel = new QLabel(tr("Scene"), headerRow);
    m_headerLabel->setStyleSheet("font-weight: bold;");
    headerLayout->addWidget(m_headerLabel);
    headerLayout->addStretch(1);

    m_saveButton = new QToolButton(headerRow);
    m_saveButton->setText(tr("Save"));
    m_saveButton->setEnabled(false);   // updated by onDirtyChanged
    m_saveButton->setToolTip(tr("No changes to save"));
    connect(m_saveButton, &QToolButton::clicked,
            this, [this]() { performSceneSave(/*useLoadedPath=*/true); });
    headerLayout->addWidget(m_saveButton);

    m_saveAsButton = new QToolButton(headerRow);
    m_saveAsButton->setText(tr("Save As…"));
    m_saveAsButton->setEnabled(false);
    m_saveAsButton->setToolTip(tr("No changes to save"));
    connect(m_saveAsButton, &QToolButton::clicked,
            this, [this]() { performSceneSave(/*useLoadedPath=*/false); });
    headerLayout->addWidget(m_saveAsButton);

    root->addWidget(headerRow);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    // The dirtyChanged signal is emitted via QueuedConnection from
    // the bridge's C-trampoline (see ViewportBridge ctor), so a
    // direct Qt::AutoConnection here delivers on the GUI thread.
    if (m_bridge) {
        connect(m_bridge, &ViewportBridge::dirtyChanged,
                this, &ViewportProperties::onDirtyChanged);
    }

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

        // Cameras section gets a "+" button next to the combo for
        // cloning the active camera under a new name.  Other sections
        // use the combo alone — no add affordance for them yet.
        if (cat == Category::Camera) {
            auto* row = new QHBoxLayout();
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(4);
            row->addWidget(w.combo, 1);
            auto* addBtn = new QToolButton(w.body);
            addBtn->setText(QStringLiteral("+"));
            addBtn->setToolTip(tr("Clone the active camera under a new name"));
            addBtn->setStyleSheet("QToolButton { padding: 1px 6px; }");
            connect(addBtn, &QToolButton::clicked, this, &ViewportProperties::onAddCameraClicked);
            row->addWidget(addBtn);
            bodyLayout->addLayout(row);
        } else {
            bodyLayout->addWidget(w.combo);
        }

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
        // Phase 4b multi-section model:
        //  - Open: empty-name SetSelection sets the per-cat expanded
        //    flag without picking a specific entity.
        //  - Close: per-section CollapseSection (clears just THIS
        //    section's expanded flag + per-cat selection, leaves
        //    other sections alone).  Pre-Phase-4b this was a panel-
        //    wide collapse via SetSelection(None), which would now
        //    close every expanded section including the auto-synced
        //    Material section.
        connect(w.toggle, &QToolButton::toggled, this,
                [this, cat](bool checked) {
                    if (!m_bridge) return;
                    if (checked) {
                        m_bridge->setSelection(cat, QString());
                    } else {
                        m_bridge->collapseSection(cat);
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

// ============================================================
// Phase 6.5: scene-file save.  Header buttons drive these.
// ============================================================

void ViewportProperties::onDirtyChanged(bool hasUnsavedChanges)
{
    if (!m_saveButton || !m_saveAsButton || !m_bridge) return;
    const QString loaded = m_bridge->loadedFilePath();
    // In-place Save needs both: edits AND a known target path
    // (otherwise the click would have to fall through to a
    // dialog anyway — we let the user explicitly pick Save As…
    // in that case to surface the intent).
    m_saveButton->setEnabled(hasUnsavedChanges && !loaded.isEmpty());
    m_saveAsButton->setEnabled(hasUnsavedChanges);

    if (!hasUnsavedChanges) {
        m_saveButton->setToolTip(tr("No changes to save"));
        m_saveAsButton->setToolTip(tr("No changes to save"));
    } else {
        m_saveButton->setToolTip(loaded.isEmpty()
            ? tr("Use Save As… (no loaded path)")
            : tr("Save scene to %1").arg(loaded));
        m_saveAsButton->setToolTip(tr("Save scene to a chosen path…"));
    }
}

void ViewportProperties::performSceneSave(bool useLoadedPath)
{
    if (!m_bridge) return;

    QString target;
    if (useLoadedPath) {
        target = m_bridge->loadedFilePath();
    }
    if (target.isEmpty()) {
        // Either the caller explicitly asked for Save As… or the
        // in-place save fell through because no path was known.
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
        // Re-anchor + refresh the scene-editor text pane via
        // MainWindow (only it owns RenderEngine + SceneEditor).
        // dirtyChanged(false) already flipped the Save buttons back
        // to disabled on the C++ side.
        emit sceneSavedToPath(target);
        break;
    case ViewportBridge::SaveStatus::NoOp:
        // Silent success — file unchanged on disk; no re-anchor or
        // refresh needed.
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

    // Track primary for the panel header etc.
    m_currentSelectionCat  = m_bridge->selectionCategory();
    m_currentSelectionName = m_bridge->selectionName();

    // Phase 4b: per-category expansion is tracked SEPARATELY from
    // the per-category selection (so a header click expands the
    // section even when no entity is picked yet).  Auto-sync of
    // Material expansion on Object pick is handled controller-side.
    for (auto it = m_sections.begin(); it != m_sections.end(); ++it) {
        SectionWidgets& w = it.value();
        const Category sectionCat = static_cast<Category>(it.key());
        const QString perCatName = m_bridge->selectionNameForCategory(sectionCat);
        const bool open = m_bridge->isSectionExpanded(sectionCat);

        const QSignalBlocker toggleBlock(w.toggle);
        w.toggle->setChecked(open);
        w.toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        w.body->setVisible(open);

        if (w.combo) {
            const QSignalBlocker comboBlock(w.combo);
            QString display = !perCatName.isEmpty()
                                  ? perCatName
                                  : m_bridge->activeNameForCategory(sectionCat);
            if (!display.isEmpty()) {
                const int idx = w.combo->findText(display, Qt::MatchExactly);
                w.combo->setCurrentIndex(idx);
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

    // Force a refresh so the controller's per-category snapshots
    // are up to date before we read them per-section.
    (void)m_bridge->propertySnapshot();

    // Phase 4b: build property rows for every section that is
    // currently expanded.  Empty-selection expanded sections still
    // get a (possibly empty) row list — Camera/Rasterizer/Film
    // render their active-entity rows even when no specific entity
    // is picked.  Edits route through SetPropertyForCategory so the
    // right per-section entity is targeted.
    for (auto it = m_sections.begin(); it != m_sections.end(); ++it) {
        const Category sectionCat = static_cast<Category>(it.key());
        SectionWidgets& section = it.value();
        QVBoxLayout* propsLayout = section.propsLayout;
        if (!propsLayout) continue;

        if (!m_bridge->isSectionExpanded(sectionCat)) continue;

        rebuildPropertyRowsFor(sectionCat, section);
    }
}

void ViewportProperties::rebuildPropertyRowsFor(Category sectionCat, SectionWidgets& section)
{
    if (!m_bridge) return;
    QVBoxLayout* propsLayout = section.propsLayout;
    if (!propsLayout) return;

    const QVector<ViewportProperty> props = m_bridge->propertySnapshotFor(sectionCat);
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
            // Phase 4b: tag every QLineEdit with its section's
            // category so onLineEditFinished routes through the
            // per-category SetProperty.  Without this, edits in the
            // Material section while Object is the primary would
            // wrong-target the object.
            edit->setProperty("riseSectionCategory", static_cast<int>(sectionCat));
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
                        [this, sectionCat](const QString& n, const QString& v) {
                            if (!m_bridge) return;
                            if (m_bridge->setPropertyForCategory(sectionCat, n, v)) {
                                m_lastValue.insert(n, v);
                            }
                        },
                        [this]() {
                            m_scrubbing = true;
                            if (m_bridge) m_bridge->beginPropertyScrub();
                        },
                        [this]() {
                            m_scrubbing = false;
                            if (m_bridge) m_bridge->endPropertyScrub();
                            // Pull canonical values back after the user lets go.
                            refresh();
                        });
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
                                [this, sectionCat, propName, val]() {
                                    if (!m_bridge) return;
                                    if (m_bridge->setPropertyForCategory(sectionCat, propName, val)) {
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
    // While a ScrubHandle drag is in flight, do NOT rebuild the rows —
    // doing so would deleteLater() the handle whose mouseMoveEvent put
    // setProperty() → imageUpdated → here on the stack, killing the
    // mouse grab.  endPropertyScrub calls refresh() once the user lets
    // go to re-sync canonical values.
    if (m_scrubbing) return;

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
    // Phase 4b: read the section's category off the edit widget so
    // the edit routes through SetPropertyForCategory.  Pre-Phase-4b
    // this used the primary selection, which was wrong for rows in
    // an auto-synced secondary section (e.g. Material rows when
    // Object was primary).
    const int catInt = edit->property("riseSectionCategory").toInt();
    const Category cat = static_cast<Category>(catInt);
    if (m_bridge->setPropertyForCategory(cat, name, val)) {
        m_lastValue.insert(name, val);
        refresh();
    }
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
           "• The clone is in-memory only — saving the .RISEscene file\n"
           "  from the editor does not yet emit added cameras.\n"
           "• Duplicate names get a numeric suffix appended."),
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

    // Promote the new camera to the panel's selection so the user
    // sees its properties immediately.
    m_bridge->setSelection(Category::Camera, chosenName);
    refresh();

    // One-shot persistence caveat per session.
    if (!m_addCameraCaveatShown) {
        m_addCameraCaveatShown = true;
        QMessageBox::information(
            this,
            tr("New camera \"%1\" added").arg(chosenName),
            tr("Heads up — added cameras are kept in memory only until the\n"
               "scene-text round-trip lands.  Save your scene file from a\n"
               "text editor to preserve them across reloads."));
    }
}
