//! The Omatrack session library, without GPUI: configuration
//! (`omatrack.yml`), portable folder metadata (`TRACK.yml`), the metadata
//! precedence rule, library locations, the index cache, and the
//! Track > Date > Session > Laps catalog.
//!
//! Everything here is blocking, `Send + Sync` and cancellable where it
//! does I/O: the app runs it on a background executor and applies the
//! results on the UI thread. Telemetry and video files are never written.

pub mod catalog;
pub mod config;
mod fsutil;
pub mod index_cache;
pub mod location;
pub mod metadata;
pub mod paths;
pub mod recent;
pub mod scan;
pub mod summary;
pub mod track_yml;

pub use catalog::{
    CatalogRecord, DateNode, Facet, Facets, LapNode, LibraryFilter, LibrarySnapshot, SessionNode,
    TrackNode,
};
pub use config::{Config, ConfigError, ConfigFile};
pub use index_cache::{CacheOutcome, FileIdentity, IndexCache};
pub use location::{
    Cancel, DiscoveredFile, FileKind, FolderLocation, Location, LocationId, OpenMode,
};
pub use metadata::{EffectiveMetadata, MetadataLayer, effective_metadata};
pub use paths::Paths;
pub use recent::MAX_RECENT_FILES;
pub use scan::{ScanOutcome, ScanProgress, locations_from_config, scan_library};
pub use summary::RecordingSummary;
