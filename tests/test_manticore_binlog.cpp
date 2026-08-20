#include "data/manticore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using rats::data::Manticore;

// A binlog searchd cannot replay is fatal and unrepairable, so Manticore::start()
// drops it and retries instead of leaving the app permanently unable to launch.
// These tests pin the two halves of that recovery which are testable without a
// real searchd: which log lines are treated as "binlog is unusable", and which
// files a reset removes.
class TestManticoreBinlog : public QObject {
    Q_OBJECT

private slots:
    void fatalBinlogLine_data();
    void fatalBinlogLine();
    void resetBinlog_removesWholeChainOnly();
    void resetBinlog_withoutBinlogs_reportsNothingDone();
};

void TestManticoreBinlog::fatalBinlogLine_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("isFailure");

    QTest::newRow("missing txn marker") << "FATAL: binlog: log missing txn marker at pos=3094734 (corrupted?)" << true;
    QTest::newRow("replay error") << "FATAL: binlog: replay: failed to open binlog.0005" << true;
    QTest::newRow("replaying progress") << "binlog: replaying log /home/john/.local/share/Rats Search/binlog.0005"
                                        << false;
    QTest::newRow("unrelated fatal") << "FATAL: failed to lock pid file 'searchd.pid'" << false;
    QTest::newRow("precaching") << "precaching table 'torrents'" << false;
    QTest::newRow("deprecation warning") << "WARNING: key 'preopen_indexes' is deprecated in sphinx.conf line 78"
                                         << false;
}

void TestManticoreBinlog::fatalBinlogLine()
{
    QFETCH(QString, line);
    QFETCH(bool, isFailure);

    QCOMPARE(Manticore::isBinlogFailureLine(line), isFailure);
}

void TestManticoreBinlog::resetBinlog_removesWholeChainOnly()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());

    const QStringList binlogs { "binlog.0004", "binlog.0005", "binlog.meta", "binlog.lock" };
    // Everything else in the data directory must survive the reset.
    const QStringList keep { "sphinx.conf", "searchd.log", "rats.json" };

    for (const QString& name : binlogs + keep) {
        QFile file(dataDir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    Manticore manticore(dataDir.path());
    QVERIFY(manticore.resetBinlog());

    for (const QString& name : binlogs) {
        QVERIFY2(!QFile::exists(dataDir.filePath(name)), qPrintable(name));
    }
    for (const QString& name : keep) {
        QVERIFY2(QFile::exists(dataDir.filePath(name)), qPrintable(name));
    }
}

void TestManticoreBinlog::resetBinlog_withoutBinlogs_reportsNothingDone()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());

    Manticore manticore(dataDir.path());
    QVERIFY(!manticore.resetBinlog());
}

QTEST_MAIN(TestManticoreBinlog)
#include "test_manticore_binlog.moc"
