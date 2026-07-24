//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.cpp - Grouped always-visible tool buttons (see the
//    header's discoverability-redesign doc) + (RISE UI redesign) the
//    right-hand status-chip cluster.
//
//////////////////////////////////////////////////////////////////////

#include "ViewportToolbar.h"
#include "Theme.h"

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QWidgetAction>
#include <QStringList>
#include <QSize>
#include <QTimer>

ViewportToolbar::ViewportToolbar(QWidget* parent)
    : QWidget(parent)
{
    // Discoverability redesign: grown from 40 to 52pt to fit the now-
    // labeled ~44pt-tall tool buttons (icon + 9px label below) -- mirrors
    // ContentView.swift's viewportToolbarRow height bump.  The rest of
    // the row's chips are unchanged in height and simply re-center
    // within the taller row (QHBoxLayout's default vertical centering).
    setFixedHeight(52);
    setAutoFillBackground(true);
    // objectName-scoped selector (rather than a bare declaration list)
    // so it can safely combine with the QToolButton{...} rules appended
    // below in one setStyleSheet call -- mixing a bare "prop: value;"
    // declaration with a later selector block in the same stylesheet
    // string is not a combination Qt's CSS subset reliably supports.
    setObjectName(QStringLiteral("viewportToolbar"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 0);
    layout->setSpacing(8);

    // ---- Grouped tool buttons ---------------------------------------
    // Discoverability redesign (see header doc): every tool is now its
    // own always-visible, labeled button -- no more category slot +
    // right-click flyout.  Three visually separated clusters inside one
    // bordered container: Select | Camera (Orbit/Pan/Zoom/Roll) |
    // Object (Move/Rotate/Scale).
    // Promoted to a member (was a ctor-local) so restyleTheme() can
    // re-apply its token-dependent QSS on a live theme switch -- see the
    // LIVE THEME-SWITCH CONTRACT in Theme.h.
    m_toolGroup = new QWidget(this);
    m_toolGroup->setObjectName(QStringLiteral("toolGroup"));
    auto* toolGroupLayout = new QHBoxLayout(m_toolGroup);
    toolGroupLayout->setContentsMargins(2, 2, 2, 2);
    toolGroupLayout->setSpacing(0);

    // Numeric category values mirror RISE::SceneEditController::ToolCategory
    // and the C-API SceneEditToolCategory_* constants; `subToolsForCategory`
    // is still the single source of truth for which tools belong to which
    // visual group.  ScrubTimeline lives in the bottom timeline bar, not
    // in this toolbar.
    const ViewportBridge::ToolCategory cats[] = {
        ViewportBridge::ToolCategory::Select,
        ViewportBridge::ToolCategory::Camera,
        ViewportBridge::ToolCategory::ObjectTransform,
    };
    bool firstGroup = true;
    for (auto c : cats) {
        if (!firstGroup) {
            // Promoted to m_categoryDividers (was a loop-local) so
            // restyleTheme() can re-apply its border color.
            auto* divider = new QFrame(m_toolGroup);
            divider->setFrameShape(QFrame::VLine);
            divider->setFixedHeight(30);
            toolGroupLayout->addWidget(divider);
            m_categoryDividers.append(divider);
        }
        firstGroup = false;
        for (ViewportTool t : subToolsForCategory(c)) {
            ToolButtonEntry entry;
            entry.tool   = t;
            entry.button = makeToolButton(t);
            toolGroupLayout->addWidget(entry.button);
            m_toolButtons.append(entry);
        }
    }
    layout->addWidget(m_toolGroup);

    // Promoted to a member (was a ctor-local) -- see restyleTheme().
    m_sep1 = new QFrame(this);
    m_sep1->setFrameShape(QFrame::VLine);
    m_sep1->setFixedHeight(18);
    layout->addWidget(m_sep1);

    // Bundled Lucide icons (undo-2/redo-2), tinted to the same
    // textTertiary/textDisabled pair the #toolBtn cluster to their left
    // uses for its unchecked/disabled states -- Theme::icon's 3-arg
    // overload registers the disabled tint as QIcon::Disabled so
    // setUndoRedoEnabled's setEnabled(false) dims the glyph automatically,
    // matching the tooltip swap already below.
    m_undoBtn = new QToolButton(this);
    m_undoBtn->setIcon(Theme::icon(QStringLiteral("undo-2"), 16, Theme::textTertiary, Theme::textDisabled));
    m_undoBtn->setIconSize(QSize(16, 16));
    m_undoBtn->setToolTip(tr("Undo — revert the last edit (per-drag composites are one entry)"));
    connect(m_undoBtn, &QToolButton::clicked, this, &ViewportToolbar::undoClicked);
    layout->addWidget(m_undoBtn);

    m_redoBtn = new QToolButton(this);
    m_redoBtn->setIcon(Theme::icon(QStringLiteral("redo-2"), 16, Theme::textTertiary, Theme::textDisabled));
    m_redoBtn->setIconSize(QSize(16, 16));
    m_redoBtn->setToolTip(tr("Redo — re-apply the most recently undone edit"));
    connect(m_redoBtn, &QToolButton::clicked, this, &ViewportToolbar::redoClicked);
    layout->addWidget(m_redoBtn);

    // Promoted to a member (was a ctor-local) -- see restyleTheme().
    m_sep2 = new QFrame(this);
    m_sep2->setFrameShape(QFrame::VLine);
    m_sep2->setFixedHeight(18);
    layout->addWidget(m_sep2);

    // ---- N-up multi-viewport layout picker (docs/gui/RENDER_MODES.md §7.5) -
    // 4 fixed icons -- reuses #toolGroup's bordered-container styling (the
    // stylesheet installed on `this` covers #toolBtn's hover/checked
    // states, which these buttons also use). Promoted to a member (was a
    // ctor-local) so restyleTheme() can re-apply its token-dependent QSS.
    m_layoutGroup = new QWidget(this);
    m_layoutGroup->setObjectName(QStringLiteral("toolGroup"));
    auto* layoutGroupLayout = new QHBoxLayout(m_layoutGroup);
    layoutGroupLayout->setContentsMargins(2, 2, 2, 2);
    layoutGroupLayout->setSpacing(0);
    const ViewportBridge::ViewportLayout layouts[] = {
        ViewportBridge::ViewportLayout::Single,
        ViewportBridge::ViewportLayout::TwoH,
        ViewportBridge::ViewportLayout::OnePlusTwo,
        ViewportBridge::ViewportLayout::Quad,
    };
    for (auto l : layouts) {
        LayoutButtonEntry entry;
        entry.layout = l;
        entry.button = makeLayoutButton(l);
        layoutGroupLayout->addWidget(entry.button);
        m_layoutButtons.append(entry);
    }
    layout->addWidget(m_layoutGroup);

    // Promoted to a member (was a ctor-local) -- see restyleTheme().
    m_sep3 = new QFrame(this);
    m_sep3->setFrameShape(QFrame::VLine);
    m_sep3->setFixedHeight(18);
    layout->addWidget(m_sep3);

    // ---- Active-camera chip (read-only) ----------------------------------
    m_cameraChip = new QLabel(this);
    m_cameraChip->setFont(Theme::sans(11));
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

    m_evMenu = new QMenu(m_evChip);
    auto* evPopup = new QWidget();
    auto* evPopupLayout = new QVBoxLayout(evPopup);
    evPopupLayout->setContentsMargins(12, 10, 12, 10);
    evPopupLayout->setSpacing(6);
    // Promoted to a member (was a ctor-local) -- see restyleTheme().
    m_evTitleLabel = new QLabel(tr("Exposure"), evPopup);
    m_evTitleLabel->setFont(Theme::sans(10));
    evPopupLayout->addWidget(m_evTitleLabel);
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

    refreshLayoutButtons();
    updateCameraChip();
    updateEvChipLabel(0);
    setEdrEnabled(false);

    // LIVE THEME-SWITCH CONTRACT (Theme.h): applies every token-dependent
    // stylesheet/icon/palette site above from the CURRENT Theme:: values
    // -- also covers refreshAllToolButtons() / updateRegionChip() /
    // setEdrChecked(), which is why those three aren't called directly
    // above the way they used to be pre-conversion.
    m_themeReady = true;
    restyleTheme();
}

void ViewportToolbar::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void ViewportToolbar::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();
    // Reapplies every one of this toolbar's own token-dependent styling
    // sites from the CURRENT Theme:: token values. Called once at the
    // end of the constructor and again from changeEvent() on every
    // QEvent::PaletteChange. Idempotent, creates no widgets. See Theme.h's
    // LIVE THEME-SWITCH CONTRACT (MainWindow::restyleTheme is the
    // reference implementation this mirrors).

    // setPalette() is an explicit per-widget override, so it does NOT
    // automatically track QApplication::setPalette() -- has to be
    // re-applied here from the current token.
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgCenter);
        setPalette(pal);
    }

    // This widget's own border + the shared #toolBtn selector (tool
    // cluster AND layout-picker buttons, which both use objectName
    // "toolBtn"/"toolGroup"). The checked-state tint is Theme::
    // iconOnAccent, not a bare #ffffff literal -- see that token's doc
    // in Theme.h for why (matches Mac's hardcoded white unconditionally
    // of mode).
    setStyleSheet(QStringLiteral(
        "#viewportToolbar { border-bottom: 1px solid %1; }"
        "QToolButton#toolBtn {"
        "  border: none;"
        "  border-bottom: 2px solid transparent;"
        "  border-radius: %2px;"
        "  padding: 2px 4px;"
        "  color: %3;"
        "}"
        "QToolButton#toolBtn:hover { background: %4; }"
        "QToolButton#toolBtn:checked {"
        "  background: %5;"
        "  color: %6;"
        "  border-bottom: 2px solid %7;"
        "}")
        .arg(Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusSmall)
        .arg(Theme::hex(Theme::textTertiary))
        .arg(Theme::rgba(Theme::fillHover), Theme::rgba(Theme::fillActive),
             Theme::hex(Theme::iconOnAccent), Theme::hex(Theme::accent)));

    const QString toolGroupQss = QStringLiteral(
        "#toolGroup { background-color: %1; border: 1px solid %2; border-radius: %3px; }")
        .arg(Theme::hex(Theme::bgPanel), Theme::hex(Theme::borderHairline))
        .arg(Theme::radiusMedium);
    if (m_toolGroup) m_toolGroup->setStyleSheet(toolGroupQss);
    if (m_layoutGroup) m_layoutGroup->setStyleSheet(toolGroupQss);

    const QString dividerQss = QStringLiteral("color: %1;").arg(Theme::hex(Theme::borderLight));
    for (QFrame* divider : m_categoryDividers) {
        if (divider) divider->setStyleSheet(dividerQss);
    }
    if (m_sep1) m_sep1->setStyleSheet(dividerQss);
    if (m_sep2) m_sep2->setStyleSheet(dividerQss);
    if (m_sep3) m_sep3->setStyleSheet(dividerQss);

    // Bundled Lucide icons (undo-2/redo-2), tinted to the same
    // textTertiary/textDisabled pair the #toolBtn cluster uses for its
    // unchecked/disabled states.
    if (m_undoBtn) m_undoBtn->setIcon(Theme::icon(QStringLiteral("undo-2"), 16, Theme::textTertiary, Theme::textDisabled));
    if (m_redoBtn) m_redoBtn->setIcon(Theme::icon(QStringLiteral("redo-2"), 16, Theme::textTertiary, Theme::textDisabled));

    if (m_cameraChip) {
        m_cameraChip->setStyleSheet(QStringLiteral(
            "color: %1; background-color: %2; border: 1px solid %3; border-radius: %4px; padding: 5px 11px;")
            .arg(Theme::hex(Theme::textPrimary), Theme::hex(Theme::bgPanel), Theme::hex(Theme::borderHairline))
            .arg(Theme::radiusMedium));
    }

    if (m_evChip) {
        m_evChip->setStyleSheet(QStringLiteral(
            "QToolButton { color: %1; border: 1px solid %2; border-radius: 4px; padding: 2px 7px; }"
            "QToolButton::menu-indicator { image: none; }")
            .arg(Theme::hex(Theme::textFaint), Theme::hex(Theme::borderLight)));
    }
    if (m_evTitleLabel) m_evTitleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
    if (m_evValueLabel) m_evValueLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));

    // Icon tint + state-dependent stylesheets that are already re-derived
    // from live state on every call -- re-run them here so a theme switch
    // also refreshes their token-derived colors, not just the static QSS
    // above.
    refreshAllToolButtons();
    updateRegionChip();
    setEdrChecked(m_edrChecked);
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
    refreshAllToolButtons();
    refreshLayoutButtons();
    updateRegionChip();
    updateCameraChip();
}

QToolButton* ViewportToolbar::makeToolButton(ViewportTool t)
{
    auto* btn = new QToolButton(this);
    btn->setObjectName(QStringLiteral("toolBtn"));   // matches the QSS selector installed in the ctor
    btn->setCheckable(true);
    btn->setProperty("tool", static_cast<int>(t));
    btn->setToolTip(tooltipForTool(t));
    btn->setFixedSize(44, 44);
    btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    btn->setIconSize(QSize(15, 15));
    btn->setFont(Theme::sans(9));

    // Icon is left unset here -- refreshAllToolButtons() (called once at
    // the end of the ctor, and on every tool/bridge change thereafter)
    // is the single place that sets both the checked state AND the icon
    // tint together, since the two must always agree.
    //
    // Full label always shown below the icon -- every tool is
    // discoverable by name, not just by pictogram.
    btn->setText(labelForTool(t));

    connect(btn, &QToolButton::clicked, this, &ViewportToolbar::onToolButtonClicked);
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

QIcon ViewportToolbar::iconForTool(ViewportTool t, const QColor& color, int sizePx) const
{
    // Bundled Lucide SVGs (Theme::icon), tinted per call site -- replaces
    // the old QIcon::fromTheme(KDE/freedesktop name) lookup, which never
    // resolved on Windows (no icon theme installed by default) and left
    // these buttons icon-less.  Mirrors ViewportToolbar.swift's per-tool
    // SF Symbol: cursorarrow, move.3d, rotate.3d, scale.3d,
    // arrow.trianglehead.2.counterclockwise.rotate.90 (orbit), hand.draw,
    // plus.magnifyingglass, timeline.selection (scrub), rotate.right.
    const char* iconName = "";
    switch (t) {
    case ViewportTool::Select:          iconName = "mouse-pointer-2";     break;
    case ViewportTool::TranslateObject: iconName = "move-3d";             break;
    case ViewportTool::RotateObject:    iconName = "rotate-3d";           break;
    case ViewportTool::ScaleObject:     iconName = "scale-3d";            break;
    case ViewportTool::OrbitCamera:     iconName = "orbit";               break;
    case ViewportTool::PanCamera:       iconName = "hand";                break;
    case ViewportTool::ZoomCamera:      iconName = "zoom-in";             break;
    // "rotate-cw" (not "iteration-cw", which reads as repeat/loop) --
    // mirrors ViewportToolbar.swift's SF "rotate.right".
    case ViewportTool::RollCamera:      iconName = "rotate-cw";           break;
    case ViewportTool::ScrubTimeline:   iconName = "sliders-horizontal";  break;
    }
    return Theme::icon(QString::fromLatin1(iconName), sizePx, color);
}

QString ViewportToolbar::labelForTool(ViewportTool t) const
{
    switch (t) {
    case ViewportTool::Select:          return tr("Select");
    // Discoverability redesign: "Translate" -> "Move" (mirrors
    // ViewportToolbar.swift's rename — the button reads as a verb a
    // first-time user recognizes without already knowing the jargon).
    case ViewportTool::TranslateObject: return tr("Move");
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

QString ViewportToolbar::tooltipForTool(ViewportTool t) const
{
    // No keyboard-shortcut hint is appended -- none of these tools are
    // bound to a keyboard shortcut anywhere in this app (checked
    // MainWindow::createMenuBar's QAction::setShortcut list), so a
    // fabricated hint would mislead rather than help.  Mirrors
    // ViewportToolbar.swift's per-tool tooltip strings verbatim.
    switch (t) {
    case ViewportTool::Select:
        return tr("Select — click an object in the viewport to make it the target of the next edit");
    case ViewportTool::TranslateObject:
        return tr("Move — drag the selected object to move it through the scene");
    case ViewportTool::RotateObject:
        return tr("Rotate — drag to rotate the selected object around its origin");
    case ViewportTool::ScaleObject:
        return tr("Scale — drag up/down to scale the selected object");
    case ViewportTool::OrbitCamera:
        return tr("Orbit Camera — drag to rotate the camera around the scene");
    case ViewportTool::PanCamera:
        return tr("Pan Camera — drag to translate the camera in screen plane");
    case ViewportTool::ZoomCamera:
        return tr("Zoom Camera — drag to dolly the camera closer or farther");
    case ViewportTool::RollCamera:
        return tr("Roll Camera — drag horizontally to roll the camera around the (camera→look-at) axis");
    case ViewportTool::ScrubTimeline:
        return tr("Scrub Timeline — drag the timeline slider at the bottom to scrub through animation");
    }
    return QString();
}

void ViewportToolbar::refreshAllToolButtons()
{
    // Icon tint follows the same checked/unchecked split the #toolBtn
    // QSS already applies to background/text (checked -> Theme::
    // iconOnAccent on the accent fill; unchecked -> textTertiary) --
    // QIcon has no CSS hook, so the pixmap has to be re-tinted and
    // re-set here in lockstep with setChecked() rather than baked once
    // at button-construction time. Reading Theme::iconOnAccent live
    // (rather than a local static) is also what makes this correct to
    // call again from restyleTheme() on a live theme switch -- see
    // Theme.h's doc on that token for why it's the same value in both
    // palettes today.
    for (const auto& entry : m_toolButtons) {
        const bool checked = (entry.tool == m_current);
        entry.button->setChecked(checked);
        entry.button->setIcon(iconForTool(entry.tool, checked ? Theme::iconOnAccent : Theme::textTertiary, 15));
    }
}

void ViewportToolbar::onToolButtonClicked()
{
    auto* sender = qobject_cast<QToolButton*>(this->sender());
    if (!sender) return;
    // Discoverability redesign: a click activates EXACTLY the tool this
    // button represents -- no more "shown sub-tool" indirection, since
    // every tool now has its own always-visible button.
    const auto t = static_cast<ViewportTool>(sender->property("tool").toInt());
    applyToolSelection(t);
}

void ViewportToolbar::applyToolSelection(ViewportTool t)
{
    m_current = t;
    refreshAllToolButtons();
    emit toolChanged(m_current);
}

// ============================================================
// RISE UI redesign: right-hand status-chip cluster
// ============================================================

void ViewportToolbar::pollState()
{
    updateCameraChip();
    updateRegionChip();
    refreshLayoutButtons();
}

void ViewportToolbar::updateCameraChip()
{
    if (!m_cameraChip) return;
    const QString name = m_bridge
        ? m_bridge->activeNameForCategory(ViewportBridge::Category::Camera)
        : QString();
    QString text = QString::fromUtf8("\xE2\x97\x89 ") + (name.isEmpty() ? tr("No camera") : name);
    // Camera chip lens summary (design brief "Camera chip lens
    // summary"): appended ONLY when honestly available -- see
    // cameraLensSummary's doc for the selection-echo gate.
    const QString lens = cameraLensSummary(name);
    if (!lens.isEmpty()) {
        text += QStringLiteral("   ") + lens;
    }
    m_cameraChip->setText(text);
}

QString ViewportToolbar::cameraLensSummary(const QString& activeCameraName) const
{
    // Lens summary for the active camera, e.g. "50mm · f/4" (ThinLens)
    // or "62° FOV · f/8" (Pinhole -- no focal_length parameter exists on
    // that camera type, so this honestly shows FOV instead of
    // fabricating a focal length).  Returns an empty string (name-only
    // fallback) when there is nothing honest to show.
    //
    // DATA SOURCE CAVEAT (mirrors ContentView.swift's cameraLensSummary):
    // propertySnapshotFor(Camera) is keyed to the Properties panel's OWN
    // camera-category SELECTION (SceneEditController::mSelectionByCategory
    // [Camera]), not directly to the scene's active camera.  Those two are
    // the SAME thing for any camera the user has actually clicked in the
    // outliner/panel -- setSelection(Camera, name) is what calls
    // IScene::SetActiveCamera in the first place -- but immediately after
    // scene load, before any camera selection, the panel's selection can
    // be empty while a camera is nonetheless active by scene default.
    // This function ONLY trusts the snapshot when the panel's current
    // selection name already echoes activeCameraName; otherwise it
    // returns empty rather than either showing stale/wrong data or
    // forcing setSelection as a side effect of a toolbar read (that call
    // is a real scene mutation with its own cancel-and-park
    // serialization -- not something a read-only chip should trigger).
    if (!m_bridge || activeCameraName.isEmpty()) return QString();
    if (m_bridge->selectionCategory() != ViewportBridge::Category::Camera) return QString();
    if (m_bridge->selectionName() != activeCameraName) return QString();

    // propertySnapshotFor's own doc requires a fresh propertySnapshot()
    // call in the same cycle to guarantee RefreshProperties() ran --
    // ViewportProperties::refresh() already does this on every preview
    // frame in practice, but this poll (500ms, independent of render
    // frames) can't rely on that timing, so force it here.
    m_bridge->propertySnapshot();

    QString focalLengthMm, fovDegrees, fstop;
    const QVector<ViewportProperty> props =
        m_bridge->propertySnapshotFor(ViewportBridge::Category::Camera);
    for (const auto& row : props) {
        if (row.name == QLatin1String("focal_length")) focalLengthMm = row.value;
        else if (row.name == QLatin1String("fov"))      fovDegrees = row.value;
        else if (row.name == QLatin1String("fstop"))     fstop = row.value;
    }

    QStringList parts;
    if (!focalLengthMm.isEmpty()) {
        parts << focalLengthMm + QStringLiteral("mm");
    } else if (!fovDegrees.isEmpty()) {
        parts << fovDegrees + QString::fromUtf8("\xC2\xB0 FOV");
    }
    if (!fstop.isEmpty()) {
        parts << QStringLiteral("f/") + fstop;
    }
    return parts.join(QStringLiteral(" \xC2\xB7 "));
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

// ============================================================
// N-up multi-viewport layout picker (docs/gui/RENDER_MODES.md §7.5)
// ============================================================

QToolButton* ViewportToolbar::makeLayoutButton(ViewportBridge::ViewportLayout layout)
{
    auto* btn = new QToolButton(this);
    btn->setObjectName(QStringLiteral("toolBtn"));   // matches the QSS selector installed in the ctor
    btn->setCheckable(true);
    btn->setProperty("layout", static_cast<int>(layout));
    btn->setToolTip(tooltipForLayout(layout));
    btn->setFixedSize(36, 36);
    btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    btn->setFont(Theme::mono(9));
    btn->setText(labelForLayout(layout));
    connect(btn, &QToolButton::clicked, this, &ViewportToolbar::onLayoutButtonClicked);
    return btn;
}

QString ViewportToolbar::labelForLayout(ViewportBridge::ViewportLayout layout) const
{
    // Compact glyphs -- the N-up layout picker intentionally stays
    // text-only (no Lucide glyph reads as "2H" / "1+2" at a glance the
    // way the tool cluster's icons read as their tool), so the label
    // itself is the affordance.
    switch (layout) {
    case ViewportBridge::ViewportLayout::Single:     return QStringLiteral("1");
    case ViewportBridge::ViewportLayout::TwoH:       return QStringLiteral("2H");
    case ViewportBridge::ViewportLayout::OnePlusTwo: return QStringLiteral("1+2");
    case ViewportBridge::ViewportLayout::Quad:       return QStringLiteral("4");
    }
    return QString();
}

QString ViewportToolbar::tooltipForLayout(ViewportBridge::ViewportLayout layout) const
{
    switch (layout) {
    case ViewportBridge::ViewportLayout::Single:
        return tr("Single — one full-size viewport (pane 0)");
    case ViewportBridge::ViewportLayout::TwoH:
        return tr("Two Up — panes 0 | 1 side-by-side");
    case ViewportBridge::ViewportLayout::OnePlusTwo:
        return tr("One + Two — pane 0 large, panes 1/2 stacked at right");
    case ViewportBridge::ViewportLayout::Quad:
        return tr("Quad — panes 0-3 in a 2×2 grid");
    }
    return QString();
}

void ViewportToolbar::refreshLayoutButtons()
{
    const ViewportBridge::ViewportLayout current = m_bridge
        ? m_bridge->viewportLayout()
        : ViewportBridge::ViewportLayout::Single;
    for (const auto& entry : m_layoutButtons) {
        entry.button->setEnabled(m_bridge != nullptr);
        entry.button->setChecked(entry.layout == current);
    }
}

void ViewportToolbar::onLayoutButtonClicked()
{
    if (!m_bridge) return;
    auto* sender = qobject_cast<QToolButton*>(this->sender());
    if (!sender) return;
    const auto layout = static_cast<ViewportBridge::ViewportLayout>(sender->property("layout").toInt());
    // The set CAN fail (render-owns-scene) -- always re-read the effective
    // layout afterward rather than trusting the click (matches TopBar's
    // onRenderModeComboChanged discipline).
    m_bridge->setViewportLayout(layout);
    refreshLayoutButtons();
    emit layoutChanged();
}
