#pragma once

#include <QObject>

class AppUpdateTest : public QObject {
    Q_OBJECT

private slots:
    void normalizesTagAndMetadata();
    void comparesSemverComponents();
    void parsesGithubReleaseAsset();
    void selectsWindowsVelopackAssets();
    void selectsWindowsZipAsset();
    void selectsMacDmgAsset();
    void acceptsSinglePlatformRelease();
    void rejectsReleaseWithoutAnyAsset();
    void readsSha256Listing();
    void hashesAndReplacesAppImage();
    void restoreOriginalWhenInstallRenameFails();
    void findsNestedWindowsPayload();
    void writesWindowsApplyScript();
    void findsMacAppBundle();
    void writesMacApplyScript();
};
