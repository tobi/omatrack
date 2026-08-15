#include "RemoteCache.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QEventLoop>
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
        QMetaObject::invokeMethod(this, []() {}, Qt::BlockingQueuedConnection);
    }

private:
    NetworkIo() = default;

    template <typename T>
    static T waitFor(QFuture<T> future) {
        // A worker can block. The GUI/test thread must keep pumping: the
        // mock HTTP server (and any UX) lives there. This is not a nested
        // I/O loop — QNAM runs on the I/O thread.
        QCoreApplication* app = QCoreApplication::instance();
        if (app && QThread::currentThread() == app->thread()) {
            while (!future.isFinished())
                app->processEvents(QEventLoop::AllEvents, 20);
        } else {
            future.waitForFinished();
        }
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
        QObject::connect(
            reply, &QNetworkReply::readyRead, this,
            [reply, timeout, progress, cancel, state, output]() {
                const QByteArray chunk = reply->readAll();
                if (output->write(chunk) != chunk.size()) {
                    state->writeFailed = true;
                    reply->abort();
                    return;
                }
                state->bytes += chunk.size();
                timeout->start(kDownloadTimeoutMs);
                const QVariant declared =
                    reply->header(QNetworkRequest::ContentLengthHeader);
                if ((progress &&
                     !progress(state->bytes, declared.isValid()
                                                 ? declared.toLongLong()
                                                 : qint64(-1))) ||
                    ioCancelled(cancel)) {
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

HttpResponse sendFollowing(QNetworkAccessManager&, const QUrl& url,
                           const QByteArray& method,
                           const RequestFactory& build, const QByteArray& body,
                           const IoCancel& cancel) {
    return NetworkIo::instance().send(url, method, build, body,
                                      effectiveCancel(cancel));
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
    for (const QString& directory : cacheDirectories()) {
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
    }
    return usage;
}

qint64 clearCache() {
    const CacheUsage usage = cacheUsage();
    for (const QString& directory : cacheDirectories())
        QDir(directory).removeRecursively();
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
    for (const QString& directory : cacheDirectories()) {
        QDirIterator files(directory, QDir::Files | QDir::Hidden,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            const QFileInfo info = files.fileInfo();
            // Video is off this ledger on both sides. It is only ever here
            // because somebody pinned it for a flight, and one recording would
            // otherwise blow the whole budget and take a season of telemetry
            // with it.
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

bool isSidecarPath(const QString& relativePath) {
    if (relativePath.startsWith(QStringLiteral(".omatrack/"))) return true;
    const QFileInfo info(relativePath);
    if (!info.fileName().startsWith(QLatin1Char('.'))) return false;
    const QString suffix = info.suffix().toLower();
    return suffix == QStringLiteral("telemetry") ||
           suffix == QStringLiteral("json") || suffix == QStringLiteral("ld") ||
           suffix == QStringLiteral("ldx");
}

bool isPortableTelemetryCompanion(const QString& relativePath) {
    const QFileInfo info(relativePath);
    return info.fileName().startsWith(QLatin1Char('.')) &&
           info.suffix().compare(QStringLiteral("telemetry"),
                                 Qt::CaseInsensitive) == 0;
}

QString cachedObjectEtag(const RemoteConnection& connection,
                         const QString& localPath) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return {};
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty() || isSidecarPath(relative)) return {};
    return readIndex(QDir(cachePath).filePath(QStringLiteral("index.json")))
        .value(QStringLiteral("entries"))
        .toObject()
        .value(relative)
        .toObject()
        .value(QStringLiteral("etag"))
        .toString();
}

qint64 cachedObjectSize(const RemoteConnection& connection,
                        const QString& localPath) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return -1;
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty() || isSidecarPath(relative)) return -1;
    return readIndex(QDir(cachePath).filePath(QStringLiteral("index.json")))
        .value(QStringLiteral("entries"))
        .toObject()
        .value(relative)
        .toObject()
        .value(QStringLiteral("size"))
        .toVariant()
        .toLongLong();
}

namespace {
RemoteBackend backendFor(const RemoteConnection& connection) {
    switch (connection.type) {
        case LocationType::WebDav: return makeWebDavBackend(connection);
        case LocationType::S3:
        case LocationType::Gcs: return makeS3Backend(connection);
        case LocationType::Folder: break;
    }
    return {};
}
}  // namespace

QString putObject(const RemoteConnection& connection,
                  const QString& relativePath, const QByteArray& body,
                  const IoCancel& cancel) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty() || relativePath.isEmpty())
        return QStringLiteral("This file is not on a server.");
    if (!localPathError(relativePath).isEmpty())
        return QStringLiteral("Cannot store that name as a file.");

    const QString localPath = QDir(cachePath).filePath(relativePath);
    if (!QDir().mkpath(QFileInfo(localPath).absolutePath()))
        return QStringLiteral("Unable to create a cache folder");

    RemoteConnection signedConnection = connection;
    const QString scope =
        readIndex(QDir(cachePath).filePath(QStringLiteral("index.json")))
            .value(QStringLiteral("scope"))
            .toString();
    if (signedConnection.options.value(QStringLiteral("region")).isEmpty() &&
        !scope.isEmpty())
        signedConnection.options.insert(QStringLiteral("region"), scope);

    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.urlFor || !backend.sign) {
        if (QFileInfo(localPath).size() > 0) return {};
        QSaveFile local(localPath);
        if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            local.write(body) != body.size() || !local.commit())
            return QStringLiteral("Unable to write the metadata cache");
        return {};
    }
    const QUrl url = backend.urlFor(relativePath);
    if (!url.isValid()) return {};

    const QString contentType =
        QFileInfo(relativePath)
                    .suffix()
                    .compare(QStringLiteral("json"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("application/json")
            : QStringLiteral("application/octet-stream");

    QNetworkAccessManager unused;
    const RequestFactory put = [backend, body, contentType](const QUrl& hop) {
        QNetworkRequest request = makeRequest(hop);
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        request.setRawHeader("If-None-Match", "*");
        backend.sign(request, "PUT", body);
        return request;
    };
    const HttpResponse response =
        sendFollowing(unused, url, "PUT", put, body, cancel);
    if (response.status == 200 || response.status == 201 ||
        response.status == 204) {
        qCInfo(lcIo).noquote()
            << "write put" << relativePath << omatrack::formatBytes(body.size())
            << "HTTP" << response.status;
        if (QFileInfo(localPath).size() > 0) return {};
        QSaveFile local(localPath);
        if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            local.write(body) != body.size() || !local.commit())
            return QStringLiteral("Unable to write the metadata cache");
        qCInfo(lcIo).noquote()
            << "write cache" << omatrack::displayPath(localPath)
            << omatrack::formatBytes(body.size());
        return {};
    }
    if (response.status == 412) {
        qCInfo(lcIo).noquote()
            << "write exists" << relativePath << "HTTP 412 (kept server copy)";
        if (QFileInfo(localPath).size() > 0) return {};
        const RequestFactory get = [backend](const QUrl& hop) {
            QNetworkRequest request = makeRequest(hop);
            backend.sign(request, "GET", {});
            return request;
        };
        const HttpResponse existing =
            sendFollowing(unused, url, "GET", get, {}, cancel);
        if (existing.status != 200)
            return existing.error.isEmpty()
                       ? QStringLiteral("Download returned HTTP %1")
                             .arg(existing.status)
                       : existing.error;
        QSaveFile local(localPath);
        if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            local.write(existing.body) != existing.body.size() ||
            !local.commit())
            return QStringLiteral("Unable to write the metadata cache");
        qCInfo(lcIo).noquote()
            << "write cache fetched" << omatrack::displayPath(localPath)
            << omatrack::formatBytes(existing.body.size());
        return {};
    }
    return response.error.isEmpty()
               ? QStringLiteral("Upload returned HTTP %1").arg(response.status)
               : response.error;
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

    const QJsonObject index = readIndex(QDir(cachePath).filePath("index.json"));
    const QJsonObject entry = index.value(QStringLiteral("entries"))
                                  .toObject()
                                  .value(relative)
                                  .toObject();
    if (!entry.value(QStringLiteral("stream")).toBool()) return {};
    const QUrl url(entry.value(QStringLiteral("url")).toString(),
                   QUrl::StrictMode);
    if (!url.isValid()) return {};

    switch (connection.type) {
        case LocationType::WebDav: {
            // ffmpeg reads the credential straight out of the URL, so the
            // player needs to know nothing about how this server authenticates.
            QUrl authenticated = url;
            if (!connection.username.isEmpty()) {
                authenticated.setUserName(connection.username);
                authenticated.setPassword(connection.password);
            }
            return authenticated;
        }
        case LocationType::S3:
        case LocationType::Gcs:
            return s3PresignedUrl(
                connection, index.value(QStringLiteral("scope")).toString(),
                url, kStreamExpirySeconds);
        case LocationType::Folder: break;
    }
    return {};
}

QUrl objectUrlForPath(const RemoteConnection& connection,
                      const QString& localPath) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty()) return {};
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty()) return {};
    const QUrl url(
        readIndex(QDir(cachePath).filePath(QStringLiteral("index.json")))
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
    const QJsonObject index =
        readIndex(QDir(cachePath).filePath(QStringLiteral("index.json")));
    const QString scope = index.value(QStringLiteral("scope")).toString();
    if (signedConnection.options.value(QStringLiteral("region")).isEmpty() &&
        !scope.isEmpty())
        signedConnection.options.insert(QStringLiteral("region"), scope);

    const RemoteBackend backend = backendFor(signedConnection);
    if (!backend.sign) {
        if (error) *error = QStringLiteral("No protocol backend");
        return false;
    }

    QUrl target = url;
    std::function<QNetworkRequest(const QUrl&, const ObjectRange&)> build;
    switch (signedConnection.type) {
        case LocationType::WebDav:
            build = [&backend](const QUrl& hop, const ObjectRange& range) {
                QNetworkRequest request = makeRequest(hop);
                request.setRawHeader(
                    "Range",
                    QByteArray("bytes=") + QByteArray::number(range.offset) +
                        '-' +
                        QByteArray::number(range.offset + range.length - 1));
                backend.sign(request, "GET", {});
                return request;
            };
            break;
        case LocationType::S3:
        case LocationType::Gcs:
            // Range is deliberately not among the signed headers, so one
            // presigned object URL can multiplex every sample request.
            target = s3PresignedUrl(signedConnection, scope, url,
                                    kStreamExpirySeconds);
            build = [](const QUrl& hop, const ObjectRange& range) {
                QNetworkRequest request = makeRequest(hop);
                request.setRawHeader(
                    "Range",
                    QByteArray("bytes=") + QByteArray::number(range.offset) +
                        '-' +
                        QByteArray::number(range.offset + range.length - 1));
                return request;
            };
            break;
        case LocationType::Folder:
            if (error) *error = QStringLiteral("Not a remote object");
            return false;
    }
    if (!target.isValid()) {
        if (error) *error = QStringLiteral("Unable to sign object URL");
        return false;
    }

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
    return offlineNames(readIndex(QDir(cachePath).filePath("index.json")))
        .contains(relative);
}

QString pinOfflineVideo(const RemoteConnection& connection,
                        const QString& localPath, bool pinned) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty())
        return QStringLiteral("This recording is not on a server.");
    const QString relative = relativeInCache(cachePath, localPath);
    if (relative.isEmpty())
        return QStringLiteral("This recording is not in the cache.");

    const QString indexPath = QDir(cachePath).filePath("index.json");
    QJsonObject index = readIndex(indexPath);
    if (!index.value(QStringLiteral("entries")).toObject().contains(relative))
        return QStringLiteral("The server has not listed this recording.");

    QSet<QString> names = offlineNames(index);
    if (pinned == names.contains(relative)) return {};
    if (pinned)
        names.insert(relative);
    else
        names.remove(relative);
    index.insert(QStringLiteral("offline"), sortedArray(names));
    if (!writeIndex(indexPath, index))
        return QStringLiteral("Unable to write the cache index.");
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

    const QJsonObject index = readIndex(QDir(cachePath).filePath("index.json"));
    const QJsonObject entry = index.value(QStringLiteral("entries"))
                                  .toObject()
                                  .value(relative)
                                  .toObject();
    const QUrl url(entry.value(QStringLiteral("url")).toString(),
                   QUrl::StrictMode);
    if (!url.isValid())
        return QStringLiteral("The server has not listed this recording.");

    QUrl target = url;
    RequestFactory build;
    switch (connection.type) {
        case LocationType::WebDav: {
            const RemoteBackend backend = makeWebDavBackend(connection);
            build = [backend](const QUrl& hop) {
                QNetworkRequest request = makeRequest(hop);
                backend.sign(request, "GET", {});
                return request;
            };
            break;
        }
        case LocationType::S3:
        case LocationType::Gcs:
            // A header signature is scoped to the bucket's region, which only
            // a listing discovers. The sync wrote that scope down, so signing
            // into the URL reproduces it here without a second round trip —
            // and it is the same URL the player would have streamed.
            target = s3PresignedUrl(
                connection, index.value(QStringLiteral("scope")).toString(),
                url, kStreamExpirySeconds);
            build = [](const QUrl& hop) { return makeRequest(hop); };
            break;
        case LocationType::Folder:
            return QStringLiteral("This recording is not on a server.");
    }

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
    const QString indexPath = QDir(result.cachePath).filePath("index.json");
    const QJsonObject oldIndex = readIndex(indexPath);
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

    RemoteBackend backend;
    switch (connection.type) {
        case LocationType::WebDav:
            backend = makeWebDavBackend(connection);
            break;
        case LocationType::S3:
        case LocationType::Gcs:
            // Google Cloud Storage speaks the S3 API — SigV4 with an HMAC
            // interoperability key, and ListObjectsV2 paging. One backend.
            backend = makeS3Backend(connection);
            break;
        case LocationType::Folder: break;
    }

    QNetworkAccessManager manager;
    QVector<RemoteObject> objects;
    QString listingError;
    qCInfo(lcIo).noquote() << "sync" << locationTypeKey(connection.type)
                           << connection.name << connection.target;
    if (!backend.list(manager, &objects, &listingError)) {
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
        const QJsonObject old =
            oldEntries.value(object.relativePath).toObject();
        const QString localPath =
            QDir(result.cachePath).filePath(object.relativePath);
        if (!QDir().mkpath(QFileInfo(localPath).absolutePath())) {
            result.error = QStringLiteral("Unable to create a cache folder");
            result.status = result.error;
            return result;
        }

        // Video streams instead of being downloaded. One onboard recording
        // runs 5–30 GB against telemetry's kilobytes, so mirroring it would
        // fill the disk to hold something mpv reads perfectly well over HTTP
        // range requests. What lands on disk is a zero-byte stand-in, which
        // keeps discovery, pairing, pins and recents working off a local path
        // exactly as they do for a file that really is here.
        if (isSidecarPath(object.relativePath)) {
            if (!isPortableTelemetryCompanion(object.relativePath)) {
                qCInfo(lcIo).noquote()
                    << "cache skip leftover sidecar" << object.relativePath;
                continue;
            }
            const bool unchanged =
                QFileInfo::exists(localPath) && !object.etag.isEmpty() &&
                object.etag == old.value(QStringLiteral("etag")).toString();
            if (!unchanged) {
                qCInfo(lcIo).noquote()
                    << "cache miss sidecar" << object.relativePath
                    << omatrack::formatBytes(object.size);
                const RequestFactory build = [&backend](const QUrl& url) {
                    QNetworkRequest request = makeRequest(url);
                    backend.sign(request, "GET", {});
                    return request;
                };
                const DownloadResult download =
                    downloadToFile(object.url, build, localPath, {}, cancel);
                if (ioCancelled(cancel)) {
                    result.error = QStringLiteral("Cancelled");
                    result.status = result.error;
                    return result;
                }
                if (download.status != 200 || !download.error.isEmpty()) {
                    qCInfo(lcIo).noquote()
                        << "sidecar download failed" << object.relativePath
                        << (download.error.isEmpty()
                                ? QStringLiteral("HTTP %1").arg(download.status)
                                : download.error);
                    continue;
                }
                result.downloadedBytes += download.bytes;
                qCInfo(lcIo).noquote()
                    << "write download" << object.relativePath
                    << omatrack::formatBytes(download.bytes);
            } else {
                qCInfo(lcIo).noquote()
                    << "cache hit sidecar" << object.relativePath;
            }
            newEntries.insert(
                object.relativePath,
                QJsonObject{{QStringLiteral("etag"), object.etag},
                            {QStringLiteral("modified"), object.modified},
                            {QStringLiteral("size"), object.size},
                            {QStringLiteral("url"),
                             object.url.toString(QUrl::FullyEncoded)}});
            continue;
        }
        if (isVideoFile(object.relativePath)) {
            // A recording pinned for offline use is kept exactly as it is,
            // provided the server still offers the same one. The sync never
            // fetches it: that transfer takes long enough to belong in a job
            // with a progress bar rather than inside a library scan.
            const bool keep =
                pinned.contains(object.relativePath) &&
                QFileInfo(localPath).size() > 0 && !object.etag.isEmpty() &&
                object.etag == old.value(QStringLiteral("etag")).toString() &&
                object.modified ==
                    old.value(QStringLiteral("modified")).toString();
            if (keep) {
                qCInfo(lcIo).noquote()
                    << "cache keep offline" << object.relativePath
                    << omatrack::formatBytes(QFileInfo(localPath).size());
            } else {
                qCInfo(lcIo).noquote()
                    << "cache stub video" << object.relativePath
                    << omatrack::formatBytes(object.size);
            }
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

        const bool unchanged =
            QFileInfo::exists(localPath) && !object.etag.isEmpty() &&
            object.etag == old.value(QStringLiteral("etag")).toString() &&
            object.modified == old.value(QStringLiteral("modified")).toString();
        if (!unchanged) {
            qCInfo(lcIo).noquote() << "cache miss object" << object.relativePath
                                   << omatrack::formatBytes(object.size);
            const RequestFactory build = [&backend](const QUrl& url) {
                QNetworkRequest request = makeRequest(url);
                backend.sign(request, "GET", {});
                return request;
            };
            const DownloadResult download =
                downloadToFile(object.url, build, localPath, {}, cancel);
            if (download.status != 200 || !download.error.isEmpty()) {
                result.error = download.error.isEmpty()
                                   ? QStringLiteral("Download returned HTTP %1")
                                         .arg(download.status)
                                   : download.error;
                result.status = result.error;
                return result;
            }
            result.downloadedBytes += download.bytes;
            qCInfo(lcIo).noquote() << "write download" << object.relativePath
                                   << omatrack::formatBytes(download.bytes);
        } else {
            qCInfo(lcIo).noquote() << "cache hit object" << object.relativePath;
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

    // A pin for something the server dropped is a pin on nothing; everything
    // else survives a re-sync, which is what makes "keep this for the flight"
    // mean it.
    QSet<QString> keptPins;
    for (const QString& name : pinned)
        if (newEntries.contains(name)) keptPins.insert(name);

    const QJsonObject index{
        {QStringLiteral("version"), 1},
        {QStringLiteral("url"), connection.target},
        {QStringLiteral("offline"), sortedArray(keptPins)},
        // Recorded rather than rediscovered so that presigning a stream URL
        // is arithmetic on the UI thread and not a network round trip.
        {QStringLiteral("scope"), backend.scope ? backend.scope() : QString()},
        {QStringLiteral("entries"), newEntries},
        {QStringLiteral("syncedAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!writeIndex(indexPath, index)) {
        result.error = QStringLiteral("Unable to write the cache index");
        result.status = result.error;
        return result;
    }
    qCInfo(lcIo).noquote() << "write cache-index"
                           << omatrack::displayPath(indexPath)
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
