/**
 * @file test_row_id.cpp
 * @brief Unit tests for the infohash -> Manticore row id mapping.
 *
 * The mapping is load-bearing: it is what turns every lookup Rats performs from
 * a full table scan into a docid lookup, and it is what makes a row id mean the
 * same thing on every peer. These tests pin the properties the rest of the data
 * layer relies on.
 */

#include <QtTest/QtTest>

#include "data/row_id.h"

using rats::data::rowIdFromHash;

class TestRowId : public QObject {
    Q_OBJECT

private slots:
    void testDerivesFromLeadingBits();
    void testIgnoresCase();
    void testNeverNegative();
    void testNeverZeroForValidHash();
    void testRejectsUnusableInput();
    void testStableAcrossCalls();
    void testDistinctHashesGiveDistinctIds();
    void testDropsTheLowestBit();
    void testIgnoresTrailingBits();
};

void TestRowId::testDerivesFromLeadingBits()
{
    // The id is the first 16 hex digits (64 bits) shifted right by one, so the
    // value stays inside qint64's positive range.
    const QString hash = QStringLiteral("77a41c1ad550f90320f7b7b61ec516c638fed27d");
    QCOMPARE(rowIdFromHash(hash), static_cast<qint64>(0x77a41c1ad550f903ULL >> 1));
}

void TestRowId::testIgnoresCase()
{
    // Hashes reach Rats from the DHT, from peers and from dumps, and those do not
    // agree on case. The same torrent must land on the same row either way.
    const QString lower = QStringLiteral("abcdef0123456789abcdef0123456789abcdef01");
    QCOMPARE(rowIdFromHash(lower.toUpper()), rowIdFromHash(lower));
}

void TestRowId::testNeverNegative()
{
    // A hash with the top bit set is the case the >> 1 exists for: without it the
    // value would come back negative and Manticore would reject it.
    const QString topBitSet = QStringLiteral("ffffffffffffffff") + QString(24, QLatin1Char('a'));
    const qint64 id = rowIdFromHash(topBitSet);
    QVERIFY(id > 0);
    QCOMPARE(id, static_cast<qint64>(0x7fffffffffffffffLL));
}

void TestRowId::testNeverZeroForValidHash()
{
    // Manticore reads id 0 as "assign one for me", so it can never be produced.
    // The only input that would is all-zero leading bits.
    const QString allZeroPrefix = QString(16, QLatin1Char('0')) + QString(24, QLatin1Char('b'));
    QCOMPARE(rowIdFromHash(allZeroPrefix), static_cast<qint64>(1));
    // 0x1 >> 1 is also 0 and must be nudged the same way.
    const QString one = QString(15, QLatin1Char('0')) + QStringLiteral("1") + QString(24, QLatin1Char('b'));
    QCOMPARE(rowIdFromHash(one), static_cast<qint64>(1));
}

void TestRowId::testRejectsUnusableInput()
{
    // 0 is the "not a usable hash" signal; callers test for it.
    QCOMPARE(rowIdFromHash(QString()), static_cast<qint64>(0));
    QCOMPARE(rowIdFromHash(QStringLiteral("abc")), static_cast<qint64>(0)); // too short
    QCOMPARE(rowIdFromHash(QStringLiteral("zzzzzzzzzzzzzzzz")), static_cast<qint64>(0)); // not hex
    QCOMPARE(rowIdFromHash(QStringLiteral("0123456789abcde")), static_cast<qint64>(0)); // 15 digits
}

void TestRowId::testStableAcrossCalls()
{
    const QString hash = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    QCOMPARE(rowIdFromHash(hash), rowIdFromHash(hash));
}

void TestRowId::testDistinctHashesGiveDistinctIds()
{
    // Not a collision-rate claim, just the basic property: hashes differing in
    // their leading bits must not share a row.
    QSet<qint64> ids;
    for (int i = 0; i < 512; ++i) {
        const QString hash = QStringLiteral("%1").arg(i, 8, 16, QChar('0')) + QString(32, QLatin1Char('c'));
        ids.insert(rowIdFromHash(hash));
    }
    QCOMPARE(ids.size(), 512);
}

void TestRowId::testDropsTheLowestBit()
{
    // 63 bits, not 64: the last bit of the first 16 hex digits is shifted away,
    // so two hashes differing only there share a row. That is the deliberate
    // price of keeping the id positive in a qint64, and it is why the repository
    // verifies the stored hash instead of trusting a row it found by id.
    const QString even = QStringLiteral("0123456789abcde0") + QString(24, QLatin1Char('a'));
    const QString odd = QStringLiteral("0123456789abcde1") + QString(24, QLatin1Char('a'));
    QCOMPARE(rowIdFromHash(even), rowIdFromHash(odd));
}

void TestRowId::testIgnoresTrailingBits()
{
    // Only the first 64 bits participate. This is the source of the collision
    // risk the repository guards against, so state it explicitly rather than
    // leaving it implied.
    const QString prefix = QStringLiteral("0123456789abcdef");
    QCOMPARE(
        rowIdFromHash(prefix + QString(24, QLatin1Char('0'))), rowIdFromHash(prefix + QString(24, QLatin1Char('f'))));
}

QTEST_MAIN(TestRowId)
#include "test_row_id.moc"
