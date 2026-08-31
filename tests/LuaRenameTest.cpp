#include "app/LuaRename.h"
#include "app/PathJail.h"

#include <QDir>
#include <QTest>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <mpv/client.h>
#include <clocale>
#include <string>

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

    void sandboxDoesNotInterposeMpvsLuaRuntime() {
        // Link both runtimes, just like the GUI. libmpv may use LuaJIT 5.1;
        // its calls must never bind to our statically bundled Lua 5.4 ABI.
        const auto result = omatrack::runLuaRename(
            QStringLiteral("function rename(ctx) return 'recording.mp4' end"),
            {});
        QVERIFY(result.ok);
        const std::string originalLocale = std::setlocale(LC_NUMERIC, nullptr);
        const auto restoreLocale = qScopeGuard([originalLocale]() {
            std::setlocale(LC_NUMERIC, originalLocale.c_str());
        });
        std::setlocale(LC_NUMERIC, "C");
        mpv_handle* handle = mpv_create();
        QVERIFY(handle);
        const auto cleanup =
            qScopeGuard([handle]() { mpv_terminate_destroy(handle); });
        QCOMPARE(mpv_set_option_string(handle, "config", "no"), 0);
        QCOMPARE(mpv_set_option_string(handle, "terminal", "no"), 0);
        QCOMPARE(mpv_set_option_string(handle, "vo", "null"), 0);
        QVERIFY(mpv_initialize(handle) >= 0);
        // Joining the script threads at teardown also checks that their Lua
        // state was created and freed by the same interpreter.
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

    void jailRejectsSymlinkAncestorBeforeMissingDirectories() {
#ifdef Q_OS_WIN
        QSKIP("POSIX symbolic-link fixture");
#else
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = temp.filePath(QStringLiteral("dest"));
        const QString outside = temp.filePath(QStringLiteral("outside"));
        QVERIFY(QDir().mkpath(root));
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(
            QFile::link(outside, QDir(root).filePath(QStringLiteral("link"))));
        const auto result = omatrack::jailRelativePath(
            root, QStringLiteral("link/new/session/camera.mp4"));
        QVERIFY2(
            !result.ok,
            "A missing tail must not conceal an escaping symlink ancestor");
        QVERIFY(
            !QFileInfo::exists(QDir(outside).filePath(QStringLiteral("new"))));
#endif
    }

    void unknownFormatTokensDoNotSilentlyDisappear() {
        QCOMPARE(
            omatrack::expandCopyFormat(
                QStringLiteral("{unknown}/{original}"),
                {{QStringLiteral("original"), QStringLiteral("camera.mp4")}}),
            QStringLiteral("{unknown}/camera.mp4"));
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
