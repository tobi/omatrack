// Cursor-priority scheduling for a finite, progressively filled video timeline.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>

namespace omatrack::inference {

class ImageScanScheduler {
public:
    void fromCursor(std::size_t slot) { next_ = slot; }

    /// The current cursor wins. In ahead mode fill the remainder then wrap and
    /// backfill earlier holes, so starting in the middle can still complete a
    /// whole recording. A visited-but-unknown slot is not a hole.
    template <typename IsVisited>
    std::optional<std::size_t> next(std::size_t count, std::size_t cursor,
                                    bool ahead, IsVisited visited) {
        if (!count) return {};
        cursor = std::min(cursor, count - 1);
        if (!visited(cursor)) {
            next_ = (cursor + 1) % count;
            return cursor;
        }
        if (!ahead) return {};
        next_ %= count;
        for (std::size_t tried = 0; tried < count; ++tried) {
            const std::size_t candidate = next_;
            next_ = (next_ + 1) % count;
            if (!visited(candidate)) return candidate;
        }
        return {};
    }

private:
    std::size_t next_ = 0;
};
}  // namespace omatrack::inference
