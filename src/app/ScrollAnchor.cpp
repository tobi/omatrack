#include "ScrollAnchor.h"

#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QTimer>
#include <algorithm>

ScrollAnchor::ScrollAnchor(QObject* parent) : QObject(parent) {}

void ScrollAnchor::setView(QQuickItem* view) {
    if (view_ == view) return;
    if (view_) view_->disconnect(this);
    view_ = view;
    if (view_) {
        // The bound model can be swapped (filter proxies, late binding).
        const int index = view_->metaObject()->indexOfProperty("model");
        if (index >= 0) {
            const QMetaProperty property = view_->metaObject()->property(index);
            if (property.hasNotifySignal()) {
                const QMetaMethod slot =
                    metaObject()->method(metaObject()->indexOfSlot("rebind()"));
                connect(view_, property.notifySignal(), this, slot);
            }
        }
    }
    rebind();
    emit viewChanged();
}

void ScrollAnchor::setRole(const QString& role) {
    if (role_ == role) return;
    role_ = role;
    emit roleChanged();
}

void ScrollAnchor::rebind() {
    QAbstractItemModel* model = nullptr;
    if (view_) {
        const QVariant value = view_->property("model");
        model = value.value<QAbstractItemModel*>();
        if (!model) {
            if (auto* object = value.value<QObject*>())
                model = qobject_cast<QAbstractItemModel*>(object);
        }
    }
    bindModel(model);
}

void ScrollAnchor::bindModel(QAbstractItemModel* model) {
    if (model_ == model) return;
    if (model_) model_->disconnect(this);
    model_ = model;
    if (!model_) return;
    const auto before = [this]() { capture(); };
    const auto after = [this]() {
        if (pending_) return;
        pending_ = true;
        // Let the view finish its own reaction to the change first.
        QTimer::singleShot(0, this, [this]() {
            pending_ = false;
            restore();
        });
    };
    connect(model_, &QAbstractItemModel::rowsAboutToBeInserted, this, before);
    connect(model_, &QAbstractItemModel::rowsAboutToBeRemoved, this, before);
    connect(model_, &QAbstractItemModel::rowsAboutToBeMoved, this, before);
    connect(model_, &QAbstractItemModel::layoutAboutToBeChanged, this, before);
    connect(model_, &QAbstractItemModel::modelAboutToBeReset, this, before);
    connect(model_, &QAbstractItemModel::rowsInserted, this, after);
    connect(model_, &QAbstractItemModel::rowsRemoved, this, after);
    connect(model_, &QAbstractItemModel::rowsMoved, this, after);
    connect(model_, &QAbstractItemModel::layoutChanged, this, after);
    connect(model_, &QAbstractItemModel::modelReset, this, after);
}

int ScrollAnchor::roleId() const {
    if (!model_) return -1;
    const auto names = model_->roleNames();
    const QByteArray wanted = role_.toUtf8();
    for (auto it = names.cbegin(); it != names.cend(); ++it)
        if (it.value() == wanted) return it.key();
    return -1;
}

int ScrollAnchor::findRow(const QVariant& identity) const {
    const int role = roleId();
    if (role < 0 || !identity.isValid()) return -1;
    const int rows = model_->rowCount();
    for (int row = 0; row < rows; ++row)
        if (model_->index(row, 0).data(role) == identity) return row;
    return -1;
}

void ScrollAnchor::capture() {
    anchorIdentity_ = QVariant();
    neighbourIdentities_.clear();
    if (!view_ || !model_ || roleId() < 0) return;
    // A capture during an in-flight restore would record the intermediate
    // layout the view is about to leave.
    if (pending_) return;
    const qreal contentY = view_->property("contentY").toReal();
    const qreal originY = view_->property("originY").toReal();
    if (contentY <= originY + 0.5) return;  // at the top: stay at the top
    int row = -1;
    QMetaObject::invokeMethod(view_, "indexAt", Q_RETURN_ARG(int, row),
                              Q_ARG(double, 1.0),
                              Q_ARG(double, contentY + 1.0));
    if (row < 0) return;
    QQuickItem* item = nullptr;
    QMetaObject::invokeMethod(view_, "itemAtIndex",
                              Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, row));
    if (!item) return;
    const int role = roleId();
    anchorIdentity_ = model_->index(row, 0).data(role);
    anchorOffset_ = contentY - item->y();
    // Fallback candidates, nearest first, in case the anchored row is gone.
    for (int above = row - 1, below = row + 1;
         neighbourIdentities_.size() < 6 &&
         (above >= 0 || below < model_->rowCount());
         --above, ++below) {
        if (above >= 0)
            neighbourIdentities_.append(model_->index(above, 0).data(role));
        if (below < model_->rowCount())
            neighbourIdentities_.append(model_->index(below, 0).data(role));
    }
}

void ScrollAnchor::restore() {
    if (!view_ || !model_ || !anchorIdentity_.isValid()) return;
    int row = findRow(anchorIdentity_);
    qreal offset = anchorOffset_;
    if (row < 0) {
        offset = 0;
        for (const QVariant& identity : std::as_const(neighbourIdentities_)) {
            row = findRow(identity);
            if (row >= 0) break;
        }
    }
    anchorIdentity_ = QVariant();
    neighbourIdentities_.clear();
    if (row < 0) return;
    QMetaObject::invokeMethod(view_, "positionViewAtIndex", Q_ARG(int, row),
                              Q_ARG(int, 0 /* ListView.Beginning */));
    if (offset > 0) {
        QQuickItem* item = nullptr;
        QMetaObject::invokeMethod(view_, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, row));
        if (item) {
            const qreal contentHeight =
                view_->property("contentHeight").toReal();
            const qreal height = view_->height();
            const qreal originY = view_->property("originY").toReal();
            const qreal maxY =
                std::max(originY, originY + contentHeight - height);
            view_->setProperty("contentY",
                               std::clamp(item->y() + offset, originY, maxY));
        }
    }
}
