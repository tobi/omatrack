// Finder delivers documents as QFileOpenEvent, including during application
// startup.
#pragma once

#include <QFileOpenEvent>
#include <QObject>
#include <QStringList>

#include <functional>
#include <utility>

namespace omatrack {
class FileOpenEvents : public QObject {
public:
    using Handler = std::function<void(const QStringList&)>;
    explicit FileOpenEvents(QObject* application) : QObject(application) {
        application->installEventFilter(this);
    }
    void setHandler(Handler handler) {
        handler_ = std::move(handler);
        if (handler_ && !pending_.isEmpty()) {
            const auto paths = std::exchange(pending_, {});
            handler_(paths);
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::FileOpen)
            return QObject::eventFilter(watched, event);
        const auto* opened = static_cast<QFileOpenEvent*>(event);
        const QString path = opened->url().isLocalFile()
                                 ? opened->url().toLocalFile()
                                 : opened->file();
        // No remote URL schemes or I/O in the event handler.
        if (path.isEmpty()) return false;
        if (handler_)
            handler_(QStringList{path});
        else
            pending_.append(path);
        event->accept();
        return true;
    }

private:
    Handler handler_;
    QStringList pending_;
};
}  // namespace omatrack
