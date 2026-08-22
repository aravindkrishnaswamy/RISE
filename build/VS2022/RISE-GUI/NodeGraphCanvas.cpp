//////////////////////////////////////////////////////////////////////
//
//  NodeGraphCanvas.cpp - RISE UI redesign, left-panel "Graph" tab.
//
//  See NodeGraphCanvas.h for the slice scope (doc-88 Phase 3 S16+S22).
//  The PARITY TABLE mapping every macOS behavior to its Qt mirror, and
//  every deviation a Qt idiom forced, lives in one block comment at
//  the BOTTOM of this file (search "PARITY TABLE").
//
//  Author: Aravind Krishnaswamy
//  Tabs: 4
//
//  License Information: Please see the attached LICENSE.TXT file
//
//////////////////////////////////////////////////////////////////////

#include "NodeGraphCanvas.h"
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
#include <QVariant>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QLineF>
#include <QFontMetrics>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QShortcut>
#include <QKeySequence>
#include <QMessageBox>
#include <QDialog>
#include <QStackedWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QPushButton>
#include <QFileDialog>
#include <QAbstractButton>
#include <QSize>
#include <QFrame>
#include <QSet>

#include <algorithm>
#include <cmath>

// ======================================================================
// Layout constants -- mirrors NodeGraphCanvas.swift's private
// `GraphMetrics` enum. Two values differ from the Mac source (nodeHeight,
// headerHeight); see the PARITY TABLE at the bottom of this file.
// ======================================================================

namespace GraphMetrics {
constexpr qreal nodeWidth          = 172.0;
constexpr qreal nodeHeight         = 118.0;   // Mac: 108.0
constexpr qreal thumbnailSize      = 40.0;
constexpr qreal headerHeight       = 32.0;    // Mac: 22.0
constexpr qreal padding            = 80.0;
constexpr qreal labelZoomThreshold = 0.55;
constexpr qreal minScale           = 0.2;
constexpr qreal maxScale           = 2.5;
constexpr qreal outputHandleRadius = 6.0;
constexpr qreal portHitRadius      = 16.0;
}

// ======================================================================
// Free helpers -- category tint/label, port anchor geometry, wire/label
// drawing. Anonymous-namespace, but visible to the global-namespace item
// classes below via ordinary unqualified lookup within this one TU.
// ======================================================================

namespace {

/// Category-tinted header color -- the only per-node color-coding this
/// slice does (sect. 5: "No node color/shape encodes information the
/// descriptor doesn't already carry" -- category IS already carried,
/// directly off PainterGraphNode::category). Mirrors
/// NodeGraphCanvas.swift's graphCategoryTint.
QColor graphCategoryTint(int category)
{
    switch (category) {
    case 2:  return Theme::catMaterial;   // Material
    case 1:  return Theme::purple;        // Function
    default: return Theme::teal;          // Painter (0), and any unmodeled default
    }
}

QString graphCategoryLabel(int category)
{
    switch (category) {
    case 2:  return QObject::tr("Material");
    case 1:  return QObject::tr("Function");
    default: return QObject::tr("Painter");
    }
}

/// The LEFT-edge input-port anchors for a node with `outEdgeCount`
/// reference slots, spread evenly top-to-bottom in declaration order --
/// mirrors NodeGraphCanvas.swift's free function `graphInputPortAnchors`,
/// moved out of any one item class so the wires layer's paint pass and
/// NodeGraphCanvas::findPortTarget's hit-test can never drift apart;
/// both call this one function. `nodePos` is the node's CURRENT
/// (QGraphicsItem::pos(), possibly live-drag) top-left, in scene
/// (== graph-space) coordinates.
QVector<qreal> graphInputPortAnchors(const QPointF& nodePos, int outEdgeCount)
{
    const int count = qMax(outEdgeCount, 1);
    QVector<qreal> out;
    if (count <= 1) {
        out.append(nodePos.y() + GraphMetrics::nodeHeight / 2.0);
        return out;
    }
    const qreal top = nodePos.y() + GraphMetrics::headerHeight + 6.0;
    const qreal bottom = nodePos.y() + GraphMetrics::nodeHeight - 6.0;
    const qreal step = (bottom - top) / qreal(count - 1);
    out.reserve(count);
    for (int i = 0; i < count; ++i) out.append(top + step * qreal(i));
    return out;
}

void drawWire(QPainter* painter, const QPointF& from, const QPointF& to, const QColor& color, bool dashed)
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

/// Right-aligned so the label sits just left of the input port it labels
/// -- matches "param label drawn at the target end" (NodeGraphCanvas.swift
/// GraphWiresLayer.drawLabel). Uses a fixed-width box rather than
/// FontMetrics-measured text (simpler, no measurement-vs-paint drift).
void drawLabel(QPainter* painter, const QString& text, const QPointF& near, const QColor& color)
{
    painter->setPen(color);
    painter->setFont(Theme::mono(8));
    const QRectF box(near.x() - 4.0 - 130.0, near.y() - 14.0, 130.0, 12.0);
    painter->drawText(box, Qt::AlignRight | Qt::AlignVCenter, text);
}

} // namespace

// ======================================================================
// GraphNodeItem -- one node box. Forward-declared in NodeGraphCanvas.h
// as a friend, so declared here in the GLOBAL namespace (must match).
// ======================================================================

class GraphNodeItem : public QGraphicsItem
{
public:
    GraphNodeItem(const GraphNodeData& data, NodeGraphCanvas* canvas)
        : m_data(data), m_canvas(canvas)
    {
        setFlag(QGraphicsItem::ItemIsMovable, true);
        setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        setAcceptHoverEvents(true);
        setZValue(0.0);
        setToolTip(data.name);
    }

    QRectF boundingRect() const override
    {
        return QRectF(0.0, 0.0, GraphMetrics::nodeWidth, GraphMetrics::nodeHeight);
    }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    const GraphNodeData& data() const { return m_data; }

    void setNodeSelected(bool selected)
    {
        if (m_isSelected == selected) return;
        m_isSelected = selected;
        update();
    }

    /// This node is part of the CURRENT object-pick spotlight closure
    /// (NodeGraphCanvas::m_spotlightHandles) -- a visually DISTINCT state
    /// from `m_isSelected` on purpose: `m_isSelected` is this canvas's own
    /// click-to-select, while a spotlight is driven by the shared
    /// selection sitting on an OBJECT elsewhere (viewport/outliner), which
    /// this canvas must never claim as its own selection (no
    /// ViewportBridge::setSelection call from a spotlight -- see
    /// NodeGraphCanvas::refreshSpotlight). Several nodes can be spotlit at
    /// once; at most one is ever `m_isSelected`.
    void setNodeSpotlit(bool spotlit)
    {
        if (m_isSpotlit == spotlit) return;
        m_isSpotlit = spotlit;
        update();
    }

    /// Called by NodeGraphCanvas::commitNodeMove on a SUCCESSFUL layout
    /// write: `pos` becomes the new "last known good" position, so a
    /// LATER failed drag reverts to this point, not the original
    /// fetched-snapshot position.
    void commitKnownGoodPosition(const QPointF& pos) { m_data.position = pos; }
    /// Called on a FAILED layout write: snap the item's visible position
    /// back to the last known-good value.
    void revertToKnownGoodPosition() { setPos(m_data.position); }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
    {
        if (change == ItemPositionChange && scene()) {
            QPointF p = value.toPointF();
            // Positions stay >= 0 "by construction" (mirrors the Mac
            // model's own invariant, NodeGraphCanvas.swift:107) -- clamp
            // here rather than after the fact so a drag can never leave
            // a node north/west of the canvas origin.
            if (p.x() < 0.0) p.setX(0.0);
            if (p.y() < 0.0) p.setY(0.0);
            return p;
        }
        if (change == ItemPositionHasChanged && m_canvas) {
            // Live wire-repaint while dragging -- Qt's own item geometry
            // is the "did anything move" signal; see the PARITY TABLE
            // entry "Live drag position source of truth."
            m_canvas->notifyNodeGeometryChanged();
        }
        return QGraphicsItem::itemChange(change, value);
    }

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

    // A double-click always selects (Mac: "the exclusively(before:)
    // gesture combinator means the single-tap handler never ALSO fires
    // for a double-click" -- here Qt's own event delivery reproduces the
    // SAME observable behavior for free: mousePressEvent always fires
    // first and always selects, and mouseDoubleClickEvent additionally
    // fires on top of it for a real double-click, matching "select on
    // both single and double click").
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragStartPos = pos();
            if (m_canvas) m_canvas->selectNode(m_data);
        }
        QGraphicsItem::mousePressEvent(event);   // arms the move-drag (ItemIsMovable)
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override
    {
        QGraphicsItem::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton && m_canvas && pos() != m_dragStartPos) {
            m_canvas->commitNodeMove(this, pos());
        }
    }

    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (m_canvas) m_canvas->openDefs(m_data);
        QGraphicsItem::mouseDoubleClickEvent(event);
    }

private:
    void paintThumbnail(QPainter* painter, const QRectF& rect) const;
    void drawPillBadge(QPainter* painter, const QRectF& rect, const QString& text,
                        const QColor& textColor, const QColor& bgColor = QColor(0, 0, 0, 71)) const;

    GraphNodeData     m_data;
    NodeGraphCanvas*  m_canvas;
    bool              m_isSelected = false;
    bool              m_isSpotlit  = false;
    bool              m_isHovered  = false;
    QPointF           m_dragStartPos;
};

void GraphNodeItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRectF rect(0.0, 0.0, GraphMetrics::nodeWidth, GraphMetrics::nodeHeight);

    // Warm-gold spotlight glow, drawn BEHIND the card so it reads as an
    // outer halo -- a no-op (nothing drawn) when not spotlit. Qt has no
    // direct equivalent of SwiftUI's `.shadow` without a QGraphicsEffect,
    // which would apply to this item's ALREADY-DRAWN content as a whole;
    // two concentric rounded-rect strokes approximate the same "warm glow
    // around the card" read the Mac source achieves with `.shadow`.
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
    const QRectF headerRect(0.0, 0.0, GraphMetrics::nodeWidth, GraphMetrics::headerHeight);
    QColor headerFill = graphCategoryTint(m_data.category);
    headerFill.setAlphaF(0.85);
    painter->fillRect(headerRect, headerFill);
    painter->restore();

    // Fan-out / orphan badge, top-right of the header -- mutually
    // exclusive by construction (isShared needs > 1 in-edges, isOrphaned
    // needs 0), so there is never a layout conflict over which shows.
    const qreal badgeReserve = (m_data.isShared() || m_data.isOrphaned()) ? 40.0 : 6.0;

    painter->setPen(Theme::textOnAccent);
    const QFont nameFont = Theme::sans(10, QFont::DemiBold);
    painter->setFont(nameFont);
    const QFontMetrics nameFm(nameFont);
    const QRectF nameRect(8.0, 3.0, GraphMetrics::nodeWidth - 8.0 - badgeReserve, 14.0);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                       nameFm.elidedText(m_data.name, Qt::ElideRight, int(nameRect.width())));

    QColor kwColor = Theme::textOnAccent;
    kwColor.setAlphaF(0.75);
    painter->setPen(kwColor);
    const QFont kwFont = Theme::mono(8);
    painter->setFont(kwFont);
    const QFontMetrics kwFm(kwFont);
    const QRectF kwRect(8.0, 18.0, GraphMetrics::nodeWidth - 16.0, 12.0);
    painter->drawText(kwRect, Qt::AlignLeft | Qt::AlignVCenter,
                       kwFm.elidedText(m_data.chunkKeyword, Qt::ElideRight, int(kwRect.width())));

    if (m_data.isShared()) {
        drawPillBadge(painter, QRectF(GraphMetrics::nodeWidth - 36.0, 5.0, 30.0, 14.0),
                      QStringLiteral("⑂ %1").arg(m_data.inEdges.size()), Theme::textOnAccent);
    } else if (m_data.isOrphaned()) {
        drawPillBadge(painter, QRectF(GraphMetrics::nodeWidth - 52.0, 5.0, 46.0, 14.0),
                      QObject::tr("orphan"), Theme::warn);
    }

    // Content area: thumbnail + category label + def-count pill +
    // dangling-ref badge, top-to-bottom -- mirrors GraphNodeBoxView.content.
    const QRectF thumbRect(8.0, GraphMetrics::headerHeight + 8.0, GraphMetrics::thumbnailSize, GraphMetrics::thumbnailSize);
    paintThumbnail(painter, thumbRect);

    const qreal textX = thumbRect.right() + 8.0;
    qreal textY = thumbRect.top();
    const qreal textW = qMax(GraphMetrics::nodeWidth - textX - 6.0, 20.0);

    painter->setPen(Theme::textDim);
    painter->setFont(Theme::mono(8));
    painter->drawText(QRectF(textX, textY, textW, 12.0), Qt::AlignLeft | Qt::AlignVCenter,
                       graphCategoryLabel(m_data.category));
    textY += 15.0;

    if (m_data.defCount > 0) {
        const QString label = QObject::tr("%1 def%2").arg(m_data.defCount)
            .arg(m_data.defCount == 1 ? QString() : QStringLiteral("s"));
        drawPillBadge(painter, QRectF(textX, textY, qMin(qreal(66.0), textW), 14.0),
                      label, Theme::textDim, Theme::fillTrough);
        textY += 17.0;
    }

    const bool dangling = std::any_of(m_data.outEdges.cbegin(), m_data.outEdges.cend(),
                                       [](const GraphPortData& p) { return p.otherNodeIndex < 0; });
    if (dangling) {
        painter->setPen(Theme::warn);
        painter->setFont(Theme::mono(7));
        painter->drawText(QRectF(textX, textY, textW, 12.0), Qt::AlignLeft | Qt::AlignVCenter,
                           QObject::tr("dangling ref"));
    }
}

void GraphNodeItem::paintThumbnail(QPainter* painter, const QRectF& rect) const
{
    const QColor tint = graphCategoryTint(m_data.category);

    // Materials have no PainterPreview evaluation (that engine only
    // evaluates IPainter/IScalarPainter chunks) -- a neutral placeholder
    // tile, matching GraphNodeBoxView.thumbnail's own category==2 branch.
    if (m_data.category == 2) {
        QPainterPath p; p.addRoundedRect(rect, 4.0, 4.0);
        QColor fill = tint; fill.setAlphaF(0.12);
        painter->fillPath(p, fill);
        painter->setPen(tint);
        painter->setFont(Theme::sans(14));
        painter->drawText(rect, Qt::AlignCenter, QStringLiteral("◐"));
        return;
    }

    if (!m_canvas) return;
    const QImage& img = m_canvas->thumbnailFor(m_data.name);
    if (img.isNull()) {
        QPainterPath p; p.addRoundedRect(rect, 4.0, 4.0);
        QColor fill = tint; fill.setAlphaF(0.10);
        painter->fillPath(p, fill);
        return;
    }

    QRectF target = rect;
    if (m_data.isRampPainter()) {
        // Ramp strip is authored at a shorter aspect (mirrors
        // RampStripThumbnailView's height*0.45) -- center it vertically
        // in the square thumbnail slot rather than stretching it.
        const qreal h = rect.height() * 0.45;
        target = QRectF(rect.left(), rect.top() + (rect.height() - h) / 2.0, rect.width(), h);
    }
    painter->drawImage(target, img);
}

void GraphNodeItem::drawPillBadge(QPainter* painter, const QRectF& rect, const QString& text,
                                   const QColor& textColor, const QColor& bgColor) const
{
    QPainterPath p;
    p.addRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
    painter->fillPath(p, bgColor);
    painter->setPen(textColor);
    painter->setFont(Theme::mono(7, QFont::DemiBold));
    painter->drawText(rect, Qt::AlignCenter, text);
}

// ======================================================================
// GraphOutputHandleItem -- the drag-to-wire hotspot, a CHILD of its
// node so Qt's own child-item event priority isolates its gesture from
// the node's own move-drag (mirrors the Mac file header's "not children
// of the node box... so a wire-drag's own DragGesture never contends
// with the node body's reposition-drag" rule, achieved here by Qt child
// items receiving mouse events before their parent for the same point,
// rather than by a second sibling ZStack layer). Not referenced from
// NodeGraphCanvas.h, so this can live entirely in this TU.
// ======================================================================

class GraphOutputHandleItem : public QGraphicsItem
{
public:
    GraphOutputHandleItem(GraphNodeItem* parentNode, NodeGraphCanvas* canvas)
        : QGraphicsItem(parentNode), m_parentNode(parentNode), m_canvas(canvas)
    {
        // A generous 24x24 hit target (ZStack centers children by
        // default in the Mac source; here the boundingRect itself IS
        // the hit target) independent of the smaller VISIBLE circle
        // drawn on top -- mirrors NodeGraphCanvas.swift's own comment on
        // this exact tradeoff.
        setPos(GraphMetrics::nodeWidth - 12.0, GraphMetrics::nodeHeight / 2.0 - 12.0);
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::CrossCursor);
        setZValue(2.0);
        setToolTip(QObject::tr("Drag to wire \"%1\" into another node's reference").arg(parentNode->data().name));
    }

    QRectF boundingRect() const override { return QRectF(0.0, 0.0, 24.0, 24.0); }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QColor tint = graphCategoryTint(m_parentNode->data().category);
        painter->setPen(QPen(Theme::bgWell, 1.5));
        painter->setBrush(tint);
        painter->drawEllipse(QRectF(12.0 - GraphMetrics::outputHandleRadius, 12.0 - GraphMetrics::outputHandleRadius,
                                     GraphMetrics::outputHandleRadius * 2.0, GraphMetrics::outputHandleRadius * 2.0));
    }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !m_canvas) return;
        m_canvas->beginWireDrag(m_parentNode->data(), event->scenePos());
        event->accept();
    }
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (m_canvas) m_canvas->updateWireDrag(event->scenePos());
        event->accept();
    }
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override
    {
        if (m_canvas) m_canvas->endWireDrag(event->scenePos());
        event->accept();
    }

private:
    GraphNodeItem*    m_parentNode;
    NodeGraphCanvas*  m_canvas;
};

// ======================================================================
// GraphWiresLayerItem -- draws every real edge exactly once (walked via
// each node's outEdges), plus a warning-tinted stub for a dangling port,
// plus the live wire-drag draft. One low-z item covering the content
// area; forward-declared in NodeGraphCanvas.h, so global-namespace here.
// ======================================================================

class GraphWiresLayerItem : public QGraphicsItem
{
public:
    explicit GraphWiresLayerItem(NodeGraphCanvas* canvas) : m_canvas(canvas)
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
    NodeGraphCanvas* m_canvas;
    QRectF m_rect{0.0, 0.0, 400.0, 300.0};
};

void GraphWiresLayerItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*)
{
    if (!m_canvas) return;
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool showLabels = m_canvas->currentScale() >= GraphMetrics::labelZoomThreshold;
    const QVector<GraphNodeItem*>& items = m_canvas->m_nodeItems;

    for (GraphNodeItem* nodeItem : items) {
        const GraphNodeData& data = nodeItem->data();
        const QPointF nodePos = nodeItem->pos();
        const QVector<qreal> destYs = graphInputPortAnchors(nodePos, data.outEdges.size());
        for (int portIdx = 0; portIdx < data.outEdges.size() && portIdx < destYs.size(); ++portIdx) {
            const GraphPortData& port = data.outEdges[portIdx];
            const QPointF destPoint(nodePos.x(), destYs[portIdx]);
            if (port.otherNodeIndex >= 0 && port.otherNodeIndex < items.size()) {
                GraphNodeItem* srcItem = items[port.otherNodeIndex];
                const QPointF srcPos = srcItem->pos();
                const QPointF srcPoint(srcPos.x() + GraphMetrics::nodeWidth, srcPos.y() + GraphMetrics::nodeHeight / 2.0);
                QColor c = graphCategoryTint(srcItem->data().category);
                c.setAlphaF(0.75);
                drawWire(painter, srcPoint, destPoint, c, false);
                if (showLabels) drawLabel(painter, port.paramName, destPoint, Theme::textFaint);
            } else {
                // Dangling / out-of-modeled-category reference: a short
                // warning-tinted stub, no destination box.
                const QPointF stubStart(destPoint.x() - 22.0, destPoint.y());
                QColor c = Theme::warn; c.setAlphaF(0.85);
                drawWire(painter, stubStart, destPoint, c, true);
                if (showLabels) drawLabel(painter, port.paramName, destPoint, Theme::warn);
            }
        }
    }

    if (m_canvas->m_wireDragActive) {
        GraphNodeItem* srcItem = nullptr;
        for (GraphNodeItem* it : items) {
            if (it->data().handle == m_canvas->m_wireDrag.sourceHandle) { srcItem = it; break; }
        }
        if (srcItem) {
            const QPointF from(srcItem->pos().x() + GraphMetrics::nodeWidth, srcItem->pos().y() + GraphMetrics::nodeHeight / 2.0);
            const QPointF to = m_canvas->m_wireDrag.currentPoint;
            const int legal = m_canvas->m_wireDrag.verdictLegal;
            const QColor color = (legal == 0) ? Theme::errorStrong : (legal == 1) ? Theme::success : Theme::textFaint;
            drawWire(painter, from, to, color, legal != 1);
            if (m_canvas->m_wireDrag.hasTarget) {
                const QColor ringColor = (legal == 0) ? Theme::errorStrong : Theme::success;
                painter->setPen(QPen(ringColor, 2.0));
                painter->setBrush(Qt::NoBrush);
                painter->drawEllipse(m_canvas->m_wireDrag.targetAnchor, 9.0, 9.0);
            }
        }
    }
}

// ======================================================================
// NodeGraphView -- QGraphicsView subclass: wheel-zoom (clamped, anchored
// under the cursor) + panning on empty canvas space via Qt's own
// ScrollHandDrag (a press that lands on a GraphNodeItem or
// GraphOutputHandleItem is consumed by that item first -- see either
// class's mousePressEvent -- so pan only ever engages for an empty-space
// drag, mirroring the Mac file's own "a drag that starts ON a node is
// consumed by that node's own gesture before it ever reaches [pan]"
// rule, achieved here by Qt's item-before-view event precedence instead
// of SwiftUI's most-specific-view-first resolution).
// ======================================================================

class NodeGraphView : public QGraphicsView
{
public:
    explicit NodeGraphView(NodeGraphCanvas* canvas, QWidget* parent = nullptr)
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
        const qreal next = qBound(GraphMetrics::minScale, cur * factor, GraphMetrics::maxScale);
        const qreal applied = (cur > 0.0) ? next / cur : 1.0;
        if (!qFuzzyCompare(applied, qreal(1.0))) scale(applied, applied);
        event->accept();
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QGraphicsView::resizeEvent(event);
        if (m_canvas) m_canvas->repositionStatusLabel();
    }

private:
    NodeGraphCanvas* m_canvas;
};

// ======================================================================
// Add-node palette dialog (S18/S21/S22) -- mirrors
// NodeGraphPalette.swift's NodeGraphAddNodeSheet. TU-local: nothing
// outside this file constructs one directly (NodeGraphCanvas::
// openAddNodeDialog owns its one call site).
// ======================================================================

namespace {

/// A node already on the canvas, reduced to what the requirement
/// candidate picker needs. Mirrors NodeGraphPalette.swift's
/// PaletteCandidateNode.
struct PaletteCandidateNode
{
    QString name;
    QString keyword;
    int     category = -1;
};

class NodeGraphAddNodeDialog : public QDialog
{
public:
    NodeGraphAddNodeDialog(ViewportBridge* bridge, const QVector<PaletteCandidateNode>& existing, QWidget* parent)
        : QDialog(parent), m_bridge(bridge), m_existing(existing)
    {
        setWindowTitle(tr("Add Node"));
        resize(420, 480);

        auto* root = new QVBoxLayout(this);

        m_stack = new QStackedWidget(this);
        root->addWidget(m_stack, 1);

        buildKeywordPage();
        buildRequirementPage();
        m_stack->addWidget(m_keywordPage);
        m_stack->addWidget(m_requirementPage);
        m_stack->setCurrentWidget(m_keywordPage);

        m_errorLabel = new QLabel(this);
        m_errorLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::errorStrong)));
        m_errorLabel->setWordWrap(true);
        m_errorLabel->hide();
        root->addWidget(m_errorLabel);

        reloadKeywords();
    }

    QString createdName() const { return m_createdName; }
    int createdCategory() const { return m_createdCategory; }

private:
    void buildKeywordPage()
    {
        m_keywordPage = new QWidget();
        auto* v = new QVBoxLayout(m_keywordPage);

        m_categoryCombo = new QComboBox(m_keywordPage);
        m_categoryCombo->addItem(tr("Painter"), 0);
        m_categoryCombo->addItem(tr("Function"), 1);
        m_categoryCombo->addItem(tr("Material"), 2);
        v->addWidget(m_categoryCombo);

        m_searchEdit = new QLineEdit(m_keywordPage);
        m_searchEdit->setPlaceholderText(tr("Search keywords…"));
        v->addWidget(m_searchEdit);

        m_keywordList = new QListWidget(m_keywordPage);
        v->addWidget(m_keywordList, 1);

        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        auto* cancelBtn = new QPushButton(tr("Cancel"), m_keywordPage);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(cancelBtn);
        v->addLayout(buttons);

        connect(m_categoryCombo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, [this](int) { reloadKeywords(); });
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString&) { applyFilter(); });
        connect(m_keywordList, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
            selectKeyword(item->text());
        });
    }

    void buildRequirementPage()
    {
        m_requirementPage = new QWidget();
        auto* v = new QVBoxLayout(m_requirementPage);

        auto* backRow = new QHBoxLayout();
        auto* backBtn = new QPushButton(tr("← Back"), m_requirementPage);
        connect(backBtn, &QPushButton::clicked, this, [this]() {
            m_errorLabel->hide();
            m_stack->setCurrentWidget(m_keywordPage);
        });
        backRow->addWidget(backBtn);
        backRow->addStretch(1);
        v->addLayout(backRow);

        auto* nameLabel = new QLabel(tr("Name"), m_requirementPage);
        v->addWidget(nameLabel);
        m_nameEdit = new QLineEdit(m_requirementPage);
        v->addWidget(m_nameEdit);

        m_requirementsArea = new QScrollArea(m_requirementPage);
        m_requirementsArea->setWidgetResizable(true);
        m_requirementsHost = new QWidget();
        m_requirementsLayout = new QVBoxLayout(m_requirementsHost);
        m_requirementsLayout->setAlignment(Qt::AlignTop);
        m_requirementsArea->setWidget(m_requirementsHost);
        v->addWidget(m_requirementsArea, 1);

        auto* createRow = new QHBoxLayout();
        createRow->addStretch(1);
        m_createBtn = new QPushButton(tr("Create"), m_requirementPage);
        connect(m_createBtn, &QPushButton::clicked, this, &NodeGraphAddNodeDialog::create);
        createRow->addWidget(m_createBtn);
        v->addLayout(createRow);
    }

    void reloadKeywords()
    {
        m_keywords = m_bridge ? m_bridge->paletteKeywords(m_categoryCombo->currentData().toInt()) : QStringList();
        applyFilter();
    }

    void applyFilter()
    {
        m_keywordList->clear();
        const QString needle = m_searchEdit->text().trimmed().toLower();
        for (const QString& kw : qAsConst(m_keywords)) {
            if (needle.isEmpty() || kw.toLower().contains(needle)) m_keywordList->addItem(kw);
        }
    }

    struct RequirementRow
    {
        QString    param;
        QComboBox* combo = nullptr;   ///< non-null for a reference requirement
        QLineEdit* edit  = nullptr;   ///< non-null otherwise (literal or file-slot display)
    };

    void selectKeyword(const QString& keyword)
    {
        m_selectedKeyword = keyword;
        m_errorLabel->hide();
        m_nameEdit->clear();
        m_nameEdit->setPlaceholderText(keyword);

        QLayoutItem* item;
        while ((item = m_requirementsLayout->takeAt(0)) != nullptr) {
            delete item->widget();
            delete item;
        }
        m_requirementRows.clear();

        const QVector<ViewportBridge::ChunkNodeRequirement> reqs =
            m_bridge ? m_bridge->chunkNodeRequirements(keyword) : QVector<ViewportBridge::ChunkNodeRequirement>();
        for (const ViewportBridge::ChunkNodeRequirement& req : reqs) addRequirementRow(keyword, req);

        m_stack->setCurrentWidget(m_requirementPage);
        updateCreateEnabled();

        // A zero-requirement keyword creates immediately -- no reason to
        // make the user look at an empty form and press Create (mirrors
        // NodeGraphPalette.swift's selectKeyword).
        if (reqs.isEmpty()) create();
    }

    /// The S17 pure-descriptor candidate filter: every existing node
    /// whose (keyword, category) checkConnectionByKeyword accepts for
    /// THIS specific (new keyword, param) pair -- run BEFORE the new
    /// chunk exists to address by name. ADVISORY, NOT AUTHORITATIVE, the
    /// SAME caveat NodeGraphPalette.swift documents at length: the real
    /// commit-time guard is createChunkNode's own full-derivability
    /// dry-run.
    void addRequirementRow(const QString& keyword, const ViewportBridge::ChunkNodeRequirement& req)
    {
        auto* box = new QWidget(m_requirementsHost);
        auto* rowLayout = new QVBoxLayout(box);
        rowLayout->setContentsMargins(0, 6, 0, 6);

        auto* label = new QLabel(tr("%1 (required)").arg(req.param), box);
        rowLayout->addWidget(label);
        if (!req.description.isEmpty()) {
            auto* desc = new QLabel(req.description, box);
            desc->setWordWrap(true);
            rowLayout->addWidget(desc);
        }

        RequirementRow row;
        row.param = req.param;

        if (req.isReference) {
            auto* combo = new QComboBox(box);
            combo->addItem(tr("Choose…"), QString());
            for (const PaletteCandidateNode& cand : qAsConst(m_existing)) {
                QString diag;
                if (ViewportBridge::checkConnectionByKeyword(keyword, req.param, cand.keyword, cand.category, &diag)) {
                    combo->addItem(cand.name, cand.name);
                }
            }
            connect(combo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                    this, [this](int) { updateCreateEnabled(); });
            rowLayout->addWidget(combo);
            row.combo = combo;
        } else if (req.param == QLatin1String("file")) {
            // The S18 file-slot pre-flight: a required `file` param opens
            // a real file picker rather than a free-text field, so a
            // bad path can never reach the EXR/PNG/TIFF/HDR reader at
            // derive time -- that reader throws rather than returning an
            // error (mirrors NodeGraphPalette.swift's fileSlotPicker).
            auto* fileRow = new QWidget(box);
            auto* fileLayout = new QHBoxLayout(fileRow);
            fileLayout->setContentsMargins(0, 0, 0, 0);
            auto* edit = new QLineEdit(fileRow);
            edit->setReadOnly(true);
            edit->setPlaceholderText(tr("No file chosen"));
            auto* chooseBtn = new QPushButton(tr("Choose…"), fileRow);
            connect(chooseBtn, &QPushButton::clicked, this, [this, edit]() {
                const QString path = QFileDialog::getOpenFileName(
                    this, tr("Choose an image file"), QString(),
                    tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.exr *.hdr)"));
                if (!path.isEmpty()) { edit->setText(path); updateCreateEnabled(); }
            });
            fileLayout->addWidget(edit, 1);
            fileLayout->addWidget(chooseBtn);
            rowLayout->addWidget(fileRow);
            row.edit = edit;
        } else {
            auto* edit = new QLineEdit(box);
            connect(edit, &QLineEdit::textChanged, this, [this](const QString&) { updateCreateEnabled(); });
            rowLayout->addWidget(edit);
            row.edit = edit;
        }

        m_requirementsLayout->addWidget(box);
        m_requirementRows.append(row);
    }

    QString rowValue(const RequirementRow& row) const
    {
        return row.combo ? row.combo->currentData().toString() : row.edit->text();
    }

    void updateCreateEnabled()
    {
        bool allFilled = true;
        for (const RequirementRow& row : qAsConst(m_requirementRows)) {
            if (rowValue(row).isEmpty()) { allFilled = false; break; }
        }
        if (m_createBtn) m_createBtn->setEnabled(allFilled);
    }

    void create()
    {
        if (!m_bridge || m_selectedKeyword.isEmpty()) return;
        for (const RequirementRow& row : qAsConst(m_requirementRows)) {
            if (rowValue(row).isEmpty()) {
                m_errorLabel->setText(tr("Fill every required field first."));
                m_errorLabel->show();
                return;
            }
        }

        QStringList argParams, argValues;
        argParams.reserve(m_requirementRows.size());
        argValues.reserve(m_requirementRows.size());
        for (const RequirementRow& row : qAsConst(m_requirementRows)) {
            argParams.append(row.param);
            argValues.append(rowValue(row));
        }

        QString outName, outMessage;
        const QString baseName = m_nameEdit->text().trimmed();
        const bool applied = m_bridge->createChunkNode(m_selectedKeyword, baseName, argParams, argValues,
                                                         &outName, &outMessage);
        if (applied && !outName.isEmpty()) {
            m_createdName = outName;
            m_createdCategory = m_categoryCombo->currentData().toInt();
            accept();
        } else {
            m_errorLabel->setText(outMessage.isEmpty() ? tr("The node could not be created.") : outMessage);
            m_errorLabel->show();
        }
    }

    ViewportBridge*                 m_bridge;
    QVector<PaletteCandidateNode>   m_existing;

    QStackedWidget* m_stack         = nullptr;
    QWidget*        m_keywordPage   = nullptr;
    QComboBox*      m_categoryCombo = nullptr;
    QLineEdit*      m_searchEdit    = nullptr;
    QListWidget*    m_keywordList   = nullptr;
    QStringList     m_keywords;

    QWidget*         m_requirementPage   = nullptr;
    QLineEdit*       m_nameEdit          = nullptr;
    QScrollArea*     m_requirementsArea  = nullptr;
    QWidget*         m_requirementsHost  = nullptr;
    QVBoxLayout*     m_requirementsLayout = nullptr;
    QVector<RequirementRow> m_requirementRows;
    QPushButton*     m_createBtn = nullptr;
    QLabel*          m_errorLabel = nullptr;

    QString m_selectedKeyword;
    QString m_createdName;
    int     m_createdCategory = -1;
};

// ======================================================================
// Wire-drop refusal dialog -- mirrors NodeGraphCanvas.swift's
// WireRefusalPanel, as a modal QDialog rather than a floating overlay
// (see the PARITY TABLE entry "Refusal presentation").
// ======================================================================

class WireRefusalDialog : public QDialog
{
public:
    enum { DuplicateAndRetry = QDialog::Accepted + 1 };

    WireRefusalDialog(const ViewportBridge::RewireOutcome& outcome, const QString& sourceName,
                       const QString& targetName, const QString& targetParam, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(tr("Can't Wire \"%1\" In").arg(sourceName));
        auto* v = new QVBoxLayout(this);

        auto* targetLabel = new QLabel(tr("Target: %1.%2").arg(targetName, targetParam), this);
        targetLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textFaint)));
        v->addWidget(targetLabel);

        auto* msgLabel = new QLabel(outcome.message.isEmpty() ? tr("The wire was refused.") : outcome.message, this);
        msgLabel->setWordWrap(true);
        v->addWidget(msgLabel);

        if (!outcome.sharedChunks.isEmpty()) v->addWidget(makeListLabel(tr("Not solely owned:"), outcome.sharedChunks));
        if (!outcome.outOfClosureReferrers.isEmpty()) {
            v->addWidget(makeListLabel(tr("Referenced from outside:"), outcome.outOfClosureReferrers));
        }

        auto* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        auto* cancelBtn = new QPushButton(tr("Cancel"), this);
        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        buttons->addWidget(cancelBtn);

        // The Duplicate & Retry escape hatch is offered ONLY when the
        // refusal is the one case it unblocks (closure == SharedTarget,
        // S19/S20's own rule) -- mirrors WireRefusalPanel's identical gate.
        if (outcome.closure == ViewportBridge::RewireClosure::SharedTarget) {
            auto* dupBtn = new QPushButton(tr("Duplicate \"%1\" && Retry").arg(sourceName), this);
            dupBtn->setDefault(true);
            connect(dupBtn, &QPushButton::clicked, this, [this]() { done(DuplicateAndRetry); });
            buttons->addWidget(dupBtn);
        }
        v->addLayout(buttons);
    }

private:
    static QLabel* makeListLabel(const QString& title, const QStringList& items)
    {
        auto* label = new QLabel();
        label->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::hex(Theme::textDim)));
        label->setWordWrap(true);
        QString text = QStringLiteral("<b>%1</b><br>").arg(title.toHtmlEscaped());
        for (const QString& it : items) text += QStringLiteral("• %1<br>").arg(it.toHtmlEscaped());
        label->setText(text);
        return label;
    }
};

} // namespace

// ======================================================================
// NodeGraphCanvas
// ======================================================================

NodeGraphCanvas::NodeGraphCanvas(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Slim header strip -- mirrors OutlinerWidget's own header
    // construction (title + count label, "+"/refresh affordances) and
    // MainWindow::buildLeftPanel's tabStrip border-bottom idiom, since
    // this widget sits in the SAME left-panel tab stack as Agent/Scene
    // file (see this file's own header comment, "ENTRY POINT").
    m_header = new QWidget(this);
    m_header->setFixedHeight(34);
    auto* headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(14, 0, 10, 0);
    headerLayout->setSpacing(8);

    m_titleLabel = new QLabel(tr("Graph"), m_header);
    m_titleLabel->setFont(Theme::sans(12, QFont::DemiBold));
    headerLayout->addWidget(m_titleLabel);

    m_countLabel = new QLabel(m_header);
    m_countLabel->setFont(Theme::mono(10));
    headerLayout->addWidget(m_countLabel);
    headerLayout->addStretch(1);

    m_addBtn = new QToolButton(m_header);
    m_addBtn->setAutoRaise(true);
    m_addBtn->setCursor(Qt::PointingHandCursor);
    m_addBtn->setToolTip(tr("Add node"));
    m_addBtn->setIconSize(QSize(12, 12));
    m_addBtn->setEnabled(false);   // gated true by setSceneEditable
    connect(m_addBtn, &QToolButton::clicked, this, &NodeGraphCanvas::openAddNodeDialog);
    headerLayout->addWidget(m_addBtn);

    m_refreshBtn = new QToolButton(m_header);
    m_refreshBtn->setAutoRaise(true);
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setToolTip(tr("Refresh graph"));
    m_refreshBtn->setIconSize(QSize(11, 11));
    connect(m_refreshBtn, &QToolButton::clicked, this, &NodeGraphCanvas::refreshForce);
    headerLayout->addWidget(m_refreshBtn);

    // User-requested slice: "All" vs "Focused" view-scope toggle --
    // placed BEFORE the "+"/refresh cluster (mirrors NodeGraphCanvas.swift's
    // header layout, where the Picker sits between the node/edge count
    // and the add-node button). A checkable QToolButton is this app's own
    // idiomatic control for a two-state header toggle -- see the header's
    // own comment on why a segmented control has no direct single-widget
    // Qt equivalent this cheap.
    m_viewScopeBtn = new QToolButton(m_header);
    m_viewScopeBtn->setAutoRaise(true);
    m_viewScopeBtn->setCheckable(true);
    m_viewScopeBtn->setCursor(Qt::PointingHandCursor);
    m_viewScopeBtn->setText(tr("Focused"));
    m_viewScopeBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_viewScopeBtn->setToolTip(tr("All: every Painter/Material/Function chunk. Focused: only the selected object's own appearance subgraph -- clicking a node in the graph shows its details without changing what's focused."));
    connect(m_viewScopeBtn, &QToolButton::toggled, this, &NodeGraphCanvas::onViewScopeToggled);
    headerLayout->insertWidget(headerLayout->indexOf(m_addBtn), m_viewScopeBtn);   // before m_addBtn, after the stretch

    root->addWidget(m_header);

    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(0, 0, 800, 600);
    m_wiresLayer = new GraphWiresLayerItem(this);
    m_scene->addItem(m_wiresLayer);

    m_view = new NodeGraphView(this);
    m_view->setScene(m_scene);
    root->addWidget(m_view, 1);

    // Wire-drag status line -- floats over the view's viewport, shown
    // only while a wire drag is in flight. Mirrors NodeGraphCanvas.swift's
    // wireStatusLine.
    m_statusLabel = new QLabel(m_view->viewport());
    m_statusLabel->setFont(Theme::mono(10));
    m_statusLabel->hide();

    // Keyboard delete/duplicate (S20 verbs), scoped to this widget's own
    // subtree so they never contend with a global menu shortcut (mirrors
    // NodeGraphCanvas.swift's hidden-button keyboardShortcut idiom, whose
    // effective scope is "while this view has focus" -- the Qt mirror of
    // that scope is WidgetWithChildrenShortcutContext). Delete AND
    // Backspace both fire delete, matching the Mac file's
    // `.keyboardShortcut(.delete...)` / `.keyboardShortcut(.deleteForward...)`
    // pair.
    auto* delShortcut = new QShortcut(QKeySequence(QStringLiteral("Delete")), this);
    delShortcut->setContext(Qt::WidgetWithChildrenShortcutContext);
    connect(delShortcut, &QShortcut::activated, this, &NodeGraphCanvas::deleteSelected);

    auto* backspaceShortcut = new QShortcut(QKeySequence(QStringLiteral("Backspace")), this);
    backspaceShortcut->setContext(Qt::WidgetWithChildrenShortcutContext);
    connect(backspaceShortcut, &QShortcut::activated, this, &NodeGraphCanvas::deleteSelected);

    auto* dupShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+D")), this);
    dupShortcut->setContext(Qt::WidgetWithChildrenShortcutContext);
    connect(dupShortcut, &QShortcut::activated, this, &NodeGraphCanvas::duplicateSelected);

    updateHeaderCounts();

    m_themeReady = true;
    restyleTheme();
}

void NodeGraphCanvas::setBridge(ViewportBridge* bridge)
{
    m_bridge = bridge;
    m_lastFetchedEpoch = 0;
    m_hasFetchedOnce = false;
    m_hasCentered = false;
    m_selectedHandleValid = false;
    m_wireDragActive = false;
    // User-feedback review round: the sticky focus memo is EXPLICITLY
    // cleared here, unlike every other per-scene reset above which was
    // already present -- this canvas is a PERSISTENT widget (built once
    // by MainWindow, never recreated per tab-switch), and
    // rebuildViewportForLoadedScene() calls setBridge() with a BRAND NEW
    // ViewportBridge on every scene load, so this is the one reliable
    // "a scene just (re)loaded" signal available here. Without this, a
    // sticky object name from the PREVIOUS scene would silently survive
    // into the new one and try to re-resolve against it.
    m_stickyFocusHasObject = false;
    m_stickyFocusObjectName.clear();
    // Review-round P2 fix: the cheap gate that guards updateStickyFocusObject()'s
    // expensive selectionRowName() walk must clear alongside the sticky
    // memo above -- otherwise a coincidental (category, selectionName())
    // match against the PREVIOUS scene's stale gate values could skip the
    // very first re-derivation the new scene needs.
    m_lastStickyFocusCategory = ViewportBridge::Category::None;
    m_lastStickyFocusSelectionName.clear();
    hideStatusLabel();
    performReload(true);
}

void NodeGraphCanvas::setSceneEditable(bool editable)
{
    m_sceneEditable = editable;
    if (m_addBtn) m_addBtn->setEnabled(editable);
}

void NodeGraphCanvas::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange && m_themeReady && m_themeEpochSeen != Theme::paletteEpoch()) {
        restyleTheme();
    }
}

void NodeGraphCanvas::restyleTheme()
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
    if (m_addBtn) m_addBtn->setIcon(Theme::icon(QStringLiteral("plus"), 12, Theme::textDim, Theme::textDisabled));
    if (m_refreshBtn) m_refreshBtn->setIcon(Theme::icon(QStringLiteral("refresh-cw"), 11, Theme::textDim, Theme::textDisabled));
    if (m_view) m_view->setBackgroundBrush(Theme::bgWell);
    if (m_statusLabel) {
        m_statusLabel->setStyleSheet(QStringLiteral(
            "background-color: %1; color: %2; border-radius: %3px; padding: 4px 10px;")
            .arg(Theme::hex(Theme::bgCard), Theme::hex(Theme::textPrimary))
            .arg(Theme::radiusMedium));
    }

    // OutlinerWidget's exemption (Theme.h LIVE THEME-SWITCH CONTRACT
    // point 5): the node/wire ITEM layer reads every Theme:: token live
    // on each paint (no cached token baked at build time -- see
    // GraphNodeItem::paint/GraphWiresLayerItem::paint), so it needs no
    // queued rebuild, only an explicit repaint, which is genuinely the
    // whole fix per that exemption's three requirements.
    if (m_view) m_view->viewport()->update();
}

// ---- reload -----------------------------------------------------------

void NodeGraphCanvas::refresh()      { performReload(false); }
void NodeGraphCanvas::refreshForce() { performReload(true); }

void NodeGraphCanvas::performReload(bool force)
{
    if (!m_bridge) { applySnapshot(ViewportBridge::PainterGraph()); refreshSpotlight(true); return; }

    // MUST run before anything below consults the sticky focus memo
    // (currentFocusTarget/performFocusedReload) -- see its own comment.
    updateStickyFocusObject();

    if (m_viewScope == GraphViewScope::Focused) {
        int category = -1;
        QString name;
        if (currentFocusTarget(category, name)) {
            // No transition-force logic needed here (review-round P1 fix
            // on 0562b9c4, see below): performFocusedReload's own cheap
            // gate refuses to skip whenever m_lastFocusedHasTarget is
            // false, and hasTarget is exactly what the no-target branch
            // below clears -- so the "just came back from a no-target (or
            // degraded) interlude" case is already covered by that gate,
            // not by anything here.
            performFocusedReload(category, name, force);
            return;
        }
        // No selection -- a STABLE state, not a hiccup (design decision,
        // distinct from a DEGRADED focused resolve, which must NOT fall
        // back -- see performFocusedReload's own comment): fall through to
        // the ORDINARY epoch-gated All-view fetch below, which is exactly
        // right for this sub-state ("Focused with nothing to focus on" IS
        // the All-view, just reached from Focused mode). Force a fresh
        // fetch on the TRANSITION into this sub-state (the prior call had
        // a target, or this is the very first call) so it is never
        // skipped by the epoch gate; once settled, further frames defer
        // to the ordinary gate like any other All-view frame.
        if (m_lastFocusedHasTarget || !m_hasFetchedOnce) force = true;
        // All THREE focused-gate fields clear TOGETHER, one policy
        // (review-round P1 fix on 0562b9c4): m_lastFocusedHasTarget must
        // never go false while m_lastFocusedCategory/Name still hold a
        // stale target, or a later reselect of a name that happens to
        // coincide with the stale memo would wrongly skip its refetch via
        // performFocusedReload's cheap gate -- see that function's own
        // comment for the matching paired commit on the success side.
        m_lastFocusedHasTarget = false;
        m_lastFocusedCategory  = -1;
        m_lastFocusedName.clear();
    }

    const unsigned int epoch = m_bridge->sceneEpoch();
    bool rebuilt = false;
    if (force || !m_hasFetchedOnce || epoch != m_lastFetchedEpoch) {
        m_lastFetchedEpoch = epoch;
        m_hasFetchedOnce = true;
        applySnapshot(m_bridge->painterMaterialGraph());
        rebuilt = true;   // every GraphNodeItem is BRAND NEW -- none of them carry the prior spotlight state
    }
    // Independent of the epoch-gated branch above: a plain Object pick
    // does not bump sceneEpoch (see this method's own header comment), so
    // the spotlight must be re-derived on every call regardless of
    // whether the structural graph itself was just refetched.
    refreshSpotlight(rebuilt);
}

void NodeGraphCanvas::updateStickyFocusObject()
{
    if (!m_bridge) return;
    // User-feedback review round: updates the sticky memo ONLY when the
    // CURRENT shared selection is actually an Object -- a canvas-node
    // click sets the shared selection to Painter/Function/Material
    // (selectNode), which this deliberately ignores, leaving the memo
    // untouched. That asymmetry IS what makes it "sticky": the panel
    // still follows a canvas-node click (unaffected, reads the shared
    // selection directly), but the Focused subgraph does not.
    const ViewportBridge::Category cat = m_bridge->selectionCategory();
    if (cat != ViewportBridge::Category::Object) return;

    // CHEAP pre-check (review-round P2 fix) -- SAME two-tier gate
    // refreshSpotlight's own comment documents in full: selectionRowName()
    // below is an O(rows), per-row-heap-allocating tree walk, and this
    // method runs on EVERY preview frame (performReload's per-frame poll
    // cadence, not once per user gesture) -- an object sitting selected
    // through a long render must not re-pay that walk every single frame.
    // selectionRowName() is a PURE function of (selectionCategory(),
    // selectionName()), so if neither O(1) read changed since the gate
    // was last committed, its answer cannot have either. Unlike
    // refreshSpotlight's gate, there is no "degraded, leave uncommitted"
    // wrinkle here -- reading the shared selection never contends a
    // render's commit lock -- so this commits unconditionally on every
    // take of the expensive path below.
    //
    // NOTE (asymmetry with the Mac twin, intentional -- do not
    // naively port this gate in either direction): Mac's
    // `updateStickyFocusObject(bridge:)` has NO such gate, because Mac's
    // `performReload` runs only on discrete triggers (`.onAppear`, a
    // `refreshTrigger` bump, a `viewScope` toggle) -- never on a bare
    // per-frame poll -- so there is no steady-state "same object selected
    // through many frames" cost to guard against there. This canvas's
    // `performReload`, by contrast, rides `ViewportBridge::imageUpdated`
    // and genuinely fires every rendered preview frame.
    const QString cheapName = m_bridge->selectionName();
    if (cat == m_lastStickyFocusCategory && cheapName == m_lastStickyFocusSelectionName) return;
    m_lastStickyFocusCategory = cat;
    m_lastStickyFocusSelectionName = cheapName;

    const QString n = m_bridge->selectionRowName();
    // An Object-category selection with an empty resolved name (should
    // not normally happen, but mirrors NodeGraphCanvas.swift's identical
    // guard) counts as "no object" -- clears the memo rather than leaving
    // a stale one in place.
    m_stickyFocusHasObject = !n.isEmpty();
    m_stickyFocusObjectName = n;
}

bool NodeGraphCanvas::currentFocusTarget(int& outCategory, QString& outName) const
{
    // User-feedback review round: ALWAYS the sticky object memo -- the
    // PRIOR "or the canvas's own selected node" branch is removed
    // entirely, not merely bypassed, per the explicit design change
    // ("clicking a painter in focused mode narrows the view, I don't
    // want that").
    if (!m_stickyFocusHasObject) return false;
    outCategory = 8;   // RISE::ChunkCategory::Object -- see the .h's own PainterGraphNode::category comment for this numbering
    outName = m_stickyFocusObjectName;
    return true;
}

void NodeGraphCanvas::performFocusedReload(int category, const QString& name, bool force)
{
    if (!m_bridge) return;

    // Cheap pre-check (mirrors refreshSpotlight's own two-tier gate,
    // review-round P2-1 lesson): this canvas re-derives on EVERY preview
    // frame, not once per gesture -- skip the full applySnapshot rebuild
    // (tears down and recreates every GraphNodeItem) when the focus
    // target identity did not change since the last COMMITTED
    // (non-degraded) outcome. Gated on m_lastFocusedHasTarget too
    // (review-round P1 fix on 0562b9c4): a degraded resolve below
    // intentionally leaves ALL THREE gate fields exactly as they were,
    // INCLUDING hasTarget -- without this check, hasTarget could be true
    // from a stale prior success while category/name still coincidentally
    // match a just-reselected target, wrongly skipping the retry forever
    // (the P1 repro: focus A -> deselect -> reselect A mid-render).
    if (!force && m_lastFocusedHasTarget && category == m_lastFocusedCategory && name == m_lastFocusedName) return;

    bool degraded = false;
    const ViewportBridge::PainterGraph g = m_bridge->painterMaterialGraphFocused(category, name, &degraded);
    if (degraded) {
        // DEGRADED (a render owns the scene): leave ALL THREE gate fields
        // (hasTarget + the identity memo) exactly as they were -- in
        // particular hasTarget is NOT stamped true here, so the cheap gate
        // above can never wrongly treat this attempt as committed. The
        // NEXT imageUpdated frame retries the deep resolve for real -- no
        // explicit timer needed the way the Mac canvas requires
        // (NodeGraphCanvas.swift's own spotlightRetryWorkItem/
        // focusedRetryWorkItem comment explains why THAT platform needs
        // one and this one does not: this canvas already re-derives every
        // preview frame). Keep showing whatever is CURRENTLY on screen --
        // never flip to All and back, which would flicker every time a
        // render happens to be in flight.
        return;
    }
    // Commit all THREE gate fields TOGETHER, one policy (review-round P1
    // fix on 0562b9c4): hasTarget and the identity memo change in lockstep
    // -- never one without the other -- so a stale identity can never
    // coincidentally re-match after an intervening no-target interlude or
    // a degraded attempt. See performReload's no-target branch for the
    // matching paired CLEAR.
    m_lastFocusedHasTarget = true;
    m_lastFocusedCategory = category;
    m_lastFocusedName = name;
    // User-feedback review round: a genuinely empty result (reached here,
    // so NOT degraded) means the sticky object no longer resolves --
    // deleted, renamed, or (rarer) has no material bound.
    // ReadPainterMaterialGraphLaidOutFocused's Object-category case
    // cannot distinguish those cases from here, and doesn't need to:
    // either way, staying pinned to a dead name would strand the user on
    // a permanently blank canvas, so fall back to All and CLEAR the
    // sticky memo (a SEPARATE field from the three just committed above)
    // so the same dead name can't keep silently re-resolving to nothing
    // on every later frame -- the NEXT performReload call sees
    // m_stickyFocusHasObject == false, which naturally clears the three
    // gate fields too via the no-target branch.
    if (g.nodes.isEmpty()) {
        m_stickyFocusHasObject = false;
        m_stickyFocusObjectName.clear();
        applySnapshot(m_bridge->painterMaterialGraph());
        refreshSpotlight(true);
        return;
    }
    applySnapshot(g);
    refreshSpotlight(true);
}

void NodeGraphCanvas::onViewScopeToggled(bool checked)
{
    m_viewScope = checked ? GraphViewScope::Focused : GraphViewScope::All;
    // Toggling ALWAYS forces a fresh fetch, regardless of epoch -- same
    // reasoning as NodeGraphCanvas.swift's own .onChange(of: viewScope):
    // switching Focused -> All must re-fetch the all-view even when
    // sceneEpoch hasn't moved since the last (focused) fetch, or the
    // canvas would keep showing the stale focused subgraph.
    performReload(true);
}

void NodeGraphCanvas::applySnapshot(const ViewportBridge::PainterGraph& g)
{
    for (GraphNodeItem* item : qAsConst(m_nodeItems)) {
        m_scene->removeItem(item);
        delete item;   // also deletes its GraphOutputHandleItem child
    }
    m_nodeItems.clear();
    m_nodes.clear();
    m_thumbnailCache.clear();

    m_nodes.reserve(g.nodes.size());
    for (int i = 0; i < g.nodes.size(); ++i) {
        const ViewportBridge::PainterGraphNode& n = g.nodes[i];
        GraphNodeData d;
        d.handle = n.handle;
        d.index = i;
        d.name = n.name;
        d.chunkKeyword = n.chunkKeyword;
        d.category = n.category;
        d.defCount = n.defCount;
        d.position = QPointF(n.x, n.y);
        d.outEdges.reserve(n.outEdges.size());
        for (const ViewportBridge::PainterGraphPort& p : n.outEdges) {
            d.outEdges.append(GraphPortData{ p.paramName, p.occurrence, p.otherNodeIndex, p.otherName });
        }
        d.inEdges.reserve(n.inEdges.size());
        for (const ViewportBridge::PainterGraphPort& p : n.inEdges) {
            d.inEdges.append(GraphPortData{ p.paramName, p.occurrence, p.otherNodeIndex, p.otherName });
        }
        m_nodes.append(d);
    }

    // A structural refresh can retarget/invalidate an outstanding
    // selection (the node could have been deleted); dropping it is the
    // safe default -- mirrors performReload's own comment in the Mac
    // source (matches OutlinerView's stance of not second-guessing a
    // vanished selection).
    if (m_selectedHandleValid) {
        bool stillPresent = false;
        for (const GraphNodeData& d : qAsConst(m_nodes)) {
            if (d.handle == m_selectedHandle) { stillPresent = true; break; }
        }
        if (!stillPresent) m_selectedHandleValid = false;
    }

    prefetchThumbnails();

    qreal maxX = 320.0, maxY = 220.0;
    m_nodeItems.reserve(m_nodes.size());
    for (const GraphNodeData& d : qAsConst(m_nodes)) {
        auto* item = new GraphNodeItem(d, this);
        item->setPos(d.position);
        item->setNodeSelected(m_selectedHandleValid && d.handle == m_selectedHandle);
        m_scene->addItem(item);
        new GraphOutputHandleItem(item, this);   // child; no separate ownership needed
        m_nodeItems.append(item);
        maxX = qMax(maxX, d.position.x() + GraphMetrics::nodeWidth);
        maxY = qMax(maxY, d.position.y() + GraphMetrics::nodeHeight);
    }

    const QRectF contentRect(0.0, 0.0, maxX + GraphMetrics::padding, maxY + GraphMetrics::padding);
    // Extra pan margin beyond the content bounds -- the WiresLayerItem's
    // own boundingRect must cover the SAME area the scene rect does, or
    // a wire-drag draft dragged into that margin would paint outside its
    // declared bounding rect (undefined per QGraphicsItem's own docs).
    const QRectF panRect = contentRect.adjusted(-300.0, -300.0, 300.0, 300.0);
    if (m_wiresLayer) m_wiresLayer->setContentRect(panRect);
    if (m_scene) m_scene->setSceneRect(panRect);

    updateHeaderCounts();
    if (m_wiresLayer) m_wiresLayer->update();

    // First layout (or a fresh generation with no prior manual pan) gets
    // a one-shot centering pass -- mirrors centerIfNeeded's `hasCentered`
    // gate: a LATER structural refresh must NOT recenter under the
    // user's hands.
    if (!m_hasCentered && !m_nodes.isEmpty() && m_view) {
        m_hasCentered = true;
        m_view->centerOn(contentRect.center());
    }
}

void NodeGraphCanvas::prefetchThumbnails()
{
    if (!m_bridge) return;
    for (const GraphNodeData& d : qAsConst(m_nodes)) {
        if (d.category == 2) continue;   // Material: no PainterPreview
        m_thumbnailCache.insert(d.name, fetchThumbnail(d));
    }
}

void NodeGraphCanvas::updateHeaderCounts()
{
    if (!m_countLabel) return;
    int edgeCount = 0;
    for (const GraphNodeData& d : qAsConst(m_nodes)) {
        for (const GraphPortData& p : d.outEdges) {
            if (p.otherNodeIndex >= 0) ++edgeCount;
        }
    }
    m_countLabel->setText(tr("%1 node%2, %3 edge%4")
        .arg(m_nodes.size()).arg(m_nodes.size() == 1 ? QString() : QStringLiteral("s"))
        .arg(edgeCount).arg(edgeCount == 1 ? QString() : QStringLiteral("s")));
}

// ---- object-pick spotlight (viewport/outliner -> canvas) --------------

void NodeGraphCanvas::refreshSpotlight(bool forceReapply)
{
    if (!m_bridge) {
        m_lastSpotlightCategory = ViewportBridge::Category::None;
        m_lastSpotlightSelectionName.clear();
        m_lastSpotlightObjectName.clear();
        if (!m_spotlightHandles.isEmpty() || forceReapply) {
            m_spotlightHandles.clear();
            applySpotlightToItems();
        }
        return;
    }

    // CHEAP pre-check first. selectionCategory() and selectionName() are
    // both O(1) field reads; selectionRowName() below is an O(rows),
    // per-row-heap-allocating tree walk (SelectionRowName ->
    // ResolveTreeRowName's fold-chain search) -- and this method runs on
    // EVERY preview frame (performReload's own per-frame poll cadence),
    // not once per user gesture. selectionRowName() is a PURE function of
    // (selectionCategory(), selectionName()): if NEITHER changed since the
    // cheap-gate memo was last COMMITTED, its answer cannot have either,
    // so the expensive walk is safe to skip.
    //
    // NOT COMMITTED YET (review-round P2-1 fix): a prior draft stamped
    // m_lastSpotlightCategory/m_lastSpotlightSelectionName here,
    // unconditionally, before ever attempting the deep resolve below. That
    // poisoned retries: if appearanceClosureForObject() degrades to empty
    // because a render currently owns the commit lock
    // (AppearanceClosureForObject's own try_to_lock contract), the cheap
    // gate would already show "nothing changed" on every SUBSEQUENT
    // preview frame (selectionCategory()/selectionName() genuinely are
    // unchanged -- the same object is still selected), so the expensive
    // path -- and therefore the retry -- would never run again, even after
    // the render finished and a real resolve became possible. The gate is
    // now committed ONLY alongside a REAL, RESOLVED deep-resolve outcome
    // (see below -- "resolved" includes a genuinely empty closure, not
    // only a non-empty one); a DEGRADED attempt leaves it uncommitted so
    // the NEXT imageUpdated frame retries for real.
    const ViewportBridge::Category cat = m_bridge->selectionCategory();
    const QString cheapName = (cat == ViewportBridge::Category::Object) ? m_bridge->selectionName() : QString();
    if (!forceReapply && cat == m_lastSpotlightCategory && cheapName == m_lastSpotlightSelectionName) return;

    // selectionRowName(), NOT selectionName(): a viewport/outliner pick on
    // a count_u/count_v repeated or subtree-copied instance names a
    // SYNTHESIZED per-repetition/per-member entity (`I[1,0]`/`I.child`)
    // that is never itself an addressable chunk -- selectionRowName() is
    // the same "resolve back to the instancing chunk's own row" the
    // outliner already performs, which is what
    // appearanceClosureForObject() needs.
    const QString objectName = (cat == ViewportBridge::Category::Object) ? m_bridge->selectionRowName() : QString();

    QSet<quint64> handles;
    GraphNodeItem* primaryItem = nullptr;
    // Auto-scroll must fire only when the ROW-NAME identity of the
    // external selection actually CHANGED since the last SUCCESSFUL
    // resolve, NOT on a forceReapply-only pass (a structural edit/rewire
    // while the SAME object stays selected must not yank the view out
    // from under an in-progress canvas edit). Defaults to false; only set
    // true in the successful-resolve branch below.
    bool selectionChanged = false;
    if (cat == ViewportBridge::Category::Object && !objectName.isEmpty()) {
        // (category, name) matching, NOT name alone (review-round P1 fix):
        // a Painter and a Material chunk may legally share a name, so
        // matching this closure's entries against m_nodes by name only
        // could silently spotlight the wrong node on a cross-category
        // collision.
        //
        // DEGRADED VS GENUINELY-EMPTY IS LOAD-BEARING (a later external
        // review round caught a P1 in the prior fix, which treated any
        // empty `closureEntries` as "not yet resolved" and left the cheap
        // gate uncommitted -- so a genuinely material-less object paid a
        // full deep resolve on EVERY preview frame forever, since nothing
        // ever told this code the empty answer was final). `degraded` is
        // `appearanceClosureForObject`'s explicit out-param (surfacing
        // `SceneEditController::AppearanceClosureForObject`'s
        // `outDegraded` across the bridge) -- `true` ONLY when the commit
        // lock was contended and this call could not even ATTEMPT a real
        // answer; `false` on a genuine resolution, whether or not the
        // closure itself came back empty.
        bool degraded = false;
        const QVector<ViewportBridge::AppearanceClosureEntry> closureEntries = m_bridge->appearanceClosureForObject(objectName, &degraded);
        if (!degraded) {
            // REAL, RESOLVED answer -- commit BOTH memos now, whether the
            // closure is non-empty (success -- the loop below populates
            // `handles`) or genuinely empty (nothing to spotlight -- the
            // loop below is simply a no-op over an empty vector). Either
            // way this is a final answer for this selection: the cheap
            // gate can safely skip the expensive walk on a later
            // steady-state frame, and the object-identity memo can safely
            // gate the auto-scroll-on-change decision.
            m_lastSpotlightCategory = cat;
            m_lastSpotlightSelectionName = cheapName;
            selectionChanged = (objectName != m_lastSpotlightObjectName);
            m_lastSpotlightObjectName = objectName;
            for (const ViewportBridge::AppearanceClosureEntry& entry : closureEntries) {
                for (int i = 0; i < m_nodes.size(); ++i) {
                    if (m_nodes[i].category != entry.category || m_nodes[i].name != entry.name) continue;
                    handles.insert(m_nodes[i].handle);
                    // First entry = the object's bound material, per
                    // appearanceClosureForObject's own contract -- the
                    // auto-scroll target.
                    if (!primaryItem && i < m_nodeItems.size()) primaryItem = m_nodeItems[i];
                    break;
                }
            }
        } else {
            // DEGRADED: this call could not even attempt a real answer, so
            // CLEAR all three memos rather than committing a placeholder.
            // Clearing (not merely "leaving alone") matters for a THIRD
            // scenario beyond the render-in-flight one: object A resolves
            // and is spotlit, the user picks object B while a render
            // degrades B's resolve, then picks A again -- with the object
            // memo left at "A" from the first resolve, re-selecting A
            // would read as "unchanged" and skip the auto-scroll entirely.
            // Clearing makes every post-degrade re-selection (including of
            // the SAME object) a fresh change. Leaving the cheap-gate memo
            // UNCOMMITTED (cleared, not stamped) is what makes the NEXT
            // imageUpdated frame retry the deep resolve instead of
            // short-circuiting above -- this canvas re-derives every
            // preview frame, so retrying while contended costs nothing
            // beyond that frame's own deep resolve, self-terminating the
            // moment the resolve stops degrading.
            m_lastSpotlightCategory = ViewportBridge::Category::None;
            m_lastSpotlightSelectionName.clear();
            m_lastSpotlightObjectName.clear();
        }
    } else {
        // Selection genuinely moved away from Object (or there is none) --
        // a REAL, resolved transition, not a degraded read, so commit the
        // cheap gate and clear the object memo for real.
        m_lastSpotlightCategory = cat;
        m_lastSpotlightSelectionName = cheapName;
        m_lastSpotlightObjectName.clear();
    }
    m_spotlightHandles = handles;
    applySpotlightToItems();

    // Auto-scroll (never re-zoom) so the primary node is visible --
    // QGraphicsView::ensureVisible is already a no-op when the item is
    // fully visible, matching centerIfNeeded's "don't move the view out
    // from under the user" posture on the Mac side without needing any
    // bespoke geometry math here.
    if (selectionChanged && primaryItem && m_view) m_view->ensureVisible(primaryItem);
}

void NodeGraphCanvas::applySpotlightToItems()
{
    for (GraphNodeItem* item : qAsConst(m_nodeItems)) {
        item->setNodeSpotlit(m_spotlightHandles.contains(item->data().handle));
    }
}

// ---- selection / def-focus (sect. 5 interaction minimums 1 & 2) -------

void NodeGraphCanvas::selectNode(const GraphNodeData& node)
{
    if (!m_bridge) return;
    // ChunkCategory::Function(1) has no UI section of its own; the
    // C++ selection-routing convention (UiCategoryForChunkCategory)
    // maps it onto Painter -- mirrors selectNode's identical comment
    // in NodeGraphCanvas.swift.
    const ViewportBridge::Category cat =
        (node.category == 2) ? ViewportBridge::Category::Material : ViewportBridge::Category::Painter;
    m_bridge->setSelection(cat, node.name);
    setSelectedHandle(node.handle, true);
    // review-round P1 fix: per the spotlight contract (NODE_GRAPH_CANVAS.md
    // sect. 5's interaction minimums), clicking a canvas node moves the
    // shared selection to a non-Object category, so any live spotlight must
    // clear IMMEDIATELY -- not wait for the next per-frame performReload()
    // poll (which, on Mac, IS immediate via the refreshTrigger bump this
    // same click already causes; this platform's poll has no such
    // synchronous companion, so it needs an explicit call here).
    refreshSpotlight(false);
    // User-feedback review round: NO forced Focused-mode refetch here
    // anymore -- this used to force one (via a deferred QTimer::singleShot,
    // to dodge a use-after-free: applySnapshot() deletes every
    // GraphNodeItem, including the one whose mousePressEvent is still on
    // the call stack) because the canvas's own node selection USED TO BE
    // a focus-target source, so a node click could retarget the subgraph.
    // It no longer is (currentFocusTarget() is the STICKY object memo
    // only, updateStickyFocusObject()'s own comment) -- clicking a canvas
    // node now changes only the shared selection (panel follows it,
    // handled above/below) and never the focused subgraph, so there is
    // nothing here left to refetch. This is precisely the behavior the
    // user asked for: "clicking a painter should still bring up the
    // painter details in the panel" without narrowing the graph.
    emit selectionActivated();
}

void NodeGraphCanvas::setSelectedHandle(quint64 handle, bool valid)
{
    m_selectedHandle = handle;
    m_selectedHandleValid = valid;
    for (GraphNodeItem* item : qAsConst(m_nodeItems)) {
        item->setNodeSelected(valid && item->data().handle == handle);
    }
}

void NodeGraphCanvas::openDefs(const GraphNodeData& node)
{
    // A double-click always selects -- the def-focus step is a
    // documented no-op on this platform today: no Qt equivalent of the
    // Mac reference's RenderViewModel.focusPainterDefRows scroll-anchor
    // exists in ViewportProperties. The property panel already lists
    // every def[i] row as an ordinary row once the node is selected, so
    // the information is one click away either way -- see the PARITY
    // TABLE entry "Double-click def-focus" for the full rationale and
    // the extension point a future ViewportProperties change would hook
    // here.
    selectNode(node);
}

// ---- drag-to-reposition (S21/S22) --------------------------------------

void NodeGraphCanvas::notifyNodeGeometryChanged()
{
    if (m_wiresLayer) m_wiresLayer->update();
}

void NodeGraphCanvas::commitNodeMove(GraphNodeItem* item, const QPointF& newPos)
{
    // FOCUSED MODE NEVER COMMITS (user-requested view-scope slice):
    // .risegraph.json sidecar positions belong to the All-view's
    // persisted layout ONLY -- a focused subgraph's layout is transient,
    // recomputed fresh on every focused fetch (ReadPainterMaterialGraphLaidOutFocused's
    // own contract), so writing a drag's endpoint into the sidecar while
    // focused would silently corrupt the all-view's layout with a
    // position that has no business there. Least-code choice matching
    // NodeGraphCanvas.swift's identical decision: the drag still plays
    // out visually (ItemIsMovable, unchanged), but skipping the write
    // here and reverting means the node simply snaps back to its
    // transient auto-layout position once released -- never a dialog,
    // never a sidecar write.
    if (m_viewScope == GraphViewScope::Focused) {
        if (item) item->revertToKnownGoodPosition();
        return;
    }

    // Deliberately NOT gated on m_sceneEditable -- writeGraphNodeLayoutPosition
    // touches only the layout sidecar file, never the CST document, and
    // refuses on its own (returns false) while a render owns the scene.
    // Mirrors NodeGraphCanvas.swift's handleNodeDragEnded, which performs
    // the identical unconditional call and handles the boolean result --
    // see the PARITY TABLE entry "Reposition-write gating."
    if (!m_bridge || !item) return;
    QString err;
    const bool ok = m_bridge->writeGraphNodeLayoutPosition(item->data().name, newPos.x(), newPos.y(), &err);
    if (ok) {
        item->commitKnownGoodPosition(newPos);
        for (GraphNodeData& d : m_nodes) {
            if (d.handle == item->data().handle) { d.position = newPos; break; }
        }
    } else {
        item->revertToKnownGoodPosition();
        QMessageBox::critical(this, tr("Couldn't Save Position"),
            err.isEmpty() ? tr("The position was not saved.") : err);
    }
}

// ---- drag-to-wire (S21/S22) ---------------------------------------------

void NodeGraphCanvas::beginWireDrag(const GraphNodeData& source, const QPointF& scenePt)
{
    m_wireDragActive = true;
    m_wireDrag = WireDragState{};
    m_wireDrag.sourceHandle = source.handle;
    m_wireDrag.sourceName = source.name;
    m_wireDrag.sourceCategory = source.category;
    m_wireDrag.currentPoint = scenePt;
    updateStatusLabel();
    if (m_wiresLayer) m_wiresLayer->update();
}

void NodeGraphCanvas::updateWireDrag(const QPointF& scenePt)
{
    if (!m_wireDragActive) return;
    m_wireDrag.currentPoint = scenePt;

    WirePortTarget target;
    const bool found = findPortTarget(scenePt, target);
    bool targetChanged = false;
    if (found) {
        targetChanged = !m_wireDrag.hasTarget
            || m_wireDrag.targetHandle != target.nodeHandle
            || m_wireDrag.targetParam != target.paramName
            || m_wireDrag.targetOccurrence != target.occurrence;
        m_wireDrag.hasTarget = true;
        m_wireDrag.targetHandle = target.nodeHandle;
        m_wireDrag.targetName = target.nodeName;
        m_wireDrag.targetCategory = target.nodeCategory;
        m_wireDrag.targetParam = target.paramName;
        m_wireDrag.targetOccurrence = target.occurrence;
        m_wireDrag.targetAnchor = target.anchor;
    } else if (m_wireDrag.hasTarget) {
        targetChanged = true;
        m_wireDrag.hasTarget = false;
    }

    // Debounces the legality re-check so a fast drag across several
    // ports doesn't fire a checkConnection call every pixel -- only when
    // the hovered PORT actually changes. Mirrors updateWireVerdict's own
    // `lastCheckedTarget` gate (folded in here since Qt's call is
    // synchronous, so there is no separate "stale answer" race to guard
    // against -- see the PARITY TABLE entry "Wire-verdict staleness
    // guard").
    if (targetChanged) {
        m_wireDrag.verdictLegal = -1;
        m_wireDrag.verdictMessage.clear();
        if (m_wireDrag.hasTarget && m_bridge) {
            QString diag;
            const bool legal = m_bridge->checkConnection(
                m_wireDrag.targetCategory, m_wireDrag.targetName, m_wireDrag.targetParam,
                m_wireDrag.sourceCategory, m_wireDrag.sourceName, &diag);
            m_wireDrag.verdictLegal = legal ? 1 : 0;
            m_wireDrag.verdictMessage = legal ? QString() : (diag.isEmpty() ? tr("Not a legal connection") : diag);
        }
    }

    updateStatusLabel();
    if (m_wiresLayer) m_wiresLayer->update();
}

void NodeGraphCanvas::endWireDrag(const QPointF& scenePt)
{
    Q_UNUSED(scenePt);
    if (!m_wireDragActive) return;
    const WireDragState drag = m_wireDrag;
    m_wireDragActive = false;
    hideStatusLabel();
    if (m_wiresLayer) m_wiresLayer->update();

    if (!drag.hasTarget || !m_bridge || !m_sceneEditable) return;
    commitRewire(drag.sourceHandle, drag.sourceName, drag.sourceCategory,
                 drag.targetHandle, drag.targetName, drag.targetCategory,
                 drag.targetParam, drag.targetOccurrence);
}

bool NodeGraphCanvas::findPortTarget(const QPointF& scenePt, WirePortTarget& outTarget) const
{
    bool found = false;
    qreal bestDist = 0.0;
    // Content-space threshold that maps back to a constant ~16pt
    // ON-SCREEN radius regardless of zoom -- mirrors portHitRadius's own
    // comment in NodeGraphCanvas.swift almost verbatim; currentScale()
    // is clamped to [minScale, maxScale] by construction (NodeGraphView::
    // wheelEvent), so this never divides by zero.
    const qreal hitRadius = GraphMetrics::portHitRadius / qMax(currentScale(), 0.0001);

    for (GraphNodeItem* item : qAsConst(m_nodeItems)) {
        const GraphNodeData& data = item->data();
        const QPointF nodePos = item->pos();
        const QVector<qreal> anchors = graphInputPortAnchors(nodePos, data.outEdges.size());
        for (int idx = 0; idx < anchors.size() && idx < data.outEdges.size(); ++idx) {
            const QPointF anchor(nodePos.x(), anchors[idx]);
            const qreal dist = QLineF(anchor, scenePt).length();
            if (dist > hitRadius) continue;
            if (!found || dist < bestDist) {
                found = true;
                bestDist = dist;
                const GraphPortData& port = data.outEdges[idx];
                outTarget.nodeHandle = data.handle;
                outTarget.nodeName = data.name;
                outTarget.nodeCategory = data.category;
                outTarget.paramName = port.paramName;
                outTarget.occurrence = port.occurrence;
                outTarget.anchor = anchor;
            }
        }
    }
    return found;
}

void NodeGraphCanvas::commitRewire(quint64 sourceHandle, const QString& sourceName, int sourceCategory,
                                    quint64 targetHandle, const QString& targetName, int targetCategory,
                                    const QString& param, int occurrence)
{
    if (!m_bridge) return;
    ViewportBridge::RewireOutcome outcome;
    const bool applied = m_bridge->rewireConnection(targetCategory, targetName, param, occurrence,
                                                      sourceCategory, sourceName, &outcome);
    if (applied) {
        m_bridge->setSelection(
            targetCategory == 2 ? ViewportBridge::Category::Material : ViewportBridge::Category::Painter, targetName);
        setSelectedHandle(targetHandle, true);
        emit selectionActivated();
        // The orphan badge is snapshot-derived (GraphNodeData::isOrphaned),
        // so it falls out of the forced refresh below for free -- no
        // separate "which node just got orphaned" event plumbing needed,
        // mirroring commitRewire's own P2-1 comment in the Mac source.
        refreshForce();
    } else {
        showWireRefusal(outcome, sourceHandle, sourceName, sourceCategory,
                         targetHandle, targetName, targetCategory, param, occurrence);
    }
}

void NodeGraphCanvas::showWireRefusal(const ViewportBridge::RewireOutcome& outcome,
                                       quint64 sourceHandle, const QString& sourceName, int sourceCategory,
                                       quint64 targetHandle, const QString& targetName, int targetCategory,
                                       const QString& param, int occurrence)
{
    WireRefusalDialog dlg(outcome, sourceName, targetName, param, this);
    const int result = dlg.exec();
    if (result != WireRefusalDialog::DuplicateAndRetry) return;
    if (!m_bridge || !m_sceneEditable) return;

    ViewportBridge::DuplicateOutcome dup;
    const bool applied = m_bridge->duplicateGraphNode(sourceCategory, sourceName, &dup);
    if (!applied || dup.newName.isEmpty()) {
        QMessageBox::warning(this, tr("Couldn't Duplicate \"%1\"").arg(sourceName),
            dup.message.isEmpty() ? tr("The duplicate was refused.") : dup.message);
        return;
    }
    refreshForce();
    commitRewire(sourceHandle, dup.newName, sourceCategory, targetHandle, targetName, targetCategory, param, occurrence);
}

void NodeGraphCanvas::updateStatusLabel()
{
    if (!m_statusLabel) return;
    QString text;
    if (!m_wireDrag.hasTarget) {
        text = tr("Drag onto an input port to wire \"%1\" in").arg(m_wireDrag.sourceName);
    } else if (m_wireDrag.verdictLegal == 0) {
        text = tr("%1.%2: %3").arg(m_wireDrag.targetName, m_wireDrag.targetParam, m_wireDrag.verdictMessage);
    } else {
        text = tr("Wire into %1.%2").arg(m_wireDrag.targetName, m_wireDrag.targetParam);
    }
    m_statusLabel->setText(text);
    m_statusLabel->adjustSize();
    m_statusLabel->show();
    repositionStatusLabel();
}

void NodeGraphCanvas::hideStatusLabel()
{
    if (m_statusLabel) m_statusLabel->hide();
}

void NodeGraphCanvas::repositionStatusLabel()
{
    if (!m_statusLabel || !m_view || !m_statusLabel->isVisible()) return;
    const QSize vp = m_view->viewport()->size();
    const QSize sz = m_statusLabel->sizeHint();
    m_statusLabel->move((vp.width() - sz.width()) / 2, vp.height() - sz.height() - 14);
}

// ---- delete / duplicate (S20 verbs) --------------------------------------

GraphNodeData* NodeGraphCanvas::selectedNodeMutable()
{
    if (!m_selectedHandleValid) return nullptr;
    for (GraphNodeData& d : m_nodes) {
        if (d.handle == m_selectedHandle) return &d;
    }
    return nullptr;
}

void NodeGraphCanvas::deleteSelected()
{
    if (!m_bridge || !m_sceneEditable) return;
    GraphNodeData* node = selectedNodeMutable();
    if (!node) return;
    const QString name = node->name;
    const int category = node->category;
    const quint64 handle = node->handle;

    // The confirm dialog below is what offers cascade explicitly (mirrors
    // deleteSelected's "cascade ONLY behind an explicit confirm" rule in
    // the Mac source, and MainWindow::removeEntity's own confirm-then-
    // re-check-the-gate pattern this function otherwise mirrors).
    QMessageBox confirm(this);
    confirm.setWindowTitle(tr("Delete \"%1\"?").arg(name));
    confirm.setText(tr("Only this node is removed. Any painter it solely owned that becomes "
                        "unreferenced is left in place as an orphan, unless you choose Delete + "
                        "Remove Unused Below. This can be undone with Edit > Undo."));
    confirm.setIcon(QMessageBox::Warning);
    auto* deleteBtn = confirm.addButton(tr("Delete"), QMessageBox::AcceptRole);
    auto* cascadeBtn = confirm.addButton(tr("Delete + Remove Unused Below"), QMessageBox::DestructiveRole);
    confirm.addButton(QMessageBox::Cancel);
    confirm.exec();
    QAbstractButton* clicked = confirm.clickedButton();
    if (clicked != deleteBtn && clicked != cascadeBtn) return;
    const bool doCascade = (clicked == cascadeBtn);

    // Re-check the edit gate AFTER the modal returns -- QMessageBox::exec
    // blocks this function's caller but not Qt's own event processing, so
    // a chat-driven agent render can begin or end while it's up (mirrors
    // MainWindow::removeEntity's identical re-check).
    if (!m_sceneEditable) return;

    ViewportBridge::DeleteOutcome outcome;
    const bool applied = m_bridge->deleteGraphNode(
        category, name, doCascade ? ViewportBridge::GraphDeleteMode::Cascade : ViewportBridge::GraphDeleteMode::TargetOnly,
        &outcome);
    if (applied) {
        if (m_selectedHandleValid && m_selectedHandle == handle) m_selectedHandleValid = false;
        emit selectionActivated();
        refreshForce();
        if (outcome.removed.size() > 1) {
            QMessageBox::information(this, tr("Removed %1 chunks").arg(outcome.removed.size()),
                outcome.removed.join(QStringLiteral(", ")));
        }
    } else {
        QMessageBox::warning(this, tr("Couldn't Delete \"%1\"").arg(name),
            outcome.message.isEmpty() ? tr("The delete was refused.") : outcome.message);
    }
}

void NodeGraphCanvas::duplicateSelected()
{
    if (!m_bridge || !m_sceneEditable) return;
    GraphNodeData* node = selectedNodeMutable();
    if (!node) return;
    const QString name = node->name;
    const int category = node->category;

    ViewportBridge::DuplicateOutcome outcome;
    const bool applied = m_bridge->duplicateGraphNode(category, name, &outcome);
    if (applied) {
        refreshForce();
        if (!outcome.newName.isEmpty()) {
            m_bridge->setSelection(
                category == 2 ? ViewportBridge::Category::Material : ViewportBridge::Category::Painter, outcome.newName);
        }
    } else {
        QMessageBox::warning(this, tr("Couldn't Duplicate \"%1\"").arg(name),
            outcome.message.isEmpty() ? tr("The duplicate was refused.") : outcome.message);
    }
}

// ---- add-node palette (S18/S21/S22) --------------------------------------

void NodeGraphCanvas::openAddNodeDialog()
{
    if (!m_bridge || !m_sceneEditable) return;

    // Convert the viewport's on-screen center into content-space so a
    // newly created node lands where the user is actually looking --
    // mirrors openPalette's identical inverse-transform comment (Qt's
    // QGraphicsView::mapToScene does this natively, where the Mac source
    // has to invert its own pan/zoom state by hand).
    QPointF dropPoint(60.0, 60.0);
    if (m_view && m_view->viewport()->width() > 1 && m_view->viewport()->height() > 1) {
        const QPointF centerScene = m_view->mapToScene(m_view->viewport()->rect().center());
        dropPoint = QPointF(centerScene.x() - GraphMetrics::nodeWidth / 2.0,
                             centerScene.y() - GraphMetrics::nodeHeight / 2.0);
    }

    QVector<PaletteCandidateNode> candidates;
    candidates.reserve(m_nodes.size());
    for (const GraphNodeData& d : qAsConst(m_nodes)) {
        if (d.category == 2) continue;   // a Material can never fill a painter reference slot
        candidates.append(PaletteCandidateNode{ d.name, d.chunkKeyword, d.category });
    }

    NodeGraphAddNodeDialog dlg(m_bridge, candidates, this);
    if (dlg.exec() != QDialog::Accepted || dlg.createdName().isEmpty()) return;

    // The sheet already committed via createChunkNode; this only (a)
    // persists the drop-point position (create-time auto-layout would
    // otherwise place it by rank, ignoring where the user asked for it)
    // and (b) selects + refreshes -- mirrors didCreateNode exactly.
    //
    // SKIP THE SIDECAR WRITE WHILE FOCUSED (review-round P3-2 fix on
    // 0562b9c4): `dropPoint` was computed from `m_view`'s CURRENT pan/zoom,
    // which while Focused is showing the transient focused-subgraph layout
    // space, not the All-view's persisted coordinate space -- writing it
    // into the sidecar would land the new node at a nonsense position once
    // the user toggles back to All. Let the new node fall back to its
    // natural rank-layout position in the All view instead (the same thing
    // that already happens to any node with no sidecar entry) -- same
    // least-code choice as commitNodeMove's own Focused-mode skip.
    if (m_viewScope != GraphViewScope::Focused) {
        QString err;
        m_bridge->writeGraphNodeLayoutPosition(dlg.createdName(), dropPoint.x(), dropPoint.y(), &err);
    }
    m_bridge->setSelection(
        dlg.createdCategory() == 2 ? ViewportBridge::Category::Material : ViewportBridge::Category::Painter,
        dlg.createdName());
    emit selectionActivated();
    refreshForce();
}

// ---- thumbnails (S10 Qt carry) --------------------------------------------

QImage NodeGraphCanvas::fetchThumbnail(const GraphNodeData& node) const
{
    if (!m_bridge || node.category == 2) return QImage();
    QByteArray rgba;
    if (node.isRampPainter()) {
        const unsigned int w = 40, h = 18;
        if (!m_bridge->rampStripPreview(node.name, w, h, rgba)) return QImage();
        if (static_cast<unsigned int>(rgba.size()) != w * h * 4) return QImage();
        const QImage img(reinterpret_cast<const uchar*>(rgba.constData()), int(w), int(h), QImage::Format_RGBA8888);
        return img.copy();   // detach: `rgba` is about to go out of scope
    }
    const unsigned int w = 40, h = 40;
    if (!m_bridge->painterPreview(node.name, -1, w, h, rgba)) return QImage();
    if (static_cast<unsigned int>(rgba.size()) != w * h * 4) return QImage();
    const QImage img(reinterpret_cast<const uchar*>(rgba.constData()), int(w), int(h), QImage::Format_RGBA8888);
    return img.copy();
}

const QImage& NodeGraphCanvas::thumbnailFor(const QString& name) const
{
    static const QImage kEmpty;
    const auto it = m_thumbnailCache.constFind(name);
    return it != m_thumbnailCache.constEnd() ? it.value() : kEmpty;
}

// ---- geometry -------------------------------------------------------------

qreal NodeGraphCanvas::currentScale() const
{
    return m_view ? m_view->currentScale() : 1.0;
}

// ======================================================================
// PARITY TABLE -- doc-88 Phase 3 S16+S22 (docs/gui/NODE_GRAPH_CANVAS.md
// sect. 6). Maps every macOS reference behavior (build/XCode/rise/
// RISE-GUI/App/NodeGraphCanvas.swift + NodeGraphPalette.swift) to its
// Windows/Qt mirror in THIS file, and calls out every place a Qt idiom
// forced a deviation. Read this before the owed MSVC-verification pass
// reviews this slice -- it is the checklist that session should work
// against.
//
// | # | Mac behavior                              | Qt mirror                                         | Deviation / note |
// |---|--------------------------------------------|----------------------------------------------------|-------------------|
// | 1 | SwiftUI `Canvas` node boxes + bezier wires  | `QGraphicsScene`/`QGraphicsView`, `GraphNodeItem` + `GraphWiresLayerItem` | Prescribed by NODE_GRAPH_CANVAS.md sect. 3 for Windows; no deviation. |
// | 2 | Node header: name + keyword, category tint | `GraphNodeItem::paint` header bar, `graphCategoryTint`/`graphCategoryLabel` | `headerHeight` 22→32, `nodeHeight` 108→118: SwiftUI's Text auto-shrinks two stacked lines into a fixed-height container; Qt's `QPainter::drawText` does not, so the header grew ~10pt to fit "name" + "keyword" as two legible lines without a multi-pass fit algorithm. Content-area height (nodeHeight − headerHeight) is UNCHANGED at 86pt either way. |
// | 3 | Fan-out badge (`isShared`, >1 in-edges)     | `GraphNodeItem::paint`, `GraphNodeData::isShared()` | Direct port. |
// | 4 | Orphan badge (snapshot-derived, not event-sourced) | `GraphNodeData::isOrphaned()`, recomputed every `applySnapshot` | Direct port, including the "why recomputed, not event-sourced" rationale (see GraphNodeData::isOrphaned's own comment). |
// | 5 | Def-count pill                              | `GraphNodeItem::paint` def-count pill | Direct port. |
// | 6 | Dangling-reference badge                    | `GraphNodeItem::paint`, `std::any_of` over `outEdges` | Direct port. |
// | 7 | Thumbnail via `PainterThumbnailView`/`RampStripThumbnailView` (S10, live per-view fetch+cache) | `NodeGraphCanvas::fetchThumbnail`/`prefetchThumbnails`/`thumbnailFor`, one prefetch pass per `applySnapshot` | Mac fetches lazily per-view with its own internal cache; Qt PREFETCHES every node's thumbnail synchronously once per snapshot fetch and caches by name, so `GraphNodeItem::paint` never calls into the engine (`ViewportBridge::painterPreview`/`rampStripPreview`) from inside a paint callback — matches this codebase's engine-calls-never-happen-in-paint convention (ViewportWidget et al.), at the cost of fetching thumbnails for nodes not currently visible. Bounded by node count (G6 discipline keeps it low). |
// | 8 | Pan (drag empty canvas) / zoom (magnify gesture, top-left anchored) | `QGraphicsView::ScrollHandDrag` / `NodeGraphView::wheelEvent`, `AnchorUnderMouse` | Zoom anchors under the CURSOR on Windows vs top-left on Mac — a deliberate, arguably-better Qt-idiomatic choice (`QGraphicsView::AnchorUnderMouse` is the standard idiom every Qt node-editor tutorial uses), not a fidelity gap; both clamp to the SAME [0.2, 2.5] range. |
// | 9 | Click-select → `setSelectionCategory`      | `GraphNodeItem::mousePressEvent` → `NodeGraphCanvas::selectNode` | Direct port, including the Function(1)→Painter category-routing comment. |
// | 10 | Double-click def-focus (select + scroll/highlight the def row) | `GraphNodeItem::mouseDoubleClickEvent` → `NodeGraphCanvas::openDefs` | **Real gap, not cosmetic**: `openDefs` only selects the node. No Qt equivalent of `RenderViewModel.focusPainterDefRows`/a def-row scroll-anchor exists in `ViewportProperties` today — grep confirms no `defStageIndex`/scroll-to-row concept there. The def rows ARE visible (ViewportProperties lists every `def[i]` row once the node is selected), just not auto-scrolled-to. Closing this gap is `ViewportProperties` work, out of this slice's scope; flagged for the MSVC-verification checklist below. |
// | 11 | "A double-click always selects" (`exclusively(before:)` combinator) | Qt's natural event order: `mousePressEvent` always fires and selects; `mouseDoubleClickEvent` fires ADDITIONALLY on a real double-click | Same observable behavior via a DIFFERENT mechanism — Qt has no `exclusively(before:)` analogue, but its default single/double-click delivery order reproduces the identical result for free. |
// | 12 | Live drag-to-reposition, `liveDragOverride` / `displayedNodes` override array, write-on-release only | `GraphNodeItem` (`ItemIsMovable`), `itemChange(ItemPositionChange/-HasChanged)`, `commitNodeMove` on `mouseReleaseEvent` | **No override array on Qt.** SwiftUI state and view geometry are decoupled, so Mac needs a parallel `liveDragOverride`/`displayedNodes` mechanism to make wires track a live drag. Qt's `QGraphicsItem` OWNS its canonical position — `GraphNodeItem::pos()` IS the live value, full stop. `itemChange(ItemPositionHasChanged)` repaints the wires layer on every incremental move. This removes an entire class of sync bugs the Mac shape has to manage by hand; both converge on the same visible effect (wires track the dragged node in real time). |
// | 13 | Reposition-write NOT gated on `isSceneEditableForAgents` (relies on the API's own render-owns-scene refusal) | `commitNodeMove` — same, deliberately un-gated on `m_sceneEditable` | Direct port of an intentional Mac asymmetry (layout writes touch only the sidecar, never the CST document) — NOT an oversight; see `commitNodeMove`'s own comment. |
// | 14 | Drag-to-wire hotspot: sibling ZStack circle, NOT a child of the node box (so its DragGesture never contends with the node's own drag) | `GraphOutputHandleItem`, a CHILD `QGraphicsItem` of `GraphNodeItem` | Different mechanism, same isolation guarantee: Qt delivers a mouse press to the TOPMOST (child-before-parent) item under the cursor, so a press landing exactly on the handle is consumed there and never reaches the node's own `ItemIsMovable` drag — no sibling-layer trick needed. |
// | 15 | Drag-to-wire live legality preview (`checkConnection`, debounced by `lastCheckedTarget`, discards a stale async answer) | `NodeGraphCanvas::updateWireDrag`/`findPortTarget`, `checkConnection` | `checkConnection` is a SYNCHRONOUS local C++ call on Qt (no async race), so the Mac file's "discard a stale answer" guard has nothing to guard against — folded away rather than ported literally. The "only re-check when the hovered PORT changes" debounce IS kept (`targetChanged` gate). |
// | 16 | Wire-drag status line, floating over the canvas | `m_statusLabel`, a `QLabel` child of the view's viewport, repositioned on resize | Direct port. |
// | 17 | Port-hit-radius screen-constant correction (divide by `effectiveScale`) | `findPortTarget`'s `hitRadius = portHitRadius / currentScale()` | Direct port of the corrected (S21 review round 1 P2-2) formula. |
// | 18 | Refusal panel: inline SwiftUI `.overlay`, Cancel + conditional "Duplicate & Retry" | `WireRefusalDialog`, a modal `QDialog` | **Refusal presentation deviation.** A refusal only ever appears AFTER a drag already ended (not mid-drag), so there is no overlay-during-gesture requirement to preserve; a modal `QDialog` is the standard Qt idiom for a blocking decision here and needs no per-resize repositioning logic the floating panel would. Content (message, shared-chunks list, out-of-closure-referrers list, closure-gated Duplicate-and-Retry button) is a direct 1:1 port. |
// | 19 | Duplicate-then-retry escape hatch | `WireRefusalDialog::DuplicateAndRetry` result → `NodeGraphCanvas::showWireRefusal` → `duplicateGraphNode` → `commitRewire` | Direct port of `retryWithDuplicate`. |
// | 20 | Delete confirm: Delete / Delete+Cascade / Cancel, re-check gate after modal returns | `QMessageBox` with `AcceptRole`/`DestructiveRole` buttons, re-check `m_sceneEditable` after `exec()` | Direct port; button ROLES differ (Qt's `DestructiveRole` vs a plain second NSAlert button) but the three-way choice and the post-modal re-check race guard are identical, and also match this codebase's OWN established `MainWindow::removeEntity` convention. |
// | 21 | Keyboard Delete/⌦/⌘D, scoped to the canvas's own view hierarchy | `QShortcut` × 3 (`Delete`, `Backspace`, `Ctrl+D`), `Qt::WidgetWithChildrenShortcutContext` | Direct port; Windows convention substitutes `Ctrl+D` for `⌘D` (no other shortcut in this app claims either combination — verified by grep before adding). |
// | 22 | Add-node palette: category segmented control, search, keyword list, then a required-arg form (reference picker via `checkConnectionByKeyword`, file picker, literal field), zero-requirement keyword creates immediately | `NodeGraphAddNodeDialog`, a two-page `QStackedWidget` inside a `QDialog` | Direct port, including the "advisory, not authoritative" candidate-filter caveat and the immediate-create-on-zero-requirements shortcut. Qt's `QComboBox` segmented-style category picker replaces SwiftUI's `.pickerStyle(.segmented)` — cosmetic only. |
// | 23 | "+" button opens the palette at the current viewport center (inverse pan/zoom transform) | `NodeGraphCanvas::openAddNodeDialog`, `QGraphicsView::mapToScene` | Same effect via Qt's BUILT-IN inverse transform instead of Mac's by-hand `(screenCenter - offset) / scale` math — Qt already tracks the view↔scene transform, so there is nothing to invert manually. |
// | 24 | Refresh cadence: epoch-gated (`sceneEpoch()` compare) PLUS a 250ms GCD debounce (`DispatchWorkItem` cancel-and-reschedule) for `refreshTrigger` bursts | `NodeGraphCanvas::refresh()`/`refreshForce()`/`performReload`, epoch-gated, NO debounce timer | **Deliberate simplification, backed by direct sibling precedent.** This app's OWN established pattern for an identically-shaped problem (`OutlinerWidget::refresh()`, `ViewportProperties::refresh()`) is a bare epoch compare tied to `imageUpdated`, with no debounce layer at all — because every self-driven mutation in THIS codebase (create/rewire/delete/duplicate/reposition) already calls `refreshForce()` immediately after committing (mirroring Mac's own explicit `force: true` calls at the same call sites), and an externally-driven burst (agent edits with no render frame in between) still collapses to exactly one `painterMaterialGraph()` fetch on the NEXT `refresh()` call, because only the FINAL epoch value is ever visible to the gate — a stronger coalescing guarantee than a fixed 250ms window provides. Mac's debounce solves a SwiftUI-specific problem (a shared `refreshTrigger` counter with no per-caller cheapness guarantee) that does not exist in the Qt shell's concrete-signal wiring. Inventing a timer-based debounce nobody else in this codebase uses, to solve a problem this codebase's own idiom already solves more simply, would have been the LESS faithful port. |
// | 25 | Position persistence: sidecar file via `writeGraphNodeLayoutPosition`/S13 | `NodeGraphCanvas::commitNodeMove`, same bridge call | Direct port; shared C++ core (S13), no platform-specific behavior to diverge on. |
// | 26 | Category/edge count header ("N nodes, M edges") | `updateHeaderCounts` | Direct port. |
// | 27 | Node-graph model types (`GraphCanvasNode`/`GraphCanvasPort`) | `GraphNodeData`/`GraphPortData` | Structurally identical MINUS the `position` field's role — see #12; Qt's `GraphNodeData::position` is a "last-known-good" seed/revert value, not the live render position. |
// | 28 | Object-pick spotlight: viewport/outliner Object selection → gold glow/border on the object's bound material + its full Painter/Function/Material closure, auto-scroll to the material ONLY on an actual selection change, NEVER calls `setSelection` from the spotlight path | `NodeGraphCanvas::refreshSpotlight`/`applySpotlightToItems`, `GraphNodeItem::setNodeSpotlit`, `ViewportBridge::appearanceClosureForObject` | **NOT a direct port on first landing — an external review round caught a real gap, now closed; this row describes the FIXED state.** (a) Mac's `.shadow` glow has no Qt equivalent without a whole-item `QGraphicsEffect`, so `GraphNodeItem::paint` approximates it with two concentric rounded-rect strokes drawn behind the card — same warm-gold (`Theme::gold`) read, different primitive; a genuine cosmetic-only deviation. (b) Mac observes `refreshTrigger`/`sceneEpoch` via SwiftUI `.onChange`, and Mac's own `selectNode` bumps `refreshTrigger` SYNCHRONOUSLY on a canvas click, clearing a live spotlight immediately. Qt has no such push signal for a plain selection change (confirmed: `SceneEditController::SetSelectionInner_`'s UI-only Object/Material path never bumps `mSceneEpoch`), and `NodeGraphCanvas::refresh()` is NOT a true per-frame timer poll — it rides `ViewportBridge::imageUpdated` (MainWindow.cpp), which fires on a RENDERED FRAME, not on a bare selection change. The FIRST landing wired ONLY that connection, so a viewport/outliner Object pick with no render in flight (the common case: browsing the scene graph on an already-converged or static preview) never lit the spotlight at all, and a canvas-node click never cleared one immediately either. Fixed two ways: `MainWindow.cpp`'s `OutlinerWidget::selectionActivated` connect now ALSO calls `NodeGraphCanvas::refresh()` (the same explicit-follow pattern that connection already uses for `ViewportProperties::refresh`), and `NodeGraphCanvas::selectNode` now calls `refreshSpotlight(false)` directly, matching Mac's synchronous clear. `refreshSpotlight` itself runs on every `refresh()`/`performReload()` call (whichever of the two paths triggered it) with a two-tier early-exit: a CHEAP `(selectionCategory(), selectionName())` pre-check before ever paying the O(rows) `selectionRowName()` walk, plus a `forceReapply` flag so a just-rebuilt node set (whose items are all new) always gets the spotlight re-applied even when the selection identity itself did not change — but `forceReapply` does NOT by itself trigger a re-scroll; auto-scroll is gated on the RESOLVED row name actually differing from the last one computed, so a structural edit/rewire while the same object stays selected never yanks the view. Auto-scroll uses `QGraphicsView::ensureVisible` (built-in, already a no-op when the target is fully visible) rather than porting Mac's by-hand screen↔content inversion. |
// | 29 | View-scope toggle: "All" vs "Focused" (STICKY object-only subgraph) — Focused mode's target is ALWAYS the last externally-selected Object (viewport/outliner), never a canvas-node click; clicking a canvas node still moves the shared selection (properties panel follows it, spotlight clears) but does NOT retarget the focused subgraph; layout is TRANSIENT (never reads/writes the `.risegraph.json` sidecar); no sticky object, a degraded resolve, or the sticky object no longer resolving all have distinct handling (see below); node drags are disabled (snap back) while Focused; create-while-focused never writes the drop-point into the sidecar | `NodeGraphCanvas::performReload`/`updateStickyFocusObject`/`performFocusedReload`/`currentFocusTarget`/`onViewScopeToggled`/`setBridge`, `m_viewScopeBtn`, `m_stickyFocusHasObject`/`m_stickyFocusObjectName`, `ViewportBridge::painterMaterialGraphFocused`, `SceneEditController::ReadPainterMaterialGraphLaidOutFocused` | **Revised by a user-feedback review round after the first two landings (`0562b9c4`, `80032065`) shipped a DIFFERENT design** -- the original shape let a canvas Painter/Function/Material click retarget the focused subgraph to that node's own upstream closure; a real user tried it and reported the surprise directly: "I click a painter and it narrows the view, I don't want that... clicking a painter should still bring up the painter details in the panel." This row now describes the CORRECTED design, not the original one -- the core API `ReadPainterMaterialGraphLaidOutFocused` is UNCHANGED and still accepts a Painter/Function/Material category (kept general for a future explicit "focus on this node" affordance); only this SHELL's policy for what counts as a focus target changed. Direct port of the Mac `GraphViewScope` segmented Picker, same shared-core C++ call so the subgraph selection + transient layout logic is identical bit-for-bit on both platforms -- only the toggle widget and the refetch plumbing differ. Toggle is a checkable `QToolButton` ("Focused") inserted into the header row immediately before the "+" palette button, mirroring the Mac segmented control's position. `updateStickyFocusObject` is the SOLE writer of the sticky memo (`m_stickyFocusHasObject`/`m_stickyFocusObjectName`), called at the top of EVERY `performReload` regardless of `m_viewScope` (so the memo is already current the instant the user toggles INTO Focused mode): it updates the memo ONLY when `m_bridge->selectionCategory() == Object`, and leaves it completely untouched otherwise -- a canvas-node click sets the shared selection to Painter/Material (`selectNode`), which this deliberately ignores, and THAT asymmetry is the entire mechanism that makes the memo "sticky." `currentFocusTarget` was reduced to a single read of that memo -- the PRIOR "or the canvas's own selected node" branch (and the Function-vs-Painter UI-category-collapse workaround it needed) is GONE, not merely dead code, since a canvas-node selection is no longer a focus-target source at all. `performFocusedReload` still carries the cheap `(category, name)` identity gate from the P1 fix (`0562b9c4`/`80032065`, all THREE of `m_lastFocusedHasTarget`/`m_lastFocusedCategory`/`m_lastFocusedName` commit and clear together, unchanged by this round) but `category` is now ALWAYS 8 (Object) in practice. NEW this round: a genuinely empty (non-degraded) focused result -- the sticky object was deleted, renamed, or has no material bound, all indistinguishable from this call site and treated identically -- falls back to showing All AND clears the sticky memo (`m_stickyFocusHasObject = false`), so a dead name can't keep silently re-resolving to nothing on every later frame; a DEGRADED resolve is unaffected by this and still just keeps last-good content, retried on the next `imageUpdated` frame as before. NEW this round: `setBridge()` now EXPLICITLY clears the sticky memo -- this row's ORIGINAL text argued no reset was needed because `m_hasFetchedOnce = false` alone forced a correct re-evaluation; that reasoning covered the (then-general) focus-target gate fields but never actually protected the sticky memo introduced this round (which has no relationship to `m_hasFetchedOnce`), so an explicit clear was added in `setBridge()` -- verified necessary because this canvas is a PERSISTENT widget (built once by `MainWindow`, never recreated per tab-switch) and `rebuildViewportForLoadedScene()` mints a BRAND NEW `ViewportBridge` on every scene load, calling `setBridge()` with it -- the one reliable "a scene just (re)loaded" signal available here. `commitNodeMove`'s Focused-mode drag-disable and `openAddNodeDialog`'s Focused-mode sidecar-write skip (both from the earlier P3-2 fix) are UNCHANGED by this round -- neither depended on how the focus target was chosen. **Follow-up P2 fix (same review pass):** `updateStickyFocusObject` initially called `selectionRowName()` -- an O(rows), per-row-heap-allocating tree walk -- unconditionally whenever the shared selection was an Object, but this method runs on EVERY preview frame (`performReload`'s per-frame `imageUpdated` poll cadence), so an object sitting selected through a long render paid that walk every single frame, regressing the per-frame-efficiency invariant `refreshSpotlight`'s own cheap gate established (round-1 P2-1). Fixed with the SAME two-tier gate: a cheap `(selectionCategory(), selectionName())` pre-check (`m_lastStickyFocusCategory`/`m_lastStickyFocusSelectionName`, cleared alongside the sticky memo in `setBridge()`) skips the expensive walk when neither O(1) read changed since it was last committed. **This gate is Qt-ONLY, deliberately -- the Mac twin (`updateStickyFocusObject(bridge:)`) has NO equivalent, and porting one in either direction would be wrong**: Mac's `performReload` runs only on discrete triggers (`.onAppear`, a `refreshTrigger` bump, a `viewScope` toggle), never on a bare per-frame poll, so there is no steady-state "same object selected through many frames" cost on that platform to guard against; this canvas's `performReload`, by contrast, genuinely fires every rendered preview frame via `ViewportBridge::imageUpdated`. |
//
// ---- Bridge gaps closed in this slice (build/VS2022/RISE-GUI/ViewportBridge.{h,cpp}) ----
//
// The S14 Windows bridge carry (2026-08-xx) stopped at the READ-ONLY
// surface (`painterMaterialGraph()`, `categoryTree()`) plus the S18/S19/
// S20 mutation verbs (`createChunkNode`, `rewireConnection`,
// `deleteGraphNode`, `duplicateGraphNode`) — everything a Phase-A
// read-only canvas plus a keyword-driven create/rewire/delete/duplicate
// flow needs. It did NOT carry the S21 EDIT-surface leaf calls the
// interactive Qt widget in THIS file needs, because nothing on Windows
// called them yet:
//
//   - `checkConnection` (RISE_API_SceneEditController_CheckConnection passthrough)
//   - `wouldCycle` (RISE_API_SceneEditController_WouldCycle passthrough)
//   - `checkConnectionByKeyword` (STATIC — RISE_API_ConnectionLegality_CheckConnectionByKeyword passthrough)
//   - `writeGraphNodeLayoutPosition` (RISE_API_SceneEditController_WriteGraphNodeLayoutPosition passthrough)
//   - `paletteKeywords` (RISE_API_SceneEditController_PaletteKeywordCount/PaletteKeyword passthrough)
//
// All five were ALREADY EXPORTED on the C ABI (src/Library/RISE_API.h) —
// the macOS bridge (RISEViewportBridge.h/.mm) already wraps every one of
// them for S21. This slice adds the missing Windows-side thin
// passthroughs, written to match RISEViewportBridge.mm's five
// implementations line-for-line (same argument order, same buffer-size
// conventions, same "candidateIsPerChannelValues: pass 0, unknown"
// choice for checkConnectionByKeyword). No C++ library code changed —
// this is a pure GUI-layer bridge addition, so `make -j8 all` (which
// does not build RISE-GUI on this platform) is unaffected and is only
// a REGRESSION check that the library side stayed byte-identical.
//
// ---- MSVC-verification checklist (for the owed Windows build session) ----
//
//  [ ] HIGHEST-RISK ITEM, CHECK FIRST: the nested-struct fix in
//      `ViewportBridge.h` (`PainterGraphPort`/`PainterGraphNode`/
//      `PainterGraph`/`AppearanceClosureEntry` moved from global-namespace
//      siblings of `class ViewportBridge` to public NESTED members, plus
//      the two return-type qualifications and the `ConvertGraphPorts`
//      free-function qualification this required in `ViewportBridge.cpp`)
//      was verified ONLY via a throwaway `clang++ -fsyntax-only -std=c++17`
//      mock harness (QString/QVector aliased to std types) in a scratch
//      directory, NOT a real MSVC compile -- this whole file (and
//      `ViewportBridge.h`/`.cpp`) had never actually been built by MSVC
//      before that fix, per this checklist's own standing note below.
//      Re-verify a REAL MSVC compile of `ViewportBridge.h`/`.cpp` +
//      `NodeGraphCanvas.cpp` before trusting anything else on this list --
//      MSVC's name-lookup diagnostics (C2039) are expected to agree with
//      Clang's here, but that expectation is unconfirmed on the actual
//      toolchain this code ships for.
//  [ ] `NodeGraphCanvas.h`/`.cpp` compile clean at the project's warning
//      level (0 warnings, per this repo's "Compiler Warnings Are Bugs"
//      policy) -- watch specifically for MSVC C4244 (double/qreal->int
//      narrowing) at every `Theme::sans`/`Theme::mono` call site and
//      every `QRectF`/`QPointF` literal; every call in this file was
//      written with explicit `.0` qreal literals and explicit `int(...)`
//      casts specifically to avoid this, but MSVC's narrowing checks are
//      the ones that actually enforce it.
//  [ ] `ViewportBridge.h`/`.cpp`'s five new S16/S22 passthroughs compile
//      and link against the already-exported `RISE_API_*` C ABI symbols
//      (no new library-side export needed -- verify the five symbol
//      names against src/Library/RISE_API.h if the linker disagrees).
//  [ ] Open a scene with a shared painter bound into two materials;
//      confirm ONE node renders with a fan-out badge (`⑂ 2`), not two
//      nodes (mirrors S15's Mac-manual-checklist item).
//  [ ] Drag-reposition a node; confirm the wire redraws live during the
//      drag (no lag/snap) and the position survives a scene reload
//      (sidecar round-trip).
//  [ ] Drag a wire from one painter's output handle onto a legal input
//      port; confirm the live green/red preview + status line, and that
//      the commit lands + the property panel follows the new selection.
//  [ ] Drag a wire onto a SHARED painter's slot to provoke a
//      `SharedTarget` refusal; confirm the dialog's "Duplicate & Retry"
//      button appears ONLY for that closure kind, and that clicking it
//      forks the source and completes the rewire against the fork.
//  [ ] Open the "+" palette; create a `ramp_painter` (zero required
//      args -- should create immediately on keyword pick) and a
//      `blend_painter` (has required reference args -- should show the
//      requirement form, with the candidate combo correctly filtered by
//      `checkConnectionByKeyword`).
//  [ ] Select a node, press Delete; confirm the two-choice cascade
//      dialog, and separately press Ctrl+D to duplicate.
//  [ ] Confirm the keyboard shortcuts do NOT fire when focus is in an
//      unrelated widget (e.g. the Scene-file text editor) — the
//      `WidgetWithChildrenShortcutContext` scoping is the thing under
//      test here.
//  [ ] Toggle File > Theme Dark/Light while the Graph tab is visible;
//      confirm the header chrome AND every node/wire repaint with the
//      new palette (the OutlinerWidget-style "reads Theme:: live, needs
//      only a repaint" exemption this file claims — see restyleTheme's
//      own comment).
//  [ ] SourceHygieneTest and a clean `make -j8 all` both stay green (the
//      Qt files are not in the make build by design; this is a
//      regression check that the library side is untouched).
//  [ ] Object-pick spotlight (row 28) -- WHILE NOTHING IS ACTIVELY
//      RENDERING (the scene has converged / is static, no imageUpdated
//      frames arriving): pick an object in the OUTLINER whose material has
//      a multi-stage painter chain; confirm the spotlight lights up
//      IMMEDIATELY (this is the exact gap the review-round fix closed --
//      it must NOT require nudging the viewport to force a render frame
//      first). Confirm its bound material AND every node in the painter
//      chain light up with the gold glow/border, the view auto-scrolls to
//      the material if it starts offscreen, and the properties panel keeps
//      showing the OBJECT the whole time (the spotlight must never steal
//      selection onto a painter/material node). Pick an instanced copy
//      (a `standard_object` with `source`, or a `count_u`/`count_v`
//      repeated instance) and confirm the SAME material/closure lights up
//      as picking the source directly. Then click a bare canvas node and
//      confirm the spotlight clears IMMEDIATELY (not on the next render
//      frame). Separately: construct a scene where a Painter and a
//      Material chunk share one name (only the Material is the object's
//      actual binding) and confirm the spotlight lights up the MATERIAL,
//      not a same-named Painter -- the (category, name) matching this
//      review round added.
//  [ ] View-scope toggle (row 29, STICKY OBJECT-ONLY design -- rewritten
//      after a user-feedback review round; the steps below replace the
//      PRIOR version, which tested a canvas-node-narrows-the-view
//      behavior that no longer exists): select an OBJECT in the outliner,
//      then click "Focused"; confirm the graph narrows to that object's
//      full appearance closure. Click a PAINTER node ON THE CANVAS; confirm
//      (a) the subgraph is COMPLETELY UNCHANGED (still the same object's
//      closure, NOT narrowed to the clicked painter's own upstream inputs
//      -- this is the exact behavior the user reported and this fix
//      removes), (b) the properties panel switches to show the painter's
//      details, and (c) the spotlight glow clears (selection moved off
//      the Object). Now select a DIFFERENT object in the outliner; confirm
//      the graph re-narrows to the NEW object's closure. Click "All";
//      confirm the full graph returns and every node sits at its
//      PERSISTED sidecar position (not a re-laid-out position) -- Focused
//      layout must never have written the sidecar. While Focused, drag a
//      node; confirm it snaps back to its known-good position on release
//      (no sidecar write), then confirm a subsequent scene reload shows
//      the node at its original (pre-drag) position. Create a new node
//      while Focused with an object selected; confirm (a) the new node's
//      sidecar position is NOT written from the focused-view drop point
//      (P3-2, unchanged by this round), and (b) the new node appears in
//      the focused subgraph ONLY if it happens to already be reachable
//      from the sticky object's material (an unwired freestanding create
//      typically will NOT appear until rewired in -- this is the natural,
//      correct consequence of the sticky-object design, not a bug).
//      DELETE the sticky object (or rename it) while Focused with it
//      selected; confirm the view falls back to All AND the sticky memo
//      is cleared -- reselecting nothing further must not silently
//      re-resolve to the dead name (distinguish this from a DEGRADED
//      resolve: trigger render contention (kick off a render, then select
//      an object and toggle to Focused) and confirm the canvas instead
//      keeps showing its last-good content, retrying until the render
//      ends, WITHOUT clearing the sticky memo or flashing to All and
//      back). Finally, with the Graph tab open and an object's Focused
//      view showing, load a DIFFERENT scene (File > Open); confirm the
//      sticky memo does NOT carry over from the old scene (no attempt to
//      resolve the old scene's object name against the new scene) --
//      this exercises the new `setBridge()` clear, verified necessary
//      because this canvas is a persistent widget that survives a scene
//      switch while `m_bridge` itself is swapped for a brand-new
//      `ViewportBridge`.
//
// ======================================================================
