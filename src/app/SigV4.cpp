// AWS Signature Version 4: canonical request → string to sign → signature.

#include "SigV4.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace omatrack::sigv4 {
namespace {

constexpr auto kAlgorithm = "AWS4-HMAC-SHA256";
constexpr auto kTerminator = "aws4_request";
constexpr auto kAmzDateFormat = "yyyyMMdd'T'hhmmss'Z'";

QByteArray hmac(const QByteArray& key, const QByteArray& message) {
    return QMessageAuthenticationCode::hash(message, key,
                                            QCryptographicHash::Sha256);
}

QByteArray sha256Hex(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

/// One query parameter, already in its canonical encoding. Kept encoded so
/// presign() can add its own parameters without a decode/re-encode round trip
/// that would have to be exactly reversible to be safe.
struct QueryPair {
    QByteArray name;
    QByteArray value;
};

bool operator<(const QueryPair& lhs, const QueryPair& rhs) {
    if (lhs.name != rhs.name) return lhs.name < rhs.name;
    return lhs.value < rhs.value;
}

QList<QueryPair> encodedPairs(const QUrl& url) {
    QList<QueryPair> pairs;
    const QUrlQuery query(url);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    pairs.reserve(items.size());
    for (const auto& [name, value] : items)
        pairs.append({uriEncode(name, false), uriEncode(value, false)});
    return pairs;
}

QByteArray canonicalQuery(QList<QueryPair> pairs) {
    std::sort(pairs.begin(), pairs.end());
    QByteArray canonical;
    for (const QueryPair& pair : std::as_const(pairs)) {
        if (!canonical.isEmpty()) canonical.append('&');
        canonical.append(pair.name).append('=').append(pair.value);
    }
    return canonical;
}

QByteArray canonicalPath(const QUrl& url) {
    const QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty()) return "/";
    return uriEncode(path, true);
}

/// host:port, with the port omitted when it is the scheme's default — the
/// signature has to name the host exactly as the Host header will.
QByteArray hostHeader(const QUrl& url) {
    QByteArray host = url.host().toUtf8();
    const int port = url.port(-1);
    const int standard = url.scheme() == QStringLiteral("http") ? 80 : 443;
    if (port > 0 && port != standard)
        host.append(':').append(QByteArray::number(port));
    return host;
}

QByteArray credentialScope(const Scope& scope, const QString& datestamp) {
    return datestamp.toUtf8() + '/' + scope.region.toUtf8() + '/' +
           scope.service.toUtf8() + '/' + kTerminator;
}

QByteArray signingKey(const QString& secret, const Scope& scope,
                      const QString& datestamp) {
    QByteArray key = hmac("AWS4" + secret.toUtf8(), datestamp.toUtf8());
    key = hmac(key, scope.region.toUtf8());
    key = hmac(key, scope.service.toUtf8());
    return hmac(key, kTerminator);
}

QByteArray joinSignedHeaders(const HeaderMap& headers) {
    QByteArray names;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        if (!names.isEmpty()) names.append(';');
        names.append(it.key());
    }
    return names;
}

/// Everything both signing forms share. The two forms differ only in where
/// the result is carried — a header or a query parameter — so the arithmetic
/// lives here once.
QByteArray signature(const Credentials& credentials, const Scope& scope,
                     const QByteArray& method, const QUrl& url,
                     const QList<QueryPair>& query, const HeaderMap& headers,
                     const QByteArray& payloadSha256, const QDateTime& when) {
    QByteArray canonicalHeaders;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        canonicalHeaders.append(it.key())
            .append(':')
            .append(it.value().trimmed())
            .append('\n');

    const QByteArray canonicalRequest =
        method + '\n' + canonicalPath(url) + '\n' + canonicalQuery(query) +
        '\n' + canonicalHeaders + '\n' + joinSignedHeaders(headers) + '\n' +
        payloadSha256;

    const QDateTime utc = when.toUTC();
    const QString datestamp = utc.toString(QStringLiteral("yyyyMMdd"));
    const QByteArray stringToSign =
        QByteArray(kAlgorithm) + '\n' +
        utc.toString(QString::fromLatin1(kAmzDateFormat)).toUtf8() + '\n' +
        credentialScope(scope, datestamp) + '\n' + sha256Hex(canonicalRequest);

    return hmac(signingKey(credentials.secretAccessKey, scope, datestamp),
                stringToSign)
        .toHex();
}

}  // namespace

const QByteArray kEmptyPayload = sha256Hex({});
const QByteArray kUnsignedPayload = "UNSIGNED-PAYLOAD";

QByteArray payloadHash(const QByteArray& payload) { return sha256Hex(payload); }

QByteArray uriEncode(const QString& value, bool keepSlashes) {
    static constexpr auto kHex = "0123456789ABCDEF";
    QByteArray encoded;
    for (const char byte : value.toUtf8()) {
        const auto raw = static_cast<unsigned char>(byte);
        const bool unreserved = (raw >= 'A' && raw <= 'Z') ||
                                (raw >= 'a' && raw <= 'z') ||
                                (raw >= '0' && raw <= '9') || raw == '-' ||
                                raw == '_' || raw == '.' || raw == '~';
        if (unreserved || (raw == '/' && keepSlashes)) {
            encoded.append(byte);
        } else {
            encoded.append('%').append(kHex[raw >> 4]).append(kHex[raw & 0x0F]);
        }
    }
    return encoded;
}

QByteArray authorizationHeader(const Credentials& credentials,
                               const Scope& scope, const QByteArray& method,
                               const QUrl& url, const HeaderMap& headers,
                               const QByteArray& payloadSha256,
                               const QDateTime& when) {
    const QByteArray signed_ =
        signature(credentials, scope, method, url, encodedPairs(url), headers,
                  payloadSha256, when);
    const QString datestamp = when.toUTC().toString(QStringLiteral("yyyyMMdd"));
    return QByteArray(kAlgorithm) +
           " Credential=" + credentials.accessKeyId.toUtf8() + '/' +
           credentialScope(scope, datestamp) +
           ", SignedHeaders=" + joinSignedHeaders(headers) +
           ", Signature=" + signed_;
}

HeaderMap signedHeaders(const Credentials& credentials, const Scope& scope,
                        const QByteArray& method, const QUrl& url,
                        const QByteArray& payloadSha256,
                        const QDateTime& when) {
    HeaderMap headers;
    headers["host"] = hostHeader(url);
    headers["x-amz-content-sha256"] = payloadSha256;
    headers["x-amz-date"] =
        when.toUTC().toString(QString::fromLatin1(kAmzDateFormat)).toUtf8();
    headers["authorization"] = authorizationHeader(
        credentials, scope, method, url, headers, payloadSha256, when);
    return headers;
}

QUrl presign(const Credentials& credentials, const Scope& scope,
             const QByteArray& method, const QUrl& url, int expirySeconds,
             const QDateTime& when) {
    const QDateTime utc = when.toUTC();
    const QByteArray amzDate =
        utc.toString(QString::fromLatin1(kAmzDateFormat)).toUtf8();
    const QString datestamp = utc.toString(QStringLiteral("yyyyMMdd"));

    HeaderMap headers;
    headers["host"] = hostHeader(url);

    const QString credential =
        credentials.accessKeyId + QLatin1Char('/') +
        QString::fromUtf8(credentialScope(scope, datestamp));

    QList<QueryPair> query = encodedPairs(url);
    query.append(QueryPair{"X-Amz-Algorithm", kAlgorithm});
    query.append(QueryPair{"X-Amz-Credential", uriEncode(credential, false)});
    query.append(QueryPair{"X-Amz-Date", amzDate});
    query.append(QueryPair{"X-Amz-Expires", QByteArray::number(expirySeconds)});
    query.append(QueryPair{"X-Amz-SignedHeaders", joinSignedHeaders(headers)});

    query.append(QueryPair{"X-Amz-Signature",
                           signature(credentials, scope, method, url, query,
                                     headers, kUnsignedPayload, when)});

    QUrl presigned = url;
    // The query is already in canonical encoding, and StrictMode is what keeps
    // Qt from touching it — the %2F in X-Amz-Credential is part of the signed
    // bytes, so re-encoding or decoding it invalidates the signature.
    presigned.setQuery(QString::fromLatin1(canonicalQuery(query)),
                       QUrl::StrictMode);
    return presigned;
}

}  // namespace omatrack::sigv4
