#pragma once

#include "RemoteCache.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace omatrack {

struct AimSampleRange {
    qint64 offset = 0;
    quint32 size = 0;

    bool operator==(const AimSampleRange& other) const {
        return offset == other.offset && size == other.size;
    }
};
/// Sample table of the `aimd` track, parsed from a `moov` box.
bool parseAimdSamples(const QByteArray& moov, qint64 fileSize,
                      QVector<AimSampleRange>* samples, QString* error);

/// Packs `ftyp` + concatenated samples + a `moov` whose aimd `stco` has been
/// rewritten to the packed layout. The existing parser can open the result.
QByteArray packAimExtract(const QByteArray& ftyp, const QByteArray& moov,
                          const QVector<AimSampleRange>& samples,
                          const QVector<QByteArray>& sampleBytes,
                          QString* error);

/// Range-fetches the aimd track of a streamed MP4 into
/// `{cache}/.omatrack/aim-{etag}.mp4`. Empty return is success.
QString materializeAimExtract(const RemoteConnection& connection,
                              const QString& stubPath, const QString& etag,
                              const IoCancel& cancel = {});

/// Extract path for a stub, or empty when there is no ETag.
QString aimExtractPath(const RemoteConnection& connection, const QString& etag);

/// Path the parser should open: a materialized extract for a stream stub,
/// otherwise the original file.
QString telemetryOpenPath(const RemoteConnection* connection,
                          const QString& path);

}  // namespace omatrack
