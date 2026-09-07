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
           b.right() <= 1 && b.bottom() <= 1;
}
double overlap(const QRectF& a, const QRectF& b) {
    const auto r = a.intersected(b);
    const double i = r.isEmpty() ? 0 : r.width() * r.height();
    const double u = a.width() * a.height() + b.width() * b.height() - i;
    return u > 0 ? i / u : 0;
}
}  // namespace
bool GaugeSetup::valid() const {
    if (sourceSize.width() <= 0 || sourceSize.height() <= 0 ||
        sourceSize.width() > 16384 || sourceSize.height() > 16384 ||
        regions.isEmpty() || regions.size() > 32 || detectorIdentity.isEmpty())
        return false;
    QSet<QString> ids;
    for (const auto& r : regions) {
        if (!validBox(r.box) || r.id.isEmpty() || ids.contains(r.id) ||
            r.semantic.size() > 64 || r.representation.size() > 64 ||
            r.direction.size() > 32)
            return false;
        ids.insert(r.id);
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
    if (rows.size() > 32) return {};
    for (const auto& value : rows) {
        const auto m = value.toMap();
        GaugeRegion r;
        r.id = m.value("id").toString();
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
    if (setup.sourceSize.isValid() && setup.sourceSize != size) {
        // Preserve proposals visibly, but reset validation. Source geometry is
        // part of the config and reader compatibility will reject rescaling.
        for (auto& r : setup.regions) {
            r.hits = 0;
            r.confirmed = false;
        }
    }
    setup.sourceSize = size;
    QSet<int> used;
    for (auto& r : setup.regions) {
        int best = -1;
        double bestOverlap = .5;
        for (int i = 0; i < observations.size(); ++i) {
            const auto& o = observations[i];
            if (used.contains(i) || !validBox(o.box)) continue;
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
                o.representation == r.representation) {
                r.hits = std::min(20, r.hits + 1);
                r.misses = 0;
                r.score = o.score;
                continue;
            }
        }
        r.hits = std::max(0, r.hits - 1);
        ++r.misses;
        if (r.misses >= 2) r.confirmed = false;
    }
    setup.regions.erase(
        std::remove_if(setup.regions.begin(), setup.regions.end(),
                       [](const auto& r) {
                           return r.misses >= 3 && !r.edited && !r.confirmed &&
                                  !r.proposal;
                       }),
        setup.regions.end());
    for (int i = 0; i < observations.size() && setup.regions.size() < 32; ++i) {
        if (used.contains(i) || !validBox(observations[i].box)) continue;
        auto r = observations[i];
        do {
            r.id = QStringLiteral("g%1").arg(nextId_++);
        } while (std::any_of(
            setup.regions.cbegin(), setup.regions.cend(),
            [&r](const auto& existing) { return existing.id == r.id; }));
        r.confirmed = false;
        r.hits = 1;
        setup.regions.append(r);
    }
    return true;
}
}  // namespace omatrack
