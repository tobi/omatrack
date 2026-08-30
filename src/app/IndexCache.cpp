#include "IndexCache.h"

#include "core/TelemetryEngine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace omatrack {
namespace {

QString cacheRoot() {
    const QString xdg = QString::fromLocal8Bit(qgetenv("XDG_CACHE_HOME"));
    if (!xdg.trimmed().isEmpty())
        return QDir(xdg).filePath(QStringLiteral("omatrack"));
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

QString generationKey() {
    return QString::fromStdString(omatrack::converterGeneration());
}

}  // namespace

FileIdentity fileIdentity(const QString& path) {
    FileIdentity identity;
    identity.generation = generationKey();
    if (path.isEmpty()) return identity;
#ifdef Q_OS_UNIX
    struct stat st = {};
    if (stat(QFile::encodeName(path).constData(), &st) != 0) return identity;
    identity.device = static_cast<quint64>(st.st_dev);
    identity.inode = static_cast<quint64>(st.st_ino);
    identity.size = static_cast<qint64>(st.st_size);
#if defined(__APPLE__)
    identity.mtimeNs =
        static_cast<qint64>(st.st_mtimespec.tv_sec) * 1000000000LL +
        static_cast<qint64>(st.st_mtimespec.tv_nsec);
#else
    identity.mtimeNs = static_cast<qint64>(st.st_mtim.tv_sec) * 1000000000LL +
                       static_cast<qint64>(st.st_mtim.tv_nsec);
#endif
    identity.valid = identity.inode != 0 || identity.size >= 0;
    return identity;
#elif defined(Q_OS_WIN)
    const QString native = QDir::toNativeSeparators(path);
    HANDLE handle =
        CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return identity;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!ok) return identity;
    identity.device = info.dwVolumeSerialNumber;
    identity.inode =
        (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    identity.size =
        (static_cast<qint64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    ULARGE_INTEGER mtime;
    mtime.LowPart = info.ftLastWriteTime.dwLowDateTime;
    mtime.HighPart = info.ftLastWriteTime.dwHighDateTime;
    identity.mtimeNs = static_cast<qint64>(mtime.QuadPart);
    identity.valid = true;
    return identity;
#else
    const QFileInfo info(path);
    if (!info.exists()) return identity;
    identity.size = info.size();
    identity.mtimeNs = info.lastModified().toMSecsSinceEpoch() * 1000000LL;
    identity.valid = true;
    return identity;
#endif
}

QString indexCachePath(const FileIdentity& identity) {
    if (!identity.valid || identity.generation.isEmpty()) return {};
    const QString dir =
        QDir(cacheRoot())
            .filePath(QStringLiteral("index/") + identity.generation);
    return QDir(dir).filePath(QStringLiteral("%1-%2-%3-%4.json")
                                  .arg(identity.device)
                                  .arg(identity.inode)
                                  .arg(identity.size)
                                  .arg(identity.mtimeNs));
}

QJsonObject loadIndexCache(const QString& path) {
    const FileIdentity identity = fileIdentity(path);
    const QString cachePath = indexCachePath(identity);
    if (cachePath.isEmpty()) return {};
    QFile file(cachePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};
    QJsonObject object = document.object();
    if (object.value(QStringLiteral("converterGeneration")).toString() !=
        identity.generation)
        return {};
    const QJsonObject metadata =
        object.value(QStringLiteral("metadata")).toObject();
    return metadata;
}

bool storeIndexCache(const QString& path, const QJsonObject& metadata) {
    if (metadata.isEmpty()) return false;
    const FileIdentity identity = fileIdentity(path);
    const QString cachePath = indexCachePath(identity);
    if (cachePath.isEmpty()) return false;
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QJsonObject object{
        {QStringLiteral("converterGeneration"), identity.generation},
        {QStringLiteral("device"), QString::number(identity.device)},
        {QStringLiteral("inode"), QString::number(identity.inode)},
        {QStringLiteral("size"), identity.size},
        {QStringLiteral("mtimeNs"), identity.mtimeNs},
        {QStringLiteral("metadata"), metadata},
    };
    QSaveFile file(cachePath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

}  // namespace omatrack
