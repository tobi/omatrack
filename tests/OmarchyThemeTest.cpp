#include "OmarchyTheme.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), contents.size());
}

}  // namespace

class OmarchyThemeTest : public QObject {
    Q_OBJECT

private slots:
    void reloadsAfterAtomicThemeSwap();
    void staysInactiveOutsideOmarchy();
};

void OmarchyThemeTest::reloadsAfterAtomicThemeSwap() {
#ifndef Q_OS_LINUX
    QSKIP("Omarchy theme monitoring is Linux-only");
#else
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString omarchyPath = temporary.path() + QStringLiteral("/install");
    const QString stateHome = temporary.path() + QStringLiteral("/state");
    const QString currentPath = stateHome + QStringLiteral("/omarchy/current");
    QVERIFY(QDir().mkpath(omarchyPath));
    QVERIFY(QDir().mkpath(currentPath + QStringLiteral("/theme")));
    writeFile(currentPath + QStringLiteral("/theme.name"), "old\n");
    writeFile(currentPath + QStringLiteral("/theme/colors.toml"),
              "accent = \"#112233\"\nbackground = \"#010203\"\n");

    qputenv("OMARCHY_PATH", omarchyPath.toUtf8());
    qputenv("XDG_STATE_HOME", stateHome.toUtf8());
    qputenv("DESKTOP_SESSION", "omarchy");
    OmarchyTheme theme;
    QCOMPARE(theme.colors().value(QStringLiteral("accent")).toString(),
             QStringLiteral("#112233"));
    QSignalSpy changed(&theme, &OmarchyTheme::colorsChanged);

    QDir oldTheme(currentPath + QStringLiteral("/theme"));
    QVERIFY(oldTheme.removeRecursively());
    QVERIFY(QDir().mkpath(currentPath + QStringLiteral("/theme")));
    writeFile(currentPath + QStringLiteral("/theme/colors.toml"),
              "accent = \"#aabbcc\"\nbackground = \"#101112\"\n");
    writeFile(currentPath + QStringLiteral("/theme.name"), "new\n");

    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 2000);
    QCOMPARE(theme.colors().value(QStringLiteral("accent")).toString(),
             QStringLiteral("#aabbcc"));
#endif
}

void OmarchyThemeTest::staysInactiveOutsideOmarchy() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString omarchyPath = temporary.path() + QStringLiteral("/install");
    const QString stateHome = temporary.path() + QStringLiteral("/state");
    const QString currentPath = stateHome + QStringLiteral("/omarchy/current");
    QVERIFY(QDir().mkpath(omarchyPath));
    QVERIFY(QDir().mkpath(currentPath + QStringLiteral("/theme")));
    writeFile(currentPath + QStringLiteral("/theme/colors.toml"),
              "accent = \"#112233\"\n");

    qputenv("OMARCHY_PATH", omarchyPath.toUtf8());
    qputenv("XDG_STATE_HOME", stateHome.toUtf8());
    qputenv("DESKTOP_SESSION", "plasma");
    OmarchyTheme theme;
    QVERIFY(theme.colors().isEmpty());
}

QTEST_MAIN(OmarchyThemeTest)
#include "OmarchyThemeTest.moc"
