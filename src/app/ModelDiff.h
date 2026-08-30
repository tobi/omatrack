// Identity-preserving list updates for QAbstractListModel.
//
// beginResetModel() rebuilds every QML delegate and jumps Flickable.contentY
// to 0. Refresh and filter must insert/remove/dataChanged against a stable
// row identity instead. LibraryModel and the StoreModels share this helper.
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>
#include <utility>

class IdentityListModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

protected:
    /// Replace `rows` with `next` by identity. Existing identities keep
    /// their delegates; missing rows are removed and new ones inserted.
    /// `identity` returns a stable key; rows with the same key are assigned
    /// through and emit dataChanged when `unchanged` is false.
    template <typename T, typename IdentityFn, typename UnchangedFn>
    void replaceByIdentity(QVector<T>& rows, QVector<T> next,
                           IdentityFn identity, UnchangedFn unchanged) {
        QHash<QString, int> nextIndex;
        nextIndex.reserve(next.size());
        for (int i = 0; i < next.size(); ++i)
            nextIndex.insert(identity(next.at(i)), i);

        for (int i = rows.size() - 1; i >= 0; --i) {
            if (nextIndex.contains(identity(rows.at(i)))) continue;
            beginRemoveRows(QModelIndex(), i, i);
            rows.removeAt(i);
            endRemoveRows();
        }

        QHash<QString, int> currentIndex;
        const auto rebuildCurrent = [&]() {
            currentIndex.clear();
            currentIndex.reserve(rows.size());
            for (int i = 0; i < rows.size(); ++i)
                currentIndex.insert(identity(rows.at(i)), i);
        };
        rebuildCurrent();

        for (int i = 0; i < next.size(); ++i) {
            const QString id = identity(next.at(i));
            const auto found = currentIndex.constFind(id);
            if (found == currentIndex.cend()) {
                beginInsertRows(QModelIndex(), i, i);
                rows.insert(i, std::move(next[i]));
                endInsertRows();
                rebuildCurrent();
                continue;
            }
            const int from = found.value();
            if (from != i) {
                // Qt documents destination as the index *before* the move
                // for a downward move; beginMoveRows wants the row the
                // source will sit at after removal of `from`.
                const int destination = from < i ? i + 1 : i;
                if (beginMoveRows(QModelIndex(), from, from, QModelIndex(),
                                  destination)) {
                    rows.move(from, i);
                    endMoveRows();
                }
                rebuildCurrent();
            }
            const bool same = unchanged(rows[i], next.at(i));
            rows[i] = std::move(next[i]);
            if (!same) emit dataChanged(index(i), index(i));
        }
        if (rows.size() > next.size()) {
            beginRemoveRows(QModelIndex(), next.size(), rows.size() - 1);
            rows.resize(next.size());
            endRemoveRows();
        }
    }
};
