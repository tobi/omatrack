// GUI-thread coalescing priority queue with deadlines.
//
// Latest-wins per key: a later upsert replaces the pending job for that key
// and keeps the sooner deadline so a storm cannot push work out forever.
// Ready items run highest-priority first. The pump is driven by QTimer, never
// from MpvVideoItem::processEvents / time-pos / wakeup.

#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class DeadlineQueue : public QObject {
    Q_OBJECT

public:
    enum class Priority : int { Low = 0, Normal = 1, High = 2 };

    explicit DeadlineQueue(QObject* parent = nullptr);

    using Job = std::function<void()>;

    /// Insert or replace the pending item for `key`. The callable is the
    /// latest work; an existing sooner deadline is kept; priority is the
    /// higher of the two.
    void upsert(const QString& key, Priority priority, QDeadlineTimer deadline,
                Job job);

    /// Run every currently due item (highest priority first). Empty is a
    /// no-op. Returns the number of jobs invoked.
    int pump();

    bool isEmpty() const { return items_.isEmpty(); }
    int size() const { return items_.size(); }
    /// True while a job for key is waiting. Callers that would only
    /// replace it with an equivalent latest-spot reader skip allocating.
    bool contains(const QString& key) const { return items_.contains(key); }

    /// Tests call pump() themselves. Production leaves auto-pump on so a
    /// QTimer(0) fires while anything is due, else a single-shot for the
    /// next deadline.
    void setAutoPump(bool enabled);

private:
    struct Item {
        QString key;
        Priority priority = Priority::Normal;
        QDeadlineTimer deadline;
        Job job;
        quint64 serial = 0;
    };

    void schedule();
    static bool sooner(const QDeadlineTimer& a, const QDeadlineTimer& b);

    QHash<QString, Item> items_;
    QTimer timer_;
    quint64 serial_ = 0;
    bool autoPump_ = true;
    bool pumping_ = false;
    bool pumpAgain_ = false;
    bool pumpScheduled_ = false;
};
