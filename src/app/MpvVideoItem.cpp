#include "MpvVideoItem.h"

#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QPointer>
#include <QQuickOpenGLUtils>
#include <QtGui/qguiapplication_platform.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <algorithm>
#include <cstring>
#include <vector>

struct MpvSharedState {
    mpv_handle* handle = nullptr;
    mpv_render_context* renderContext = nullptr;

    ~MpvSharedState() {
        if (handle) mpv_terminate_destroy(handle);
    }
};

namespace {

void* resolveOpenGl(void*, const char* name) {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) return nullptr;
    return reinterpret_cast<void*>(context->getProcAddress(QByteArray(name)));
}

bool setOption(mpv_handle* handle, const char* name, const char* value) {
    const int result = mpv_set_option_string(handle, name, value);
    if (result < 0) {
        qWarning() << "libmpv option failed:" << name
                   << mpv_error_string(result);
        return false;
    }
    return true;
}

}  // namespace

class MpvVideoRenderer final : public QQuickFramebufferObject::Renderer {
public:
    MpvVideoRenderer(std::shared_ptr<MpvSharedState> state, MpvVideoItem* item)
        : state_(std::move(state)), item_(item) {}

    ~MpvVideoRenderer() override {
        if (state_ && state_->renderContext) {
            mpv_render_context_set_update_callback(state_->renderContext,
                                                   nullptr, nullptr);
            mpv_render_context_free(state_->renderContext);
            state_->renderContext = nullptr;
        }
    }

    QOpenGLFramebufferObject* createFramebufferObject(
        const QSize& size) override {
        if (state_ && state_->handle && !state_->renderContext)
            createRenderContext();

        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

    void render() override {
        QQuickOpenGLUtils::resetOpenGLState();
        if (state_ && state_->renderContext) {
            QOpenGLFramebufferObject* target = framebufferObject();
            mpv_opengl_fbo fbo{
                static_cast<int>(target->handle()),
                target->width(),
                target->height(),
                0,
            };
            int flipY = 0;
            mpv_render_param parameters[] = {
                {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                {MPV_RENDER_PARAM_FLIP_Y, &flipY},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            const int result =
                mpv_render_context_render(state_->renderContext, parameters);
            if (result < 0 && !renderErrorReported_) {
                renderErrorReported_ = true;
                notifyFailure(
                    QStringLiteral("Video render failed: %1")
                        .arg(QString::fromUtf8(mpv_error_string(result))));
            }
        }
        QQuickOpenGLUtils::resetOpenGLState();
    }

private:
    static void redraw(void* context) {
        static_cast<MpvVideoRenderer*>(context)->requestUpdate();
    }

    void createRenderContext() {
        mpv_opengl_init_params openGl{resolveOpenGl, nullptr};
        mpv_render_param display{MPV_RENDER_PARAM_INVALID, nullptr};

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN) && !defined(Q_OS_ANDROID) && \
    !defined(Q_OS_HAIKU)
        if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
#if QT_CONFIG(xcb)
            if (auto* native = qGuiApp->nativeInterface<
                               QNativeInterface::QX11Application>()) {
                display.type = MPV_RENDER_PARAM_X11_DISPLAY;
                display.data = native->display();
            }
#endif
        } else if (QGuiApplication::platformName() ==
                   QStringLiteral("wayland")) {
#if QT_CONFIG(wayland)
            if (auto* native = qGuiApp->nativeInterface<
                               QNativeInterface::QWaylandApplication>()) {
                display.type = MPV_RENDER_PARAM_WL_DISPLAY;
                display.data = native->display();
            }
#endif
        }
#endif

        mpv_render_param parameters[] = {
            {MPV_RENDER_PARAM_API_TYPE,
             const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
            {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &openGl},
            display,
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        const int result = mpv_render_context_create(
            &state_->renderContext, state_->handle, parameters);
        if (result < 0) {
            state_->renderContext = nullptr;
            notifyFailure(
                QStringLiteral("Unable to initialize video renderer: %1")
                    .arg(QString::fromUtf8(mpv_error_string(result))));
            return;
        }

        mpv_render_context_set_update_callback(state_->renderContext,
                                               &MpvVideoRenderer::redraw, this);
        if (item_)
            QMetaObject::invokeMethod(item_, "markRendererReady",
                                      Qt::QueuedConnection);
    }

    void requestUpdate() {
        if (item_)
            QMetaObject::invokeMethod(item_, "requestVideoFrame",
                                      Qt::QueuedConnection);
    }

    void notifyFailure(const QString& message) {
        if (item_)
            QMetaObject::invokeMethod(item_, "markRendererFailed",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, message));
    }

    std::shared_ptr<MpvSharedState> state_;
    QPointer<MpvVideoItem> item_;
    bool renderErrorReported_ = false;
};

MpvVideoItem::MpvVideoItem(QQuickItem* parent)
    : QQuickFramebufferObject(parent),
      state_(std::make_shared<MpvSharedState>()) {
    setTextureFollowsItemSize(true);
    setMirrorVertically(false);

    state_->handle = mpv_create();
    if (!state_->handle) {
        setError(QStringLiteral("Unable to create libmpv context"));
        return;
    }

    setOption(state_->handle, "config", "no");
    setOption(state_->handle, "terminal", "no");
    setOption(state_->handle, "input-default-bindings", "no");
    setOption(state_->handle, "input-vo-keyboard", "no");
    setOption(state_->handle, "osc", "no");
    setOption(state_->handle, "vo", "libmpv");
    setOption(state_->handle, "hwdec", "auto-safe");
    setOption(state_->handle, "keep-open", "yes");
    setOption(state_->handle, "pause", "yes");
    setOption(state_->handle, "audio-client-name", "Racecraft");
    setOption(state_->handle, "volume", "75");

    const int result = mpv_initialize(state_->handle);
    if (result < 0) {
        setError(QStringLiteral("Unable to initialize libmpv: %1")
                     .arg(QString::fromUtf8(mpv_error_string(result))));
        mpv_terminate_destroy(state_->handle);
        state_->handle = nullptr;
        return;
    }

    mpv_request_log_messages(state_->handle, "warn");
    mpv_observe_property(state_->handle, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(state_->handle, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(state_->handle, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(state_->handle, 4, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(state_->handle, 5, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(state_->handle, 6, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(state_->handle, 7, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(state_->handle, 8, "speed", MPV_FORMAT_DOUBLE);
    mpv_set_wakeup_callback(state_->handle, &MpvVideoItem::wakeup, this);
}

MpvVideoItem::~MpvVideoItem() {
    if (state_ && state_->handle)
        mpv_set_wakeup_callback(state_->handle, nullptr, nullptr);
}

QQuickFramebufferObject::Renderer* MpvVideoItem::createRenderer() const {
    return new MpvVideoRenderer(state_, const_cast<MpvVideoItem*>(this));
}

void MpvVideoItem::wakeup(void* context) {
    auto* item = static_cast<MpvVideoItem*>(context);
    QMetaObject::invokeMethod(item, "processEvents", Qt::QueuedConnection);
}

void MpvVideoItem::processEvents() {
    if (!state_ || !state_->handle) return;

    while (true) {
        mpv_event* event = mpv_wait_event(state_->handle, 0.0);
        if (!event || event->event_id == MPV_EVENT_NONE) break;

        switch (event->event_id) {
            case MPV_EVENT_START_FILE:
                if (loaded_) {
                    loaded_ = false;
                    emit loadedChanged();
                }
                break;
            case MPV_EVENT_FILE_LOADED:
                if (!loaded_) {
                    loaded_ = true;
                    emit loadedChanged();
                }
                setError(QString());
                if (autoplayPending_) {
                    autoplayPending_ = false;
                    setPaused(false);
                }
                break;
            case MPV_EVENT_END_FILE: {
                const auto* end = static_cast<mpv_event_end_file*>(event->data);
                if (end && end->reason == MPV_END_FILE_REASON_ERROR)
                    setError(QStringLiteral("Video playback failed: %1")
                                 .arg(QString::fromUtf8(
                                     mpv_error_string(end->error))));
                break;
            }
            case MPV_EVENT_SEEK:
                if (!seeking_) {
                    seeking_ = true;
                    emit seekingChanged();
                }
                break;
            case MPV_EVENT_PLAYBACK_RESTART:
                if (seeking_) {
                    seeking_ = false;
                    emit seekingChanged();
                }
                break;
            case MPV_EVENT_PROPERTY_CHANGE: {
                const auto* property =
                    static_cast<mpv_event_property*>(event->data);
                if (!property || !property->name || !property->data) break;
                if (std::strcmp(property->name, "time-pos") == 0 &&
                    property->format == MPV_FORMAT_DOUBLE) {
                    const double value = *static_cast<double*>(property->data);
                    if (!qFuzzyCompare(position_ + 1.0, value + 1.0)) {
                        position_ = std::max(0.0, value);
                        emit positionChanged();
                    }
                } else if (std::strcmp(property->name, "duration") == 0 &&
                           property->format == MPV_FORMAT_DOUBLE) {
                    const double value = *static_cast<double*>(property->data);
                    if (!qFuzzyCompare(duration_ + 1.0, value + 1.0)) {
                        duration_ = std::max(0.0, value);
                        emit durationChanged();
                    }
                } else if (std::strcmp(property->name, "pause") == 0 &&
                           property->format == MPV_FORMAT_FLAG) {
                    const bool value = *static_cast<int*>(property->data) != 0;
                    if (paused_ != value) {
                        paused_ = value;
                        emit pausedChanged();
                    }
                } else if (std::strcmp(property->name, "mute") == 0 &&
                           property->format == MPV_FORMAT_FLAG) {
                    const bool value = *static_cast<int*>(property->data) != 0;
                    if (muted_ != value) {
                        muted_ = value;
                        emit mutedChanged();
                    }
                } else if (std::strcmp(property->name, "volume") == 0 &&
                           property->format == MPV_FORMAT_DOUBLE) {
                    const double value = *static_cast<double*>(property->data);
                    if (!qFuzzyCompare(volume_ + 1.0, value + 1.0)) {
                        volume_ = value;
                        emit volumeChanged();
                    }
                } else if (std::strcmp(property->name, "media-title") == 0 &&
                           property->format == MPV_FORMAT_STRING) {
                    const char* value = *static_cast<char**>(property->data);
                    const QString next = QString::fromUtf8(value ? value : "");
                    if (title_ != next) {
                        title_ = next;
                        emit titleChanged();
                    }
                } else if (std::strcmp(property->name, "seeking") == 0 &&
                           property->format == MPV_FORMAT_FLAG) {
                    const bool value = *static_cast<int*>(property->data) != 0;
                    if (seeking_ != value) {
                        seeking_ = value;
                        emit seekingChanged();
                    }
                } else if (std::strcmp(property->name, "speed") == 0 &&
                           property->format == MPV_FORMAT_DOUBLE) {
                    const double value = *static_cast<double*>(property->data);
                    if (!qFuzzyCompare(playbackRate_ + 1.0, value + 1.0)) {
                        playbackRate_ = value;
                        emit playbackRateChanged();
                    }
                }
                break;
            }
            case MPV_EVENT_COMMAND_REPLY:
            case MPV_EVENT_SET_PROPERTY_REPLY:
                if (event->error < 0)
                    setError(QString::fromUtf8(mpv_error_string(event->error)));
                break;
            case MPV_EVENT_LOG_MESSAGE: {
                const auto* message =
                    static_cast<mpv_event_log_message*>(event->data);
                if (message && message->text)
                    qWarning().noquote() << "libmpv:" << message->text;
                break;
            }
            default: break;
        }
    }
}

void MpvVideoItem::requestVideoFrame() { update(); }

void MpvVideoItem::markRendererReady() {
    if (ready_) return;
    ready_ = true;
    emit readyChanged();
    loadPendingMedia();
}

void MpvVideoItem::markRendererFailed(const QString& message) {
    setError(message);
}

void MpvVideoItem::openMedia(const QUrl& source) {
    if (!source.isValid() || source.isEmpty()) {
        setError(QStringLiteral("No video file selected"));
        return;
    }
    exactSeekCount_ = 0;

    source_ = source;
    pendingSource_ = source;
    position_ = 0.0;
    duration_ = 0.0;
    loaded_ = false;
    autoplayPending_ = true;
    title_ = source.isLocalFile() ? QFileInfo(source.toLocalFile()).fileName()
                                  : source.fileName();
    setError(QString());
    emit sourceChanged();
    emit positionChanged();
    emit durationChanged();
    emit loadedChanged();
    emit titleChanged();
    loadPendingMedia();
}

void MpvVideoItem::loadPendingMedia() {
    if (!ready_ || pendingSource_.isEmpty() || !state_ || !state_->handle)
        return;

    const QUrl source = pendingSource_;
    pendingSource_ = QUrl();
    const QString media = source.isLocalFile()
                              ? source.toLocalFile()
                              : source.toString(QUrl::FullyEncoded);
    const int result = command({
        QByteArrayLiteral("loadfile"),
        media.toUtf8(),
        QByteArrayLiteral("replace"),
    });
    if (result < 0)
        setError(QStringLiteral("Unable to open video: %1")
                     .arg(QString::fromUtf8(mpv_error_string(result))));
}

void MpvVideoItem::closeMedia() {
    pendingSource_ = QUrl();
    autoplayPending_ = false;
    if (state_ && state_->handle) command({QByteArrayLiteral("stop")});
    source_ = QUrl();
    title_.clear();
    position_ = 0.0;
    duration_ = 0.0;
    if (loaded_) {
        loaded_ = false;
        emit loadedChanged();
    }
    emit sourceChanged();
    emit titleChanged();
    emit positionChanged();
    emit durationChanged();
}

void MpvVideoItem::setPaused(bool paused) {
    if (paused_ == paused || !state_ || !state_->handle) return;
    paused_ = paused;
    emit pausedChanged();
    int value = paused ? 1 : 0;
    const int result = mpv_set_property_async(state_->handle, 0, "pause",
                                              MPV_FORMAT_FLAG, &value);
    if (result < 0) setError(QString::fromUtf8(mpv_error_string(result)));
}

void MpvVideoItem::togglePaused() { setPaused(!paused_); }

void MpvVideoItem::setMuted(bool muted) {
    if (muted_ == muted || !state_ || !state_->handle) return;
    muted_ = muted;
    emit mutedChanged();
    int value = muted ? 1 : 0;
    const int result = mpv_set_property_async(state_->handle, 0, "mute",
                                              MPV_FORMAT_FLAG, &value);
    if (result < 0) setError(QString::fromUtf8(mpv_error_string(result)));
}

void MpvVideoItem::setVolume(double volume) {
    volume = std::clamp(volume, 0.0, 100.0);
    if (qFuzzyCompare(volume_ + 1.0, volume + 1.0) || !state_ ||
        !state_->handle)
        return;
    volume_ = volume;
    emit volumeChanged();
    const int result = mpv_set_property_async(state_->handle, 0, "volume",
                                              MPV_FORMAT_DOUBLE, &volume);
    if (result < 0) setError(QString::fromUtf8(mpv_error_string(result)));
}

void MpvVideoItem::setPlaybackRate(double rate) {
    rate = std::clamp(rate, 0.5, 2.0);
    if (qFuzzyCompare(playbackRate_ + 1.0, rate + 1.0) || !state_ ||
        !state_->handle)
        return;
    playbackRate_ = rate;
    emit playbackRateChanged();
    const int result = mpv_set_property_async(state_->handle, 0, "speed",
                                              MPV_FORMAT_DOUBLE, &rate);
    if (result < 0) setError(QString::fromUtf8(mpv_error_string(result)));
}

void MpvVideoItem::seek(double seconds) {
    if (!loaded_) return;
    seconds = std::max(0.0, seconds);
    if (duration_ > 0.0) seconds = std::min(seconds, duration_);
    ++exactSeekCount_;
    command({
        QByteArrayLiteral("seek"),
        QByteArray::number(seconds, 'f', 6),
        QByteArrayLiteral("absolute+exact"),
    });
}

void MpvVideoItem::seekRelative(double seconds) { seek(position_ + seconds); }

void MpvVideoItem::frameStep() {
    if (!loaded_) return;
    setPaused(true);
    command({QByteArrayLiteral("frame-step")});
}

void MpvVideoItem::setError(const QString& message) {
    if (errorString_ == message) return;
    errorString_ = message;
    emit errorStringChanged();
}

int MpvVideoItem::command(const QList<QByteArray>& arguments) {
    if (!state_ || !state_->handle || arguments.isEmpty())
        return MPV_ERROR_UNINITIALIZED;

    std::vector<const char*> pointers;
    pointers.reserve(static_cast<size_t>(arguments.size()) + 1);
    for (const QByteArray& argument : arguments)
        pointers.push_back(argument.constData());
    pointers.push_back(nullptr);
    return mpv_command_async(state_->handle, 0, pointers.data());
}
