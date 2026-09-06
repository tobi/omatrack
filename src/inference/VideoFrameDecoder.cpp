#include "VideoFrameDecoder.h"

#include <algorithm>
#include <chrono>
#include <limits>

#ifdef OMATRACK_HAVE_VIDEO_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace omatrack::inference {
struct VideoFrameDecoder::Impl {
    std::string error;
    bool metadata = false;
    bool end = false;
#ifdef OMATRACK_HAVE_VIDEO_DECODER
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scale = nullptr;
    int streamIndex = -1;
    std::int64_t originNs = 0;
    std::int64_t lastPts = std::numeric_limits<std::int64_t>::min();
    bool eofSent = false;
    Cancel cancel;
    std::chrono::steady_clock::time_point deadline;

    ~Impl() {
        sws_freeContext(scale);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    bool interrupted() const {
        return (cancel && cancel->load()) ||
               std::chrono::steady_clock::now() > deadline;
    }
    static int interrupt(void* opaque) {
        return static_cast<Impl*>(opaque)->interrupted() ? 1 : 0;
    }
    void begin(const Cancel& token, int milliseconds) {
        cancel = token;
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(milliseconds);
        error.clear();
    }
    bool fail(const std::string& message) {
        error = interrupted() ? "Video decode cancelled or timed out" : message;
        return false;
    }
#endif
};

VideoFrameDecoder::VideoFrameDecoder() : impl_(std::make_unique<Impl>()) {}
VideoFrameDecoder::~VideoFrameDecoder() = default;

bool VideoFrameDecoder::available() {
#ifdef OMATRACK_HAVE_VIDEO_DECODER
    return true;
#else
    return false;
#endif
}

bool VideoFrameDecoder::open(const std::string& path, const Cancel& cancel) {
    impl_ = std::make_unique<Impl>();
    auto& p = *impl_;
#ifdef OMATRACK_HAVE_VIDEO_DECODER
    p.begin(cancel, 5000);
    p.format = avformat_alloc_context();
    if (!p.format) return p.fail("Unable to allocate video decoder");
    p.format->interrupt_callback = {Impl::interrupt, &p};
    if (avformat_open_input(&p.format, path.c_str(), nullptr, nullptr) < 0)
        return p.fail("Unable to open video for image extraction");
    if (avformat_find_stream_info(p.format, nullptr) < 0)
        return p.fail("Unable to inspect video streams");
    for (unsigned i = 0; i < p.format->nb_streams; ++i) {
        const auto* parameters = p.format->streams[i]->codecpar;
        if (parameters->codec_type == AVMEDIA_TYPE_DATA ||
            parameters->codec_tag == MKTAG('a', 'i', 'm', 'd'))
            p.metadata = true;
    }
    p.streamIndex =
        av_find_best_stream(p.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (p.streamIndex < 0) return p.fail("No decodable video stream");
    const AVStream* stream = p.format->streams[p.streamIndex];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) return p.fail("Video codec is not available");
    p.codec = avcodec_alloc_context3(decoder);
    if (!p.codec ||
        avcodec_parameters_to_context(p.codec, stream->codecpar) < 0)
        return p.fail("Unable to initialize video decoder");
    // Bounded CPU use; image extraction never competes for the renderer's GL
    // context.
    p.codec->thread_count = 2;
    if (avcodec_open2(p.codec, decoder, nullptr) < 0)
        return p.fail("Unable to start video decoder");
    p.frame = av_frame_alloc();
    p.packet = av_packet_alloc();
    if (!p.frame || !p.packet)
        return p.fail("Unable to allocate decoded frame");
    // mpv rebases the media timeline to the demuxer's declared start time.
    // Keep both clocks in every observation so a non-zero source origin is
    // explicit.
    if (p.format->start_time != AV_NOPTS_VALUE)
        p.originNs =
            av_rescale_q(p.format->start_time, AVRational{1, AV_TIME_BASE},
                         AVRational{1, 1000000000});
    else if (stream->start_time != AV_NOPTS_VALUE)
        p.originNs = av_rescale_q(stream->start_time, stream->time_base,
                                  AVRational{1, 1000000000});
    return !p.interrupted();
#else
    (void)path;
    (void)cancel;
    p.error = "This build has no FFmpeg image decoder";
    return false;
#endif
}

bool VideoFrameDecoder::frameAtOrAfter(std::int64_t presentationNs,
                                       DecodedRgbFrame& result,
                                       const Cancel& cancel) {
    result = {};
    auto& p = *impl_;
#ifdef OMATRACK_HAVE_VIDEO_DECODER
    if (!p.codec || !p.format || !p.frame) {
        p.error = "Video decoder is not open";
        return false;
    }
    p.begin(cancel, 1500);
    p.end = false;
    presentationNs = std::max<std::int64_t>(0, presentationNs);
    const AVStream* stream = p.format->streams[p.streamIndex];
    const bool first = p.lastPts == std::numeric_limits<std::int64_t>::min();
    if ((first && presentationNs > 2000000000LL) ||
        (!first && (presentationNs <= p.lastPts ||
                    presentationNs - p.lastPts > 2000000000LL))) {
        const auto timestamp =
            av_rescale_q(presentationNs + p.originNs, AVRational{1, 1000000000},
                         stream->time_base);
        if (av_seek_frame(p.format, p.streamIndex, timestamp,
                          AVSEEK_FLAG_BACKWARD) < 0)
            return p.fail("Unable to seek the image decoder");
        avcodec_flush_buffers(p.codec);
        p.eofSent = false;
    }
    for (int attempts = 0; attempts < 20000 && !p.interrupted(); ++attempts) {
        const int received = avcodec_receive_frame(p.codec, p.frame);
        if (received == 0) {
            if (p.frame->best_effort_timestamp == AV_NOPTS_VALUE)
                return p.fail("Decoded frame has no presentation timestamp");
            const auto sourceNs =
                av_rescale_q(p.frame->best_effort_timestamp, stream->time_base,
                             AVRational{1, 1000000000});
            p.lastPts = sourceNs - p.originNs;
            if (p.lastPts < presentationNs) continue;
            const int width = p.frame->width;
            const int height = p.frame->height;
            // Known-layout reader currently expects 1080p, but safely decode up
            // to 4K.
            if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
                return p.fail(
                    "Image dimensions exceed the bounded extraction limit");
            result.width = width;
            result.height = height;
            result.stride = std::size_t(width) * 3;
            result.pixels.resize(result.stride * std::size_t(height));
            p.scale = sws_getCachedContext(
                p.scale, width, height,
                static_cast<AVPixelFormat>(p.frame->format), width, height,
                AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!p.scale)
                return p.fail("Unable to convert video pixels to RGB");
            // sws_getCachedContext does not inherit the AVFrame's matrix/range.
            // Its BT.601 default changes real BT.709 HUD pixels compared with
            // the training decoder, even though dimensions and PTS agree.
            int matrix = SWS_CS_DEFAULT;
            switch (p.frame->colorspace) {
                case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
                case AVCOL_SPC_FCC: matrix = SWS_CS_FCC; break;
                case AVCOL_SPC_BT470BG:
                case AVCOL_SPC_SMPTE170M: matrix = SWS_CS_ITU601; break;
                case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
                case AVCOL_SPC_BT2020_NCL:
                case AVCOL_SPC_BT2020_CL: matrix = SWS_CS_BT2020; break;
                default: break;
            }
            const int* coefficients = sws_getCoefficients(matrix);
            if (sws_setColorspaceDetails(
                    p.scale, coefficients,
                    p.frame->color_range == AVCOL_RANGE_JPEG, coefficients, 1,
                    0, 1 << 16, 1 << 16) < 0)
                return p.fail("Unable to apply video color matrix/range");
            std::uint8_t* planes[4] = {result.pixels.data(), nullptr, nullptr,
                                       nullptr};
            int strides[4] = {int(result.stride), 0, 0, 0};
            if (sws_scale(p.scale, p.frame->data, p.frame->linesize, 0, height,
                          planes, strides) != height)
                return p.fail("Incomplete RGB frame conversion");
            result.sourcePtsNs = sourceNs;
            result.presentationPtsNs = p.lastPts;
            result.timelineOriginNs = p.originNs;
            return !p.interrupted();
        }
        if (received == AVERROR_EOF) {
            p.end = true;
            return p.fail("No observation after end of video");
        }
        if (received != AVERROR(EAGAIN))
            return p.fail("Video frame decode failed");
        if (p.eofSent)
            return p.fail("Video decoder ended without another frame");
        int packetStatus;
        do {
            av_packet_unref(p.packet);
            packetStatus = av_read_frame(p.format, p.packet);
        } while (packetStatus >= 0 && p.packet->stream_index != p.streamIndex &&
                 !p.interrupted());
        if (p.interrupted()) return p.fail("Decode cancelled");
        if (packetStatus < 0) {
            if (packetStatus != AVERROR_EOF)
                return p.fail("Unable to read video packet");
            p.eofSent = true;
            if (avcodec_send_packet(p.codec, nullptr) < 0)
                return p.fail("Unable to drain the video decoder");
        } else {
            const int sent = avcodec_send_packet(p.codec, p.packet);
            av_packet_unref(p.packet);
            if (sent < 0)
                return p.fail("Unable to submit compressed video packet");
        }
    }
    return p.fail("Image decode exceeded its work budget");
#else
    (void)presentationNs;
    (void)cancel;
    p.error = "This build has no FFmpeg image decoder";
    return false;
#endif
}

bool VideoFrameDecoder::hasMetadataTrack() const { return impl_->metadata; }
std::int64_t VideoFrameDecoder::timelineOriginNs() const {
#ifdef OMATRACK_HAVE_VIDEO_DECODER
    return impl_->originNs;
#else
    return 0;
#endif
}
bool VideoFrameDecoder::atEnd() const { return impl_->end; }
const std::string& VideoFrameDecoder::error() const { return impl_->error; }
}  // namespace omatrack::inference
