//! The Lucide icons of the trace toolbar that the component bundle
//! (`gpui_kit::assets::Assets`) does not carry, embedded like the video
//! bar's (`panels/video/icons.rs`): only these SVGs, handed to [`Icon`] as
//! data. Built once per panel; an [`Icon`] clone shares its bytes.

use gpui_kit::AssetSource as _;
use gpui_kit::assets::IconName;
use gpui_kit::component::Icon;

gpui_kit::assets::icon_assets!(TraceIconAssets, [ZoomIn, ZoomOut, Scan]);

/// The toolbar's icons.
#[derive(Clone)]
pub struct TraceIcons {
    pub zoom_in: Icon,
    pub zoom_out: Icon,
    pub fit: Icon,
}

impl TraceIcons {
    pub fn new() -> Self {
        Self {
            zoom_in: icon(IconName::ZoomIn),
            zoom_out: icon(IconName::ZoomOut),
            fit: icon(IconName::Scan),
        }
    }
}

impl Default for TraceIcons {
    fn default() -> Self {
        Self::new()
    }
}

fn icon(name: IconName) -> Icon {
    match TraceIconAssets.load(&name.path()) {
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
    fn every_toolbar_icon_is_embedded() {
        for name in [IconName::ZoomIn, IconName::ZoomOut, IconName::Scan] {
            let bytes = TraceIconAssets.load(&name.path()).unwrap().unwrap();
            assert!(bytes.starts_with(b"<svg"), "{name:?}");
        }
    }
}
