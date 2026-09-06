#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace omatrack::inference {

// Borrowed RGB24, top-down, full decoded frame (no display
// scaling/letterboxing). The caller retains ownership for read(). PTS is
// deliberately not manufactured here: associate the result with the actual
// decoded frame PTS in the controller.
struct GaugeRgb24Frame {
    const std::uint8_t* pixels = nullptr;
    std::size_t byteSize = 0;
    int width = 0;
    int height = 0;
    std::size_t stride = 0;
};

enum class GaugeAdmission {
    NotChecked,
    UnsupportedGeometry,
    Rejected,
    Supported
};
enum class GaugeError {
    None,
    InvalidFrame,
    RuntimeUnavailable,
    ModelLoadFailed,
    InferenceFailed
};

struct GaugeObservation {
    std::optional<double> value;  // Unknown is never zero.
    std::string unknownReason;
};

struct GaugeResult {
    GaugeAdmission admission = GaugeAdmission::NotChecked;
    GaugeError error = GaugeError::None;
    std::string detail;
    GaugeObservation gear;
    GaugeObservation
        stintLap;  // Displayed counter, not authoritative session lap.
    GaugeObservation brakeFillPct;  // Visible bar fill, NOT brake pressure.
    GaugeObservation throttleFillPct;
    double latencyMs = 0;  // admission + preprocessing + inference + decoding;
                           // excludes model load
};

// Qt-free optional ONNX Runtime CPU reader. Construct and call on a worker;
// one instance per serial worker (not concurrently callable). No file writes,
// network, telemetry, source filenames, timestamps or temporal smoothing.
// Without OMATRACK_HAVE_ONNXRUNTIME=1 it builds a diagnostic-only stub.
class GaugeReader {
public:
    explicit GaugeReader(const std::string& modelPath);
    ~GaugeReader();
    GaugeReader(const GaugeReader&) = delete;
    GaugeReader& operator=(const GaugeReader&) = delete;

    static bool runtimeAvailable();
    bool ready() const;
    const std::string& modelError() const;
    GaugeResult read(const GaugeRgb24Frame& frame);

    // Image-only admission is available even in builds without ONNX Runtime.
    // This is a conservative fixed-layout heuristic, not arbitrary HUD
    // detection.
    static GaugeResult inspectLayout(const GaugeRgb24Frame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace omatrack::inference
