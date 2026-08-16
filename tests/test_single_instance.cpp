#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <memory>

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

    // In production the two guards live in different processes; here they share a
    // thread, and the ping blocks until the primary's activation server accepts it
    // — which only happens once this thread reaches its event loop. Sending from a
    // worker thread breaks that deadlock (on Windows it is a hard hang: the named
    // pipe write never completes until the server side is accepted).
    bool notified = false;
    std::unique_ptr<QThread> sender(QThread::create([&] { notified = secondary.notifyRunningInstance(2000); }));
    sender->start();

    QVERIFY(spy.wait(2000));
    QVERIFY(sender->wait(2000));
    QVERIFY(notified);
    QCOMPARE(spy.count(), 1);
}

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "test_single_instance.moc"
