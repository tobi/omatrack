// AWS Signature Version 4 against published vectors.
//
// Every expected signature below is copied from a public AWS document — the
// aws-sig-v4-test-suite and the S3 signing examples — and each was reproduced
// independently from the specification before being pinned here, so a failure
// means this signer is wrong rather than the vector.
//
// A wrong signature is not a subtle failure in production: S3 answers
// SignatureDoesNotMatch and nothing syncs at all. But it is impossible to
// debug against a live bucket, because the server never says which of the
// dozen canonicalization rules was broken. That is what these vectors are for.

#include "app/SigV4.h"

#include <QDateTime>
#include <QTest>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

using namespace omatrack::sigv4;

namespace {

/// The suite's key pair, used by every vector that is not S3-specific.
const Credentials kExampleKey{QStringLiteral("AKIDEXAMPLE"),
                              QStringLiteral(
                                  "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY")};

/// The key pair from the S3 documentation examples. Note the `/` where the
/// suite key has a `+` — they are different secrets.
const Credentials kS3Key{
    QStringLiteral("AKIAIOSFODNN7EXAMPLE"),
    QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY")};

QDateTime utc(int year, int month, int day, int hour, int minute, int second) {
    return QDateTime(QDate(year, month, day), QTime(hour, minute, second),
                     QTimeZone::UTC);
}

/// Pulls Signature= out of an Authorization header so a failure prints the
/// hex being compared rather than 200 characters of surrounding boilerplate.
QByteArray signatureOf(const QByteArray& authorization) {
    const QByteArray marker("Signature=");
    const int at = authorization.indexOf(marker);
    return at < 0 ? QByteArray() : authorization.mid(at + marker.size());
}

}  // namespace

class SigV4Test : public QObject {
    Q_OBJECT

private slots:
    void encodesTheUnreservedSetOnly();
    void signsTheSuiteVanillaVector();
    void sortsQueryParametersCanonically();
    void signsTheS3GetObjectVector();
    void presignsTheS3UrlVector();
    void presignedUrlNeedsNoCredentialHeader();
    void signsOnlyTheThreeHeadersGcsPreserves();
};

/// SigV4 does not use the same alphabet as any of Qt's encoders: the
/// sub-delimiters Qt leaves alone must be escaped here, and `/` survives in a
/// path but not in a query value.
void SigV4Test::encodesTheUnreservedSetOnly() {
    QCOMPARE(uriEncode(QStringLiteral("quote'~-_."), false),
             QByteArray("quote%27~-_."));
    QCOMPARE(uriEncode(QStringLiteral("a b"), false), QByteArray("a%20b"));
    QCOMPARE(uriEncode(QStringLiteral("key/with+plus"), true),
             QByteArray("key/with%2Bplus"));
    QCOMPARE(uriEncode(QStringLiteral("key/with+plus"), false),
             QByteArray("key%2Fwith%2Bplus"));
    // Telemetry filenames carry driver and track names; a non-ASCII one must
    // encode as UTF-8 bytes, not as anything Latin-1 shaped.
    QCOMPARE(uriEncode(QStringLiteral("ünïcode"), false),
             QByteArray("%C3%BCn%C3%AFcode"));
}

/// aws-sig-v4-test-suite: get-vanilla.
void SigV4Test::signsTheSuiteVanillaVector() {
    HeaderMap headers;
    headers["host"] = "example.amazonaws.com";
    headers["x-amz-date"] = "20150830T123600Z";

    const QByteArray authorization = authorizationHeader(
        kExampleKey, {QStringLiteral("us-east-1"), QStringLiteral("service")},
        "GET", QUrl(QStringLiteral("https://example.amazonaws.com/")), headers,
        kEmptyPayload, utc(2015, 8, 30, 12, 36, 0));

    QCOMPARE(signatureOf(authorization),
             QByteArray("5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aa"
                        "e1d763fbf31"));
    QVERIFY(authorization.startsWith(
        "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/"
        "aws4_request, SignedHeaders=host;x-amz-date, Signature="));
}

/// aws-sig-v4-test-suite: get-vanilla-query-order-key-case. The parameters
/// arrive out of order, so this fails unless the canonical query is sorted —
/// which matters directly, because a listing URL carries prefix,
/// continuation-token and list-type together.
void SigV4Test::sortsQueryParametersCanonically() {
    HeaderMap headers;
    headers["host"] = "example.amazonaws.com";
    headers["x-amz-date"] = "20150830T123600Z";

    const QByteArray authorization = authorizationHeader(
        kExampleKey, {QStringLiteral("us-east-1"), QStringLiteral("service")},
        "GET",
        QUrl(QStringLiteral(
            "https://example.amazonaws.com/?Param2=value2&Param1=value1")),
        headers, kEmptyPayload, utc(2015, 8, 30, 12, 36, 0));

    QCOMPARE(signatureOf(authorization),
             QByteArray("b97d918cfa904a5beff61c982a1b6f458b799221646efd99d3219"
                        "ec94cdf2500"));
}

/// The S3 documentation's single-chunk GET Object example, which unlike the
/// suite vectors signs a real object path and an extra Range header.
void SigV4Test::signsTheS3GetObjectVector() {
    HeaderMap headers;
    headers["host"] = "examplebucket.s3.amazonaws.com";
    headers["range"] = "bytes=0-9";
    headers["x-amz-content-sha256"] = kEmptyPayload;
    headers["x-amz-date"] = "20130524T000000Z";

    const QByteArray authorization = authorizationHeader(
        kS3Key, {QStringLiteral("us-east-1")}, "GET",
        QUrl(QStringLiteral("https://examplebucket.s3.amazonaws.com/test.txt")),
        headers, kEmptyPayload, utc(2013, 5, 24, 0, 0, 0));

    QCOMPARE(signatureOf(authorization),
             QByteArray("f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd9103"
                        "9c6036bdb41"));
}

/// The S3 documentation's presigned-URL example. Presigning is what lets mpv
/// stream a private object, so this vector guards the video path.
void SigV4Test::presignsTheS3UrlVector() {
    const QUrl presigned = presign(
        kS3Key, {QStringLiteral("us-east-1")}, "GET",
        QUrl(QStringLiteral("https://examplebucket.s3.amazonaws.com/test.txt")),
        86400, utc(2013, 5, 24, 0, 0, 0));

    const QUrlQuery query(presigned);
    QCOMPARE(query.queryItemValue(QStringLiteral("X-Amz-Signature")),
             QStringLiteral("aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957"
                            "d157751f604d404"));
    QCOMPARE(query.queryItemValue(QStringLiteral("X-Amz-SignedHeaders")),
             QStringLiteral("host"));

    // The credential's slashes are signed as %2F, so they have to survive
    // Qt's URL handling untouched or the signature no longer matches.
    QVERIFY(presigned.toString(QUrl::FullyEncoded)
                .contains(QStringLiteral(
                    "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-"
                    "east-1%2Fs3%2Faws4_request")));
}

/// Whatever opens a presigned URL — mpv, ffmpeg, a browser — is never given
/// the secret key, which is the whole reason video streams this way.
void SigV4Test::presignedUrlNeedsNoCredentialHeader() {
    const QUrl presigned = presign(
        kS3Key, {QStringLiteral("us-east-1")}, "GET",
        QUrl(QStringLiteral("https://examplebucket.s3.amazonaws.com/lap.mp4")),
        43200, utc(2013, 5, 24, 0, 0, 0));

    const QString text = presigned.toString(QUrl::FullyEncoded);
    QVERIFY(!text.contains(kS3Key.secretAccessKey));
    QVERIFY(text.contains(QStringLiteral("X-Amz-Expires=43200")));
    // A presigned URL cannot commit to a body hash, so S3 requires the
    // literal in its place rather than the empty-string digest.
    QVERIFY(!text.contains(QString::fromLatin1(kEmptyPayload)));
}

/// GCS strips headers on the way in, so signing anything beyond this set
/// produces SignatureDoesNotMatch against a bucket that is perfectly fine.
void SigV4Test::signsOnlyTheThreeHeadersGcsPreserves() {
    const HeaderMap headers = signedHeaders(
        kS3Key, {QStringLiteral("auto")}, "GET",
        QUrl(QStringLiteral("https://storage.googleapis.com/bucket/lap.vbo")),
        kEmptyPayload, utc(2026, 8, 11, 9, 30, 0));

    QCOMPARE(headers.value("host"), QByteArray("storage.googleapis.com"));
    QCOMPARE(headers.value("x-amz-date"), QByteArray("20260811T093000Z"));
    QCOMPARE(headers.value("x-amz-content-sha256"), kEmptyPayload);
    QVERIFY(headers.value("authorization")
                .contains("SignedHeaders=host;x-amz-content-sha256;x-amz-date,"));

    // A non-default port belongs in the signed host, which is what makes the
    // local test server in remote-cache-test reachable at all.
    const HeaderMap onPort = signedHeaders(
        kS3Key, {QStringLiteral("auto")}, "GET",
        QUrl(QStringLiteral("http://127.0.0.1:8993/bucket/lap.vbo")),
        kEmptyPayload, utc(2026, 8, 11, 9, 30, 0));
    QCOMPARE(onPort.value("host"), QByteArray("127.0.0.1:8993"));
}

QTEST_MAIN(SigV4Test)
#include "SigV4Test.moc"
