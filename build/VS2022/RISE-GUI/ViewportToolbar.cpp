//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.cpp - Photoshop-style category-slot toolbar +
//    (RISE UI redesign) the right-hand status-chip cluster.
//
//////////////////////////////////////////////////////////////////////

#include "ViewportToolbar.h"
#include "Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QWidgetAction>
#include <QStyle>
#include <QTimer>

ViewportToolbar::ViewportToolbar(QWidget* parent)
    : QWidget(parent)
{
    setFixedHeight(40);
    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgCenter);
        setPalette(pal);
    }
    // objectName-scoped selector (rather than a bare declaration list)
    // so it can safely combine with the QToolButton{...} rules appended
    // below in one setStyleSheet call -- mixing a bare "prop: value;"
    // declaration with a later selector block in the same stylesheet
    // string is not a combination Qt's CSS subset reliably supports.
    setObjectName(QStringLiteral("viewportToolbar"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(8);

    // ---- Segmented tool group -------------------------------------------
    auto* toolGroup = new QWidget(this);
    toolGroup->setObjectName(QStringLiteral("toolGroup"));
    toolGroup->setStyleSheet(QStringLiteral(
        "#toolGroup { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
        .arg(Theme::hex(Theme::bgPanel), Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusMedium));
    auto* toolGroupLayout = new QHBoxLayout(toolGroup);
    toolGroupLayout->setContentsMargins(2, 2, 2, 2);
    toolGroupLayout->setSpacing(2);

    setStyleSheet(QStringLiteral(
        "#viewportToolbar { border-bottom: 1px solid %1; }"
        "QToolButton {"
        "  border: 1px solid transparent;"
        "  border-radius: %2px;"
        "  padding: 2px;"
        "}"
        "QToolButton:hover { background: %3; }"
        "QToolButton:checked {"
        "  background: %4;"
        "  color: #0d1116;"
        "  border: 1px solid %4;"
        "}")
        .arg(Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusSmall)
        .arg(Theme::rgba(Theme::fillHover), Theme::hex(Theme::accent)));

    // Three category slots, in canonical order: Select, Camera,
    // ObjectTransform.  Numeric values mirror
    // RISE::SceneEditController::ToolCategory and the C-API
    // SceneEditToolCategory_* constants.  ScrubTimeline lives in the
    // bottom timeline bar, not in the toolbar.
    const ViewportBridge::ToolCategory cats[] = {
        ViewportBridge::ToolCategory::Select,
        ViewportBridge::ToolCategory::Camera,
        ViewportBridge::ToolCategory::ObjectTransform,
    };
    for (auto c : cats) {
        Slot s;
        s.category = c;
        s.button   = makeSlotButton(c);
        toolGroupLayout->addWidget(s.button);
        m_slots.append(s);
    }
    layout->addWidget(toolGroup);

    auto* sep1 = new QFrame(this);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setFixedHeight(18);
    sep1->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight)));
    layout->addWidget(sep1);

    m_undoBtn = new QToolButton(this);
    m_undoBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_undoBtn->setToolTip(tr("Undo — revert the last edit (per-drag composites are one entry)"));
    connect(m_undoBtn, &QToolButton::clicked, this, &ViewportToolbar::undoClicked);
    layout->addWidget(m_undoBtn);

    m_redoBtn = new QToolButton(this);
    m_redoBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    m_redoBtn->setToolTip(tr("Redo — re-apply the most recently undone edit"));
    connect(m_redoBtn, &QToolButton::clicked, this, &ViewportToolbar::redoClicked);
    layout->addWidget(m_redoBtn);

    auto* sep2 = new QFrame(this);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setFixedHeight(18);
    sep2->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight)));
    layout->addWidget(sep2);

    // ---- Active-camera chip (read-only) ----------------------------------
    m_cameraChip = new QLabel(this);
    m_cameraChip->setFont(Theme::sans(11));
    m_cameraChip->setStyleSheet(QStringLiteral(
        "color: %1; background-color: %2; border: 1px solid %3; border-radius: %4px; padding: 5px 11px;")
        .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::bgPanel), Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusMedium));
    layout->addWidget(m_cameraChip);

    layout->addStretch(1);

    // ---- REGION chip -------------------------------------------------------
    m_regionChip = new QToolButton(this);
    m_regionChip->setFont(Theme::mono(10));
    m_regionChip->setCursor(Qt::PointingHandCursor);
    m_regionChip->setToolTip(tr("Toggle region refinement"));
    connect(m_regionChip, &QToolButton::clicked, this, &ViewportToolbar::onRegionChipClicked);
    layout->addWidget(m_regionChip);

    // ---- EV chip (popup exposure slider) ------------------------------------
    m_evChip = new QToolButton(this);
    m_evChip->setFont(Theme::mono(10));
    m_evChip->setCursor(Qt::PointingHandCursor);
    m_evChip->setPopupMode(QToolButton::InstantPopup);
    m_evChip->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: 4px; padding: 2px 7px; }"
        "QToolButton::menu-indicator { image: none; }")
        .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderLight)));

    m_evMenu = new QMenu(m_evChip);
    auto* evPopup = new QWidget();
    auto* evPopupLayout = new QVBoxLayout(evPopup);
    evPopupLayout->setContentsMargins(12, 10, 12, 10);
    evPopupLayout->setSpacing(6);
    auto* evTitle = new QLabel(tr("Exposure"), evPopup);
    evTitle->setFont(Theme::sans(10));
    evTitle->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    evPopupLayout->addWidget(evTitle);
    m_exposureSlider = new ExposureSlider(evPopup);
    m_exposureSlider->setRange(kExposureSliderMin, kExposureSliderMax);
    m_exposureSlider->setValue(0);
    m_exposureSlider->setSingleStep(1);
    m_exposureSlider->setPageStep(10);
    m_exposureSlider->setMinimumWidth(180);
    m_exposureSlider->setToolTip(tr("Display exposure in EV stops.  Double-click the slider to reset to 0."));
    evPopupLayout->addWidget(m_exposureSlider);
    m_evValueLabel = new QLabel(QStringLiteral("0.0 EV"), evPopup);
    m_evValueLabel->setFont(Theme::mono(10));
    m_evValueLabel->setAlignment(Qt::AlignRight);
    m_evValueLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    evPopupLayout->addWidget(m_evValueLabel);
    auto* evWidgetAction = new QWidgetAction(m_evMenu);
    evWidgetAction->setDefaultWidget(evPopup);
    m_evMenu->addAction(evWidgetAction);
    m_evChip->setMenu(m_evMenu);
    connect(m_exposureSlider, &QSlider::valueChanged, this, &ViewportToolbar::updateEvChipLabel);
    connect(m_exposureSlider, &ExposureSlider::resetRequested, this, [this]() { m_exposureSlider->setValue(0); });
    layout->addWidget(m_evChip);

    // ---- EDR chip ------------------------------------------------------------
    m_edrChip = new QToolButton(this);
    m_edrChip->setText(QStringLiteral("EDR"));
    m_edrChip->setFont(Theme::mono(10));
    m_edrChip->setCursor(Qt::PointingHandCursor);
    m_edrChip->setEnabled(false);
    connect(m_edrChip, &QToolButton::clicked, this, &ViewportToolbar::onEdrChipClicked);
    layout->addWidget(m_edrChip);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(500);
    connect(m_pollTimer, &QTimer::timeout, this, &ViewportToolbar::pollState);
    m_pollTimer->start();

    refreshAllSlots();
    updateRegionChip();
    updateCameraChip();
    updateEvChipLabel(0);
    setEdrChecked(false);
    setEdrEnabled(false);
}

void ViewportToolbar::setUndoRedoEnabled(bool enabled)
{
    if (m_undoBtn) {
        m_undoBtn->setEnabled(enabled);
        m_undoBtn->setToolTip(enabled
            ? tr("Undo — revert the last edit (per-drag composites are one entry)")
            : tr("Unavailable while a render is in flight"));
    }
    if (m_redoBtn) {
        m_redoBtn->setEnabled(enabled);
        m_redoBtn->setToolTip(enabled
            ? tr("Redo — re-apply the most recently undone edit")
            : tr("Unavailable while a render is in flight"));
    }
}

void ViewportToolbar::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_regionArmed = false;
    refreshAllSlots();
    updateRegionChip();
    updateCameraChip();
}

QToolButton* ViewportToolbar::makeSlotButton(ViewportBridge::ToolCategory cat)
{
    auto* btn = new QToolButton(this);
    btn->setCheckable(true);
    btn->setProperty("category", static_cast<int>(cat));
    btn->setToolTip(tooltipForCategory(cat));
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(btn, &QToolButton::clicked, this, &ViewportToolbar::onSlotClicked);

    // Right-click flyout — same list as macOS's secondary-action menu.
    // Single-tool slots (Select) skip the menu entirely.
    const auto subs = subToolsForCategory(cat);
    if (subs.size() > 1) {
        connect(btn, &QToolButton::customContextMenuRequested,
                this, [this, btn, subs](const QPoint& pos) {
            QMenu menu(btn);
            for (auto sub : subs) {
                QIcon icon = iconForTool(sub);
                QAction* act = icon.isNull()
                    ? menu.addAction(labelForTool(sub))
                    : menu.addAction(icon, labelForTool(sub));
                connect(act, &QAction::triggered, this, [this, sub] {
                    applyToolSelection(sub);
                });
            }
            menu.exec(btn->mapToGlobal(pos));
        });
    }
    return btn;
}

QVector<ViewportTool> ViewportToolbar::subToolsForCategory(
    ViewportBridge::ToolCategory cat) const
{
    switch (cat) {
    case ViewportBridge::ToolCategory::Select:
        return { ViewportTool::Select };
    case ViewportBridge::ToolCategory::Camera:
        return { ViewportTool::OrbitCamera,
                 ViewportTool::PanCamera,
                 ViewportTool::ZoomCamera,
                 ViewportTool::RollCamera };
    case ViewportBridge::ToolCategory::ObjectTransform:
        return { ViewportTool::TranslateObject,
                 ViewportTool::RotateObject,
                 ViewportTool::ScaleObject };
    }
    return {};
}

QIcon ViewportToolbar::iconForTool(ViewportTool t) const
{
    // Themed icon names (KDE / freedesktop).  Falls back to no icon
    // on Windows where icon themes aren't installed by default; the
    // tooltip + slot color still convey the active tool.
    const char* themeName = "";
    switch (t) {
    case ViewportTool::Select:          themeName = "edit-select";              break;
    case ViewportTool::TranslateObject: themeName = "transform-move";           break;
    case ViewportTool::RotateObject:    themeName = "transform-rotate";         break;
    case ViewportTool::ScaleObject:     themeName = "transform-scale";          break;
    case ViewportTool::OrbitCamera:     themeName = "view-rotate";              break;
    case ViewportTool::PanCamera:       themeName = "transform-move-horizontal";break;
    case ViewportTool::ZoomCamera:      themeName = "zoom-in";                  break;
    case ViewportTool::RollCamera:      themeName = "object-rotate-right";      break;
    case ViewportTool::ScrubTimeline:   themeName = "media-seek-forward";       break;
    }
    return QIcon::fromTheme(themeName);
}

QString ViewportToolbar::labelForTool(ViewportTool t) const
{
    switch (t) {
    case ViewportTool::Select:          return tr("Select");
    case ViewportTool::TranslateObject: return tr("Translate");
    case ViewportTool::RotateObject:    return tr("Rotate");
    case ViewportTool::ScaleObject:     return tr("Scale");
    case ViewportTool::OrbitCamera:     return tr("Orbit");
    case ViewportTool::PanCamera:       return tr("Pan");
    case ViewportTool::ZoomCamera:      return tr("Zoom");
    case ViewportTool::RollCamera:      return tr("Roll");
    case ViewportTool::ScrubTimeline:   return tr("Scrub");
    }
    return QString();
}

QString ViewportToolbar::tooltipForCategory(ViewportBridge::ToolCategory cat) const
{
    switch (cat) {
    case ViewportBridge::ToolCategory::Select:
        return tr("Select — click an object in the viewport to make it the next edit's target");
    case ViewportBridge::ToolCategory::Camera:
        return tr("Camera — orbit, pan, zoom, or roll the camera (right-click to switch sub-tool)");
    case ViewportBridge::ToolCategory::ObjectTransform:
        return tr("Transform — translate, rotate, or scale the selected object via the gizmo (right-click to switch sub-tool)");
    }
    return QString();
}

void ViewportToolbar::refreshSlot(const Slot& s)
{
    // Pick which tool's icon/label to show on the slot button.  If
    // the current tool belongs to the slot's category, show it.
    // Otherwise the bridge's per-category last-used pick — or the
    // category default when the bridge isn't wired (test mode).
    ViewportTool shown;
    if (ViewportBridge::categoryForTool(m_current) == s.category) {
        shown = m_current;
    } else if (m_bridge) {
        shown = m_bridge->lastSubToolForCategory(s.category);
    } else {
        shown = ViewportBridge::defaultSubToolForCategory(s.category);
    }
    QIcon icon = iconForTool(shown);
    if (!icon.isNull()) {
        s.button->setIcon(icon);
        s.button->setText(QString());
        s.button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    } else {
        // No icon theme — use a 2-char abbreviation so the slot still
        // distinguishes the active sub-tool at a glance.
        s.button->setText(labelForTool(shown).left(2));
        s.button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    const bool isActiveCategory =
        ( ViewportBridge::categoryForTool(m_current) == s.category );
    s.button->setChecked(isActiveCategory);
}

void ViewportToolbar::refreshAllSlots()
{
    for (const auto& s : m_slots) refreshSlot(s);
}

void ViewportToolbar::onSlotClicked()
{
    auto* sender = qobject_cast<QToolButton*>(this->sender());
    if (!sender) return;
    const auto cat = static_cast<ViewportBridge::ToolCategory>(
        sender->property("category").toInt());
    // Click activates the slot's currently-shown sub-tool (last-used
    // memory).  Matches macOS Button-action behavior.
    ViewportTool shown;
    if (ViewportBridge::categoryForTool(m_current) == cat) {
        shown = m_current;
    } else if (m_bridge) {
        shown = m_bridge->lastSubToolForCategory(cat);
    } else {
        shown = ViewportBridge::defaultSubToolForCategory(cat);
    }
    applyToolSelection(shown);
}

void ViewportToolbar::applyToolSelection(ViewportTool t)
{
    m_current = t;
    refreshAllSlots();
    emit toolChanged(m_current);
}

// ============================================================
// RISE UI redesign: right-hand status-chip cluster
// ============================================================

void ViewportToolbar::pollState()
{
    updateCameraChip();
    updateRegionChip();
}

void ViewportToolbar::updateCameraChip()
{
    if (!m_cameraChip) return;
    const QString name = m_bridge
        ? m_bridge->activeNameForCategory(ViewportBridge::Category::Camera)
        : QString();
    m_cameraChip->setText(QString::fromUtf8("\xE2\x97\x89 ")
        + (name.isEmpty() ? tr("No camera") : name));
}

void ViewportToolbar::updateRegionChip()
{
    if (!m_regionChip) return;

    const bool honors = m_bridge && m_bridge->interactiveRasterizerHonorsRegion();
    m_regionChip->setEnabled(honors);
    m_regionChip->setToolTip(honors
        ? tr("Toggle region refinement")
        : tr("This rasterizer doesn't support region refinement"));

    unsigned int l = 0, t = 0, r = 0, b = 0;
    const bool hasRegion = honors && m_bridge && m_bridge->getInteractiveRegion(&l, &t, &r, &b);

    // A drag completed since we armed -- disarm now that the bridge
    // reports an active region.  Poll-driven rather than event-driven
    // so ViewportWidget doesn't need a back-channel signal.
    if (hasRegion && m_regionArmed) {
        m_regionArmed = false;
        emit regionArmedChanged(false);
    }

    QString text;
    QColor color;
    if (!honors) {
        text = tr("REGION");
        color = Theme::textDisabled;
    } else if (hasRegion) {
        text = tr("REGION");
        color = Theme::warn;
    } else if (m_regionArmed) {
        text = tr("REGION \xE2\x80\xA6");
        color = Theme::warn;
    } else {
        text = tr("REGION off");
        color = Theme::textFaint;
    }
    m_regionChip->setText(text);
    m_regionChip->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: 1px solid %2; border-radius: 4px; padding: 2px 7px; }")
        .arg(Theme::hex(color),
             Theme::rgba(QColor(color.red(), color.green(), color.blue(), static_cast<int>(0.35 * 255)))));
}

void ViewportToolbar::onRegionChipClicked()
{
    if (!m_bridge || !m_regionChip->isEnabled()) return;

    unsigned int l = 0, t = 0, r = 0, b = 0;
    const bool hasRegion = m_bridge->getInteractiveRegion(&l, &t, &r, &b);
    if (hasRegion) {
        m_bridge->clearInteractiveRegion();
        m_regionArmed = false;
    } else {
        // Toggle armed on/off (a second click while armed cancels
        // without ever drawing a box).
        m_regionArmed = !m_regionArmed;
    }
    emit regionArmedChanged(m_regionArmed);
    updateRegionChip();
}

void ViewportToolbar::cancelRegionArm()
{
    if (!m_regionArmed) return;
    m_regionArmed = false;
    emit regionArmedChanged(false);
    updateRegionChip();
}

void ViewportToolbar::updateEvChipLabel(int sliderValue)
{
    const double ev = sliderValue / 10.0;
    const QString text = QStringLiteral("EV %1%2")
        .arg(ev > 0 ? QStringLiteral("+") : QString())
        .arg(ev, 0, 'f', 1);
    if (m_evChip) m_evChip->setText(text);
    if (m_evValueLabel) m_evValueLabel->setText(text);
}

void ViewportToolbar::setEvEnabled(bool enabled)
{
    if (m_evChip) m_evChip->setEnabled(enabled);
    if (m_exposureSlider) m_exposureSlider->setEnabled(enabled);
}

void ViewportToolbar::setEdrChecked(bool checked)
{
    m_edrChecked = checked;
    if (!m_edrChip) return;
    m_edrChip->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; background-color: %2; border: 1px solid %3; border-radius: 4px; padding: 2px 7px; }")
        .arg(Theme::hex(checked ? Theme::accentLight : Theme::textFaint),
             checked ? Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(), Theme::accent.blue(), static_cast<int>(0.16 * 255)))
                     : QStringLiteral("transparent"),
             Theme::hex(checked ? Theme::accent : Theme::borderLight)));
}

void ViewportToolbar::setEdrEnabled(bool enabled)
{
    m_edrAvailable = enabled;
    if (m_edrChip) m_edrChip->setEnabled(enabled);
}

void ViewportToolbar::onEdrChipClicked()
{
    if (!m_edrAvailable) return;
    emit edrToggleClicked();
}
