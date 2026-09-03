// Pixel-budget lane sizing and splitter math; no store, I/O or scene graph.
#pragma once

#include <QString>
#include <algorithm>
#include <cmath>
#include <vector>

namespace trace {
constexpr double minimumLaneHeight = 20.0;

inline double validLaneWeight(double value) {
    return std::isfinite(value) && value > 0.0 ? value : 1.0;
}
inline double laneHeightBoost(const QString& key) {
    return key == QStringLiteral("speed") ? 1.35 : 1.0;
}

// Proportional allocation with a readable minimum, reduced only when the
// pane cannot fit that minimum for every lane. Normalize before summing so
// hand-edited, very large finite weights cannot overflow.
inline std::vector<double> fitLaneHeights(std::vector<double> weights,
                                          double available) {
    std::vector<double> heights(weights.size(), 0.0);
    if (weights.empty() || !std::isfinite(available) || available <= 0)
        return heights;
    double largest = 1.0;
    for (double& weight : weights) {
        weight = validLaneWeight(weight);
        largest = std::max(largest, weight);
    }
    const double floor =
        std::min(minimumLaneHeight, available / weights.size());
    double remaining = available, total = 0.0;
    for (size_t i = 0; i < weights.size(); ++i) {
        weights[i] /= largest;
        if (weights[i] == 0) {
            heights[i] = floor;
            remaining -= floor;
        } else
            total += weights[i];
    }
    bool changed;
    do {
        changed = false;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (weights[i] <= 0 || total <= 0) continue;
            if (remaining * (weights[i] / total) < floor) {
                heights[i] = floor;
                remaining -= floor;
                total -= weights[i];
                weights[i] = 0;
                changed = true;
            }
        }
    } while (changed);
    for (size_t i = 0; i < weights.size(); ++i)
        if (weights[i] > 0 && total > 0)
            heights[i] = std::max(0.0, remaining) * (weights[i] / total);
    return heights;
}

// Move the divider after `upper`. Start with the neighbour; once it reaches
// its minimum, borrow from the next lane. This lets one drag make a trace
// nearly pane-sized rather than stopping at the neighbour's original height.
inline std::vector<double> resizeLaneBoundary(
    const std::vector<double>& original, size_t upper, double delta) {
    auto heights = original;
    if (upper + 1 >= heights.size() || !std::isfinite(delta)) return heights;
    double floor = minimumLaneHeight;
    for (double height : heights) {
        if (!std::isfinite(height) || height < 0) return original;
        floor = std::min(floor, height);
    }
    double needed = std::abs(delta);
    const size_t receiver = delta >= 0 ? upper : upper + 1;
    if (delta >= 0) {
        for (size_t i = upper + 1; i < heights.size() && needed > 0; ++i) {
            const double moved = std::min(needed, heights[i] - floor);
            heights[i] -= moved;
            heights[receiver] += moved;
            needed -= moved;
        }
    } else {
        for (size_t i = upper + 1; i > 0 && needed > 0;) {
            --i;
            const double moved = std::min(needed, heights[i] - floor);
            heights[i] -= moved;
            heights[receiver] += moved;
            needed -= moved;
        }
    }
    return heights;
}
}  // namespace trace
