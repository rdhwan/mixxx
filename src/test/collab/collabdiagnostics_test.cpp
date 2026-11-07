#include "collab/collabdiagnostics.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QList>
#include <QStringList>
#include <QTemporaryDir>

#include "collab/clocksync.h"

namespace mixxx {
namespace collab {

namespace {

QStringList readLines(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

} // namespace

TEST(CollabDiagnosticsTest, WritesHeaderOnceAndRowsWithMatchingColumns) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("diagnostics.csv"));

    {
        CollabDiagnostics diagnostics;
        ASSERT_TRUE(diagnostics.open(path));
        EXPECT_TRUE(diagnostics.isOpen());

        CollabClockSample sample;
        sample.t1Us = 1000;
        sample.t2Us = 6000;
        sample.t3Us = 6050;
        sample.t4Us = 2050;
        sample.rttUs = 2000;
        sample.offsetUs = 5000;
        sample.midUs = 1500;

        diagnostics.logClockSample(2000, 7000, sample, 5000, 10, 12.5);
        diagnostics.logScheduledCommand(42, 10000, 10020, 20, false);
        diagnostics.logScheduledCommand(43, 10000, 30000, 20000, true);
        diagnostics.logClockJump(4000, 25000, 2);
    }

    const QStringList lines = readLines(path);
    ASSERT_EQ(lines.size(), 5);
    EXPECT_TRUE(lines[0].startsWith(QStringLiteral("kind,")));

    for (int i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i].split(QLatin1Char(',')).size(), 18) << lines[i].toStdString();
    }
    EXPECT_TRUE(lines[1].startsWith(QStringLiteral("clock,")));
    EXPECT_TRUE(lines[2].startsWith(QStringLiteral("command,")));
    EXPECT_TRUE(lines[2].contains(QStringLiteral(",42,")));
    EXPECT_TRUE(lines[4].startsWith(QStringLiteral("clockjump,")));
    EXPECT_TRUE(lines[4].endsWith(QStringLiteral("25000,2")));
    EXPECT_TRUE(lines[3].endsWith(QStringLiteral(",1,0,0")));
}

TEST(CollabDiagnosticsTest, ReopenAppendsWithoutDuplicateHeader) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("diagnostics.csv"));

    CollabClockSample sample;
    {
        CollabDiagnostics first;
        ASSERT_TRUE(first.open(path));
        first.logClockSample(1, 2, sample, 0, 0, 0.0);
    }
    {
        CollabDiagnostics second;
        ASSERT_TRUE(second.open(path));
        second.logClockSample(3, 4, sample, 0, 0, 0.0);
    }

    const QStringList lines = readLines(path);
    ASSERT_EQ(lines.size(), 3);
    EXPECT_EQ(lines[0].count(QStringLiteral("kind,")), 1);
    EXPECT_TRUE(lines[1].startsWith(QStringLiteral("clock,")));
    EXPECT_TRUE(lines[2].startsWith(QStringLiteral("clock,")));
}

TEST(CollabDiagnosticsTest, EmptyPathIsRejected) {
    CollabDiagnostics diagnostics;
    QString error;
    EXPECT_FALSE(diagnostics.open(QString(), &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(diagnostics.isOpen());
}

} // namespace collab
} // namespace mixxx
