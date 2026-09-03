#pragma once

namespace omatrack {

/// True when a primary/reference swap can proceed. The store no-ops when
/// no reference session is set (hotkey X and the filmstrip menu). The swap
/// keeps cursor and viewport fractions in place; only the roles change.
inline bool swapRolesPossible(bool hasReference) { return hasReference; }

}  // namespace omatrack
