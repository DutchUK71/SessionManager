#include "Date.h"

Date::Date(string dateStr)
{
    int day = dateStr.substr(0, 2).empty() ? 0 : stoi(dateStr.substr(0, 2));
    int month = dateStr.substr(2, 2).empty() ? 0 : stoi(dateStr.substr(2, 2));
    int year = dateStr.substr(4, 4).empty() ? 0 : stoi(dateStr.substr(4, 4));

    this->year = year;
    this->month = month;
    this->day = day;

    Validate(year, month, day);
}

bool Date::operator==(const Date& other) const
{
    return year == other.year &&
        month == other.month &&
        day == other.day;
}

bool Date::operator!=(const Date& other) const
{
    return !(*this == other);
}

bool Date::operator<(const Date& other) const
{
    return std::tie(year, month, day) < std::tie(other.year, other.month, other.day);
}

bool Date::operator<=(const Date& other) const
{
    return *this < other || *this == other;
}

bool Date::operator>(const Date& other) const
{
    return other < *this;
}

bool Date::operator>=(const Date& other) const
{
    return !(*this < other);
}

string Date::GetFormattedDate() 
{
    string ss = "";

    if (day < 10)
    {
        ss += "0";
    }
    ss += to_string(day);
    ss += "-";

    if (month < 10)
    {
        ss += "0";
    }
    ss += to_string(month);
    ss += "-";

    ss += to_string(year);

    return ss;
}

bool Date::IsLeapYear(int year) 
{
    return (year % 400 == 0) || (year % 4 == 0 && year % 100 != 0);
}

bool Date::IsValidDate(const std::string date) 
{
	// The date needs to conform to DDMMYYYY format
    if (date.length() != 8)
    {
        return false;
    }

    for (char c : date) 
    {
        if (!isdigit(c))
        {
            return false;
        }
    }

    int day = std::stoi(date.substr(0, 2));
    int month = std::stoi(date.substr(2, 2));
    int year = std::stoi(date.substr(4, 4));

    return Validate(year, month, day);
}

bool Date::Validate(int y, int m, int d)
{
    if (m < 1 || m > 12)
    {
        return false;
    }

    int daysInMonth[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    if (m == 2 && IsLeapYear(y))
    {
        daysInMonth[1] = 29;
    }

    if (d < 1 || d > daysInMonth[m - 1])
    {
        return false;
    }

    return true;
}