#include "OmarchyTheme.h"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>

namespace {

QVariantMap loadOmarchyColors() {
    QVariantMap colors;
    QProcess themeProcess;
    themeProcess.start(QStringLiteral("omarchy"),
                       {QStringLiteral("theme"), QStringLiteral("current")});
    if (!themeProcess.waitForFinished(500)) return colors;
    QString theme = QString::fromUtf8(themeProcess.readAllStandardOutput())
                        .trimmed()
                        .toLower();
    static const QRegularExpression unsafe(QStringLiteral("[^a-z0-9-]"));
    theme.replace(unsafe, QStringLiteral("-"));
    if (theme.isEmpty()) return colors;
    const QString omarchyPath = qEnvironmentVariable(
        "OMARCHY_PATH", QStringLiteral("/usr/share/omarchy"));
    QFile file(omarchyPath + QStringLiteral("/themes/") + theme +
               QStringLiteral("/colors.toml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return colors;
    static const QRegularExpression assignment(
        QStringLiteral("^\\s*([a-z_]+)\\s*=\\s*\"(#[0-9a-fA-F]{6,8})\""));
    const QString text = QString::fromUtf8(file.readAll());
    const QStringList lines = text.split('\n');
    for (const QString& line : lines) {
        const QRegularExpressionMatch match = assignment.match(line);
        if (match.hasMatch())
            colors.insert(match.captured(1), match.captured(2));
    }
    return colors;
}

}  // namespace

OmarchyTheme::OmarchyTheme(QObject* parent)
    : QObject(parent), colors_(loadOmarchyColors()) {}
