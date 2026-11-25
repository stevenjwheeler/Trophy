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

// A positive angleDelta is a wheel turned away from the user; QQuickFlickable
// then decreases contentY/contentX. The stock implementation was measured to
// move +72 px for angleDelta -120, which is what these expectations mirror.
TEST(WheelScrollNormalizer, OneNotchMovesOneScrollDistance) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, HorizontalWheelMovesContentX) {
    Scene scene(kHorizontalFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(-kNotch, 0));
    EXPECT_NEAR(contentX(scene), notchDistance(), 0.01);
    EXPECT_NEAR(contentY(scene), 0.0, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(kNotch, 0));
    EXPECT_NEAR(contentX(scene), 0.0, 0.01);
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
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, RapidFullNotchesAreNotThrottled) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // Ten ordinary notches in quick succession must travel ten notches.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 10);
    EXPECT_NEAR(contentY(scene), notchDistance() * 10, 0.01);
    // Reversing immediately keeps every step.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch), 4);
    EXPECT_NEAR(contentY(scene), notchDistance() * 6, 0.01);
}

TEST(WheelScrollNormalizer, SameMillisecondEventsAreNotDropped) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // No waits at all: every event arrives in the same millisecond. The stock
    // implementation drops all but the first because it cannot compute a
    // velocity from a zero elapsed time.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -15), 32);
    EXPECT_NEAR(contentY(scene), notchDistance() * 32 / 8.0, 0.01);
}

TEST(WheelScrollNormalizer, FractionalRotationAccumulatesWithoutRounding) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // 40 events of 3 units are 1/40 of a notch each: 72 px in total, not 0.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -3), 40);
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
}

TEST(WheelScrollNormalizer, BothDirections) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch));
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch / 2));
    EXPECT_NEAR(contentY(scene), notchDistance() * 1.5, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, kNotch));
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
    EXPECT_NEAR(contentY(scene), -30.0, 0.01);
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 100);
    EXPECT_NEAR(contentY(scene), 710.0, 0.01);
}

TEST(WheelScrollNormalizer, StopsAtBoundsWithoutSwallowingTheEvent) {
    Scene scene(kSingleFlickable);
    ASSERT_NE(scene.item("flick"), nullptr);
    // 4000 px of content in a 300 px viewport: 3700 px of travel.
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 200);
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);
    // Scrolling further does nothing, and the event stays available for a
    // parent scrollable (see NestedFlickableTakesOverAtBounds).
    wheel(&scene.window, QPoint(200, 150), QPoint(0, -kNotch), 5);
    EXPECT_NEAR(contentY(scene), 3700.0, 0.01);
}

TEST(WheelScrollNormalizer, NestedFlickableTakesOverAtBounds) {
    Scene scene(kNestedFlickables);
    ASSERT_NE(scene.item("inner"), nullptr);
    ASSERT_NE(scene.item("outer"), nullptr);
    // The inner Flickable can scroll: it keeps the wheel.
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
    EXPECT_NEAR(contentY(scene, "inner"), notchDistance(), 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), 0.0, 0.01);
    // 53 further notches reach -3816; the 54th is clamped to the inner
    // Flickable's 3850 px end and still belongs to it.
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch), 53);
    EXPECT_NEAR(contentY(scene, "inner"), 3850.0, 0.01);
    EXPECT_NEAR(contentY(scene, "outer"), 0.0, 0.01);
    // Once it is at its end the outer one scrolls instead.
    wheel(&scene.window, QPoint(200, 75), QPoint(0, -kNotch));
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
    EXPECT_EQ(scene.root->property("wheelEvents").toInt(), 1);
    EXPECT_NEAR(contentY(scene), notchDistance(), 0.01);
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
    const qreal afterNormal = contentY(scene);
    flickable->setProperty("contentY", 0.0);
    ASSERT_TRUE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                  &inverted));
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
    Scene scene(kSingleFlickable);
    QQuickItem *flickable = scene.item("flick");
    ASSERT_NE(flickable, nullptr);
    flickable->setProperty("contentY", 0.0);
    // Declining lets the delivery loop pass the event to a parent Flickable
    // (see NestedFlickableTakesOverAtBounds), so the return value matters.
    WheelEvent up(QPoint(0, kNotch));
    EXPECT_FALSE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                   &up));
    WheelEvent down(QPoint(0, -kNotch));
    EXPECT_TRUE(TrophyBackend::WheelScrollNormalizer::consumeWheel(flickable,
                                                                  &down));
    EXPECT_TRUE(down.isAccepted());
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
