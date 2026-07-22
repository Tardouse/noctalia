#include "time/chinese_lunar.h"

#include <array>
#include <chrono>
#include <libqalculate/QalculateDateTime.h>
#include <libqalculate/includes.h>
#include <string_view>

namespace {

  constexpr std::array<std::string_view, 12> kMonthNames = {
      "正月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "冬月", "腊月",
  };

  constexpr std::array<std::string_view, 30> kDayNames = {
      "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
      "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
      "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
  };

  std::string lunarMonthName(const ChineseLunarDate& date) {
    if (date.month < 1 || date.month > static_cast<int>(kMonthNames.size())) {
      return {};
    }
    std::string result;
    if (date.leapMonth) {
      result = "闰";
    }
    result += kMonthNames[static_cast<std::size_t>(date.month - 1)];
    return result;
  }

  std::string_view lunarDayName(int day) {
    if (day < 1 || day > static_cast<int>(kDayNames.size())) {
      return {};
    }
    return kDayNames[static_cast<std::size_t>(day - 1)];
  }

} // namespace

std::optional<ChineseLunarDate>
chineseLunarFromGregorian(int gregorianYear, unsigned gregorianMonth, unsigned gregorianDay) {
  const std::chrono::year_month_day gregorian{
      std::chrono::year{gregorianYear}, std::chrono::month{gregorianMonth}, std::chrono::day{gregorianDay}
  };
  if (!gregorian.ok() || CALCULATOR == nullptr) {
    return std::nullopt;
  }

  const QalculateDateTime date(
      static_cast<long>(gregorianYear), static_cast<int>(gregorianMonth), static_cast<int>(gregorianDay)
  );
  long lunarYear = 0;
  long lunarMonth = 0;
  long lunarDay = 0;
  if (!dateToCalendar(date, lunarYear, lunarMonth, lunarDay, CALENDAR_CHINESE)) {
    return std::nullopt;
  }

  const bool leapMonth = lunarMonth > 12;
  if (leapMonth) {
    lunarMonth -= 12;
  }
  if (lunarMonth < 1 || lunarMonth > 12 || lunarDay < 1 || lunarDay > 30) {
    return std::nullopt;
  }

  return ChineseLunarDate{
      .year = lunarYear,
      .month = static_cast<int>(lunarMonth),
      .day = static_cast<int>(lunarDay),
      .leapMonth = leapMonth,
  };
}

std::string formatChineseLunarCell(const ChineseLunarDate& date) {
  if (date.day == 1) {
    return lunarMonthName(date);
  }
  return std::string(lunarDayName(date.day));
}

std::string formatChineseLunarDate(const ChineseLunarDate& date) {
  const std::string month = lunarMonthName(date);
  const std::string_view day = lunarDayName(date.day);
  if (month.empty() || day.empty()) {
    return {};
  }
  return "农历" + month + std::string(day);
}
