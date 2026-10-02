#include "SessionModelTemporalSupport.h"

#include <cctype>

namespace
{
	std::string RemoveMatchingQuotesCopy(const std::string& value)
	{
		const std::string trimmed = SessionModelTemporalSupport::Trim(value);

		if(trimmed.size() >= 2
			&& ((trimmed.front() == '"' && trimmed.back() == '"')
				|| (trimmed.front() == '\'' && trimmed.back() == '\'')))
		{
			return trimmed.substr(1, trimmed.size() - 2);
		}

		return trimmed;
	}

	bool TryParseDigits(const std::string& value, size_t offset, size_t length, int& result)
	{
		if(offset + length > value.size())
		{
			return false;
		}

		int parsed = 0;

		for(size_t i = 0; i < length; ++i)
		{
			const char ch = value[offset + i];

			if(ch < '0' || ch > '9')
			{
				return false;
			}

			parsed = parsed * 10 + (ch - '0');
		}

		result = parsed;

		return true;
	}

	bool IsLeapYear(int year)
	{
		return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
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

	bool IsValidTime(int hour, int minute, int second)
	{
		return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
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

	bool TryParseTransactionTime(const std::string& rawValue, long long& seconds)
	{
		const std::string value = RemoveMatchingQuotesCopy(rawValue);

		if(value.size() < 10 || value[4] != '-' || value[7] != '-')
		{
			return false;
		}

		int year = 0;
		int month = 0;
		int day = 0;

		if(!TryParseDigits(value, 0, 4, year)
			|| !TryParseDigits(value, 5, 2, month)
			|| !TryParseDigits(value, 8, 2, day)
			|| !IsValidDate(year, month, day))
		{
			return false;
		}

		size_t pos = 10;
		int hour = 0;
		int minute = 0;
		int second = 0;

		if(pos < value.size())
		{
			if(value[pos] != 'T' && value[pos] != ' ')
			{
				return false;
			}

			++pos;

			if(!TryParseDigits(value, pos, 2, hour))
			{
				return false;
			}

			pos += 2;

			if(pos >= value.size() || value[pos] != ':')
			{
				return false;
			}

			++pos;

			if(!TryParseDigits(value, pos, 2, minute))
			{
				return false;
			}

			pos += 2;

			if(pos < value.size() && value[pos] == ':')
			{
				++pos;

				if(!TryParseDigits(value, pos, 2, second))
				{
					return false;
				}

				pos += 2;
			}

			if(!IsValidTime(hour, minute, second))
			{
				return false;
			}

			if(pos < value.size() && value[pos] == '.')
			{
				++pos;

				const size_t fractionStart = pos;

				while(pos < value.size() && std::isdigit(static_cast<unsigned char>(value[pos])))
				{
					++pos;
				}

				if(pos == fractionStart)
				{
					return false;
				}
			}
		}

		int offsetSeconds = 0;

		if(pos < value.size())
		{
			if(value[pos] == 'Z' || value[pos] == 'z')
			{
				++pos;
			}
			else if(value[pos] == '+' || value[pos] == '-')
			{
				const int sign = value[pos] == '+' ? 1 : -1;
				int offsetHour = 0;
				int offsetMinute = 0;

				++pos;

				if(!TryParseDigits(value, pos, 2, offsetHour))
				{
					return false;
				}

				pos += 2;

				if(pos < value.size() && value[pos] == ':')
				{
					++pos;
				}

				if(!TryParseDigits(value, pos, 2, offsetMinute))
				{
					return false;
				}

				pos += 2;

				if(offsetHour > 23 || offsetMinute > 59)
				{
					return false;
				}

				offsetSeconds = sign * (offsetHour * 3600 + offsetMinute * 60);
			}
			else
			{
				return false;
			}
		}

		if(pos != value.size())
		{
			return false;
		}

		seconds = DaysFromCivil(year, month, day) * 86400LL
			+ static_cast<long long>(hour) * 3600LL
			+ static_cast<long long>(minute) * 60LL
			+ static_cast<long long>(second)
			- static_cast<long long>(offsetSeconds);

		return true;
	}
}

namespace SessionModelTemporalSupport
{
	std::string Trim(const std::string& value)
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

	bool IsValidTransactionTime(const std::string& value)
	{
		if(Trim(value).empty())
		{
			return true;
		}

		long long seconds = 0;

		return TryParseTransactionTime(value, seconds);
	}

	bool IsValidTransactionWindow(const std::string& transactionStart, const std::string& transactionEnd)
	{
		long long startSeconds = 0;
		long long endSeconds = 0;
		const bool hasStart = !Trim(transactionStart).empty();
		const bool hasEnd = !Trim(transactionEnd).empty();

		if(hasStart && !TryParseTransactionTime(transactionStart, startSeconds))
		{
			return false;
		}

		if(hasEnd && !TryParseTransactionTime(transactionEnd, endSeconds))
		{
			return false;
		}

		return !hasStart || !hasEnd || endSeconds >= startSeconds;
	}

	bool TransactionWindowVisibleAtSnapshot(const std::string& transactionStart,
	                                        const std::string& transactionEnd,
	                                        const std::string& snapshotTime)
	{
		if(snapshotTime.empty())
		{
			return true;
		}

		long long snapshotSeconds = 0;

		if(!TryParseTransactionTime(snapshotTime, snapshotSeconds))
		{
			return true;
		}

		long long startSeconds = 0;

		if(!transactionStart.empty())
		{
			if(!TryParseTransactionTime(transactionStart, startSeconds))
			{
				return true;
			}

			if(snapshotSeconds < startSeconds)
			{
				return false;
			}
		}

		if(!transactionEnd.empty())
		{
			long long endSeconds = 0;

			if(!TryParseTransactionTime(transactionEnd, endSeconds))
			{
				return true;
			}

			if(!transactionStart.empty() && endSeconds < startSeconds)
			{
				return true;
			}

			if(snapshotSeconds >= endSeconds)
			{
				return false;
			}
		}

		return true;
	}
}
