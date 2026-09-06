#include "app/WindowsAssociations.h"

#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QTest>

#ifdef Q_OS_WIN
#include <QUuid>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

bool writeRegistry(const QString& path, const QString& value,
                   const QString& name = {}) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        reinterpret_cast<LPCWSTR>(path.utf16()), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS status = RegSetValueExW(
        key, name.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(name.utf16()),
        0, REG_SZ, reinterpret_cast<const BYTE*>(value.utf16()),
        DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

QString readRegistry(const QString& path, const QString& name = {}) {
    wchar_t value[32768]{};
    DWORD bytes = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()),
        name.isEmpty() ? nullptr : reinterpret_cast<LPCWSTR>(name.utf16()),
        RRF_RT_REG_SZ, nullptr, value, &bytes);
    return status == ERROR_SUCCESS ? QString::fromWCharArray(value) : QString();
}

bool keyExists(const QString& path) {
    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(path.utf16()), 0, KEY_READ,
        &key);
    if (status == ERROR_SUCCESS) RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace
#endif

class WindowsAssociationsTest : public QObject {
    Q_OBJECT

#ifdef Q_OS_WIN
    HKEY sandbox_ = nullptr;
    QString sandboxPath_;
    bool redirected_ = false;
#endif

private slots:
#ifdef Q_OS_WIN
    void init() {
        // Fail closed BEFORE calling any production registry API. All HKCU
        // access in this process is redirected to a unique isolated test key;
        // neither Software\Classes nor Explorer's real UserChoice is touched.
        sandboxPath_ = QStringLiteral("Software\\OmatrackAssociationTests\\") +
                       QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(
            RegCreateKeyExW(HKEY_CURRENT_USER,
                            reinterpret_cast<LPCWSTR>(sandboxPath_.utf16()), 0,
                            nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                            nullptr, &sandbox_, nullptr),
            LSTATUS(ERROR_SUCCESS));
        const LSTATUS status =
            RegOverridePredefKey(HKEY_CURRENT_USER, sandbox_);
        redirected_ = status == ERROR_SUCCESS;
        QVERIFY2(redirected_,
                 "Refusing to test against the real HKCU registry");
    }

    void cleanup() {
        if (redirected_) {
            if (RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) !=
                ERROR_SUCCESS)
                qFatal("Could not restore HKCU after association sandbox test");
            redirected_ = false;
        }
        if (sandbox_) {
            RegCloseKey(sandbox_);
            sandbox_ = nullptr;
            QCOMPARE(
                RegDeleteTreeW(HKEY_CURRENT_USER,
                               reinterpret_cast<LPCWSTR>(sandboxPath_.utf16())),
                LSTATUS(ERROR_SUCCESS));
        }
    }
#endif

    void catalogDefaults() {
        const QSet<QString> expectedTelemetry{
            QStringLiteral("pds"), QStringLiteral("ld"), QStringLiteral("ldx"),
            QStringLiteral("vbo"), QStringLiteral("telemetry")};
        const QSet<QString> expectedVideo{
            QStringLiteral("mp4"), QStringLiteral("mov"),
            QStringLiteral("mkv"), QStringLiteral("avi"),
            QStringLiteral("m4v"), QStringLiteral("webm")};
        QSet<QString> telemetry;
        QSet<QString> video;
        QSet<QString> seen;
        for (const auto& association : omatrack::fileAssociations()) {
            QVERIFY(!seen.contains(association.extension));
            seen.insert(association.extension);
            QVERIFY(!association.description.isEmpty());
            QCOMPARE(association.extension, association.extension.toLower());
            QVERIFY(!association.extension.startsWith(QLatin1Char('.')));
            QCOMPARE(association.defaultEnabled, !association.video);
            (association.video ? video : telemetry)
                .insert(association.extension);
        }
        QCOMPARE(telemetry, expectedTelemetry);
        QCOMPARE(video, expectedVideo);
    }

#ifdef Q_OS_WIN
    void videoOptIn_data() {
        QTest::addColumn<QString>("extension");
        for (const auto& association : omatrack::fileAssociations()) {
            if (association.video)
                QTest::newRow(qPrintable(association.extension))
                    << association.extension;
        }
    }

    void videoOptIn() {
        QFETCH(QString, extension);
        const QString path = QStringLiteral("Software\\Classes\\.") + extension;
        const QString progId = QStringLiteral("Omatrack.") + extension;
        const QString openWith = path + QStringLiteral("\\OpenWithProgids");
        const QString userChoice =
            QStringLiteral(
                "Software\\Microsoft\\Windows\\CurrentVersion\\"
                "Explorer\\FileExts\\.") +
            extension + QStringLiteral("\\UserChoice");
        QVERIFY(writeRegistry(path, QStringLiteral("OtherPlayer.Video")));
        QVERIFY(writeRegistry(path, QStringLiteral("video/test"),
                              QStringLiteral("Content Type")));
        QVERIFY(writeRegistry(openWith, QStringLiteral("keep"),
                              QStringLiteral("OtherPlayer.Video")));
        QVERIFY(writeRegistry(userChoice, QStringLiteral("OtherPlayer.Video"),
                              QStringLiteral("ProgId")));
        QVERIFY(!omatrack::associationEnabled(extension));
        QVERIFY(omatrack::setAssociationEnabled(extension.toUpper(), true));
        QVERIFY(omatrack::associationEnabled(extension.toUpper()));
        QCOMPARE(readRegistry(path), progId);
        const QString command =
            readRegistry(QStringLiteral("Software\\Classes\\") + progId +
                         QStringLiteral("\\shell\\open\\command"));
        QCOMPARE(command, QStringLiteral("\"%1\" \"%2\"")
                              .arg(QDir::toNativeSeparators(
                                       QCoreApplication::applicationFilePath()),
                                   QStringLiteral("%1")));
        QVERIFY(omatrack::setAssociationEnabled(extension, false));
        QVERIFY(!omatrack::associationEnabled(extension));
        QVERIFY(readRegistry(path).isEmpty());
        QVERIFY(!keyExists(QStringLiteral("Software\\Classes\\") + progId));
        QCOMPARE(readRegistry(path, QStringLiteral("Content Type")),
                 QStringLiteral("video/test"));
        QCOMPARE(readRegistry(openWith, QStringLiteral("OtherPlayer.Video")),
                 QStringLiteral("keep"));
        QVERIFY(readRegistry(openWith, progId).isNull());
        QCOMPARE(readRegistry(userChoice, QStringLiteral("ProgId")),
                 QStringLiteral("OtherPlayer.Video"));
        // Repeated disable is harmless when our default / ProgID is absent.
        QVERIFY(omatrack::setAssociationEnabled(extension, false));
    }

    void defaultsLeaveVideoUntouched() {
        for (const auto& association : omatrack::fileAssociations()) {
            if (association.video)
                QVERIFY(writeRegistry(QStringLiteral("Software\\Classes\\.") +
                                          association.extension,
                                      QStringLiteral("OtherPlayer.Video")));
        }
        omatrack::registerDefaultAssociations();
        for (const auto& association : omatrack::fileAssociations()) {
            QCOMPARE(omatrack::associationEnabled(association.extension),
                     association.defaultEnabled);
            if (association.video) {
                QCOMPARE(readRegistry(QStringLiteral("Software\\Classes\\.") +
                                      association.extension),
                         QStringLiteral("OtherPlayer.Video"));
                QVERIFY(
                    !keyExists(QStringLiteral("Software\\Classes\\Omatrack.") +
                               association.extension));
            }
        }
        // An update must neither enroll other videos nor revoke an opt-in.
        QVERIFY(omatrack::setAssociationEnabled(QStringLiteral("mov"), true));
        omatrack::registerDefaultAssociations();
        QVERIFY(omatrack::associationEnabled(QStringLiteral("mov")));
        QVERIFY(!omatrack::associationEnabled(QStringLiteral("mp4")));
        omatrack::unregisterAllAssociations();
        for (const auto& association : omatrack::fileAssociations()) {
            // Uninstall must also leave videos we NEVER registered alone.
            if (association.video &&
                association.extension != QStringLiteral("mov"))
                QCOMPARE(readRegistry(QStringLiteral("Software\\Classes\\.") +
                                      association.extension),
                         QStringLiteral("OtherPlayer.Video"));
        }
    }

    void uninstallPreservesOtherApplications() {
        for (const auto& association : omatrack::fileAssociations()) {
            QVERIFY(
                omatrack::setAssociationEnabled(association.extension, true));
            const QString path =
                QStringLiteral("Software\\Classes\\.") + association.extension;
            // Simulate another application taking over after Omatrack.
            QVERIFY(writeRegistry(path, QStringLiteral("OtherPlayer.File")));
            QVERIFY(writeRegistry(path + QStringLiteral("\\OpenWithProgids"),
                                  QStringLiteral("keep"),
                                  QStringLiteral("OtherPlayer.File")));
        }
        QVERIFY(omatrack::setAssociationEnabled(QStringLiteral("mp4"), false));
        omatrack::unregisterAllAssociations();
        omatrack::unregisterAllAssociations();
        for (const auto& association : omatrack::fileAssociations()) {
            const QString path =
                QStringLiteral("Software\\Classes\\.") + association.extension;
            QCOMPARE(readRegistry(path), QStringLiteral("OtherPlayer.File"));
            QCOMPARE(readRegistry(path + QStringLiteral("\\OpenWithProgids"),
                                  QStringLiteral("OtherPlayer.File")),
                     QStringLiteral("keep"));
            QVERIFY(!keyExists(QStringLiteral("Software\\Classes\\Omatrack.") +
                               association.extension));
        }
    }

    void rejectsUnsupportedTypes() {
        for (const QString& extension :
             {QString(), QStringLiteral("exe"), QStringLiteral("mp3"),
              QStringLiteral(".mp4"), QStringLiteral("mp4\\shell")}) {
            QVERIFY(!omatrack::setAssociationEnabled(extension, true));
            QVERIFY(!omatrack::setAssociationEnabled(extension, false));
        }
        QVERIFY(!keyExists(QStringLiteral("Software\\Classes")));
    }
#endif
};

QTEST_GUILESS_MAIN(WindowsAssociationsTest)
#include "windows_associations_test.moc"
