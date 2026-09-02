#include "DeadlineQueue.h"

#include <algorithm>
#include <vector>

DeadlineQueue::DeadlineQueue(QObject* parent) : QObject(parent) {
    timer_.setSingleShot(true);
    QObject::connect(&timer_, &QTimer::timeout, this, &DeadlineQueue::pump);
}

void DeadlineQueue::setAutoPump(bool enabled) {
    autoPump_ = enabled;
    if (!autoPump_) {
        timer_.stop();
        pumpScheduled_ = false;
        return;
    }
    schedule();
}

bool DeadlineQueue::sooner(const QDeadlineTimer& a, const QDeadlineTimer& b) {
    if (a.hasExpired() && !b.hasExpired()) return true;
    if (!a.hasExpired() && b.hasExpired()) return false;
    if (a.isForever()) return false;
    if (b.isForever()) return true;
    return a.remainingTimeNSecs() < b.remainingTimeNSecs();
}

void DeadlineQueue::upsert(const QString& key, Priority priority,
                           QDeadlineTimer deadline, Job job) {
    if (key.isEmpty() || !job) return;
    auto it = items_.find(key);
    if (it != items_.end()) {
        Item& item = it.value();
        item.job = std::move(job);
        if (int(priority) > int(item.priority)) item.priority = priority;
        if (sooner(deadline, item.deadline)) item.deadline = deadline;
    } else {
        Item item;
        item.key = key;
        item.priority = priority;
        item.deadline = deadline;
        item.job = std::move(job);
        item.serial = ++serial_;
        items_.insert(key, std::move(item));
    }
    if (pumping_ && deadline.hasExpired()) {
        pumpAgain_ = true;
        return;
    }
    if (autoPump_ && !pumping_) schedule();
}

int DeadlineQueue::pump() {
    pumpScheduled_ = false;
    if (items_.isEmpty()) return 0;
    if (pumping_) {
        pumpAgain_ = true;
        return 0;
    }
    pumping_ = true;
    int ran = 0;
    do {
        pumpAgain_ = false;
        std::vector<Item> due;
        due.reserve(size_t(items_.size()));
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->deadline.hasExpired()) {
                due.push_back(std::move(it.value()));
                it = items_.erase(it);
            } else {
                ++it;
            }
        }
        std::sort(due.begin(), due.end(), [](const Item& a, const Item& b) {
            if (a.priority != b.priority)
                return int(a.priority) > int(b.priority);
            return a.serial < b.serial;
        });
        for (Item& item : due) {
            if (item.job) item.job();
            ++ran;
        }
    } while (pumpAgain_);
    pumping_ = false;
    if (autoPump_) schedule();
    return ran;
}

void DeadlineQueue::schedule() {
    if (!autoPump_ || pumping_ || items_.isEmpty()) {
        timer_.stop();
        return;
    }
    bool anyDue = false;
    qint64 minRemainNs = -1;
    for (const Item& item : items_) {
        if (item.deadline.hasExpired()) {
            anyDue = true;
            break;
        }
        if (item.deadline.isForever()) continue;
        const qint64 ns = item.deadline.remainingTimeNSecs();
        if (ns <= 0) {
            anyDue = true;
            break;
        }
        if (minRemainNs < 0 || ns < minRemainNs) minRemainNs = ns;
    }
    if (anyDue) {
        if (!pumpScheduled_) {
            pumpScheduled_ = true;
            QTimer::singleShot(0, this, [this]() {
                pumpScheduled_ = false;
                pump();
            });
        }
        return;
    }
    if (minRemainNs < 0) return;
    const int ms = int(std::max<qint64>(1, (minRemainNs + 999999) / 1000000));
    timer_.start(ms);
}
