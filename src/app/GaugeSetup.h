#pragma once

#include "inference/GaugeReader.h"
#include <QRectF>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <array>

namespace omatrack {

// Source-normalized geometry. No display coordinates or physical units.
struct GaugeRegion {
    QString id, representation = QStringLiteral("unknown"),
                semantic = QStringLiteral("unknown"),
                direction = QStringLiteral("unknown");
    // Nonempty only for one of the four independently image-verified profile
    // anchors. It survives user edits and never comes from the learned
    // detector.
    QString profileKey;
    // Actual learned backend/content identity; profile anchors leave this
    // empty.
    QString detectorIdentity;
    QRectF box;
    bool enabled = true, confirmed = false, edited = false;
    bool proposal =
        false;  // runtime protection for persisted/user-confirmed setup
    int hits = 0, misses = 0;
    double score = 0;
};
struct GaugeSetup {
    static constexpr int MaxInventoryRegions = 32;
    static constexpr int MaxProfileRegions = 4;
    QSize sourceSize;
    QString detectorIdentity;
    QVector<GaugeRegion> regions;
    bool valid() const;
    QVariantMap toMap() const;
    static GaugeSetup fromMap(const QVariantMap& map);
    QString fingerprint() const;
    // Extraction identity excludes disabled inventory and transient track IDs,
    // but includes every selected confirmed crop, source size and backend.
    QString readingFingerprint() const;
    // Compatibility is deliberately separate from localization/confirmation.
    // Until independently validated, arbitrary crops must not enter the reader.
    std::array<bool, 4> readableFields() const;
    inference::GaugeReadConfiguration readerConfiguration(
        bool reviewedOnly = true) const;
    static QVector<GaugeRegion> reviewedRegions();
    static int readerField(const GaugeRegion& region, QSize sourceSize);
};

// GUI-owned temporal state, fed only worker observations with actual decoded
// PTS. A seek cancels a job but does not make a previously seen frame
// independent.
class GaugeEvidence {
public:
    GaugeSetup setup;
    bool observe(qint64 ptsNs, QSize sourceSize,
                 const QVector<GaugeRegion>& observations);
    void propose(GaugeSetup proposal);
    void clear();
    int sampleCount() const { return seen_.size(); }
    bool reviewedLayoutVerified() const { return reviewedLayoutVerified_; }

private:
    bool reviewedLayoutVerified_ = false;
    QSet<qint64> seen_;
    int nextId_ = 1;
};

}  // namespace omatrack
