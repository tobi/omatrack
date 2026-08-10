#include "LibraryLocation.h"

namespace omatrack {

QString locationTypeKey(LocationType type) {
    switch (type) {
        case LocationType::WebDav: return QStringLiteral("webdav");
        case LocationType::Folder: break;
    }
    return QStringLiteral("folder");
}

LocationType locationTypeFromKey(const QString& key, bool* ok) {
    if (ok) *ok = true;
    if (key == QStringLiteral("webdav")) return LocationType::WebDav;
    if (key == QStringLiteral("folder")) return LocationType::Folder;
    if (ok) *ok = false;
    return LocationType::Folder;
}

}  // namespace omatrack
