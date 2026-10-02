#pragma once

#include <string>

namespace SessionModelTemporalDateTimeSupport
{
	struct DateTimeParts
	{
		int year = 1970;
		int month = 1;
		int day = 1;
		int hour = 0;
		int minute = 0;
		int second = 0;
		bool hasDate = false;
		bool hasTime = false;
	};

	bool TryParseDateTimeParts(const std::string& rawValue, DateTimeParts& parts);

	int DaysInMonth(int year, int month);

	bool IsValidDate(int year, int month, int day);

	long long DaysFromCivil(int year, int month, int day);

	DateTimeParts CivilFromDays(long long days);

	std::string FormatDate(const DateTimeParts& parts);

	std::string FormatTime(const DateTimeParts& parts);

	std::string FormatDateTime(const DateTimeParts& parts);

	DateTimeParts AddMonthsToDate(DateTimeParts parts, int deltaMonths);

	long long SecondsFromParts(const DateTimeParts& parts);

	DateTimeParts PartsFromSeconds(long long seconds);

	int CompareDateTimeParts(const DateTimeParts& left, const DateTimeParts& right);

	int WholeMonthDifference(DateTimeParts left, DateTimeParts right);

	int WholeYearDifference(DateTimeParts left, DateTimeParts right);

	int DayOfWeekMondayZero(long long days);
}
