// One entry in the telemetry library: a local folder, or a connection to an
// outside server.
//
// The library is a single ordered list of heterogeneous sources. A folder is
// scanned in place; a connection is synchronized into a local cache directory
// and that cache is scanned, so everything downstream of discovery sees plain
// local paths and never learns which kind of source produced them.
//
// WebDAV is the only connection type today. Adding another one means adding a
// LocationType, teaching syncConnection() how to populate a cache directory
// for it, and listing it in connectionTypes() — the preferences UI builds its
// "Connect" menu from that list and needs no change.
#pragma once

#include <QString>

namespace omatrack {

enum class LocationType {
    Folder,  // a directory on this machine, scanned where it sits
    WebDav,  // an authenticated http(s) WebDAV collection
};

struct LibraryLocation {
    QString id;
    LocationType type = LocationType::Folder;
    /// User-facing label. Empty means "derive one from the target".
    QString name;
    /// Local absolute path for Folder, the collection URL for WebDav.
    QString target;
    QString username;
    QString password;
    /// Disabled locations stay configured but are skipped by every scan.
    bool enabled = true;

    bool isConnection() const { return type != LocationType::Folder; }
};

/// Stable `type` strings for omatrack.yml and the QML bridge.
QString locationTypeKey(LocationType type);
LocationType locationTypeFromKey(const QString& key, bool* ok = nullptr);

}  // namespace omatrack
