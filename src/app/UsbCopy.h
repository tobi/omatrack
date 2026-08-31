// Read-only USB import planning, then explicitly requested create-only copying.
#pragma once

#include <QDateTime>
#include <QStringList>
#include <QVector>
#include <functional>

namespace omatrack {
struct UsbCopyOptions {
    QString destination;
    QString format;
    QString script;
    QString track;
    QString date;
    QString session;
};
struct UsbCopyEntry {
    enum class State { Ready, Existing, Invalid };
    QString source;
    QString destination;
    QString relative;
    qint64 size = 0;
    QDateTime modified;
    State state = State::Invalid;
    QString message;
};
struct UsbCopyPlan {
    UsbCopyOptions options;
    QVector<UsbCopyEntry> entries;
    qint64 totalBytes = 0;
    int ready = 0;
    int existing = 0;
    int invalid = 0;
};
struct UsbCopyResult {
    int copied = 0;
    int skipped = 0;
    qint64 completedBytes = 0;
    bool cancelled = false;
    QString error;
};
using CopyCancelled = std::function<bool()>;
// Completed planned bytes, including a target that appeared and was skipped.
using CopyProgress = std::function<void(qint64)>;

// Worker-only. Planning performs no writes and never copies a source.
UsbCopyPlan planUsbCopy(QStringList files, const UsbCopyOptions& options,
                        CopyCancelled cancelled = {});
// Worker-only. Immutable plan; no overwrite, streaming cancellation, and no
// published partial recordings. Cancellation cannot interrupt a blocked OS I/O.
UsbCopyResult executeUsbCopy(const UsbCopyPlan& plan,
                             CopyCancelled cancelled = {},
                             CopyProgress progress = {});
}  // namespace omatrack
