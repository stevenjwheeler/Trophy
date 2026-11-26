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
#include <QTimer>
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

// Hitting an end of an *unclipped* Flickable recoils by this much and springs
// back. The content cannot leave the extents there (it would spill over the
// page below), so that bounce is a short move *inward* from the bound.
constexpr qreal kBounceDistance = 12.0;
constexpr int kBounceDurationMs = 260;
// Fraction of the bounce spent travelling in; the rest eases back to the
// bound, so the recoil itself feels quick while the return settles.
constexpr qreal kBouncePeakKey = 0.22;

// A clipped view instead stretches *past* the end. QQuickFlickable does not
// clamp contentY/contentX writes, so the content really moves and the clipped
// viewport reveals the page below: a rubber band, not a recoil.
//
// The pull is capped relative to the viewport so it feels the same on a small
// panel and a large window, and it saturates, so holding the wheel stops the
// stretch after a short distance instead of dragging the content away.
constexpr qreal kStretchFraction = 0.16;
constexpr qreal kMinStretch = 40.0;
constexpr qreal kMaxStretch = 120.0;
// Each further step eases out over this long, so a burst of sub-notch events
// reads as one continuous pull instead of a row of jerks.
constexpr int kStretchStepDurationMs = 90;
// The wheel has to stay quiet this long before the band lets go.
constexpr int kStretchReleaseDelayMs = 120;
// Springing back is quick and overshoots slightly into the content before
// settling, which is what makes the release feel elastic.
constexpr int kStretchReleaseDurationMs = 380;
constexpr qreal kReleaseAmplitude = 2.0;
constexpr qreal kReleasePeriod = 0.55;

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

// The visual size of the rubber band on one axis.
qreal stretchLimit(const QObject *flickable, bool vertical) {
    const qreal extent = realProperty(flickable, vertical ? "height" : "width");
    return qBound(kMinStretch, extent * kStretchFraction, kMaxStretch);
}

// Converts accumulated wheel distance past a bound into the visible offset.
// The rational curve starts at 1:1 and flattens out, so the first notch pulls
// noticeably while a held wheel quickly stops gaining ground.
qreal stretchOffset(qreal raw, qreal limit) {
    const qreal magnitude = qAbs(raw);
    if (magnitude <= 0.0 || limit <= 0.0)
        return 0.0;
    const qreal visual = limit * magnitude / (magnitude + limit);
    return raw < 0 ? -visual : visual;
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

    // Rubber band on a clipped view: wheel distance accumulated past an end,
    // signed (+ past the bottom/right bound). Non-zero means a pull is being
    // gathered; stretchActive means the content is visibly past the bound and
    // the spring back has not started yet.
    qreal stretchRaw = 0.0;
    bool stretchVertical = true;
    bool stretchActive = false;
    // The spring back lands exactly on the bound instead of on whatever
    // fraction of a pixel the elastic curve finished at.
    bool snapOnFinish = false;
    QPointer<QTimer> releaseTimer;
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
    state->snapOnFinish = false;
    state->stretchRaw = 0.0;
    state->stretchActive = false;
    if (state->releaseTimer)
        state->releaseTimer->stop();
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
    if (state->releaseTimer)
        state->releaseTimer->stop();
    // The animation and the release timer are parented to the Flickable (if it
    // still exists) and are destroyed with it; only the bookkeeping is ours to
    // free.
    delete state;
}

void animationFinished(QObject *flickable);
void startStretchStep(QObject *flickable, ScrollAnimation *state);
void releaseStretch(QObject *flickable, ScrollAnimation *state);

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

    auto *releaseTimer = new QTimer(flickable);
    releaseTimer->setSingleShot(true);
    state->releaseTimer = releaseTimer;
    QObject::connect(releaseTimer, &QTimer::timeout, flickable, [flickable] {
        ScrollAnimation *current = animations().value(flickable);
        if (current && current->stretchActive)
            releaseStretch(flickable, current);
    });

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
    state->snapOnFinish = false;
    state->stretchRaw = 0.0;
    state->stretchActive = false;
    if (state->releaseTimer)
        state->releaseTimer->stop();
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

// Where the stretched content should rest for the currently accumulated pull.
// The perpendicular axis keeps the position it was already resting on, clamped
// back into its own extents.
QPointF stretchTarget(QObject *flickable, const ScrollAnimation *state) {
    const bool vertical = state->stretchVertical;
    const ScrollRange verticalExtent = verticalRange(flickable);
    const ScrollRange horizontalExtent = horizontalRange(flickable);
    const ScrollRange range = vertical ? verticalExtent : horizontalExtent;
    const qreal bound = state->stretchRaw > 0 ? range.high : range.low;
    const qreal offset =
        stretchOffset(state->stretchRaw, stretchLimit(flickable, vertical));

    QPointF target = (state->gliding || state->bouncing)
                         ? state->target
                         : QPointF(realProperty(flickable, "contentX"),
                                   realProperty(flickable, "contentY"));
    target.setX(qBound(horizontalExtent.low, target.x(), horizontalExtent.high));
    target.setY(qBound(verticalExtent.low, target.y(), verticalExtent.high));
    if (vertical)
        target.setY(bound + offset);
    else
        target.setX(bound + offset);
    return target;
}

// Applies the accumulated pull: eases from wherever the content is now to the
// stretched position, and (re)arms the timer that lets the band go.
void startStretchStep(QObject *flickable, ScrollAnimation *state) {
    QVariantAnimation *animation = state ? state->animation : nullptr;
    if (!animation || qFuzzyIsNull(state->stretchRaw))
        return;

    const QPointF from(realProperty(flickable, "contentX"),
                       realProperty(flickable, "contentY"));
    const QPointF to = stretchTarget(flickable, state);

    state->reconfiguring = true;
    animation->stop();
    animation->setKeyValues(QVariantAnimation::KeyValues());
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setDuration(kStretchStepDurationMs);
    animation->setStartValue(from);
    animation->setEndValue(to);
    state->target = to;
    state->lastWritten = from;
    state->gliding = true;
    state->bouncing = false;
    state->bounceQueued = false;
    state->stretchActive = true;
    state->snapOnFinish = false;
    QMetaObject::invokeMethod(flickable, "cancelFlick");
    animation->start();
    state->reconfiguring = false;

    if (state->releaseTimer)
        state->releaseTimer->start(kStretchReleaseDelayMs);
}

// Lets the rubber band go: springs back to the bound the content was pulled
// away from, overshooting a little into the content on the way.
void releaseStretch(QObject *flickable, ScrollAnimation *state) {
    if (!state)
        return;
    if (state->releaseTimer)
        state->releaseTimer->stop();

    const bool wasActive = state->stretchActive;
    const bool vertical = state->stretchVertical;
    const ScrollRange range =
        vertical ? verticalRange(flickable) : horizontalRange(flickable);
    const qreal bound = state->stretchRaw > 0 ? range.high : range.low;
    state->stretchRaw = 0.0;
    state->stretchActive = false;
    if (!wasActive)
        return;

    QVariantAnimation *animation = state->animation;
    if (!animation)
        return;

    const QPointF from(realProperty(flickable, "contentX"),
                       realProperty(flickable, "contentY"));
    QPointF to = from;
    if (vertical)
        to.setY(bound);
    else
        to.setX(bound);

    QEasingCurve curve(QEasingCurve::OutElastic);
    curve.setAmplitude(kReleaseAmplitude);
    curve.setPeriod(kReleasePeriod);

    state->reconfiguring = true;
    animation->stop();
    animation->setKeyValues(QVariantAnimation::KeyValues());
    animation->setEasingCurve(curve);
    animation->setDuration(kStretchReleaseDurationMs);
    animation->setStartValue(from);
    animation->setEndValue(to);
    state->target = to;
    state->lastWritten = from;
    state->gliding = false;
    state->bouncing = true;
    state->bounceQueued = false;
    state->snapOnFinish = true;
    QMetaObject::invokeMethod(flickable, "cancelFlick");
    animation->start();
    state->reconfiguring = false;
}

void animationFinished(QObject *flickable) {
    ScrollAnimation *state = animations().value(flickable);
    if (!state)
        return;
    const bool wasBouncing = state->bouncing;
    const bool snap = state->snapOnFinish;
    state->gliding = false;
    state->bouncing = false;
    state->snapOnFinish = false;

    if (snap) {
        // The elastic curve can stop a fraction of a pixel short; land exactly
        // on the bound so the next wheel step starts from the extent.
        state->lastWritten = state->target;
        flickable->setProperty("contentX", state->target.x());
        flickable->setProperty("contentY", state->target.y());
        return;
    }
    if (!qFuzzyIsNull(state->stretchRaw) && !state->stretchActive) {
        // A pull gathered while the glide was still travelling to the end.
        startStretchStep(flickable, state);
        return;
    }
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
    const bool clipping = receiver->property("clip").toBool();
    const bool verticalPreferred =
        qAbs(angleDelta.y()) >= qAbs(angleDelta.x()) && angleDelta.y() != 0;
    const qreal dominantDelta = verticalPreferred ? deltaY : deltaX;

    // Already stretched past an end: more rotation in the same direction pulls
    // further out, anything else lets the band go and is applied normally.
    if (!qFuzzyIsNull(state->stretchRaw)) {
        const bool sameAxis = state->stretchVertical == verticalPreferred;
        const bool pulling = !qFuzzyIsNull(dominantDelta) &&
                             (dominantDelta > 0) == (state->stretchRaw > 0);
        if (sameAxis && pulling) {
            state->stretchRaw += dominantDelta;
            if (state->stretchActive)
                startStretchStep(receiver, state);
            // Otherwise a glide is still travelling to the end; the pull is
            // applied by animationFinished().
            event->accept();
            return true;
        }
        releaseStretch(receiver, state);
    }

    // While a glide is still running its destination, not the current
    // position, is what the next rotation adds to. Adding to the current
    // position instead would throw away every distance still in flight, which
    // is exactly how the stock implementation slows high-resolution wheels.
    const bool animating = state->gliding || state->bouncing;
    qreal baseX = animating ? state->target.x() : currentX;
    qreal baseY = animating ? state->target.y() : currentY;
    // A stretch deliberately rests outside the extents; clamping here keeps
    // that residual from inventing movement on the next ordinary step.
    baseX = qBound(horizontal.low, baseX, horizontal.high);
    baseY = qBound(vertical.low, baseY, vertical.high);

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
    // continues when this returns false).
    if (hasAncestorFlickable(receiver))
        return false;

    if (!clipping) {
        // The content is not clipped by this view, so stretching past an end
        // would draw it over the page below. Keep the small inward recoil.
        if (state->gliding) {
            // Still gliding towards the end: bounce once it settles there
            // rather than fighting the glide with a second animation.
            state->bounceQueued = true;
            state->bounceVertical = verticalPreferred;
            event->accept();
            return true;
        }
        if (state->bouncing) {
            // A bounce is already under way; a wheel still held against the
            // end must not restart or interrupt it.
            event->accept();
            return true;
        }
        // Leave the event alone when there is nothing to recoil from (a view
        // that does not scroll at all, or content shorter than its viewport);
        // Qt's own handler then does exactly what it would have done without
        // this filter.
        if (!startBounce(receiver, state, verticalPreferred))
            return false;
        event->accept();
        return true;
    }

    // A clipped view can show the pages below while the content is pulled past
    // its end, so it gets a real rubber band.
    const ScrollRange range = verticalPreferred ? vertical : horizontal;
    if (qFuzzyIsNull(dominantDelta) || range.high - range.low <= kBoundEpsilon)
        return false; // Nothing to stretch: leave Qt's own no-op intact.

    state->stretchVertical = verticalPreferred;
    state->stretchRaw = dominantDelta;
    if (state->gliding) {
        // The step that reaches the end is still gliding; stretch once it has
        // arrived instead of cutting the glide short.
        event->accept();
        return true;
    }
    startStretchStep(receiver, state);
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
