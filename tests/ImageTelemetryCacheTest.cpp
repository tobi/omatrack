// origin: PUBLIC — all samples, timestamps and file bytes here are synthetic.
#include "app/ImageTelemetryCache.h"
#include "omatrack_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <zstd.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace omatrack;
using namespace omatrack::inference;
using Status = ImageTelemetryCache::Status;
namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void status(const ImageTelemetryCache::Result& result, Status expected) {
    if (result.status != expected)
        throw std::runtime_error("Unexpected cache status " +
                                 std::to_string(int(result.status)) + ": " +
                                 result.error.toStdString());
}
void write(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    check(file.open(QIODevice::WriteOnly), "fixture open");
    check(file.write(bytes) == bytes.size() && file.commit(),
          "fixture atomic write");
}
QByteArray read(const QString& path) {
    QFile file(path);
    check(file.open(QIODevice::ReadOnly), "fixture read");
    return file.readAll();
}
QByteArray plain(const QString& path) {
    const auto compressed = read(path);
    QByteArray bytes(8 * 1024 * 1024, Qt::Uninitialized);
    const auto n =
        ZSTD_decompress(bytes.data(), std::size_t(bytes.size()),
                        compressed.constData(), std::size_t(compressed.size()));
    check(!ZSTD_isError(n), "fixture decompress");
    bytes.resize(qsizetype(n));
    return bytes;
}
void compress(const QString& path, const QByteArray& bytes) {
    QByteArray compressed(
        qsizetype(ZSTD_compressBound(std::size_t(bytes.size()))),
        Qt::Uninitialized);
    const auto n =
        ZSTD_compress(compressed.data(), std::size_t(compressed.size()),
                      bytes.constData(), std::size_t(bytes.size()), 1);
    check(!ZSTD_isError(n), "fixture compress");
    compressed.resize(qsizetype(n));
    write(path, compressed);
}
void setFrame(ImageTelemetrySeries& series, std::size_t index,
              bool supported = true) {
    auto& slot = series.cells[index];
    slot.visited = true;
    slot.layoutSupported = supported;
    slot.presentationPtsNs =
        std::int64_t(index) * ImageTelemetryPeriodNs + 3'000'001;
    slot.sourcePtsNs = series.timelineOriginNs + *slot.presentationPtsNs;
}
void identical(const ImageTelemetrySeries& a, const ImageTelemetrySeries& b) {
    check(a.durationNs == b.durationNs &&
              a.timelineOriginNs == b.timelineOriginNs &&
              a.cells.size() == b.cells.size(),
          "roundtrip shape/clock");
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        const auto& x = a.cells[i];
        const auto& y = b.cells[i];
        check(x.visited == y.visited &&
                  x.layoutSupported == y.layoutSupported &&
                  x.presentationPtsNs == y.presentationPtsNs &&
                  x.sourcePtsNs == y.sourcePtsNs && x.values == y.values,
              "roundtrip sample/mask mismatch");
    }
}
void parserRoundtrip(const QString& path, std::size_t cells) {
    // Normal public file dispatcher, not a test-only cache decoder. Both plain
    // and zstd .telemetry encodings are exercised by the Rust bridge tests.
    void* handle = omatrack_open(path.toUtf8().constData());
    check(handle != nullptr,
          "normal parser could not open standard .telemetry cache");
    check(omatrack_channel_count(handle) == 11,
          "normal parser lost image/mask channels");
    check(omatrack_source_lap_count(handle) == 0, "fabricated lap metadata");
    bool foundActualPts = false;
    for (std::size_t c = 0; c < omatrack_channel_count(handle); ++c) {
        check(omatrack_channel_sample_count(handle, c) == cells,
              "normal parser wrong sample count");
        check(bool(omatrack_channel_visible(handle, c)) == (c < 4),
              "auxiliary mask/timestamp visibility lost by generic dispatcher");
        check(omatrack_chunk_period_ns(handle, c, 0) == ImageTelemetryPeriodNs,
              "normal parser wrong sample clock");
        if (QByteArray(omatrack_channel_name(handle, c)) ==
            "image_derived_presentation_pts_ns") {
            std::vector<double> values(cells);
            check(omatrack_channel_decode_all(handle, c, values.data(),
                                              values.size()) == cells,
                  "normal parser short decode");
            check(values[0] == 3'000'001 && std::isnan(values[2]),
                  "actual timestamp/gap was replaced with lattice or held "
                  "sample");
            foundActualPts = true;
        }
    }
    omatrack_close(handle);
    check(foundActualPts, "missing actual decoded-PTS channel");
}

void run() {
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary fixture directory");
    const auto inputs = temporary.filePath(QStringLiteral("inputs"));
    const auto root = temporary.filePath(QStringLiteral("cache"));
    const auto copiedModels =
        temporary.filePath(QStringLiteral("copied-models"));
    check(QDir().mkpath(inputs) && QDir().mkpath(copiedModels),
          "fixture mkdir");
    const auto source = QDir(inputs).filePath(QStringLiteral("fixture.video"));
    const auto model = QDir(inputs).filePath(QStringLiteral("fixture.onnx"));
    write(source, "PUBLIC synthetic source bytes, not a recording\n");
    write(model, "PUBLIC synthetic model bytes, no weights\n");
    const auto sourceBefore = read(source);
    const auto modelBefore = read(model);
    ImageTelemetryCache cache(root);
    // An epoch-sized origin with low bits that cannot survive binary64.
    constexpr std::int64_t origin = 1'700'000'000'000'000'001;
    auto prepared = cache.prepare(source, model, 950'000'001, origin, true);
    status(prepared, Status::Ready);
    check(prepared.series && prepared.series->cells.size() == 5,
          "ceil(duration/200ms)");
    status(cache.load(*prepared.series), Status::Miss);
    auto partial = *prepared.series;
    partial.revision = 7;
    setFrame(partial, 0);
    partial.cells[0].values = {2., 11., .25, 75.};
    setFrame(partial, 1,
             false);       // visited unsupported, actual frame remains known
    setFrame(partial, 3);  // supported but every field unreadable
    partial.cells[4].visited =
        true;  // terminal gap, no frame, not a held prediction
    status(cache.save(partial), Status::Partial);
    auto loaded = cache.load(*prepared.series);
    status(loaded, Status::Partial);
    identical(partial, *loaded.series);
    check(loaded.series->visitedCount() == 4 && !loaded.series->complete() &&
              loaded.series->revision == 7,
          "partial coverage mislabeled complete");
    check(loaded.series->cells[0].sourcePtsNs == origin + 3'000'001,
          "large raw source timestamp lost integer precision");
    parserRoundtrip(loaded.path, partial.cells.size());
    const auto document = plain(loaded.path);
    check(document.endsWith('\n') && document.contains("\"q\":200000000") &&
              document.contains("\"dur\":1000000000"),
          "MTJ lattice/header shape");
    check(!document.contains("\"vpts\"") && !document.contains("\"vf\"") &&
              !document.contains("\"utc\""),
          "invented video frame table or UTC metadata");
    check(document.contains("image_predictions_not_gold") &&
              document.contains("\"native_used\":\"false\""),
          "missing prediction provenance");

    // Renderer lookup is bounded and O(1); unknown cells remain represented.
    check(partial.slotRange(-1, 1) ==
              std::make_pair(std::size_t(0), std::size_t(1)),
          "range at start");
    check(partial.slotRange(200'000'000, 600'000'000) ==
              std::make_pair(std::size_t(1), std::size_t(3)),
          "range exact boundaries");
    check(partial.slotRange(600'000'000, 600'000'000).first ==
              partial.slotRange(600'000'000, 600'000'000).second,
          "empty range");
    check(ImageTelemetrySeries::slotCount(0) == 0 &&
              ImageTelemetrySeries::slotCount(ImageTelemetryMaxDurationNs +
                                              1) == 0,
          "duration allocation bound");

    auto invalid = partial;
    invalid.identity.nativeTelemetryAbsent = false;
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells[0].layoutSupported = false;
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells[2].values[0] = 0.;
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells[0].presentationPtsNs = ImageTelemetryPeriodNs;
    invalid.cells[0].sourcePtsNs = origin + ImageTelemetryPeriodNs;
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells[0].sourcePtsNs = *invalid.cells[0].sourcePtsNs + 1;
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells[0].values[2] = std::numeric_limits<double>::quiet_NaN();
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells[0].values[3] = 100.01;
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.timelineOriginNs = std::numeric_limits<std::int64_t>::max();
    status(cache.save(invalid), Status::Invalid);
    invalid = partial;
    invalid.cells.pop_back();
    status(cache.save(invalid), Status::Invalid);
    status(
        cache.prepare(source, model, ImageTelemetryMaxDurationNs + 1, 0, true),
        Status::Invalid);

    // An unverified/zero-origin prepare can reuse a previously verified cache,
    // without opening a decoder, constructing a reader or linking ONNX Runtime.
    auto probe = cache.prepare(source, model, partial.durationNs, 0, false);
    status(probe, Status::Ready);
    auto reused = cache.load(*probe.series);
    status(reused, Status::Partial);
    check(reused.series->identity.nativeTelemetryAbsent &&
              reused.series->timelineOriginNs == origin,
          "cached absence/origin not restored");
    const auto copiedModel =
        QDir(copiedModels).filePath(QStringLiteral("identical.onnx"));
    check(QFile::copy(model, copiedModel), "copy identical model");
    auto relocated =
        cache.prepare(source, copiedModel, partial.durationNs, 0, false);
    status(relocated, Status::Ready);
    check(cache.pathFor(*relocated.series) == cache.pathFor(partial),
          "identical model bytes relocated must reuse cache key");
    status(cache.load(*relocated.series), Status::Partial);
    auto revision = partial;
    revision.identity.layoutRevision += "-changed";
    status(cache.load(revision), Status::Miss);
    revision = partial;
    revision.identity.decoderRevision += "-changed";
    status(cache.load(revision), Status::Miss);
    revision = partial;
    revision.identity.schemaRevision += "-changed";
    status(cache.load(revision), Status::Invalid);

    auto cancellation = std::make_shared<std::atomic<bool>>(true);
    const auto beforeCancelled = read(loaded.path);
    QLockFile competingWriter(loaded.path + QStringLiteral(".lock"));
    check(competingWriter.tryLock(0), "test competing-writer lock");
    status(cache.save(partial), Status::Error);
    check(read(loaded.path) == beforeCancelled,
          "competing writer bypassed publication lock");
    competingWriter.unlock();
    status(cache.save(partial, cancellation), Status::Cancelled);
    status(cache.load(partial, cancellation), Status::Cancelled);
    check(read(loaded.path) == beforeCancelled,
          "cancelled save changed the previous cache");
    auto retained = cache.save(*prepared.series);
    status(retained, Status::Partial);
    check(retained.series && retained.series->visitedCount() == 4,
          "late empty draft lost prior coverage");
    auto lessKnown = partial;
    lessKnown.cells[0].values[0].reset();
    auto retainedKnown = cache.save(lessKnown);
    status(retainedKnown, Status::Partial);
    check(retainedKnown.series && retainedKnown.series->cells[0].values[0] ==
                                      partial.cells[0].values[0],
          "unknown replaced known during merge");

    auto complete = partial;
    complete.cells[2].visited =
        true;  // explicitly scanned terminal/unreadable gap
    ++complete.revision;
    status(cache.save(complete), Status::Complete);
    auto full = cache.load(*probe.series);
    status(full, Status::Complete);
    identical(complete, *full.series);
    const auto latePartial = cache.save(partial);
    status(latePartial, Status::Complete);
    check(latePartial.series && latePartial.series->complete(),
          "late partial regressed complete coverage");
    const auto completeBytes = read(full.path);

    auto disjointReady = cache.prepare(source, model, 800'000'000, 0, true);
    status(disjointReady, Status::Ready);
    auto writerA = *disjointReady.series, writerB = *disjointReady.series;
    setFrame(writerA, 0);
    writerA.cells[0].values[0] = 1.;
    setFrame(writerB, 1);
    writerB.cells[1].values[1] = 3.;
    status(cache.save(writerA), Status::Partial);
    const auto unionResult = cache.save(writerB);
    status(unionResult, Status::Partial);
    check(unionResult.series && unionResult.series->visitedCount() == 2 &&
              unionResult.series->cells[0].values[0] == 1. &&
              unionResult.series->cells[1].values[1] == 3.,
          "disjoint writers lost observations");
    auto conflict = *unionResult.series;
    conflict.cells[0].values[0] = 2.;
    status(cache.save(conflict), Status::Invalid);
    conflict = *unionResult.series;
    ++*conflict.cells[0].presentationPtsNs;
    ++*conflict.cells[0].sourcePtsNs;
    status(cache.save(conflict), Status::Invalid);
    auto united = cache.load(*disjointReady.series);
    status(united, Status::Partial);
    identical(*unionResult.series, *united.series);

    // Tamper only owned synthetic cache fixtures to exercise fail-closed loads.
    auto corrupted = plain(full.path);
    check(corrupted.contains("\"visited_slots\":\"5\""),
          "coverage provenance fixture");
    corrupted.replace("\"visited_slots\":\"5\"", "\"visited_slots\":\"4\"");
    compress(full.path, corrupted);
    status(cache.load(*probe.series), Status::Invalid);
    corrupted = document;
    corrupted.replace("\"complete\":\"0\"", "\"complete\":\"1\"");
    compress(full.path, corrupted);
    status(cache.load(*probe.series), Status::Invalid);
    corrupted = document;
    corrupted.replace("\"native_telemetry\":\"absent\"",
                      "\"native_telemetry\":\"unverified\"");
    compress(full.path, corrupted);
    status(cache.load(*probe.series), Status::Invalid);
    corrupted = document;
    corrupted.replace(
        "\"image_derived_gear_known\",\"hz\":5,\"vis\":0,\"v\":[1",
        "\"image_derived_gear_known\",\"hz\":5,\"vis\":0,\"v\":[0");
    compress(full.path, corrupted);
    status(cache.load(*probe.series), Status::Invalid);
    write(full.path, completeBytes.left(completeBytes.size() / 2));
    status(cache.load(*probe.series), Status::Invalid);
    write(full.path, completeBytes + "trailing");
    status(cache.load(*probe.series), Status::Invalid);
    write(full.path, completeBytes);

    ImageTelemetryCache besideSource(inputs);
    status(besideSource.save(complete), Status::Invalid);
    check(read(source) == sourceBefore && read(model) == modelBefore,
          "cache modified source/model bytes");

    // A long, compressible snapshot exceeds a decompressor's 64KiB output
    // block.
    auto longPrepared = cache.prepare(source, model, 600'000'000'000, 0, true);
    status(longPrepared, Status::Ready);
    auto longSeries = *longPrepared.series;
    for (auto& slot : longSeries.cells) slot.visited = true;
    status(cache.save(longSeries), Status::Complete);
    status(cache.load(*longPrepared.series), Status::Complete);

    auto negativeOrigin =
        cache.prepare(source, model, 300'000'000, -10'000'000, true);
    status(negativeOrigin, Status::Ready);
    auto negativeClock = *negativeOrigin.series;
    setFrame(negativeClock, 0);
    negativeClock.cells[0].values[0] = 0.;
    status(cache.save(negativeClock), Status::Partial);
    auto negativeRestored = cache.load(*negativeOrigin.series);
    status(negativeRestored, Status::Partial);
    identical(negativeClock, *negativeRestored.series);

    // Synthetic float32-derived percentages exercise MTJ's decimal codec,
    // unlike simple integer/.25 fixtures. Re-saving unchanged observations
    // must not conflict merely because the interchange parser rounded an ULP.
    for (int mode = 0; mode < 2; ++mode) {
        auto decimalReady = cache.prepare(
            source, model, (4096 + mode) * ImageTelemetryPeriodNs, 0, true);
        status(decimalReady, Status::Ready);
        auto decimalSeries = *decimalReady.series;
        std::mt19937 random(20260905);
        for (std::size_t i = 0; i < decimalSeries.cells.size(); ++i) {
            setFrame(decimalSeries, i);
            const float fraction = std::ldexp(
                float(0x800000 + (random() & 0x7fffff)), -24 - int(i % 24));
            decimalSeries.cells[i].values[2] =
                mode == 0 ? double(fraction) * 100.
                          : std::generate_canonical<double, 53>(random) * 100.;
        }
        // Match progressive collection: save a first partial batch, grow it,
        // and reload before completing. The original bug retained only batch1.
        auto progressive = *decimalReady.series;
        std::copy_n(decimalSeries.cells.begin(), 32, progressive.cells.begin());
        progressive.revision = 1;
        status(cache.save(progressive), Status::Partial);
        std::copy_n(decimalSeries.cells.begin(), 128,
                    progressive.cells.begin());
        progressive.revision = 2;
        status(cache.save(progressive), Status::Partial);
        auto resumedDecimals = cache.load(*decimalReady.series);
        status(resumedDecimals, Status::Partial);
        check(resumedDecimals.series->visitedCount() == 128,
              "later decimal coverage was not persisted");
        identical(progressive, *resumedDecimals.series);
        decimalSeries.revision = 3;
        status(cache.save(decimalSeries), Status::Complete);
        const auto decimals = cache.load(decimalSeries);
        status(decimals, Status::Complete);
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < decimalSeries.cells.size(); ++i)
            mismatches += decimalSeries.cells[i].values[2] !=
                          decimals.series->cells[i].values[2];
        std::cout << "decimal parity mode=" << mode
                  << " mismatches=" << mismatches << '\n';
        status(cache.save(decimalSeries), Status::Complete);
        check(mismatches == 0,
              "MTJ decimal parser changed finite observation bits");
    }

    // File/model changes must invalidate old identities; no stale reuse.
    write(model, modelBefore + "changed");
    status(cache.load(*probe.series), Status::Stale);
    auto newModel =
        cache.prepare(source, model, partial.durationNs, origin, true);
    status(newModel, Status::Ready);
    status(cache.load(*newModel.series), Status::Miss);
    write(source, sourceBefore + "changed");
    status(cache.load(*newModel.series), Status::Stale);
    auto newSource =
        cache.prepare(source, model, partial.durationNs, origin, true);
    status(newSource, Status::Ready);
    status(cache.load(*newSource.series), Status::Miss);
    std::cout
        << "PASS image-cache MTJ/Rust roundtrip; partial/resume/complete; "
           "exact PTS/origin; masks; model relocation; "
           "stale/revision/cancellation/corruption; source isolation\n";
}
}  // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
