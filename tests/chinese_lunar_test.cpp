#include "time/chinese_lunar.h"

#include <libqalculate/Calculator.h>
#include <print>
#include <string_view>

namespace {

  bool expectDate(
      int year, unsigned month, unsigned day, const ChineseLunarDate& expected, std::string_view expectedCell,
      std::string_view expectedFull
  ) {
    const auto actual = chineseLunarFromGregorian(year, month, day);
    if (!actual.has_value()) {
      std::println(stderr, "chinese_lunar_test: conversion failed for {:04}-{:02}-{:02}", year, month, day);
      return false;
    }
    if (*actual != expected) {
      std::println(
          stderr, "chinese_lunar_test: wrong date for {:04}-{:02}-{:02}: got {}-{}-{} leap={}", year, month, day,
          actual->year, actual->month, actual->day, actual->leapMonth
      );
      return false;
    }
    if (formatChineseLunarCell(*actual) != expectedCell || formatChineseLunarDate(*actual) != expectedFull) {
      std::println(stderr, "chinese_lunar_test: wrong label for {:04}-{:02}-{:02}", year, month, day);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  Calculator calculator;

  bool ok = true;
  ok = expectDate(2024, 2, 10, {4721, 1, 1, false}, "正月", "农历正月初一") && ok;
  ok = expectDate(2024, 9, 17, {4721, 8, 15, false}, "十五", "农历八月十五") && ok;
  ok = expectDate(2025, 1, 29, {4722, 1, 1, false}, "正月", "农历正月初一") && ok;
  ok = expectDate(2026, 2, 17, {4723, 1, 1, false}, "正月", "农历正月初一") && ok;
  ok = expectDate(2023, 3, 22, {4720, 2, 1, true}, "闰二月", "农历闰二月初一") && ok;

  if (chineseLunarFromGregorian(2024, 2, 30).has_value()) {
    std::println(stderr, "chinese_lunar_test: invalid Gregorian date was accepted");
    ok = false;
  }
  return ok ? 0 : 1;
}
