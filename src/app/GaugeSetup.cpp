#include "GaugeSetup.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace omatrack {
namespace {
bool validBox(const QRectF& b) {
    return std::isfinite(b.x()) && std::isfinite(b.y()) &&
           std::isfinite(b.width()) && std::isfinite(b.height()) &&
           b.x() >= 0 && b.y() >= 0 && b.width() > 0 && b.height() > 0 &&
           // Independently rounded YAML x/width or y/height can sum a tiny
           // amount above one. This is subpixel serialization tolerance only;
           // readerField's exact reviewed-crop admission remains unchanged.
           b.right() <= 1 + 1e-9 && b.bottom() <= 1 + 1e-9;
}
double area(const QRectF& r) { return r.width() * r.height(); }
double intersection(const QRectF& a, const QRectF& b) {
    const auto r = a.intersected(b);
    return r.isEmpty() ? 0 : area(r);
}
double overlap(const QRectF& a, const QRectF& b) {
    const double i = intersection(a, b);
    const double u = area(a) + area(b) - i;
    return u > 0 ? i / u : 0;
}
// Named product decisions for the per-frame ballot (see GaugeEvidence).
// A detection re-identifies a track above this IoU.
constexpr double kMatchIou = 0.5;
// Two tracks conflict when their intersection covers this fraction of the
// smaller box: nesting and near-duplicates, not touching neighbours.
constexpr double kConflictFraction = 0.5;
// Weight = votes × area^kAreaExponent. 0.5 makes a box four times the area
// worth twice the votes, so consistent large gauges beat many small glyphs
// without a one-vote screen-sized box beating an established small gauge.
constexpr double kAreaExponent = 0.5;
constexpr int kMaxVotes = 20;
// Consecutive independent frames without a vote before an automatic track
// is dropped and before a validated track loses its confirmation.
constexpr int kMissesToRetire = 3;
constexpr int kMissesToUnconfirm = 2;

bool conflicts(const QRectF& a, const QRectF& b) {
    const double smaller = std::min(area(a), area(b));
    return smaller > 0 && intersection(a, b) / smaller >= kConflictFraction;
}
double weight(const GaugeRegion& r) {
    return r.hits * std::pow(area(r.box), kAreaExponent);
}
}  // namespace
bool GaugeRegion::visible() const {
    return !suppressed && (anchored() || hits >= GaugeSetup::VotesToShow);
}
void GaugeSetup::pruneInvisible() {
    regions.erase(std::remove_if(regions.begin(), regions.end(),
                                 [](const auto& r) { return !r.visible(); }),
                  regions.end());
}
bool GaugeSetup::valid() const {
    if (sourceSize.width() <= 0 || sourceSize.height() <= 0 ||
        sourceSize.width() > 16384 || sourceSize.height() > 16384 ||
        regions.isEmpty() ||
        regions.size() > MaxInventoryRegions + MaxProfileRegions ||
        detectorIdentity.isEmpty())
        return false;
    QSet<QString> ids, profiles;
    int inventory = 0;
    for (const auto& r : regions) {
        if (!validBox(r.box) || r.id.isEmpty() || ids.contains(r.id) ||
            r.semantic.size() > 64 || r.representation.size() > 64 ||
            r.direction.size() > 32 || r.detectorIdentity.size() > 512)
            return false;
        ids.insert(r.id);
        if (r.profileKey.isEmpty()) {
            if (++inventory > MaxInventoryRegions) return false;
        } else {
            if (!QStringList{"gear", "stint_lap", "brake", "throttle"}.contains(
                    r.profileKey) ||
                profiles.contains(r.profileKey))
                return false;
            profiles.insert(r.profileKey);
        }
    }
    return true;
}
QVariantMap GaugeSetup::toMap() const {
    // Canonicalize at omatrack.yml's numeric precision BEFORE hashing.
    // Otherwise a cold restart changes the cache key when YAML rounds
    // normalized doubles. This is far below one source pixel and does not relax
    // reader admission.
    const auto coordinate = [](double value) {
        return QString::number(value, 'g', 10).toDouble();
    };
    QVariantList rows;
    for (const auto& r : regions)
        rows.append(QVariantMap{{"id", r.id},
                                {"profile_key", r.profileKey},
                                {"detector_identity", r.detectorIdentity},
                                {"representation", r.representation},
                                {"semantic", r.semantic},
                                {"direction", r.direction},
                                {"x", coordinate(r.box.x())},
                                {"y", coordinate(r.box.y())},
                                {"width", coordinate(r.box.width())},
                                {"height", coordinate(r.box.height())},
                                {"enabled", r.enabled},
                                {"confirmed", r.confirmed},
                                {"edited", r.edited}});
    return {{"schema", 1},
            {"source_width", sourceSize.width()},
            {"source_height", sourceSize.height()},
            {"detector_identity", detectorIdentity},
            {"regions", rows}};
}
GaugeSetup GaugeSetup::fromMap(const QVariantMap& map) {
    GaugeSetup s;
    if (map.value("schema").toInt() != 1) return s;
    s.sourceSize = {map.value("source_width").toInt(),
                    map.value("source_height").toInt()};
    s.detectorIdentity = map.value("detector_identity").toString();
    const auto rows = map.value("regions").toList();
    if (rows.size() > MaxInventoryRegions + MaxProfileRegions) return {};
    for (const auto& value : rows) {
        const auto m = value.toMap();
        GaugeRegion r;
        r.id = m.value("id").toString();
        r.profileKey = m.value("profile_key").toString();
        r.detectorIdentity = m.value("detector_identity").toString();
        r.representation = m.value("representation").toString();
        r.semantic = m.value("semantic").toString();
        r.direction = m.value("direction").toString();
        r.box = {m.value("x").toDouble(), m.value("y").toDouble(),
                 m.value("width").toDouble(), m.value("height").toDouble()};
        r.enabled = m.value("enabled", true).toBool();
        r.confirmed = m.value("confirmed").toBool();
        r.edited = m.value("edited").toBool();
        s.regions.append(r);
    }
    return s.valid() ? s : GaugeSetup{};
}
QString GaugeSetup::fingerprint() const {
    if (!valid()) return {};
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument::fromVariant(toMap()).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}
QString GaugeSetup::readingFingerprint() const {
    if (!valid()) return {};
    auto map = toMap();
    QList<QByteArray> selected;
    for (const auto& value : map.value("regions").toList()) {
        auto row = value.toMap();
        if (!row.value("enabled").toBool() || !row.value("confirmed").toBool())
            continue;
        row.remove("id");
        row.remove("edited");
        selected.append(
            QJsonDocument::fromVariant(row).toJson(QJsonDocument::Compact));
    }
    std::sort(selected.begin(), selected.end());
    QVariantList rows;
    for (const auto& row : selected)
        rows.append(QJsonDocument::fromJson(row).toVariant());
    map["regions"] = rows;
    map["identity_scope"] = QStringLiteral("confirmed-selection-v1");
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument::fromVariant(map).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256)
            .toHex());
}
QVector<GaugeRegion> GaugeSetup::reviewedRegions() {
    const QRect boxes[] = {{1399, 1010, 76, 69},
                           {408, 994, 71, 50},
                           {956, 628, 43, 266},
                           {1011, 628, 44, 266}};
    const QStringList fields{"gear", "stint_lap", "brake", "throttle"};
    QVector<GaugeRegion> rows;
    for (int i = 0; i < 4; ++i) {
        GaugeRegion r;
        r.semantic = fields[i];
        r.profileKey = fields[i];
        r.representation =
            i < 2 ? QStringLiteral("digits") : QStringLiteral("bar");
        r.direction =
            i < 2 ? QStringLiteral("unknown") : QStringLiteral("bottom_to_top");
        r.box = {boxes[i].x() / 1920.0, boxes[i].y() / 1080.0,
                 boxes[i].width() / 1920.0, boxes[i].height() / 1080.0};
        r.score = 1;  // structural heuristic matched, NOT a probability
        rows.append(r);
    }
    return rows;
}
int GaugeSetup::readerField(const GaugeRegion& r, QSize sourceSize) {
    if (sourceSize != QSize(1920, 1080)) return -1;
    const auto reviewed = reviewedRegions();
    for (int i = 0; i < reviewed.size(); ++i) {
        const auto& expected = reviewed[i];
        if (r.semantic != expected.semantic ||
            r.representation != expected.representation ||
            r.direction != expected.direction)
            continue;
        // Subpixel serialization tolerance only; never a loose IoU admission.
        const auto delta = r.box.topLeft() - expected.box.topLeft();
        if (std::abs(delta.x()) < 1e-9 && std::abs(delta.y()) < 1e-9 &&
            std::abs(r.box.width() - expected.box.width()) < 1e-9 &&
            std::abs(r.box.height() - expected.box.height()) < 1e-9)
            return i;
    }
    return -1;
}
std::array<bool, 4> GaugeSetup::readableFields() const {
    std::array<bool, 4> fields{};
    std::array<int, 4> counts{};
    for (const auto& r : regions) {
        const int i = readerField(r, sourceSize);
        if (i >= 0 && r.enabled && r.confirmed) ++counts[i];
    }
    for (int i = 0; i < 4; ++i) fields[i] = counts[i] == 1;
    return fields;
}
inference::GaugeReadConfiguration GaugeSetup::readerConfiguration(
    bool reviewedOnly) const {
    using namespace inference;
    GaugeReadConfiguration result;
    result.sourceWidth = sourceSize.width();
    result.sourceHeight = sourceSize.height();
    const QStringList fields{"gear", "stint_lap", "brake", "throttle"};
    std::array<int, 4> counts{};
    for (const auto& r : regions) {
        const int field = fields.indexOf(r.semantic);
        if (!r.enabled || !r.confirmed || field < 0 ||
            r.representation != (field < 2 ? QStringLiteral("digits")
                                           : QStringLiteral("bar")) ||
            (reviewedOnly && readerField(r, sourceSize) < 0))
            continue;
        auto direction = GaugeFillDirection::Unknown;
        if (r.direction == "bottom_to_top")
            direction = GaugeFillDirection::BottomToTop;
        else if (r.direction == "top_to_bottom")
            direction = GaugeFillDirection::TopToBottom;
        else if (r.direction == "left_to_right")
            direction = GaugeFillDirection::LeftToRight;
        else if (r.direction == "right_to_left")
            direction = GaugeFillDirection::RightToLeft;
        if (field >= 2 && direction == GaugeFillDirection::Unknown) continue;
        ++counts[field];
        result.crops[field] = {
            int(std::floor(r.box.x() * sourceSize.width() + 1e-7)),
            int(std::floor(r.box.y() * sourceSize.height() + 1e-7)),
            int(std::ceil(r.box.right() * sourceSize.width() - 1e-7)),
            int(std::ceil(r.box.bottom() * sourceSize.height() - 1e-7)),
            true,
            direction};
    }
    for (int i = 0; i < 4; ++i) result.crops[i].enabled = counts[i] == 1;
    return result;
}
void GaugeEvidence::clear() {
    setup = {};
    reviewedLayoutVerified_ = false;
    seen_.clear();
    nextId_ = 1;
}
void GaugeEvidence::propose(GaugeSetup proposal) {
    clear();
    if (!proposal.valid()) return;
    setup = std::move(proposal);
    for (auto& r : setup.regions) {
        r.confirmed = false;
        r.proposal = true;
        r.hits = r.misses = 0;
    }
}
bool GaugeEvidence::observe(qint64 pts, QSize size,
                            const QVector<GaugeRegion>& observations) {
    if (pts < 0 || size.width() <= 0 || size.height() <= 0 ||
        size.width() > 16384 || size.height() > 16384 || seen_.size() >= 30000)
        return false;
    // Independence means at least two seconds from EVERY previously used PTS,
    // including frames revisited after a seek, not just a wall-clock timeout.
    for (const auto stamp : seen_)
        if (std::abs(pts - stamp) < 2'000'000'000LL) return false;
    seen_.insert(pts);
    // Runtime evidence only, never restored from extension/per-file proposals.
    QSet<QString> observedProfiles;
    for (const auto& observation : observations)
        if (!observation.profileKey.isEmpty() &&
            GaugeSetup::readerField(observation, size) >= 0)
            observedProfiles.insert(observation.profileKey);
    reviewedLayoutVerified_ =
        observedProfiles.size() == GaugeSetup::MaxProfileRegions;
    if (setup.sourceSize.isValid() && setup.sourceSize != size) {
        // Preserve proposals visibly, but reset validation. Source geometry is
        // part of the config and reader compatibility will reject rescaling.
        for (auto& r : setup.regions) {
            r.hits = 0;
            r.confirmed = false;
        }
    }
    setup.sourceSize = size;
    // Ballot: every existing track either collects this frame's vote or
    // loses one.
    QSet<int> used;
    for (auto& r : setup.regions) {
        int best = -1;
        double bestOverlap = kMatchIou;
        for (int i = 0; i < observations.size(); ++i) {
            const auto& o = observations[i];
            if (used.contains(i) || !validBox(o.box) ||
                r.profileKey != o.profileKey ||
                (r.profileKey.isEmpty() &&
                 r.detectorIdentity != o.detectorIdentity))
                continue;
            if (!r.profileKey.isEmpty()) {
                // Consume this stable profile anchor even after the user moves,
                // relabels or disables it. Never respawn an enabled canonical
                // duplicate over their edit or match a learned glyph box to it.
                best = i;
                break;
            }
            const double iou = overlap(r.box, o.box);
            if (iou > bestOverlap) {
                best = i;
                bestOverlap = iou;
            }
        }
        if (best >= 0) {
            used.insert(best);
            const auto& o = observations[best];
            if (o.semantic == r.semantic &&
                o.representation == r.representation &&
                (r.profileKey.isEmpty() ||
                 (o.direction == r.direction &&
                  GaugeSetup::readerField(r, size) >= 0))) {
                r.hits = std::min(kMaxVotes, r.hits + 1);
                r.misses = 0;
                r.score = o.score;
                continue;
            }
        }
        r.hits = std::max(0, r.hits - 1);
        ++r.misses;
        if (r.misses >= kMissesToUnconfirm) r.confirmed = false;
    }
    setup.regions.erase(
        std::remove_if(setup.regions.begin(), setup.regions.end(),
                       [](const auto& r) {
                           return r.misses >= kMissesToRetire && !r.userOwned();
                       }),
        setup.regions.end());
    // Unmatched detections become hidden candidates with one vote; they show
    // once a second independent frame agrees.
    for (int i = 0; i < observations.size(); ++i) {
        if (used.contains(i) || !validBox(observations[i].box)) continue;
        auto r = observations[i];
        const int inventory = std::count_if(
            setup.regions.cbegin(), setup.regions.cend(),
            [](const auto& region) { return region.profileKey.isEmpty(); });
        if ((r.profileKey.isEmpty() &&
             inventory >= GaugeSetup::MaxInventoryRegions) ||
            (!r.profileKey.isEmpty() &&
             setup.regions.size() - inventory >= GaugeSetup::MaxProfileRegions))
            continue;
        do {
            r.id = QStringLiteral("g%1").arg(nextId_++);
        } while (std::any_of(
            setup.regions.cbegin(), setup.regions.cend(),
            [&r](const auto& existing) { return existing.id == r.id; }));
        r.confirmed = false;
        r.hits = 1;
        r.misses = 0;
        setup.regions.append(r);
    }
    resolveOverlaps();
    return true;
}
void GaugeEvidence::resolveOverlaps() {
    // Anchored tracks keep their place unconditionally (a user may deliberately
    // overlap two edits). Automatic tracks are admitted heaviest first and
    // suppressed when they conflict with anything already admitted. A
    // candidate below the vote threshold is hidden anyway and cannot
    // suppress anything: one frame's screen-sized box never hides an
    // established gauge. Order is deterministic: weight, then area, then id.
    QVector<int> automatic, kept;
    for (int i = 0; i < setup.regions.size(); ++i) {
        auto& r = setup.regions[i];
        r.suppressed = false;
        if (r.anchored())
            kept.append(i);
        else
            automatic.append(i);
    }
    std::sort(automatic.begin(), automatic.end(), [this](int a, int b) {
        const auto& ra = setup.regions[a];
        const auto& rb = setup.regions[b];
        const double wa = weight(ra), wb = weight(rb);
        if (wa != wb) return wa > wb;
        const double aa = area(ra.box), ab = area(rb.box);
        if (aa != ab) return aa > ab;
        return ra.id < rb.id;
    });
    for (const int i : automatic) {
        auto& r = setup.regions[i];
        for (const int k : kept)
            if (conflicts(r.box, setup.regions[k].box)) {
                r.suppressed = true;
                break;
            }
        if (!r.suppressed && r.hits >= GaugeSetup::VotesToShow) kept.append(i);
    }
}
}  // namespace omatrack
