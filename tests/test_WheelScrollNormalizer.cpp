/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/WheelScrollNormalizer.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <gtest/gtest.h>
#include <memory>

namespace {

constexpr int kNotch = 120;
// QQuickFlickable's nominal distance for one wheel notch:
// wheelScrollLines() * 24 (24 is its height of one scroll line).
qreal notchDistance() {
    return QGuiApplication::styleHints()->wheelScrollLines() * 24.0;
}

// QWheelEvent's copy constructor is protected, so events are built in place.
struct WheelEvent : QWheelEvent {
    WheelEvent(QPoint angleDelta,
               Qt::MouseEventSource source = Qt::MouseEventNotSynthesized,
               bool inverted = false, QPoint pixelDelta = QPoint())
        : QWheelEvent(QPointF(10, 10), QPointF(10, 10), pixelDelta, angleDelta,
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, inverted,
                      source) {}
};

const char *kSingleFlickable = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        contentWidth: width
        contentHeight: 4000
    }
}
)";

const char *kHorizontalFlickable = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        contentWidth: 4000
        contentHeight: height
    }
}
)";

// A ScrollView's internal Flickable is clipped, and the library lists set
// clip: true. Those views may stretch past an end: nothing spills over the
// page below.
const char *kClippedFlickable = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: 4000
    }
}
)";

const char *kClippedHorizontalFlickable = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        clip: true
        contentWidth: 4000
        contentHeight: height
    }
}
)";

const char *kNestedFlickables = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "outer"
        anchors.fill: parent
        contentHeight: 4000
        Item {
            width: 400
            height: 150
            Flickable {
                objectName: "inner"
                width: 400
                height: 150
                contentHeight: 4000
            }
        }
    }
}
)";

// Same layout, but the inner Flickable is shorter than its viewport.
const char *kNestedFixedInner = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "outer"
        anchors.fill: parent
        contentHeight: 4000
        Item {
            width: 400
            height: 150
            Flickable {
                objectName: "inner"
                width: 400
                height: 150
                contentHeight: 100
            }
        }
    }
}
)";

// The same nesting with both views clipped: the outer one may rubber-band, but
// the inner one still has to hand the wheel over when it runs out of travel.
const char *kClippedNestedFlickables = R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "outer"
        anchors.fill: parent
        clip: true
        contentHeight: 4000
        Item {
            width: 400
            height: 150
            Flickable {
                objectName: "inner"
                width: 400
                height: 150
                clip: true
                contentHeight: 4000
            }
        }
    }
}
)";

const char *kHandlerInsideFlickable = R"(
import QtQuick

Item {
    id: scene
    width: 400
    height: 300
    property int wheelEvents: 0
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        contentHeight: 4000
        Rectangle {
            width: 400
            height: 150
            color: "transparent"
            WheelHandler {
                onWheel: (event) => {
                    scene.wheelEvents += 1;
                    event.accepted = true;
                }
            }
        }
    }
}
)";

struct Scene {
    QQmlEngine engine;
    QQuickWindow window;
    std::unique_ptr<QObject> object;
    QQuickItem *root = nullptr;

    explicit Scene(const char *qml, QSize size = QSize(400, 300)) {
        window.resize(size);
        QQmlComponent component(&engine);
        component.setData(qml, QUrl(QStringLiteral("qrc:/wheel-test.qml")));
        EXPECT_TRUE(component.isReady()) << component.errorString().toStdString();
        object.reset(component.create());
        root = qobject_cast<QQuickItem *>(object.get());
        EXPECT_NE(root, nullptr) << component.errorString().toStdString();
        if (!root)
            return;
        root->setParentItem(window.contentItem());
        root->setPosition(QPointF(0, 0));
        window.show();
        QTest::qWait(20);
    }

    QQuickItem *item(const char *name) const {
        return root ? root->findChild<QQuickItem *>(QString::fromLatin1(name))
                    : nullptr;
    }
};

// QTest::wheelEvent() feeds the event through the window system interface with
// a Qt::MouseEventNotSynthesized source, exactly like a real mouse wheel. It
// also processes pending events, so the Flickable is up to date on return.
void wheel(QQuickWindow *window, QPoint position, QPoint angleDelta,
           int count = 1) {
    for (int i = 0; i < count; ++i)
        QTest::wheelEvent(window, position, angleDelta);
}

// Wheel steps glide for 110 ms. Reaching an end starts a 120 ms quiet period
// before the band lets go, and the spring back runs for 380 ms; wait for the
// longest sequence to come to rest.
constexpr int kSettleMs = 700;
void settle() { QTest::qWait(kSettleMs); }

qreal contentPosition(const Scene &scene, const char *name, const char *axis) {
    QQuickItem *flickable = scene.item(name);
    EXPECT_NE(flickable, nullptr) << name;
    return flickable ? flickable->property(axis).toReal() : 0.0;
}

qreal contentY(const Scene &scene, const char *name = "flick") {
    return contentPosition(scene, name, "contentY");
}

qreal contentX(const Scene &scene, const char *name = "flick") {
    return contentPosition(scene, name, "contentX");
}

// Samples a content position while a bounce runs, so the recoil can be
// asserted without relying on a single sample landing exactly on its peak.
struct PositionRange {
    qreal minimum = 0;
    qreal maximum = 0;
};

std::shared_ptr<PositionRange> trackPosition(QQuickItem *item, const char *axis,
                                             qreal initial) {
    auto range = std::make_shared<PositionRange>();
    range->minimum = range->maximum = initial;
    auto *timer = new QTimer(item);
    timer->setInterval(4);
    QObject::connect(timer, &QTimer::timeout, timer, [range, item, axis] {
        const qreal value = item->property(axis).toReal();
        range->minimum = qMin(range->minimum, value);
        range->maximum = qMax(range->maximum, value);
    });
    timer->start();
    return range;
}

// A positive angleDelta is a wheel turned away from the user; QQuickFlickable
// then decreases contentY/contentX. The stock implementation was measured to
// move +72 px for angleDelta -120, which is what these expectations mirror.
TEST(WheelScrollNormalizer, OneNotchMovesOneScrollDistance) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, HorizontalWheelMovesContentX) {
    Scene scene(kHorizontalFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(-kNotch, 0));
    settle();
    EXPECT_NEAR(contentX(scene), notchDistance(), 0.01);
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(kNotch, 0));
    settle();
    EXPECT_NEAR(contentX(scene), 0.0, 0.01);
}

// The step is a short glide, not a jump: right after the event the view is
// still on its way, and it lands exactly on the rotation-derived distance.
TEST(WheelScrollNormalizer, EachStepGlidesToItsDestination) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    const qreal moving = contentY(scene);
    EXPECT_GE(moving, 0.0);
    EXPECT_LT(moving, notchDistance() - 1.0);
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

// A high-resolution wheel reports one notch as several sub-notch events a few
// milliseconds apart. Each must contribute its exact share: with the stock
// QQuickFlickable animation only the first fraction of every event survives.
TEST(WheelScrollNormalizer, SubNotchEventsAddUpToOneNotch) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    const int steps = 8;
    const int step = kNotch / steps;
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -step), steps);
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

// Events that arrive while the previous step is still gliding must add onto
// its destination. Adding onto the current (still animating) position would
// silently throw away the in-flight distance.
TEST(WheelScrollNormalizer, StepsDuringGlideAccumulateWithoutLoss) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    for (int i = 0; i < 8; ++i) {
        QTest::wheelEvent(&scene.window, QPoint(200, 150), QPoint(0, -15));
        QTest::qWait(5); // still mid-glide when the next event arrives
    }
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, RapidFullNotchesAreNotThrottled) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // Ten ordinary notches in quick succession must travel ten notches.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 10);
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance() * 10, 0.01);
    // Reversing immediately keeps every step.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch), 4);
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance() * 6, 0.01);
}

TEST(WheelScrollNormalizer, ReversingMidGlideReturnsToTheStart) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    QTest::qWait(20);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch));
    settle();
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
}

TEST(WheelScrollNormalizer, SameMillisecondEventsAreNotDropped) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // No waits at all: every event arrives in the same millisecond. The stock
    // implementation drops all but the first because it cannot compute a
    // velocity from a zero elapsed time.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -15), 32);
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance() * 32 / 8.0, 0.01);
}

TEST(WheelScrollNormalizer, FractionalRotationAccumulatesWithoutRounding) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // 40 events of 3 units are 1/40 of a notch each: 72 px in total, not 0.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -3), 40);
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, BothDirections) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch / 2));
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance() * 1.5, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch));
    settle();
    EXPECT_NEAR(contentY(scene), notchDistance() * 0.5, 0.01);
}

TEST(WheelScrollNormalizer, TopMarginIsPartOfTheRange) {
    Scene scene(R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        contentHeight: 1000
        topMargin: 30
        bottomMargin: 10
    }
}
)");
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    // The stock Flickable rests at -topMargin and ends at
    // height - contentHeight - bottomMargin in the negated coordinate.
    EXPECT_NEAR(contentY(scene), -30.0, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch), 5);
    settle();
    EXPECT_NEAR(contentY(scene), -30.0, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 100);
    settle();
    EXPECT_NEAR(contentY(scene), 710.0, 0.01);
}

// An unclipped Flickable cannot show content outside its own rectangle without
// drawing it over the page below, so the end of the content answers with a
// small inward recoil rather than a stretch.
TEST(WheelScrollNormalizer, UnclippedFlickableRecoilsAndSettlesAtTheEnd) {
    Scene scene(kSingleFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    // 4000 px of content in a 300 px viewport: 3700 px of travel.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 200);
    settle();
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);

    const auto range = trackPosition(flick, "contentY", 3700.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    settle();
    // It recoiled inward (contentY decreases at the bottom) and returned.
    EXPECT_LE(range->minimum, 3700.0 - 6.0);
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);
}

TEST(WheelScrollNormalizer, UnclippedBounceRecoilsAtTheTopToo) {
    Scene scene(kSingleFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    const auto range = trackPosition(flick, "contentY", 0.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch));
    settle();
    EXPECT_GE(range->maximum, 6.0);
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
}

// The same recoil applies to the axis that ran out of travel, whichever that
// is: here the view scrolls sideways only.
TEST(WheelScrollNormalizer, UnclippedHorizontalBounceRecoilsAtTheRightEdge) {
    Scene scene(kHorizontalFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(-kNotch, 0), 100);
    settle();
    EXPECT_NEAR(contentX(scene), 3600.0, 0.01);

    const auto range = trackPosition(flick, "contentX", 3600.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(-kNotch, 0));
    settle();
    EXPECT_LE(range->minimum, 3600.0 - 6.0);
    EXPECT_NEAR(contentX(scene), 3600.0, 0.01);
}

// A clipped view can reveal the page below while the content is pulled past an
// end, so there the wheel gets a real rubber band: the content really moves
// outwards, then springs back.
TEST(WheelScrollNormalizer, ClippedViewStretchesPastTheBottom) {
    Scene scene(kClippedFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 60);
    settle();
    ASSERT_NEAR(contentY(scene), 3700.0, 0.01);

    const auto range = trackPosition(flick, "contentY", 3700.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    settle();
    // One notch of pull is a good fraction of the 48 px cap for a 300 px
    // viewport, and the band lands back exactly on the extent.
    EXPECT_GE(range->maximum, 3700.0 + 20.0);
    EXPECT_LE(range->maximum, 3700.0 + 48.0);
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);
}

// Holding the wheel against an end must stop stretching after a short distance
// instead of dragging the content away without limit.
TEST(WheelScrollNormalizer, ClippedStretchSaturates) {
    Scene scene(kClippedFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 60);
    settle();
    ASSERT_NEAR(contentY(scene), 3700.0, 0.01);

    const auto range = trackPosition(flick, "contentY", 3700.0);
    for (int i = 0; i < 8; ++i) {
        QTest::wheelEvent(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
        QTest::qWait(5);
    }
    settle();
    // Eight notches pull far past one notch, but never past the cap.
    EXPECT_GE(range->maximum, 3700.0 + 35.0);
    EXPECT_LE(range->maximum, 3700.0 + 48.0);
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);
}

// The release is a spring, not a straight line: it overshoots a little way back
// into the content once before settling on the bound.
TEST(WheelScrollNormalizer, ClippedStretchSpringsBackElastically) {
    Scene scene(kClippedFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 60);
    settle();
    ASSERT_NEAR(contentY(scene), 3700.0, 0.01);

    const auto range = trackPosition(flick, "contentY", 3700.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    settle();
    // Past the bound outwards, then past it inwards by a couple of pixels.
    EXPECT_GE(range->maximum, 3700.0 + 20.0);
    EXPECT_LE(range->minimum, 3699.0);
    EXPECT_GE(range->minimum, 3680.0);
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);
}

// Turning the wheel back while the band is still held lets go at once: the
// reversal must not wait out the quiet period or the spring.
TEST(WheelScrollNormalizer, ClippedStretchReleasesImmediatelyOnReversal) {
    Scene scene(kClippedFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 60);
    settle();
    ASSERT_NEAR(contentY(scene), 3700.0, 0.01);

    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    QTest::qWait(60); // still inside the 120 ms hold
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch));
    QTest::qWait(200);
    EXPECT_LT(contentY(scene), 3660.0);
    settle();
    EXPECT_NEAR(contentY(scene), 3700.0 - notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, ClippedStretchWorksAtTheTopToo) {
    Scene scene(kClippedFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    const auto range = trackPosition(flick, "contentY", 0.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch));
    settle();
    EXPECT_LE(range->minimum, -20.0);
    EXPECT_GE(range->minimum, -48.0);
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
}

TEST(WheelScrollNormalizer, ClippedStretchWorksHorizontally) {
    Scene scene(kClippedHorizontalFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(-kNotch, 0), 60);
    settle();
    ASSERT_NEAR(contentX(scene), 3600.0, 0.01);

    const auto range = trackPosition(flick, "contentX", 3600.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(-kNotch, 0));
    settle();
    // 400 px wide viewport: the cap is 64 px.
    EXPECT_GE(range->maximum, 3600.0 + 20.0);
    EXPECT_LE(range->maximum, 3600.0 + 64.0);
    EXPECT_NEAR(contentX(scene), 3600.0, 0.01);
}

// A clipped inner view must still hand the wheel to its ancestor rather than
// pulling a rubber band of its own.
TEST(WheelScrollNormalizer, NestedClippedFlickableStillHandsOff) {
    Scene scene(kClippedNestedFlickables);
    QQuickItem *inner = scene.item("inner");
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(scene.item("outer"), nullptr);
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch), 54);
    settle();
    EXPECT_NEAR(contentY(scene, "inner"), 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), 0.0, 0.01);

    const auto innerRange = trackPosition(inner, "contentY", 3850.0);
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
    settle();
    EXPECT_NEAR(innerRange->maximum, 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "inner"), 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), notchDistance(), 0.01);
}

// A view whose content fits in its viewport has nowhere to go: it must not
// fake a bounce, and the wheel event stays available for anything else.
TEST(WheelScrollNormalizer, ContentThatFitsDoesNotBounce) {
    Scene scene(R"(
import QtQuick

Item {
    width: 400
    height: 300
    Flickable {
        objectName: "flick"
        anchors.fill: parent
        clip: true
        contentHeight: 100
        contentWidth: width
    }
}
)");
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    const auto range = trackPosition(flick, "contentY", 0.0);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    settle();
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
    EXPECT_NEAR(range->minimum, 0.0, 0.01);
    EXPECT_NEAR(range->maximum, 0.0, 0.01);

    WheelEvent down(QPoint(0, -kNotch));
    down.ignore();
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flick, &down));
    EXPECT_FALSE(down.isAccepted());
}

// A wheel on an axis with no travel at all is left to Qt instead of being
// swallowed with a recoil the view cannot use.
TEST(WheelScrollNormalizer, AxisWithoutTravelIsNotConsumed) {
    Scene scene(kSingleFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    WheelEvent sideways(QPoint(-kNotch, 0));
    sideways.ignore();
    EXPECT_FALSE(
        TrophyBackend::WheelScrollNormalizer::consumeWheel(flick, &sideways));
    EXPECT_FALSE(sideways.isAccepted());
    EXPECT_NEAR(contentX(scene), 0.0, 0.01);
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
}

TEST(WheelScrollNormalizer, NestedFlickableTakesOverAtBounds) {
    Scene scene(kNestedFlickables);
    ASSERT_NE(scene.item("inner"), nullptr);
    ASSERT_NE(scene.item("outer"), nullptr);
    // The inner Flickable can scroll: it keeps the wheel.
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
    settle();
    EXPECT_NEAR(contentY(scene, "inner"), notchDistance(), 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), 0.0, 0.01);
    // 53 further notches reach -3816; the 54th is clamped to the inner
    // Flickable's 3850 px end and still belongs to it.
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch), 53);
    settle();
    EXPECT_NEAR(contentY(scene, "inner"), 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), 0.0, 0.01);
    // Once it is at its end the outer one scrolls instead, and the inner one
    // does not recoil: the handoff wins over the bounce.
    QQuickItem *inner = scene.item("inner");
    const auto innerRange = trackPosition(inner, "contentY", 3850.0);
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
    settle();
    EXPECT_NEAR(innerRange->minimum, 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "inner"), 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, NestedFlickableTakesOverWhenInnerCannotScroll) {
    Scene scene(kNestedFixedInner);
    ASSERT_NE(scene.item("inner"), nullptr);
    ASSERT_NE(scene.item("outer"), nullptr);
    // The inner one has nowhere to go; the wheel must reach the outer
    // Flickable instead of being consumed by a no-op.
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
    settle();
    EXPECT_NEAR(contentY(scene, "inner"), 0.0, 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), notchDistance(), 0.01);
}

// A pointer handler under the cursor (the player's zoom handler, a slider, a
// spin box) must keep its own wheel behaviour.
TEST(WheelScrollNormalizer, PointerHandlerUnderTheCursorKeepsTheWheel) {
    Scene scene(kHandlerInsideFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
    EXPECT_EQ(scene.root->property("wheelEvents").toInt(), 1);
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
    // Below the handler the Flickable scrolls normally.
    wheel(&scene.window, QPoint(200, 250), QPoint(0, -kNotch));
    settle();
    EXPECT_EQ(scene.root->property("wheelEvents").toInt(), 1);
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

// An external jump (Settings scrolls a section into view with its own
// NumberAnimation) must take over from a glide that is still running.
TEST(WheelScrollNormalizer, ExternalJumpCancelsARunningGlide) {
    Scene scene(kSingleFlickable);
    QQuickItem *flick = scene.item("flick");
    ASSERT_NE(flick, nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    QTest::qWait(20);
    flick->setProperty("contentY", 500.0);
    settle();
    EXPECT_NEAR(contentY(scene), 500.0, 0.01);
}

// Direct checks of the decision helper: these cover event properties the
// window system interface cannot set through QTest::wheelEvent.
TEST(WheelScrollNormalizerTest, TouchpadPixelEventsAreLeftToQt) {
    Scene scene(kSingleFlickable);
    QQuickItem *flickable = scene.item("flick");
    ASSERT_NE(flickable, nullptr);
    WheelEvent touchpad(QPoint(0, -120), Qt::MouseEventSynthesizedBySystem,
                        false, QPoint(0, -40));
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                   &touchpad));
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
}

TEST(WheelScrollNormalizerTest, InvertedFlagFollowsTheAngleDelta) {
    Scene scene(kSingleFlickable);
    QQuickItem *flickable = scene.item("flick");
    ASSERT_NE(flickable, nullptr);
    // Qt's Flickable ignores QWheelEvent::inverted(): the compositor already
    // flipped angleDelta for natural scrolling. Mirror that, so both flags
    // produce the same travel for the same delta.
    WheelEvent normal(QPoint(0, -kNotch));
    WheelEvent inverted(QPoint(0, -kNotch), Qt::MouseEventNotSynthesized, true);
    ASSERT_TRUE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                  &normal));
    settle();
    const qreal afterNormal = contentY(scene);
    flickable->setProperty("contentY", 0.0);
    ASSERT_TRUE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                  &inverted));
    settle();
    EXPECT_NEAR(contentY(scene), afterNormal, 0.01);
}

TEST(WheelScrollNormalizerTest, NonInteractiveOrDisabledFlickableIsIgnored) {
    Scene scene(kSingleFlickable);
    QQuickItem *flickable = scene.item("flick");
    ASSERT_NE(flickable, nullptr);
    flickable->setProperty("interactive", false);
    WheelEvent event(QPoint(0, -kNotch));
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                   &event));
    flickable->setProperty("interactive", true);
    flickable->setProperty("enabled", false);
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                   &event));
    flickable->setProperty("enabled", true);
    EXPECT_TRUE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                  &event));
    settle();
}

TEST(WheelScrollNormalizerTest, EventsWithoutRotationAreIgnored) {
    Scene scene(kSingleFlickable);
    QQuickItem *flickable = scene.item("flick");
    ASSERT_NE(flickable, nullptr);
    WheelEvent empty(QPoint(0, 0));
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                   &empty));
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(nullptr,
                                                                   &empty));
}

TEST(WheelScrollNormalizerTest, EventIsReportedConsumedOnlyWhenSomethingMoved) {
    // The inner Flickable rests at its top, so scrolling up cannot move it and
    // the event must stay available for the outer Flickable.
    Scene scene(kNestedFlickables);
    QQuickItem *inner = scene.item("inner");
    ASSERT_NE(inner, nullptr);
    WheelEvent up(QPoint(0, kNotch));
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(inner, &up));
    WheelEvent down(QPoint(0, -kNotch));
    EXPECT_TRUE(TrophyBackend::WheelScrollNormalizer::consumeWheel(inner, &down));
    EXPECT_TRUE(down.isAccepted());
    settle();
}

} // namespace

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");
    QGuiApplication app(argc, argv);
    // The filter is what the application installs; keep it alive for the run.
    TrophyBackend::WheelScrollNormalizer::install();
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
