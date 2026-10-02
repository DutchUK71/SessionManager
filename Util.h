#pragma once

#include <iostream>
#include <functional>
#include <vector>
#include <variant>
#include <string>
#include <memory>
#include <objbase.h>
#include <iomanip>
#include <sstream> 
#include <random>

using namespace std;

// Utility class
class Util
{
public:
	// Method to generate a GUID as a string
	static string GenerateGUID();
	static string ToUpper(std::string& str);
};

