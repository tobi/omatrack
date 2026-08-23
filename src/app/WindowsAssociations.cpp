#include "WindowsAssociations.h"

#ifdef Q_OS_WIN

#include <QDir>
#include <QFileInfo>

#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

namespace omatrack {
namespace {

QString exePath() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return QString::fromWCharArray(buffer, int(length));
}

QString progIdFor(const QString& extension) {
    return QStringLiteral("Omatrack.") + extension.toLower();
}

bool setDefaultValue(const QString& subkey, const QString& value) {
    HKEY key = nullptr;
    const QString path = QStringLiteral("Software\\Classes\\") + subkey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        reinterpret_cast<LPCWSTR>(path.utf16()), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring wide = value.toStdWString();
    const LONG status = RegSetValueExW(
        key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(wide.c_str()),
        DWORD((wide.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool deleteKeyTree(const QString& subkey) {
    const QString path = QStringLiteral("Software\\Classes\\") + subkey;
    return RegDeleteTreeW(HKEY_CURRENT_USER,
                          reinterpret_cast<LPCWSTR>(path.utf16())) ==
               ERROR_SUCCESS ||
           GetLastError() == ERROR_FILE_NOT_FOUND;
}

void notifyShell() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

bool writeAssociation(const FileAssociation& association, const QString& exe) {
    const QString ext = association.extension.toLower();
    const QString progId = progIdFor(ext);
    const QString command =
        QStringLiteral("\"%1\" \"%2\"")
            .arg(QDir::toNativeSeparators(exe), QStringLiteral("%1"));
    if (!setDefaultValue(QStringLiteral(".") + ext, progId)) return false;
    if (!setDefaultValue(progId, association.description)) return false;
    if (!setDefaultValue(progId + QStringLiteral("\\DefaultIcon"),
                         QDir::toNativeSeparators(exe) + QStringLiteral(",0")))
        return false;
    return setDefaultValue(progId + QStringLiteral("\\shell\\open\\command"),
                           command);
}

}  // namespace

QVector<FileAssociation> fileAssociations() {
    return {
        {QStringLiteral("pds"), QStringLiteral("Pi/Cosworth telemetry session"),
         false, true},
        {QStringLiteral("ld"), QStringLiteral("MoTeC telemetry session"), false,
         true},
        // A MoTeC layout is not a recording, but it is the file i2 users
        // double-click; the store opens the sibling `.ld` for it.
        {QStringLiteral("ldx"), QStringLiteral("MoTeC telemetry layout"), false,
         true},
        {QStringLiteral("vbo"),
         QStringLiteral("Racelogic VBOX telemetry session"), false, true},
        {QStringLiteral("telemetry"), QStringLiteral("Omatrack telemetry"),
         false, true},
        {QStringLiteral("mp4"), QStringLiteral("MPEG-4 video"), true, false},
    };
}

bool consumeWindowsSetupHook(int argc, char** argv) {
    QString hook;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--veloapp-install") ||
            argument == QStringLiteral("--veloapp-updated") ||
            argument == QStringLiteral("--veloapp-obsolete") ||
            argument == QStringLiteral("--veloapp-uninstall")) {
            hook = argument;
            break;
        }
    }
    if (hook.isEmpty()) return false;
    if (hook == QStringLiteral("--veloapp-install") ||
        hook == QStringLiteral("--veloapp-updated"))
        registerDefaultAssociations();
    else if (hook == QStringLiteral("--veloapp-uninstall"))
        unregisterAllAssociations();
    return true;
}

bool associationEnabled(const QString& extension) {
    const QString ext = extension.toLower();
    const QString path = QStringLiteral("Software\\Classes\\.") + ext;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      reinterpret_cast<LPCWSTR>(path.utf16()), 0, KEY_READ,
                      &key) != ERROR_SUCCESS)
        return false;
    wchar_t value[256];
    DWORD bytes = sizeof(value);
    const LONG status =
        RegQueryValueExW(key, nullptr, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(value), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return false;
    return QString::fromWCharArray(value) == progIdFor(ext);
}

bool setAssociationEnabled(const QString& extension, bool enabled) {
    const QString ext = extension.toLower();
    FileAssociation match;
    bool found = false;
    for (const FileAssociation& association : fileAssociations()) {
        if (association.extension == ext) {
            match = association;
            found = true;
            break;
        }
    }
    if (!found) return false;
    bool ok = false;
    if (enabled)
        ok = writeAssociation(match, exePath());
    else {
        deleteKeyTree(QStringLiteral(".") + ext);
        ok = deleteKeyTree(progIdFor(ext));
    }
    notifyShell();
    return ok;
}

void registerDefaultAssociations() {
    const QString exe = exePath();
    if (exe.isEmpty()) return;
    for (const FileAssociation& association : fileAssociations()) {
        if (association.defaultEnabled) writeAssociation(association, exe);
    }
    notifyShell();
}

void unregisterAllAssociations() {
    for (const FileAssociation& association : fileAssociations()) {
        deleteKeyTree(QStringLiteral(".") + association.extension);
        deleteKeyTree(progIdFor(association.extension));
    }
    notifyShell();
}

bool velopackFirstRun() {
    const QByteArray value = qgetenv("VELOPACK_FIRSTRUN");
    return !value.isEmpty() && value != "0";
}

QString velopackUpdateExe(const QString& applicationDir) {
    const QString candidate =
        QFileInfo(applicationDir + QStringLiteral("/../Update.exe"))
            .absoluteFilePath();
    return QFileInfo::exists(candidate) ? candidate : QString();
}

}  // namespace omatrack

#endif
