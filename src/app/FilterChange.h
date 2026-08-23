// QSortFilterProxyModel base with one sanctioned way to change the filter
// criteria. Qt 6.10 deprecated invalidateFilter() in favour of
// begin/endFilterChange(), which lets the proxy emit precise row
// insert/remove instead of a layout change. The declared minimum is Qt 6.5,
// so older builds fall back to invalidateFilter(). Both hooks are protected,
// which is why the bracket lives in a base class rather than a free helper.
#pragma once

#include <QSortFilterProxyModel>
#include <QtGlobal>

#include <utility>

class FilterChangeProxyModel : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    /// Run `mutate` (which updates the criteria that filterAcceptsRow reads)
    /// inside the proxy's filter-change bracket.
    template <typename Mutate>
    void changeFilter(Mutate&& mutate) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        std::forward<Mutate>(mutate)();
        endFilterChange();
#else
        std::forward<Mutate>(mutate)();
        invalidateFilter();
#endif
    }
};
