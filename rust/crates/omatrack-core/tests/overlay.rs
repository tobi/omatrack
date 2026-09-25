//! The channel-source seam: both providers put channels on the lap grid.

use omatrack_core::overlay::{ChannelProvider, SourceChannels, StandardChannels};
use omatrack_core::{ChannelOverrides, RawChannel, Recording};

fn recording() -> Recording {
    Recording::synthetic(
        "",
        vec![
            RawChannel::synthetic("Speed", "km/h", 2.0, 2.0, vec![100.0, 120.0, 140.0, 160.0, 180.0]),
            RawChannel::synthetic("RPM", "rpm", 2.0, 2.0, vec![6000.0, 6500.0, 7000.0, 7500.0, 8000.0]),
        ],
    )
}

#[test]
fn standard_channels_follow_the_lap_grid() {
    let rec = recording();
    let lap = rec.unify_lap(0.0, 2.0, &ChannelOverrides::new());
    let catalog = StandardChannels.catalog(&rec);
    assert_eq!(catalog[0].key, "speed");
    assert!(catalog.iter().any(|c| c.key == "gear"));
    let group = StandardChannels
        .resample(&rec, &lap, &["speed".into(), "gear".into()])
        .unwrap();
    assert_eq!(group.channels.len(), 2);
    assert!(group.channels.iter().all(|c| c.values.len() == lap.len()));
    assert_eq!(group.channel("speed").unwrap().values[50], 140.0);
    assert!(StandardChannels.resample(&rec, &lap, &["nope".into()]).is_err());
}

#[test]
fn source_channels_resample_through_the_source_clock() {
    let rec = recording();
    let lap = rec.unify_lap(0.5, 2.0, &ChannelOverrides::new());
    assert_eq!(lap.start_time, 0.5);
    let catalog = SourceChannels.catalog(&rec);
    assert_eq!(catalog.iter().map(|c| c.key.as_str()).collect::<Vec<_>>(), ["raw:Speed", "raw:RPM"]);
    assert!(catalog.iter().all(|c| !c.default_visible));
    let group = SourceChannels.resample(&rec, &lap, &["raw:RPM".into()]).unwrap();
    let rpm = &group.channels[0].values;
    assert_eq!(rpm.len(), lap.len());
    // Sample 0 is file time 0.5 s = the second source sample.
    assert_eq!(rpm[0], 6500.0);
    assert_eq!(rpm[25], 7000.0);
    assert!(SourceChannels.resample(&rec, &lap, &["raw:Missing".into()]).is_err());
}
