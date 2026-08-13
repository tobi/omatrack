// Parse/pack contract for the AiM remote extract.
//
// materializeAimExtract range-fetches the aimd track of a streamed MP4. These
// tests pin the ISO-BMFF half of that path — sample-table decoding and the
// packed-layout rewrite — so a malformed moov is rejected here rather than
// after a 30 GB download.

#include "app/AimRemoteIndex.h"
#include "app/LibraryLocation.h"
#include "app/RemoteCache.h"

#include <QtTest>

#include <QDir>
#include <QPair>
#include <QFile>
#include <QTemporaryDir>

#include <limits>

using namespace omatrack;

namespace {

void putBe32(QByteArray* data, qsizetype offset, quint32 value) {
    (*data)[offset] = char((value >> 24U) & 0xffU);
    (*data)[offset + 1] = char((value >> 16U) & 0xffU);
    (*data)[offset + 2] = char((value >> 8U) & 0xffU);
    (*data)[offset + 3] = char(value & 0xffU);
}

void appendBe32(QByteArray* data, quint32 value) {
    const qsizetype offset = data->size();
    data->resize(offset + 4);
    putBe32(data, offset, value);
}

void appendBe64(QByteArray* data, quint64 value) {
    appendBe32(data, quint32(value >> 32U));
    appendBe32(data, quint32(value & 0xffffffffU));
}

quint32 be32(const QByteArray& data, qsizetype offset) {
    const auto* bytes =
        reinterpret_cast<const uchar*>(data.constData() + offset);
    return (quint32(bytes[0]) << 24U) | (quint32(bytes[1]) << 16U) |
           (quint32(bytes[2]) << 8U) | quint32(bytes[3]);
}

QByteArray box(const char* type, const QByteArray& payload) {
    QByteArray out;
    appendBe32(&out, quint32(8 + payload.size()));
    out.append(type, 4);
    out.append(payload);
    return out;
}

QByteArray fullBox(const char* type, const QByteArray& payload) {
    QByteArray body;
    appendBe32(&body, 0);
    body.append(payload);
    return box(type, body);
}

QByteArray ftypBox() {
    QByteArray payload("isom");
    appendBe32(&payload, 0);
    payload.append("isom", 4);
    return box("ftyp", payload);
}

QByteArray aimdSampleEntry() {
    QByteArray payload(8, '\0');
    payload[7] = 1;
    return box("aimd", payload);
}

QByteArray avc1SampleEntry() {
    QByteArray payload(8, '\0');
    payload[7] = 1;
    return box("avc1", payload);
}

QByteArray stsdBox(const QByteArray& entry) {
    QByteArray payload;
    appendBe32(&payload, 1);
    payload.append(entry);
    return fullBox("stsd", payload);
}

QByteArray stszBox(const QVector<quint32>& sizes, quint32 defaultSize = 0) {
    QByteArray payload;
    appendBe32(&payload, defaultSize);
    appendBe32(&payload, quint32(sizes.size()));
    if (defaultSize == 0)
        for (quint32 size : sizes) appendBe32(&payload, size);
    return fullBox("stsz", payload);
}

QByteArray stscBox(const QVector<QPair<quint32, quint32>>& mappings) {
    QByteArray payload;
    appendBe32(&payload, quint32(mappings.size()));
    for (const auto& mapping : mappings) {
        appendBe32(&payload, mapping.first);
        appendBe32(&payload, mapping.second);
        appendBe32(&payload, 1);
    }
    return fullBox("stsc", payload);
}

QByteArray stcoBox(const QVector<quint32>& offsets) {
    QByteArray payload;
    appendBe32(&payload, quint32(offsets.size()));
    for (quint32 offset : offsets) appendBe32(&payload, offset);
    return fullBox("stco", payload);
}

QByteArray co64Box(const QVector<quint64>& offsets) {
    QByteArray payload;
    appendBe32(&payload, quint32(offsets.size()));
    for (quint64 offset : offsets) appendBe64(&payload, offset);
    return fullBox("co64", payload);
}

QByteArray moovWith(const QByteArray& sampleEntry, const QByteArray& tables) {
    return box("moov",
               box("trak", box("mdia",
                               box("minf", box("stbl", sampleEntry + tables)))));
}

QByteArray twoSampleMoov(bool useCo64 = false, quint32 defaultSize = 0) {
    const QVector<quint32> sizes{4, 4};
    const QByteArray tables =
        stszBox(sizes, defaultSize) + stscBox({{1, 2}}) +
        (useCo64 ? co64Box({64}) : stcoBox({64}));
    return moovWith(stsdBox(aimdSampleEntry()), tables);
}

QByteArray boxAt(const QByteArray& data, const char* type) {
    for (qsizetype offset = 0; offset + 8 <= data.size();) {
        const quint32 size = be32(data, offset);
        if (size < 8 || offset + qsizetype(size) > data.size()) break;
        if (data.mid(offset + 4, 4) == type)
            return data.mid(offset, qsizetype(size));
        offset += qsizetype(size);
    }
    return {};
}

}  // namespace

class AimRemoteIndexTest : public QObject {
    Q_OBJECT

private slots:
    void parseRejectsAMissingOutput() {
        QString error;
        QVERIFY(!parseAimdSamples(twoSampleMoov(), 128, nullptr, &error));
        QCOMPARE(error, QStringLiteral("Missing sample output"));
    }

    void parseRejectsATruncatedMoov() {
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY(!parseAimdSamples(QByteArrayLiteral("nope"), 128, &samples,
                                  &error));
        QCOMPARE(error, QStringLiteral("Invalid MP4 moov box"));
    }

    void parseRejectsAMoovWithoutAnAimTrack() {
        const QByteArray tables =
            stszBox({4}) + stscBox({{1, 1}}) + stcoBox({32});
        const QByteArray moov = moovWith(stsdBox(avc1SampleEntry()), tables);
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY(!parseAimdSamples(moov, 128, &samples, &error));
        QCOMPARE(error, QStringLiteral("MP4 has no AiM aimd track"));
    }

    void parseReadsPerSampleSizes() {
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY2(parseAimdSamples(twoSampleMoov(), 72, &samples, &error),
                 qPrintable(error));
        QCOMPARE(samples.size(), 2);
        QCOMPARE(samples[0].offset, qint64(64));
        QCOMPARE(samples[0].size, quint32(4));
        QCOMPARE(samples[1].offset, qint64(68));
        QCOMPARE(samples[1].size, quint32(4));
    }

    void parseReadsADefaultSampleSize() {
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY2(parseAimdSamples(twoSampleMoov(false, 4), 72, &samples, &error),
                 qPrintable(error));
        QCOMPARE(samples.size(), 2);
        QCOMPARE(samples[1].offset, qint64(68));
    }

    void parseReadsSixtyFourBitChunkOffsets() {
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY2(parseAimdSamples(twoSampleMoov(true), 72, &samples, &error),
                 qPrintable(error));
        QCOMPARE(samples.front().offset, qint64(64));
        QCOMPARE(samples.back().offset, qint64(68));
    }

    void parseReadsTwoChunks() {
        const QByteArray tables =
            stszBox({4, 5}) + stscBox({{1, 1}, {2, 1}}) + stcoBox({32, 48});
        const QByteArray moov = moovWith(stsdBox(aimdSampleEntry()), tables);
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY2(parseAimdSamples(moov, 64, &samples, &error), qPrintable(error));
        QCOMPARE(samples.size(), 2);
        QCOMPARE(samples[0].offset, qint64(32));
        QCOMPARE(samples[0].size, quint32(4));
        QCOMPARE(samples[1].offset, qint64(48));
        QCOMPARE(samples[1].size, quint32(5));
    }

    void parseRejectsASamplePastTheFile() {
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY(!parseAimdSamples(twoSampleMoov(), 66, &samples, &error));
        QCOMPARE(error, QStringLiteral("AiM sample points outside MP4"));
    }

    void parseRejectsAnEmptySampleCount() {
        const QByteArray tables =
            stszBox({}) + stscBox({{1, 1}}) + stcoBox({32});
        const QByteArray moov = moovWith(stsdBox(aimdSampleEntry()), tables);
        QVector<AimSampleRange> samples;
        QString error;
        QVERIFY(!parseAimdSamples(moov, 128, &samples, &error));
        QCOMPARE(error, QStringLiteral("Invalid AiM sample count"));
    }

    void packRewritesChunkOffsetsOntoThePackedMdat() {
        const QByteArray ftyp = ftypBox();
        const QByteArray moov = twoSampleMoov();
        QVector<AimSampleRange> samples;
        QVERIFY(parseAimdSamples(moov, 72, &samples, nullptr));
        const QVector<QByteArray> bytes{QByteArrayLiteral("AIM0"),
                                        QByteArrayLiteral("AIM1")};
        QString error;
        const QByteArray packed =
            packAimExtract(ftyp, moov, samples, bytes, &error);
        QVERIFY2(!packed.isEmpty(), qPrintable(error));
        QCOMPARE(boxAt(packed, "ftyp"), ftyp);
        QVERIFY(!boxAt(packed, "mdat").isEmpty());

        const QByteArray packedMoov = boxAt(packed, "moov");
        QVERIFY(!packedMoov.isEmpty());
        QVector<AimSampleRange> rewritten;
        QVERIFY2(parseAimdSamples(packedMoov, packed.size(), &rewritten, &error),
                 qPrintable(error));
        QCOMPARE(rewritten.size(), 2);
        QCOMPARE(rewritten[0].offset, qint64(ftyp.size() + 8));
        QCOMPARE(rewritten[1].offset, qint64(ftyp.size() + 12));
        QCOMPARE(packed.mid(int(rewritten[0].offset), 4),
                 QByteArrayLiteral("AIM0"));
        QCOMPARE(packed.mid(int(rewritten[1].offset), 4),
                 QByteArrayLiteral("AIM1"));
    }

    void packRewritesSixtyFourBitChunkOffsets() {
        const QByteArray ftyp = ftypBox();
        const QByteArray moov = twoSampleMoov(true);
        QVector<AimSampleRange> samples;
        QVERIFY(parseAimdSamples(moov, 72, &samples, nullptr));
        QString error;
        const QByteArray packed = packAimExtract(
            ftyp, moov, samples,
            {QByteArrayLiteral("AIM0"), QByteArrayLiteral("AIM1")}, &error);
        QVERIFY2(!packed.isEmpty(), qPrintable(error));
        QVector<AimSampleRange> rewritten;
        QVERIFY(parseAimdSamples(boxAt(packed, "moov"), packed.size(),
                                 &rewritten, &error));
        QCOMPARE(rewritten[0].offset, qint64(ftyp.size() + 8));
    }

    void packRejectsAnInvalidFtyp() {
        QString error;
        QVERIFY(packAimExtract(QByteArrayLiteral("xxxx"), twoSampleMoov(), {},
                               {}, &error)
                    .isEmpty());
        QCOMPARE(error, QStringLiteral("Invalid MP4 ftyp box"));
    }

    void packRejectsASampleCountMismatch() {
        const QByteArray moov = twoSampleMoov();
        QVector<AimSampleRange> samples;
        QVERIFY(parseAimdSamples(moov, 72, &samples, nullptr));
        QString error;
        QVERIFY(packAimExtract(ftypBox(), moov, samples,
                               {QByteArrayLiteral("AIM0")}, &error)
                    .isEmpty());
        QCOMPARE(error, QStringLiteral("AiM sample payload count changed"));
    }

    void packRejectsAShortSamplePayload() {
        const QByteArray moov = twoSampleMoov();
        QVector<AimSampleRange> samples;
        QVERIFY(parseAimdSamples(moov, 72, &samples, nullptr));
        QString error;
        QVERIFY(packAimExtract(ftypBox(), moov, samples,
                               {QByteArrayLiteral("AIM"),
                                QByteArrayLiteral("AIM1")},
                               &error)
                    .isEmpty());
        QCOMPARE(error, QStringLiteral("Short AiM sample payload"));
    }

    void extractPathIsEmptyWithoutAnEtag() {
        RemoteConnection connection;
        connection.type = LocationType::S3;
        connection.target = QStringLiteral("s3://bucket/");
        connection.id = QStringLiteral("id");
        QVERIFY(aimExtractPath(connection, {}).isEmpty());
        QVERIFY(aimExtractPath(connection, QStringLiteral("???")).isEmpty());
    }

    void extractPathUsesTheSidecarKey() {
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
        RemoteConnection connection;
        connection.type = LocationType::S3;
        connection.target = QStringLiteral("s3://bucket/");
        connection.id = QStringLiteral("id");
        QCOMPARE(aimExtractPath(connection, QStringLiteral("\"abc/1\"")),
                 QDir(cacheDirectory(connection))
                     .filePath(QStringLiteral(".omatrack/aim-abc_1.mp4")));
        qunsetenv("XDG_CACHE_HOME");
    }

    void telemetryOpenPathLeavesARealFileAlone() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("session.mp4"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("data"), 4);
        file.close();
        RemoteConnection connection;
        connection.type = LocationType::S3;
        QCOMPARE(telemetryOpenPath(&connection, path), path);
        QCOMPARE(telemetryOpenPath(nullptr, path), path);
    }

    void telemetryOpenPathFallsBackToAStubWithoutAnExtract() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString stub = directory.filePath(QStringLiteral("onboard.mp4"));
        QVERIFY(QFile(stub).open(QIODevice::WriteOnly));
        RemoteConnection connection;
        connection.type = LocationType::S3;
        connection.id = QStringLiteral("id");
        QCOMPARE(telemetryOpenPath(&connection, stub), stub);
    }
};

QTEST_MAIN(AimRemoteIndexTest)
#include "AimRemoteIndexTest.moc"
