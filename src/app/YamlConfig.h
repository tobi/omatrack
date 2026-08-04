// YAML-backed application configuration.
//
// `racecraft.yml` is the single source of truth for user configuration:
// telemetry directories, channel display, driver naming, last selection, and
// per-track corner overrides. It lives at $XDG_CONFIG_HOME/racecraft (or
// ~/.config/racecraft) so it can be read, diffed, and edited by hand.
//
// Track Atlas data, decoded telemetry, and caches stay out of it: the file
// holds only what the user chose, never what an upstream source provided.
#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace racecraft {

class YamlConfig {
public:
    /// Process-wide document. Loaded once, saved explicitly.
    static YamlConfig& instance();

    /// Absolute path of racecraft.yml (parent directory is created).
    static QString filePath();

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

    /// Persist to disk atomically. No-op when nothing changed.
    void save();

    /// True when the file did not exist at load time (fresh install).
    bool isFresh() const { return fresh_; }

private:
    YamlConfig();
    void load();

    QVariantMap root_;
    bool fresh_ = false;
    bool dirty_ = false;
};

}  // namespace racecraft
