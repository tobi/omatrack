//! Corner intelligence: per-lap metrics, the check registry, and zones.
//!
//! ```text
//! measure_corner(lap, start, end)       one pass over the corner's samples
//!         -> CornerMetrics
//! CornerContext{primary, reference metrics, aligned deltas}
//!         -> checks::run()  -> Vec<CornerNote>
//! ```

pub mod checks;
pub mod metrics;
pub mod zones;

pub use checks::{CornerCheck, CornerContext, CornerNote, NoteSeverity, REGISTRY};
pub use metrics::{CornerMetrics, measure_corner};
pub use zones::{ComplexZone, CornerZone, StationMapper, ZoneSource, auto_generate_corners};
