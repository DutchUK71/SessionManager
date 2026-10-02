#pragma once

#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <string>

using namespace std;

class Date
{
public:
    enum DateFormat
    {
        YYYYMMDD,
        DDMMYYYY,
        MMDDYYYY,
        UNKNOWN_FORMAT
    };
    enum DateSeperator
    {
        DASH,
        SLASH,
        DOT,
        NONE,
        UNKNOWN_SEPERATOR
	};

private:
    int year = 1900;
    int month = 1;
    int day = 1;

    static bool IsLeapYear(int y);

	
    static bool Validate(int y, int m, int d);

public:
    Date() {}

	Date(string stringDate);

    Date(int y, int m, int d) : year(y), month(m), day(d)
    {
        Validate(y, m, d);
    }
   
    static bool IsValidDate(const std::string date);

    // Accessors
    int getYear() const { return year; }
    int getMonth() const { return month; }
    int getDay() const { return day; }

    // Comparison operators
    bool operator==(const Date& other) const;

    bool operator!=(const Date& other) const;

    bool operator<(const Date& other) const;

    bool operator<=(const Date& other) const;

    bool operator>(const Date& other) const;

    bool operator>=(const Date& other) const;
    
    string GetFormattedDate();
};
