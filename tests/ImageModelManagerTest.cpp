// origin: PUBLIC — synthetic HTTP responses/artifacts only; no model weights.
#include "app/ImageModelManager.h"
#include "app/ImageModelManifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>

using namespace omatrack::image_model;
namespace {
QMap<QString, QString> metadata() {
    return {
        {"omatrack.contract", "omatrack-crop-count-v1"},
        {"omatrack.preprocessing", "pillow-rgb-bilinear-22bit-crop-count-v1"},
        {"omatrack.layout", "tds_aim_orange-1920x1080"},
        {"omatrack.decoder",
         "count-argmax-plus-one-ctc-prefix-beam-10-per-length"},
        {"omatrack.fields", "gear,stint_lap,brake_fill_pct,throttle_fill_pct"},
        {"omatrack.checkpoint_sha256", QString(64, QLatin1Char('c'))}};
}
QByteArray manifest(const QString& version, const QByteArray& body) {
    QJsonObject meta;
    const auto fields = metadata();
    for (auto it = fields.begin(); it != fields.end(); ++it)
        meta.insert(it.key(), it.value());
    return QJsonDocument(
               QJsonObject{{"schema", QLatin1String(ManifestSchema)},
                           {"version", version},
                           {"filename", QLatin1String(ModelFilename)},
                           {"sha256", QString::fromLatin1(
                                          QCryptographicHash::hash(
                                              body, QCryptographicHash::Sha256)
                                              .toHex())},
                           {"size_bytes", body.size()},
                           {"reader_contract", "omatrack-crop-count-v1"},
                           {"min_app_version", "1.8.2"},
                           {"model_metadata", meta}})
        .toJson(QJsonDocument::Compact);
}
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("synthetic fixture write failed");
}
struct FakeHttp {
    std::atomic<qint64> now{1800000000};
    std::atomic<int> release{1}, calls{0}, artifactChecks{0};
    std::atomic<bool> moveMain{false}, badHash{false}, badSize{false},
        failHttp{false}, incompatible{false};
    std::atomic<bool> block{false}, entered{false}, returned{false};
    QSemaphore gate;
    QMutex mutex;
    QStringList urls;
    const QByteArray first = "synthetic compatible artifact one";
    const QByteArray second =
        "synthetic compatible artifact two with a new digest";
    QString revision(int version) const {
        return QString(40, QLatin1Char(version == 1 ? 'a' : 'b'));
    }
    ImageModelServices services(const QString& root) {
        ImageModelServices result;
        result.dataRoot = QDir(root).filePath("data");
        result.cacheRoot = QDir(root).filePath("cache");
        result.appVersion = "1.8.2";
        result.now = [this] { return now.load(); };
        result.transfer = [this](const QUrl& url, const QString& path,
                                 qint64 cap,
                                 const omatrack::DownloadProgress& progress,
                                 const omatrack::IoCancel& cancel) {
            ++calls;
            {
                QMutexLocker lock(&mutex);
                urls.append(url.toString());
            }
            QByteArray body;
            const bool artifact =
                url.path().endsWith(QLatin1String(ModelFilename));
            if (url == revisionUrl()) {
                const int current = release.load();
                body =
                    QJsonDocument(QJsonObject{{"id", QLatin1String(Repository)},
                                              {"private", false},
                                              {"sha", revision(current)}})
                        .toJson();
                if (moveMain) release = 2;
            } else {
                const int version = url.path().contains(revision(1))   ? 1
                                    : url.path().contains(revision(2)) ? 2
                                                                       : 0;
                if (!version)
                    return omatrack::FileDownload{
                        404, "synthetic unknown revision", 0};
                const auto bytes = version == 1 ? first : second;
                if (url.path().endsWith("/manifest.json"))
                    body = manifest(version == 1 ? "1.0.0" : "1.1.0", bytes);
                else if (artifact)
                    body = bytes;
                else
                    return omatrack::FileDownload{404, "synthetic path missing",
                                                  0};
            }
            if (artifact && block) {
                entered = true;
                if (!gate.tryAcquire(1, 5000))
                    return omatrack::FileDownload{
                        0, "synthetic transfer gate timeout", 0};
                returned = true;
            }
            if (omatrack::ioCancelled(cancel))
                return omatrack::FileDownload{0, "cancelled", 0};
            if (artifact && failHttp)
                return omatrack::FileDownload{503, "synthetic HTTP failure", 0};
            if (artifact && badHash) body[0] = char(body[0] ^ 1);
            if (artifact && badSize) body += 'x';
            if (body.size() > cap ||
                (progress && !progress(body.size(), body.size())))
                return omatrack::FileDownload{
                    0, "synthetic size/cancel rejection", body.size()};
            write(path, body);
            return omatrack::FileDownload{200, {}, body.size()};
        };
        result.verifyModel = [this](const QString& path, const Manifest&,
                                    QString* error) {
            const auto bytes = read(path);
            if (path.contains("/.stage-")) ++artifactChecks;
            if (incompatible || (bytes != first && bytes != second)) {
                if (error) *error = "synthetic tensor incompatibility";
                return false;
            }
            return true;
        };
        return result;
    }
};
}  // namespace

class ImageModelManagerTest : public QObject {
    Q_OBJECT
private slots:
    void manifestAndEndpointContract() {
        const auto bytes = manifest("1.0.0", "synthetic");
        QString error;
        QVERIFY(parseManifest(bytes, "1.8.2", &error));
        QVERIFY(!parseManifest(bytes, "1.8.1", &error));
        QVERIFY(!versionParts("1.0"));
        QVERIFY(!versionParts("01.0.0"));
        QVERIFY(!versionParts("1.0.0/../escape"));
        QVERIFY(!versionParts("1.0.0-rc1"));
        QVERIFY(!versionParts("1.0.0\n"));
        QVERIFY(!validSha256(QString(64, '0')));
        QVERIFY(validSha256(QString(64, 'a')));
        QVERIFY(!validSha256(QString(64, 'a') + QLatin1Char('\n')));
        QVERIFY(!manifestUrl(QString(40, 'a') + QLatin1Char('\n')).isValid());
        for (const auto& field :
             {QStringLiteral("schema"), QStringLiteral("filename"),
              QStringLiteral("reader_contract")}) {
            auto object = QJsonDocument::fromJson(bytes).object();
            object.insert(field, "incompatible");
            QVERIFY(!parseManifest(QJsonDocument(object).toJson(), "1.8.2"));
        }
        for (const auto& field : {QStringLiteral("omatrack.layout"),
                                  QStringLiteral("omatrack.decoder"),
                                  QStringLiteral("omatrack.preprocessing"),
                                  QStringLiteral("omatrack.fields")}) {
            auto object = QJsonDocument::fromJson(bytes).object();
            auto meta = object["model_metadata"].toObject();
            meta.insert(field, "incompatible");
            object.insert("model_metadata", meta);
            QVERIFY(!parseManifest(QJsonDocument(object).toJson(), "1.8.2"));
        }
        auto object = QJsonDocument::fromJson(bytes).object();
        object.insert("url", "https://evil.invalid/model.onnx");
        QVERIFY(!parseManifest(QJsonDocument(object).toJson(), "1.8.2"));
        object = QJsonDocument::fromJson(bytes).object();
        object.insert("size_bytes", double(MaximumModelBytes + 1));
        QVERIFY(!parseManifest(QJsonDocument(object).toJson(), "1.8.2"));
        QVERIFY(
            !parseManifest(QByteArray(MaximumManifestBytes + 1, ' '), "1.8.2"));
        for (const char* url : {"https://huggingface.co/x",
                                "https://us.aws.cdn.hf.co/x?public=signed",
                                "https://cas-bridge.xethub.hf.co/x"})
            QVERIFY(allowedDownloadUrl(QUrl(QLatin1String(url))));
        for (const char* url :
             {"http://huggingface.co/x",
              "https://huggingface.co.evil.invalid/x", "file:///tmp/model",
              "https://user:pass@huggingface.co/x", "https://@huggingface.co/x",
              "https://huggingface.co:444/x",
              "https://huggingface.co/x#fragment", "https://huggingface.co/x#",
              "https://user.hf.space/x", "https://unlisted.hf.co/x"})
            QVERIFY(!allowedDownloadUrl(QUrl(QLatin1String(url))));
        QVERIFY(!modelUrl("main").isValid());
        QVERIFY(!manifestUrl("../main").isValid());
    }
    void noConsentNoNetworkAndLocalPathPreserved() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        const QString local = root.filePath("custom.onnx");
        write(local, "local user's model");
        manager.setActivePath(local);
        manager.checkForUpdates();
        manager.downloadLatest();
        manager.applyPending();
        QTest::qWait(50);
        QCOMPARE(manager.networkRequestCount(), quint64(0));
        QCOMPARE(http.calls.load(), 0);
        QVERIFY(!manager.busy());
        QCOMPARE(manager.activePath(), local);
        QVERIFY(!QFileInfo::exists(root.filePath("data")));
        QVERIFY(!QFileInfo::exists(root.filePath("cache")));
    }
    void immutablePinAndDeferredExplicitApply() {
        QTemporaryDir root;
        FakeHttp http;
        http.moveMain = true;
        ImageModelManager manager(http.services(root.path()));
        manager.setAutoUpdate(false);
        manager.setActivationBlocked(true);
        QSignalSpy activated(&manager, &ImageModelManager::modelActivated);
        manager.setManaged(true);
        manager.downloadLatest();
        QTRY_VERIFY_WITH_TIMEOUT(manager.readyToApply() && !manager.busy(),
                                 3000);
        QCOMPARE(manager.pendingVersion(), QString("1.0.0"));
        QCOMPARE(activated.count(), 0);
        QCOMPARE(read(manager.pendingPath()), http.first);
        {
            QMutexLocker lock(&http.mutex);
            QVERIFY(
                http.urls.contains(manifestUrl(http.revision(1)).toString()));
            QVERIFY(http.urls.contains(modelUrl(http.revision(1)).toString()));
            QVERIFY(!http.urls.contains(modelUrl(http.revision(2)).toString()));
        }
        manager.applyPending();
        QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, 3000);
        QCOMPARE(manager.installedVersion(), QString("1.0.0"));
        QVERIFY(manager.activationBlocked());
        const QString selected = manager.activePath();
        manager.setManaged(false);
        QCOMPARE(manager.activePath(), selected);
        QCOMPARE(read(selected), http.first);
        const auto count = manager.networkRequestCount();
        manager.downloadLatest();
        manager.checkForUpdates();
        QTest::qWait(30);
        QCOMPARE(manager.networkRequestCount(), count);
    }
    void automaticActivationWaitsForVideo() {
        QTemporaryDir root;
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        QSignalSpy activated(&manager, &ImageModelManager::modelActivated);
        manager.setActivationBlocked(true);
        manager.setManaged(true);
        QTRY_VERIFY_WITH_TIMEOUT(manager.readyToApply() && !manager.busy(),
                                 3000);
        QCOMPARE(activated.count(), 0);
        manager.setActivationBlocked(false);
        QTRY_COMPARE_WITH_TIMEOUT(activated.count(), 1, 3000);
        QCOMPARE(manager.installedVersion(), QString("1.0.0"));
    }
    void hashSizeHttpAndCompatibilityRollback() {
        QTemporaryDir root;
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        manager.setAutoUpdate(false);
        manager.setManaged(true);
        manager.downloadLatest();
        QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(), QString("1.0.0"),
                                  3000);
        const auto oldPath = manager.activePath();
        const auto oldBytes = read(oldPath);
        http.release = 2;
        http.now += 31;
        manager.checkForUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(manager.updateAvailable() && !manager.busy(),
                                 3000);
        for (auto* failure : {&http.badHash, &http.badSize, &http.failHttp,
                              &http.incompatible}) {
            *failure = true;
            manager.downloadLatest();
            QTRY_VERIFY_WITH_TIMEOUT(
                !manager.busy() && !manager.error().isEmpty(), 3000);
            QCOMPARE(manager.activePath(), oldPath);
            QCOMPARE(read(oldPath), oldBytes);
            QCOMPARE(manager.installedVersion(), QString("1.0.0"));
            QVERIFY(!manager.readyToApply());
            QVERIFY(QDir(root.filePath("data"))
                        .entryList({".stage-*"}, QDir::Dirs | QDir::Hidden |
                                                     QDir::NoDotAndDotDot)
                        .isEmpty());
            *failure = false;
        }
        manager.downloadLatest();
        QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(), QString("1.1.0"),
                                  3000);
        QVERIFY(manager.activePath() != oldPath);
        QCOMPARE(read(oldPath), oldBytes);
    }
    void cancelAndOptOutCannotActivateStaleDownload() {
        QTemporaryDir root;
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        manager.setAutoUpdate(false);
        manager.setManaged(true);
        manager.downloadLatest();
        QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(), QString("1.0.0"),
                                  3000);
        const auto previous = manager.activePath();
        QSignalSpy activated(&manager, &ImageModelManager::modelActivated);
        http.release = 2;
        http.now += 31;
        manager.checkForUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(manager.updateAvailable() && !manager.busy(),
                                 3000);
        http.block = true;
        manager.downloadLatest();
        QTRY_VERIFY_WITH_TIMEOUT(http.entered.load(), 3000);
        manager.cancel();
        manager.setManaged(false);
        const QString local = root.filePath("manual-choice.onnx");
        write(local, "manual local choice");
        manager.setActivePath(local);
        http.gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(http.returned.load(), 3000);
        QTest::qWait(50);
        QCOMPARE(activated.count(), 0);
        QCOMPARE(manager.activePath(), local);
        QCOMPARE(read(previous), http.first);
        QVERIFY(!manager.readyToApply());
    }
    void optOutAtFinalProgressCannotActivate() {
        QTemporaryDir root;
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        manager.setAutoUpdate(false);
        QSignalSpy activated(&manager, &ImageModelManager::modelActivated);
        connect(&manager, &ImageModelManager::progressChanged, &manager,
                [&manager] {
                    if (manager.progress() >= 1) manager.setManaged(false);
                });
        manager.setManaged(true);
        manager.downloadLatest();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.managed(), 3000);
        QTest::qWait(50);
        QCOMPARE(activated.count(), 0);
        QVERIFY(manager.activePath().isEmpty());
    }
    void reentrantOptOutBeforeTransport() {
        QTemporaryDir root;
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        connect(&manager, &ImageModelManager::stateChanged, &manager,
                [&manager] {
                    if (manager.busy()) manager.setManaged(false);
                });
        manager.setManaged(true);
        QTest::qWait(50);
        QCOMPARE(manager.networkRequestCount(), quint64(0));
        QVERIFY(!manager.managed());
    }
    void clockRollbackDoesNotFreezeUpdates() {
        QTemporaryDir root;
        FakeHttp http;
        ImageModelManager manager(http.services(root.path()));
        manager.setManaged(true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(), QString("1.0.0"),
                                  3000);
        const auto before = http.calls.load();
        http.now -= 3600;
        http.release = 2;
        manager.setAutoUpdate(false);
        manager.setAutoUpdate(true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(), QString("1.1.0"),
                                  3000);
        QVERIFY(http.calls.load() > before);
    }
    void dailyAutomaticAndShortManualCooldownPersist() {
        QTemporaryDir root;
        FakeHttp http;
        QString active;
        {
            ImageModelManager manager(http.services(root.path()));
            manager.setManaged(true);
            QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(),
                                      QString("1.0.0"), 3000);
            active = manager.activePath();
            const auto count = http.calls.load();
            manager.checkForUpdates();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
            QCOMPARE(http.calls.load(), count);
            manager.setAutoUpdate(false);
            manager.setAutoUpdate(true);
            QTest::qWait(20);
            QCOMPARE(http.calls.load(), count);
        }
        ImageModelManager manager(http.services(root.path()));
        manager.setActivePath(active);
        manager.setManaged(true);
        const auto previous = http.calls.load();
        QTRY_VERIFY_WITH_TIMEOUT(
            !manager.busy() && manager.installedVersion() == QString("1.0.0"),
            3000);
        QCOMPARE(http.calls.load(), previous);
        http.now += 31;
        manager.checkForUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy(), 3000);
        QCOMPARE(http.calls.load(), previous + 2);
        http.release = 2;
        http.now += 86401;
        manager.setAutoUpdate(false);
        manager.setAutoUpdate(true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.installedVersion(), QString("1.1.0"),
                                  3000);
        const auto newer = manager.activePath();
        http.release = 1;
        http.now += 31;
        manager.checkForUpdates();
        QTRY_VERIFY_WITH_TIMEOUT(!manager.busy() && !manager.error().isEmpty(),
                                 3000);
        QCOMPARE(manager.activePath(), newer);
        QCOMPARE(manager.installedVersion(), QString("1.1.0"));
    }
};
QTEST_GUILESS_MAIN(ImageModelManagerTest)
#include "ImageModelManagerTest.moc"
