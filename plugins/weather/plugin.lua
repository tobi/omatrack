-- Weather at the track during the open session, from Open-Meteo.
--
-- Install: copy this folder to ~/.config/omatrack/plugins/weather/ and enable
-- "Weather" in Channels… → PLUGINS. Needs a recording with GPS (for the
-- location) and a wall clock (utc_start_ns). Open-Meteo is model data on a
-- ~1–11 km grid, not a physical station; the nearest grid point is used.
-- Free, no API key: https://open-meteo.com/ (CC BY 4.0).

local DAY_S = 86400

local VARIABLES = {
  { key = "temperature_2m", name = "Air temperature", unit = "°C", default_visible = true },
  { key = "precipitation", name = "Precipitation", unit = "mm/h", default_visible = true },
  { key = "rain", name = "Rain", unit = "mm/h", default_visible = false },
  { key = "wind_speed_10m", name = "Wind speed", unit = "km/h", default_visible = false },
  { key = "relative_humidity_2m", name = "Humidity", unit = "%", default_visible = false },
  { key = "surface_pressure", name = "Pressure", unit = "hPa", default_visible = false },
}

local function usable(session)
  return session.utc_start_ns ~= nil and session.latitude ~= nil and session.longitude ~= nil
end

local function iso_date(unix_s)
  return os.date("!%Y-%m-%d", unix_s)
end

-- Hourly rows covering [start, end] with one hour of margin on each side so
-- the lap grid can always interpolate between two neighbours.
local function fetch(session)
  local start_s = math.floor(session.utc_start_ns / 1e9) - 3600
  local end_s = math.floor(session.utc_end_ns / 1e9) + 3600
  local lat = math.floor(session.latitude * 100 + 0.5) / 100
  local lon = math.floor(session.longitude * 100 + 0.5) / 100
  local cache_key = string.format("v1:%s:%s:%s:%s", lat, lon, iso_date(start_s), iso_date(end_s))
  local cached = kv.get(cache_key)
  if cached then return cached end

  local names = {}
  for _, v in ipairs(VARIABLES) do names[#names + 1] = v.key end
  local query = string.format(
    "latitude=%s&longitude=%s&start_date=%s&end_date=%s&hourly=%s&timezone=UTC&timeformat=unixtime",
    lat, lon, iso_date(start_s), iso_date(end_s), table.concat(names, ","))

  -- The archive lags a few days; recent sessions come from the forecast API,
  -- which serves the past three months.
  local age_days = (os.time() - end_s) / DAY_S
  local url
  if age_days > 7 then
    url = "https://archive-api.open-meteo.com/v1/archive?" .. query
  else
    url = "https://api.open-meteo.com/v1/forecast?" .. query
  end
  local response = http.get(url, { timeout_ms = 20000 })
  if response.status ~= 200 then
    error(string.format("Open-Meteo returned %d %s", response.status, response.error or ""))
  end
  local body, err = json.decode(response.body)
  if not body then error("Open-Meteo JSON: " .. tostring(err)) end
  if not body.hourly or not body.hourly.time then error("Open-Meteo answer has no hourly data") end

  local rows = { time = body.hourly.time }
  local any = false
  for _, v in ipairs(VARIABLES) do
    local series = body.hourly[v.key]
    if series then
      for i = 1, #series do
        if series[i] ~= nil then any = true break end
      end
      rows[v.key] = series
    end
  end
  if not any then error("Open-Meteo has no values for this window yet") end
  -- Past weather does not change; recent windows may still be revised.
  kv.set(cache_key, rows, age_days > 7 and 90 * DAY_S or DAY_S)
  return rows
end

return {
  id = "weather",
  name = "Weather",
  version = 1,

  channels = function(session)
    if session.utc_start_ns == nil then return {}, "recording has no wall clock" end
    if session.latitude == nil then return {}, "recording has no GPS position" end
    local offers = {}
    for _, v in ipairs(VARIABLES) do
      offers[#offers + 1] = { key = v.key, name = v.name, unit = v.unit, default_visible = v.default_visible }
    end
    return offers
  end,

  samples = function(session, keys)
    if not usable(session) then return {} end
    local rows = fetch(session)
    local utc_ns = {}
    for i = 1, #rows.time do utc_ns[i] = math.tointeger(rows.time[i]) * 1000000000 end
    local out = {}
    for _, key in ipairs(keys) do
      local series = rows[key]
      if series then
        local v = {}
        for i = 1, #utc_ns do
          local value = series[i]
          if value == nil then value = 0 / 0 end -- NaN: no reading
          v[i] = value
        end
        out[key] = { utc_ns = utc_ns, v = v }
      end
    end
    log(string.format("weather: %d hourly rows for %s", #utc_ns, session.track))
    return out
  end,
}
