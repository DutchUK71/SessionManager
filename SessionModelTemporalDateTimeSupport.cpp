#include "SessionModelTemporalDateTimeSupport.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace
{
	std::string TrimCopy(const std::string& value)
	{
		size_t begin = 0;

		while(begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
		{
			++begin;
		}

		size_t end = value.size();

		while(end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
		{
			--end;
		}

		return value.substr(begin, end - begin);
	}

	std::string RemoveMatchingQuotesCopy(const std::string& value)
	{
		std::string trimmed = TrimCopy(value);

		if(trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
		{
			return trimmed.substr(1, trimmed.size() - 2);
		}

		return trimmed;
	}

	bool TryParseSegment(const std::string& value, size_t offset, size_t length, int& result)
	{
		if(offset + length > value.size())
		{
			return false;
		}

		for(size_t index = offset; index < offset + length; ++index)
		{
			if(!std::isdigit(static_cast<unsigned char>(value[index])))
			{
				return false;
			}
		}

		result = std::stoi(value.substr(offset, length));

		return true;
	}

	bool IsLeapYear(int year)
	{
		return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	}

	bool IsValidTime(int hour, int minute, int second)
	{
		return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
	}

	std::string TwoDigits(int value)
	{
		std::ostringstream stream;

		stream << std::setw(2) << std::setfill('0') << value;

		return stream.str();
	}

	long long PositiveModulo(long long value, long long divisor)
	{
		long long result = value % divisor;

		return result < 0 ? result + divisor : result;
	}
}

namespace SessionModelTemporalDateTimeSupport
{
	bool TryParseDateTimeParts(const std::string& rawValue, DateTimeParts& parts)
	{
		const std::string value = RemoveMatchingQuotesCopy(rawValue);

		if(value.size() >= 10 && value[4] == '-' && value[7] == '-')
		{
			if(!TryParseSegment(value, 0, 4, parts.year)
				|| !TryParseSegment(value, 5, 2, parts.month)
				|| !TryParseSegment(value, 8, 2, parts.day)
				|| !IsValidDate(parts.year, parts.month, parts.day))
			{
				return false;
			}

			parts.hasDate = true;

			if(value.size() == 10)
			{
				return true;
			}

			if(value.size() >= 19 && (value[10] == 'T' || value[10] == ' '))
			{
				if(!TryParseSegment(value, 11, 2, parts.hour)
					|| !TryParseSegment(value, 14, 2, parts.minute)
					|| !TryParseSegment(value, 17, 2, parts.second)
					|| !IsValidTime(parts.hour, parts.minute, parts.second))
				{
					return false;
				}

				parts.hasTime = true;

				return true;
			}

			return false;
		}

		if(value.size() >= 8 && value[2] == ':' && value[5] == ':')
		{
			if(!TryParseSegment(value, 0, 2, parts.hour)
				|| !TryParseSegment(value, 3, 2, parts.minute)
				|| !TryParseSegment(value, 6, 2, parts.second)
				|| !IsValidTime(parts.hour, parts.minute, parts.second))
			{
				return false;
			}

			parts.hasTime = true;

			return true;
		}

		return false;
	}

	int DaysInMonth(int year, int month)
	{
		static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

		if(month == 2 && IsLeapYear(year))
		{
			return 29;
		}

		return month >= 1 && month <= 12 ? days[month - 1] : 0;
	}

	bool IsValidDate(int year, int month, int day)
	{
		return month >= 1 && month <= 12 && day >= 1 && day <= DaysInMonth(year, month);
	}

	long long DaysFromCivil(int year, int month, int day)
	{
		year -= month <= 2;
		const int era = (year >= 0 ? year : year - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(year - era * 400);
		const unsigned doy = (153 * (static_cast<unsigned>(month) + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

		return static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
	}

	DateTimeParts CivilFromDays(long long days)
	{
		days += 719468;
		const long long era = (days >= 0 ? days : days - 146096) / 146097;
		const unsigned doe = static_cast<unsigned>(days - era * 146097);
		const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		int year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
		const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const unsigned mp = (5 * doy + 2) / 153;
		const unsigned day = doy - (153 * mp + 2) / 5 + 1;
		const unsigned month = mp + (mp < 10 ? 3 : -9);

		year += month <= 2;

		DateTimeParts parts;
		parts.year = year;
		parts.month = static_cast<int>(month);
		parts.day = static_cast<int>(day);
		parts.hasDate = true;

		return parts;
	}

	std::string FormatDate(const DateTimeParts& parts)
	{
		std::ostringstream stream;

		stream << std::setw(4) << std::setfill('0') << parts.year
			<< "-" << TwoDigits(parts.month)
			<< "-" << TwoDigits(parts.day);

		return stream.str();
	}

	std::string FormatTime(const DateTimeParts& parts)
	{
		return TwoDigits(parts.hour) + ":" + TwoDigits(parts.minute) + ":" + TwoDigits(parts.second);
	}

	std::string FormatDateTime(const DateTimeParts& parts)
	{
		return FormatDate(parts) + "T" + FormatTime(parts);
	}

	DateTimeParts AddMonthsToDate(DateTimeParts parts, int deltaMonths)
	{
		int totalMonths = parts.year * 12 + (parts.month - 1) + deltaMonths;
		int year = totalMonths / 12;
		int month = totalMonths % 12;

		if(month < 0)
		{
			month += 12;
			--year;
		}

		parts.year = year;
		parts.month = month + 1;
		const int maxDay = DaysInMonth(parts.year, parts.month);

		if(parts.day > maxDay)
		{
			parts.day = maxDay;
		}

		return parts;
	}

	long long SecondsFromParts(const DateTimeParts& parts)
	{
		return DaysFromCivil(parts.year, parts.month, parts.day) * 86400LL
			+ static_cast<long long>(parts.hour) * 3600LL
			+ static_cast<long long>(parts.minute) * 60LL
			+ static_cast<long long>(parts.second);
	}

	DateTimeParts PartsFromSeconds(long long seconds)
	{
		long long days = seconds / 86400LL;
		long long remainder = seconds % 86400LL;

		if(remainder < 0)
		{
			remainder += 86400LL;
			--days;
		}

		DateTimeParts parts = CivilFromDays(days);
		parts.hour = static_cast<int>(remainder / 3600LL);
		remainder %= 3600LL;
		parts.minute = static_cast<int>(remainder / 60LL);
		parts.second = static_cast<int>(remainder % 60LL);
		parts.hasTime = true;

		return parts;
	}

	int CompareDateTimeParts(const DateTimeParts& left, const DateTimeParts& right)
	{
		const long long leftSeconds = SecondsFromParts(left);
		const long long rightSeconds = SecondsFromParts(right);

		if(leftSeconds < rightSeconds)
		{
			return -1;
		}

		if(leftSeconds > rightSeconds)
		{
			return 1;
		}

		return 0;
	}

	int WholeMonthDifference(DateTimeParts left, DateTimeParts right)
	{
		if(CompareDateTimeParts(left, right) > 0)
		{
			std::swap(left, right);
		}

		int months = (right.year - left.year) * 12 + (right.month - left.month);
		DateTimeParts candidate = AddMonthsToDate(left, months);

		if(CompareDateTimeParts(candidate, right) > 0)
		{
			--months;
		}

		return months;
	}

	int WholeYearDifference(DateTimeParts left, DateTimeParts right)
	{
		if(CompareDateTimeParts(left, right) > 0)
		{
			std::swap(left, right);
		}

		int years = right.year - left.year;
		DateTimeParts candidate = AddMonthsToDate(left, years * 12);

		if(CompareDateTimeParts(candidate, right) > 0)
		{
			--years;
		}

		return years;
	}

	int DayOfWeekMondayZero(long long days)
	{
		return static_cast<int>(PositiveModulo(days + 3, 7));
	}
}
