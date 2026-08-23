// Latest-wins background job primitives shared by TelemetryStore pipelines.
//
// TelemetryStore runs every disk / network / parse / unify / scan off the GUI
// thread with QtConcurrent. Each pipeline used to hand-roll a QFutureWatcher,
// a generation counter and an IoCancel shared_ptr; AsyncJob bundles those three
// into one latest-wins unit, and SerialJobQueue chains jobs one in flight.

#pragma once

#include "RemoteCache.h"  // IoCancel

#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

/// QObject base carrying the `running` state and its change signal, so the
/// store can bind loading Q_PROPERTYs to any AsyncJob without templating.
class AsyncJobBase : public QObject {
    Q_OBJECT
public:
    explicit AsyncJobBase(QObject* parent) : QObject(parent) {}
    bool running() const { return running_; }

signals:
    void runningChanged();

protected:
    void setRunning(bool running) {
        if (running_ == running) return;
        running_ = running;
        emit runningChanged();
    }
    bool running_ = false;
};

/// Latest-wins background job. Owns one QFutureWatcher, a generation counter
/// and an IoCancel. `start` cancels and supersedes any run in flight; the
/// latest result is delivered to `onLatest` only when its generation still
/// matches. The destructor cancels and, if a future is still running, waits
/// for it, so destroying the store never leaves a worker writing into freed
/// state.
template <typename T>
class AsyncJob : public AsyncJobBase {
public:
    explicit AsyncJob(QObject* parent)
        : AsyncJobBase(parent), watcher_(new QFutureWatcher<T>(this)) {
        // One connection for the life of the job. Each run captures its own
        // generation; a finished that arrives for a superseded run is filtered
        // by the generation check, and the rare stale finished that races a
        // setFuture is filtered by isRunning().
        QObject::connect(watcher_, &QFutureWatcher<T>::finished, this,
                         [this]() {
                             if (watcher_->isRunning())
                                 return;  // stale: a superseded future
                                          // completed just before setFuture
                             const quint64 gen = watchedGeneration_;
                             if (gen == processedGeneration_)
                                 return;  // already delivered
                             processedGeneration_ = gen;
                             if (gen != generation_)
                                 return;  // superseded: a newer run owns
                                          // the running state
                             // Leave the running state before delivering, so a
                             // re-entrant start() inside the callback (or a
                             // queue pump) sees an idle job.
                             setRunning(false);
                             const T result = watcher_->result();
                             if (auto callback = std::move(onLatest_))
                                 callback(result);
                         });
    }

    ~AsyncJob() override {
        if (cancel_) cancel_->store(true);
        if (watcher_->isRunning()) watcher_->waitForFinished();
        // Superseded runs were detached from the watcher by setFuture() but
        // their workers may still be executing; they were told to cancel when
        // they were superseded, so this wait is normally short.
        for (QFuture<T>& future : superseded_) future.waitForFinished();
    }

    /// Cancel token for the current run; empty before the first start.
    omatrack::IoCancel cancel() const { return cancel_; }

    /// Cancels and supersedes the previous run, then launches `work` on `pool`
    /// (the global pool when null). `onLatest` receives the result of the
    /// latest run only, on the owning thread.
    void start(std::function<T(omatrack::IoCancel)> work,
               std::function<void(T)> onLatest, QThreadPool* pool = nullptr) {
        if (cancel_) cancel_->store(true);
        rememberSuperseded();
        generation_ = nextGeneration();
        watchedGeneration_ = generation_;
        cancel_ = std::make_shared<std::atomic<bool>>(false);
        onLatest_ = std::move(onLatest);
        setRunning(true);
        const omatrack::IoCancel cancel = cancel_;
        QFuture<T> future =
            pool ? QtConcurrent::run(pool, [work = std::move(work),
                                            cancel]() { return work(cancel); })
                 : QtConcurrent::run([work = std::move(work), cancel]() {
                       return work(cancel);
                   });
        watcher_->setFuture(future);
    }

    /// Cancel the current run and discard its result; running() becomes false.
    /// Does not block: the worker is left to observe cancellation on its own.
    void reset() {
        if (cancel_) cancel_->store(true);
        generation_ = nextGeneration();
        setRunning(false);
    }

    /// Blocks until the current run finishes; its result is still delivered
    /// through the event loop. For shutdown paths only.
    void wait() {
        if (watcher_->isRunning()) watcher_->waitForFinished();
        for (QFuture<T>& future : superseded_) future.waitForFinished();
        superseded_.clear();
    }

private:
    /// Keep a handle on a run that start() is about to detach from the
    /// watcher, and drop the ones that have already finished, so the
    /// destructor can wait for every worker this job ever launched.
    void rememberSuperseded() {
        superseded_.erase(
            std::remove_if(superseded_.begin(), superseded_.end(),
                           [](const QFuture<T>& f) { return f.isFinished(); }),
            superseded_.end());
        if (watcher_->isRunning()) superseded_.push_back(watcher_->future());
    }

    static quint64 nextGeneration() {
        static std::atomic<quint64> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    QFutureWatcher<T>* watcher_;
    std::vector<QFuture<T>> superseded_;
    omatrack::IoCancel cancel_;
    std::function<void(T)> onLatest_;
    // Monotonic intent counter, bumped on every start() and reset(). A run is
    // delivered only when its watched generation is still the latest intent.
    quint64 generation_ = 0;
    quint64 watchedGeneration_ = 0;  // generation of the future being watched
    quint64 processedGeneration_ =
        0;  // generation whose result has been delivered
};

/// FIFO queue of jobs with one in flight at a time. `enqueue` appends and
/// pumps; `clear` cancels the in-flight run and drops everything pending.
template <typename T>
class SerialJobQueue {
public:
    using Work = std::function<T(omatrack::IoCancel)>;
    using Done = std::function<void(T)>;

    explicit SerialJobQueue(QObject* parent, QThreadPool* pool = nullptr)
        : job_(parent), pool_(pool) {}

    void enqueue(const QString& key, Work work, Done onDone) {
        queue_.push_back({key, std::move(work), std::move(onDone)});
        pump();
    }

    void clear() {
        queue_.clear();
        runningKey_.clear();
        job_.reset();
    }

    /// Halt pumping new jobs; the in-flight run is left to finish. Used by the
    /// sidebar metadata queue to defer to file-open / lap-load work.
    void pause() { paused_ = true; }
    void resume() {
        paused_ = false;
        pump();
    }

    bool busy() const { return job_.running(); }
    bool empty() const { return queue_.empty(); }
    QString runningKey() const { return runningKey_; }

    /// The underlying job, for binding loading state to runningChanged().
    AsyncJob<T>& inner() { return job_; }

private:
    struct Entry {
        QString key;
        Work work;
        Done onDone;
    };

    void pump() {
        if (paused_ || job_.running() || queue_.empty()) return;
        Entry entry = std::move(queue_.front());
        queue_.pop_front();
        runningKey_ = entry.key;
        job_.start(
            std::move(entry.work),
            [this, onDone = std::move(entry.onDone)](T result) {
                onDone(std::move(result));
                pump();
            },
            pool_);
    }

    std::deque<Entry> queue_;
    AsyncJob<T> job_;
    QThreadPool* pool_;
    QString runningKey_;
    bool paused_ = false;
};
