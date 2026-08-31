// Identity-preserving list updates for QAbstractListModel.
//
// Existing rows keep their delegates/persistent indexes. Keys should identify
// logical rows, but duplicate keys must never reuse a row already placed in
// the output prefix (or move beyond the current list's end).
#pragma once

#include <QAbstractListModel>
#include <QSet>
#include <QString>
#include <QVector>
#include <utility>

class IdentityListModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

protected:
    template <typename T, typename IdentityFn, typename UnchangedFn>
    void replaceByIdentity(QVector<T>& rows, QVector<T> next,
                           IdentityFn identity, UnchangedFn unchanged) {
        QSet<QString> nextKeys;
        for (const auto& row : next) nextKeys.insert(identity(row));
        for (int i = rows.size() - 1; i >= 0; --i) {
            if (nextKeys.contains(identity(rows.at(i)))) continue;
            beginRemoveRows(QModelIndex(), i, i);
            rows.removeAt(i);
            endRemoveRows();
        }

        for (int i = 0; i < next.size(); ++i) {
            const QString id = identity(next.at(i));
            int from = i;
            // [0, i) is settled. Search only unmatched rows: a second
            // occurrence of a key needs its own row, not a move from there.
            while (from < rows.size() && identity(rows.at(from)) != id) ++from;
            if (from == rows.size()) {
                beginInsertRows(QModelIndex(), i, i);
                rows.insert(i, std::move(next[i]));
                endInsertRows();
                continue;
            }
            if (from != i) {
                // Only upward moves are needed; both endpoints are existing
                // rows. Qt's destination is the index before removal.
                if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), i))
                    return;
                rows.move(from, i);
                endMoveRows();
            }
            const bool same = unchanged(rows.at(i), next.at(i));
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
