/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick

// One persistent image animation for the whole library, including view switches.
// Only its visual parent moves into the active thumbnail; QObject ownership
// stays with the dashboard. A short delay skips fleeting hovers and lets
// synchronous delegate rebinds settle before loading another video.
Item {
    id: root
    required property Item parkingItem
    property Item target: null
    property Item pendingTarget: null
    property Item failedTarget: null
    property url failedSource
    property url pendingSource
    property url targetSource
    property url playbackSource
    property url receivedSource
    // The cache can be supplied explicitly; this default supports existing hosts.
    property var cache: typeof previewCache !== "undefined" && previewCache ? previewCache : null // qmllint disable unqualified
    property int cacheRequestId: 0
    property bool awaitingCache: false
    readonly property url source: playbackSource
    property string frameKey: ""
    property int frameCount: 0
    property int fps: 15
    property int frameIndex: 0
    readonly property int position: Math.floor(frameIndex * 1000 / fps)
    readonly property int duration: Math.floor(frameCount * 1000 / fps)
    readonly property bool playing: frameClock.running
    Component.onDestruction: {
        if (cache && frameKey.length)
            cache.releaseFrames(frameKey)
        if (cache && awaitingCache)
            cache.cancelPreview(cacheRequestId)
    }
    property bool frameReady: false
    readonly property int fadeDuration: 180
    property int loopStart: 0

    anchors.fill: parent
    visible: target !== null && frameReady
    opacity: 0
    NumberAnimation {
        id: fadeIn
        target: root
        property: "opacity"
        from: 0
        to: 1
        duration: root.fadeDuration
        easing.type: Easing.OutCubic
    }

    function chooseStart(durationMs, fraction) {
        const lastStart = Math.floor(Math.max(0, durationMs - Math.min(6000, durationMs / 2)))
        return lastStart > 0 ? 1 + Math.floor(fraction * lastStart) : 0
    }

    function park() {
        fadeIn.stop()
        opacity = 0
        const cancelled = cacheRequestId
        ++cacheRequestId
        if (awaitingCache && cache)
            cache.cancelPreview(cancelled)
        awaitingCache = false
        frameReady = false
        if (frameKey.length && cache)
            cache.releaseFrames(frameKey)
        frameKey = ""
        frameCount = 0
        parent = parkingItem
    }

    onTargetChanged: {
        // Also handles a delegate disappearing without an explicit release.
        if (!target) {
            park()
            if (!pendingTarget)
                idleCleanup.restart()
        }
    }
    onPendingTargetChanged: {
        if (!pendingTarget) {
            hoverDelay.stop()
            if (!target)
                idleCleanup.restart()
        }
    }

    function request(host, url) {
        if (!host || !url.toString().length)
            return
        if (failedTarget === host) {
            if (failedSource.toString() === url.toString())
                return
            failedTarget = null
        }
        if ((pendingTarget === host && pendingSource.toString() === url.toString())
                || (target === host && targetSource.toString() === url.toString()))
            return
        idleCleanup.stop()
        if (target)
            target = null
        pendingSource = url
        pendingTarget = host
        hoverDelay.restart()
    }

    function release(host) {
        // A late exit from the old delegate must never cancel its successor.
        if (pendingTarget === host)
            pendingTarget = null
        if (target === host)
            target = null
        if (failedTarget === host)
            failedTarget = null
        if (!target && !pendingTarget)
            idleCleanup.restart()
    }

    function seek(ms) {
        frameIndex = Math.max(0, Math.min(frameCount - 1, Math.floor(ms * fps / 1000)))
    }

    function activatePending() {
        const host = pendingTarget
        const url = pendingSource
        if (!host)
            return
        pendingTarget = null
        park()
        targetSource = url
        target = host
        parent = host
        idleCleanup.stop()
        if (!cache) {
            fail("Preview cache is unavailable")
            return
        }
        awaitingCache = true
        ++cacheRequestId
        cache.requestPreview(url, cacheRequestId)
    }

    function useCachedPreview(sourceUrl: url, requestId: int, previewUrl: url, key: string, count: int, rate: int) {
        // Signal URLs can arrive as fully encoded JS strings; URL properties
        // use QUrl's display form. Normalize before comparing (spaces/#/%).
        receivedSource = sourceUrl
        if (!awaitingCache || !target || requestId !== cacheRequestId
                || receivedSource.toString() !== targetSource.toString())
            return
        awaitingCache = false
        if (!previewUrl.toString().length || !key.length || count < 1 || count > 900 || rate < 1 || rate > 30) {
            fail("Could not generate cached preview")
            return
        }
        playbackSource = previewUrl
        cache.retainFrames(key)
        frameCount = count
        fps = rate
        loopStart = chooseStart(duration, Math.random())
        seek(loopStart)
        frameKey = key
    }

    Connections {
        target: root.cache
        function onPreviewFramesReady(source, requestId, preview, key, count, fps) {
            root.useCachedPreview(source, requestId, preview, key, count, fps)
        }
    }

    function fail(reason) {
        if (!target)
            return
        console.warn("Hover preview failed:", targetSource, reason)
        failedSource = targetSource
        failedTarget = target
        target = null
    }

    Timer {
        id: hoverDelay
        objectName: "hoverPreviewStart"
        interval: 50
        onTriggered: root.activatePending()
    }
    Timer {
        id: idleCleanup
        // Pause/hide immediately, but don't tear down the media backend for
        // every small gap between rows. Idle eventually releases the file.
        interval: 1500
        onTriggered: {
            if (!root.target && !root.pendingTarget)
                root.playbackSource = ""
        }
    }
    Image {
        objectName: "hoverPreviewImage"
        anchors.fill: parent
        fillMode: Image.PreserveAspectCrop
        cache: false
        source: root.frameKey.length ? "image://hoverpreview/" + root.frameKey + "/" + root.frameIndex : ""
        onStatusChanged: {
            if (status === Image.Ready && root.target && !root.frameReady) {
                root.frameReady = true
                fadeIn.restart()
            } else if (status === Image.Error && root.target && root.frameKey.length) {
                root.fail("Preview frame unavailable")
            }
        }
    }
    Timer {
        id: frameClock
        interval: Math.round(1000 / root.fps)
        running: root.target !== null && root.frameReady && root.frameCount > 1
        repeat: true
        onTriggered: {
            const start = Math.floor(root.loopStart * root.fps / 1000)
            root.frameIndex = root.frameIndex + 1 >= Math.min(root.frameCount, start + root.fps * 6)
                ? start : root.frameIndex + 1
        }
    }
}
