// Embedded libmpv video item for the Qt Quick scene.

#pragma once

#include <QByteArray>
#include <QList>
#include <QQuickFramebufferObject>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <QUrl>

#include <memory>

struct MpvSharedState;
class MpvVideoRenderer;

class MpvVideoItem : public QQuickFramebufferObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY seekingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QUrl source READ source NOTIFY sourceChanged)

public:
    explicit MpvVideoItem(QQuickItem* parent = nullptr);
    ~MpvVideoItem() override;

    Renderer* createRenderer() const override;

    bool ready() const { return ready_; }
    bool loaded() const { return loaded_; }
    bool paused() const { return paused_; }
    bool muted() const { return muted_; }
    bool seeking() const { return seeking_; }
    double position() const { return position_; }
    double duration() const { return duration_; }
    double volume() const { return volume_; }
    const QString& title() const { return title_; }
    const QString& errorString() const { return errorString_; }
    const QUrl& source() const { return source_; }

    void setPaused(bool paused);
    void setMuted(bool muted);
    void setVolume(double volume);

    Q_INVOKABLE void openMedia(const QUrl& source);
    Q_INVOKABLE void closeMedia();
    Q_INVOKABLE void togglePaused();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void frameStep();

signals:
    void readyChanged();
    void loadedChanged();
    void pausedChanged();
    void mutedChanged();
    void seekingChanged();
    void positionChanged();
    void durationChanged();
    void volumeChanged();
    void titleChanged();
    void errorStringChanged();
    void sourceChanged();

private slots:
    void processEvents();
    void requestVideoFrame();
    void markRendererReady();
    void markRendererFailed(const QString& message);

private:
    friend class MpvVideoRenderer;

    static void wakeup(void* context);
    void loadPendingMedia();
    void setError(const QString& message);
    int command(const QList<QByteArray>& arguments);

    std::shared_ptr<MpvSharedState> state_;
    QUrl source_;
    QUrl pendingSource_;
    QString title_;
    QString errorString_;
    double position_ = 0.0;
    double duration_ = 0.0;
    double volume_ = 75.0;
    bool ready_ = false;
    bool loaded_ = false;
    bool paused_ = true;
    bool muted_ = false;
    bool seeking_ = false;
    bool autoplayPending_ = false;
};
