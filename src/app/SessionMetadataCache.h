#pragma once

#include <QJsonObject>
#include <QString>

class SessionMetadataCache {
public:
    struct Lookup {
        bool found = false;
        bool supported = false;
        QJsonObject metadata;
    };

    explicit SessionMetadataCache(QString path);

    static QString fingerprint(const QString& path);

    Lookup lookup(const QString& fingerprint);
    void store(const QString& fingerprint, const QString& canonicalPath,
               bool supported, const QJsonObject& metadata);
    bool save();

private:
    static constexpr int kSchemaVersion = 5;
    static constexpr qint64 kMaxEntryAgeMs = 90LL * 24 * 60 * 60 * 1000;

    QString path_;
    QJsonObject entries_;
    QJsonObject dirtyEntries_;
};
