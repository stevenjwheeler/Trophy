/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend {

// Human-readable signal name ("SIGSEGV"), or "signal N" when unknown.
QString describeSignal(int signalNumber);

// Append fatal-signal and std::terminate reports to `logPath`.
//
// A crash otherwise leaves the log mid-sentence with no evidence: the Qt
// message handler only runs for messages, and a SIGSEGV never produces one.
// The handler writes a header (signal, fault address, version, time) plus a
// best-effort backtrace, then restores the default disposition and re-raises
// so core dumps still work.
//
// Call once, after the log path is known. Safe to call again (no-op).
void installCrashHandler(const QString &logPath);

// Testing hooks: install against an already-open append-log descriptor.
void installCrashHandlerForFd(int fd, const char *version);

} // namespace TrophyBackend
