// Unit tests for the portable AppImage updater helpers.

#include "AppUpdateTest.h"

#include "app/AppUpdate.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using namespace omatrack;

void AppUpdateTest::normalizesTagAndMetadata() {
    QCOMPARE(normalizeVersion(QStringLiteral("v1.2.3")),
             QStringLiteral("1.2.3"));
    QCOMPARE(normalizeVersion(QStringLiteral("1.2.3-rc.1")),
             QStringLiteral("1.2.3"));
    QCOMPARE(normalizeVersion(QStringLiteral("1.2.3+build")),
             QStringLiteral("1.2.3"));
}

void AppUpdateTest::comparesSemverComponents() {
    QCOMPARE(compareVersions(QStringLiteral("1.2.0"), QStringLiteral("1.2")),
             0);
    QCOMPARE(compareVersions(QStringLiteral("1.2.1"), QStringLiteral("1.2.0")),
             1);
    QCOMPARE(compareVersions(QStringLiteral("v1.1.9"), QStringLiteral("1.2.0")),
             -1);
    QVERIFY(versionIsNewer(QStringLiteral("1.2.0"), QStringLiteral("1.1.0")));
    QVERIFY(!versionIsNewer(QStringLiteral("1.1.0"), QStringLiteral("1.1.0")));
}

void AppUpdateTest::parsesGithubReleaseAsset() {
    const QByteArray body = R"({
      "tag_name": "v1.2.0",
      "html_url": "https://github.com/tobi/omatrack/releases/tag/v1.2.0",
      "body": "Notes",
      "assets": [
        {
          "name": "Omatrack-1.2.0-linux-x86_64.AppImage",
          "browser_download_url": "https://example.test/Omatrack.AppImage",
          "size": 42
        },
        {
          "name": "Omatrack-1.2.0-windows-x86_64.zip",
          "browser_download_url": "https://example.test/Omatrack.zip",
          "size": 7
        },
        {
          "name": "Omatrack-1.2.0-macOS-arm64.dmg",
          "browser_download_url": "https://example.test/Omatrack.dmg",
          "size": 9
        },
        {
          "name": "SHA256SUMS.txt",
          "browser_download_url": "https://example.test/SHA256SUMS.txt"
        }
      ]
    })";
    auto release = parseGithubRelease(body);
    QVERIFY(release.has_value());
    QCOMPARE(release->version, QStringLiteral("1.2.0"));
    QCOMPARE(release->linuxAssetName,
             QStringLiteral("Omatrack-1.2.0-linux-x86_64.AppImage"));
    QCOMPARE(release->windowsAssetName,
             QStringLiteral("Omatrack-1.2.0-windows-x86_64.zip"));
    QCOMPARE(release->macAssetName,
             QStringLiteral("Omatrack-1.2.0-macOS-arm64.dmg"));
    QCOMPARE(release->checksumsUrl.toString(),
             QStringLiteral("https://example.test/SHA256SUMS.txt"));
    QVERIFY(selectReleaseAsset(&*release, UpdateChannel::LinuxAppImage));
    QCOMPARE(release->assetName,
             QStringLiteral("Omatrack-1.2.0-linux-x86_64.AppImage"));
    QCOMPARE(release->assetUrl.toString(),
             QStringLiteral("https://example.test/Omatrack.AppImage"));
    QCOMPARE(release->assetSize, 42);
}

void AppUpdateTest::selectsWindowsVelopackAssets() {
    const QByteArray body = R"({
      "tag_name": "v1.2.0",
      "assets": [
        {
          "name": "io.github.tobi.omatrack-1.2.0-full.nupkg",
          "browser_download_url": "https://example.test/app.nupkg",
          "size": 11
        },
        {
          "name": "Omatrack-1.2.0-windows-x86_64-Setup.exe",
          "browser_download_url": "https://example.test/Setup.exe",
          "size": 13
        },
        {
          "name": "Omatrack-1.2.0-windows-x86_64.zip",
          "browser_download_url": "https://example.test/Omatrack.zip",
          "size": 7
        }
      ]
    })";
    auto release = parseGithubRelease(body);
    QVERIFY(release.has_value());
    QVERIFY(selectWindowsUpdateAsset(&*release, true));
    QCOMPARE(release->assetName,
             QStringLiteral("io.github.tobi.omatrack-1.2.0-full.nupkg"));
    QVERIFY(selectWindowsUpdateAsset(&*release, false));
    QCOMPARE(release->assetName,
             QStringLiteral("Omatrack-1.2.0-windows-x86_64-Setup.exe"));
}

void AppUpdateTest::selectsWindowsZipAsset() {
    const QByteArray body = R"({
      "tag_name": "v1.2.0",
      "assets": [
        {
          "name": "Omatrack-1.2.0-linux-x86_64.AppImage",
          "browser_download_url": "https://example.test/Omatrack.AppImage",
          "size": 42
        },
        {
          "name": "Omatrack-1.2.0-windows-x86_64.zip",
          "browser_download_url": "https://example.test/Omatrack.zip",
          "size": 7
        }
      ]
    })";
    auto release = parseGithubRelease(body);
    QVERIFY(release.has_value());
    QVERIFY(selectReleaseAsset(&*release, UpdateChannel::WindowsZip));
    QCOMPARE(release->assetName,
             QStringLiteral("Omatrack-1.2.0-windows-x86_64.zip"));
    QCOMPARE(release->assetUrl.toString(),
             QStringLiteral("https://example.test/Omatrack.zip"));
    QCOMPARE(release->assetSize, 7);
}

void AppUpdateTest::selectsMacDmgAsset() {
    const QByteArray body = R"({
      "tag_name": "v1.2.0",
      "assets": [
        {
          "name": "Omatrack-1.2.0-macOS-arm64.dmg",
          "browser_download_url": "https://example.test/Omatrack.dmg",
          "size": 9
        }
      ]
    })";
    auto release = parseGithubRelease(body);
    QVERIFY(release.has_value());
    QVERIFY(selectReleaseAsset(&*release, UpdateChannel::MacDmg));
    QCOMPARE(release->assetName,
             QStringLiteral("Omatrack-1.2.0-macOS-arm64.dmg"));
    QCOMPARE(release->assetUrl.toString(),
             QStringLiteral("https://example.test/Omatrack.dmg"));
    QCOMPARE(release->assetSize, 9);
    QVERIFY(!selectReleaseAsset(&*release, UpdateChannel::LinuxAppImage));
}

void AppUpdateTest::acceptsSinglePlatformRelease() {
    const QByteArray windowsOnly = R"({
      "tag_name": "v1.2.0",
      "assets": [
        {
          "name": "Omatrack-1.2.0-windows-x86_64.zip",
          "browser_download_url": "https://example.test/Omatrack.zip"
        }
      ]
    })";
    auto windows = parseGithubRelease(windowsOnly);
    QVERIFY(windows.has_value());
    QVERIFY(selectReleaseAsset(&*windows, UpdateChannel::WindowsZip));
    QVERIFY(!selectReleaseAsset(&*windows, UpdateChannel::LinuxAppImage));
}

void AppUpdateTest::rejectsReleaseWithoutAnyAsset() {
    const QByteArray body = R"({
      "tag_name": "v1.2.0",
      "assets": [
        {
          "name": "SHA256SUMS.txt",
          "browser_download_url": "https://example.test/SHA256SUMS.txt"
        }
      ]
    })";
    QVERIFY(!parseGithubRelease(body).has_value());
}

void AppUpdateTest::readsSha256Listing() {
    const QByteArray sums =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
        "Omatrack-1.2.0-linux-x86_64.AppImage\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb *"
        "Omatrack-1.2.0-windows-x86_64.zip\n";
    QCOMPARE(checksumForFile(
                 sums, QStringLiteral("Omatrack-1.2.0-linux-x86_64.AppImage")),
             QStringLiteral(
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaaaaaaa"));
    QCOMPARE(checksumForFile(
                 sums, QStringLiteral("Omatrack-1.2.0-windows-x86_64.zip")),
             QStringLiteral(
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                 "bbbb"));
    QCOMPARE(checksumForFile(sums, QStringLiteral("missing")), QString());
}

void AppUpdateTest::hashesAndReplacesAppImage() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString current =
        directory.filePath(QStringLiteral("Omatrack.AppImage"));
    const QString incoming =
        directory.filePath(QStringLiteral("Omatrack.AppImage.part"));
    QFile currentFile(current);
    QVERIFY(currentFile.open(QIODevice::WriteOnly));
    QVERIFY(currentFile.write("OLD") == 3);
    currentFile.close();
    QFile incomingFile(incoming);
    QVERIFY(incomingFile.open(QIODevice::WriteOnly));
    QVERIFY(incomingFile.write("NEW-BYTES") == 9);
    incomingFile.close();

    const QString digest = fileSha256(incoming);
    QCOMPARE(digest.size(), 64);

    QString error;
    QVERIFY(replaceAppImage(current, incoming, &error));
    QVERIFY(error.isEmpty());
    QFile replaced(current);
    QVERIFY(replaced.open(QIODevice::ReadOnly));
    QCOMPARE(replaced.readAll(), QByteArrayLiteral("NEW-BYTES"));
    QVERIFY(!QFile::exists(incoming));
    QVERIFY(!QFile::exists(current + QStringLiteral(".old")));
#ifdef Q_OS_UNIX
    QVERIFY(QFile::permissions(current) & QFile::ExeOwner);
#endif
}

void AppUpdateTest::restoreOriginalWhenInstallRenameFails() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString current =
        directory.filePath(QStringLiteral("Omatrack.AppImage"));
    QFile currentFile(current);
    QVERIFY(currentFile.open(QIODevice::WriteOnly));
    QVERIFY(currentFile.write("KEEP") == 4);
    currentFile.close();

    QString error;
    QVERIFY(!replaceAppImage(current, current + QStringLiteral(".missing"),
                             &error));
    QVERIFY(!error.isEmpty());
    QFile remaining(current);
    QVERIFY(remaining.open(QIODevice::ReadOnly));
    QCOMPARE(remaining.readAll(), QByteArrayLiteral("KEEP"));
}

void AppUpdateTest::findsNestedWindowsPayload() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QCOMPARE(windowsPayloadRoot(directory.path()), QString());
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("Omatrack/lib")));
    QFile nested(directory.filePath(QStringLiteral("Omatrack/omatrack.exe")));
    QVERIFY(nested.open(QIODevice::WriteOnly));
    nested.close();
    QCOMPARE(windowsPayloadRoot(directory.path()),
             QFileInfo(directory.filePath(QStringLiteral("Omatrack")))
                 .absoluteFilePath());
}

void AppUpdateTest::writesWindowsApplyScript() {
    const QString script = windowsApplyScript(
        4242, QStringLiteral("/tmp/src"), QStringLiteral("/tmp/dest"),
        {QStringLiteral("C:\\Telemetry")}, QStringLiteral("/tmp/src"));
    QVERIFY(script.contains(QStringLiteral("set \"PID=4242\"")));
    QVERIFY(script.contains(QStringLiteral("robocopy")));
    QVERIFY(script.contains(QStringLiteral("omatrack.exe")));
    QVERIFY(script.contains(QStringLiteral("C:\\Telemetry")));
    QVERIFY(script.contains(QStringLiteral("Wait-Process")));
}

void AppUpdateTest::findsMacAppBundle() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QCOMPARE(macPayloadRoot(directory.path()), QString());
    QVERIFY(QDir(directory.path())
                .mkpath(QStringLiteral("Omatrack.app/Contents/MacOS")));
    QFile plist(
        directory.filePath(QStringLiteral("Omatrack.app/Contents/Info.plist")));
    QVERIFY(plist.open(QIODevice::WriteOnly));
    plist.close();
    QCOMPARE(macPayloadRoot(directory.path()),
             QFileInfo(directory.filePath(QStringLiteral("Omatrack.app")))
                 .absoluteFilePath());
}

void AppUpdateTest::writesMacApplyScript() {
    const QString script = macApplyScript(
        99, QStringLiteral("/tmp/src/Omatrack.app"),
        QStringLiteral("/Applications/Omatrack.app"),
        {QStringLiteral("/Users/me/Telemetry")}, QStringLiteral("/tmp/src"));
    QVERIFY(script.contains(QStringLiteral("PID=99")));
    QVERIFY(script.contains(QStringLiteral("ditto")));
    QVERIFY(script.contains(QStringLiteral("open -n")));
    QVERIFY(script.contains(QStringLiteral("/Users/me/Telemetry")));
}

QTEST_MAIN(AppUpdateTest)
