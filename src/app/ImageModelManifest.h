// origin: PUBLIC — fixed public model catalog and compatibility contract.
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>

#include <array>
#include <optional>

namespace omatrack::image_model {
inline constexpr char Repository[] = "tobil/omatrack-telemetry-reader";
inline constexpr char ManifestSchema[] = "omatrack-gauge-model-manifest-v1";
inline constexpr char ModelFilename[] = "gauge-reader.onnx";
inline constexpr qint64 MaximumModelBytes = 128 * 1024 * 1024;
inline constexpr qint64 MaximumCatalogBytes = 2 * 1024 * 1024;
inline constexpr qint64 MaximumManifestBytes = 64 * 1024;

struct Manifest {
    QString version;
    QString minimumAppVersion;
    QByteArray sha256;
    qint64 sizeBytes = 0;
    QMap<QString, QString> metadata;
};

std::optional<std::array<int, 3>> versionParts(const QString& version);
std::optional<Manifest> parseManifest(const QByteArray& bytes,
                                      const QString& appVersion,
                                      QString* error = nullptr);
std::optional<QString> parseRevision(const QByteArray& bytes,
                                     QString* error = nullptr);
bool validSha256(const QString& digest);
bool allowedDownloadUrl(const QUrl& url);
QUrl revisionUrl();
QUrl manifestUrl(const QString& revision);
QUrl modelUrl(const QString& revision);
}  // namespace omatrack::image_model
