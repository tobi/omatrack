#include "RemoteCache.h"

#include "core/TelemetryEngine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFuture>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPromise>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include "VerboseLog.h"

#include <algorithm>
#include <mutex>

namespace omatrack {
namespace {

constexpr int kRequestTimeoutMs = 30'000;
constexpr int kDownloadTimeoutMs = 120'000;
/// Enough for the usual http→https or bucket-region hop, low enough that a
/// redirect loop ends as an error rather than a hang.
constexpr int kMaximumRedirects = 5;

struct DownloadResult {
    int status = 0;
    QString error;
    qint64 bytes = 0;
};

/// The redirect target, or an invalid URL when the response was not one.
QUrl redirectTarget(QNetworkReply* reply, const QUrl& from) {
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 301 && status != 302 && status != 303 && status != 307 &&
        status != 308)
        return {};
    const QByteArray location = reply->rawHeader("Location");
    if (location.isEmpty()) return {};
    const QUrl target = from.resolved(QUrl::fromEncoded(location));
    return target.isValid() ? target : QUrl();
}

QString safeReplyError(const QNetworkReply* reply) {
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status > 0) return QStringLiteral("HTTP %1").arg(status);
    return QStringLiteral("Network request failed (%1)")
        .arg(int(reply->error()));
}

thread_local IoCancel t_ioCancel;

struct IoCancelScope {
    IoCancel previous;
    explicit IoCancelScope(IoCancel cancel) : previous(t_ioCancel) {
        t_ioCancel = std::move(cancel);
    }
    ~IoCancelScope() { t_ioCancel = previous; }
};

IoCancel effectiveCancel(const IoCancel& cancel) {
    return cancel ? cancel : t_ioCancel;
}

HttpResponse cancelledResponse() {
    HttpResponse result;
    result.error = QStringLiteral("Cancelled");
    return result;
}

/// One process-lifetime thread that owns QNetworkAccessManager. QNAM needs an
/// event loop on its thread; that loop is QThread::exec(), not a nested
/// QEventLoop on the GUI or a QtConcurrent worker. Callers wait on a QFuture.
class NetworkIo : public QObject {
public:
    static NetworkIo& instance() {
        // Leaked: a QObject that has lived on a worker thread cannot be
        // destroyed safely after that thread has stopped.
        static NetworkIo* io = []() {
            auto* instance = new NetworkIo;
            instance->ensureStarted();
            return instance;
        }();
        return *io;
    }

    HttpResponse send(const QUrl& url, const QByteArray& method,
                      const RequestFactory& build, const QByteArray& body,
                      const IoCancel& cancel) {
        auto promise = std::make_shared<QPromise<HttpResponse>>();
        QFuture<HttpResponse> future = promise->future();
        promise->start();
        QMetaObject::invokeMethod(
            this,
            [this, url, method, build, body, cancel, promise]() {
                startSend(url, method, build, body, cancel, 0, promise);
            },
            Qt::QueuedConnection);
        return waitFor(std::move(future));
    }

    DownloadResult download(const QUrl& url, const RequestFactory& build,
                            const QString& path,
                            const DownloadProgress& progress,
                            const IoCancel& cancel) {
        auto promise = std::make_shared<QPromise<DownloadResult>>();
        QFuture<DownloadResult> future = promise->future();
        promise->start();
        QMetaObject::invokeMethod(
            this,
            [this, url, build, path, progress, cancel, promise]() {
                auto output = std::make_shared<QSaveFile>(path);
                startDownload(url, build, path, progress, cancel, 0, output,
                              promise);
            },
            Qt::QueuedConnection);
        return waitFor(std::move(future));
    }

    using RangeFactory =
        std::function<QNetworkRequest(const QUrl&, const ObjectRange&)>;

    bool ranges(const RangeFactory& rangeBuild, const QUrl& target,
                const QVector<ObjectRange>& ranges, QVector<QByteArray>* bodies,
                QString* error, const IoCancel& cancel) {
        auto promise = std::make_shared<QPromise<RangeResult>>();
        QFuture<RangeResult> future = promise->future();
        promise->start();
        QMetaObject::invokeMethod(
            this,
            [this, rangeBuild, target, ranges, cancel, promise]() {
                startRanges(rangeBuild, target, ranges, cancel, promise);
            },
            Qt::QueuedConnection);
        const RangeResult result = waitFor(std::move(future));
        if (error) *error = result.error;
        if (!result.ok) {
            if (bodies) bodies->clear();
            return false;
        }
        if (bodies) *bodies = result.bodies;
        return true;
    }

    void drain() {
        const auto noop = []() {};
        QMetaObject::invokeMethod(this, noop, Qt::BlockingQueuedConnection);
    }

private:
    NetworkIo() = default;

    template <typename T>
    static T waitFor(QFuture<T> future) {
        // Blocks the calling thread until the I/O thread finishes. The GUI
        // thread must never reach here: it would freeze the loop, and a mock
        // server on that loop would starve. Callers run the blocking engine
        // on a QtConcurrent worker and observe the result with a
        // QFutureWatcher.
        future.waitForFinished();
        return future.result();
    }

    void ensureStarted() {
        std::call_once(started_, [this]() {
            thread_.setObjectName(QStringLiteral("omatrack-io"));
            moveToThread(&thread_);
            QObject::connect(&thread_, &QThread::started, this, [this]() {
                manager_ = new QNetworkAccessManager(this);
                ready_.store(true, std::memory_order_release);
            });
            thread_.start();
            while (!ready_.load(std::memory_order_acquire))
                QThread::yieldCurrentThread();
            if (QCoreApplication::instance()) {
                QObject::connect(
                    QCoreApplication::instance(),
                    &QCoreApplication::aboutToQuit, this,
                    [this]() {
                        thread_.quit();
                        thread_.wait(3000);
                    },
                    Qt::DirectConnection);
            }
        });
    }

    void watchCancel(QNetworkReply* reply, const IoCancel& cancel) {
        if (!cancel || !reply) return;
        auto* poll = new QTimer(reply);
        poll->setInterval(100);
        QObject::connect(poll, &QTimer::timeout, reply, [reply, cancel]() {
            if (ioCancelled(cancel) && reply->isRunning()) reply->abort();
        });
        poll->start();
    }

    void startSend(const QUrl& url, const QByteArray& method,
                   const RequestFactory& build, const QByteArray& body,
                   const IoCancel& cancel, int hop,
                   const std::shared_ptr<QPromise<HttpResponse>>& promise) {
        if (ioCancelled(cancel)) {
            promise->addResult(cancelledResponse());
            promise->finish();
            return;
        }
        if (hop > kMaximumRedirects) {
            HttpResponse result;
            result.error = QStringLiteral("Too many redirects");
            promise->addResult(result);
            promise->finish();
            return;
        }
        QNetworkReply* reply =
            manager_->sendCustomRequest(build(url), method, body);
        watchCancel(reply, cancel);
        auto* timeout = new QTimer(reply);
        timeout->setSingleShot(true);
        QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
            if (reply->isRunning()) reply->abort();
        });
        timeout->start(kRequestTimeoutMs);
        QObject::connect(
            reply, &QNetworkReply::finished, this,
            [this, reply, url, method, build, body, cancel, hop, promise,
             timeout]() {
                const bool timedOut = !timeout->isActive();
                timeout->stop();
                HttpResponse result;
                result.status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                if (reply->isOpen()) result.body = reply->readAll();
                for (const auto& [name, value] : reply->rawHeaderPairs())
                    result.headers.insert(name.toLower(), value);
                const QUrl next = timedOut || ioCancelled(cancel)
                                      ? QUrl()
                                      : redirectTarget(reply, url);
                if (timedOut)
                    result.error = QStringLiteral("Request timed out");
                else if (ioCancelled(cancel))
                    result.error = QStringLiteral("Cancelled");
                else if (!next.isValid() &&
                         reply->error() != QNetworkReply::NoError)
                    result.error = safeReplyError(reply);
                reply->deleteLater();
                if (next.isValid()) {
                    startSend(next, method, build, body, cancel, hop + 1,
                              promise);
                    return;
                }
                promise->addResult(result);
                promise->finish();
            });
    }

    void startDownload(
        const QUrl& url, const RequestFactory& build, const QString& path,
        const DownloadProgress& progress, const IoCancel& cancel, int hop,
        const std::shared_ptr<QSaveFile>& output,
        const std::shared_ptr<QPromise<DownloadResult>>& promise) {
        if (ioCancelled(cancel)) {
            promise->addResult({0, QStringLiteral("Download cancelled"), 0});
            promise->finish();
            return;
        }
        if (hop > kMaximumRedirects) {
            promise->addResult({0, QStringLiteral("Too many redirects"), 0});
            promise->finish();
            return;
        }
        if (hop == 0) {
            if (!output->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                promise->addResult(
                    {0, QStringLiteral("Unable to open cache file"), 0});
                promise->finish();
                return;
            }
        } else {
            output->seek(0);
            output->resize(0);
        }

        QNetworkReply* reply = manager_->get(build(url));
        watchCancel(reply, cancel);
        auto* timeout = new QTimer(reply);
        timeout->setSingleShot(true);
        QObject::connect(timeout, &QTimer::timeout, reply, [reply]() {
            if (reply->isRunning()) reply->abort();
        });
        timeout->start(kDownloadTimeoutMs);

        struct DownloadState {
            qint64 bytes = 0;
            bool writeFailed = false;
            bool abandoned = false;
        };
        auto state = std::make_shared<DownloadState>();
        QObject::connect(reply, &QNetworkReply::readyRead, this,
                         [reply, timeout, cancel, state, output]() {
                             const QByteArray chunk = reply->readAll();
                             if (output->write(chunk) != chunk.size()) {
                                 state->writeFailed = true;
                                 reply->abort();
                                 return;
                             }
                             state->bytes += chunk.size();
                             timeout->start(kDownloadTimeoutMs);
                             if (ioCancelled(cancel)) {
                                 state->abandoned = true;
                                 reply->abort();
                             }
                         });
        // Progress comes from Qt's own byte counter rather than the readyRead
        // tally: it is the same number, reported on the same thread, without
        // re-reading the Content-Length header on every chunk.
        QObject::connect(
            reply, &QNetworkReply::downloadProgress, this,
            [reply, progress, cancel, state](qint64 received, qint64 total) {
                if (ioCancelled(cancel)) {
                    state->abandoned = true;
                    reply->abort();
                    return;
                }
                if (progress && !progress(received, total)) {
                    state->abandoned = true;
                    reply->abort();
                }
            });
        QObject::connect(
            reply, &QNetworkReply::finished, this,
            [this, reply, url, build, path, progress, cancel, hop, promise,
             timeout, state, output]() {
                const bool timedOut = !timeout->isActive();
                timeout->stop();
                const QByteArray finalChunk =
                    reply->isOpen() ? reply->readAll() : QByteArray();
                if (!finalChunk.isEmpty() && !state->writeFailed) {
                    if (output->write(finalChunk) != finalChunk.size())
                        state->writeFailed = true;
                    else
                        state->bytes += finalChunk.size();
                }
                DownloadResult result;
                result.status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                result.bytes = state->bytes;
                const QUrl next =
                    timedOut || state->abandoned || ioCancelled(cancel)
                        ? QUrl()
                        : redirectTarget(reply, url);
                if (state->abandoned || ioCancelled(cancel))
                    result.error = QStringLiteral("Download cancelled");
                else if (timedOut)
                    result.error = QStringLiteral("Download timed out");
                else if (state->writeFailed)
                    result.error = QStringLiteral("Unable to write cache file");
                else if (next.isValid())
                    result.error.clear();
                else if (reply->error() != QNetworkReply::NoError)
                    result.error = safeReplyError(reply);
                if (progress && !next.isValid() && result.error.isEmpty()) {
                    const QVariant declared =
                        reply->header(QNetworkRequest::ContentLengthHeader);
                    progress(state->bytes, declared.isValid()
                                               ? declared.toLongLong()
                                               : qint64(-1));
                }
                reply->deleteLater();
                if (next.isValid()) {
                    startDownload(next, build, path, progress, cancel, hop + 1,
                                  output, promise);
                    return;
                }
                if (result.status != 200 || !result.error.isEmpty() ||
                    !output->commit()) {
                    output->cancelWriting();
                    if (result.error.isEmpty())
                        result.error =
                            QStringLiteral("Unable to commit cache file");
                    promise->addResult(result);
                    promise->finish();
                    return;
                }
                promise->addResult(result);
                promise->finish();
            });
    }

    struct RangeResult {
        bool ok = false;
        QString error;
        QVector<QByteArray> bodies;
    };

    struct RangeJob {
        RangeFactory build;
        QUrl target;
        QVector<ObjectRange> ranges;
        IoCancel cancel;
        std::shared_ptr<QPromise<RangeResult>> promise;
        QVector<QByteArray> bodies;
        QHash<QNetworkReply*, int> active;
        QHash<QNetworkReply*, int> redirects;
        QHash<QNetworkReply*, QUrl> urls;
        int next = 0;
        int completed = 0;
        bool failed = false;
        QString error;
        QTimer* timeout = nullptr;
    };

    void startRanges(const RangeFactory& build, const QUrl& target,
                     const QVector<ObjectRange>& ranges, const IoCancel& cancel,
                     const std::shared_ptr<QPromise<RangeResult>>& promise) {
        auto job = std::make_shared<RangeJob>();
        job->build = build;
        job->target = target;
        job->ranges = ranges;
        job->cancel = cancel;
        job->promise = promise;
        job->bodies.resize(ranges.size());
        job->timeout = new QTimer(this);
        job->timeout->setSingleShot(true);
        QObject::connect(job->timeout, &QTimer::timeout, this, [this, job]() {
            failRanges(job, QStringLiteral("Range GET timed out"));
        });
        constexpr int kConcurrentRanges = 24;
        while (job->next < job->ranges.size() &&
               job->active.size() < kConcurrentRanges)
            launchRange(job, job->next++, job->target, 0);
        job->timeout->start(kRequestTimeoutMs);
    }

    void launchRange(const std::shared_ptr<RangeJob>& job, int index,
                     const QUrl& url, int redirects) {
        if (job->failed) return;
        if (ioCancelled(job->cancel)) {
            failRanges(job, QStringLiteral("Cancelled"));
            return;
        }
        QNetworkRequest request = job->build(url, job->ranges[index]);
        QNetworkReply* reply = manager_->get(request);
        job->active.insert(reply, index);
        job->redirects.insert(reply, redirects);
        job->urls.insert(reply, url);
        watchCancel(reply, job->cancel);
        QObject::connect(reply, &QNetworkReply::finished, this,
                         [this, job, reply]() { finishRange(job, reply); });
    }

    void finishRange(const std::shared_ptr<RangeJob>& job,
                     QNetworkReply* reply) {
        if (!job->active.contains(reply)) {
            reply->deleteLater();
            return;
        }
        const int index = job->active.take(reply);
        const int redirects = job->redirects.take(reply);
        const QUrl from = job->urls.take(reply);
        job->timeout->start(kRequestTimeoutMs);
        const QUrl redirect = redirectTarget(reply, from);
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body =
            reply->isOpen() ? reply->readAll() : QByteArray();
        const QString replyError = reply->error() == QNetworkReply::NoError
                                       ? QString()
                                       : safeReplyError(reply);
        reply->deleteLater();
        if (job->failed) return;
        if (ioCancelled(job->cancel)) {
            failRanges(job, QStringLiteral("Cancelled"));
            return;
        }
        if (redirect.isValid()) {
            if (redirects >= kMaximumRedirects) {
                failRanges(job, QStringLiteral("Too many redirects"));
                return;
            }
            launchRange(job, index, redirect, redirects + 1);
            return;
        }
        const qint64 expected = job->ranges[index].length;
        if (status != 206 || body.size() != expected) {
            failRanges(job, !replyError.isEmpty()
                                ? replyError
                                : QStringLiteral("Range GET returned HTTP %1 "
                                                 "with %2 of %3 bytes")
                                      .arg(status)
                                      .arg(body.size())
                                      .arg(expected));
            return;
        }
        job->bodies[index] = body;
        ++job->completed;
        constexpr int kConcurrentRanges = 24;
        while (!job->failed && job->active.size() < kConcurrentRanges &&
               job->next < job->ranges.size())
            launchRange(job, job->next++, job->target, 0);
        if (job->completed == job->ranges.size()) {
            job->timeout->stop();
            job->timeout->deleteLater();
            RangeResult result;
            result.ok = true;
            result.bodies = job->bodies;
            job->promise->addResult(result);
            job->promise->finish();
        }
    }

    void failRanges(const std::shared_ptr<RangeJob>& job,
                    const QString& error) {
        if (job->failed) return;
        job->failed = true;
        job->error = error;
        job->timeout->stop();
        job->timeout->deleteLater();
        for (QNetworkReply* reply : job->active.keys()) reply->abort();
        RangeResult result;
        result.error = error;
        job->promise->addResult(result);
        job->promise->finish();
    }

    QThread thread_;
    QNetworkAccessManager* manager_ = nullptr;
    std::once_flag started_;
    std::atomic<bool> ready_{false};
};

DownloadResult downloadToFile(const QUrl& url, const RequestFactory& build,
                              const QString& path,
                              const DownloadProgress& progress = {},
                              const IoCancel& cancel = {}) {
    return NetworkIo::instance().download(url, build, path, progress,
                                          effectiveCancel(cancel));
}

}  // namespace

void drainNetworkIo() { NetworkIo::instance().drain(); }

FileDownload downloadFile(const QUrl& url, const RequestFactory& build,
                          const QString& path, const DownloadProgress& progress,
                          const IoCancel& cancel) {
    const DownloadResult result =
        downloadToFile(url, build, path, progress, cancel);
    FileDownload download;
    download.status = result.status;
    download.error = result.error;
    download.bytes = result.bytes;
    return download;
}

QNetworkRequest makeRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

HttpResponse sendFollowing(const QUrl& url, const QByteArray& method,
                           const RequestFactory& build, const QByteArray& body,
                           const IoCancel& cancel) {
    return NetworkIo::instance().send(url, method, build, body,
                                      effectiveCancel(cancel));
}

HttpResponse RemoteBackend::head(const QUrl& url,
                                 const IoCancel& cancel) const {
    const RequestFactory build = [this](const QUrl& hop) {
        QNetworkRequest request = makeRequest(hop);
        sign(request, "HEAD", {});
        return request;
    };
    return sendFollowing(url, "HEAD", build, {}, cancel);
}

HttpResponse RemoteBackend::get(const QUrl& url, const IoCancel& cancel) const {
    const RequestFactory build = [this](const QUrl& hop) {
        QNetworkRequest request = makeRequest(hop);
        sign(request, "GET", {});
        return request;
    };
    return sendFollowing(url, "GET", build, {}, cancel);
}

QString RemoteBackend::putIfAbsent(const QUrl& url, const QByteArray& body,
                                   const IoCancel& cancel) const {
    const RequestFactory build = [this, body](const QUrl& hop) {
        QNetworkRequest request = makeRequest(hop);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/octet-stream"));
        request.setRawHeader("If-None-Match", "*");
        sign(request, "PUT", body);
        return request;
    };
    const HttpResponse response =
        sendFollowing(url, "PUT", build, body, cancel);
    if (response.status == 200 || response.status == 201 ||
        response.status == 204 || response.status == 412)
        return {};
    return response.error.isEmpty()
               ? QStringLiteral("Upload returned HTTP %1").arg(response.status)
               : response.error;
}

namespace {

QJsonObject readIndex(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

bool writeIndex(const QString& path, const QJsonObject& index) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(QJsonDocument(index).toJson(QJsonDocument::Compact)) < 0)
        return false;
    return file.commit();
}

/// What the last sync left behind and is still on disk — the answer when the
/// server cannot be reached.
QVector<RemoteObject> cachedObjects(const QJsonObject& entries,
                                    const QString& cachePath) {
    QVector<RemoteObject> result;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject row = it.value().toObject();
        const QString relative = it.key();
        if (relative.isEmpty()) continue;
        if (!QFileInfo::exists(QDir(cachePath).filePath(relative))) continue;
        RemoteObject object;
        object.relativePath = relative;
        object.etag = row.value(QStringLiteral("etag")).toString();
        object.modified = row.value(QStringLiteral("modified")).toString();
        object.size =
            row.value(QStringLiteral("size")).toVariant().toLongLong();
        result.append(object);
    }
    return result;
}

/// Empty when `relative` names a file this machine can actually hold under
/// the cache root, else why it cannot.
///
/// A server chooses these names, so they are untrusted input in both
/// directions. An href or an object key can be built to climb out of the
/// cache directory, and S3 permits characters — `:`, `?`, `*`, `|` — that
/// Windows simply will not create. The rule is applied on every platform so
/// that a library looks the same everywhere rather than quietly holding files
/// that vanish when the same bucket is opened on another machine.
QString localPathError(const QString& relative) {
    if (relative.isEmpty()) return QStringLiteral("empty name");
    if (relative != QDir::cleanPath(relative) ||
        relative.startsWith(QLatin1Char('/')) ||
        relative == QStringLiteral("..") ||
        relative.startsWith(QStringLiteral("../")) ||
        relative.contains(QStringLiteral("/../")))
        return QStringLiteral("path escapes the cache folder");
    for (const QChar character : relative) {
        const char16_t code = character.unicode();
        if (code < 0x20 || code == u'<' || code == u'>' || code == u':' ||
            code == u'"' || code == u'|' || code == u'?' || code == u'*' ||
            code == u'\\')
            return QStringLiteral("name contains %1, which cannot be a file")
                .arg(character);
    }
    return {};
}

/// Creates or empties the placeholder standing in for a streamed video.
///
/// Truncating rather than only creating matters on upgrade: a cache filled by
/// a build that downloaded video still holds those gigabytes, and this is
/// where they are handed back.
bool ensureStub(const QString& localPath) {
    const QFileInfo info(localPath);
    if (info.exists() && info.size() == 0) return true;
    QFile stub(localPath);
    return stub.open(QIODevice::WriteOnly | QIODevice::Truncate);
}

/// Where `localPath` sits inside `cachePath`, or empty when it sits outside
/// it. The caller is handing over a path the library produced, so this is the
/// check that keeps a plain local folder from being read as a connection's.
QString relativeInCache(const QString& cachePath, const QString& localPath) {
    const QString relative = QDir(cachePath).relativeFilePath(localPath);
    if (relative.isEmpty() || relative.startsWith(QStringLiteral("..")))
        return {};
    return relative;
}

/// The pinned-for-offline names an index carries, as a set.
QSet<QString> offlineNames(const QJsonObject& index) {
    QSet<QString> names;
    const QJsonArray pinned = index.value(QStringLiteral("offline")).toArray();
    for (const QJsonValue& value : pinned) {
        const QString name = value.toString();
        if (!name.isEmpty()) names.insert(name);
    }
    return names;
}

QJsonArray sortedArray(const QSet<QString>& names) {
    QStringList ordered(names.cbegin(), names.cend());
    ordered.sort();
    QJsonArray array;
    for (const QString& name : std::as_const(ordered)) array.append(name);
    return array;
}

/// One per-location index.json, serialised by a mutex. A sync that reads the
/// index, talks to the server for minutes, and writes it back would otherwise
/// lose a pin added mid-sync; the rebuild re-reads the offline set under the
/// lock and merges it instead of overwriting it from a stale snapshot.
class CacheIndex {
public:
    explicit CacheIndex(const QString& cachePath)
        : path_(QDir(cachePath).filePath(QStringLiteral("index.json"))) {}

    QJsonObject read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return readIndex(path_);
    }

    /// The pinned-for-offline names, read under the lock.
    QSet<QString> offlinePins() const { return offlineNames(read()); }

    /// Atomically add or withdraw a pin. Returns true on success, false when
    /// the recording is not listed or the index could not be written.
    bool setOfflinePin(const QString& relative, bool pinned) {
        std::lock_guard<std::mutex> lock(mutex_);
        const QJsonObject index = readIndex(path_);
        if (!index.value(QStringLiteral("entries"))
                 .toObject()
                 .contains(relative))
            return false;
        QSet<QString> names = offlineNames(index);
        if (pinned == names.contains(relative)) return true;
        if (pinned)
            names.insert(relative);
        else
            names.remove(relative);
        QJsonObject updated = index;
        updated.insert(QStringLiteral("offline"), sortedArray(names));
        return writeIndex(path_, updated);
    }

    /// Rebuild the entries while preserving pins added since the sync's
    /// snapshot. `keptPins` receives the pins that survived (those whose file
    /// the server still lists). Returns false when the write fails.
    bool rebuild(const QJsonObject& newEntries, const QString& scope,
                 const QString& targetUrl, QSet<QString>* keptPins = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        const QSet<QString> pinned = offlineNames(readIndex(path_));
        QSet<QString> kept;
        for (const QString& name : pinned)
            if (newEntries.contains(name)) kept.insert(name);
        const QJsonObject index{
            {QStringLiteral("version"), 1},
            {QStringLiteral("url"), targetUrl},
            {QStringLiteral("offline"), sortedArray(kept)},
            {QStringLiteral("scope"), scope},
            {QStringLiteral("entries"), newEntries},
            {QStringLiteral("syncedAt"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
        if (keptPins) *keptPins = kept;
        return writeIndex(path_, index);
    }

private:
    QString path_;
    mutable std::mutex mutex_;
};

/// One CacheIndex per cache directory, living for the process. The directory
/// is stable per connection, so a second sync of the same server reaches the
/// same mutex as the first.
CacheIndex& cacheIndexFor(const QString& cachePath) {
    static std::mutex mapMutex;
    static QHash<QString, CacheIndex*> indices;
    std::lock_guard<std::mutex> lock(mapMutex);
    const auto it = indices.constFind(cachePath);
    if (it != indices.cend()) return **it;
    // Leaked like NetworkIo: a CacheIndex outlives any thread that could
    // safely destroy it, and one per connection is a handful of bytes.
    auto* index = new CacheIndex(cachePath);
    indices.insert(cachePath, index);
    return *index;
}

RemoteSyncResult offlineResult(RemoteSyncResult result,
                               const QVector<RemoteObject>& cached,
                               const QString& reason) {
    result.fromCache = !cached.isEmpty();
    for (const RemoteObject& object : cached)
        result.files.append(QDir().cleanPath(object.relativePath));
    result.success = result.fromCache;
    result.status = result.fromCache
                        ? QStringLiteral("Offline cache (%1 files)")
                              .arg(result.files.size())
                        : QStringLiteral("Server unavailable: %1").arg(reason);
    return result;
}

}  // namespace

QString locationId(const QString& target, const QString& username) {
    const QByteArray key =
        (target.trimmed() + QLatin1Char('\n') + username).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString cacheRoot() {
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/omatrack");
}

/// Every directory a protocol keeps caches in.
///
/// Enumerated from the protocols rather than from the configured locations,
/// so a cache belonging to a location that was removed while this build was
/// not running is still found, counted, and cleared.
QStringList cacheDirectories() {
    QStringList directories;
    for (const LocationType type :
         {LocationType::WebDav, LocationType::S3, LocationType::Gcs}) {
        const QString path =
            cacheRoot() + QLatin1Char('/') + locationTypeKey(type);
        if (QFileInfo::exists(path)) directories.append(path);
    }
    return directories;
}

CacheUsage cacheUsage() {
    CacheUsage usage;
    const auto countDirectory = [&usage](const QString& directory) {
        QDirIterator files(directory, QDir::Files | QDir::Hidden,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            if (isVideoFile(info.fileName()))
                usage.videoBytes += info.size();
            else
                usage.bytes += info.size();
        }
    };
    for (const QString& directory : cacheDirectories())
        countDirectory(directory);
    countDirectory(telemetryCacheRoot());
    return usage;
}

qint64 clearCache() {
    const CacheUsage usage = cacheUsage();
    for (const QString& directory : cacheDirectories())
        QDir(directory).removeRecursively();
    QDir(telemetryCacheRoot()).removeRecursively();
    return usage.bytes + usage.videoBytes;
}

qint64 parseByteSize(const QString& text, qint64 fallback) {
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*([0-9]+(?:[.,][0-9]+)?)\\s*([a-zA-Z]*)\\s*$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return fallback;
    bool ok = false;
    const double amount = QString(match.captured(1))
                              .replace(QLatin1Char(','), QLatin1Char('.'))
                              .toDouble(&ok);
    if (!ok || amount <= 0) return fallback;

    // GB and GiB both mean 2^30 here. Nobody writing a cache limit means the
    // decimal one, and reading it that way would make the number on screen —
    // which Qt formats in binary units — disagree with the number typed.
    static const QMap<QString, qint64> units{
        {QStringLiteral(""), 1},
        {QStringLiteral("b"), 1},
        {QStringLiteral("k"), 1LL << 10},
        {QStringLiteral("kb"), 1LL << 10},
        {QStringLiteral("kib"), 1LL << 10},
        {QStringLiteral("m"), 1LL << 20},
        {QStringLiteral("mb"), 1LL << 20},
        {QStringLiteral("mib"), 1LL << 20},
        {QStringLiteral("g"), 1LL << 30},
        {QStringLiteral("gb"), 1LL << 30},
        {QStringLiteral("gib"), 1LL << 30},
        {QStringLiteral("t"), 1LL << 40},
        {QStringLiteral("tb"), 1LL << 40},
        {QStringLiteral("tib"), 1LL << 40}};
    const auto unit = units.constFind(match.captured(2).toLower());
    if (unit == units.cend()) return fallback;
    return qint64(amount * double(unit.value()));
}

qint64 enforceCacheBudget(qint64 limitBytes, const QSet<QString>& keepPaths) {
    if (limitBytes <= 0) return 0;

    struct Candidate {
        QString path;
        qint64 size = 0;
        QDateTime used;
    };
    QVector<Candidate> candidates;
    qint64 total = 0;
    // The discovery caches and the ETag-keyed normalized-telemetry mirror
    // share one budget and one LRU, and evicting them by the same mtime rule
    // keeps the whole cache honest. The Track Atlas snapshot lives
    // outside this — it is a single small file refreshed on a timer, not a
    // growing pile of laps, and evicting it would only force an immediate
    // re-fetch.
    QStringList directories = cacheDirectories();
    directories.append(telemetryCacheRoot());
    for (const QString& directory : std::as_const(directories)) {
        QDirIterator files(directory, QDir::Files | QDir::Hidden,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            // Video is off this ledger on both sides. It is only ever here
            // because somebody pinned it for a flight, and one recording
            // would otherwise blow the whole budget and take a season of
            // telemetry with it.
            if (isVideoFile(info.fileName())) continue;
            total += info.size();
            // A stub costs nothing and stands for a session that is still
            // there, and index.json is the bookkeeping that makes everything
            // beside it reusable rather than a pile of orphans.
            if (info.size() == 0 ||
                info.fileName() == QStringLiteral("index.json"))
                continue;
            if (keepPaths.contains(info.absoluteFilePath())) continue;
            candidates.append(
                {info.absoluteFilePath(), info.size(), info.lastModified()});
        }
    }
    if (total <= limitBytes) return 0;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.used < right.used;
              });
    qint64 freed = 0;
    for (const Candidate& candidate : candidates) {
        if (total - freed <= limitBytes) break;
        // The index still lists what was just deleted, which is what makes
        // this safe: the next sync finds the file missing and fetches it
        // again, rather than treating the gap as something the server dropped.
        if (QFile::remove(candidate.path)) {
            qCInfo(lcIo).noquote()
                << "cache evict" << omatrack::displayPath(candidate.path)
                << omatrack::formatBytes(candidate.size);
            freed += candidate.size;
        }
    }
    return freed;
}

bool isVideoFile(const QString& path) {
    static const QSet<QString> extensions{
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("avi"), QStringLiteral("m4v"), QStringLiteral("webm")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

QString cacheDirectory(const RemoteConnection& connection) {
    if (connection.type == LocationType::Folder) return {};
    const QString id = connection.id.isEmpty()
                           ? locationId(connection.target, connection.username)
                           : connection.id;
    return cacheRoot() + QLatin1Char('/') + locationTypeKey(connection.type) +
           QLatin1Char('/') + id;
}

QString etagFileKey(const QString& etag) {
    QString key = etag.trimmed();
    if (key.size() >= 2 && key.front() == QLatin1Char('"') &&
        key.back() == QLatin1Char('"'))
        key = key.mid(1, key.size() - 2);
    key.replace(QLatin1Char('/'), QLatin1Char('_'));
    key.replace(QLatin1Char('\\'), QLatin1Char('_'));
    key.remove(QLatin1Char('"'));
    if (key.isEmpty() || !localPathError(key).isEmpty()) return {};
    return key;
}
QString telemetryCacheRoot() {
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/.omatrack/c");
}

QString telemetryCacheRelativeDirectory() {
    return QStringLiteral(".omatrack/c/") +
           QString::fromStdString(omatrack::converterGeneration());
}

QString telemetryCacheDirectory() {
    return telemetryCacheRoot() + QLatin1Char('/') +
           QString::fromStdString(omatrack::converterGeneration());
}

int pruneStaleTelemetryCaches() {
    // A directory under .omatrack/c that is not this build's generation was
    // written by a converter this build no longer trusts; nothing will ever
    // read it again, so it would only sit there counting against the budget.
    const QString current = QFileInfo(telemetryCacheDirectory()).fileName();
    int removed = 0;
    const QDir root(telemetryCacheRoot());
    for (const QFileInfo& entry :
         root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.fileName() == current) continue;
        if (QDir(entry.absoluteFilePath()).removeRecursively()) ++removed;
    }
    // Pre-generation layout kept `{key}.telemetry` directly in the root.
    for (const QFileInfo& entry : root.entryInfoList(QDir::Files)) {
        if (QFile::remove(entry.absoluteFilePath())) ++removed;
    }
    return removed;
}

QString telemetryCachePath(const QString& key) {
    const QString safe = etagFileKey(key);
    if (safe.isEmpty()) return {};
    return QDir(telemetryCacheDirectory())
        .filePath(safe + QStringLiteral(".telemetry"));
}

bool isSidecarPath(const QString& relativePath) {
    if (relativePath.startsWith(QStringLiteral(".omatrack/"))) return true;
    const QFileInfo info(relativePath);
    const QString name = info.fileName().toLower();
    if (name.contains(QStringLiteral(".ext.jsonl")) ||
        name.contains(QStringLiteral(".mtx.jsonl")))
        return true;
    if (!info.fileName().startsWith(QLatin1Char('.'))) return false;
    const QString suffix = info.suffix().toLower();
    return suffix == QStringLiteral("telemetry") ||
           suffix == QStringLiteral("json") || suffix == QStringLiteral("ld") ||
           suffix == QStringLiteral("ldx");
}

QString cachedObjectEtag(const RemoteConnection& connection,
                         const QString& localPath) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return {};
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty() || isSidecarPath(relative)) return {};
    return cacheIndexFor(cachePath)
        .read()
        .value(QStringLiteral("entries"))
        .toObject()
        .value(relative)
        .toObject()
        .value(QStringLiteral("etag"))
        .toString();
}
namespace {
RemoteBackend backendFor(const RemoteConnection& connection) {
    switch (connection.type) {
        case LocationType::WebDav: return makeWebDavBackend(connection);
        case LocationType::S3:
        case LocationType::Gcs:
            // Google Cloud Storage speaks the S3 API — SigV4 with an HMAC
            // interoperability key and ListObjectsV2 paging — so one backend
            // covers both. This is the only switch on LocationType outside
            // the protocol constructors themselves.
            return makeS3Backend(connection);
        case LocationType::Folder: break;
    }
    return {};
}
}  // namespace

QString fetchRemoteObject(const RemoteConnection& connection,
                          const QString& relativePath,
                          const QString& destination, const IoCancel& cancel) {
    if (connection.type == LocationType::Folder || relativePath.isEmpty())
        return QStringLiteral("This file is not on a server.");
    if (!localPathError(relativePath).isEmpty())
        return QStringLiteral("Cannot store that name as a file.");
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return QStringLiteral("Unable to create the telemetry cache folder");

    RemoteConnection signedConnection = connection;
    const QString scope = cacheIndexFor(cacheDirectory(connection))
                              .read()
                              .value(QStringLiteral("scope"))
                              .toString();
    if (signedConnection.options.value(QStringLiteral("region")).isEmpty() &&
        !scope.isEmpty())
        signedConnection.options.insert(QStringLiteral("region"), scope);
    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.objectUrl || !backend.sign)
        return QStringLiteral("No protocol backend");
    const QUrl url = backend.objectUrl(relativePath);
    if (!url.isValid()) return QStringLiteral("Invalid remote cache URL");
    const RequestFactory get = [backend](const QUrl& hop) {
        QNetworkRequest request = makeRequest(hop);
        backend.sign(request, "GET", {});
        return request;
    };
    const DownloadResult download =
        downloadToFile(url, get, destination, {}, cancel);
    if (download.status == 404) {
        QFile::remove(destination);
        return {};
    }
    if (download.status != 200 || !download.error.isEmpty()) {
        QFile::remove(destination);
        return download.error.isEmpty()
                   ? QStringLiteral("Download returned HTTP %1")
                         .arg(download.status)
                   : download.error;
    }
    return {};
}

QString publishRemoteObject(const RemoteConnection& connection,
                            const QString& relativePath, const QByteArray& body,
                            const IoCancel& cancel) {
    if (connection.type == LocationType::Folder || relativePath.isEmpty())
        return QStringLiteral("This file is not on a server.");
    if (!localPathError(relativePath).isEmpty())
        return QStringLiteral("Cannot store that name as a file.");

    RemoteConnection signedConnection = connection;
    const QString scope = cacheIndexFor(cacheDirectory(connection))
                              .read()
                              .value(QStringLiteral("scope"))
                              .toString();
    if (signedConnection.options.value(QStringLiteral("region")).isEmpty() &&
        !scope.isEmpty())
        signedConnection.options.insert(QStringLiteral("region"), scope);
    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.objectUrl || !backend.sign)
        return QStringLiteral("No protocol backend");
    const QUrl url = backend.objectUrl(relativePath);
    if (!url.isValid()) return QStringLiteral("Invalid remote cache URL");
    return backend.putIfAbsent(url, body, cancel);
}

QUrl streamSource(const RemoteConnection& connection,
                  const QString& localPath) {
    // Downloaded for a flight: the file is the source, and no signature it
    // could be given would be better than bytes that are already here.
    if (QFileInfo(localPath).size() > 0) return {};

    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return {};
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty()) return {};

    const QJsonObject index = cacheIndexFor(cachePath).read();
    const QJsonObject entry = index.value(QStringLiteral("entries"))
                                  .toObject()
                                  .value(relative)
                                  .toObject();
    if (!entry.value(QStringLiteral("stream")).toBool()) return {};
    const QUrl url(entry.value(QStringLiteral("url")).toString(),
                   QUrl::StrictMode);
    if (!url.isValid()) return {};

    // The protocol decides how a credential-less client fetches the URL:
    // SigV4 presigning for S3/GCS, credentials embedded for WebDAV. The
    // scope the sync recorded stands in for the region a fresh signature
    // would otherwise need a round trip to discover.
    RemoteConnection signedConnection = connection;
    const QString scope = index.value(QStringLiteral("scope")).toString();
    if (signedConnection.options.value(QStringLiteral("region")).isEmpty() &&
        !scope.isEmpty())
        signedConnection.options.insert(QStringLiteral("region"), scope);
    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.presign) return {};
    return backend.presign(url, kStreamExpirySeconds);
}

QUrl objectUrlForPath(const RemoteConnection& connection,
                      const QString& localPath) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return {};
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty()) return {};
    const QUrl url(cacheIndexFor(cachePath)
                       .read()
                       .value(QStringLiteral("entries"))
                       .toObject()
                       .value(relative)
                       .toObject()
                       .value(QStringLiteral("url"))
                       .toString(),
                   QUrl::StrictMode);
    return url.isValid() ? url : QUrl();
}

bool getObjectRanges(const RemoteConnection& connection, const QUrl& url,
                     const QVector<ObjectRange>& ranges,
                     QVector<QByteArray>* bodies, QString* error,
                     const IoCancel& cancel) {
    if (error) error->clear();
    if (!bodies || !url.isValid()) {
        if (error) *error = QStringLiteral("Invalid range request");
        return false;
    }
    bodies->clear();
    bodies->resize(ranges.size());
    if (ranges.isEmpty()) return true;
    for (const ObjectRange& range : ranges) {
        if (range.offset < 0 || range.length <= 0) {
            if (error) *error = QStringLiteral("Invalid range request");
            return false;
        }
    }

    RemoteConnection signedConnection = connection;
    const QString cachePath = cacheDirectory(connection);
    if (!cachePath.isEmpty()) {
        const QString scope = cacheIndexFor(cachePath)
                                  .read()
                                  .value(QStringLiteral("scope"))
                                  .toString();
        if (signedConnection.options.value(QStringLiteral("region"))
                .isEmpty() &&
            !scope.isEmpty())
            signedConnection.options.insert(QStringLiteral("region"), scope);
    }

    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.presign) {
        if (error) *error = QStringLiteral("No protocol backend");
        return false;
    }
    // Range is deliberately not among the signed headers, so one presigned
    // object URL can multiplex every sample request — the credential rides
    // in the URL for both protocols.
    const QUrl target = backend.presign(url, kStreamExpirySeconds);
    if (!target.isValid()) {
        if (error) *error = QStringLiteral("Unable to sign object URL");
        return false;
    }
    const auto build = [](const QUrl& hop, const ObjectRange& range) {
        QNetworkRequest request = makeRequest(hop);
        request.setRawHeader(
            "Range", QByteArray("bytes=") + QByteArray::number(range.offset) +
                         '-' +
                         QByteArray::number(range.offset + range.length - 1));
        return request;
    };

    return NetworkIo::instance().ranges(build, target, ranges, bodies, error,
                                        effectiveCancel(cancel));
}

QByteArray getObjectRange(const RemoteConnection& connection, const QUrl& url,
                          qint64 offset, qint64 length, QString* error,
                          const IoCancel& cancel) {
    QVector<QByteArray> bodies;
    if (!getObjectRanges(connection, url, {{offset, length}}, &bodies, error,
                         cancel))
        return {};
    return bodies.front();
}

bool offlineVideoPinned(const RemoteConnection& connection,
                        const QString& localPath) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return false;
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty()) return false;
    return cacheIndexFor(cachePath).offlinePins().contains(relative);
}

QString pinOfflineVideo(const RemoteConnection& connection,
                        const QString& localPath, bool pinned) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty())
        return QStringLiteral("This recording is not on a server.");
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty())
        return QStringLiteral("This recording is not in the cache.");

    CacheIndex& index = cacheIndexFor(cachePath);
    if (!index.setOfflinePin(relative, pinned)) {
        // Either the server has not listed this recording, or the index could
        // not be written — both are reasons to stop rather than pretend.
        const QJsonObject current = index.read();
        if (!current.value(QStringLiteral("entries"))
                 .toObject()
                 .contains(relative))
            return QStringLiteral("The server has not listed this recording.");
        return QStringLiteral("Unable to write the cache index.");
    }
    // Giving the space back is the whole point of unpinning, and the stub is
    // what keeps the recording in the library and streaming afterwards.
    if (!pinned && !ensureStub(localPath))
        return QStringLiteral("Unable to reclaim the downloaded file.");
    return {};
}

QString fetchObject(const RemoteConnection& connection,
                    const QString& localPath, const DownloadProgress& progress,
                    const IoCancel& cancel) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty())
        return QStringLiteral("This recording is not on a server.");
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty())
        return QStringLiteral("This recording is not in the cache.");

    const QJsonObject index = cacheIndexFor(cachePath).read();
    const QJsonObject entry = index.value(QStringLiteral("entries"))
                                  .toObject()
                                  .value(relative)
                                  .toObject();
    const QUrl url(entry.value(QStringLiteral("url")).toString(),
                   QUrl::StrictMode);
    if (!url.isValid())
        return QStringLiteral("The server has not listed this recording.");

    // The same presigned URL the player would stream: the scope the sync
    // recorded stands in for the region a fresh signature would otherwise
    // need a round trip to discover, and the credential rides in the URL so
    // the download needs no header signing.
    RemoteConnection signedConnection = connection;
    const QString scope = index.value(QStringLiteral("scope")).toString();
    if (signedConnection.options.value(QStringLiteral("region")).isEmpty() &&
        !scope.isEmpty())
        signedConnection.options.insert(QStringLiteral("region"), scope);
    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.presign)
        return QStringLiteral("This recording is not on a server.");
    const QUrl target = backend.presign(url, kStreamExpirySeconds);
    if (!target.isValid())
        return QStringLiteral("The server has not listed this recording.");
    const RequestFactory build = [](const QUrl& hop) {
        return makeRequest(hop);
    };

    qCInfo(lcIo).noquote() << "download start" << relative;
    const DownloadResult download =
        downloadToFile(target, build, localPath, progress, cancel);
    if (download.status != 200 || !download.error.isEmpty()) {
        qCInfo(lcIo).noquote()
            << "download failed" << relative << download.error;
        // Whatever partial state a failure left, the recording still has to be
        // playable, and a stub is what the rest of the app expects to find.
        ensureStub(localPath);
        return download.error.isEmpty()
                   ? QStringLiteral("Download returned HTTP %1")
                         .arg(download.status)
                   : download.error;
    }
    qCInfo(lcIo).noquote() << "write download" << relative
                           << formatBytes(download.bytes);
    return {};
}

ConnectionAddress splitAddress(LocationType type, const QString& address) {
    switch (type) {
        case LocationType::S3:
        case LocationType::Gcs: return s3SplitAddress(type, address);
        case LocationType::WebDav: {
            // `https://user:pass@server/dav/` is how a WebDAV URL carries a
            // credential, and QUrl parses that authority correctly — unlike an
            // S3 one, where the bucket is not a host.
            ConnectionAddress split;
            QUrl url(address.trimmed());
            split.username = url.userName();
            split.password = url.password();
            url.setUserInfo(QString());
            split.target = url.toString(QUrl::FullyEncoded);
            return split;
        }
        case LocationType::Folder: break;
    }
    ConnectionAddress split;
    split.target = address.trimmed();
    return split;
}

QString validateTarget(LocationType type, const QString& target) {
    switch (type) {
        case LocationType::WebDav: return webDavTargetError(target);
        case LocationType::S3:
        case LocationType::Gcs: return s3TargetError(type, target);
        case LocationType::Folder: break;
    }
    return QStringLiteral("Unsupported connection type.");
}

QString normalizeTarget(LocationType type, const QString& target) {
    switch (type) {
        case LocationType::WebDav:
            // Unchanged from when WebDAV stood alone, so no configured server
            // gets a new id and loses the cache it already downloaded.
            return QUrl(target.trimmed()).toString(QUrl::FullyEncoded);
        case LocationType::S3:
        case LocationType::Gcs: return s3NormalizedTarget(type, target);
        case LocationType::Folder: break;
    }
    return target.trimmed();
}

RemoteSyncResult syncConnection(const RemoteConnection& connection,
                                const IoCancel& cancel) {
    const IoCancelScope cancelScope(cancel);
    if (ioCancelled(cancel)) {
        RemoteSyncResult result;
        result.error = QStringLiteral("Cancelled");
        result.status = result.error;
        return result;
    }
    RemoteSyncResult result;
    result.id = connection.id.isEmpty()
                    ? locationId(connection.target, connection.username)
                    : connection.id;
    result.cachePath = cacheDirectory(connection);
    CacheIndex& cacheIndex = cacheIndexFor(result.cachePath);
    const QJsonObject oldIndex = cacheIndex.read();
    const QJsonObject oldEntries =
        oldIndex.value(QStringLiteral("entries")).toObject();
    const QSet<QString> pinned = offlineNames(oldIndex);
    const QVector<RemoteObject> cached =
        cachedObjects(oldEntries, result.cachePath);

    const QString invalid = validateTarget(connection.type, connection.target);
    if (!invalid.isEmpty()) {
        result.error = invalid;
        result = offlineResult(std::move(result), cached, invalid);
        if (!result.fromCache) result.status = invalid;
        return result;
    }

    if (!QDir().mkpath(result.cachePath)) {
        result.error = QStringLiteral("Unable to create the cache folder");
        result.status = result.error;
        return result;
    }

    // backend construction only — no other code switches on the protocol.
    const RemoteBackend backend = backendFor(connection);

    QVector<RemoteObject> objects;
    QString scope;
    QString listingError;
    qCInfo(lcIo).noquote() << "sync" << locationTypeKey(connection.type)
                           << connection.name << connection.target;
    if (!backend.list(&objects, &scope, &listingError, cancel)) {
        result.error = listingError;
        qCInfo(lcIo).noquote()
            << "sync offline" << listingError << cached.size() << "cached";
        return offlineResult(std::move(result), cached, listingError);
    }

    std::sort(objects.begin(), objects.end(),
              [](const RemoteObject& left, const RemoteObject& right) {
                  return left.relativePath < right.relativePath;
              });

    QJsonObject newEntries;
    QSet<QString> seen;
    for (const RemoteObject& object : objects) {
        if (ioCancelled(cancel)) {
            result.error = QStringLiteral("Cancelled");
            result.status = result.error;
            return result;
        }
        if (seen.contains(object.relativePath)) continue;
        seen.insert(object.relativePath);
        // Local extract leftovers. Never a share-root metadata store.
        if (object.relativePath.startsWith(QStringLiteral(".omatrack/")))
            continue;
        // Left out of newEntries as well as of the download, so the prune
        // below cannot mistake a name this machine never stored for a file
        // the server deleted.
        if (!localPathError(object.relativePath).isEmpty()) {
            result.skipped.append(object.relativePath);
            continue;
        }
        if (isSidecarPath(object.relativePath)) continue;
        const QJsonObject old =
            oldEntries.value(object.relativePath).toObject();

        const QString localPath =
            QDir(result.cachePath).filePath(object.relativePath);
        if (!QDir().mkpath(QFileInfo(localPath).absolutePath())) {
            result.error = QStringLiteral("Unable to create a cache folder");
            result.status = result.error;
            return result;
        }

        if (isVideoFile(object.relativePath)) {
            const bool keep =
                pinned.contains(object.relativePath) &&
                QFileInfo(localPath).size() > 0 && !object.etag.isEmpty() &&
                object.etag == old.value(QStringLiteral("etag")).toString() &&
                object.modified ==
                    old.value(QStringLiteral("modified")).toString();
            if (!keep && !ensureStub(localPath)) {
                result.error =
                    QStringLiteral("Unable to write the cache placeholder");
                result.status = result.error;
                return result;
            }
            newEntries.insert(
                object.relativePath,
                QJsonObject{{QStringLiteral("etag"), object.etag},
                            {QStringLiteral("modified"), object.modified},
                            {QStringLiteral("size"), object.size},
                            {QStringLiteral("stream"), true},
                            {QStringLiteral("url"),
                             object.url.toString(QUrl::FullyEncoded)}});
            result.files.append(object.relativePath);
            continue;
        }
        if (!ensureStub(localPath)) {
            result.error =
                QStringLiteral("Unable to write the cache placeholder");
            result.status = result.error;
            return result;
        }
        newEntries.insert(
            object.relativePath,
            QJsonObject{{QStringLiteral("etag"), object.etag},
                        {QStringLiteral("modified"), object.modified},
                        {QStringLiteral("size"), object.size},
                        {QStringLiteral("url"),
                         object.url.toString(QUrl::FullyEncoded)}});
        result.files.append(object.relativePath);
    }

    // Anything the server no longer lists is no longer reachable, so the local
    // copy is dead weight rather than an offline fallback.
    for (auto it = oldEntries.begin(); it != oldEntries.end(); ++it)
        if (!newEntries.contains(it.key()))
            QFile::remove(QDir(result.cachePath).filePath(it.key()));

    // Rebuild the index under the per-location lock, re-reading the offline
    // set so a pin added while this sync was talking to the server survives
    // rather than being overwritten by the snapshot taken at the start. A pin
    // for something the server dropped is still a pin on nothing: rebuild
    // intersects the current pins with the new entries.
    if (!cacheIndex.rebuild(newEntries, scope, connection.target)) {
        result.error = QStringLiteral("Unable to write the cache index");
        result.status = result.error;
        return result;
    }
    qCInfo(lcIo).noquote() << "write cache-index"
                           << omatrack::displayPath(
                                  QDir(result.cachePath).filePath("index.json"))
                           << newEntries.size() << "entries"
                           << omatrack::formatBytes(result.downloadedBytes)
                           << "downloaded";
    result.success = true;
    result.status =
        result.downloadedBytes > 0
            ? QStringLiteral("Connected · %1 files · %2 MB")
                  .arg(result.files.size())
                  .arg(double(result.downloadedBytes) / (1024.0 * 1024.0), 0,
                       'f', 1)
            : QStringLiteral("Connected · %1 files (cached)")
                  .arg(result.files.size());
    // Say so rather than letting files quietly go missing: a driver comparing
    // the bucket to the library would otherwise have no way to tell why.
    if (!result.skipped.isEmpty())
        result.status.append(QStringLiteral(" · %1 unusable name%2")
                                 .arg(result.skipped.size())
                                 .arg(result.skipped.size() == 1 ? "" : "s"));
    return result;
}

}  // namespace omatrack
