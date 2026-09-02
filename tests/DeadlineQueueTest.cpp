#include "app/DeadlineQueue.h"

#include <QTest>

#include <chrono>

class DeadlineQueueTest : public QObject {
    Q_OBJECT
private slots:
    void upsertCoalescesToLatest() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        int value = 0;
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() { value = 1; });
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() { value = 2; });
        QCOMPARE(queue.size(), 1);
        QCOMPARE(queue.pump(), 1);
        QCOMPARE(value, 2);
        QVERIFY(queue.isEmpty());
    }

    void higherPriorityRunsFirstAmongDue() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        QString order;
        queue.upsert(QStringLiteral("low"), DeadlineQueue::Priority::Low,
                     QDeadlineTimer(), [&]() { order += QLatin1Char('L'); });
        queue.upsert(QStringLiteral("high"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() { order += QLatin1Char('H'); });
        queue.upsert(QStringLiteral("normal"), DeadlineQueue::Priority::Normal,
                     QDeadlineTimer(), [&]() { order += QLatin1Char('N'); });
        QCOMPARE(queue.pump(), 3);
        QCOMPARE(order, QStringLiteral("HNL"));
    }

    void overdueKeepsLatestWins() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        int runs = 0;
        int value = 0;
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() {
                         ++runs;
                         value = 1;
                     });
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() {
                         ++runs;
                         value = 2;
                     });
        QCOMPARE(queue.pump(), 1);
        QCOMPARE(runs, 1);
        QCOMPARE(value, 2);
    }

    void emptyPumpIsNoOp() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        QCOMPARE(queue.pump(), 0);
        QVERIFY(queue.isEmpty());
    }

    void futureDeadlineIsNotRun() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        bool ran = false;
        queue.upsert(QStringLiteral("later"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(std::chrono::hours(1)),
                     [&]() { ran = true; });
        QCOMPARE(queue.pump(), 0);
        QVERIFY(!ran);
        QCOMPARE(queue.size(), 1);
    }

    void coalesceKeepsSoonerDeadline() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        int value = 0;
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() { value = 1; });
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(std::chrono::hours(1)),
                     [&]() { value = 2; });
        QCOMPARE(queue.pump(), 1);
        QCOMPARE(value, 2);
        QVERIFY(queue.isEmpty());
    }

    void containsReportsPendingKey() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        QVERIFY(!queue.contains(QStringLiteral("cursor")));
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(std::chrono::hours(1)), []() {});
        QVERIFY(queue.contains(QStringLiteral("cursor")));
        QVERIFY(!queue.contains(QStringLiteral("hud")));
        QCOMPARE(queue.size(), 1);
    }

    void skipUpsertWhenKeyAlreadyPending() {
        DeadlineQueue queue;
        queue.setAutoPump(false);
        int runs = 0;
        int value = 0;
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(std::chrono::hours(1)), [&]() {
                         ++runs;
                         value = 1;
                     });
        if (!queue.contains(QStringLiteral("cursor")))
            queue.upsert(QStringLiteral("cursor"),
                         DeadlineQueue::Priority::High,
                         QDeadlineTimer(std::chrono::hours(1)), [&]() {
                             ++runs;
                             value = 2;
                         });
        QCOMPARE(queue.size(), 1);
        QVERIFY(queue.contains(QStringLiteral("cursor")));
        QCOMPARE(queue.pump(), 0);
        QCOMPARE(runs, 0);
        QCOMPARE(value, 0);
        queue.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                     QDeadlineTimer(), [&]() {
                         ++runs;
                         value = 3;
                     });
        QCOMPARE(queue.pump(), 1);
        QCOMPARE(runs, 1);
        QCOMPARE(value, 3);
        QVERIFY(!queue.contains(QStringLiteral("cursor")));
    }
};

QTEST_GUILESS_MAIN(DeadlineQueueTest)
#include "DeadlineQueueTest.moc"
