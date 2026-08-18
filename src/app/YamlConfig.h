// YAML-backed application configuration.
//
// `omatrack.yml` is the source of truth for application-wide user
// configuration: telemetry directories, channel display, driver naming, last
// selection, per-video overrides, per-track corner overrides, and portable
// AppImage update checks. Portable
// folder metadata lives in hierarchical TRACK.yml files. omatrack.yml lives
// under the platform's standard
// configuration directory (or `$XDG_CONFIG_HOME` when set) so it can be read,
// diffed, and edited by hand.
//
// Track Atlas data, decoded telemetry, and caches stay out of it: the file
// holds only what the user chose, never what an upstream source provided.
#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace omatrack {

class YamlConfig {
public:
    /// Process-wide document. Loaded once, saved explicitly.
    static YamlConfig& instance();

    /// Absolute path of omatrack.yml (parent directory is created).
    static QString filePath();

    /// Parse an arbitrary YAML mapping without changing it.
    static QVariantMap readDocument(const QString& path,
                                    QString* errorString = nullptr);

    /// Atomically replace an arbitrary YAML mapping. This does not affect the
    /// process-wide omatrack.yml document.
    static bool writeDocument(const QString& path, const QVariantMap& document,
                              QString* errorString = nullptr);

    /// Read a value addressed by nested map keys; keys are used verbatim.
    QVariant value(const QStringList& path,
                   const QVariant& fallback = QVariant()) const;
    /// Convenience for paths without '/' in any key.
    QVariant value(const QString& slashPath,
                   const QVariant& fallback = QVariant()) const;

    /// Write a value, creating intermediate maps. An invalid value removes it.
    void setValue(const QStringList& path, const QVariant& value);
    void setValue(const QString& slashPath, const QVariant& value);

    /// Nested map at `path`, or an empty map when absent.
    QVariantMap map(const QStringList& path) const;
    void setMap(const QStringList& path, const QVariantMap& value);

    void remove(const QStringList& path);

    /// Persist to disk atomically. Synchronous: serialises `root_` and writes
    /// it through `QSaveFile`, checking `write()`/`commit()`. Returns false and
    /// fills `errorString` (and `this->errorString()`) on a commit failure. No
    /// call (returns true) when nothing changed since the last successful save.
    bool save(QString* errorString = nullptr);

    /// Serialise the process-wide document to bytes on the calling (GUI)
    /// thread. Cheap: no filesystem work. The bytes include the leading
    /// comment line so they can be written verbatim by `writeDocumentBytes`.
    QByteArray serializeDocument() const;

    /// Atomically write `bytes` to `path` through `QSaveFile`, checking both
    /// `write()` and `commit()`. Used by `save()` and the debounced async
    /// preference writer so they share one checked write path.
    static bool writeDocumentBytes(const QString& path, const QByteArray& bytes,
                                   QString* errorString = nullptr);

    /// Mark the document as having unsaved changes (used to retry after an
    /// async write failure).
    void markDirty() { dirty_ = true; }
    /// Clear the dirty flag after a snapshot has been captured for an async
    /// write, so changes made during the write re-mark it.
    void clearDirty() { dirty_ = false; }
    bool isDirty() const { return dirty_; }

    /// True when the file did not exist at load time (fresh install).
    bool isFresh() const { return fresh_; }
    /// Existing malformed/unreadable configuration is never overwritten.
    bool isWritable() const { return writable_; }
    QString errorString() const { return errorString_; }
    // Public so tests can create instances pointed at a temp config dir.
    // Production code uses instance().
    YamlConfig();

private:
    void load();

    QVariantMap root_;
    bool fresh_ = false;
    bool writable_ = true;
    QString errorString_;
    bool dirty_ = false;
};

}  // namespace omatrack
