//! The Lucide icons of the video transport bar that the component bundle
//! (`gpui_kit::assets::Assets`) does not carry.
//!
//! They are embedded with the kit's `icon_assets!` (only these SVGs, static
//! bytes) and handed to [`Icon`] as data, so the application's asset source
//! stays the default bundle. Built once per panel and cloned per render: an
//! [`Icon`] clone shares its bytes.

use gpui_kit::AssetSource as _;
use gpui_kit::assets::IconName;
use gpui_kit::component::Icon;

gpui_kit::assets::icon_assets!(
    VideoIconAssets,
    [
        Volume2,
        VolumeX,
        Columns2,
        PictureInPicture2,
        Square,
        Maximize2,
        Minimize2,
        Gauge
    ]
);

/// The bar's icons.
#[derive(Clone)]
pub struct VideoIcons {
    pub volume: Icon,
    pub muted: Icon,
    pub split: Icon,
    pub inset: Icon,
    pub single: Icon,
    pub fullscreen: Icon,
    pub exit_fullscreen: Icon,
    pub hud: Icon,
}

impl VideoIcons {
    pub fn new() -> Self {
        Self {
            volume: icon(IconName::Volume2),
            muted: icon(IconName::VolumeX),
            split: icon(IconName::Columns2),
            inset: icon(IconName::PictureInPicture2),
            single: icon(IconName::Square),
            fullscreen: icon(IconName::Maximize2),
            exit_fullscreen: icon(IconName::Minimize2),
            hud: icon(IconName::Gauge),
        }
    }
}

impl Default for VideoIcons {
    fn default() -> Self {
        Self::new()
    }
}

fn icon(name: IconName) -> Icon {
    match VideoIconAssets.load(&name.path()) {
        Ok(Some(bytes)) => Icon::default().data(&bytes),
        // Listed above, so always embedded; an empty icon keeps the button's
        // accessible name and tooltip if that ever changes.
        _ => Icon::empty(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_bar_icon_is_embedded() {
        for name in [
            IconName::Volume2,
            IconName::VolumeX,
            IconName::Columns2,
            IconName::PictureInPicture2,
            IconName::Square,
            IconName::Maximize2,
            IconName::Minimize2,
            IconName::Gauge,
        ] {
            let bytes = VideoIconAssets.load(&name.path()).unwrap().unwrap();
            assert!(bytes.starts_with(b"<svg"), "{name:?}");
        }
    }
}
