//////////////////////////////////////////////////////////////////////
//
//  OutlinerWidget.cpp - Category tree implementation.  See header for
//    the macOS OutlinerView.swift cross-reference.
//
//////////////////////////////////////////////////////////////////////

#include "OutlinerWidget.h"
#include "ViewportBridge.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QFrame>
#include <QMouseEvent>
#include <QPalette>

#include <functional>
#include <utility>

namespace {

// A row that reports a plain left-click via a std::function callback.
// No Q_OBJECT / signals -- mirrors ViewportProperties.cpp's ScrubHandle
// and TopBar.cpp's TopBarLogoSwatch, which document why a pure-input
// helper widget local to one .cpp doesn't need moc registration.
class ClickableRow : public QWidget
{
public:
    using ClickFn = std::function<void()>;

    explicit ClickableRow(ClickFn onClick, QWidget* parent = nullptr)
        : QWidget(parent)
        , m_onClick(std::move(onClick))
    {
        setCursor(Qt::PointingHandCursor);
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

} // namespace

OutlinerWidget::OutlinerWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        setPalette(pal);
    }
    setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 12, 14, 6);
    headerLayout->setSpacing(8);

    auto* title = new QLabel(tr("Scene"), header);
    title->setFont(Theme::sans(12, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    headerLayout->addWidget(title);

    m_countLabel = new QLabel(header);
    m_countLabel->setFont(Theme::mono(10));
    m_countLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    headerLayout->addWidget(m_countLabel);
    headerLayout->addStretch(1);

    root->addWidget(header);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setMaximumHeight(305);
    m_scroll->setStyleSheet(QStringLiteral("QScrollArea { background-color: %1; }")
        .arg(Theme::hex(Theme::bgPanel)));

    m_listHolder = new QWidget();
    {
        QPalette pal = m_listHolder->palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        m_listHolder->setAutoFillBackground(true);
        m_listHolder->setPalette(pal);
    }
    m_listLayout = new QVBoxLayout(m_listHolder);
    m_listLayout->setContentsMargins(8, 2, 8, 10);
    m_listLayout->setSpacing(0);

    m_scroll->setWidget(m_listHolder);
    root->addWidget(m_scroll);

    rebuild();
}

void OutlinerWidget::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_lastEpoch = 0;   // force a fresh entity-list pull on the next refresh()
    m_entitiesByCategory.clear();
    refresh();
}

void OutlinerWidget::refresh()
{
    if (!m_bridge) {
        m_entitiesByCategory.clear();
        rebuild();
        return;
    }

    const unsigned int epoch = m_bridge->sceneEpoch();
    if (epoch != m_lastEpoch || m_entitiesByCategory.isEmpty()) {
        m_lastEpoch = epoch;
        m_entitiesByCategory.clear();
        static const Category cats[] = {
            Category::Rasterizer, Category::Camera, Category::Light, Category::Object,
            Category::Material, Category::Medium, Category::Film,
            Category::Animation, Category::SceneVariant,
        };
        for (Category c : cats) {
            m_entitiesByCategory.insert(static_cast<int>(c), m_bridge->categoryEntities(c));
        }
    }

    rebuild();
}

void OutlinerWidget::toggleCategory(const CategoryDef& def)
{
    if (!m_bridge) return;
    const bool expanded = m_bridge->isSectionExpanded(def.category);
    if (expanded) {
        m_bridge->collapseSection(def.category);
    } else {
        // Empty-name selection opens the section without picking a row --
        // same mechanism the pre-redesign accordion used.
        m_bridge->setSelection(def.category, QString());
    }
    emit selectionActivated();
    refresh();
}

void OutlinerWidget::selectChild(Category cat, const QString& name)
{
    if (!m_bridge) return;
    m_bridge->setSelection(cat, name);
    emit selectionActivated();
    refresh();
}

void OutlinerWidget::rebuild()
{
    // Full rebuild on every call -- mirrors OutlinerView.swift's
    // declarative reload(); the tree is small (<= 9 categories + a
    // scene's entity counts), so this is cheap relative to a render
    // frame and avoids persistent-widget/bridge-state sync bugs.
    QLayoutItem* item;
    while ((item = m_listLayout->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    // Order mirrors the pre-redesign accordion's grouping (render
    // surface first, then the entities that feed it) -- matches
    // OutlinerView.swift's kOutlinerCategories, an intentional
    // continuity choice over the design comp's cosmetic order.
    static const CategoryDef kCategories[] = {
        { Category::Rasterizer,   "Rasterizer",      "RND", Theme::catRender },
        { Category::Camera,       "Cameras",         "CAM", Theme::catCamera },
        { Category::Light,        "Lights",          "LGT", Theme::catLight },
        { Category::Object,       "Objects",         "OBJ", Theme::catObject },
        { Category::Material,     "Materials",       "MAT", Theme::catMaterial },
        { Category::Medium,       "Media",           "MED", Theme::catMedia },
        { Category::Film,         "Output Settings", "FLM", Theme::catFilm },
        { Category::Animation,    "Animation",       "ANM", Theme::catAnimation },
        { Category::SceneVariant, "Variants",         "VAR", Theme::catVariant },
    };

    int totalEntities = 0;
    for (const CategoryDef& def : kCategories) {
        const QStringList children = m_entitiesByCategory.value(static_cast<int>(def.category));
        totalEntities += children.size();

        const bool expanded = m_bridge && m_bridge->isSectionExpanded(def.category);
        const QString activeName = m_bridge ? m_bridge->activeNameForCategory(def.category) : QString();
        const Category selCat = m_bridge ? m_bridge->selectionCategory() : Category::None;
        const QString selName = m_bridge ? m_bridge->selectionName() : QString();

        auto* headerRow = new ClickableRow([this, def]() { toggleCategory(def); }, m_listHolder);
        auto* headerLayout = new QHBoxLayout(headerRow);
        headerLayout->setContentsMargins(8, 4, 8, 4);
        headerLayout->setSpacing(7);

        auto* arrow = new QLabel((expanded && !children.isEmpty())
            ? QString::fromUtf8("\xE2\x96\xBE")    // ▾
            : QString::fromUtf8("\xE2\x96\xB8"),   // ▸
            headerRow);
        arrow->setFont(Theme::sans(8));
        arrow->setFixedWidth(10);
        arrow->setAlignment(Qt::AlignCenter);
        arrow->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        headerLayout->addWidget(arrow);

        auto* tag = new QLabel(QString::fromUtf8(def.tag), headerRow);
        tag->setFont(Theme::mono(9));
        tag->setFixedWidth(26);
        tag->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(def.tagColor)));
        headerLayout->addWidget(tag);

        auto* name = new QLabel(QString::fromUtf8(def.title), headerRow);
        name->setFont(Theme::sans(11));
        name->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textTertiary)));
        headerLayout->addWidget(name);
        headerLayout->addStretch(1);

        auto* count = new QLabel(QString::number(children.size()), headerRow);
        count->setFont(Theme::mono(10));
        count->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDisabled)));
        headerLayout->addWidget(count);

        m_listLayout->addWidget(headerRow);

        if (expanded) {
            for (const QString& childName : children) {
                const bool isSelected = (selCat == def.category && selName == childName);
                const bool isActive = !activeName.isEmpty() && activeName == childName;

                auto* childRow = new ClickableRow(
                    [this, cat = def.category, childName]() { selectChild(cat, childName); }, m_listHolder);
                childRow->setStyleSheet(QStringLiteral(
                    "background-color: %1; border-radius: %2px;")
                    .arg(isSelected
                        ? Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(),
                                              Theme::accent.blue(), static_cast<int>(0.14 * 255)))
                        : QStringLiteral("transparent"))
                    .arg(Theme::radiusSmall));
                auto* childLayout = new QHBoxLayout(childRow);
                childLayout->setContentsMargins(30, 4, 8, 4);
                childLayout->setSpacing(7);

                auto* childLabel = new QLabel(childName, childRow);
                childLabel->setFont(Theme::sans(11));
                childLabel->setStyleSheet(QStringLiteral("color: %1;")
                    .arg(isSelected ? QStringLiteral("#ffffff") : Theme::hex(Theme::textMuted)));
                childLabel->setToolTip(childName);
                childLayout->addWidget(childLabel);
                childLayout->addStretch(1);

                if (isActive) {
                    auto* activeBadge = new QLabel(tr("ACTIVE"), childRow);
                    activeBadge->setFont(Theme::mono(9));
                    activeBadge->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::accentLight)));
                    childLayout->addWidget(activeBadge);
                }

                m_listLayout->addWidget(childRow);
            }
        }
    }

    m_countLabel->setText(tr("%1 entities").arg(totalEntities));
}
