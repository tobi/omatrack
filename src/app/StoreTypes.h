// Q_GADGET value types crossing the TelemetryStore → QML boundary.
//
// Replacing QVariantList/QVariantMap builders with typed models requires
// typed row storage. Each gadget here is registered with the QML module so
// qmllint and qmlcachegen can resolve property access on rows returned from
// Q_INVOKABLE (CursorReadout, CornerFocusSummary) or exposed through model
// roles (LapRow, ChannelRow, CornerRow, DriverMappingRow, SyncStrategyRow).

#pragma once

#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class UsbCopyRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString sourcePath MEMBER sourcePath)
    Q_PROPERTY(QString targetPath MEMBER targetPath)
    Q_PROPERTY(QString statusText MEMBER statusText)
    Q_PROPERTY(QString sizeText MEMBER sizeText)
    Q_PROPERTY(bool ready MEMBER ready)
public:
    QString sourcePath;
    QString targetPath;
    QString statusText;
    QString sizeText;
    bool ready = false;
};

// ── lap row ─────────────────────────────────────────────────────────

class LapRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(int lapId MEMBER lapId)
    Q_PROPERTY(QString label MEMBER label)
    Q_PROPERTY(QString timeText MEMBER timeText)
    Q_PROPERTY(int timeMs MEMBER timeMs)
    Q_PROPERTY(double startTime MEMBER startTime)
    Q_PROPERTY(bool isFastest MEMBER isFastest)
    Q_PROPERTY(bool isComplete MEMBER isComplete)
    Q_PROPERTY(bool isPitLap MEMBER isPitLap)
    Q_PROPERTY(bool countsForBest MEMBER countsForBest)
    Q_PROPERTY(QString hoverText MEMBER hoverText)
public:
    int lapId = 0;
    QString label;
    QString timeText;
    int timeMs = 0;
    double startTime = 0.0;
    bool isFastest = false;
    bool isComplete = false;
    bool isPitLap = false;
    bool countsForBest = false;
    QString hoverText;
};

// ── channel row ─────────────────────────────────────────────────────

class TraceLaneRow {
    Q_GADGET
    QML_VALUE_TYPE(traceLaneRow)
    Q_PROPERTY(QString key MEMBER key)
    Q_PROPERTY(QString kind MEMBER kind)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString unit MEMBER unit)
    Q_PROPERTY(double y MEMBER y)
    Q_PROPERTY(double height MEMBER height)
    Q_PROPERTY(bool expanded MEMBER expanded)
    Q_PROPERTY(QString chromeText MEMBER chromeText)
public:
    QString key, kind, title, unit, chromeText;
    double y = 0.0;
    double height = 0.0;
    bool expanded = false;
};

class ChannelRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString key MEMBER key)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString unit MEMBER unit)
    Q_PROPERTY(bool visible MEMBER visible)
    Q_PROPERTY(QString color MEMBER color)
    Q_PROPERTY(double weight MEMBER weight)
    Q_PROPERTY(double strokeWidth MEMBER strokeWidth)
    Q_PROPERTY(double fillOpacity MEMBER fillOpacity)
    Q_PROPERTY(QString referenceColor MEMBER referenceColor)
    Q_PROPERTY(bool source MEMBER source)
    Q_PROPERTY(bool sidecar MEMBER sidecar)
    Q_PROPERTY(bool span MEMBER span)
public:
    QString key;
    QString title;
    QString unit;
    bool visible = false;
    QString color;
    double weight = 1.0;
    double strokeWidth = 1.25;
    double fillOpacity = 0.0;
    QString referenceColor;
    bool source = false;
    bool sidecar = false;
    bool span = false;
};

// ── corner note row ─────────────────────────────────────────────────

class CornerNoteRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString text MEMBER text)
    Q_PROPERTY(QString severity MEMBER severity)
public:
    QString id;
    QString text;
    QString severity;
};

// ── corner row (CornerListModel) ────────────────────────────────────
// Combines the basic corner range (name/start/end from cornerList) with
// the per-lap comparison columns (from cornerComparison). Compare-only
// fields default to NaN so QML isFinite() checks work unchanged.

class CornerRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(double start MEMBER start)
    Q_PROPERTY(double end MEMBER end)
    Q_PROPERTY(double entrySpeed MEMBER entrySpeed)
    Q_PROPERTY(double apexSpeed MEMBER apexSpeed)
    Q_PROPERTY(double exitSpeed MEMBER exitSpeed)
    Q_PROPERTY(double speedDrop MEMBER speedDrop)
    Q_PROPERTY(double speedGain MEMBER speedGain)
    Q_PROPERTY(double time MEMBER time)
    Q_PROPERTY(int minGear MEMBER minGear)
    Q_PROPERTY(double maxSteering MEMBER maxSteering)
    Q_PROPERTY(double maxBrake MEMBER maxBrake)
    Q_PROPERTY(double minThrottle MEMBER minThrottle)
    Q_PROPERTY(double brakePoint MEMBER brakePoint)
    Q_PROPERTY(double liftPoint MEMBER liftPoint)
    Q_PROPERTY(double turnInPosition MEMBER turnInPosition)
    Q_PROPERTY(double apexPosition MEMBER apexPosition)
    Q_PROPERTY(double throttlePosition MEMBER throttlePosition)
    Q_PROPERTY(double turnInPoint MEMBER turnInPoint)
    Q_PROPERTY(double apexPoint MEMBER apexPoint)
    Q_PROPERTY(double throttlePoint MEMBER throttlePoint)
    Q_PROPERTY(double apexFraction MEMBER apexFraction)
    Q_PROPERTY(double cornerStartPosition MEMBER cornerStartPosition)
    Q_PROPERTY(double cornerEndPosition MEMBER cornerEndPosition)
    Q_PROPERTY(double contextWindowMeters MEMBER contextWindowMeters)
    Q_PROPERTY(double cornerLengthMeters MEMBER cornerLengthMeters)
    Q_PROPERTY(bool hasCompare MEMBER hasCompare)
    Q_PROPERTY(double compareApexFraction MEMBER compareApexFraction)
    Q_PROPERTY(double compareEntrySpeed MEMBER compareEntrySpeed)
    Q_PROPERTY(double compareApexSpeed MEMBER compareApexSpeed)
    Q_PROPERTY(double compareExitSpeed MEMBER compareExitSpeed)
    Q_PROPERTY(double compareTime MEMBER compareTime)
    Q_PROPERTY(int compareMinGear MEMBER compareMinGear)
    Q_PROPERTY(double compareMaxSteering MEMBER compareMaxSteering)
    Q_PROPERTY(double compareMaxBrake MEMBER compareMaxBrake)
    Q_PROPERTY(double compareMinThrottle MEMBER compareMinThrottle)
    Q_PROPERTY(double compareBrakePoint MEMBER compareBrakePoint)
    Q_PROPERTY(double compareLiftPoint MEMBER compareLiftPoint)
    Q_PROPERTY(double compareTurnInPosition MEMBER compareTurnInPosition)
    Q_PROPERTY(double compareApexPosition MEMBER compareApexPosition)
    Q_PROPERTY(double compareThrottlePosition MEMBER compareThrottlePosition)
    Q_PROPERTY(double compareTurnInPoint MEMBER compareTurnInPoint)
    Q_PROPERTY(double compareApexPoint MEMBER compareApexPoint)
    Q_PROPERTY(double compareThrottlePoint MEMBER compareThrottlePoint)
    Q_PROPERTY(double delta MEMBER delta)
    Q_PROPERTY(double entryTimeDelta MEMBER entryTimeDelta)
    Q_PROPERTY(double exitTimeDelta MEMBER exitTimeDelta)
    Q_PROPERTY(double entryDelta MEMBER entryDelta)
    Q_PROPERTY(double apexDelta MEMBER apexDelta)
    Q_PROPERTY(double exitDelta MEMBER exitDelta)
    Q_PROPERTY(double brakePointDelta MEMBER brakePointDelta)
    Q_PROPERTY(double liftPointDelta MEMBER liftPointDelta)
    Q_PROPERTY(double turnInDelta MEMBER turnInDelta)
    Q_PROPERTY(double apexPointDelta MEMBER apexPointDelta)
    Q_PROPERTY(double throttlePointDelta MEMBER throttlePointDelta)
    Q_PROPERTY(double score MEMBER score)
    Q_PROPERTY(QVariantList notes MEMBER notes)
    Q_PROPERTY(QString note MEMBER note)
public:
    QString name;
    double start = 0.0;
    double end = 0.0;
    double entrySpeed = 0.0;
    double apexSpeed = 0.0;
    double exitSpeed = 0.0;
    double speedDrop = 0.0;
    double speedGain = 0.0;
    double time = 0.0;
    int minGear = 0;
    double maxSteering = 0.0;
    double maxBrake = 0.0;
    double minThrottle = 1.0;
    double brakePoint = 0.0;
    double liftPoint = 0.0;
    double turnInPosition = 0.0;
    double apexPosition = 0.0;
    double throttlePosition = 0.0;
    double turnInPoint = 0.0;
    double apexPoint = 0.0;
    double throttlePoint = 0.0;
    double apexFraction = 0.0;
    double cornerStartPosition = 0.0;
    double cornerEndPosition = 0.0;
    double contextWindowMeters = 0.0;
    double cornerLengthMeters = 0.0;
    bool hasCompare = false;
    double compareApexFraction = std::numeric_limits<double>::quiet_NaN();
    double compareEntrySpeed = std::numeric_limits<double>::quiet_NaN();
    double compareApexSpeed = std::numeric_limits<double>::quiet_NaN();
    double compareExitSpeed = std::numeric_limits<double>::quiet_NaN();
    double compareTime = std::numeric_limits<double>::quiet_NaN();
    int compareMinGear = 0;
    double compareMaxSteering = std::numeric_limits<double>::quiet_NaN();
    double compareMaxBrake = std::numeric_limits<double>::quiet_NaN();
    double compareMinThrottle = std::numeric_limits<double>::quiet_NaN();
    double compareBrakePoint = std::numeric_limits<double>::quiet_NaN();
    double compareLiftPoint = std::numeric_limits<double>::quiet_NaN();
    double compareTurnInPosition = std::numeric_limits<double>::quiet_NaN();
    double compareApexPosition = std::numeric_limits<double>::quiet_NaN();
    double compareThrottlePosition = std::numeric_limits<double>::quiet_NaN();
    double compareTurnInPoint = std::numeric_limits<double>::quiet_NaN();
    double compareApexPoint = std::numeric_limits<double>::quiet_NaN();
    double compareThrottlePoint = std::numeric_limits<double>::quiet_NaN();
    double delta = std::numeric_limits<double>::quiet_NaN();
    double entryTimeDelta = std::numeric_limits<double>::quiet_NaN();
    double exitTimeDelta = std::numeric_limits<double>::quiet_NaN();
    double entryDelta = std::numeric_limits<double>::quiet_NaN();
    double apexDelta = std::numeric_limits<double>::quiet_NaN();
    double exitDelta = std::numeric_limits<double>::quiet_NaN();
    double brakePointDelta = std::numeric_limits<double>::quiet_NaN();
    double liftPointDelta = std::numeric_limits<double>::quiet_NaN();
    double turnInDelta = std::numeric_limits<double>::quiet_NaN();
    double apexPointDelta = std::numeric_limits<double>::quiet_NaN();
    double throttlePointDelta = std::numeric_limits<double>::quiet_NaN();
    double score = std::numeric_limits<double>::quiet_NaN();
    QVariantList notes;
    QString note;
};

// ── driver mapping row ──────────────────────────────────────────────

class DriverMappingRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString key MEMBER key)
    Q_PROPERTY(QString carNumber MEMBER carNumber)
    Q_PROPERTY(QString carClass MEMBER carClass)
    Q_PROPERTY(QString driverId MEMBER driverId)
    Q_PROPERTY(QString display MEMBER display)
public:
    QString key;
    QString carNumber;
    QString carClass;
    QString driverId;
    QString display;
};

// ── sync strategy row ───────────────────────────────────────────────

class SyncStrategyRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString label MEMBER label)
    Q_PROPERTY(QString shortLabel MEMBER shortLabel)
    Q_PROPERTY(QString detail MEMBER detail)
public:
    QString id;
    QString label;
    QString shortLabel;
    QString detail;
};

// ── cursor readout (returned by value from Q_INVOKABLE) ──────────────

class CursorReadout {
    Q_GADGET
    QML_VALUE_TYPE(cursorReadout)
    Q_PROPERTY(double dist MEMBER dist)
    Q_PROPERTY(double time MEMBER time)
    Q_PROPERTY(double speed MEMBER speed)
    Q_PROPERTY(int gear MEMBER gear)
    Q_PROPERTY(QString corner MEMBER corner)
    Q_PROPERTY(double delta MEMBER delta)
    Q_PROPERTY(bool hasDelta MEMBER hasDelta)
public:
    double dist = 0.0;
    double time = 0.0;
    double speed = 0.0;
    int gear = 0;
    QString corner;
    double delta = std::numeric_limits<double>::quiet_NaN();
    bool hasDelta = false;
};

// ── corner focus summary (returned by value from Q_INVOKABLE) ────────
// All CornerRow comparison fields plus the consistency columns that
// cornerFocusSummary() adds for the focused corner.

class CornerFocusSummary {
    Q_GADGET
    QML_VALUE_TYPE(cornerFocusSummary)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(double start MEMBER start)
    Q_PROPERTY(double end MEMBER end)
    Q_PROPERTY(double entrySpeed MEMBER entrySpeed)
    Q_PROPERTY(double apexSpeed MEMBER apexSpeed)
    Q_PROPERTY(double exitSpeed MEMBER exitSpeed)
    Q_PROPERTY(double speedDrop MEMBER speedDrop)
    Q_PROPERTY(double speedGain MEMBER speedGain)
    Q_PROPERTY(double time MEMBER time)
    Q_PROPERTY(int minGear MEMBER minGear)
    Q_PROPERTY(double maxSteering MEMBER maxSteering)
    Q_PROPERTY(double maxBrake MEMBER maxBrake)
    Q_PROPERTY(double minThrottle MEMBER minThrottle)
    Q_PROPERTY(double brakePoint MEMBER brakePoint)
    Q_PROPERTY(double liftPoint MEMBER liftPoint)
    Q_PROPERTY(double turnInPosition MEMBER turnInPosition)
    Q_PROPERTY(double apexPosition MEMBER apexPosition)
    Q_PROPERTY(double throttlePosition MEMBER throttlePosition)
    Q_PROPERTY(double turnInPoint MEMBER turnInPoint)
    Q_PROPERTY(double apexPoint MEMBER apexPoint)
    Q_PROPERTY(double throttlePoint MEMBER throttlePoint)
    Q_PROPERTY(double apexFraction MEMBER apexFraction)
    Q_PROPERTY(double cornerStartPosition MEMBER cornerStartPosition)
    Q_PROPERTY(double cornerEndPosition MEMBER cornerEndPosition)
    Q_PROPERTY(double contextWindowMeters MEMBER contextWindowMeters)
    Q_PROPERTY(double cornerLengthMeters MEMBER cornerLengthMeters)
    Q_PROPERTY(bool hasCompare MEMBER hasCompare)
    Q_PROPERTY(double compareApexFraction MEMBER compareApexFraction)
    Q_PROPERTY(double compareEntrySpeed MEMBER compareEntrySpeed)
    Q_PROPERTY(double compareApexSpeed MEMBER compareApexSpeed)
    Q_PROPERTY(double compareExitSpeed MEMBER compareExitSpeed)
    Q_PROPERTY(double compareTime MEMBER compareTime)
    Q_PROPERTY(int compareMinGear MEMBER compareMinGear)
    Q_PROPERTY(double compareMaxSteering MEMBER compareMaxSteering)
    Q_PROPERTY(double compareMaxBrake MEMBER compareMaxBrake)
    Q_PROPERTY(double compareMinThrottle MEMBER compareMinThrottle)
    Q_PROPERTY(double compareBrakePoint MEMBER compareBrakePoint)
    Q_PROPERTY(double compareLiftPoint MEMBER compareLiftPoint)
    Q_PROPERTY(double compareTurnInPosition MEMBER compareTurnInPosition)
    Q_PROPERTY(double compareApexPosition MEMBER compareApexPosition)
    Q_PROPERTY(double compareThrottlePosition MEMBER compareThrottlePosition)
    Q_PROPERTY(double compareTurnInPoint MEMBER compareTurnInPoint)
    Q_PROPERTY(double compareApexPoint MEMBER compareApexPoint)
    Q_PROPERTY(double compareThrottlePoint MEMBER compareThrottlePoint)
    Q_PROPERTY(double delta MEMBER delta)
    Q_PROPERTY(double entryTimeDelta MEMBER entryTimeDelta)
    Q_PROPERTY(double exitTimeDelta MEMBER exitTimeDelta)
    Q_PROPERTY(double entryDelta MEMBER entryDelta)
    Q_PROPERTY(double apexDelta MEMBER apexDelta)
    Q_PROPERTY(double exitDelta MEMBER exitDelta)
    Q_PROPERTY(double brakePointDelta MEMBER brakePointDelta)
    Q_PROPERTY(double liftPointDelta MEMBER liftPointDelta)
    Q_PROPERTY(double turnInDelta MEMBER turnInDelta)
    Q_PROPERTY(double apexPointDelta MEMBER apexPointDelta)
    Q_PROPERTY(double throttlePointDelta MEMBER throttlePointDelta)
    Q_PROPERTY(double score MEMBER score)
    Q_PROPERTY(QVariantList notes MEMBER notes)
    Q_PROPERTY(QString note MEMBER note)
    Q_PROPERTY(bool consistencyLoading MEMBER consistencyLoading)
    Q_PROPERTY(int consistencyLapCount MEMBER consistencyLapCount)
    Q_PROPERTY(int consistencyValidLapCount MEMBER consistencyValidLapCount)
    Q_PROPERTY(int consistencyBrakeLapCount MEMBER consistencyBrakeLapCount)
    Q_PROPERTY(bool brakeConsistencyAvailable MEMBER brakeConsistencyAvailable)
    Q_PROPERTY(double brakePointMedian MEMBER brakePointMedian)
    Q_PROPERTY(double brakePointStdDev MEMBER brakePointStdDev)
    Q_PROPERTY(double brakePointRange MEMBER brakePointRange)
    Q_PROPERTY(double brakePointVsMedian MEMBER brakePointVsMedian)
public:
    QString name;
    double start = 0.0;
    double end = 0.0;
    double entrySpeed = 0.0;
    double apexSpeed = 0.0;
    double exitSpeed = 0.0;
    double speedDrop = 0.0;
    double speedGain = 0.0;
    double time = 0.0;
    int minGear = 0;
    double maxSteering = 0.0;
    double maxBrake = 0.0;
    double minThrottle = 1.0;
    double brakePoint = 0.0;
    double liftPoint = 0.0;
    double turnInPosition = 0.0;
    double apexPosition = 0.0;
    double throttlePosition = 0.0;
    double turnInPoint = 0.0;
    double apexPoint = 0.0;
    double throttlePoint = 0.0;
    double apexFraction = 0.0;
    double cornerStartPosition = 0.0;
    double cornerEndPosition = 0.0;
    double contextWindowMeters = 0.0;
    double cornerLengthMeters = 0.0;
    bool hasCompare = false;
    double compareApexFraction = std::numeric_limits<double>::quiet_NaN();
    double compareEntrySpeed = std::numeric_limits<double>::quiet_NaN();
    double compareApexSpeed = std::numeric_limits<double>::quiet_NaN();
    double compareExitSpeed = std::numeric_limits<double>::quiet_NaN();
    double compareTime = std::numeric_limits<double>::quiet_NaN();
    int compareMinGear = 0;
    double compareMaxSteering = std::numeric_limits<double>::quiet_NaN();
    double compareMaxBrake = std::numeric_limits<double>::quiet_NaN();
    double compareMinThrottle = std::numeric_limits<double>::quiet_NaN();
    double compareBrakePoint = std::numeric_limits<double>::quiet_NaN();
    double compareLiftPoint = std::numeric_limits<double>::quiet_NaN();
    double compareTurnInPosition = std::numeric_limits<double>::quiet_NaN();
    double compareApexPosition = std::numeric_limits<double>::quiet_NaN();
    double compareThrottlePosition = std::numeric_limits<double>::quiet_NaN();
    double compareTurnInPoint = std::numeric_limits<double>::quiet_NaN();
    double compareApexPoint = std::numeric_limits<double>::quiet_NaN();
    double compareThrottlePoint = std::numeric_limits<double>::quiet_NaN();
    double delta = std::numeric_limits<double>::quiet_NaN();
    double entryTimeDelta = std::numeric_limits<double>::quiet_NaN();
    double exitTimeDelta = std::numeric_limits<double>::quiet_NaN();
    double entryDelta = std::numeric_limits<double>::quiet_NaN();
    double apexDelta = std::numeric_limits<double>::quiet_NaN();
    double exitDelta = std::numeric_limits<double>::quiet_NaN();
    double brakePointDelta = std::numeric_limits<double>::quiet_NaN();
    double liftPointDelta = std::numeric_limits<double>::quiet_NaN();
    double turnInDelta = std::numeric_limits<double>::quiet_NaN();
    double apexPointDelta = std::numeric_limits<double>::quiet_NaN();
    double throttlePointDelta = std::numeric_limits<double>::quiet_NaN();
    double score = std::numeric_limits<double>::quiet_NaN();
    QVariantList notes;
    QString note;
    bool consistencyLoading = false;
    int consistencyLapCount = 0;
    int consistencyValidLapCount = 0;
    int consistencyBrakeLapCount = 0;
    bool brakeConsistencyAvailable = false;
    double brakePointMedian = std::numeric_limits<double>::quiet_NaN();
    double brakePointStdDev = std::numeric_limits<double>::quiet_NaN();
    double brakePointRange = std::numeric_limits<double>::quiet_NaN();
    double brakePointVsMedian = std::numeric_limits<double>::quiet_NaN();
};

// ── location row (gadget only, model wiring is wave 2b) ─────────────

class LocationRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString target MEMBER target)
    Q_PROPERTY(bool enabled MEMBER enabled)
    Q_PROPERTY(QString type MEMBER type)
public:
    QString id;
    QString name;
    QString target;
    bool enabled = true;
    QString type;
};

// ── session info row (returned by value from Q_INVOKABLE) ───────────

class SessionInfoRow {
    Q_GADGET
    QML_VALUE_TYPE(sessionInfo)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString driver MEMBER driver)
    Q_PROPERTY(QString track MEMBER track)
    Q_PROPERTY(QString date MEMBER date)
    Q_PROPERTY(int lapCount MEMBER lapCount)
    Q_PROPERTY(QString bestLapText MEMBER bestLapText)
public:
    QString name;
    QString driver;
    QString track;
    QString date;
    int lapCount = 0;
    QString bestLapText;
};

// ── filmstrip session row ───────────────────────────────────────────

class FilmstripSessionRow {
    Q_GADGET
    QML_ANONYMOUS
    Q_PROPERTY(QString sessionKey MEMBER sessionKey)
    Q_PROPERTY(QString driverName MEMBER driverName)
    Q_PROPERTY(QString bestTime MEMBER bestTime)
    Q_PROPERTY(bool reference MEMBER reference)
public:
    QString sessionKey;
    QString driverName;
    QString bestTime;
    bool reference = false;
};

// ── canonical active-session identity ───────────────────────────────
// Video, traces, filmstrip, and sidebar must name the same session.

class ActiveSessionRoles {
    Q_GADGET
    QML_VALUE_TYPE(activeSessionRoles)
    Q_PROPERTY(QString sessionKey MEMBER sessionKey)
    Q_PROPERTY(QString videoIdentity MEMBER videoIdentity)
    Q_PROPERTY(QString filmstripKey MEMBER filmstripKey)
    Q_PROPERTY(QString sidebarKey MEMBER sidebarKey)
public:
    QString sessionKey;
    QString videoIdentity;
    QString filmstripKey;
    QString sidebarKey;

    bool agree() const {
        if (sessionKey != filmstripKey || sessionKey != sidebarKey)
            return false;
        if (!videoIdentity.isEmpty() && videoIdentity != sessionKey)
            return false;
        return true;
    }
};

Q_DECLARE_METATYPE(TraceLaneRow)
Q_DECLARE_METATYPE(CursorReadout)
Q_DECLARE_METATYPE(CornerFocusSummary)
Q_DECLARE_METATYPE(SessionInfoRow)
Q_DECLARE_METATYPE(ActiveSessionRoles)
