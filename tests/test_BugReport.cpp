/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/BugReport.h"

#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest/QtTest>
#include <gtest/gtest.h>

using namespace TrophyBackend;

TEST(BugReportTest, LogPathEndsWithTrophyLog) {
  EXPECT_TRUE(bugReportLogPath().endsWith(QStringLiteral("/trophy/trophy.log")));
}

TEST(BugReportTest, MissingFileReadsEmpty) {
  EXPECT_TRUE(
      readLogTail(QStringLiteral("/nonexistent/trophy.log")).isEmpty());
}

TEST(BugReportTest, ShortFileReadsFully) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.path() + QStringLiteral("/trophy.log");
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
  file.write("line1\nline2\n");
  file.close();
  EXPECT_EQ(readLogTail(path), QStringLiteral("line1\nline2"));
}

TEST(BugReportTest, KeepsOnlyLastLines) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.path() + QStringLiteral("/trophy.log");
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
  QStringList lines;
  for (int i = 0; i < 10; ++i)
    lines << QStringLiteral("line%1").arg(i);
  file.write(lines.join(u'\n').toUtf8());
  file.close();
  EXPECT_EQ(readLogTail(path, 3, 100000),
            QStringLiteral("line7\nline8\nline9"));
}

TEST(BugReportTest, TruncatesToMaxCharsFromEnd) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.path() + QStringLiteral("/trophy.log");
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
  file.write("abcdefghij");
  file.close();
  EXPECT_EQ(readLogTail(path, 50, 4), QStringLiteral("ghij"));
}

TEST(BugReportTest, UrlPrefillsIssueWithVersionAndTail) {
  const QUrl url = bugReportUrl(QStringLiteral("1.2.3"),
                                QStringLiteral("boom happened"));
  EXPECT_EQ(url.host(), QStringLiteral("github.com"));
  EXPECT_TRUE(url.path().endsWith(QStringLiteral("/issues/new")));
  const QUrlQuery query(url);
  EXPECT_EQ(query.queryItemValue(QStringLiteral("labels")),
            QStringLiteral("bug"));
  EXPECT_EQ(query.queryItemValue(QStringLiteral("title")),
            QStringLiteral("[Bug]: "));
  const QString body = query.queryItemValue(QStringLiteral("body"));
  EXPECT_TRUE(body.contains(QStringLiteral("1.2.3")));
  EXPECT_TRUE(body.contains(QStringLiteral("boom happened")));
  EXPECT_TRUE(body.contains(bugReportLogPath()));
}

TEST(BugReportTest, UrlFallsBackOnEmptyInputs) {
  const QUrl url = bugReportUrl({}, {});
  const QUrlQuery query(url);
  const QString body = query.queryItemValue(QStringLiteral("body"));
  EXPECT_TRUE(body.contains(QStringLiteral("unknown")));
  EXPECT_TRUE(
      body.contains(QStringLiteral("(log unavailable or empty)")));
}
