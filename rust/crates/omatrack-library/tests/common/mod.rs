//! Shared helpers: synthetic recordings and a location that serves them
//! for real (temporary) files, so scans exercise the whole pipeline.

#![allow(dead_code)]

use omatrack_core::laps::{Lap, LapKind};
use omatrack_core::{OpenError, RawChannel, Recording};
use omatrack_library::location::{Cancel, DiscoveredFile, Location, LocationId, OpenMode};
use omatrack_library::{FileIdentity, RecordingSummary};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

/// A recording with a speed channel and `lap_times_ms` complete laps after
/// a 5 s out fragment.
pub fn synthetic_recording(lap_times_ms: &[f64]) -> Recording {
    let total: f64 = 5.0 + lap_times_ms.iter().sum::<f64>() / 1000.0;
    let count = (total * 10.0) as usize + 1;
    let speed = (0..count).map(|i| 100.0 + (i % 50) as f64).collect();
    let mut recording = Recording::synthetic(
        "",
        vec![RawChannel::synthetic("Speed", "km/h", 10.0, total, speed)],
    );
    let mut laps = vec![{
        let mut out = Lap::new(0, 0.0, 5.0, 5000.0, false);
        out.kind = LapKind::Out;
        out
    }];
    let mut start = 5.0;
    for (i, &time_ms) in lap_times_ms.iter().enumerate() {
        let id = i as i32 + 1;
        let mut lap = Lap::new(id, start, start + time_ms / 1000.0, time_ms, true);
        lap.source_number = Some(id);
        lap.kind = LapKind::Flying;
        laps.push(lap);
        start += time_ms / 1000.0;
    }
    recording.set_source_laps(laps);
    recording
}

/// A summary from a synthetic recording, with a wall clock.
pub fn summary(lap_times_ms: &[f64], utc_start_ns: i64, timezone: &str) -> RecordingSummary {
    let mut summary = RecordingSummary::from_recording(&synthetic_recording(lap_times_ms));
    summary.utc_start_ns = utc_start_ns;
    summary.timezone = timezone.to_string();
    summary
}

/// Serves files under `root` whose name ends in `.pds` as synthetic
/// recordings; a file named `broken.pds` fails to open. Counts opens.
pub struct FakeLocation {
    id: LocationId,
    root: PathBuf,
    pub opens: AtomicUsize,
    pub lap_times: Mutex<Vec<f64>>,
}

impl FakeLocation {
    pub fn new(root: &Path) -> Self {
        Self {
            id: LocationId::new("fake"),
            root: root.to_path_buf(),
            opens: AtomicUsize::new(0),
            lap_times: Mutex::new(vec![80_000.0, 78_500.0, 79_000.0]),
        }
    }
    pub fn opens(&self) -> usize {
        self.opens.load(Ordering::SeqCst)
    }
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<_> = std::fs::read_dir(dir).unwrap().flatten().collect();
    entries.sort_by_key(|e| e.file_name());
    for entry in entries {
        let path = entry.path();
        if path.is_dir() {
            walk(&path, out);
        } else if path.extension().is_some_and(|e| e == "pds") {
            out.push(path);
        }
    }
}

impl Location for FakeLocation {
    fn id(&self) -> &LocationId {
        &self.id
    }
    fn name(&self) -> &str {
        "Fake"
    }
    fn scan(&self, cancel: &Cancel, sink: &mut dyn FnMut(DiscoveredFile)) -> io::Result<()> {
        let mut files = Vec::new();
        walk(&self.root, &mut files);
        for path in files {
            if cancel.is_cancelled() {
                return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled"));
            }
            let identity = FileIdentity::of(&path)?;
            sink(DiscoveredFile::new(self.id.clone(), path, identity));
        }
        Ok(())
    }
    fn open(&self, file: &DiscoveredFile, _mode: OpenMode) -> Result<Recording, OpenError> {
        self.opens.fetch_add(1, Ordering::SeqCst);
        if file.path().file_name().is_some_and(|n| n == "broken.pds") {
            return Err(OpenError("not a recording".into()));
        }
        Ok(synthetic_recording(&self.lap_times.lock().unwrap()))
    }
    fn media_path(&self, _file: &DiscoveredFile) -> Option<PathBuf> {
        None
    }
}

/// Unix nanoseconds of a UTC civil time.
pub fn utc_ns(text: &str) -> i64 {
    let timestamp: jiff::Timestamp = text.parse().unwrap();
    i64::try_from(timestamp.as_nanosecond()).unwrap()
}
