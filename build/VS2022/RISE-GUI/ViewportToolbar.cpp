//////////////////////////////////////////////////////////////////////
//
//  ViewportToolbar.cpp
//
//////////////////////////////////////////////////////////////////////

#include "ViewportToolbar.h"

#include <QHBoxLayout>
#include <QFrame>
#include <QStyle>

ViewportToolbar::ViewportToolbar(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);

    // Prominent visual state for the selected tool.  The default
    // QToolButton "checked" rendering is too subtle on Windows; force
    // an accent-coloured fill + white icon so the active tool is
    // unambiguous at a glance.  Applied via stylesheet to all child
    // QToolButtons inside this widget.
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

    // Tooltip text mirrors macOS ViewportToolbar.swift verbatim so the
    // user-facing copy is identical across platforms.
    auto add = [&](ViewportTool t, const QString& iconName,
                   const QString& label, const QString& tip) {
        auto* btn = makeToolButton(t, iconName, label, tip);
        layout->addWidget(btn);
        m_toolButtons.append(btn);
    };

    // Object Translate / Rotate / Scale and standalone Scrub are
    // intentionally omitted from the toolbar.  Object editing is too
    // much complexity for the current state of the app, and timeline
    // scrubbing is driven directly by the bottom timeline bar.  The
    // C++ enum still has all eight values so the controller is
    // forward-compatible if those tools come back later.
    add(ViewportTool::Select,          "edit-select",
        "Select",
        tr("Select — click an object in the viewport to make it the target of the next edit"));
    add(ViewportTool::OrbitCamera,     "view-rotate",
        "Orbit",
        tr("Orbit Camera — drag to rotate the camera around the scene"));
    add(ViewportTool::PanCamera,       "transform-move-horizontal",
        "Pan",
        tr("Pan Camera — drag to translate the camera in screen plane"));
    add(ViewportTool::ZoomCamera,      "zoom-in",
        "Zoom",
        tr("Zoom Camera — drag to dolly the camera closer or farther"));
    add(ViewportTool::RollCamera,      "object-rotate-right",
        "Roll",
        tr("Roll Camera — drag horizontally to roll the camera around the (camera→look-at) axis"));

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

    // Visually mark the initial selection.
    if (!m_toolButtons.isEmpty()) m_toolButtons[0]->setChecked(true);
}

QToolButton* ViewportToolbar::makeToolButton(ViewportTool t,
                                             const QString& iconName,
                                             const QString& label,
                                             const QString& tooltip)
{
    auto* btn = new QToolButton(this);
    // Try a themed icon (X11/KDE), fall back to plain text label —
    // works fine on Windows where icon themes aren't always present.
    QIcon icon = QIcon::fromTheme(iconName);
    if (!icon.isNull()) {
        btn->setIcon(icon);
    } else {
        btn->setText(label.left(2));
    }
    btn->setToolTip(tooltip);
    btn->setCheckable(true);
    btn->setProperty("toolValue", static_cast<int>(t));
    connect(btn, &QToolButton::clicked, this, &ViewportToolbar::onToolButtonClicked);
    return btn;
}

void ViewportToolbar::onToolButtonClicked()
{
    auto* sender = qobject_cast<QToolButton*>(this->sender());
    if (!sender) return;
    const int v = sender->property("toolValue").toInt();
    m_current = static_cast<ViewportTool>(v);
    for (auto* b : m_toolButtons) {
        b->setChecked(b == sender);
    }
    emit toolChanged(m_current);
}
