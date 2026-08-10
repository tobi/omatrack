#include "SessionMetadataCache.h"
#include "omatrack_bridge.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

#include <array>
#include <utility>

namespace {
QMutex s_cacheMutex;
}

SessionMetadataCache::SessionMetadataCache(QString path)
    : path_(std::move(path)) {
    const QMutexLocker locker(&s_cacheMutex);
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return;
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != kSchemaVersion) return;
    entries_ = root.value(QStringLiteral("entries")).toObject();
}

QString SessionMetadataCache::fingerprint(const QString& path) {
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath().isEmpty()
                                      ? info.absoluteFilePath()
                                      : info.canonicalFilePath();
    const QByteArray encodedPath = canonicalPath.toUtf8();
    std::array<char, 65> output{};
    if (!omatrack_fingerprint(encodedPath.constData(), output.data(),
                              output.size()))
        return {};
    return QString::fromLatin1(output.data(), 64);
}

SessionMetadataCache::Lookup SessionMetadataCache::lookup(
    const QString& fingerprint) {
    const auto it = entries_.find(fingerprint);
    if (it == entries_.end() || !it->isObject()) return {};
    QJsonObject entry = it->toObject();
    entry.insert(QStringLiteral("lastSeen"),
                 double(QDateTime::currentMSecsSinceEpoch()));
    it.value() = entry;
    dirtyEntries_.insert(fingerprint, entry);
    Lookup result;
    result.found = true;
    result.supported = entry.value(QStringLiteral("supported")).toBool();
    result.metadata = entry.value(QStringLiteral("metadata")).toObject();
    return result;
}

void SessionMetadataCache::store(const QString& fingerprint,
                                 const QString& canonicalPath, bool supported,
                                 const QJsonObject& metadata) {
    if (fingerprint.isEmpty()) return;
    const QJsonObject entry{
        {QStringLiteral("path"), canonicalPath},
        {QStringLiteral("supported"), supported},
        {QStringLiteral("lastSeen"),
         double(QDateTime::currentMSecsSinceEpoch())},
        {QStringLiteral("metadata"), metadata},
    };
    entries_.insert(fingerprint, entry);
    dirtyEntries_.insert(fingerprint, entry);
}

bool SessionMetadataCache::save() {
    const QMutexLocker locker(&s_cacheMutex);
    QJsonObject mergedEntries;
    // Scoped so the read handle is closed before QSaveFile commits below.
    // Windows refuses to replace a file that anyone still has open, so leaving
    // this handle alive would fail every save after the first one.
    {
        QFile existing(path_);
        if (existing.open(QIODevice::ReadOnly)) {
            QJsonParseError error;
            const QJsonDocument document =
                QJsonDocument::fromJson(existing.readAll(), &error);
            if (error.error == QJsonParseError::NoError &&
                document.isObject()) {
                const QJsonObject root = document.object();
                if (root.value(QStringLiteral("version")).toInt() ==
                    kSchemaVersion)
                    mergedEntries =
                        root.value(QStringLiteral("entries")).toObject();
            }
        }
    }
    for (auto it = dirtyEntries_.constBegin(); it != dirtyEntries_.constEnd();
         ++it)
        mergedEntries.insert(it.key(), it.value());

    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kMaxEntryAgeMs;
    for (auto it = mergedEntries.begin(); it != mergedEntries.end();) {
        const qint64 lastSeen =
            qint64(it->toObject().value(QStringLiteral("lastSeen")).toDouble());
        if (lastSeen <= 0 || lastSeen < oldest)
            it = mergedEntries.erase(it);
        else
            ++it;
    }

    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return false;
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QJsonObject root{{QStringLiteral("version"), kSchemaVersion},
                           {QStringLiteral("entries"), mergedEntries}};
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0)
        return false;
    if (!file.commit()) return false;
    entries_ = mergedEntries;
    dirtyEntries_ = {};
    return true;
}
