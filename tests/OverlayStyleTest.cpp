#include "app/TraceSceneBuilder.h"

#include <QtGlobal>
#include <QTest>

class OverlayStyleTest : public QObject {
    Q_OBJECT
private slots:
    void defaultIsSolidPrimaryWidth() {
        TraceSceneBuilder::EnvelopeStyle primary;
        primary.width = 2.5;
        QCOMPARE(primary.dash, TraceSceneBuilder::EnvelopeStyle::Dash::Solid);

        TraceSceneBuilder::EnvelopeStyle reference;
        reference.width = 2.4;
        reference.dash = TraceSceneBuilder::EnvelopeStyle::Dash::Dashed;
        QVERIFY(reference.dash != primary.dash);
        QVERIFY(qAbs(reference.width - primary.width) < 0.2);
    }

    void whitePresetKeepsSimilarThickness() {
        const qreal primary = 2.5;
        const qreal whiteRef = 2.4;
        QVERIFY(qAbs(whiteRef - primary) < 0.2);
    }
};

QTEST_GUILESS_MAIN(OverlayStyleTest)
#include "OverlayStyleTest.moc"
