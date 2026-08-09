#include "TrackMetadata.h"

#include "YamlConfig.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSet>

#include <cmath>

namespace omatrack::track_metadata {
namespace {
QString canonicalDirectory(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) return {};
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QStringList directoryLineage(const QString& directoryPath) {
    QStringList result;
    QDir directory(directoryPath);
    for (;;) {
        const QString current = directory.absolutePath();
        result.prepend(current);
        if (!directory.cdUp() || directory.absolutePath() == current) break;
    }
    return result;
}
}  // namespace

QString filePath(const QString& directoryPath) {
    const QString canonical = canonicalDirectory(directoryPath);
    return canonical.isEmpty()
               ? QString()
               : QDir(canonical).filePath(QStringLiteral("TRACK.yml"));
}

QStringList hierarchyPaths(const QString& directoryPath, bool includeTarget) {
    const QString canonical = canonicalDirectory(directoryPath);
    if (canonical.isEmpty()) return {};

    QStringList result;
    const QStringList lineage = directoryLineage(canonical);
    for (const QString& directory : lineage) {
        if (!includeTarget && directory == canonical) continue;
        const QString path =
            QDir(directory).filePath(QStringLiteral("TRACK.yml"));
        if (QFileInfo::exists(path)) result.append(path);
    }
    return result;
}

void merge(QVariantMap* base, const QVariantMap& overlay) {
    if (!base) return;
    for (auto it = overlay.cbegin(); it != overlay.cend(); ++it) {
        if (it.value().typeId() == QMetaType::QVariantMap) {
            QVariantMap nested = base->value(it.key()).toMap();
            merge(&nested, it.value().toMap());
            if (nested.isEmpty())
                base->remove(it.key());
            else
                base->insert(it.key(), nested);
            continue;
        }
        if (it.value().typeId() == QMetaType::QString &&
            it.value().toString().trimmed().isEmpty())
            continue;
        base->insert(it.key(), it.value());
    }
}

QString normalizedDriverMappingKey(const QVariant& value) {
    const QString text = value.toString().trimmed();
    if (text == QStringLiteral("*")) return text;

    bool ok = false;
    const double id = text.toDouble(&ok);
    return ok && std::isfinite(id) && id > 0.0 ? QString::number(id, 'g', 15)
                                               : QString();
}

QString driverNameForId(const QVariantMap& metadata, const QVariant& driverId) {
    const QString id = normalizedDriverMappingKey(driverId);
    if (id.isEmpty() || id == QStringLiteral("*")) return {};

    const QVariantMap mappings = metadata.value(QStringLiteral("driver"))
                                     .toMap()
                                     .value(QStringLiteral("mappings"))
                                     .toMap();
    QVariantMap normalizedMappings;
    for (auto it = mappings.cbegin(); it != mappings.cend(); ++it) {
        const QString key = normalizedDriverMappingKey(it.key());
        const QString name = it.value().toString().trimmed();
        if (!key.isEmpty() && !name.isEmpty())
            normalizedMappings.insert(key, name);
    }
    const QString exact = normalizedMappings.value(id).toString();
    return exact.isEmpty()
               ? normalizedMappings.value(QStringLiteral("*")).toString()
               : exact;
}

QVariantMap readHierarchy(const QString& directoryPath, bool includeTarget,
                          QStringList* paths) {
    const QStringList documents = hierarchyPaths(directoryPath, includeTarget);
    if (paths) *paths = documents;
    QVariantMap result;
    for (const QString& path : documents)
        merge(&result, YamlConfig::readDocument(path));
    return result;
}

bool update(const QString& directoryPath, const QVariantMap& ownedMetadata,
            QString* errorString) {
    if (errorString) errorString->clear();
    const QString target = filePath(directoryPath);
    if (target.isEmpty()) {
        if (errorString)
            *errorString = QStringLiteral("Metadata folder does not exist");
        return false;
    }

    QVariantMap document;
    if (QFileInfo::exists(target)) {
        QString readError;
        document = YamlConfig::readDocument(target, &readError);
        if (!readError.isEmpty()) {
            if (errorString) *errorString = readError;
            return false;
        }
    }

    static const QSet<QString> ownedKeys{
        QStringLiteral("schema"),  QStringLiteral("driver"),
        QStringLiteral("car"),     QStringLiteral("event"),
        QStringLiteral("series"),  QStringLiteral("track"),
        QStringLiteral("channels")};
    for (const QString& key : ownedKeys) document.remove(key);
    for (auto it = ownedMetadata.cbegin(); it != ownedMetadata.cend(); ++it)
        if (ownedKeys.contains(it.key())) document.insert(it.key(), it.value());

    return YamlConfig::writeDocument(target, document, errorString);
}

}  // namespace omatrack::track_metadata
