#include "LibraryModel.h"

#include <functional>
#include <QSet>
#include <QVariantMap>
#include <QVector>
#include <algorithm>

// ── LibraryModel ────────────────────────────────────────────────────

LibraryModel::LibraryModel(QObject* parent) : IdentityListModel(parent) {}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    return {
        {KindRole, "kind"},
        {KeyRole, "key"},
        {PathRole, "path"},
        {TitleRole, "title"},
        {DetailRole, "detail"},
        {BestLapTextRole, "bestLapText"},
        {StartTimeTextRole, "startTimeText"},
        {LapCountRole, "lapCount"},
        {DriveTimeTextRole, "driveTimeText"},
        {IsVideoRole, "isVideo"},
        {IsPrimaryRole, "isPrimary"},
        {IsReferenceRole, "isReference"},
        {PinnedRole, "pinned"},
        {SessionKeyRole, "sessionKey"},
        {LapIdRole, "lapId"},
        {FolderNameRole, "folderName"},
        {DriverRole, "driver"},
        {CarClassRole, "carClass"},
        {SeriesNameRole, "seriesName"},
        {SessionDateRole, "sessionDate"},
        {SessionNameRole, "sessionName"},
        {MappingKeyRole, "mappingKey"},
        {HasSessionRole, "hasSession"},
        {AvailableRole, "available"},
        {ChildCountRole, "childCount"},
        {IndentRole, "indent"},
        {ExpandedRole, "expanded"},
        {ModifiedRole, "modified"},
        {TopQuartileTimeRole, "topQuartileTime"},
        {SessionDayKeyRole, "sessionDayKey"},
        {TrackRole, "track"},
    };
}

int LibraryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : flat_.size();
}

QVariant LibraryModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const FlatRow& row = flat_[index.row()];
    const Node* n = row.node;
    if (!n) return {};
    switch (role) {
        case KindRole: return n->kind;
        case KeyRole: return n->key;
        case PathRole: return n->path;
        case TitleRole: return n->title;
        case DetailRole: {
            if (n->kind == QLatin1String("file"))
                return (n->startTimeText.isEmpty() ? QStringLiteral("—")
                                                   : n->startTimeText) +
                       QStringLiteral(" · ") + QString::number(n->lapCount) +
                       (n->lapCount == 1 ? QStringLiteral(" lap")
                                         : QStringLiteral(" laps")) +
                       QStringLiteral(" · ") +
                       (n->driveTimeText.isEmpty() ? QStringLiteral("—")
                                                   : n->driveTimeText);
            return {};
        }
        case BestLapTextRole: return n->bestLapText;
        case StartTimeTextRole: return n->startTimeText;
        case LapCountRole: return n->lapCount;
        case DriveTimeTextRole: return n->driveTimeText;
        case IsVideoRole: return n->isVideo;
        case IsPrimaryRole: return row.isPrimary;
        case IsReferenceRole: return row.isReference;
        case PinnedRole: return n->pinned;
        case SessionKeyRole: return n->key;
        case LapIdRole: return -1;
        case FolderNameRole: return n->title;
        case DriverRole: return n->driver;
        case CarClassRole: return n->carClass;
        case SeriesNameRole: return n->seriesName;
        case SessionDateRole: return n->sessionDate;
        case SessionNameRole: return n->sessionName;
        case MappingKeyRole: return n->mappingKey;
        case HasSessionRole: return n->hasSession;
        case AvailableRole: return n->available;
        case ChildCountRole: return n->childCount;
        case IndentRole: return row.indent;
        case ExpandedRole: return row.expanded;
        case ModifiedRole: return n->modified;
        case TopQuartileTimeRole: return n->topQuartileTime;
        case SessionDayKeyRole: return n->sessionDayKey;
        case TrackRole: return n->track;
    }
    return {};
}

LibraryModel::Node LibraryModel::fromVariantMap(const QVariantMap& vm) {
    Node node;
    node.kind = vm.value(QStringLiteral("role")).toString();
    node.key = vm.value(QStringLiteral("key")).toString();
    node.path = vm.value(QStringLiteral("path")).toString();
    node.title = vm.value(QStringLiteral("name")).toString();
    node.driver = vm.value(QStringLiteral("driver")).toString();
    node.bestLapText = vm.value(QStringLiteral("bestTime")).toString();
    node.topQuartileTime =
        vm.value(QStringLiteral("topQuartileTime")).toString();
    node.driveTimeText = vm.value(QStringLiteral("driveTime")).toString();
    node.startTimeText = vm.value(QStringLiteral("sessionStart")).toString();
    node.sessionDate = vm.value(QStringLiteral("sessionDate")).toString();
    node.sessionName = vm.value(QStringLiteral("sessionName")).toString();
    node.sessionDayKey = vm.value(QStringLiteral("sessionDayKey")).toString();
    node.track = vm.value(QStringLiteral("track")).toString();
    node.mappingKey = vm.value(QStringLiteral("mappingKey")).toString();
    node.modified = vm.value(QStringLiteral("modified")).toString();
    node.carClass = vm.value(QStringLiteral("carClass")).toString();
    node.seriesName = vm.value(QStringLiteral("seriesName")).toString();
    node.lapCount = vm.value(QStringLiteral("lapCount")).toInt();
    node.hasSession = vm.value(QStringLiteral("hasSession")).toBool();
    node.isVideo = vm.value(QStringLiteral("isVideo")).toBool();
    node.pinned = vm.value(QStringLiteral("pinned")).toBool();
    node.available = vm.value(QStringLiteral("available"), true).toBool();
    const QVariantList children = vm.value(QStringLiteral("children")).toList();
    node.children.reserve(children.size());
    for (const QVariant& child : children)
        node.children.append(fromVariantMap(child.toMap()));
    // childCount: for source/folder/pins/recent, the file count from the
    // scan; for day nodes, the number of file children; for files, 0.
    if (node.kind == QLatin1String("source") ||
        node.kind == QLatin1String("pins") ||
        node.kind == QLatin1String("recent"))
        node.childCount = vm.value(QStringLiteral("fileCount")).toInt();
    else if (node.kind == QLatin1String("folder"))
        node.childCount = static_cast<int>(node.children.size());
    else if (node.kind == QLatin1String("day"))
        node.childCount = static_cast<int>(node.children.size());
    else
        node.childCount = 0;
    return node;
}

void LibraryModel::setTree(const QVariantList& sources) {
    QVector<Node> nextRoots;
    nextRoots.reserve(sources.size());
    for (const QVariant& source : sources)
        nextRoots.append(fromVariantMap(source.toMap()));
    sessionAncestors_.clear();
    for (const Node& node : std::as_const(nextRoots)) {
        QStringList ancestors;
        buildAncestorCache(node, ancestors);
    }
    if (filtering_)
        for (Node& node : nextRoots) expandAll(node);
    QVector<FlatRow> nextFlat = flattenTree(nextRoots);
    applyFlat(std::move(nextFlat));
    rootNodes_ = std::move(nextRoots);
    collectFacets();
    emit refreshed();
}

void LibraryModel::buildAncestorCache(const Node& node,
                                      const QStringList& ancestors) {
    const QString kind = node.kind;
    if (kind.isEmpty()) return;  // guard for the clear call
    QStringList nextAncestors = ancestors;
    if (kind == QLatin1String("source") || kind == QLatin1String("folder") ||
        kind == QLatin1String("pins") || kind == QLatin1String("recent")) {
        nextAncestors.append(nodeKey(kind, node.path));
    }
    if (kind == QLatin1String("file") && !node.key.isEmpty() &&
        !nextAncestors.isEmpty() && !sessionAncestors_.contains(node.key))
        sessionAncestors_[node.key] = nextAncestors;
    for (const Node& child : std::as_const(node.children))
        buildAncestorCache(child, nextAncestors);
}

bool LibraryModel::nodeExpanded(const QString& kind,
                                const QString& path) const {
    if (filtering_) return true;
    if (kind == QLatin1String("day")) return true;
    const QString key = nodeKey(kind, path);
    auto it = expanded_.constFind(key);
    if (it != expanded_.cend()) return it.value();
    return kind == QLatin1String("source") || kind == QLatin1String("pins") ||
           kind == QLatin1String("recent");
}

void LibraryModel::expandAll(Node& node) const {
    for (Node& child : node.children) expandAll(child);
}

QVector<LibraryModel::FlatRow> LibraryModel::flattenTree(
    const QVector<Node>& roots) const {
    QVector<FlatRow> flat;
    for (const Node& node : roots) {
        const bool expanded = filtering_ || nodeExpanded(node.kind, node.path);
        flat.append({&node, 0, expanded,
                     node.kind == QLatin1String("file") &&
                         !node.key.isEmpty() && node.key == primaryKey_,
                     node.kind == QLatin1String("file") &&
                         !node.key.isEmpty() && node.key == referenceKey_});
        if (expanded) {
            // Recurse into children
            QVector<const Node*> stack;
            QVector<int> indentStack;
            for (int i = node.children.size() - 1; i >= 0; --i) {
                stack.append(&node.children[i]);
                indentStack.append(1);
            }
            while (!stack.isEmpty()) {
                const Node* current = stack.takeLast();
                int indent = indentStack.takeLast();
                const bool curExpanded =
                    filtering_ || nodeExpanded(current->kind, current->path);
                flat.append({current, indent, curExpanded,
                             current->kind == QLatin1String("file") &&
                                 !current->key.isEmpty() &&
                                 current->key == primaryKey_,
                             current->kind == QLatin1String("file") &&
                                 !current->key.isEmpty() &&
                                 current->key == referenceKey_});
                if (curExpanded) {
                    for (int i = current->children.size() - 1; i >= 0; --i) {
                        stack.append(&current->children[i]);
                        indentStack.append(indent + 1);
                    }
                }
            }
        }
    }
    return flat;
}

void LibraryModel::applyFlat(QVector<FlatRow> next) {
    replaceByIdentity(
        flat_, std::move(next),
        [this](const FlatRow& row) {
            return row.node ? rowIdentity(*row.node) : QString();
        },
        [](const FlatRow& left, const FlatRow& right) {
            if (!left.node || !right.node) return left.node == right.node;
            const Node& a = *left.node;
            const Node& b = *right.node;
            return left.indent == right.indent &&
                   left.expanded == right.expanded &&
                   left.isPrimary == right.isPrimary &&
                   left.isReference == right.isReference &&
                   a.title == b.title && a.bestLapText == b.bestLapText &&
                   a.driver == b.driver && a.sessionName == b.sessionName &&
                   a.track == b.track && a.lapCount == b.lapCount &&
                   a.childCount == b.childCount && a.available == b.available &&
                   a.pinned == b.pinned && a.isVideo == b.isVideo &&
                   a.hasSession == b.hasSession && a.key == b.key &&
                   a.startTimeText == b.startTimeText &&
                   a.driveTimeText == b.driveTimeText &&
                   a.sessionDayKey == b.sessionDayKey;
        });
}

void LibraryModel::collectFacets() {
    QSet<QString> drivers, years, tracks;
    for (const FlatRow& row : std::as_const(flat_)) {
        if (!row.node || row.node->kind != QLatin1String("file")) continue;
        const QString driver = row.node->driver.trimmed();
        if (!driver.isEmpty()) drivers.insert(driver);
        const QString day = row.node->sessionDayKey;
        if (day.length() >= 4 && day != QLatin1String("unknown"))
            years.insert(day.left(4));
        const QString track = row.node->track.trimmed();
        if (!track.isEmpty()) tracks.insert(track);
    }
    driverPills_ = QStringList(drivers.cbegin(), drivers.cend());
    driverPills_.sort();
    yearPills_ = QStringList(years.cbegin(), years.cend());
    yearPills_.sort();
    // Newest first
    std::reverse(yearPills_.begin(), yearPills_.end());
    trackPills_ = QStringList(tracks.cbegin(), tracks.cend());
    trackPills_.sort();
}

void LibraryModel::toggleNode(const QString& kind, const QString& path) {
    const QString key = nodeKey(kind, path);
    const bool currently = nodeExpanded(kind, path);
    expanded_[key] = !currently;
    applyFlat(flattenTree(rootNodes_));
}

bool LibraryModel::revealSession(const QString& sessionKey) {
    if (sessionKey.isEmpty() || filtering_) return false;
    const QStringList ancestors = sessionAncestors_.value(sessionKey);
    if (ancestors.isEmpty()) return false;
    bool changed = false;
    for (const QString& ancestor : ancestors) {
        if (!expanded_.value(ancestor, false)) {
            expanded_[ancestor] = true;
            changed = true;
        }
    }
    if (changed) applyFlat(flattenTree(rootNodes_));
    return changed;
}

void LibraryModel::updateFileMetadata(const QString& path,
                                      const QVariantMap& details) {
    // Update the node in the tree and the flat row.
    bool changed = false;
    for (int i = 0; i < flat_.size(); ++i) {
        const FlatRow& row = flat_[i];
        if (!row.node || row.node->kind != QLatin1String("file") ||
            row.node->path != path)
            continue;
        // Nodes are const pointers in FlatRow; we need to find and update
        // the actual Node in rootNodes_.
        changed = true;
        break;
    }
    if (!changed) return;
    // Walk rootNodes_ to find and update the matching node, then refresh
    // the flat rows in-place (no reset needed for data changes).
    std::function<bool(LibraryModel::Node&, const QString&, const QVariantMap&)>
        updateNode = [&](Node& node, const QString& targetPath,
                         const QVariantMap& d) -> bool {
        if (node.kind == QLatin1String("file") && node.path == targetPath) {
            node.bestLapText = d.value(QStringLiteral("bestTime")).toString();
            node.carClass = d.value(QStringLiteral("carClass")).toString();
            node.driveTimeText =
                d.value(QStringLiteral("driveTime")).toString();
            node.driver = d.value(QStringLiteral("driver")).toString();
            node.hasSession = d.value(QStringLiteral("hasSession")).toBool();
            node.isVideo = d.value(QStringLiteral("isVideo")).toBool();
            node.key = d.value(QStringLiteral("key")).toString();
            node.lapCount = d.value(QStringLiteral("lapCount")).toInt();
            node.mappingKey = d.value(QStringLiteral("mappingKey")).toString();
            node.seriesName = d.value(QStringLiteral("seriesName")).toString();
            node.sessionDate =
                d.value(QStringLiteral("sessionDate")).toString();
            node.sessionName =
                d.value(QStringLiteral("sessionName")).toString();
            node.startTimeText =
                d.value(QStringLiteral("sessionStart")).toString();
            node.topQuartileTime =
                d.value(QStringLiteral("topQuartileTime")).toString();
            node.track = d.value(QStringLiteral("track")).toString();
            return true;
        }
        for (Node& child : node.children)
            if (updateNode(child, targetPath, d)) return true;
        return false;
    };
    for (Node& node : rootNodes_) updateNode(node, path, details);
    // Emit dataChanged for the affected row(s).
    for (int i = 0; i < flat_.size(); ++i) {
        if (flat_[i].node && flat_[i].node->kind == QLatin1String("file") &&
            flat_[i].node->path == path) {
            emit dataChanged(index(i), index(i));
            break;
        }
    }
    // Facets may have changed.
    collectFacets();
    emit refreshed();
}

void LibraryModel::updateSelection(const QString& primaryKey,
                                   const QString& referenceKey) {
    primaryKey_ = primaryKey;
    referenceKey_ = referenceKey;
    for (int i = 0; i < flat_.size(); ++i) {
        const Node* n = flat_[i].node;
        if (!n || n->kind != QLatin1String("file")) continue;
        const bool isPrimary = !n->key.isEmpty() && n->key == primaryKey;
        const bool isReference = !n->key.isEmpty() && n->key == referenceKey;
        if (flat_[i].isPrimary != isPrimary ||
            flat_[i].isReference != isReference) {
            flat_[i].isPrimary = isPrimary;
            flat_[i].isReference = isReference;
            emit dataChanged(index(i), index(i));
        }
    }
}

void LibraryModel::setFilteringActive(bool active) {
    if (filtering_ == active) return;
    filtering_ = active;
    if (active)
        for (Node& node : rootNodes_) expandAll(node);
    applyFlat(flattenTree(rootNodes_));
}

QStringList LibraryModel::filePaths() const {
    QStringList paths;
    QSet<QString> seen;
    std::function<void(const QVector<Node>&)> collect =
        [&](const QVector<Node>& nodes) {
            for (const Node& node : nodes) {
                if (node.kind == QLatin1String("file") &&
                    !node.path.isEmpty() && !seen.contains(node.path)) {
                    seen.insert(node.path);
                    paths.append(node.path);
                }
                collect(node.children);
            }
        };
    collect(rootNodes_);
    return paths;
}

// ── LibraryFilterModel ──────────────────────────────────────────────

LibraryFilterModel::LibraryFilterModel(QObject* parent)
    : FilterChangeProxyModel(parent) {
    setRecursiveFilteringEnabled(true);
}

void LibraryFilterModel::syncFilteringActive() {
    if (!source_) return;
    const bool active = !filterText_.isEmpty() || facetsActive();
    source_->setFilteringActive(active);
}

void LibraryFilterModel::setSourceModel(QAbstractItemModel* model) {
    source_ = qobject_cast<LibraryModel*>(model);
    QSortFilterProxyModel::setSourceModel(model);
}

void LibraryFilterModel::toggleNode(const QString& kind, const QString& path) {
    if (source_) source_->toggleNode(kind, path);
}

bool LibraryFilterModel::revealSession(const QString& sessionKey) {
    return source_ ? source_->revealSession(sessionKey) : false;
}

void LibraryFilterModel::setFilterText(const QString& text) {
    if (filterText_ == text) return;
    changeFilter([&] {
        filterText_ = text;
        emit filterTextChanged();
        syncFilteringActive();
    });
}

void LibraryFilterModel::setSelectedDrivers(const QStringList& drivers) {
    if (selectedDrivers_ == drivers) return;
    changeFilter([&] {
        selectedDrivers_ = drivers;
        emit selectedDriversChanged();
        syncFilteringActive();
    });
}

void LibraryFilterModel::setSelectedYears(const QStringList& years) {
    if (selectedYears_ == years) return;
    changeFilter([&] {
        selectedYears_ = years;
        emit selectedYearsChanged();
        syncFilteringActive();
    });
}

void LibraryFilterModel::setSelectedTrack(const QString& track) {
    if (selectedTrack_ == track) return;
    changeFilter([&] {
        selectedTrack_ = track;
        emit selectedTrackChanged();
        syncFilteringActive();
    });
}

void LibraryFilterModel::setSelectedKind(const QString& kind) {
    if (selectedKind_ == kind) return;
    changeFilter([&] {
        selectedKind_ = kind;
        emit selectedKindChanged();
        syncFilteringActive();
    });
}

void LibraryFilterModel::setSelectedDay(const QString& day) {
    if (selectedDay_ == day) return;
    changeFilter([&] {
        selectedDay_ = day;
        emit selectedDayChanged();
        syncFilteringActive();
    });
}

bool LibraryFilterModel::facetsActive() const {
    return !selectedDrivers_.isEmpty() || !selectedYears_.isEmpty() ||
           !selectedTrack_.isEmpty() || !selectedKind_.isEmpty() ||
           !selectedDay_.isEmpty();
}

bool LibraryFilterModel::rowPassesFacets(int sourceRow) const {
    if (!source_) return true;
    const QModelIndex idx = source_->index(sourceRow);
    if (selectedDrivers_.isEmpty() && selectedYears_.isEmpty() &&
        selectedTrack_.isEmpty() && selectedKind_.isEmpty() &&
        selectedDay_.isEmpty())
        return true;
    const QString kind = idx.data(LibraryModel::KindRole).toString();
    // Only filter file rows; non-file rows pass (they are kept by
    // recursive filtering if any child passes).
    if (kind != QLatin1String("file")) return true;
    if (!selectedDrivers_.isEmpty()) {
        const QString driver =
            idx.data(LibraryModel::DriverRole).toString().trimmed();
        if (!selectedDrivers_.contains(driver)) return false;
    }
    if (!selectedYears_.isEmpty()) {
        const QString day =
            idx.data(LibraryModel::SessionDayKeyRole).toString();
        const QString year =
            day.length() >= 4 && day != QLatin1String("unknown") ? day.left(4)
                                                                 : QString();
        if (!selectedYears_.contains(year)) return false;
    }
    if (!selectedTrack_.isEmpty()) {
        const QString track =
            idx.data(LibraryModel::TrackRole).toString().trimmed();
        if (track != selectedTrack_) return false;
    }
    if (!selectedKind_.isEmpty()) {
        const bool isVideo = idx.data(LibraryModel::IsVideoRole).toBool();
        if (selectedKind_ == QLatin1String("video") && !isVideo) return false;
        if (selectedKind_ == QLatin1String("telemetry") && isVideo)
            return false;
    }
    if (!selectedDay_.isEmpty()) {
        const QString day =
            idx.data(LibraryModel::SessionDayKeyRole).toString();
        if (day != selectedDay_) return false;
    }
    return true;
}

bool LibraryFilterModel::filterAcceptsRow(
    int sourceRow, const QModelIndex& sourceParent) const {
    if (sourceParent.isValid()) return true;
    if (!source_) return true;
    const QModelIndex idx = source_->index(sourceRow);
    const QString kind = idx.data(LibraryModel::KindRole).toString();

    // When no filter is active, show everything.
    if (filterText_.isEmpty() && !facetsActive()) return true;

    // When filtering, only show matching file rows.
    if (kind != QLatin1String("file")) return false;

    // Facet filters.
    if (!selectedDrivers_.isEmpty()) {
        const QString driver =
            idx.data(LibraryModel::DriverRole).toString().trimmed();
        if (!selectedDrivers_.contains(driver)) return false;
    }
    if (!selectedYears_.isEmpty()) {
        const QString day =
            idx.data(LibraryModel::SessionDayKeyRole).toString();
        const QString year =
            day.length() >= 4 && day != QLatin1String("unknown") ? day.left(4)
                                                                 : QString();
        if (!selectedYears_.contains(year)) return false;
    }
    if (!selectedTrack_.isEmpty()) {
        const QString track =
            idx.data(LibraryModel::TrackRole).toString().trimmed();
        if (track != selectedTrack_) return false;
    }
    if (!selectedKind_.isEmpty()) {
        const bool isVideo = idx.data(LibraryModel::IsVideoRole).toBool();
        if (selectedKind_ == QLatin1String("video") && !isVideo) return false;
        if (selectedKind_ == QLatin1String("telemetry") && isVideo)
            return false;
    }
    if (!selectedDay_.isEmpty()) {
        const QString day =
            idx.data(LibraryModel::SessionDayKeyRole).toString();
        if (day != selectedDay_) return false;
    }

    // Text search: case-insensitive substring across multiple fields.
    if (!filterText_.isEmpty()) {
        const QStringList fields = {
            idx.data(LibraryModel::TitleRole).toString(),
            idx.data(LibraryModel::PathRole).toString(),
            idx.data(LibraryModel::DriverRole).toString(),
            idx.data(LibraryModel::SessionNameRole).toString(),
            idx.data(LibraryModel::StartTimeTextRole).toString(),
            idx.data(LibraryModel::BestLapTextRole).toString(),
            idx.data(LibraryModel::CarClassRole).toString(),
            idx.data(LibraryModel::SeriesNameRole).toString(),
            idx.data(LibraryModel::SessionDateRole).toString(),
            idx.data(LibraryModel::TrackRole).toString(),
        };
        const QString searchable = fields.join(QLatin1Char(' ')).toLower();
        if (!searchable.contains(filterText_.toLower())) return false;
    }
    return true;
}
