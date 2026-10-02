#pragma once

#include "json.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace SessionModelExpressionParserSupport
{
	struct ParsedFunctionCall
	{
		bool parsed = false;
		bool valid = false;
		std::string name;
		std::vector<std::string> arguments;
	};

	std::string Trim(const std::string& value);
	std::string NormalizeLookup(const std::string& value);
	std::string ToLowerAscii(std::string value);
	std::string RemoveMatchingQuotes(const std::string& value);
	std::vector<std::string> SplitLiteralList(const std::string& value);
	bool ValueInLiteralList(const std::string& currentValue, const std::string& literalList);
	bool IsQuotedString(const std::string& value);
	bool IsFunctionName(const std::string& value);
	ParsedFunctionCall ParseFunctionCall(const std::string& value);

	bool TryGetString(const nlohmann::json& node, const char* key, std::string& value);
	std::string GetString(const nlohmann::json& node, const char* key);
	std::string FirstNonEmpty(const nlohmann::json& node, std::initializer_list<const char*> keys);
	std::string FunctionArgumentValue(const nlohmann::json& argument);
	ParsedFunctionCall BuildCallFromNode(const nlohmann::json& node, const std::string& functionName);
}
