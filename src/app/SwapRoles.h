#pragma once

namespace omatrack {

/// True when a primary/reference swap can proceed. The store no-ops when
/// no reference session is set (hotkey X and the filmstrip menu).
inline bool swapRolesPossible(bool hasReference) { return hasReference; }

// Viewports may extend into neighbouring laps. Map the known interval, but
// preserve overscroll instead of clamping it away when roles change. Manual
// alignment is a normalized translation and can be inverted exactly.
template <typename Mapping>
double swappedViewportFraction(double fraction, bool manual, double shift,
                               Mapping map) {
    if (manual) return fraction - shift;
    if (fraction < 0.0) return map(0.0) + fraction;
    if (fraction > 1.0) return map(1.0) + fraction - 1.0;
    return map(fraction);
}

}  // namespace omatrack
