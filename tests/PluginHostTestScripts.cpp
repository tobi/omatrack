#include <QString>

QString luaScript(int index) {
    static const char* const scripts[] = {
        R"lua(
return {
  id = "demo", name = "Demo", version = 3,
  channels = function(session)
    kv.set("seen", session.track)
    kv.set("short", 1, 1)
    if session.utc_start_ns ~= 1700000000000000000 then error("utc lost precision") end
    if session.lap.id ~= 2 then error("lap missing") end
    return { { key = "a", name = "Alpha", unit = "u" }, { key = "b", default_visible = false } }
  end,
  samples = function(session, keys)
    local out = {}
    -- file-relative integer nanoseconds
    out.a = { t = { 0, 300000000000, 600000000000 }, v = { 1, 2, 3 } }
    -- absolute Unix nanoseconds, converted by the host; nil -> NaN
    out.b = { utc_ns = { session.utc_start_ns, session.utc_start_ns + 600000000000 }, v = { 10, nil } }
    out.seen = kv.get("seen")
    return out
  end,
}
)lua",
        R"lua(
return { id = "x", channels = function() return {{key="a"}} end,
  samples = function() return { a = { t = {5, 1}, v = {1, 2} } } end }
)lua",
        R"lua(
return { id = "jail",
  channels = function()
    if load ~= nil or require ~= nil or dofile ~= nil or os.execute ~= nil or os.remove ~= nil then error("unsafe global present") end
    if io.read("plugin.lua") == nil then error("cannot read own plugin.lua") end
    local escaped, err = io.read("../../../etc/hostname")
    if escaped ~= nil then error("escaped the jail") end
    if not io.write("state/notes.txt", "hello") then error("cannot write cache") end
    if io.read("state/notes.txt") ~= "hello" then error("cache round trip failed") end
    -- writing "into" the plugin folder lands in the cache folder, never beside the code
    io.write("plugin.lua", "overwritten?")
    return {}
  end,
  samples = function() return {} end }
)lua",
        R"lua(
return { id = "net",
  channels = function()
    local r = http.get("http://127.0.0.1:%1/v1?x=1", { headers = { ["X-Test"] = "1" } })
    if r.status ~= 200 then error("status " .. tostring(r.status) .. " " .. tostring(r.error)) end
    local body, err = json.decode(r.body)
    if not body then error(err) end
    if body.hourly.time[2] ~= 1700003600 then error("json ints lost") end
    if body.hourly.t[2] ~= nil then error("null should be nil") end
    if json.encode({a = 1, b = {1, 2}}) ~= '{"a":1,"b":[1,2]}' then error("encode: " .. json.encode({a = 1, b = {1, 2}})) end
    local f = http.get("ftp://example.invalid/x")
    if f.status ~= 0 then error("ftp allowed") end
    return { { key = "t", name = "Temp" } }
  end,
  samples = function() return {} end }
)lua",
    };
    return QString::fromUtf8(scripts[index]);
}
