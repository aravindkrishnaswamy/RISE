//////////////////////////////////////////////////////////////////////
//
//  ObjectGraphCanvas.cpp - RISE UI redesign, left-panel "Object Graph"
//  tab.
//
//  See ObjectGraphCanvas.h for the slice scope (S3, Qt carry). The
//  PARITY TABLE mapping every macOS behavior to its Qt mirror lives in
//  ONE shared block comment at the BOTTOM of NodeGraphCanvas.cpp
//  (search "PARITY TABLE") -- row 30 there is this canvas's entry; this
//  file does not carry a second table.
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "ObjectGraphCanvas.h"
#include "Theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsItem>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSceneHoverEvent>
#include <QStyleOptionGraphicsItem>
#include <QColor>
#include <QFont>
#include <QRectF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QFontMetrics>
#include <QWheelEvent>
#include <QSize>
#include <QFrame>

#include <cmath>

// ======================================================================
// Layout constants -- mirrors ObjectGraphCanvas.swift's private
// ObjGraphMetrics enum. nodeHeight/headerHeight differ from that file's
// own values for the SAME reason NodeGraphCanvas.cpp's own GraphMetrics
// differs from its Mac twin (see that file's PARITY TABLE, "Two values
// differ from the Mac source") -- Qt's QFontMetrics-measured text needs
// slightly more vertical room than SwiftUI's layout gives the same
// content on this platform's default fonts. No thumbnail here (unlike
// NodeGraphCanvas::GraphMetrics::thumbnailSize) -- an Object/Geometry
// node has no PainterPreview-style thumbnail to show.
// ======================================================================

namespace ObjectGraphMetrics {
constexpr qreal nodeWidth          = 172.0;
constexpr qreal nodeHeight         = 72.0;
constexpr qreal headerHeight       = 32.0;
constexpr qreal padding            = 80.0;
constexpr qreal labelZoomThreshold = 0.55;
constexpr qreal minScale           = 0.2;
constexpr qreal maxScale           = 2.5;
}

// ======================================================================
// Free helpers -- category tint/label, port anchor geometry, wire/label
// drawing. Anonymous-namespace, visible to the global-namespace item
// classes below via ordinary unqualified lookup within this one TU.
// Ported from (not shared with) NodeGraphCanvas.cpp's own anonymous-
// namespace twins -- two `private`-equivalent TU-local helper sets, the
// same "no shared header for this much code" call ObjectGraphPortData's
// own .h comment makes for the plain value types.
// ======================================================================

namespace {

/// RISE::ChunkCategory ordinals (ChunkDescriptor.h): Geometry=5, Object=8
/// (a rect_light/shape_light node already arrives re-categorized as
/// Object=8 -- see ObjectGraphNodeData::category's own comment).
QColor objectGraphCategoryTint(int category)
{
    return (category == 5) ? Theme::teal : Theme::catObject;
}

QString objectGraphCategoryLabel(int category)
{
    return (category == 5) ? QObject::tr("Geometry") : QObject::tr("Object");
}

QVector<qreal> objectGraphInputPortAnchors(const QPointF& nodePos, int outEdgeCount)
{
    const int count = qMax(outEdgeCount, 1);
    QVector<qreal> out;
    if (count <= 1) {
        out.append(nodePos.y() + ObjectGraphMetrics::nodeHeight / 2.0);
        return out;
    }
    const qreal top = nodePos.y() + ObjectGraphMetrics::headerHeight + 6.0;
    const qreal bottom = nodePos.y() + ObjectGraphMetrics::nodeHeight - 6.0;
    const qreal step = (bottom - top) / qreal(count - 1);
    out.reserve(count);
    for (int i = 0; i < count; ++i) out.append(top + step * qreal(i));
    return out;
}

void objectGraphDrawWire(QPainter* painter, const QPointF& from, const QPointF& to, const QColor& color, bool dashed)
{
    QPainterPath path(from);
    const qreal dx = qMax(qAbs(to.x() - from.x()) * 0.5, 24.0);
    const QPointF c1(from.x() + dx, from.y());
    const QPointF c2(to.x() - dx, to.y());
    path.cubicTo(c1, c2, to);
    QPen pen(color, 1.5);
    if (dashed) pen.setStyle(Qt::DashLine);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
}

void objectGraphDrawLabel(QPainter* painter, const QString& text, const QPointF& near, const QColor& color)
{
    painter->setPen(color);
    painter->setFont(Theme::mono(8));
    const QRectF box(near.x() - 4.0 - 130.0, near.y() - 14.0, 130.0, 12.0);
    painter->drawText(box, Qt::AlignRight | Qt::AlignVCenter, text);
}

} // namespace

// ======================================================================
// ObjectGraphNodeItem -- one node box, READ-ONLY (no ItemIsMovable, no
// drag gestures at all -- see ObjectGraphCanvas.h's own "SCOPE" note).
// Forward-declared in ObjectGraphCanvas.h as a friend, so declared here
// in the GLOBAL namespace (must match).
// ======================================================================

class ObjectGraphNodeItem : public QGraphicsItem
{
public:
    ObjectGraphNodeItem(const ObjectGraphNodeData& data, ObjectGraphCanvas* canvas)
        : m_data(data), m_canvas(canvas)
    {
        // Deliberately NO ItemIsMovable, NO ItemSendsGeometryChanges --
        // this phase never drags a node (layout is always transient; see
        // the .h's own header comment).
        setAcceptHoverEvents(true);
        setZValue(0.0);
        setToolTip(data.name);
    }

    QRectF boundingRect() const override
    {
        return QRectF(0.0, 0.0, ObjectGraphMetrics::nodeWidth, ObjectGraphMetrics::nodeHeight);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    const ObjectGraphNodeData& data() const { return m_data; }

    void setNodeSelected(bool selected)
    {
        if (m_isSelected == selected) return;
        m_isSelected = selected;
        update();
    }

    /// This node matches the CURRENT external Object selection -- a
    /// visually DISTINCT state from m_isSelected (this canvas's own
    /// click-to-select) for the SAME reason NodeGraphCanvas::
    /// GraphNodeItem::setNodeSpotlit's identical comment gives.
    void setNodeSpotlit(bool spotlit)
    {
        if (m_isSpotlit == spotlit) return;
        m_isSpotlit = spotlit;
        update();
    }

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override
    {
        Q_UNUSED(event);
        m_isHovered = true;
        update();
    }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override
    {
        Q_UNUSED(event);
        m_isHovered = false;
        update();
    }

    // Plain click-to-select only -- no double-click affordance exists
    // this phase (ObjectGraphCanvas.h's own "SCOPE" note), so there is no
    // mouseDoubleClickEvent override here at all, unlike GraphNodeItem's.
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_canvas) {
            m_canvas->selectNode(m_data);
        }
        QGraphicsItem::mousePressEvent(event);
    }

private:
    void drawRepeatBadge(QPainter* painter, const QRectF& rect) const;

    ObjectGraphNodeData m_data;
    ObjectGraphCanvas*  m_canvas;
    bool                m_isSelected = false;
    bool                m_isSpotlit  = false;
    bool                m_isHovered  = false;
};

void ObjectGraphNodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRectF rect(0.0, 0.0, ObjectGraphMetrics::nodeWidth, ObjectGraphMetrics::nodeHeight);

    // Warm-gold spotlight glow, drawn BEHIND the card -- same
    // concentric-stroke approximation NodeGraphCanvas::GraphNodeItem::paint
    // uses for the identical Mac `.shadow` effect Qt has no direct
    // equivalent of.
    if (m_isSpotlit) {
        QColor glow = Theme::gold;
        glow.setAlphaF(0.35);
        QPainterPath glowPath;
        glowPath.addRoundedRect(rect.adjusted(-3.0, -3.0, 3.0, 3.0),
                                 Theme::radiusMedium + 3.0, Theme::radiusMedium + 3.0);
        painter->setPen(QPen(glow, 3.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(glowPath);
    }

    QPainterPath cardPath;
    cardPath.addRoundedRect(rect, Theme::radiusMedium, Theme::radiusMedium);
    painter->fillPath(cardPath, Theme::bgCard);

    QColor borderColor = Theme::borderLight;
    qreal borderWidth = 1.0;
    if (m_isSelected) { borderColor = Theme::accent; borderWidth = 2.0; }
    else if (m_isSpotlit) { borderColor = Theme::gold; borderWidth = 2.0; }
    else if (m_isHovered) { borderColor = Theme::borderHover; }
    painter->setPen(QPen(borderColor, borderWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(cardPath);

    // Header bar, clipped to the card's own rounded top corners.
    painter->save();
    painter->setClipPath(cardPath);
    const QRectF headerRect(0.0, 0.0, ObjectGraphMetrics::nodeWidth, ObjectGraphMetrics::headerHeight);
    QColor headerFill = objectGraphCategoryTint(m_data.category);
    headerFill.setAlphaF(0.85);
    painter->fillRect(headerRect, headerFill);
    painter->restore();

    // repeatCount badge, top-right of the header -- reserve space only
    // when it will actually be drawn.
    const bool showBadge = m_data.repeatCount >= 1;
    const qreal badgeReserve = showBadge ? 30.0 : 6.0;

    painter->setPen(Theme::textOnAccent);
    const QFont nameFont = Theme::sans(10, QFont::DemiBold);
    painter->setFont(nameFont);
    const QFontMetrics nameFm(nameFont);
    const QRectF nameRect(8.0, 3.0, ObjectGraphMetrics::nodeWidth - 8.0 - badgeReserve, 14.0);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                       nameFm.elidedText(m_data.name, Qt::ElideRight, int(nameRect.width())));

    QColor kwColor = Theme::textOnAccent;
    kwColor.setAlphaF(0.75);
    painter->setPen(kwColor);
    const QFont kwFont = Theme::mono(8);
    painter->setFont(kwFont);
    const QFontMetrics kwFm(kwFont);
    const QRectF kwRect(8.0, 18.0, ObjectGraphMetrics::nodeWidth - 16.0, 12.0);
    painter->drawText(kwRect, Qt::AlignLeft | Qt::AlignVCenter,
                       kwFm.elidedText(m_data.chunkKeyword, Qt::ElideRight, int(kwRect.width())));

    if (showBadge) {
        drawRepeatBadge(painter, QRectF(ObjectGraphMetrics::nodeWidth - 34.0, 5.0, 28.0, 14.0));
    }

    // Content row: category label only (no thumbnail, no def-count pill,
    // no dangling badge -- this graph's nodes carry none of those).
    const QRectF contentRect(8.0, ObjectGraphMetrics::headerHeight + 6.0,
                              ObjectGraphMetrics::nodeWidth - 16.0, 16.0);
    painter->setPen(Theme::textDim);
    painter->setFont(Theme::mono(8.5));
    painter->drawText(contentRect, Qt::AlignLeft | Qt::AlignVCenter,
                       objectGraphCategoryLabel(m_data.category));
}

/// Shown for ANY repeatCount >= 1, including the `count_u 1` case ("×1")
/// -- see ObjectGraphNodeData::repeatCount's own header comment. Mirrors
/// NodeGraphCanvas::GraphNodeItem::drawPillBadge's shape (not a shared
/// call -- that one is `private` to its own class).
void ObjectGraphNodeItem::drawRepeatBadge(QPainter* painter, const QRectF& rect) const
{
    QPainterPath p;
    p.addRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
    painter->fillPath(p, QColor(0, 0, 0, 71));
    painter->setPen(Theme::textOnAccent);
    painter->setFont(Theme::mono(7, QFont::DemiBold));
    painter->drawText(rect, Qt::AlignCenter, QStringLiteral("×%1").arg(m_data.repeatCount));
}

// ======================================================================
// ObjectGraphWiresLayerItem -- draws every real edge exactly once
// (walked via each node's outEdges), plus a short warning-tinted stub
// for every dangling port. No wire-drag draft to draw (unlike
// NodeGraphCanvas::GraphWiresLayerItem) -- this phase has no drag-to-
// wire at all.
// ======================================================================

class ObjectGraphWiresLayerItem : public QGraphicsItem
{
public:
    explicit ObjectGraphWiresLayerItem(ObjectGraphCanvas* canvas) : m_canvas(canvas)
    {
        setZValue(-10.0);
    }

    QRectF boundingRect() const override { return m_rect; }

    void setContentRect(const QRectF& r)
    {
        prepareGeometryChange();
        m_rect = r;
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override;

private:
    ObjectGraphCanvas* m_canvas;
    QRectF m_rect{0.0, 0.0, 400.0, 300.0};
};

void ObjectGraphWiresLayerItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    if (!m_canvas) return;
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool showLabels = m_canvas->currentScale() >= ObjectGraphMetrics::labelZoomThreshold;
    const QVector<ObjectGraphNodeItem*>& items = m_canvas->m_nodeItems;

    for (ObjectGraphNodeItem* nodeItem : items) {
        const ObjectGraphNodeData& data = nodeItem->data();
        const QPointF nodePos = nodeItem->pos();
        const QVector<qreal> destYs = objectGraphInputPortAnchors(nodePos, data.outEdges.size());
        for (int portIdx = 0; portIdx < data.outEdges.size() && portIdx < destYs.size(); ++portIdx) {
            const ObjectGraphPortData& port = data.outEdges[portIdx];
            const QPointF destPoint(nodePos.x(), destYs[portIdx]);
            if (port.otherNodeIndex >= 0 && port.otherNodeIndex < items.size()) {
                ObjectGraphNodeItem* srcItem = items[port.otherNodeIndex];
                const QPointF srcPos = srcItem->pos();
                const QPointF srcPoint(srcPos.x() + ObjectGraphMetrics::nodeWidth, srcPos.y() + ObjectGraphMetrics::nodeHeight / 2.0);
                QColor c = objectGraphCategoryTint(srcItem->data().category);
                c.setAlphaF(0.75);
                objectGraphDrawWire(painter, srcPoint, destPoint, c, false);
                if (showLabels) objectGraphDrawLabel(painter, port.paramName, destPoint, Theme::textFaint);
            } else {
                const QPointF stubStart(destPoint.x() - 22.0, destPoint.y());
                QColor c = Theme::warn; c.setAlphaF(0.85);
                objectGraphDrawWire(painter, stubStart, destPoint, c, true);
                if (showLabels) objectGraphDrawLabel(painter, port.paramName, destPoint, Theme::warn);
            }
        }
    }
}

// ======================================================================
// ObjectGraphView -- QGraphicsView subclass: wheel-zoom (clamped,
// anchored under the cursor) + panning via Qt's own ScrollHandDrag.
// Since NO item in this canvas is ever movable (read-only, no
// ItemIsMovable anywhere), a press ANYWHERE -- including on a node --
// that Qt's item layer does not itself consume for a click falls
// through to this view's pan drag; a plain click (no meaningful
// movement) is what ObjectGraphNodeItem::mousePressEvent already
// handles as a select, so the two never conflict in practice (a real
// pan-drag that happens to start on a node card behaves exactly like
// one starting on empty canvas space).
// ======================================================================

class ObjectGraphView : public QGraphicsView
{
public:
    explicit ObjectGraphView(ObjectGraphCanvas* canvas, QWidget* parent = nullptr)
        : QGraphicsView(parent), m_canvas(canvas)
    {
        setRenderHint(QPainter::Antialiasing, true);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
        setFrameShape(QFrame::NoFrame);
        setFocusPolicy(Qt::StrongFocus);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }

    qreal currentScale() const { return transform().m11(); }

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        const qreal cur = currentScale();
        const qreal factor = std::pow(1.0015, event->angleDelta().y());
        const qreal next = qBound(ObjectGraphMetrics::minScale, cur * factor, ObjectGraphMetrics::maxScale);
        const qreal applied = (cur > 0.0) ? next / cur : 1.0;
        if (!qFuzzyCompare(applied, qreal(1.0))) scale(applied, applied);
        event->accept();
    }

private:
    ObjectGraphCanvas* m_canvas;
};

// ======================================================================
// ObjectGraphCanvas
// ======================================================================

ObjectGraphCanvas::ObjectGraphCanvas(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_header = new QWidget(this);
    m_header->setFixedHeight(34);
    auto* headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(14, 0, 10, 0);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(tr("Object Graph"), m_header);
    m_titleLabel->setFont(Theme::sans(12, QFont::DemiBold));
    headerLayout->addWidget(m_titleLabel);

    m_countLabel = new QLabel(m_header);
    m_countLabel->setFont(Theme::mono(10));
    headerLayout->addWidget(m_countLabel);
    headerLayout->addStretch(1);

    // All/Focused toggle -- same checkable-QToolButton idiom
    // NodeGraphCanvas::m_viewScopeBtn uses (see that field's own .h
    // comment for why a segmented control has no cheap single-widget Qt
    // equivalent).
    m_viewScopeBtn = new QToolButton(m_header);
    m_viewScopeBtn->setAutoRaise(true);
    m_viewScopeBtn->setCheckable(true);
    m_viewScopeBtn->setCursor(Qt::PointingHandCursor);
    m_viewScopeBtn->setText(tr("Focused"));
    m_viewScopeBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_viewScopeBtn->setToolTip(tr("All: every object and geometry chunk. Focused: the selected object's parents, children, and geometry only."));
    connect(m_viewScopeBtn, &QToolButton::toggled, this, &ObjectGraphCanvas::onViewScopeToggled);
    headerLayout->addWidget(m_viewScopeBtn);

    m_refreshBtn = new QToolButton(m_header);
    m_refreshBtn->setAutoRaise(true);
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setToolTip(tr("Refresh graph"));
    m_refreshBtn->setIconSize(QSize(11, 11));
    connect(m_refreshBtn, &QToolButton::clicked, this, &ObjectGraphCanvas::refreshForce);
    headerLayout->addWidget(m_refreshBtn);

    root->addWidget(m_header);

    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(0, 0, 800, 600);
    m_wiresLayer = new ObjectGraphWiresLayerItem(this);
    m_scene->addItem(m_wiresLayer);

    m_view = new ObjectGraphView(this);
    m_view->setScene(m_scene);
    root->addWidget(m_view, 1);

    updateHeaderCounts();

    m_themeReady = true;
    restyleTheme();
}

void ObjectGraphCanvas::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_lastFetchedEpoch = 0;
    m_hasFetchedOnce = false;
    m_hasCentered = false;
    m_selectedHandleValid = false;
    // Sticky-focus memo + its cheap gate + the focused-fetch gate all
    // clear TOGETHER -- same lockstep-clear discipline NodeGraphCanvas::
    // setBridge's identical block documents (this canvas is a PERSISTENT
    // widget, so a scene (re)load is the only reliable "stale name from
    // the previous scene" signal available here).
    m_stickyFocusHasObject = false;
    m_stickyFocusObjectName.clear();
    m_lastStickyFocusCategory = ViewportBridge::Category::None;
    m_lastStickyFocusSelectionName.clear();
    m_lastFocusedHasTarget = false;
    m_lastFocusedName.clear();
    // Spotlight's own cheap gate, same reasoning.
    m_lastSpotlightCategory = ViewportBridge::Category::None;
    m_lastSpotlightSelectionName.clear();
    performReload(true);
}

void ObjectGraphCanvas::setSceneEditable(bool editable)
{
    m_sceneEditable = editable;   // reserved -- see the .h's own comment
}

void ObjectGraphCanvas::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void ObjectGraphCanvas::restyleTheme()
{
    m_themeEpochSeen = Theme::paletteEpoch();

    if (m_header) {
        m_header->setAutoFillBackground(true);
        QPalette pal = m_header->palette();
        pal.setColor(QPalette::Window, Theme::bgPanel);
        m_header->setPalette(pal);
        m_header->setStyleSheet(QStringLiteral("border-bottom: 1px solid %1;").arg(Theme::hex(Theme::borderHairline)));
    }
    if (m_titleLabel) m_titleLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textPrimary)));
    if (m_countLabel) m_countLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
    if (m_refreshBtn) m_refreshBtn->setIcon(Theme::icon(QStringLiteral("refresh-cw"), 11, Theme::textDim, Theme::textDisabled));
    if (m_view) m_view->setBackgroundBrush(Theme::bgWell);

    // OutlinerWidget's exemption (Theme.h LIVE THEME-SWITCH CONTRACT
    // point 5) -- same claim NodeGraphCanvas::restyleTheme's identical
    // block makes for its own item layer.
    if (m_view) m_view->viewport()->update();
}

// ---- reload -------------------------------------------------------------

void ObjectGraphCanvas::refresh()      { performReload(false); }
void ObjectGraphCanvas::refreshForce() { performReload(true); }

void ObjectGraphCanvas::performReload(bool force)
{
    if (!m_bridge) { applySnapshot(ViewportBridge::PainterGraph()); refreshSpotlight(true); return; }

    // MUST run before anything below consults the sticky focus memo.
    updateStickyFocusObject();

    if (m_viewScope == ObjectGraphViewScope::Focused) {
        QString name;
        if (currentFocusTarget(name)) {
            performFocusedReload(name, force);
            return;
        }
        // No selection -- fall through to the ORDINARY epoch-gated
        // All-view fetch below, forcing a fresh fetch on the TRANSITION
        // into this sub-state -- same reasoning NodeGraphCanvas::
        // performReload's identical block documents.
        if (m_lastFocusedHasTarget || !m_hasFetchedOnce) force = true;
        m_lastFocusedHasTarget = false;
        m_lastFocusedName.clear();
    }

    const unsigned int epoch = m_bridge->sceneEpoch();
    bool rebuilt = false;
    if (force || !m_hasFetchedOnce || epoch != m_lastFetchedEpoch) {
        m_lastFetchedEpoch = epoch;
        m_hasFetchedOnce = true;
        applySnapshot(m_bridge->objectGraph());
        rebuilt = true;
    }
    // Independent of the epoch-gated branch above: a plain Object pick
    // does not bump sceneEpoch, so the spotlight must be re-derived on
    // every call regardless of whether the structural graph was just
    // refetched -- same reasoning NodeGraphCanvas::performReload's
    // identical tail documents.
    refreshSpotlight(rebuilt);
}

void ObjectGraphCanvas::updateStickyFocusObject()
{
    if (!m_bridge) return;
    // Updates the sticky memo ONLY when the CURRENT shared selection is
    // actually an Object -- a canvas-node click sets the shared selection
    // to Object OR Geometry (selectNode below); a Geometry pick is
    // deliberately ignored here (leaves the memo untouched, i.e. sticky),
    // while an Object pick IS honored -- clicking an object node in this
    // canvas is exactly "focus on this object", the same gesture picking
    // it in the outliner would be. That asymmetry falls out of this ONE
    // guard with no extra code -- the natural analogue of
    // NodeGraphCanvas's own "a Painter/Material click never retargets
    // Focused mode" rule, adapted to a canvas whose OWN nodes can
    // themselves be valid focus roots (ported from
    // ObjectGraphCanvas.swift's identical comment on the SAME emergent
    // behavior).
    const ViewportBridge::Category cat = m_bridge->selectionCategory();
    if (cat != ViewportBridge::Category::Object) return;

    // CHEAP pre-check -- same two-tier gate NodeGraphCanvas::
    // updateStickyFocusObject's own comment documents in full (this
    // canvas re-derives on EVERY preview frame too).
    const QString cheapName = m_bridge->selectionName();
    if (cat == m_lastStickyFocusCategory && cheapName == m_lastStickyFocusSelectionName) return;
    m_lastStickyFocusCategory = cat;
    m_lastStickyFocusSelectionName = cheapName;

    const QString n = m_bridge->selectionRowName();
    m_stickyFocusHasObject = !n.isEmpty();
    m_stickyFocusObjectName = n;
}

bool ObjectGraphCanvas::currentFocusTarget(QString& outName) const
{
    if (!m_stickyFocusHasObject) return false;
    outName = m_stickyFocusObjectName;
    return true;
}

void ObjectGraphCanvas::performFocusedReload(const QString& name, bool force)
{
    if (!m_bridge) return;

    // Cheap pre-check, gated on m_lastFocusedHasTarget too -- same
    // lockstep-commit discipline NodeGraphCanvas::performFocusedReload's
    // own comment documents in full (the P1 fix that closed the
    // reselect-during-contention wedge).
    if (!force && m_lastFocusedHasTarget && name == m_lastFocusedName) return;

    bool degraded = false;
    const ViewportBridge::PainterGraph g = m_bridge->objectGraphFocused(name, &degraded);
    if (degraded) {
        // DEGRADED: leave BOTH gate fields exactly as they were -- the
        // NEXT imageUpdated frame retries the deep resolve for real (no
        // explicit timer needed, this canvas already re-derives every
        // preview frame -- same reasoning NodeGraphCanvas's identical
        // branch documents).
        return;
    }
    m_lastFocusedHasTarget = true;
    m_lastFocusedName = name;
    // A genuinely empty result means the sticky object no longer
    // resolves -- fall back to All and CLEAR the sticky memo, same
    // design NodeGraphCanvas::performFocusedReload's own empty-result
    // branch documents.
    if (g.nodes.isEmpty()) {
        m_stickyFocusHasObject = false;
        m_stickyFocusObjectName.clear();
        applySnapshot(m_bridge->objectGraph());
        refreshSpotlight(true);
        return;
    }
    applySnapshot(g);
    refreshSpotlight(true);
}

void ObjectGraphCanvas::onViewScopeToggled(bool checked)
{
    m_viewScope = checked ? ObjectGraphViewScope::Focused : ObjectGraphViewScope::All;
    performReload(true);
}

void ObjectGraphCanvas::applySnapshot(const ViewportBridge::PainterGraph& g)
{
    for (ObjectGraphNodeItem* item : qAsConst(m_nodeItems)) {
        m_scene->removeItem(item);
        delete item;
    }
    m_nodeItems.clear();
    m_nodes.clear();

    m_nodes.reserve(g.nodes.size());
    for (int i = 0; i < g.nodes.size(); ++i) {
        const ViewportBridge::PainterGraphNode& n = g.nodes[i];
        ObjectGraphNodeData d;
        d.handle = n.handle;
        d.index = i;
        d.name = n.name;
        d.chunkKeyword = n.chunkKeyword;
        d.category = n.category;
        d.repeatCount = n.repeatCount;
        d.position = QPointF(n.x, n.y);
        d.outEdges.reserve(n.outEdges.size());
        for (const ViewportBridge::PainterGraphPort& p : n.outEdges) {
            d.outEdges.append(ObjectGraphPortData{ p.paramName, p.occurrence, p.otherNodeIndex, p.otherName });
        }
        d.inEdges.reserve(n.inEdges.size());
        for (const ViewportBridge::PainterGraphPort& p : n.inEdges) {
            d.inEdges.append(ObjectGraphPortData{ p.paramName, p.occurrence, p.otherNodeIndex, p.otherName });
        }
        m_nodes.append(d);
    }

    if (m_selectedHandleValid) {
        bool stillPresent = false;
        for (const ObjectGraphNodeData& d : qAsConst(m_nodes)) {
            if (d.handle == m_selectedHandle) { stillPresent = true; break; }
        }
        if (!stillPresent) m_selectedHandleValid = false;
    }

    qreal maxX = 320.0, maxY = 220.0;
    m_nodeItems.reserve(m_nodes.size());
    for (const ObjectGraphNodeData& d : qAsConst(m_nodes)) {
        auto* item = new ObjectGraphNodeItem(d, this);
        item->setPos(d.position);
        item->setNodeSelected(m_selectedHandleValid && d.handle == m_selectedHandle);
        m_scene->addItem(item);
        m_nodeItems.append(item);
        maxX = qMax(maxX, d.position.x() + ObjectGraphMetrics::nodeWidth);
        maxY = qMax(maxY, d.position.y() + ObjectGraphMetrics::nodeHeight);
    }

    const QRectF contentRect(0.0, 0.0, maxX + ObjectGraphMetrics::padding, maxY + ObjectGraphMetrics::padding);
    const QRectF panRect = contentRect.adjusted(-300.0, -300.0, 300.0, 300.0);
    if (m_wiresLayer) m_wiresLayer->setContentRect(panRect);
    if (m_scene) m_scene->setSceneRect(panRect);

    updateHeaderCounts();
    if (m_wiresLayer) m_wiresLayer->update();

    if (!m_hasCentered && !m_nodes.isEmpty() && m_view) {
        m_hasCentered = true;
        m_view->centerOn(contentRect.center());
    }
}

void ObjectGraphCanvas::updateHeaderCounts()
{
    if (!m_countLabel) return;
    int edgeCount = 0;
    for (const ObjectGraphNodeData& d : qAsConst(m_nodes)) {
        for (const ObjectGraphPortData& p : d.outEdges) {
            if (p.otherNodeIndex >= 0) ++edgeCount;
        }
    }
    m_countLabel->setText(tr("%1 node%2, %3 edge%4")
        .arg(m_nodes.size()).arg(m_nodes.size() == 1 ? QString() : QStringLiteral("s"))
        .arg(edgeCount).arg(edgeCount == 1 ? QString() : QStringLiteral("s")));
}

// ---- object-pick spotlight (viewport/outliner -> canvas) ----------------

void ObjectGraphCanvas::refreshSpotlight(bool forceReapply)
{
    if (!m_bridge) {
        m_lastSpotlightCategory = ViewportBridge::Category::None;
        m_lastSpotlightSelectionName.clear();
        if (m_spotlightHandleValid || forceReapply) {
            m_spotlightHandleValid = false;
            applySpotlightToItems();
        }
        return;
    }

    // CHEAP pre-check, same shape NodeGraphCanvas::updateStickyFocusObject's
    // own gate uses -- see this method's own .h comment for why this is a
    // SEPARATE gate from the sticky-focus one (different field, different
    // persistence).
    const ViewportBridge::Category cat = m_bridge->selectionCategory();
    const QString cheapName = (cat == ViewportBridge::Category::Object) ? m_bridge->selectionName() : QString();
    if (!forceReapply && cat == m_lastSpotlightCategory && cheapName == m_lastSpotlightSelectionName) return;
    m_lastSpotlightCategory = cat;
    m_lastSpotlightSelectionName = cheapName;

    m_spotlightHandleValid = false;
    m_spotlightHandle = 0;
    if (cat == ViewportBridge::Category::Object) {
        // selectionRowName(), NOT selectionName() -- same "resolve back to
        // the instancing chunk" reasoning NodeGraphCanvas::refreshSpotlight's
        // identical comment documents.
        const QString rowName = m_bridge->selectionRowName();
        if (!rowName.isEmpty()) {
            for (const ObjectGraphNodeData& d : qAsConst(m_nodes)) {
                // Object category only (8) -- a Geometry node is never a
                // valid spotlight target (the external selection this
                // spotlight follows is always an Object row).
                if (d.category == 8 && d.name == rowName) {
                    m_spotlightHandle = d.handle;
                    m_spotlightHandleValid = true;
                    break;
                }
            }
        }
    }
    applySpotlightToItems();
}

void ObjectGraphCanvas::applySpotlightToItems()
{
    for (ObjectGraphNodeItem* item : qAsConst(m_nodeItems)) {
        item->setNodeSpotlit(m_spotlightHandleValid && item->data().handle == m_spotlightHandle);
    }
}

// ---- selection ------------------------------------------------------------

void ObjectGraphCanvas::selectNode(const ObjectGraphNodeData& node)
{
    if (!m_bridge) return;
    // RISE::ChunkCategory ordinals: Geometry=5, Object=8 -- see
    // ObjectGraphNodeData::category's own comment.
    const ViewportBridge::Category cat =
        (node.category == 5) ? ViewportBridge::Category::Geometry : ViewportBridge::Category::Object;
    m_bridge->setSelection(cat, node.name);
    setSelectedHandle(node.handle, true);
    // Clicking a canvas node moves the shared selection -- clear/re-derive
    // the spotlight IMMEDIATELY rather than waiting for the next preview
    // frame, same reasoning NodeGraphCanvas::selectNode's identical call
    // documents.
    refreshSpotlight(false);
    emit selectionActivated();
}

void ObjectGraphCanvas::setSelectedHandle(quint64 handle, bool valid)
{
    m_selectedHandle = handle;
    m_selectedHandleValid = valid;
    for (ObjectGraphNodeItem* item : qAsConst(m_nodeItems)) {
        item->setNodeSelected(valid && item->data().handle == handle);
    }
}

// ---- geometry helpers -------------------------------------------------

qreal ObjectGraphCanvas::currentScale() const
{
    return m_view ? m_view->currentScale() : 1.0;
}
