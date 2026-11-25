/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WheelScrollNormalizer.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QStyleHints>
#include <QVariant>
#include <QWheelEvent>
#include <QtMath>

namespace TrophyBackend {

namespace {

// QQuickFlickable is the C++ class behind Flickable, ListView, GridView,
// PathView and the Flickable inside ScrollView. It is compared by name so this
// stays independent of Qt's private headers.
constexpr auto kFlickableClass = "QQuickFlickable";

// Qt reports wheel rotation in eighths of a degree; one notch is 120.
constexpr qreal kWheelNotch = 120.0;
// QQuickFlickable's nominal height of one scroll line
// (initialWheelFlickDistance = wheelScrollLines * 24).
constexpr qreal kLineHeight = 24.0;

bool movesEnough(qreal from, qreal to) { return qAbs(to - from) > 0.001; }

qreal realProperty(const QObject *object, const char *name) {
    return object->property(name).toReal();
}

struct ScrollRange {
    qreal low;
    qreal high;
};

// QQuickFlickable's extents are expressed in its internal "move" coordinate,
// which is the negated content position: contentX/contentY are -move, and the
// content position starts at -minExtent (so topMargin shows up as a negative
// contentY at rest) and ends at -maxExtent.
ScrollRange rangeOf(qreal minExtent, qreal maxExtent) {
    return {qMin(-minExtent, -maxExtent), qMax(-minExtent, -maxExtent)};
}

// The extents behind QQuickFlickable::minYExtent()/maxYExtent(), expressed
// through their public properties. minYExtent() is topMargin (originY is
// derived as -minYExtent + topMargin, i.e. 0 for Flickable, ListView and
// GridView) and maxYExtent() is min(minYExtent, height - contentHeight -
// bottomMargin). Same for X.
ScrollRange verticalRange(const QObject *flickable) {
    const qreal minExtent =
        realProperty(flickable, "topMargin") - realProperty(flickable, "originY");
    const qreal maxExtent = qMin(minExtent, realProperty(flickable, "height") -
                                               realProperty(flickable, "contentHeight") -
                                               realProperty(flickable, "bottomMargin"));
    return rangeOf(minExtent, maxExtent);
}

ScrollRange horizontalRange(const QObject *flickable) {
    const qreal minExtent =
        realProperty(flickable, "leftMargin") - realProperty(flickable, "originX");
    const qreal maxExtent = qMin(minExtent, realProperty(flickable, "width") -
                                               realProperty(flickable, "contentWidth") -
                                               realProperty(flickable, "rightMargin"));
    return rangeOf(minExtent, maxExtent);
}

} // namespace

WheelScrollNormalizer::WheelScrollNormalizer(QObject *parent) : QObject(parent) {
    setObjectName(QStringLiteral("wheelScrollNormalizer"));
}

void WheelScrollNormalizer::install() {
    QCoreApplication *application = QCoreApplication::instance();
    if (!application ||
        application->findChild<WheelScrollNormalizer *>(QString(),
                                                        Qt::FindDirectChildrenOnly))
        return;
    auto *filter = new WheelScrollNormalizer(application);
    // A filter installed on the application object sees every event before the
    // receiving object handles it, including the per-item deliveries Qt Quick
    // performs while walking the items under the cursor.
    application->installEventFilter(filter);
}

bool WheelScrollNormalizer::consumeWheel(QObject *receiver, QWheelEvent *event) {
    if (!receiver || !event || event->type() != QEvent::Wheel)
        return false;
    // Only the Flickable itself: the delivery agent reaches it after every item
    // and pointer handler under the cursor has ignored the wheel, so controls
    // keep their own wheel handling.
    if (!receiver->inherits(kFlickableClass))
        return false;
    // Touchpads report synthesized pixel deltas, which QQuickFlickable already
    // turns into rate-independent dragging. Only mouse wheels are normalized.
    if (event->source() != Qt::MouseEventNotSynthesized)
        return false;
    const QPoint angleDelta = event->angleDelta();
    if (angleDelta.isNull())
        return false;
    if (!receiver->property("interactive").toBool() ||
        !receiver->property("enabled").toBool())
        return false;

    const QVariant contentXProperty = receiver->property("contentX");
    const QVariant contentYProperty = receiver->property("contentY");
    if (!contentXProperty.isValid() || !contentYProperty.isValid())
        return false;

    const qreal step =
        QGuiApplication::styleHints()->wheelScrollLines() * kLineHeight;
    if (step <= 0)
        return false;

    const ScrollRange vertical = verticalRange(receiver);
    const ScrollRange horizontal = horizontalRange(receiver);
    const qreal contentX = contentXProperty.toReal();
    const qreal contentY = contentYProperty.toReal();
    // QQuickFlickable moves its content position by -angleDelta/120 * step, so
    // a positive angleDelta (wheel turned away from the user) decreases
    // contentY/contentX. Mirroring that keeps the direction identical to the
    // stock implementation, including compositor flips for natural scrolling.
    const qreal targetX =
        qBound(horizontal.low, contentX - angleDelta.x() / kWheelNotch * step,
               horizontal.high);
    const qreal targetY =
        qBound(vertical.low, contentY - angleDelta.y() / kWheelNotch * step,
               vertical.high);

    bool moved = false;
    if (movesEnough(contentX, targetX)) {
        receiver->setProperty("contentX", targetX);
        moved = true;
    }
    if (movesEnough(contentY, targetY)) {
        receiver->setProperty("contentY", targetY);
        moved = true;
    }
    if (!moved)
        return false; // At the end of the content: let a parent scrollable take it.
    event->accept();
    return true;
}

bool WheelScrollNormalizer::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Wheel &&
        consumeWheel(watched, static_cast<QWheelEvent *>(event)))
        return true;
    return QObject::eventFilter(watched, event);
}

} // namespace TrophyBackend
