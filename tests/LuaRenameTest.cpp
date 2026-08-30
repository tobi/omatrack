#include "app/LuaRename.h"
#include "app/PathJail.h"

#include <QDir>
#include <QTest>

class LuaRenameTest : public QObject {
    Q_OBJECT
private slots:
    void emptyScriptOk() {
        const auto result = omatrack::runLuaRename({}, {});
        QVERIFY(result.ok);
        QVERIFY(result.relativePath.isEmpty());
    }

    void returnsRelativePath() {
        const auto result = omatrack::runLuaRename(
            QStringLiteral("function rename(ctx) return ctx.track .. "
                           "\"/\" .. ctx.original end"),
            QVariantMap{{QStringLiteral("track"), QStringLiteral("sebring")},
                        {QStringLiteral("original"), QStringLiteral("a.ld")}});
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.relativePath, QStringLiteral("sebring/a.ld"));
    }

    void loadIsBlocked() {
        const auto result = omatrack::runLuaRename(
            QStringLiteral("function rename(ctx) return load(\"return 1\")() "
                           ".. \"x\" end"),
            {});
        QVERIFY(!result.ok);
    }

    void stringRepIsCapped() {
        const auto result = omatrack::runLuaRename(
            QStringLiteral("function rename(ctx) return string.rep(\"a\", "
                           "10000000) end"),
            {}, 50, 64 * 1024);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains(QStringLiteral("memory")) ||
                result.error.contains(QStringLiteral("timed")));
    }

    void jailRejectsDotDot() {
        const auto jailed = omatrack::jailRelativePath(
            QStringLiteral("/tmp/dest"), QStringLiteral("../etc/passwd"));
        QVERIFY(!jailed.ok);
    }

    void jailRejectsAbsolute() {
        QVERIFY(omatrack::isForbiddenRelative(QStringLiteral("/etc/passwd")));
        QVERIFY(omatrack::isForbiddenRelative(QStringLiteral("C:/Windows")));
        QVERIFY(
            omatrack::isForbiddenRelative(QStringLiteral("\\\\server\\share")));
    }

    void jailAcceptsNested() {
        const QString dest =
            QDir::temp().filePath(QStringLiteral("omatrack-jail"));
        QVERIFY(QDir().mkpath(dest));
        const auto jailed = omatrack::jailRelativePath(
            dest, QStringLiteral("sebring/2026-08-30/c1/a.ld"));
        QVERIFY2(jailed.ok, qPrintable(jailed.error));
        QVERIFY(jailed.absolutePath.startsWith(QDir(dest).canonicalPath()) ||
                jailed.absolutePath.startsWith(QDir::cleanPath(dest)));
    }
};

QTEST_GUILESS_MAIN(LuaRenameTest)
#include "LuaRenameTest.moc"
