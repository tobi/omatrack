#include "StoreModels.h"

#include <QString>
#include <algorithm>

int UsbCopyListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}
QVariant UsbCopyListModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const auto& row = rows_[index.row()];
    switch (role) {
        case SourcePathRole: return row.sourcePath;
        case TargetPathRole: return row.targetPath;
        case StatusTextRole: return row.statusText;
        case SizeTextRole: return row.sizeText;
        case ReadyRole: return row.ready;
    }
    return {};
}
QHash<int, QByteArray> UsbCopyListModel::roleNames() const {
    return {{SourcePathRole, "sourcePath"},
            {TargetPathRole, "targetPath"},
            {StatusTextRole, "statusText"},
            {SizeTextRole, "sizeText"},
            {ReadyRole, "ready"}};
}
void UsbCopyListModel::refresh(const QVector<UsbCopyRow>& rows) {
    replaceByIdentity(
        rows_, rows, [](const UsbCopyRow& row) { return row.sourcePath; },
        [](const UsbCopyRow& a, const UsbCopyRow& b) {
            return a.sourcePath == b.sourcePath &&
                   a.targetPath == b.targetPath &&
                   a.statusText == b.statusText && a.sizeText == b.sizeText &&
                   a.ready == b.ready;
        });
    emit refreshed();
}

// ── LapListModel ────────────────────────────────────────────────────

LapListModel::LapListModel(QObject* parent) : IdentityListModel(parent) {}

int LapListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant LapListModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const LapRow& row = rows_[index.row()];
    switch (role) {
        case LapIdRole: return row.lapId;
        case LabelRole: return row.label;
        case TimeTextRole: return row.timeText;
        case TimeMsRole: return row.timeMs;
        case StartTimeRole: return row.startTime;
        case IsFastestRole: return row.isFastest;
        case IsCompleteRole: return row.isComplete;
        case IsPitLapRole: return row.isPitLap;
        case CountsForBestRole: return row.countsForBest;
        case HoverTextRole: return row.hoverText;
    }
    return {};
}

QHash<int, QByteArray> LapListModel::roleNames() const {
    return {
        {LapIdRole, "lapId"},
        {LabelRole, "label"},
        {TimeTextRole, "timeText"},
        {TimeMsRole, "timeMs"},
        {StartTimeRole, "startTime"},
        {IsFastestRole, "isFastest"},
        {IsCompleteRole, "isComplete"},
        {IsPitLapRole, "isPitLap"},
        {CountsForBestRole, "countsForBest"},
        {HoverTextRole, "hoverText"},
    };
}

void LapListModel::refresh(const QVector<LapRow>& rows) {
    replaceByIdentity(
        rows_, rows,
        [](const LapRow& row) { return QString::number(row.lapId); },
        [](const LapRow& a, const LapRow& b) {
            return a.lapId == b.lapId && a.label == b.label &&
                   a.timeText == b.timeText && a.timeMs == b.timeMs &&
                   a.startTime == b.startTime && a.isFastest == b.isFastest &&
                   a.isComplete == b.isComplete && a.isPitLap == b.isPitLap &&
                   a.countsForBest == b.countsForBest &&
                   a.hoverText == b.hoverText;
        });
    fixedLapCount_ = 0;
    flexibleTimeMs_ = 0;
    totalTimeMs_ = 0;
    for (const LapRow& r : rows_) {
        totalTimeMs_ += std::max(1, r.timeMs);
        if (!r.countsForBest)
            ++fixedLapCount_;
        else
            flexibleTimeMs_ += std::max(1, r.timeMs);
    }
    flexibleTimeMs_ = std::max(1, flexibleTimeMs_);
    totalTimeMs_ = std::max(1, totalTimeMs_);
    emit refreshed();
}

// ── FilmstripSessionListModel ───────────────────────────────────────

FilmstripSessionListModel::FilmstripSessionListModel(QObject* parent)
    : IdentityListModel(parent) {}

int FilmstripSessionListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant FilmstripSessionListModel::data(const QModelIndex& index,
                                         int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const FilmstripSessionRow& row = rows_[index.row()];
    switch (role) {
        case SessionKeyRole: return row.sessionKey;
        case DriverNameRole: return row.driverName;
        case BestTimeRole: return row.bestTime;
        case ReferenceRole: return row.reference;
    }
    return {};
}

QHash<int, QByteArray> FilmstripSessionListModel::roleNames() const {
    return {
        {SessionKeyRole, "sessionKey"},
        {DriverNameRole, "driverName"},
        {BestTimeRole, "bestTime"},
        {ReferenceRole, "reference"},
    };
}

void FilmstripSessionListModel::refresh(
    const QVector<FilmstripSessionRow>& rows) {
    replaceByIdentity(
        rows_, rows,
        [](const FilmstripSessionRow& row) {
            return row.reference ? QStringLiteral("reference")
                                 : QStringLiteral("primary");
        },
        [](const FilmstripSessionRow& a, const FilmstripSessionRow& b) {
            return a.sessionKey == b.sessionKey &&
                   a.driverName == b.driverName && a.bestTime == b.bestTime &&
                   a.reference == b.reference;
        });
    emit refreshed();
}

QString FilmstripSessionListModel::primarySessionKey() const {
    for (const FilmstripSessionRow& row : rows_)
        if (!row.reference) return row.sessionKey;
    return {};
}

// ── ChannelListModel ────────────────────────────────────────────────

ChannelListModel::ChannelListModel(QObject* parent)
    : IdentityListModel(parent) {}

int ChannelListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant ChannelListModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const ChannelRow& row = rows_[index.row()];
    switch (role) {
        case KeyRole: return row.key;
        case TitleRole: return row.title;
        case UnitRole: return row.unit;
        case VisibleRole: return row.visible;
        case ColorRole: return row.color;
        case WeightRole: return row.weight;
        case SourceRole: return row.source;
        case SidecarRole: return row.sidecar;
        case SpanRole: return row.span;
    }
    return {};
}

QHash<int, QByteArray> ChannelListModel::roleNames() const {
    return {
        {KeyRole, "key"},
        {TitleRole, "title"},
        {UnitRole, "unit"},
        {VisibleRole, "channelVisible"},
        {ColorRole, "channelColor"},
        {WeightRole, "weight"},
        {SourceRole, "source"},
        {SidecarRole, "sidecar"},
        {SpanRole, "span"},
    };
}

void ChannelListModel::refresh(const QVector<ChannelRow>& rows) {
    replaceByIdentity(
        rows_, rows, [](const ChannelRow& row) { return row.key; },
        [](const ChannelRow& a, const ChannelRow& b) {
            return a.key == b.key && a.title == b.title && a.unit == b.unit &&
                   a.visible == b.visible && a.color == b.color &&
                   a.weight == b.weight && a.source == b.source &&
                   a.sidecar == b.sidecar && a.span == b.span;
        });
}

// ── CornerListModel ─────────────────────────────────────────────────

CornerListModel::CornerListModel(QObject* parent) : IdentityListModel(parent) {}

int CornerListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant CornerListModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const CornerRow& row = rows_[index.row()];
    switch (role) {
        case NameRole: return row.name;
        case StartRole: return row.start;
        case EndRole: return row.end;
        case EntrySpeedRole: return row.entrySpeed;
        case ApexSpeedRole: return row.apexSpeed;
        case ExitSpeedRole: return row.exitSpeed;
        case SpeedDropRole: return row.speedDrop;
        case SpeedGainRole: return row.speedGain;
        case TimeRole: return row.time;
        case MinGearRole: return row.minGear;
        case MaxSteeringRole: return row.maxSteering;
        case MaxBrakeRole: return row.maxBrake;
        case MinThrottleRole: return row.minThrottle;
        case BrakePointRole: return row.brakePoint;
        case LiftPointRole: return row.liftPoint;
        case TurnInPositionRole: return row.turnInPosition;
        case ApexPositionRole: return row.apexPosition;
        case ThrottlePositionRole: return row.throttlePosition;
        case TurnInPointRole: return row.turnInPoint;
        case ApexPointRole: return row.apexPoint;
        case ThrottlePointRole: return row.throttlePoint;
        case ApexFractionRole: return row.apexFraction;
        case CornerStartPositionRole: return row.cornerStartPosition;
        case CornerEndPositionRole: return row.cornerEndPosition;
        case ContextWindowMetersRole: return row.contextWindowMeters;
        case CornerLengthMetersRole: return row.cornerLengthMeters;
        case HasCompareRole: return row.hasCompare;
        case CompareApexFractionRole: return row.compareApexFraction;
        case CompareEntrySpeedRole: return row.compareEntrySpeed;
        case CompareApexSpeedRole: return row.compareApexSpeed;
        case CompareExitSpeedRole: return row.compareExitSpeed;
        case CompareTimeRole: return row.compareTime;
        case CompareMinGearRole: return row.compareMinGear;
        case CompareMaxSteeringRole: return row.compareMaxSteering;
        case CompareMaxBrakeRole: return row.compareMaxBrake;
        case CompareMinThrottleRole: return row.compareMinThrottle;
        case CompareBrakePointRole: return row.compareBrakePoint;
        case CompareLiftPointRole: return row.compareLiftPoint;
        case CompareTurnInPositionRole: return row.compareTurnInPosition;
        case CompareApexPositionRole: return row.compareApexPosition;
        case CompareThrottlePositionRole: return row.compareThrottlePosition;
        case CompareTurnInPointRole: return row.compareTurnInPoint;
        case CompareApexPointRole: return row.compareApexPoint;
        case CompareThrottlePointRole: return row.compareThrottlePoint;
        case DeltaRole: return row.delta;
        case EntryTimeDeltaRole: return row.entryTimeDelta;
        case ExitTimeDeltaRole: return row.exitTimeDelta;
        case EntryDeltaRole: return row.entryDelta;
        case ApexDeltaRole: return row.apexDelta;
        case ExitDeltaRole: return row.exitDelta;
        case BrakePointDeltaRole: return row.brakePointDelta;
        case LiftPointDeltaRole: return row.liftPointDelta;
        case TurnInDeltaRole: return row.turnInDelta;
        case ApexPointDeltaRole: return row.apexPointDelta;
        case ThrottlePointDeltaRole: return row.throttlePointDelta;
        case ScoreRole: return row.score;
        case NotesRole: return row.notes;
        case NoteRole: return row.note;
    }
    return {};
}

QHash<int, QByteArray> CornerListModel::roleNames() const {
    return {
        {NameRole, "name"},
        {StartRole, "start"},
        {EndRole, "end"},
        {EntrySpeedRole, "entrySpeed"},
        {ApexSpeedRole, "apexSpeed"},
        {ExitSpeedRole, "exitSpeed"},
        {SpeedDropRole, "speedDrop"},
        {SpeedGainRole, "speedGain"},
        {TimeRole, "time"},
        {MinGearRole, "minGear"},
        {MaxSteeringRole, "maxSteering"},
        {MaxBrakeRole, "maxBrake"},
        {MinThrottleRole, "minThrottle"},
        {BrakePointRole, "brakePoint"},
        {LiftPointRole, "liftPoint"},
        {TurnInPositionRole, "turnInPosition"},
        {ApexPositionRole, "apexPosition"},
        {ThrottlePositionRole, "throttlePosition"},
        {TurnInPointRole, "turnInPoint"},
        {ApexPointRole, "apexPoint"},
        {ThrottlePointRole, "throttlePoint"},
        {ApexFractionRole, "apexFraction"},
        {CornerStartPositionRole, "cornerStartPosition"},
        {CornerEndPositionRole, "cornerEndPosition"},
        {ContextWindowMetersRole, "contextWindowMeters"},
        {CornerLengthMetersRole, "cornerLengthMeters"},
        {HasCompareRole, "hasCompare"},
        {CompareApexFractionRole, "compareApexFraction"},
        {CompareEntrySpeedRole, "compareEntrySpeed"},
        {CompareApexSpeedRole, "compareApexSpeed"},
        {CompareExitSpeedRole, "compareExitSpeed"},
        {CompareTimeRole, "compareTime"},
        {CompareMinGearRole, "compareMinGear"},
        {CompareMaxSteeringRole, "compareMaxSteering"},
        {CompareMaxBrakeRole, "compareMaxBrake"},
        {CompareMinThrottleRole, "compareMinThrottle"},
        {CompareBrakePointRole, "compareBrakePoint"},
        {CompareLiftPointRole, "compareLiftPoint"},
        {CompareTurnInPositionRole, "compareTurnInPosition"},
        {CompareApexPositionRole, "compareApexPosition"},
        {CompareThrottlePositionRole, "compareThrottlePosition"},
        {CompareTurnInPointRole, "compareTurnInPoint"},
        {CompareApexPointRole, "compareApexPoint"},
        {CompareThrottlePointRole, "compareThrottlePoint"},
        {DeltaRole, "delta"},
        {EntryTimeDeltaRole, "entryTimeDelta"},
        {ExitTimeDeltaRole, "exitTimeDelta"},
        {EntryDeltaRole, "entryDelta"},
        {ApexDeltaRole, "apexDelta"},
        {ExitDeltaRole, "exitDelta"},
        {BrakePointDeltaRole, "brakePointDelta"},
        {LiftPointDeltaRole, "liftPointDelta"},
        {TurnInDeltaRole, "turnInDelta"},
        {ApexPointDeltaRole, "apexPointDelta"},
        {ThrottlePointDeltaRole, "throttlePointDelta"},
        {ScoreRole, "score"},
        {NotesRole, "notes"},
        {NoteRole, "note"},
    };
}

void CornerListModel::refresh(const QVector<CornerRow>& rows) {
    replaceByIdentity(
        rows_, rows,
        [](const CornerRow& row) {
            return row.name + QChar(0) + QString::number(row.start, 'g', 12);
        },
        [](const CornerRow& a, const CornerRow& b) {
            return a.name == b.name && a.start == b.start && a.end == b.end &&
                   a.entrySpeed == b.entrySpeed && a.apexSpeed == b.apexSpeed &&
                   a.exitSpeed == b.exitSpeed && a.time == b.time &&
                   a.hasCompare == b.hasCompare && a.delta == b.delta &&
                   a.note == b.note && a.score == b.score;
        });
}

// ── DriverMappingModel ──────────────────────────────────────────────

DriverMappingModel::DriverMappingModel(QObject* parent)
    : IdentityListModel(parent) {}

int DriverMappingModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant DriverMappingModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const DriverMappingRow& row = rows_[index.row()];
    switch (role) {
        case KeyRole: return row.key;
        case CarNumberRole: return row.carNumber;
        case CarClassRole: return row.carClass;
        case DriverIdRole: return row.driverId;
        case DisplayRole: return row.display;
    }
    return {};
}

QHash<int, QByteArray> DriverMappingModel::roleNames() const {
    return {
        {KeyRole, "key"},           {CarNumberRole, "carNumber"},
        {CarClassRole, "carClass"}, {DriverIdRole, "driverId"},
        {DisplayRole, "display"},
    };
}

void DriverMappingModel::refresh(const QVector<DriverMappingRow>& rows) {
    replaceByIdentity(
        rows_, rows, [](const DriverMappingRow& row) { return row.key; },
        [](const DriverMappingRow& a, const DriverMappingRow& b) {
            return a.key == b.key && a.carNumber == b.carNumber &&
                   a.carClass == b.carClass && a.driverId == b.driverId &&
                   a.display == b.display;
        });
}

// ── SyncStrategyModel ───────────────────────────────────────────────

SyncStrategyModel::SyncStrategyModel(QObject* parent)
    : IdentityListModel(parent) {}

int SyncStrategyModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant SyncStrategyModel::data(const QModelIndex& index, int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const SyncStrategyRow& row = rows_[index.row()];
    switch (role) {
        case IdRole: return row.id;
        case LabelRole: return row.label;
        case ShortLabelRole: return row.shortLabel;
        case DetailRole: return row.detail;
    }
    return {};
}

QHash<int, QByteArray> SyncStrategyModel::roleNames() const {
    return {
        {IdRole, "strategyId"},
        {LabelRole, "label"},
        {ShortLabelRole, "shortLabel"},
        {DetailRole, "detail"},
    };
}

void SyncStrategyModel::refresh(const QVector<SyncStrategyRow>& rows) {
    replaceByIdentity(
        rows_, rows, [](const SyncStrategyRow& row) { return row.id; },
        [](const SyncStrategyRow& a, const SyncStrategyRow& b) {
            return a.id == b.id && a.label == b.label &&
                   a.shortLabel == b.shortLabel && a.detail == b.detail;
        });
}

// ── RowFilterModel ──────────────────────────────────────────────────

RowFilterModel::RowFilterModel(QObject* parent)
    : FilterChangeProxyModel(parent) {}

void RowFilterModel::setFilterText(const QString& text) {
    if (filterText_ == text) return;
    changeFilter([&] { filterText_ = text; });
    emit filterTextChanged();
}

bool RowFilterModel::filterAcceptsRow(int sourceRow,
                                      const QModelIndex& sourceParent) const {
    if (filterText_.isEmpty()) return true;
    const QString query = filterText_.trimmed().toLower();
    if (query.isEmpty()) return true;
    const QAbstractItemModel* m = sourceModel();
    if (!m) return true;
    const QHash<int, QByteArray> names = m->roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        const QVariant value =
            m->data(m->index(sourceRow, 0, sourceParent), it.key());
        if (value.typeId() == QMetaType::QString) {
            if (value.toString().toLower().contains(query)) return true;
        }
    }
    return false;
}
