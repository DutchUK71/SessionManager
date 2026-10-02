#include "SessionModelExpressionParserSupport.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace SessionModelExpressionParserSupport
{
	std::string Trim(const std::string& value)
	{
		size_t first = 0;

		while(first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
		{
			++first;
		}

		size_t last = value.size();

		while(last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
		{
			--last;
		}

		return value.substr(first, last - first);
	}

	std::string NormalizeLookup(const std::string& value)
	{
		std::string normalized = Trim(value);

		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});

		return normalized;
	}

	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});

		return value;
	}

	std::string RemoveMatchingQuotes(const std::string& value)
	{
		std::string trimmed = Trim(value);

		if(trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"')
		{
			return trimmed.substr(1, trimmed.size() - 2);
		}

		return trimmed;
	}

	std::vector<std::string> SplitLiteralList(const std::string& value)
	{
		std::vector<std::string> items;
		std::stringstream stream(RemoveMatchingQuotes(value));
		std::string item;

		while(std::getline(stream, item))
		{
			std::string trimmed = Trim(item);

			if(!trimmed.empty())
			{
				items.push_back(trimmed);
			}
		}

		if(items.empty())
		{
			std::string trimmed = Trim(RemoveMatchingQuotes(value));

			if(!trimmed.empty())
			{
				items.push_back(trimmed);
			}
		}

		return items;
	}

	bool ValueInLiteralList(const std::string& currentValue, const std::string& literalList)
	{
		const std::string normalizedCurrent = NormalizeLookup(RemoveMatchingQuotes(currentValue));

		for(const std::string& item : SplitLiteralList(literalList))
		{
			if(NormalizeLookup(RemoveMatchingQuotes(item)) == normalizedCurrent)
			{
				return true;
			}
		}

		return false;
	}

	bool IsQuotedString(const std::string& value)
	{
		const std::string trimmed = Trim(value);

		return trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"';
	}

	bool IsFunctionName(const std::string& value)
	{
		if(value.empty())
		{
			return false;
		}

		for(char ch : value)
		{
			const unsigned char uch = static_cast<unsigned char>(ch);

			if(!std::isalnum(uch) && ch != '_')
			{
				return false;
			}
		}

		return true;
	}

	ParsedFunctionCall ParseFunctionCall(const std::string& value)
	{
		ParsedFunctionCall result;
		const std::string trimmed = Trim(value);

		if(trimmed.empty() || trimmed.back() != ')')
		{
			return result;
		}

		const size_t openParen = trimmed.find('(');

		if(openParen == std::string::npos)
		{
			return result;
		}

		result.name = Trim(trimmed.substr(0, openParen));

		if(!IsFunctionName(result.name))
		{
			return ParsedFunctionCall();
		}

		result.parsed = true;

		const std::string body = trimmed.substr(openParen + 1, trimmed.size() - openParen - 2);
		std::string current;
		int depth = 0;
		bool inQuote = false;
		bool escaped = false;

		for(char ch : body)
		{
			if(escaped)
			{
				current.push_back(ch);
				escaped = false;

				continue;
			}

			if(ch == '\\' && inQuote)
			{
				current.push_back(ch);
				escaped = true;

				continue;
			}

			if(ch == '"')
			{
				inQuote = !inQuote;
				current.push_back(ch);

				continue;
			}

			if(!inQuote)
			{
				if(ch == '(')
				{
					++depth;
				}
				else if(ch == ')')
				{
					--depth;

					if(depth < 0)
					{
						return result;
					}
				}
				else if(ch == ',' && depth == 0)
				{
					result.arguments.push_back(Trim(current));
					current.clear();

					continue;
				}
			}

			current.push_back(ch);
		}

		if(inQuote || depth != 0)
		{
			return result;
		}

		if(!body.empty() || !current.empty())
		{
			result.arguments.push_back(Trim(current));
		}

		result.valid = true;

		return result;
	}

	bool TryGetString(const nlohmann::json& node, const char* key, std::string& value)
	{
		if(!node.is_object() || !node.contains(key) || node.at(key).is_null())
		{
			return false;
		}

		const nlohmann::json& item = node.at(key);

		if(item.is_string())
		{
			value = item.get<std::string>();

			return true;
		}

		value = item.dump();

		return true;
	}

	std::string GetString(const nlohmann::json& node, const char* key)
	{
		std::string value;

		TryGetString(node, key, value);

		return value;
	}

	std::string FirstNonEmpty(const nlohmann::json& node, std::initializer_list<const char*> keys)
	{
		for(const char* key : keys)
		{
			std::string value = GetString(node, key);

			if(!value.empty())
			{
				return value;
			}
		}

		return "";
	}

	std::string FunctionArgumentValue(const nlohmann::json& argument)
	{
		if(!argument.is_object())
		{
			if(argument.is_string())
			{
				return argument.get<std::string>();
			}

			if(argument.is_boolean())
			{
				return argument.get<bool>() ? "true" : "false";
			}

			if(argument.is_number())
			{
				return argument.dump();
			}

			return "";
		}

		if(argument.contains("relationshipScope") && argument.at("relationshipScope").is_object())
		{
			const std::string relationshipId = GetString(argument.at("relationshipScope"), "relationshipId");

			if(!relationshipId.empty())
			{
				return relationshipId;
			}
		}

		return FirstNonEmpty(argument, {
			"relationshipId",
			"valueRelationshipId",
			"sourceRelationshipId",
			"comparisonRelationshipId",
			"value",
			"attributeId",
			"attributeName",
			"coreName"
		});
	}

	ParsedFunctionCall BuildCallFromNode(const nlohmann::json& node, const std::string& functionName)
	{
		ParsedFunctionCall call;

		call.parsed = true;
		call.valid = true;
		call.name = functionName;

		if(node.contains("functionArguments") && node.at("functionArguments").is_array())
		{
			for(const nlohmann::json& argument : node.at("functionArguments"))
			{
				call.arguments.push_back(FunctionArgumentValue(argument));
			}
		}

		return call;
	}
}
