// Sandboxed Lua 5.4 rename scripts for USB copy.
//
// Lives in src/app: omatrack_core stays Qt- and Lua-free. The sandbox opens
// only base/string/table/math/utf8, strips load/loadfile/dofile/setmetatable,
// caps memory (string.rep bypasses the instruction hook), and aborts on a
// COUNT hook plus wall-clock. ctx is a plain table of strings and numbers.
#pragma once

#include <QString>
#include <QVariantMap>

namespace omatrack {

struct LuaRenameResult {
    bool ok = false;
    QString relativePath;
    QString error;
};

/// Default example shown in preferences — not loaded as a plugin.
QString exampleLuaRenameScript();

/// Run `script` off whatever thread the caller is on. `rename(ctx)` must
/// return a relative path string. Empty script is a no-op (ok, empty path)
/// so the format string remains the default layout.
LuaRenameResult runLuaRename(const QString& script, const QVariantMap& ctx,
                             int timeoutMs = 50,
                             size_t memoryCapBytes = 1024 * 1024);

}  // namespace omatrack
