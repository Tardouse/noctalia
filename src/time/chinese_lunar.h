#pragma once

#include <optional>
#include <string>

struct ChineseLunarDate {
  long year = 0;
  int month = 0;
  int day = 0;
  bool leapMonth = false;

  bool operator==(const ChineseLunarDate&) const = default;
};

// Converts a Gregorian civil date to the traditional Chinese calendar. The
// conversion uses libqalculate, which is already a required Noctalia dependency.
// It is unavailable until Noctalia's calculator service has been initialized.
[[nodiscard]] std::optional<ChineseLunarDate>
chineseLunarFromGregorian(int gregorianYear, unsigned gregorianMonth, unsigned gregorianDay);

// Short label used in a month grid: the month name on its first day, otherwise
// the lunar day name (for example "正月", "十五", or "闰二月").
[[nodiscard]] std::string formatChineseLunarCell(const ChineseLunarDate& date);

// Full label without a lunar year, suitable beside a Gregorian date (for
// example "农历八月十五").
[[nodiscard]] std::string formatChineseLunarDate(const ChineseLunarDate& date);
