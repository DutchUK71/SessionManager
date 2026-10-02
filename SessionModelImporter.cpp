#include "SessionModelImporter.h"
#include "SessionModelTemporalFunctionCatalog.h"

#include <fstream>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace
{
	struct ParsedFunctionCall
	{
		bool parsed = false;
		bool valid = false;
		std::string name;
		std::vector<std::string> arguments;
	};

	struct RelationshipIndexEntry
	{
		std::string relationshipId;
		std::string fromEntityId;
		std::string toEntityId;
	};

	std::string GetString(const json& value, const char* key)
	{
		if(!value.is_object() || !value.contains(key) || value.at(key).is_null())
		{
			return "";
		}

		const json& item = value.at(key);

		if(item.is_string())
		{
			return item.get<std::string>();
		}

		return item.dump();
	}

	bool GetBool(const json& value, const char* key, bool fallback = false)
	{
		if(!value.is_object() || !value.contains(key) || value.at(key).is_null())
		{
			return fallback;
		}

		const json& item = value.at(key);

		if(item.is_boolean())
		{
			return item.get<bool>();
		}

		if(item.is_string())
		{
			const std::string text = item.get<std::string>();

			return text == "true" || text == "True" || text == "TRUE" || text == "1";
		}

		return fallback;
	}

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

	bool IsSafeExternalSecurityIdentifier(const std::string& value)
	{
		const std::string trimmed = Trim(value);
		if(trimmed.empty() || trimmed.size() > 128)
		{
			return false;
		}

		return std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char character) {
			return std::isalnum(character) || character == '-' || character == '_' || character == '.';
		});
	}

	bool IsValidExternalSecurityHttpsUrl(const std::string& value)
	{
		const std::string trimmed = Trim(value);
		const std::string normalized = NormalizeLookup(trimmed);
		if(normalized.rfind("https://", 0) != 0 || trimmed.find('#') != std::string::npos)
		{
			return false;
		}

		const size_t authorityStart = 8;
		const size_t authorityEnd = trimmed.find_first_of("/?#", authorityStart);
		const std::string authority = trimmed.substr(authorityStart, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart);
		return !authority.empty()
			&& authority.find('@') == std::string::npos
			&& std::none_of(trimmed.begin(), trimmed.end(), [](unsigned char character) { return std::isspace(character); });
	}

	std::string NormalizeDataForgetPolicyMode(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty() || normalized == "forget")
		{
			return "Forget";
		}

		if(normalized == "retain")
		{
			return "Retain";
		}

		return "";
	}

	std::string NormalizeTransitionForgetPolicyMode(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty() || normalized == "inherit")
		{
			return "Inherit";
		}

		if(normalized == "apply")
		{
			return "Apply";
		}

		if(normalized == "skip")
		{
			return "Skip";
		}

		return "";
	}

	bool IsRootEntityName(const std::string& value)
	{
		return NormalizeLookup(value) == "root";
	}

	bool IsRootEntityEndpoint(const std::unordered_set<std::string>& rootEntityIds,
	                          const std::string& entityId,
	                          const std::string& entityName)
	{
		return (!entityId.empty() && rootEntityIds.find(entityId) != rootEntityIds.end())
			|| IsRootEntityName(entityName)
			|| IsRootEntityName(entityId);
	}

	bool IsOneToManyCardinality(const std::string& cardinality)
	{
		const std::string normalized = NormalizeLookup(cardinality);

		return normalized == "one-to-many" || normalized == "1m";
	}

	bool IsManyToOneCardinality(const std::string& cardinality)
	{
		const std::string normalized = NormalizeLookup(cardinality);

		return normalized == "many-to-one" || normalized == "m1";
	}

	bool IsManyToManyCardinality(const std::string& cardinality)
	{
		const std::string normalized = NormalizeLookup(cardinality);

		return normalized == "many-to-many" || normalized == "mm";
	}

	bool RelationshipMakesRootRepeatable(const SessionModelImporter::RelationshipDefinition& relationship,
	                                     const std::unordered_set<std::string>& rootEntityIds)
	{
		const bool fromIsRoot = IsRootEntityEndpoint(rootEntityIds, relationship.fromEntityId, relationship.fromEntityName);
		const bool toIsRoot = IsRootEntityEndpoint(rootEntityIds, relationship.toEntityId, relationship.toEntityName);

		return (toIsRoot && IsOneToManyCardinality(relationship.cardinality))
			|| (fromIsRoot && IsManyToOneCardinality(relationship.cardinality))
			|| ((fromIsRoot || toIsRoot) && IsManyToManyCardinality(relationship.cardinality));
	}

	std::string RootSingletonRelationshipContext(const SessionModelImporter::RelationshipDefinition& relationship)
	{
		return "relationship:" + relationship.relationshipId
			+ " from:" + relationship.fromEntityId
			+ " to:" + relationship.toEntityId
			+ " cardinality:" + relationship.cardinality;
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

	bool IsQuotedString(const std::string& value)
	{
		const std::string trimmed = Trim(value);

		return trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"';
	}

	void AddLookup(std::unordered_map<std::string, std::string>& lookup, const std::string& key, const std::string& value)
	{
		if(key.empty() || value.empty())
		{
			return;
		}

		lookup.emplace(NormalizeLookup(key), value);
	}

	void AddRelationshipLookup(std::unordered_map<std::string, RelationshipIndexEntry>& lookup,
	                           const std::string& key,
	                           const RelationshipIndexEntry& value)
	{
		if(key.empty() || value.relationshipId.empty())
		{
			return;
		}

		lookup.emplace(NormalizeLookup(key), value);
	}

	bool IsDatabaseSourceType(const std::string& value)
	{
		return NormalizeLookup(value) == "database";
	}

	bool IsSupportedDatabaseProvider(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		return normalized == "mysql"
			|| normalized == "sql server"
			|| normalized == "sqlserver";
	}

	bool AttributeDataTypesCompatible(const std::string& expected, const std::string& actual)
	{
		if(expected.empty() || actual.empty())
		{
			return true;
		}

		const std::string expectedMapped = SessionModelImporter::MapAttributeType(expected);
		const std::string actualMapped = SessionModelImporter::MapAttributeType(actual);

		if(expectedMapped != "unknown" && actualMapped != "unknown")
		{
			return expectedMapped == actualMapped;
		}

		return NormalizeLookup(expected) == NormalizeLookup(actual);
	}

	std::string LookupValue(const std::unordered_map<std::string, std::string>& lookup, const std::string& key)
	{
		auto it = lookup.find(NormalizeLookup(key));

		return it == lookup.end() ? "" : it->second;
	}

	std::string ResolveRelationshipId(const std::string& scope,
	                                  const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup);

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

	bool IsKnownExpressionName(const std::string& value)
	{
		if(SessionModelTemporalFunctions::IsKnownName(value))
		{
			return true;
		}

		return value == "For"
			|| value == "ForScope"
			|| value == "Filter"
			|| value == "Count"
			|| value == "CountDistinct"
			|| value == "Sum"
			|| value == "Average"
			|| value == "Concatenate"
			|| value == "InstanceCount"
			|| value == "InstanceCountIf"
			|| value == "InstanceValueIf"
			|| value == "InferInstance"
			|| value == "InferInstanceFor"
			|| value == "Error"
			|| value == "Warning"
			|| value == "InstanceEquals"
			|| value == "InstanceMaximum"
			|| value == "InstanceMaximumIf"
			|| value == "InstanceMinimum"
			|| value == "InstanceMinimumIf"
			|| value == "InstanceSum"
			|| value == "InstanceSumIf"
			|| value == "Not"
			|| value == "And"
			|| value == "Or"
			|| value == "If"
			|| value == "Equals"
			|| value == "NotEquals"
			|| value == "Known"
			|| value == "Certain"
			|| value == "Unknown"
			|| value == "Uncertain"
			|| value == "Exists"
			|| value == "ExistsScope"
			|| value == "ForAll"
			|| value == "ForAllScope"
			|| value == "IsMemberOf"
			|| value == "InstanceNotEquals"
			|| value == "GreaterThan"
			|| value == "GreaterThanEquals"
			|| value == "GreaterThanOrEquals"
			|| value == "greaterThanOrEqual"
			|| value == "LessThan"
			|| value == "LessThanEquals"
			|| value == "LessThanOrEquals"
			|| value == "IsNumber"
			|| value == "Default"
			|| value == "DefaultWithUnknown"
			|| value == "Value"
			|| value == "CurrentDate"
			|| value == "CurrentDateTime"
			|| value == "CurrentTime"
			|| value == "CurrentLocale"
			|| value == "DeploymentName"
			|| value == "DeploymentVersion"
			|| value == "Date"
			|| value == "DateTime"
			|| value == "TimeOfDay"
			|| value == "AddDays"
			|| value == "AddWeeks"
			|| value == "AddMonths"
			|| value == "AddYears"
			|| value == "AddHours"
			|| value == "AddMinutes"
			|| value == "AddSeconds"
			|| value == "ConcatenateDateTime"
			|| value == "ExtractDate"
			|| value == "ExtractTimeOfDay"
			|| value == "ExtractDay"
			|| value == "ExtractMonth"
			|| value == "ExtractYear"
			|| value == "ExtractHour"
			|| value == "ExtractMinute"
			|| value == "ExtractSecond"
			|| value == "MakeDate"
			|| value == "DayDifference"
			|| value == "DayDifferenceExclusive"
			|| value == "DayDifferenceInclusive"
			|| value == "WeekDifference"
			|| value == "MonthDifference"
			|| value == "YearDifference"
			|| value == "HourDifference"
			|| value == "MinuteDifference"
			|| value == "SecondDifference"
			|| value == "SecondDifferenceExclusive"
			|| value == "SecondDifferenceInclusive"
			|| value == "NextDate"
			|| value == "NextDayOfTheWeek"
			|| value == "WeekdayCount"
			|| value == "YearStart"
			|| value == "YearEnd"
			|| value == "Contains"
			|| value == "EndsWith"
			|| value == "Find"
			|| value == "Length"
			|| value == "Lower"
			|| value == "NewLine"
			|| value == "Quote"
			|| value == "StartsWith"
			|| value == "Substring"
			|| value == "Text"
			|| value == "Upper"
			|| value == "Abs"
			|| value == "Acos"
			|| value == "Asin"
			|| value == "Atan"
			|| value == "Cos"
			|| value == "Ex"
			|| value == "Ln"
			|| value == "Log"
			|| value == "Maximum"
			|| value == "Minimum"
			|| value == "Number"
			|| value == "Remainder"
			|| value == "Round"
			|| value == "Sin"
			|| value == "Sqrt"
			|| value == "Tan"
			|| value == "Trunc"
			|| value == "Xy";
	}

	bool IsConditionalInstanceAggregateFunctionName(const std::string& value)
	{
		return value == "InstanceMaximumIf"
			|| value == "InstanceMinimumIf"
			|| value == "InstanceSumIf";
	}

	bool IsInstanceAggregateFunctionName(const std::string& value)
	{
		return value == "InstanceMaximum"
			|| value == "InstanceMinimum"
			|| value == "InstanceSum"
			|| IsConditionalInstanceAggregateFunctionName(value);
	}

	bool IsTemporalFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsKnownName(value);
	}

	bool IsTemporalIntervalFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsIntervalFunctionName(value);
	}

	bool IsTemporalExtremaFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsExtremaFunctionName(value);
	}

	bool IsTemporalWeightedAggregateFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsWeightedAggregateFunctionName(value);
	}

	bool IsTemporalConditionalWeightedAggregateFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsConditionalWeightedAggregateFunctionName(value);
	}

	bool IsTemporalCountDistinctAggregateFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsCountDistinctAggregateFunctionName(value);
	}

	bool IsTemporalConditionalCountDistinctAggregateFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsConditionalCountDistinctAggregateFunctionName(value);
	}

	bool IsTemporalScalarDateComparisonFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsScalarDateComparisonFunctionName(value);
	}

	bool IsTemporalElapsedDateUnitFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsElapsedDateUnitFunctionName(value);
	}

	bool IsTemporalScalarDatePredicateFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsScalarDatePredicateFunctionName(value);
	}

	bool IsTemporalConditionalAggregateFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsConditionalAggregateFunctionName(value);
	}

	bool IsTemporalDayCountFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsDayCountFunctionName(value);
	}

	bool IsTemporalRelativeWindowFunctionName(const std::string& value)
	{
		return SessionModelTemporalFunctions::IsRelativeWindowFunctionName(value);
	}

	Attribute::AttributeType ParseHistoryValueType(const std::string& value, Attribute::AttributeType fallback)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty())
		{
			return fallback;
		}

		if(normalized == "integer" || normalized == "int")
		{
			return Attribute::INTEGER;
		}

		if(normalized == "number" || normalized == "numeric" || normalized == "decimal" || normalized == "double" || normalized == "currency")
		{
			return Attribute::DOUBLE;
		}

		if(normalized == "text" || normalized == "string")
		{
			return Attribute::STRING;
		}

		if(normalized == "boolean" || normalized == "bool")
		{
			return Attribute::BOOLEAN;
		}

		if(normalized == "date")
		{
			return Attribute::DATE;
		}

		if(normalized == "datetime" || normalized == "date-time")
		{
			return Attribute::DATETIME;
		}

		if(normalized == "timeofday" || normalized == "time" || normalized == "time-of-day")
		{
			return Attribute::TIMEOFDAY;
		}

		return fallback;
	}

	Attribute::KnowledgeState ParseHistoryKnowledgeState(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized == "unknown")
		{
			return Attribute::KnowledgeState::Unknown;
		}

		if(normalized == "uncertain")
		{
			return Attribute::KnowledgeState::Uncertain;
		}

		return Attribute::KnowledgeState::Certain;
	}

	bool IsNumericLiteralValue(const std::string& value)
	{
		const std::string trimmed = Trim(value);

		if(trimmed.empty())
		{
			return false;
		}

		char* end = nullptr;
		std::strtod(trimmed.c_str(), &end);

		return end != nullptr && *end == '\0';
	}

	bool IsBooleanLiteralValue(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		return normalized == "true" || normalized == "false";
	}

	bool LooksLikeDateOrTimeLiteralValue(const std::string& value)
	{
		const std::string trimmed = Trim(value);

		if(trimmed.empty())
		{
			return false;
		}

		bool hasDigit = false;
		bool hasDateTimeSeparator = false;

		for(char ch : trimmed)
		{
			const unsigned char uch = static_cast<unsigned char>(ch);

			if(std::isdigit(uch))
			{
				hasDigit = true;
				continue;
			}

			if(ch == '-' || ch == ':' || ch == 'T' || ch == 't' || ch == 'Z' || ch == 'z' || ch == '.' || ch == '+')
			{
				hasDateTimeSeparator = true;
				continue;
			}

			return false;
		}

		return hasDigit && hasDateTimeSeparator;
	}

	bool IsLiteralExpressionArgument(const std::string& argument)
	{
		return IsQuotedString(argument)
			|| IsNumericLiteralValue(argument)
			|| IsBooleanLiteralValue(argument)
			|| LooksLikeDateOrTimeLiteralValue(argument);
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

	void AddCount(std::unordered_map<std::string, int>& counts, const std::string& key)
	{
		if(!key.empty())
		{
			counts[key]++;
		}
	}

	bool StartsWith(const std::string& value, const std::string& prefix)
	{
		return value.rfind(prefix, 0) == 0;
	}

	bool IsInputReferenceSource(const std::string& source)
	{
		return StartsWith(source, "condition")
			|| StartsWith(source, "decisionTable.conditionCells")
			|| StartsWith(source, "decisionTable.conclusionCells.")
			|| StartsWith(source, "rule.conclusionExpression")
			|| StartsWith(source, "rule.conclusionValue");
	}

	std::string FirstNonEmpty(const json& value, std::initializer_list<const char*> keys)
	{
		for(const char* key : keys)
		{
			const std::string item = GetString(value, key);

			if(!item.empty())
			{
				return item;
			}
		}

		return "";
	}

	const json* FindObjectMember(const json& value, std::initializer_list<const char*> keys)
	{
		if(!value.is_object())
		{
			return nullptr;
		}

		for(const char* key : keys)
		{
			if(value.contains(key) && value.at(key).is_object())
			{
				return &value.at(key);
			}
		}

		return nullptr;
	}

	bool IsLegacyDecisionTableOtherwiseCell(const json& cell)
	{
		if(!cell.is_object())
		{
			return false;
		}

		const json* fragment = FindObjectMember(cell, {"fragment", "Fragment"});
		const std::string markerValue = NormalizeLookup(FirstNonEmpty(cell, {"value", "Value", "text", "Text"}));
		const std::string markerKind = NormalizeLookup(FirstNonEmpty(cell, {"valueKind", "ValueKind", "valueType", "ValueType", "kind", "Kind"}));
		const std::string markerMode = NormalizeLookup(FirstNonEmpty(cell, {"mode", "Mode"}));
		const std::string fragmentValue = fragment != nullptr ? NormalizeLookup(FirstNonEmpty(*fragment, {"text", "Text", "value", "Value"})) : "";
		const std::string fragmentKind = fragment != nullptr ? NormalizeLookup(FirstNonEmpty(*fragment, {"valueKind", "ValueKind", "valueType", "ValueType", "kind", "Kind"})) : "";
		const std::string fragmentMode = fragment != nullptr ? NormalizeLookup(FirstNonEmpty(*fragment, {"mode", "Mode"})) : "";

		const bool hasOtherwiseMarker = markerValue == "otherwise"
			|| markerKind == "otherwise"
			|| markerMode == "otherwise"
			|| fragmentValue == "otherwise"
			|| fragmentKind == "otherwise"
			|| fragmentMode == "otherwise";

		if(!hasOtherwiseMarker)
		{
			return false;
		}

		if(!FirstNonEmpty(cell, {"entityId", "EntityId", "attributeId", "AttributeId", "relationshipId", "RelationshipId", "functionName", "FunctionName"}).empty())
		{
			return false;
		}

		const std::string nodeType = NormalizeLookup(FirstNonEmpty(cell, {"nodeType", "NodeType"}));
		if(nodeType == "group" || nodeType == "expression")
		{
			return false;
		}

		if(cell.contains("children") && cell.at("children").is_array() && !cell.at("children").empty())
		{
			return false;
		}

		return true;
	}

	bool IsLegacyDecisionTableOtherwiseRow(const json& row)
	{
		if(!row.is_object())
		{
			return false;
		}

		if(GetBool(row, "isOtherwise") || GetBool(row, "IsOtherwise"))
		{
			return true;
		}

		if(!row.contains("conditionCells") || !row.at("conditionCells").is_array())
		{
			return false;
		}

		const json& conditionCells = row.at("conditionCells");
		return conditionCells.size() == 1 && IsLegacyDecisionTableOtherwiseCell(conditionCells.at(0));
	}

	void NormalizeDecisionTableOtherwiseRows(json& executableDecisionTable)
	{
		if(!executableDecisionTable.is_object()
			|| !executableDecisionTable.contains("rows")
			|| !executableDecisionTable.at("rows").is_array())
		{
			return;
		}

		for(json& row : executableDecisionTable["rows"])
		{
			if(IsLegacyDecisionTableOtherwiseRow(row))
			{
				row["conditionCells"] = json::array();
			}
		}
	}

	std::string NormalizeDataConnectionSaveOperationPolicy(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty()
			|| normalized == "updateonly"
			|| normalized == "update"
			|| normalized == "updateexisting")
		{
			return "UpdateOnly";
		}

		if(normalized == "upsert"
			|| normalized == "createifmissing"
			|| normalized == "insertifmissing"
			|| normalized == "updateorinsert"
			|| normalized == "updateorcreate")
		{
			return "Upsert";
		}

		return "";
	}

	std::string NormalizeDataConnectionValueGeneration(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty() || normalized == "none")
		{
			return "";
		}

		if(normalized == "databasegenerated"
			|| normalized == "generatedoncreate"
			|| normalized == "identity"
			|| normalized == "autoincrement"
			|| normalized == "autogenerated")
		{
			return "DatabaseGenerated";
		}

		return "";
	}

	std::string NormalizeDataConnectionLoadFilterValueSource(const std::string& value)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized == "sessionuserid"
			|| normalized == "authenticateduserid"
			|| normalized == "currentuserid"
			|| normalized == "signedinuserid"
			|| normalized == "signedinuser")
		{
			return "SessionUserId";
		}

		return "CurrentRuntimeValue";
	}

	void AddExpressionAttributeReference(const std::string& attributeName,
	                                     const std::string& ruleId,
	                                     const std::string& source,
	                                     const std::string& entityId,
	                                     const std::string& relationshipId,
	                                     std::vector<SessionModelImporter::RuleReference>& references,
	                                     const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup)
	{
		if(entityId.empty())
		{
			return;
		}

		auto attributeLookupIt = attributeIdByEntityLookup.find(entityId);

		if(attributeLookupIt == attributeIdByEntityLookup.end())
		{
			return;
		}

		const std::string attributeId = LookupValue(attributeLookupIt->second, attributeName);

		if(attributeId.empty())
		{
			return;
		}

		SessionModelImporter::RuleReference reference;

		reference.ruleId = ruleId;
		reference.source = source;
		reference.entityId = entityId;
		reference.attributeId = attributeId;
		reference.relationshipId = relationshipId;

		references.push_back(reference);
	}

	void AddFunctionReference(const std::string& ruleId,
	                          const std::string& source,
	                          const std::string& functionName,
	                          std::vector<SessionModelImporter::RuleReference>& references)
	{
		if(functionName.empty())
		{
			return;
		}

		SessionModelImporter::RuleReference reference;

		reference.ruleId = ruleId;
		reference.source = source;
		reference.functionName = functionName;

		references.push_back(reference);
	}

	bool IsTemporalCurrentDateBoundaryPredicateFunctionName(const std::string& value)
	{
		return value == "TemporalFromStartDate"
			|| value == "TemporalFromEndDate"
			|| value == "TemporalFromRange"
			|| value == "TemporalOncePerMonth";
	}

	bool IsTemporalCurrentDateComparisonPredicateFunctionName(const std::string& value)
	{
		return value == "TemporalOn"
			|| value == "TemporalBefore"
			|| value == "TemporalAfter"
			|| value == "TemporalOnOrBefore"
			|| value == "TemporalOnOrAfter";
	}

	bool IsTemporalExplicitAnchorLikeArgument(const std::string& argument)
	{
		const std::string unquoted = RemoveMatchingQuotes(argument);

		if(LooksLikeDateOrTimeLiteralValue(unquoted))
		{
			return true;
		}

		ParsedFunctionCall call = ParseFunctionCall(argument);

		return call.parsed && (call.name == "Date" || call.name == "DateTime");
	}

	bool IsSessionContextFunctionName(const std::string& value)
	{
		return value == "CurrentDate"
			|| value == "CurrentDateTime"
			|| value == "CurrentTime"
			|| value == "CurrentLocale"
			|| value == "DeploymentName"
			|| value == "DeploymentVersion";
	}

	void CollectExpressionReferences(const std::string& expression,
	                                  const std::string& ruleId,
	                                  const std::string& source,
	                                  const std::string& defaultEntityId,
	                                  const std::string& defaultRelationshipId,
	                                  std::vector<SessionModelImporter::RuleReference>& references,
	                                  std::unordered_map<std::string, int>& functionCounts,
	                                  const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                                  const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
	                                  const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		ParsedFunctionCall call = ParseFunctionCall(expression);

		if(!call.parsed || !IsKnownExpressionName(call.name))
		{
			return;
		}

		AddCount(functionCounts, call.name);

		if(!call.valid)
		{
			return;
		}

		if(IsSessionContextFunctionName(call.name))
		{
			AddFunctionReference(ruleId,
			                     source + ".function",
			                     call.name,
			                     references);
		}

		if(call.name == "Concatenate")
		{
			for(const std::string& argument : call.arguments)
			{
				if(IsQuotedString(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);

					continue;
				}

				AddExpressionAttributeReference(argument,
				                                ruleId,
				                                source + ".expression",
				                                defaultEntityId,
				                                "",
				                                references,
				                                attributeIdByEntityLookup);
			}

			return;
		}

		if(call.name == "ValueAt" && call.arguments.size() == 2)
		{
			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + (index == 0 ? ".temporal" : ".time"),
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);

					continue;
				}

				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
				                                ruleId,
				                                source + (index == 0 ? ".temporal" : ".time"),
				                                defaultEntityId,
				                                defaultRelationshipId,
				                                references,
				                                attributeIdByEntityLookup);
			}

			return;
		}

		if((call.name == "Earliest" || call.name == "Latest" || call.name == "WhenLast" || call.name == "WhenNext")
			&& !call.arguments.empty() && call.arguments.size() <= 2)
		{
			ParsedFunctionCall attributeCall = ParseFunctionCall(call.arguments[0]);

			if(attributeCall.parsed && IsKnownExpressionName(attributeCall.name))
			{
				CollectExpressionReferences(call.arguments[0],
				                            ruleId,
				                            source + ".temporal",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}
			else
			{
				AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[0]),
				                                ruleId,
				                                source + ".temporal",
				                                defaultEntityId,
				                                defaultRelationshipId,
				                                references,
				                                attributeIdByEntityLookup);
			}

			if(call.arguments.size() == 2 && !IsLiteralExpressionArgument(call.arguments[1]))
			{
				ParsedFunctionCall anchorCall = ParseFunctionCall(call.arguments[1]);

				if(anchorCall.parsed && IsKnownExpressionName(anchorCall.name))
				{
					CollectExpressionReferences(call.arguments[1],
					                            ruleId,
					                            source + ".time",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[1]),
					                                ruleId,
					                                source + ".time",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsTemporalRelativeWindowFunctionName(call.name) && (call.arguments.size() == 2 || call.arguments.size() == 3))
		{
			const size_t conditionIndex = call.arguments.size() == 3 ? 2 : 1;

			if(call.name == "TemporalConsecutiveDays"
				&& call.arguments.size() == 3
				&& !IsTemporalExplicitAnchorLikeArgument(call.arguments[0]))
			{
				AddFunctionReference(ruleId,
				                     source + ".currentDate",
				                     "CurrentDate",
				                     references);
			}

			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool isCondition = index == conditionIndex;

				if(!isCondition && IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + (isCondition ? ".temporal" : ".time"),
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (isCondition ? ".temporal" : ".time"),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "TemporalDaysSince" && call.arguments.size() == 2)
		{
			for(const std::string& argument : call.arguments)
			{
				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall timeCall = ParseFunctionCall(argument);

				if(timeCall.parsed && IsKnownExpressionName(timeCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".time",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + ".time",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsTemporalElapsedDateUnitFunctionName(call.name) && call.arguments.size() == 2)
		{
			for(const std::string& argument : call.arguments)
			{
				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + ".expression",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "TemporalOncePerMonth" && call.arguments.size() == 2)
		{
			AddFunctionReference(ruleId,
			                     source + ".currentDate",
			                     "CurrentDate",
			                     references);

			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool isCondition = index == 1;

				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (isCondition ? ".temporal" : ".expression"),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "TemporalOncePerMonth" && call.arguments.size() == 3)
		{
			AddFunctionReference(ruleId,
			                     source + ".currentDate",
			                     "CurrentDate",
			                     references);

			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool possibleCondition = index == 2;

				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (possibleCondition ? ".temporal" : ".expression"),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "TemporalOncePerMonth" && call.arguments.size() == 4)
		{
			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool isCondition = index == 3;

				if(!isCondition && IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (isCondition ? ".temporal" : ".expression"),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsTemporalScalarDatePredicateFunctionName(call.name) && call.arguments.size() == 1)
		{
			const std::string& argument = call.arguments[0];

			if(IsTemporalCurrentDateBoundaryPredicateFunctionName(call.name))
			{
				AddFunctionReference(ruleId,
				                     source + ".currentDate",
				                     "CurrentDate",
				                     references);
			}

			if(!IsLiteralExpressionArgument(argument))
			{
				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + ".expression",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "TemporalFromRange" && call.arguments.size() == 2)
		{
			AddFunctionReference(ruleId,
			                     source + ".currentDate",
			                     "CurrentDate",
			                     references);

			for(const std::string& argument : call.arguments)
			{
				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + ".expression",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsTemporalScalarDateComparisonFunctionName(call.name) && call.arguments.size() == 1)
		{
			const std::string& argument = call.arguments[0];

			if(IsTemporalCurrentDateComparisonPredicateFunctionName(call.name))
			{
				AddFunctionReference(ruleId,
				                     source + ".currentDate",
				                     "CurrentDate",
				                     references);
			}

			if(!IsLiteralExpressionArgument(argument))
			{
				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + ".expression",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsTemporalScalarDateComparisonFunctionName(call.name) && call.arguments.size() == 2)
		{
			for(const std::string& argument : call.arguments)
			{
				if(IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + ".expression",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + ".expression",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if((IsTemporalExtremaFunctionName(call.name)
			|| IsTemporalWeightedAggregateFunctionName(call.name)
			|| IsTemporalCountDistinctAggregateFunctionName(call.name)) && call.arguments.size() == 3)
		{
			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool isValue = index == 2;

				if(!isValue && IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + (isValue ? ".temporal" : ".time"),
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (isValue ? ".temporal" : ".time"),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if((IsTemporalConditionalAggregateFunctionName(call.name)
			|| IsTemporalConditionalWeightedAggregateFunctionName(call.name)
			|| IsTemporalConditionalCountDistinctAggregateFunctionName(call.name)) && call.arguments.size() >= 3)
		{
			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool isValue = index == 2;
				const bool isCondition = index == 3;
				const bool isTime = index < 2;

				if(!isValue && IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);
				const bool isTemporalDependency = isValue || (isCondition && !(nestedCall.parsed && IsKnownExpressionName(nestedCall.name)));

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + (isTemporalDependency ? ".temporal" : (isTime ? ".time" : ".expression")),
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (isTemporalDependency ? ".temporal" : (isTime ? ".time" : ".expression")),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsTemporalIntervalFunctionName(call.name)
			&& call.arguments.size() == (IsTemporalDayCountFunctionName(call.name) ? 4 : 3))
		{
			const bool hasDays = IsTemporalDayCountFunctionName(call.name);
			const size_t exportedConditionIndex = hasDays ? 3 : 2;
			const bool looksExported = !IsLiteralExpressionArgument(call.arguments[exportedConditionIndex])
				&& (IsLiteralExpressionArgument(call.arguments[0])
					|| IsLiteralExpressionArgument(call.arguments[1])
					|| (hasDays && IsLiteralExpressionArgument(call.arguments[2])));
			const size_t conditionIndex = looksExported ? exportedConditionIndex : 0;

			for(size_t index = 0; index < call.arguments.size(); ++index)
			{
				const std::string& argument = call.arguments[index];
				const bool isCondition = index == conditionIndex;

				if(!isCondition && IsLiteralExpressionArgument(argument))
				{
					continue;
				}

				ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

				if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
				{
					CollectExpressionReferences(argument,
					                            ruleId,
					                            source + (isCondition ? ".temporal" : ".time"),
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
					                                ruleId,
					                                source + (isCondition ? ".temporal" : ".time"),
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "InferInstance" && call.arguments.size() == 1)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".relationship";
				reference.entityId = defaultEntityId;
				reference.relationshipId = relationshipIt->second.relationshipId;
				reference.functionName = call.name;

				references.push_back(reference);
			}

			return;
		}

		if(call.name == "InferInstanceFor" && (call.arguments.size() == 2 || call.arguments.size() == 3))
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const size_t conditionIndex = call.arguments.size() == 3 ? 2 : 1;
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".relationship";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;
				reference.functionName = call.name;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(sourceEntityId.empty() || call.arguments.size() <= conditionIndex || IsQuotedString(call.arguments[conditionIndex]))
			{
				return;
			}

			ParsedFunctionCall conditionCall = ParseFunctionCall(call.arguments[conditionIndex]);

			if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
			{
				CollectExpressionReferences(call.arguments[conditionIndex],
				                            ruleId,
				                            source + ".condition",
				                            sourceEntityId,
				                            sourceRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[conditionIndex]),
			                                ruleId,
			                                source + ".condition",
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if((call.name == "Error" || call.name == "Warning") && call.arguments.size() == 1)
		{
			const std::string messageExpression = call.arguments[0];

			if(IsQuotedString(messageExpression))
			{
				return;
			}

			ParsedFunctionCall messageCall = ParseFunctionCall(messageExpression);

			if(messageCall.parsed && IsKnownExpressionName(messageCall.name))
			{
				CollectExpressionReferences(messageExpression,
				                            ruleId,
				                            source + ".message",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(messageExpression),
			                                ruleId,
			                                source + ".message",
			                                defaultEntityId,
			                                defaultRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if(call.name == "InstanceCount" && call.arguments.size() == 1)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = relationshipIt->second.relationshipId;

				references.push_back(reference);
			}

			return;
		}

		if(call.name == "Filter" && call.arguments.size() == 2)
		{
			const std::string collectionExpression = call.arguments[0];
			const std::string condition = call.arguments[1];
			ParsedFunctionCall collectionCall = ParseFunctionCall(collectionExpression);

			if(collectionCall.parsed && IsKnownExpressionName(collectionCall.name))
			{
				CollectExpressionReferences(collectionExpression,
				                            ruleId,
				                            source + ".collection",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			if(!collectionCall.parsed
				|| !collectionCall.valid
				|| (collectionCall.name != "ForScope" && collectionCall.name != "ForScopeAt")
				|| collectionCall.arguments.empty())
			{
				return;
			}

			const std::string scope = RemoveMatchingQuotes(collectionCall.arguments[0]);
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(sourceEntityId.empty() || IsLiteralExpressionArgument(condition))
			{
				return;
			}

			ParsedFunctionCall conditionCall = ParseFunctionCall(condition);

			if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
			{
				CollectExpressionReferences(condition,
				                            ruleId,
				                            source + ".condition",
				                            sourceEntityId,
				                            sourceRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(condition),
			                                ruleId,
			                                source + ".condition",
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if(call.name == "ForScope" && call.arguments.size() >= 2 && call.arguments.size() <= 3)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const size_t expressionIndex = call.arguments.size() >= 3 ? 2 : 1;
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(sourceEntityId.empty() || call.arguments.size() <= expressionIndex || IsQuotedString(call.arguments[expressionIndex]))
			{
				return;
			}

			ParsedFunctionCall expressionCall = ParseFunctionCall(call.arguments[expressionIndex]);

			if(expressionCall.parsed && IsKnownExpressionName(expressionCall.name))
			{
				CollectExpressionReferences(call.arguments[expressionIndex],
				                            ruleId,
				                            source + ".expression",
				                            sourceEntityId,
				                            sourceRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[expressionIndex]),
			                                ruleId,
			                                source + ".expression",
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if((call.name == "ForScopeAt" || call.name == "ForAllScopeAt" || call.name == "ForAllAt") && (call.arguments.size() == 3 || call.arguments.size() == 4))
		{
			const bool isForScopeAt = call.name == "ForScopeAt";
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const size_t timeIndex = call.arguments.size() == 4 ? 2 : 1;
			const size_t expressionIndex = call.arguments.size() == 4 ? 3 : 2;
			const std::string scopedSource = source + (isForScopeAt ? ".expression" : ".condition");
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".temporalRelationship";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(sourceRelationshipId.empty() && !IsLiteralExpressionArgument(call.arguments[0]))
			{
				ParsedFunctionCall relationshipCall = ParseFunctionCall(call.arguments[0]);

				if(relationshipCall.parsed && IsKnownExpressionName(relationshipCall.name))
				{
					CollectExpressionReferences(call.arguments[0],
					                            ruleId,
					                            source + ".relationship",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[0]),
					                                ruleId,
					                                source + ".relationship",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			if(!IsLiteralExpressionArgument(call.arguments[timeIndex]))
			{
				ParsedFunctionCall timeCall = ParseFunctionCall(call.arguments[timeIndex]);

				if(timeCall.parsed && IsKnownExpressionName(timeCall.name))
				{
					CollectExpressionReferences(call.arguments[timeIndex],
					                            ruleId,
					                            source + ".time",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[timeIndex]),
					                                ruleId,
					                                source + ".time",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			if(sourceEntityId.empty() || IsQuotedString(call.arguments[expressionIndex]))
			{
				return;
			}

			ParsedFunctionCall expressionCall = ParseFunctionCall(call.arguments[expressionIndex]);

			if(expressionCall.parsed && IsKnownExpressionName(expressionCall.name))
			{
				CollectExpressionReferences(call.arguments[expressionIndex],
				                            ruleId,
				                            scopedSource,
				                            sourceEntityId,
				                            sourceRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[expressionIndex]),
			                                ruleId,
			                                scopedSource,
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if((call.name == "Exists" || call.name == "ExistsScope" || call.name == "ForAll" || call.name == "ForAllScope") && !call.arguments.empty() && call.arguments.size() <= 3)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const size_t conditionIndex = call.arguments.size() >= 3 ? 2 : 1;
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(sourceEntityId.empty() || call.arguments.size() <= conditionIndex || IsQuotedString(call.arguments[conditionIndex]))
			{
				return;
			}

			ParsedFunctionCall conditionCall = ParseFunctionCall(call.arguments[conditionIndex]);

			if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
			{
				CollectExpressionReferences(call.arguments[conditionIndex],
				                            ruleId,
				                            source + ".condition",
				                            sourceEntityId,
				                            sourceRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[conditionIndex]),
			                                ruleId,
			                                source + ".condition",
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if(call.name == "IsMemberOfAt" && (call.arguments.size() == 3 || call.arguments.size() == 4))
		{
			const size_t relationshipIndex = call.arguments.size() == 4 ? 2 : 1;
			const size_t timeIndex = call.arguments.size() == 4 ? 3 : 2;
			const std::string targetExpression = call.arguments[0];
			const std::string relationshipScope = RemoveMatchingQuotes(call.arguments[relationshipIndex]);
			const std::string relationshipId = ResolveRelationshipId(relationshipScope, relationshipByLookup);
			const std::string scopedRelationshipId = !relationshipId.empty() ? relationshipId : defaultRelationshipId;

			if(!relationshipId.empty())
			{
				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".temporalRelationship";
				reference.entityId = defaultEntityId;
				reference.relationshipId = relationshipId;

				references.push_back(reference);
			}

			if(!IsLiteralExpressionArgument(targetExpression))
			{
				ParsedFunctionCall targetCall = ParseFunctionCall(targetExpression);

				if(targetCall.parsed && IsKnownExpressionName(targetCall.name))
				{
					CollectExpressionReferences(targetExpression,
					                            ruleId,
					                            source + ".target",
					                            defaultEntityId,
					                            scopedRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(targetExpression),
					                                ruleId,
					                                source + ".target",
					                                defaultEntityId,
					                                scopedRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			if(relationshipId.empty() && !IsLiteralExpressionArgument(call.arguments[relationshipIndex]))
			{
				ParsedFunctionCall relationshipCall = ParseFunctionCall(call.arguments[relationshipIndex]);

				if(relationshipCall.parsed && IsKnownExpressionName(relationshipCall.name))
				{
					CollectExpressionReferences(call.arguments[relationshipIndex],
					                            ruleId,
					                            source + ".relationship",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[relationshipIndex]),
					                                ruleId,
					                                source + ".relationship",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			if(!IsLiteralExpressionArgument(call.arguments[timeIndex]))
			{
				ParsedFunctionCall timeCall = ParseFunctionCall(call.arguments[timeIndex]);

				if(timeCall.parsed && IsKnownExpressionName(timeCall.name))
				{
					CollectExpressionReferences(call.arguments[timeIndex],
					                            ruleId,
					                            source + ".time",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[timeIndex]),
					                                ruleId,
					                                source + ".time",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "IsMemberOf" && (call.arguments.size() == 2 || call.arguments.size() == 3))
		{
			const size_t relationshipIndex = call.arguments.size() == 3 ? 2 : 1;
			const std::string targetExpression = call.arguments[0];
			const std::string relationshipScope = RemoveMatchingQuotes(call.arguments[relationshipIndex]);
			const std::string relationshipId = ResolveRelationshipId(relationshipScope, relationshipByLookup);
			const std::string scopedRelationshipId = !relationshipId.empty() ? relationshipId : defaultRelationshipId;

			if(!relationshipId.empty())
			{
				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = relationshipId;

				references.push_back(reference);
			}

			if(!IsLiteralExpressionArgument(targetExpression))
			{
				ParsedFunctionCall targetCall = ParseFunctionCall(targetExpression);

				if(targetCall.parsed && IsKnownExpressionName(targetCall.name))
				{
					CollectExpressionReferences(targetExpression,
					                            ruleId,
					                            source + ".target",
					                            defaultEntityId,
					                            scopedRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(targetExpression),
					                                ruleId,
					                                source + ".target",
					                                defaultEntityId,
					                                scopedRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			if(relationshipId.empty() && !IsLiteralExpressionArgument(call.arguments[relationshipIndex]))
			{
				ParsedFunctionCall relationshipCall = ParseFunctionCall(call.arguments[relationshipIndex]);

				if(relationshipCall.parsed && IsKnownExpressionName(relationshipCall.name))
				{
					CollectExpressionReferences(call.arguments[relationshipIndex],
					                            ruleId,
					                            source + ".relationship",
					                            defaultEntityId,
					                            defaultRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(call.arguments[relationshipIndex]),
					                                ruleId,
					                                source + ".relationship",
					                                defaultEntityId,
					                                defaultRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "InstanceCountIf" && call.arguments.size() == 2)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const std::string condition = call.arguments[1];
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(sourceEntityId.empty() || IsQuotedString(condition))
			{
				return;
			}

			ParsedFunctionCall conditionCall = ParseFunctionCall(condition);

			if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
			{
				CollectExpressionReferences(condition,
				                            ruleId,
				                            source + ".condition",
				                            sourceEntityId,
				                            sourceRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(condition),
			                                ruleId,
			                                source + ".condition",
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if(call.name == "InstanceValueIf" && call.arguments.size() == 3)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const std::string valueExpression = call.arguments[1];
			const std::string condition = call.arguments[2];
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			if(!IsLiteralExpressionArgument(valueExpression))
			{
				ParsedFunctionCall valueCall = ParseFunctionCall(valueExpression);

				if(valueCall.parsed && IsKnownExpressionName(valueCall.name))
				{
					CollectExpressionReferences(valueExpression,
					                            ruleId,
					                            source + ".expression",
					                            sourceEntityId,
					                            sourceRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(valueExpression),
					                                ruleId,
					                                source + ".expression",
					                                sourceEntityId,
					                                sourceRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			if(!IsLiteralExpressionArgument(condition))
			{
				ParsedFunctionCall conditionCall = ParseFunctionCall(condition);

				if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
				{
					CollectExpressionReferences(condition,
					                            ruleId,
					                            source + ".condition",
					                            sourceEntityId,
					                            sourceRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(condition),
					                                ruleId,
					                                source + ".condition",
					                                sourceEntityId,
					                                sourceRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(IsInstanceAggregateFunctionName(call.name) && call.arguments.size() == (IsConditionalInstanceAggregateFunctionName(call.name) ? 3 : 2))
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const std::string attributeName = RemoveMatchingQuotes(call.arguments[1]);
			const std::string condition = call.arguments.size() >= 3 ? call.arguments[2] : "";
			std::string sourceEntityId;
			std::string sourceRelationshipId;
			auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

			if(relationshipIt != relationshipByLookup.end())
			{
				const RelationshipIndexEntry& relationship = relationshipIt->second;

				sourceRelationshipId = relationship.relationshipId;

				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".collection";
				reference.entityId = defaultEntityId;
				reference.relationshipId = sourceRelationshipId;

				references.push_back(reference);

				if(relationship.fromEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.toEntityId;
				}
				else if(relationship.toEntityId == defaultEntityId)
				{
					sourceEntityId = relationship.fromEntityId;
				}
				else
				{
					sourceEntityId = relationship.toEntityId;
				}
			}

			if(sourceEntityId.empty())
			{
				sourceEntityId = LookupValue(entityIdByLookup, scope);
			}

			AddExpressionAttributeReference(attributeName,
			                                ruleId,
			                                source + ".aggregate",
			                                sourceEntityId,
			                                sourceRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			if(!condition.empty() && !IsLiteralExpressionArgument(condition))
			{
				ParsedFunctionCall conditionCall = ParseFunctionCall(condition);

				if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
				{
					CollectExpressionReferences(condition,
					                            ruleId,
					                            source + ".condition",
					                            sourceEntityId,
					                            sourceRelationshipId,
					                            references,
					                            functionCounts,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
				else
				{
					AddExpressionAttributeReference(RemoveMatchingQuotes(condition),
					                                ruleId,
					                                source + ".condition",
					                                sourceEntityId,
					                                sourceRelationshipId,
					                                references,
					                                attributeIdByEntityLookup);
				}
			}

			return;
		}

		if(call.name == "Not" && call.arguments.size() == 1)
		{
			const std::string argument = call.arguments[0];

			if(IsQuotedString(argument))
			{
				return;
			}

			ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

			if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
			{
				CollectExpressionReferences(argument,
				                            ruleId,
				                            source + ".expression",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				return;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
			                                ruleId,
			                                source + ".expression",
			                                defaultEntityId,
			                                defaultRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);

			return;
		}

		if(call.name == "For" && call.arguments.size() == 2)
		{
			const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
			const std::string attributeName = RemoveMatchingQuotes(call.arguments[1]);
			std::string sourceEntityId;
			std::string sourceRelationshipId;

			if(NormalizeLookup(scope).empty() || NormalizeLookup(scope) == "scope-ref")
			{
				sourceEntityId = defaultEntityId;
				sourceRelationshipId = defaultRelationshipId;
			}
			else
			{
				auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

				if(relationshipIt != relationshipByLookup.end())
				{
					const RelationshipIndexEntry& relationship = relationshipIt->second;

					sourceRelationshipId = relationship.relationshipId;

					if(relationship.fromEntityId == defaultEntityId)
					{
						sourceEntityId = relationship.toEntityId;
					}
					else if(relationship.toEntityId == defaultEntityId)
					{
						sourceEntityId = relationship.fromEntityId;
					}
					else
					{
						sourceEntityId = relationship.toEntityId;
					}
				}

				if(sourceEntityId.empty())
				{
					sourceEntityId = LookupValue(entityIdByLookup, scope);
				}
			}

			if(sourceEntityId.empty())
			{
				return;
			}

			auto attributeLookupIt = attributeIdByEntityLookup.find(sourceEntityId);

			if(attributeLookupIt == attributeIdByEntityLookup.end())
			{
				return;
			}

			const std::string sourceAttributeId = LookupValue(attributeLookupIt->second, attributeName);

			if(sourceAttributeId.empty())
			{
				return;
			}

			SessionModelImporter::RuleReference reference;

			reference.ruleId = ruleId;
			reference.source = source + ".expression";
			reference.entityId = sourceEntityId;
			reference.attributeId = sourceAttributeId;
			reference.relationshipId = sourceRelationshipId;

			references.push_back(reference);

			return;
		}

		for(const std::string& argument : call.arguments)
		{
			if(IsLiteralExpressionArgument(argument))
			{
				continue;
			}

			ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

			if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
			{
				CollectExpressionReferences(argument,
				                            ruleId,
				                            source + ".expression",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            functionCounts,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				continue;
			}

			AddExpressionAttributeReference(RemoveMatchingQuotes(argument),
			                                ruleId,
			                                source + ".expression",
			                                defaultEntityId,
			                                defaultRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);
		}
	}

	std::string AstArgumentValue(const json& argument)
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

		return FirstNonEmpty(argument, {
			"value",
			"attributeId",
			"attributeName",
			"coreName"
		});
	}

	bool AstArgumentLooksTemporalAnchor(const json& argument)
	{
		if(!argument.is_object())
		{
			return LooksLikeDateOrTimeLiteralValue(AstArgumentValue(argument));
		}

		const std::string functionName = argument.contains("nestedFunction") && argument.at("nestedFunction").is_object()
			? GetString(argument.at("nestedFunction"), "functionName")
			: GetString(argument, "functionName");

		if(functionName == "Date" || functionName == "DateTime")
		{
			return true;
		}

		const std::string valueMode = GetString(argument, "valueMode");

		if(valueMode.empty() || valueMode == "Literal")
		{
			return LooksLikeDateOrTimeLiteralValue(AstArgumentValue(argument));
		}

		return false;
	}

	std::string ResolveRelatedEntityId(const std::string& defaultEntityId, const RelationshipIndexEntry& relationship)
	{
		if(relationship.fromEntityId == defaultEntityId)
		{
			return relationship.toEntityId;
		}

		if(relationship.toEntityId == defaultEntityId)
		{
			return relationship.fromEntityId;
		}

		return !relationship.toEntityId.empty() ? relationship.toEntityId : relationship.fromEntityId;
	}

	std::string ResolveRelationshipId(const std::string& scope,
	                                  const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

		return relationshipIt == relationshipByLookup.end() ? "" : relationshipIt->second.relationshipId;
	}

	std::string AstRelationshipScopeValue(const json& argument)
	{
		if(argument.is_object())
		{
			if(argument.contains("relationshipScope") && argument.at("relationshipScope").is_object())
			{
				const std::string relationshipId = GetString(argument.at("relationshipScope"), "relationshipId");

				if(!relationshipId.empty())
				{
					return relationshipId;
				}
			}

			const std::string relationshipId = FirstNonEmpty(argument, {
				"relationshipId",
				"valueRelationshipId",
				"sourceRelationshipId",
				"comparisonRelationshipId",
				"relationshipName",
				"value"
			});

			if(!relationshipId.empty())
			{
				return relationshipId;
			}
		}

		return AstArgumentValue(argument);
	}

	bool IsZeroArgumentCalculatedValueFunctionName(const std::string& value)
	{
		return IsSessionContextFunctionName(value)
			|| value == "NewLine"
			|| value == "Quote";
	}

	bool IsPromotableCalculatedValueFunctionName(const std::string& value)
	{
		if(value.empty())
		{
			return false;
		}

		if(IsTemporalFunctionName(value))
		{
			return true;
		}

		return value == "Count"
			|| value == "For"
			|| value == "CountDistinct"
			|| value == "Sum"
			|| value == "Average"
			|| value == "Concatenate"
			|| value == "InstanceCount"
			|| value == "InstanceCountIf"
			|| value == "InstanceValueIf"
			|| IsInstanceAggregateFunctionName(value)
			|| value == "Not"
			|| value == "And"
			|| value == "Or"
			|| value == "If"
			|| value == "Equals"
			|| value == "NotEquals"
			|| value == "Known"
			|| value == "Certain"
			|| value == "Unknown"
			|| value == "Uncertain"
			|| value == "Exists"
			|| value == "ExistsScope"
			|| value == "ForAll"
			|| value == "ForAllScope"
			|| value == "IsMemberOf"
			|| value == "InstanceEquals"
			|| value == "InstanceNotEquals"
			|| value == "GreaterThan"
			|| value == "GreaterThanEquals"
			|| value == "GreaterThanOrEquals"
			|| value == "greaterThanOrEqual"
			|| value == "LessThan"
			|| value == "LessThanEquals"
			|| value == "LessThanOrEquals"
			|| value == "IsNumber"
			|| value == "Default"
			|| value == "DefaultWithUnknown"
			|| value == "Value"
			|| IsSessionContextFunctionName(value)
			|| value == "Date"
			|| value == "DateTime"
			|| value == "TimeOfDay"
			|| value == "AddDays"
			|| value == "AddWeeks"
			|| value == "AddMonths"
			|| value == "AddYears"
			|| value == "AddHours"
			|| value == "AddMinutes"
			|| value == "AddSeconds"
			|| value == "ConcatenateDateTime"
			|| value == "ExtractDate"
			|| value == "ExtractTimeOfDay"
			|| value == "ExtractDay"
			|| value == "ExtractMonth"
			|| value == "ExtractYear"
			|| value == "ExtractHour"
			|| value == "ExtractMinute"
			|| value == "ExtractSecond"
			|| value == "MakeDate"
			|| value == "DayDifference"
			|| value == "DayDifferenceExclusive"
			|| value == "DayDifferenceInclusive"
			|| value == "WeekDifference"
			|| value == "MonthDifference"
			|| value == "YearDifference"
			|| value == "HourDifference"
			|| value == "MinuteDifference"
			|| value == "SecondDifference"
			|| value == "SecondDifferenceExclusive"
			|| value == "SecondDifferenceInclusive"
			|| value == "NextDate"
			|| value == "NextDayOfTheWeek"
			|| value == "WeekdayCount"
			|| value == "YearStart"
			|| value == "YearEnd"
			|| value == "Contains"
			|| value == "EndsWith"
			|| value == "Find"
			|| value == "Length"
			|| value == "Lower"
			|| value == "NewLine"
			|| value == "Quote"
			|| value == "StartsWith"
			|| value == "Substring"
			|| value == "Text"
			|| value == "Upper"
			|| value == "Abs"
			|| value == "Acos"
			|| value == "Asin"
			|| value == "Atan"
			|| value == "Cos"
			|| value == "Ex"
			|| value == "Ln"
			|| value == "Log"
			|| value == "Maximum"
			|| value == "Minimum"
			|| value == "Number"
			|| value == "Remainder"
			|| value == "Round"
			|| value == "Sin"
			|| value == "Sqrt"
			|| value == "Tan"
			|| value == "Trunc"
			|| value == "Xy";
	}

	bool TryBuildAstExpressionString(const json& node, std::string& expression);

	bool LooksNumericLiteral(const std::string& value)
	{
		const std::string trimmed = Trim(value);
		if(trimmed.empty())
		{
			return false;
		}

		char* end = nullptr;
		std::strtod(trimmed.c_str(), &end);

		return end != nullptr && *end == '\0';
	}

	std::string QuoteExpressionStringLiteral(const std::string& value)
	{
		std::string quoted = "\"";

		for(char ch : value)
		{
			if(ch == '\\' || ch == '"')
			{
				quoted.push_back('\\');
			}

			quoted.push_back(ch);
		}

		quoted.push_back('"');

		return quoted;
	}

	bool TryGetAstLiteralArgumentValue(const json& argument, std::string& value)
	{
		if(!argument.is_object())
		{
			return false;
		}

		const std::string valueMode = FirstNonEmpty(argument, {"valueMode", "ValueMode"});
		if(NormalizeLookup(valueMode) != "literal")
		{
			return false;
		}

		const json* item = nullptr;
		if(argument.contains("value"))
		{
			item = &argument.at("value");
		}
		else if(argument.contains("Value"))
		{
			item = &argument.at("Value");
		}

		if(item == nullptr || item->is_null())
		{
			value = QuoteExpressionStringLiteral("");
			return true;
		}

		if(item->is_boolean())
		{
			value = item->get<bool>() ? "true" : "false";
			return true;
		}

		if(item->is_number())
		{
			value = item->dump();
			return true;
		}

		const std::string literal = item->is_string()
			? item->get<std::string>()
			: item->dump();

		if(IsQuotedString(literal))
		{
			value = Trim(literal);
			return true;
		}

		const std::string normalizedLiteral = NormalizeLookup(literal);

		if(normalizedLiteral == "true" || normalizedLiteral == "false" || LooksNumericLiteral(literal))
		{
			value = Trim(literal);
			return true;
		}

		value = QuoteExpressionStringLiteral(literal);

		return true;
	}

	std::string AstExpressionArgumentValue(const json& argument)
	{
		if(argument.is_object())
		{
			if(argument.contains("condition") && argument.at("condition").is_object())
			{
				return "";
			}

			if(argument.contains("nestedFunction") && argument.at("nestedFunction").is_object())
			{
				std::string nestedExpression;

				if(TryBuildAstExpressionString(argument.at("nestedFunction"), nestedExpression))
				{
					return nestedExpression;
				}
			}

			if(argument.contains("functionName"))
			{
				std::string nestedExpression;

				if(TryBuildAstExpressionString(argument, nestedExpression))
				{
					return nestedExpression;
				}
			}

			if(argument.contains("relationshipScope") || argument.contains("relationshipId") || argument.contains("relationshipName"))
			{
				return AstRelationshipScopeValue(argument);
			}

			std::string literalValue;
			if(TryGetAstLiteralArgumentValue(argument, literalValue))
			{
				return literalValue;
			}
		}

		return AstArgumentValue(argument);
	}

	bool FunctionFirstArgumentIsScope(const std::string& functionName)
	{
		return functionName == "For"
			|| functionName == "Exists"
			|| functionName == "ExistsScope"
			|| functionName == "ForAll"
			|| functionName == "ForAllScope"
			|| functionName == "ForScope"
			|| functionName == "ForScopeAt"
			|| functionName == "InferInstance"
			|| functionName == "InstanceCount"
			|| functionName == "InstanceCountIf"
			|| functionName == "InstanceValueIf"
			|| IsInstanceAggregateFunctionName(functionName);
	}

	std::string AstExpressionArgumentValue(const std::string& functionName, size_t argumentIndex, const json& argument)
	{
		if(argumentIndex == 0 && FunctionFirstArgumentIsScope(functionName))
		{
			if(argument.is_object())
			{
				std::string scopeValue = AstRelationshipScopeValue(argument);
				if(!scopeValue.empty())
				{
					return scopeValue;
				}
			}

			return AstArgumentValue(argument);
		}

		return AstExpressionArgumentValue(argument);
	}

	bool TryBuildAstExpressionString(const json& node, std::string& expression)
	{
		if(!node.is_object())
		{
			return false;
		}

		const std::string functionName = GetString(node, "functionName");

		if(functionName.empty())
		{
			return false;
		}

		const bool hasFunctionArguments = node.contains("functionArguments");

		if(hasFunctionArguments && !node.at("functionArguments").is_array())
		{
			return false;
		}

		if(!hasFunctionArguments && !IsZeroArgumentCalculatedValueFunctionName(functionName))
		{
			return false;
		}

		std::ostringstream builder;

		builder << functionName << "(";

		const json emptyArguments = json::array();
		const json& arguments = hasFunctionArguments ? node.at("functionArguments") : emptyArguments;

		for(size_t index = 0; index < arguments.size(); ++index)
		{
			const std::string argumentValue = AstExpressionArgumentValue(functionName, index, arguments.at(index));

			if(argumentValue.empty())
			{
				return false;
			}

			if(index > 0)
			{
				builder << ", ";
			}

			builder << argumentValue;
		}

		builder << ")";
		expression = builder.str();

		return true;
	}

	bool TryBuildPromotedCalculatedValueExpression(const json& condition, std::string& expression)
	{
		if(!condition.is_object())
		{
			return false;
		}

		const std::string nodeType = GetString(condition, "nodeType");

		if(nodeType == "group")
		{
			const std::string junction = NormalizeLookup(GetString(condition, "junction"));

			if(junction != "and" || !condition.contains("children") || !condition.at("children").is_array() || condition.at("children").size() != 1)
			{
				return false;
			}

			return TryBuildPromotedCalculatedValueExpression(condition.at("children").at(0), expression);
		}

		if(nodeType != "expression" && !nodeType.empty())
		{
			return false;
		}

		const std::string functionName = GetString(condition, "functionName");

		if(!IsPromotableCalculatedValueFunctionName(functionName))
		{
			return false;
		}

		return TryBuildAstExpressionString(condition, expression);
	}

	bool TryResolveForExpressionReferences(
		const std::string& expression,
		const std::string& defaultEntityId,
		const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
		const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup,
		std::string& relationshipId,
		std::string& attributeId)
	{
		const ParsedFunctionCall call = ParseFunctionCall(expression);

		if(!call.valid || call.name != "For" || call.arguments.size() != 2)
		{
			return false;
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		auto relationshipIt = relationshipByLookup.find(NormalizeLookup(scope));

		if(relationshipIt == relationshipByLookup.end())
		{
			return false;
		}

		const RelationshipIndexEntry& relationship = relationshipIt->second;
		std::string relatedEntityId;

		if(relationship.fromEntityId == defaultEntityId)
		{
			relatedEntityId = relationship.toEntityId;
		}
		else if(relationship.toEntityId == defaultEntityId)
		{
			relatedEntityId = relationship.fromEntityId;
		}
		else
		{
			return false;
		}

		auto attributeLookupIt = attributeIdByEntityLookup.find(relatedEntityId);

		if(attributeLookupIt == attributeIdByEntityLookup.end())
		{
			return false;
		}

		const std::string resolvedAttributeId = LookupValue(
			attributeLookupIt->second,
			RemoveMatchingQuotes(call.arguments[1]));

		if(resolvedAttributeId.empty())
		{
			return false;
		}

		relationshipId = relationship.relationshipId;
		attributeId = resolvedAttributeId;

		return true;
	}

	bool CalculatedConclusionMatchesPromotedExpression(
		const std::string& conclusionValue,
		const std::string& promotedExpression,
		const std::string& defaultEntityId,
		const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
		const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		if(NormalizeLookup(conclusionValue) == NormalizeLookup(promotedExpression))
		{
			return true;
		}

		std::string conclusionRelationshipId;
		std::string conclusionAttributeId;
		std::string promotedRelationshipId;
		std::string promotedAttributeId;

		return TryResolveForExpressionReferences(
				conclusionValue,
				defaultEntityId,
				attributeIdByEntityLookup,
				relationshipByLookup,
				conclusionRelationshipId,
				conclusionAttributeId)
			&& TryResolveForExpressionReferences(
				promotedExpression,
				defaultEntityId,
				attributeIdByEntityLookup,
				relationshipByLookup,
				promotedRelationshipId,
				promotedAttributeId)
			&& conclusionRelationshipId == promotedRelationshipId
			&& conclusionAttributeId == promotedAttributeId;
	}

	std::string ResolveAstRelationshipId(const json& argument,
	                                     const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		return ResolveRelationshipId(AstRelationshipScopeValue(argument), relationshipByLookup);
	}

	void ResolveAstScope(const json& argument,
	                     const std::string& defaultEntityId,
	                     const std::string& defaultRelationshipId,
	                     const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                     const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup,
	                     std::string& scopedEntityId,
	                     std::string& scopedRelationshipId)
	{
		if(argument.is_object() && argument.contains("relationshipScope") && argument.at("relationshipScope").is_object())
		{
			const json& relationshipScope = argument.at("relationshipScope");
			const std::string relationshipId = GetString(relationshipScope, "relationshipId");
			const std::string sourceEntityId = GetString(relationshipScope, "sourceEntityId");
			const std::string targetEntityId = GetString(relationshipScope, "targetEntityId");

			if(!relationshipId.empty())
			{
				scopedRelationshipId = relationshipId;
			}

			if(defaultEntityId == sourceEntityId)
			{
				scopedEntityId = targetEntityId;
			}
			else if(defaultEntityId == targetEntityId)
			{
				scopedEntityId = sourceEntityId;
			}
			else
			{
				scopedEntityId = !targetEntityId.empty() ? targetEntityId : sourceEntityId;
			}

			if(!scopedEntityId.empty() || !scopedRelationshipId.empty())
			{
				return;
			}
		}

		const std::string scope = AstArgumentValue(argument);
		const std::string normalizedScope = NormalizeLookup(scope);

		if(normalizedScope.empty() || normalizedScope == "scope-ref")
		{
			scopedEntityId = defaultEntityId;
			scopedRelationshipId = defaultRelationshipId;

			return;
		}

		auto relationshipIt = relationshipByLookup.find(normalizedScope);

		if(relationshipIt != relationshipByLookup.end())
		{
			scopedRelationshipId = relationshipIt->second.relationshipId;
			scopedEntityId = ResolveRelatedEntityId(defaultEntityId, relationshipIt->second);

			return;
		}

		if(scopedEntityId.empty())
		{
			scopedEntityId = LookupValue(entityIdByLookup, scope);
		}
	}

	void AddAstAttributeReference(const json& argument,
	                              const std::string& ruleId,
	                              const std::string& source,
	                              const std::string& defaultEntityId,
	                              const std::string& defaultRelationshipId,
	                              std::vector<SessionModelImporter::RuleReference>& references,
	                              const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup)
	{
		if(!argument.is_object() || GetString(argument, "valueMode") != "Attribute")
		{
			return;
		}

		std::string entityId = FirstNonEmpty(argument, {
			"entityId",
			"valueEntityId",
			"sourceEntityId",
			"comparisonEntityId"
		});

		if(entityId.empty())
		{
			entityId = defaultEntityId;
		}

		std::string relationshipId = FirstNonEmpty(argument, {
			"relationshipId",
			"valueRelationshipId",
			"sourceRelationshipId",
			"comparisonRelationshipId"
		});

		if(relationshipId.empty())
		{
			relationshipId = defaultRelationshipId;
		}

		AddExpressionAttributeReference(AstArgumentValue(argument),
		                                ruleId,
		                                source,
		                                entityId,
		                                relationshipId,
		                                references,
		                                attributeIdByEntityLookup);
	}

	void CollectAstArgumentReferences(const json& argument,
	                                  const std::string& ruleId,
	                                  const std::string& source,
	                                  const std::string& defaultEntityId,
	                                  const std::string& defaultRelationshipId,
	                                  std::vector<SessionModelImporter::RuleReference>& references,
	                                  const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                                  const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
	                                  const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup);

	void CollectAstReferences(const json& node,
	                          const std::string& ruleId,
	                          const std::string& source,
	                          const std::string& defaultEntityId,
	                          const std::string& defaultRelationshipId,
	                          std::vector<SessionModelImporter::RuleReference>& references,
	                          const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                          const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
	                          const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup);

	void AddAstRelationshipReference(const std::string& ruleId,
	                                 const std::string& source,
	                                 const std::string& entityId,
	                                 const std::string& relationshipId,
	                                 std::vector<SessionModelImporter::RuleReference>& references)
	{
		if(relationshipId.empty())
		{
			return;
		}

		SessionModelImporter::RuleReference reference;

		reference.ruleId = ruleId;
		reference.source = source;
		reference.entityId = entityId;
		reference.relationshipId = relationshipId;

		references.push_back(reference);
	}

	void CollectAstFunctionReferences(const json& node,
	                                  const std::string& ruleId,
	                                  const std::string& source,
	                                  const std::string& defaultEntityId,
	                                  const std::string& defaultRelationshipId,
	                                  std::vector<SessionModelImporter::RuleReference>& references,
	                                  const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                                  const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
	                                  const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		if(!node.is_object())
		{
			return;
		}

		const std::string functionName = GetString(node, "functionName");

		if(functionName.empty() || !node.contains("functionArguments") || !node.at("functionArguments").is_array())
		{
			return;
		}

		const json& arguments = node.at("functionArguments");

		if(functionName == "ValueAt" && arguments.size() == 2)
		{
			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".temporal",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			CollectAstArgumentReferences(arguments.at(1),
			                            ruleId,
			                            source + ".time",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if((functionName == "Earliest" || functionName == "Latest" || functionName == "WhenLast" || functionName == "WhenNext")
			&& !arguments.empty() && arguments.size() <= 2)
		{
			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".temporal",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			if(arguments.size() == 2)
			{
				CollectAstArgumentReferences(arguments.at(1),
				                            ruleId,
				                            source + ".time",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(IsTemporalRelativeWindowFunctionName(functionName) && (arguments.size() == 2 || arguments.size() == 3))
		{
			const size_t conditionIndex = arguments.size() == 3 ? 2 : 1;

			if(functionName == "TemporalConsecutiveDays"
				&& arguments.size() == 3
				&& !AstArgumentLooksTemporalAnchor(arguments.at(0)))
			{
				AddFunctionReference(ruleId,
				                     source + ".currentDate",
				                     "CurrentDate",
				                     references);
			}

			for(size_t index = 0; index < arguments.size(); ++index)
			{
				CollectAstArgumentReferences(arguments.at(index),
				                            ruleId,
				                            source + (index == conditionIndex ? ".temporal" : ".time"),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(functionName == "TemporalDaysSince" && arguments.size() == 2)
		{
			for(const json& argument : arguments)
			{
				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + ".time",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(IsTemporalElapsedDateUnitFunctionName(functionName) && arguments.size() == 2)
		{
			for(const json& argument : arguments)
			{
				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + ".expression",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(functionName == "TemporalOncePerMonth" && arguments.size() == 2)
		{
			AddFunctionReference(ruleId,
			                     source + ".currentDate",
			                     "CurrentDate",
			                     references);

			for(size_t index = 0; index < arguments.size(); ++index)
			{
				const json& argument = arguments.at(index);
				const bool directCondition = index == 1
					&& argument.is_object()
					&& !argument.contains("nestedFunction")
					&& !argument.contains("functionName");

				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + (directCondition ? ".temporal" : ".expression"),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(functionName == "TemporalOncePerMonth" && arguments.size() == 3)
		{
			AddFunctionReference(ruleId,
			                     source + ".currentDate",
			                     "CurrentDate",
			                     references);

			for(size_t index = 0; index < arguments.size(); ++index)
			{
				const json& argument = arguments.at(index);
				const bool possibleCondition = index == 2
					&& argument.is_object()
					&& !argument.contains("nestedFunction")
					&& !argument.contains("functionName");

				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + (possibleCondition ? ".temporal" : ".expression"),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(functionName == "TemporalOncePerMonth" && arguments.size() == 4)
		{
			for(size_t index = 0; index < arguments.size(); ++index)
			{
				const json& argument = arguments.at(index);
				const bool isCondition = index == 3;
				const bool directCondition = isCondition
					&& argument.is_object()
					&& !argument.contains("nestedFunction")
					&& !argument.contains("functionName");

				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + (directCondition ? ".temporal" : ".expression"),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(IsTemporalScalarDatePredicateFunctionName(functionName) && arguments.size() == 1)
		{
			if(IsTemporalCurrentDateBoundaryPredicateFunctionName(functionName))
			{
				AddFunctionReference(ruleId,
				                     source + ".currentDate",
				                     "CurrentDate",
				                     references);
			}

			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".expression",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if(functionName == "TemporalFromRange" && arguments.size() == 2)
		{
			AddFunctionReference(ruleId,
			                     source + ".currentDate",
			                     "CurrentDate",
			                     references);

			for(const json& argument : arguments)
			{
				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + ".expression",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(IsTemporalScalarDateComparisonFunctionName(functionName) && arguments.size() == 1)
		{
			if(IsTemporalCurrentDateComparisonPredicateFunctionName(functionName))
			{
				AddFunctionReference(ruleId,
				                     source + ".currentDate",
				                     "CurrentDate",
				                     references);
			}

			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".expression",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if(IsTemporalScalarDateComparisonFunctionName(functionName) && arguments.size() == 2)
		{
			for(const json& argument : arguments)
			{
				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + ".expression",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if((IsTemporalExtremaFunctionName(functionName)
			|| IsTemporalWeightedAggregateFunctionName(functionName)
			|| IsTemporalCountDistinctAggregateFunctionName(functionName)) && arguments.size() == 3)
		{
			for(size_t index = 0; index < arguments.size(); ++index)
			{
				CollectAstArgumentReferences(arguments.at(index),
				                            ruleId,
				                            source + (index == 2 ? ".temporal" : ".time"),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if((IsTemporalConditionalAggregateFunctionName(functionName)
			|| IsTemporalConditionalWeightedAggregateFunctionName(functionName)
			|| IsTemporalConditionalCountDistinctAggregateFunctionName(functionName)) && arguments.size() >= 3)
		{
			for(size_t index = 0; index < arguments.size(); ++index)
			{
				CollectAstArgumentReferences(arguments.at(index),
				                            ruleId,
				                            source + ((index == 2 || index == 3) ? ".temporal" : (index < 2 ? ".time" : ".expression")),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(IsTemporalIntervalFunctionName(functionName)
			&& arguments.size() == (IsTemporalDayCountFunctionName(functionName) ? 4 : 3))
		{
			const bool hasDays = IsTemporalDayCountFunctionName(functionName);
			const size_t exportedConditionIndex = hasDays ? 3 : 2;
			const bool exportedConditionIsAttribute = arguments.at(exportedConditionIndex).is_object()
				&& GetString(arguments.at(exportedConditionIndex), "valueMode") == "Attribute";
			const bool compatibilityConditionIsAttribute = arguments.at(0).is_object()
				&& GetString(arguments.at(0), "valueMode") == "Attribute";
			const size_t conditionIndex = exportedConditionIsAttribute && !compatibilityConditionIsAttribute ? exportedConditionIndex : 0;

			for(size_t index = 0; index < arguments.size(); ++index)
			{
				CollectAstArgumentReferences(arguments.at(index),
				                            ruleId,
				                            source + (index == conditionIndex ? ".temporal" : ".time"),
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(functionName == "Filter" && arguments.size() == 2)
		{
			const json& collectionArgument = arguments.at(0);
			const json* collectionFunction = nullptr;

			if(collectionArgument.is_object() && collectionArgument.contains("nestedFunction") && collectionArgument.at("nestedFunction").is_object())
			{
				collectionFunction = &collectionArgument.at("nestedFunction");
			}
			else if(collectionArgument.is_object() && collectionArgument.contains("functionName"))
			{
				collectionFunction = &collectionArgument;
			}

			if(collectionFunction == nullptr)
			{
				return;
			}

			CollectAstFunctionReferences(*collectionFunction,
			                             ruleId,
			                             source + ".collection",
			                             defaultEntityId,
			                             defaultRelationshipId,
			                             references,
			                             entityIdByLookup,
			                             attributeIdByEntityLookup,
			                             relationshipByLookup);

			const std::string collectionFunctionName = GetString(*collectionFunction, "functionName");

			if((collectionFunctionName != "ForScope" && collectionFunctionName != "ForScopeAt")
				|| !collectionFunction->contains("functionArguments")
				|| !collectionFunction->at("functionArguments").is_array()
				|| collectionFunction->at("functionArguments").empty())
			{
				return;
			}

			const json& collectionArguments = collectionFunction->at("functionArguments");
			std::string scopedEntityId;
			std::string scopedRelationshipId;

			ResolveAstScope(collectionArguments.at(0),
			                defaultEntityId,
			                defaultRelationshipId,
			                entityIdByLookup,
			                relationshipByLookup,
			                scopedEntityId,
			                scopedRelationshipId);

			CollectAstArgumentReferences(arguments.at(1),
			                            ruleId,
			                            source + ".condition",
			                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
			                            scopedRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if(functionName == "IsMemberOfAt" && (arguments.size() == 3 || arguments.size() == 4))
		{
			const size_t relationshipIndex = arguments.size() == 4 ? 2 : 1;
			const size_t timeIndex = arguments.size() == 4 ? 3 : 2;
			const std::string relationshipId = ResolveAstRelationshipId(arguments.at(relationshipIndex), relationshipByLookup);
			const std::string scopedRelationshipId = !relationshipId.empty() ? relationshipId : defaultRelationshipId;

			AddAstRelationshipReference(ruleId, source + ".temporalRelationship", defaultEntityId, relationshipId, references);

			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".target",
			                            defaultEntityId,
			                            scopedRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			if(relationshipId.empty())
			{
				CollectAstArgumentReferences(arguments.at(relationshipIndex),
				                            ruleId,
				                            source + ".relationship",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			CollectAstArgumentReferences(arguments.at(timeIndex),
			                            ruleId,
			                            source + ".time",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if((functionName == "ForScopeAt" || functionName == "ForAllScopeAt" || functionName == "ForAllAt") && (arguments.size() == 3 || arguments.size() == 4))
		{
			const bool isForScopeAt = functionName == "ForScopeAt";
			const size_t timeIndex = arguments.size() == 4 ? 2 : 1;
			const size_t expressionIndex = arguments.size() == 4 ? 3 : 2;
			const std::string scopedSource = source + (isForScopeAt ? ".expression" : ".condition");
			std::string scopedEntityId;
			std::string scopedRelationshipId;

			ResolveAstScope(arguments.at(0),
			                defaultEntityId,
			                defaultRelationshipId,
			                entityIdByLookup,
			                relationshipByLookup,
			                scopedEntityId,
			                scopedRelationshipId);

			AddAstRelationshipReference(ruleId, source + ".temporalRelationship", defaultEntityId, scopedRelationshipId, references);

			if(scopedRelationshipId.empty())
			{
				CollectAstArgumentReferences(arguments.at(0),
				                            ruleId,
				                            source + ".relationship",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			CollectAstArgumentReferences(arguments.at(timeIndex),
			                            ruleId,
			                            source + ".time",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			CollectAstArgumentReferences(arguments.at(expressionIndex),
			                            ruleId,
			                            scopedSource,
			                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
			                            scopedRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if(functionName == "IsMemberOf" && (arguments.size() == 2 || arguments.size() == 3))
		{
			const size_t relationshipIndex = arguments.size() == 3 ? 2 : 1;
			const std::string relationshipId = ResolveAstRelationshipId(arguments.at(relationshipIndex), relationshipByLookup);
			const std::string scopedRelationshipId = !relationshipId.empty() ? relationshipId : defaultRelationshipId;

			AddAstRelationshipReference(ruleId, source + ".collection", defaultEntityId, relationshipId, references);

			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".target",
			                            defaultEntityId,
			                            scopedRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			if(relationshipId.empty())
			{
				CollectAstArgumentReferences(arguments.at(relationshipIndex),
				                            ruleId,
				                            source + ".relationship",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}

			return;
		}

		if(functionName == "InferInstance" && arguments.size() == 1)
		{
			std::string scopedEntityId;
			std::string scopedRelationshipId;

			ResolveAstScope(arguments.at(0),
			                defaultEntityId,
			                defaultRelationshipId,
			                entityIdByLookup,
			                relationshipByLookup,
			                scopedEntityId,
			                scopedRelationshipId);

			AddAstRelationshipReference(ruleId, source + ".relationship", defaultEntityId, scopedRelationshipId, references);

			return;
		}

		if(functionName == "InferInstanceFor" && (arguments.size() == 2 || arguments.size() == 3))
		{
			std::string scopedEntityId;
			std::string scopedRelationshipId;
			const size_t conditionIndex = arguments.size() == 3 ? 2 : 1;

			ResolveAstScope(arguments.at(0),
			                defaultEntityId,
			                defaultRelationshipId,
			                entityIdByLookup,
			                relationshipByLookup,
			                scopedEntityId,
			                scopedRelationshipId);

			AddAstRelationshipReference(ruleId, source + ".relationship", defaultEntityId, scopedRelationshipId, references);

			CollectAstArgumentReferences(arguments.at(conditionIndex),
			                            ruleId,
			                            source + ".condition",
			                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
			                            scopedRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if((functionName == "Error" || functionName == "Warning") && arguments.size() == 1)
		{
			CollectAstArgumentReferences(arguments.at(0),
			                            ruleId,
			                            source + ".message",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if((functionName == "Exists" || functionName == "ExistsScope" || functionName == "ForAll" || functionName == "ForAllScope" || functionName == "ForScope" || functionName == "InstanceCount" || functionName == "InstanceCountIf" || functionName == "InstanceValueIf" || IsInstanceAggregateFunctionName(functionName))
			&& !arguments.empty())
		{
			std::string scopedEntityId;
			std::string scopedRelationshipId;

			ResolveAstScope(arguments.at(0),
			                defaultEntityId,
			                defaultRelationshipId,
			                entityIdByLookup,
			                relationshipByLookup,
			                scopedEntityId,
			                scopedRelationshipId);

			AddAstRelationshipReference(ruleId, source + ".collection", defaultEntityId, scopedRelationshipId, references);

			if(functionName == "ForScope" && arguments.size() >= 2)
			{
				const size_t expressionIndex = arguments.size() >= 3 ? 2 : 1;

				CollectAstArgumentReferences(arguments.at(expressionIndex),
				                            ruleId,
				                            source + ".expression",
				                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
				                            scopedRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}
			else if((functionName == "Exists" || functionName == "ExistsScope" || functionName == "ForAll" || functionName == "ForAllScope") && arguments.size() >= 2)
			{
				const size_t conditionIndex = arguments.size() >= 3 ? 2 : 1;

				CollectAstArgumentReferences(arguments.at(conditionIndex),
				                            ruleId,
				                            source + ".condition",
				                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
				                            scopedRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}
			else if(functionName == "InstanceCountIf" && arguments.size() >= 2)
			{
				CollectAstArgumentReferences(arguments.at(1),
				                            ruleId,
				                            source + ".condition",
				                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
				                            scopedRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}
			else if(functionName == "InstanceValueIf" && arguments.size() >= 3)
			{
				CollectAstArgumentReferences(arguments.at(1),
				                            ruleId,
				                            source + ".expression",
				                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
				                            scopedRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);

				CollectAstArgumentReferences(arguments.at(2),
				                            ruleId,
				                            source + ".condition",
				                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
				                            scopedRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}
			else if(IsInstanceAggregateFunctionName(functionName) && arguments.size() >= 2)
			{
				AddAstAttributeReference(arguments.at(1),
				                         ruleId,
				                         source + ".aggregate",
				                         scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
				                         scopedRelationshipId,
				                         references,
				                         attributeIdByEntityLookup);

				if(IsConditionalInstanceAggregateFunctionName(functionName) && arguments.size() >= 3)
				{
					CollectAstArgumentReferences(arguments.at(2),
					                            ruleId,
					                            source + ".condition",
					                            scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
					                            scopedRelationshipId,
					                            references,
					                            entityIdByLookup,
					                            attributeIdByEntityLookup,
					                            relationshipByLookup);
				}
			}

			return;
		}

		if(functionName == "For" && arguments.size() == 2)
		{
			std::string scopedEntityId;
			std::string scopedRelationshipId;

			ResolveAstScope(arguments.at(0),
			                defaultEntityId,
			                defaultRelationshipId,
			                entityIdByLookup,
			                relationshipByLookup,
			                scopedEntityId,
			                scopedRelationshipId);

			AddAstAttributeReference(arguments.at(1),
			                         ruleId,
			                         source + ".expression",
			                         scopedEntityId.empty() ? defaultEntityId : scopedEntityId,
			                         scopedRelationshipId,
			                         references,
			                         attributeIdByEntityLookup);

			return;
		}

		for(const json& argument : arguments)
		{
			CollectAstArgumentReferences(argument,
			                            ruleId,
			                            source + ".expression",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);
		}
	}

	void CollectAstArgumentReferences(const json& argument,
	                                  const std::string& ruleId,
	                                  const std::string& source,
	                                  const std::string& defaultEntityId,
	                                  const std::string& defaultRelationshipId,
	                                  std::vector<SessionModelImporter::RuleReference>& references,
	                                  const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                                  const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
	                                  const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		if(!argument.is_object())
		{
			return;
		}

		if(argument.contains("condition") && argument.at("condition").is_object())
		{
			CollectAstReferences(argument.at("condition"),
			                     ruleId,
			                     source,
			                     defaultEntityId,
			                     defaultRelationshipId,
			                     references,
			                     entityIdByLookup,
			                     attributeIdByEntityLookup,
			                     relationshipByLookup);

			return;
		}

		if(argument.contains("nestedFunction") && argument.at("nestedFunction").is_object())
		{
			CollectAstFunctionReferences(argument.at("nestedFunction"),
			                            ruleId,
			                            source,
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		if(argument.contains("functionName"))
		{
			CollectAstFunctionReferences(argument,
			                            ruleId,
			                            source,
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);

			return;
		}

		AddAstAttributeReference(argument,
		                         ruleId,
		                         source,
		                         defaultEntityId,
		                         defaultRelationshipId,
		                         references,
		                         attributeIdByEntityLookup);
	}

	void CollectAstReferences(const json& node,
	                          const std::string& ruleId,
	                          const std::string& source,
	                          const std::string& defaultEntityId,
	                          const std::string& defaultRelationshipId,
	                          std::vector<SessionModelImporter::RuleReference>& references,
	                          const std::unordered_map<std::string, std::string>& entityIdByLookup,
	                          const std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& attributeIdByEntityLookup,
	                          const std::unordered_map<std::string, RelationshipIndexEntry>& relationshipByLookup)
	{
		if(!node.is_object())
		{
			return;
		}

		const std::string operatorName = NormalizeLookup(GetString(node, "operator"));
		std::string nodeEntityId = FirstNonEmpty(node, {
			"entityId",
			"valueEntityId",
			"sourceEntityId",
			"comparisonEntityId"
		});

		if(nodeEntityId.empty())
		{
			nodeEntityId = defaultEntityId;
		}

		std::string nodeRelationshipId = FirstNonEmpty(node, {
			"relationshipId",
			"valueRelationshipId",
			"sourceRelationshipId",
			"comparisonRelationshipId"
		});

		if(nodeRelationshipId.empty())
		{
			nodeRelationshipId = defaultRelationshipId;
		}

		const std::string attributeId = GetString(node, "attributeId");
		if(!attributeId.empty() && operatorName != "any")
		{
			AddExpressionAttributeReference(attributeId,
			                                ruleId,
			                                source,
			                                nodeEntityId,
			                                nodeRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);
		}

		const std::string compareAttributeId = GetString(node, "compareAttributeId");
		if(!compareAttributeId.empty() && operatorName != "any")
		{
			std::string compareEntityId = FirstNonEmpty(node, {
				"compareEntityId",
				"comparisonEntityId",
				"valueEntityId"
			});

			if(compareEntityId.empty())
			{
				compareEntityId = nodeEntityId;
			}

			std::string compareRelationshipId = FirstNonEmpty(node, {
				"compareRelationshipId",
				"comparisonRelationshipId",
				"valueRelationshipId"
			});

			if(compareRelationshipId.empty())
			{
				compareRelationshipId = nodeRelationshipId;
			}

			AddExpressionAttributeReference(compareAttributeId,
			                                ruleId,
			                                source + ".compare",
			                                compareEntityId,
			                                compareRelationshipId,
			                                references,
			                                attributeIdByEntityLookup);
		}

		if(node.contains("functionName"))
		{
			CollectAstFunctionReferences(node,
			                            ruleId,
			                            source,
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);
		}

		if(node.contains("children") && node.at("children").is_array())
		{
			for(const json& child : node.at("children"))
			{
				CollectAstReferences(child,
				                     ruleId,
				                     source + ".children",
				                     defaultEntityId,
				                     defaultRelationshipId,
				                     references,
				                     entityIdByLookup,
				                     attributeIdByEntityLookup,
				                     relationshipByLookup);
			}
		}

		if(node.contains("functionArguments") && node.at("functionArguments").is_array())
		{
			for(const json& argument : node.at("functionArguments"))
			{
				CollectAstArgumentReferences(argument,
				                            ruleId,
				                            source + ".functionArguments",
				                            defaultEntityId,
				                            defaultRelationshipId,
				                            references,
				                            entityIdByLookup,
				                            attributeIdByEntityLookup,
				                            relationshipByLookup);
			}
		}

		if(node.contains("nestedFunction") && node.at("nestedFunction").is_object())
		{
			CollectAstFunctionReferences(node.at("nestedFunction"),
			                            ruleId,
			                            source + ".nestedFunction",
			                            defaultEntityId,
			                            defaultRelationshipId,
			                            references,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);
		}
	}

	void CollectReferences(const json& node,
	                       const std::string& ruleId,
	                       const std::string& source,
	                       std::vector<SessionModelImporter::RuleReference>& references,
	                       std::unordered_map<std::string, int>& operatorCounts,
	                       std::unordered_map<std::string, int>& functionCounts)
	{
		if(!node.is_object())
		{
			return;
		}

		const std::string entityId = GetString(node, "entityId");
		const std::string attributeId = GetString(node, "attributeId");
		const std::string relationshipId = GetString(node, "relationshipId");
		const std::string nodeType = GetString(node, "nodeType");
		const std::string operatorName = GetString(node, "operator");
		const std::string functionName = GetString(node, "functionName");
		const std::string valueMode = GetString(node, "valueMode");

		AddCount(operatorCounts, operatorName);
		AddCount(functionCounts, functionName);

		if(IsSessionContextFunctionName(functionName))
		{
			SessionModelImporter::RuleReference reference;

			reference.ruleId = ruleId;
			reference.source = source + ".function";
			reference.functionName = functionName;

			references.push_back(reference);
		}

		if(operatorName != "any" && (!entityId.empty() || !attributeId.empty() || !relationshipId.empty()))
		{
			SessionModelImporter::RuleReference reference;

			reference.ruleId = ruleId;
			reference.source = source;
			reference.entityId = entityId;
			reference.attributeId = attributeId;
			reference.relationshipId = relationshipId;
			reference.nodeType = nodeType;
			reference.operatorName = operatorName;
			reference.functionName = functionName;

			references.push_back(reference);
		}

		if(valueMode == "Attribute")
		{
			std::string sourceAttributeId = FirstNonEmpty(node, {
				"valueAttributeId",
				"sourceAttributeId",
				"comparisonAttributeId",
				"attributeValueId"
			});

			if(sourceAttributeId.empty())
			{
				sourceAttributeId = GetString(node, "value");
			}

			std::string sourceEntityId = FirstNonEmpty(node, {
				"valueEntityId",
				"sourceEntityId",
				"comparisonEntityId"
			});

			if(sourceEntityId.empty())
			{
				sourceEntityId = entityId;
			}

			const std::string sourceRelationshipId = FirstNonEmpty(node, {
				"valueRelationshipId",
				"sourceRelationshipId",
				"comparisonRelationshipId"
			});

			if(!sourceEntityId.empty() || !sourceAttributeId.empty() || !sourceRelationshipId.empty())
			{
				SessionModelImporter::RuleReference reference;

				reference.ruleId = ruleId;
				reference.source = source + ".value";
				reference.entityId = sourceEntityId;
				reference.attributeId = sourceAttributeId;
				reference.relationshipId = sourceRelationshipId;
				reference.nodeType = nodeType;
				reference.operatorName = operatorName;
				reference.functionName = functionName;

				references.push_back(reference);
			}
		}

		if(node.contains("children") && node.at("children").is_array())
		{
			for(const json& child : node.at("children"))
			{
				CollectReferences(child, ruleId, source + ".children", references, operatorCounts, functionCounts);
			}
		}

		if(node.contains("functionArguments") && node.at("functionArguments").is_array())
		{
			for(const json& argument : node.at("functionArguments"))
			{
				CollectReferences(argument, ruleId, source + ".functionArguments", references, operatorCounts, functionCounts);

				if(argument.is_object() && argument.contains("nestedFunction"))
				{
					CollectReferences(argument.at("nestedFunction"), ruleId, source + ".nestedFunction", references, operatorCounts, functionCounts);
				}

				if(argument.is_object() && argument.contains("condition"))
				{
					CollectReferences(argument.at("condition"), ruleId, source + ".condition", references, operatorCounts, functionCounts);
				}
			}
		}

		if(node.contains("nestedFunction"))
		{
			CollectReferences(node.at("nestedFunction"), ruleId, source + ".nestedFunction", references, operatorCounts, functionCounts);
		}
	}

	std::string ReferenceContext(const SessionModelImporter::RuleReference& reference)
	{
		std::ostringstream context;

		context << "rule:" << reference.ruleId;

		if(!reference.source.empty())
		{
			context << " source:" << reference.source;
		}

		if(!reference.entityId.empty())
		{
			context << " entity:" << reference.entityId;
		}

		if(!reference.attributeId.empty())
		{
			context << " attribute:" << reference.attributeId;
		}

		if(!reference.relationshipId.empty())
		{
			context << " relationship:" << reference.relationshipId;
		}

		return context.str();
	}

	void DeduplicateRuleReferences(std::vector<SessionModelImporter::RuleReference>& references)
	{
		std::unordered_set<std::string> seen;
		std::vector<SessionModelImporter::RuleReference> uniqueReferences;

		uniqueReferences.reserve(references.size());

		for(const SessionModelImporter::RuleReference& reference : references)
		{
			const std::string key = reference.source
				+ "\x1f" + reference.entityId
				+ "\x1f" + reference.attributeId
				+ "\x1f" + reference.relationshipId
				+ "\x1f" + reference.functionName;

			if(seen.insert(key).second)
			{
				uniqueReferences.push_back(reference);
			}
		}

		references.swap(uniqueReferences);
	}
}

std::vector<std::string> SessionModelImporter::SessionModel::GetActiveRulesReferencingAttribute(const std::string& entityId, const std::string& attributeId) const
{
	auto it = activeRuleIdsByReferencedAttribute.find(RuleSet::AttributeDependencyKey(entityId, attributeId));

	return it == activeRuleIdsByReferencedAttribute.end() ? std::vector<std::string>() : it->second;
}

std::vector<std::string> SessionModelImporter::SessionModel::GetActiveRulesReferencingRelationship(const std::string& relationshipId) const
{
	auto it = activeRuleIdsByReferencedRelationship.find(relationshipId);

	return it == activeRuleIdsByReferencedRelationship.end() ? std::vector<std::string>() : it->second;
}

std::vector<std::string> SessionModelImporter::SessionModel::GetActiveRulesReferencingFunction(const std::string& functionName) const
{
	auto it = activeRuleIdsByReferencedFunction.find(functionName);

	return it == activeRuleIdsByReferencedFunction.end() ? std::vector<std::string>() : it->second;
}

std::vector<std::string> SessionModelImporter::SessionModel::GetActiveRulesConcludingAttribute(const std::string& entityId, const std::string& attributeId) const
{
	auto it = activeRuleIdsByConcludedAttribute.find(RuleSet::AttributeDependencyKey(entityId, attributeId));

	return it == activeRuleIdsByConcludedAttribute.end() ? std::vector<std::string>() : it->second;
}

SessionModelImporter::ImportResult SessionModelImporter::LoadFile(const std::string& filePath) const
{
	ImportResult result;

	std::ifstream input(filePath);

	if(!input.is_open())
	{
		result.diagnostics.push_back(Diagnostic{
			DiagnosticSeverity::Error,
			"file_open_failed",
			"Unable to open session model file.",
			filePath
		});

		return result;
	}

	try
	{
		json root;

		input >> root;

		return LoadJson(root);
	}
	catch(const std::exception& ex)
	{
		result.diagnostics.push_back(Diagnostic{
			DiagnosticSeverity::Error,
			"json_parse_failed",
			ex.what(),
			filePath
		});
	}

	return result;
}

SessionModelImporter::ImportResult SessionModelImporter::LoadJson(const json& root) const
{
	ImportResult result;
	std::unordered_set<std::string> diagnosticKeys;

	auto addDiagnostic = [&](DiagnosticSeverity severity, const std::string& code, const std::string& message, const std::string& context) {
		const std::string key = code + "|" + context;

		if(diagnosticKeys.insert(key).second)
		{
			result.diagnostics.push_back(Diagnostic{severity, code, message, context});
		}
	};

	if(!root.is_object())
	{
		addDiagnostic(DiagnosticSeverity::Error, "invalid_session_model", "Session model root must be a JSON object.", "root");

		return result;
	}

	if(!LooksLikeSessionModel(root))
	{
		addDiagnostic(DiagnosticSeverity::Error, "invalid_session_model", "Session model must contain entities and rules arrays; relationships is optional when empty.", "root");

		return result;
	}

	if(root.contains("forgetPolicy") && !root.at("forgetPolicy").is_null())
	{
		const json& forgetPolicy = root.at("forgetPolicy");

		if(!forgetPolicy.is_object())
		{
			addDiagnostic(DiagnosticSeverity::Error, "forget_policy_invalid", "forgetPolicy must be an object.", "forgetPolicy");
		}
		else if(!forgetPolicy.contains("enabled") || !forgetPolicy.at("enabled").is_boolean())
		{
			addDiagnostic(DiagnosticSeverity::Error, "forget_policy_enabled_invalid", "forgetPolicy.enabled must be an explicit boolean.", "forgetPolicy.enabled");
		}
		else
		{
			result.model.forgetPolicyEnabled = forgetPolicy.at("enabled").get<bool>();
		}
	}

	if(root.contains("externalSecurity") && !root.at("externalSecurity").is_null())
	{
		const json& security = root.at("externalSecurity");

		if(!security.is_object())
		{
			addDiagnostic(DiagnosticSeverity::Error, "external_security_invalid", "externalSecurity must be an object.", "externalSecurity");
		}
		else if(!security.contains("enabled") || !security.at("enabled").is_boolean())
		{
			addDiagnostic(DiagnosticSeverity::Error, "external_security_enabled_invalid", "externalSecurity.enabled must be an explicit boolean.", "externalSecurity.enabled");
		}
		else if(security.at("enabled").get<bool>())
		{
			RuleSet::ExternalSecurityPolicy& policy = result.model.externalSecurity;
			policy.enabled = true;
			policy.provider = Trim(GetString(security, "provider"));
			policy.approvedProfileKey = Trim(GetString(security, "approvedProfileKey"));
			policy.issuerUrl = Trim(GetString(security, "issuerUrl"));
			policy.discoveryUrl = Trim(GetString(security, "discoveryUrl"));
			policy.authorizationUrl = Trim(GetString(security, "authorizationUrl"));
			policy.logoutUrl = Trim(GetString(security, "logoutUrl"));
			policy.tokenUrl = Trim(GetString(security, "tokenUrl"));
			policy.jwksUrl = Trim(GetString(security, "jwksUrl"));
			policy.introspectionUrl = Trim(GetString(security, "introspectionUrl"));
			policy.tokenExchangeUrl = Trim(GetString(security, "tokenExchangeUrl"));
			policy.userInfoUrl = Trim(GetString(security, "userInfoUrl"));
			policy.audience = Trim(GetString(security, "audience"));
			policy.returnRouteIdentifier = Trim(GetString(security, "returnRouteIdentifier"));

			if(security.contains("scopes") && security.at("scopes").is_array())
			{
				for(const json& scope : security.at("scopes"))
				{
					if(scope.is_string() && !Trim(scope.get<std::string>()).empty())
					{
						policy.scopes.push_back(Trim(scope.get<std::string>()));
					}
				}
			}
			else
			{
				addDiagnostic(DiagnosticSeverity::Error, "external_security_scopes_invalid", "Enabled externalSecurity.scopes must be an array.", "externalSecurity.scopes");
			}

			if(NormalizeLookup(policy.provider) != "fastform")
			{
				addDiagnostic(DiagnosticSeverity::Error, "external_security_provider_invalid", "The only supported external security provider is FastForm.", "externalSecurity.provider");
			}

			if(!IsSafeExternalSecurityIdentifier(policy.approvedProfileKey))
			{
				addDiagnostic(DiagnosticSeverity::Error, "external_security_profile_invalid", "The approved external security profile key is invalid.", "externalSecurity.approvedProfileKey");
			}

			if(!IsSafeExternalSecurityIdentifier(policy.returnRouteIdentifier))
			{
				addDiagnostic(DiagnosticSeverity::Error, "external_security_return_route_invalid", "The external security return route identifier is invalid.", "externalSecurity.returnRouteIdentifier");
			}

			const std::vector<std::pair<std::string, std::string>> requiredStrings = {
				{"approvedProfileKey", policy.approvedProfileKey},
				{"issuerUrl", policy.issuerUrl},
				{"discoveryUrl", policy.discoveryUrl},
				{"authorizationUrl", policy.authorizationUrl},
				{"logoutUrl", policy.logoutUrl},
				{"tokenUrl", policy.tokenUrl},
				{"jwksUrl", policy.jwksUrl},
				{"audience", policy.audience},
				{"returnRouteIdentifier", policy.returnRouteIdentifier}
			};

			for(const auto& required : requiredStrings)
			{
				if(required.second.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "external_security_field_required", "Enabled external security is missing a required field.", "externalSecurity." + required.first);
				}
			}

			const std::vector<std::pair<std::string, std::string>> urls = {
				{"issuerUrl", policy.issuerUrl},
				{"discoveryUrl", policy.discoveryUrl},
				{"authorizationUrl", policy.authorizationUrl},
				{"logoutUrl", policy.logoutUrl},
				{"tokenUrl", policy.tokenUrl},
				{"jwksUrl", policy.jwksUrl},
				{"introspectionUrl", policy.introspectionUrl},
				{"tokenExchangeUrl", policy.tokenExchangeUrl},
				{"userInfoUrl", policy.userInfoUrl}
			};

			for(const auto& endpoint : urls)
			{
				if(!endpoint.second.empty() && !IsValidExternalSecurityHttpsUrl(endpoint.second))
				{
					addDiagnostic(DiagnosticSeverity::Error, "external_security_url_invalid", "External security endpoints must be absolute HTTPS URLs without fragments.", "externalSecurity." + endpoint.first);
				}
			}

			const bool hasOpenId = std::any_of(policy.scopes.begin(), policy.scopes.end(), [](const std::string& scope) {
				return scope == "openid";
			});
			if(!hasOpenId)
			{
				addDiagnostic(DiagnosticSeverity::Error, "external_security_openid_scope_required", "Enabled external security scopes must include openid.", "externalSecurity.scopes");
			}

			for(const char* forbiddenKey : {"clientSecret", "secret", "privateKey", "serviceCredential", "password"})
			{
				if(security.contains(forbiddenKey))
				{
					addDiagnostic(DiagnosticSeverity::Error, "external_security_secret_forbidden", "Secrets and private credentials are forbidden in Session Model external security metadata.", std::string("externalSecurity.") + forbiddenKey);
				}
			}
		}
	}

	if(root.contains("notify") && !root.at("notify").is_null())
	{
		const json& notify = root.at("notify");
		if(!notify.is_object() || !notify.contains("enabled") || !notify.at("enabled").is_boolean())
		{
			addDiagnostic(DiagnosticSeverity::Error, "notify_invalid", "notify must be an object with an explicit enabled Boolean.", "notify");
		}
		else if(notify.at("enabled").get<bool>())
		{
			RuleSet::NotifyPolicy& policy = result.model.notify;
			policy.enabled = true;
			policy.provider = Trim(GetString(notify, "provider"));
			policy.approvedProfileKey = Trim(GetString(notify, "approvedProfileKey"));
			policy.defaultReplyToEmailId = Trim(GetString(notify, "defaultReplyToEmailId"));
			if(NormalizeLookup(policy.provider) != "gov.uk notify")
			{
				addDiagnostic(DiagnosticSeverity::Error, "notify_provider_invalid", "The only supported email provider is GOV.UK Notify.", "notify.provider");
			}
			if(!IsSafeExternalSecurityIdentifier(policy.approvedProfileKey))
			{
				addDiagnostic(DiagnosticSeverity::Error, "notify_profile_invalid", "The approved Notify profile key is invalid.", "notify.approvedProfileKey");
			}
			for(const char* forbiddenKey : {"apiKey", "secret", "privateKey", "serviceCredential", "password"})
			{
				if(notify.contains(forbiddenKey))
				{
					addDiagnostic(DiagnosticSeverity::Error, "notify_secret_forbidden", "Secrets and private credentials are forbidden in Session Model Notify metadata.", std::string("notify.") + forbiddenKey);
				}
			}
		}
	}

	std::unordered_map<std::string, std::unordered_set<std::string>> attributesByEntity;
	std::unordered_map<std::string, int> attributeIdCounts;
	std::unordered_map<std::string, std::string> entityIdByLookup;
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> attributeIdByEntityLookup;
	std::unordered_map<std::string, Attribute::AttributeType> attributeTypeByEntityAttribute;
	std::unordered_map<std::string, AttributeDefinition> attributeDefinitionByEntityAttribute;
	std::unordered_map<std::string, RelationshipIndexEntry> relationshipByLookup;
	std::unordered_map<std::string, RelationshipIndexEntry> relationshipById;
	std::unordered_set<std::string> entityIds;
	std::unordered_set<std::string> rootEntityIds;
	std::unordered_set<std::string> relationshipIds;

	for(const json& entityJson : root.at("entities"))
	{
		if(!entityJson.is_object())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "entity_invalid", "Entity item is not an object.", "entities");

			continue;
		}

		EntityDefinition entity;

		entity.entityId = GetString(entityJson, "entityId");
		entity.entityName = GetString(entityJson, "entityName");

		if(entity.entityId.empty())
		{
			addDiagnostic(DiagnosticSeverity::Error, "entity_missing_id", "Entity is missing entityId.", "entity:" + entity.entityName);
		}
		else if(!entityIds.insert(entity.entityId).second)
		{
			addDiagnostic(DiagnosticSeverity::Error, "entity_duplicate_id", "Duplicate entityId.", "entity:" + entity.entityId);
		}

		if(entity.entityName.empty())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "entity_missing_name", "Entity is missing entityName.", "entity:" + entity.entityId);
		}

		if(!entity.entityId.empty() && (IsRootEntityName(entity.entityName) || IsRootEntityName(entity.entityId)))
		{
			rootEntityIds.insert(entity.entityId);
		}

		AddLookup(entityIdByLookup, entity.entityId, entity.entityId);
		AddLookup(entityIdByLookup, entity.entityName, entity.entityId);

		if(entityJson.contains("attributes") && entityJson.at("attributes").is_array())
		{
			std::unordered_set<std::string>& entityAttributes = attributesByEntity[entity.entityId];
			std::unordered_map<std::string, std::string>& attributeLookup = attributeIdByEntityLookup[entity.entityId];

			for(const json& attributeJson : entityJson.at("attributes"))
			{
				if(!attributeJson.is_object())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "attribute_invalid", "Attribute item is not an object.", "entity:" + entity.entityId);

					continue;
				}

				AttributeDefinition attribute;

				attribute.attributeId = GetString(attributeJson, "attributeId");
				attribute.attributeName = GetString(attributeJson, "attributeName");
				attribute.coreName = GetString(attributeJson, "coreName");
				attribute.attributeType = GetString(attributeJson, "attributeType");
				attribute.defaultValue = FirstNonEmpty(attributeJson, {"defaultValue", "defaultvalue", "default"});
				attribute.wholeInteger = GetBool(attributeJson, "wholeInteger", GetBool(attributeJson, "wholeinteger", false));
				attribute.mainIdentifier = GetBool(attributeJson, "mainIdentifier", GetBool(attributeJson, "mainidentifier", GetBool(attributeJson, "isMainIdentifier", false)));
				attribute.startSupplied = GetBool(attributeJson, "startSupplied", GetBool(attributeJson, "startsupplied", false));
				attribute.forgetPolicyMode = NormalizeDataForgetPolicyMode(GetString(attributeJson, "forgetPolicyMode"));
				if(attribute.forgetPolicyMode.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "attribute_forget_policy_invalid", "Attribute forgetPolicyMode must be Forget or Retain.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId);
					attribute.forgetPolicyMode = "Forget";
				}
				attribute.engineType = MapAttributeType(attribute.attributeType);
				if(attribute.attributeType == "Number" && attribute.wholeInteger)
				{
					attribute.engineType = "integer";
				}
				attribute.runtimeSupported = IsRuntimeSupportedType(attribute.attributeType);

				if(attribute.attributeId.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "attribute_missing_id", "Attribute is missing attributeId.", "entity:" + entity.entityId);
				}
				else if(!entityAttributes.insert(attribute.attributeId).second)
				{
					addDiagnostic(DiagnosticSeverity::Error, "attribute_duplicate_id", "Duplicate attributeId within entity.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId);
				}
				else
				{
					attributeIdCounts[attribute.attributeId]++;
				}

				const std::string attributeDependencyKey = RuleSet::AttributeDependencyKey(entity.entityId, attribute.attributeId);

				attributeTypeByEntityAttribute[attributeDependencyKey] = Attribute::GetAttributeType(attribute.engineType);
				attributeDefinitionByEntityAttribute[attributeDependencyKey] = attribute;

				AddLookup(attributeLookup, attribute.attributeId, attribute.attributeId);
				AddLookup(attributeLookup, attribute.attributeName, attribute.attributeId);
				AddLookup(attributeLookup, attribute.coreName, attribute.attributeId);

				if(attribute.attributeName.empty())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "attribute_missing_name", "Attribute is missing attributeName.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId);
				}

				if(attribute.attributeType.empty())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "attribute_missing_type", "Attribute is missing attributeType.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId);
				}
				else if(!attribute.runtimeSupported)
				{
					addDiagnostic(DiagnosticSeverity::Warning, "unsupported_attribute_type", "Attribute type is parsed but not yet supported by the runtime.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId + " type:" + attribute.attributeType);
				}

				if(attribute.startSupplied && rootEntityIds.find(entity.entityId) == rootEntityIds.end())
				{
					addDiagnostic(DiagnosticSeverity::Error, "start_input_non_root", "Only Root attributes can be marked startSupplied.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId);
				}

				if(attribute.startSupplied && !attribute.runtimeSupported)
				{
					addDiagnostic(DiagnosticSeverity::Error, "start_input_type_unsupported", "A startSupplied attribute must use a runtime-supported data type.", "entity:" + entity.entityId + " attribute:" + attribute.attributeId + " type:" + attribute.attributeType);
				}

				entity.attributes.push_back(attribute);
			}
		}
		else
		{
			addDiagnostic(DiagnosticSeverity::Warning, "entity_missing_attributes", "Entity has no attributes array.", "entity:" + entity.entityId);
		}

		result.model.entities.push_back(entity);
	}

	const json emptyRelationships = json::array();
	const json& relationships = root.contains("relationships") ? root.at("relationships") : emptyRelationships;

	for(const json& relationshipJson : relationships)
	{
		if(!relationshipJson.is_object())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "relationship_invalid", "Relationship item is not an object.", "relationships");

			continue;
		}

		RelationshipDefinition relationship;

		relationship.relationshipId = GetString(relationshipJson, "relationshipId");
		relationship.relationshipName = GetString(relationshipJson, "relationshipName");
		relationship.fromEntityId = GetString(relationshipJson, "fromEntityId");
		relationship.fromEntityName = GetString(relationshipJson, "fromEntityName");
		relationship.toEntityId = GetString(relationshipJson, "toEntityId");
		relationship.toEntityName = GetString(relationshipJson, "toEntityName");
		relationship.cardinality = GetString(relationshipJson, "cardinality");
		relationship.forgetPolicyMode = NormalizeDataForgetPolicyMode(GetString(relationshipJson, "forgetPolicyMode"));
		if(relationship.forgetPolicyMode.empty())
		{
			addDiagnostic(DiagnosticSeverity::Error, "relationship_forget_policy_invalid", "Relationship forgetPolicyMode must be Forget or Retain.", "relationship:" + relationship.relationshipId);
			relationship.forgetPolicyMode = "Forget";
		}

		if(relationship.relationshipId.empty())
		{
			addDiagnostic(DiagnosticSeverity::Error, "relationship_missing_id", "Relationship is missing relationshipId.", relationship.relationshipName);
		}
		else if(!relationshipIds.insert(relationship.relationshipId).second)
		{
			addDiagnostic(DiagnosticSeverity::Error, "relationship_duplicate_id", "Duplicate relationshipId.", "relationship:" + relationship.relationshipId);
		}

		if(!relationship.fromEntityId.empty() && entityIds.find(relationship.fromEntityId) == entityIds.end())
		{
			addDiagnostic(DiagnosticSeverity::Error, "relationship_endpoint_missing", "Relationship fromEntityId does not resolve to an entity.", "relationship:" + relationship.relationshipId + " entity:" + relationship.fromEntityId);
		}

		if(!relationship.toEntityId.empty() && entityIds.find(relationship.toEntityId) == entityIds.end())
		{
			addDiagnostic(DiagnosticSeverity::Error, "relationship_endpoint_missing", "Relationship toEntityId does not resolve to an entity.", "relationship:" + relationship.relationshipId + " entity:" + relationship.toEntityId);
		}

		if(RelationshipMakesRootRepeatable(relationship, rootEntityIds))
		{
			addDiagnostic(
				DiagnosticSeverity::Error,
				"root_singleton_relationship_invalid",
				"Root must remain a singleton. Relationship cardinality places Root on a repeatable side.",
				RootSingletonRelationshipContext(relationship));
		}

		RelationshipIndexEntry relationshipEntry;

		relationshipEntry.relationshipId = relationship.relationshipId;
		relationshipEntry.fromEntityId = relationship.fromEntityId;
		relationshipEntry.toEntityId = relationship.toEntityId;

		AddRelationshipLookup(relationshipByLookup, relationship.relationshipId, relationshipEntry);
		AddRelationshipLookup(relationshipByLookup, relationship.relationshipName, relationshipEntry);

		if(!relationship.relationshipId.empty())
		{
			relationshipById[relationship.relationshipId] = relationshipEntry;
		}

		result.model.relationships.push_back(relationship);
	}

	std::unordered_set<std::string> emailTemplateIds;
	if(root.contains("emailTemplates"))
	{
		if(!root.at("emailTemplates").is_array())
		{
			addDiagnostic(DiagnosticSeverity::Error, "email_templates_invalid", "emailTemplates must be an array.", "emailTemplates");
		}
		else
		{
			if(root.at("emailTemplates").size() > 1000)
			{
				addDiagnostic(DiagnosticSeverity::Error, "email_template_limit_exceeded", "A Session Model cannot contain more than 1000 email templates.", "emailTemplates");
			}
			for(const json& templateJson : root.at("emailTemplates"))
			{
				if(!templateJson.is_object())
				{
					addDiagnostic(DiagnosticSeverity::Error, "email_template_invalid", "Email template must be an object.", "emailTemplates");
					continue;
				}
				const std::string templateId = Trim(GetString(templateJson, "id"));
				const std::string notifyTemplateId = Trim(GetString(templateJson, "notifyTemplateId"));
				const std::string recipientEntityId = Trim(GetString(templateJson, "recipientEntityId"));
				const std::string recipientAttributeId = Trim(GetString(templateJson, "recipientAttributeId"));
				const std::string context = "emailTemplate:" + templateId;
				if(templateId.empty() || !emailTemplateIds.insert(templateId).second)
				{
					addDiagnostic(DiagnosticSeverity::Error, "email_template_id_invalid", "Email template id must be present and unique.", context);
				}
				if(notifyTemplateId.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "email_notify_template_missing", "Email template must reference a Notify template.", context);
				}
				if(templateJson.contains("attachments"))
				{
					addDiagnostic(DiagnosticSeverity::Error, "email_attachments_unsupported", "Email attachments are not supported by this contract version.", context);
				}
				if(entityIds.find(recipientEntityId) == entityIds.end() || attributesByEntity[recipientEntityId].find(recipientAttributeId) == attributesByEntity[recipientEntityId].end())
				{
					addDiagnostic(DiagnosticSeverity::Error, "email_recipient_attribute_missing", "Email recipient must reference an existing entity attribute.", context);
				}
				if(!templateJson.contains("personalisation") || !templateJson.at("personalisation").is_array())
				{
					addDiagnostic(DiagnosticSeverity::Error, "email_personalisation_invalid", "Email template personalisation must be an array.", context);
				}
				else
				{
					if(templateJson.at("personalisation").size() > 500)
					{
						addDiagnostic(DiagnosticSeverity::Error, "email_mapping_limit_exceeded", "An email template cannot contain more than 500 personalisation mappings.", context);
					}
					std::unordered_set<std::string> placeholders;
					for(const json& mapping : templateJson.at("personalisation"))
					{
						if(!mapping.is_object())
						{
							addDiagnostic(DiagnosticSeverity::Error, "email_mapping_invalid", "Email personalisation mapping must be an object.", context);
							continue;
						}
						const std::string placeholder = Trim(GetString(mapping, "placeholderName"));
						const std::string mode = Trim(GetString(mapping, "valueMode"));
						if(placeholder.empty() || !placeholders.insert(NormalizeLookup(placeholder)).second)
						{
							addDiagnostic(DiagnosticSeverity::Error, "email_mapping_placeholder_invalid", "Email placeholders must be present and unique.", context);
						}
						if(mode != "Static" && mode != "Attribute" && mode != "AttributeList" && mode != "OptionalBoolean" && mode != "ComposedMessage")
						{
							addDiagnostic(DiagnosticSeverity::Error, "email_mapping_mode_invalid", "Email mapping valueMode is unsupported.", context + " placeholder:" + placeholder);
						}
						if(mode == "Static")
						{
							const std::string staticValue = GetString(mapping, "staticValue");
							if(staticValue.size() > 100000)
							{
								addDiagnostic(DiagnosticSeverity::Error, "email_static_value_limit_exceeded", "Email static personalisation cannot exceed 100000 characters.", context + " placeholder:" + placeholder);
							}
						}
						else if(mode == "ComposedMessage")
						{
							if(!mapping.contains("messageParagraphs") || !mapping.at("messageParagraphs").is_array())
							{
								addDiagnostic(DiagnosticSeverity::Error, "email_message_paragraphs_invalid", "ComposedMessage personalisation must contain a messageParagraphs array.", context + " placeholder:" + placeholder);
							}
							else
							{
								const json& paragraphs = mapping.at("messageParagraphs");
								if(paragraphs.size() > 500)
								{
									addDiagnostic(DiagnosticSeverity::Error, "email_message_paragraph_limit_exceeded", "A composed email message cannot contain more than 500 paragraphs.", context + " placeholder:" + placeholder);
								}
								std::size_t runCount = 0;
								std::size_t textSize = 0;
								for(const json& paragraph : paragraphs)
								{
									if(!paragraph.is_object() || !paragraph.contains("runs") || !paragraph.at("runs").is_array())
									{
										addDiagnostic(DiagnosticSeverity::Error, "email_message_paragraph_invalid", "Each composed email paragraph must contain a runs array.", context + " placeholder:" + placeholder);
										continue;
									}
									for(const json& run : paragraph.at("runs"))
									{
										++runCount;
										if(!run.is_object())
										{
											addDiagnostic(DiagnosticSeverity::Error, "email_message_run_invalid", "Each composed email content part must be an object.", context + " placeholder:" + placeholder);
											continue;
										}
										const std::string runType = Trim(GetString(run, "runType"));
										if(runType == "Text")
										{
											textSize += GetString(run, "text").size();
											continue;
										}
										if(runType != "Attribute")
										{
											addDiagnostic(DiagnosticSeverity::Error, "email_message_run_type_invalid", "Composed email content type must be Text or Attribute.", context + " placeholder:" + placeholder);
											continue;
										}
										const std::string runEntityId = Trim(GetString(run, "entityId"));
										const std::string runAttributeId = Trim(GetString(run, "attributeId"));
										if(entityIds.find(runEntityId) == entityIds.end() || attributesByEntity[runEntityId].find(runAttributeId) == attributesByEntity[runEntityId].end())
										{
											addDiagnostic(DiagnosticSeverity::Error, "email_message_attribute_missing", "Composed email attributes must reference an existing entity attribute.", context + " placeholder:" + placeholder);
										}
									}
								}
								if(runCount > 5000)
								{
									addDiagnostic(DiagnosticSeverity::Error, "email_message_run_limit_exceeded", "A composed email message cannot contain more than 5000 content parts.", context + " placeholder:" + placeholder);
								}
								if(textSize > 100000)
								{
									addDiagnostic(DiagnosticSeverity::Error, "email_message_text_limit_exceeded", "Composed email static text cannot exceed 100000 characters.", context + " placeholder:" + placeholder);
								}
							}
						}
						else
						{
							const std::string mappingEntityId = Trim(GetString(mapping, "entityId"));
							const std::string mappingAttributeId = Trim(GetString(mapping, "attributeId"));
							if(entityIds.find(mappingEntityId) == entityIds.end() || attributesByEntity[mappingEntityId].find(mappingAttributeId) == attributesByEntity[mappingEntityId].end())
							{
								addDiagnostic(DiagnosticSeverity::Error, "email_mapping_attribute_missing", "Email mapping must reference an existing entity attribute.", context + " placeholder:" + placeholder);
							}
							else if(mode == "OptionalBoolean")
							{
								const std::string key = RuleSet::AttributeDependencyKey(mappingEntityId, mappingAttributeId);
								const auto type = attributeTypeByEntityAttribute.find(key);
								if(type == attributeTypeByEntityAttribute.end() || type->second != Attribute::BOOLEAN)
								{
									addDiagnostic(DiagnosticSeverity::Error, "email_optional_boolean_type_invalid", "OptionalBoolean personalisation must reference a Boolean attribute.", context + " placeholder:" + placeholder);
								}
							}
						}
					}
				}
				result.model.emailTemplates.push_back(templateJson);
			}
		}
	}

	if(root.contains("flow") && root.at("flow").is_object() && root.at("flow").contains("screens") && root.at("flow").at("screens").is_array())
	{
		for(const json& flowScreenJson : root.at("flow").at("screens"))
		{
			if(!flowScreenJson.is_object() || !flowScreenJson.contains("transitions") || !flowScreenJson.at("transitions").is_array())
			{
				continue;
			}

			std::unordered_set<std::string> effectiveSubmitModes;
			for(const json& transitionJson : flowScreenJson.at("transitions"))
			{
				if(!transitionJson.is_object())
				{
					continue;
				}

				const std::string mode = NormalizeTransitionForgetPolicyMode(GetString(transitionJson, "forgetPolicyMode"));
				const std::string transitionEmailTemplateId = Trim(GetString(transitionJson, "emailTemplateId"));
				if(!transitionEmailTemplateId.empty())
				{
					if(emailTemplateIds.find(transitionEmailTemplateId) == emailTemplateIds.end())
					{
						addDiagnostic(DiagnosticSeverity::Error, "transition_email_template_missing", "Flow transition references a missing email template.", "transition:" + FirstNonEmpty(transitionJson, {"transitionId", "id"}));
					}
					if(!result.model.notify.enabled)
					{
						addDiagnostic(DiagnosticSeverity::Error, "transition_notify_disabled", "Flow transition sends email but Notify is disabled.", "transition:" + FirstNonEmpty(transitionJson, {"transitionId", "id"}));
					}
				}
				if(mode.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error,
					              "transition_forget_policy_invalid",
					              "Flow transition forgetPolicyMode must be Inherit, Apply, or Skip.",
					              "transition:" + FirstNonEmpty(transitionJson, {"transitionId", "id"}));
				}
				else if(NormalizeLookup(FirstNonEmpty(transitionJson, {"transitionRole", "role"})) != "add")
				{
					effectiveSubmitModes.insert(mode == "Inherit"
						? (result.model.forgetPolicyEnabled ? "Apply" : "Skip")
						: mode);
				}
			}

			if(effectiveSubmitModes.size() > 1)
			{
				addDiagnostic(DiagnosticSeverity::Error,
				              "transition_forget_policy_conflict",
				              "Transitions from the same submitted screen must resolve to one effective Forget policy mode before routing.",
				              "screen:" + FirstNonEmpty(flowScreenJson, {"screenId", "id"}));
			}
		}
	}

	if(root.contains("pdfTemplates"))
	{
		if(!root.at("pdfTemplates").is_array())
		{
			addDiagnostic(DiagnosticSeverity::Error, "pdf_templates_invalid", "pdfTemplates must be an array.", "pdfTemplates");
		}
		else
		{
			std::unordered_set<std::string> pdfTemplateIds;
			for(const json& templateJson : root.at("pdfTemplates"))
			{
				if(!templateJson.is_object())
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_invalid", "PDF template must be an object.", "pdfTemplates");
					continue;
				}

				const std::string templateId = GetString(templateJson, "id");
				const std::string templateName = GetString(templateJson, "name");
				const std::string rootRegionId = GetString(templateJson, "rootRegionId");
				const std::string rootEntityId = GetString(templateJson, "rootEntityId");
				const std::string context = "pdfTemplate:" + (templateId.empty() ? templateName : templateId);

				if(templateId.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_missing_id", "PDF template is missing id.", context);
				}
				else if(!pdfTemplateIds.insert(templateId).second)
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_duplicate_id", "Duplicate PDF template id.", context);
				}
				if(templateName.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_missing_name", "PDF template is missing name.", context);
				}
				if(rootRegionId.empty())
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_missing_root_region", "PDF template is missing rootRegionId.", context);
				}
				if(rootEntityId.empty() || entityIds.find(rootEntityId) == entityIds.end())
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_root_entity_missing", "PDF template rootEntityId does not resolve to an entity.", context);
				}
				if(!templateJson.contains("blocks") || !templateJson.at("blocks").is_array())
				{
					addDiagnostic(DiagnosticSeverity::Error, "pdf_template_blocks_invalid", "PDF template blocks must be an array.", context);
				}

				result.model.pdfTemplates.push_back(templateJson);
			}
		}
	}

	if(root.contains("dataConnections"))
	{
		const json& dataConnectionsJson = root.at("dataConnections");

		if(!dataConnectionsJson.is_object())
		{
			addDiagnostic(DiagnosticSeverity::Error, "data_connections_invalid", "dataConnections must be a JSON object.", "dataConnections");
		}
		else
		{
			std::unordered_set<std::string> dataConnectionSourceIds;
			std::unordered_map<std::string, RuleSet::DataConnectionSource> dataConnectionSourcesById;

			if(!dataConnectionsJson.contains("sources"))
			{
				addDiagnostic(DiagnosticSeverity::Warning, "data_connection_sources_missing", "dataConnections has no sources array.", "dataConnections");
			}
			else if(!dataConnectionsJson.at("sources").is_array())
			{
				addDiagnostic(DiagnosticSeverity::Error, "data_connection_sources_invalid", "dataConnections.sources must be an array.", "dataConnections.sources");
			}
			else
			{
				for(const json& sourceJson : dataConnectionsJson.at("sources"))
				{
					if(!sourceJson.is_object())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "data_connection_source_invalid", "Data connection source item is not an object.", "dataConnections.sources");

						continue;
					}

					RuleSet::DataConnectionSource source;

					source.sourceId = GetString(sourceJson, "sourceId");
					source.sourceName = GetString(sourceJson, "sourceName");
					source.sourceType = GetString(sourceJson, "sourceType");
					source.externalObjectName = GetString(sourceJson, "externalObjectName");
					source.databaseProvider = GetString(sourceJson, "databaseProvider");
					source.serverConnectionKey = GetString(sourceJson, "serverConnectionKey");

					const std::string sourceContext = source.sourceId.empty() ? "dataConnections.sources" : "dataConnections.source:" + source.sourceId;

					if(source.sourceId.empty())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_missing_id", "Data connection source is missing sourceId.", sourceContext);
					}
					else if(!dataConnectionSourceIds.insert(source.sourceId).second)
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_duplicate_id", "Duplicate data connection sourceId.", sourceContext);
					}
					else
					{
						dataConnectionSourcesById.emplace(source.sourceId, source);
					}

					if(source.sourceType.empty())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_missing_type", "Data connection source is missing sourceType.", sourceContext);
					}
					else if(!IsDatabaseSourceType(source.sourceType))
					{
						addDiagnostic(DiagnosticSeverity::Warning, "data_connection_source_type_unsupported", "Data connection sourceType is parsed but not yet supported for runtime persistence.", sourceContext + " type:" + source.sourceType);
					}

					if(IsDatabaseSourceType(source.sourceType))
					{
						if(source.databaseProvider.empty())
						{
							addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_missing_provider", "Database data connection source is missing databaseProvider.", sourceContext);
						}
						else if(!IsSupportedDatabaseProvider(source.databaseProvider))
						{
							addDiagnostic(DiagnosticSeverity::Warning, "data_connection_database_provider_unsupported", "Database provider is parsed but not yet supported for runtime persistence.", sourceContext + " provider:" + source.databaseProvider);
						}

						if(source.serverConnectionKey.empty())
						{
							addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_missing_server_key", "Database data connection source is missing serverConnectionKey.", sourceContext);
						}
					}

					result.model.dataConnectionSources.push_back(source);
				}
			}

			if(!dataConnectionsJson.contains("entityMappings"))
			{
				addDiagnostic(DiagnosticSeverity::Warning, "data_connection_entity_mappings_missing", "dataConnections has no entityMappings array.", "dataConnections");
			}
			else if(!dataConnectionsJson.at("entityMappings").is_array())
			{
				addDiagnostic(DiagnosticSeverity::Error, "data_connection_entity_mappings_invalid", "dataConnections.entityMappings must be an array.", "dataConnections.entityMappings");
			}
			else
			{
				for(const json& entityMappingJson : dataConnectionsJson.at("entityMappings"))
				{
					if(!entityMappingJson.is_object())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "data_connection_entity_mapping_invalid", "Data connection entity mapping item is not an object.", "dataConnections.entityMappings");

						continue;
					}

					RuleSet::DataConnectionEntityMapping entityMapping;

					entityMapping.entityId = GetString(entityMappingJson, "entityId");
					entityMapping.entityName = GetString(entityMappingJson, "entityName");
					entityMapping.sourceId = GetString(entityMappingJson, "sourceId");
					entityMapping.sourceName = GetString(entityMappingJson, "sourceName");
					entityMapping.sourceType = GetString(entityMappingJson, "sourceType");
					entityMapping.sourceObjectName = GetString(entityMappingJson, "sourceObjectName");

					const std::string rawSaveOperationPolicy = FirstNonEmpty(entityMappingJson, {
						"saveOperationPolicy",
						"savePolicy",
						"saveMode",
						"writePolicy",
						"operationPolicy"
					});
					const bool createIfMissing =
						GetBool(entityMappingJson, "createIfMissing",
							GetBool(entityMappingJson, "insertIfMissing",
								GetBool(entityMappingJson, "upsert", false)));

					const std::string entityMappingContext = "dataConnections.entityMapping:"
						+ (entityMapping.entityId.empty() ? "<missing-entity>" : entityMapping.entityId)
						+ ":"
						+ (entityMapping.sourceObjectName.empty() ? "<missing-object>" : entityMapping.sourceObjectName);

					const std::string normalizedSaveOperationPolicy = NormalizeDataConnectionSaveOperationPolicy(rawSaveOperationPolicy);
					if(!rawSaveOperationPolicy.empty() && normalizedSaveOperationPolicy.empty())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "data_connection_save_operation_policy_unknown", "Data connection entity mapping has an unknown save operation policy; defaulting to update-only.", entityMappingContext + " policy:" + rawSaveOperationPolicy);
						entityMapping.saveOperationPolicy = "UpdateOnly";
					}
					else if(!rawSaveOperationPolicy.empty())
					{
						entityMapping.saveOperationPolicy = normalizedSaveOperationPolicy;
					}
					else
					{
						entityMapping.saveOperationPolicy = createIfMissing ? "Upsert" : "UpdateOnly";
					}

					if(entityMapping.entityId.empty())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_entity_missing_id", "Data connection entity mapping is missing entityId.", entityMappingContext);
					}
					else if(entityIds.find(entityMapping.entityId) == entityIds.end())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_entity_missing", "Data connection entity mapping references an unknown entity.", entityMappingContext);
					}

					auto sourceIt = dataConnectionSourcesById.end();

					if(entityMapping.sourceId.empty())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_entity_source_missing_id", "Data connection entity mapping is missing sourceId.", entityMappingContext);
					}
					else
					{
						sourceIt = dataConnectionSourcesById.find(entityMapping.sourceId);

						if(sourceIt == dataConnectionSourcesById.end())
						{
							addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_missing", "Data connection entity mapping references an unknown sourceId.", entityMappingContext + " source:" + entityMapping.sourceId);
						}
					}

					if(entityMapping.sourceObjectName.empty())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_source_object_missing", "Data connection entity mapping is missing sourceObjectName.", entityMappingContext);
					}

					if(sourceIt != dataConnectionSourcesById.end())
					{
						const RuleSet::DataConnectionSource& source = sourceIt->second;

						if(!entityMapping.sourceName.empty()
							&& !source.sourceName.empty()
							&& NormalizeLookup(entityMapping.sourceName) != NormalizeLookup(source.sourceName))
						{
							addDiagnostic(DiagnosticSeverity::Warning, "data_connection_source_name_mismatch", "Data connection entity mapping sourceName differs from the registered source.", entityMappingContext + " source:" + entityMapping.sourceId);
						}

						if(!entityMapping.sourceType.empty()
							&& !source.sourceType.empty()
							&& NormalizeLookup(entityMapping.sourceType) != NormalizeLookup(source.sourceType))
						{
							addDiagnostic(DiagnosticSeverity::Warning, "data_connection_source_type_mismatch", "Data connection entity mapping sourceType differs from the registered source.", entityMappingContext + " source:" + entityMapping.sourceId);
						}
					}

					if(!entityMappingJson.contains("attributeMappings"))
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_attribute_mappings_missing", "Data connection entity mapping has no attributeMappings array.", entityMappingContext);
					}
					else if(!entityMappingJson.at("attributeMappings").is_array())
					{
						addDiagnostic(DiagnosticSeverity::Error, "data_connection_attribute_mappings_invalid", "Data connection attributeMappings must be an array.", entityMappingContext);
					}
					else
					{
						std::unordered_set<std::string> mappedAttributeIds;

						for(const json& attributeMappingJson : entityMappingJson.at("attributeMappings"))
						{
							if(!attributeMappingJson.is_object())
							{
								addDiagnostic(DiagnosticSeverity::Warning, "data_connection_attribute_invalid", "Data connection attribute mapping item is not an object.", entityMappingContext);

								continue;
							}

							RuleSet::DataConnectionAttributeMapping attributeMapping;

							attributeMapping.attributeId = GetString(attributeMappingJson, "attributeId");
							attributeMapping.attributeName = GetString(attributeMappingJson, "attributeName");
							attributeMapping.attributeCoreName = GetString(attributeMappingJson, "attributeCoreName");
							attributeMapping.attributeDataType = GetString(attributeMappingJson, "attributeDataType");
							attributeMapping.sourceFieldName = GetString(attributeMappingJson, "sourceFieldName");
							attributeMapping.input = GetBool(attributeMappingJson, "input", false);
							attributeMapping.output = GetBool(attributeMappingJson, "output", false);
							attributeMapping.hasLoadFilterMetadata = attributeMappingJson.contains("loadFilter")
								|| attributeMappingJson.contains("loadKey")
								|| attributeMappingJson.contains("recordSelectionFilter")
								|| attributeMappingJson.contains("recordSelectionKey");
							attributeMapping.loadFilter = GetBool(attributeMappingJson, "loadFilter",
								GetBool(attributeMappingJson, "loadKey",
									GetBool(attributeMappingJson, "recordSelectionFilter",
										GetBool(attributeMappingJson, "recordSelectionKey", false))));
							attributeMapping.loadFilterOperator = FirstNonEmpty(attributeMappingJson, {"loadFilterOperator", "filterOperator", "operator"});
							if(attributeMapping.loadFilterOperator.empty())
							{
								attributeMapping.loadFilterOperator = "Equals";
							}
							attributeMapping.loadFilterValueSource = NormalizeDataConnectionLoadFilterValueSource(FirstNonEmpty(attributeMappingJson, {
								"loadFilterValueSource",
								"loadFilterSource",
								"recordSelectionValueSource",
								"recordSelectionFilterValueSource"
							}));
							attributeMapping.saveConflictCheck = GetBool(attributeMappingJson, "saveConflictCheck",
								GetBool(attributeMappingJson, "concurrencyCheck",
									GetBool(attributeMappingJson, "optimisticConcurrencyCheck",
										GetBool(attributeMappingJson, "saveConcurrencyCheck", false))));
							const std::string rawValueGeneration = FirstNonEmpty(attributeMappingJson, {
								"valueGeneration",
								"generation",
								"generatedValue",
								"keyGeneration"
							});
							attributeMapping.valueGeneration = NormalizeDataConnectionValueGeneration(rawValueGeneration);
							attributeMapping.generatedOnCreate = GetBool(attributeMappingJson, "generatedOnCreate",
								GetBool(attributeMappingJson, "databaseGenerated",
									GetBool(attributeMappingJson, "autoGenerated",
										GetBool(attributeMappingJson, "generatedKey", false))));
							if(attributeMapping.generatedOnCreate && attributeMapping.valueGeneration.empty())
							{
								attributeMapping.valueGeneration = "DatabaseGenerated";
							}
							if(attributeMapping.valueGeneration == "DatabaseGenerated")
							{
								attributeMapping.generatedOnCreate = true;
								attributeMapping.output = true;
							}

							const std::string attributeMappingContext = entityMappingContext
								+ " attribute:"
								+ (attributeMapping.attributeId.empty() ? "<missing-attribute>" : attributeMapping.attributeId);

							if(attributeMapping.attributeId.empty())
							{
								addDiagnostic(DiagnosticSeverity::Error, "data_connection_attribute_missing_id", "Data connection attribute mapping is missing attributeId.", attributeMappingContext);
							}
							else
							{
								if(!mappedAttributeIds.insert(attributeMapping.attributeId).second)
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_attribute_duplicate", "Duplicate attribute mapping within data connection entity mapping.", attributeMappingContext);
								}

								auto entityAttributesIt = attributesByEntity.find(entityMapping.entityId);

								if(entityAttributesIt == attributesByEntity.end()
									|| entityAttributesIt->second.find(attributeMapping.attributeId) == entityAttributesIt->second.end())
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_attribute_missing", "Data connection attribute mapping references an unknown attribute for the mapped entity.", attributeMappingContext);
								}
								else
								{
									auto attributeDefinitionIt = attributeDefinitionByEntityAttribute.find(RuleSet::AttributeDependencyKey(entityMapping.entityId, attributeMapping.attributeId));

									if(attributeDefinitionIt != attributeDefinitionByEntityAttribute.end())
									{
										const AttributeDefinition& attributeDefinition = attributeDefinitionIt->second;

										if(!attributeMapping.attributeCoreName.empty()
											&& !attributeDefinition.coreName.empty()
											&& NormalizeLookup(attributeMapping.attributeCoreName) != NormalizeLookup(attributeDefinition.coreName))
										{
											addDiagnostic(DiagnosticSeverity::Warning, "data_connection_attribute_core_name_mismatch", "Data connection attributeCoreName differs from the imported attribute coreName.", attributeMappingContext);
										}

										if(!AttributeDataTypesCompatible(attributeDefinition.attributeType, attributeMapping.attributeDataType))
										{
											addDiagnostic(DiagnosticSeverity::Warning, "data_connection_attribute_type_mismatch", "Data connection attributeDataType differs from the imported attribute type.", attributeMappingContext + " expected:" + attributeDefinition.attributeType + " actual:" + attributeMapping.attributeDataType);
										}
									}
								}
							}

							if(attributeMapping.sourceFieldName.empty())
							{
								addDiagnostic(DiagnosticSeverity::Error, "data_connection_field_missing", "Data connection attribute mapping is missing sourceFieldName.", attributeMappingContext);
							}

							if(!attributeMapping.input && !attributeMapping.output && !attributeMapping.loadFilter && !attributeMapping.saveConflictCheck)
							{
								addDiagnostic(DiagnosticSeverity::Warning, "data_connection_attribute_no_direction", "Data connection attribute mapping has neither input, output, loadFilter, nor saveConflictCheck enabled.", attributeMappingContext);
							}

							if(!rawValueGeneration.empty() && attributeMapping.valueGeneration.empty())
							{
								addDiagnostic(DiagnosticSeverity::Warning, "data_connection_attribute_value_generation_unsupported", "Data connection attribute mapping uses unsupported valueGeneration metadata. Supported generated-key value is DatabaseGenerated.", attributeMappingContext);
							}

							entityMapping.attributeMappings.push_back(attributeMapping);
						}
					}

					const json* relationshipFieldMappingsJson = nullptr;

					for(const std::string& key : std::vector<std::string>{"relationshipFieldMappings", "relationshipFields", "foreignKeyMappings", "foreignKeyFields", "relationshipForeignKeyMappings"})
					{
						if(entityMappingJson.contains(key))
						{
							relationshipFieldMappingsJson = &entityMappingJson.at(key);
							break;
						}
					}

					if(relationshipFieldMappingsJson != nullptr)
					{
						if(!relationshipFieldMappingsJson->is_array())
						{
							addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_mappings_invalid", "Data connection relationshipFieldMappings must be an array.", entityMappingContext);
						}
						else
						{
							std::unordered_set<std::string> mappedSourceFieldNames;

							for(const RuleSet::DataConnectionAttributeMapping& attributeMapping : entityMapping.attributeMappings)
							{
								if(!attributeMapping.sourceFieldName.empty())
								{
									mappedSourceFieldNames.insert(NormalizeLookup(attributeMapping.sourceFieldName));
								}
							}

							for(const json& relationshipFieldMappingJson : *relationshipFieldMappingsJson)
							{
								if(!relationshipFieldMappingJson.is_object())
								{
									addDiagnostic(DiagnosticSeverity::Warning, "data_connection_relationship_field_invalid", "Data connection relationship field mapping item is not an object.", entityMappingContext);

									continue;
								}

								RuleSet::DataConnectionRelationshipFieldMapping relationshipFieldMapping;

								relationshipFieldMapping.relationshipId = FirstNonEmpty(relationshipFieldMappingJson, {
									"relationshipId",
									"relationship",
									"relationshipName"
								});
								relationshipFieldMapping.relationshipName = GetString(relationshipFieldMappingJson, "relationshipName");
								relationshipFieldMapping.sourceFieldName = FirstNonEmpty(relationshipFieldMappingJson, {
									"sourceFieldName",
									"foreignKeyFieldName",
									"fieldName",
									"targetSourceFieldName"
								});
								relationshipFieldMapping.relatedEntityId = FirstNonEmpty(relationshipFieldMappingJson, {
									"relatedEntityId",
									"parentEntityId",
									"sourceEntityId",
									"fromEntityId"
								});
								relationshipFieldMapping.relatedEntityName = FirstNonEmpty(relationshipFieldMappingJson, {
									"relatedEntityName",
									"parentEntityName",
									"sourceEntityName",
									"fromEntityName"
								});
								relationshipFieldMapping.relatedAttributeId = FirstNonEmpty(relationshipFieldMappingJson, {
									"relatedAttributeId",
									"parentAttributeId",
									"sourceAttributeId",
									"fromAttributeId",
									"attributeId"
								});
								relationshipFieldMapping.relatedAttributeName = FirstNonEmpty(relationshipFieldMappingJson, {
									"relatedAttributeName",
									"parentAttributeName",
									"sourceAttributeName",
									"fromAttributeName",
									"attributeName"
								});
								relationshipFieldMapping.relatedAttributeCoreName = FirstNonEmpty(relationshipFieldMappingJson, {
									"relatedAttributeCoreName",
									"parentAttributeCoreName",
									"sourceAttributeCoreName",
									"fromAttributeCoreName",
									"attributeCoreName"
								});
								relationshipFieldMapping.relatedAttributeDataType = FirstNonEmpty(relationshipFieldMappingJson, {
									"relatedAttributeDataType",
									"parentAttributeDataType",
									"sourceAttributeDataType",
									"fromAttributeDataType",
									"attributeDataType"
								});
								relationshipFieldMapping.output = GetBool(relationshipFieldMappingJson, "output", true);

								const std::string relationshipFieldMappingContext = entityMappingContext
									+ " relationshipField:"
									+ (relationshipFieldMapping.sourceFieldName.empty() ? "<missing-field>" : relationshipFieldMapping.sourceFieldName);

								if(relationshipFieldMapping.sourceFieldName.empty())
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_missing", "Data connection relationship field mapping is missing sourceFieldName.", relationshipFieldMappingContext);
								}
								else if(!mappedSourceFieldNames.insert(NormalizeLookup(relationshipFieldMapping.sourceFieldName)).second)
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_duplicate", "Data connection relationship field mapping duplicates another sourceFieldName in the entity mapping.", relationshipFieldMappingContext);
								}

								if(relationshipFieldMapping.relationshipId.empty())
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_relationship_missing", "Data connection relationship field mapping is missing relationshipId.", relationshipFieldMappingContext);
								}
								else
								{
									auto relationshipLookupIt = relationshipByLookup.find(NormalizeLookup(relationshipFieldMapping.relationshipId));

									if(relationshipLookupIt == relationshipByLookup.end())
									{
										addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_relationship_unknown", "Data connection relationship field mapping references an unknown relationship.", relationshipFieldMappingContext + " relationship:" + relationshipFieldMapping.relationshipId);
									}
									else
									{
										const RelationshipIndexEntry& relationship = relationshipLookupIt->second;

										relationshipFieldMapping.relationshipId = relationship.relationshipId;

										if(relationshipFieldMapping.relatedEntityId.empty())
										{
											if(entityMapping.entityId == relationship.fromEntityId)
											{
												relationshipFieldMapping.relatedEntityId = relationship.toEntityId;
											}
											else if(entityMapping.entityId == relationship.toEntityId)
											{
												relationshipFieldMapping.relatedEntityId = relationship.fromEntityId;
											}
										}

										if(entityMapping.entityId != relationship.fromEntityId && entityMapping.entityId != relationship.toEntityId)
										{
											addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_entity_not_endpoint", "Data connection relationship field mapping relationship does not include the mapped entity.", relationshipFieldMappingContext + " relationship:" + relationshipFieldMapping.relationshipId);
										}

										if(!relationshipFieldMapping.relatedEntityId.empty()
											&& relationshipFieldMapping.relatedEntityId != relationship.fromEntityId
											&& relationshipFieldMapping.relatedEntityId != relationship.toEntityId)
										{
											addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_related_entity_not_endpoint", "Data connection relationship field mapping relatedEntityId is not an endpoint of the relationship.", relationshipFieldMappingContext + " entity:" + relationshipFieldMapping.relatedEntityId);
										}
									}
								}

								if(relationshipFieldMapping.relatedEntityId.empty())
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_related_entity_missing", "Data connection relationship field mapping could not resolve relatedEntityId.", relationshipFieldMappingContext);
								}
								else if(entityIds.find(relationshipFieldMapping.relatedEntityId) == entityIds.end())
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_related_entity_unknown", "Data connection relationship field mapping references an unknown relatedEntityId.", relationshipFieldMappingContext + " entity:" + relationshipFieldMapping.relatedEntityId);
								}

								if(relationshipFieldMapping.relatedAttributeId.empty())
								{
									addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_related_attribute_missing", "Data connection relationship field mapping is missing relatedAttributeId.", relationshipFieldMappingContext);
								}
								else
								{
									auto entityAttributesIt = attributesByEntity.find(relationshipFieldMapping.relatedEntityId);

									if(entityAttributesIt == attributesByEntity.end()
										|| entityAttributesIt->second.find(relationshipFieldMapping.relatedAttributeId) == entityAttributesIt->second.end())
									{
										addDiagnostic(DiagnosticSeverity::Error, "data_connection_relationship_field_related_attribute_unknown", "Data connection relationship field mapping references an unknown related attribute.", relationshipFieldMappingContext + " attribute:" + relationshipFieldMapping.relatedAttributeId);
									}
									else
									{
										auto attributeDefinitionIt = attributeDefinitionByEntityAttribute.find(RuleSet::AttributeDependencyKey(relationshipFieldMapping.relatedEntityId, relationshipFieldMapping.relatedAttributeId));

										if(attributeDefinitionIt != attributeDefinitionByEntityAttribute.end())
										{
											const AttributeDefinition& attributeDefinition = attributeDefinitionIt->second;

											if(!relationshipFieldMapping.relatedAttributeCoreName.empty()
												&& !attributeDefinition.coreName.empty()
												&& NormalizeLookup(relationshipFieldMapping.relatedAttributeCoreName) != NormalizeLookup(attributeDefinition.coreName))
											{
												addDiagnostic(DiagnosticSeverity::Warning, "data_connection_relationship_field_core_name_mismatch", "Data connection relationship field relatedAttributeCoreName differs from the imported attribute coreName.", relationshipFieldMappingContext);
											}

											if(!AttributeDataTypesCompatible(attributeDefinition.attributeType, relationshipFieldMapping.relatedAttributeDataType))
											{
												addDiagnostic(DiagnosticSeverity::Warning, "data_connection_relationship_field_type_mismatch", "Data connection relationship field relatedAttributeDataType differs from the imported attribute type.", relationshipFieldMappingContext + " expected:" + attributeDefinition.attributeType + " actual:" + relationshipFieldMapping.relatedAttributeDataType);
											}
										}
									}
								}

								entityMapping.relationshipFieldMappings.push_back(relationshipFieldMapping);
							}
						}
					}

					result.model.dataConnectionEntityMappings.push_back(entityMapping);
				}
			}
		}
	}

	const std::string rootTransactionStart = FirstNonEmpty(root, {"asOfTransactionTime", "transactionStart"});
	const std::string rootTransactionEnd = GetString(root, "transactionEnd");
	const std::string rootAssertedAt = GetString(root, "assertedAt");
	const std::string rootEvaluationTime = GetString(root, "evaluationTime");
	const std::string rootModelTimeZone = GetString(root, "modelTimeZone");

	if(root.contains("instances"))
	{
		if(!root.at("instances").is_array())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "history_instances_invalid", "History instances payload must be an array.", "instances");
		}
		else
		{
			for(const json& instanceJson : root.at("instances"))
			{
				if(!instanceJson.is_object())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "history_instance_invalid", "History instance item is not an object.", "instances");

					continue;
				}

				const std::string entityId = GetString(instanceJson, "entityId");
				const std::string instanceId = GetString(instanceJson, "instanceId");

				if(entityId.empty() || instanceId.empty())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "history_instance_missing_key", "History instance requires entityId and instanceId.", instanceJson.dump());

					continue;
				}

				if(entityIds.find(entityId) == entityIds.end())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "history_entity_missing", "History instance references an unknown entity.", "entity:" + entityId);

					continue;
				}

				if(instanceJson.contains("existence"))
				{
					const json* existenceSegmentsJson = &instanceJson.at("existence");

					if(existenceSegmentsJson->is_object() && existenceSegmentsJson->contains("segments") && existenceSegmentsJson->at("segments").is_array())
					{
						existenceSegmentsJson = &existenceSegmentsJson->at("segments");
					}

					if(!existenceSegmentsJson->is_array())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "existence_history_segments_invalid", "Entity existence history payload must be an array.", "entity:" + entityId + " instance:" + instanceId);
					}
					else
					{
						for(const json& segmentJson : *existenceSegmentsJson)
						{
							if(!segmentJson.is_object())
							{
								addDiagnostic(DiagnosticSeverity::Warning, "existence_history_segment_invalid", "Entity existence history segment is not an object.", "entity:" + entityId + " instance:" + instanceId);

								continue;
							}

							RuleSet::EntityExistenceHistorySegment segment;

							segment.entityId = entityId;
							segment.instanceId = instanceId;
							segment.effectiveStart = GetString(segmentJson, "effectiveStart");
							segment.effectiveEnd = segmentJson.contains("effectiveEnd") && segmentJson.at("effectiveEnd").is_null()
								? ""
								: GetString(segmentJson, "effectiveEnd");
							segment.knowledgeState = ParseHistoryKnowledgeState(FirstNonEmpty(segmentJson, {"knownness", "knowledgeState"}));
							segment.source = GetString(segmentJson, "source");
							segment.transactionStart = FirstNonEmpty(segmentJson, {"transactionStart", "asOfTransactionTime"});
							segment.transactionEnd = GetString(segmentJson, "transactionEnd");
							segment.assertedAt = GetString(segmentJson, "assertedAt");
							segment.evaluationTime = GetString(segmentJson, "evaluationTime");
							segment.modelTimeZone = GetString(segmentJson, "modelTimeZone");
							segment.provenance = GetString(segmentJson, "provenance");

							if(segment.transactionStart.empty())
							{
								segment.transactionStart = rootTransactionStart;
							}

							if(segment.transactionEnd.empty())
							{
								segment.transactionEnd = rootTransactionEnd;
							}

							if(segment.assertedAt.empty())
							{
								segment.assertedAt = rootAssertedAt;
							}

							if(segment.evaluationTime.empty())
							{
								segment.evaluationTime = rootEvaluationTime;
							}

							if(segment.modelTimeZone.empty())
							{
								segment.modelTimeZone = rootModelTimeZone;
							}

							if(segment.effectiveStart.empty())
							{
								addDiagnostic(DiagnosticSeverity::Warning, "existence_history_segment_missing_effective_start", "Entity existence history segment requires effectiveStart.", "entity:" + entityId + " instance:" + instanceId);

								continue;
							}

							result.model.entityExistenceHistorySegments.push_back(segment);
						}
					}
				}

				if(!instanceJson.contains("attributes") || !instanceJson.at("attributes").is_object())
				{
					continue;
				}

				const json& attributesJson = instanceJson.at("attributes");

				for(auto attributeIt = attributesJson.begin(); attributeIt != attributesJson.end(); ++attributeIt)
				{
					const std::string attributeId = attributeIt.key();
					const std::string attributeKey = RuleSet::AttributeDependencyKey(entityId, attributeId);
					auto attributeTypeIt = attributeTypeByEntityAttribute.find(attributeKey);

					if(attributeTypeIt == attributeTypeByEntityAttribute.end())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "history_attribute_missing", "History references an unknown attribute.", "entity:" + entityId + " attribute:" + attributeId);

						continue;
					}

					const json* segmentsJson = &attributeIt.value();

					if(segmentsJson->is_object() && segmentsJson->contains("segments") && segmentsJson->at("segments").is_array())
					{
						segmentsJson = &segmentsJson->at("segments");
					}

					if(!segmentsJson->is_array())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "history_segments_invalid", "Attribute history payload must be an array.", "entity:" + entityId + " attribute:" + attributeId);

						continue;
					}

					for(const json& segmentJson : *segmentsJson)
					{
						if(!segmentJson.is_object())
						{
							addDiagnostic(DiagnosticSeverity::Warning, "history_segment_invalid", "Attribute history segment is not an object.", "entity:" + entityId + " attribute:" + attributeId);

							continue;
						}

						RuleSet::AttributeHistorySegment segment;

						segment.entityId = entityId;
						segment.instanceId = instanceId;
						segment.attributeId = attributeId;
						segment.effectiveStart = GetString(segmentJson, "effectiveStart");
						segment.effectiveEnd = segmentJson.contains("effectiveEnd") && segmentJson.at("effectiveEnd").is_null()
							? ""
							: GetString(segmentJson, "effectiveEnd");
						segment.value = GetString(segmentJson, "value");
						segment.valueType = ParseHistoryValueType(FirstNonEmpty(segmentJson, {"valueType", "type"}), attributeTypeIt->second);
						segment.knowledgeState = ParseHistoryKnowledgeState(FirstNonEmpty(segmentJson, {"knownness", "knowledgeState"}));
						segment.source = GetString(segmentJson, "source");
						segment.transactionStart = FirstNonEmpty(segmentJson, {"transactionStart", "asOfTransactionTime"});
						segment.transactionEnd = GetString(segmentJson, "transactionEnd");
						segment.assertedAt = GetString(segmentJson, "assertedAt");
						segment.evaluationTime = GetString(segmentJson, "evaluationTime");
						segment.modelTimeZone = GetString(segmentJson, "modelTimeZone");
						segment.provenance = GetString(segmentJson, "provenance");

						if(segment.transactionStart.empty())
						{
							segment.transactionStart = rootTransactionStart;
						}

						if(segment.transactionEnd.empty())
						{
							segment.transactionEnd = rootTransactionEnd;
						}

						if(segment.assertedAt.empty())
						{
							segment.assertedAt = rootAssertedAt;
						}

						if(segment.evaluationTime.empty())
						{
							segment.evaluationTime = rootEvaluationTime;
						}

						if(segment.modelTimeZone.empty())
						{
							segment.modelTimeZone = rootModelTimeZone;
						}

						if(!RuleSet::IsValidTransactionWindow(segment.transactionStart, segment.transactionEnd))
						{
							addDiagnostic(DiagnosticSeverity::Warning, "history_transaction_time_invalid", "Attribute history segment has invalid transactionStart/transactionEnd metadata.", "entity:" + entityId + " instance:" + instanceId + " attribute:" + attributeId);
						}

						if(segment.effectiveStart.empty())
						{
							addDiagnostic(DiagnosticSeverity::Warning, "history_segment_missing_effective_start", "Attribute history segment requires effectiveStart.", "entity:" + entityId + " instance:" + instanceId + " attribute:" + attributeId);

							continue;
						}

						result.model.attributeHistorySegments.push_back(segment);
					}
				}
			}
		}
	}

	for(const json& relationshipJson : relationships)
	{
		if(!relationshipJson.is_object() || (!relationshipJson.contains("links") && !relationshipJson.contains("coverage")))
		{
			continue;
		}

		const std::string relationshipId = GetString(relationshipJson, "relationshipId");
		auto relationshipIt = relationshipById.find(relationshipId);

		if(relationshipIt == relationshipById.end())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "relationship_history_relationship_missing", "Relationship history references an unknown relationship.", "relationship:" + relationshipId);

			continue;
		}

		const RelationshipIndexEntry& relationship = relationshipIt->second;
		const std::string parentSourceInstanceId = FirstNonEmpty(relationshipJson, {"sourceInstanceId", "fromInstanceId"});
		const std::string parentSourceEntityId = FirstNonEmpty(relationshipJson, {"sourceEntityId", "fromEntityId"});
		const std::string parentTargetEntityId = FirstNonEmpty(relationshipJson, {"targetEntityId", "toEntityId"});

		if(relationshipJson.contains("coverage"))
		{
			const json* coverageSegmentsJson = &relationshipJson.at("coverage");

			if(coverageSegmentsJson->is_object() && coverageSegmentsJson->contains("segments") && coverageSegmentsJson->at("segments").is_array())
			{
				coverageSegmentsJson = &coverageSegmentsJson->at("segments");
			}

			if(!coverageSegmentsJson->is_array())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "relationship_coverage_history_segments_invalid", "Relationship coverage history payload must be an array.", "relationship:" + relationshipId);
			}
			else
			{
				for(const json& segmentJson : *coverageSegmentsJson)
				{
					if(!segmentJson.is_object())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "relationship_coverage_history_segment_invalid", "Relationship coverage history segment is not an object.", "relationship:" + relationshipId);

						continue;
					}

					RuleSet::RelationshipCoverageHistorySegment segment;

					segment.relationshipId = relationshipId;
					segment.sourceInstanceId = FirstNonEmpty(segmentJson, {"sourceInstanceId", "fromInstanceId"});
					segment.effectiveStart = GetString(segmentJson, "effectiveStart");
					segment.effectiveEnd = segmentJson.contains("effectiveEnd") && segmentJson.at("effectiveEnd").is_null()
						? ""
						: GetString(segmentJson, "effectiveEnd");
					segment.knowledgeState = ParseHistoryKnowledgeState(FirstNonEmpty(segmentJson, {"knownness", "knowledgeState"}));
					segment.source = GetString(segmentJson, "source");
					segment.transactionStart = FirstNonEmpty(segmentJson, {"transactionStart", "asOfTransactionTime"});
					segment.transactionEnd = GetString(segmentJson, "transactionEnd");
					segment.assertedAt = GetString(segmentJson, "assertedAt");
					segment.evaluationTime = GetString(segmentJson, "evaluationTime");
					segment.modelTimeZone = GetString(segmentJson, "modelTimeZone");
					segment.provenance = GetString(segmentJson, "provenance");

					if(segment.sourceInstanceId.empty())
					{
						segment.sourceInstanceId = parentSourceInstanceId;
					}

					if(segment.transactionStart.empty())
					{
						segment.transactionStart = rootTransactionStart;
					}

					if(segment.transactionEnd.empty())
					{
						segment.transactionEnd = rootTransactionEnd;
					}

					if(segment.assertedAt.empty())
					{
						segment.assertedAt = rootAssertedAt;
					}

					if(segment.evaluationTime.empty())
					{
						segment.evaluationTime = rootEvaluationTime;
					}

					if(segment.modelTimeZone.empty())
					{
						segment.modelTimeZone = rootModelTimeZone;
					}

					if(segment.sourceInstanceId.empty())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "relationship_coverage_history_missing_source", "Relationship coverage history requires sourceInstanceId.", "relationship:" + relationshipId);

						continue;
					}

					if(segment.effectiveStart.empty())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "relationship_coverage_history_missing_effective_start", "Relationship coverage history segment requires effectiveStart.", "relationship:" + relationshipId + " source:" + segment.sourceInstanceId);

						continue;
					}

					result.model.relationshipCoverageHistorySegments.push_back(segment);
				}
			}
		}

		if(!relationshipJson.contains("links"))
		{
			continue;
		}

		const json& linksJson = relationshipJson.at("links");

		if(!linksJson.is_array())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "relationship_history_links_invalid", "Relationship history links payload must be an array.", "relationship:" + relationshipId);

			continue;
		}

		for(const json& linkJson : linksJson)
		{
			if(!linkJson.is_object())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "relationship_history_link_invalid", "Relationship history link is not an object.", "relationship:" + relationshipId);

				continue;
			}

			RuleSet::RelationshipLinkHistorySegment segment;

			segment.relationshipId = relationshipId;
			segment.sourceEntityId = FirstNonEmpty(linkJson, {"sourceEntityId", "fromEntityId"});
			segment.sourceInstanceId = FirstNonEmpty(linkJson, {"sourceInstanceId", "fromInstanceId"});
			segment.targetEntityId = FirstNonEmpty(linkJson, {"targetEntityId", "toEntityId"});
			segment.targetInstanceId = FirstNonEmpty(linkJson, {"targetInstanceId", "toInstanceId", "instanceId"});
			segment.effectiveStart = GetString(linkJson, "effectiveStart");
			segment.effectiveEnd = linkJson.contains("effectiveEnd") && linkJson.at("effectiveEnd").is_null()
				? ""
				: GetString(linkJson, "effectiveEnd");
			segment.knowledgeState = ParseHistoryKnowledgeState(FirstNonEmpty(linkJson, {"knownness", "knowledgeState"}));
			segment.source = GetString(linkJson, "source");
			segment.transactionStart = FirstNonEmpty(linkJson, {"transactionStart", "asOfTransactionTime"});
			segment.transactionEnd = GetString(linkJson, "transactionEnd");
			segment.assertedAt = GetString(linkJson, "assertedAt");
			segment.evaluationTime = GetString(linkJson, "evaluationTime");
			segment.modelTimeZone = GetString(linkJson, "modelTimeZone");
			segment.provenance = GetString(linkJson, "provenance");

			if(segment.sourceEntityId.empty())
			{
				segment.sourceEntityId = parentSourceEntityId.empty() ? relationship.fromEntityId : parentSourceEntityId;
			}

			if(segment.sourceInstanceId.empty())
			{
				segment.sourceInstanceId = parentSourceInstanceId;
			}

			if(segment.targetEntityId.empty())
			{
				segment.targetEntityId = parentTargetEntityId.empty() ? relationship.toEntityId : parentTargetEntityId;
			}

			if(segment.transactionStart.empty())
			{
				segment.transactionStart = rootTransactionStart;
			}

			if(segment.transactionEnd.empty())
			{
				segment.transactionEnd = rootTransactionEnd;
			}

			if(segment.assertedAt.empty())
			{
				segment.assertedAt = rootAssertedAt;
			}

			if(segment.evaluationTime.empty())
			{
				segment.evaluationTime = rootEvaluationTime;
			}

			if(segment.modelTimeZone.empty())
			{
				segment.modelTimeZone = rootModelTimeZone;
			}

			if(segment.sourceInstanceId.empty() || segment.targetInstanceId.empty())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "relationship_history_link_missing_instance", "Relationship history link requires sourceInstanceId and targetInstanceId.", "relationship:" + relationshipId);

				continue;
			}

			if(segment.effectiveStart.empty())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "relationship_history_link_missing_effective_start", "Relationship history link requires effectiveStart.", "relationship:" + relationshipId + " source:" + segment.sourceInstanceId + " target:" + segment.targetInstanceId);

				continue;
			}

			result.model.relationshipLinkHistorySegments.push_back(segment);
		}
	}

	const json emptyRules = json::array();
	const json& rules = root.contains("rules") ? root.at("rules") : emptyRules;

	for(const json& ruleJson : rules)
	{
		if(!ruleJson.is_object())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "rule_invalid", "Rule item is not an object.", "rules");

			continue;
		}

		RuleDefinition rule;

		rule.ruleId = GetString(ruleJson, "ruleId");
		rule.ruleName = GetString(ruleJson, "ruleName");
		rule.isActive = GetBool(ruleJson, "isActive", false);
		rule.conclusionMode = GetString(ruleJson, "conclusionMode");
		rule.conclusionEntityId = GetString(ruleJson, "conclusionEntityId");
		rule.conclusionAttributeId = GetString(ruleJson, "conclusionAttributeId");
		rule.conclusionValue = GetString(ruleJson, "conclusionValue");
		rule.conclusionValueMode = GetString(ruleJson, "conclusionValueMode");
		rule.elseValue = GetString(ruleJson, "elseValue");
		rule.relationshipId = GetString(ruleJson, "relationshipId");

		if(ruleJson.contains("conclusionExpression") && ruleJson.at("conclusionExpression").is_object())
		{
			rule.conclusionExpression = ruleJson.at("conclusionExpression");
			CollectReferences(rule.conclusionExpression, rule.ruleId, "rule.conclusionExpression", rule.references, result.model.operatorCounts, result.model.functionCounts);
			CollectAstReferences(rule.conclusionExpression,
			                     rule.ruleId,
			                     "rule.conclusionExpression",
			                     rule.conclusionEntityId,
			                     rule.relationshipId,
			                     rule.references,
			                     entityIdByLookup,
				                     attributeIdByEntityLookup,
				                     relationshipByLookup);
		}

		if(!rule.conclusionExpression.is_object()
			&& (rule.conclusionValueMode.empty() || rule.conclusionValueMode == "Literal"))
		{
			CollectExpressionReferences(rule.conclusionValue,
			                            rule.ruleId,
			                            "rule.conclusionValue",
			                            rule.conclusionEntityId,
			                            rule.relationshipId,
			                            rule.references,
			                            result.model.functionCounts,
			                            entityIdByLookup,
			                            attributeIdByEntityLookup,
			                            relationshipByLookup);
		}

		if(rule.ruleId.empty())
		{
			addDiagnostic(DiagnosticSeverity::Error, "rule_missing_id", "Rule is missing ruleId.", rule.ruleName);
		}

		if(ruleJson.contains("condition"))
		{
			rule.condition = ruleJson.at("condition");

			CollectReferences(rule.condition, rule.ruleId, "condition", rule.references, result.model.operatorCounts, result.model.functionCounts);
			CollectAstReferences(rule.condition,
			                     rule.ruleId,
			                     "condition",
			                     rule.conclusionEntityId,
			                     rule.relationshipId,
			                     rule.references,
			                     entityIdByLookup,
			                     attributeIdByEntityLookup,
			                     relationshipByLookup);

			if(!rule.conclusionExpression.is_object()
				&& rule.conclusionMode == "Value"
				&& !rule.conclusionAttributeId.empty()
				&& rule.elseValue.empty()
				&& (rule.conclusionValueMode.empty() || rule.conclusionValueMode == "Literal"))
			{
				std::string calculatedConclusionValue;

				if(TryBuildPromotedCalculatedValueExpression(rule.condition, calculatedConclusionValue)
					&& (rule.conclusionValue.empty()
						|| CalculatedConclusionMatchesPromotedExpression(
							rule.conclusionValue,
							calculatedConclusionValue,
							rule.conclusionEntityId,
							attributeIdByEntityLookup,
							relationshipByLookup)))
				{
					const bool conclusionWasEmpty = rule.conclusionValue.empty();
					rule.conclusionValue = calculatedConclusionValue;
					rule.conclusionValueMode = "Literal";
					rule.condition = json{
						{"nodeType", "condition"},
						{"operator", "any"}
					};

					if(conclusionWasEmpty)
					{
						CollectExpressionReferences(rule.conclusionValue,
						                            rule.ruleId,
						                            "rule.conclusionValue",
						                            rule.conclusionEntityId,
						                            rule.relationshipId,
						                            rule.references,
						                            result.model.functionCounts,
						                            entityIdByLookup,
						                            attributeIdByEntityLookup,
						                            relationshipByLookup);
					}
				}
			}
		}

		if(ruleJson.contains("conclusions") && ruleJson.at("conclusions").is_array())
		{
			for(const json& conclusionJson : ruleJson.at("conclusions"))
			{
				CollectReferences(conclusionJson, rule.ruleId, "conclusions", rule.references, result.model.operatorCounts, result.model.functionCounts);
				CollectAstReferences(conclusionJson,
				                     rule.ruleId,
				                     "conclusions",
				                     rule.conclusionEntityId,
				                     rule.relationshipId,
				                     rule.references,
				                     entityIdByLookup,
				                     attributeIdByEntityLookup,
				                     relationshipByLookup);
			}
		}

		if(ruleJson.contains("executableDecisionTable") && ruleJson.at("executableDecisionTable").is_object())
		{
			rule.hasExecutableDecisionTable = true;
			rule.executableDecisionTable = ruleJson.at("executableDecisionTable");
			NormalizeDecisionTableOtherwiseRows(rule.executableDecisionTable);

			const json& table = rule.executableDecisionTable;

			if(table.contains("rows") && table.at("rows").is_array())
			{
				rule.decisionTableRows = static_cast<int>(table.at("rows").size());

				for(const json& rowJson : table.at("rows"))
				{
					if(rowJson.contains("conditionCells") && rowJson.at("conditionCells").is_array())
					{
						for(const json& cellJson : rowJson.at("conditionCells"))
						{
							CollectReferences(cellJson, rule.ruleId, "decisionTable.conditionCells", rule.references, result.model.operatorCounts, result.model.functionCounts);
							CollectAstReferences(cellJson,
							                     rule.ruleId,
							                     "decisionTable.conditionCells",
							                     rule.conclusionEntityId,
							                     rule.relationshipId,
							                     rule.references,
							                     entityIdByLookup,
							                     attributeIdByEntityLookup,
							                     relationshipByLookup);
						}
					}

					if(rowJson.contains("conclusionCells") && rowJson.at("conclusionCells").is_array())
					{
						for(const json& cellJson : rowJson.at("conclusionCells"))
						{
							CollectReferences(cellJson, rule.ruleId, "decisionTable.conclusionCells", rule.references, result.model.operatorCounts, result.model.functionCounts);
							CollectAstReferences(cellJson,
							                     rule.ruleId,
							                     "decisionTable.conclusionCells",
							                     rule.conclusionEntityId,
							                     rule.relationshipId,
							                     rule.references,
							                     entityIdByLookup,
							                     attributeIdByEntityLookup,
							                     relationshipByLookup);

							const std::string cellValueMode = GetString(cellJson, "valueMode");

							if(cellValueMode.empty() || cellValueMode == "Literal")
							{
								CollectExpressionReferences(GetString(cellJson, "value"),
								                            rule.ruleId,
								                            "decisionTable.conclusionCells",
								                            rule.conclusionEntityId,
								                            rule.relationshipId,
								                            rule.references,
								                            result.model.functionCounts,
								                            entityIdByLookup,
								                            attributeIdByEntityLookup,
								                            relationshipByLookup);
							}
						}
					}
				}
			}
		}

		if(rule.conclusionMode == "DecisionTable" && !rule.hasExecutableDecisionTable)
		{
			addDiagnostic(DiagnosticSeverity::Warning, "decision_table_missing_executable", "DecisionTable rule has no executableDecisionTable block.", "rule:" + rule.ruleId);
		}

		if(!rule.relationshipId.empty())
		{
			RuleReference reference;

			reference.ruleId = rule.ruleId;
			reference.source = "rule.relationshipId";
			reference.relationshipId = rule.relationshipId;

			rule.references.push_back(reference);
		}

		if(rule.conclusionValueMode == "Attribute" && !rule.conclusionValue.empty())
		{
			RuleReference reference;

			reference.ruleId = rule.ruleId;
			reference.source = "rule.conclusionValue";
			reference.entityId = rule.conclusionEntityId;
			reference.attributeId = rule.conclusionValue;
			reference.relationshipId = rule.relationshipId;

			rule.references.push_back(reference);
		}

		DeduplicateRuleReferences(rule.references);

		if(!rule.conclusionEntityId.empty() && entityIds.find(rule.conclusionEntityId) == entityIds.end())
		{
			addDiagnostic(DiagnosticSeverity::Warning, "rule_conclusion_entity_missing", "Rule conclusionEntityId does not resolve to an entity.", "rule:" + rule.ruleId + " entity:" + rule.conclusionEntityId);
		}

		if(!rule.conclusionAttributeId.empty())
		{
			auto entityIt = attributesByEntity.find(rule.conclusionEntityId);

			if(rule.conclusionEntityId.empty() || entityIt == attributesByEntity.end() || entityIt->second.find(rule.conclusionAttributeId) == entityIt->second.end())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "rule_conclusion_attribute_missing", "Rule conclusionAttributeId does not resolve within conclusionEntityId.", "rule:" + rule.ruleId + " entity:" + rule.conclusionEntityId + " attribute:" + rule.conclusionAttributeId);
			}
		}

		for(const RuleReference& reference : rule.references)
		{
			if(!reference.entityId.empty() && entityIds.find(reference.entityId) == entityIds.end())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "rule_reference_entity_missing", "Rule references an unknown entity.", ReferenceContext(reference));
			}

			if(!reference.attributeId.empty())
			{
				if(!reference.entityId.empty())
				{
					auto entityIt = attributesByEntity.find(reference.entityId);

					if(entityIt == attributesByEntity.end() || entityIt->second.find(reference.attributeId) == entityIt->second.end())
					{
						addDiagnostic(DiagnosticSeverity::Warning, "rule_reference_attribute_missing", "Rule references an attribute that is not on the referenced entity.", ReferenceContext(reference));
					}
				}
				else if(attributeIdCounts.find(reference.attributeId) == attributeIdCounts.end())
				{
					addDiagnostic(DiagnosticSeverity::Warning, "rule_reference_attribute_missing", "Rule references an unknown attribute.", ReferenceContext(reference));
				}
			}

			if(!reference.relationshipId.empty() && relationshipIds.find(reference.relationshipId) == relationshipIds.end())
			{
				addDiagnostic(DiagnosticSeverity::Warning, "relationship_reference_missing", "Rule references an unknown relationship.", "rule:" + reference.ruleId + " relationship:" + reference.relationshipId);
			}
		}

		if(rule.isActive && !rule.ruleId.empty())
		{
			if(!rule.conclusionEntityId.empty() && !rule.conclusionAttributeId.empty())
			{
				result.model.activeRuleIdsByConcludedAttribute[RuleSet::AttributeDependencyKey(rule.conclusionEntityId, rule.conclusionAttributeId)].push_back(rule.ruleId);
			}

			std::unordered_set<std::string> indexedReferenceKeys;
			std::unordered_set<std::string> indexedRelationshipKeys;
			std::unordered_set<std::string> indexedFunctionKeys;

			for(const RuleReference& reference : rule.references)
			{
				if(IsInputReferenceSource(reference.source) && !reference.entityId.empty() && !reference.attributeId.empty())
				{
					const std::string key = RuleSet::AttributeDependencyKey(reference.entityId, reference.attributeId);

					if(indexedReferenceKeys.insert(key).second)
					{
						result.model.activeRuleIdsByReferencedAttribute[key].push_back(rule.ruleId);
					}
				}

				if((IsInputReferenceSource(reference.source) || StartsWith(reference.source, "decisionTable.conclusionCells"))
					&& IsSessionContextFunctionName(reference.functionName))
				{
					if(indexedFunctionKeys.insert(reference.functionName).second)
					{
						result.model.activeRuleIdsByReferencedFunction[reference.functionName].push_back(rule.ruleId);
					}
				}

				if((IsInputReferenceSource(reference.source) || (rule.conclusionMode == "Relationship" && reference.source == "rule.relationshipId")) && !reference.relationshipId.empty())
				{
					if(indexedRelationshipKeys.insert(reference.relationshipId).second)
					{
						result.model.activeRuleIdsByReferencedRelationship[reference.relationshipId].push_back(rule.ruleId);
					}
				}
			}
		}

		result.model.rules.push_back(rule);
	}

	return result;
}

SessionModelImporter::RuntimeConversionResult SessionModelImporter::ConvertToRuntimeRuleSet(const SessionModel& model) const
{
	RuntimeConversionResult result;
	result.ruleSet.forgetPolicyEnabled = model.forgetPolicyEnabled;
	result.ruleSet.externalSecurity = model.externalSecurity;
	result.ruleSet.notify = model.notify;
	std::unordered_map<std::string, std::shared_ptr<Entity>> entitiesById;
	std::unordered_map<std::string, int> incomingRelationshipCounts;
	std::unordered_map<std::string, std::unordered_set<std::string>> concludedAttributesByEntity;
	std::unordered_set<std::string> rootEntityIds;

	for(const EntityDefinition& entity : model.entities)
	{
		if(!entity.entityId.empty() && (IsRootEntityName(entity.entityName) || IsRootEntityName(entity.entityId)))
		{
			rootEntityIds.insert(entity.entityId);
		}
	}

	for(const RelationshipDefinition& relationship : model.relationships)
	{
		if(RelationshipMakesRootRepeatable(relationship, rootEntityIds))
		{
			result.diagnostics.push_back(Diagnostic{
				DiagnosticSeverity::Error,
				"root_singleton_relationship_invalid",
				"Root must remain a singleton. Relationship cardinality places Root on a repeatable side.",
				RootSingletonRelationshipContext(relationship)
			});

			continue;
		}

		if(!relationship.toEntityId.empty())
		{
			incomingRelationshipCounts[relationship.toEntityId]++;
		}
	}

	for(const RuleDefinition& rule : model.rules)
	{
		if(!rule.conclusionEntityId.empty() && !rule.conclusionAttributeId.empty())
		{
			concludedAttributesByEntity[rule.conclusionEntityId].insert(rule.conclusionAttributeId);
		}
	}

	for(const EntityDefinition& sourceEntity : model.entities)
	{
		std::shared_ptr<Entity> entity = std::make_shared<Entity>();

		entity->entityid = sourceEntity.entityId;
		entity->description = sourceEntity.entityName;
		entity->root = incomingRelationshipCounts[sourceEntity.entityId] == 0;
		entity->useRuntimeDefinitionsForInstances = true;

		for(const AttributeDefinition& sourceAttribute : sourceEntity.attributes)
		{
			(*result.ruleSet.attributeForgetPolicyModeByKey)[RuleSet::AttributeDependencyKey(sourceEntity.entityId, sourceAttribute.attributeId)] = sourceAttribute.forgetPolicyMode;

			JSONFile::JSONAttribute jsonAttribute;

			jsonAttribute.attributeid = sourceAttribute.attributeId;
			jsonAttribute.text = sourceAttribute.attributeName;
			jsonAttribute.coreName = sourceAttribute.coreName;
			jsonAttribute.type = sourceAttribute.engineType;
			if(jsonAttribute.type == "auto" || jsonAttribute.type == "unknown")
			{
				jsonAttribute.type = "text";
			}
			jsonAttribute.defaultvalue = sourceAttribute.defaultValue;
			jsonAttribute.mainIdentifier = sourceAttribute.mainIdentifier;

			if(concludedAttributesByEntity[sourceEntity.entityId].find(sourceAttribute.attributeId) != concludedAttributesByEntity[sourceEntity.entityId].end())
			{
				jsonAttribute.inferredbyrule = "session-model";
			}

			if(jsonAttribute.type == "text" && !IsRuntimeSupportedType(sourceAttribute.attributeType))
			{
				result.diagnostics.push_back(Diagnostic{
					DiagnosticSeverity::Warning,
					"runtime_type_fallback",
					"Attribute type is not natively supported yet and was converted to text for runtime import.",
					"entity:" + sourceEntity.entityId + " attribute:" + sourceAttribute.attributeId + " type:" + sourceAttribute.attributeType
				});
			}

			shared_ptr<Attribute> runtimeAttribute = std::make_shared<Attribute>(jsonAttribute);
			if(runtimeAttribute->IsMainIdentifier())
			{
				entity->identifyingAttribute = runtimeAttribute;
			}

			entity->attributes.push_back(runtimeAttribute);

			if(sourceAttribute.startSupplied && rootEntityIds.find(sourceEntity.entityId) != rootEntityIds.end())
			{
				result.ruleSet.startInputs->push_back(RuleSet::StartInputDefinition{
					sourceEntity.entityId,
					sourceAttribute.attributeId,
					sourceAttribute.attributeName,
					sourceAttribute.coreName,
					sourceAttribute.attributeType,
					true
				});
			}
		}

		result.ruleSet.entities->push_back(entity);
		entitiesById[entity->entityid] = entity;
	}

	for(const RelationshipDefinition& sourceRelationship : model.relationships)
	{
		(*result.ruleSet.relationshipForgetPolicyModeById)[sourceRelationship.relationshipId] = sourceRelationship.forgetPolicyMode;

		if(RelationshipMakesRootRepeatable(sourceRelationship, rootEntityIds))
		{
			continue;
		}

		auto sourceEntityIt = entitiesById.find(sourceRelationship.fromEntityId);

		if(sourceEntityIt == entitiesById.end())
		{
			result.diagnostics.push_back(Diagnostic{
				DiagnosticSeverity::Error,
				"runtime_relationship_source_missing",
				"Relationship source entity was not imported.",
				"relationship:" + sourceRelationship.relationshipId + " entity:" + sourceRelationship.fromEntityId
			});

			continue;
		}

		if(entitiesById.find(sourceRelationship.toEntityId) == entitiesById.end())
		{
			result.diagnostics.push_back(Diagnostic{
				DiagnosticSeverity::Error,
				"runtime_relationship_target_missing",
				"Relationship target entity was not imported.",
				"relationship:" + sourceRelationship.relationshipId + " entity:" + sourceRelationship.toEntityId
			});

			continue;
		}

		std::shared_ptr<Relationship> relationship = std::make_shared<Relationship>();

		relationship->id = sourceRelationship.relationshipId;
		relationship->description = sourceRelationship.relationshipName;
		relationship->sourceEntity = sourceRelationship.fromEntityId;
		relationship->targetEntity = sourceRelationship.toEntityId;
		relationship->type = NormalizeCardinality(sourceRelationship.cardinality);

		sourceEntityIt->second->relationships.push_back(relationship);
	}

	result.ruleSet.RebuildRelationshipDefinitionIndex();

	result.ruleSet.sessionModelRules->reserve(model.rules.size());

	for(const RuleDefinition& sourceRule : model.rules)
	{
		RuleSet::SessionModelRuleMetadata runtimeRule;

		runtimeRule.ruleId = sourceRule.ruleId;
		runtimeRule.ruleName = sourceRule.ruleName;
		runtimeRule.isActive = sourceRule.isActive;
		runtimeRule.conclusionMode = sourceRule.conclusionMode;
		runtimeRule.conclusionEntityId = sourceRule.conclusionEntityId;
		runtimeRule.conclusionAttributeId = sourceRule.conclusionAttributeId;
		runtimeRule.conclusionValue = sourceRule.conclusionValue;
		runtimeRule.conclusionValueMode = sourceRule.conclusionValueMode;
		runtimeRule.conclusionExpression = sourceRule.conclusionExpression;
		runtimeRule.elseValue = sourceRule.elseValue;
		runtimeRule.relationshipId = sourceRule.relationshipId;
		runtimeRule.hasExecutableDecisionTable = sourceRule.hasExecutableDecisionTable;
		runtimeRule.decisionTableRows = sourceRule.decisionTableRows;
		runtimeRule.condition = sourceRule.condition;
		runtimeRule.executableDecisionTable = sourceRule.executableDecisionTable;
		runtimeRule.RebuildDecisionTableMetadata();
		runtimeRule.references.reserve(sourceRule.references.size());

		for(const RuleReference& sourceReference : sourceRule.references)
		{
			RuleSet::SessionModelRuleReference runtimeReference;

			runtimeReference.source = sourceReference.source;
			runtimeReference.entityId = sourceReference.entityId;
			runtimeReference.attributeId = sourceReference.attributeId;
			runtimeReference.relationshipId = sourceReference.relationshipId;
			runtimeReference.nodeType = sourceReference.nodeType;
			runtimeReference.operatorName = sourceReference.operatorName;
			runtimeReference.functionName = sourceReference.functionName;

			runtimeRule.references.push_back(runtimeReference);
		}

		result.ruleSet.sessionModelRules->push_back(runtimeRule);
	}

	result.ruleSet.RebuildSessionModelRuleIndex();

	*result.ruleSet.activeSessionModelRuleIdsByReferencedAttribute = model.activeRuleIdsByReferencedAttribute;
	*result.ruleSet.activeSessionModelRuleIdsByReferencedRelationship = model.activeRuleIdsByReferencedRelationship;
	*result.ruleSet.activeSessionModelRuleIdsByReferencedFunction = model.activeRuleIdsByReferencedFunction;
	*result.ruleSet.activeSessionModelRuleIdsByConcludedAttribute = model.activeRuleIdsByConcludedAttribute;

	*result.ruleSet.dataConnectionSources = model.dataConnectionSources;
	*result.ruleSet.dataConnectionEntityMappings = model.dataConnectionEntityMappings;
	*result.ruleSet.pdfTemplates = model.pdfTemplates;
	*result.ruleSet.emailTemplates = model.emailTemplates;

	for(const RuleSet::AttributeHistorySegment& segment : model.attributeHistorySegments)
	{
		result.ruleSet.AddAttributeHistorySegment(segment);
	}

	for(const RuleSet::EntityExistenceHistorySegment& segment : model.entityExistenceHistorySegments)
	{
		result.ruleSet.AddEntityExistenceHistorySegment(segment);
	}

	for(const RuleSet::RelationshipLinkHistorySegment& segment : model.relationshipLinkHistorySegments)
	{
		result.ruleSet.AddRelationshipLinkHistorySegment(segment);
	}

	for(const RuleSet::RelationshipCoverageHistorySegment& segment : model.relationshipCoverageHistorySegments)
	{
		result.ruleSet.AddRelationshipCoverageHistorySegment(segment);
	}

	if(!model.rules.empty())
	{
		result.diagnostics.push_back(Diagnostic{
			DiagnosticSeverity::Info,
			"runtime_rules_not_converted",
			"SessionModel rules were parsed but not converted to runtime Rule objects yet.",
			"rules:" + std::to_string(model.rules.size())
		});
	}

	return result;
}

bool SessionModelImporter::LooksLikeSessionModel(const json& root)
{
	if(!root.is_object()
		|| !root.contains("entities") || !root.at("entities").is_array())
	{
		return false;
	}

	const bool hasRulesArray = root.contains("rules") && root.at("rules").is_array();
	const bool hasScreensArray = root.contains("screens") && root.at("screens").is_array();

	if(!hasRulesArray && !hasScreensArray)
	{
		return false;
	}

	return !root.contains("relationships") || root.at("relationships").is_array();
}

std::string SessionModelImporter::MapAttributeType(const std::string& attributeType)
{
	if(attributeType == "Text")
	{
		return "text";
	}

	if(attributeType == "Boolean")
	{
		return "boolean";
	}

	if(attributeType == "Number")
	{
		return "double";
	}

	if(attributeType == "Currency")
	{
		return "double";
	}

	if(attributeType == "Date")
	{
		return "date";
	}

	if(attributeType == "DateTime")
	{
		return "datetime";
	}

	if(attributeType == "Time" || attributeType == "TimeOfDay")
	{
		return "timeofday";
	}

	if(attributeType == "Auto")
	{
		return "auto";
	}

	return "unknown";
}

std::string SessionModelImporter::RuntimeAttributeType(const std::string& attributeType)
{
	const std::string mappedType = MapAttributeType(attributeType);

	if(mappedType == "auto" || mappedType == "unknown")
	{
		return "text";
	}

	return mappedType;
}

bool SessionModelImporter::IsRuntimeSupportedType(const std::string& attributeType)
{
	const std::string mappedType = MapAttributeType(attributeType);

	return mappedType == "text"
		|| mappedType == "boolean"
		|| mappedType == "double"
		|| mappedType == "date"
		|| mappedType == "datetime"
		|| mappedType == "timeofday";
}

std::string SessionModelImporter::NormalizeCardinality(const std::string& cardinality)
{
	if(cardinality == "One-to-many")
	{
		return "1M";
	}

	if(cardinality == "Many-to-one")
	{
		return "M1";
	}

	if(cardinality == "Many-to-many")
	{
		return "MM";
	}

	if(cardinality == "One-to-one")
	{
		return "11";
	}

	return cardinality;
}
