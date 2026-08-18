//////////////////////////////////////////////////////////////////////
//
//  OutlinerWidget.cpp - 87 section 5 step 4c: the outliner as a REAL
//    TREE MODEL over the authored graph.  See the header for the macOS
//    OutlinerView.swift (step 4b) cross-reference.
//
//  Three collaborators, all local to this file except the model (which
//  the widget holds):
//
//    OutlinerWidget::TreeModel   a QAbstractItemModel whose ONE flat row
//                                table holds the eleven category headers
//                                at level 0 and each category's authored
//                                tree below them.
//    OutlinerRowDelegate         paints a row.  Reads every Theme:: token
//                                LIVE on each paint, which is what makes
//                                a theme switch a repaint rather than a
//                                widget rebuild.
//    OutlinerTreeView            owns hit-testing and routes clicks; the
//                                view NEVER expands or selects on its
//                                own, so this widget stays the single
//                                writer of both kinds of expansion state.
//
//  WHY A MODEL AND NOT A LIST OF WIDGETS.  The pre-4c version tore down
//  and recreated every row QWidget on every call to rebuild(), which
//  rides every preview frame.  A tree makes that worse (a row per node at
//  every depth) and the two-level structure could not express depth at
//  all.  The model resets only when the row table's SHAPE changes, so a
//  preview frame is now a repaint; scroll position and disclosure state
//  survive it.
//
//////////////////////////////////////////////////////////////////////

#include "OutlinerWidget.h"
#include "ViewportBridge.h"
#include "Theme.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAction>
#include <QColor>
#include <QCursor>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QScrollBar>
#include <QSize>
#include <QStyleOption>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>

#include <functional>
#include <utility>

namespace {

// ============================================================
// Category table
// ============================================================

struct CategoryDef {
    ViewportBridge::Category category;
    const char*              title;
    const char*              tag;
    // POINTS AT the live Theme:: token rather than copying it.  The
    // Theme:: category colors are plain globals that applyThemeTokens()
    // REASSIGNS IN PLACE on a light/dark switch (Theme.h), so a QColor
    // copied into a table that is initialised once would render the old
    // palette's tag chips forever after the first switch -- which is
    // exactly what the pre-4c `static const CategoryDef kCategories[]`
    // local inside rebuild() did.  Dereferenced at PAINT time instead.
    QColor*                  tagColor;
};

// Order mirrors the pre-redesign accordion's grouping (render surface
// first, then the entities that feed it) -- matches OutlinerView.swift's
// kOutlinerCategories, an intentional continuity choice over the design
// comp's cosmetic order.
//
// Constant-initialised: every element is a constant expression (an enum
// value, two string literals and the address of a namespace-scope
// object), so there is no static-initialisation-order hazard against the
// Theme:: token globals this points into.
const CategoryDef kCategories[] = {
    { ViewportBridge::Category::Rasterizer,   "Rasterizer",      "RND", &Theme::catRender },
    { ViewportBridge::Category::Camera,       "Cameras",         "CAM", &Theme::catCamera },
    { ViewportBridge::Category::Light,        "Lights",          "LGT", &Theme::catLight },
    { ViewportBridge::Category::Object,       "Objects",         "OBJ", &Theme::catObject },
    { ViewportBridge::Category::Material,     "Materials",       "MAT", &Theme::catMaterial },
    // Painters feed materials/media -- listed right after Materials,
    // mirroring OutlinerView.swift's kOutlinerCategories.  Theme.h has no
    // dedicated painter token (owned by a parallel workstream);
    // catMaterial's lilac is the closest conceptual match and is the same
    // fallback the Mac slice uses.
    { ViewportBridge::Category::Painter,      "Painters",        "PNT", &Theme::catMaterial },
    { ViewportBridge::Category::Medium,       "Media",           "MED", &Theme::catMedia },
    // Geometry (GUI redesign 2026-07-22): every "*_geometry" chunk, the
    // shapes objects reference by name.  Mirrors the Mac OutlinerView
    // ordering (after Media, before the singleton rows); catObject is the
    // conceptual neighbour, same fallback as Mac.
    { ViewportBridge::Category::Geometry,     "Geometry",        "GEO", &Theme::catObject },
    { ViewportBridge::Category::Film,         "Output Settings", "FLM", &Theme::catFilm },
    { ViewportBridge::Category::Animation,    "Animation",       "ANM", &Theme::catAnimation },
    { ViewportBridge::Category::SceneVariant, "Variants",        "VAR", &Theme::catVariant },
};

constexpr int kCategoryCount = static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));

// ============================================================
// Model roles
// ============================================================
//
// Everything the delegate paints and everything the view hit-tests comes
// through these, so neither of them needs to see TreeModel's definition
// -- and neither can reach past the model into the controller.

enum OutlinerRole {
    RoleIsCategory = Qt::UserRole + 1,  //!< bool: a level-0 category header
    RoleTreeDepth,                      //!< int: 0 for a category's ROOT entity
    RoleTag,                            //!< QString: the 3-letter chip
    RoleTagColor,                       //!< QColor: chip tint, read live
    RoleCategory,                       //!< int: ViewportBridge::Category
    RoleName,                           //!< QString: entity name (selection identity)
    RolePathKey,                        //!< QString: per-node expand-state key
    RoleCountText,                      //!< QString: header count, "" on entity rows
    RoleHasAddButton,                   //!< bool: category has >= 1 template
    RoleAddTooltip,                     //!< QString: "Add <singular>"
    RoleSceneEditable,                  //!< bool: MainWindow's bridgeInteractingEnabled
    RoleIsSelected,                     //!< bool: matches the bridge's PRIMARY selection
    RoleIsActive,                       //!< bool: the category's scene-ACTIVE entity
};

// ============================================================
// Row geometry
// ============================================================
//
// ONE source of truth for the two things that are both painted and
// clicked (the disclosure glyph and the "+" button), so the delegate and
// the view's hit-testing can never disagree about where they are.
//
// The constants reproduce the pre-4c widget layout, which was:
//   m_listHolder contentsMargins(8, 2, 8, 10), m_listLayout spacing 0
//   header row   contentsMargins(8, 4, 8, 4), spacing 7,
//                arrow(10) tag(26) title ... [+ (16)] count
//   child row    contentsMargins(30, 4, 8, 4), spacing 7,
//                label ... [ACTIVE]
// Only the holder's HORIZONTAL 8px is a constant here (kHolderMarginX,
// folded into contentRectOf() because the selection tint has to span it).
// The holder's 2px/10px vertical margins are not row geometry at all and
// are set on the QVBoxLayout around the view -- see OutlinerWidget's
// constructor for why they cannot live on the view itself.
// A depth-0 entity name therefore still lands at holder-x 30
// (13 indent + 10 glyph + 7 spacing), which is where the flat rows sat
// -- the disclosure gutter is CARVED OUT of the old 30px leading inset,
// not added to it, so a flat category looks unchanged.  Same arithmetic
// OutlinerChildRow.swift's `leadingInset` does on Mac.

constexpr int kHolderMarginX = 8;   //!< m_listHolder's left/right contents margin
constexpr int kRowMarginX    = 8;   //!< a row's own left/right contents margin
constexpr int kSpacing       = 7;   //!< the row QHBoxLayout's spacing
constexpr int kGlyphBox      = 10;  //!< the chevron QLabel's fixed 10x10 box
constexpr int kGlyphPx       = 9;   //!< the chevron SVG's rendered size
constexpr int kTagW          = 26;  //!< the tag chip's fixed width
constexpr int kIndentStep    = 13;  //!< per tree level
constexpr int kAddBox        = 16;  //!< the "+" QToolButton's fixed 16x16 box
constexpr int kAddIconPx     = 11;  //!< the "circle-plus" SVG's rendered size
constexpr int kRowPadY       = 4;   //!< a row's top/bottom contents margin

/// The pre-4c row widget's own rectangle: the viewport row minus the
/// list holder's side margins.  The selection tint spans exactly this,
/// as the old per-row stylesheet background did.
QRect contentRectOf(const QRect& rowRect)
{
    return rowRect.adjusted(kHolderMarginX, 0, -kHolderMarginX, 0);
}

/// The 10x10 disclosure box.  `treeDepth` is ignored for a category
/// header (they are all at level 0 by construction).
QRect glyphBoxOf(const QRect& rowRect, bool isCategory, int treeDepth)
{
    const QRect c = contentRectOf(rowRect);
    const int x = isCategory ? (c.left() + kRowMarginX)
                             : (c.left() + kIndentStep * (treeDepth + 1));
    return QRect(x, c.top() + (c.height() - kGlyphBox) / 2, kGlyphBox, kGlyphBox);
}

// The QFont + QFontMetrics pairs below (and the two inside
// OutlinerRowDelegate::paint) are built PER CALL, deliberately.  They are
// not cached because caching them would be the wrong trade twice over:
// only VISIBLE rows paint -- the view is capped at 305px, so ~15 of them --
// and a cache would have to be invalidated on a font or DPI change, which
// nothing here is notified of, trading a measurable correctness risk for an
// unmeasurable saving.  Both QFont and QFontMetrics are shared-data handles
// into the font-engine cache, so construction is a lookup and a refcount,
// not a font load.  If a profile ever says otherwise, cache at the DELEGATE
// (which has a QObject to hang a QEvent::FontChange hook off), not in these
// free functions.
int countTextWidth(const QString& text)
{
    return text.isEmpty() ? 0 : QFontMetrics(Theme::mono(10)).horizontalAdvance(text);
}

/// The 16x16 "+" box, immediately left of the right-aligned count.
QRect addBoxOf(const QRect& rowRect, const QString& countText)
{
    const QRect c = contentRectOf(rowRect);
    const int countLeft = c.right() + 1 - kRowMarginX - countTextWidth(countText);
    const int rightEdge = countLeft - kSpacing;
    return QRect(rightEdge - kAddBox, c.top() + (c.height() - kAddBox) / 2, kAddBox, kAddBox);
}

int rowHeightHint()
{
    return qMax(QFontMetrics(Theme::sans(11)).height(), kGlyphBox) + 2 * kRowPadY;
}

/// Bundled-SVG chevron, replacing the U+25BE / U+25B8 Unicode triangles
/// the design comp uses: those fall back through IBM Plex -> Segoe UI
/// Symbol on Windows and render inconsistently.  Deliberate Windows-side
/// exception (approved by the orchestrating session, 2026-07-23): Mac
/// renders them as text, Windows uses these SVGs for cross-panel
/// consistency with the chat/log disclosure chevrons.
///
/// Theme::iconPixmap caches on (name, size, color, dpr), so calling it
/// per row per paint is a hash lookup, not an SVG rasterisation -- which
/// is what lets the tint be read live instead of baked at build time.
void drawChevron(QPainter* painter, const QRect& box, bool expanded, qreal dpr)
{
    const QPixmap pm = Theme::iconPixmap(expanded ? QStringLiteral("chevron-down")
                                                  : QStringLiteral("chevron-right"),
                                         kGlyphPx, Theme::textDim, dpr);
    if (pm.isNull()) return;
    painter->drawPixmap(QRect(box.center().x() - kGlyphPx / 2,
                              box.center().y() - kGlyphPx / 2,
                              kGlyphPx, kGlyphPx), pm);
}

// ============================================================
// Expand-state path key
// ============================================================

/// The key a category's ROOTS extend.  Every node's key starts here, so
/// the category qualifies the whole tree (see outlinerPathKey).
QString outlinerPathRoot(ViewportBridge::Category cat)
{
    return QString::number(static_cast<int>(cat));
}

/// Extend a PARENT's key by one component, naming this node (87 section 5
/// step 4 keys expand state on the tree PATH, not on the node name -- see
/// OutlinerWidget::m_expandedPaths for the two reasons why).
///
/// The encoding is LENGTH-PREFIXED (`<byteCount>:<name>` per component)
/// rather than separator-joined, because names may legitimately contain
/// any separator one might pick: 87 section 5 step 3 records that an
/// author may write `name my.object`, and step 3's instanced descendants
/// are named `I.X` by construction.  A length prefix makes the
/// composition injective whatever the bytes are, so two distinct paths
/// can never collapse onto one key.
///
/// NAMES ALONE ARE NOT INJECTIVE OVER NODES, which is why `occurrence` is
/// here.  Category::Painter is a UNION of two managers (colour +
/// physical-scalar, CLAUDE.md's IScalarPainter split) whose contents are
/// deliberately not deduped, so `uniformcolor_painter { name DUP }` +
/// `scalar_painter { name DUP }` arrive as two DIFFERENT, separately-
/// addressable entities that are two same-named sibling roots
/// (SceneGraphNodeApiTest case T).  A key made of names alone gives them
/// ONE string.  So the k-th node to claim a given `<parent>/<len>:<name>`
/// stem gets `#k` appended (k > 0); that suffix cannot be confused with
/// part of a name, because the length prefix already fixes where the name
/// ends.  The counter is per-STEM, hence per-sibling-list (a stem embeds
/// the parent's whole key, which is node-unique).  DELIBERATELY not a
/// plain sibling ordinal: a bare ordinal would re-key every later sibling
/// when an unrelated one is inserted ahead of it, silently collapsing
/// their disclosure state.  Only a row that actually shares a name with an
/// earlier sibling carries a suffix, so the common case is
/// insertion-stable.
///
/// WHAT THE SUFFIX BUYS *HERE* is narrower than on Mac, and worth being
/// precise about.  On this shell a path key is NOT row identity -- identity
/// is the row table slot carried in QModelIndex::internalId() -- so a
/// duplicate key could only ever have tied two rows' DISCLOSURE state
/// together, never dropped a row.  And it could not even do that today:
/// Category::Painter is the only category whose entries can collide, and
/// its nodes are childless by construction (BuildCategoryTreeLocked_ seeds
/// every Painter row's parent empty), so neither row has a glyph to click.
/// The suffix is carried anyway because the exposure becomes live the
/// moment any hierarchy-capable category admits duplicate sibling names,
/// and because the Mac shell needs it for a harder reason
/// (`ForEach(rows, id: \.path)` silently DROPS a duplicated id, which is a
/// real missing row) -- keeping one encoding across the two shells is
/// cheaper than keeping two.
///
/// Same encoding as OutlinerView.swift's `outlinerPathKey` (the two shells
/// never exchange keys -- this is per-process view state -- but the two
/// implementations should not drift).  HONEST SCOPE OF THE INJECTIVITY
/// CLAIM, matching the Swift twin's: the composition is injective over the
/// key BYTES.  QString compares by code unit, so unlike Swift's canonical
/// -equivalence comparison this shell really does distinguish `q` + U+0307
/// + U+0323 from `q` + U+0323 + U+0307 -- the Qt side is the STRICTER of
/// the two, and the shared encoding is sound on both.
QString outlinerPathKey(const QString& base, const QString& name,
                        QHash<QString, int>& occurrence)
{
    QString stem = base;
    stem += QLatin1Char('/');
    stem += QString::number(name.toUtf8().size());
    stem += QLatin1Char(':');
    stem += name;

    // operator[] value-initialises a missing int to 0, so the first
    // claimant of a stem gets occurrenceIndex 0 and no suffix.  One hash
    // lookup, not the two a value()/insert() pair would cost.
    int& seen = occurrence[stem];
    const int occurrenceIndex = seen++;
    if (occurrenceIndex == 0) return stem;

    QString key = stem;
    key += QLatin1Char('#');
    key += QString::number(occurrenceIndex);
    return key;
}

QString singularOf(const char* title)
{
    const QString t = QString::fromUtf8(title);
    return t.endsWith(QLatin1Char('s')) ? t.chopped(1) : t;
}

// ============================================================
// OutlinerRowDelegate
// ============================================================
//
// No Q_OBJECT / signals -- mirrors ClickableRow (this file, pre-4c),
// ViewportProperties.cpp's ScrubHandle and TopBar.cpp's TopBarLogoSwatch,
// which document why a pure-paint/-input helper local to one .cpp does
// not need moc registration.

class OutlinerRowDelegate : public QStyledItemDelegate
{
public:
    explicit OutlinerRowDelegate(QTreeView* view)
        : QStyledItemDelegate(view)
        , m_view(view)
    {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    // Borrowed, and also this delegate's QObject parent, so it cannot
    // outlive the view.  Held because DISCLOSURE STATE LIVES IN THE VIEW,
    // not in the model: QTreeView::isExpanded is the authority, and
    // reading it here is exact where inferring it from
    // QStyleOptionViewItem::state would depend on which State_ flags
    // QTreeView::drawRow happens to forward.
    QTreeView* m_view;
};

QSize OutlinerRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    // Width is advisory only: QTreeView's constructor turns on
    // stretchLastSection, and column 0 is the only column, so the row
    // always spans the viewport.
    return QSize(160, rowHeightHint());
}

void OutlinerRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const QRect content    = contentRectOf(option.rect);
    const bool  isCategory = index.data(RoleIsCategory).toBool();
    const bool  hasKids    = index.model() != nullptr && index.model()->rowCount(index) > 0;
    const bool  expanded   = m_view != nullptr && m_view->isExpanded(index);
    // Read off the VIEW, not off painter->device(): the same idiom
    // Theme.cpp's bindIconLabel filter uses, and the only one that is
    // unambiguously the widget's screen DPR on a mixed-DPI monitor drag.
    const qreal dpr        = m_view != nullptr ? m_view->devicePixelRatioF() : qreal(1.0);
    const QFont nameFont   = Theme::sans(11);

    if (isCategory) {
        const QRect box = glyphBoxOf(option.rect, true, 0);
        // A category with no entities keeps the collapsed glyph even when
        // the bridge says its section is open -- pre-4c behaviour
        // (`expanded && !children.isEmpty()`).
        drawChevron(painter, box, expanded && hasKids, dpr);

        const QRect tagRect(box.right() + 1 + kSpacing, content.top(), kTagW, content.height());
        painter->setFont(Theme::mono(9));
        painter->setPen(index.data(RoleTagColor).value<QColor>());
        painter->drawText(tagRect, Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(RoleTag).toString());

        const QString countText = index.data(RoleCountText).toString();
        const int countW = countTextWidth(countText);
        const QRect countRect(content.right() + 1 - kRowMarginX - countW,
                              content.top(), countW, content.height());
        painter->setFont(Theme::mono(10));
        painter->setPen(Theme::textDisabled);
        painter->drawText(countRect, Qt::AlignRight | Qt::AlignVCenter, countText);

        int titleRight = countRect.left() - kSpacing;
        if (index.data(RoleHasAddButton).toBool()) {
            // Per-category "Add Entity" affordance, mirroring
            // OutlinerView.swift's "+" Menu.  DISABLED (dimmed), not
            // hidden, while the scene isn't editable, so its presence
            // doesn't flicker during a render -- the same reason the
            // pre-4c QToolButton used setEnabled rather than setVisible.
            const QRect add = addBoxOf(option.rect, countText);
            const QPixmap pm = Theme::iconPixmap(
                QStringLiteral("circle-plus"), kAddIconPx,
                index.data(RoleSceneEditable).toBool() ? Theme::textDim : Theme::textDisabled,
                dpr);
            if (!pm.isNull()) {
                painter->drawPixmap(QRect(add.center().x() - kAddIconPx / 2,
                                          add.center().y() - kAddIconPx / 2,
                                          kAddIconPx, kAddIconPx), pm);
            }
            titleRight = add.left() - kSpacing;
        }

        const int titleX = tagRect.right() + 1 + kSpacing;
        const QRect titleRect(titleX, content.top(), qMax(0, titleRight - titleX), content.height());
        painter->setFont(nameFont);
        painter->setPen(Theme::textTertiary);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(),
                                                            Qt::ElideRight, titleRect.width()));
        painter->restore();
        return;
    }

    const bool isSelected = index.data(RoleIsSelected).toBool();
    const bool isActive   = index.data(RoleIsActive).toBool();

    if (isSelected) {
        // The pre-4c row stylesheet's 14%-alpha accent fill.
        QColor tint = Theme::accent;
        tint.setAlpha(static_cast<int>(0.14 * 255));
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint);
        painter->drawRoundedRect(content, Theme::radiusSmall, Theme::radiusSmall);
    }

    const QRect box = glyphBoxOf(option.rect, false, index.data(RoleTreeDepth).toInt());
    if (hasKids) {
        // Same glyph the category header uses, so ONE visual language
        // covers both levels of disclosure.  A childless row reserves the
        // width and draws nothing, which keeps names in a column instead
        // of jittering by whether a node has parts -- and every row of a
        // flat category is exactly that.
        drawChevron(painter, box, expanded, dpr);
    }

    int nameRight = content.right() + 1 - kRowMarginX;
    if (isActive) {
        const QString badge = OutlinerWidget::tr("ACTIVE");
        const QFont badgeFont = Theme::mono(9);
        const int badgeW = QFontMetrics(badgeFont).horizontalAdvance(badge);
        const QRect badgeRect(nameRight - badgeW, content.top(), badgeW, content.height());
        painter->setFont(badgeFont);
        painter->setPen(Theme::accentLight);
        painter->drawText(badgeRect, Qt::AlignRight | Qt::AlignVCenter, badge);
        nameRight = badgeRect.left() - kSpacing;
    }

    const int nameX = box.right() + 1 + kSpacing;
    const QRect nameRect(nameX, content.top(), qMax(0, nameRight - nameX), content.height());
    painter->setFont(nameFont);
    // Selected text is Theme::textPrimary, not the Mac's literal `.white`
    // (OutlinerView.swift): the selected fill is only a 14%-alpha accent
    // tint over bgPanel, so pure white is unreadable on the Light
    // palette's near-white bgPanel.  textPrimary is near-white in Dark
    // (matches the Mac look) and flips to near-black in Light.
    painter->setPen(isSelected ? Theme::textPrimary : Theme::textMuted);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(index.data(Qt::DisplayRole).toString(),
                                                        Qt::ElideRight, nameRect.width()));

    painter->restore();
}

// ============================================================
// OutlinerTreeView
// ============================================================

enum class OutlinerHit { Row, Glyph, AddButton };

/// A QTreeView that NEVER changes its own expansion or selection.
///
/// Both matter.  Category expansion is owned by ViewportBridge
/// (isSectionExpanded / collapseSection / setSelection), which
/// ViewportProperties reads too, so the view toggling a header on its own
/// would desync the two panels.  Per-node expansion is owned by
/// OutlinerWidget's path-keyed set, which the view knows nothing about.
/// So: selection mode is NoSelection, double-click expansion is off,
/// itemsExpandable is off, tab-key navigation is off, keyPressEvent skips
/// QAbstractItemView entirely, and mousePressEvent / mouseReleaseEvent do
/// NOT chain to the base implementation.  Every expansion change arrives
/// from OutlinerWidget via setExpanded().
///
/// IT IS STILL FOCUSABLE, and deliberately so.  An earlier draft used
/// Qt::NoFocus on the reasoning that the pre-4c ClickableRow ROWS were not
/// focusable -- true of the rows, but the wrong baseline: the container
/// they sat in was a QScrollArea, and QAbstractScrollAreaPrivate::init()
/// gives that Qt::StrongFocus.  So the pre-4c outliner list WAS
/// tab-reachable and arrow-key scrollable, and NoFocus was a regression in
/// both.  StrongFocus restores them.  It costs nothing here because
/// keyboard ACTIVATION of a row never existed and still doesn't: the keys
/// that QAbstractItemView would turn into current-index moves, expand /
/// collapse (QTreeView::moveCursor's MoveLeft / MoveRight) or type-ahead
/// search are handed to QAbstractScrollArea::keyPressEvent instead, which
/// is exactly the handler the pre-4c QScrollArea used -- so the keyboard
/// scrolls the view and does nothing else.
class OutlinerTreeView : public QTreeView
{
public:
    /// (index, what was hit, viewport-relative position).
    using HitFn = std::function<void(const QModelIndex&, OutlinerHit, const QPoint&)>;

    explicit OutlinerTreeView(QWidget* parent = nullptr)
        : QTreeView(parent)
    {
        setHeaderHidden(true);
        // Indentation 0 + no root decoration means QTreeView draws no
        // branch column at all and hands the delegate the FULL row rect at
        // every depth, so the depth inset and the disclosure glyph are
        // drawn (and hit-tested) by this file's own geometry helpers.
        // That is what keeps a depth-0 row pixel-identical to the pre-4c
        // flat row.
        setRootIsDecorated(false);
        setIndentation(0);
        setUniformRowHeights(true);
        setSelectionMode(QAbstractItemView::NoSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setExpandsOnDoubleClick(false);
        setAutoScroll(false);
        // USER-driven expansion off.  QTreeView::expand() / collapse() --
        // i.e. what OutlinerWidget::applyExpansionState calls through
        // setExpanded() -- do not consult this flag, so it takes nothing
        // away from the one writer; it removes QTreeView::moveCursor's
        // MoveLeft / MoveRight expand-collapse and
        // QTreeViewPrivate::expandOrCollapseItemAtPos from being writers at
        // all, rather than leaving them inert only because indentation is 0.
        setItemsExpandable(false);
        // Tab must LEAVE this widget, as it did when the container was a
        // plain QScrollArea.  QAbstractItemView defaults tabKeyNavigation to
        // true, which would instead walk Tab from row to row and trap focus
        // in the panel.
        setTabKeyNavigation(false);
        // See the class comment: focusable, but the keyboard only scrolls.
        setFocusPolicy(Qt::StrongFocus);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setContextMenuPolicy(Qt::CustomContextMenu);
        viewport()->setMouseTracking(true);
        // NOTE: the pre-4c list holder's contentsMargins(_, 2, _, 10) are
        // NOT set here.  setViewportMargins() would be dead code:
        // QTreeView::updateGeometries() owns the viewport margins and ends
        // by writing setViewportMargins(0, headerHeight, 0, 0) itself, which
        // with setHeaderHidden(true) above zeroes all four -- and that runs
        // before the first paint (doItemsLayout() calls it, the delayed
        // layout fires as soon as a model is set and the event loop turns)
        // and again on every resize.  The 2px/10px live on the QVBoxLayout
        // around this view instead (OutlinerWidget's constructor), which is
        // where a margin that QTreeView does not own belongs.  The 8px SIDE
        // margins are a separate matter and really are folded into
        // contentRectOf(), because the selection tint has to span them.
    }

    void setHitHandler(HitFn fn) { m_onHit = std::move(fn); }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        m_pressed = QPersistentModelIndex();
        if (e->button() != Qt::LeftButton) {
            // Right/middle press chains to the base so nothing the platform
            // hangs off it changes -- the context menu arrives separately as
            // a QContextMenuEvent, routed to customContextMenuRequested.
            // Safe to chain: itemsExpandable only fires through
            // itemDecorationAt(), whose rect is zero-width at indentation 0,
            // and NoSelection makes the selection path a no-op.
            QTreeView::mousePressEvent(e);
            return;
        }
        e->accept();
        const QModelIndex idx = indexAt(e->pos());
        if (!idx.isValid()) return;

        const OutlinerHit hit = hitTest(idx, e->pos());
        if (hit == OutlinerHit::AddButton) {
            // The pre-4c "+" was a QToolButton with InstantPopup, which
            // opens its menu on PRESS.  Kept.
            if (m_onHit) m_onHit(idx, hit, e->pos());
            return;
        }
        // The pre-4c rows were ClickableRow, which fires on RELEASE inside
        // the row that was pressed.  Kept.
        m_pressed = idx;
        m_pressedHit = hit;
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) {
            QTreeView::mouseReleaseEvent(e);
            return;
        }
        e->accept();

        // Settle ALL of this view's own state before the callback runs:
        // the handler re-enters OutlinerWidget::refresh(), which can reset
        // the model out from under us.  Nothing below this point reads a
        // member.  (The base implementations are never called, so
        // QAbstractItemView has no pressed/current index of its own for a
        // reset to invalidate either.)
        const QPersistentModelIndex pressed = m_pressed;
        const OutlinerHit hit = m_pressedHit;
        m_pressed = QPersistentModelIndex();

        if (!pressed.isValid()) return;
        const QModelIndex idx = indexAt(e->pos());
        // A model reset between press and release invalidates `pressed`,
        // so the click is dropped -- which is right: the row it named may
        // not exist any more.
        if (!idx.isValid() || idx.internalId() != pressed.internalId()) return;
        if (m_onHit) m_onHit(idx, hit, e->pos());
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        // The pre-4c rows each set Qt::PointingHandCursor; the empty strip
        // below the last row did not have one, so this tracks the row under
        // the pointer rather than tinting the whole viewport.
        viewport()->setCursor(indexAt(e->pos()).isValid() ? Qt::PointingHandCursor
                                                          : Qt::ArrowCursor);
        QTreeView::mouseMoveEvent(e);
    }

    /// Keyboard = SCROLLING ONLY, which is what this list did pre-4c when
    /// it was a QScrollArea.
    ///
    /// Deliberately routed to QAbstractScrollArea rather than to
    /// QTreeView/QAbstractItemView, and the base class is SKIPPED rather
    /// than chained.  QAbstractItemView::keyPressEvent turns the arrow keys
    /// into moveCursor() + a current-index move, QTreeView::moveCursor
    /// makes Left/Right collapse/expand, and an unmatched printable key
    /// falls through to keyboardSearch(), which sets the current index and
    /// scrolls to a match.  None of those is behaviour this list ever had,
    /// and the first would make the view a second writer of per-node
    /// expansion state -- the one thing the class comment says it must
    /// never be.  Whether the arrow keys would ALSO have been swallowed
    /// silently under NoSelection depends on QStyle::
    /// SH_ItemView_ShowDecorationSelected, i.e. on the active style; not
    /// depending on that at all is the point.
    ///
    /// QAbstractScrollArea::keyPressEvent gives: Up/Down = one scroll step,
    /// Left/Right = one horizontal step (a no-op here, the horizontal bar is
    /// ScrollBarAlwaysOff and its range is empty), PageUp/PageDown = one
    /// page.  Everything else it ignores, so it propagates to the parent
    /// exactly as it did from the pre-4c QScrollArea.  Tab never reaches
    /// here at all -- setTabKeyNavigation(false) sends it back through
    /// QWidget's focus-chain handling.
    void keyPressEvent(QKeyEvent* e) override
    {
        QAbstractScrollArea::keyPressEvent(e);
    }

    // Indentation is 0 and root decoration is off, so QTreeView hands this
    // a ZERO-WIDTH branch rect at every depth; making it an explicit no-op
    // removes any question of the style painting into the gutter the
    // delegate has already drawn the disclosure glyph in.
    void drawBranches(QPainter*, const QRect&, const QModelIndex&) const override {}

    bool viewportEvent(QEvent* e) override
    {
        // The name tooltip rides Qt::ToolTipRole and is handled by the base
        // implementation; the "+" button's own tooltip is not a row
        // property, so it is served here from the same hit rect the click
        // uses.
        //
        // The RoleSceneEditable term is NOT redundant with RoleHasAddButton:
        // it keeps this condition IDENTICAL to hitTest()'s, which is the
        // invariant that matters -- a disabled "+" falls through to the
        // header toggle there, so advertising "Add Light" over it would name
        // an action the click does not perform.  It also restores pre-4c
        // behaviour: the "+" was a DISABLED QToolButton, and Qt does not
        // deliver QEvent::ToolTip to a disabled widget, so no tooltip
        // appeared then either.
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            const QModelIndex idx = indexAt(he->pos());
            if (idx.isValid()
             && idx.data(RoleIsCategory).toBool()
             && idx.data(RoleHasAddButton).toBool()
             && idx.data(RoleSceneEditable).toBool()
             && addBoxOf(visualRect(idx), idx.data(RoleCountText).toString()).contains(he->pos())) {
                QToolTip::showText(he->globalPos(), idx.data(RoleAddTooltip).toString(), this);
                return true;
            }
        }
        return QTreeView::viewportEvent(e);
    }

private:
    OutlinerHit hitTest(const QModelIndex& idx, const QPoint& pos) const
    {
        const QRect r = visualRect(idx);
        if (idx.data(RoleIsCategory).toBool()) {
            // A DISABLED "+" deliberately falls through to the header
            // toggle: the pre-4c QToolButton was disabled, not hidden, and
            // Qt delivers a disabled child's mouse event to the enclosing
            // ClickableRow, which toggled the category.
            if (idx.data(RoleHasAddButton).toBool()
             && idx.data(RoleSceneEditable).toBool()
             && addBoxOf(r, idx.data(RoleCountText).toString()).contains(pos)) {
                return OutlinerHit::AddButton;
            }
            // Anywhere else on a header toggles it, exactly as before.
            return OutlinerHit::Row;
        }
        if (model() != nullptr && model()->rowCount(idx) > 0
         && glyphBoxOf(r, false, idx.data(RoleTreeDepth).toInt()).contains(pos)) {
            return OutlinerHit::Glyph;
        }
        return OutlinerHit::Row;
    }

    HitFn m_onHit;
    QPersistentModelIndex m_pressed;
    OutlinerHit m_pressedHit = OutlinerHit::Row;
};

} // namespace

// ============================================================
// OutlinerWidget::TreeModel
// ============================================================

class OutlinerWidget::TreeModel : public QAbstractItemModel
{
public:
    explicit TreeModel(QObject* parent = nullptr)
        : QAbstractItemModel(parent)
    {
    }

    /// Rebuild the row table from one authored tree per category, in
    /// kCategories order.  Resets the model -- and therefore throws away
    /// QTreeView's own expanded set -- ONLY when the resulting shape
    /// differs from what is already published, so a preview frame that
    /// changed nothing costs a comparison instead of a reset.
    ///
    /// Returns true if it reset.
    bool setTrees(const QVector<SceneTree>& trees);

    // Paint-only state.  None of it is structural, so none of it resets
    // the model; the caller repaints the viewport.
    void setSelection(int category, const QString& name)
    {
        m_selCategory = category;
        m_selName = name;
    }
    void setActiveNames(const QHash<int, QString>& activeByCategory) { m_activeNames = activeByCategory; }
    void setTemplateCounts(const QHash<int, unsigned int>& counts) { m_templateCounts = counts; }
    void setSceneEditable(bool editable) { m_sceneEditable = editable; }

    /// Every node at every depth, across every category.
    ///
    /// Counts AUTHORED nodes, which for Objects is no longer the same as
    /// the flat entity count: 87 step 3's instancing expansions fold into
    /// the chunk that produced them, so an 8x8 count_u/count_v array
    /// counts 1 here and 64 in the render list.  That is the point of
    /// drawing the authored graph -- the count now matches what the author
    /// can actually edit.
    int totalEntityCount() const
    {
        return static_cast<int>(m_rows.size()) - static_cast<int>(m_topLevel.size());
    }

    int rowTableSize() const { return static_cast<int>(m_rows.size()); }

    /// The QModelIndex for a row-table slot.  Needed because createIndex()
    /// is protected: OutlinerWidget re-applies expansion by walking the
    /// table, and cannot mint the indices itself.
    QModelIndex indexForRow(int slot) const
    {
        if (slot < 0 || slot >= static_cast<int>(m_rows.size())) return QModelIndex();
        return createIndex(m_rows.at(slot).rowInParent, 0, static_cast<quintptr>(slot));
    }

    // ---- QAbstractItemModel ----
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex& parent) const override;
    QVariant data(const QModelIndex& idx, int role) const override;

private:
    /// One row of the FLAT table that backs the whole tree -- the eleven
    /// category headers and every authored node under them, in one array.
    ///
    /// A QModelIndex's internalId() is this row's slot in that array.
    /// THAT IS THE ONLY IDENTITY A QModelIndex EVER CARRIES HERE, and it
    /// is valid only against the CURRENTLY PUBLISHED table: setTrees()
    /// rebuilds the table from scratch and resets the model, which
    /// invalidates every index and every QPersistentModelIndex minted
    /// against the old one.  A controller TreeNodeHandle must never go
    /// here -- 87 step 4a settled that handles do not survive an
    /// event-loop turn, and a QModelIndex is exactly the thing that
    /// tempts one into being parked in a view.
    ///
    /// A flat table (rather than a node-per-object graph) is also what
    /// makes parent() answerable in O(1) without a second lookup
    /// structure: `rowInParent` is precomputed, so parent() is two array
    /// reads.
    struct Row {
        int          category = 0;      //!< ViewportBridge::Category as int
        bool         isCategory = false;
        QString      label;             //!< category title, or entity name
        QString      name;              //!< entity name; "" on a category row
        QString      pathKey;           //!< expand-state key; "" on a category row
        QString      tag;               //!< 3-letter chip; "" on an entity row
        QColor*      tagColor = nullptr;//!< live Theme:: token; see CategoryDef
        QString      addTooltip;
        int          parent = -1;       //!< slot in the table; -1 for a header
        int          rowInParent = 0;   //!< position within the parent's children
        int          treeDepth = 0;     //!< 0 for a category's ROOT entity
        int          entityCount = 0;   //!< header rows: descendants at every depth
        QVector<int> children;          //!< slots in the table
    };

    static bool sameShape(const Row& a, const Row& b)
    {
        return a.category    == b.category
            && a.isCategory  == b.isCategory
            && a.label       == b.label
            && a.name        == b.name
            && a.pathKey     == b.pathKey
            && a.parent      == b.parent
            && a.rowInParent == b.rowInParent
            && a.treeDepth   == b.treeDepth
            && a.entityCount == b.entityCount
            && a.children    == b.children;
    }

    /// Table slot behind `idx`, or -1.  Every override funnels through
    /// this, so a stale or foreign index degrades to "no such row" rather
    /// than to an out-of-bounds read.
    int slotOf(const QModelIndex& idx) const
    {
        if (!idx.isValid() || idx.model() != this) return -1;
        const quintptr id = idx.internalId();
        if (id >= static_cast<quintptr>(m_rows.size())) return -1;
        return static_cast<int>(id);
    }

    QVector<Row> m_rows;
    QVector<int> m_topLevel;   //!< slots of the eleven category headers

    int                      m_selCategory = static_cast<int>(ViewportBridge::Category::None);
    QString                  m_selName;
    QHash<int, QString>      m_activeNames;
    QHash<int, unsigned int> m_templateCounts;
    bool                     m_sceneEditable = false;
};

bool OutlinerWidget::TreeModel::setTrees(const QVector<SceneTree>& trees)
{
    QVector<Row> rows;
    QVector<int> topLevel;
    topLevel.reserve(kCategoryCount);

    for (int slot = 0; slot < kCategoryCount; ++slot) {
        const CategoryDef& def = kCategories[slot];
        const SceneTree emptyTree;
        const SceneTree& tree = slot < static_cast<int>(trees.size()) ? trees.at(slot) : emptyTree;

        const int headerSlot = static_cast<int>(rows.size());
        {
            Row header;
            header.category    = static_cast<int>(def.category);
            header.isCategory  = true;
            header.label       = QString::fromUtf8(def.title);
            header.tag         = QString::fromUtf8(def.tag);
            header.tagColor    = def.tagColor;
            header.addTooltip  = OutlinerWidget::tr("Add %1").arg(singularOf(def.title));
            header.parent      = -1;
            header.rowInParent = static_cast<int>(topLevel.size());
            rows.append(header);
            topLevel.append(headerSlot);
        }

        // Appends one entity row and links it into its parent's child
        // list.  The parent's `children` order IS display order, which is
        // 87 section 2's "child order for display comes from declaration
        // order" -- nothing is re-sorted here.
        //
        // `parentKey` is the parent's ALREADY-COMPLETE path key, passed BY
        // VALUE (a QString refcount bump, O(1)).  Not by reference: the
        // only thing a caller has to hand is `rows.at(parentSlot).pathKey`,
        // and `rows.append(row)` below can reallocate the vector out from
        // under such a reference.
        const QString categoryRoot = outlinerPathRoot(def.category);
        QHash<QString, int> occurrence;   //!< same-name-sibling counter

        auto appendNode = [&](const QString& nodeName, int parentSlot, int depth,
                              const QString parentKey) -> int {
            const int here = static_cast<int>(rows.size());
            Row row;
            row.category   = static_cast<int>(def.category);
            row.isCategory = false;
            row.label      = nodeName;
            row.name       = nodeName;
            row.pathKey    = outlinerPathKey(parentKey, nodeName, occurrence);
            row.tagColor   = def.tagColor;
            row.parent     = parentSlot;
            row.treeDepth  = depth;
            rows.append(row);
            rows[parentSlot].children.append(here);
            rows[here].rowInParent = static_cast<int>(rows.at(parentSlot).children.size()) - 1;
            return here;
        };

        const QVector<SceneTreeNode>& nodes = tree.nodes;
        const int nodeCount = static_cast<int>(nodes.size());
        QVector<bool> visited(nodeCount, false);

        // DEPTH-FIRST, ITERATIVELY, for the same reason
        // SceneEditController::BuildAuthoredTree is iterative: a
        // pathologically deep parent chain must not be able to overflow the
        // stack in the UI layer after the core went to the trouble of not
        // overflowing in its own.
        //
        // Roots come from tree.roots, NOT from a scan for `parent == -1`
        // (SceneTree's doc in ViewportBridge.h says why).  Reversed pushes
        // so the stack pops siblings in declaration order.
        //
        // The stack carries the parent's ROW SLOT, not a copy of its
        // ancestry: a key is built by appending ONE component to the
        // parent's already-built key, which is already in `rows` and, since
        // the table only ever grows, is complete before any child of that
        // row is pushed.  Carrying a QStringList instead made this
        // O(depth^2) in both time AND live stack memory on a deep chain
        // (every push detached and deep-copied the parent's list, and
        // rebuilding the key re-walked it from the root) --
        // SceneGraphNodeApiTest case E4 pins 4096 levels as supported, and
        // this runs on every scene-epoch bump.  Now the stack is O(depth)
        // and the total key-building work equals the size of the keys
        // themselves, which path keying makes unavoidable.
        struct StackItem {
            int treeIndex;
            int parentSlot;   //!< slot in `rows`; the header slot for a root
            int depth;
        };
        QVector<StackItem> stack;
        stack.reserve(nodeCount);
        for (int k = static_cast<int>(tree.roots.size()) - 1; k >= 0; --k) {
            stack.append(StackItem{ tree.roots.at(k), headerSlot, 0 });
        }

        while (!stack.isEmpty()) {
            const StackItem top = stack.takeLast();
            if (top.treeIndex < 0 || top.treeIndex >= nodeCount) continue;
            // The core guarantees every node is reached exactly once
            // (pinned by SceneGraphNodeApiTest case Y); this only stops a
            // hypothetically malformed tree from spinning here rather than
            // failing visibly.
            if (visited.at(top.treeIndex)) continue;
            visited[top.treeIndex] = true;

            const SceneTreeNode& node = nodes.at(top.treeIndex);
            // Materialised as a local (a refcount bump) rather than bound as
            // a reference into `rows`, which appendNode may reallocate.
            const QString parentKey = top.parentSlot == headerSlot
                                    ? categoryRoot
                                    : rows.at(top.parentSlot).pathKey;

            const int here = appendNode(node.name, top.parentSlot, top.depth, parentKey);
            for (int k = static_cast<int>(node.children.size()) - 1; k >= 0; --k) {
                stack.append(StackItem{ node.children.at(k), here, top.depth + 1 });
            }
        }

        // TOTALITY, defensively.  BuildAuthoredTree's contract is that
        // nothing is ever dropped -- a dangling parent is rooted and a
        // cycle is broken so every node stays visible.  If that ever
        // stopped holding, an entity would silently VANISH from the
        // outliner, which reads as a deleted object rather than as a bug.
        // So anything the walk missed is shown as a root instead of lost.
        // A salvaged row keys as a root and goes through the SAME
        // `occurrence` counter the walk did, so colliding with a real root's
        // name gets it a `#k` suffix rather than a shared disclosure key.
        for (int i = 0; i < nodeCount; ++i) {
            if (visited.at(i)) continue;
            visited[i] = true;
            appendNode(nodes.at(i).name, headerSlot, 0, categoryRoot);
        }

        rows[headerSlot].entityCount = static_cast<int>(rows.size()) - headerSlot - 1;
    }

    if (topLevel == m_topLevel && rows.size() == m_rows.size()) {
        bool identical = true;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (!sameShape(rows.at(i), m_rows.at(i))) { identical = false; break; }
        }
        // Nothing structural moved, so publishing would only cost the view
        // its expansion state and scroll position for one frame.
        if (identical) return false;
    }

    beginResetModel();
    m_rows = rows;
    m_topLevel = topLevel;
    endResetModel();
    return true;
}

QModelIndex OutlinerWidget::TreeModel::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column != 0) return QModelIndex();
    if (!parent.isValid()) {
        if (row >= static_cast<int>(m_topLevel.size())) return QModelIndex();
        return createIndex(row, column, static_cast<quintptr>(m_topLevel.at(row)));
    }
    const int parentSlot = slotOf(parent);
    if (parentSlot < 0) return QModelIndex();
    const QVector<int>& kids = m_rows.at(parentSlot).children;
    if (row >= static_cast<int>(kids.size())) return QModelIndex();
    return createIndex(row, column, static_cast<quintptr>(kids.at(row)));
}

QModelIndex OutlinerWidget::TreeModel::parent(const QModelIndex& child) const
{
    const int slot = slotOf(child);
    if (slot < 0) return QModelIndex();
    const int parentSlot = m_rows.at(slot).parent;
    if (parentSlot < 0) return QModelIndex();
    return createIndex(m_rows.at(parentSlot).rowInParent, 0, static_cast<quintptr>(parentSlot));
}

int OutlinerWidget::TreeModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid()) return static_cast<int>(m_topLevel.size());
    const int slot = slotOf(parent);
    if (slot < 0) return 0;
    return static_cast<int>(m_rows.at(slot).children.size());
}

int OutlinerWidget::TreeModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return 1;
}

QVariant OutlinerWidget::TreeModel::data(const QModelIndex& idx, int role) const
{
    const int slot = slotOf(idx);
    if (slot < 0) return QVariant();
    const Row& row = m_rows.at(slot);

    switch (role) {
    case Qt::DisplayRole:   return row.label;
    // The pre-4c child QLabel carried the full name as its tooltip; the
    // delegate elides, so this is what recovers a truncated name.
    case Qt::ToolTipRole:   return row.isCategory ? QString() : row.label;
    case RoleIsCategory:    return row.isCategory;
    case RoleTreeDepth:     return row.treeDepth;
    case RoleTag:           return row.tag;
    case RoleTagColor:      return QVariant::fromValue(row.tagColor != nullptr ? *row.tagColor
                                                                              : Theme::textDim);
    case RoleCategory:      return row.category;
    case RoleName:          return row.name;
    case RolePathKey:       return row.pathKey;
    case RoleCountText:     return row.isCategory ? QString::number(row.entityCount) : QString();
    case RoleHasAddButton:  return row.isCategory && m_templateCounts.value(row.category, 0u) > 0u;
    case RoleAddTooltip:    return row.addTooltip;
    case RoleSceneEditable: return m_sceneEditable;
    case RoleIsSelected:    return !row.isCategory
                                && m_selCategory == row.category
                                && m_selName == row.name;
    case RoleIsActive:      return !row.isCategory
                                && !m_activeNames.value(row.category).isEmpty()
                                && m_activeNames.value(row.category) == row.name;
    default:                break;
    }
    return QVariant();
}

// ============================================================
// OutlinerWidget
// ============================================================

OutlinerWidget::OutlinerWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    // Palette fill + border-bottom stylesheet set in restyleTheme()
    // (LIVE THEME-SWITCH CONTRACT, Theme.h) -- persistent chrome.

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
    // text is touched by refresh()).
    headerLayout->addWidget(m_countLabel);
    headerLayout->addStretch(1);

    root->addWidget(header);

    auto* tree = new OutlinerTreeView(this);
    m_tree = tree;
    m_tree->setMaximumHeight(305);
    // Palette + scrollbar stylesheets set in restyleTheme().

    m_model = new TreeModel(this);
    m_tree->setModel(m_model);
    m_tree->setItemDelegate(new OutlinerRowDelegate(m_tree));

    tree->setHitHandler([this](const QModelIndex& idx, OutlinerHit hit, const QPoint& pos) {
        if (!idx.isValid()) return;
        // Read everything off the index BEFORE dispatching: two of these
        // three routes re-enter refresh(), which can reset the model and
        // invalidate `idx`.
        const Category cat = static_cast<Category>(idx.data(RoleCategory).toInt());
        const bool isCategory = idx.data(RoleIsCategory).toBool();
        const QString name = idx.data(RoleName).toString();
        const QString pathKey = idx.data(RolePathKey).toString();
        const QPoint global = m_tree->viewport()->mapToGlobal(pos);

        switch (hit) {
        case OutlinerHit::AddButton: showAddMenu(cat, global); break;
        case OutlinerHit::Glyph:     toggleNodeExpansion(pathKey); break;
        case OutlinerHit::Row:
            if (isCategory) toggleCategory(cat);
            else            selectChild(cat, name);
            break;
        }
    });

    // customContextMenuRequested is declared on QWidget, so it is named
    // through QWidget -- `pos` arrives in VIEWPORT coordinates, which is
    // what QAbstractScrollArea forwards.
    connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_tree->indexAt(pos);
        // Category headers had no context menu before 4c and still don't.
        if (!idx.isValid() || idx.data(RoleIsCategory).toBool()) return;
        showRowContextMenu(static_cast<Category>(idx.data(RoleCategory).toInt()),
                           idx.data(RoleName).toString(),
                           m_tree->viewport()->mapToGlobal(pos));
    });

    // The pre-4c list holder's contentsMargins(_, 2, _, 10).  They live
    // HERE, on the layout around the view, and not on the view itself: the
    // viewport margins are QTreeView's OWN state, which
    // updateGeometries() rewrites on every layout pass (see
    // OutlinerTreeView's constructor), so anything set there is silently
    // overwritten before the first paint.  Two QSpacerItems say the same
    // thing in a place QTreeView does not reach.
    //
    // One honest difference from pre-4c: these two gaps no longer SCROLL
    // (they were the scroll CONTENT's margins, inside the QScrollArea's
    // 305px cap), so the outliner block is up to 12px taller when the tree
    // is at its maximum height.  At rest -- which is how the panel is seen
    // essentially always -- it looks the same.
    root->addSpacing(2);
    root->addWidget(m_tree);
    root->addSpacing(10);

    // With no bridge yet this publishes the eleven category headers with
    // zero counts -- what the pre-4c constructor's rebuild() drew.
    refresh();

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
    // stylesheet, the "Scene" title label, the entity-count label, and the
    // tree view's background + scrollbars -- all set once in the
    // constructor and never revisited by refresh().
    //
    // THE QUEUED REBUILD (LIVE THEME-SWITCH CONTRACT point 5) IS GONE, and
    // that is the point of 4c rather than an omission.  Point 5 has been
    // amended to record this as its one exemption, so the two agree; read
    // it there before re-adding anything here.
    //
    // The reasoning, precisely.  Point 5's hazard is not "rows are widgets"
    // on its own -- it is "the theme switched while the viewport is
    // completely IDLE, so the self-heal those panels lean on (the next
    // imageUpdated-driven refresh, or the next selection/edit) never
    // fires".  A QTimer::singleShot(0, ...) rebuild was the least-bad way
    // to force that self-heal without creating widgets synchronously inside
    // the palette-change cascade (point 2).  Here the hazard is closed at
    // the source instead of deferred: this function ENDS with an explicit
    // m_tree->viewport()->update(), and it runs off QEvent::PaletteChange,
    // which contract point 1 guarantees reaches every nested widget in the
    // app whether or not anything is rendering.  So an idle viewport
    // repaints on the switch itself.  And a repaint is the WHOLE fix,
    // because the outliner's rows are no longer widgets: OutlinerRowDelegate
    // reads every token (and every Theme::iconPixmap tint) LIVE on each
    // paint.  A repaint creates nothing, needs no deferral, and cannot lose
    // a race with a concurrent refresh().
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
    if (m_tree) {
        // An explicit rule on the tree itself, not just the palette: the
        // widget-level `border-bottom` set above has an implicit universal
        // selector and would otherwise cascade into every descendant.  Same
        // reason the pre-4c code set `QScrollArea { background-color: ... }`
        // on the scroll area.
        m_tree->setStyleSheet(QStringLiteral("QTreeView { background-color: %1; border: none; }")
            .arg(Theme::hex(Theme::bgPanel)));
        QPalette pal = m_tree->palette();
        pal.setColor(QPalette::Base, Theme::bgPanel);
        pal.setColor(QPalette::Window, Theme::bgPanel);
        pal.setColor(QPalette::Text, Theme::textPrimary);
        m_tree->setPalette(pal);
        // Slim themed scrollbars (Task A): applied directly to the scrollbar
        // widgets, not the view, so the QSS can't leak into any other
        // selector.  Bakes token colors, so re-applied on every
        // restyleTheme() call.
        if (QScrollBar* vbar = m_tree->verticalScrollBar()) {
            vbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
        if (QScrollBar* hbar = m_tree->horizontalScrollBar()) {
            hbar->setStyleSheet(Theme::scrollBarStyleSheet());
        }
        m_tree->viewport()->update();
    }
}

void OutlinerWidget::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_lastEpoch = 0;       // force a fresh tree pull on the next refresh()
    m_treesPulled = false;
    // m_expandedPaths is deliberately NOT cleared: the keys are made of
    // names, so reopening the same document restores its disclosure state,
    // and keys from a different one are inert strings.
    refresh();
}

void OutlinerWidget::refresh()
{
    if (!m_bridge) {
        // Eleven empty trees -> eleven headers with zero counts.
        m_treesPulled = false;
        m_model->setTrees(QVector<SceneTree>(kCategoryCount));
        m_model->setSelection(static_cast<int>(Category::None), QString());
        m_model->setActiveNames(QHash<int, QString>());
        m_model->setTemplateCounts(QHash<int, unsigned int>());
        applyExpansionState();
        m_countLabel->setText(tr("%1 entities").arg(m_model->totalEntityCount()));
        m_tree->viewport()->update();
        return;
    }

    // Trees only need re-pulling when the scene structure actually changed
    // (add/remove/re-parent) -- cheap epoch check, mirroring the
    // pre-redesign accordion's caching.  See refresh()'s doc for the
    // drag-to-reparent obligation this creates.
    const unsigned int epoch = m_bridge->sceneEpoch();
    if (epoch != m_lastEpoch || !m_treesPulled) {
        m_lastEpoch = epoch;
        m_treesPulled = true;
        QVector<SceneTree> trees;
        trees.reserve(kCategoryCount);
        for (const CategoryDef& def : kCategories) {
            trees.append(m_bridge->categoryTree(def.category));
        }
        m_model->setTrees(trees);
    }

    // Paint-only state, re-read on every call -- the pre-4c rebuild() did
    // the same, and these are the same cheap C-API round-trips it made.
    QHash<int, QString> active;
    QHash<int, unsigned int> templates;
    for (const CategoryDef& def : kCategories) {
        const int catInt = static_cast<int>(def.category);
        active.insert(catInt, m_bridge->activeNameForCategory(def.category));
        // Templates are queried live so the "+" affordance disappears
        // automatically for categories with none registered (Camera /
        // Rasterizer / Film / Animation / SceneVariant).
        templates.insert(catInt, m_bridge->entityTemplateCount(def.category));
    }
    m_model->setActiveNames(active);
    m_model->setTemplateCounts(templates);
    m_model->setSelection(static_cast<int>(m_bridge->selectionCategory()), m_bridge->selectionName());

    applyExpansionState();
    m_countLabel->setText(tr("%1 entities").arg(m_model->totalEntityCount()));
    m_tree->viewport()->update();
}

void OutlinerWidget::applyExpansionState()
{
    if (!m_tree || !m_model) return;

    // Re-applied in full on EVERY refresh, for two separate reasons.
    // (1) QTreeView clears its own expanded set on a model reset, so a
    //     shape change would otherwise collapse everything.
    // (2) Category expansion can change from OUTSIDE this widget -- a
    //     viewport click-to-pick calls setSelection, which opens that
    //     category's section, and ViewportProperties reads the same state.
    // Guarded by isExpanded() rather than calling setExpanded()
    // unconditionally: isExpanded() is two hash lookups, whereas expand()'s
    // own early-out is an internal detail, and the loop runs once per
    // authored node per preview frame.  With the guard the steady-state
    // cost is provably O(nodes) hash lookups and no view relayout.
    const int n = m_model->rowTableSize();
    for (int slot = 0; slot < n; ++slot) {
        const QModelIndex idx = m_model->indexForRow(slot);
        if (!idx.isValid()) continue;
        if (m_model->rowCount(idx) == 0) continue;   // nothing to disclose

        bool want = false;
        if (idx.data(RoleIsCategory).toBool()) {
            // The BRIDGE owns category expansion (ViewportProperties reads
            // the same flag), so it is read, never inferred from the view.
            want = m_bridge != nullptr
                && m_bridge->isSectionExpanded(static_cast<Category>(idx.data(RoleCategory).toInt()));
        } else {
            want = m_expandedPaths.contains(idx.data(RolePathKey).toString());
        }
        if (m_tree->isExpanded(idx) != want) m_tree->setExpanded(idx, want);
    }
}

void OutlinerWidget::setSceneEditable(bool editable)
{
    if (m_sceneEditable == editable) return;
    m_sceneEditable = editable;
    // Re-derive the "+" tint and the context menu's enable state
    // immediately rather than waiting for the next epoch-gated refresh() --
    // a render finishing (or starting) should flip them on the SAME tick,
    // matching PropertiesPanel.swift's live binding.  Nothing structural
    // depends on the flag, so this is a pure repaint.
    m_model->setSceneEditable(editable);
    if (m_tree) m_tree->viewport()->update();
}

void OutlinerWidget::toggleCategory(Category cat)
{
    if (!m_bridge) return;
    if (m_bridge->isSectionExpanded(cat)) {
        m_bridge->collapseSection(cat);
    } else {
        // Empty-name selection opens the section without picking a row --
        // same mechanism the pre-redesign accordion used.
        m_bridge->setSelection(cat, QString());
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

void OutlinerWidget::toggleNodeExpansion(const QString& pathKey)
{
    // Purely local: the bridge models expansion per CATEGORY only
    // (isSectionExpanded / collapseSection, which ViewportProperties also
    // reads) and has no per-node concept, so nothing is re-read and no
    // selection changes -- which is also why this does NOT emit
    // selectionActivated().
    if (pathKey.isEmpty()) return;
    if (m_expandedPaths.contains(pathKey)) m_expandedPaths.remove(pathKey);
    else                                   m_expandedPaths.insert(pathKey);
    applyExpansionState();
}

void OutlinerWidget::showAddMenu(Category cat, const QPoint& globalPos)
{
    if (!m_bridge || !m_sceneEditable) return;
    const unsigned int templateCount = m_bridge->entityTemplateCount(cat);
    if (templateCount == 0) return;

    QMenu menu(this);
    QVector<QAction*> actions;
    actions.reserve(static_cast<int>(templateCount));
    for (unsigned int i = 0; i < templateCount; ++i) {
        actions.append(menu.addAction(m_bridge->entityTemplateLabel(cat, i)));
    }
    // MainWindow owns the actual instantiate + select-new-entity +
    // failure-alert sequence; this widget only reports the user's pick.
    QAction* picked = menu.exec(globalPos);
    if (!picked) return;
    const qsizetype slot = actions.indexOf(picked);
    if (slot >= 0) emit addEntityRequested(cat, static_cast<unsigned int>(slot));
}

void OutlinerWidget::showRowContextMenu(Category cat, const QString& name, const QPoint& globalPos)
{
    // "Reveal in scene file" (item 3): Rasterizer/Film rows have no chunk
    // address (registry/preset names, not chunk names) -- disabled rather
    // than a silent no-op, mirroring OutlinerView.swift's review-P3 fix.
    // Resolvability of a particular name/category (does it actually HAVE a
    // chunk location) isn't pre-checked here -- an unresolvable target just
    // no-ops on the MainWindow side, same as the properties-panel chip when
    // hidden mid-render.
    const bool canReveal = m_sceneEditable
        && cat != Category::Rasterizer
        && cat != Category::Film;
    // Entity-creation slice: Duplicate/Delete need the SAME chunk-name
    // addressing "Reveal in scene file" needs (no Rasterizer/Film) plus the
    // scene-editable gate -- kept as a separate flag (== canReveal today)
    // rather than reused directly, mirroring OutlinerChildRow.swift's
    // canReveal/canMutate split so a future divergence between the two
    // gates doesn't require touching every call site.
    const bool canMutate = canReveal;

    QMenu menu(this);
    QAction* revealAction = menu.addAction(tr("Reveal in scene file"));
    revealAction->setEnabled(canReveal);
    menu.addSeparator();
    QAction* duplicateAction = menu.addAction(tr("Duplicate"));
    duplicateAction->setEnabled(canMutate);
    QAction* deleteAction = menu.addAction(tr("Delete"));
    deleteAction->setEnabled(canMutate);

    QAction* triggered = menu.exec(globalPos);
    if (triggered == revealAction) {
        emit revealRequested(cat, name);
    } else if (triggered == duplicateAction) {
        emit duplicateRequested(cat, name);
    } else if (triggered == deleteAction) {
        emit deleteRequested(cat, name);
    }
}
