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
#include <QScrollBar>
#include <QFrame>
#include <QMouseEvent>
#include <QPalette>
#include <QMenu>
#include <QAction>
#include <QToolButton>
#include <QFont>
#include <QSize>
#include <QTimer>

#include <functional>
#include <utility>

namespace {

// Tree indentation (arc-86 slice 5).  Level 1 (category headers) uses the
// list layout's own 8px margin; level 2 (entities, and the group rows that
// sit at the same depth) indents to kChildIndent; level 3 (a group's
// members) indents to kMemberIndent, just past the width a group row's
// disclosure control plus the 7px row spacing occupy (30 + 10 + 7 = 47), so
// a member's name starts flush under its group's name.  The literal 48
// matches OutlinerView.swift's `leadingIndent: 48` exactly rather than
// being re-derived here -- the two shells are hand-mirrored, and a 1px
// drift between them is the kind of thing nobody ever reconciles later.
// kDiscloseSize matches both the category header's own chevron QLabel
// (setFixedSize(10, 10)) and the Mac group row's triangle frame width.
constexpr int kChildIndent  = 30;
constexpr int kDiscloseSize = 10;
constexpr int kMemberIndent = 48;

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
    // Palette fill + border-bottom stylesheet set in restyleTheme()
    // (LIVE THEME-SWITCH CONTRACT, Theme.h) -- persistent chrome that
    // rebuild() never revisits.

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(14, 12, 14, 6);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(tr("Scene"), header);
    m_titleLabel->setFont(Theme::sans(12, QFont::DemiBold));
    // Stylesheet set in restyleTheme() -- persistent chrome.
    headerLayout->addWidget(m_titleLabel);

    m_countLabel = new QLabel(header);
    m_countLabel->setFont(Theme::mono(10));
    // Stylesheet set in restyleTheme() -- persistent chrome (only the
    // text is touched by rebuild()).
    headerLayout->addWidget(m_countLabel);
    headerLayout->addStretch(1);

    root->addWidget(header);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setMaximumHeight(305);
    // Stylesheet set in restyleTheme() -- persistent chrome.

    m_listHolder = new QWidget();
    m_listHolder->setAutoFillBackground(true);
    // Palette fill set in restyleTheme() -- persistent chrome.
    m_listLayout = new QVBoxLayout(m_listHolder);
    m_listLayout->setContentsMargins(8, 2, 8, 10);
    m_listLayout->setSpacing(0);

    m_scroll->setWidget(m_listHolder);
    root->addWidget(m_scroll);

    rebuild();

    // LIVE THEME-SWITCH CONTRACT (Theme.h): run once at construction so
    // the persistent chrome above (no longer set inline) actually gets
    // styled -- restyleTheme() is the single source of truth for it.
    m_themeReady = true;
    restyleTheme();
}

// ============================================================
// LIVE THEME-SWITCH CONTRACT (Theme.h)
// ============================================================

void OutlinerWidget::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void OutlinerWidget::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();
    // Persistent chrome only: `this` widget's palette + border-bottom
    // stylesheet, the "Scene" title label, the entity-count label, the
    // scroll area's background, and the list holder's palette -- all
    // set once in the constructor and never revisited by rebuild()
    // (which only touches m_listLayout's CHILDREN, plus m_countLabel's
    // TEXT).
    //
    // Deliberately does NOT call rebuild() SYNCHRONOUSLY here to refresh
    // the category/child rows it builds (tag chips using per-category
    // Theme::cat* tokens, the expand chevron, per-row selection tint,
    // "Add Entity" buttons, etc.). Those rows already read every Theme::
    // token live at build time (every setStyleSheet()/Theme::icon() call
    // inside rebuild() itself) -- rebuild() just needs to run again to
    // pick up new colors, and it already does UNCONDITIONALLY on every
    // single call to refresh(), which rides every ViewportBridge::
    // imageUpdated frame for as long as a scene is loaded (see
    // MainWindow's imageUpdated -> OutlinerWidget::refresh connection)
    // and fires immediately on the next selection/expansion change
    // regardless. Calling rebuild() SYNCHRONOUSLY FROM restyleTheme()
    // would violate the contract's "must not create widgets" rule
    // (rebuild() deleteLater()s and recreates every row) -- the only
    // window where it would matter is a completely idle viewport (no
    // pending frame) mid theme-switch; P2 fix (2026-07-23 review)
    // below QUEUES a rebuild() for exactly that case instead, via
    // QTimer::singleShot(0, ...), landing on the next event-loop turn
    // rather than inside this synchronous palette-change cascade. Both
    // palettes define every
    // Theme::cat* token used by the tag chips (verified in Theme.cpp's
    // DarkPalette()/LightPalette(); catMaterial/catFilm/catVariant are
    // explicit per-palette fields, the rest -- catRender/catCamera/
    // catLight/catObject/catAnimation/catMedia -- are re-derived from
    // other palette-switched tokens in applyThemeTokens()), so the next
    // rebuild() always renders correctly regardless of which mode is
    // active. The expand chevron (bindIconLabel in rebuild() below) is
    // additionally self-updating on its own via bindIconLabel's
    // installed PaletteChange filter (Theme.h), independent of
    // rebuild().
    setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        setPalette(pal);
    }

    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    }
    if (m_countLabel) {
        m_countLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    }
    if (m_scroll) {
        m_scroll->setStyleSheet(QStringLiteral("QScrollArea { background-color: %1; }")
            .arg(Theme::hex(Theme::bgPanel)));
        // Slim themed scrollbars (Task A): applied directly to the
        // QScrollArea's own scrollbar widgets, not the scroll area itself,
        // so the QSS can't leak into any other selector. Bakes token
        // colors, so re-applied on every restyleTheme() call.
        if (QScrollBar* vbar = m_scroll->verticalScrollBar()) {
            vbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
        if (QScrollBar* hbar = m_scroll->horizontalScrollBar()) {
            hbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
    }
    if (m_listHolder) {
        QPalette pal = m_listHolder->palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        m_listHolder->setPalette(pal);
    }

    // Idle-viewport queued rebuild (P2 fix, 2026-07-23 review; blessed by
    // Theme.h's LIVE THEME-SWITCH CONTRACT point 5): rebuild() is skipped
    // above deliberately (see this method's long comment) because a
    // pending render frame or the next selection/expansion change
    // self-heals it -- but if the theme switches while the viewport is
    // completely IDLE (no imageUpdated frame coming, nothing selected),
    // nothing re-triggers refresh()/rebuild() and these rows stay baked
    // with the OLD palette's colors until the next real interaction.
    // QTimer::singleShot(0, ...) defers the rebuild OUT of this
    // synchronous palette-change cascade -- restyleTheme() itself must
    // not create widgets (contract point 2) -- and onto the next event-
    // loop turn, where deleteLater()/recreate is safe.  rebuild(), not
    // refresh(), is the cheapest path: it replays straight from the
    // already-cached m_entitiesByCategory plus a handful of already-in-
    // memory ViewportBridge accessors (isSectionExpanded/
    // activeNameForCategory/selectionCategory/selectionName) -- no
    // categoryEntities()/sceneEpoch() engine round-trip.  Harmless no-op
    // if a real refresh()/rebuild() already ran first (rebuild() is
    // idempotent against the same cached state).
    QTimer::singleShot(0, this, [this]() { rebuild(); });
}

void OutlinerWidget::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_lastEpoch = 0;   // force a fresh entity-list pull on the next refresh()
    m_entitiesByCategory.clear();
    // arc-86 slice 5: a new bridge means a new scene -- drop both group-tree
    // caches so group names from the OLD scene can't leak into the new one's
    // tree (m_membersByGroup) or silently pre-expand a same-named group in it
    // (m_expandedGroups).
    m_membersByGroup.clear();
    m_expandedGroups.clear();
    refresh();
}

void OutlinerWidget::refresh()
{
    if (!m_bridge) {
        m_entitiesByCategory.clear();
        m_membersByGroup.clear();
        rebuild();
        return;
    }

    const unsigned int epoch = m_bridge->sceneEpoch();
    if (epoch != m_lastEpoch || m_entitiesByCategory.isEmpty()) {
        m_lastEpoch = epoch;
        m_entitiesByCategory.clear();
        static const Category cats[] = {
            Category::Rasterizer, Category::Camera, Category::Light, Category::Object,
            Category::Material, Category::Painter, Category::Medium, Category::Geometry,
            // arc-86 slice 5.  Kept in the same slot kCategories in rebuild()
            // puts it (end of the entity block, before the singleton rows);
            // this list only decides WHAT is pulled, not the display order,
            // but keeping the two in step is what stops the next category
            // from being silently omitted here the way Group nearly was.
            Category::Group,
            Category::Film, Category::Animation, Category::SceneVariant,
        };
        for (Category c : cats) {
            m_entitiesByCategory.insert(static_cast<int>(c), m_bridge->categoryEntities(c));
        }

        // arc-86 slice 5 / P2 fix (F1, GUI-fix-round): second level.  Drop
        // the WHOLE cache on an epoch change -- a group's membership can
        // only change alongside a structural edit, so any group that
        // survives the edit still needs a FRESH pull (not just groups that
        // vanished): a stale-but-still-declared entry would otherwise keep
        // serving its PRE-EDIT member list forever.  This clear does not by
        // itself repopulate anything; the unconditional prime loop just
        // below -- which runs on every refresh() call, not only on an epoch
        // change -- does that, one groupMembers() call per declared group.
        // Splitting the drop from the (re)fill is what fixes the actual P2
        // bug: that loop only CACHES a NON-empty pull, so a group whose pull
        // bails right here (mRenderOwnsScene, or a lost mMutex try_lock)
        // is left UNPRIMED instead of caching an empty result behind this
        // epoch stamp -- the old code called groupMembers() and inserted
        // its result unconditionally right in this block, baking in the
        // empty read until the NEXT epoch bump.  Left unprimed, it simply
        // retries on the very next refresh() call (every preview frame),
        // self-healing within one frame instead of freezing at count 0
        // with no disclosure triangle until the next structural edit.
        m_membersByGroup.clear();
        // m_expandedGroups is DELIBERATELY not re-keyed to the group list here
        // (matching OutlinerView.swift, which documents the same choice):
        // the controller serves group data from a snapshot that is skipped
        // while a render owns the scene and falls back to the previous one on
        // lock contention, so this list can legitimately be momentarily stale
        // or short -- pruning against it would silently collapse rows the user
        // opened.  A stale key costs one bool; setBridge() clears the whole
        // hash on scene change, which is the only point it actually matters.
    }

    // arc-86 P2 fix (F1): prime any group not yet cached, EVERY refresh()
    // call -- independent of the epoch gate above.  Only fills in MISSING
    // keys (never overwrites an already-cached, presumed-good entry), and
    // only caches a NON-empty pull -- see the comment above for why an empty
    // pull must never be cached.  This is what lets a group whose pull
    // bailed on the epoch-change frame (setBridge() during a production
    // render, or a contended mMutex) retry on the very next preview frame
    // instead of freezing empty until the next structural edit.
    const QStringList currentGroupNames =
        m_entitiesByCategory.value(static_cast<int>(Category::Group));
    for (const QString& groupName : currentGroupNames) {
        if (m_membersByGroup.contains(groupName)) continue;
        const QStringList members = m_bridge->groupMembers(groupName);
        if (!members.isEmpty()) m_membersByGroup.insert(groupName, members);
    }

    rebuild();
}

void OutlinerWidget::setSceneEditable(bool editable)
{
    if (m_sceneEditable == editable) return;
    m_sceneEditable = editable;
    // Re-derive each row's context-menu enable state immediately rather
    // than waiting for the next epoch-gated refresh() -- a render
    // finishing (or starting) should flip the item's enabled state on
    // the SAME tick, matching PropertiesPanel.swift's live binding.
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

void OutlinerWidget::toggleGroup(const QString& groupName)
{
    // arc-86 slice 5.  Unlike toggleCategory() this makes NO bridge call and
    // emits NO selectionActivated(): the controller has no per-ENTITY
    // expansion concept (isSectionExpanded/collapseSection are per-CATEGORY),
    // and opening a group changes nothing about what is selected -- so the
    // properties panel has nothing to re-read.  Local state + a direct
    // rebuild() is the whole operation; going through refresh() would be a
    // pointless sceneEpoch() round-trip that can only short-circuit anyway.
    if (groupName.isEmpty()) return;
    m_expandedGroups.insert(groupName, !m_expandedGroups.value(groupName, false));
    rebuild();
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
        // Painters feed materials/media -- listed right after Materials,
        // mirroring OutlinerView.swift's kOutlinerCategories.  Theme.h
        // has no dedicated painter token (owned by a parallel
        // workstream); catMaterial's lilac is the closest conceptual
        // match and is the same fallback the Mac slice uses.
        { Category::Painter,      "Painters",        "PNT", Theme::catMaterial },
        { Category::Medium,       "Media",           "MED", Theme::catMedia },
        // Geometry (GUI redesign 2026-07-22): every "*_geometry" chunk,
        // the shapes objects reference by name.  Mirrors the Mac
        // OutlinerView ordering (after Media, before the singleton rows);
        // catObject is the conceptual neighbour, same fallback as Mac.
        { Category::Geometry,     "Geometry",        "GEO", Theme::catObject },
        // Groups (arc-86 slice 5): `group` chunks, each a named set of
        // objects carrying one shared transform, and the ONLY category
        // here whose rows expand to a THIRD level (their member objects).
        // Appended at the end of the entity block -- the same place
        // Geometry was added -- rather than next to Objects, so the
        // established ordering above is undisturbed; matches
        // OutlinerView.swift's kOutlinerCategories exactly.  Theme.h has
        // no dedicated group token (owned by a parallel workstream);
        // catObject's hue is the conceptual neighbour, since a group is a
        // composition OF objects -- the same fallback the Mac slice uses.
        { Category::Group,        "Groups",          "GRP", Theme::catObject },
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

        // Bundled-SVG chevron, replacing the ▾/▸ Unicode triangles -- those
        // fall back through IBM Plex -> Segoe UI Symbol on Windows and
        // render inconsistently (mirrors OutlinerView.swift's expand
        // indicator, which stays a plain SF-adjacent glyph on Mac since
        // that platform doesn't hit the fallback problem). Deliberate
        // Windows-side exception: Mac renders ▾/▸ as text, Windows uses
        // the chevron SVGs here for cross-panel consistency with the
        // chat/log disclosure chevrons (approved by the orchestrating
        // session, 2026-07-23).
        auto* arrow = new QLabel(headerRow);
        arrow->setFixedSize(10, 10);
        arrow->setAlignment(Qt::AlignCenter);
        // Theme::bindIconLabel keeps this pixmap correct across a mixed-DPI
        // monitor drag (plain setPixmap(iconPixmap(..., devicePixelRatioF()))
        // captures DPR once at construction and goes stale).
        Theme::bindIconLabel(arrow,
            (expanded && !children.isEmpty()) ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
            9, []{ return Theme::textDim; });
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

        // Entity-creation slice: per-category "Add Entity" affordance,
        // mirrors OutlinerView.swift's "+" Menu.  Templates are queried
        // live (cheap C-API round-trips) so the button disappears
        // automatically for categories with none registered (Camera/
        // Rasterizer/Film/Animation/SceneVariant), and is disabled
        // (not hidden) while the scene isn't editable so its presence
        // doesn't flicker during a render.
        const unsigned int templateCount = m_bridge ? m_bridge->entityTemplateCount(def.category) : 0;
        if (templateCount > 0) {
            auto* addBtn = new QToolButton(headerRow);
            // Bundled-SVG "circle-plus" icon, replacing the "+" text glyph
            // -- mirrors OutlinerView.swift:184's category-header add menu
            // (`Image(systemName: "plus.circle")`).  Qt auto-swaps to the
            // Disabled-mode tint (registered via the icon() overload below)
            // when the button's enabled state flips off, so the QSS
            // :disabled color rule below stays in effect via the icon
            // rather than text color.  The third (Active) tint restores
            // the textPrimary hover accent the old QSS `:hover { color:
            // ... }` rule provided before this button became icon-only --
            // autoRaise + State_MouseOver maps to QIcon::Active via
            // QCommonStyle::CE_ToolButtonLabel.
            addBtn->setIcon(Theme::icon("circle-plus", 11, Theme::textDim, Theme::textDisabled, Theme::textPrimary));
            addBtn->setIconSize(QSize(11, 11));
            addBtn->setAutoRaise(true);
            addBtn->setCursor(Qt::PointingHandCursor);
            addBtn->setPopupMode(QToolButton::InstantPopup);
            addBtn->setEnabled(m_sceneEditable);
            addBtn->setFixedSize(16, 16);
            addBtn->setStyleSheet(QStringLiteral(
                "QToolButton { border: none; padding: 0; }"
                "QToolButton::menu-indicator { image: none; }"));
            const QString singular = QString::fromUtf8(def.title).endsWith(QLatin1Char('s'))
                ? QString::fromUtf8(def.title).chopped(1) : QString::fromUtf8(def.title);
            addBtn->setToolTip(tr("Add %1").arg(singular));

            auto* addMenu = new QMenu(addBtn);
            for (unsigned int i = 0; i < templateCount; ++i) {
                const QString label = m_bridge->entityTemplateLabel(def.category, i);
                QAction* templateAction = addMenu->addAction(label);
                connect(templateAction, &QAction::triggered, this,
                        [this, cat = def.category, i]() { emit addEntityRequested(cat, i); });
            }
            addBtn->setMenu(addMenu);
            headerLayout->addWidget(addBtn);
        }

        auto* count = new QLabel(QString::number(children.size()), headerRow);
        count->setFont(Theme::mono(10));
        count->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDisabled)));
        headerLayout->addWidget(count);

        m_listLayout->addWidget(headerRow);

        // arc-86 slice 5: the Group category is the only THREE-level branch --
        // group rows, and under an expanded group its member rows.  Every
        // other category keeps the flat two-level shape below, unchanged.
        if (expanded && def.category == Category::Group) {
            for (const QString& groupName : children) {
                const QStringList members = m_membersByGroup.value(groupName);
                const bool groupExpanded = m_expandedGroups.value(groupName, false);
                const bool groupSelected = (selCat == Category::Group && selName == groupName);

                // static_cast: QStringList::size() is qsizetype (64-bit in
                // Qt 6) and buildGroupRow takes an int -- narrowing it here
                // explicitly rather than letting the call site do it
                // implicitly, so no MSVC C4267 shows up in a clean rebuild.
                m_listLayout->addWidget(
                    buildGroupRow(groupName, static_cast<int>(members.size()),
                                  groupExpanded, groupSelected));

                if (!groupExpanded) continue;

                for (const QString& memberName : members) {
                    // A member is an ORDINARY object: selected as
                    // Category::Object (never Group), so the existing object
                    // properties panel and the viewport gizmo act on it with
                    // no special-casing, and its selection highlight stays in
                    // sync with the SAME object's row in the flat Objects
                    // list (both rows tint when either is picked -- correct,
                    // they are one entity shown twice).
                    const bool memberSelected =
                        (selCat == Category::Object && selName == memberName);
                    // Object has no scene-level "active entity" concept
                    // (activeNameForCategory is documented to return empty
                    // for it), so no member row can ever carry the ACTIVE
                    // badge -- passed as a literal false rather than
                    // recomputing an always-empty comparison per member.
                    //
                    // Reveal and Duplicate are offered with EXACTLY the gate
                    // the flat Objects list uses (Object is neither Rasterizer
                    // nor Film, so the flag collapses to m_sceneEditable).
                    //
                    // DELETE IS DISABLED HERE, and only here.  Removing a
                    // standard_object chunk that a `group` chunk still names as
                    // a `member` cannot succeed: ApplyCstRemoveChunk's dry-run
                    // re-derive fails on the group's unresolvable member, so the
                    // core refuses.  Offering the item would produce a
                    // guaranteed confirm-dialog-then-failure-dialog pair every
                    // time.  It stays ENABLED on the SAME object's row in the
                    // flat Objects list -- not an inconsistency to fix by
                    // re-enabling it here, but the honest state of the two
                    // views: from the flat list the object's group membership
                    // is not visible, whereas this row exists BECAUSE of that
                    // membership, so this is the one place the widget can
                    // explain the refusal instead of letting the core produce
                    // it.  The tooltip carries the explanation.
                    m_listLayout->addWidget(
                        buildEntityRow(Category::Object, memberName, kMemberIndent,
                                       memberSelected, false,
                                       m_sceneEditable, m_sceneEditable,
                                       /*canDelete*/ false,
                                       tr("Remove this object from the `group` chunk's "
                                          "`member` list first -- a still-referenced member "
                                          "cannot be deleted.")));
                }
            }
        } else if (expanded) {
            for (const QString& childName : children) {
                const bool isSelected = (selCat == def.category && selName == childName);
                const bool isActive = !activeName.isEmpty() && activeName == childName;

                // "Reveal in scene file" context-menu item (item 3):
                // Rasterizer/Film rows have no chunk address (registry/
                // preset names, not chunk names) -- disabled rather than
                // a silent no-op, mirroring OutlinerView.swift's review-
                // P3 fix.  Resolvability of a particular name/category
                // (does it actually HAVE a chunk location) isn't pre-
                // checked here -- an unresolvable target just no-ops on
                // the MainWindow side, same as the properties-panel chip
                // when hidden mid-render.
                const bool canReveal = m_sceneEditable
                    && def.category != Category::Rasterizer
                    && def.category != Category::Film;
                // Entity-creation slice: Duplicate/Delete need the SAME
                // chunk-name addressing "Reveal in scene file" needs (no
                // Rasterizer/Film) plus the scene-editable gate -- kept as
                // a separate flag (== canReveal today) rather than reused
                // directly, mirroring OutlinerChildRow.swift's canReveal/
                // canMutate split so a future divergence between the two
                // gates doesn't require touching every call site.
                const bool canMutate = canReveal;

                m_listLayout->addWidget(
                    buildEntityRow(def.category, childName, kChildIndent,
                                   isSelected, isActive, canReveal, canMutate));
            }
        }
    }

    m_countLabel->setText(tr("%1 entities").arg(totalEntities));
}

// ============================================================
// Row builders (arc-86 slice 5)
// ============================================================
//
// Factored out of rebuild() so a group's MEMBER rows are literally the
// same row as a flat category child -- mirroring OutlinerView.swift,
// which reuses `OutlinerChildRow` for members instead of growing a
// second row type.  Both builders parent to m_listHolder (like every
// other row rebuild() creates) and are pure constructors: they read no
// state rebuild() hasn't already resolved, so rebuild() stays the single
// place that decides WHAT is on screen.

QWidget* OutlinerWidget::buildEntityRow(Category selectCat, const QString& name, int leftIndent,
                                        bool isSelected, bool isActive,
                                        bool canReveal, bool canMutate,
                                        bool canDelete, const QString& deleteDisabledTip)
{
    auto* childRow = new ClickableRow(
        [this, selectCat, name]() { selectChild(selectCat, name); }, m_listHolder);
    childRow->setStyleSheet(QStringLiteral(
        "background-color: %1; border-radius: %2px;")
        .arg(isSelected
            ? Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(),
                                  Theme::accent.blue(), static_cast<int>(0.14 * 255)))
            : QStringLiteral("transparent"))
        .arg(Theme::radiusSmall));
    auto* childLayout = new QHBoxLayout(childRow);
    childLayout->setContentsMargins(leftIndent, 4, 8, 4);
    childLayout->setSpacing(7);

    auto* childLabel = new QLabel(name, childRow);
    childLabel->setFont(Theme::sans(11));
    // Selected text is Theme::textPrimary, not the Mac's literal
    // `.white` (OutlinerView.swift:296): the selected fill is only
    // a 14%-alpha accent tint over bgPanel, so pure white is
    // unreadable on the Light palette's near-white bgPanel.
    // textPrimary is near-white in Dark (matches the Mac look)
    // and flips to near-black in Light.
    childLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(isSelected ? Theme::textPrimary : Theme::textMuted)));
    childLabel->setToolTip(name);
    childLayout->addWidget(childLabel);
    childLayout->addStretch(1);

    if (isActive) {
        auto* activeBadge = new QLabel(tr("ACTIVE"), childRow);
        activeBadge->setFont(Theme::mono(9));
        activeBadge->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::accentLight)));
        childLayout->addWidget(activeBadge);
    }

    childRow->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(childRow, &QWidget::customContextMenuRequested, this,
            [this, childRow, selectCat, name, canReveal, canMutate,
             canDelete, deleteDisabledTip](const QPoint& pos) {
        QMenu menu(childRow);
        // Action tooltips are OFF by default in QMenu; the disabled-Delete
        // case is the only reason this row's menu has one, and a disabled
        // item that cannot say WHY is exactly the dead control this fix
        // exists to remove.
        menu.setToolTipsVisible(true);
        QAction* revealAction = menu.addAction(tr("Reveal in scene file"));
        revealAction->setEnabled(canReveal);
        menu.addSeparator();
        QAction* duplicateAction = menu.addAction(tr("Duplicate"));
        duplicateAction->setEnabled(canMutate);
        QAction* deleteAction = menu.addAction(tr("Delete"));
        deleteAction->setEnabled(canMutate && canDelete);
        if (!canDelete && !deleteDisabledTip.isEmpty()) deleteAction->setToolTip(deleteDisabledTip);
        QAction* triggered = menu.exec(childRow->mapToGlobal(pos));
        if (triggered == revealAction) {
            emit revealRequested(selectCat, name);
        } else if (triggered == duplicateAction) {
            emit duplicateRequested(selectCat, name);
        } else if (triggered == deleteAction) {
            emit deleteRequested(selectCat, name);
        }
    });

    return childRow;
}

QWidget* OutlinerWidget::buildGroupRow(const QString& groupName, int memberCount,
                                       bool expanded, bool isSelected)
{
    // Selection behaves exactly like any other entity row: a click anywhere
    // on the row that is NOT the disclosure control selects the group as
    // Category::Group, which is what drives the group's own
    // position/orientation/scale panel.
    auto* row = new ClickableRow(
        [this, groupName]() { selectChild(Category::Group, groupName); }, m_listHolder);
    row->setStyleSheet(QStringLiteral(
        "background-color: %1; border-radius: %2px;")
        .arg(isSelected
            ? Theme::rgba(QColor(Theme::accent.red(), Theme::accent.green(),
                                  Theme::accent.blue(), static_cast<int>(0.14 * 255)))
            : QStringLiteral("transparent"))
        .arg(Theme::radiusSmall));
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(kChildIndent, 4, 8, 4);
    rowLayout->setSpacing(7);

    // Disclosure control.  A QToolButton (not a QLabel like the category
    // header's chevron) precisely BECAUSE it accepts mouse events: it
    // consumes the click instead of letting it fall through to the
    // ClickableRow underneath, which is what keeps "open the group" and
    // "select the group" as two separate gestures on one row.  Same
    // icon-only autoRaise recipe as the category header's "+" button above.
    if (memberCount > 0) {
        auto* disclose = new QToolButton(row);
        disclose->setIcon(Theme::icon(
            expanded ? QStringLiteral("chevron-down") : QStringLiteral("chevron-right"),
            9, Theme::textDim, Theme::textDisabled, Theme::textPrimary));
        disclose->setIconSize(QSize(9, 9));
        disclose->setAutoRaise(true);
        disclose->setCursor(Qt::PointingHandCursor);
        disclose->setFixedSize(kDiscloseSize, kDiscloseSize);
        disclose->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: 0; }"));
        disclose->setToolTip(expanded ? tr("Collapse group") : tr("Expand group"));
        connect(disclose, &QToolButton::clicked, this,
                [this, groupName]() { toggleGroup(groupName); });
        rowLayout->addWidget(disclose);
    } else {
        // Empty group: no control, but the same width reserved so its name
        // stays column-aligned with the groups that do have one.
        auto* spacer = new QWidget(row);
        spacer->setFixedSize(kDiscloseSize, kDiscloseSize);
        rowLayout->addWidget(spacer);
    }

    auto* label = new QLabel(groupName, row);
    label->setFont(Theme::sans(11));
    label->setStyleSheet(QStringLiteral("color: %1;")
        .arg(Theme::hex(isSelected ? Theme::textPrimary : Theme::textMuted)));
    label->setToolTip(groupName);
    rowLayout->addWidget(label);
    rowLayout->addStretch(1);

    // Member count, styled like the category headers' count for the same
    // reason: a group row is a container row, not a leaf.
    auto* count = new QLabel(QString::number(memberCount), row);
    count->setFont(Theme::mono(10));
    count->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDisabled)));
    rowLayout->addWidget(count);

    // Context menu: "Reveal in scene file" ONLY.  Duplicate and Delete are
    // DELIBERATELY omitted (not merely disabled) -- same decision as
    // OutlinerView.swift's groupRow, and for two concrete reasons on top of
    // its "a control that may half-work is worse than none":
    //
    //  * Duplicate copies the chunk bytes verbatim and only rewrites its
    //    `name` (SceneEditController::DuplicateEntity), so the copy would
    //    list the SAME `member` objects.  Both groups then push their matrix
    //    onto those objects at derive time (G_dup x G_orig x M) and every
    //    member silently double-transforms.  A correct group duplicate has
    //    to deep-copy the members too -- a core-side verb this slice lacks.
    //  * Delete removes the `group` chunk, which does NOT delete anything
    //    the user can see: the members survive as ordinary objects and
    //    merely lose the group's transform, i.e. they all JUMP to their
    //    authored positions.  That is an "Ungroup" verb wearing a "Delete"
    //    label.  When an Ungroup verb exists it belongs here under its own
    //    name.
    //
    // Group CRUD stays with the scene text this slice.  NB the two routes
    // are not blocked at the core -- Stage A wired Category::Group into
    // RoleKindSuffixForCategory, so DuplicateEntity/RemoveEntity WOULD
    // apply if a shell called them; this widget simply does not offer them.
    row->setContextMenuPolicy(Qt::CustomContextMenu);
    const bool canReveal = m_sceneEditable;
    connect(row, &QWidget::customContextMenuRequested, this,
            [this, row, groupName, canReveal](const QPoint& pos) {
        QMenu menu(row);
        QAction* revealAction = menu.addAction(tr("Reveal in scene file"));
        revealAction->setEnabled(canReveal);
        QAction* triggered = menu.exec(row->mapToGlobal(pos));
        if (triggered == revealAction) {
            emit revealRequested(Category::Group, groupName);
        }
    });

    return row;
}
