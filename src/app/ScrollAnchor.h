// Keeps a ListView's viewport pinned to a row *identity* across model
// changes.
//
// A numeric contentY (or a row index) is not a logical anchor: rows inserted
// or removed above the viewport shift every later row, and a model reset
// returns the view to the top. ScrollAnchor records the identity role value
// of the row under the top edge plus its pixel offset right before a
// structural change, then re-finds that identity and restores the same
// on-screen position afterwards. When the anchored row is gone it falls back
// to the nearest surviving neighbour that was above it.
//
// Attach once per store-backed list:
//   ScrollAnchor { view: tree; role: "path" }
#pragma once

#include <QAbstractItemModel>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

class ScrollAnchor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickItem* view READ view WRITE setView NOTIFY viewChanged)
    Q_PROPERTY(QString role READ role WRITE setRole NOTIFY roleChanged)
public:
    explicit ScrollAnchor(QObject* parent = nullptr);

    QQuickItem* view() const { return view_; }
    void setView(QQuickItem* view);
    QString role() const { return role_; }
    void setRole(const QString& role);

    /// Identity of the row under the top edge and its offset. Empty when the
    /// view is at the top or has no rows: then the top is the right answer.
    Q_INVOKABLE void capture();
    /// Restore the captured identity; no-op when nothing was captured.
    Q_INVOKABLE void restore();

signals:
    void viewChanged();
    void roleChanged();

private:
    Q_SLOT void rebind();
    void bindModel(QAbstractItemModel* model);
    int roleId() const;
    int findRow(const QVariant& identity) const;

    QPointer<QQuickItem> view_;
    QPointer<QAbstractItemModel> model_;
    QString role_;
    QVariant anchorIdentity_;
    QVariantList neighbourIdentities_;
    qreal anchorOffset_ = 0;
    bool pending_ = false;
};
