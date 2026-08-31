#pragma once

#include <QSet>
#include <QString>
#include <algorithm>
#include <memory>
#include <vector>

namespace omatrack {

// Discovery reconciles the registry, not the user's analysis selection.
// Preserve selected snapshots even when a volume is temporarily unavailable.
// A pending role load may target any indexed session, so defer pruning until
// that intent has settled. Moving unique_ptrs never moves their session data.
template <typename Session, typename Path>
void retainDiscoveredSessions(std::vector<std::unique_ptr<Session>>& sessions,
                              const QSet<QString>& present,
                              const Session* primary, const Session* reference,
                              bool selectionPending, Path path) {
    if (selectionPending) return;
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [&](const auto& session) {
                                      return session.get() != primary &&
                                             session.get() != reference &&
                                             !present.contains(path(*session));
                                  }),
                   sessions.end());
}

}  // namespace omatrack
