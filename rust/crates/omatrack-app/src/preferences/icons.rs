//! The Lucide icons of the Preferences section list that the component
//! bundle (`gpui_kit::assets::Assets`) does not carry, embedded with the
//! kit's `icon_assets!` and handed to [`Icon`] as data (as the video bar
//! does), so the application's asset source stays the default bundle.

use gpui_kit::AssetSource as _;
use gpui_kit::assets::IconName;
use gpui_kit::component::Icon;

gpui_kit::assets::icon_assets!(PreferencesIconAssets, [ChartSpline, Video, Users, Route]);

/// An embedded icon; empty if `name` is not embedded (the item keeps its
/// label, so nothing is lost but the glyph).
pub(super) fn icon(name: IconName) -> Icon {
    match PreferencesIconAssets.load(&name.path()) {
        Ok(Some(bytes)) => Icon::default().data(&bytes),
        _ => Icon::empty(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_section_icon_is_embedded() {
        for name in [
            IconName::ChartSpline,
            IconName::Video,
            IconName::Users,
            IconName::Route,
        ] {
            let bytes = PreferencesIconAssets.load(&name.path()).unwrap().unwrap();
            assert!(bytes.starts_with(b"<svg"), "{name:?}");
        }
    }
}
