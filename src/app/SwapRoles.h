#pragma once

namespace omatrack {

/// True when a primary/reference swap can proceed. The store no-ops when
/// no reference session is set (hotkey X and the filmstrip menu).
inline bool swapRolesPossible(bool hasReference) { return hasReference; }

}  // namespace omatrack
