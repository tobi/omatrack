// AWS Signature Version 4, the authentication scheme S3 speaks.
//
// Google Cloud Storage speaks it too: its XML API accepts SigV4 with an HMAC
// interoperability key, region "auto", and service "s3". So one signer covers
// both of Omatrack's object-storage backends, and the only thing that varies
// between them is the Scope.
//
// Nothing here touches the network or the clock. `when` is always a parameter,
// which is what lets the whole algorithm be pinned to published test vectors.
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

class QDateTime;
class QUrl;

namespace omatrack::sigv4 {

/// An access key pair. Empty means "send the request unsigned", which is how
/// a public bucket is read.
struct Credentials {
    QString accessKeyId;
    QString secretAccessKey;

    bool isEmpty() const {
        return accessKeyId.isEmpty() || secretAccessKey.isEmpty();
    }
};

/// The region/service pair a signature is scoped to. A signature is only
/// valid for the exact scope it names, which is why the wrong region fails
/// outright rather than redirecting.
struct Scope {
    QString region;
    QString service = QStringLiteral("s3");
};

/// Header names in lowercase, mapped to their values. QMap keeps them in the
/// byte order the canonical form requires.
using HeaderMap = QMap<QByteArray, QByteArray>;

/// Hex SHA-256 of the empty string: the payload hash of every GET and HEAD.
extern const QByteArray kEmptyPayload;

/// The literal S3 requires in place of a payload hash when a signature cannot
/// commit to a body — which is every presigned URL, since whoever opens it
/// was never told what the body would be.
extern const QByteArray kUnsignedPayload;

QByteArray payloadHash(const QByteArray& payload);

/// RFC 3986 percent-encoding, the strict form canonicalization requires: only
/// A-Za-z0-9-_.~ survive and everything else becomes uppercase %XX. Qt's own
/// encoders leave the sub-delimiters (!$&'()*+,;=) intact, which is a
/// different alphabet and therefore a different signature.
///
/// Path segments keep their separators (`keepSlashes`); query components do
/// not, because a `/` inside a parameter value is data.
QByteArray uriEncode(const QString& value, bool keepSlashes);

/// The Authorization header value for a request, given every header that
/// should be signed. Signing an unnecessary header is not harmless: GCS
/// strips some on the way in, and the signature then fails to verify against
/// what it actually received.
QByteArray authorizationHeader(const Credentials& credentials,
                               const Scope& scope, const QByteArray& method,
                               const QUrl& url, const HeaderMap& headers,
                               const QByteArray& payloadSha256,
                               const QDateTime& when);

/// The headers a signed Omatrack request carries. Signs the minimum set —
/// host, x-amz-content-sha256, x-amz-date — for the reason above.
HeaderMap signedHeaders(const Credentials& credentials, const Scope& scope,
                        const QByteArray& method, const QUrl& url,
                        const QByteArray& payloadSha256, const QDateTime& when);

/// A URL that carries its own credential and expiry in the query string.
///
/// This is how remote video reaches the player: mpv is handed an ordinary
/// https URL, seeks it with byte ranges, and never holds the secret key.
QUrl presign(const Credentials& credentials, const Scope& scope,
             const QByteArray& method, const QUrl& url, int expirySeconds,
             const QDateTime& when);

}  // namespace omatrack::sigv4
