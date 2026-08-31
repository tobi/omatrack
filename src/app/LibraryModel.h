// Flat list model backing the file-browser sidebar. Replaces the
// QVariantList tree from TelemetryStore::fileSources() and the JS
// flattening/filtering loops that FileBrowserPane.qml used to run.
//
// The store calls setTree() with the enriched QVariantList tree (built
// from the same scan data as the old fileSources() builder). The model
// converts it to an internal Node tree, flattens it into visible rows
// according to expansion state, and exposes one row per visible node.
// A LibraryFilterModel proxy drives text/track/driver/year/kind filtering.
// Facet lists (drivers, tracks, years) are exposed as Q_PROPERTYs so the
// pill bars can bind directly.

#pragma once

#include "FilterChange.h"
#include "ModelDiff.h"
#include <QHash>

#include <QSortFilterProxyModel>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QtQml/qqmlregistration.h>

class LibraryModel : public IdentityListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QStringList driverPills READ driverPills NOTIFY refreshed)
    Q_PROPERTY(QStringList trackPills READ trackPills NOTIFY refreshed)
    Q_PROPERTY(QStringList yearPills READ yearPills NOTIFY refreshed)
public:
    enum Role {
        KindRole = Qt::UserRole,  // source, folder, day, file, pins, recent
        KeyRole,                  // session key (file rows with sessions)
        PathRole,                 // file/folder path
        TitleRole,                // display name
        DetailRole,               // pre-computed second-line text
        BestLapTextRole,          // best lap time text
        StartTimeTextRole,        // session start clock
        LapCountRole,
        DriveTimeTextRole,
        IsVideoRole,
        IsPrimaryRole,
        IsReferenceRole,
        PinnedRole,
        SessionKeyRole,
        LapIdRole,
        FolderNameRole,
        // ── additional roles for the file-tree delegate ──
        DriverRole,
        CarClassRole,
        SeriesNameRole,
        SessionDateRole,
        SessionNameRole,
        MappingKeyRole,
        HasSessionRole,
        AvailableRole,
        ChildCountRole,
        IndentRole,
        ExpandedRole,
        ModifiedRole,
        TopQuartileTimeRole,
        SessionDayKeyRole,
        TrackRole,
        RowIdentityRole,  // section-scoped stable identity (ScrollAnchor)
    };
    Q_ENUM(Role)

    struct Node {
        QString kind;
        QString key;
        QString path;
        QString title;
        QString driver;
        QString bestLapText;
        QString topQuartileTime;
        QString driveTimeText;
        QString startTimeText;
        QString sessionDate;
        QString sessionName;
        QString sessionDayKey;
        QString track;
        QString mappingKey;
        QString modified;
        QString carClass;
        QString seriesName;
        int lapCount = 0;
        int childCount = 0;
        bool hasSession = false;
        bool isVideo = false;
        bool pinned = false;
        bool available = true;
        QVector<Node> children;
    };

    explicit LibraryModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QStringList driverPills() const { return driverPills_; }
    QStringList trackPills() const { return trackPills_; }
    QStringList yearPills() const { return yearPills_; }

    /// Replace the full tree from the store's enriched QVariantList and
    /// re-flatten. Called on sessionsChanged / filePinsChanged /
    /// recentFilesChanged / driverMappingsChanged.
    void setTree(const QVariantList& sources);

    /// Toggle expansion of a node and update the flat row list.
    Q_INVOKABLE void toggleNode(const QString& kind, const QString& path);

    /// Reveal the ancestors of `sessionKey` by expanding every folder
    /// on the path to it. Returns true if expansion state changed.
    Q_INVOKABLE bool revealSession(const QString& sessionKey);

    /// Update one file row from arriving sidebar metadata.
    void updateFileMetadata(const QString& path, const QVariantMap& details);

    /// Update primary/reference highlighting on all rows.
    void updateSelection(const QString& primaryKey,
                         const QString& referenceKey);
    QString primarySessionKey() const { return primaryKey_; }

    /// True when any facet filter or text search is active (all nodes
    /// expand while filtering).
    bool filteringActive() const { return filtering_; }
    void setFilteringActive(bool active);

    /// Collect all file paths in tree order (for AutotestHarness).
    QStringList filePaths() const;

signals:
    void refreshed();

private:
    struct FlatRow {
        const Node* node;
        int indent;
        bool expanded;
        bool isPrimary;
        bool isReference;
        QString identity;
    };

    static Node fromVariantMap(const QVariantMap& vm);
    QString rowIdentity(const Node& node) const {
        // A metadata arrival can fill/change the session key. The visible
        // file is still the same row; identity follows its stable path.
        return node.kind + QChar(0) +
               (node.path.isEmpty() ? node.title : node.path);
    }
    QVector<FlatRow> flattenTree(const QVector<Node>& roots) const;
    void applyFlat(QVector<FlatRow> next);
    void collectFacets();
    void expandAll(Node& node) const;
    bool nodeExpanded(const QString& kind, const QString& path) const;
    QString nodeKey(const QString& kind, const QString& path) const {
        return kind + QLatin1Char(':') + path;
    }
    void buildAncestorCache(const Node& node, const QStringList& ancestors);

    QVector<FlatRow> flat_;
    QVector<Node> rootNodes_;
    QHash<QString, bool> expanded_;
    bool filtering_ = false;
    QStringList driverPills_;
    QStringList trackPills_;
    QStringList yearPills_;
    QString primaryKey_;
    QString referenceKey_;
    QHash<QString, QStringList> sessionAncestors_;
};

// ── filter proxy ────────────────────────────────────────────────────

class LibraryFilterModel : public FilterChangeProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY
                   filterTextChanged)
    Q_PROPERTY(QStringList selectedDrivers READ selectedDrivers WRITE
                   setSelectedDrivers NOTIFY selectedDriversChanged)
    Q_PROPERTY(QStringList selectedYears READ selectedYears WRITE
                   setSelectedYears NOTIFY selectedYearsChanged)
    Q_PROPERTY(QString selectedTrack READ selectedTrack WRITE setSelectedTrack
                   NOTIFY selectedTrackChanged)
    Q_PROPERTY(QString selectedKind READ selectedKind WRITE setSelectedKind
                   NOTIFY selectedKindChanged)
    Q_PROPERTY(QString selectedDay READ selectedDay WRITE setSelectedDay NOTIFY
                   selectedDayChanged)
    Q_PROPERTY(
        bool eventFilterActive READ eventFilterActive NOTIFY eventFilterChanged)
    Q_PROPERTY(
        bool anyFilterActive READ anyFilterActive NOTIFY anyFilterActiveChanged)
public:
    explicit LibraryFilterModel(QObject* parent = nullptr);

    QString filterText() const { return filterText_; }
    void setFilterText(const QString& text);

    QStringList selectedDrivers() const { return selectedDrivers_; }
    void setSelectedDrivers(const QStringList& drivers);

    QStringList selectedYears() const { return selectedYears_; }
    void setSelectedYears(const QStringList& years);

    QString selectedTrack() const { return selectedTrack_; }
    void setSelectedTrack(const QString& track);

    QString selectedKind() const { return selectedKind_; }
    void setSelectedKind(const QString& kind);

    QString selectedDay() const { return selectedDay_; }
    void setSelectedDay(const QString& day);

    /// Event mode owns the track and day facets while it is on. Entering
    /// stashes the user's manual track/day; leaving restores them. Changing
    /// the event's track or day while on re-applies without touching the
    /// stash.
    Q_INVOKABLE void setEventFilter(bool enabled, const QString& track,
                                    const QString& day);
    bool eventFilterActive() const { return eventFilter_; }
    /// Text, facets, day or event filter.
    bool anyFilterActive() const;
    /// Clears every facet and the text. Also leaves the event filter, since
    /// the event filter is a facet preset, not a mode outside the filters.
    Q_INVOKABLE void clearAllFilters();

    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex& sourceParent) const override;

    void setSourceModel(QAbstractItemModel* model) override;
    Q_INVOKABLE void toggleNode(const QString& kind, const QString& path);
    Q_INVOKABLE bool revealSession(const QString& sessionKey);

signals:
    void filterTextChanged();
    void selectedDriversChanged();
    void selectedYearsChanged();
    void selectedTrackChanged();
    void selectedKindChanged();
    void selectedDayChanged();
    void eventFilterChanged();
    void anyFilterActiveChanged();

private:
    void syncFilteringActive();
    bool facetsActive() const;
    bool rowPassesFacets(int sourceRow) const;

    QString filterText_;
    QStringList selectedDrivers_;
    QStringList selectedYears_;
    QString selectedTrack_;
    QString selectedKind_;
    QString selectedDay_;
    bool eventFilter_ = false;
    QString manualTrack_;
    QString manualDay_;
    LibraryModel* source_ = nullptr;
};
