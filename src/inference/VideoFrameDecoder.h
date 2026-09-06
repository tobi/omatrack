// Independent image-inference decoder. Never reads pixels from the mpv render
// thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omatrack::inference {

struct DecodedRgbFrame {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
    std::int64_t sourcePtsNs = 0;
    std::int64_t presentationPtsNs = 0;
    std::int64_t timelineOriginNs = 0;
};

/// One instance is confined to one serial worker. Input files remain read-only.
/// Returns actual decoded timestamps, not frame-index / nominal-FPS estimates.
class VideoFrameDecoder {
public:
    using Cancel = std::shared_ptr<std::atomic<bool>>;
    VideoFrameDecoder();
    ~VideoFrameDecoder();
    VideoFrameDecoder(const VideoFrameDecoder&) = delete;
    VideoFrameDecoder& operator=(const VideoFrameDecoder&) = delete;

    static bool available();
    bool open(const std::string& path, const Cancel& cancel);
    bool frameAtOrAfter(std::int64_t presentationNs, DecodedRgbFrame& result,
                        const Cancel& cancel);
    /// Any data stream (including aimd) is a conservative veto: parser failure
    /// does not establish that native telemetry is absent.
    bool hasMetadataTrack() const;
    std::int64_t timelineOriginNs() const;
    bool atEnd() const;
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace omatrack::inference
