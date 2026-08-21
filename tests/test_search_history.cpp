#include "app/search_history_store.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using rats::app::SearchHistoryStore;

// SearchHistoryStore is the single place recent queries are kept. These tests
// pin the behaviour the front-ends rely on: most-recent-first ordering,
// case-insensitive dedupe, the entry cap, the enabled gate pushed in from
// ConfigStore, and that every mutation persists and notifies.
class TestSearchHistory : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void add_prependsAndNotifies();
    void add_ignoresEmptyAndBlank();
    void add_dedupesCaseInsensitivelyAndBumpsCount();
    void add_capsAtMaxEntries();
    void add_trimsAndTruncates();
    void disabled_doesNotRecordButKeepsExisting();
    void remove_andClear();
    void saveThenLoad_roundTrips();

private:
    QTemporaryDir* dir_ = nullptr;
    SearchHistoryStore* history_ = nullptr;
};

void TestSearchHistory::init()
{
    dir_ = new QTemporaryDir();
    QVERIFY(dir_->isValid());
    history_ = new SearchHistoryStore(dir_->path());
}

void TestSearchHistory::cleanup()
{
    delete history_;
    history_ = nullptr;
    delete dir_;
    dir_ = nullptr;
}

void TestSearchHistory::add_prependsAndNotifies()
{
    QSignalSpy spy(history_, &SearchHistoryStore::historyChanged);

    QVERIFY(history_->add("ubuntu"));
    QVERIFY(history_->add("debian"));

    QCOMPARE(spy.count(), 2);
    QCOMPARE(history_->queries(), QStringList({ "debian", "ubuntu" }));
    QCOMPARE(history_->entries().first().count, 1);
}

void TestSearchHistory::add_ignoresEmptyAndBlank()
{
    QSignalSpy spy(history_, &SearchHistoryStore::historyChanged);

    QVERIFY(!history_->add(QString()));
    QVERIFY(!history_->add("   "));

    QCOMPARE(spy.count(), 0);
    QCOMPARE(history_->size(), 0);
}

void TestSearchHistory::add_dedupesCaseInsensitivelyAndBumpsCount()
{
    history_->add("Ubuntu");
    history_->add("debian");
    history_->add("UBUNTU");

    // One entry, moved back to the front, spelled the way it was last typed.
    QCOMPARE(history_->queries(), QStringList({ "UBUNTU", "debian" }));
    QCOMPARE(history_->entries().first().count, 2);
}

void TestSearchHistory::add_capsAtMaxEntries()
{
    for (int i = 0; i < SearchHistoryStore::kMaxEntries + 25; ++i)
        history_->add(QStringLiteral("query %1").arg(i));

    QCOMPARE(history_->size(), SearchHistoryStore::kMaxEntries);
    // The newest survives, the oldest was dropped.
    QCOMPARE(history_->queries().first(), QStringLiteral("query %1").arg(SearchHistoryStore::kMaxEntries + 24));
    QVERIFY(!history_->queries().contains("query 0"));
}

void TestSearchHistory::add_trimsAndTruncates()
{
    history_->add("  spaced out  ");
    QCOMPARE(history_->queries().first(), QStringLiteral("spaced out"));

    history_->add(QString(SearchHistoryStore::kMaxQueryLength + 100, QLatin1Char('x')));
    QCOMPARE(history_->queries().first().length(), SearchHistoryStore::kMaxQueryLength);
}

void TestSearchHistory::disabled_doesNotRecordButKeepsExisting()
{
    history_->add("ubuntu");
    history_->setEnabled(false);

    QVERIFY(!history_->add("debian"));
    QCOMPARE(history_->queries(), QStringList({ "ubuntu" }));

    history_->setEnabled(true);
    QVERIFY(history_->add("debian"));
    QCOMPARE(history_->queries(), QStringList({ "debian", "ubuntu" }));
}

void TestSearchHistory::remove_andClear()
{
    history_->add("ubuntu");
    history_->add("debian");

    QVERIFY(!history_->remove("fedora"));
    QVERIFY(history_->remove("UBUNTU")); // case-insensitive, like add()
    QCOMPARE(history_->queries(), QStringList({ "debian" }));

    QVERIFY(history_->clear());
    QCOMPARE(history_->size(), 0);
    QVERIFY(!history_->clear()); // already empty: nothing changed
}

void TestSearchHistory::saveThenLoad_roundTrips()
{
    history_->add("ubuntu");
    history_->add("debian");
    history_->add("ubuntu"); // count 2, back to the front

    SearchHistoryStore reloaded(dir_->path());
    QCOMPARE(reloaded.queries(), QStringList({ "ubuntu", "debian" }));
    QCOMPARE(reloaded.entries().first().count, 2);
    QVERIFY(reloaded.entries().first().lastSearchedAt.isValid());
}

QTEST_MAIN(TestSearchHistory)
#include "test_search_history.moc"
