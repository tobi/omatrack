#include "LuaRename.h"

#include <QElapsedTimer>
#include <QMetaType>
#include <QVariant>

#include <sol/sol.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace omatrack {
namespace {

constexpr const char* kAbortKey = "omatrack.abort";

struct AllocState {
    size_t used = 0;
    size_t cap = 0;
    bool overflow = false;
};

struct HookState {
    QElapsedTimer clock;
    int timeoutMs = 50;
    AllocState* alloc = nullptr;
    bool aborted = false;
    const char* reason = nullptr;
};

void abortLua(lua_State* L, HookState* hook, const char* reason) {
    hook->aborted = true;
    hook->reason = reason;
    lua_pushstring(L, reason);
    lua_error(L);
}

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
    if (hook->alloc && hook->alloc->overflow)
        abortLua(L, hook, "rename script exceeded memory cap");
    if (hook->clock.hasExpired(hook->timeoutMs))
        abortLua(L, hook, "rename script timed out");
}

void stripUnsafe(lua_State* L) {
    const char* names[] = {"load", "loadfile", "dofile", "setmetatable",
                           "loadstring"};
    for (const char* name : names) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_getglobal(L, "string");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
    }
    lua_pop(L, 1);
}

void pushCtx(lua_State* L, const QVariantMap& ctx) {
    // sol2 builds a plain table of strings/numbers — never Qt usertypes.
    // create_table can leave extras on the stack; pin the function slot
    // and push exactly one argument.
    const int fnTop = lua_gettop(L);
    sol::state_view lua(L);
    sol::table table = lua.create_table(0, ctx.size());
    for (auto it = ctx.cbegin(); it != ctx.cend(); ++it) {
        const std::string key = it.key().toStdString();
        const QVariant value = it.value();
        switch (value.typeId()) {
            case QMetaType::Int:
            case QMetaType::LongLong:
            case QMetaType::UInt:
            case QMetaType::ULongLong:
            case QMetaType::Double:
            case QMetaType::Float: table[key] = value.toDouble(); break;
            case QMetaType::Bool: table[key] = value.toBool(); break;
            default: table[key] = value.toString().toStdString(); break;
        }
    }
    lua_settop(L, fnTop);
    sol::stack::push(L, table);
}

QString failMessage(const AllocState& alloc, const HookState& hook,
                    lua_State* L) {
    if (alloc.overflow)
        return QStringLiteral("rename script exceeded memory cap");
    if (hook.reason) return QString::fromUtf8(hook.reason);
    if (const char* message = lua_tostring(L, -1))
        return QString::fromUtf8(message);
    return QStringLiteral("rename script failed");
}

}  // namespace

QString exampleLuaRenameScript() {
    return QStringLiteral(
        "-- rename(ctx) returns a destination-relative path.\n"
        "-- ctx.track / date / session / original / driver / car\n"
        "function rename(ctx)\n"
        "  return ctx.track .. \"/\" .. ctx.date .. \"/\" .. ctx.session"
        " .. \"/\" .. ctx.original\n"
        "end\n");
}

LuaRenameResult runLuaRename(const QString& script, const QVariantMap& ctx,
                             int timeoutMs, size_t memoryCapBytes) {
    LuaRenameResult result;
    const QString trimmed = script.trimmed();
    if (trimmed.isEmpty()) {
        result.ok = true;
        return result;
    }

    AllocState alloc;
    alloc.cap = memoryCapBytes;
    lua_State* L = lua_newstate(cappedAlloc, &alloc);
    if (!L) {
        result.error = QStringLiteral("rename script exceeded memory cap");
        return result;
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
    stripUnsafe(L);

    HookState hook;
    hook.timeoutMs = std::max(1, timeoutMs);
    hook.alloc = &alloc;
    hook.clock.start();
    lua_pushstring(L, kAbortKey);
    lua_pushlightuserdata(L, &hook);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_sethook(L, countHook, LUA_MASKCOUNT, 1000);

    const std::string source = trimmed.toStdString();
    if (luaL_loadbuffer(L, source.data(), source.size(), "=rename") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        result.error = failMessage(alloc, hook, L);
        lua_close(L);
        return result;
    }

    lua_getglobal(L, "rename");
    if (!lua_isfunction(L, -1)) {
        result.error = QStringLiteral("rename script must define rename(ctx)");
        lua_close(L);
        return result;
    }
    pushCtx(L, ctx);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        result.error = failMessage(alloc, hook, L);
        lua_close(L);
        return result;
    }
    if (lua_isnil(L, -1)) {
        result.error = QStringLiteral("Rename produced a null path");
        lua_close(L);
        return result;
    }
    if (!lua_isstring(L, -1)) {
        result.error = QStringLiteral("rename(ctx) must return a string");
        lua_close(L);
        return result;
    }
    size_t length = 0;
    const char* relative = lua_tolstring(L, -1, &length);
    if (!relative) {
        result.error = QStringLiteral("rename(ctx) must return a string");
        lua_close(L);
        return result;
    }
    std::string path(relative, length);
    if (path.find('\0') != std::string::npos) {
        result.error = QStringLiteral("Rename produced a null byte");
        lua_close(L);
        return result;
    }
    result.relativePath = QString::fromStdString(path);
    result.ok = true;
    lua_close(L);
    return result;
}

}  // namespace omatrack
