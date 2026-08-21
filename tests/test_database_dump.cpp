/**
 * @file test_database_dump.cpp
 * @brief Unit tests for the .ratsdb whole-database dump format
 *        (rats::service::DumpWriter / DumpReader). The format is what two users
 *        exchange to replicate an index, so the round trip, the truncation
 *        behaviour and the resume offsets are all contracts, not details.
 */

#include <QtTest/QtTest>

#include "domain/torrent.h"
#include "services/database_dump.h"

#include <QFile>
#include <QTemporaryDir>

using namespace rats::domain;
using namespace rats::service;

namespace {

Torrent makeTorrent(int index)
{
    Torrent t;
    t.hash = QStringLiteral("%1").arg(index, 40, 16, QLatin1Char('0'));
    t.name = QStringLiteral("Torrent number %1").arg(index);
    t.size = 1024LL * 1024LL * (index + 1);
    t.files = 2;
    t.pieceLength = 262144;
    t.added = QDateTime::fromSecsSinceEpoch(1700000000 + index);
    t.contentType = ContentType::Video;
    t.contentCategory = ContentCategory::Movie;
    t.seeders = index;
    t.leechers = index * 2;
    t.good = index % 5;
    t.bad = index % 3;
    t.fileList = { File { QStringLiteral("dir/file%1.mkv").arg(index), t.size - 100 },
        File { QStringLiteral("dir/readme%1.txt").arg(index), 100 } };
    return t;
}

dump::Header makeHeader(qint64 torrents)
{
    dump::Header header;
    header.client = QStringLiteral("2.2.3");
    header.peerId = QStringLiteral("abcdef");
    header.created = QDateTime::fromSecsSinceEpoch(1700000000);
    header.torrents = torrents;
    return header;
}

} // namespace

class TestDatabaseDump : public QObject {
    Q_OBJECT

private slots:
    void testRoundTrip();
    void testHeaderAndFooter();
    void testMultipleFrames();
    void testUnfinishedWriteLeavesNoFile();
    void testTruncatedDumpStillReadable();
    void testRejectsForeignFile();
    void testResumeFromOffset();

private:
    QTemporaryDir dir_;
    QString path(const QString& name) const { return dir_.filePath(name); }
};

void TestDatabaseDump::testRoundTrip()
{
    const QString file = path(QStringLiteral("roundtrip.ratsdb"));

    DumpWriter writer;
    QVERIFY(writer.open(file, makeHeader(3)));
    for (int i = 0; i < 3; ++i)
        QVERIFY(writer.write(makeTorrent(i)));
    QVERIFY(writer.finish());
    QCOMPARE(writer.torrentsWritten(), qint64(3));

    DumpReader reader;
    QVERIFY(reader.open(file));

    QVector<Torrent> batch;
    QVERIFY(reader.readBatch(batch));
    QCOMPARE(batch.size(), 3);

    for (int i = 0; i < 3; ++i) {
        const Torrent expected = makeTorrent(i);
        QCOMPARE(batch[i].hash, expected.hash);
        QCOMPARE(batch[i].name, expected.name);
        QCOMPARE(batch[i].size, expected.size);
        QCOMPARE(batch[i].files, expected.files);
        QVERIFY(batch[i].contentType == expected.contentType);
        QVERIFY(batch[i].contentCategory == expected.contentCategory);
        QCOMPARE(batch[i].good, expected.good);
        QCOMPARE(batch[i].bad, expected.bad);
        // The file list travels with the torrent — without it an imported row
        // would be searchable by name only.
        QCOMPARE(batch[i].fileList.size(), 2);
        QCOMPARE(batch[i].fileList[0].path, expected.fileList[0].path);
        QCOMPARE(batch[i].fileList[0].size, expected.fileList[0].size);
    }

    // One frame held everything, so the next read reports the end.
    QVERIFY(!reader.readBatch(batch));
    QVERIFY(reader.atEnd());
    QVERIFY(reader.complete());
}

void TestDatabaseDump::testHeaderAndFooter()
{
    const QString file = path(QStringLiteral("header.ratsdb"));

    DumpWriter writer;
    QVERIFY(writer.open(file, makeHeader(2)));
    QVERIFY(writer.write(makeTorrent(0)));
    QVERIFY(writer.write(makeTorrent(1)));
    QVERIFY(writer.finish());

    DumpReader reader;
    QVERIFY(reader.open(file));
    QCOMPARE(reader.header().version, quint32(dump::kFormatVersion));
    QCOMPARE(reader.header().client, QStringLiteral("2.2.3"));
    QCOMPARE(reader.header().torrents, qint64(2));

    QVector<Torrent> batch;
    reader.readBatch(batch);
    QVERIFY(!reader.readBatch(batch));

    // The footer carries the exact totals, which is how an importer knows the
    // dump was written to the end rather than cut short.
    QVERIFY(reader.complete());
    QCOMPARE(reader.footer().torrents, qint64(2));
    QCOMPARE(reader.footer().files, qint64(4));
    QCOMPARE(reader.footer().totalSize, makeTorrent(0).size + makeTorrent(1).size);
}

void TestDatabaseDump::testMultipleFrames()
{
    const QString file = path(QStringLiteral("frames.ratsdb"));
    const int count = dump::kFrameTorrents + 7; // forces a second frame

    DumpWriter writer;
    QVERIFY(writer.open(file, makeHeader(count)));
    for (int i = 0; i < count; ++i)
        QVERIFY(writer.write(makeTorrent(i)));
    QVERIFY(writer.finish());

    DumpReader reader;
    QVERIFY(reader.open(file));

    int total = 0;
    int frames = 0;
    QVector<Torrent> batch;
    while (reader.readBatch(batch)) {
        total += batch.size();
        ++frames;
    }
    QCOMPARE(total, count);
    QCOMPARE(frames, 2);
    QVERIFY(reader.complete());
    QCOMPARE(reader.footer().torrents, static_cast<qint64>(count));
}

void TestDatabaseDump::testUnfinishedWriteLeavesNoFile()
{
    const QString file = path(QStringLiteral("aborted.ratsdb"));
    {
        DumpWriter writer;
        QVERIFY(writer.open(file, makeHeader(1)));
        QVERIFY(writer.write(makeTorrent(0)));
        // Destroyed without finish(): a file with no footer would look complete
        // enough to import, so the writer removes it.
    }
    QVERIFY(!QFile::exists(file));
}

void TestDatabaseDump::testTruncatedDumpStillReadable()
{
    const QString file = path(QStringLiteral("truncated.ratsdb"));
    const int count = dump::kFrameTorrents + 5;

    DumpWriter writer;
    QVERIFY(writer.open(file, makeHeader(count)));
    for (int i = 0; i < count; ++i)
        QVERIFY(writer.write(makeTorrent(i)));
    QVERIFY(writer.finish());

    // Where the first frame ends — the point an interrupted transfer is most
    // likely to be cut at.
    qint64 afterFirstFrame = 0;
    {
        DumpReader probe;
        QVERIFY(probe.open(file));
        QVector<Torrent> batch;
        QVERIFY(probe.readBatch(batch));
        afterFirstFrame = probe.offset();
    }

    auto truncateTo = [&file](qint64 size) {
        QFile handle(file);
        QVERIFY(handle.open(QIODevice::ReadWrite));
        QVERIFY(handle.resize(size));
        handle.close();
    };

    auto readAll = [&file](bool* complete) {
        DumpReader reader;
        if (!reader.open(file))
            return -1;
        int total = 0;
        QVector<Torrent> batch;
        while (reader.readBatch(batch))
            total += batch.size();
        *complete = reader.complete();
        return total;
    };

    // Lose just the footer: every frame is still there and every torrent in it
    // is still worth importing — the dump is merely not certified complete.
    truncateTo(QFileInfo(file).size() - 40);
    bool complete = true;
    QCOMPARE(readAll(&complete), count);
    QVERIFY(!complete);

    // Cut inside the second frame: that frame is lost, the first one is not.
    truncateTo(afterFirstFrame + 10);
    complete = true;
    QCOMPARE(readAll(&complete), dump::kFrameTorrents);
    QVERIFY(!complete);
}

void TestDatabaseDump::testRejectsForeignFile()
{
    const QString file = path(QStringLiteral("foreign.bin"));
    QFile handle(file);
    QVERIFY(handle.open(QIODevice::WriteOnly));
    handle.write("this is not a rats database at all, not even close");
    handle.close();

    DumpReader reader;
    QString error;
    QVERIFY(!reader.open(file, &error));
    QVERIFY(!error.isEmpty());
}

void TestDatabaseDump::testResumeFromOffset()
{
    const QString file = path(QStringLiteral("resume.ratsdb"));
    const int count = dump::kFrameTorrents * 2;

    DumpWriter writer;
    QVERIFY(writer.open(file, makeHeader(count)));
    for (int i = 0; i < count; ++i)
        QVERIFY(writer.write(makeTorrent(i)));
    QVERIFY(writer.finish());

    // Read one frame, remember where we stopped — this is what an interrupted
    // import persists.
    qint64 offset = 0;
    {
        DumpReader reader;
        QVERIFY(reader.open(file));
        QVector<Torrent> batch;
        QVERIFY(reader.readBatch(batch));
        QCOMPARE(batch.size(), dump::kFrameTorrents);
        offset = reader.offset();
    }
    QVERIFY(offset > 0);

    DumpReader resumed;
    QVERIFY(resumed.open(file));
    QVERIFY(resumed.seekTo(offset));

    int total = 0;
    QVector<Torrent> batch;
    while (resumed.readBatch(batch))
        total += batch.size();

    // Only the remaining frame is re-read: no work is repeated and none is lost.
    QCOMPARE(total, dump::kFrameTorrents);
    QVERIFY(resumed.complete());
}

QTEST_MAIN(TestDatabaseDump)
#include "test_database_dump.moc"
