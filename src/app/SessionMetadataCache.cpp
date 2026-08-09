#include "SessionMetadataCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <utility>

namespace {
constexpr qint64 kFingerprintBytes = 1024LL * 1024;
constexpr qsizetype kReadBufferBytes = qsizetype(64) * 1024;

bool addFileFingerprint(QCryptographicHash& hash, const QString& path) {
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath().isEmpty()
                                      ? info.absoluteFilePath()
                                      : info.canonicalFilePath();
    QFile file(canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) return false;

    hash.addData(canonicalPath.toUtf8());
    hash.addData(QByteArrayView("\0", 1));
    hash.addData(QByteArray::number(info.size()));
    hash.addData(QByteArrayView("\0", 1));

    std::array<char, size_t(kReadBufferBytes)> buffer{};
    qint64 remaining = kFingerprintBytes;
    while (remaining > 0) {
        const qint64 count = file.read(
            buffer.data(), std::min(remaining, qint64(buffer.size())));
        if (count <= 0) break;
        hash.addData(QByteArrayView(buffer.data(), count));
        remaining -= count;
    }
    return true;
}
}  // namespace

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

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("omatrack-session-index-v2\0", 26));
    hash.addData(QByteArrayView("primary\0", 8));
    if (!addFileFingerprint(hash, canonicalPath)) return {};

    if (info.suffix().compare(QStringLiteral("ld"), Qt::CaseInsensitive) == 0) {
        hash.addData(QByteArrayView("sidecar\0", 8));
        const QFileInfo canonicalInfo(canonicalPath);
        const QString sidecarPath = canonicalInfo.dir().filePath(
            canonicalInfo.completeBaseName() + QStringLiteral(".ldx"));
        if (!addFileFingerprint(hash, sidecarPath))
            hash.addData(QByteArrayView("missing\0", 8));
    }
    return QString::fromLatin1(hash.result().toHex());
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
