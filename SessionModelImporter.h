#pragma once

#include "json.hpp"
#include "RuleSet.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

class SessionModelImporter
{
public:
	enum class DiagnosticSeverity
	{
		Info,
		Warning,
		Error
	};

	struct Diagnostic
	{
		DiagnosticSeverity severity = DiagnosticSeverity::Info;
		std::string code;
		std::string message;
		std::string context;
	};

	struct AttributeDefinition
	{
		std::string attributeId;
		std::string attributeName;
		std::string coreName;
		std::string attributeType;
		std::string defaultValue;
		bool wholeInteger = false;
		bool mainIdentifier = false;
		bool startSupplied = false;
		std::string engineType;
		bool runtimeSupported = false;
		std::string forgetPolicyMode = "Forget";
	};

	struct EntityDefinition
	{
		std::string entityId;
		std::string entityName;
		std::vector<AttributeDefinition> attributes;
	};

	struct RelationshipDefinition
	{
		std::string relationshipId;
		std::string relationshipName;
		std::string fromEntityId;
		std::string fromEntityName;
		std::string toEntityId;
		std::string toEntityName;
		std::string cardinality;
		std::string forgetPolicyMode = "Forget";
	};

	struct RuleReference
	{
		std::string ruleId;
		std::string source;
		std::string entityId;
		std::string attributeId;
		std::string relationshipId;
		std::string nodeType;
		std::string operatorName;
		std::string functionName;
	};

	struct RuleDefinition
	{
		std::string ruleId;
		std::string ruleName;
		bool isActive = false;
		std::string conclusionMode;
		std::string conclusionEntityId;
		std::string conclusionAttributeId;
		std::string conclusionValue;
		std::string conclusionValueMode;
		nlohmann::json conclusionExpression;
		std::string elseValue;
		std::string relationshipId;
		bool hasExecutableDecisionTable = false;
		int decisionTableRows = 0;
		nlohmann::json condition;
		nlohmann::json executableDecisionTable;
		std::vector<RuleReference> references;
	};

	struct SessionModel
	{
		bool forgetPolicyEnabled = false;
		RuleSet::ExternalSecurityPolicy externalSecurity;
		RuleSet::NotifyPolicy notify;
		std::vector<EntityDefinition> entities;
		std::vector<RelationshipDefinition> relationships;
		std::vector<RuleDefinition> rules;
		std::vector<nlohmann::json> pdfTemplates;
		std::vector<nlohmann::json> emailTemplates;
		std::vector<RuleSet::DataConnectionSource> dataConnectionSources;
		std::vector<RuleSet::DataConnectionEntityMapping> dataConnectionEntityMappings;
		std::vector<RuleSet::AttributeHistorySegment> attributeHistorySegments;
		std::vector<RuleSet::EntityExistenceHistorySegment> entityExistenceHistorySegments;
		std::vector<RuleSet::RelationshipLinkHistorySegment> relationshipLinkHistorySegments;
		std::vector<RuleSet::RelationshipCoverageHistorySegment> relationshipCoverageHistorySegments;
		std::unordered_map<std::string, int> operatorCounts;
		std::unordered_map<std::string, int> functionCounts;
		std::unordered_map<std::string, std::vector<std::string>> activeRuleIdsByReferencedAttribute;
		std::unordered_map<std::string, std::vector<std::string>> activeRuleIdsByReferencedRelationship;
		std::unordered_map<std::string, std::vector<std::string>> activeRuleIdsByReferencedFunction;
		std::unordered_map<std::string, std::vector<std::string>> activeRuleIdsByConcludedAttribute;

		size_t AttributeCount() const
		{
			size_t count = 0;

			for(const EntityDefinition& entity : entities)
			{
				count += entity.attributes.size();
			}

			return count;
		}

		size_t DataConnectionAttributeMappingCount() const
		{
			size_t count = 0;

			for(const RuleSet::DataConnectionEntityMapping& mapping : dataConnectionEntityMappings)
			{
				count += mapping.attributeMappings.size();
			}

			return count;
		}

		size_t DataConnectionRelationshipFieldMappingCount() const
		{
			size_t count = 0;

			for(const RuleSet::DataConnectionEntityMapping& mapping : dataConnectionEntityMappings)
			{
				count += mapping.relationshipFieldMappings.size();
			}

			return count;
		}

		int OperatorCount(const std::string& name) const
		{
			auto it = operatorCounts.find(name);

			return it == operatorCounts.end() ? 0 : it->second;
		}

		int FunctionCount(const std::string& name) const
		{
			auto it = functionCounts.find(name);

			return it == functionCounts.end() ? 0 : it->second;
		}

		std::vector<std::string> GetActiveRulesReferencingAttribute(const std::string& entityId, const std::string& attributeId) const;
		std::vector<std::string> GetActiveRulesReferencingRelationship(const std::string& relationshipId) const;
		std::vector<std::string> GetActiveRulesReferencingFunction(const std::string& functionName) const;
		std::vector<std::string> GetActiveRulesConcludingAttribute(const std::string& entityId, const std::string& attributeId) const;
	};

	struct ImportResult
	{
		SessionModel model;
		std::vector<Diagnostic> diagnostics;

		bool HasErrors() const
		{
			return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
				return diagnostic.severity == DiagnosticSeverity::Error;
			});
		}

		int CountDiagnostics(const std::string& code) const
		{
			return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& diagnostic) {
				return diagnostic.code == code;
			}));
		}
	};

	struct RuntimeConversionResult
	{
		RuleSet ruleSet;
		std::vector<Diagnostic> diagnostics;

		bool HasErrors() const
		{
			return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
				return diagnostic.severity == DiagnosticSeverity::Error;
			});
		}

		int CountDiagnostics(const std::string& code) const
		{
			return static_cast<int>(std::count_if(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& diagnostic) {
				return diagnostic.code == code;
			}));
		}
	};

	ImportResult LoadFile(const std::string& filePath) const;
	ImportResult LoadJson(const nlohmann::json& root) const;
	RuntimeConversionResult ConvertToRuntimeRuleSet(const SessionModel& model) const;

	static bool LooksLikeSessionModel(const nlohmann::json& root);
	static std::string MapAttributeType(const std::string& attributeType);
	static std::string RuntimeAttributeType(const std::string& attributeType);
	static bool IsRuntimeSupportedType(const std::string& attributeType);
	static std::string NormalizeCardinality(const std::string& cardinality);
};
