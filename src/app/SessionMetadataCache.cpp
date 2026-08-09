#include "SessionMetadataCache.h"
#include "omatrack_bridge.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <array>
#include <utility>

SessionMetadataCache::SessionMetadataCache(QString path)
    : path_(std::move(path)) {
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
    entries_.insert(fingerprint,
                    QJsonObject{{QStringLiteral("path"), canonicalPath},
                                {QStringLiteral("supported"), supported},
                                {QStringLiteral("lastSeen"),
                                 double(QDateTime::currentMSecsSinceEpoch())},
                                {QStringLiteral("metadata"), metadata}});
}

bool SessionMetadataCache::save() {
    const qint64 oldest = QDateTime::currentMSecsSinceEpoch() - kMaxEntryAgeMs;
    for (auto it = entries_.begin(); it != entries_.end();) {
        const qint64 lastSeen =
            qint64(it->toObject().value(QStringLiteral("lastSeen")).toDouble());
        if (lastSeen <= 0 || lastSeen < oldest)
            it = entries_.erase(it);
        else
            ++it;
    }

    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return false;
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QJsonObject root{{QStringLiteral("version"), kSchemaVersion},
                           {QStringLiteral("entries"), entries_}};
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0)
        return false;
    return file.commit();
}
