// origin: PUBLIC — no configurable repository or manifest/model URL.
#include "ImageModelManifest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace omatrack::image_model {
namespace {
void fail(QString* error, const char* message) {
    if (error) *error = QString::fromLatin1(message);
}
bool revisionValid(const QString& revision) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A[0-9a-f]{40}\\z"));
    return pattern.match(revision).hasMatch();
}
QUrl atRevision(const QString& revision, const QString& filename) {
    if (!revisionValid(revision)) return {};
    return QUrl(QStringLiteral("https://huggingface.co/%1/resolve/%2/%3")
                    .arg(QLatin1String(Repository), revision, filename));
}
}  // namespace

bool validSha256(const QString& digest) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return pattern.match(digest).hasMatch() &&
           digest != QString(64, QLatin1Char('0'));
}
std::optional<std::array<int, 3>> versionParts(const QString& version) {
    static const QRegularExpression pattern(
        QStringLiteral("\\A(0|[1-9][0-9]{0,8})\\.(0|[1-9][0-9]{0,8})\\.(0|[1-9]"
                       "[0-9]{0,8})\\z"));
    const auto match = pattern.match(version);
    if (!match.hasMatch()) return {};
    return std::array<int, 3>{{match.captured(1).toInt(),
                               match.captured(2).toInt(),
                               match.captured(3).toInt()}};
}
std::optional<Manifest> parseManifest(const QByteArray& bytes,
                                      const QString& appVersion,
                                      QString* error) {
    if (bytes.isEmpty() || bytes.size() > MaximumManifestBytes) {
        fail(error, "Model manifest is missing or too large");
        return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, "Invalid model manifest JSON");
        return {};
    }
    const auto object = document.object();
    const QSet<QString> keys{
        QStringLiteral("schema"),          QStringLiteral("version"),
        QStringLiteral("filename"),        QStringLiteral("sha256"),
        QStringLiteral("size_bytes"),      QStringLiteral("reader_contract"),
        QStringLiteral("min_app_version"), QStringLiteral("model_metadata")};
    if (object.size() != keys.size()) {
        fail(error, "Unexpected model manifest fields");
        return {};
    }
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!keys.contains(it.key())) {
            fail(error, "Unexpected model manifest field");
            return {};
        }
    if (object.value(QStringLiteral("schema")).toString() !=
            QLatin1String(ManifestSchema) ||
        object.value(QStringLiteral("filename")).toString() !=
            QLatin1String(ModelFilename) ||
        object.value(QStringLiteral("reader_contract")).toString() !=
            QStringLiteral("omatrack-crop-count-v1")) {
        fail(error, "Incompatible model schema, filename or reader contract");
        return {};
    }
    Manifest result;
    result.version = object.value(QStringLiteral("version")).toString();
    result.minimumAppVersion =
        object.value(QStringLiteral("min_app_version")).toString();
    const auto version = versionParts(result.version),
               minimum = versionParts(result.minimumAppVersion),
               app = versionParts(appVersion);
    if (!version || !minimum || !app) {
        fail(error, "Invalid model/application version");
        return {};
    }
    if (*app < *minimum) {
        fail(error, "This model requires a newer Omatrack version");
        return {};
    }
    const auto hash = object.value(QStringLiteral("sha256")).toString();
    if (!validSha256(hash)) {
        fail(error, "Invalid model SHA-256");
        return {};
    }
    result.sha256 = hash.toLatin1();
    const auto size = object.value(QStringLiteral("size_bytes"));
    const double number = size.toDouble(-1);
    if (!size.isDouble() || !std::isfinite(number) || number < 1 ||
        number > MaximumModelBytes || std::floor(number) != number) {
        fail(error, "Invalid or excessive model size");
        return {};
    }
    result.sizeBytes = qint64(number);
    const auto metadata = object.value(QStringLiteral("model_metadata"));
    if (!metadata.isObject()) {
        fail(error, "Missing model compatibility metadata");
        return {};
    }
    const QMap<QString, QString> semantics{
        {QStringLiteral("omatrack.contract"),
         QStringLiteral("omatrack-crop-count-v1")},
        {QStringLiteral("omatrack.preprocessing"),
         QStringLiteral("pillow-rgb-bilinear-22bit-crop-count-v1")},
        {QStringLiteral("omatrack.layout"),
         QStringLiteral("tds_aim_orange-1920x1080")},
        {QStringLiteral("omatrack.decoder"),
         QStringLiteral("count-argmax-plus-one-ctc-prefix-beam-10-per-length")},
        {QStringLiteral("omatrack.fields"),
         QStringLiteral("gear,stint_lap,brake_fill_pct,throttle_fill_pct")}};
    const auto entries = metadata.toObject();
    if (entries.size() != semantics.size() + 1) {
        fail(error, "Unexpected model metadata fields");
        return {};
    }
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (!it.value().isString()) {
            fail(error, "Model metadata must be strings");
            return {};
        }
        result.metadata.insert(it.key(), it.value().toString());
    }
    for (auto it = semantics.begin(); it != semantics.end(); ++it)
        if (result.metadata.value(it.key()) != it.value()) {
            fail(error,
                 "Incompatible layout, preprocessing, fields or decoder");
            return {};
        }
    if (!validSha256(result.metadata.value(
            QStringLiteral("omatrack.checkpoint_sha256")))) {
        fail(error, "Missing or invalid model checkpoint provenance");
        return {};
    }
    return result;
}
std::optional<QString> parseRevision(const QByteArray& bytes, QString* error) {
    if (bytes.isEmpty() || bytes.size() > MaximumCatalogBytes) {
        fail(error, "Model catalog is missing or too large");
        return {};
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(error, "Invalid Hugging Face catalog JSON");
        return {};
    }
    const auto object = doc.object();
    const auto revision = object.value(QStringLiteral("sha")).toString();
    if (object.value(QStringLiteral("id")).toString() !=
            QLatin1String(Repository) ||
        !object.value(QStringLiteral("private")).isBool() ||
        object.value(QStringLiteral("private")).toBool() ||
        !revisionValid(revision)) {
        fail(error, "Unexpected public repository or immutable revision");
        return {};
    }
    return revision;
}
bool allowedDownloadUrl(const QUrl& url) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
        url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) ||
        url.hasFragment() || (url.port(-1) != -1 && url.port() != 443))
        return false;
    const QString host = url.host().toLower();
    // No hf.space/user-hosted apps, arbitrary hosts, HTTP downgrade or
    // credentials.
    static const QSet<QString> hosts{
        QStringLiteral("huggingface.co"),
        QStringLiteral("cdn-lfs.huggingface.co"),
        QStringLiteral("cdn-lfs-us-1.huggingface.co"),
        QStringLiteral("cdn-lfs-eu-1.huggingface.co"),
        QStringLiteral("cdn-lfs.hf.co"),
        QStringLiteral("cdn-lfs-us-1.hf.co"),
        QStringLiteral("cdn-lfs-eu-1.hf.co"),
        QStringLiteral("cas-bridge.xethub.hf.co"),
        QStringLiteral("us.aws.cdn.hf.co"),
        QStringLiteral("eu.aws.cdn.hf.co")};
    return hosts.contains(host);
}
QUrl revisionUrl() {
    return QUrl(
        QStringLiteral("https://huggingface.co/api/models/%1/revision/main")
            .arg(QLatin1String(Repository)));
}
QUrl manifestUrl(const QString& revision) {
    return atRevision(revision, QStringLiteral("manifest.json"));
}
QUrl modelUrl(const QString& revision) {
    return atRevision(revision, QLatin1String(ModelFilename));
}
}  // namespace omatrack::image_model
