#include "system/weather_service.h"

#include "config/config_service.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "time/time_format.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

  constexpr Logger kLog("weather");
  constexpr std::size_t kForecastDays = 7;
  constexpr std::size_t kForecastHours = kForecastDays * 24;

  using Clock = std::chrono::system_clock;

  std::chrono::system_clock::time_point fromUnixSeconds(std::int64_t value) {
    return Clock::time_point{std::chrono::seconds{value}};
  }

  std::int64_t toUnixSeconds(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
  }

  bool isIsoDate(std::string_view text) {
    return text.size() == 10
        && std::isdigit(static_cast<unsigned char>(text[0])) != 0
        && std::isdigit(static_cast<unsigned char>(text[1])) != 0
        && std::isdigit(static_cast<unsigned char>(text[2])) != 0
        && std::isdigit(static_cast<unsigned char>(text[3])) != 0
        && text[4] == '-'
        && std::isdigit(static_cast<unsigned char>(text[5])) != 0
        && std::isdigit(static_cast<unsigned char>(text[6])) != 0
        && text[7] == '-'
        && std::isdigit(static_cast<unsigned char>(text[8])) != 0
        && std::isdigit(static_cast<unsigned char>(text[9])) != 0;
  }

  bool isIsoHour(std::string_view text) {
    return text.size() == 16
        && isIsoDate(text.substr(0, 10))
        && text[10] == 'T'
        && std::isdigit(static_cast<unsigned char>(text[11])) != 0
        && std::isdigit(static_cast<unsigned char>(text[12])) != 0
        && text[13] == ':'
        && std::isdigit(static_cast<unsigned char>(text[14])) != 0
        && std::isdigit(static_cast<unsigned char>(text[15])) != 0;
  }

  std::string todayIsoForOffset(std::int32_t utcOffsetSeconds) {
    const auto shiftedNow = Clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = Clock::to_time_t(shiftedNow);
    std::tm tm{};
    gmtime_r(&time, &tm);

    return formatStrftime("%Y-%m-%d", tm);
  }

  std::string currentHourIsoForOffset(std::int32_t utcOffsetSeconds) {
    const auto shiftedNow = Clock::now() + std::chrono::seconds{utcOffsetSeconds};
    const std::time_t time = Clock::to_time_t(shiftedNow);
    std::tm tm{};
    gmtime_r(&time, &tm);

    return formatStrftime("%Y-%m-%dT%H:00", tm);
  }

  bool dropPastForecastDays(WeatherSnapshot& snapshot) {
    if (!snapshot.valid || snapshot.forecastDays.empty()) {
      return false;
    }

    const std::string todayIso = todayIsoForOffset(snapshot.utcOffsetSeconds);
    if (!isIsoDate(todayIso)) {
      return false;
    }

    const auto oldSize = snapshot.forecastDays.size();
    std::erase_if(snapshot.forecastDays, [&todayIso](const WeatherForecastDay& day) {
      return isIsoDate(day.dateIso) && day.dateIso < todayIso;
    });
    return snapshot.forecastDays.size() != oldSize;
  }

  bool dropPastForecastHours(WeatherSnapshot& snapshot) {
    if (!snapshot.valid || snapshot.forecastHours.empty()) {
      return false;
    }

    const std::string currentHourIso = currentHourIsoForOffset(snapshot.utcOffsetSeconds);
    if (!isIsoHour(currentHourIso)) {
      return false;
    }

    const auto oldSize = snapshot.forecastHours.size();
    std::erase_if(snapshot.forecastHours, [&currentHourIso](const WeatherForecastHour& hour) {
      return isIsoHour(hour.timeIso) && hour.timeIso < currentHourIso;
    });
    return snapshot.forecastHours.size() != oldSize;
  }

  double readNumber(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
      throw std::runtime_error(std::string("missing numeric key: ") + key);
    }
    return it->get<double>();
  }

  double readOptionalNumber(const nlohmann::json& json, const char* key, double fallback = 0.0) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number()) {
      return fallback;
    }
    return it->get<double>();
  }

  std::int32_t readOptionalInt(const nlohmann::json& json, const char* key, std::int32_t fallback = 0) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_number_integer()) {
      return fallback;
    }
    return it->get<std::int32_t>();
  }

  std::string readString(const nlohmann::json& json, const char* key) {
    const auto it = json.find(key);
    if (it == json.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  }

  double readTextNumber(const nlohmann::json& json, const char* key, double fallback = 0.0) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
      return fallback;
    }
    if (it->is_number()) {
      return it->get<double>();
    }
    if (!it->is_string()) {
      return fallback;
    }
    try {
      return std::stod(it->get<std::string>());
    } catch (...) {
      return fallback;
    }
  }

  std::int32_t readTextInt(const nlohmann::json& json, const char* key, std::int32_t fallback = 0) {
    return static_cast<std::int32_t>(std::lround(readTextNumber(json, key, fallback)));
  }

  std::string trim(std::string value) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
  }

  std::string normalizeQWeatherUrl(std::string value) {
    value = trim(std::move(value));
    if (value.empty()) {
      return {};
    }
    if (!value.starts_with("https://")) {
      if (value.find("://") != std::string::npos) {
        return {};
      }
      value.insert(0, "https://");
    }
    while (value.size() > std::string_view("https://").size() && value.ends_with('/')) {
      value.pop_back();
    }
    const std::string_view authority(
        value.data() + std::string_view("https://").size(), value.size() - std::string_view("https://").size()
    );
    if (authority.empty() || authority.find_first_of("/?#@\r\n") != std::string_view::npos) {
      return {};
    }
    return value;
  }

  std::string localIsoMinute(std::string_view iso) {
    return iso.size() >= 16 ? std::string(iso.substr(0, 16)) : std::string(iso);
  }

  std::int32_t utcOffsetFromIso(std::string_view iso) {
    if (iso.ends_with('Z')) {
      return 0;
    }
    if (iso.size() < 6) {
      return 0;
    }
    const std::string_view offset = iso.substr(iso.size() - 6);
    if ((offset[0] != '+' && offset[0] != '-') || offset[3] != ':') {
      return 0;
    }
    try {
      const int hours = std::stoi(std::string(offset.substr(1, 2)));
      const int minutes = std::stoi(std::string(offset.substr(4, 2)));
      const int seconds = hours * 3600 + minutes * 60;
      return static_cast<std::int32_t>(offset[0] == '-' ? -seconds : seconds);
    } catch (...) {
      return 0;
    }
  }

  std::string utcOffsetLabel(std::int32_t offsetSeconds) {
    const char sign = offsetSeconds < 0 ? '-' : '+';
    const int absolute = std::abs(offsetSeconds);
    return std::format("UTC{}{:02}:{:02}", sign, absolute / 3600, (absolute % 3600) / 60);
  }

  std::int32_t wmoCodeForQWeatherIcon(std::int32_t icon) {
    if (icon == 100 || icon == 150)
      return 0;
    if (icon == 101 || icon == 102 || icon == 151 || icon == 152)
      return 1;
    if (icon == 103 || icon == 153)
      return 2;
    if (icon == 104)
      return 3;
    if (icon >= 200 && icon <= 213)
      return 2;
    if (icon >= 300 && icon <= 304)
      return 95;
    if (icon == 305 || icon == 309 || icon == 314)
      return 51;
    if (icon == 306 || icon == 315)
      return 61;
    if ((icon >= 307 && icon <= 318) || icon == 399)
      return 65;
    if (icon == 400 || icon == 408)
      return 71;
    if (icon == 401 || icon == 409)
      return 73;
    if (icon == 402 || icon == 403 || icon == 410 || icon == 499)
      return 75;
    if (icon >= 404 && icon <= 407)
      return 85;
    if (icon >= 500 && icon <= 515)
      return 45;
    return 3;
  }

  bool qWeatherIconIsDay(std::int32_t icon, std::string_view localTime) {
    if (icon >= 150 && icon <= 199) {
      return false;
    }
    if (icon >= 100 && icon <= 149) {
      return true;
    }
    if (localTime.size() >= 13) {
      try {
        const int hour = std::stoi(std::string(localTime.substr(11, 2)));
        return hour >= 6 && hour < 18;
      } catch (...) {
      }
    }
    return true;
  }

  std::optional<std::string> qWeatherResponseError(const HttpResponse& response) {
    if (!response.transportOk) {
      return i18n::tr("weather.errors.fetch-failed");
    }
    if (response.status == 401 || response.status == 403) {
      return i18n::tr("weather.errors.authentication-failed");
    }
    if (response.status < 200 || response.status >= 300) {
      return i18n::tr("weather.errors.api-failed");
    }
    try {
      const auto json = nlohmann::json::parse(response.body);
      const std::string code = readString(json, "code");
      if (code == "200") {
        return std::nullopt;
      }
      if (code == "401" || code == "402" || code == "403") {
        return i18n::tr("weather.errors.authentication-failed");
      }
      return i18n::tr("weather.errors.api-failed");
    } catch (...) {
      return i18n::tr("weather.errors.parse-weather");
    }
  }

  bool readBool(const nlohmann::json& json, const char* key, bool fallback = false) {
    const auto it = json.find(key);
    if (it == json.end()) {
      return fallback;
    }
    if (it->is_boolean()) {
      return it->get<bool>();
    }
    if (it->is_number_integer()) {
      return it->get<int>() != 0;
    }
    return fallback;
  }

  nlohmann::json currentUnitsToJson(const WeatherCurrentUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"interval", units.interval},
        {"temperature", units.temperature},
        {"wind_speed", units.windSpeed},
        {"wind_direction", units.windDirection},
        {"is_day", units.isDay},
        {"weather_code", units.weatherCode},
        {"uv_index", units.uvIndex},
    };
  }

  nlohmann::json dailyUnitsToJson(const WeatherDailyUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"temperature_max", units.temperatureMax},
        {"temperature_min", units.temperatureMin},
        {"weather_code", units.weatherCode},
        {"sunrise", units.sunrise},
        {"sunset", units.sunset},
    };
  }

  nlohmann::json hourlyUnitsToJson(const WeatherHourlyUnits& units) {
    return nlohmann::json{
        {"time", units.time},
        {"temperature", units.temperature},
        {"relative_humidity", units.relativeHumidity},
        {"precipitation_probability", units.precipitationProbability},
        {"weather_code", units.weatherCode},
        {"is_day", units.isDay},
        {"wind_speed", units.windSpeed},
    };
  }

  WeatherCurrentUnits currentUnitsFromJson(const nlohmann::json& json) {
    WeatherCurrentUnits units;
    units.time = readString(json, "time");
    units.interval = readString(json, "interval");
    units.temperature = readString(json, "temperature");
    units.windSpeed = readString(json, "wind_speed");
    units.windDirection = readString(json, "wind_direction");
    units.isDay = readString(json, "is_day");
    units.weatherCode = readString(json, "weather_code");
    units.uvIndex = readString(json, "uv_index");
    return units;
  }

  WeatherDailyUnits dailyUnitsFromJson(const nlohmann::json& json) {
    WeatherDailyUnits units;
    units.time = readString(json, "time");
    units.temperatureMax = readString(json, "temperature_max");
    units.temperatureMin = readString(json, "temperature_min");
    units.weatherCode = readString(json, "weather_code");
    units.sunrise = readString(json, "sunrise");
    units.sunset = readString(json, "sunset");
    return units;
  }

  WeatherHourlyUnits hourlyUnitsFromJson(const nlohmann::json& json) {
    WeatherHourlyUnits units;
    units.time = readString(json, "time");
    units.temperature = readString(json, "temperature");
    units.relativeHumidity = readString(json, "relative_humidity");
    units.precipitationProbability = readString(json, "precipitation_probability");
    units.weatherCode = readString(json, "weather_code");
    units.isDay = readString(json, "is_day");
    units.windSpeed = readString(json, "wind_speed");
    return units;
  }

} // namespace

WeatherService::WeatherService(ConfigService& configService, HttpClient& httpClient)
    : m_configService(configService), m_httpClient(httpClient) {}

void WeatherService::initialize() {
  m_activeConfig = m_configService.config().weather;
  m_activeAddress = m_configService.config().location.address;
  m_configService.addReloadCallback([this]() { onConfigReload(); });
  loadCache();
  requestRefresh();
}

void WeatherService::addChangeCallback(ChangeCallback callback) { m_callbacks.push_back(std::move(callback)); }

void WeatherService::setLocation(
    std::optional<WeatherCoordinates> coordinates, std::string name, std::string sourceLabel
) {
  m_locationName = std::move(name);
  m_locationSource = std::move(sourceLabel);

  if (!coordinates.has_value()) {
    if (m_hasLocation || m_snapshot.valid || !m_error.empty()) {
      m_hasLocation = false;
      clearState();
      notifyChanged();
    }
    return;
  }

  constexpr double kEpsilon = 1e-6;
  const bool coordinatesChanged = !m_hasLocation
      || std::abs(m_resolvedLatitude - coordinates->latitude) > kEpsilon
      || std::abs(m_resolvedLongitude - coordinates->longitude) > kEpsilon;

  m_resolvedLatitude = coordinates->latitude;
  m_resolvedLongitude = coordinates->longitude;
  m_hasLocation = true;

  if (coordinatesChanged) {
    requestRefresh();
  }
  notifyChanged();
}

int WeatherService::pollTimeoutMs() const {
  if (!m_activeConfig.enabled || m_requestKind != RequestKind::None) {
    return -1;
  }
  if (m_refreshQueued) {
    return 0;
  }
  if (!m_hasLocation) {
    return -1;
  }

  const auto now = Clock::now();
  if (m_nextRefreshAt <= now) {
    return 0;
  }
  return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextRefreshAt - now).count());
}

void WeatherService::tick() {
  if (m_requestKind != RequestKind::None) {
    return;
  }
  if (!m_activeConfig.enabled) {
    return;
  }
  if (!m_hasLocation) {
    m_refreshQueued = false;
    if (m_snapshot.valid || !m_error.empty()) {
      clearState();
      notifyChanged();
    }
    return;
  }

  const auto now = Clock::now();
  if (!m_refreshQueued && now < m_nextRefreshAt) {
    return;
  }
  m_refreshQueued = false;

  startWeatherFetch();
}

void WeatherService::requestRefresh() {
  m_refreshQueued = true;
  m_nextRefreshAt = Clock::time_point{};
}

bool WeatherService::enabled() const noexcept { return m_activeConfig.enabled; }

bool WeatherService::locationConfigured() const noexcept { return m_hasLocation; }

bool WeatherService::useImperial() const noexcept { return m_activeConfig.unit == "imperial"; }

double WeatherService::displayTemperature(double celsius) const noexcept {
  if (!useImperial()) {
    return celsius;
  }
  return 32.0 + (celsius * 1.8);
}

const char* WeatherService::displayTemperatureUnit() const noexcept { return useImperial() ? "\u00b0F" : "\u00b0C"; }

std::string WeatherService::glyphForCode(std::int32_t code, bool isDay) {
  if (code == 0) {
    return isDay ? "weather-sun" : "weather-moon";
  }
  if (code == 1 || code == 2) {
    return isDay ? "weather-cloud-sun" : "weather-moon-stars";
  }
  if (code == 3) {
    return "weather-cloud";
  }
  if (code >= 45 && code <= 48) {
    return "weather-cloud-haze";
  }
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return "weather-cloud-rain";
  }
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) {
    return "weather-cloud-snow";
  }
  if (code >= 95 && code <= 99) {
    return "weather-cloud-lightning";
  }
  return "weather-cloud";
}

std::string WeatherService::shortDescriptionForCode(std::int32_t code) {
  if (code == 0) {
    return i18n::tr("weather.conditions.short.clear");
  }
  if (code == 1) {
    return i18n::tr("weather.conditions.short.mostly-clear");
  }
  if (code == 2) {
    return i18n::tr("weather.conditions.short.cloudy");
  }
  if (code == 3) {
    return i18n::tr("weather.conditions.short.overcast");
  }
  if (code == 45 || code == 48) {
    return i18n::tr("weather.conditions.short.fog");
  }
  if (code >= 51 && code <= 67) {
    return i18n::tr("weather.conditions.short.drizzle");
  }
  if (code >= 71 && code <= 77) {
    return i18n::tr("weather.conditions.short.snow");
  }
  if (code >= 80 && code <= 82) {
    return i18n::tr("weather.conditions.short.showers");
  }
  if (code >= 85 && code <= 86) {
    return i18n::tr("weather.conditions.short.snow-showers");
  }
  if (code >= 95 && code <= 99) {
    return i18n::tr("weather.conditions.short.storm");
  }
  return i18n::tr("weather.conditions.short.weather");
}

std::string WeatherService::descriptionForCode(std::int32_t code) {
  if (code == 0) {
    return i18n::tr("weather.conditions.full.clear-sky");
  }
  if (code == 1) {
    return i18n::tr("weather.conditions.full.mainly-clear");
  }
  if (code == 2) {
    return i18n::tr("weather.conditions.full.partly-cloudy");
  }
  if (code == 3) {
    return i18n::tr("weather.conditions.full.overcast");
  }
  if (code == 45 || code == 48) {
    return i18n::tr("weather.conditions.full.fog");
  }
  if (code >= 51 && code <= 67) {
    return i18n::tr("weather.conditions.full.drizzle");
  }
  if (code >= 71 && code <= 77) {
    return i18n::tr("weather.conditions.full.snow");
  }
  if (code >= 80 && code <= 82) {
    return i18n::tr("weather.conditions.full.rain-showers");
  }
  if (code >= 85 && code <= 86) {
    return i18n::tr("weather.conditions.full.snow-showers");
  }
  if (code >= 95 && code <= 99) {
    return i18n::tr("weather.conditions.full.thunderstorm");
  }
  return i18n::tr("weather.conditions.full.unknown");
}

void WeatherService::onConfigReload() {
  const WeatherConfig previousConfig = m_activeConfig;
  const WeatherConfig nextConfig = m_configService.config().weather;
  const std::string nextAddress = m_configService.config().location.address;
  const bool addressChanged = m_activeAddress != nextAddress;
  if (previousConfig == nextConfig && !addressChanged) {
    return;
  }

  m_activeConfig = nextConfig;
  m_activeAddress = nextAddress;
  m_error.clear();
  const bool credentialsChanged = previousConfig.url != nextConfig.url || previousConfig.key != nextConfig.key;
  if (!m_activeConfig.enabled) {
    clearState();
    notifyChanged();
    return;
  }

  if (credentialsChanged) {
    // Invalidate callbacks that may still carry the previous credential and do not
    // display cached data from a different QWeather project while reconnecting.
    clearState();
    requestRefresh();
  } else if (!previousConfig.enabled) {
    // Re-enabled: reload cache and refresh for the current location.
    loadCache();
    requestRefresh();
  } else if (addressChanged) {
    requestRefresh();
  } else if (m_snapshot.valid) {
    m_nextRefreshAt = m_snapshot.fetchedAt + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
  } else {
    requestRefresh();
  }
  notifyChanged();
}

void WeatherService::clearState() {
  ++m_requestSerial;
  m_loading = false;
  m_error.clear();
  m_requestKind = RequestKind::None;
  m_snapshot = WeatherSnapshot{};
  m_nextRefreshAt = Clock::time_point{};
}

void WeatherService::notifyChanged() {
  for (const auto& callback : m_callbacks) {
    callback();
  }
}

void WeatherService::startWeatherFetch() {
  const std::string baseUrl = normalizeQWeatherUrl(m_activeConfig.url);
  const std::string apiKey = trim(m_activeConfig.key);
  if (m_activeConfig.url.empty() || apiKey.empty()) {
    failWeatherRequest(i18n::tr("weather.errors.missing-credentials"), m_requestSerial);
    return;
  }
  if (baseUrl.empty() || apiKey.find_first_of("\r\n") != std::string::npos) {
    failWeatherRequest(i18n::tr("weather.errors.invalid-credentials"), m_requestSerial);
    return;
  }

  const std::uint64_t serial = ++m_requestSerial;
  m_loading = true;
  m_error.clear();
  m_requestKind = RequestKind::FetchWeather;
  notifyChanged();

  const std::string address = trim(m_configService.config().location.address);
  if (address.empty()) {
    startWeatherDataFetch(
        baseUrl, apiKey,
        std::format("{},{}", formatCoordinate(m_resolvedLongitude), formatCoordinate(m_resolvedLatitude)),
        m_locationName, m_resolvedLatitude, m_resolvedLongitude, serial
    );
    return;
  }

  HttpRequest lookupRequest{
      .url = baseUrl + "/geo/v2/city/lookup?location=" + StringUtils::urlEncode(address) + "&number=1&lang=zh",
      .headers = {"Accept: application/json", "X-QW-Api-Key: " + apiKey},
  };
  m_httpClient.request(std::move(lookupRequest), [this, baseUrl, apiKey, serial](HttpResponse response) mutable {
    if (serial != m_requestSerial || !m_activeConfig.enabled) {
      return;
    }
    if (auto error = qWeatherResponseError(response)) {
      failWeatherRequest(std::move(*error), serial);
      return;
    }

    try {
      const auto json = nlohmann::json::parse(response.body);
      const auto& locations = json.at("location");
      if (!locations.is_array() || locations.empty() || !locations.front().is_object()) {
        throw std::runtime_error("QWeather location not found");
      }
      const auto& place = locations.front();
      const std::string locationId = readString(place, "id");
      if (locationId.empty()) {
        throw std::runtime_error("QWeather location is missing an id");
      }

      std::string displayName = readString(place, "name");
      const std::string adm2 = readString(place, "adm2");
      const std::string adm1 = readString(place, "adm1");
      if (!adm2.empty() && adm2 != displayName) {
        displayName += ", " + adm2;
      }
      if (!adm1.empty() && adm1 != displayName && adm1 != adm2) {
        displayName += ", " + adm1;
      }

      startWeatherDataFetch(
          baseUrl, apiKey, locationId, std::move(displayName), readTextNumber(place, "lat", m_resolvedLatitude),
          readTextNumber(place, "lon", m_resolvedLongitude), serial
      );
    } catch (const std::exception& e) {
      kLog.warn("QWeather location lookup failed: {}", e.what());
      failWeatherRequest(i18n::tr("weather.errors.location-not-found"), serial);
    }
  });
}

void WeatherService::startWeatherDataFetch(
    std::string baseUrl, std::string apiKey, std::string location, std::string displayName, double latitude,
    double longitude, std::uint64_t serial
) {
  const std::string query = "?location=" + StringUtils::urlEncode(location) + "&unit=m&lang=zh";
  HttpRequest currentRequest{
      .url = baseUrl + "/v7/weather/now" + query,
      .headers = {"Accept: application/json", "X-QW-Api-Key: " + apiKey},
  };
  m_httpClient.request(
      std::move(currentRequest),
      [this, baseUrl, query, apiKey, displayName = std::move(displayName), latitude, longitude,
       serial](HttpResponse current) mutable {
        if (serial != m_requestSerial || !m_activeConfig.enabled) {
          return;
        }
        if (auto error = qWeatherResponseError(current)) {
          failWeatherRequest(std::move(*error), serial);
          return;
        }

        HttpRequest dailyRequest{
            .url = baseUrl + "/v7/weather/7d" + query,
            .headers = {"Accept: application/json", "X-QW-Api-Key: " + apiKey},
        };
        m_httpClient.request(
            std::move(dailyRequest),
            [this, baseUrl, query, apiKey, displayName = std::move(displayName), latitude, longitude, serial,
             currentBody = std::move(current.body)](HttpResponse daily) mutable {
              if (serial != m_requestSerial || !m_activeConfig.enabled) {
                return;
              }
              if (auto error = qWeatherResponseError(daily)) {
                failWeatherRequest(std::move(*error), serial);
                return;
              }

              HttpRequest hourlyRequest{
                  .url = baseUrl + "/v7/weather/24h" + query,
                  .headers = {"Accept: application/json", "X-QW-Api-Key: " + apiKey},
              };
              m_httpClient.request(
                  std::move(hourlyRequest),
                  [this, displayName = std::move(displayName), latitude, longitude, serial,
                   currentBody = std::move(currentBody),
                   dailyBody = std::move(daily.body)](HttpResponse hourly) mutable {
                    if (serial != m_requestSerial || !m_activeConfig.enabled) {
                      return;
                    }
                    if (auto error = qWeatherResponseError(hourly)) {
                      failWeatherRequest(std::move(*error), serial);
                      return;
                    }
                    handleWeatherResponses(
                        std::move(currentBody), std::move(dailyBody), std::move(hourly.body), std::move(displayName),
                        latitude, longitude, serial
                    );
                  }
              );
            }
        );
      }
  );
}

void WeatherService::handleWeatherResponses(
    std::string currentBody, std::string dailyBody, std::string hourlyBody, std::string displayName, double latitude,
    double longitude, std::uint64_t serial
) {
  if (serial != m_requestSerial || !m_activeConfig.enabled) {
    return;
  }
  m_requestKind = RequestKind::None;
  m_loading = false;

  try {
    const auto currentJson = nlohmann::json::parse(currentBody);
    const auto dailyJson = nlohmann::json::parse(dailyBody);
    const auto hourlyJson = nlohmann::json::parse(hourlyBody);
    const auto& current = currentJson.at("now");
    const auto& daily = dailyJson.at("daily");
    const auto& hourly = hourlyJson.at("hourly");
    if (!current.is_object() || !daily.is_array() || !hourly.is_array()) {
      throw std::runtime_error("unexpected QWeather response shape");
    }

    WeatherSnapshot next;
    next.valid = true;
    next.locationName = std::move(displayName);
    next.sourceLabel = m_locationSource;
    next.latitude = latitude;
    next.longitude = longitude;
    const std::string observationTime = readString(current, "obsTime");
    next.utcOffsetSeconds = utcOffsetFromIso(observationTime);
    next.timezoneAbbreviation = utcOffsetLabel(next.utcOffsetSeconds);
    next.currentUnits.temperature = "°C";
    next.currentUnits.windSpeed = "km/h";
    next.currentUnits.windDirection = "°";
    next.dailyUnits.temperatureMax = "°C";
    next.dailyUnits.temperatureMin = "°C";
    next.hourlyUnits.temperature = "°C";
    next.hourlyUnits.relativeHumidity = "%";
    next.hourlyUnits.precipitationProbability = "%";
    next.hourlyUnits.windSpeed = "km/h";
    next.current.timeIso = localIsoMinute(observationTime);
    next.current.conditionText = readString(current, "text");
    next.current.intervalSeconds = 3600;
    next.current.temperatureC = readTextNumber(current, "temp");
    next.current.windSpeedKmh = readTextNumber(current, "windSpeed");
    next.current.windDirectionDeg = readTextInt(current, "wind360");
    const std::int32_t currentIcon = readTextInt(current, "icon", 999);
    next.current.isDay = qWeatherIconIsDay(currentIcon, next.current.timeIso);
    next.current.weatherCode = wmoCodeForQWeatherIcon(currentIcon);
    next.fetchedAt = Clock::now();

    next.forecastDays.reserve(std::min(daily.size(), kForecastDays));
    for (const auto& day : daily) {
      if (!day.is_object() || next.forecastDays.size() >= kForecastDays) {
        continue;
      }
      const std::string date = readString(day, "fxDate");
      const std::string sunrise = readString(day, "sunrise");
      const std::string sunset = readString(day, "sunset");
      next.forecastDays.push_back(
          WeatherForecastDay{
              .dateIso = date,
              .conditionText = readString(day, "textDay"),
              .weatherCode = wmoCodeForQWeatherIcon(readTextInt(day, "iconDay", 999)),
              .temperatureMaxC = readTextNumber(day, "tempMax"),
              .temperatureMinC = readTextNumber(day, "tempMin"),
              .sunriseIso = sunrise.empty() ? std::string{} : date + "T" + sunrise,
              .sunsetIso = sunset.empty() ? std::string{} : date + "T" + sunset,
          }
      );
    }
    dropPastForecastDays(next);
    if (!daily.empty() && daily.front().is_object()) {
      next.current.uvIndex = readTextNumber(daily.front(), "uvIndex");
    }

    next.forecastHours.reserve(std::min(hourly.size(), kForecastHours));
    for (const auto& hour : hourly) {
      if (!hour.is_object() || next.forecastHours.size() >= kForecastHours) {
        continue;
      }
      const std::string time = localIsoMinute(readString(hour, "fxTime"));
      const std::int32_t icon = readTextInt(hour, "icon", 999);
      next.forecastHours.push_back(
          WeatherForecastHour{
              .timeIso = time,
              .conditionText = readString(hour, "text"),
              .weatherCode = wmoCodeForQWeatherIcon(icon),
              .temperatureC = readTextNumber(hour, "temp"),
              .relativeHumidityPercent = readTextInt(hour, "humidity"),
              .precipitationProbabilityPercent = readTextInt(hour, "pop"),
              .isDay = qWeatherIconIsDay(icon, time),
              .windSpeedKmh = readTextNumber(hour, "windSpeed"),
          }
      );
    }
    dropPastForecastHours(next);

    m_snapshot = std::move(next);
    m_error.clear();
    m_nextRefreshAt = Clock::now() + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
    saveCache();
    notifyChanged();
  } catch (const std::exception& e) {
    m_error = i18n::tr("weather.errors.parse-weather");
    scheduleRetryAfterFailure();
    kLog.warn("{}: {}", m_error, e.what());
    notifyChanged();
  }
}

void WeatherService::failWeatherRequest(std::string error, std::uint64_t serial) {
  if (serial != m_requestSerial || !m_activeConfig.enabled) {
    return;
  }
  m_requestKind = RequestKind::None;
  m_loading = false;
  m_error = std::move(error);
  scheduleRetryAfterFailure();
  dropPastForecastHours(m_snapshot);
  dropPastForecastDays(m_snapshot);
  notifyChanged();
}

void WeatherService::scheduleRetryAfterFailure() {
  m_refreshQueued = false;
  m_nextRefreshAt = Clock::now() + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));
}

bool WeatherService::coordinatesValid() const noexcept {
  return std::isfinite(m_resolvedLatitude)
      && std::isfinite(m_resolvedLongitude)
      && m_resolvedLatitude >= -90.0
      && m_resolvedLatitude <= 90.0
      && m_resolvedLongitude >= -180.0
      && m_resolvedLongitude <= 180.0;
}

std::filesystem::path WeatherService::stateCacheFilePath() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && xdg[0] != '\0') {
    return std::filesystem::path(xdg) / "noctalia" / "weather.json";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
    return std::filesystem::path(home) / ".cache" / "noctalia" / "weather.json";
  }
  return std::filesystem::path("/tmp") / "noctalia-weather-cache.json";
}

std::string WeatherService::formatCoordinate(double value) { return std::format("{:.2f}", value); }

void WeatherService::loadCache() {
  clearState();
  const auto path = stateCacheFilePath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return;
  }

  try {
    std::ifstream file(path);
    const auto json = nlohmann::json::parse(file);
    if (readString(json, "provider") != "qweather") {
      return;
    }

    const auto& snapshot = json.at("snapshot");
    if (!readBool(snapshot, "valid")) {
      return;
    }

    m_snapshot.valid = true;
    m_snapshot.locationName = readString(snapshot, "location_name");
    m_snapshot.sourceLabel = readString(snapshot, "source_label");
    m_snapshot.latitude = readNumber(snapshot, "latitude");
    m_snapshot.longitude = readNumber(snapshot, "longitude");
    m_snapshot.generationTimeMs = readOptionalNumber(snapshot, "generation_time_ms");
    m_snapshot.utcOffsetSeconds = readOptionalInt(snapshot, "utc_offset_seconds");
    m_snapshot.timezone = readString(snapshot, "timezone");
    m_snapshot.timezoneAbbreviation = readString(snapshot, "timezone_abbreviation");
    m_snapshot.elevationM = readOptionalNumber(snapshot, "elevation_m");
    if (const auto it = snapshot.find("current_units"); it != snapshot.end() && it->is_object()) {
      m_snapshot.currentUnits = currentUnitsFromJson(*it);
    }
    if (const auto it = snapshot.find("daily_units"); it != snapshot.end() && it->is_object()) {
      m_snapshot.dailyUnits = dailyUnitsFromJson(*it);
    }
    if (const auto it = snapshot.find("hourly_units"); it != snapshot.end() && it->is_object()) {
      m_snapshot.hourlyUnits = hourlyUnitsFromJson(*it);
    }
    if (const auto it = snapshot.find("current"); it != snapshot.end() && it->is_object()) {
      m_snapshot.current.timeIso = readString(*it, "time_iso");
      m_snapshot.current.conditionText = readString(*it, "condition_text");
      m_snapshot.current.intervalSeconds = readOptionalInt(*it, "interval_seconds");
      m_snapshot.current.temperatureC = readOptionalNumber(*it, "temperature_c");
      m_snapshot.current.windSpeedKmh = readOptionalNumber(*it, "wind_speed_kmh");
      m_snapshot.current.windDirectionDeg = readOptionalInt(*it, "wind_direction_deg");
      m_snapshot.current.isDay = readBool(*it, "is_day", true);
      m_snapshot.current.weatherCode = readOptionalInt(*it, "weather_code");
      m_snapshot.current.uvIndex = readOptionalNumber(*it, "uv_index");
    }
    if (const auto it = snapshot.find("forecast_hours"); it != snapshot.end() && it->is_array()) {
      m_snapshot.forecastHours.clear();
      for (const auto& item : *it) {
        if (!item.is_object()) {
          continue;
        }
        m_snapshot.forecastHours.push_back(
            WeatherForecastHour{
                .timeIso = readString(item, "time_iso"),
                .conditionText = readString(item, "condition_text"),
                .weatherCode = readOptionalInt(item, "weather_code"),
                .temperatureC = readOptionalNumber(item, "temperature_c"),
                .relativeHumidityPercent = readOptionalInt(item, "relative_humidity_percent"),
                .precipitationProbabilityPercent = readOptionalInt(item, "precipitation_probability_percent"),
                .isDay = readBool(item, "is_day", true),
                .windSpeedKmh = readOptionalNumber(item, "wind_speed_kmh"),
            }
        );
      }
    }
    if (const auto it = snapshot.find("forecast_days"); it != snapshot.end() && it->is_array()) {
      m_snapshot.forecastDays.clear();
      for (const auto& item : *it) {
        if (!item.is_object()) {
          continue;
        }
        m_snapshot.forecastDays.push_back(
            WeatherForecastDay{
                .dateIso = readString(item, "date_iso"),
                .conditionText = readString(item, "condition_text"),
                .weatherCode = readOptionalInt(item, "weather_code"),
                .temperatureMaxC = readOptionalNumber(item, "temperature_max_c"),
                .temperatureMinC = readOptionalNumber(item, "temperature_min_c"),
                .sunriseIso = readString(item, "sunrise_iso"),
                .sunsetIso = readString(item, "sunset_iso"),
            }
        );
      }
    }
    m_snapshot.fetchedAt = fromUnixSeconds(readOptionalInt(snapshot, "fetched_at"));
    dropPastForecastHours(m_snapshot);
    dropPastForecastDays(m_snapshot);

    m_resolvedLatitude = m_snapshot.latitude;
    m_resolvedLongitude = m_snapshot.longitude;
    m_locationName = m_snapshot.locationName;
    m_locationSource = m_snapshot.sourceLabel;
    m_hasLocation = coordinatesValid();
    m_nextRefreshAt = m_snapshot.fetchedAt + std::chrono::minutes(std::max(5, m_activeConfig.refreshMinutes));

    kLog.info("loaded cached weather data");
  } catch (const std::exception& e) {
    kLog.warn("failed to load weather cache: {}", e.what());
  }
}

void WeatherService::saveCache() const {
  if (!m_snapshot.valid) {
    return;
  }

  const auto path = stateCacheFilePath();
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);

  nlohmann::json json{
      {"provider", "qweather"},
      {"snapshot",
       {
           {"valid", true},
           {"location_name", m_snapshot.locationName},
           {"source_label", m_snapshot.sourceLabel},
           {"latitude", m_snapshot.latitude},
           {"longitude", m_snapshot.longitude},
           {"generation_time_ms", m_snapshot.generationTimeMs},
           {"utc_offset_seconds", m_snapshot.utcOffsetSeconds},
           {"timezone", m_snapshot.timezone},
           {"timezone_abbreviation", m_snapshot.timezoneAbbreviation},
           {"elevation_m", m_snapshot.elevationM},
           {"current_units", currentUnitsToJson(m_snapshot.currentUnits)},
           {"daily_units", dailyUnitsToJson(m_snapshot.dailyUnits)},
           {"hourly_units", hourlyUnitsToJson(m_snapshot.hourlyUnits)},
           {"current",
            {
                {"time_iso", m_snapshot.current.timeIso},
                {"condition_text", m_snapshot.current.conditionText},
                {"interval_seconds", m_snapshot.current.intervalSeconds},
                {"temperature_c", m_snapshot.current.temperatureC},
                {"wind_speed_kmh", m_snapshot.current.windSpeedKmh},
                {"wind_direction_deg", m_snapshot.current.windDirectionDeg},
                {"is_day", m_snapshot.current.isDay},
                {"weather_code", m_snapshot.current.weatherCode},
                {"uv_index", m_snapshot.current.uvIndex},
            }},
           {"forecast_hours", nlohmann::json::array()},
           {"forecast_days", nlohmann::json::array()},
           {"fetched_at", toUnixSeconds(m_snapshot.fetchedAt)},
       }}
  };

  for (const auto& hour : m_snapshot.forecastHours) {
    json["snapshot"]["forecast_hours"].push_back({
        {"time_iso", hour.timeIso},
        {"condition_text", hour.conditionText},
        {"weather_code", hour.weatherCode},
        {"temperature_c", hour.temperatureC},
        {"relative_humidity_percent", hour.relativeHumidityPercent},
        {"precipitation_probability_percent", hour.precipitationProbabilityPercent},
        {"is_day", hour.isDay},
        {"wind_speed_kmh", hour.windSpeedKmh},
    });
  }

  for (const auto& day : m_snapshot.forecastDays) {
    json["snapshot"]["forecast_days"].push_back({
        {"date_iso", day.dateIso},
        {"condition_text", day.conditionText},
        {"weather_code", day.weatherCode},
        {"temperature_max_c", day.temperatureMaxC},
        {"temperature_min_c", day.temperatureMinC},
        {"sunrise_iso", day.sunriseIso},
        {"sunset_iso", day.sunsetIso},
    });
  }

  try {
    std::ofstream file(path);
    file << json.dump(2);
  } catch (const std::exception& e) {
    kLog.warn("failed to save weather cache: {}", e.what());
  }
}
