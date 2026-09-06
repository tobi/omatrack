// origin: PUBLIC — generated cache content stays in the user's private app
// cache.
#include "ImageTelemetryCache.h"

#include "omatrack_bridge.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <zstd.h>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace omatrack {
namespace {
using namespace inference;
using Status = ImageTelemetryCache::Status;
using Result = ImageTelemetryCache::Result;
using Cancel = ImageTelemetryCache::Cancel;
constexpr qsizetype MaxPlainBytes = 128 * 1024 * 1024;
constexpr qint64 MaxCompressedBytes = 32 * 1024 * 1024;
constexpr qsizetype BlockBytes = 64 * 1024;
constexpr char PassName[] = "omatrack.image_prediction";
constexpr char VisitedName[] = "image_derived_visited";
constexpr char PtsName[] = "image_derived_presentation_pts_ns";
constexpr char Regularization[] =
    "first_decoded_frame_in_200ms_slot;actual_pts_separate;gaps_null_not_held";

struct Failure : std::runtime_error {
    Status status;
    Failure(Status s, const QString& text)
        : std::runtime_error(text.toStdString()), status(s) {}
};
void checkCancel(const Cancel& cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed))
        throw Failure(Status::Cancelled,
                      QStringLiteral("Image cache operation cancelled"));
}
QString text(const std::string& value) {
    return QString::fromUtf8(value.data(), qsizetype(value.size()));
}
void require(bool condition, const char* error) {
    if (!condition) throw Failure(Status::Invalid, QString::fromUtf8(error));
}
Result failed(const Failure& failure, QString path = {}) {
    return {
        failure.status, {}, std::move(path), QString::fromUtf8(failure.what())};
}

ImageTelemetryFileIdentity identify(const QString& path) {
    const QFileInfo info(path);
    require(info.exists() && info.isFile(),
            "Source or model is not a regular file");
    ImageTelemetryFileIdentity result;
    result.canonicalPath = info.canonicalFilePath().toUtf8().toStdString();
    require(!result.canonicalPath.empty(),
            "Cannot canonicalize source or model");
#ifdef Q_OS_UNIX
    struct stat st = {};
    require(::stat(QFile::encodeName(text(result.canonicalPath)).constData(),
                   &st) == 0 &&
                S_ISREG(st.st_mode),
            "Cannot stat source or model");
    result.device = st.st_dev;
    result.inode = st.st_ino;
    result.size = st.st_size;
#if defined(Q_OS_DARWIN)
    result.mtimeNs = std::int64_t(st.st_mtimespec.tv_sec) * 1000000000LL +
                     st.st_mtimespec.tv_nsec;
    result.changeNs = std::int64_t(st.st_ctimespec.tv_sec) * 1000000000LL +
                      st.st_ctimespec.tv_nsec;
#else
    result.mtimeNs =
        std::int64_t(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
    result.changeNs =
        std::int64_t(st.st_ctim.tv_sec) * 1000000000LL + st.st_ctim.tv_nsec;
#endif
#elif defined(Q_OS_WIN)
    const auto native = QDir::toNativeSeparators(text(result.canonicalPath));
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Cannot identify source or model");
    BY_HANDLE_FILE_INFORMATION fileInfo{};
    FILE_BASIC_INFO basic{};
    const bool ok = GetFileInformationByHandle(handle, &fileInfo) &&
                    GetFileInformationByHandleEx(handle, FileBasicInfo, &basic,
                                                 sizeof(basic));
    CloseHandle(handle);
    require(ok, "Cannot read source or model identity");
    result.device = fileInfo.dwVolumeSerialNumber;
    result.inode =
        (std::uint64_t(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow;
    result.size = std::int64_t((std::uint64_t(fileInfo.nFileSizeHigh) << 32) |
                               fileInfo.nFileSizeLow);
    auto unixNs = [](LONGLONG ticks) {
        return (ticks - 116444736000000000LL) * 100LL;
    };
    result.mtimeNs = unixNs(basic.LastWriteTime.QuadPart);
    result.changeNs = unixNs(basic.ChangeTime.QuadPart);
#else
    result.size = info.size();
    result.mtimeNs = info.lastModified().toMSecsSinceEpoch() * 1000000LL;
    result.changeNs = info.metadataChangeTime().toMSecsSinceEpoch() * 1000000LL;
#endif
    require(result.size >= 0, "Invalid source or model size");
    return result;
}

bool same(const ImageTelemetryFileIdentity& a,
          const ImageTelemetryFileIdentity& b) {
    return a.canonicalPath == b.canonicalPath && a.device == b.device &&
           a.inode == b.inode && a.size == b.size && a.mtimeNs == b.mtimeNs &&
           a.changeNs == b.changeNs;
}
std::string modelHash(const ImageTelemetryFileIdentity& model,
                      const Cancel& cancel) {
    QFile file(text(model.canonicalPath));
    if (!file.open(QIODevice::ReadOnly))
        throw Failure(Status::Error, file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 remaining = model.size;
    while (remaining > 0) {
        checkCancel(cancel);
        auto block = file.read(std::min(qint64(BlockBytes), remaining));
        if (block.isEmpty())
            throw Failure(
                Status::Stale,
                QStringLiteral(
                    "Model shortened or became unreadable during hashing"));
        hash.addData(block);
        remaining -= block.size();
    }
    if (file.size() != model.size)
        throw Failure(Status::Stale,
                      QStringLiteral("Model changed size during hashing"));
    return hash.result().toHex().toStdString();
}
void current(const ImageTelemetrySeries& series, const Cancel& cancel) {
    checkCancel(cancel);
    try {
        if (!same(identify(text(series.identity.source.canonicalPath)),
                  series.identity.source) ||
            !same(identify(text(series.identity.model.canonicalPath)),
                  series.identity.model) ||
            modelHash(series.identity.model, cancel) !=
                series.identity.modelSha256 ||
            !same(identify(text(series.identity.model.canonicalPath)),
                  series.identity.model))
            throw Failure(
                Status::Stale,
                QStringLiteral(
                    "Source or model changed; image cache identity is stale"));
    } catch (const Failure& error) {
        if (error.status == Status::Cancelled) throw;
        throw Failure(
            Status::Stale,
            QStringLiteral("Source or model identity no longer matches"));
    }
}

void addIdentity(QJsonObject& p, const QString& prefix,
                 const ImageTelemetryFileIdentity& file) {
    p.insert(prefix + QStringLiteral("path"), text(file.canonicalPath));
    p.insert(prefix + QStringLiteral("device"), QString::number(file.device));
    p.insert(prefix + QStringLiteral("inode"), QString::number(file.inode));
    p.insert(prefix + QStringLiteral("size"), QString::number(file.size));
    p.insert(prefix + QStringLiteral("mtime_ns"),
             QString::number(file.mtimeNs));
    p.insert(prefix + QStringLiteral("change_ns"),
             QString::number(file.changeNs));
}
QJsonObject identityParameters(const ImageTelemetrySeries& series) {
    QJsonObject p;
    addIdentity(p, QStringLiteral("source_"), series.identity.source);
    addIdentity(p, QStringLiteral("model_"), series.identity.model);
    p.insert(QStringLiteral("model_sha256"), text(series.identity.modelSha256));
    p.insert(QStringLiteral("schema_revision"),
             text(series.identity.schemaRevision));
    p.insert(QStringLiteral("layout_revision"),
             text(series.identity.layoutRevision));
    p.insert(QStringLiteral("decoder_revision"),
             text(series.identity.decoderRevision));
    p.insert(QStringLiteral("native_telemetry"),
             series.identity.nativeTelemetryAbsent
                 ? QStringLiteral("absent")
                 : QStringLiteral("unverified"));
    p.insert(QStringLiteral("native_used"), QStringLiteral("false"));
    p.insert(QStringLiteral("semantics"),
             QStringLiteral("image_predictions_not_gold"));
    p.insert(QStringLiteral("input_kind"),
             QStringLiteral("decoded_full_resolution_rgb24"));
    p.insert(QStringLiteral("duration_ns"), QString::number(series.durationNs));
    p.insert(QStringLiteral("source_origin_ns"),
             QString::number(series.timelineOriginNs));
    p.insert(QStringLiteral("period_ns"),
             QString::number(ImageTelemetryPeriodNs));
    p.insert(QStringLiteral("actual_pts_channel"),
             QString::fromLatin1(PtsName));
    p.insert(QStringLiteral("regularization"),
             QString::fromLatin1(Regularization));
    return p;
}
QJsonObject keyParameters(const ImageTelemetrySeries& series) {
    auto p = identityParameters(series);
    // Model content + semantic revisions determine inference, not where an
    // identical export was copied. Keep file metadata only for provenance and
    // current-file revalidation. The source identity determines its clock;
    // load restores that clock without requiring a decoder/runtime to run.
    for (const auto* suffix :
         {"path", "device", "inode", "size", "mtime_ns", "change_ns"})
        p.remove(QStringLiteral("model_") + QString::fromLatin1(suffix));
    p.remove(QStringLiteral("source_origin_ns"));
    p.remove(QStringLiteral("native_telemetry"));
    return p;
}
std::array<QByteArray, 11> channelNames() {
    std::array<QByteArray, 11> names;
    for (std::size_t i = 0; i < 4; ++i) {
        names[i] = ImageTelemetryChannelNames[i];
        names[i + 4] = names[i] + "_known";
    }
    names[8] = VisitedName;
    names[9] = PtsName;
    names[10] = "image_layout_supported";
    return names;
}

void validate(const ImageTelemetrySeries& series, const Cancel& cancel,
              bool requireNativeAbsence = true) {
    require(!requireNativeAbsence || series.identity.nativeTelemetryAbsent,
            "Native telemetry absence must be established before caching "
            "predictions");
    require(series.identity.schemaRevision == ImageTelemetrySchemaRevision,
            "Unsupported image-series schema revision");
    require(!series.identity.layoutRevision.empty() &&
                !series.identity.decoderRevision.empty(),
            "Missing image reader revision");
    require(series.identity.modelSha256.size() == 64 &&
                std::all_of(series.identity.modelSha256.begin(),
                            series.identity.modelSha256.end(),
                            [](char c) {
                                return (c >= '0' && c <= '9') ||
                                       (c >= 'a' && c <= 'f');
                            }),
            "Invalid model content hash");
    const auto count = ImageTelemetrySeries::slotCount(series.durationNs);
    require(count > 0 && series.cells.size() == count,
            "Invalid image-series duration or slot count (limit 24h)");
    require(!series.identity.source.canonicalPath.empty() &&
                !series.identity.model.canonicalPath.empty() &&
                series.identity.source.canonicalPath !=
                    series.identity.model.canonicalPath,
            "Invalid source/model identities");
    for (std::size_t i = 0; i < count; ++i) {
        if (i % 1024 == 0) checkCancel(cancel);
        const auto& slot = series.cells[i];
        require(
            slot.presentationPtsNs.has_value() == slot.sourcePtsNs.has_value(),
            "Both actual timestamp domains are required together");
        require(
            !slot.layoutSupported || (slot.visited && slot.presentationPtsNs),
            "Layout support requires a visited decoded frame");
        if (slot.presentationPtsNs) {
            const auto pts = *slot.presentationPtsNs;
            const auto start = std::int64_t(i) * ImageTelemetryPeriodNs;
            require(slot.visited && pts >= start &&
                        pts < std::min(start + ImageTelemetryPeriodNs,
                                       series.durationNs),
                    "Actual PTS does not belong to its visited lattice slot");
            require(series.timelineOriginNs <=
                        std::numeric_limits<std::int64_t>::max() - pts,
                    "Source PTS transform overflow");
            require(*slot.sourcePtsNs == series.timelineOriginNs + pts,
                    "Source/presentation PTS transform mismatch");
        }
        for (std::size_t field = 0; field < 4; ++field) {
            if (!slot.values[field]) continue;
            const auto value = *slot.values[field];
            require(slot.visited && slot.layoutSupported &&
                        slot.presentationPtsNs && std::isfinite(value),
                    "Known value requires a supported visited frame and finite "
                    "observation");
            require(field < 2 ? (value >= 0 && value <= 999 &&
                                 std::floor(value) == value)
                              : (value >= 0 && value <= 100),
                    "Image observation outside its declared numeric domain");
        }
    }
}

QString safePath(const QString& root, const ImageTelemetrySeries& series,
                 const QString& filename, bool create) {
    const auto sourceParent =
        QFileInfo(text(series.identity.source.canonicalPath)).absolutePath();
    const auto modelParent =
        QFileInfo(text(series.identity.model.canonicalPath)).absolutePath();
    const auto absoluteRoot = QDir(root).absolutePath();
    require(absoluteRoot != sourceParent && absoluteRoot != modelParent,
            "Image cache must not be beside source/model");
    if (create && !QDir().mkpath(root))
        throw Failure(Status::Error,
                      QStringLiteral("Cannot create image cache directory"));
    const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
    if (!canonicalRoot.isEmpty()) {
        require(canonicalRoot != sourceParent && canonicalRoot != modelParent,
                "Image cache symlink resolves beside source/model");
    }
    const auto path = QDir(root).filePath(filename);
    require(!QFileInfo(path).isSymLink(),
            "Refusing symlink at image cache destination");
    require(QFileInfo(path).absoluteFilePath() !=
                    text(series.identity.source.canonicalPath) &&
                QFileInfo(path).absoluteFilePath() !=
                    text(series.identity.model.canonicalPath),
            "Cache destination is source/model");
    return path;
}

struct Compressor {
    QSaveFile& file;
    Cancel cancel;
    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> context{
        ZSTD_createCCtx(), ZSTD_freeCCtx};
    std::array<char, BlockBytes> output{};
    qint64 written = 0;
    qsizetype plain = 0;
    Compressor(QSaveFile& f, Cancel c) : file(f), cancel(std::move(c)) {
        if (!context)
            throw Failure(Status::Error,
                          QStringLiteral("Cannot allocate zstd compressor"));
        require(!ZSTD_isError(ZSTD_CCtx_setParameter(
                    context.get(), ZSTD_c_compressionLevel, 3)),
                "Cannot configure zstd compressor");
    }
    void write(const QByteArray& bytes, bool end = false) {
        plain += bytes.size();
        require(plain <= MaxPlainBytes,
                "Image cache exceeds decompressed bound");
        ZSTD_inBuffer in{bytes.constData(), std::size_t(bytes.size()), 0};
        size_t remaining = 1;
        do {
            checkCancel(cancel);
            ZSTD_outBuffer out{output.data(), output.size(), 0};
            remaining = ZSTD_compressStream2(
                context.get(), &out, &in, end ? ZSTD_e_end : ZSTD_e_continue);
            if (ZSTD_isError(remaining))
                throw Failure(Status::Error,
                              QString::fromUtf8(ZSTD_getErrorName(remaining)));
            written += qint64(out.pos);
            require(written <= MaxCompressedBytes,
                    "Image cache exceeds compressed bound");
            if (out.pos &&
                file.write(output.data(), qint64(out.pos)) != qint64(out.pos))
                throw Failure(Status::Error, file.errorString());
        } while (in.pos < in.size || (end && remaining));
    }
};
QByteArray decompress(const QString& path, const Cancel& cancel) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Failure(Status::Error, file.errorString());
    require(file.size() > 0 && file.size() <= MaxCompressedBytes,
            "Invalid compressed image-cache size");
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context{
        ZSTD_createDCtx(), ZSTD_freeDCtx};
    require(bool(context), "Cannot allocate zstd decoder");
    require(!ZSTD_isError(
                ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 27)),
            "Cannot bound zstd window");
    std::array<char, BlockBytes> block{};
    QByteArray plain;
    bool ended = false;
    while (!file.atEnd()) {
        checkCancel(cancel);
        auto input = file.read(BlockBytes);
        if (input.isEmpty() && file.error() != QFileDevice::NoError)
            throw Failure(Status::Error, file.errorString());
        ZSTD_inBuffer in{input.constData(), std::size_t(input.size()), 0};
        bool flushOutput = false;
        do {
            require(!ended, "Trailing or concatenated data in image cache");
            checkCancel(cancel);
            ZSTD_outBuffer out{block.data(), block.size(), 0};
            const auto remaining =
                ZSTD_decompressStream(context.get(), &out, &in);
            require(!ZSTD_isError(remaining), "Corrupt zstd image cache");
            require(plain.size() + qsizetype(out.pos) <= MaxPlainBytes,
                    "Image cache decompression exceeds bound");
            plain.append(block.data(), qsizetype(out.pos));
            ended = remaining == 0;
            flushOutput = out.pos == out.size && !ended;
        } while (in.pos < in.size || flushOutput);
    }
    require(ended && !plain.isEmpty(), "Truncated zstd image cache");
    return plain;
}

QString passParameter(void* handle, const QByteArray& key) {
    const auto bytes =
        omatrack_pass_parameter(handle, PassName, key.constData(), nullptr, 0);
    require(bytes > 0 && bytes <= 16384,
            "Missing/oversized image prediction provenance");
    QByteArray buffer(qsizetype(bytes), Qt::Uninitialized);
    require(omatrack_pass_parameter(handle, PassName, key.constData(),
                                    buffer.data(),
                                    std::size_t(buffer.size())) == bytes,
            "Invalid image prediction provenance");
    return QString::fromUtf8(buffer.constData(), buffer.size() - 1);
}
using Handle = std::unique_ptr<void, decltype(&omatrack_close)>;
std::shared_ptr<ImageTelemetrySeries> decode(
    const QByteArray& bytes, const ImageTelemetrySeries& expected,
    const Cancel& cancel) {
    checkCancel(cancel);
    Handle handle(omatrack_open_mtj_bytes(
                      reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                      std::size_t(bytes.size())),
                  omatrack_close);
    if (!handle)
        throw Failure(Status::Invalid,
                      QString::fromUtf8(omatrack_last_error()));
    const auto parameters = keyParameters(expected);
    for (auto i = parameters.begin(); i != parameters.end(); ++i)
        require(passParameter(handle.get(), i.key().toUtf8()) ==
                    i.value().toString(),
                "Image cache provenance mismatch");
    require(omatrack_source_lap_count(handle.get()) == 0,
            "Image cache must not fabricate classified laps");
    auto series = std::make_shared<ImageTelemetrySeries>();
    series->identity = expected.identity;
    require(passParameter(handle.get(), "native_telemetry") ==
                QStringLiteral("absent"),
            "Cached prediction lacks verified native absence");
    series->identity.nativeTelemetryAbsent = true;
    series->durationNs = expected.durationNs;
    bool ok = false;
    series->timelineOriginNs =
        passParameter(handle.get(), "source_origin_ns").toLongLong(&ok);
    require(ok, "Invalid exact source timeline origin");
    series->revision =
        passParameter(handle.get(), "snapshot_revision").toULongLong(&ok);
    require(ok, "Invalid snapshot revision");
    const auto count = ImageTelemetrySeries::slotCount(series->durationNs);
    series->cells.resize(count);
    const auto names = channelNames();
    require(omatrack_channel_count(handle.get()) == names.size(),
            "Wrong image-cache channel count");
    std::array<bool, 11> seen{};
    std::array<std::vector<double>, 4> known;
    std::vector<double> values(count);
    for (std::size_t c = 0; c < names.size(); ++c) {
        checkCancel(cancel);
        const QByteArray name(omatrack_channel_name(handle.get(), c));
        const auto it = std::find(names.begin(), names.end(), name);
        require(it != names.end(), "Unexpected image-cache channel");
        const auto channel = std::size_t(it - names.begin());
        require(!seen[channel], "Duplicate image-cache channel");
        seen[channel] = true;
        const QByteArray unit(omatrack_channel_unit(handle.get(), c));
        require(unit == (channel == 2 || channel == 3 ? QByteArray("%")
                         : channel == 9               ? QByteArray("ns")
                                                      : QByteArray()),
                "Wrong image-cache channel unit");
        require(omatrack_channel_sample_count(handle.get(), c) == count &&
                    omatrack_channel_chunk_count(handle.get(), c) == 1 &&
                    omatrack_chunk_period_ns(handle.get(), c, 0) ==
                        ImageTelemetryPeriodNs &&
                    omatrack_chunk_time_base_ns(handle.get(), c, 0) == 0 &&
                    omatrack_channel_duration_ns(handle.get(), c) ==
                        count * ImageTelemetryPeriodNs,
                "Image-cache channel is not the complete declared 5Hz lattice");
        require(omatrack_channel_decode_all(handle.get(), c, values.data(),
                                            values.size()) == count,
                "Short image-cache channel decode");
        for (std::size_t i = 0; i < count; ++i) {
            const double value = values[i];
            auto& slot = series->cells[i];
            if (channel < 4) {
                require(!std::isinf(value), "Infinite image-cache observation");
                if (std::isfinite(value)) slot.values[channel] = value;
            } else if (channel < 8) {
                require(value == 0 || value == 1,
                        "Invalid per-field known mask");
            } else if (channel == 8) {
                require(value == 0 || value == 1, "Invalid visited mask");
                slot.visited = value == 1;
            } else if (channel == 10) {
                require(value == 0 || value == 1,
                        "Invalid layout support mask");
                slot.layoutSupported = value == 1;
            } else if (std::isfinite(value)) {
                require(value >= 0 && value < double(series->durationNs) &&
                            value == std::floor(value),
                        "Invalid actual presentation PTS");
                const auto pts = std::int64_t(value);
                require(series->timelineOriginNs <=
                            std::numeric_limits<std::int64_t>::max() - pts,
                        "Source PTS reconstruction overflow");
                slot.presentationPtsNs = pts;
                slot.sourcePtsNs = series->timelineOriginNs + pts;
            } else {
                require(std::isnan(value), "Invalid actual presentation PTS");
            }
        }
        if (channel >= 4 && channel < 8) known[channel - 4] = values;
    }
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t field = 0; field < 4; ++field)
            require(series->cells[i].values[field].has_value() ==
                        (known[field][i] == 1),
                    "Known mask contradicts null/value samples");
    validate(*series, cancel);
    const auto visited =
        passParameter(handle.get(), "visited_slots").toULongLong(&ok);
    require(ok && visited == series->visitedCount(), "Coverage count mismatch");
    require(
        passParameter(handle.get(), "complete") ==
            (series->complete() ? QStringLiteral("1") : QStringLiteral("0")),
        "Completion declaration contradicts visited coverage");
    return series;
}

QByteArray number(double value) {
    std::array<char, 64> buffer{};
    if (value == std::floor(value) && std::abs(value) <= 9007199254740991.)
        return QByteArray::number(qlonglong(value));
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    require(result.ec == std::errc{}, "Cannot encode image observation");
    return QByteArray(buffer.data(), qsizetype(result.ptr - buffer.data()));
}
void encode(Compressor& compressor, const ImageTelemetrySeries& series,
            const Cancel& cancel) {
    auto parameters = identityParameters(series);
    parameters.insert(QStringLiteral("snapshot_revision"),
                      QString::number(series.revision));
    parameters.insert(QStringLiteral("visited_slots"),
                      QString::number(series.visitedCount()));
    parameters.insert(QStringLiteral("complete"), series.complete()
                                                      ? QStringLiteral("1")
                                                      : QStringLiteral("0"));
    const auto names = channelNames();
    QJsonArray outputs;
    for (const auto& name : names) outputs.append(QString::fromLatin1(name));
    QJsonObject pass{{QStringLiteral("n"), QString::fromLatin1(PassName)},
                     {QStringLiteral("v"), 1},
                     {QStringLiteral("p"), parameters},
                     {QStringLiteral("out"), outputs}};
    QJsonObject header{
        {QStringLiteral("mtj"), 1},
        {QStringLiteral("q"), qint64(ImageTelemetryPeriodNs)},
        {QStringLiteral("dur"),
         qint64(series.cells.size() * ImageTelemetryPeriodNs)},
        {QStringLiteral("src"), QStringLiteral("image_prediction")},
        {QStringLiteral("passes"), QJsonArray{pass}}};
    compressor.write(QJsonDocument(header).toJson(QJsonDocument::Compact) +
                     "\n[]\n");
    for (std::size_t channel = 0; channel < names.size(); ++channel) {
        QByteArray buffer = "{\"n\":\"" + names[channel] + "\",\"hz\":5";
        if (channel == 2 || channel == 3) buffer += ",\"u\":\"%\"";
        if (channel == 9) buffer += ",\"u\":\"ns\"";
        if (channel >= 4) buffer += ",\"vis\":0";
        buffer += ",\"v\":[";
        for (std::size_t i = 0; i < series.cells.size(); ++i) {
            if (i % 1024 == 0) checkCancel(cancel);
            if (i) buffer += ',';
            const auto& slot = series.cells[i];
            if (channel < 4)
                buffer += slot.values[channel] ? number(*slot.values[channel])
                                               : QByteArray("null");
            else if (channel < 8)
                buffer += slot.values[channel - 4] ? '1' : '0';
            else if (channel == 8)
                buffer += slot.visited ? '1' : '0';
            else if (channel == 10)
                buffer += slot.layoutSupported ? '1' : '0';
            else
                buffer +=
                    slot.presentationPtsNs
                        ? QByteArray::number(qlonglong(*slot.presentationPtsNs))
                        : QByteArray("null");
            if (buffer.size() >= BlockBytes) {
                compressor.write(buffer);
                buffer.clear();
            }
        }
        buffer += "]}\n";
        compressor.write(buffer);
    }
    compressor.write({}, true);
}
}  // namespace

ImageTelemetryCache::ImageTelemetryCache(QString root)
    : root_(std::move(root)) {
    if (root_.isEmpty()) {
        const auto xdg = QString::fromLocal8Bit(qgetenv("XDG_CACHE_HOME"));
        const auto appCache =
            xdg.isEmpty() ? QStandardPaths::writableLocation(
                                QStandardPaths::CacheLocation)
                          : QDir(xdg).filePath(QStringLiteral("omatrack"));
        root_ = QDir(appCache).filePath(QStringLiteral("image-telemetry/v1"));
    }
}
QString ImageTelemetryCache::pathFor(const ImageTelemetrySeries& series) const {
    const auto bytes =
        QJsonDocument(keyParameters(series)).toJson(QJsonDocument::Compact);
    const auto key =
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    return QDir(root_).filePath(QString::fromLatin1(key) +
                                QStringLiteral(".telemetry"));
}
ImageTelemetryCache::Result ImageTelemetryCache::prepare(
    const QString& sourcePath, const QString& modelPath,
    std::int64_t durationNs, std::int64_t timelineOriginNs,
    bool nativeTelemetryAbsent, const Cancel& cancel) const {
    try {
        checkCancel(cancel);
        auto series = std::make_shared<ImageTelemetrySeries>();
        series->identity.source = identify(sourcePath);
        series->identity.model = identify(modelPath);
        series->identity.modelSha256 =
            modelHash(series->identity.model, cancel);
        series->identity.nativeTelemetryAbsent = nativeTelemetryAbsent;
        series->durationNs = durationNs;
        series->timelineOriginNs = timelineOriginNs;
        series->cells.resize(ImageTelemetrySeries::slotCount(durationNs));
        validate(*series, cancel, false);
        current(*series, cancel);
        return {Status::Ready, series, pathFor(*series), {}};
    } catch (const Failure& error) {
        return failed(error);
    } catch (const std::exception& error) {
        return {Status::Error, {}, {}, QString::fromUtf8(error.what())};
    }
}
ImageTelemetryCache::Result ImageTelemetryCache::load(
    const ImageTelemetrySeries& expected, const Cancel& cancel) const {
    const auto path = pathFor(expected);
    try {
        validate(expected, cancel, false);
        current(expected, cancel);
        safePath(root_, expected, QFileInfo(path).fileName(), false);
        if (!QFileInfo::exists(path)) return {Status::Miss, {}, path, {}};
        auto series = decode(decompress(path, cancel), expected, cancel);
        current(expected, cancel);
        return {series->complete() ? Status::Complete : Status::Partial,
                series,
                path,
                {}};
    } catch (const Failure& error) {
        return failed(error, path);
    } catch (const std::exception& error) {
        return {Status::Error, {}, path, QString::fromUtf8(error.what())};
    }
}
ImageTelemetryCache::Result ImageTelemetryCache::save(
    const ImageTelemetrySeries& series, const Cancel& cancel) const {
    const auto path = pathFor(series);
    try {
        validate(series, cancel);
        current(series, cancel);
        safePath(root_, series, QFileInfo(path).fileName(), true);
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(0))
            throw Failure(
                Status::Error,
                QStringLiteral("Image cache publication already in progress"));
        // Union disjoint progress under the publication lock. Unknown never
        // erases known; competing observations for one actual frame must agree.
        ImageTelemetrySnapshot mergedResult;
        const ImageTelemetrySeries* published = &series;
        if (QFileInfo::exists(path)) {
            const auto previous = load(series, cancel);
            if (previous.status == Status::Cancelled ||
                previous.status == Status::Stale ||
                previous.status == Status::Error)
                return previous;
            if (previous.series) {
                require(
                    previous.series->timelineOriginNs ==
                        series.timelineOriginNs,
                    "Clock changed under identical source/decoder identity");
                auto merged = std::make_shared<ImageTelemetrySeries>(series);
                bool changed = false, differsFromPrevious = false;
                auto sameCell = [](const ImageTelemetrySlot& a,
                                   const ImageTelemetrySlot& b) {
                    return a.visited == b.visited &&
                           a.layoutSupported == b.layoutSupported &&
                           a.presentationPtsNs == b.presentationPtsNs &&
                           a.sourcePtsNs == b.sourcePtsNs &&
                           a.values == b.values;
                };
                for (std::size_t i = 0; i < series.cells.size(); ++i) {
                    if (i % 1024 == 0) checkCancel(cancel);
                    const auto& before = previous.series->cells[i];
                    auto& after = merged->cells[i];
                    if (before.presentationPtsNs && after.presentationPtsNs)
                        require(before.presentationPtsNs ==
                                        after.presentationPtsNs &&
                                    before.sourcePtsNs == after.sourcePtsNs,
                                "Conflicting actual frame timestamps under "
                                "identical cache identity");
                    if (!after.presentationPtsNs && before.presentationPtsNs) {
                        after.presentationPtsNs = before.presentationPtsNs;
                        after.sourcePtsNs = before.sourcePtsNs;
                    }
                    after.visited |= before.visited;
                    after.layoutSupported |= before.layoutSupported;
                    for (std::size_t field = 0; field < 4; ++field) {
                        if (before.values[field] && after.values[field])
                            require(before.values[field] == after.values[field],
                                    "Conflicting known image observations "
                                    "under identical cache identity");
                        if (!after.values[field])
                            after.values[field] = before.values[field];
                    }
                    changed |= !sameCell(after, series.cells[i]);
                    differsFromPrevious |= !sameCell(after, before);
                }
                auto revision =
                    std::max(series.revision, previous.series->revision);
                if (changed || (differsFromPrevious &&
                                series.revision <= previous.series->revision)) {
                    require(
                        revision < std::numeric_limits<std::uint64_t>::max(),
                        "Image snapshot revision overflow");
                    ++revision;
                }
                if (changed || revision != series.revision) {
                    merged->revision = revision;
                    validate(*merged, cancel);
                    mergedResult = std::move(merged);
                    published = mergedResult.get();
                }
            }
        }
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            throw Failure(Status::Error, file.errorString());
        if (!file.setPermissions(QFileDevice::ReadOwner |
                                 QFileDevice::WriteOwner))
            throw Failure(
                Status::Error,
                QStringLiteral("Cannot restrict image cache permissions"));
        Compressor compressor(file, cancel);
        encode(compressor, *published, cancel);
        checkCancel(cancel);
        current(*published, cancel);
        safePath(root_, *published, QFileInfo(path).fileName(), false);
        checkCancel(cancel);
        if (!file.commit()) throw Failure(Status::Error, file.errorString());
        return {published->complete() ? Status::Complete : Status::Partial,
                mergedResult,
                path,
                {}};
    } catch (const Failure& error) {
        return failed(error, path);
    } catch (const std::exception& error) {
        return {Status::Error, {}, path, QString::fromUtf8(error.what())};
    }
}
}  // namespace omatrack
