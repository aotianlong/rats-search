#include "data/manticore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using rats::data::Manticore;

// Manticore's data directory needs housekeeping that searchd does not do itself:
// a binlog it cannot replay is fatal and unrepairable, so Manticore::start()
// drops it and retries instead of leaving the app permanently unable to launch,
// and its logs grow forever unless pruned. These tests pin the parts of that
// housekeeping which are testable without a real searchd.
class TestManticoreMaintenance : public QObject {
    Q_OBJECT

private slots:
    void fatalBinlogLine_data();
    void fatalBinlogLine();
    void resetBinlog_removesWholeChainOnly();
    void resetBinlog_withoutBinlogs_reportsNothingDone();
    void pruneLogs_removesQueryLog();
    void pruneLogs_rotatesOversizedSearchdLog();
    void pruneLogs_keepsSmallSearchdLog();
};

void TestManticoreMaintenance::fatalBinlogLine_data()
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

void TestManticoreMaintenance::fatalBinlogLine()
{
    QFETCH(QString, line);
    QFETCH(bool, isFailure);

    QCOMPARE(Manticore::isBinlogFailureLine(line), isFailure);
}

void TestManticoreMaintenance::resetBinlog_removesWholeChainOnly()
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

void TestManticoreMaintenance::resetBinlog_withoutBinlogs_reportsNothingDone()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());

    Manticore manticore(dataDir.path());
    QVERIFY(!manticore.resetBinlog());
}

namespace {

// Write `size` bytes into <dir>/<name>.
void writeFile(const QString& path, qint64 size)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(static_cast<int>(size), 'x')), size);
}

} // namespace

void TestManticoreMaintenance::pruneLogs_removesQueryLog()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    writeFile(dataDir.filePath("query.log"), 4096);

    Manticore manticore(dataDir.path());
    manticore.pruneLogs();

    QVERIFY(!QFile::exists(dataDir.filePath("query.log")));
}

void TestManticoreMaintenance::pruneLogs_rotatesOversizedSearchdLog()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    writeFile(dataDir.filePath("searchd.log"), qint64(17) * 1024 * 1024); // over the 16 MB cap
    // A previous generation must be replaced, not accumulate.
    writeFile(dataDir.filePath("searchd.log.1"), 8);

    Manticore manticore(dataDir.path());
    manticore.pruneLogs();

    QVERIFY(!QFile::exists(dataDir.filePath("searchd.log")));
    QCOMPARE(QFileInfo(dataDir.filePath("searchd.log.1")).size(), qint64(17) * 1024 * 1024);
}

void TestManticoreMaintenance::pruneLogs_keepsSmallSearchdLog()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    writeFile(dataDir.filePath("searchd.log"), 1024);

    Manticore manticore(dataDir.path());
    manticore.pruneLogs();

    QCOMPARE(QFileInfo(dataDir.filePath("searchd.log")).size(), qint64(1024));
    QVERIFY(!QFile::exists(dataDir.filePath("searchd.log.1")));
}

QTEST_MAIN(TestManticoreMaintenance)
#include "test_manticore_maintenance.moc"
