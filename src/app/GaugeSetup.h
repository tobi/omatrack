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
    // Runtime only: lost an overlap contest to a heavier track this frame.
    // Keeps voting; shown again if it outweighs the winner later.
    bool suppressed = false;
    // hits is the vote count: +1 per independent frame that re-detects the
    // track, -1 per independent frame that does not. misses counts
    // consecutive frames without a match.
    int hits = 0, misses = 0;
    double score = 0;
    // A user edit, a visual confirmation or a persisted proposal: misses mark
    // it stale but never retire it.
    bool userOwned() const { return edited || confirmed || proposal; }
    // User-owned, or one of the reviewed structural profile anchors: shown
    // from its first frame and never outvoted or hidden by an automatic track.
    bool anchored() const { return userOwned() || !profileKey.isEmpty(); }
    // What the setup panel lists and the video overlays. An automatic learned
    // track must be seen on two independent frames before it appears at all,
    // and must not currently be suppressed by an overlapping heavier track.
    bool visible() const;
};
struct GaugeSetup {
    static constexpr int MaxInventoryRegions = 32;
    static constexpr int MaxProfileRegions = 4;
    // Independent-frame votes before an automatic learned track is shown, and
    // before an enabled track can be confirmed without visual confirmation.
    static constexpr int VotesToShow = 2;
    static constexpr int VotesToConfirm = 3;
    QSize sourceSize;
    QString detectorIdentity;
    QVector<GaugeRegion> regions;
    bool valid() const;
    // Drops automatic tracks that are not visible (below the vote threshold
    // or suppressed) so a saved setup carries evidence, not per-frame noise.
    void pruneInvisible();
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
//
// Every independent frame is one ballot. A detection that overlaps an
// existing track by IoU > 0.5 (same backend, same representation and
// semantic) votes for it; a track no frame voted for loses a vote. New
// detections enter as hidden candidates with one vote. After the ballot,
// overlapping tracks are resolved: anchored tracks (profile anchors, user
// edits, confirmations, persisted proposals) always keep their place; among
// automatic tracks the heavier one wins, where weight is votes × √area, so a
// gauge four times the area counts double and one consistently seen large
// box beats a cloud of small glyph boxes inside it. Losers are suppressed,
// not deleted: they keep voting and reappear if they outweigh the winner.
// Two boxes conflict when the intersection covers at least half of the
// smaller one, so slightly touching neighbours coexist but nesting does not.
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
    void resolveOverlaps();
    bool reviewedLayoutVerified_ = false;
    QSet<qint64> seen_;
    int nextId_ = 1;
};

}  // namespace omatrack
