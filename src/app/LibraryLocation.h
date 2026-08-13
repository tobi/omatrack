// One entry in the telemetry library: a local folder, or a connection to an
// outside server.
//
// The library is a single ordered list of heterogeneous sources. A folder is
// scanned in place; a connection is synchronized into a local cache directory
// and that cache is scanned, so everything downstream of discovery sees plain
// local paths and never learns which kind of source produced them.
//
// Adding a connection type means adding a LocationType, teaching
// syncConnection() how to populate a cache directory for it, and listing it in
// connectionTypes() — the preferences UI builds its "Connect" menu and its
// form fields from that list and needs no change.
#pragma once

#include <QMap>
#include <QString>

namespace omatrack {

enum class LocationType {
    Folder,  // a directory on this machine, scanned where it sits
    WebDav,  // an authenticated http(s) WebDAV collection
    S3,      // an Amazon S3 bucket, or anything that speaks its API
    Gcs,     // a Google Cloud Storage bucket, through its S3-compatible API
};

struct LibraryLocation {
    QString id;
    LocationType type = LocationType::Folder;
    /// User-facing label. Empty means "derive one from the target".
    QString name;
    /// Local absolute path for Folder, the collection URL for WebDav, and
    /// `s3://bucket/prefix` for S3.
    QString target;
    QString username;
    QString password;
    /// Protocol-specific tuning: an S3 `region`, or an `endpoint` pointing at
    /// something other than AWS. Deliberately not part of `target`, because
    /// the connection id is a hash of the target — a knob added there would
    /// orphan every file already downloaded the first time it was adjusted.
    QMap<QString, QString> options;
    /// Disabled locations stay configured but are skipped by every scan.
    bool enabled = true;
    /// Runtime-only mount discovered by the USB watcher. Never persisted;
    /// omitted from the sidebar when its scan finds no supported files.
    bool transient = false;

    bool isConnection() const { return type != LocationType::Folder; }
};

/// Stable `type` strings for omatrack.yml and the QML bridge.
QString locationTypeKey(LocationType type);
LocationType locationTypeFromKey(const QString& key, bool* ok = nullptr);

}  // namespace omatrack
