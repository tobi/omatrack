#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace omatrack::track_metadata {

/// Canonical TRACK.yml path for a directory, whether or not it exists yet.
QString filePath(const QString& directoryPath);

/// Existing TRACK.yml files from filesystem root to the selected directory.
QStringList hierarchyPaths(const QString& directoryPath,
                           bool includeTarget = true);

/// Recursively merge every TRACK.yml in root-to-leaf order.
QVariantMap readHierarchy(const QString& directoryPath,
                          bool includeTarget = true,
                          QStringList* paths = nullptr);

/// Replace Omatrack-owned metadata in the directory's TRACK.yml while
/// retaining unrelated top-level content such as `files`.
bool update(const QString& directoryPath, const QVariantMap& ownedMetadata,
            QString* errorString = nullptr);

/// Recursive map merge used by recording overrides after folder inheritance.
void merge(QVariantMap* base, const QVariantMap& overlay);

/// Canonicalize a driver mapping key. Positive numeric codes retain fractional
/// precision; "*" is the fallback key for any detected driver ID.
QString normalizedDriverMappingKey(const QVariant& value);

/// Return the name for a detected driver ID. An exact mapping wins over "*".
QString driverNameForId(const QVariantMap& metadata, const QVariant& driverId);

}  // namespace omatrack::track_metadata
