/**
 * @file test_manticore_queries.cpp
 * @brief Integration test that starts a real Manticore instance and drives the
 *        new data layer (rats::data::Manticore + Database + TorrentRepository).
 *
 * Verifies the end-to-end CRUD / search / statistics path against a live
 * searchd, replacing the old raw-SphinxQL query test. Requires the bundled
 * searchd executable to be discoverable via the standard import paths.
 */

#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include "data/database.h"
#include "data/manticore.h"
#include "data/query.h"
#include "data/row_id.h"
#include "data/torrent_repository.h"
#include "domain/content.h"
#include "domain/torrent.h"

using rats::data::Database;
using rats::data::Manticore;
using rats::data::SelectQuery;
using rats::data::TorrentRepository;
using rats::domain::ContentCategory;
using rats::domain::ContentType;
using rats::domain::File;
using rats::domain::Torrent;

class TestManticoreQueries : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testConnected();
    void testAddAndGet();
    void testExists();
    void testGetWithFiles();
    void testSearchByName();
    void testSearchByHash();
    void testUpdateTrackerCounts();
    void testStatistics();
    void testRecent();
    void testTop();
    void testRemove();
    void testSelectQueryAgainstLiveIndex();
    void testPageAfterIdWalksPastMaxMatches();
    void testRowIdIsDerivedFromHash();
    void testFilesRowSharesTheTorrentId();
    void testReAddDoesNotDuplicateRow();
    void testRowIdCollisionIsRefused();
    void testMigrationReKeysLegacyRows();
    void testUpdateAppliesAndKeepsSearchable();

private:
    // Build a valid torrent with a distinct 40-hex hash derived from `n`.
    Torrent makeTorrent(
        int n, const QString& name, ContentType type = ContentType::Video, qint64 size = 1024 * 1024, int seeders = 10);
    // Poll get() until the RT row is visible (RT attributes settle asynchronously).
    bool waitForTorrent(const QString& hash, int maxRetries = 50, int delayMs = 20);
    // Same, for the files row. The two tables settle independently, so a
    // torrent being visible says nothing about its file list yet.
    bool waitForFiles(const QString& hash, int expected, int maxRetries = 50, int delayMs = 20);

    QTemporaryDir* tempDir_ = nullptr;
    Manticore* manticore_ = nullptr;
    Database* db_ = nullptr;
    TorrentRepository* repo_ = nullptr;
};

Torrent TestManticoreQueries::makeTorrent(int n, const QString& name, ContentType type, qint64 size, int seeders)
{
    Torrent t;
    // 40-char hex hash: an 8-hex prefix from n, padded with 'a'.
    t.hash = QString("%1").arg(n, 8, 16, QChar('0')) + QString(32, 'a');
    t.name = name;
    t.size = size;
    t.files = 1;
    t.pieceLength = 262144;
    t.added = QDateTime::currentDateTime();
    t.ipv4 = "192.168.1.1";
    t.port = 6881;
    t.contentType = type;
    t.contentCategory = ContentCategory::Unknown;
    t.seeders = seeders;
    t.leechers = 5;
    t.completed = 100;
    return t;
}

bool TestManticoreQueries::waitForTorrent(const QString& hash, int maxRetries, int delayMs)
{
    for (int i = 0; i < maxRetries; ++i) {
        if (repo_->exists(hash))
            return true;
        QThread::msleep(delayMs);
    }
    return repo_->exists(hash);
}

bool TestManticoreQueries::waitForFiles(const QString& hash, int expected, int maxRetries, int delayMs)
{
    // exists() only asks the torrents table. A file list is a row in `files`,
    // written by a separate statement and made visible by the RT index on its own
    // schedule, so waiting for the torrent says nothing about its files yet.
    for (int i = 0; i < maxRetries; ++i) {
        if (repo_->filesOf(hash).size() == expected)
            return true;
        QThread::msleep(delayMs);
    }
    return repo_->filesOf(hash).size() == expected;
}

void TestManticoreQueries::initTestCase()
{
    tempDir_ = new QTemporaryDir();
    QVERIFY2(tempDir_->isValid(), "Failed to create temporary directory");
    qInfo() << "Test data directory:" << tempDir_->path();

    manticore_ = new Manticore(tempDir_->path());
    const bool started = manticore_->start();
    QVERIFY2(started, "Failed to start Manticore Search. Is searchd available?");
    QVERIFY(manticore_->isRunning());
    qInfo() << "Manticore started on port" << manticore_->port();

    db_ = new Database(manticore_);
    QVERIFY(db_->isConnected());

    repo_ = new TorrentRepository(db_);
    repo_->primeFromDatabase();
}

void TestManticoreQueries::cleanupTestCase()
{
    delete repo_;
    repo_ = nullptr;
    delete db_;
    db_ = nullptr;
    if (manticore_) {
        manticore_->stop();
        delete manticore_;
        manticore_ = nullptr;
    }
    delete tempDir_;
    tempDir_ = nullptr;
}

void TestManticoreQueries::testConnected()
{
    QVERIFY(db_->isConnected());
    QVERIFY(manticore_->isRunning());
}

void TestManticoreQueries::testAddAndGet()
{
    const Torrent t = makeTorrent(1, "Ubuntu Linux ISO", ContentType::Archive, 4LL * 1024 * 1024 * 1024, 42);
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->hash, t.hash);
    QCOMPARE(got->name, QString("Ubuntu Linux ISO"));
    QCOMPARE(got->size, (qint64)(4LL * 1024 * 1024 * 1024));
    QCOMPARE(got->seeders, 42);
    QCOMPARE(rats::domain::toId(got->contentType), rats::domain::toId(ContentType::Archive));
}

void TestManticoreQueries::testExists()
{
    QVERIFY(repo_->exists(makeTorrent(1, "x").hash)); // added in testAddAndGet
    QVERIFY(!repo_->exists(QString(40, 'f'))); // never inserted
}

void TestManticoreQueries::testGetWithFiles()
{
    Torrent t = makeTorrent(2, "Multi File Pack", ContentType::Video, 2000, 5);
    t.files = 2;
    t.fileList.append(File { "folder/movie.mkv", 1900 });
    t.fileList.append(File { "folder/readme.txt", 100 });
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));
    QVERIFY(waitForFiles(t.hash, 2));

    const auto got = repo_->get(t.hash, /*includeFiles=*/true);
    QVERIFY(got.has_value());
    QCOMPARE(got->fileList.size(), 2);
    QCOMPARE(got->fileList[0].path, QString("folder/movie.mkv"));
    QCOMPARE(got->fileList[0].size, (qint64)1900);
}

void TestManticoreQueries::testSearchByName()
{
    QVERIFY(repo_->add(makeTorrent(3, "Debian Server Edition", ContentType::Archive)));
    QVERIFY(waitForTorrent(makeTorrent(3, "x").hash));

    TorrentRepository::SearchQuery q;
    q.text = "Debian";
    q.limit = 10;
    const auto hits = repo_->searchTorrents(q);
    bool found = false;
    for (const auto& h : hits) {
        if (h.torrent.name.contains("Debian"))
            found = true;
    }
    QVERIFY2(found, "Full-text search should find the 'Debian' torrent");
}

void TestManticoreQueries::testSearchByHash()
{
    const Torrent t = makeTorrent(1, "x"); // Ubuntu torrent's hash
    TorrentRepository::SearchQuery q;
    q.text = t.hash; // a 40-hex string is treated as an exact hash lookup
    const auto hits = repo_->searchTorrents(q);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().torrent.hash, t.hash);
}

void TestManticoreQueries::testUpdateTrackerCounts()
{
    const Torrent t = makeTorrent(1, "x");
    QVERIFY(repo_->updateTrackerCounts(t.hash, 111, 22, 3));

    const auto got = repo_->get(t.hash);
    QVERIFY(got.has_value());
    QCOMPARE(got->seeders, 111);
    QCOMPARE(got->leechers, 22);
    QCOMPARE(got->completed, 3);
}

void TestManticoreQueries::testStatistics()
{
    const auto stats = repo_->statistics();
    // At least the three torrents added above.
    QVERIFY2(stats.torrents >= 3, qPrintable(QString("torrents=%1").arg(stats.torrents)));
    QVERIFY(stats.totalSize > 0);
}

void TestManticoreQueries::testRecent()
{
    const auto recent = repo_->recent(10);
    QVERIFY(!recent.isEmpty());
}

void TestManticoreQueries::testTop()
{
    // Ubuntu torrent now has 111 seeders; it should appear in the top list.
    const auto top = repo_->top(QString(), QString(), 0, 10);
    QVERIFY(!top.isEmpty());
    // Ordered by seeders descending.
    for (int i = 1; i < top.size(); ++i)
        QVERIFY(top[i - 1].seeders >= top[i].seeders);
}

void TestManticoreQueries::testRemove()
{
    const Torrent t = makeTorrent(3, "x"); // Debian torrent
    QVERIFY(repo_->exists(t.hash));
    QVERIFY(repo_->remove(t.hash));
    // Removal should eventually be visible.
    bool gone = false;
    for (int i = 0; i < 50; ++i) {
        if (!repo_->exists(t.hash)) {
            gone = true;
            break;
        }
        QThread::msleep(20);
    }
    QVERIFY2(gone, "Torrent should be removed");
}

void TestManticoreQueries::testSelectQueryAgainstLiveIndex()
{
    // Exercise the SelectQuery builder end-to-end against the live index.
    const QString sql = SelectQuery("torrents")
                            .columns("id, hash, name")
                            .whereRaw("seeders > 0")
                            .orderBy("seeders", true)
                            .limit(0, 5)
                            .build();
    bool ok = false;
    const auto rows = db_->query(sql, {}, &ok);
    QVERIFY(ok);
    // The Ubuntu torrent (111 seeders) guarantees at least one row.
    QVERIFY(!rows.isEmpty());
}

// A full-index sweep (the `torrent.cleanup` maintenance path) must be able to
// walk past Manticore's max_matches, which rejects any OFFSET >= 1000 with
// "offset out of bounds" — the reason the filter cleanup used to stop after the
// first 1000 torrents.
void TestManticoreQueries::testPageAfterIdWalksPastMaxMatches()
{
    constexpr int kExtra = 1200;
    const int base = 10000;
    for (int i = 0; i < kExtra; ++i)
        QVERIFY(repo_->add(makeTorrent(base + i, QString("Sweep Sample %1").arg(i))));
    QVERIFY(waitForTorrent(makeTorrent(base + kExtra - 1, "x").hash));

    // Plain OFFSET paging is what breaks: prove the limit is real, so this test
    // fails loudly if the sweep ever goes back to offsets.
    bool ok = false;
    db_->query(SelectQuery("torrents").orderBy("id", false).limit(1000, 500).build(), {}, &ok);
    QVERIFY2(!ok, "Manticore should reject an offset of 1000 (max_matches)");

    int seen = 0;
    qint64 afterId = 0;
    qint64 previousId = 0;
    for (;;) {
        const QVector<Torrent> batch = repo_->pageAfterId(afterId, 500);
        if (batch.isEmpty())
            break;
        for (const Torrent& t : batch) {
            QVERIFY(t.id > previousId); // strictly ascending, no repeats
            previousId = t.id;
            ++seen;
        }
        afterId = batch.last().id;
    }
    QVERIFY2(seen >= kExtra, qPrintable(QString("swept %1 of >= %2 torrents").arg(seen).arg(kExtra)));
    QCOMPARE(static_cast<qint64>(seen), repo_->statistics().torrents);
}

// ---------------------------------------------------------------------------
// Row ids derived from the infohash
// ---------------------------------------------------------------------------

void TestManticoreQueries::testRowIdIsDerivedFromHash()
{
    const Torrent t = makeTorrent(770001, "Derived Id Sample");
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    // The row id is not a counter value: it is the hash, and a peer computing it
    // from the same hash must arrive at the same number.
    QCOMPARE(stored->id, rats::data::rowIdFromHash(t.hash));
}

void TestManticoreQueries::testFilesRowSharesTheTorrentId()
{
    Torrent t = makeTorrent(770002, "Shared Id Sample");
    t.fileList = { File { "shared/one.mkv", 111 }, File { "shared/two.mkv", 222 } };
    t.files = t.fileList.size();
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));
    QVERIFY(waitForFiles(t.hash, 2));

    // The files row carrying the torrent's own id is what turns the export join
    // into a docid lookup, so assert the id itself rather than just the contents.
    const qint64 expected = rats::data::rowIdFromHash(t.hash);
    const auto rows = db_->query(QStringLiteral("SELECT id FROM files WHERE id = %1").arg(expected));
    QCOMPARE(rows.size(), 1);

    const QVector<File> files = repo_->filesOf(t.hash);
    QCOMPARE(files.size(), 2);
    QCOMPARE(files.at(0).path, QString("shared/one.mkv"));
}

void TestManticoreQueries::testReAddDoesNotDuplicateRow()
{
    // With a derived id, adding the same torrent twice targets one row, and the
    // second add leaves the stored copy alone. Under the old counter the same
    // sequence produced two rows carrying one hash.
    Torrent t = makeTorrent(770003, "Duplicate Guard");
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    t.name = "Duplicate Guard Reindexed";
    QVERIFY(repo_->add(t));

    const auto rows
        = db_->query(QStringLiteral("SELECT id FROM torrents WHERE id = %1").arg(rats::data::rowIdFromHash(t.hash)));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(repo_->get(t.hash)->name, QString("Duplicate Guard"));
}

void TestManticoreQueries::testRowIdCollisionIsRefused()
{
    // Two hashes that agree on their first 64 bits and differ afterwards map to
    // one row. The second one must be refused, never stored over the first.
    const QString shared = QStringLiteral("beefbeefbeefbeef");
    Torrent first = makeTorrent(1, "Collision First");
    first.hash = shared + QString(24, QLatin1Char('1'));
    Torrent second = makeTorrent(2, "Collision Second");
    second.hash = shared + QString(24, QLatin1Char('2'));
    QCOMPARE(rats::data::rowIdFromHash(first.hash), rats::data::rowIdFromHash(second.hash));

    QVERIFY(repo_->add(first));
    QVERIFY(waitForTorrent(first.hash));

    QVERIFY2(!repo_->add(second), "a torrent whose row id belongs to another must be refused");

    // The original survives untouched, and the impostor is not findable.
    const auto stored = repo_->get(first.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QString("Collision First"));
    QVERIFY(!repo_->get(second.hash).has_value());
    QVERIFY(!repo_->exists(second.hash));

    // Batch lookups must reach the same verdict, and report which hash lost.
    QSet<QString> collided;
    const auto found = repo_->getMany({ first.hash, second.hash }, &collided);
    QVERIFY(found.contains(first.hash));
    QVERIFY(!found.contains(second.hash));
    QVERIFY(collided.contains(second.hash));
}

void TestManticoreQueries::testMigrationReKeysLegacyRows()
{
    // Write rows the way the pre-migration code did: a small sequential id that
    // has nothing to do with the hash. The repository can no longer produce these,
    // so they go in through the raw database layer.
    struct Legacy {
        qint64 id;
        QString hash;
        QString name;
    };
    const QVector<Legacy> legacy {
        { 41, QStringLiteral("11110000") + QString(32, QLatin1Char('d')), QStringLiteral("Legacy One") },
        { 42, QStringLiteral("22220000") + QString(32, QLatin1Char('d')), QStringLiteral("Legacy Two") },
    };

    for (const Legacy& row : legacy) {
        QVERIFY(db_->insert("torrents",
            QVariantMap { { "id", row.id }, { "hash", row.hash }, { "name", row.name }, { "nameIndex", row.name },
                { "size", 4096 }, { "files", 1 }, { "piecelength", 262144 },
                { "added", QDateTime::currentSecsSinceEpoch() }, { "ipv4", "10.0.0.1" }, { "port", 6881 },
                { "contentType", 1 }, { "contentCategory", 0 }, { "seeders", 1 }, { "leechers", 1 }, { "completed", 1 },
                { "trackersChecked", 0 }, { "good", 0 }, { "bad", 0 } }));
        QVERIFY(db_->insert("files",
            QVariantMap { { "id", row.id }, { "hash", row.hash }, { "path", "legacy/file.bin" }, { "size", "4096" } }));
    }

    // A legacy row is invisible to a lookup precisely because its id is wrong —
    // that is what the migration exists to fix.
    QVERIFY(!repo_->exists(legacy.first().hash));

    for (const QString& table : { QStringLiteral("torrents"), QStringLiteral("files") }) {
        qint64 cursor = 0;
        for (;;) {
            const auto page = repo_->migrateRowIdPage(table, cursor, 500);
            // A failed page leaves the cursor where it was, so looping on
            // `finished` alone would spin forever.
            QVERIFY2(!page.failed, qPrintable("migration page failed on " + table));
            if (page.finished)
                break;
        }
    }

    for (const Legacy& row : legacy) {
        QVERIFY2(waitForTorrent(row.hash), qPrintable("not reachable after migration: " + row.name));
        QVERIFY2(waitForFiles(row.hash, 1), qPrintable("files not reachable after migration: " + row.name));
        const auto stored = repo_->get(row.hash, /*includeFiles*/ true);
        QVERIFY(stored.has_value());
        QCOMPARE(stored->name, row.name);
        // Re-keyed, and the full-text column survived the rewrite.
        QCOMPARE(stored->id, rats::data::rowIdFromHash(row.hash));
        QCOMPARE(stored->fileList.size(), 1);
        QCOMPARE(stored->fileList.at(0).path, QString("legacy/file.bin"));
        // The old row is gone rather than left behind as a duplicate.
        QVERIFY(db_->query(QStringLiteral("SELECT id FROM torrents WHERE id = %1").arg(row.id)).isEmpty());
    }

    TorrentRepository::SearchQuery q;
    q.text = "Legacy One";
    q.limit = 10;
    QVERIFY2(!repo_->searchTorrents(q).isEmpty(), "re-keyed row must still be findable by name");

    // Running it again must be a no-op, not a second rewrite.
    qint64 cursor = 0;
    int rewritten = 0;
    for (;;) {
        const auto page = repo_->migrateRowIdPage(QStringLiteral("torrents"), cursor, 500);
        QVERIFY(!page.failed);
        if (page.finished)
            break;
        rewritten += page.rewritten;
    }
    QCOMPARE(rewritten, 0);
}

void TestManticoreQueries::testUpdateAppliesAndKeepsSearchable()
{
    // update() used to be a partial UPDATE naming nameIndex, which Manticore
    // rejects outright as a full-text field -- so nothing was written and vote
    // merges vanished. Pin that it now actually applies.
    Torrent t = makeTorrent(770004, "Updatable Sample zephyr");
    t.good = 1;
    QVERIFY(repo_->add(t));
    QVERIFY(waitForTorrent(t.hash));

    Torrent changed = *repo_->get(t.hash);
    changed.good = 42;
    changed.bad = 7;
    changed.seeders = 999;
    QVERIFY(repo_->update(changed));

    const auto stored = repo_->get(t.hash);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->good, 42);
    QCOMPARE(stored->bad, 7);
    QCOMPARE(stored->seeders, 999);
    // Rewriting the row must not cost it its place in the full-text index, and
    // must not leave a second row behind.
    QCOMPARE(stored->id, rats::data::rowIdFromHash(t.hash));
    TorrentRepository::SearchQuery q;
    q.text = "zephyr";
    q.limit = 10;
    QCOMPARE(repo_->searchTorrents(q).size(), 1);
}

QTEST_MAIN(TestManticoreQueries)
#include "test_manticore_queries.moc"
