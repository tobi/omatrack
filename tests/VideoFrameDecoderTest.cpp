#include "VideoFrameDecoder.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace omatrack::inference;

int main(int argc, char** argv) {
    try {
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        VideoFrameDecoder decoder;
        if (argc == 1) {
            if (decoder.open("/not/a/real/video.mkv", cancel) ||
                decoder.error().empty())
                throw std::runtime_error("Missing input must fail explicitly");
            DecodedRgbFrame frame;
            if (decoder.frameAtOrAfter(0, frame, cancel))
                throw std::runtime_error("Unopened decoder produced pixels");
            std::cout << "PASS missing file and unopened decoder\n";
            return 0;
        }
        if (argc < 3 || !decoder.open(argv[1], cancel))
            throw std::runtime_error(decoder.error());
        for (int i = 2; i < argc; ++i) {
            const auto target = static_cast<std::int64_t>(
                std::llround(std::stod(argv[i]) * 1e9));
            DecodedRgbFrame frame;
            if (!decoder.frameAtOrAfter(target, frame, cancel))
                throw std::runtime_error(decoder.error());
            if (frame.presentationPtsNs < std::max<std::int64_t>(0, target) ||
                frame.sourcePtsNs - frame.timelineOriginNs !=
                    frame.presentationPtsNs ||
                frame.pixels.size() != frame.stride * std::size_t(frame.height))
                throw std::runtime_error("Invalid decoded-frame identity");
            const auto offset = (std::size_t(frame.height / 2) * frame.stride) +
                                std::size_t(frame.width / 2) * 3;
            std::cout << "{\"metadata\":"
                      << (decoder.hasMetadataTrack() ? "true" : "false")
                      << ",\"pts_ns\":" << frame.presentationPtsNs
                      << ",\"source_pts_ns\":" << frame.sourcePtsNs
                      << ",\"origin_ns\":" << frame.timelineOriginNs
                      << ",\"width\":" << frame.width
                      << ",\"height\":" << frame.height << ",\"center_rgb\":["
                      << int(frame.pixels.at(offset)) << ','
                      << int(frame.pixels.at(offset + 1)) << ','
                      << int(frame.pixels.at(offset + 2)) << "]}\n";
        }
        cancel->store(true);
        DecodedRgbFrame cancelled;
        if (decoder.frameAtOrAfter(0, cancelled, cancel))
            throw std::runtime_error("Cancelled decode returned fresh pixels");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
