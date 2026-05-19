//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.cpp - Photoshop-style category-slot toolbar.
//
//////////////////////////////////////////////////////////////////////

#include "ViewportToolbar.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QStyle>

ViewportToolbar::ViewportToolbar(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);

    // Accent-coloured fill + white icon when the slot's category is
    // active — mirrors macOS SlotIcon's `isSelected` highlight.  Per-
    // category visibility of the highlight is driven via `setChecked`.
    setStyleSheet(
        "QToolButton {"
        "  border: 1px solid transparent;"
        "  border-radius: 4px;"
        "  padding: 2px;"
        "}"
        "QToolButton:hover { background: palette(midlight); }"
        "QToolButton:checked {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "  border: 1px solid palette(highlight);"
        "}"
    );

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
        layout->addWidget(s.button);
        m_slots.append(s);
    }

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sep);

    auto* undoBtn = new QToolButton(this);
    undoBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    undoBtn->setToolTip(tr("Undo — revert the last edit (per-drag composites are one entry)"));
    connect(undoBtn, &QToolButton::clicked, this, &ViewportToolbar::undoClicked);
    layout->addWidget(undoBtn);

    auto* redoBtn = new QToolButton(this);
    redoBtn->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    redoBtn->setToolTip(tr("Redo — re-apply the most recently undone edit"));
    connect(redoBtn, &QToolButton::clicked, this, &ViewportToolbar::redoClicked);
    layout->addWidget(redoBtn);

    layout->addStretch(1);

    refreshAllSlots();
}

void ViewportToolbar::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    refreshAllSlots();
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
