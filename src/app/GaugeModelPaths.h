#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace omatrack {
// String-only layout selection: safe on the GUI thread. Non-code assets belong
// in Resources on macOS, never in the code-signing-sensitive MacOS directory.
inline QString gaugeModelRootForDirectory(const QString& executableDirectory,
                                          bool macBundle) {
    return QDir::cleanPath(
        QDir(executableDirectory)
            .filePath(macBundle ? QStringLiteral("../Resources/models")
                                : QStringLiteral("models")));
}
inline QString gaugeModelRoot(const QString& executableDirectory =
                                  QCoreApplication::applicationDirPath()) {
#ifdef Q_OS_MACOS
    return gaugeModelRootForDirectory(executableDirectory, true);
#else
    return gaugeModelRootForDirectory(executableDirectory, false);
#endif
}
inline QString gaugeReaderModelPath() {
    return QDir(gaugeModelRoot()).filePath(QStringLiteral("gauge-reader.onnx"));
}
// Read-only CLI/test guard, not a GUI path lookup. Anchor both the expected
// subdirectory and the actual file to the executable's real Contents directory;
// canonicalizing an external Resources/Frameworks symlink alone is
// insufficient.
inline bool gaugeMacPackageContains(const QString& path,
                                    const QString& executableDirectory,
                                    const QString& section) {
    const auto contents =
        QFileInfo(QDir(executableDirectory).filePath("..")).canonicalFilePath();
    const auto expected =
        QFileInfo(QDir(executableDirectory).filePath("../" + section))
            .canonicalFilePath();
    const auto actual = QFileInfo(path).canonicalFilePath();
    return !contents.isEmpty() && !expected.isEmpty() && !actual.isEmpty() &&
           expected.startsWith(contents + '/') &&
           actual.startsWith(contents + '/') &&
           actual.startsWith(expected + '/');
}
}  // namespace omatrack
