#include "AimRemoteIndex.h"
#include "VerboseLog.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <limits>

namespace omatrack {
namespace {

struct Box {
    qsizetype start = 0;
    qsizetype payload = 0;
    qsizetype end = 0;
    QByteArray type;
};

struct Chunk {
    qsizetype firstSample = 0;
    qsizetype sampleCount = 0;
};

struct AimTable {
    QVector<AimSampleRange> samples;
    QVector<Chunk> chunks;
    Box chunkOffsets;
    bool offsets64 = false;
};

quint32 be32(const QByteArray& data, qsizetype offset, bool* ok = nullptr) {
    const bool valid = offset >= 0 && offset + 4 <= data.size();
    if (ok) *ok = valid;
    if (!valid) return 0;
    const auto* bytes =
        reinterpret_cast<const uchar*>(data.constData() + offset);
    return (quint32(bytes[0]) << 24U) | (quint32(bytes[1]) << 16U) |
           (quint32(bytes[2]) << 8U) | quint32(bytes[3]);
}

quint64 be64(const QByteArray& data, qsizetype offset, bool* ok = nullptr) {
    bool highOk = false;
    bool lowOk = false;
    const quint64 high = be32(data, offset, &highOk);
    const quint64 low = be32(data, offset + 4, &lowOk);
    if (ok) *ok = highOk && lowOk;
    return (high << 32U) | low;
}

void putBe32(QByteArray* data, qsizetype offset, quint32 value) {
    (*data)[offset] = char((value >> 24U) & 0xffU);
    (*data)[offset + 1] = char((value >> 16U) & 0xffU);
    (*data)[offset + 2] = char((value >> 8U) & 0xffU);
    (*data)[offset + 3] = char(value & 0xffU);
}

void putBe64(QByteArray* data, qsizetype offset, quint64 value) {
    putBe32(data, offset, quint32(value >> 32U));
    putBe32(data, offset + 4, quint32(value & 0xffffffffU));
}

void appendBe32(QByteArray* data, quint32 value) {
    const qsizetype offset = data->size();
    data->resize(offset + 4);
    putBe32(data, offset, value);
}

bool readBox(const QByteArray& data, qsizetype start, qsizetype limit,
             Box* box) {
    bool sizeOk = false;
    const quint32 compactSize = be32(data, start, &sizeOk);
    if (!sizeOk || start + 8 > limit) return false;
    qsizetype header = 8;
    quint64 size = compactSize;
    if (compactSize == 1) {
        bool extendedOk = false;
        size = be64(data, start + 8, &extendedOk);
        header = 16;
        if (!extendedOk) return false;
    } else if (compactSize == 0) {
        size = quint64(limit - start);
    }
    if (size < quint64(header) || size > quint64(limit - start)) return false;
    box->start = start;
    box->payload = start + header;
    box->end = start + qsizetype(size);
    box->type = data.mid(start + 4, 4);
    return true;
}

QVector<Box> children(const QByteArray& data, const Box& parent) {
    QVector<Box> result;
    for (qsizetype offset = parent.payload; offset + 8 <= parent.end;) {
        Box child;
        if (!readBox(data, offset, parent.end, &child)) return {};
        result.append(child);
        offset = child.end;
    }
    return result;
}

bool childOfType(const QByteArray& data, const Box& parent,
                 const QByteArray& type, Box* result) {
    for (const Box& child : children(data, parent)) {
        if (child.type == type) {
            *result = child;
            return true;
        }
    }
    return false;
}

bool aimSampleTable(const QByteArray& moov, qint64 fileSize, AimTable* result,
                    QString* error) {
    if (error) error->clear();
    Box root;
    if (!readBox(moov, 0, moov.size(), &root) || root.type != "moov" ||
        root.end != moov.size()) {
        if (error) *error = QStringLiteral("Invalid MP4 moov box");
        return false;
    }

    Box stbl;
    bool foundAim = false;
    for (const Box& trak : children(moov, root)) {
        if (trak.type != "trak") continue;
        Box mdia;
        Box minf;
        Box candidate;
        Box stsd;
        if (!childOfType(moov, trak, "mdia", &mdia) ||
            !childOfType(moov, mdia, "minf", &minf) ||
            !childOfType(moov, minf, "stbl", &candidate) ||
            !childOfType(moov, candidate, "stsd", &stsd))
            continue;
        bool countOk = false;
        const quint32 entryCount = be32(moov, stsd.payload + 4, &countOk);
        qsizetype entryOffset = stsd.payload + 8;
        bool isAim = false;
        for (quint32 entry = 0; countOk && entry < entryCount; ++entry) {
            Box sampleEntry;
            if (!readBox(moov, entryOffset, stsd.end, &sampleEntry)) break;
            if (sampleEntry.type == "aimd") {
                isAim = true;
                break;
            }
            entryOffset = sampleEntry.end;
        }
        if (isAim) {
            stbl = candidate;
            foundAim = true;
            break;
        }
    }
    if (!foundAim) {
        if (error) *error = QStringLiteral("MP4 has no AiM aimd track");
        return false;
    }

    Box stsz;
    Box stsc;
    Box offsets;
    bool offsets64 = false;
    if (!childOfType(moov, stbl, "stsz", &stsz) ||
        !childOfType(moov, stbl, "stsc", &stsc)) {
        if (error) *error = QStringLiteral("Incomplete AiM sample table");
        return false;
    }
    if (!childOfType(moov, stbl, "stco", &offsets)) {
        if (!childOfType(moov, stbl, "co64", &offsets)) {
            if (error)
                *error = QStringLiteral("AiM track has no chunk offsets");
            return false;
        }
        offsets64 = true;
    }

    bool ok = false;
    const quint32 defaultSize = be32(moov, stsz.payload + 4, &ok);
    if (!ok) {
        if (error) *error = QStringLiteral("Truncated AiM sample sizes");
        return false;
    }
    const quint32 sampleCount = be32(moov, stsz.payload + 8, &ok);
    if (!ok || sampleCount == 0 ||
        sampleCount > quint32(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("Invalid AiM sample count");
        return false;
    }
    QVector<quint32> sizes;
    sizes.reserve(int(sampleCount));
    for (quint32 sample = 0; sample < sampleCount; ++sample) {
        const quint32 size =
            defaultSize == 0
                ? be32(moov, stsz.payload + 12 + qsizetype(sample) * 4, &ok)
                : defaultSize;
        if (!ok || size == 0) {
            if (error) *error = QStringLiteral("Invalid AiM sample size");
            return false;
        }
        sizes.append(size);
    }

    const quint32 mappingCount = be32(moov, stsc.payload + 4, &ok);
    if (!ok || mappingCount == 0) {
        if (error) *error = QStringLiteral("Invalid AiM chunk map");
        return false;
    }
    struct Mapping {
        quint32 firstChunk = 0;
        quint32 samplesPerChunk = 0;
    };
    QVector<Mapping> mappings;
    mappings.reserve(int(mappingCount));
    for (quint32 entry = 0; entry < mappingCount; ++entry) {
        const qsizetype at = stsc.payload + 8 + qsizetype(entry) * 12;
        const quint32 first = be32(moov, at, &ok);
        const quint32 perChunk = be32(moov, at + 4, &ok);
        const quint32 description = be32(moov, at + 8, &ok);
        if (!ok || first == 0 || perChunk == 0 || description == 0 ||
            (!mappings.isEmpty() && first <= mappings.back().firstChunk)) {
            if (error) *error = QStringLiteral("Invalid AiM chunk map");
            return false;
        }
        mappings.append({first, perChunk});
    }

    const quint32 chunkCount = be32(moov, offsets.payload + 4, &ok);
    const qsizetype offsetWidth = offsets64 ? 8 : 4;
    if (!ok || chunkCount == 0 ||
        offsets.payload + 8 + qsizetype(chunkCount) * offsetWidth >
            offsets.end) {
        if (error) *error = QStringLiteral("Invalid AiM chunk offsets");
        return false;
    }

    result->samples.clear();
    result->samples.reserve(int(sampleCount));
    result->chunks.clear();
    result->chunks.reserve(int(chunkCount));
    qsizetype sampleIndex = 0;
    int mappingIndex = 0;
    for (quint32 chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        const quint32 oneBasedChunk = chunkIndex + 1;
        while (mappingIndex + 1 < mappings.size() &&
               mappings[mappingIndex + 1].firstChunk <= oneBasedChunk)
            ++mappingIndex;
        if (mappings[mappingIndex].firstChunk > oneBasedChunk) {
            if (error) *error = QStringLiteral("AiM chunk map starts late");
            return false;
        }
        const qsizetype count = mappings[mappingIndex].samplesPerChunk;
        if (sampleIndex + count > sizes.size()) {
            if (error) *error = QStringLiteral("AiM chunk map exceeds samples");
            return false;
        }
        const qsizetype at =
            offsets.payload + 8 + qsizetype(chunkIndex) * offsetWidth;
        const quint64 rawOffset =
            offsets64 ? be64(moov, at, &ok) : quint64(be32(moov, at, &ok));
        if (!ok || rawOffset > quint64(std::numeric_limits<qint64>::max())) {
            if (error) *error = QStringLiteral("Invalid AiM chunk offset");
            return false;
        }
        qint64 sampleOffset = qint64(rawOffset);
        result->chunks.append({sampleIndex, count});
        for (qsizetype inChunk = 0; inChunk < count; ++inChunk) {
            const quint32 size = sizes[sampleIndex];
            if (sampleOffset < 0 || sampleOffset > fileSize - qint64(size)) {
                if (error)
                    *error = QStringLiteral("AiM sample points outside MP4");
                return false;
            }
            result->samples.append({sampleOffset, size});
            sampleOffset += size;
            ++sampleIndex;
        }
    }
    if (sampleIndex != sizes.size()) {
        if (error)
            *error = QStringLiteral("AiM chunks do not cover all samples");
        return false;
    }
    result->chunkOffsets = offsets;
    result->offsets64 = offsets64;
    return true;
}

QByteArray findAimdMoov(const QByteArray& tail, qint64 tailOffset,
                        qint64 fileSize, QString* error) {
    for (qsizetype typeAt = tail.size() - 4; typeAt >= 4; --typeAt) {
        if (tail.mid(typeAt, 4) != "moov") continue;
        const qsizetype boxStart = typeAt - 4;
        bool ok = false;
        quint64 size = be32(tail, boxStart, &ok);
        if (!ok) continue;
        if (size == 1) size = be64(tail, boxStart + 8, &ok);
        if (!ok || size < 8 || size > quint64(tail.size() - boxStart)) continue;
        const qint64 absoluteStart = tailOffset + boxStart;
        if (absoluteStart < 0 ||
            quint64(absoluteStart) + size > quint64(fileSize))
            continue;
        const QByteArray candidate = tail.mid(boxStart, qsizetype(size));
        AimTable table;
        QString ignored;
        if (aimSampleTable(candidate, fileSize, &table, &ignored))
            return candidate;
    }
    if (error) *error = QStringLiteral("Unable to locate AiM MP4 metadata");
    return {};
}

}  // namespace

bool parseAimdSamples(const QByteArray& moov, qint64 fileSize,
                      QVector<AimSampleRange>* samples, QString* error) {
    if (!samples) {
        if (error) *error = QStringLiteral("Missing sample output");
        return false;
    }
    AimTable table;
    if (!aimSampleTable(moov, fileSize, &table, error)) return false;
    *samples = std::move(table.samples);
    return true;
}

QByteArray packAimExtract(const QByteArray& ftyp, const QByteArray& moov,
                          const QVector<AimSampleRange>& samples,
                          const QVector<QByteArray>& sampleBytes,
                          QString* error) {
    if (error) error->clear();
    Box ftypBox;
    if (!readBox(ftyp, 0, ftyp.size(), &ftypBox) || ftypBox.type != "ftyp" ||
        ftypBox.end != ftyp.size()) {
        if (error) *error = QStringLiteral("Invalid MP4 ftyp box");
        return {};
    }
    qint64 originalSize = 0;
    for (const AimSampleRange& sample : samples)
        originalSize =
            std::max(originalSize, sample.offset + qint64(sample.size));
    AimTable table;
    if (!aimSampleTable(moov, originalSize, &table, error)) return {};
    if (samples != table.samples || sampleBytes.size() != samples.size()) {
        if (error) *error = QStringLiteral("AiM sample payload count changed");
        return {};
    }

    quint64 payloadSize = 0;
    for (int index = 0; index < sampleBytes.size(); ++index) {
        if (sampleBytes[index].size() != samples[index].size) {
            if (error) *error = QStringLiteral("Short AiM sample payload");
            return {};
        }
        payloadSize += quint64(sampleBytes[index].size());
    }
    if (payloadSize + 8 > quint64(std::numeric_limits<quint32>::max()) ||
        payloadSize + quint64(ftyp.size()) + quint64(moov.size()) + 8 >
            quint64(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("AiM extract is too large");
        return {};
    }

    QByteArray patchedMoov = moov;
    const quint64 mdatPayload = quint64(ftyp.size()) + 8;
    quint64 packedOffset = mdatPayload;
    qsizetype chunkIndex = 0;
    for (qsizetype sampleIndex = 0; sampleIndex < samples.size();
         ++sampleIndex) {
        if (chunkIndex < table.chunks.size() &&
            table.chunks[chunkIndex].firstSample == sampleIndex) {
            const qsizetype field = table.chunkOffsets.payload + 8 +
                                    chunkIndex * (table.offsets64 ? 8 : 4);
            if (table.offsets64)
                putBe64(&patchedMoov, field, packedOffset);
            else
                putBe32(&patchedMoov, field, quint32(packedOffset));
            ++chunkIndex;
        }
        packedOffset += quint64(sampleBytes[sampleIndex].size());
    }
    if (chunkIndex != table.chunks.size()) {
        if (error) *error = QStringLiteral("AiM chunk rewrite failed");
        return {};
    }

    QByteArray result;
    result.reserve(int(quint64(ftyp.size()) + payloadSize +
                       quint64(patchedMoov.size()) + 8));
    result.append(ftyp);
    appendBe32(&result, quint32(payloadSize + 8));
    result.append("mdat", 4);
    for (const QByteArray& sample : sampleBytes) result.append(sample);
    result.append(patchedMoov);
    return result;
}

QString aimExtractPath(const RemoteConnection& connection,
                       const QString& etag) {
    const QString key = etagFileKey(etag);
    if (key.isEmpty()) return {};
    return QDir(cacheDirectory(connection))
        .filePath(QStringLiteral(".omatrack/aim-") + key +
                  QStringLiteral(".mp4"));
}

QString telemetryOpenPath(const RemoteConnection* connection,
                          const QString& path) {
    if (!connection || QFileInfo(path).size() != 0) return path;
    const QString extract =
        aimExtractPath(*connection, cachedObjectEtag(*connection, path));
    return QFileInfo(extract).size() > 0 ? extract : path;
}

QString materializeAimExtract(const RemoteConnection& connection,
                              const QString& stubPath, const QString& etag,
                              const IoCancel& cancel) {
    const QString outputPath = aimExtractPath(connection, etag);
    if (outputPath.isEmpty()) return QStringLiteral("Remote video has no ETag");
    if (QFileInfo(outputPath).size() > 0) {
        qCInfo(lcIo).noquote() << "cache hit extract" << displayPath(outputPath)
                               << formatBytes(QFileInfo(outputPath).size());
        return {};
    }
    qCInfo(lcIo).noquote() << "cache miss extract" << displayPath(outputPath)
                           << "etag" << etag;

    const qint64 fileSize = cachedObjectSize(connection, stubPath);
    const QUrl objectUrl = objectUrlForPath(connection, stubPath);
    if (fileSize < 16 || !objectUrl.isValid())
        return QStringLiteral("Remote video metadata is unavailable");

    QString error;
    const qint64 prefixLength = std::min<qint64>(fileSize, 64 * 1024);
    if (ioCancelled(cancel)) return QStringLiteral("Cancelled");
    QByteArray prefix =
        getObjectRange(connection, objectUrl, 0, prefixLength, &error, cancel);
    if (prefix.isEmpty()) return error;
    Box ftypBox;
    if (!readBox(prefix, 0, prefix.size(), &ftypBox) || ftypBox.type != "ftyp")
        return QStringLiteral("Remote video has no MP4 ftyp box");
    if (ftypBox.end > prefix.size()) {
        prefix = getObjectRange(connection, objectUrl, 0, ftypBox.end, &error,
                                cancel);
        if (prefix.isEmpty()) return error;
    }
    const QByteArray ftyp = prefix.left(ftypBox.end);

    QByteArray moov;
    constexpr qint64 kInitialTail = 2LL * 1024 * 1024;
    constexpr qint64 kMaximumTail = 64LL * 1024 * 1024;
    for (qint64 requested = std::min(fileSize, kInitialTail);;
         requested = std::min(fileSize, requested * 2)) {
        if (ioCancelled(cancel)) return QStringLiteral("Cancelled");
        const qint64 tailOffset = fileSize - requested;
        const QByteArray tail = getObjectRange(
            connection, objectUrl, tailOffset, requested, &error, cancel);
        if (tail.isEmpty()) return error;
        moov = findAimdMoov(tail, tailOffset, fileSize, nullptr);
        if (!moov.isEmpty()) break;
        if (requested == fileSize || requested >= kMaximumTail)
            return QStringLiteral("AiM MP4 metadata exceeds 64 MiB");
    }

    QVector<AimSampleRange> samples;
    if (!parseAimdSamples(moov, fileSize, &samples, &error)) return error;

    QVector<ObjectRange> fetchRanges;
    fetchRanges.reserve(samples.size());
    for (const AimSampleRange& sample : samples) {
        if (!fetchRanges.isEmpty() &&
            fetchRanges.back().offset + fetchRanges.back().length ==
                sample.offset) {
            fetchRanges.back().length += sample.size;
        } else {
            fetchRanges.append({sample.offset, sample.size});
        }
    }
    QVector<QByteArray> fetched;
    if (!getObjectRanges(connection, objectUrl, fetchRanges, &fetched, &error,
                         cancel))
        return error;

    QVector<QByteArray> sampleBytes;
    sampleBytes.reserve(samples.size());
    qsizetype sampleIndex = 0;
    for (int rangeIndex = 0; rangeIndex < fetchRanges.size(); ++rangeIndex) {
        qsizetype offset = 0;
        const qint64 rangeEnd =
            fetchRanges[rangeIndex].offset + fetchRanges[rangeIndex].length;
        while (sampleIndex < samples.size() &&
               samples[sampleIndex].offset < rangeEnd) {
            const qsizetype size = samples[sampleIndex].size;
            if (offset + size > fetched[rangeIndex].size())
                return QStringLiteral("Short AiM range response");
            sampleBytes.append(fetched[rangeIndex].mid(offset, size));
            offset += size;
            ++sampleIndex;
        }
    }
    if (sampleIndex != samples.size())
        return QStringLiteral("Incomplete AiM range response");

    const QByteArray extract =
        packAimExtract(ftyp, moov, samples, sampleBytes, &error);
    if (extract.isEmpty()) return error;
    if (!QDir().mkpath(QFileInfo(outputPath).absolutePath()))
        return QStringLiteral("Unable to create AiM extract cache");
    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly) ||
        output.write(extract) != extract.size() || !output.commit())
        return QStringLiteral("Unable to write AiM extract cache");
    qCInfo(lcIo).noquote() << "write extract" << displayPath(outputPath)
                           << formatBytes(extract.size());
    return {};
}

}  // namespace omatrack
