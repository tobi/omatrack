#include "PluginHost.h"

#include "PathJail.h"
#include "RemoteCache.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

Q_LOGGING_CATEGORY(lcPlugin, "omatrack.plugin")

namespace omatrack {
namespace {

constexpr const char* kAbortKey = "omatrack.plugin.abort";
constexpr size_t kMemoryCap = 64 * 1024 * 1024;
constexpr size_t kMaxBody = 32 * 1024 * 1024;

// ── sandbox ─────────────────────────────────────────────────────────

struct AllocState {
    size_t used = 0;
    size_t cap = kMemoryCap;
    bool overflow = false;
};

struct HookState {
    QElapsedTimer clock;
    int timeoutMs = 60000;
    AllocState* alloc = nullptr;
    const char* reason = nullptr;
};

void* cappedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* alloc = static_cast<AllocState*>(ud);
    size_t current = alloc->used;
    if (ptr) current -= osize;
    if (nsize == 0) {
        alloc->used = current;
        std::free(ptr);
        return nullptr;
    }
    if (current + nsize > alloc->cap) {
        alloc->overflow = true;
        return nullptr;
    }
    void* next = std::realloc(ptr, nsize);
    if (next) alloc->used = current + nsize;
    return next;
}

void countHook(lua_State* L, lua_Debug*) {
    lua_pushstring(L, kAbortKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* hook = static_cast<HookState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!hook) return;
    if (hook->alloc && hook->alloc->overflow) {
        hook->reason = "plugin exceeded its memory cap";
        luaL_error(L, "%s", hook->reason);
    }
    if (hook->clock.hasExpired(hook->timeoutMs)) {
        hook->reason = "plugin timed out";
        luaL_error(L, "%s", hook->reason);
    }
}

// ── value conversion ────────────────────────────────────────────────

sol::object toLua(sol::state_view lua, const QJsonValue& value);

sol::object toLua(sol::state_view lua, const QVariant& value) {
    switch (value.typeId()) {
        case QMetaType::Bool: return sol::make_object(lua, value.toBool());
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::UInt:
        case QMetaType::ULongLong:
            return sol::make_object(lua, value.toLongLong());
        case QMetaType::Double:
        case QMetaType::Float: return sol::make_object(lua, value.toDouble());
        case QMetaType::QString:
            return sol::make_object(lua, value.toString().toStdString());
        case QMetaType::QVariantMap: {
            sol::table table = lua.create_table();
            const QVariantMap map = value.toMap();
            for (auto it = map.cbegin(); it != map.cend(); ++it)
                table[it.key().toStdString()] = toLua(lua, it.value());
            return table;
        }
        case QMetaType::QVariantList: {
            sol::table table = lua.create_table();
            const QVariantList list = value.toList();
            for (int index = 0; index < list.size(); ++index)
                table[index + 1] = toLua(lua, list.at(index));
            return table;
        }
        case QMetaType::QJsonValue: return toLua(lua, value.toJsonValue());
        default: return sol::make_object(lua, sol::lua_nil);
    }
}

sol::object toLua(sol::state_view lua, const QJsonValue& value) {
    switch (value.type()) {
        case QJsonValue::Bool: return sol::make_object(lua, value.toBool());
        case QJsonValue::Double: {
            const double number = value.toDouble();
            if (std::floor(number) == number &&
                std::abs(number) < 9007199254740992.0)
                return sol::make_object(lua, qint64(number));
            return sol::make_object(lua, number);
        }
        case QJsonValue::String:
            return sol::make_object(lua, value.toString().toStdString());
        case QJsonValue::Array: {
            sol::table table = lua.create_table();
            const QJsonArray array = value.toArray();
            for (int index = 0; index < array.size(); ++index)
                table[index + 1] = toLua(lua, array.at(index));
            return table;
        }
        case QJsonValue::Object: {
            sol::table table = lua.create_table();
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it)
                table[it.key().toStdString()] = toLua(lua, it.value());
            return table;
        }
        default: return sol::make_object(lua, sol::lua_nil);
    }
}

QJsonValue toJson(const sol::object& value, int depth = 0) {
    if (depth > 32) return QJsonValue();
    switch (value.get_type()) {
        case sol::type::boolean: return value.as<bool>();
        case sol::type::number: {
            const double number = value.as<double>();
            return std::isfinite(number) ? QJsonValue(number) : QJsonValue();
        }
        case sol::type::string:
            return QString::fromStdString(value.as<std::string>());
        case sol::type::table: {
            sol::table table = value.as<sol::table>();
            // A pure 1..n sequence becomes an array; anything else an object.
            const size_t size = table.size();
            bool sequence = size > 0;
            if (sequence) {
                for (const auto& pair : table) {
                    const double index =
                        pair.first.get_type() == sol::type::number
                            ? pair.first.as<double>()
                            : 0.0;
                    if (index < 1.0 || index > double(size) ||
                        std::floor(index) != index) {
                        sequence = false;
                        break;
                    }
                }
            }
            if (sequence) {
                QJsonArray array;
                for (size_t index = 1; index <= size; ++index)
                    array.append(toJson(table[index], depth + 1));
                return array;
            }
            QJsonObject object;
            for (const auto& pair : table) {
                QString key;
                if (pair.first.get_type() == sol::type::string)
                    key = QString::fromStdString(pair.first.as<std::string>());
                else if (pair.first.get_type() == sol::type::number)
                    key = QString::number(pair.first.as<double>());
                else
                    continue;
                object.insert(key, toJson(pair.second, depth + 1));
            }
            return object;
        }
        default: return QJsonValue();
    }
}

// ── kv store ────────────────────────────────────────────────────────

struct KvStore {
    QString path;
    QJsonObject entries;
    bool loaded = false;
    bool dirty = false;

    void load() {
        if (loaded) return;
        loaded = true;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        entries = QJsonDocument::fromJson(file.readAll()).object();
    }
    sol::object get(sol::state_view lua, const std::string& key) {
        load();
        const QJsonObject entry =
            entries.value(QString::fromStdString(key)).toObject();
        if (entry.isEmpty()) return sol::make_object(lua, sol::lua_nil);
        const double expires = entry.value(QStringLiteral("exp")).toDouble(0.0);
        if (expires > 0.0 &&
            expires < double(QDateTime::currentSecsSinceEpoch())) {
            entries.remove(QString::fromStdString(key));
            dirty = true;
            return sol::make_object(lua, sol::lua_nil);
        }
        return toLua(lua, entry.value(QStringLiteral("v")));
    }
    void set(const std::string& key, const sol::object& value,
             sol::optional<double> ttlSeconds) {
        load();
        QJsonObject entry{{QStringLiteral("v"), toJson(value)}};
        if (ttlSeconds && *ttlSeconds > 0.0)
            entry.insert(
                QStringLiteral("exp"),
                double(QDateTime::currentSecsSinceEpoch()) + *ttlSeconds);
        entries.insert(QString::fromStdString(key), entry);
        dirty = true;
    }
    void remove(const std::string& key) {
        load();
        if (entries.contains(QString::fromStdString(key))) {
            entries.remove(QString::fromStdString(key));
            dirty = true;
        }
    }
    void save() {
        if (!dirty) return;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(QJsonDocument(entries).toJson(QJsonDocument::Compact));
        file.commit();
        dirty = false;
    }
};

// ── one sandboxed run ───────────────────────────────────────────────

struct Runtime {
    PluginInfo plugin;
    PluginPaths paths;
    IoCancel cancel;
    AllocState alloc;
    HookState hook;
    KvStore kv;
    QStringList logLines;
    lua_State* L = nullptr;

    ~Runtime() {
        if (L) lua_close(L);
    }

    QString cacheDir() const {
        return QDir(paths.cacheRoot).filePath(plugin.id);
    }

    /// Resolve a plugin-relative path. Reads may come from the plugin folder
    /// or the cache folder; writes only from the cache folder.
    QString resolve(const std::string& relative, bool forWrite,
                    QString* error) {
        const QString rel = QString::fromStdString(relative);
        if (!forWrite) {
            const auto inPlugin = jailRelativePath(plugin.directory, rel);
            if (inPlugin.ok && QFileInfo::exists(inPlugin.absolutePath))
                return inPlugin.absolutePath;
        }
        QDir().mkpath(cacheDir());
        const auto inCache = jailRelativePath(cacheDir(), rel);
        if (!inCache.ok) {
            if (error) *error = inCache.error;
            return {};
        }
        return inCache.absolutePath;
    }

    bool open(int timeoutMs, QString* error) {
        alloc.cap = kMemoryCap;
        L = lua_newstate(cappedAlloc, &alloc);
        if (!L) {
            *error = QStringLiteral("plugin exceeded its memory cap");
            return false;
        }
        luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 1);
        lua_pop(L, 1);
        for (const char* name : {"load", "loadfile", "dofile", "loadstring",
                                 "require", "collectgarbage", "rawset",
                                 "rawget", "setmetatable", "getmetatable"}) {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }
        sol::state_view lua(L);
        sol::table os = lua["os"];
        // Keep the clock, drop everything that touches the process or disk.
        for (const char* name : {"execute", "exit", "getenv", "remove",
                                 "rename", "setlocale", "tmpname"})
            os[name] = sol::lua_nil;
        lua["string"]["dump"] = sol::lua_nil;
        lua["io"] = sol::lua_nil;
        lua["package"] = sol::lua_nil;
        installApi(lua);

        hook.timeoutMs = std::max(1, timeoutMs);
        hook.alloc = &alloc;
        hook.clock.start();
        lua_pushstring(L, kAbortKey);
        lua_pushlightuserdata(L, &hook);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_sethook(L, countHook, LUA_MASKCOUNT, 1000);
        kv.path = QDir(cacheDir()).filePath(QStringLiteral("kv.json"));
        return true;
    }

    void installApi(sol::state_view lua) {
        Runtime* self = this;
        lua["log"] = [self](sol::variadic_args args) {
            QStringList parts;
            for (const auto& arg : args) {
                sol::object value = arg;
                if (value.get_type() == sol::type::string)
                    parts << QString::fromStdString(value.as<std::string>());
                else if (value.get_type() == sol::type::number)
                    parts << QString::number(value.as<double>(), 'g', 15);
                else if (value.get_type() == sol::type::boolean)
                    parts << (value.as<bool>() ? QStringLiteral("true")
                                               : QStringLiteral("false"));
                else if (value.get_type() == sol::type::table)
                    parts << QString::fromUtf8(
                        QJsonDocument::fromVariant(toJson(value).toVariant())
                            .toJson(QJsonDocument::Compact));
                else
                    parts << QStringLiteral("nil");
            }
            const QString line = parts.join(QLatin1Char(' '));
            self->logLines.append(line);
            qCInfo(lcPlugin).noquote() << self->plugin.id << line;
        };

        sol::table json = lua.create_named_table("json");
        json["decode"] = [](sol::this_state state, const std::string& text) {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(
                QByteArray::fromStdString(text), &parseError);
            sol::state_view lua(state);
            if (parseError.error != QJsonParseError::NoError)
                return std::make_tuple(
                    sol::make_object(lua, sol::lua_nil),
                    sol::make_object(lua,
                                     parseError.errorString().toStdString()));
            const QJsonValue root = document.isArray()
                                        ? QJsonValue(document.array())
                                        : QJsonValue(document.object());
            return std::make_tuple(toLua(lua, root),
                                   sol::make_object(lua, sol::lua_nil));
        };
        json["encode"] = [](const sol::object& value) {
            const QJsonValue json = toJson(value);
            QJsonDocument document;
            if (json.isArray())
                document.setArray(json.toArray());
            else if (json.isObject())
                document.setObject(json.toObject());
            else {
                // Scalars: wrap, serialise, unwrap.
                const QByteArray wrapped = QJsonDocument(QJsonArray{json})
                                               .toJson(QJsonDocument::Compact);
                return wrapped.mid(1, wrapped.size() - 2).toStdString();
            }
            return document.toJson(QJsonDocument::Compact).toStdString();
        };

        sol::table kvTable = lua.create_named_table("kv");
        kvTable["get"] = [self](sol::this_state state, const std::string& key) {
            return self->kv.get(sol::state_view(state), key);
        };
        kvTable["set"] = [self](const std::string& key,
                                const sol::object& value,
                                sol::optional<double> ttl) {
            self->kv.set(key, value, ttl);
        };
        kvTable["delete"] = [self](const std::string& key) {
            self->kv.remove(key);
        };

        sol::table http = lua.create_named_table("http");
        http["get"] = [self](sol::this_state state, const std::string& url,
                             sol::optional<sol::table> options) {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            const QUrl target(QString::fromStdString(url));
            if (!target.isValid() ||
                (target.scheme() != QLatin1String("https") &&
                 target.scheme() != QLatin1String("http"))) {
                result["status"] = 0;
                result["error"] = "only http(s) URLs are allowed";
                return result;
            }
            QVector<std::pair<QByteArray, QByteArray>> headers;
            if (options) {
                sol::optional<sol::table> headerTable =
                    options->get<sol::optional<sol::table>>("headers");
                if (headerTable) {
                    for (const auto& pair : *headerTable) {
                        if (pair.first.get_type() != sol::type::string ||
                            pair.second.get_type() != sol::type::string)
                            continue;
                        headers.append({QByteArray::fromStdString(
                                            pair.first.as<std::string>()),
                                        QByteArray::fromStdString(
                                            pair.second.as<std::string>())});
                    }
                }
            }
            const QString userAgent =
                QStringLiteral("omatrack-plugin/%1").arg(self->plugin.id);
            const RequestFactory build = [headers, userAgent](const QUrl& hop) {
                QNetworkRequest request = makeRequest(hop);
                request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
                for (const auto& header : headers)
                    request.setRawHeader(header.first, header.second);
                return request;
            };
            const HttpResponse response =
                sendFollowing(target, "GET", build, {}, self->cancel);
            result["status"] = response.status;
            if (response.body.size() > qsizetype(kMaxBody)) {
                result["error"] = "response body exceeds 32 MiB";
                return result;
            }
            result["body"] = response.body.toStdString();
            if (!response.error.isEmpty())
                result["error"] = response.error.toStdString();
            sol::table headerOut = lua.create_table();
            for (auto it = response.headers.cbegin();
                 it != response.headers.cend(); ++it)
                headerOut[it.key().toStdString()] = it.value().toStdString();
            result["headers"] = headerOut;
            return result;
        };

        sol::table io = lua.create_named_table("io");
        io["plugin_dir"] = plugin.directory.toStdString();
        io["cache_dir"] = cacheDir().toStdString();
        io["read"] = [self](sol::this_state state,
                            const std::string& relative) {
            sol::state_view lua(state);
            QString error;
            const QString path = self->resolve(relative, false, &error);
            QFile file(path);
            if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
                return std::make_tuple(
                    sol::make_object(lua, sol::lua_nil),
                    sol::make_object(
                        lua, (error.isEmpty() ? file.errorString() : error)
                                 .toStdString()));
            return std::make_tuple(
                sol::make_object(lua, file.readAll().toStdString()),
                sol::make_object(lua, sol::lua_nil));
        };
        io["write"] = [self](const std::string& relative,
                             const std::string& text) {
            QString error;
            const QString path = self->resolve(relative, true, &error);
            if (path.isEmpty()) return false;
            QDir().mkpath(QFileInfo(path).absolutePath());
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            file.write(QByteArray::fromStdString(text));
            return file.commit();
        };
        io["exists"] = [self](const std::string& relative) {
            QString error;
            const QString path = self->resolve(relative, false, &error);
            return !path.isEmpty() && QFileInfo::exists(path);
        };
        io["list"] = [self](sol::this_state state,
                            const std::string& relative) {
            sol::state_view lua(state);
            sol::table names = lua.create_table();
            QString error;
            const QString path =
                self->resolve(relative.empty() ? "." : relative, false, &error);
            if (path.isEmpty()) return names;
            int index = 1;
            for (const QString& name : QDir(path).entryList(
                     QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                     QDir::Name))
                names[index++] = name.toStdString();
            return names;
        };
    }

    QString failure(const char* fallback) const {
        if (alloc.overflow)
            return QStringLiteral("plugin exceeded its memory cap");
        if (hook.reason) return QString::fromUtf8(hook.reason);
        if (L && lua_isstring(L, -1))
            return QString::fromUtf8(lua_tostring(L, -1));
        return QString::fromUtf8(fallback);
    }

    /// Load plugin.lua and leave its table on the stack.
    bool loadPlugin(QString* error) {
        QFile file(
            QDir(plugin.directory).filePath(QStringLiteral("plugin.lua")));
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("cannot read plugin.lua");
            return false;
        }
        const QByteArray source = file.readAll();
        const QByteArray chunk =
            QStringLiteral("=%1/plugin.lua").arg(plugin.id).toUtf8();
        if (luaL_loadbuffer(L, source.constData(), size_t(source.size()),
                            chunk.constData()) != LUA_OK ||
            lua_pcall(L, 0, 1, 0) != LUA_OK) {
            *error = failure("plugin.lua failed to load");
            return false;
        }
        if (!lua_istable(L, -1)) {
            *error = QStringLiteral("plugin.lua must return a table");
            return false;
        }
        return true;
    }

    sol::table pushSession(sol::state_view lua, const PluginSession& session) {
        sol::table table = lua.create_table();
        table["path"] = session.path.toStdString();
        table["name"] = session.name.toStdString();
        table["track"] = session.track.toStdString();
        table["driver"] = session.driver.toStdString();
        table["date"] = session.date.toStdString();
        table["timezone"] = session.timezone.toStdString();
        if (session.utcStartNs >= 0) {
            table["utc_start_ns"] = session.utcStartNs;
            table["utc_end_ns"] =
                session.utcStartNs + (session.endNs - session.startNs);
        }
        table["start_ns"] = session.startNs;
        table["end_ns"] = session.endNs;
        table["duration_ns"] = session.endNs - session.startNs;
        if (session.hasLocation) {
            table["latitude"] = session.latitude;
            table["longitude"] = session.longitude;
        }
        if (session.lapId >= 0) {
            sol::table lap = lua.create_table();
            lap["id"] = session.lapId;
            lap["start_ns"] = session.lapStartNs;
            lap["end_ns"] = session.lapEndNs;
            table["lap"] = lap;
        }
        return table;
    }
};

const QRegularExpression& idPattern() {
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9][a-z0-9_-]{0,63}$"));
    return pattern;
}

QString stringField(const sol::table& table, const char* name) {
    sol::object value = table[name];
    return value.get_type() == sol::type::string
               ? QString::fromStdString(value.as<std::string>())
               : QString();
}

}  // namespace

// ── paths ───────────────────────────────────────────────────────────

PluginPaths PluginPaths::defaults() {
    PluginPaths paths;
    paths.pluginRoot =
        QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
            .filePath(QStringLiteral("omatrack/plugins"));
    paths.cacheRoot =
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("plugins"));
    return paths;
}

// ── worker entry points ─────────────────────────────────────────────

PluginInfo describePlugin(const QString& directory) {
    PluginInfo info;
    info.directory = QDir(directory).absolutePath();
    info.id = QFileInfo(info.directory).fileName();
    Runtime runtime;
    runtime.plugin = info;
    runtime.paths.cacheRoot = QDir::tempPath();  // describe never writes
    QString error;
    if (!runtime.open(5000, &error) || !runtime.loadPlugin(&error)) {
        info.error = error;
        return info;
    }
    sol::state_view lua(runtime.L);
    sol::table table = sol::stack::get<sol::table>(runtime.L, -1);
    const QString id = stringField(table, "id");
    if (!idPattern().match(id).hasMatch()) {
        info.error = QStringLiteral(
                         "plugin id must match [a-z0-9][a-z0-9_-]* (got '%1')")
                         .arg(id);
        return info;
    }
    info.id = id;
    info.name = stringField(table, "name");
    if (info.name.isEmpty()) info.name = id;
    sol::object version = table["version"];
    info.version =
        version.get_type() == sol::type::number ? int(version.as<double>()) : 0;
    if (table["channels"].get_type() != sol::type::function ||
        table["samples"].get_type() != sol::type::function)
        info.error = QStringLiteral(
            "plugin must define channels(session) and samples(session, keys)");
    return info;
}

PluginOffer runPluginChannels(const PluginInfo& plugin,
                              const PluginSession& session,
                              const PluginPaths& paths, const IoCancel& cancel,
                              int timeoutMs) {
    PluginOffer offer;
    offer.plugin = plugin;
    if (!plugin.error.isEmpty()) {
        offer.error = plugin.error;
        return offer;
    }
    Runtime runtime;
    runtime.plugin = plugin;
    runtime.paths = paths;
    runtime.cancel = cancel;
    QString error;
    if (!runtime.open(timeoutMs, &error) || !runtime.loadPlugin(&error)) {
        offer.error = error;
        return offer;
    }
    sol::state_view lua(runtime.L);
    sol::table table = sol::stack::get<sol::table>(runtime.L, -1);
    sol::protected_function channels = table["channels"];
    sol::protected_function_result result =
        channels(runtime.pushSession(lua, session));
    runtime.kv.save();
    if (!result.valid()) {
        sol::error luaError = result;
        offer.error = runtime.hook.reason
                          ? QString::fromUtf8(runtime.hook.reason)
                          : QString::fromUtf8(luaError.what());
        return offer;
    }
    if (result.return_count() >= 2) {
        sol::object note = result[1];
        if (note.get_type() == sol::type::string)
            offer.note = QString::fromStdString(note.as<std::string>());
    }
    sol::object first = result[0];
    if (first.get_type() != sol::type::table) {
        if (first.get_type() != sol::type::lua_nil)
            offer.error = QStringLiteral("channels() must return a list");
        return offer;
    }
    sol::table list = first.as<sol::table>();
    for (const auto& pair : list) {
        if (pair.second.get_type() != sol::type::table) continue;
        sol::table row = pair.second.as<sol::table>();
        PluginChannelOffer channel;
        channel.key = stringField(row, "key");
        channel.name = stringField(row, "name");
        channel.unit = stringField(row, "unit");
        sol::object visible = row["default_visible"];
        channel.defaultVisible =
            visible.get_type() != sol::type::boolean || visible.as<bool>();
        if (channel.key.isEmpty()) continue;
        if (channel.name.isEmpty()) channel.name = channel.key;
        offer.channels.append(std::move(channel));
    }
    return offer;
}

PluginSamplesResult runPluginSamples(const PluginInfo& plugin,
                                     const PluginSession& session,
                                     const QStringList& keys,
                                     const PluginPaths& paths,
                                     const IoCancel& cancel, int timeoutMs) {
    PluginSamplesResult out;
    out.pluginId = plugin.id;
    out.session = session;
    if (!plugin.error.isEmpty()) {
        out.error = plugin.error;
        return out;
    }
    Runtime runtime;
    runtime.plugin = plugin;
    runtime.paths = paths;
    runtime.cancel = cancel;
    QString error;
    if (!runtime.open(timeoutMs, &error) || !runtime.loadPlugin(&error)) {
        out.error = error;
        return out;
    }
    sol::state_view lua(runtime.L);
    sol::table table = sol::stack::get<sol::table>(runtime.L, -1);
    sol::table keyList = lua.create_table();
    for (int index = 0; index < keys.size(); ++index)
        keyList[index + 1] = keys.at(index).toStdString();
    sol::protected_function samples = table["samples"];
    sol::protected_function_result result =
        samples(runtime.pushSession(lua, session), keyList);
    runtime.kv.save();
    out.logLines = runtime.logLines;
    if (!result.valid()) {
        sol::error luaError = result;
        out.error = runtime.hook.reason ? QString::fromUtf8(runtime.hook.reason)
                                        : QString::fromUtf8(luaError.what());
        return out;
    }
    if (result.get_type() != sol::type::table) {
        out.error =
            QStringLiteral("samples() must return a table keyed by channel");
        return out;
    }
    sol::table byKey = result;
    for (const QString& key : keys) {
        sol::object entry = byKey[key.toStdString()];
        if (entry.get_type() != sol::type::table) continue;
        sol::table series = entry.as<sol::table>();
        sol::object tObject = series["t"];
        sol::object utcObject = series["utc_ns"];
        sol::object vObject = series["v"];
        const bool absolute = tObject.get_type() != sol::type::table;
        if ((absolute && utcObject.get_type() != sol::type::table) ||
            vObject.get_type() != sol::type::table)
            continue;
        if (absolute && session.utcStartNs < 0) {
            out.error = QStringLiteral(
                            "channel '%1' uses utc_ns but the recording has no "
                            "wall clock")
                            .arg(key);
            continue;
        }
        sol::table times =
            absolute ? utcObject.as<sol::table>() : tObject.as<sol::table>();
        sol::table values = vObject.as<sol::table>();
        // The time array defines the length: `v = {10, nil}` has Lua length
        // 1, and a missing value is a missing reading (NaN), not a shorter
        // series.
        const size_t count = times.size();
        PluginSeries out_series;
        out_series.key = key;
        out_series.times = std::make_shared<std::vector<qint64>>();
        out_series.values = std::make_shared<std::vector<double>>();
        out_series.times->reserve(count);
        out_series.values->reserve(count);
        // Read times through the raw API: Lua integers must not round
        // through a double (Unix nanoseconds exceed 2^53).
        times.push();
        const int timesIndex = lua_gettop(runtime.L);
        for (size_t index = 1; index <= count; ++index) {
            lua_rawgeti(runtime.L, timesIndex, lua_Integer(index));
            qint64 ns = 0;
            bool numeric = true;
            if (lua_isinteger(runtime.L, -1))
                ns = lua_tointeger(runtime.L, -1);
            else if (lua_isnumber(runtime.L, -1))
                ns = qint64(std::llround(lua_tonumber(runtime.L, -1)));
            else
                numeric = false;
            lua_pop(runtime.L, 1);
            if (!numeric) continue;
            sol::object v = values[index];
            if (absolute) ns -= session.utcStartNs;
            double value = std::numeric_limits<double>::quiet_NaN();
            if (v.get_type() == sol::type::number) value = v.as<double>();
            if (!out_series.times->empty() && ns < out_series.times->back()) {
                out.error = QStringLiteral(
                                "channel '%1' samples are not sorted by time")
                                .arg(key);
                break;
            }
            out_series.times->push_back(ns);
            out_series.values->push_back(value);
        }
        lua_settop(runtime.L, timesIndex - 1);
        if (!out_series.times->empty())
            out.series.append(std::move(out_series));
    }
    return out;
}

// ── host ────────────────────────────────────────────────────────────

PluginHost::PluginHost(QObject* parent, PluginPaths paths)
    : QObject(parent), paths_(std::move(paths)), discoveryJob_(this) {
    qRegisterMetaType<PluginSamplesResult>();
}

void PluginHost::discover() {
    const QString root = paths_.pluginRoot;
    discoveryJob_.start(
        [root](IoCancel cancel) {
            QVector<PluginInfo> found;
            const QDir directory(root);
            if (!directory.exists()) return found;
            for (const QString& name : directory.entryList(
                     QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
                if (ioCancelled(cancel)) break;
                const QString path = directory.filePath(name);
                if (!QFileInfo::exists(
                        QDir(path).filePath(QStringLiteral("plugin.lua"))))
                    continue;
                found.append(describePlugin(path));
            }
            return found;
        },
        [this](QVector<PluginInfo> found) {
            QVector<PluginOffer> next;
            for (const PluginInfo& info : found) {
                PluginOffer offer;
                offer.plugin = info;
                offer.error = info.error;
                // Keep a previous offer for an unchanged plugin so the list
                // does not flicker to "no channels" during re-discovery.
                for (const PluginOffer& previous : offers_)
                    if (previous.plugin.id == info.id &&
                        previous.plugin.directory == info.directory)
                        offer = previous;
                next.append(offer);
            }
            offers_ = next;
            emit pluginsChanged();
            runChannelsForAll();
        });
}

void PluginHost::setSession(const PluginSession& session) {
    if (session == session_) return;
    session_ = session;
    runChannelsForAll();
}

void PluginHost::setEnabled(const QStringList& ids) {
    if (enabled_ == ids) return;
    enabled_ = ids;
    emit pluginsChanged();
}

void PluginHost::runChannelsForAll() {
    for (PluginOffer& offer : offers_) {
        if (!offer.plugin.error.isEmpty()) continue;
        if (!hasSession()) {
            offer.channels.clear();
            offer.loading = false;
            continue;
        }
        offer.loading = true;
        auto& job = channelJobs_[offer.plugin.id];
        if (!job) job = new AsyncJob<PluginOffer>(this);  // Qt-owned
        const PluginInfo plugin = offer.plugin;
        const PluginSession session = session_;
        const PluginPaths paths = paths_;
        job->start(
            [plugin, session, paths](IoCancel cancel) {
                return runPluginChannels(plugin, session, paths, cancel);
            },
            [this, session](PluginOffer result) {
                if (!(session == session_)) return;  // superseded
                for (PluginOffer& current : offers_) {
                    if (current.plugin.id != result.plugin.id) continue;
                    current.channels = result.channels;
                    current.error = result.error;
                    current.note = result.note;
                    current.loading = false;
                    if (!result.error.isEmpty())
                        qCWarning(lcPlugin).noquote()
                            << result.plugin.id
                            << "channels():" << result.error;
                    if (enabled_.contains(result.plugin.id) &&
                        !result.channels.isEmpty())
                        requestSamples(result.plugin.id);
                }
                emit pluginsChanged();
            });
    }
    emit pluginsChanged();
}

void PluginHost::requestSamples(const QString& pluginId) {
    const PluginOffer* offer = nullptr;
    for (const PluginOffer& candidate : offers_)
        if (candidate.plugin.id == pluginId) offer = &candidate;
    if (!offer || !hasSession() || offer->channels.isEmpty() ||
        !offer->plugin.error.isEmpty())
        return;
    QStringList keys;
    for (const PluginChannelOffer& channel : offer->channels)
        keys.append(channel.key);
    auto& job = sampleJobs_[pluginId];
    if (!job) job = new AsyncJob<PluginSamplesResult>(this);  // Qt-owned
    const PluginInfo plugin = offer->plugin;
    const PluginSession session = session_;
    const PluginPaths paths = paths_;
    job->start(
        [plugin, session, keys, paths](IoCancel cancel) {
            return runPluginSamples(plugin, session, keys, paths, cancel);
        },
        [this](PluginSamplesResult result) {
            if (!(result.session == session_)) return;  // superseded
            if (!result.error.isEmpty()) {
                qCWarning(lcPlugin).noquote()
                    << result.pluginId << "samples():" << result.error;
                emit operationError(
                    QStringLiteral("Plugin %1").arg(result.pluginId),
                    result.error);
            }
            emit samplesReady(result);
        });
}

QVariantList PluginHost::library() const {
    QVariantList rows;
    for (const PluginOffer& offer : offers_) {
        QString status;
        if (!offer.plugin.error.isEmpty())
            status = QStringLiteral("broken");
        else if (offer.loading)
            status = QStringLiteral("loading");
        else if (!offer.error.isEmpty())
            status = QStringLiteral("error");
        else if (!hasSession())
            status = QStringLiteral("no session");
        else if (offer.channels.isEmpty())
            status = offer.note.isEmpty()
                         ? QStringLiteral("nothing for this session")
                         : offer.note;
        else
            status = QStringLiteral("%1 channels").arg(offer.channels.size());
        rows.append(QVariantMap{
            {QStringLiteral("id"), offer.plugin.id},
            {QStringLiteral("name"), offer.plugin.name},
            {QStringLiteral("version"), offer.plugin.version},
            {QStringLiteral("directory"), offer.plugin.directory},
            {QStringLiteral("status"), status},
            {QStringLiteral("error"),
             offer.plugin.error.isEmpty() ? offer.error : offer.plugin.error},
            {QStringLiteral("channelCount"), offer.channels.size()},
            {QStringLiteral("enabled"), enabled_.contains(offer.plugin.id)},
            {QStringLiteral("loading"), offer.loading},
        });
    }
    return rows;
}

}  // namespace omatrack
