#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "bootstrap/single_instance.h"

using rats::bootstrap::SingleInstanceGuard;

/**
 * Pins the double-launch guard: one live instance per data directory, a second
 * launch is refused and instead pings the running one, and the claim is released
 * when the owner goes away.
 */
class TestSingleInstance : public QObject {
    Q_OBJECT

private slots:
    void secondInstanceIsRefused();
    void lockIsReleasedOnDestruction();
    void separateDataDirectoriesAreIndependent();
    void secondLaunchNotifiesTheRunningInstance();
};

void TestSingleInstance::secondInstanceIsRefused()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SingleInstanceGuard primary(dir.path());
    QVERIFY(primary.tryAcquire());
    QVERIFY(primary.isPrimary());

    SingleInstanceGuard secondary(dir.path());
    QVERIFY(!secondary.tryAcquire());
    QVERIFY(!secondary.isPrimary());
    // The refusal must name the holder, so the user learns what is running.
    QVERIFY(secondary.runningInstanceInfo().contains(QString::number(QCoreApplication::applicationPid())));
}

void TestSingleInstance::lockIsReleasedOnDestruction()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    {
        SingleInstanceGuard primary(dir.path());
        QVERIFY(primary.tryAcquire());
    }

    SingleInstanceGuard relaunch(dir.path());
    QVERIFY(relaunch.tryAcquire());
}

void TestSingleInstance::separateDataDirectoriesAreIndependent()
{
    QTemporaryDir first;
    QTemporaryDir second;
    QVERIFY(first.isValid() && second.isValid());

    SingleInstanceGuard a(first.path());
    SingleInstanceGuard b(second.path());
    QVERIFY(a.tryAcquire());
    QVERIFY(b.tryAcquire());
}

void TestSingleInstance::secondLaunchNotifiesTheRunningInstance()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SingleInstanceGuard primary(dir.path());
    QVERIFY(primary.tryAcquire());
    QSignalSpy spy(&primary, &SingleInstanceGuard::secondInstanceStarted);

    SingleInstanceGuard secondary(dir.path());
    QVERIFY(!secondary.tryAcquire());
    QVERIFY(secondary.notifyRunningInstance(2000));

    QVERIFY(spy.wait(2000));
    QCOMPARE(spy.count(), 1);
}

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "test_single_instance.moc"
