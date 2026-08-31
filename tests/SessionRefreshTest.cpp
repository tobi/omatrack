#include "app/SessionRefresh.h"

#include <QTest>

namespace {
struct Session {
    QString path;
    std::vector<double> cachedLap;
};
auto sessions() {
    std::vector<std::unique_ptr<Session>> result;
    for (const QString& path :
         {QStringLiteral("primary"), QStringLiteral("reference"),
          QStringLiteral("kept"), QStringLiteral("removed")})
        result.push_back(
            std::make_unique<Session>(Session{path, {1.0, 2.0, 3.0}}));
    return result;
}
const auto pathOf = [](const Session& session) { return session.path; };
}  // namespace

class SessionRefreshTest : public QObject {
    Q_OBJECT
private slots:
    void selectedSnapshotsSurviveEvenIfNotInDiscovery() {
        auto registry = sessions();
        auto* primary = registry[0].get();
        auto* reference = registry[1].get();
        const double* data = primary->cachedLap.data();
        omatrack::retainDiscoveredSessions(registry, {QStringLiteral("kept")},
                                           primary, reference, false, pathOf);
        QCOMPARE(registry.size(), size_t(3));
        QCOMPARE(registry[0].get(), primary);
        QCOMPARE(registry[1].get(), reference);
        QCOMPARE(primary->cachedLap.data(), data);
        QCOMPARE(primary->cachedLap[2], 3.0);
    }
    void pendingIntentPreventsPruningItsTarget() {
        auto registry = sessions();
        auto* primary = registry[0].get();
        const auto* pending = registry[3].get();
        omatrack::retainDiscoveredSessions(registry, {}, primary, primary, true,
                                           pathOf);
        QCOMPARE(registry.size(), size_t(4));
        QCOMPARE(registry[3].get(), pending);
        omatrack::retainDiscoveredSessions(registry, {}, primary, primary,
                                           false, pathOf);
        QCOMPARE(registry.size(), size_t(1));
        QCOMPARE(registry.front().get(), primary);
    }
    void reorderDoesNotReconstructSessionObjects() {
        auto registry = sessions();
        auto* kept = registry[2].get();
        omatrack::retainDiscoveredSessions(
            registry, {QStringLiteral("kept")}, static_cast<Session*>(nullptr),
            static_cast<Session*>(nullptr), false, pathOf);
        QCOMPARE(registry.size(), size_t(1));
        QCOMPARE(registry.front().get(), kept);
    }
};
QTEST_GUILESS_MAIN(SessionRefreshTest)
#include "SessionRefreshTest.moc"
