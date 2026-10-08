#pragma once

#include "Rules/RuleSet.h"
#include "json.hpp"

#include <string>
#include <utility>
#include <vector>

class SessionModelRuleEvaluator
{
public:
	struct ChangedAttribute
	{
		std::string entityId;
		std::string instanceId;
		std::string attributeId;
	};

	struct EvaluationDiagnostic
	{
		std::string ruleId;
		std::string code;
		std::string message;
		std::string context;
	};

	struct EvaluationStats
	{
		struct EvaluationTraceNode
		{
			std::string kind;
			std::string label;
			std::string detail;
			std::string knowledgeState;
			std::string ruleId;
			std::string entityId;
			std::string instanceId;
			std::string attributeId;
			std::string relationshipId;
			std::vector<EvaluationTraceNode> children;
		};

		struct InferredInstanceChange
		{
			std::string instanceId;
			std::string entityId;
			std::string sourceInstanceId;
			std::string drivingInstanceId;
			std::string relationshipId;
			std::string createdByRuleId;
		};

		struct InferredLinkChange
		{
			std::string relationshipId;
			std::string sourceInstanceId;
			std::string targetInstanceId;
			std::string createdByRuleId;
		};

		struct ValidationEventChange
		{
			std::string eventId;
			std::string severity;
			std::string ruleId;
			std::string ruleName;
			std::string entityId;
			std::string instanceId;
			std::string message;
			bool messageKnown = true;
			std::string messageKnowledgeState;
		};

		int queuedAttributes = 0;
		int queuedRelationships = 0;
		int candidateRules = 0;
		int evaluatedRules = 0;
		int appliedConclusions = 0;
		int skippedRules = 0;
		int unsupportedRules = 0;
		int maxDepthStops = 0;
		int inferredInstancesCreated = 0;
		int inferredInstancesReused = 0;
		int inferredInstancesRemoved = 0;
		int inferredLinksCreated = 0;
		int inferredLinksReused = 0;
		int inferredLinksRemoved = 0;
		int validationEventsCreated = 0;
		int validationEventsReused = 0;
		int validationEventsUpdated = 0;
		int validationEventsCleared = 0;
		int validationErrors = 0;
		int validationWarnings = 0;
		bool captureTrace = false;
		std::vector<InferredInstanceChange> createdInferredInstances;
		std::vector<InferredInstanceChange> reusedInferredInstances;
		std::vector<InferredInstanceChange> removedInferredInstances;
		std::vector<InferredLinkChange> createdInferredLinks;
		std::vector<InferredLinkChange> reusedInferredLinks;
		std::vector<InferredLinkChange> removedInferredLinks;
		std::vector<ValidationEventChange> createdValidationEvents;
		std::vector<ValidationEventChange> reusedValidationEvents;
		std::vector<ValidationEventChange> updatedValidationEvents;
		std::vector<ValidationEventChange> clearedValidationEvents;
		std::vector<EvaluationDiagnostic> diagnostics;
		std::vector<EvaluationTraceNode> trace;
	};

	EvaluationStats EvaluateChangedAttribute(RuleSet& ruleSet,
	                                         const std::string& entityId,
	                                         const std::string& instanceId,
	                                         const std::string& attributeId,
	                                         bool captureTrace = false) const;

	EvaluationStats EvaluateChangedAttributes(RuleSet& ruleSet,
	                                          const std::vector<ChangedAttribute>& attributes,
	                                          bool captureTrace = false) const;

	EvaluationStats EvaluateInitialRules(RuleSet& ruleSet) const;

	EvaluationStats EvaluateAllRules(RuleSet& ruleSet, bool captureTrace = false) const;

	EvaluationStats EvaluateChangedAttributeHistory(RuleSet& ruleSet,
	                                                const std::string& entityId,
	                                                const std::string& instanceId,
	                                                const std::string& attributeId,
	                                                const std::vector<std::pair<std::string, std::string>>& effectiveIntervals) const;

	EvaluationStats EvaluateChangedRelationshipHistory(RuleSet& ruleSet,
	                                                   const std::string& relationshipId,
	                                                   const std::string& sourceEntityId,
	                                                   const std::string& sourceInstanceId,
	                                                   const std::string& targetEntityId,
	                                                   const std::string& targetInstanceId,
	                                                   const std::vector<std::pair<std::string, std::string>>& effectiveIntervals) const;

	EvaluationStats EvaluateChangedEntityExistenceHistory(RuleSet& ruleSet,
	                                                      const std::string& entityId,
	                                                      const std::string& instanceId,
	                                                      const std::vector<std::pair<std::string, std::string>>& effectiveIntervals) const;

	EvaluationStats EvaluateChangedHistorySnapshot(RuleSet& ruleSet) const;

	EvaluationStats EvaluateChangedSessionContext(RuleSet& ruleSet, const std::vector<std::string>& functionNames) const;

	EvaluationStats EvaluateChangedRelationship(RuleSet& ruleSet, const std::string& entityId, const std::string& instanceId, const std::string& relationshipId) const;

	bool EvaluateCondition(const RuleSet& ruleSet,
	                       const std::string& entityId,
	                       const std::string& instanceId,
	                       const nlohmann::json& condition,
	                       EvaluationStats* stats = nullptr) const;

	bool EvaluateFlowCondition(RuleSet& ruleSet,
	                           const std::string& entityId,
	                           const std::string& instanceId,
	                           const nlohmann::json& condition,
	                           EvaluationStats* stats = nullptr) const;
};
