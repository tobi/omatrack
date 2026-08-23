// QAbstractListModel subclasses backing the Store Q_PROPERTYs that replace
// QVariantList/QVariantMap builders. Each model stores a QVector of the
// corresponding StoreTypes.h gadget and exposes its fields as roles. The
// Store owns the instances and calls refresh() on the appropriate signal.
//
// RowFilterModel is a QML_ELEMENT QSortFilterProxyModel that does
// case-insensitive substring matching across all string roles — the
// replacement for JS-array filtering loops in ChannelsWindow and
// PreferencesDriversPage.

#pragma once

#include "FilterChange.h"
#include "StoreTypes.h"

#include <QAbstractListModel>
#include <QSortFilterProxyModel>
#include <QVector>
#include <QtQml/qqmlregistration.h>

// ── lap list model ──────────────────────────────────────────────────

class LapListModel : public QAbstractListModel {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int fixedLapCount READ fixedLapCount NOTIFY refreshed)
    Q_PROPERTY(int flexibleTimeMs READ flexibleTimeMs NOTIFY refreshed)
public:
    enum Role {
        LapIdRole = Qt::UserRole,
        LabelRole,
        TimeTextRole,
        TimeMsRole,
        StartTimeRole,
        IsFastestRole,
        IsCompleteRole,
        IsPitLapRole,
        CountsForBestRole,
    };
    Q_ENUM(Role)

    explicit LapListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void refresh(const QVector<LapRow>& rows);
    int fixedLapCount() const { return fixedLapCount_; }
    int flexibleTimeMs() const { return flexibleTimeMs_; }

signals:
    void refreshed();

private:
    QVector<LapRow> rows_;
    int fixedLapCount_ = 0;
    int flexibleTimeMs_ = 0;
};

// ── channel list model ──────────────────────────────────────────────

class ChannelListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        KeyRole = Qt::UserRole,
        TitleRole,
        UnitRole,
        VisibleRole,
        ColorRole,
        WeightRole,
        SourceRole,
        SidecarRole,
        SpanRole,
    };
    Q_ENUM(Role)

    explicit ChannelListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void refresh(const QVector<ChannelRow>& rows);

private:
    QVector<ChannelRow> rows_;
};

// ── corner list model (basic ranges + comparison columns) ───────────

class CornerListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        NameRole = Qt::UserRole,
        StartRole,
        EndRole,
        EntrySpeedRole,
        ApexSpeedRole,
        ExitSpeedRole,
        SpeedDropRole,
        SpeedGainRole,
        TimeRole,
        MinGearRole,
        MaxSteeringRole,
        MaxBrakeRole,
        MinThrottleRole,
        BrakePointRole,
        LiftPointRole,
        TurnInPositionRole,
        ApexPositionRole,
        ThrottlePositionRole,
        TurnInPointRole,
        ApexPointRole,
        ThrottlePointRole,
        ApexFractionRole,
        CornerStartPositionRole,
        CornerEndPositionRole,
        ContextWindowMetersRole,
        CornerLengthMetersRole,
        HasCompareRole,
        CompareApexFractionRole,
        CompareEntrySpeedRole,
        CompareApexSpeedRole,
        CompareExitSpeedRole,
        CompareTimeRole,
        CompareMinGearRole,
        CompareMaxSteeringRole,
        CompareMaxBrakeRole,
        CompareMinThrottleRole,
        CompareBrakePointRole,
        CompareLiftPointRole,
        CompareTurnInPositionRole,
        CompareApexPositionRole,
        CompareThrottlePositionRole,
        CompareTurnInPointRole,
        CompareApexPointRole,
        CompareThrottlePointRole,
        DeltaRole,
        EntryTimeDeltaRole,
        ExitTimeDeltaRole,
        EntryDeltaRole,
        ApexDeltaRole,
        ExitDeltaRole,
        BrakePointDeltaRole,
        LiftPointDeltaRole,
        TurnInDeltaRole,
        ApexPointDeltaRole,
        ThrottlePointDeltaRole,
        ScoreRole,
        NotesRole,
        NoteRole,
    };
    Q_ENUM(Role)

    explicit CornerListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void refresh(const QVector<CornerRow>& rows);

private:
    QVector<CornerRow> rows_;
};

// ── driver mapping model ────────────────────────────────────────────

class DriverMappingModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        KeyRole = Qt::UserRole,
        CarNumberRole,
        CarClassRole,
        DriverIdRole,
        DisplayRole,
    };
    Q_ENUM(Role)

    explicit DriverMappingModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Same row count → dataChanged (preserves editor focus);
    // different count → reset.
    void refresh(const QVector<DriverMappingRow>& rows);

private:
    QVector<DriverMappingRow> rows_;
};

// ── sync strategy model ─────────────────────────────────────────────

class SyncStrategyModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        IdRole = Qt::UserRole,
        LabelRole,
        ShortLabelRole,
        DetailRole,
    };
    Q_ENUM(Role)

    explicit SyncStrategyModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void refresh(const QVector<SyncStrategyRow>& rows);

private:
    QVector<SyncStrategyRow> rows_;
};

// ── row filter proxy (QML_ELEMENT) ──────────────────────────────────
// Case-insensitive substring match across every string-valued role of
// the source model. QML instantiates this, binds sourceModel, and sets
// filterText to drive filtering without JS array loops.

class RowFilterModel : public FilterChangeProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY
                   filterTextChanged)
public:
    explicit RowFilterModel(QObject* parent = nullptr);

    QString filterText() const { return filterText_; }
    void setFilterText(const QString& text);

    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex& sourceParent) const override;

signals:
    void filterTextChanged();

private:
    QString filterText_;
};
