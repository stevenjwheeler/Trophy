/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WheelScrollNormalizer.h"

#include <QCoreApplication>
#include <QEasingCurve>
#include <QGuiApplication>
#include <QHash>
#include <QPointer>
#include <QStyleHints>
#include <QVariant>
#include <QVariantAnimation>
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

// A wheel step glides to its destination instead of snapping. Keeping the
// animation shorter than a high-resolution wheel's inter-event gap would make
// it snap again; longer than ~150 ms starts to lag behind the wheel.
constexpr int kGlideDurationMs = 110;

// Hitting an end of the content recoils by this much and springs back. The
// content position cannot leave the Flickable's extents, so the bounce is a
// short move *inward* from the bound, not an overscroll past it.
constexpr qreal kBounceDistance = 12.0;
constexpr int kBounceDurationMs = 260;
// Fraction of the bounce spent travelling in; the rest eases back to the
// bound, so the recoil itself feels quick while the return settles.
constexpr qreal kBouncePeakKey = 0.22;
// Values closer than this are treated as "did not move": the filter must not
// consume an event that changed nothing, or a parent scrollable never sees it.
constexpr qreal kMovementEpsilon = 0.001;
// Tolerance for deciding whether a view is already sitting on an extent.
constexpr qreal kBoundEpsilon = 0.5;

bool movesEnough(qreal from, qreal to) {
    return qAbs(to - from) > kMovementEpsilon;
}

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

// The visual parent of a QQuickItem is exposed as the "parent" property, so
// the item tree can be walked without Qt Quick's private headers.
bool hasAncestorFlickable(const QObject *object) {
    QObject *ancestor = object->property("parent").value<QObject *>();
    for (int depth = 0; ancestor && depth < 64; ++depth) {
        if (ancestor->inherits(kFlickableClass))
            return true;
        ancestor = ancestor->property("parent").value<QObject *>();
    }
    return false;
}

struct ScrollAnimation {
    QPointer<QObject> flickable;
    QPointer<QVariantAnimation> animation;
    // Resting content position. While a glide runs this is the destination,
    // not the current (still animating) position, so a burst of events
    // accumulates against where the view is going to end up.
    QPointF target;
    // The last content position this filter wrote, used to tell an external
    // move (a section jump, a drag, a layout change) from our own writes.
    QPointF lastWritten;
    bool gliding = false;
    bool bouncing = false;
    bool bounceQueued = false;
    bool bounceVertical = true;
    // QVariantAnimation emits valueChanged for intermediate values while it is
    // stopped and re-configured (the old position at stop() and each setter's
    // own value). Those are not positions this filter wants: writing them
    // moves the view backwards and aborts the next glide. Ticks are dropped
    // until the new run is fully configured and started.
    bool reconfiguring = false;
};

QHash<QObject *, ScrollAnimation *> &animations() {
    static QHash<QObject *, ScrollAnimation *> map;
    return map;
}

void stopAnimation(ScrollAnimation *state) {
    if (state->animation) {
        state->reconfiguring = true;
        state->animation->stop();
        state->reconfiguring = false;
    }
    state->gliding = false;
    state->bouncing = false;
    state->bounceQueued = false;
}

void forgetFlickable(QObject *flickable) {
    ScrollAnimation *state = animations().take(flickable);
    if (!state)
        return;
    if (state->animation) {
        state->reconfiguring = true;
        state->animation->stop();
        state->reconfiguring = false;
    }
    // The animation is parented to the Flickable (if it still exists) and is
    // destroyed with it; only the bookkeeping is ours to free.
    delete state;
}

void animationFinished(QObject *flickable);

ScrollAnimation *stateFor(QObject *flickable) {
    auto found = animations().find(flickable);
    if (found != animations().end())
        return *found;

    auto *state = new ScrollAnimation;
    state->flickable = flickable;

    auto *animation = new QVariantAnimation(flickable);
    animation->setStartValue(QPointF());
    animation->setEndValue(QPointF());
    animation->setDuration(kGlideDurationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    state->animation = animation;

    QObject::connect(animation, &QVariantAnimation::valueChanged, flickable,
                     [flickable](const QVariant &value) {
                         ScrollAnimation *current = animations().value(flickable);
                         if (!current || current->reconfiguring)
                             return;
                         // If the view is no longer where this filter last put
                         // it, something else moved it (a section jump, a drag,
                         // a layout change). That owner wins: abandon the glide
                         // instead of fighting it.
                         const QPointF actual(realProperty(flickable, "contentX"),
                                              realProperty(flickable, "contentY"));
                         if (qAbs(actual.x() - current->lastWritten.x()) >
                                 kBoundEpsilon ||
                             qAbs(actual.y() - current->lastWritten.y()) >
                                 kBoundEpsilon) {
                             stopAnimation(current);
                             current->target = actual;
                             current->lastWritten = actual;
                             return;
                         }
                         const QPointF position = value.toPointF();
                         if (!qFuzzyCompare(position.x(), current->lastWritten.x())) {
                             current->lastWritten.setX(position.x());
                             flickable->setProperty("contentX", position.x());
                         }
                         if (!qFuzzyCompare(position.y(), current->lastWritten.y())) {
                             current->lastWritten.setY(position.y());
                             flickable->setProperty("contentY", position.y());
                         }
                     });
    QObject::connect(animation, &QVariantAnimation::finished, flickable,
                     [flickable] { animationFinished(flickable); });

    QObject::connect(flickable, &QObject::destroyed, qApp,
                     [flickable] { forgetFlickable(flickable); });

    animations().insert(flickable, state);
    return state;
}

void startGlide(QObject *flickable, ScrollAnimation *state, const QPointF &from,
                const QPointF &to) {
    QVariantAnimation *animation = state->animation;
    if (!animation)
        return;
    state->reconfiguring = true;
    animation->stop();
    animation->setKeyValues(QVariantAnimation::KeyValues());
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setDuration(kGlideDurationMs);
    animation->setStartValue(from);
    animation->setEndValue(to);
    state->target = to;
    state->lastWritten = from;
    state->gliding = true;
    state->bouncing = false;
    state->bounceQueued = false;
    // A wheel event stops a touch/touchpad flick the same way Qt's own
    // wheel handling does.
    QMetaObject::invokeMethod(flickable, "cancelFlick");
    animation->start();
    state->reconfiguring = false;
}

bool startBounce(QObject *flickable, ScrollAnimation *state, bool vertical) {
    const ScrollRange range =
        vertical ? verticalRange(flickable) : horizontalRange(flickable);
    if (range.high - range.low <= kBoundEpsilon)
        return false; // Nothing to scroll: no rubber band.

    const char *axis = vertical ? "contentY" : "contentX";
    const qreal current = realProperty(flickable, axis);
    const bool atLow = qAbs(current - range.low) <= kBoundEpsilon;
    const bool atHigh = qAbs(current - range.high) <= kBoundEpsilon;
    if (!atLow && !atHigh)
        return false;

    const qreal bound = atLow ? range.low : range.high;
    const qreal inward =
        qBound(range.low, bound + (atLow ? kBounceDistance : -kBounceDistance),
               range.high);
    if (qAbs(inward - bound) < 1.0)
        return false;

    const QPointF start(realProperty(flickable, "contentX"),
                        realProperty(flickable, "contentY"));
    QPointF peak = start;
    QPointF end = start;
    if (vertical) {
        end.setY(bound);
        peak = end;
        peak.setY(inward);
    } else {
        end.setX(bound);
        peak = end;
        peak.setX(inward);
    }

    QVariantAnimation *animation = state->animation;
    if (!animation)
        return false;
    state->reconfiguring = true;
    animation->stop();
    animation->setKeyValues(QVariantAnimation::KeyValues());
    animation->setEasingCurve(QEasingCurve::InOutQuad);
    animation->setDuration(kBounceDurationMs);
    animation->setStartValue(end);
    animation->setKeyValueAt(kBouncePeakKey, peak);
    animation->setEndValue(end);
    state->target = end;
    state->lastWritten = end;
    state->gliding = false;
    state->bouncing = true;
    state->bounceQueued = false;
    QMetaObject::invokeMethod(flickable, "cancelFlick");
    animation->start();
    state->reconfiguring = false;
    return true;
}

void animationFinished(QObject *flickable) {
    ScrollAnimation *state = animations().value(flickable);
    if (!state)
        return;
    const bool wasBouncing = state->bouncing;
    state->gliding = false;
    state->bouncing = false;
    state->target = QPointF(realProperty(flickable, "contentX"),
                            realProperty(flickable, "contentY"));
    state->lastWritten = state->target;
    if (wasBouncing)
        return;
    if (state->bounceQueued) {
        state->bounceQueued = false;
        startBounce(flickable, state, state->bounceVertical);
    }
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
    const qreal currentX = contentXProperty.toReal();
    const qreal currentY = contentYProperty.toReal();
    // QQuickFlickable moves its content position by -angleDelta/120 * step, so
    // a positive angleDelta (wheel turned away from the user) decreases
    // contentY/contentX. Mirroring that keeps the direction identical to the
    // stock implementation, including compositor flips for natural scrolling.
    const qreal deltaX = -angleDelta.x() / kWheelNotch * step;
    const qreal deltaY = -angleDelta.y() / kWheelNotch * step;

    ScrollAnimation *state = stateFor(receiver);
    // While a glide is still running its destination, not the current
    // position, is what the next rotation adds to. Adding to the current
    // position instead would throw away every distance still in flight, which
    // is exactly how the stock implementation slows high-resolution wheels.
    const bool animating = state->gliding || state->bouncing;
    const qreal baseX = animating ? state->target.x() : currentX;
    const qreal baseY = animating ? state->target.y() : currentY;

    const qreal targetX = qBound(horizontal.low, baseX + deltaX, horizontal.high);
    const qreal targetY = qBound(vertical.low, baseY + deltaY, vertical.high);
    const bool movedX = movesEnough(baseX, targetX);
    const bool movedY = movesEnough(baseY, targetY);

    if (movedX || movedY) {
        startGlide(receiver, state, QPointF(currentX, currentY),
                   QPointF(targetX, targetY));
        event->accept();
        return true;
    }

    // The view cannot move any further in the requested direction. A nested
    // Flickable must leave the event for its ancestor (the delivery loop only
    // continues when this returns false), while the outermost one gets a
    // small rubber-band recoil.
    if (hasAncestorFlickable(receiver))
        return false;

    const bool verticalPreferred =
        qAbs(angleDelta.y()) >= qAbs(angleDelta.x()) && angleDelta.y() != 0;
    if (state->gliding) {
        // Still gliding towards the end: bounce once it settles there rather
        // than fighting the glide with a second animation.
        state->bounceQueued = true;
        state->bounceVertical = verticalPreferred;
        event->accept();
        return true;
    }
    if (state->bouncing) {
        // A bounce is already under way; a wheel still held against the end
        // must not restart or interrupt it.
        event->accept();
        return true;
    }
    // Leave the event alone when there is nothing to recoil from (a view that
    // does not scroll at all, or content shorter than its viewport); Qt's own
    // handler then does exactly what it would have done without this filter.
    if (!startBounce(receiver, state, verticalPreferred))
        return false;
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
