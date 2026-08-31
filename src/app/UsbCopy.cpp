#include "UsbCopy.h"

#include "LuaRename.h"
#include "PathJail.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <limits>

namespace omatrack {
namespace {
bool stopped(const CopyCancelled& cancelled) {
    return cancelled && cancelled();
}
bool occupied(const QString& path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymbolicLink();
}
QString collisionKey(QString path) {
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}
}  // namespace

UsbCopyPlan planUsbCopy(QStringList files, const UsbCopyOptions& options,
                        CopyCancelled cancelled) {
    UsbCopyPlan plan;
    plan.options = options;
    files.removeDuplicates();
    files.sort();
    QHash<QString, int> targets;
    const QString format = options.format.trimmed().isEmpty()
                               ? defaultCopyFormat()
                               : options.format;
    for (const auto& source : files) {
        if (stopped(cancelled)) return {};
        const QFileInfo info(source);
        UsbCopyEntry entry;
        entry.source = source;
        entry.size = info.size();
        entry.modified = info.lastModified();
        const QVariantMap context{
            {QStringLiteral("track"), options.track.isEmpty()
                                          ? QStringLiteral("unknown")
                                          : options.track},
            {QStringLiteral("date"), options.date},
            {QStringLiteral("session"), options.session.isEmpty()
                                            ? QStringLiteral("session")
                                            : options.session},
            {QStringLiteral("original"), info.fileName()},
            {QStringLiteral("stem"), info.completeBaseName()},
            {QStringLiteral("ext"), info.suffix()},
            {QStringLiteral("driver"), QString()},
            {QStringLiteral("index"), plan.entries.size() + 1},
            {QStringLiteral("size"), entry.size},
        };
        QString relative;
        if (!options.script.trimmed().isEmpty()) {
            const auto lua = runLuaRename(options.script, context);
            if (!lua.ok)
                entry.message = lua.error;
            else
                relative = lua.relativePath;
        }
        if (entry.message.isEmpty() && relative.trimmed().isEmpty()) {
            static const QRegularExpression token(
                QStringLiteral("\\{([A-Za-z0-9_]+)\\}"));
            auto matches = token.globalMatch(format);
            while (matches.hasNext()) {
                const QString key = matches.next().captured(1);
                if (!context.contains(key)) {
                    entry.message =
                        QStringLiteral("Unknown naming token: {%1}").arg(key);
                    break;
                }
            }
            relative = expandCopyFormat(format, context);
        }
        if (entry.message.isEmpty() && (!info.isFile() || !info.isReadable()))
            entry.message =
                QStringLiteral("Source is unavailable or unreadable");
        if (entry.message.isEmpty()) {
            const auto jailed = jailRelativePath(options.destination, relative);
            if (!jailed.ok)
                entry.message = jailed.error;
            else {
                entry.destination = jailed.absolutePath;
                entry.relative = jailed.relativePath;
                entry.state = occupied(entry.destination)
                                  ? UsbCopyEntry::State::Existing
                                  : UsbCopyEntry::State::Ready;
                entry.message = entry.state == UsbCopyEntry::State::Existing
                                    ? QStringLiteral(
                                          "Existing target — skipped, contents "
                                          "not verified")
                                    : QStringLiteral("New file");
                const QString key = collisionKey(entry.destination);
                if (targets.contains(key)) {
                    auto& previous = plan.entries[targets.value(key)];
                    previous.state = entry.state = UsbCopyEntry::State::Invalid;
                    previous.message = entry.message = QStringLiteral(
                        "Multiple sources resolve to this target; change the "
                        "naming rule");
                } else
                    targets.insert(key, plan.entries.size());
            }
        }
        plan.entries.append(std::move(entry));
    }
    for (auto& entry : plan.entries) {
        if (entry.state == UsbCopyEntry::State::Ready &&
            (entry.size < 0 || entry.size > std::numeric_limits<qint64>::max() -
                                                plan.totalBytes)) {
            entry.state = UsbCopyEntry::State::Invalid;
            entry.message = QStringLiteral("Invalid or excessive planned size");
        }
        switch (entry.state) {
            case UsbCopyEntry::State::Ready:
                ++plan.ready;
                plan.totalBytes += entry.size;
                break;
            case UsbCopyEntry::State::Existing: ++plan.existing; break;
            case UsbCopyEntry::State::Invalid: ++plan.invalid; break;
        }
    }
    return plan;
}

UsbCopyResult executeUsbCopy(const UsbCopyPlan& plan, CopyCancelled cancelled,
                             CopyProgress progress) {
    UsbCopyResult result;
    if (plan.invalid) {
        result.error =
            QStringLiteral("Resolve invalid preview rows before copying");
        return result;
    }
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    const auto report = [&]() {
        if (progress) progress(result.completedBytes);
    };
    for (const auto& entry : plan.entries) {
        if (stopped(cancelled)) {
            result.cancelled = true;
            break;
        }
        if (entry.state == UsbCopyEntry::State::Existing) {
            ++result.skipped;
            continue;
        }
        if (entry.state != UsbCopyEntry::State::Ready) {
            result.error = QStringLiteral("Invalid copy plan");
            break;
        }
        const QFileInfo sourceInfo(entry.source);
        if (!sourceInfo.isFile() || sourceInfo.size() != entry.size ||
            sourceInfo.lastModified() != entry.modified) {
            result.error =
                QStringLiteral(
                    "Source changed or disappeared; refresh preview: %1")
                    .arg(entry.source);
            break;
        }
        auto jailed =
            jailRelativePath(plan.options.destination, entry.relative);
        if (!jailed.ok || jailed.absolutePath != entry.destination) {
            result.error =
                QStringLiteral("Destination changed since preview: %1")
                    .arg(entry.relative);
            break;
        }
        if (occupied(entry.destination)) {
            ++result.skipped;
            result.completedBytes += entry.size;
            report();
            continue;
        }
        const QString parent = QFileInfo(entry.destination).absolutePath();
        if (!QDir().mkpath(parent)) {
            result.error =
                QStringLiteral("Cannot create destination: %1").arg(parent);
            break;
        }
        jailed = jailRelativePath(plan.options.destination, entry.relative);
        if (!jailed.ok || jailed.absolutePath != entry.destination) {
            result.error =
                QStringLiteral("Destination changed while preparing copy");
            break;
        }
        QFile source(entry.source);
        QTemporaryFile temporary(QDir(parent).filePath(
            QStringLiteral(".omatrack-copy-XXXXXX.part")));
        if (!source.open(QIODevice::ReadOnly) || !temporary.open()) {
            result.error =
                QStringLiteral(
                    "Cannot open source or temporary destination for %1")
                    .arg(entry.relative);
            break;
        }
        qint64 copied = 0;
        for (;;) {
            if (stopped(cancelled)) {
                result.cancelled = true;
                break;
            }
            const qint64 count = source.read(buffer.data(), buffer.size());
            if (count < 0) {
                result.error = source.errorString();
                break;
            }
            if (!count) break;
            if (temporary.write(buffer.constData(), count) != count) {
                result.error = temporary.errorString();
                break;
            }
            copied += count;
            result.completedBytes += count;
            report();
            if (copied > entry.size) {
                result.error = QStringLiteral("Source grew during copy");
                break;
            }
        }
        if (stopped(cancelled)) result.cancelled = true;
        if (result.cancelled || !result.error.isEmpty()) break;
        const QFileInfo after(entry.source);
        if (copied != entry.size || after.size() != entry.size ||
            after.lastModified() != entry.modified || !after.exists()) {
            result.error = QStringLiteral(
                "Source changed during copy; temporary file discarded");
            break;
        }
        if (!temporary.flush() ||
            !temporary.setFileTime(entry.modified,
                                   QFileDevice::FileModificationTime)) {
            result.error =
                QStringLiteral("Cannot finish temporary destination for %1")
                    .arg(entry.relative);
            break;
        }
        temporary.close();
        if (stopped(cancelled)) {
            result.cancelled = true;
            break;
        }
        jailed = jailRelativePath(plan.options.destination, entry.relative);
        if (!jailed.ok || jailed.absolutePath != entry.destination) {
            result.error = QStringLiteral("Destination changed during copy");
            break;
        }
        // QFile::rename is create-only. The temporary lives in the same
        // directory so a complete recording is published in one rename.
        if (!QFile::rename(temporary.fileName(), entry.destination)) {
            if (occupied(entry.destination)) {
                ++result.skipped;
                continue;
            }
            result.error = QStringLiteral("Cannot publish copied file: %1")
                               .arg(entry.relative);
            break;
        }
        temporary.setAutoRemove(false);
        ++result.copied;
        // Apply read-only permissions only after publication: Windows cannot
        // remove a read-only temporary on a cancelled/failed copy.
        if (!QFile::setPermissions(entry.destination,
                                   sourceInfo.permissions())) {
            result.error =
                QStringLiteral(
                    "Copied bytes but could not preserve permissions: %1")
                    .arg(entry.relative);
            break;
        }
    }
    return result;
}
}  // namespace omatrack
