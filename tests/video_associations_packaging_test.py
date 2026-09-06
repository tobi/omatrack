"""Read-only registration policy checks; never install or select OS handlers.

Run: uv run --no-project tests/video_associations_packaging_test.py
"""

import configparser
from pathlib import Path
import plistlib
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
VIDEO_EXTENSIONS = {"mp4", "mov", "mkv", "avi", "m4v", "webm"}


class VideoAssociationsPackagingTest(unittest.TestCase):
    def test_linux_open_with_capabilities(self):
        desktop = configparser.ConfigParser(interpolation=None)
        desktop.read(ROOT / "packaging/linux/io.github.tobi.omatrack.desktop")
        entry = desktop["Desktop Entry"]
        types = entry["MimeType"].strip(";").split(";")
        self.assertEqual(len(types), len(set(types)))
        self.assertEqual(
            set(types),
            {
                # Preserve the telemetry registrations already shipped.
                "application/x-cosworth-pds",
                "application/x-motec-ld",
                "application/x-racelogic-vbo",
                # Current MIME names plus historical AVI/M4V names.
                "video/mp4",
                "video/quicktime",
                "video/x-matroska",
                "video/vnd.avi",
                "video/x-msvideo",
                "video/x-m4v",
                "video/webm",
            },
        )
        self.assertEqual(entry["Exec"], "omatrack %f")
        self.assertEqual(entry["Terminal"], "false")

    def test_linux_does_not_redefine_generic_video_mime_types(self):
        mime = ET.parse(ROOT / "packaging/linux/omatrack.xml")
        ns = {"mime": "http://www.freedesktop.org/standards/shared-mime-info"}
        types = mime.findall("mime:mime-type", ns)
        self.assertTrue(types)
        for mime_type in types:
            self.assertFalse(mime_type.attrib["type"].startswith("video/"))
            for glob in mime_type.findall("mime:glob", ns):
                extension = glob.attrib["pattern"].removeprefix("*.").lower()
                self.assertNotIn(extension, VIDEO_EXTENSIONS)

    def test_macos_alternate_viewer_for_exact_supported_extensions(self):
        with (ROOT / "packaging/macos/Info.plist.in").open("rb") as source:
            plist = plistlib.load(source)
        self.assertEqual(
            plist["CFBundleExecutable"], "${MACOSX_BUNDLE_EXECUTABLE_NAME}"
        )
        self.assertEqual(
            plist["CFBundleIdentifier"], "${MACOSX_BUNDLE_GUI_IDENTIFIER}"
        )
        documents = plist["CFBundleDocumentTypes"]
        self.assertEqual(len(documents), 1)
        video = documents[0]
        self.assertEqual(video["CFBundleTypeRole"], "Viewer")
        self.assertEqual(video["LSHandlerRank"], "Alternate")
        extensions = video["CFBundleTypeExtensions"]
        self.assertEqual(set(extensions), VIDEO_EXTENSIONS)
        self.assertEqual(len(extensions), len(VIDEO_EXTENSIONS))
        # LSItemContentTypes takes precedence over extension matching. Do not
        # accidentally broaden this into all movies / videos or all documents.
        self.assertNotIn("LSItemContentTypes", video)
        self.assertNotIn("UTExportedTypeDeclarations", plist)


if __name__ == "__main__":
    unittest.main()
