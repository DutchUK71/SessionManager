#include "SessionModelRuleEvaluator.h"

#include "Rules/Attribute.h"
#include "DataManager.h"
#include "Rules/Entity.h"
#include "Rules/Instance.h"
#include "SessionModelExpressionParserSupport.h"
#include "SessionModelTemporalDateTimeSupport.h"
#include "SessionModelTemporalFunctionCatalog.h"
#include "SessionModelTemporalHistorySupport.h"
#include "Util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace
{
	enum class NodeResult
	{
		False,
		True,
		Unknown,
		Uncertain,
		Unsupported
	};

	std::string NodeResultToTraceText(NodeResult result)
	{
		switch(result)
		{
			case NodeResult::True:
				return "True";
			case NodeResult::False:
				return "False";
			case NodeResult::Unknown:
				return "Unknown";
			case NodeResult::Uncertain:
				return "Uncertain";
			case NodeResult::Unsupported:
				return "Unsupported";
			default:
				return "Not evaluated";
		}
	}

	std::string NodeResultToTraceKnowledgeState(NodeResult result)
	{
		switch(result)
		{
			case NodeResult::True:
			case NodeResult::False:
				return "Known";
			case NodeResult::Unknown:
			case NodeResult::Uncertain:
			case NodeResult::Unsupported:
				return "Unknown";
			default:
				return "NotEvaluated";
		}
	}

	struct EvaluationContext
	{
		std::string ruleId;
		const RuleSet* ruleSet = nullptr;
		SessionModelRuleEvaluator::EvaluationStats* stats = nullptr;
		std::vector<SessionModelRuleEvaluator::EvaluationDiagnostic>* diagnostics = nullptr;
		std::vector<SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode>* traceSink = nullptr;
		std::vector<std::shared_ptr<Instance>> scopeStack;
		bool useConcreteUncertainValues = false;
		bool resolveFlowDerivedBooleans = false;
		std::unordered_map<const Attribute*, bool> flowDerivedBooleanValues;
		std::unordered_set<const Attribute*> unresolvedFlowDerivedBooleans;
		std::unordered_set<const Attribute*> resolvingFlowDerivedBooleans;
	};

	using EvaluationTraceNode = SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode;

	struct TraceSinkCapture
	{
		explicit TraceSinkCapture(EvaluationContext& context)
			: evaluationContext(context), parentSink(context.traceSink), active(context.traceSink != nullptr)
		{
			if(active)
			{
				evaluationContext.traceSink = &nodes;
			}
		}

		TraceSinkCapture(const TraceSinkCapture&) = delete;
		TraceSinkCapture& operator=(const TraceSinkCapture&) = delete;

		~TraceSinkCapture()
		{
			Restore();
		}

		std::vector<EvaluationTraceNode> Release()
		{
			Restore();

			return std::move(nodes);
		}

	private:
		void Restore()
		{
			if(active)
			{
				evaluationContext.traceSink = parentSink;
				active = false;
			}
		}

		EvaluationContext& evaluationContext;
		std::vector<EvaluationTraceNode>* parentSink = nullptr;
		bool active = false;

	public:
		std::vector<EvaluationTraceNode> nodes;
	};

	struct AttributeChange
	{
		std::string entityId;
		std::string instanceId;
		std::string attributeId;
	};

	struct RelationshipChange
	{
		std::string entityId;
		std::string instanceId;
		std::string relationshipId;
		std::string sourceRuleId;
	};

	struct RuleTrigger
	{
		std::string entityId;
		std::string attributeId;
		std::string relationshipId;
	};

	struct ScopedValue
	{
		std::shared_ptr<Instance> sourceInstance;
		bool supported = true;
		bool known = false;
		std::string value;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		Attribute::AttributeType valueType = Attribute::UNKNOWN;
	};

	struct ResolvedValue
	{
		bool supported = false;
		bool known = false;
		std::string value;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		Attribute::AttributeType valueType = Attribute::UNKNOWN;
		bool isCollection = false;
		std::vector<ScopedValue> collectionValues;
		std::shared_ptr<Instance> sourceInstance;
		std::vector<ScopedValue> contributingItems;
		std::string traceRelationshipId;
		std::string traceDetailSuffix;
	};

	struct ScopedQuantifierTraceSummary
	{
		size_t relatedCount = 0;
		size_t evaluatedCount = 0;
		size_t matchedCount = 0;
	};

	SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode CreateTraceNode(
		const std::string& kind,
		const std::string& label,
		const std::string& detail,
		const std::string& knowledgeState,
		const RuleSet::SessionModelRuleMetadata& rule,
		const std::shared_ptr<Instance>& target,
		const std::string& attributeId,
		const std::string& relationshipId)
	{
		SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode node;

		node.kind = kind;
		node.label = label;
		node.detail = detail;
		node.knowledgeState = knowledgeState;
		node.ruleId = rule.ruleId;
		node.entityId = target != nullptr ? target->entityId : rule.conclusionEntityId;
		node.instanceId = target != nullptr ? target->instanceId : std::string();
		node.attributeId = attributeId;
		node.relationshipId = relationshipId;

		return node;
	}

	std::string TraceKnowledgeState(Attribute::KnowledgeState knowledgeState)
	{
		return knowledgeState == Attribute::KnowledgeState::Unknown ? "Unknown" : "Known";
	}

	std::string ResolvedValueTraceKnowledgeState(const ResolvedValue& value)
	{
		return value.supported && value.known ? TraceKnowledgeState(value.knowledgeState) : "Unknown";
	}

	std::string ScopedValueTraceDetail(const ScopedValue& item)
	{
		if(!item.supported)
		{
			return "Value: Unsupported";
		}

		if(!item.known)
		{
			return "Value: Unknown; knowledge: " + Attribute::KnowledgeStateToString(item.knowledgeState);
		}

		return "Value: " + item.value + "; knowledge: " + Attribute::KnowledgeStateToString(item.knowledgeState);
	}

	std::string ScopedValueTraceKnowledgeState(const ScopedValue& item)
	{
		return item.supported && item.known ? TraceKnowledgeState(item.knowledgeState) : "Unknown";
	}

	EvaluationTraceNode CreateContributorTraceNode(const EvaluationContext& evaluationContext,
	                                               const std::shared_ptr<Instance>& context,
	                                               const ScopedValue& item,
	                                               size_t index,
	                                               const std::string& relationshipId)
	{
		EvaluationTraceNode node;
		const std::shared_ptr<Instance>& sourceInstance = item.sourceInstance;

		node.kind = "RelationshipScope";
		node.label = "Contributor " + std::to_string(index + 1);
		node.detail = ScopedValueTraceDetail(item);
		node.knowledgeState = ScopedValueTraceKnowledgeState(item);
		node.ruleId = evaluationContext.ruleId;
		node.entityId = sourceInstance != nullptr
			? sourceInstance->entityId
			: (context != nullptr ? context->entityId : std::string());
		node.instanceId = sourceInstance != nullptr ? sourceInstance->instanceId : std::string();
		node.relationshipId = relationshipId;

		return node;
	}

	EvaluationTraceNode CreateContributorGroupTraceNode(const EvaluationContext& evaluationContext,
	                                                    const std::shared_ptr<Instance>& context,
	                                                    const std::vector<ScopedValue>& contributingItems,
	                                                    const std::string& relationshipId)
	{
		EvaluationTraceNode group;

		group.kind = "RelationshipScope";
		group.label = "Contributors";
		group.detail = std::to_string(contributingItems.size()) + " item(s)";
		group.knowledgeState = contributingItems.empty() ? "NotEvaluated" : "Known";
		group.ruleId = evaluationContext.ruleId;
		group.entityId = context != nullptr ? context->entityId : std::string();
		group.instanceId = context != nullptr ? context->instanceId : std::string();
		group.relationshipId = relationshipId;
		group.children.reserve(contributingItems.size());

		for(size_t index = 0; index < contributingItems.size(); ++index)
		{
			group.children.push_back(CreateContributorTraceNode(evaluationContext,
			                                                    context,
			                                                    contributingItems[index],
			                                                    index,
			                                                    relationshipId));
		}

		return group;
	}

	std::string AttributeTraceLabel(const std::shared_ptr<Attribute>& attribute, const std::string& attributeId)
	{
		if(attribute == nullptr)
		{
			return attributeId.empty() ? "Attribute" : attributeId;
		}

		const std::string attributeText = attribute->GetAttributeText();
		if(!attributeText.empty())
		{
			return attributeText;
		}

		const std::string coreName = attribute->GetCoreName();
		if(!coreName.empty())
		{
			return coreName;
		}

		const std::string id = attribute->GetID();
		return id.empty() ? "Attribute" : id;
	}

	std::string AttributeTraceDetail(const std::shared_ptr<Attribute>& attribute)
	{
		if(attribute == nullptr)
		{
			return "Value: Unknown; reason: attribute not found";
		}

		if(attribute->IsUnknown())
		{
			return "Value: Unknown; knowledge: " + Attribute::KnowledgeStateToString(attribute->GetKnowledgeState());
		}

		return "Value: " + attribute->GetStringValue() + "; knowledge: " + Attribute::KnowledgeStateToString(attribute->GetKnowledgeState());
	}

	void AppendAttributeReadTrace(EvaluationContext& evaluationContext,
	                              const std::shared_ptr<Instance>& context,
	                              const std::string& entityId,
	                              const std::string& instanceId,
	                              const std::string& attributeId,
	                              const std::string& relationshipId,
	                              const std::shared_ptr<Attribute>& attribute)
	{
		if(evaluationContext.traceSink == nullptr)
		{
			return;
		}

		SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode node;
		std::shared_ptr<Instance> sourceInstance = attribute != nullptr ? attribute->instance : nullptr;

		node.kind = "AttributeRead";
		node.label = AttributeTraceLabel(attribute, attributeId);
		node.detail = AttributeTraceDetail(attribute);
		node.knowledgeState = attribute != nullptr ? TraceKnowledgeState(attribute->GetKnowledgeState()) : "Unknown";
		node.ruleId = evaluationContext.ruleId;
		node.entityId = sourceInstance != nullptr ? sourceInstance->entityId : (!entityId.empty() ? entityId : (context != nullptr ? context->entityId : std::string()));
		node.instanceId = sourceInstance != nullptr ? sourceInstance->instanceId : (!instanceId.empty() ? instanceId : (context != nullptr ? context->instanceId : std::string()));
		node.attributeId = attribute != nullptr ? attribute->GetID() : attributeId;
		node.relationshipId = relationshipId;

		evaluationContext.traceSink->push_back(node);
	}

	void AppendFunctionResultTrace(EvaluationContext& evaluationContext,
	                               const std::shared_ptr<Instance>& context,
	                               const std::string& functionName,
	                               const ResolvedValue& value,
	                               std::vector<EvaluationTraceNode> children = {})
	{
		if(evaluationContext.traceSink == nullptr || functionName.empty())
		{
			return;
		}

		std::string detail;
		if(!value.supported)
		{
			detail = "Result: Unsupported";
		}
		else if(!value.known)
		{
			detail = "Result: Unknown";
		}
		else if(value.isCollection)
		{
			detail = "Result: " + std::to_string(value.collectionValues.size()) + " item(s); knowledge: " + Attribute::KnowledgeStateToString(value.knowledgeState);
		}
		else
		{
			detail = "Result: " + value.value + "; knowledge: " + Attribute::KnowledgeStateToString(value.knowledgeState);
		}

		if(!value.traceDetailSuffix.empty())
		{
			detail += "; " + value.traceDetailSuffix;
		}

		if(!value.contributingItems.empty())
		{
			detail += "; contributors: " + std::to_string(value.contributingItems.size());
		}

		SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode node;

		node.kind = "Function";
		node.label = functionName;
		node.detail = detail;
		node.knowledgeState = ResolvedValueTraceKnowledgeState(value);
		node.ruleId = evaluationContext.ruleId;
		node.entityId = context != nullptr ? context->entityId : std::string();
		node.instanceId = context != nullptr ? context->instanceId : std::string();
		node.relationshipId = value.traceRelationshipId;
		node.children = std::move(children);

		if(!value.contributingItems.empty())
		{
			node.children.push_back(CreateContributorGroupTraceNode(evaluationContext,
			                                                        context,
			                                                        value.contributingItems,
			                                                        value.traceRelationshipId));
		}

		evaluationContext.traceSink->push_back(node);
	}

	struct ExpressionResolution
	{
		bool isExpression = false;
		ResolvedValue value;
	};

	struct ResolvedInstanceReference
	{
		bool supported = true;
		bool known = false;
		std::string instanceId;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Unknown;
	};

	struct ResolvedRelationshipScope
	{
		bool supported = true;
		bool known = false;
		std::string relationshipId;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Unknown;
	};

	struct TemporalAttributeReference
	{
		bool resolved = false;
		std::string entityId;
		std::string instanceId;
		std::string attributeId;
		Attribute::AttributeType valueType = Attribute::UNKNOWN;
	};

	using SessionModelExpressionParserSupport::BuildCallFromNode;
	using SessionModelExpressionParserSupport::FirstNonEmpty;
	using SessionModelExpressionParserSupport::FunctionArgumentValue;
	using SessionModelExpressionParserSupport::GetString;
	using SessionModelExpressionParserSupport::IsQuotedString;
	using SessionModelExpressionParserSupport::NormalizeLookup;
	using SessionModelExpressionParserSupport::ParsedFunctionCall;
	using SessionModelExpressionParserSupport::ParseFunctionCall;
	using SessionModelExpressionParserSupport::RemoveMatchingQuotes;
	using SessionModelExpressionParserSupport::ToLowerAscii;
	using SessionModelExpressionParserSupport::Trim;
	using SessionModelExpressionParserSupport::TryGetString;
	using SessionModelExpressionParserSupport::ValueInLiteralList;

	using SessionModelTemporalDateTimeSupport::AddMonthsToDate;
	using SessionModelTemporalDateTimeSupport::CivilFromDays;
	using SessionModelTemporalDateTimeSupport::CompareDateTimeParts;
	using SessionModelTemporalDateTimeSupport::DateTimeParts;
	using SessionModelTemporalDateTimeSupport::DayOfWeekMondayZero;
	using SessionModelTemporalDateTimeSupport::DaysFromCivil;
	using SessionModelTemporalDateTimeSupport::DaysInMonth;
	using SessionModelTemporalDateTimeSupport::FormatDate;
	using SessionModelTemporalDateTimeSupport::FormatDateTime;
	using SessionModelTemporalDateTimeSupport::FormatTime;
	using SessionModelTemporalDateTimeSupport::IsValidDate;
	using SessionModelTemporalDateTimeSupport::PartsFromSeconds;
	using SessionModelTemporalDateTimeSupport::SecondsFromParts;
	using SessionModelTemporalDateTimeSupport::TryParseDateTimeParts;
	using SessionModelTemporalDateTimeSupport::WholeMonthDifference;
	using SessionModelTemporalDateTimeSupport::WholeYearDifference;

	using SessionModelTemporalHistorySupport::AnyHistoryRangeOverlapsInterval;
	using SessionModelTemporalHistorySupport::AnyHistoryRangeOverlapsPoint;
	using SessionModelTemporalHistorySupport::BuildTemporalEffectiveWindow;
	using SessionModelTemporalHistorySupport::ClassifyTemporalDay;
	using SessionModelTemporalHistorySupport::HasCompleteCoverage;
	using SessionModelTemporalHistorySupport::LocalDayStartSeconds;
	using SessionModelTemporalHistorySupport::RawTemporalIntervalsOverlap;
	using SessionModelTemporalHistorySupport::TemporalCoverageCoversSlice;
	using SessionModelTemporalHistorySupport::TemporalCoverageSegment;
	using SessionModelTemporalHistorySupport::TemporalCoverageSegmentOverlapsRange;
	using SessionModelTemporalHistorySupport::TemporalCoverageSegmentsOverlap;
	using SessionModelTemporalHistorySupport::TemporalCoverageSummary;
	using SessionModelTemporalHistorySupport::TemporalDayFraction;
	using SessionModelTemporalHistorySupport::TemporalDayState;
	using SessionModelTemporalHistorySupport::TemporalEffectiveWindow;
	using SessionModelTemporalHistorySupport::TemporalEffectiveWindowOverlapsRanges;
	using SessionModelTemporalHistorySupport::TemporalHistoryChangeRange;
	using SessionModelTemporalHistorySupport::TemporalInstant;

	struct TemporalExtremaObservedSegment
	{
		long long startSeconds = 0;
		long long endSeconds = 0;
		std::string normalizedValue;
		double numericValue = 0.0;
		long long temporalValue = 0;
	};

	struct TemporalWeightedObservedSegment
	{
		long long startSeconds = 0;
		long long endSeconds = 0;
		std::string normalizedValue;
		double numericValue = 0.0;
	};

	struct TemporalCountDistinctSliceState
	{
		bool valueCovered = false;
		bool valueUnknown = false;
		bool valueUncertain = false;
		bool hasCertainValue = false;
		std::string certainKey;
	};

	struct TemporalConditionSliceState
	{
		bool covered = false;
		bool certainTrue = false;
		bool certainFalse = false;
		bool unknown = false;
		bool uncertain = false;
	};

	struct TemporalIntervalCallShape
	{
		size_t conditionIndex = 0;
		size_t startIndex = 1;
		size_t endIndex = 2;
		size_t daysIndex = 0;
		bool hasDays = false;
	};

	enum class TemporalOncePerMonthCallShape
	{
		AnchorCurrentDate,
		AnchorConditionHistory,
		OverloadedThreeArgument,
		AnchorRangeConditionHistory,
		UnsupportedArity
	};

	ResolvedValue EvaluateExpressionCall(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const ParsedFunctionCall& call,
	                                     EvaluationContext& evaluationContext);

	ResolvedValue EvaluateExpressionCallWithTrace(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const ParsedFunctionCall& call,
	                                              EvaluationContext& evaluationContext);

	ResolvedValue ResolveExpressionArgument(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& context,
	                                        const std::string& argument,
	                                        EvaluationContext& evaluationContext);

	ResolvedValue EvaluateCollectionExpressionCall(const RuleSet& ruleSet,
	                                               const std::shared_ptr<Instance>& context,
	                                               const ParsedFunctionCall& call,
	                                               EvaluationContext& evaluationContext);

	ResolvedValue EvaluateCollectionExpressionCallWithTrace(const RuleSet& ruleSet,
	                                                        const std::shared_ptr<Instance>& context,
	                                                        const ParsedFunctionCall& call,
	                                                        EvaluationContext& evaluationContext);

	ResolvedValue EvaluateForScopeAtExpression(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const ParsedFunctionCall& call,
	                                           EvaluationContext& evaluationContext);

	ResolvedValue EvaluateForScopeAtCollectionExpression(const RuleSet& ruleSet,
	                                                     const std::shared_ptr<Instance>& context,
	                                                     const ParsedFunctionCall& call,
	                                                     EvaluationContext& evaluationContext);

	ResolvedValue EvaluateTemporalScopedQuantifierExpression(const RuleSet& ruleSet,
	                                                         const std::shared_ptr<Instance>& context,
	                                                         const ParsedFunctionCall& call,
	                                                         EvaluationContext& evaluationContext);

	ResolvedValue EvaluateFunctionNodeValue(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& context,
	                                        const json& node,
	                                        EvaluationContext& evaluationContext);

	ResolvedValue EvaluateFunctionNodeWithTrace(const RuleSet& ruleSet,
	                                            const std::shared_ptr<Instance>& context,
	                                            const json& node,
	                                            EvaluationContext& evaluationContext);

	ResolvedValue EvaluateCollectionNode(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const json& argument,
	                                     EvaluationContext& evaluationContext);

	ResolvedValue EvaluateCollectionNodeWithTrace(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const json& argument,
	                                              EvaluationContext& evaluationContext);

	ResolvedValue EvaluateForScopeAtNode(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const json& arguments,
	                                     EvaluationContext& evaluationContext);

	ResolvedValue EvaluateForScopeAtCollectionNode(const RuleSet& ruleSet,
	                                               const std::shared_ptr<Instance>& context,
	                                               const json& arguments,
	                                               EvaluationContext& evaluationContext);

	ResolvedValue EvaluateTemporalScopedQuantifierNode(const RuleSet& ruleSet,
	                                                   const std::shared_ptr<Instance>& context,
	                                                   const std::string& functionName,
	                                                   const json& arguments,
	                                                   EvaluationContext& evaluationContext);

	ResolvedValue EvaluateIsMemberOfAtExpression(const RuleSet& ruleSet,
	                                             const std::shared_ptr<Instance>& context,
	                                             const ParsedFunctionCall& call,
	                                             EvaluationContext& evaluationContext);
	ResolvedValue EvaluateIsMemberOfAtNode(const RuleSet& ruleSet,
	                                       const std::shared_ptr<Instance>& context,
	                                       const json& arguments,
	                                       EvaluationContext& evaluationContext);

	ResolvedValue EvaluateCountDistinctCollection(const ResolvedValue& collection,
	                                              EvaluationContext& evaluationContext);

	ResolvedValue ResolveFunctionArgumentNode(const RuleSet& ruleSet,
	                                          const std::shared_ptr<Instance>& context,
	                                          const json& argument,
	                                          EvaluationContext& evaluationContext);

	ResolvedValue ResolveScopedConditionArgumentNode(const RuleSet& ruleSet,
	                                                 const std::shared_ptr<Instance>& context,
	                                                 const json& argument,
	                                                 EvaluationContext& evaluationContext);

	std::string ResolveInstanceNodeToken(const std::shared_ptr<Instance>& context,
	                                     const json& argument,
	                                     size_t argumentIndex,
	                                     const EvaluationContext& evaluationContext);

	NodeResult EvaluateNode(const RuleSet& ruleSet,
	                        const std::shared_ptr<Instance>& context,
	                        const json& node,
	                        EvaluationContext& evaluationContext);

	ResolvedValue BooleanValue(bool value, Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain);
	ResolvedValue UnknownValue(Attribute::AttributeType valueType);
	bool TryParseBoolean(const std::string& value, bool& result);
	bool TryParseDoubleValue(const std::string& value, double& result);
	std::string FormatDoubleValue(double value);
	bool TryBuildTemporalCoverageSegment(const RuleSet::AttributeHistorySegment& segment,
	                                     const TemporalAttributeReference& reference,
	                                     const TemporalInstant& intervalStart,
	                                     const TemporalInstant& intervalEnd,
	                                     TemporalCoverageSegment& coverage,
	                                     EvaluationContext& evaluationContext);

	ResolvedValue EvaluateExpressionCallWithTrace(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const ParsedFunctionCall& call,
	                                              EvaluationContext& evaluationContext)
	{
		TraceSinkCapture traceCapture(evaluationContext);
		ResolvedValue value = EvaluateExpressionCall(ruleSet, context, call, evaluationContext);
		std::vector<EvaluationTraceNode> children = traceCapture.Release();

		AppendFunctionResultTrace(evaluationContext, context, call.name, value, std::move(children));

		return value;
	}

	ResolvedValue EvaluateFunctionNodeWithTrace(const RuleSet& ruleSet,
	                                            const std::shared_ptr<Instance>& context,
	                                            const json& node,
	                                            EvaluationContext& evaluationContext)
	{
		const std::string functionName = GetString(node, "functionName");
		TraceSinkCapture traceCapture(evaluationContext);
		ResolvedValue value = EvaluateFunctionNodeValue(ruleSet, context, node, evaluationContext);
		std::vector<EvaluationTraceNode> children = traceCapture.Release();

		AppendFunctionResultTrace(evaluationContext, context, functionName, value, std::move(children));

		return value;
	}

	std::string QueueKey(const AttributeChange& change)
	{
		return change.entityId + "\x1f" + change.instanceId + "\x1f" + change.attributeId;
	}

	std::string QueueKey(const RelationshipChange& change)
	{
		return change.entityId + "\x1f" + change.instanceId + "\x1f" + change.relationshipId;
	}

	bool MarkUniqueRuleId(std::unordered_set<std::string>& seenRuleIds, const std::string& ruleId)
	{
		return seenRuleIds.insert(ruleId).second;
	}

	bool StartsWith(const std::string& value, const std::string& prefix)
	{
		return value.rfind(prefix, 0) == 0;
	}

	bool IsSessionContextFunctionName(const std::string& functionName)
	{
		return functionName == "CurrentDate"
			|| functionName == "CurrentDateTime"
			|| functionName == "CurrentTime"
			|| functionName == "CurrentLocale"
			|| functionName == "DeploymentName"
			|| functionName == "DeploymentVersion";
	}

	bool IsReactiveReferenceSource(const std::string& source)
	{
		return StartsWith(source, "condition")
			|| StartsWith(source, "decisionTable.conditionCells")
			|| StartsWith(source, "decisionTable.conclusionCells.")
			|| StartsWith(source, "rule.conclusionExpression")
			|| StartsWith(source, "rule.conclusionValue");
	}

	bool RuleHasReactiveReference(const RuleSet::SessionModelRuleMetadata& rule)
	{
		for(const RuleSet::SessionModelRuleReference& reference : rule.references)
		{
			const bool sourceIsReactive = IsReactiveReferenceSource(reference.source);

			if(sourceIsReactive && !reference.entityId.empty() && !reference.attributeId.empty())
			{
				return true;
			}

			if((sourceIsReactive || (rule.conclusionMode == "Relationship" && reference.source == "rule.relationshipId"))
				&& !reference.relationshipId.empty())
			{
				return true;
			}

			if((sourceIsReactive || StartsWith(reference.source, "decisionTable.conclusionCells"))
				&& IsSessionContextFunctionName(reference.functionName))
			{
				return true;
			}
		}

		return false;
	}

	void AddDiagnostic(EvaluationContext& context, const std::string& code, const std::string& message, const std::string& detail)
	{
		if(context.diagnostics == nullptr)
		{
			return;
		}

		context.diagnostics->push_back(SessionModelRuleEvaluator::EvaluationDiagnostic{
			context.ruleId,
			code,
			message,
			detail
		});
	}

	bool IsCertainNodeResult(NodeResult result)
	{
		return result == NodeResult::True || result == NodeResult::False;
	}

	bool NodeTruthValue(NodeResult result)
	{
		return result == NodeResult::True;
	}

	Attribute::KnowledgeState NodeKnowledgeState(NodeResult result)
	{
		if(result == NodeResult::Uncertain)
		{
			return Attribute::KnowledgeState::Uncertain;
		}

		if(IsCertainNodeResult(result))
		{
			return Attribute::KnowledgeState::Certain;
		}

		return Attribute::KnowledgeState::Unknown;
	}

	NodeResult BooleanNodeResult(bool value,
	                             Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain,
	                             bool useConcreteUncertainValue = false)
	{
		if(knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			return NodeResult::Unknown;
		}

		if(knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return useConcreteUncertainValue
				? (value ? NodeResult::True : NodeResult::False)
				: NodeResult::Uncertain;
		}

		return value ? NodeResult::True : NodeResult::False;
	}

	NodeResult AttributeBackedNodeResult(const std::shared_ptr<Attribute>& attribute,
	                                     bool value,
	                                     bool useConcreteUncertainValue = false)
	{
		if(attribute == nullptr || attribute->IsUnknown())
		{
			return NodeResult::Unknown;
		}

		return BooleanNodeResult(value, attribute->GetKnowledgeState(), useConcreteUncertainValue);
	}

	Attribute::KnowledgeState CombineKnowledgeStates(Attribute::KnowledgeState left, Attribute::KnowledgeState right)
	{
		if(left == Attribute::KnowledgeState::Certain && right == Attribute::KnowledgeState::Certain)
		{
			return Attribute::KnowledgeState::Certain;
		}

		if(left == Attribute::KnowledgeState::Unknown && right == Attribute::KnowledgeState::Unknown)
		{
			return Attribute::KnowledgeState::Unknown;
		}

		return Attribute::KnowledgeState::Uncertain;
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

	std::shared_ptr<Entity> FindEntity(const RuleSet& ruleSet, const std::string& entityId)
	{
		DataManager dataManager;

		return dataManager.GetEntityByID(ruleSet.entities, entityId);
	}

	std::shared_ptr<Instance> FindInstance(const RuleSet& ruleSet, const std::string& entityId, const std::string& instanceId)
	{
		std::shared_ptr<Entity> entity = FindEntity(ruleSet, entityId);

		if(entity == nullptr)
		{
			return nullptr;
		}

		return entity->GetInstanceById(instanceId);
	}

	std::shared_ptr<Instance> FindInstanceInAnyEntity(const RuleSet& ruleSet,
	                                                  const std::string& instanceId,
	                                                  std::string& entityId)
	{
		entityId.clear();

		if(ruleSet.entities == nullptr)
		{
			return nullptr;
		}

		for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			std::shared_ptr<Instance> instance = entity->GetInstanceById(instanceId);

			if(instance != nullptr)
			{
				entityId = entity->entityid;

				return instance;
			}
		}

		return nullptr;
	}

	const RuleSet::SessionModelRuleMetadata* FindRule(const RuleSet& ruleSet, const std::string& ruleId)
	{
		return ruleSet.GetSessionModelRuleById(ruleId);
	}

	std::shared_ptr<Relationship> FindRelationshipById(const RuleSet& ruleSet, const std::string& relationshipId)
	{
		return ruleSet.GetRelationshipDefinitionById(relationshipId);
	}

	std::shared_ptr<Relationship> FindRelationshipByScope(const RuleSet& ruleSet, const std::string& scope)
	{
		return ruleSet.GetRelationshipDefinitionByScope(scope);
	}

	std::string RelationshipScopeValue(const json& argument)
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

		return FunctionArgumentValue(argument);
	}

	std::string ResolveRelationshipScopeTraceId(const RuleSet& ruleSet, const std::string& scope)
	{
		std::shared_ptr<Relationship> relationship = FindRelationshipByScope(ruleSet, scope);

		return relationship != nullptr ? relationship->id : std::string();
	}

	std::string ResolveRelationshipScopeTraceId(const RuleSet& ruleSet, const json& argument)
	{
		return ResolveRelationshipScopeTraceId(ruleSet, RelationshipScopeValue(argument));
	}

	std::string FormatScopedQuantifierTraceDetail(const ScopedQuantifierTraceSummary& summary)
	{
		return "related: " + std::to_string(summary.relatedCount)
			+ "; evaluated: " + std::to_string(summary.evaluatedCount)
			+ "; matched: " + std::to_string(summary.matchedCount);
	}

	ResolvedValue WithRelationshipTrace(ResolvedValue value, const std::string& relationshipId)
	{
		value.traceRelationshipId = relationshipId;

		return value;
	}

	ResolvedValue WithScopedQuantifierTrace(ResolvedValue value,
	                                        const std::string& relationshipId,
	                                        const ScopedQuantifierTraceSummary& summary)
	{
		value.traceRelationshipId = relationshipId;
		value.traceDetailSuffix = FormatScopedQuantifierTraceDetail(summary);

		return value;
	}

	std::shared_ptr<Entity> FindEntityByIdentifierOrName(const RuleSet& ruleSet, const std::string& identifier)
	{
		if(ruleSet.entities == nullptr)
		{
			return nullptr;
		}

		const std::string normalizedIdentifier = NormalizeLookup(identifier);
		DataManager dataManager;
		std::shared_ptr<Entity> exactEntity = dataManager.GetEntityByID(ruleSet.entities, identifier);

		if(exactEntity != nullptr)
		{
			return exactEntity;
		}

		for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity != nullptr && (NormalizeLookup(entity->entityid) == normalizedIdentifier || NormalizeLookup(entity->description) == normalizedIdentifier))
			{
				return entity;
			}
		}

		return nullptr;
	}

	void PushUniqueInstance(std::vector<std::shared_ptr<Instance>>& instances, const std::shared_ptr<Instance>& instance)
	{
		if(instance == nullptr)
		{
			return;
		}

		if(std::find(instances.begin(), instances.end(), instance) == instances.end())
		{
			instances.push_back(instance);
		}
	}

	void PushUniqueString(std::vector<std::string>& values, const std::string& value)
	{
		if(value.empty())
		{
			return;
		}

		if(std::find(values.begin(), values.end(), value) == values.end())
		{
			values.push_back(value);
		}
	}

	template<typename Callback>
	void ForEachRelationshipLinkById(const std::shared_ptr<Instance>& instance,
	                                 const std::string& relationshipId,
	                                 Callback callback)
	{
		if(instance == nullptr || relationshipId.empty())
		{
			return;
		}

		const std::vector<size_t> indexes = instance->FindRelationshipLinkIndexesByRelationshipId(relationshipId);

		for(size_t index : indexes)
		{
			if(index >= instance->relationshipLinks.size())
			{
				continue;
			}

			const InstanceRelationshipLink& link = instance->relationshipLinks[index];

			if(link.relationshipId == relationshipId)
			{
				callback(link);
			}
		}
	}

	template<typename Predicate>
	bool HasRelationshipLinkById(const std::shared_ptr<Instance>& instance,
	                             const std::string& relationshipId,
	                             Predicate predicate)
	{
		if(instance == nullptr || relationshipId.empty())
		{
			return false;
		}

		const std::vector<size_t> indexes = instance->FindRelationshipLinkIndexesByRelationshipId(relationshipId);

		for(size_t index : indexes)
		{
			if(index >= instance->relationshipLinks.size())
			{
				continue;
			}

			const InstanceRelationshipLink& link = instance->relationshipLinks[index];

			if(link.relationshipId == relationshipId && predicate(link))
			{
				return true;
			}
		}

		return false;
	}

	bool HasAnyRelationshipLinkById(const std::shared_ptr<Instance>& instance, const std::string& relationshipId)
	{
		return HasRelationshipLinkById(instance, relationshipId, [](const InstanceRelationshipLink&) {
			return true;
		});
	}

	bool RelationshipLinkConnectsInstances(const InstanceRelationshipLink& link,
	                                      const std::shared_ptr<Instance>& left,
	                                      const std::shared_ptr<Instance>& right)
	{
		if(left == nullptr || right == nullptr)
		{
			return false;
		}

		const bool idsMatch =
			(link.sourceInstanceId == left->instanceId && link.targetInstanceId == right->instanceId)
			|| (link.sourceInstanceId == right->instanceId && link.targetInstanceId == left->instanceId);

		if(idsMatch)
		{
			return true;
		}

		if(auto linkedInstance = link.linkedInstance.lock())
		{
			return linkedInstance == left
				|| linkedInstance == right
				|| linkedInstance->instanceId == left->instanceId
				|| linkedInstance->instanceId == right->instanceId;
		}

		return false;
	}

	bool IsExplicitRelationshipScopeArgument(const json& argument)
	{
		if(!argument.is_object())
		{
			return false;
		}

		if(argument.contains("relationshipScope") && argument.at("relationshipScope").is_object())
		{
			return true;
		}

		return !FirstNonEmpty(argument, {
			"relationshipId",
			"valueRelationshipId",
			"sourceRelationshipId",
			"comparisonRelationshipId",
			"relationshipName"
		}).empty();
	}

	std::vector<std::shared_ptr<Instance>> ResolveRelatedInstances(const std::shared_ptr<Instance>& context,
	                                                               const std::string& entityId,
	                                                               const std::string& relationshipId = "")
	{
		std::vector<std::shared_ptr<Instance>> instances;

		if(context == nullptr)
		{
			return instances;
		}

		if(context->entityId == entityId)
		{
			PushUniqueInstance(instances, context);
		}

		if(!relationshipId.empty())
		{
			bool sawRelationshipMetadata = HasAnyRelationshipLinkById(context, relationshipId);

			if(!sawRelationshipMetadata)
			{
				for(const std::shared_ptr<Instance>& linkedInstance : context->linkedInstances)
				{
					if(linkedInstance != nullptr && linkedInstance->entityId == entityId && HasAnyRelationshipLinkById(linkedInstance, relationshipId))
					{
						sawRelationshipMetadata = true;

						break;
					}
				}
			}

			ForEachRelationshipLinkById(context, relationshipId, [&](const InstanceRelationshipLink& link) {
				if(auto linkedInstance = link.linkedInstance.lock())
				{
					if(linkedInstance->entityId == entityId)
					{
						PushUniqueInstance(instances, linkedInstance);
					}
				}
			});

			for(const std::shared_ptr<Instance>& linkedInstance : context->linkedInstances)
			{
				if(linkedInstance == nullptr || linkedInstance->entityId != entityId)
				{
					continue;
				}

				bool reciprocalMatch = false;

				ForEachRelationshipLinkById(linkedInstance, relationshipId, [&](const InstanceRelationshipLink& link) {
					if(RelationshipLinkConnectsInstances(link, context, linkedInstance))
					{
						reciprocalMatch = true;
					}
				});

				if(reciprocalMatch)
				{
					PushUniqueInstance(instances, linkedInstance);
				}
			}

			return instances;
		}

		for(const InstanceRelationshipLink& link : context->relationshipLinks)
		{
			if(auto linkedInstance = link.linkedInstance.lock())
			{
				if(linkedInstance->entityId == entityId)
				{
					PushUniqueInstance(instances, linkedInstance);
				}
			}
		}

		for(const std::shared_ptr<Instance>& linkedInstance : context->linkedInstances)
		{
			if(linkedInstance != nullptr && linkedInstance->entityId == entityId)
			{
				PushUniqueInstance(instances, linkedInstance);
			}
		}

		return instances;
	}

	std::vector<std::string> ResolveTriggerRelationshipIds(const RuleSet::SessionModelRuleMetadata& rule,
	                                                       const RuleTrigger& trigger)
	{
		std::vector<std::string> relationshipIds;

		if(!trigger.relationshipId.empty())
		{
			PushUniqueString(relationshipIds, trigger.relationshipId);

			return relationshipIds;
		}

		if(!trigger.entityId.empty() && !trigger.attributeId.empty())
		{
			for(const RuleSet::SessionModelRuleReference& reference : rule.references)
			{
				if(reference.entityId == trigger.entityId
					&& reference.attributeId == trigger.attributeId
					&& !reference.relationshipId.empty())
				{
					PushUniqueString(relationshipIds, reference.relationshipId);
				}
			}
		}

		if(!relationshipIds.empty())
		{
			return relationshipIds;
		}

		PushUniqueString(relationshipIds, rule.relationshipId);

		return relationshipIds;
	}

	std::vector<std::shared_ptr<Instance>> ResolveConclusionTargets(const RuleSet& ruleSet,
	                                                                const std::shared_ptr<Instance>& changedInstance,
	                                                                const RuleSet::SessionModelRuleMetadata& rule,
	                                                                const RuleTrigger& trigger)
	{
		std::vector<std::shared_ptr<Instance>> targets;

		if(changedInstance == nullptr || rule.conclusionEntityId.empty())
		{
			return targets;
		}

		const std::vector<std::string> relationshipIds = ResolveTriggerRelationshipIds(rule, trigger);

		if(!relationshipIds.empty())
		{
			for(const std::string& relationshipId : relationshipIds)
			{
				std::vector<std::shared_ptr<Instance>> scopedTargets = ResolveRelatedInstances(changedInstance, rule.conclusionEntityId, relationshipId);

				for(const std::shared_ptr<Instance>& target : scopedTargets)
				{
					PushUniqueInstance(targets, target);
				}
			}

			return targets;
		}

		return ResolveRelatedInstances(changedInstance, rule.conclusionEntityId);
	}

	std::shared_ptr<Attribute> ResolveAttribute(const std::shared_ptr<Instance>& context,
	                                            const std::string& entityId,
	                                            const std::string& attributeId,
	                                            const std::string& relationshipId = "")
	{
		if(context == nullptr || entityId.empty() || attributeId.empty())
		{
			return nullptr;
		}

		std::vector<std::shared_ptr<Instance>> instances = ResolveRelatedInstances(context, entityId, relationshipId);

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			std::shared_ptr<Attribute> attribute = instance != nullptr ? instance->GetAttributeByID(attributeId) : nullptr;

			if(attribute != nullptr)
			{
				return attribute;
			}
		}

		return nullptr;
	}

	std::shared_ptr<Attribute> FindAttributeByIdentifierOrName(const std::shared_ptr<Instance>& instance, const std::string& identifier)
	{
		if(instance == nullptr)
		{
			return nullptr;
		}

		const std::string normalizedIdentifier = NormalizeLookup(identifier);

		for(const std::shared_ptr<Attribute>& attribute : instance->attributes)
		{
			if(attribute == nullptr)
			{
				continue;
			}

			if(NormalizeLookup(attribute->GetID()) == normalizedIdentifier
				|| NormalizeLookup(attribute->GetAttributeText()) == normalizedIdentifier
				|| NormalizeLookup(attribute->GetCoreName()) == normalizedIdentifier)
			{
				return attribute;
			}
		}

		return nullptr;
	}

	std::vector<std::shared_ptr<Instance>> ResolveForScopeInstances(const RuleSet& ruleSet,
	                                                                const std::shared_ptr<Instance>& context,
	                                                                const std::string& scope,
	                                                                bool explicitRelationshipScope = false)
	{
		std::vector<std::shared_ptr<Instance>> instances;

		if(context == nullptr)
		{
			return instances;
		}

		const std::string normalizedScope = NormalizeLookup(scope);

		if(normalizedScope.empty() || normalizedScope == "scope-ref")
		{
			PushUniqueInstance(instances, context);

			return instances;
		}

		std::shared_ptr<Relationship> relationship = FindRelationshipByScope(ruleSet, scope);

		if(relationship != nullptr)
		{
			const std::string relatedEntityId = relationship->sourceEntity == context->entityId
				? relationship->targetEntity
				: relationship->sourceEntity;

			instances = ResolveRelatedInstances(context, relatedEntityId, relationship->id);

			if(explicitRelationshipScope || !instances.empty())
			{
				return instances;
			}
		}

		if(explicitRelationshipScope)
		{
			return instances;
		}

		std::shared_ptr<Entity> entity = FindEntityByIdentifierOrName(ruleSet, scope);

		if(entity == nullptr)
		{
			return instances;
		}

		return ResolveRelatedInstances(context, entity->entityid);
	}

	std::shared_ptr<Instance> ResolveRelationshipSourceContext(const std::shared_ptr<Instance>& context,
	                                                           const EvaluationContext& evaluationContext)
	{
		if(!evaluationContext.scopeStack.empty() && evaluationContext.scopeStack.back() != nullptr)
		{
			return evaluationContext.scopeStack.back();
		}

		return context;
	}

	ResolvedValue ResolveValue(const std::shared_ptr<Instance>& context,
	                           const json& node,
	                           const std::string& defaultEntityId,
	                           const std::string& valueMode,
	                           const std::string& literalValue,
	                           bool allowAttributeIdFallback,
	                           EvaluationContext& evaluationContext)
	{
		if(valueMode.empty() || valueMode == "Literal")
		{
			return ResolvedValue{true, true, literalValue};
		}

		if(valueMode == "Attribute")
		{
			std::string sourceAttributeId = FirstNonEmpty(node, {
				"valueAttributeId",
				"sourceAttributeId",
				"comparisonAttributeId",
				"attributeValueId"
			});

			if(sourceAttributeId.empty() && !literalValue.empty())
			{
				sourceAttributeId = literalValue;
			}

			if(sourceAttributeId.empty() && allowAttributeIdFallback)
			{
				sourceAttributeId = GetString(node, "attributeId");
			}

			std::string sourceEntityId = FirstNonEmpty(node, {
				"valueEntityId",
				"sourceEntityId",
				"comparisonEntityId"
			});

			if(sourceEntityId.empty())
			{
				sourceEntityId = defaultEntityId;
			}

			const std::string sourceRelationshipId = FirstNonEmpty(node, {
				"valueRelationshipId",
				"sourceRelationshipId",
				"comparisonRelationshipId"
			});

			std::shared_ptr<Attribute> sourceAttribute = ResolveAttribute(context, sourceEntityId, sourceAttributeId, sourceRelationshipId);
			AppendAttributeReadTrace(evaluationContext,
			                         context,
			                         sourceEntityId,
			                         "",
			                         sourceAttributeId,
			                         sourceRelationshipId,
			                         sourceAttribute);

			if(sourceAttribute == nullptr)
			{
				return ResolvedValue{true, false, ""};
			}

			if(sourceAttribute->IsUnknown())
			{
				return ResolvedValue{true, false, ""};
			}

			return ResolvedValue{true, true, sourceAttribute->GetStringValue(), sourceAttribute->GetKnowledgeState()};
		}

		AddDiagnostic(evaluationContext, "unsupported_value_mode", "Value mode is not implemented yet.", valueMode);

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateForExpression(const RuleSet& ruleSet,
	                                    const std::shared_ptr<Instance>& context,
	                                    const ParsedFunctionCall& call,
	                                    EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "For expression requires scope and attribute arguments.", "For");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string attributeName = RemoveMatchingQuotes(call.arguments[1]);
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, scope);

		if(attributeName.empty())
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "For expression requires a non-empty attribute argument.", "For");

			return ResolvedValue{false, false, ""};
		}

		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(instance, attributeName);

			if(attribute == nullptr)
			{
				continue;
			}

			AppendAttributeReadTrace(evaluationContext,
			                         instance,
			                         instance != nullptr ? instance->entityId : std::string(),
			                         instance != nullptr ? instance->instanceId : std::string(),
			                         attribute->GetID(),
			                         relationshipId,
			                         attribute);

			if(attribute->IsUnknown())
			{
				return WithRelationshipTrace(ResolvedValue{true, false, ""}, relationshipId);
			}

			return WithRelationshipTrace(ResolvedValue{true, true, attribute->GetStringValue(), attribute->GetKnowledgeState()}, relationshipId);
		}

		return WithRelationshipTrace(ResolvedValue{true, false, ""}, relationshipId);
	}

	ResolvedValue EvaluateForScopeExpression(const RuleSet& ruleSet,
	                                         const std::shared_ptr<Instance>& context,
	                                         const ParsedFunctionCall& call,
	                                         EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() < 2 || call.arguments.size() > 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "ForScope expression requires scope, optional alias, and expression arguments.", "ForScope");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, scope);
		const size_t expressionIndex = call.arguments.size() >= 3 ? 2 : 1;
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);

		if(instances.empty())
		{
			return WithRelationshipTrace(ResolvedValue{true, false, ""}, relationshipId);
		}

		if(instances.size() > 1)
		{
			AddDiagnostic(evaluationContext, "for_scope_cardinality", "ForScope produced multiple related instances for a scalar expression.", scope);

			return WithRelationshipTrace(ResolvedValue{false, false, ""}, relationshipId);
		}

		evaluationContext.scopeStack.push_back(context);

		ResolvedValue value = ResolveExpressionArgument(ruleSet, instances.front(), call.arguments[expressionIndex], evaluationContext);

		evaluationContext.scopeStack.pop_back();

		return WithRelationshipTrace(value, relationshipId);
	}

	ResolvedValue EvaluateForScopeCollectionExpression(const RuleSet& ruleSet,
	                                                   const std::shared_ptr<Instance>& context,
	                                                   const ParsedFunctionCall& call,
	                                                   EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() < 2 || call.arguments.size() > 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "ForScope collection expression requires scope, optional alias, and expression arguments.", "ForScope");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, scope);
		const size_t expressionIndex = call.arguments.size() >= 3 ? 2 : 1;
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);
		ResolvedValue collection;

		collection.supported = true;
		collection.known = true;
		collection.knowledgeState = Attribute::KnowledgeState::Certain;
		collection.isCollection = true;
		collection.collectionValues.reserve(instances.size());

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveExpressionArgument(ruleSet, instance, call.arguments[expressionIndex], evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(collection.valueType == Attribute::UNKNOWN && value.valueType != Attribute::UNKNOWN)
			{
				collection.valueType = value.valueType;
			}

			collection.collectionValues.push_back(ScopedValue{
				instance,
				value.supported,
				value.known,
				value.value,
				value.knowledgeState,
				value.valueType
			});
		}

		collection.traceRelationshipId = relationshipId;

		return collection;
	}

	bool IsCollectionExpressionName(const std::string& functionName)
	{
		return functionName == "ForScope" || functionName == "ForScopeAt" || functionName == "Filter";
	}

	ResolvedValue FilterCollectionByExpression(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const ResolvedValue& collection,
	                                           const std::string& conditionExpression,
	                                           EvaluationContext& evaluationContext)
	{
		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.isCollection)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Filter requires a collection argument.", "Filter");

			return ResolvedValue{false, false, ""};
		}

		if(!collection.known)
		{
			return collection;
		}

		ResolvedValue filtered;

		filtered.supported = true;
		filtered.known = true;
		filtered.knowledgeState = collection.knowledgeState;
		filtered.valueType = collection.valueType;
		filtered.isCollection = true;
		filtered.traceRelationshipId = collection.traceRelationshipId;
		filtered.collectionValues.reserve(collection.collectionValues.size());

		bool sawUnknown = false;
		bool sawUncertain = collection.knowledgeState == Attribute::KnowledgeState::Uncertain;

		for(const ScopedValue& item : collection.collectionValues)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue predicate = ResolveExpressionArgument(ruleSet, item.sourceInstance, conditionExpression, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!predicate.supported)
			{
				return predicate;
			}

			if(!predicate.known)
			{
				sawUnknown = true;

				continue;
			}

			bool include = false;

			if(!TryParseBoolean(predicate.value, include))
			{
				AddDiagnostic(evaluationContext, "filter_condition_type", "Filter condition must evaluate to a boolean value.", conditionExpression);

				return ResolvedValue{false, false, ""};
			}

			if(predicate.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			if(include)
			{
				filtered.collectionValues.push_back(item);
			}
		}

		if(sawUnknown)
		{
			filtered.known = false;
			filtered.knowledgeState = Attribute::KnowledgeState::Unknown;
			filtered.collectionValues.clear();

			return filtered;
		}

		if(sawUncertain)
		{
			filtered.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}

		return filtered;
	}

	ResolvedValue EvaluateFilterCollectionExpression(const RuleSet& ruleSet,
	                                                 const std::shared_ptr<Instance>& context,
	                                                 const ParsedFunctionCall& call,
	                                                 EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Filter expression requires collection and condition arguments.", "Filter");

			return ResolvedValue{false, false, ""};
		}

		ParsedFunctionCall collectionCall = ParseFunctionCall(call.arguments[0]);

		if(!collectionCall.parsed || !collectionCall.valid || !IsCollectionExpressionName(collectionCall.name))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Filter currently supports ForScope, ForScopeAt, or Filter collection arguments.", call.arguments[0]);

			return ResolvedValue{false, false, ""};
		}

		return FilterCollectionByExpression(ruleSet,
		                                    context,
		                                    EvaluateCollectionExpressionCallWithTrace(ruleSet, context, collectionCall, evaluationContext),
		                                    call.arguments[1],
		                                    evaluationContext);
	}

	ResolvedValue EvaluateCollectionExpressionCall(const RuleSet& ruleSet,
	                                               const std::shared_ptr<Instance>& context,
	                                               const ParsedFunctionCall& call,
	                                               EvaluationContext& evaluationContext)
	{
		if(call.name == "ForScope")
		{
			return EvaluateForScopeCollectionExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "ForScopeAt")
		{
			return EvaluateForScopeAtCollectionExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Filter")
		{
			return EvaluateFilterCollectionExpression(ruleSet, context, call, evaluationContext);
		}

		AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Collection consumers currently support only ForScope, ForScopeAt, or Filter collection arguments.", call.name);

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateCollectionExpressionCallWithTrace(const RuleSet& ruleSet,
	                                                        const std::shared_ptr<Instance>& context,
	                                                        const ParsedFunctionCall& call,
	                                                        EvaluationContext& evaluationContext)
	{
		TraceSinkCapture traceCapture(evaluationContext);
		ResolvedValue value = EvaluateCollectionExpressionCall(ruleSet, context, call, evaluationContext);
		std::vector<EvaluationTraceNode> children = traceCapture.Release();

		AppendFunctionResultTrace(evaluationContext, context, call.name, value, std::move(children));

		return value;
	}

	ResolvedValue EvaluateCountExpression(const RuleSet& ruleSet,
	                                      const std::shared_ptr<Instance>& context,
	                                      const ParsedFunctionCall& call,
	                                      EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Count expression requires one collection argument.", "Count");

			return ResolvedValue{false, false, ""};
		}

		ParsedFunctionCall collectionCall = ParseFunctionCall(call.arguments[0]);

		if(!collectionCall.parsed || !collectionCall.valid || !IsCollectionExpressionName(collectionCall.name))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Count currently supports only ForScope, ForScopeAt, or Filter collection arguments.", call.arguments[0]);

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue collection = EvaluateCollectionExpressionCallWithTrace(ruleSet, context, collectionCall, evaluationContext);

		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.known)
		{
			ResolvedValue result = UnknownValue(Attribute::DOUBLE);

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		if(collection.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			ResolvedValue result{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		ResolvedValue result{true, true, std::to_string(collection.collectionValues.size()), collection.knowledgeState, Attribute::DOUBLE};

		result.traceRelationshipId = collection.traceRelationshipId;
		result.contributingItems = collection.collectionValues;

		return result;
	}

	ResolvedValue EvaluateCountDistinctExpression(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const ParsedFunctionCall& call,
	                                              EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "CountDistinct expression requires one collection argument.", "CountDistinct");

			return ResolvedValue{false, false, ""};
		}

		ParsedFunctionCall collectionCall = ParseFunctionCall(call.arguments[0]);

		if(!collectionCall.parsed || !collectionCall.valid || !IsCollectionExpressionName(collectionCall.name))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "CountDistinct currently supports only ForScope, ForScopeAt, or Filter collection arguments.", call.arguments[0]);

			return ResolvedValue{false, false, ""};
		}

		return EvaluateCountDistinctCollection(EvaluateCollectionExpressionCallWithTrace(ruleSet, context, collectionCall, evaluationContext),
		                                       evaluationContext);
	}

	bool IsCollectionAggregateName(const std::string& functionName)
	{
		return functionName == "Sum" || functionName == "Average" || functionName == "Maximum" || functionName == "Minimum";
	}

	bool IsNumericAttributeType(Attribute::AttributeType type)
	{
		return type == Attribute::INTEGER || type == Attribute::DOUBLE;
	}

	bool IsTemporalAttributeType(Attribute::AttributeType type)
	{
		return type == Attribute::DATE || type == Attribute::DATETIME || type == Attribute::TIMEOFDAY;
	}

	Attribute::AttributeType NormalizeAggregateItemType(Attribute::AttributeType type)
	{
		return IsNumericAttributeType(type) ? Attribute::DOUBLE : type;
	}

	Attribute::AttributeType NormalizeCountDistinctItemType(Attribute::AttributeType type)
	{
		return IsNumericAttributeType(type) ? Attribute::DOUBLE : type;
	}

	bool IsSupportedCollectionAggregateItemType(const std::string& functionName, Attribute::AttributeType type)
	{
		if(IsNumericAttributeType(type))
		{
			return true;
		}

		return functionName != "Sum" && functionName != "Average" && IsTemporalAttributeType(type);
	}

	bool AreCompatibleCollectionAggregateTypes(Attribute::AttributeType aggregateType, Attribute::AttributeType itemType)
	{
		if(IsNumericAttributeType(aggregateType) && IsNumericAttributeType(itemType))
		{
			return true;
		}

		return aggregateType == itemType;
	}

	bool IsSupportedCountDistinctItemType(Attribute::AttributeType type)
	{
		return IsNumericAttributeType(type)
			|| type == Attribute::STRING
			|| type == Attribute::BOOLEAN
			|| IsTemporalAttributeType(type);
	}

	bool AreCompatibleCountDistinctTypes(Attribute::AttributeType distinctType, Attribute::AttributeType itemType)
	{
		if(IsNumericAttributeType(distinctType) && IsNumericAttributeType(itemType))
		{
			return true;
		}

		return distinctType == itemType;
	}

	std::string CollectionAggregateTypeDiagnosticMessage(const std::string& functionName)
	{
		if(functionName == "Sum" || functionName == "Average")
		{
			return functionName + " collection aggregate requires numeric item values.";
		}

		return "Maximum and Minimum collection aggregates require numeric, date, datetime, or time-of-day item values.";
	}

	bool TryResolveAggregateComparableValue(const std::string& value,
	                                        Attribute::AttributeType valueType,
	                                        double& numericValue,
	                                        long long& temporalValue,
	                                        std::string& normalizedValue)
	{
		if(IsNumericAttributeType(valueType))
		{
			if(!TryParseDoubleValue(value, numericValue))
			{
				return false;
			}

			normalizedValue = FormatDoubleValue(numericValue);

			return true;
		}

		DateTimeParts parts;

		if(!TryParseDateTimeParts(value, parts))
		{
			return false;
		}

		if(valueType == Attribute::DATE)
		{
			if(!parts.hasDate)
			{
				return false;
			}

			temporalValue = DaysFromCivil(parts.year, parts.month, parts.day);
			normalizedValue = FormatDate(parts);

			return true;
		}

		if(valueType == Attribute::DATETIME)
		{
			if(!parts.hasDate || !parts.hasTime)
			{
				return false;
			}

			temporalValue = DaysFromCivil(parts.year, parts.month, parts.day) * 86400LL
				+ static_cast<long long>(parts.hour) * 3600LL
				+ static_cast<long long>(parts.minute) * 60LL
				+ static_cast<long long>(parts.second);
			normalizedValue = FormatDateTime(parts);

			return true;
		}

		if(valueType == Attribute::TIMEOFDAY)
		{
			if(!parts.hasTime)
			{
				return false;
			}

			temporalValue = static_cast<long long>(parts.hour) * 3600LL
				+ static_cast<long long>(parts.minute) * 60LL
				+ static_cast<long long>(parts.second);
			normalizedValue = FormatTime(parts);

			return true;
		}

		return false;
	}

	bool TryResolveCountDistinctKey(const ScopedValue& item, std::string& key)
	{
		if(IsNumericAttributeType(item.valueType) || IsTemporalAttributeType(item.valueType))
		{
			double numericValue = 0.0;
			long long temporalValue = 0;
			std::string normalizedValue;

			if(!TryResolveAggregateComparableValue(item.value, item.valueType, numericValue, temporalValue, normalizedValue))
			{
				return false;
			}

			key = std::to_string(static_cast<int>(NormalizeCountDistinctItemType(item.valueType))) + ":" + normalizedValue;

			return true;
		}

		if(item.valueType == Attribute::BOOLEAN)
		{
			bool boolValue = false;

			if(!TryParseBoolean(item.value, boolValue))
			{
				return false;
			}

			key = std::to_string(static_cast<int>(Attribute::BOOLEAN)) + ":" + (boolValue ? "true" : "false");

			return true;
		}

		if(item.valueType == Attribute::STRING)
		{
			key = std::to_string(static_cast<int>(Attribute::STRING)) + ":" + item.value;

			return true;
		}

		return false;
	}

	ResolvedValue EvaluateCountDistinctCollection(const ResolvedValue& collection,
	                                              EvaluationContext& evaluationContext)
	{
		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.known)
		{
			ResolvedValue result = UnknownValue(Attribute::DOUBLE);

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		if(collection.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			ResolvedValue result{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		bool sawUnknown = false;
		bool sawUncertain = false;
		std::unordered_set<std::string> distinctValues;
		std::vector<ScopedValue> distinctContributors;
		Attribute::AttributeType distinctType = Attribute::UNKNOWN;

		for(const ScopedValue& item : collection.collectionValues)
		{
			if(!item.supported)
			{
				AddDiagnostic(evaluationContext, "count_distinct_type_conversion", "CountDistinct cannot use an unsupported item value.", "CountDistinct");

				return ResolvedValue{false, false, ""};
			}

			if(!item.known)
			{
				sawUnknown = true;

				continue;
			}

			if(item.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			if(item.valueType == Attribute::UNKNOWN || !IsSupportedCountDistinctItemType(item.valueType))
			{
				AddDiagnostic(evaluationContext, "count_distinct_type_conversion", "CountDistinct requires scalar numeric, text, boolean, date, datetime, or time-of-day item values.", "CountDistinct:" + item.value);

				return ResolvedValue{false, false, ""};
			}

			if(distinctType == Attribute::UNKNOWN)
			{
				distinctType = NormalizeCountDistinctItemType(item.valueType);
			}
			else if(!AreCompatibleCountDistinctTypes(distinctType, item.valueType))
			{
				AddDiagnostic(evaluationContext, "count_distinct_type_conversion", "CountDistinct requires homogeneous item values.", "CountDistinct:" + item.value);

				return ResolvedValue{false, false, ""};
			}

			std::string key;

			if(!TryResolveCountDistinctKey(item, key))
			{
				AddDiagnostic(evaluationContext, "count_distinct_type_conversion", "CountDistinct cannot normalize an item value for equality.", "CountDistinct:" + item.value);

				return ResolvedValue{false, false, ""};
			}

			if(distinctValues.insert(key).second)
			{
				distinctContributors.push_back(item);
			}
		}

		if(sawUnknown)
		{
			ResolvedValue result = UnknownValue(Attribute::DOUBLE);

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		if(sawUncertain)
		{
			ResolvedValue result{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		ResolvedValue result{true, true, std::to_string(distinctValues.size()), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};

		result.traceRelationshipId = collection.traceRelationshipId;
		result.contributingItems = std::move(distinctContributors);

		return result;
	}

	ResolvedValue WithValueType(ResolvedValue value, Attribute::AttributeType valueType)
	{
		if(valueType != Attribute::UNKNOWN)
		{
			value.valueType = valueType;
		}

		return value;
	}

	ResolvedValue UnknownValue(Attribute::AttributeType valueType)
	{
		return ResolvedValue{true, false, "", Attribute::KnowledgeState::Unknown, valueType};
	}

	Attribute::AttributeType LiteralValueType(const std::string& value)
	{
		bool boolValue = false;
		double numberValue = 0.0;

		if(TryParseBoolean(value, boolValue))
		{
			return Attribute::BOOLEAN;
		}

		if(TryParseDoubleValue(value, numberValue))
		{
			return Attribute::DOUBLE;
		}

		return Attribute::STRING;
	}

	ResolvedValue EvaluateCollectionAggregate(const std::string& functionName,
	                                          const ResolvedValue& collection,
	                                          EvaluationContext& evaluationContext)
	{
		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.known)
		{
			ResolvedValue result = UnknownValue((functionName == "Sum" || functionName == "Average") ? Attribute::DOUBLE : collection.valueType);

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		ResolvedValue result;

		result.supported = true;
		result.known = true;
		result.knowledgeState = collection.knowledgeState;
		result.valueType = functionName == "Sum" || functionName == "Average" ? Attribute::DOUBLE : collection.valueType;
		result.traceRelationshipId = collection.traceRelationshipId;
		result.contributingItems = collection.collectionValues;

		if(collection.collectionValues.empty())
		{
			if(functionName == "Sum")
			{
				result.value = FormatDoubleValue(0.0);

				return result;
			}

			ResolvedValue unknownResult = UnknownValue(functionName == "Average" ? Attribute::DOUBLE : collection.valueType);

			unknownResult.traceRelationshipId = collection.traceRelationshipId;

			return unknownResult;
		}

		bool sawUnknown = false;
		bool sawUncertain = false;
		bool hasValue = false;
		size_t numericValueCount = 0;
		double numericAggregate = 0.0;
		long long temporalAggregate = 0;
		Attribute::AttributeType aggregateType = Attribute::UNKNOWN;
		std::string aggregateValue;

		for(const ScopedValue& item : collection.collectionValues)
		{
			if(!item.supported)
			{
				AddDiagnostic(evaluationContext, "aggregate_type_conversion", "Collection aggregate cannot use an unsupported item value.", functionName);

				return ResolvedValue{false, false, ""};
			}

			if(item.valueType == Attribute::UNKNOWN || !IsSupportedCollectionAggregateItemType(functionName, item.valueType))
			{
				AddDiagnostic(evaluationContext, "aggregate_type_conversion", CollectionAggregateTypeDiagnosticMessage(functionName), functionName + ":" + item.value);

				return ResolvedValue{false, false, ""};
			}

			if(aggregateType == Attribute::UNKNOWN)
			{
				aggregateType = NormalizeAggregateItemType(item.valueType);
				result.valueType = aggregateType;
			}
			else if(!AreCompatibleCollectionAggregateTypes(aggregateType, item.valueType))
			{
				AddDiagnostic(evaluationContext, "aggregate_type_conversion", "Collection aggregate requires homogeneous item values.", functionName + ":" + item.value);

				return ResolvedValue{false, false, ""};
			}

			if(!item.known)
			{
				sawUnknown = true;

				continue;
			}

			double numericValue = 0.0;
			long long temporalValue = 0;
			std::string normalizedValue;

			if(!TryResolveAggregateComparableValue(item.value, item.valueType, numericValue, temporalValue, normalizedValue))
			{
				AddDiagnostic(evaluationContext, "aggregate_type_conversion", CollectionAggregateTypeDiagnosticMessage(functionName), functionName + ":" + item.value);

				return ResolvedValue{false, false, ""};
			}

			if(item.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}

			if(functionName == "Sum" || functionName == "Average")
			{
				numericAggregate += numericValue;
				++numericValueCount;
				hasValue = true;
			}
			else
			{
				const bool numericAggregateType = IsNumericAttributeType(aggregateType);
				const bool better = !hasValue
					|| (functionName == "Maximum"
						? (numericAggregateType ? numericValue > numericAggregate : temporalValue > temporalAggregate)
						: (numericAggregateType ? numericValue < numericAggregate : temporalValue < temporalAggregate));

				if(better)
				{
					if(numericAggregateType)
					{
						numericAggregate = numericValue;
					}
					else
					{
						temporalAggregate = temporalValue;
					}

					aggregateValue = normalizedValue;
					hasValue = true;
					result.sourceInstance = item.sourceInstance;
				}
			}
		}

		if(collection.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			result.value = "";
			result.knowledgeState = Attribute::KnowledgeState::Uncertain;

			return result;
		}

		if(sawUnknown)
		{
			ResolvedValue unknownResult = UnknownValue(result.valueType);

			unknownResult.traceRelationshipId = collection.traceRelationshipId;
			unknownResult.contributingItems = collection.collectionValues;

			return unknownResult;
		}

		if(sawUncertain)
		{
			result.value = "";
			result.knowledgeState = Attribute::KnowledgeState::Uncertain;

			return result;
		}

		if(!hasValue)
		{
			if(functionName == "Sum")
			{
				result.value = FormatDoubleValue(0.0);
				result.valueType = Attribute::DOUBLE;
				result.knowledgeState = Attribute::KnowledgeState::Certain;

				return result;
			}

			return WithRelationshipTrace(UnknownValue(result.valueType), collection.traceRelationshipId);
		}

		if(functionName == "Sum")
		{
			result.value = FormatDoubleValue(numericAggregate);
		}
		else if(functionName == "Average")
		{
			result.value = FormatDoubleValue(numericAggregate / static_cast<double>(numericValueCount));
		}
		else
		{
			result.value = aggregateValue;
		}

		return result;
	}

	ResolvedValue EvaluateCollectionAggregateExpression(const RuleSet& ruleSet,
	                                                    const std::shared_ptr<Instance>& context,
	                                                    const ParsedFunctionCall& call,
	                                                    EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", call.name + " collection aggregate requires one collection argument.", call.name);

			return ResolvedValue{false, false, ""};
		}

		ParsedFunctionCall collectionCall = ParseFunctionCall(call.arguments[0]);

		if(!collectionCall.parsed || !collectionCall.valid || !IsCollectionExpressionName(collectionCall.name))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", call.name + " currently supports only ForScope, ForScopeAt, or Filter collection arguments.", call.arguments[0]);

			return ResolvedValue{false, false, ""};
		}

		return EvaluateCollectionAggregate(call.name,
		                                   EvaluateCollectionExpressionCallWithTrace(ruleSet, context, collectionCall, evaluationContext),
		                                   evaluationContext);
	}

	ResolvedValue EvaluateInstanceCountExpression(const RuleSet& ruleSet,
	                                             const std::shared_ptr<Instance>& context,
	                                             const ParsedFunctionCall& call,
	                                             EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceCount expression requires one relationship or entity scope argument.", "InstanceCount");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);

		return WithRelationshipTrace(
			ResolvedValue{true, true, std::to_string(instances.size()), Attribute::KnowledgeState::Certain, Attribute::DOUBLE},
			ResolveRelationshipScopeTraceId(ruleSet, scope));
	}

	bool TryParseBoolean(const std::string& value, bool& result)
	{
		const std::string normalized = NormalizeLookup(RemoveMatchingQuotes(value));

		if(normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "y")
		{
			result = true;

			return true;
		}

		if(normalized == "false" || normalized == "0" || normalized == "no" || normalized == "n")
		{
			result = false;

			return true;
		}

		return false;
	}

	ResolvedValue EvaluateNotExpression(const RuleSet& ruleSet,
	                                    const std::shared_ptr<Instance>& context,
	                                    const ParsedFunctionCall& call,
	                                    EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Not expression requires one argument.", "Not");

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue value = ResolveExpressionArgument(ruleSet, context, call.arguments[0], evaluationContext);

		if(!value.supported)
		{
			return value;
		}

		bool boolValue = false;

		if(!value.known)
		{
			return ResolvedValue{true, false, ""};
		}

		if(!TryParseBoolean(value.value, boolValue))
		{
			return BooleanValue(false, value.knowledgeState);
		}

		return BooleanValue(!boolValue, value.knowledgeState);
	}

	ResolvedValue EvaluateInstanceCountIfExpression(const RuleSet& ruleSet,
	                                                const std::shared_ptr<Instance>& context,
	                                                const ParsedFunctionCall& call,
	                                                EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceCountIf expression requires scope and condition arguments.", "InstanceCountIf");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string condition = call.arguments[1];
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);
		size_t matches = 0;

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveExpressionArgument(ruleSet, instance, condition, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!value.supported)
			{
				return value;
			}

			bool boolValue = false;

			if(value.known && TryParseBoolean(value.value, boolValue) && boolValue)
			{
				++matches;
			}
		}

		return WithRelationshipTrace(
			ResolvedValue{true, true, std::to_string(matches), Attribute::KnowledgeState::Certain, Attribute::DOUBLE},
			ResolveRelationshipScopeTraceId(ruleSet, scope));
	}

	ResolvedValue EvaluateInstanceValueIfExpression(const RuleSet& ruleSet,
	                                                const std::shared_ptr<Instance>& context,
	                                                const ParsedFunctionCall& call,
	                                                EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceValueIf expression requires scope, value, and condition arguments.", "InstanceValueIf");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string valueExpression = call.arguments[1];
		const std::string condition = call.arguments[2];
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);
		std::shared_ptr<Instance> matchedInstance;

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue conditionValue = ResolveExpressionArgument(ruleSet, instance, condition, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!conditionValue.supported)
			{
				return conditionValue;
			}

			if(!conditionValue.known || conditionValue.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain};
			}

			bool boolValue = false;

			if(!TryParseBoolean(conditionValue.value, boolValue) || !boolValue)
			{
				continue;
			}

			if(matchedInstance != nullptr)
			{
				return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain};
			}

			matchedInstance = instance;
		}

		if(matchedInstance == nullptr)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain};
		}

		evaluationContext.scopeStack.push_back(context);

		ResolvedValue matchedValue = ResolveExpressionArgument(ruleSet, matchedInstance, valueExpression, evaluationContext);

		evaluationContext.scopeStack.pop_back();

		return matchedValue;
	}

	bool TryParseDoubleValue(const std::string& value, double& result)
	{
		try
		{
			size_t parsedChars = 0;

			result = std::stod(value, &parsedChars);

			while(parsedChars < value.size())
			{
				if(!std::isspace(static_cast<unsigned char>(value[parsedChars])))
				{
					return false;
				}

				++parsedChars;
			}

			return true;
		}
		catch(...)
		{
			return false;
		}
	}

	std::string FormatDoubleValue(double value)
	{
		std::ostringstream stream;

		stream << value;

		return stream.str();
	}

	std::tm LocalNow()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t time = std::chrono::system_clock::to_time_t(now);
		std::tm localTime{};

#ifdef _WIN32
		localtime_s(&localTime, &time);
#else
		localtime_r(&time, &localTime);
#endif

		return localTime;
	}

	std::string FormatLocalTime(const char* format)
	{
		std::ostringstream stream;
		const std::tm localTime = LocalNow();

		stream << std::put_time(&localTime, format);

		return stream.str();
	}

	ResolvedValue EvaluateCurrentDateTimeExpression(const ParsedFunctionCall& call, EvaluationContext& evaluationContext)
	{
		if(!call.arguments.empty())
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Current date/time expressions do not accept arguments.", call.name);

			return ResolvedValue{false, false, ""};
		}

		if(call.name == "CurrentDate")
		{
			return ResolvedValue{true, true, evaluationContext.ruleSet != nullptr ? evaluationContext.ruleSet->GetSessionCurrentDate() : FormatLocalTime("%Y-%m-%d"), Attribute::KnowledgeState::Certain, Attribute::DATE};
		}

		if(call.name == "CurrentTime")
		{
			return ResolvedValue{true, true, evaluationContext.ruleSet != nullptr ? evaluationContext.ruleSet->GetSessionCurrentTime() : FormatLocalTime("%Y-%m-%dT%H:%M:%S"), Attribute::KnowledgeState::Certain, Attribute::TIMEOFDAY};
		}

		return ResolvedValue{true, true, evaluationContext.ruleSet != nullptr ? evaluationContext.ruleSet->GetSessionCurrentDateTime() : FormatLocalTime("%Y-%m-%dT%H:%M:%S"), Attribute::KnowledgeState::Certain, Attribute::DATETIME};
	}

	ResolvedValue BooleanValue(bool value, Attribute::KnowledgeState knowledgeState)
	{
		return ResolvedValue{true, true, value ? "true" : "false", knowledgeState, Attribute::BOOLEAN};
	}

	bool TryResolvedBoolean(const ResolvedValue& value, bool& result)
	{
		return value.supported && value.known && TryParseBoolean(value.value, result);
	}

	template<typename ConditionResolver>
	ResolvedValue EvaluateScopedQuantifierOverInstances(const std::vector<std::shared_ptr<Instance>>& instances,
	                                                    const std::shared_ptr<Instance>& outerContext,
	                                                    bool isForAll,
	                                                    bool hasCondition,
	                                                    bool emptyForAllIsFalse,
	                                                    EvaluationContext& evaluationContext,
	                                                    ConditionResolver resolveCondition,
	                                                    ScopedQuantifierTraceSummary* traceSummary = nullptr)
	{
		if(traceSummary != nullptr)
		{
			traceSummary->relatedCount = instances.size();
			traceSummary->evaluatedCount = hasCondition ? 0 : instances.size();
			traceSummary->matchedCount = hasCondition
				? 0
				: (isForAll ? instances.size() : (instances.empty() ? 0 : 1));
		}

		if(!hasCondition)
		{
			return BooleanValue(isForAll ? true : !instances.empty());
		}

		if(isForAll && emptyForAllIsFalse && instances.empty())
		{
			return BooleanValue(false);
		}

		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawKnown = false;

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			evaluationContext.scopeStack.push_back(outerContext);

			ResolvedValue value = resolveCondition(instance);

			evaluationContext.scopeStack.pop_back();

			if(traceSummary != nullptr)
			{
				traceSummary->evaluatedCount++;
			}

			if(!value.supported)
			{
				return value;
			}

			bool boolValue = false;

			if(!value.known)
			{
				sawUnknown = true;

				continue;
			}

			sawKnown = true;

			if(!TryParseBoolean(value.value, boolValue))
			{
				boolValue = false;
			}

			if(!isForAll && boolValue)
			{
				if(traceSummary != nullptr)
				{
					traceSummary->matchedCount++;
				}

				return BooleanValue(true, value.knowledgeState);
			}

			if(traceSummary != nullptr && boolValue)
			{
				traceSummary->matchedCount++;
			}

			if(isForAll && !boolValue)
			{
				if(value.knowledgeState == Attribute::KnowledgeState::Certain)
				{
					return BooleanValue(false);
				}

				sawUncertain = true;
			}
			else if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(isForAll)
		{
			if(sawUnknown)
			{
				return ResolvedValue{true, false, ""};
			}

			if(sawUncertain)
			{
				return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
			}

			return BooleanValue(true);
		}

		if(sawUncertain || (sawKnown && sawUnknown))
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		if(sawUnknown)
		{
			return ResolvedValue{true, false, ""};
		}

		return BooleanValue(false);
	}

	int CompareKnownValues(const std::string& left, const std::string& right)
	{
		double leftNumber = 0.0;
		double rightNumber = 0.0;

		if(TryParseDoubleValue(left, leftNumber) && TryParseDoubleValue(right, rightNumber))
		{
			if(leftNumber < rightNumber)
			{
				return -1;
			}

			if(leftNumber > rightNumber)
			{
				return 1;
			}

			return 0;
		}

		if(left < right)
		{
			return -1;
		}

		if(left > right)
		{
			return 1;
		}

		return 0;
	}

	bool TryParseIntegerValue(const std::string& value, int& result)
	{
		double number = 0.0;

		if(!TryParseDoubleValue(value, number))
		{
			return false;
		}

		result = static_cast<int>(number);

		return std::fabs(number - static_cast<double>(result)) < 0.0000001;
	}

	bool IsTemporalWeightedAggregateFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsWeightedAggregateFunctionName(functionName);
	}

	bool IsTemporalConditionalWeightedAggregateFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsConditionalWeightedAggregateFunctionName(functionName);
	}

	bool IsTemporalCountDistinctAggregateFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsCountDistinctAggregateFunctionName(functionName);
	}

	bool IsTemporalConditionalCountDistinctAggregateFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsConditionalCountDistinctAggregateFunctionName(functionName);
	}

	bool IsTemporalScalarDateComparisonFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsScalarDateComparisonFunctionName(functionName);
	}

	bool IsTemporalElapsedDateUnitFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsElapsedDateUnitFunctionName(functionName);
	}

	bool IsTemporalScalarDatePredicateFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsScalarDatePredicateFunctionName(functionName);
	}

	bool IsTemporalCurrentDateBoundaryPredicateFunctionName(const std::string& functionName)
	{
		return functionName == "TemporalFromStartDate"
			|| functionName == "TemporalFromEndDate";
	}

	bool IsTemporalCurrentDateRangePredicateFunctionName(const std::string& functionName)
	{
		return functionName == "TemporalFromRange";
	}

	bool IsTemporalCurrentDateMonthlyPredicateFunctionName(const std::string& functionName)
	{
		return functionName == "TemporalOncePerMonth";
	}

	bool IsTemporalCurrentDateComparisonPredicateFunctionName(const std::string& functionName)
	{
		return functionName == "TemporalOn"
			|| functionName == "TemporalBefore"
			|| functionName == "TemporalAfter"
			|| functionName == "TemporalOnOrBefore"
			|| functionName == "TemporalOnOrAfter";
	}

	TemporalOncePerMonthCallShape ClassifyTemporalOncePerMonthCallShape(size_t argumentCount)
	{
		if(argumentCount == 1)
		{
			return TemporalOncePerMonthCallShape::AnchorCurrentDate;
		}

		if(argumentCount == 2)
		{
			return TemporalOncePerMonthCallShape::AnchorConditionHistory;
		}

		if(argumentCount == 3)
		{
			return TemporalOncePerMonthCallShape::OverloadedThreeArgument;
		}

		if(argumentCount == 4)
		{
			return TemporalOncePerMonthCallShape::AnchorRangeConditionHistory;
		}

		return TemporalOncePerMonthCallShape::UnsupportedArity;
	}

	bool IsTemporalFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsKnownName(functionName);
	}

	bool IsTemporalIntervalFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsIntervalFunctionName(functionName);
	}

	bool IsTemporalExtremaFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsExtremaFunctionName(functionName);
	}

	bool IsTemporalConditionalAggregateFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsConditionalAggregateFunctionName(functionName);
	}

	bool IsTemporalDayCountFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsDayCountFunctionName(functionName);
	}

	bool IsTemporalRelativeWindowFunctionName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsRelativeWindowFunctionName(functionName);
	}

	const char* TemporalRelativeWindowShapeMessage(const std::string& functionName)
	{
		return "Temporal day-window functions require either days and a boolean condition timeline, or anchor, days, and a boolean condition timeline.";
	}

	bool TryParseTemporalInstant(const std::string& value, TemporalInstant& instant)
	{
		DateTimeParts parts;

		if(!TryParseDateTimeParts(value, parts) || !parts.hasDate)
		{
			return false;
		}

		instant.parsed = true;
		instant.seconds = SecondsFromParts(parts);
		instant.hasTime = parts.hasTime;

		return true;
	}

	std::string FormatTemporalInstant(long long seconds, bool hasTime)
	{
		DateTimeParts parts = PartsFromSeconds(seconds);

		return hasTime ? FormatDateTime(parts) : FormatDate(parts);
	}

	bool TryResolveTemporalInstantArgument(const RuleSet& ruleSet,
	                                       const std::shared_ptr<Instance>& context,
	                                       const std::string& argument,
	                                       EvaluationContext& evaluationContext,
	                                       TemporalInstant& instant,
	                                       Attribute::KnowledgeState& knowledgeState)
	{
		knowledgeState = Attribute::KnowledgeState::Certain;

		if(TryParseTemporalInstant(argument, instant))
		{
			return true;
		}

		if(TryParseTemporalInstant(RemoveMatchingQuotes(argument), instant))
		{
			return true;
		}

		ResolvedValue value = ResolveExpressionArgument(ruleSet, context, argument, evaluationContext);

		if(!value.supported)
		{
			return false;
		}

		knowledgeState = value.knowledgeState;

		if(!value.known)
		{
			return false;
		}

		return TryParseTemporalInstant(value.value, instant);
	}

	bool TryResolveTemporalInstantArgumentNode(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const json& argument,
	                                           EvaluationContext& evaluationContext,
	                                           TemporalInstant& instant,
	                                           Attribute::KnowledgeState& knowledgeState)
	{
		knowledgeState = Attribute::KnowledgeState::Certain;

		const std::string literalValue = FunctionArgumentValue(argument);

		if(TryParseTemporalInstant(literalValue, instant) || TryParseTemporalInstant(RemoveMatchingQuotes(literalValue), instant))
		{
			return true;
		}

		ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext);

		if(!value.supported)
		{
			return false;
		}

		knowledgeState = value.knowledgeState;

		if(!value.known)
		{
			return false;
		}

		return TryParseTemporalInstant(value.value, instant);
	}

	enum class TemporalDateOnlyLiteralResult
	{
		NotTemporalLiteral,
		Resolved,
		InvalidType
	};

	TemporalDateOnlyLiteralResult TryResolveTemporalDateOnlyLiteral(const std::string& argument, TemporalInstant& instant)
	{
		TemporalInstant parsed;

		if(!TryParseTemporalInstant(argument, parsed) && !TryParseTemporalInstant(RemoveMatchingQuotes(argument), parsed))
		{
			return TemporalDateOnlyLiteralResult::NotTemporalLiteral;
		}

		if(parsed.hasTime)
		{
			return TemporalDateOnlyLiteralResult::InvalidType;
		}

		instant = parsed;

		return TemporalDateOnlyLiteralResult::Resolved;
	}

	ResolvedValue TemporalElapsedDateTypeDiagnostic(const std::string& functionName, EvaluationContext& evaluationContext)
	{
		AddDiagnostic(evaluationContext,
		              "temporal_aggregate_type",
		              "Temporal elapsed-unit functions require Date-only arguments.",
		              functionName);

		return UnknownValue(Attribute::DOUBLE);
	}

	bool TryResolveTemporalDateOnlyExpressionArgument(const RuleSet& ruleSet,
	                                                 const std::shared_ptr<Instance>& context,
	                                                 const std::string& argument,
	                                                 const std::string& functionName,
	                                                 EvaluationContext& evaluationContext,
	                                                 TemporalInstant& instant,
	                                                 Attribute::KnowledgeState& knowledgeState,
	                                                 ResolvedValue& failureValue)
	{
		knowledgeState = Attribute::KnowledgeState::Certain;

		const TemporalDateOnlyLiteralResult literalResult = TryResolveTemporalDateOnlyLiteral(argument, instant);

		if(literalResult == TemporalDateOnlyLiteralResult::Resolved)
		{
			return true;
		}

		if(literalResult == TemporalDateOnlyLiteralResult::InvalidType)
		{
			failureValue = TemporalElapsedDateTypeDiagnostic(functionName, evaluationContext);

			return false;
		}

		ResolvedValue value = ResolveExpressionArgument(ruleSet, context, argument, evaluationContext);

		if(!value.supported)
		{
			failureValue = value;

			return false;
		}

		knowledgeState = value.knowledgeState;

		if(!value.known || value.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			failureValue = UnknownValue(Attribute::DOUBLE);

			return false;
		}

		if(value.valueType != Attribute::DATE
			|| !TryParseTemporalInstant(value.value, instant)
			|| instant.hasTime)
		{
			failureValue = TemporalElapsedDateTypeDiagnostic(functionName, evaluationContext);

			return false;
		}

		return true;
	}

	bool TryResolveTemporalDateOnlyNodeArgument(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const json& argument,
	                                           const std::string& functionName,
	                                           EvaluationContext& evaluationContext,
	                                           TemporalInstant& instant,
	                                           Attribute::KnowledgeState& knowledgeState,
	                                           ResolvedValue& failureValue)
	{
		knowledgeState = Attribute::KnowledgeState::Certain;

		const TemporalDateOnlyLiteralResult literalResult = TryResolveTemporalDateOnlyLiteral(FunctionArgumentValue(argument), instant);

		if(literalResult == TemporalDateOnlyLiteralResult::Resolved)
		{
			return true;
		}

		if(literalResult == TemporalDateOnlyLiteralResult::InvalidType)
		{
			failureValue = TemporalElapsedDateTypeDiagnostic(functionName, evaluationContext);

			return false;
		}

		ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext);

		if(!value.supported)
		{
			failureValue = value;

			return false;
		}

		knowledgeState = value.knowledgeState;

		if(!value.known || value.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			failureValue = UnknownValue(Attribute::DOUBLE);

			return false;
		}

		if(value.valueType != Attribute::DATE
			|| !TryParseTemporalInstant(value.value, instant)
			|| instant.hasTime)
		{
			failureValue = TemporalElapsedDateTypeDiagnostic(functionName, evaluationContext);

			return false;
		}

		return true;
	}

	bool TryParseHistoryBoundaryValue(const std::string& value,
	                                  const std::string& boundaryName,
	                                  const std::string& detail,
	                                  bool& isOpen,
	                                  TemporalInstant& instant,
	                                  EvaluationContext& evaluationContext)
	{
		isOpen = value.empty();

		if(isOpen)
		{
			return true;
		}

		if(TryParseTemporalInstant(value, instant))
		{
			return true;
		}

		AddDiagnostic(evaluationContext,
		              "temporal_history_boundary",
		              "Temporal history segment has an invalid effective-time boundary.",
		              detail + ":" + boundaryName + ":" + value);

		return false;
	}

	bool TryBuildHistoryCoverageSegment(const std::string& effectiveStart,
	                                    const std::string& effectiveEnd,
	                                    Attribute::KnowledgeState knowledgeState,
	                                    const std::string& detail,
	                                    const TemporalEffectiveWindow& window,
	                                    TemporalCoverageSegment& coverage,
	                                    EvaluationContext& evaluationContext)
	{
		bool startOpen = false;
		bool endOpen = false;
		TemporalInstant segmentStart;
		TemporalInstant segmentEnd;

		if(!TryParseHistoryBoundaryValue(effectiveStart, "effectiveStart", detail, startOpen, segmentStart, evaluationContext)
			|| !TryParseHistoryBoundaryValue(effectiveEnd, "effectiveEnd", detail, endOpen, segmentEnd, evaluationContext))
		{
			return false;
		}

		if(startOpen)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_boundary",
			              "Temporal history segment requires an effective start.",
			              detail);

			return false;
		}

		if(!endOpen && segmentEnd.seconds <= segmentStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_boundary",
			              "Temporal history segment has an invalid effective-time interval.",
			              detail + ":" + effectiveStart + ":" + effectiveEnd);

			return false;
		}

		const long long rawStart = segmentStart.seconds;
		const long long rawEnd = endOpen ? window.end.seconds : segmentEnd.seconds;

		if(rawEnd <= window.start.seconds || rawStart >= window.end.seconds)
		{
			coverage.source = nullptr;

			return true;
		}

		coverage.startSeconds = rawStart > window.start.seconds ? rawStart : window.start.seconds;
		coverage.endSeconds = rawEnd < window.end.seconds ? rawEnd : window.end.seconds;
		coverage.knowledgeState = knowledgeState;
		coverage.source = nullptr;

		if(coverage.endSeconds <= coverage.startSeconds)
		{
			coverage.source = nullptr;
		}

		return true;
	}

	bool TryParseSegmentBoundary(const RuleSet::AttributeHistorySegment& segment,
	                             const std::string& value,
	                             const std::string& boundaryName,
	                             bool& isOpen,
	                             TemporalInstant& instant,
	                             EvaluationContext& evaluationContext)
	{
		isOpen = value.empty();

		if(isOpen)
		{
			return true;
		}

		if(TryParseTemporalInstant(value, instant))
		{
			return true;
		}

		AddDiagnostic(evaluationContext,
		              "temporal_history_boundary",
		              "Attribute history segment has an invalid effective-time boundary.",
		              segment.instanceId + ":" + segment.attributeId + ":" + boundaryName + ":" + value);

		return false;
	}

	bool SegmentCoversInstant(const RuleSet::AttributeHistorySegment& segment,
	                          const TemporalInstant& instant,
	                          EvaluationContext& evaluationContext)
	{
		bool startOpen = false;
		bool endOpen = false;
		TemporalInstant start;
		TemporalInstant end;

		if(!TryParseSegmentBoundary(segment, segment.effectiveStart, "effectiveStart", startOpen, start, evaluationContext)
			|| !TryParseSegmentBoundary(segment, segment.effectiveEnd, "effectiveEnd", endOpen, end, evaluationContext))
		{
			return false;
		}

		if(!startOpen && start.seconds > instant.seconds)
		{
			return false;
		}

		if(!endOpen && instant.seconds >= end.seconds)
		{
			return false;
		}

		return true;
	}

	Attribute::AttributeType EffectiveHistoryValueType(const RuleSet::AttributeHistorySegment& segment,
	                                                   Attribute::AttributeType fallbackType)
	{
		return segment.valueType == Attribute::UNKNOWN ? fallbackType : segment.valueType;
	}

	bool TryNormalizeHistoryValue(const RuleSet::AttributeHistorySegment& segment,
	                              Attribute::AttributeType fallbackType,
	                              std::string& normalizedValue,
	                              Attribute::AttributeType& valueType)
	{
		valueType = EffectiveHistoryValueType(segment, fallbackType);

		if(valueType == Attribute::UNKNOWN)
		{
			normalizedValue = segment.value;

			return !normalizedValue.empty();
		}

		if(valueType == Attribute::STRING && segment.value.empty())
		{
			normalizedValue.clear();

			return true;
		}

		ValueVariant convertedValue;

		if(!Attribute::TryConvertValue(segment.value, valueType, convertedValue))
		{
			return false;
		}

		if(std::holds_alternative<int>(convertedValue))
		{
			normalizedValue = std::to_string(std::get<int>(convertedValue));
		}
		else if(std::holds_alternative<double>(convertedValue))
		{
			normalizedValue = FormatDoubleValue(std::get<double>(convertedValue));
		}
		else if(std::holds_alternative<bool>(convertedValue))
		{
			normalizedValue = std::get<bool>(convertedValue) ? "true" : "false";
		}
		else if(std::holds_alternative<std::string>(convertedValue))
		{
			normalizedValue = std::get<std::string>(convertedValue);
		}
		else
		{
			normalizedValue = segment.value;
		}

		return true;
	}

	std::shared_ptr<Attribute> FindTemporalAttributeOnInstance(const std::shared_ptr<Instance>& instance,
	                                                           const std::string& identifier)
	{
		if(instance == nullptr)
		{
			return nullptr;
		}

		std::shared_ptr<Attribute> attribute = instance->GetAttributeByID(identifier);

		return attribute != nullptr ? attribute : FindAttributeByIdentifierOrName(instance, identifier);
	}

	bool ResolveTemporalAttributeReference(const std::shared_ptr<Instance>& context,
	                                       const std::string& entityId,
	                                       const std::string& attributeIdentifier,
	                                       const std::string& relationshipId,
	                                       TemporalAttributeReference& reference)
	{
		if(context == nullptr || attributeIdentifier.empty())
		{
			return false;
		}

		const std::string resolvedEntityId = entityId.empty() ? context->entityId : entityId;
		std::vector<std::shared_ptr<Instance>> instances = ResolveRelatedInstances(context, resolvedEntityId, relationshipId);

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			std::shared_ptr<Attribute> attribute = FindTemporalAttributeOnInstance(instance, attributeIdentifier);

			if(attribute == nullptr)
			{
				continue;
			}

			reference.resolved = true;
			reference.entityId = instance->entityId;
			reference.instanceId = instance->instanceId;
			reference.attributeId = attribute->GetID();
			reference.valueType = static_cast<Attribute::AttributeType>(attribute->GetType());

			return true;
		}

		return false;
	}

	bool ResolveTemporalAttributeArgument(const std::shared_ptr<Instance>& context,
	                                      const std::string& argument,
	                                      TemporalAttributeReference& reference)
	{
		return ResolveTemporalAttributeReference(context, context != nullptr ? context->entityId : "", RemoveMatchingQuotes(argument), "", reference);
	}

	bool ResolveTemporalAttributeArgumentNode(const std::shared_ptr<Instance>& context,
	                                          const json& argument,
	                                          TemporalAttributeReference& reference)
	{
		if(!argument.is_object())
		{
			return ResolveTemporalAttributeArgument(context, FunctionArgumentValue(argument), reference);
		}

		if(argument.contains("nestedFunction") || argument.contains("functionName"))
		{
			return false;
		}

		const std::string valueMode = GetString(argument, "valueMode");

		if(valueMode.empty() || valueMode == "Literal")
		{
			return ResolveTemporalAttributeArgument(context, FunctionArgumentValue(argument), reference);
		}

		if(valueMode != "Attribute")
		{
			return false;
		}

		std::string attributeId = FirstNonEmpty(argument, {
			"attributeId",
			"valueAttributeId",
			"sourceAttributeId",
			"comparisonAttributeId",
			"attributeValueId",
			"value",
			"attributeName",
			"coreName"
		});

		std::string entityId = FirstNonEmpty(argument, {
			"entityId",
			"valueEntityId",
			"sourceEntityId",
			"comparisonEntityId"
		});

		const std::string relationshipId = FirstNonEmpty(argument, {
			"relationshipId",
			"valueRelationshipId",
			"sourceRelationshipId",
			"comparisonRelationshipId"
		});

		return ResolveTemporalAttributeReference(context, entityId, attributeId, relationshipId, reference);
	}

	ResolvedValue TemporalHistoryTypeConversionFailure(const RuleSet::AttributeHistorySegment& segment,
	                                                   EvaluationContext& evaluationContext,
	                                                   Attribute::AttributeType valueType)
	{
		AddDiagnostic(evaluationContext,
		              "temporal_history_type_conversion",
		              "Attribute history segment value is not type-correct.",
		              segment.instanceId + ":" + segment.attributeId + ":" + segment.value);

		return UnknownValue(valueType);
	}

	ResolvedValue EvaluateTemporalValueAt(const RuleSet& ruleSet,
	                                      const TemporalAttributeReference& reference,
	                                      const TemporalInstant& instant,
	                                      EvaluationContext& evaluationContext)
	{
		std::vector<const RuleSet::AttributeHistorySegment*> segments = ruleSet.GetAttributeHistorySegments(reference.instanceId, reference.attributeId);

		if(segments.empty())
		{
			return UnknownValue(reference.valueType);
		}

		bool sawMatchingSegment = false;
		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawCertain = false;
		std::string certainValue;
		std::string uncertainValue;
		Attribute::AttributeType certainType = reference.valueType;
		Attribute::AttributeType uncertainType = reference.valueType;

		for(const RuleSet::AttributeHistorySegment* segmentPointer : segments)
		{
			if(segmentPointer == nullptr || !SegmentCoversInstant(*segmentPointer, instant, evaluationContext))
			{
				continue;
			}

			const RuleSet::AttributeHistorySegment& segment = *segmentPointer;
			sawMatchingSegment = true;

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			std::string normalizedValue;
			Attribute::AttributeType valueType = reference.valueType;

			if(!TryNormalizeHistoryValue(segment, reference.valueType, normalizedValue, valueType))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, valueType);
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				if(!sawUncertain)
				{
					uncertainValue = normalizedValue;
					uncertainType = valueType;
				}

				sawUncertain = true;

				continue;
			}

			if(sawCertain && normalizedValue != certainValue)
			{
				AddDiagnostic(evaluationContext,
				              "temporal_conflict",
				              "Attribute history has conflicting certain values at the requested effective time.",
				              reference.instanceId + ":" + reference.attributeId + ":" + FormatTemporalInstant(instant.seconds, instant.hasTime));

				return UnknownValue(valueType);
			}

			sawCertain = true;
			certainValue = normalizedValue;
			certainType = valueType;
		}

		if(!sawMatchingSegment || sawUnknown)
		{
			return UnknownValue(reference.valueType);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, uncertainValue, Attribute::KnowledgeState::Uncertain, uncertainType};
		}

		if(sawCertain)
		{
			return ResolvedValue{true, true, certainValue, Attribute::KnowledgeState::Certain, certainType};
		}

		return UnknownValue(reference.valueType);
	}

	bool TryValidObservedHistorySegment(const RuleSet::AttributeHistorySegment& segment,
	                                    Attribute::AttributeType fallbackType,
	                                    TemporalInstant& start,
	                                    EvaluationContext& evaluationContext)
	{
		if(segment.knowledgeState != Attribute::KnowledgeState::Certain)
		{
			return false;
		}

		if(segment.effectiveStart.empty() || !TryParseTemporalInstant(segment.effectiveStart, start))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_boundary",
			              "Observed history segment has an invalid effective start.",
			              segment.instanceId + ":" + segment.attributeId + ":effectiveStart:" + segment.effectiveStart);

			return false;
		}

		std::string normalizedValue;
		Attribute::AttributeType valueType = fallbackType;

		if(!TryNormalizeHistoryValue(segment, fallbackType, normalizedValue, valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_type_conversion",
			              "Observed history segment value is not type-correct.",
			              segment.instanceId + ":" + segment.attributeId + ":" + segment.value);

			return false;
		}

		return true;
	}

	ResolvedValue EvaluateTemporalObservedDateFunction(const RuleSet& ruleSet,
	                                                   const std::string& functionName,
	                                                   const TemporalAttributeReference& reference,
	                                                   const TemporalInstant* anchor,
	                                                   EvaluationContext& evaluationContext)
	{
		std::vector<const RuleSet::AttributeHistorySegment*> segments = ruleSet.GetAttributeHistorySegments(reference.instanceId, reference.attributeId);
		bool hasBest = false;
		long long bestSeconds = 0;
		bool bestHasTime = false;

		for(const RuleSet::AttributeHistorySegment* segmentPointer : segments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalInstant start;

			if(!TryValidObservedHistorySegment(*segmentPointer, reference.valueType, start, evaluationContext))
			{
				continue;
			}

			if(functionName == "WhenLast" && anchor != nullptr && start.seconds > anchor->seconds)
			{
				continue;
			}

			if(functionName == "WhenNext" && anchor != nullptr && start.seconds <= anchor->seconds)
			{
				continue;
			}

			bool isBetter = !hasBest;

			if(!isBetter)
			{
				if(functionName == "Earliest" || functionName == "WhenNext")
				{
					isBetter = start.seconds < bestSeconds;
				}
				else
				{
					isBetter = start.seconds > bestSeconds;
				}
			}

			if(isBetter)
			{
				hasBest = true;
				bestSeconds = start.seconds;
				bestHasTime = start.hasTime;
			}
		}

		if(!hasBest)
		{
			return UnknownValue(Attribute::DATE);
		}

		return ResolvedValue{
			true,
			true,
			FormatTemporalInstant(bestSeconds, bestHasTime),
			Attribute::KnowledgeState::Certain,
			bestHasTime ? Attribute::DATETIME : Attribute::DATE
		};
	}

	bool TryBuildTemporalExtremaCoverageSegment(const RuleSet::AttributeHistorySegment& segment,
	                                            const TemporalInstant& intervalStart,
	                                            const TemporalInstant& intervalEnd,
	                                            TemporalCoverageSegment& coverage,
	                                            EvaluationContext& evaluationContext)
	{
		bool startOpen = false;
		bool endOpen = false;
		TemporalInstant segmentStart;
		TemporalInstant segmentEnd;

		if(!TryParseSegmentBoundary(segment, segment.effectiveStart, "effectiveStart", startOpen, segmentStart, evaluationContext)
			|| !TryParseSegmentBoundary(segment, segment.effectiveEnd, "effectiveEnd", endOpen, segmentEnd, evaluationContext))
		{
			return false;
		}

		if(!startOpen && !endOpen && segmentEnd.seconds <= segmentStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_boundary",
			              "Attribute history segment has an invalid effective-time interval.",
			              segment.instanceId + ":" + segment.attributeId + ":" + segment.effectiveStart + ":" + segment.effectiveEnd);

			return false;
		}

		const long long rawStart = startOpen ? intervalStart.seconds : segmentStart.seconds;
		const long long rawEnd = endOpen ? intervalEnd.seconds : segmentEnd.seconds;

		if(rawEnd <= intervalStart.seconds || rawStart >= intervalEnd.seconds)
		{
			coverage.source = nullptr;

			return true;
		}

		coverage.startSeconds = rawStart > intervalStart.seconds ? rawStart : intervalStart.seconds;
		coverage.endSeconds = rawEnd < intervalEnd.seconds ? rawEnd : intervalEnd.seconds;
		coverage.knowledgeState = segment.knowledgeState;
		coverage.source = &segment;

		if(coverage.endSeconds <= coverage.startSeconds)
		{
			coverage.source = nullptr;
		}

		return true;
	}

	bool TemporalExtremaObservedSegmentsOverlap(const TemporalExtremaObservedSegment& left,
	                                            const TemporalCoverageSegment& right)
	{
		const long long overlapStart = left.startSeconds > right.startSeconds ? left.startSeconds : right.startSeconds;
		const long long overlapEnd = left.endSeconds < right.endSeconds ? left.endSeconds : right.endSeconds;

		return overlapStart < overlapEnd;
	}

	bool IsSupportedTemporalExtremaValueType(Attribute::AttributeType type)
	{
		return IsNumericAttributeType(type) || IsTemporalAttributeType(type);
	}

	bool IsTemporalMaximumFunctionName(const std::string& functionName)
	{
		return functionName == "IntervalMaximum"
			|| functionName == "IntervalMaximumIf";
	}

	bool TemporalConditionHasOverlap(const std::vector<TemporalCoverageSegment>& conditionSegments,
	                                 const TemporalCoverageSegment& valueCoverage,
	                                 Attribute::KnowledgeState knowledgeState,
	                                 bool boolValue = false)
	{
		for(const TemporalCoverageSegment& condition : conditionSegments)
		{
			if(condition.knowledgeState != knowledgeState || !TemporalCoverageSegmentsOverlap(condition, valueCoverage))
			{
				continue;
			}

			if(knowledgeState != Attribute::KnowledgeState::Certain || condition.boolValue == boolValue)
			{
				return true;
			}
		}

		return false;
	}

	bool TemporalConditionHasConflictForCoverage(const std::vector<TemporalCoverageSegment>& conditionSegments,
	                                             const TemporalCoverageSegment& valueCoverage)
	{
		for(size_t leftIndex = 0; leftIndex < conditionSegments.size(); ++leftIndex)
		{
			const TemporalCoverageSegment& left = conditionSegments[leftIndex];

			if(left.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				continue;
			}

			for(size_t rightIndex = leftIndex + 1; rightIndex < conditionSegments.size(); ++rightIndex)
			{
				const TemporalCoverageSegment& right = conditionSegments[rightIndex];

				if(right.knowledgeState == Attribute::KnowledgeState::Certain
					&& left.boolValue != right.boolValue
					&& TemporalCoverageSegmentsOverlap(left, right))
				{
					const long long overlapStart = left.startSeconds > right.startSeconds ? left.startSeconds : right.startSeconds;
					const long long overlapEnd = left.endSeconds < right.endSeconds ? left.endSeconds : right.endSeconds;

					if(TemporalCoverageSegmentOverlapsRange(valueCoverage, overlapStart, overlapEnd))
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	bool TryCollectTemporalConditionSegments(const RuleSet& ruleSet,
	                                         const std::string& functionName,
	                                         const TemporalAttributeReference& conditionReference,
	                                         const TemporalInstant& intervalStart,
	                                         const TemporalInstant& intervalEnd,
	                                         EvaluationContext& evaluationContext,
	                                         std::vector<TemporalCoverageSegment>& conditionSegments)
	{
		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(conditionReference.instanceId, conditionReference.attributeId);

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalCoverageSegment(*segmentPointer, conditionReference, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return false;
			}

			if(coverage.source != nullptr)
			{
				conditionSegments.push_back(coverage);
			}
		}

		return true;
	}

	ResolvedValue EvaluateTemporalExtremaFunction(const RuleSet& ruleSet,
	                                              const std::string& functionName,
	                                              const TemporalAttributeReference& reference,
	                                              const TemporalInstant& intervalStart,
	                                              const TemporalInstant& intervalEnd,
	                                              EvaluationContext& evaluationContext)
	{
		const Attribute::AttributeType resultType = NormalizeAggregateItemType(reference.valueType);

		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(resultType);
		}

		if(!IsSupportedTemporalExtremaValueType(reference.valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "Interval extrema require Number, Date, DateTime, or TimeOfDay scalar history.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(resultType);
		}

		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(reference.instanceId, reference.attributeId);
		std::vector<TemporalCoverageSegment> coverageSegments;
		std::vector<TemporalExtremaObservedSegment> observedSegments;
		bool sawOverlap = false;
		bool sawUnknown = false;
		bool sawUncertain = false;
		bool hasBest = false;
		double bestNumericValue = 0.0;
		long long bestTemporalValue = 0;
		std::string bestValue;

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalExtremaCoverageSegment(*segmentPointer, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return UnknownValue(resultType);
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			const RuleSet::AttributeHistorySegment& segment = *coverage.source;
			sawOverlap = true;
			coverageSegments.push_back(coverage);

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			std::string normalizedValue;
			Attribute::AttributeType valueType = reference.valueType;

			if(!TryNormalizeHistoryValue(segment, reference.valueType, normalizedValue, valueType))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, resultType);
			}

			if(!IsSupportedTemporalExtremaValueType(valueType)
				|| !AreCompatibleCollectionAggregateTypes(resultType, valueType))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_aggregate_type",
				              "Interval extrema require homogeneous Number, Date, DateTime, or TimeOfDay scalar history.",
				              segment.instanceId + ":" + segment.attributeId);

				return UnknownValue(resultType);
			}

			double numericValue = 0.0;
			long long temporalValue = 0;
			std::string comparableValue;

			if(!TryResolveAggregateComparableValue(normalizedValue, valueType, numericValue, temporalValue, comparableValue))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, resultType);
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			for(const TemporalExtremaObservedSegment& observed : observedSegments)
			{
				if(TemporalExtremaObservedSegmentsOverlap(observed, coverage)
					&& observed.normalizedValue != comparableValue)
				{
					AddDiagnostic(evaluationContext,
					              "temporal_conflict",
					              "Attribute history has conflicting certain values over the requested interval.",
					              reference.instanceId + ":" + reference.attributeId + ":" + functionName);

					return UnknownValue(resultType);
				}
			}

			TemporalExtremaObservedSegment observed;
			observed.startSeconds = coverage.startSeconds;
			observed.endSeconds = coverage.endSeconds;
			observed.normalizedValue = comparableValue;
			observed.numericValue = numericValue;
			observed.temporalValue = temporalValue;
			observedSegments.push_back(observed);

			const bool numericResult = IsNumericAttributeType(resultType);
			const bool better = !hasBest
				|| (IsTemporalMaximumFunctionName(functionName)
					? (numericResult ? numericValue > bestNumericValue : temporalValue > bestTemporalValue)
					: (numericResult ? numericValue < bestNumericValue : temporalValue < bestTemporalValue));

			if(better)
			{
				bestNumericValue = numericValue;
				bestTemporalValue = temporalValue;
				bestValue = comparableValue;
				hasBest = true;
			}
		}

		if(hasBest)
		{
			return ResolvedValue{true, true, bestValue, Attribute::KnowledgeState::Certain, resultType};
		}

		if(!sawOverlap || !HasCompleteCoverage(coverageSegments, intervalStart.seconds, intervalEnd.seconds) || sawUnknown)
		{
			return UnknownValue(resultType);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, resultType};
		}

		return UnknownValue(resultType);
	}

	ResolvedValue EvaluateTemporalConditionalExtremaFunction(const RuleSet& ruleSet,
	                                                         const std::string& functionName,
	                                                         const TemporalAttributeReference& valueReference,
	                                                         const TemporalAttributeReference& conditionReference,
	                                                         const TemporalInstant& intervalStart,
	                                                         const TemporalInstant& intervalEnd,
	                                                         EvaluationContext& evaluationContext)
	{
		const Attribute::AttributeType resultType = NormalizeAggregateItemType(valueReference.valueType);

		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(resultType);
		}

		if(!IsSupportedTemporalExtremaValueType(valueReference.valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "Conditional interval extrema require Number, Date, DateTime, or TimeOfDay scalar history.",
			              valueReference.instanceId + ":" + valueReference.attributeId);

			return UnknownValue(resultType);
		}

		if(conditionReference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "Conditional interval extrema currently support direct scalar boolean committed-history condition attributes only.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId);

			return UnknownValue(resultType);
		}

		std::vector<TemporalCoverageSegment> conditionSegments;

		if(!TryCollectTemporalConditionSegments(ruleSet, functionName, conditionReference, intervalStart, intervalEnd, evaluationContext, conditionSegments))
		{
			return UnknownValue(resultType);
		}

		std::vector<const RuleSet::AttributeHistorySegment*> valueSegments = ruleSet.GetAttributeHistorySegments(valueReference.instanceId, valueReference.attributeId);
		std::vector<TemporalExtremaObservedSegment> observedSegments;
		bool sawValueOverlap = false;
		bool sawUnknown = false;
		bool sawUncertain = false;
		bool hasBest = false;
		double bestNumericValue = 0.0;
		long long bestTemporalValue = 0;
		std::string bestValue;

		for(const RuleSet::AttributeHistorySegment* segmentPointer : valueSegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalExtremaCoverageSegment(*segmentPointer, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return UnknownValue(resultType);
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			sawValueOverlap = true;

			if(TemporalConditionHasConflictForCoverage(conditionSegments, coverage))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_conflict",
				              "Conditional interval condition history has conflicting certain values over a relevant interval.",
				              conditionReference.instanceId + ":" + conditionReference.attributeId + ":" + functionName);

				return UnknownValue(resultType);
			}

			const bool hasTrueCondition = TemporalConditionHasOverlap(conditionSegments, coverage, Attribute::KnowledgeState::Certain, true);
			const bool hasUnknownCondition = TemporalConditionHasOverlap(conditionSegments, coverage, Attribute::KnowledgeState::Unknown);
			const bool hasUncertainCondition = TemporalConditionHasOverlap(conditionSegments, coverage, Attribute::KnowledgeState::Uncertain);
			const bool hasConditionGap = !HasCompleteCoverage(conditionSegments, coverage.startSeconds, coverage.endSeconds);

			if(!hasTrueCondition)
			{
				if(hasConditionGap || hasUnknownCondition || coverage.knowledgeState == Attribute::KnowledgeState::Unknown)
				{
					sawUnknown = true;
				}
				else if(hasUncertainCondition || coverage.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					sawUncertain = true;
				}

				continue;
			}

			const RuleSet::AttributeHistorySegment& segment = *coverage.source;

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			std::string normalizedValue;
			Attribute::AttributeType valueType = valueReference.valueType;

			if(!TryNormalizeHistoryValue(segment, valueReference.valueType, normalizedValue, valueType))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, resultType);
			}

			if(!IsSupportedTemporalExtremaValueType(valueType)
				|| !AreCompatibleCollectionAggregateTypes(resultType, valueType))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_aggregate_type",
				              "Conditional interval extrema require homogeneous Number, Date, DateTime, or TimeOfDay scalar history.",
				              segment.instanceId + ":" + segment.attributeId);

				return UnknownValue(resultType);
			}

			double numericValue = 0.0;
			long long temporalValue = 0;
			std::string comparableValue;

			if(!TryResolveAggregateComparableValue(normalizedValue, valueType, numericValue, temporalValue, comparableValue))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, resultType);
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			for(const TemporalCoverageSegment& condition : conditionSegments)
			{
				if(condition.knowledgeState != Attribute::KnowledgeState::Certain
					|| !condition.boolValue
					|| !TemporalCoverageSegmentsOverlap(condition, coverage))
				{
					continue;
				}

				TemporalExtremaObservedSegment observed;
				observed.startSeconds = coverage.startSeconds > condition.startSeconds ? coverage.startSeconds : condition.startSeconds;
				observed.endSeconds = coverage.endSeconds < condition.endSeconds ? coverage.endSeconds : condition.endSeconds;
				observed.normalizedValue = comparableValue;
				observed.numericValue = numericValue;
				observed.temporalValue = temporalValue;

				for(const TemporalExtremaObservedSegment& existing : observedSegments)
				{
					TemporalCoverageSegment observedCoverage;
					observedCoverage.startSeconds = observed.startSeconds;
					observedCoverage.endSeconds = observed.endSeconds;

					if(TemporalExtremaObservedSegmentsOverlap(existing, observedCoverage)
						&& existing.normalizedValue != observed.normalizedValue)
					{
						AddDiagnostic(evaluationContext,
						              "temporal_conflict",
						              "Conditional interval value history has conflicting certain values under a true condition.",
						              valueReference.instanceId + ":" + valueReference.attributeId + ":" + functionName);

						return UnknownValue(resultType);
					}
				}

				observedSegments.push_back(observed);
			}

			const bool numericResult = IsNumericAttributeType(resultType);
			const bool better = !hasBest
				|| (IsTemporalMaximumFunctionName(functionName)
					? (numericResult ? numericValue > bestNumericValue : temporalValue > bestTemporalValue)
					: (numericResult ? numericValue < bestNumericValue : temporalValue < bestTemporalValue));

			if(better)
			{
				bestNumericValue = numericValue;
				bestTemporalValue = temporalValue;
				bestValue = comparableValue;
				hasBest = true;
			}
		}

		if(hasBest)
		{
			return ResolvedValue{true, true, bestValue, Attribute::KnowledgeState::Certain, resultType};
		}

		if(!sawValueOverlap || sawUnknown)
		{
			return UnknownValue(resultType);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, resultType};
		}

		return UnknownValue(resultType);
	}

	struct TemporalScalarComparisonInterval
	{
		long long startSeconds = 0;
		long long endSeconds = 0;
		bool isInstant = false;
	};

	bool IsTemporalScalarComparisonValueType(Attribute::AttributeType valueType)
	{
		return valueType == Attribute::DATE || valueType == Attribute::DATETIME;
	}

	bool TryBuildTemporalScalarComparisonInterval(const ResolvedValue& value, TemporalScalarComparisonInterval& interval)
	{
		if(!IsTemporalScalarComparisonValueType(value.valueType))
		{
			return false;
		}

		TemporalInstant instant;

		if(!TryParseTemporalInstant(value.value, instant))
		{
			return false;
		}

		if(value.valueType == Attribute::DATE)
		{
			interval.startSeconds = LocalDayStartSeconds(instant.seconds);
			interval.endSeconds = interval.startSeconds + 86400LL;
			interval.isInstant = false;

			return true;
		}

		interval.startSeconds = instant.seconds;
		interval.endSeconds = instant.seconds;
		interval.isInstant = true;

		return true;
	}

	bool TemporalScalarComparisonIntervalsOverlap(const TemporalScalarComparisonInterval& left,
	                                              const TemporalScalarComparisonInterval& right)
	{
		if(left.isInstant && right.isInstant)
		{
			return left.startSeconds == right.startSeconds;
		}

		if(left.isInstant)
		{
			return left.startSeconds >= right.startSeconds && left.startSeconds < right.endSeconds;
		}

		if(right.isInstant)
		{
			return right.startSeconds >= left.startSeconds && right.startSeconds < left.endSeconds;
		}

		return left.startSeconds < right.endSeconds && right.startSeconds < left.endSeconds;
	}

	ResolvedValue EvaluateTemporalScalarDateComparisonFunction(const std::string& functionName,
	                                                           const std::vector<ResolvedValue>& arguments,
	                                                           EvaluationContext& evaluationContext)
	{
		if(arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_function_shape",
			              "Temporal scalar comparison predicates require exactly two Date or DateTime arguments.",
			              functionName);

			return UnknownValue(Attribute::BOOLEAN);
		}

		for(const ResolvedValue& argument : arguments)
		{
			if(!argument.supported)
			{
				return argument;
			}

			if(!argument.known || argument.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}
		}

		TemporalScalarComparisonInterval left;
		TemporalScalarComparisonInterval right;

		if(!TryBuildTemporalScalarComparisonInterval(arguments[0], left)
			|| !TryBuildTemporalScalarComparisonInterval(arguments[1], right))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "Temporal scalar comparison predicates require Date or DateTime arguments.",
			              functionName);

			return UnknownValue(Attribute::BOOLEAN);
		}

		const bool on = TemporalScalarComparisonIntervalsOverlap(left, right);
		bool result = false;

		if(functionName == "TemporalOn")
		{
			result = on;
		}
		else if(functionName == "TemporalBefore")
		{
			result = !on && left.endSeconds <= right.startSeconds;
		}
		else if(functionName == "TemporalAfter")
		{
			result = !on && left.startSeconds >= right.endSeconds;
		}
		else if(functionName == "TemporalOnOrBefore")
		{
			result = on || left.endSeconds <= right.startSeconds;
		}
		else if(functionName == "TemporalOnOrAfter")
		{
			result = on || left.startSeconds >= right.endSeconds;
		}
		else
		{
			return ResolvedValue{false, false, ""};
		}

		const Attribute::KnowledgeState knowledgeState =
			(arguments[0].knowledgeState == Attribute::KnowledgeState::Uncertain
				|| arguments[1].knowledgeState == Attribute::KnowledgeState::Uncertain)
				? Attribute::KnowledgeState::Uncertain
				: Attribute::KnowledgeState::Certain;

		return BooleanValue(result, knowledgeState);
	}

	bool TemporalWeightedObservedSegmentCovers(const TemporalWeightedObservedSegment& segment, long long startSeconds, long long endSeconds)
	{
		return segment.startSeconds <= startSeconds && segment.endSeconds >= endSeconds;
	}

	ResolvedValue EvaluateTemporalWeightedAggregateFunction(const RuleSet& ruleSet,
	                                                        const std::string& functionName,
	                                                        const TemporalAttributeReference& reference,
	                                                        const TemporalInstant& intervalStart,
	                                                        const TemporalInstant& intervalEnd,
	                                                        EvaluationContext& evaluationContext)
	{
		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(Attribute::DOUBLE);
		}

		if(!IsNumericAttributeType(reference.valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "Daily/time-weighted interval aggregates require Number or Currency scalar history.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(Attribute::DOUBLE);
		}

		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(reference.instanceId, reference.attributeId);
		std::vector<TemporalCoverageSegment> coverageSegments;
		std::vector<TemporalWeightedObservedSegment> observedSegments;
		bool sawUnknown = false;
		bool sawUncertain = false;

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalExtremaCoverageSegment(*segmentPointer, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			coverageSegments.push_back(coverage);

			const RuleSet::AttributeHistorySegment& segment = *coverage.source;

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			std::string normalizedValue;
			Attribute::AttributeType valueType = reference.valueType;

			if(!TryNormalizeHistoryValue(segment, reference.valueType, normalizedValue, valueType))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
			}

			if(!IsNumericAttributeType(valueType))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_aggregate_type",
				              "Daily/time-weighted interval aggregates require homogeneous Number or Currency scalar history.",
				              segment.instanceId + ":" + segment.attributeId);

				return UnknownValue(Attribute::DOUBLE);
			}

			double numericValue = 0.0;
			long long temporalValue = 0;
			std::string comparableValue;

			if(!TryResolveAggregateComparableValue(normalizedValue, valueType, numericValue, temporalValue, comparableValue))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			for(const TemporalWeightedObservedSegment& observed : observedSegments)
			{
				const long long overlapStart = observed.startSeconds > coverage.startSeconds ? observed.startSeconds : coverage.startSeconds;
				const long long overlapEnd = observed.endSeconds < coverage.endSeconds ? observed.endSeconds : coverage.endSeconds;

				if(overlapStart < overlapEnd && observed.normalizedValue != comparableValue)
				{
					AddDiagnostic(evaluationContext,
					              "temporal_conflict",
					              "Attribute history has conflicting certain values over the requested interval.",
					              reference.instanceId + ":" + reference.attributeId + ":" + functionName);

					return UnknownValue(Attribute::DOUBLE);
				}
			}

			observedSegments.push_back(TemporalWeightedObservedSegment{
				coverage.startSeconds,
				coverage.endSeconds,
				comparableValue,
				numericValue
			});
		}

		if(!HasCompleteCoverage(coverageSegments, intervalStart.seconds, intervalEnd.seconds) || sawUnknown)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};
		}

		std::vector<long long> boundaries;

		boundaries.push_back(intervalStart.seconds);
		boundaries.push_back(intervalEnd.seconds);

		for(long long dayBoundary = LocalDayStartSeconds(intervalStart.seconds) + 86400LL;
			dayBoundary < intervalEnd.seconds;
			dayBoundary += 86400LL)
		{
			if(dayBoundary > intervalStart.seconds)
			{
				boundaries.push_back(dayBoundary);
			}
		}

		for(const TemporalWeightedObservedSegment& observed : observedSegments)
		{
			if(observed.startSeconds > intervalStart.seconds && observed.startSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(observed.startSeconds);
			}

			if(observed.endSeconds > intervalStart.seconds && observed.endSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(observed.endSeconds);
			}
		}

		std::sort(boundaries.begin(), boundaries.end());
		boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

		double weightedSum = 0.0;
		double totalWeight = 0.0;

		for(size_t index = 1; index < boundaries.size(); ++index)
		{
			const long long sliceStart = boundaries[index - 1];
			const long long sliceEnd = boundaries[index];

			if(sliceEnd <= sliceStart)
			{
				continue;
			}

			const TemporalWeightedObservedSegment* selected = nullptr;

			for(const TemporalWeightedObservedSegment& observed : observedSegments)
			{
				if(TemporalWeightedObservedSegmentCovers(observed, sliceStart, sliceEnd))
				{
					selected = &observed;

					break;
				}
			}

			if(selected == nullptr)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			const double weight = TemporalDayFraction(sliceStart, sliceEnd);

			weightedSum += selected->numericValue * weight;
			totalWeight += weight;
		}

		if(totalWeight <= 0.0)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		const double result = functionName == "IntervalWeightedAverage"
			? weightedSum / totalWeight
			: weightedSum;

		return ResolvedValue{true, true, FormatDoubleValue(result), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
	}

	ResolvedValue EvaluateTemporalCountDistinctFunction(const RuleSet& ruleSet,
	                                                    const std::string& functionName,
	                                                    const TemporalAttributeReference& reference,
	                                                    const TemporalInstant& intervalStart,
	                                                    const TemporalInstant& intervalEnd,
	                                                    EvaluationContext& evaluationContext)
	{
		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(Attribute::DOUBLE);
		}

		if(!IsSupportedCountDistinctItemType(reference.valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "IntervalCountDistinct requires scalar numeric, text, boolean, date, datetime, or time-of-day history.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(Attribute::DOUBLE);
		}

		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(reference.instanceId, reference.attributeId);
		std::vector<TemporalCoverageSegment> coverageSegments;
		std::vector<TemporalExtremaObservedSegment> observedSegments;
		std::unordered_set<std::string> distinctValues;
		bool sawOverlap = false;
		bool sawUnknown = false;
		bool sawUncertain = false;
		Attribute::AttributeType distinctType = Attribute::UNKNOWN;

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalExtremaCoverageSegment(*segmentPointer, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			const RuleSet::AttributeHistorySegment& segment = *coverage.source;

			sawOverlap = true;
			coverageSegments.push_back(coverage);

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			std::string normalizedValue;
			Attribute::AttributeType valueType = reference.valueType;

			if(!TryNormalizeHistoryValue(segment, reference.valueType, normalizedValue, valueType))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
			}

			if(!IsSupportedCountDistinctItemType(valueType))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_aggregate_type",
				              "IntervalCountDistinct requires scalar numeric, text, boolean, date, datetime, or time-of-day history.",
				              segment.instanceId + ":" + segment.attributeId);

				return UnknownValue(Attribute::DOUBLE);
			}

			if(distinctType == Attribute::UNKNOWN)
			{
				distinctType = NormalizeCountDistinctItemType(valueType);
			}
			else if(!AreCompatibleCountDistinctTypes(distinctType, valueType))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_aggregate_type",
				              "IntervalCountDistinct requires homogeneous scalar history values.",
				              segment.instanceId + ":" + segment.attributeId);

				return UnknownValue(Attribute::DOUBLE);
			}

			ScopedValue item;
			item.supported = true;
			item.known = true;
			item.value = normalizedValue;
			item.knowledgeState = segment.knowledgeState;
			item.valueType = valueType;

			std::string key;

			if(!TryResolveCountDistinctKey(item, key))
			{
				return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			for(const TemporalExtremaObservedSegment& observed : observedSegments)
			{
				if(TemporalExtremaObservedSegmentsOverlap(observed, coverage)
					&& observed.normalizedValue != key)
				{
					AddDiagnostic(evaluationContext,
					              "temporal_conflict",
					              "Attribute history has conflicting certain values over the requested interval.",
					              reference.instanceId + ":" + reference.attributeId + ":" + functionName);

					return UnknownValue(Attribute::DOUBLE);
				}
			}

			TemporalExtremaObservedSegment observed;
			observed.startSeconds = coverage.startSeconds;
			observed.endSeconds = coverage.endSeconds;
			observed.normalizedValue = key;
			observedSegments.push_back(observed);
			distinctValues.insert(key);
		}

		if(!sawOverlap || !HasCompleteCoverage(coverageSegments, intervalStart.seconds, intervalEnd.seconds) || sawUnknown)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};
		}

		return ResolvedValue{true, true, std::to_string(distinctValues.size()), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
	}

	bool TemporalConditionHasConflictInInterval(const std::vector<TemporalCoverageSegment>& conditionSegments)
	{
		for(size_t leftIndex = 0; leftIndex < conditionSegments.size(); ++leftIndex)
		{
			const TemporalCoverageSegment& left = conditionSegments[leftIndex];

			if(left.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				continue;
			}

			for(size_t rightIndex = leftIndex + 1; rightIndex < conditionSegments.size(); ++rightIndex)
			{
				const TemporalCoverageSegment& right = conditionSegments[rightIndex];

				if(right.knowledgeState == Attribute::KnowledgeState::Certain
					&& left.boolValue != right.boolValue
					&& TemporalCoverageSegmentsOverlap(left, right))
				{
					return true;
				}
			}
		}

		return false;
	}

	TemporalConditionSliceState BuildTemporalConditionSliceState(const std::vector<TemporalCoverageSegment>& conditionSegments,
	                                                             long long sliceStart,
	                                                             long long sliceEnd)
	{
		TemporalConditionSliceState state;

		for(const TemporalCoverageSegment& condition : conditionSegments)
		{
			if(!TemporalCoverageCoversSlice(condition, sliceStart, sliceEnd))
			{
				continue;
			}

			state.covered = true;

			if(condition.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				state.unknown = true;
			}
			else if(condition.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				state.uncertain = true;
			}
			else if(condition.boolValue)
			{
				state.certainTrue = true;
			}
			else
			{
				state.certainFalse = true;
			}
		}

		return state;
	}

	bool TemporalConditionSliceIsCertainlyFalse(const TemporalConditionSliceState& state)
	{
		return state.covered
			&& state.certainFalse
			&& !state.certainTrue
			&& !state.unknown
			&& !state.uncertain;
	}

	bool TryResolveTemporalCountDistinctSegmentKey(const RuleSet::AttributeHistorySegment& segment,
	                                              Attribute::AttributeType fallbackType,
	                                              Attribute::AttributeType& distinctType,
	                                              std::string& key,
	                                              EvaluationContext& evaluationContext)
	{
		std::string normalizedValue;
		Attribute::AttributeType valueType = fallbackType;

		if(!TryNormalizeHistoryValue(segment, fallbackType, normalizedValue, valueType))
		{
			TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);

			return false;
		}

		if(!IsSupportedCountDistinctItemType(valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "IntervalCountDistinctIf requires scalar numeric, text, boolean, date, datetime, or time-of-day history.",
			              segment.instanceId + ":" + segment.attributeId);

			return false;
		}

		if(distinctType == Attribute::UNKNOWN)
		{
			distinctType = NormalizeCountDistinctItemType(valueType);
		}
		else if(!AreCompatibleCountDistinctTypes(distinctType, valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "IntervalCountDistinctIf requires homogeneous scalar history values.",
			              segment.instanceId + ":" + segment.attributeId);

			return false;
		}

		ScopedValue item;
		item.supported = true;
		item.known = true;
		item.value = normalizedValue;
		item.knowledgeState = segment.knowledgeState;
		item.valueType = valueType;

		if(!TryResolveCountDistinctKey(item, key))
		{
			TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);

			return false;
		}

		return true;
	}

	bool TryBuildTemporalCountDistinctSliceState(const std::vector<TemporalCoverageSegment>& valueSegments,
	                                            const TemporalAttributeReference& valueReference,
	                                            const std::string& functionName,
	                                            long long sliceStart,
	                                            long long sliceEnd,
	                                            Attribute::AttributeType& distinctType,
	                                            TemporalCountDistinctSliceState& state,
	                                            EvaluationContext& evaluationContext)
	{
		for(const TemporalCoverageSegment& valueCoverage : valueSegments)
		{
			if(!TemporalCoverageCoversSlice(valueCoverage, sliceStart, sliceEnd) || valueCoverage.source == nullptr)
			{
				continue;
			}

			state.valueCovered = true;

			const RuleSet::AttributeHistorySegment& segment = *valueCoverage.source;

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				state.valueUnknown = true;

				continue;
			}

			std::string key;

			if(!TryResolveTemporalCountDistinctSegmentKey(segment, valueReference.valueType, distinctType, key, evaluationContext))
			{
				return false;
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				state.valueUncertain = true;

				continue;
			}

			if(state.hasCertainValue && state.certainKey != key)
			{
				AddDiagnostic(evaluationContext,
				              "temporal_conflict",
				              "Conditional interval count-distinct value history has conflicting certain values where the condition is not certainly false.",
				              valueReference.instanceId + ":" + valueReference.attributeId + ":" + functionName);

				return false;
			}

			state.hasCertainValue = true;
			state.certainKey = key;
		}

		return true;
	}

	ResolvedValue EvaluateTemporalConditionalCountDistinctFunction(const RuleSet& ruleSet,
	                                                               const std::string& functionName,
	                                                               const TemporalAttributeReference& valueReference,
	                                                               const TemporalAttributeReference& conditionReference,
	                                                               const TemporalInstant& intervalStart,
	                                                               const TemporalInstant& intervalEnd,
	                                                               EvaluationContext& evaluationContext)
	{
		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(Attribute::DOUBLE);
		}

		if(!IsSupportedCountDistinctItemType(valueReference.valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "IntervalCountDistinctIf requires scalar numeric, text, boolean, date, datetime, or time-of-day history.",
			              valueReference.instanceId + ":" + valueReference.attributeId);

			return UnknownValue(Attribute::DOUBLE);
		}

		if(conditionReference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "IntervalCountDistinctIf currently supports direct scalar boolean committed-history condition attributes only.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId);

			return UnknownValue(Attribute::DOUBLE);
		}

		std::vector<TemporalCoverageSegment> conditionSegments;

		if(!TryCollectTemporalConditionSegments(ruleSet, functionName, conditionReference, intervalStart, intervalEnd, evaluationContext, conditionSegments))
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		if(TemporalConditionHasConflictInInterval(conditionSegments))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_conflict",
			              "Conditional interval count-distinct condition history has conflicting certain values.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId + ":" + functionName);

			return UnknownValue(Attribute::DOUBLE);
		}

		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(valueReference.instanceId, valueReference.attributeId);
		std::vector<TemporalCoverageSegment> valueSegments;
		std::vector<long long> boundaries;

		boundaries.push_back(intervalStart.seconds);
		boundaries.push_back(intervalEnd.seconds);

		for(const TemporalCoverageSegment& condition : conditionSegments)
		{
			if(condition.startSeconds > intervalStart.seconds && condition.startSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(condition.startSeconds);
			}

			if(condition.endSeconds > intervalStart.seconds && condition.endSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(condition.endSeconds);
			}
		}

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalExtremaCoverageSegment(*segmentPointer, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			valueSegments.push_back(coverage);

			if(coverage.startSeconds > intervalStart.seconds && coverage.startSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(coverage.startSeconds);
			}

			if(coverage.endSeconds > intervalStart.seconds && coverage.endSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(coverage.endSeconds);
			}
		}

		std::sort(boundaries.begin(), boundaries.end());
		boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

		std::unordered_set<std::string> trueDistinctValues;
		Attribute::AttributeType distinctType = Attribute::UNKNOWN;
		bool sawUnknown = false;
		bool sawUncertain = false;

		for(size_t index = 1; index < boundaries.size(); ++index)
		{
			const long long sliceStart = boundaries[index - 1];
			const long long sliceEnd = boundaries[index];

			if(sliceEnd <= sliceStart)
			{
				continue;
			}

			const TemporalConditionSliceState condition = BuildTemporalConditionSliceState(conditionSegments, sliceStart, sliceEnd);

			if(!condition.certainTrue)
			{
				continue;
			}

			TemporalCountDistinctSliceState valueState;

			if(!TryBuildTemporalCountDistinctSliceState(valueSegments, valueReference, functionName, sliceStart, sliceEnd, distinctType, valueState, evaluationContext))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(!valueState.valueCovered || valueState.valueUnknown)
			{
				sawUnknown = true;
			}

			if(valueState.valueUncertain)
			{
				sawUncertain = true;
			}

			if(valueState.hasCertainValue)
			{
				trueDistinctValues.insert(valueState.certainKey);
			}
		}

		for(size_t index = 1; index < boundaries.size(); ++index)
		{
			const long long sliceStart = boundaries[index - 1];
			const long long sliceEnd = boundaries[index];

			if(sliceEnd <= sliceStart)
			{
				continue;
			}

			const TemporalConditionSliceState condition = BuildTemporalConditionSliceState(conditionSegments, sliceStart, sliceEnd);

			if(condition.certainTrue || TemporalConditionSliceIsCertainlyFalse(condition))
			{
				continue;
			}

			TemporalCountDistinctSliceState valueState;

			if(!TryBuildTemporalCountDistinctSliceState(valueSegments, valueReference, functionName, sliceStart, sliceEnd, distinctType, valueState, evaluationContext))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			const bool couldChangeWithCertainValue = valueState.hasCertainValue
				&& trueDistinctValues.find(valueState.certainKey) == trueDistinctValues.end();
			const bool valueCouldBeUnseen = !valueState.valueCovered || valueState.valueUnknown || valueState.valueUncertain || !valueState.hasCertainValue;
			const bool conditionUnknown = !condition.covered || condition.unknown;

			if(conditionUnknown)
			{
				if(couldChangeWithCertainValue || valueCouldBeUnseen)
				{
					sawUnknown = true;
				}
			}
			else if(condition.uncertain)
			{
				if(couldChangeWithCertainValue || valueCouldBeUnseen)
				{
					sawUncertain = true;
				}
			}
			else
			{
				sawUnknown = true;
			}
		}

		if(sawUnknown)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};
		}

		return ResolvedValue{true, true, std::to_string(trueDistinctValues.size()), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
	}

	ResolvedValue EvaluateTemporalConditionalWeightedAggregateFunction(const RuleSet& ruleSet,
	                                                                   const std::string& functionName,
	                                                                   const TemporalAttributeReference& valueReference,
	                                                                   const TemporalAttributeReference& conditionReference,
	                                                                   const TemporalInstant& intervalStart,
	                                                                   const TemporalInstant& intervalEnd,
	                                                                   EvaluationContext& evaluationContext)
	{
		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(Attribute::DOUBLE);
		}

		if(!IsNumericAttributeType(valueReference.valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "Conditional daily/time-weighted interval aggregates require Number or Currency scalar history.",
			              valueReference.instanceId + ":" + valueReference.attributeId);

			return UnknownValue(Attribute::DOUBLE);
		}

		if(conditionReference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "Conditional daily/time-weighted interval aggregates currently support direct scalar boolean committed-history condition attributes only.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId);

			return UnknownValue(Attribute::DOUBLE);
		}

		std::vector<TemporalCoverageSegment> conditionSegments;

		if(!TryCollectTemporalConditionSegments(ruleSet, functionName, conditionReference, intervalStart, intervalEnd, evaluationContext, conditionSegments))
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(valueReference.instanceId, valueReference.attributeId);
		std::vector<TemporalCoverageSegment> valueSegments;
		std::vector<long long> boundaries;

		boundaries.push_back(intervalStart.seconds);
		boundaries.push_back(intervalEnd.seconds);

		for(long long dayBoundary = LocalDayStartSeconds(intervalStart.seconds) + 86400LL;
			dayBoundary < intervalEnd.seconds;
			dayBoundary += 86400LL)
		{
			if(dayBoundary > intervalStart.seconds)
			{
				boundaries.push_back(dayBoundary);
			}
		}

		for(const TemporalCoverageSegment& condition : conditionSegments)
		{
			if(condition.startSeconds > intervalStart.seconds && condition.startSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(condition.startSeconds);
			}

			if(condition.endSeconds > intervalStart.seconds && condition.endSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(condition.endSeconds);
			}
		}

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalExtremaCoverageSegment(*segmentPointer, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			valueSegments.push_back(coverage);

			if(coverage.startSeconds > intervalStart.seconds && coverage.startSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(coverage.startSeconds);
			}

			if(coverage.endSeconds > intervalStart.seconds && coverage.endSeconds < intervalEnd.seconds)
			{
				boundaries.push_back(coverage.endSeconds);
			}
		}

		std::sort(boundaries.begin(), boundaries.end());
		boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawTrueDuration = false;
		double weightedSum = 0.0;
		double trueWeight = 0.0;

		for(size_t index = 1; index < boundaries.size(); ++index)
		{
			const long long sliceStart = boundaries[index - 1];
			const long long sliceEnd = boundaries[index];

			if(sliceEnd <= sliceStart)
			{
				continue;
			}

			bool conditionCovered = false;
			bool conditionCertainTrue = false;
			bool conditionCertainFalse = false;
			bool conditionUnknown = false;
			bool conditionUncertain = false;

			for(const TemporalCoverageSegment& condition : conditionSegments)
			{
				if(!TemporalCoverageCoversSlice(condition, sliceStart, sliceEnd))
				{
					continue;
				}

				conditionCovered = true;

				if(condition.knowledgeState == Attribute::KnowledgeState::Unknown)
				{
					conditionUnknown = true;
				}
				else if(condition.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					conditionUncertain = true;
				}
				else if(condition.boolValue)
				{
					conditionCertainTrue = true;
				}
				else
				{
					conditionCertainFalse = true;
				}
			}

			if(conditionCertainTrue && conditionCertainFalse)
			{
				AddDiagnostic(evaluationContext,
				              "temporal_conflict",
				              "Conditional daily/time-weighted interval condition history has conflicting certain values.",
				              conditionReference.instanceId + ":" + conditionReference.attributeId + ":" + functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			const bool certainlyFalse = conditionCovered
				&& !conditionUnknown
				&& !conditionUncertain
				&& conditionCertainFalse
				&& !conditionCertainTrue;

			if(!certainlyFalse)
			{
				bool hasCertainValue = false;
				std::string selectedValue;
				double selectedNumericValue = 0.0;

				for(const TemporalCoverageSegment& valueCoverage : valueSegments)
				{
					if(!TemporalCoverageCoversSlice(valueCoverage, sliceStart, sliceEnd) || valueCoverage.source == nullptr)
					{
						continue;
					}

					const RuleSet::AttributeHistorySegment& segment = *valueCoverage.source;

					if(segment.knowledgeState != Attribute::KnowledgeState::Certain)
					{
						continue;
					}

					std::string normalizedValue;
					Attribute::AttributeType valueType = valueReference.valueType;

					if(!TryNormalizeHistoryValue(segment, valueReference.valueType, normalizedValue, valueType))
					{
						return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
					}

					if(!IsNumericAttributeType(valueType))
					{
						AddDiagnostic(evaluationContext,
						              "temporal_aggregate_type",
						              "Conditional daily/time-weighted interval aggregates require homogeneous Number or Currency scalar history.",
						              segment.instanceId + ":" + segment.attributeId);

						return UnknownValue(Attribute::DOUBLE);
					}

					double numericValue = 0.0;
					long long temporalValue = 0;
					std::string comparableValue;

					if(!TryResolveAggregateComparableValue(normalizedValue, valueType, numericValue, temporalValue, comparableValue))
					{
						return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
					}

					if(hasCertainValue && selectedValue != comparableValue)
					{
						AddDiagnostic(evaluationContext,
						              "temporal_conflict",
						              "Conditional daily/time-weighted interval value history has conflicting certain values where the condition is not certainly false.",
						              valueReference.instanceId + ":" + valueReference.attributeId + ":" + functionName);

						return UnknownValue(Attribute::DOUBLE);
					}

					hasCertainValue = true;
					selectedValue = comparableValue;
					selectedNumericValue = numericValue;
				}
			}

			if(!conditionCovered || conditionUnknown)
			{
				sawUnknown = true;

				continue;
			}

			if(conditionUncertain)
			{
				sawUncertain = true;

				continue;
			}

			if(conditionCertainFalse)
			{
				continue;
			}

			if(!conditionCertainTrue)
			{
				sawUnknown = true;

				continue;
			}

			sawTrueDuration = true;
			bool valueCovered = false;
			bool valueUnknown = false;
			bool valueUncertain = false;
			bool hasCertainValue = false;
			double selectedNumericValue = 0.0;
			std::string selectedValue;

			for(const TemporalCoverageSegment& valueCoverage : valueSegments)
			{
				if(!TemporalCoverageCoversSlice(valueCoverage, sliceStart, sliceEnd) || valueCoverage.source == nullptr)
				{
					continue;
				}

				valueCovered = true;

				const RuleSet::AttributeHistorySegment& segment = *valueCoverage.source;

				if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
				{
					valueUnknown = true;

					continue;
				}

				if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					valueUncertain = true;

					continue;
				}

				std::string normalizedValue;
				Attribute::AttributeType valueType = valueReference.valueType;

				if(!TryNormalizeHistoryValue(segment, valueReference.valueType, normalizedValue, valueType))
				{
					return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
				}

				if(!IsNumericAttributeType(valueType))
				{
					AddDiagnostic(evaluationContext,
					              "temporal_aggregate_type",
					              "Conditional daily/time-weighted interval aggregates require homogeneous Number or Currency scalar history.",
					              segment.instanceId + ":" + segment.attributeId);

					return UnknownValue(Attribute::DOUBLE);
				}

				double numericValue = 0.0;
				long long temporalValue = 0;
				std::string comparableValue;

				if(!TryResolveAggregateComparableValue(normalizedValue, valueType, numericValue, temporalValue, comparableValue))
				{
					return TemporalHistoryTypeConversionFailure(segment, evaluationContext, Attribute::DOUBLE);
				}

				if(hasCertainValue && selectedValue != comparableValue)
				{
					AddDiagnostic(evaluationContext,
					              "temporal_conflict",
					              "Conditional daily/time-weighted interval value history has conflicting certain values under a true condition.",
					              valueReference.instanceId + ":" + valueReference.attributeId + ":" + functionName);

					return UnknownValue(Attribute::DOUBLE);
				}

				hasCertainValue = true;
				selectedValue = comparableValue;
				selectedNumericValue = numericValue;
			}

			if(!valueCovered || valueUnknown)
			{
				sawUnknown = true;

				continue;
			}

			if(valueUncertain)
			{
				sawUncertain = true;
			}

			if(!hasCertainValue)
			{
				if(!valueUncertain)
				{
					sawUnknown = true;
				}

				continue;
			}

			const double weight = TemporalDayFraction(sliceStart, sliceEnd);

			weightedSum += selectedNumericValue * weight;
			trueWeight += weight;
		}

		if(sawUnknown)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		if(sawUncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};
		}

		if(!sawTrueDuration || trueWeight <= 0.0)
		{
			if(functionName == "IntervalDailySumIf")
			{
				return ResolvedValue{true, true, FormatDoubleValue(0.0), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
			}

			return UnknownValue(Attribute::DOUBLE);
		}

		const double result = functionName == "IntervalWeightedAverageIf"
			? weightedSum / trueWeight
			: weightedSum;

		return ResolvedValue{true, true, FormatDoubleValue(result), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
	}

	bool TryBuildTemporalCoverageSegment(const RuleSet::AttributeHistorySegment& segment,
	                                     const TemporalAttributeReference& reference,
	                                     const TemporalInstant& intervalStart,
	                                     const TemporalInstant& intervalEnd,
	                                     TemporalCoverageSegment& coverage,
	                                     EvaluationContext& evaluationContext)
	{
		bool startOpen = false;
		bool endOpen = false;
		TemporalInstant segmentStart;
		TemporalInstant segmentEnd;

		if(!TryParseSegmentBoundary(segment, segment.effectiveStart, "effectiveStart", startOpen, segmentStart, evaluationContext)
			|| !TryParseSegmentBoundary(segment, segment.effectiveEnd, "effectiveEnd", endOpen, segmentEnd, evaluationContext))
		{
			return false;
		}

		if(!startOpen && !endOpen && segmentEnd.seconds <= segmentStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_boundary",
			              "Attribute history segment has an invalid effective-time interval.",
			              segment.instanceId + ":" + segment.attributeId + ":" + segment.effectiveStart + ":" + segment.effectiveEnd);

			return false;
		}

		const long long rawStart = startOpen ? intervalStart.seconds : segmentStart.seconds;
		const long long rawEnd = endOpen ? intervalEnd.seconds : segmentEnd.seconds;

		if(rawEnd <= intervalStart.seconds || rawStart >= intervalEnd.seconds)
		{
			coverage.source = nullptr;

			return true;
		}

		coverage.startSeconds = rawStart > intervalStart.seconds ? rawStart : intervalStart.seconds;
		coverage.endSeconds = rawEnd < intervalEnd.seconds ? rawEnd : intervalEnd.seconds;
		coverage.knowledgeState = segment.knowledgeState;
		coverage.source = &segment;

		if(coverage.endSeconds <= coverage.startSeconds)
		{
			coverage.source = nullptr;

			return true;
		}

		if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			return true;
		}

		const Attribute::AttributeType effectiveType = EffectiveHistoryValueType(segment, reference.valueType);

		if(effectiveType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_condition_type",
			              "Interval condition history must be boolean.",
			              segment.instanceId + ":" + segment.attributeId);

			return false;
		}

		std::string normalizedValue;
		Attribute::AttributeType valueType = Attribute::BOOLEAN;

		if(!TryNormalizeHistoryValue(segment, Attribute::BOOLEAN, normalizedValue, valueType))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_type_conversion",
			              "Attribute history segment value is not type-correct.",
			              segment.instanceId + ":" + segment.attributeId + ":" + segment.value);

			return false;
		}

		bool boolValue = false;

		if(!TryParseBoolean(normalizedValue, boolValue))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_history_type_conversion",
			              "Attribute history segment value is not type-correct.",
			              segment.instanceId + ":" + segment.attributeId + ":" + segment.value);

			return false;
		}

		coverage.boolValue = boolValue;

		return true;
	}

	bool TryCollectTemporalCoverageSummary(const RuleSet& ruleSet,
	                                       const std::string& functionName,
	                                       const TemporalAttributeReference& reference,
	                                       const TemporalInstant& intervalStart,
	                                       const TemporalInstant& intervalEnd,
	                                       EvaluationContext& evaluationContext,
	                                       TemporalCoverageSummary& summary)
	{
		std::vector<const RuleSet::AttributeHistorySegment*> historySegments = ruleSet.GetAttributeHistorySegments(reference.instanceId, reference.attributeId);

		for(const RuleSet::AttributeHistorySegment* segmentPointer : historySegments)
		{
			if(segmentPointer == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildTemporalCoverageSegment(*segmentPointer, reference, intervalStart, intervalEnd, coverage, evaluationContext))
			{
				return false;
			}

			if(coverage.source == nullptr)
			{
				continue;
			}

			for(const TemporalCoverageSegment& existing : summary.coverageSegments)
			{
				if(existing.knowledgeState == Attribute::KnowledgeState::Certain
					&& coverage.knowledgeState == Attribute::KnowledgeState::Certain
					&& existing.boolValue != coverage.boolValue
					&& TemporalCoverageSegmentsOverlap(existing, coverage))
				{
					AddDiagnostic(evaluationContext,
					              "temporal_conflict",
					              "Attribute history has conflicting certain values over the requested interval.",
					              reference.instanceId + ":" + reference.attributeId + ":" + functionName);

					return false;
				}
			}

			summary.coverageSegments.push_back(coverage);

			if(coverage.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				summary.sawUnknownSegment = true;
			}
			else if(coverage.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				summary.sawUncertainSegment = true;
			}
			else if(coverage.boolValue)
			{
				summary.certainTrueSegments.push_back(coverage);
			}
			else
			{
				summary.certainFalseSegments.push_back(coverage);
			}
		}

		return true;
	}

	ResolvedValue EvaluateTemporalIntervalPredicate(const RuleSet& ruleSet,
	                                                const std::string& functionName,
	                                                const TemporalAttributeReference& reference,
	                                                const TemporalInstant& intervalStart,
	                                                const TemporalInstant& intervalEnd,
	                                                EvaluationContext& evaluationContext)
	{
		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(reference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_condition_type",
			              "Interval condition timeline must reference a boolean attribute.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		TemporalCoverageSummary summary;

		if(!TryCollectTemporalCoverageSummary(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext, summary))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		const bool completeCoverage = HasCompleteCoverage(summary.coverageSegments, intervalStart.seconds, intervalEnd.seconds);
		const bool hasGap = !completeCoverage;

		if(functionName == "IntervalAlways")
		{
			if(!summary.certainFalseSegments.empty())
			{
				return BooleanValue(false);
			}

			if(hasGap || summary.sawUnknownSegment)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			if(summary.sawUncertainSegment)
			{
				return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
			}

			return HasCompleteCoverage(summary.certainTrueSegments, intervalStart.seconds, intervalEnd.seconds)
				? BooleanValue(true)
				: UnknownValue(Attribute::BOOLEAN);
		}

		if(!summary.certainTrueSegments.empty())
		{
			return BooleanValue(true);
		}

		if(hasGap || summary.sawUnknownSegment)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(summary.sawUncertainSegment)
		{
			return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
		}

		return HasCompleteCoverage(summary.certainFalseSegments, intervalStart.seconds, intervalEnd.seconds)
			? BooleanValue(false)
			: UnknownValue(Attribute::BOOLEAN);
	}

	ResolvedValue EvaluateTemporalDayCountPredicate(const RuleSet& ruleSet,
	                                                const std::string& functionName,
	                                                const TemporalAttributeReference& reference,
	                                                const TemporalInstant& intervalStart,
	                                                const TemporalInstant& intervalEnd,
	                                                int requiredDays,
	                                                EvaluationContext& evaluationContext)
	{
		if(intervalEnd.seconds <= intervalStart.seconds)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Temporal interval end must be after start.",
			              functionName + ":" + FormatTemporalInstant(intervalStart.seconds, intervalStart.hasTime) + ":" + FormatTemporalInstant(intervalEnd.seconds, intervalEnd.hasTime));

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(intervalStart.hasTime || intervalEnd.hasTime)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_interval_boundary",
			              "Day-count temporal intervals require date-only start and end arguments.",
			              functionName);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(requiredDays < 0)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_expression_value",
			              "Day-count temporal functions require a non-negative integer number of days.",
			              functionName);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(reference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_condition_type",
			              "Day-count temporal condition timeline must reference a boolean attribute.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(requiredDays == 0)
		{
			return BooleanValue(true);
		}

		const long long totalDays = (intervalEnd.seconds - intervalStart.seconds) / 86400LL;

		if(totalDays < requiredDays)
		{
			return BooleanValue(false);
		}

		TemporalCoverageSummary summary;

		if(!TryCollectTemporalCoverageSummary(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext, summary))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		std::vector<TemporalDayState> states;
		states.reserve(static_cast<size_t>(totalDays));

		int certainTrueDays = 0;
		int unknownDays = 0;
		int uncertainDays = 0;
		int maxConsecutiveCertainTrue = 0;
		int currentConsecutiveCertainTrue = 0;

		for(long long index = 0; index < totalDays; ++index)
		{
			const TemporalDayState state = ClassifyTemporalDay(summary, intervalStart.seconds + index * 86400LL);

			states.push_back(state);

			if(state == TemporalDayState::CertainTrue)
			{
				++certainTrueDays;
				++currentConsecutiveCertainTrue;

				if(currentConsecutiveCertainTrue > maxConsecutiveCertainTrue)
				{
					maxConsecutiveCertainTrue = currentConsecutiveCertainTrue;
				}
			}
			else
			{
				currentConsecutiveCertainTrue = 0;

				if(state == TemporalDayState::Unknown)
				{
					++unknownDays;
				}
				else if(state == TemporalDayState::Uncertain)
				{
					++uncertainDays;
				}
			}
		}

		if(functionName == "IntervalAtLeastDays")
		{
			if(certainTrueDays >= requiredDays)
			{
				return BooleanValue(true);
			}

			if(certainTrueDays + unknownDays + uncertainDays < requiredDays)
			{
				return BooleanValue(false);
			}

			if(unknownDays > 0)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			if(uncertainDays > 0)
			{
				return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
			}

			return BooleanValue(false);
		}

		if(maxConsecutiveCertainTrue >= requiredDays)
		{
			return BooleanValue(true);
		}

		int currentPossibleRun = 0;
		int maxPossibleRun = 0;

		for(TemporalDayState state : states)
		{
			if(state == TemporalDayState::CertainFalse)
			{
				currentPossibleRun = 0;
			}
			else
			{
				++currentPossibleRun;

				if(currentPossibleRun > maxPossibleRun)
				{
					maxPossibleRun = currentPossibleRun;
				}
			}
		}

		if(maxPossibleRun < requiredDays)
		{
			return BooleanValue(false);
		}

		bool sawUnknownCandidate = false;
		bool sawUncertainCandidate = false;

		for(size_t start = 0; start + static_cast<size_t>(requiredDays) <= states.size(); ++start)
		{
			bool allPossible = true;
			bool windowHasUnknown = false;
			bool windowHasUncertain = false;

			for(size_t offset = 0; offset < static_cast<size_t>(requiredDays); ++offset)
			{
				const TemporalDayState state = states[start + offset];

				if(state == TemporalDayState::CertainFalse)
				{
					allPossible = false;

					break;
				}

				if(state == TemporalDayState::Unknown)
				{
					windowHasUnknown = true;
				}
				else if(state == TemporalDayState::Uncertain)
				{
					windowHasUncertain = true;
				}
			}

			if(!allPossible)
			{
				continue;
			}

			sawUnknownCandidate = sawUnknownCandidate || windowHasUnknown;
			sawUncertainCandidate = sawUncertainCandidate || windowHasUncertain;
		}

		if(sawUnknownCandidate)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertainCandidate)
		{
			return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(false);
	}

	ResolvedValue EvaluateTemporalRelativeWindowPredicate(const RuleSet& ruleSet,
	                                                      const std::string& functionName,
	                                                      const TemporalAttributeReference& reference,
	                                                      int days,
	                                                      const TemporalInstant* explicitAnchor,
	                                                      EvaluationContext& evaluationContext)
	{
		if(days < 1)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_days_must_be_positive",
			              "Relative-window temporal functions require a positive integer number of days.",
			              functionName);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(reference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "Relative-window temporal functions currently support scalar boolean committed-history attributes only.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		TemporalInstant anchor;

		if(explicitAnchor != nullptr)
		{
			anchor = *explicitAnchor;
		}
		else
		{
			const std::string anchorValue = evaluationContext.ruleSet != nullptr
				? evaluationContext.ruleSet->GetSessionCurrentDate()
				: ruleSet.GetSessionCurrentDate();

			if(!TryParseTemporalInstant(anchorValue, anchor))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_anchor_date",
				              "Relative-window temporal functions require a valid evaluation current date.",
				              functionName + ":" + anchorValue);

				return UnknownValue(Attribute::BOOLEAN);
			}
		}

		const long long anchorDayStart = LocalDayStartSeconds(anchor.seconds);
		TemporalInstant intervalStart;
		TemporalInstant intervalEnd;

		intervalStart.parsed = true;
		intervalStart.seconds = anchorDayStart - static_cast<long long>(days - 1) * 86400LL;
		intervalStart.hasTime = false;
		intervalEnd.parsed = true;
		intervalEnd.seconds = anchorDayStart + 86400LL;
		intervalEnd.hasTime = false;

		TemporalCoverageSummary summary;

		if(!TryCollectTemporalCoverageSummary(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext, summary))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		bool allTrue = true;
		bool anyTrue = false;
		bool allFalse = true;
		bool anyFalse = false;
		bool anyUnknown = false;
		bool anyUncertain = false;

		for(int index = 0; index < days; ++index)
		{
			const TemporalDayState state = ClassifyTemporalDay(summary, intervalStart.seconds + static_cast<long long>(index) * 86400LL);

			if(state == TemporalDayState::CertainTrue)
			{
				anyTrue = true;
				allFalse = false;
			}
			else if(state == TemporalDayState::CertainFalse)
			{
				anyFalse = true;
				allTrue = false;
			}
			else if(state == TemporalDayState::Unknown)
			{
				anyUnknown = true;
				allTrue = false;
				allFalse = false;
			}
			else
			{
				anyUncertain = true;
				allTrue = false;
				allFalse = false;
			}
		}

		if(functionName == "TemporalSometimesDays")
		{
			if(anyTrue)
			{
				return BooleanValue(true);
			}

			if(allFalse)
			{
				return BooleanValue(false);
			}

			if(anyUnknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			if(anyUncertain)
			{
				return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
			}

			return BooleanValue(false);
		}

		if(anyFalse)
		{
			return BooleanValue(false);
		}

		if(allTrue)
		{
			return BooleanValue(true);
		}

		if(anyUnknown)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(anyUncertain)
		{
			return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
		}

		return UnknownValue(Attribute::BOOLEAN);
	}

	bool TrySelectTemporalIntervalShape(const std::shared_ptr<Instance>& context,
	                                    const std::vector<std::string>& arguments,
	                                    bool hasDays,
	                                    TemporalIntervalCallShape& shape,
	                                    TemporalAttributeReference& reference)
	{
		const size_t exportedConditionIndex = hasDays ? 3 : 2;
		const size_t compatibilityConditionIndex = 0;

		TemporalAttributeReference exportedReference;
		const bool exportedResolved = ResolveTemporalAttributeArgument(context, arguments[exportedConditionIndex], exportedReference);

		TemporalAttributeReference compatibilityReference;
		const bool compatibilityResolved = ResolveTemporalAttributeArgument(context, arguments[compatibilityConditionIndex], compatibilityReference);

		if(exportedResolved && exportedReference.valueType == Attribute::BOOLEAN)
		{
			shape.conditionIndex = exportedConditionIndex;
			shape.startIndex = 0;
			shape.endIndex = 1;
			shape.daysIndex = hasDays ? 2 : 0;
			shape.hasDays = hasDays;
			reference = exportedReference;

			return true;
		}

		if(compatibilityResolved && compatibilityReference.valueType == Attribute::BOOLEAN)
		{
			shape.conditionIndex = compatibilityConditionIndex;
			shape.startIndex = 1;
			shape.endIndex = 2;
			shape.daysIndex = hasDays ? 3 : 0;
			shape.hasDays = hasDays;
			reference = compatibilityReference;

			return true;
		}

		if(compatibilityResolved)
		{
			shape.conditionIndex = compatibilityConditionIndex;
			shape.startIndex = 1;
			shape.endIndex = 2;
			shape.daysIndex = hasDays ? 3 : 0;
			shape.hasDays = hasDays;
			reference = compatibilityReference;

			return true;
		}

		if(exportedResolved)
		{
			shape.conditionIndex = exportedConditionIndex;
			shape.startIndex = 0;
			shape.endIndex = 1;
			shape.daysIndex = hasDays ? 2 : 0;
			shape.hasDays = hasDays;
			reference = exportedReference;

			return true;
		}

		return false;
	}

	bool TrySelectTemporalIntervalShape(const std::shared_ptr<Instance>& context,
	                                    const json& arguments,
	                                    bool hasDays,
	                                    TemporalIntervalCallShape& shape,
	                                    TemporalAttributeReference& reference)
	{
		const size_t exportedConditionIndex = hasDays ? 3 : 2;
		const size_t compatibilityConditionIndex = 0;

		TemporalAttributeReference exportedReference;
		const bool exportedResolved = ResolveTemporalAttributeArgumentNode(context, arguments.at(exportedConditionIndex), exportedReference);

		TemporalAttributeReference compatibilityReference;
		const bool compatibilityResolved = ResolveTemporalAttributeArgumentNode(context, arguments.at(compatibilityConditionIndex), compatibilityReference);

		if(exportedResolved && exportedReference.valueType == Attribute::BOOLEAN)
		{
			shape.conditionIndex = exportedConditionIndex;
			shape.startIndex = 0;
			shape.endIndex = 1;
			shape.daysIndex = hasDays ? 2 : 0;
			shape.hasDays = hasDays;
			reference = exportedReference;

			return true;
		}

		if(compatibilityResolved && compatibilityReference.valueType == Attribute::BOOLEAN)
		{
			shape.conditionIndex = compatibilityConditionIndex;
			shape.startIndex = 1;
			shape.endIndex = 2;
			shape.daysIndex = hasDays ? 3 : 0;
			shape.hasDays = hasDays;
			reference = compatibilityReference;

			return true;
		}

		if(compatibilityResolved)
		{
			shape.conditionIndex = compatibilityConditionIndex;
			shape.startIndex = 1;
			shape.endIndex = 2;
			shape.daysIndex = hasDays ? 3 : 0;
			shape.hasDays = hasDays;
			reference = compatibilityReference;

			return true;
		}

		if(exportedResolved)
		{
			shape.conditionIndex = exportedConditionIndex;
			shape.startIndex = 0;
			shape.endIndex = 1;
			shape.daysIndex = hasDays ? 2 : 0;
			shape.hasDays = hasDays;
			reference = exportedReference;

			return true;
		}

		return false;
	}

	ResolvedValue EvaluateTemporalDaysSinceFunction(const std::string& functionName,
	                                                const TemporalInstant& start,
	                                                const TemporalInstant& end,
	                                                Attribute::KnowledgeState knowledgeState,
	                                                EvaluationContext& evaluationContext)
	{
		if(start.hasTime || end.hasTime)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_expression_value",
			              "TemporalDaysSince requires date-only start and end arguments.",
			              functionName);

			return UnknownValue(Attribute::DOUBLE);
		}

		const long long signedDays = (end.seconds - start.seconds) / 86400LL;
		const long long days = signedDays < 0 ? -signedDays : signedDays;

		return ResolvedValue{true, true, std::to_string(days), knowledgeState, Attribute::DOUBLE};
	}

	DateTimeParts DatePartsFromTemporalInstant(const TemporalInstant& instant)
	{
		DateTimeParts parts = PartsFromSeconds(instant.seconds);

		parts.hour = 0;
		parts.minute = 0;
		parts.second = 0;
		parts.hasDate = true;
		parts.hasTime = false;

		return parts;
	}

	ResolvedValue EvaluateTemporalElapsedDateUnitFunction(const std::string& functionName,
	                                                      const TemporalInstant& start,
	                                                      const TemporalInstant& end,
	                                                      Attribute::KnowledgeState knowledgeState)
	{
		const long long signedDays = (end.seconds - start.seconds) / 86400LL;
		const long long days = signedDays < 0 ? -signedDays : signedDays;
		long long result = 0;

		if(functionName == "TemporalWeeksSince")
		{
			result = days / 7LL;
		}
		else if(functionName == "TemporalMonthsSince")
		{
			result = WholeMonthDifference(DatePartsFromTemporalInstant(start), DatePartsFromTemporalInstant(end));
		}
		else if(functionName == "TemporalYearsSince")
		{
			result = WholeYearDifference(DatePartsFromTemporalInstant(start), DatePartsFromTemporalInstant(end));
		}
		else
		{
			return ResolvedValue{false, false, ""};
		}

		return ResolvedValue{true, true, std::to_string(result), knowledgeState, Attribute::DOUBLE};
	}

	ResolvedValue EvaluateTemporalIsWeekdayFunction(const TemporalInstant& date,
	                                                Attribute::KnowledgeState knowledgeState)
	{
		const long long days = date.seconds / 86400LL;
		const int dayOfWeek = DayOfWeekMondayZero(days);

		return BooleanValue(dayOfWeek >= 0 && dayOfWeek < 5, knowledgeState);
	}

	ResolvedValue EvaluateTemporalCurrentDateBoundaryPredicate(const RuleSet& ruleSet,
	                                                           const std::string& functionName,
	                                                           const TemporalInstant& endpoint,
	                                                           Attribute::KnowledgeState knowledgeState)
	{
		TemporalInstant currentDate;

		if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), currentDate) || currentDate.hasTime)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(functionName == "TemporalFromStartDate")
		{
			return BooleanValue(currentDate.seconds >= endpoint.seconds, knowledgeState);
		}

		if(functionName == "TemporalFromEndDate")
		{
			return BooleanValue(currentDate.seconds <= endpoint.seconds, knowledgeState);
		}

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateTemporalCurrentDateRangePredicate(const RuleSet& ruleSet,
	                                                        const TemporalInstant& startDate,
	                                                        const TemporalInstant& endDate,
	                                                        Attribute::KnowledgeState knowledgeState)
	{
		TemporalInstant currentDate;

		if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), currentDate) || currentDate.hasTime)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		return BooleanValue(currentDate.seconds >= startDate.seconds
			&& currentDate.seconds <= endDate.seconds,
			knowledgeState);
	}

	ResolvedValue EvaluateTemporalCurrentDateComparisonPredicate(const RuleSet& ruleSet,
	                                                             const std::string& functionName,
	                                                             const TemporalInstant& date,
	                                                             Attribute::KnowledgeState knowledgeState)
	{
		TemporalInstant currentDate;

		if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), currentDate) || currentDate.hasTime)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(functionName == "TemporalOn")
		{
			return BooleanValue(currentDate.seconds == date.seconds, knowledgeState);
		}

		if(functionName == "TemporalBefore")
		{
			return BooleanValue(currentDate.seconds < date.seconds, knowledgeState);
		}

		if(functionName == "TemporalAfter")
		{
			return BooleanValue(currentDate.seconds > date.seconds, knowledgeState);
		}

		if(functionName == "TemporalOnOrBefore")
		{
			return BooleanValue(currentDate.seconds <= date.seconds, knowledgeState);
		}

		if(functionName == "TemporalOnOrAfter")
		{
			return BooleanValue(currentDate.seconds >= date.seconds, knowledgeState);
		}

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateTemporalOncePerMonthPredicate(const RuleSet& ruleSet,
	                                                    const TemporalInstant& anchorDate,
	                                                    Attribute::KnowledgeState knowledgeState)
	{
		TemporalInstant currentDate;

		if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), currentDate) || currentDate.hasTime)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		const DateTimeParts currentParts = PartsFromSeconds(currentDate.seconds);
		const DateTimeParts anchorParts = PartsFromSeconds(anchorDate.seconds);
		const int daysInCurrentMonth = DaysInMonth(currentParts.year, currentParts.month);
		const int recurrenceDay = anchorParts.day < daysInCurrentMonth ? anchorParts.day : daysInCurrentMonth;

		return BooleanValue(currentParts.day == recurrenceDay, knowledgeState);
	}

	bool TryResolveTemporalDayOfMonthValue(const ResolvedValue& value,
	                                       const std::string& functionName,
	                                       EvaluationContext& evaluationContext,
	                                       int& dayOfMonth,
	                                       Attribute::KnowledgeState& knowledgeState,
	                                       ResolvedValue& failureValue)
	{
		if(!value.supported)
		{
			failureValue = value;

			return false;
		}

		knowledgeState = value.knowledgeState;

		if(!value.known || value.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			failureValue = UnknownValue(Attribute::BOOLEAN);

			return false;
		}

		if(!IsNumericAttributeType(value.valueType) || !TryParseIntegerValue(value.value, dayOfMonth))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "TemporalOncePerMonth day-of-month argument must be an integer Number.",
			              functionName);

			failureValue = UnknownValue(Attribute::BOOLEAN);

			return false;
		}

		if(dayOfMonth < 1 || dayOfMonth > 31)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_day_of_month_range",
			              "TemporalOncePerMonth day-of-month argument must be in the range 1..31.",
			              functionName + ":" + value.value);

			failureValue = UnknownValue(Attribute::BOOLEAN);

			return false;
		}

		return true;
	}

	ResolvedValue EvaluateTemporalOncePerMonthRangePredicate(const RuleSet& ruleSet,
	                                                         const TemporalInstant& startDate,
	                                                         const TemporalInstant& endDate,
	                                                         int dayOfMonth,
	                                                         Attribute::KnowledgeState knowledgeState)
	{
		TemporalInstant currentDate;

		if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), currentDate) || currentDate.hasTime)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(startDate.seconds > endDate.seconds
			|| currentDate.seconds < startDate.seconds
			|| currentDate.seconds > endDate.seconds)
		{
			return BooleanValue(false, knowledgeState);
		}

		const DateTimeParts currentParts = PartsFromSeconds(currentDate.seconds);
		const int daysInCurrentMonth = DaysInMonth(currentParts.year, currentParts.month);
		const int recurrenceDay = dayOfMonth < daysInCurrentMonth ? dayOfMonth : daysInCurrentMonth;

		return BooleanValue(currentParts.day == recurrenceDay, knowledgeState);
	}

	Attribute::KnowledgeState CombineTemporalArgumentKnowledge(std::initializer_list<Attribute::KnowledgeState> states)
	{
		for(Attribute::KnowledgeState state : states)
		{
			if(state == Attribute::KnowledgeState::Unknown)
			{
				return Attribute::KnowledgeState::Unknown;
			}
		}

		for(Attribute::KnowledgeState state : states)
		{
			if(state == Attribute::KnowledgeState::Uncertain)
			{
				return Attribute::KnowledgeState::Uncertain;
			}
		}

		return Attribute::KnowledgeState::Certain;
	}

	ResolvedValue EvaluateTemporalOncePerMonthAnchorRangePredicate(const RuleSet& ruleSet,
	                                                               const TemporalInstant& anchorDate,
	                                                               const TemporalInstant& startDate,
	                                                               const TemporalInstant& endDate,
	                                                               Attribute::KnowledgeState knowledgeState)
	{
		const DateTimeParts anchorParts = PartsFromSeconds(anchorDate.seconds);

		return EvaluateTemporalOncePerMonthRangePredicate(ruleSet, startDate, endDate, anchorParts.day, knowledgeState);
	}

	bool IsTemporalNumericDayCountCandidate(const ResolvedValue& value)
	{
		return value.supported && IsNumericAttributeType(value.valueType);
	}

	bool TryResolveTemporalPositiveDayCountValue(const ResolvedValue& value,
	                                            const std::string& functionName,
	                                            EvaluationContext& evaluationContext,
	                                            int& days,
	                                            Attribute::KnowledgeState& knowledgeState,
	                                            ResolvedValue& failureValue)
	{
		if(!value.supported)
		{
			failureValue = value;

			return false;
		}

		knowledgeState = value.knowledgeState;

		if(!value.known || value.knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			failureValue = UnknownValue(Attribute::BOOLEAN);

			return false;
		}

		if(!IsNumericAttributeType(value.valueType) || !TryParseIntegerValue(value.value, days))
		{
			AddDiagnostic(evaluationContext,
			              "temporal_aggregate_type",
			              "Relative-window temporal day counts must be integer Numbers.",
			              functionName);

			failureValue = UnknownValue(Attribute::BOOLEAN);

			return false;
		}

		if(days < 1)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_days_must_be_positive",
			              "Relative-window temporal functions require positive integer day counts.",
			              functionName);

			failureValue = UnknownValue(Attribute::BOOLEAN);

			return false;
		}

		return true;
	}

	ResolvedValue EvaluateTemporalConsecutiveDaysSearchWindowPredicate(const RuleSet& ruleSet,
	                                                                   const TemporalAttributeReference& reference,
	                                                                   int requiredDays,
	                                                                   int searchDays,
	                                                                   Attribute::KnowledgeState argumentKnowledge,
	                                                                   EvaluationContext& evaluationContext)
	{
		if(reference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "TemporalConsecutiveDays search-window form currently supports scalar boolean committed-history attributes only.",
			              reference.instanceId + ":" + reference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(requiredDays > searchDays)
		{
			return BooleanValue(false, argumentKnowledge);
		}

		TemporalInstant anchor;
		const std::string anchorValue = evaluationContext.ruleSet != nullptr
			? evaluationContext.ruleSet->GetSessionCurrentDate()
			: ruleSet.GetSessionCurrentDate();

		if(!TryParseTemporalInstant(anchorValue, anchor) || anchor.hasTime)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_anchor_date",
			              "TemporalConsecutiveDays search-window form requires a valid evaluation current date.",
			              "TemporalConsecutiveDays:" + anchorValue);

			return UnknownValue(Attribute::BOOLEAN);
		}

		const long long anchorDayStart = LocalDayStartSeconds(anchor.seconds);
		TemporalInstant intervalStart;
		TemporalInstant intervalEnd;

		intervalStart.parsed = true;
		intervalStart.seconds = anchorDayStart - static_cast<long long>(searchDays - 1) * 86400LL;
		intervalStart.hasTime = false;
		intervalEnd.parsed = true;
		intervalEnd.seconds = anchorDayStart + 86400LL;
		intervalEnd.hasTime = false;

		ResolvedValue value = EvaluateTemporalDayCountPredicate(ruleSet,
		                                                        "TemporalConsecutiveDays",
		                                                        reference,
		                                                        intervalStart,
		                                                        intervalEnd,
		                                                        requiredDays,
		                                                        evaluationContext);

		if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
			&& value.known
			&& value.knowledgeState == Attribute::KnowledgeState::Certain)
		{
			value.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}

		return value;
	}

	std::vector<long long> MonthlyRecurrenceDayStarts(const TemporalInstant& anchorDate,
	                                                  const TemporalInstant& startDate,
	                                                  const TemporalInstant& endDate)
	{
		std::vector<long long> recurrenceDays;

		if(endDate.seconds < startDate.seconds)
		{
			return recurrenceDays;
		}

		const DateTimeParts anchorParts = PartsFromSeconds(anchorDate.seconds);
		const DateTimeParts startParts = PartsFromSeconds(startDate.seconds);
		const DateTimeParts endParts = PartsFromSeconds(endDate.seconds);
		int year = startParts.year;
		int month = startParts.month;

		while(year < endParts.year || (year == endParts.year && month <= endParts.month))
		{
			const int daysInMonthValue = DaysInMonth(year, month);
			const int recurrenceDay = anchorParts.day < daysInMonthValue ? anchorParts.day : daysInMonthValue;
			DateTimeParts recurrenceParts;

			recurrenceParts.year = year;
			recurrenceParts.month = month;
			recurrenceParts.day = recurrenceDay;
			recurrenceParts.hasDate = true;
			recurrenceParts.hasTime = false;

			const long long recurrenceStart = SecondsFromParts(recurrenceParts);

			if(recurrenceStart >= startDate.seconds && recurrenceStart <= endDate.seconds)
			{
				recurrenceDays.push_back(recurrenceStart);
			}

			++month;

			if(month > 12)
			{
				month = 1;
				++year;
			}
		}

		return recurrenceDays;
	}

	long long MonthStartSeconds(int year, int month)
	{
		DateTimeParts parts;

		parts.year = year;
		parts.month = month;
		parts.day = 1;
		parts.hasDate = true;
		parts.hasTime = false;

		return SecondsFromParts(parts);
	}

	void AdvanceMonth(int& year, int& month)
	{
		++month;

		if(month > 12)
		{
			month = 1;
			++year;
		}
	}

	ResolvedValue TemporalRequiredDayStateResult(TemporalDayState state,
	                                             Attribute::KnowledgeState argumentKnowledge)
	{
		if(state == TemporalDayState::CertainFalse)
		{
			return BooleanValue(false);
		}

		if(state == TemporalDayState::Unknown)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(state == TemporalDayState::Uncertain
			|| argumentKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(true);
	}

	ResolvedValue EvaluateTemporalOncePerMonthAnchorConditionPredicate(const RuleSet& ruleSet,
	                                                                   const TemporalInstant& anchorDate,
	                                                                   const TemporalAttributeReference& conditionReference,
	                                                                   Attribute::KnowledgeState argumentKnowledge,
	                                                                   EvaluationContext& evaluationContext)
	{
		if(conditionReference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "TemporalOncePerMonth anchor/condition form currently supports direct scalar boolean committed-history condition attributes only.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		TemporalInstant currentDate;
		const std::string currentDateValue = evaluationContext.ruleSet != nullptr
			? evaluationContext.ruleSet->GetSessionCurrentDate()
			: ruleSet.GetSessionCurrentDate();

		if(!TryParseTemporalInstant(currentDateValue, currentDate) || currentDate.hasTime)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		const DateTimeParts currentParts = PartsFromSeconds(currentDate.seconds);
		const DateTimeParts anchorParts = PartsFromSeconds(anchorDate.seconds);
		const int daysInCurrentMonth = DaysInMonth(currentParts.year, currentParts.month);
		const int recurrenceDay = anchorParts.day < daysInCurrentMonth ? anchorParts.day : daysInCurrentMonth;
		DateTimeParts recurrenceParts;

		recurrenceParts.year = currentParts.year;
		recurrenceParts.month = currentParts.month;
		recurrenceParts.day = recurrenceDay;
		recurrenceParts.hasDate = true;
		recurrenceParts.hasTime = false;

		TemporalInstant intervalStart;
		TemporalInstant intervalEnd;

		intervalStart.parsed = true;
		intervalStart.seconds = SecondsFromParts(recurrenceParts);
		intervalStart.hasTime = false;
		intervalEnd.parsed = true;
		intervalEnd.seconds = intervalStart.seconds + 86400LL;
		intervalEnd.hasTime = false;

		TemporalCoverageSummary summary;

		if(!TryCollectTemporalCoverageSummary(ruleSet, "TemporalOncePerMonth", conditionReference, intervalStart, intervalEnd, evaluationContext, summary))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		return TemporalRequiredDayStateResult(ClassifyTemporalDay(summary, intervalStart.seconds), argumentKnowledge);
	}

	ResolvedValue EvaluateTemporalOncePerMonthRangeConditionPredicate(const RuleSet& ruleSet,
	                                                                  const TemporalInstant& startDate,
	                                                                  const TemporalInstant& endDate,
	                                                                  const TemporalAttributeReference& conditionReference,
	                                                                  Attribute::KnowledgeState argumentKnowledge,
	                                                                  EvaluationContext& evaluationContext)
	{
		if(conditionReference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "TemporalOncePerMonth range/condition form currently supports direct scalar boolean committed-history condition attributes only.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(startDate.seconds > endDate.seconds)
		{
			return BooleanValue(false, argumentKnowledge);
		}

		TemporalInstant intervalStart = startDate;
		TemporalInstant intervalEnd = endDate;

		intervalEnd.seconds += 86400LL;
		intervalEnd.hasTime = false;

		TemporalCoverageSummary summary;

		if(!TryCollectTemporalCoverageSummary(ruleSet, "TemporalOncePerMonth", conditionReference, intervalStart, intervalEnd, evaluationContext, summary))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		const DateTimeParts startParts = PartsFromSeconds(startDate.seconds);
		const DateTimeParts endParts = PartsFromSeconds(endDate.seconds);
		int year = startParts.year;
		int month = startParts.month;
		bool inspectedMonth = false;
		bool sawUnknownMonth = false;
		bool sawUncertainMonth = false;

		while(year < endParts.year || (year == endParts.year && month <= endParts.month))
		{
			const long long monthStart = MonthStartSeconds(year, month);
			int nextYear = year;
			int nextMonth = month;

			AdvanceMonth(nextYear, nextMonth);

			const long long nextMonthStart = MonthStartSeconds(nextYear, nextMonth);
			const long long monthRangeStart = startDate.seconds > monthStart ? startDate.seconds : monthStart;
			const long long requestedEnd = endDate.seconds + 86400LL;
			const long long monthRangeEnd = requestedEnd < nextMonthStart ? requestedEnd : nextMonthStart;

			if(monthRangeStart < monthRangeEnd)
			{
				inspectedMonth = true;
				bool monthHasCertainTrue = false;
				bool monthSawUnknown = false;
				bool monthSawUncertain = false;

				for(long long dayStart = monthRangeStart; dayStart < monthRangeEnd; dayStart += 86400LL)
				{
					const TemporalDayState state = ClassifyTemporalDay(summary, dayStart);

					if(state == TemporalDayState::CertainTrue)
					{
						monthHasCertainTrue = true;
						break;
					}

					if(state == TemporalDayState::Unknown)
					{
						monthSawUnknown = true;
					}
					else if(state == TemporalDayState::Uncertain)
					{
						monthSawUncertain = true;
					}
				}

				if(!monthHasCertainTrue)
				{
					if(monthSawUnknown)
					{
						sawUnknownMonth = true;
					}
					else if(monthSawUncertain)
					{
						sawUncertainMonth = true;
					}
					else
					{
						return BooleanValue(false);
					}
				}
			}

			year = nextYear;
			month = nextMonth;
		}

		if(!inspectedMonth)
		{
			return BooleanValue(true, argumentKnowledge);
		}

		if(sawUnknownMonth)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertainMonth || argumentKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(true);
	}

	ResolvedValue EvaluateTemporalOncePerMonthHistoryPredicate(const RuleSet& ruleSet,
	                                                           const TemporalInstant& anchorDate,
	                                                           const TemporalInstant& startDate,
	                                                           const TemporalInstant& endDate,
	                                                           const TemporalAttributeReference& conditionReference,
	                                                           Attribute::KnowledgeState argumentKnowledge,
	                                                           EvaluationContext& evaluationContext)
	{
		if(conditionReference.valueType != Attribute::BOOLEAN)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_condition_source",
			              "TemporalOncePerMonth history/range form currently supports direct scalar boolean committed-history condition attributes only.",
			              conditionReference.instanceId + ":" + conditionReference.attributeId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(startDate.seconds > endDate.seconds)
		{
			return BooleanValue(false, argumentKnowledge);
		}

		const std::vector<long long> recurrenceDays = MonthlyRecurrenceDayStarts(anchorDate, startDate, endDate);

		if(recurrenceDays.empty())
		{
			return BooleanValue(true, argumentKnowledge);
		}

		TemporalInstant intervalStart = startDate;
		TemporalInstant intervalEnd = endDate;

		intervalEnd.seconds += 86400LL;
		intervalEnd.hasTime = false;

		TemporalCoverageSummary summary;

		if(!TryCollectTemporalCoverageSummary(ruleSet, "TemporalOncePerMonth", conditionReference, intervalStart, intervalEnd, evaluationContext, summary))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		bool sawUnknown = false;
		bool sawUncertain = false;

		for(long long recurrenceDayStart : recurrenceDays)
		{
			const TemporalDayState state = ClassifyTemporalDay(summary, recurrenceDayStart);

			if(state == TemporalDayState::CertainFalse)
			{
				return BooleanValue(false);
			}

			if(state == TemporalDayState::Unknown)
			{
				sawUnknown = true;
			}
			else if(state == TemporalDayState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(sawUnknown)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertain || argumentKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(true, Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(true);
	}

	ResolvedValue TemporalDatePredicateFailureValue(const ResolvedValue& failureValue)
	{
		if(failureValue.supported && !failureValue.known)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		return failureValue;
	}

	ResolvedValue EvaluateTemporalExpression(const RuleSet& ruleSet,
	                                         const std::shared_ptr<Instance>& context,
	                                         const ParsedFunctionCall& call,
	                                         EvaluationContext& evaluationContext)
	{
		if(call.name == "IsMemberOfAt")
		{
			return EvaluateIsMemberOfAtExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "ValueAt")
		{
			if(call.arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", "ValueAt requires an attribute and an effective-time argument.", call.name);

				return ResolvedValue{false, false, ""};
			}

			TemporalAttributeReference reference;
			TemporalInstant instant;
			Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

			if(ResolveTemporalAttributeArgument(context, call.arguments[0], reference))
			{
				if(TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, instant, timeKnowledge))
				{
					if(timeKnowledge == Attribute::KnowledgeState::Unknown)
					{
						return UnknownValue(reference.valueType);
					}

					ResolvedValue value = EvaluateTemporalValueAt(ruleSet, reference, instant, evaluationContext);

					if(timeKnowledge == Attribute::KnowledgeState::Uncertain && value.known)
					{
						value.knowledgeState = Attribute::KnowledgeState::Uncertain;
					}

					return value;
				}

				if(timeKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(reference.valueType);
				}
			}

			if(ResolveTemporalAttributeArgument(context, call.arguments[1], reference))
			{
				if(TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, instant, timeKnowledge))
				{
					if(timeKnowledge == Attribute::KnowledgeState::Unknown)
					{
						return UnknownValue(reference.valueType);
					}

					ResolvedValue value = EvaluateTemporalValueAt(ruleSet, reference, instant, evaluationContext);

					if(timeKnowledge == Attribute::KnowledgeState::Uncertain && value.known)
					{
						value.knowledgeState = Attribute::KnowledgeState::Uncertain;
					}

					return value;
				}

				if(timeKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(reference.valueType);
				}
			}

			AddDiagnostic(evaluationContext, "temporal_attribute_not_found", "ValueAt could not resolve its attribute/time arguments.", call.name);

			return UnknownValue(Attribute::UNKNOWN);
		}

		if(call.name == "Earliest" || call.name == "Latest" || call.name == "WhenLast" || call.name == "WhenNext")
		{
			if(call.arguments.empty() || call.arguments.size() > 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", call.name + " requires an attribute argument and an optional anchor time.", call.name);

				return ResolvedValue{false, false, ""};
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[0], reference))
			{
				AddDiagnostic(evaluationContext, "temporal_attribute_not_found", call.name + " could not resolve its attribute argument.", call.name);

				return UnknownValue(Attribute::DATE);
			}

			TemporalInstant anchor;
			TemporalInstant* anchorPointer = nullptr;

			if(call.name == "WhenLast" || call.name == "WhenNext")
			{
				Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;

				if(call.arguments.size() == 2)
				{
					if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, anchor, anchorKnowledge))
					{
						return UnknownValue(Attribute::DATE);
					}
				}
				else if(evaluationContext.ruleSet == nullptr || !TryParseTemporalInstant(evaluationContext.ruleSet->GetSessionCurrentDateTime(), anchor))
				{
					return UnknownValue(Attribute::DATE);
				}

				if(anchorKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::DATE);
				}

				anchorPointer = &anchor;
			}

			return EvaluateTemporalObservedDateFunction(ruleSet, call.name, reference, anchorPointer, evaluationContext);
		}

		if(IsTemporalRelativeWindowFunctionName(call.name))
		{
			if(call.arguments.size() != 2 && call.arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              TemporalRelativeWindowShapeMessage(call.name),
				              call.name);

				return ResolvedValue{false, false, ""};
			}

			if(call.arguments.size() == 3)
			{
				int anchorLastDays = 0;
				TemporalAttributeReference anchorLastReference;

				if(TryParseIntegerValue(RemoveMatchingQuotes(call.arguments[0]), anchorLastDays)
					&& ResolveTemporalAttributeArgument(context, call.arguments[1], anchorLastReference)
					&& anchorLastReference.valueType == Attribute::BOOLEAN)
				{
					AddDiagnostic(evaluationContext,
					              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
					              "Anchor-last temporal day-window forms are not supported; use anchor, days, condition.",
					              call.name);

					return UnknownValue(Attribute::BOOLEAN);
				}
			}

			if(call.name == "TemporalConsecutiveDays" && call.arguments.size() == 3)
			{
				ResolvedValue requiredDaysValue = ResolveExpressionArgument(ruleSet, context, call.arguments[0], evaluationContext);

				if(!requiredDaysValue.supported)
				{
					return requiredDaysValue;
				}

				if(IsTemporalNumericDayCountCandidate(requiredDaysValue))
				{
					ResolvedValue searchDaysValue = ResolveExpressionArgument(ruleSet, context, call.arguments[1], evaluationContext);
					int requiredDays = 0;
					int searchDays = 0;
					Attribute::KnowledgeState requiredKnowledge = Attribute::KnowledgeState::Certain;
					Attribute::KnowledgeState searchKnowledge = Attribute::KnowledgeState::Certain;
					ResolvedValue failureValue;

					if(!TryResolveTemporalPositiveDayCountValue(requiredDaysValue, call.name, evaluationContext, requiredDays, requiredKnowledge, failureValue)
						|| !TryResolveTemporalPositiveDayCountValue(searchDaysValue, call.name, evaluationContext, searchDays, searchKnowledge, failureValue))
					{
						return failureValue;
					}

					TemporalAttributeReference reference;

					if(!ResolveTemporalAttributeArgument(context, call.arguments[2], reference))
					{
						AddDiagnostic(evaluationContext,
						              "unsupported_temporal_condition_source",
						              "TemporalConsecutiveDays search-window form currently supports scalar boolean committed-history attributes only.",
						              call.name);

						return UnknownValue(Attribute::BOOLEAN);
					}

					return EvaluateTemporalConsecutiveDaysSearchWindowPredicate(ruleSet,
					                                                            reference,
					                                                            requiredDays,
					                                                            searchDays,
					                                                            CombineKnowledgeStates(requiredKnowledge, searchKnowledge),
					                                                            evaluationContext);
				}
			}

			const size_t anchorIndex = call.arguments.size() == 3 ? 0 : 0;
			const size_t daysIndex = call.arguments.size() == 3 ? 1 : 0;
			const size_t conditionIndex = call.arguments.size() == 3 ? 2 : 1;

			TemporalInstant anchor;
			TemporalInstant* anchorPointer = nullptr;
			Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;

			if(call.arguments.size() == 3)
			{
				if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[anchorIndex], evaluationContext, anchor, anchorKnowledge))
				{
					if(anchorKnowledge != Attribute::KnowledgeState::Unknown)
					{
						AddDiagnostic(evaluationContext,
						              "temporal_aggregate_type",
						              "Temporal day-window anchors must evaluate to Date or DateTime.",
						              call.name);
					}

					return UnknownValue(Attribute::BOOLEAN);
				}

				if(anchorKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				anchorPointer = &anchor;
			}

			ResolvedValue daysValue = ResolveExpressionArgument(ruleSet, context, call.arguments[daysIndex], evaluationContext);

			if(!daysValue.supported)
			{
				return daysValue;
			}

			if(!daysValue.known)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			int days = 0;

			if(!TryParseIntegerValue(daysValue.value, days))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_days_must_be_positive",
				              "Relative-window temporal functions require a positive integer number of days.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[conditionIndex], reference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "Relative-window temporal functions currently support scalar boolean committed-history attributes only.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			ResolvedValue value = EvaluateTemporalRelativeWindowPredicate(ruleSet, call.name, reference, days, anchorPointer, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = call.arguments.size() == 3
				? CombineKnowledgeStates(daysValue.knowledgeState, anchorKnowledge)
				: daysValue.knowledgeState;

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(call.name == "TemporalDaysSince")
		{
			if(call.arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", "TemporalDaysSince requires start and end date arguments.", call.name);

				return ResolvedValue{false, false, ""};
			}

			TemporalInstant start;
			TemporalInstant end;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, start, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, end, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			return EvaluateTemporalDaysSinceFunction(call.name, start, end, CombineKnowledgeStates(startKnowledge, endKnowledge), evaluationContext);
		}

		if(IsTemporalElapsedDateUnitFunctionName(call.name))
		{
			if(call.arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "Temporal elapsed-unit functions require exactly two Date arguments.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant start;
			TemporalInstant end;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, start, startKnowledge, failureValue)
				|| !TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[1], call.name, evaluationContext, end, endKnowledge, failureValue))
			{
				return failureValue;
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			const Attribute::KnowledgeState resultKnowledge =
				(startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalElapsedDateUnitFunction(call.name, start, end, resultKnowledge);
		}

		if(IsTemporalCurrentDateRangePredicateFunctionName(call.name))
		{
			if(call.arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "TemporalFromRange requires exactly two Date arguments.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalInstant startDate;
			TemporalInstant endDate;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, startDate, startKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[1], call.name, evaluationContext, endDate, endKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState resultKnowledge =
				(startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalCurrentDateRangePredicate(ruleSet, startDate, endDate, resultKnowledge);
		}

		if(call.name == "TemporalOncePerMonth"
			&& ClassifyTemporalOncePerMonthCallShape(call.arguments.size()) == TemporalOncePerMonthCallShape::AnchorConditionHistory)
		{
			TemporalInstant anchorDate;
			Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, anchorDate, anchorKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(anchorKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[1], conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "TemporalOncePerMonth anchor/condition form currently supports direct scalar boolean committed-history condition attributes only.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState argumentKnowledge =
				anchorKnowledge == Attribute::KnowledgeState::Uncertain
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalOncePerMonthAnchorConditionPredicate(ruleSet,
			                                                            anchorDate,
			                                                            conditionReference,
			                                                            argumentKnowledge,
			                                                            evaluationContext);
		}

		if(call.name == "TemporalOncePerMonth"
			&& ClassifyTemporalOncePerMonthCallShape(call.arguments.size()) == TemporalOncePerMonthCallShape::OverloadedThreeArgument)
		{
			TemporalInstant startDate;
			TemporalInstant endDate;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, startDate, startKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[1], call.name, evaluationContext, endDate, endKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference thirdReference;

			if(ResolveTemporalAttributeArgument(context, call.arguments[2], thirdReference))
			{
				if(thirdReference.valueType == Attribute::BOOLEAN)
				{
					const Attribute::KnowledgeState argumentKnowledge =
						CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge});

					return EvaluateTemporalOncePerMonthRangeConditionPredicate(ruleSet,
					                                                            startDate,
					                                                            endDate,
					                                                            thirdReference,
					                                                            argumentKnowledge,
					                                                            evaluationContext);
				}

				if(thirdReference.valueType == Attribute::DATE)
				{
					TemporalInstant anchorRangeEndDate;
					Attribute::KnowledgeState anchorRangeEndKnowledge = Attribute::KnowledgeState::Certain;

					if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[2], call.name, evaluationContext, anchorRangeEndDate, anchorRangeEndKnowledge, failureValue))
					{
						return TemporalDatePredicateFailureValue(failureValue);
					}

					if(anchorRangeEndKnowledge == Attribute::KnowledgeState::Unknown)
					{
						return UnknownValue(Attribute::BOOLEAN);
					}

					const Attribute::KnowledgeState resultKnowledge =
						CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge, anchorRangeEndKnowledge});

					return EvaluateTemporalOncePerMonthAnchorRangePredicate(ruleSet, startDate, endDate, anchorRangeEndDate, resultKnowledge);
				}
			}

			TemporalInstant literalEndDate;
			const TemporalDateOnlyLiteralResult literalDateResult = TryResolveTemporalDateOnlyLiteral(call.arguments[2], literalEndDate);

			if(literalDateResult == TemporalDateOnlyLiteralResult::Resolved)
			{
				const Attribute::KnowledgeState resultKnowledge =
					CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge});

				return EvaluateTemporalOncePerMonthAnchorRangePredicate(ruleSet, startDate, endDate, literalEndDate, resultKnowledge);
			}

			if(literalDateResult == TemporalDateOnlyLiteralResult::InvalidType)
			{
				return TemporalDatePredicateFailureValue(TemporalElapsedDateTypeDiagnostic(call.name, evaluationContext));
			}

			ResolvedValue dayValue = ResolveExpressionArgument(ruleSet, context, call.arguments[2], evaluationContext);

			if(dayValue.supported && dayValue.valueType == Attribute::DATE)
			{
				TemporalInstant anchorRangeEndDate;

				if(!dayValue.known || dayValue.knowledgeState == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				if(!TryParseTemporalInstant(dayValue.value, anchorRangeEndDate) || anchorRangeEndDate.hasTime)
				{
					return TemporalDatePredicateFailureValue(TemporalElapsedDateTypeDiagnostic(call.name, evaluationContext));
				}

				const Attribute::KnowledgeState resultKnowledge =
					CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge, dayValue.knowledgeState});

				return EvaluateTemporalOncePerMonthAnchorRangePredicate(ruleSet, startDate, endDate, anchorRangeEndDate, resultKnowledge);
			}

			if(dayValue.supported && dayValue.valueType == Attribute::BOOLEAN)
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "TemporalOncePerMonth range/condition form currently supports direct scalar boolean committed-history condition attributes only.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			int dayOfMonth = 0;
			Attribute::KnowledgeState dayKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalDayOfMonthValue(dayValue, call.name, evaluationContext, dayOfMonth, dayKnowledge, failureValue))
			{
				return failureValue;
			}

			const Attribute::KnowledgeState resultKnowledge =
				(startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain
					|| dayKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalOncePerMonthRangePredicate(ruleSet, startDate, endDate, dayOfMonth, resultKnowledge);
		}

		if(call.name == "TemporalOncePerMonth"
			&& ClassifyTemporalOncePerMonthCallShape(call.arguments.size()) == TemporalOncePerMonthCallShape::AnchorRangeConditionHistory)
		{
			TemporalInstant anchorDate;
			TemporalInstant startDate;
			TemporalInstant endDate;
			Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, anchorDate, anchorKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[1], call.name, evaluationContext, startDate, startKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[2], call.name, evaluationContext, endDate, endKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(anchorKnowledge == Attribute::KnowledgeState::Unknown
				|| startKnowledge == Attribute::KnowledgeState::Unknown
				|| endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[3], conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "TemporalOncePerMonth history/range form currently supports direct scalar boolean committed-history condition attributes only.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState argumentKnowledge =
				(anchorKnowledge == Attribute::KnowledgeState::Uncertain
					|| startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalOncePerMonthHistoryPredicate(ruleSet,
			                                                    anchorDate,
			                                                    startDate,
			                                                    endDate,
			                                                    conditionReference,
			                                                    argumentKnowledge,
			                                                    evaluationContext);
		}

		if(IsTemporalScalarDatePredicateFunctionName(call.name))
		{
			if(call.arguments.size() != 1)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "Temporal scalar date predicates require exactly one Date argument.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalInstant date;
			Attribute::KnowledgeState dateKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, date, dateKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(dateKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState resultKnowledge =
				dateKnowledge == Attribute::KnowledgeState::Uncertain
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			if(IsTemporalCurrentDateBoundaryPredicateFunctionName(call.name))
			{
				return EvaluateTemporalCurrentDateBoundaryPredicate(ruleSet, call.name, date, resultKnowledge);
			}

			if(IsTemporalCurrentDateMonthlyPredicateFunctionName(call.name))
			{
				return EvaluateTemporalOncePerMonthPredicate(ruleSet, date, resultKnowledge);
			}

			return EvaluateTemporalIsWeekdayFunction(date, resultKnowledge);
		}

		if(IsTemporalScalarDateComparisonFunctionName(call.name))
		{
			if(call.arguments.size() != 1 && call.arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "Temporal scalar comparison predicates require one Date argument or two Date/DateTime arguments.",
				              call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			if(call.arguments.size() == 1)
			{
				TemporalInstant date;
				Attribute::KnowledgeState dateKnowledge = Attribute::KnowledgeState::Certain;
				ResolvedValue failureValue;

				if(!TryResolveTemporalDateOnlyExpressionArgument(ruleSet, context, call.arguments[0], call.name, evaluationContext, date, dateKnowledge, failureValue))
				{
					return TemporalDatePredicateFailureValue(failureValue);
				}

				if(dateKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				const Attribute::KnowledgeState resultKnowledge =
					dateKnowledge == Attribute::KnowledgeState::Uncertain
						? Attribute::KnowledgeState::Uncertain
						: Attribute::KnowledgeState::Certain;

				return EvaluateTemporalCurrentDateComparisonPredicate(ruleSet, call.name, date, resultKnowledge);
			}

			std::vector<ResolvedValue> arguments;
			arguments.reserve(call.arguments.size());

			for(const std::string& argument : call.arguments)
			{
				arguments.push_back(ResolveExpressionArgument(ruleSet, context, argument, evaluationContext));
			}

			return EvaluateTemporalScalarDateComparisonFunction(call.name, arguments, evaluationContext);
		}

		if(IsTemporalConditionalAggregateFunctionName(call.name))
		{
			if(call.arguments.size() != 4)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "Conditional interval extrema require start, end, value, and condition arguments.",
				              call.name);

				return UnknownValue(Attribute::UNKNOWN);
			}

			TemporalAttributeReference valueReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[2], valueReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_value_source",
				              "Conditional interval extrema currently support direct scalar committed-history value attributes only.",
				              call.name);

				return UnknownValue(Attribute::UNKNOWN);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[3], conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "Conditional interval extrema currently support direct scalar boolean committed-history condition attributes only.",
				              call.name);

				return UnknownValue(valueReference.valueType);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(valueReference.valueType);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(valueReference.valueType);
			}

			ResolvedValue value = EvaluateTemporalConditionalExtremaFunction(ruleSet, call.name, valueReference, conditionReference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalConditionalWeightedAggregateFunctionName(call.name))
		{
			if(call.arguments.size() != 4)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              call.name + " requires start, end, value, and condition arguments.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference valueReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[2], valueReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_value_source",
				              "Conditional daily/time-weighted interval aggregates currently support direct scalar committed-history value attributes only.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[3], conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "Conditional daily/time-weighted interval aggregates currently support direct scalar boolean committed-history condition attributes only.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalConditionalWeightedAggregateFunction(ruleSet, call.name, valueReference, conditionReference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalConditionalCountDistinctAggregateFunctionName(call.name))
		{
			if(call.arguments.size() != 4)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              call.name + " requires start, end, value, and condition arguments.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference valueReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[2], valueReference))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "IntervalCountDistinctIf currently supports start, end, a direct scalar committed-history value attribute, and a direct scalar boolean committed-history condition attribute.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[3], conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "IntervalCountDistinctIf currently supports direct scalar boolean committed-history condition attributes only.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalConditionalCountDistinctFunction(ruleSet, call.name, valueReference, conditionReference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalCountDistinctAggregateFunctionName(call.name))
		{
			if(call.arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              call.name + " requires start, end, and value arguments.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[2], reference))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "IntervalCountDistinct currently supports start, end, and a direct scalar committed-history value attribute.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalCountDistinctFunction(ruleSet, call.name, reference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalWeightedAggregateFunctionName(call.name))
		{
			if(call.arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              call.name + " requires start, end, and value arguments.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[2], reference))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(call.name),
				              "Daily/time-weighted interval aggregates currently support start, end, and a direct scalar committed-history value attribute.",
				              call.name);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalWeightedAggregateFunction(ruleSet, call.name, reference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalExtremaFunctionName(call.name))
		{
			if(call.arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", call.name + " requires start/end effective-time arguments and a scalar history value.", call.name);

				return ResolvedValue{false, false, ""};
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgument(context, call.arguments[2], reference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_value_source",
				              "Interval extrema currently support direct scalar committed-history attributes only.",
				              call.name);

				return UnknownValue(Attribute::UNKNOWN);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[0], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[1], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(reference.valueType);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(reference.valueType);
			}

			ResolvedValue value = EvaluateTemporalExtremaFunction(ruleSet, call.name, reference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(call.name == "IntervalAlways"
			|| call.name == "IntervalSometimes"
			|| call.name == "IntervalAtLeastDays"
			|| call.name == "IntervalConsecutiveDays")
		{
			const bool hasDays = call.name == "IntervalAtLeastDays" || call.name == "IntervalConsecutiveDays";
			const size_t expectedArgumentCount = hasDays ? 4 : 3;

			if(call.arguments.size() != expectedArgumentCount)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", call.name + " requires start/end effective-time arguments and a boolean condition timeline.", call.name);

				return ResolvedValue{false, false, ""};
			}

			TemporalIntervalCallShape shape;
			TemporalAttributeReference reference;

			if(!TrySelectTemporalIntervalShape(context, call.arguments, hasDays, shape, reference))
			{
				AddDiagnostic(evaluationContext, "temporal_attribute_not_found", call.name + " could not resolve its condition attribute argument.", call.name);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[shape.startIndex], evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[shape.endIndex], evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);
			ResolvedValue value;

			if(hasDays)
			{
				ResolvedValue daysValue = ResolveExpressionArgument(ruleSet, context, call.arguments[shape.daysIndex], evaluationContext);

				if(!daysValue.supported)
				{
					return daysValue;
				}

				if(!daysValue.known)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				int requiredDays = 0;

				if(!TryParseIntegerValue(daysValue.value, requiredDays) || requiredDays < 0)
				{
					AddDiagnostic(evaluationContext,
					              "unsupported_expression_value",
					              "Day-count temporal functions require a non-negative integer number of days.",
					              call.name);

					return UnknownValue(Attribute::BOOLEAN);
				}

				argumentKnowledge = CombineKnowledgeStates(argumentKnowledge, daysValue.knowledgeState);
				value = EvaluateTemporalDayCountPredicate(ruleSet, call.name, reference, intervalStart, intervalEnd, requiredDays, evaluationContext);
			}
			else
			{
				value = EvaluateTemporalIntervalPredicate(ruleSet, call.name, reference, intervalStart, intervalEnd, evaluationContext);
			}

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateTemporalFunctionNode(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const json& arguments,
	                                           const std::string& functionName,
	                                           EvaluationContext& evaluationContext)
	{
		if(functionName == "IsMemberOfAt")
		{
			return EvaluateIsMemberOfAtNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "ValueAt")
		{
			if(!arguments.is_array() || arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", "ValueAt requires an attribute and an effective-time argument.", functionName);

				return ResolvedValue{false, false, ""};
			}

			TemporalAttributeReference reference;
			TemporalInstant instant;
			Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

			if(ResolveTemporalAttributeArgumentNode(context, arguments.at(0), reference))
			{
				if(TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, instant, timeKnowledge))
				{
					if(timeKnowledge == Attribute::KnowledgeState::Unknown)
					{
						return UnknownValue(reference.valueType);
					}

					ResolvedValue value = EvaluateTemporalValueAt(ruleSet, reference, instant, evaluationContext);

					if(timeKnowledge == Attribute::KnowledgeState::Uncertain && value.known)
					{
						value.knowledgeState = Attribute::KnowledgeState::Uncertain;
					}

					return value;
				}

				if(timeKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(reference.valueType);
				}
			}

			if(ResolveTemporalAttributeArgumentNode(context, arguments.at(1), reference))
			{
				if(TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, instant, timeKnowledge))
				{
					if(timeKnowledge == Attribute::KnowledgeState::Unknown)
					{
						return UnknownValue(reference.valueType);
					}

					ResolvedValue value = EvaluateTemporalValueAt(ruleSet, reference, instant, evaluationContext);

					if(timeKnowledge == Attribute::KnowledgeState::Uncertain && value.known)
					{
						value.knowledgeState = Attribute::KnowledgeState::Uncertain;
					}

					return value;
				}

				if(timeKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(reference.valueType);
				}
			}

			AddDiagnostic(evaluationContext, "temporal_attribute_not_found", "ValueAt could not resolve its attribute/time arguments.", functionName);

			return UnknownValue(Attribute::UNKNOWN);
		}

		if(functionName == "Earliest" || functionName == "Latest" || functionName == "WhenLast" || functionName == "WhenNext")
		{
			if(!arguments.is_array() || arguments.empty() || arguments.size() > 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", functionName + " requires an attribute argument and an optional anchor time.", functionName);

				return ResolvedValue{false, false, ""};
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(0), reference))
			{
				AddDiagnostic(evaluationContext, "temporal_attribute_not_found", functionName + " could not resolve its attribute argument.", functionName);

				return UnknownValue(Attribute::DATE);
			}

			TemporalInstant anchor;
			TemporalInstant* anchorPointer = nullptr;

			if(functionName == "WhenLast" || functionName == "WhenNext")
			{
				Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;

				if(arguments.size() == 2)
				{
					if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, anchor, anchorKnowledge))
					{
						return UnknownValue(Attribute::DATE);
					}
				}
				else if(evaluationContext.ruleSet == nullptr || !TryParseTemporalInstant(evaluationContext.ruleSet->GetSessionCurrentDateTime(), anchor))
				{
					return UnknownValue(Attribute::DATE);
				}

				if(anchorKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::DATE);
				}

				anchorPointer = &anchor;
			}

			return EvaluateTemporalObservedDateFunction(ruleSet, functionName, reference, anchorPointer, evaluationContext);
		}

		if(IsTemporalRelativeWindowFunctionName(functionName))
		{
			if(!arguments.is_array() || (arguments.size() != 2 && arguments.size() != 3))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              TemporalRelativeWindowShapeMessage(functionName),
				              functionName);

				return ResolvedValue{false, false, ""};
			}

			if(arguments.size() == 3)
			{
				int anchorLastDays = 0;
				TemporalAttributeReference anchorLastReference;

				if(TryParseIntegerValue(RemoveMatchingQuotes(FunctionArgumentValue(arguments.at(0))), anchorLastDays)
					&& ResolveTemporalAttributeArgumentNode(context, arguments.at(1), anchorLastReference)
					&& anchorLastReference.valueType == Attribute::BOOLEAN)
				{
					AddDiagnostic(evaluationContext,
					              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
					              "Anchor-last temporal day-window forms are not supported; use anchor, days, condition.",
					              functionName);

					return UnknownValue(Attribute::BOOLEAN);
				}
			}

			if(functionName == "TemporalConsecutiveDays" && arguments.size() == 3)
			{
				ResolvedValue requiredDaysValue = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(0), evaluationContext);

				if(!requiredDaysValue.supported)
				{
					return requiredDaysValue;
				}

				if(IsTemporalNumericDayCountCandidate(requiredDaysValue))
				{
					ResolvedValue searchDaysValue = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(1), evaluationContext);
					int requiredDays = 0;
					int searchDays = 0;
					Attribute::KnowledgeState requiredKnowledge = Attribute::KnowledgeState::Certain;
					Attribute::KnowledgeState searchKnowledge = Attribute::KnowledgeState::Certain;
					ResolvedValue failureValue;

					if(!TryResolveTemporalPositiveDayCountValue(requiredDaysValue, functionName, evaluationContext, requiredDays, requiredKnowledge, failureValue)
						|| !TryResolveTemporalPositiveDayCountValue(searchDaysValue, functionName, evaluationContext, searchDays, searchKnowledge, failureValue))
					{
						return failureValue;
					}

					TemporalAttributeReference reference;

					if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), reference))
					{
						AddDiagnostic(evaluationContext,
						              "unsupported_temporal_condition_source",
						              "TemporalConsecutiveDays search-window form currently supports scalar boolean committed-history attributes only.",
						              functionName);

						return UnknownValue(Attribute::BOOLEAN);
					}

					return EvaluateTemporalConsecutiveDaysSearchWindowPredicate(ruleSet,
					                                                            reference,
					                                                            requiredDays,
					                                                            searchDays,
					                                                            CombineKnowledgeStates(requiredKnowledge, searchKnowledge),
					                                                            evaluationContext);
				}
			}

			const size_t anchorIndex = arguments.size() == 3 ? 0 : 0;
			const size_t daysIndex = arguments.size() == 3 ? 1 : 0;
			const size_t conditionIndex = arguments.size() == 3 ? 2 : 1;

			TemporalInstant anchor;
			TemporalInstant* anchorPointer = nullptr;
			Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;

			if(arguments.size() == 3)
			{
				if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(anchorIndex), evaluationContext, anchor, anchorKnowledge))
				{
					if(anchorKnowledge != Attribute::KnowledgeState::Unknown)
					{
						AddDiagnostic(evaluationContext,
						              "temporal_aggregate_type",
						              "Temporal day-window anchors must evaluate to Date or DateTime.",
						              functionName);
					}

					return UnknownValue(Attribute::BOOLEAN);
				}

				if(anchorKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				anchorPointer = &anchor;
			}

			ResolvedValue daysValue = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(daysIndex), evaluationContext);

			if(!daysValue.supported)
			{
				return daysValue;
			}

			if(!daysValue.known)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			int days = 0;

			if(!TryParseIntegerValue(daysValue.value, days))
			{
				AddDiagnostic(evaluationContext,
				              "temporal_days_must_be_positive",
				              "Relative-window temporal functions require a positive integer number of days.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(conditionIndex), reference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "Relative-window temporal functions currently support scalar boolean committed-history attributes only.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			ResolvedValue value = EvaluateTemporalRelativeWindowPredicate(ruleSet, functionName, reference, days, anchorPointer, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = arguments.size() == 3
				? CombineKnowledgeStates(daysValue.knowledgeState, anchorKnowledge)
				: daysValue.knowledgeState;

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(functionName == "TemporalDaysSince")
		{
			if(!arguments.is_array() || arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", "TemporalDaysSince requires start and end date arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			TemporalInstant start;
			TemporalInstant end;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, start, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, end, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			return EvaluateTemporalDaysSinceFunction(functionName, start, end, CombineKnowledgeStates(startKnowledge, endKnowledge), evaluationContext);
		}

		if(IsTemporalElapsedDateUnitFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "Temporal elapsed-unit functions require exactly two Date arguments.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant start;
			TemporalInstant end;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, start, startKnowledge, failureValue)
				|| !TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(1), functionName, evaluationContext, end, endKnowledge, failureValue))
			{
				return failureValue;
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			const Attribute::KnowledgeState resultKnowledge =
				(startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalElapsedDateUnitFunction(functionName, start, end, resultKnowledge);
		}

		if(IsTemporalCurrentDateRangePredicateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "TemporalFromRange requires exactly two Date arguments.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalInstant startDate;
			TemporalInstant endDate;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, startDate, startKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(1), functionName, evaluationContext, endDate, endKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState resultKnowledge =
				(startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalCurrentDateRangePredicate(ruleSet, startDate, endDate, resultKnowledge);
		}

		if(functionName == "TemporalOncePerMonth"
			&& arguments.is_array()
			&& ClassifyTemporalOncePerMonthCallShape(arguments.size()) == TemporalOncePerMonthCallShape::AnchorConditionHistory)
		{
			TemporalInstant anchorDate;
			Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, anchorDate, anchorKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(anchorKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(1), conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "TemporalOncePerMonth anchor/condition form currently supports direct scalar boolean committed-history condition attributes only.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState argumentKnowledge =
				anchorKnowledge == Attribute::KnowledgeState::Uncertain
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalOncePerMonthAnchorConditionPredicate(ruleSet,
			                                                            anchorDate,
			                                                            conditionReference,
			                                                            argumentKnowledge,
			                                                            evaluationContext);
		}

		if(functionName == "TemporalOncePerMonth"
			&& arguments.is_array()
			&& ClassifyTemporalOncePerMonthCallShape(arguments.size()) == TemporalOncePerMonthCallShape::OverloadedThreeArgument)
		{
			TemporalInstant startDate;
			TemporalInstant endDate;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, startDate, startKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(1), functionName, evaluationContext, endDate, endKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference thirdReference;

			if(ResolveTemporalAttributeArgumentNode(context, arguments.at(2), thirdReference))
			{
				if(thirdReference.valueType == Attribute::BOOLEAN)
				{
					const Attribute::KnowledgeState argumentKnowledge =
						CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge});

					return EvaluateTemporalOncePerMonthRangeConditionPredicate(ruleSet,
					                                                            startDate,
					                                                            endDate,
					                                                            thirdReference,
					                                                            argumentKnowledge,
					                                                            evaluationContext);
				}

				if(thirdReference.valueType == Attribute::DATE)
				{
					TemporalInstant anchorRangeEndDate;
					Attribute::KnowledgeState anchorRangeEndKnowledge = Attribute::KnowledgeState::Certain;

					if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(2), functionName, evaluationContext, anchorRangeEndDate, anchorRangeEndKnowledge, failureValue))
					{
						return TemporalDatePredicateFailureValue(failureValue);
					}

					if(anchorRangeEndKnowledge == Attribute::KnowledgeState::Unknown)
					{
						return UnknownValue(Attribute::BOOLEAN);
					}

					const Attribute::KnowledgeState resultKnowledge =
						CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge, anchorRangeEndKnowledge});

					return EvaluateTemporalOncePerMonthAnchorRangePredicate(ruleSet, startDate, endDate, anchorRangeEndDate, resultKnowledge);
				}
			}

			TemporalInstant literalEndDate;
			const TemporalDateOnlyLiteralResult literalDateResult = TryResolveTemporalDateOnlyLiteral(FunctionArgumentValue(arguments.at(2)), literalEndDate);

			if(literalDateResult == TemporalDateOnlyLiteralResult::Resolved)
			{
				const Attribute::KnowledgeState resultKnowledge =
					CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge});

				return EvaluateTemporalOncePerMonthAnchorRangePredicate(ruleSet, startDate, endDate, literalEndDate, resultKnowledge);
			}

			if(literalDateResult == TemporalDateOnlyLiteralResult::InvalidType)
			{
				return TemporalDatePredicateFailureValue(TemporalElapsedDateTypeDiagnostic(functionName, evaluationContext));
			}

			ResolvedValue dayValue = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(2), evaluationContext);

			if(dayValue.supported && dayValue.valueType == Attribute::DATE)
			{
				TemporalInstant anchorRangeEndDate;

				if(!dayValue.known || dayValue.knowledgeState == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				if(!TryParseTemporalInstant(dayValue.value, anchorRangeEndDate) || anchorRangeEndDate.hasTime)
				{
					return TemporalDatePredicateFailureValue(TemporalElapsedDateTypeDiagnostic(functionName, evaluationContext));
				}

				const Attribute::KnowledgeState resultKnowledge =
					CombineTemporalArgumentKnowledge({startKnowledge, endKnowledge, dayValue.knowledgeState});

				return EvaluateTemporalOncePerMonthAnchorRangePredicate(ruleSet, startDate, endDate, anchorRangeEndDate, resultKnowledge);
			}

			if(dayValue.supported && dayValue.valueType == Attribute::BOOLEAN)
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "TemporalOncePerMonth range/condition form currently supports direct scalar boolean committed-history condition attributes only.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			int dayOfMonth = 0;
			Attribute::KnowledgeState dayKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalDayOfMonthValue(dayValue, functionName, evaluationContext, dayOfMonth, dayKnowledge, failureValue))
			{
				return failureValue;
			}

			const Attribute::KnowledgeState resultKnowledge =
				(startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain
					|| dayKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalOncePerMonthRangePredicate(ruleSet, startDate, endDate, dayOfMonth, resultKnowledge);
		}

		if(functionName == "TemporalOncePerMonth"
			&& arguments.is_array()
			&& ClassifyTemporalOncePerMonthCallShape(arguments.size()) == TemporalOncePerMonthCallShape::AnchorRangeConditionHistory)
		{
			TemporalInstant anchorDate;
			TemporalInstant startDate;
			TemporalInstant endDate;
			Attribute::KnowledgeState anchorKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, anchorDate, anchorKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(1), functionName, evaluationContext, startDate, startKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(2), functionName, evaluationContext, endDate, endKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(anchorKnowledge == Attribute::KnowledgeState::Unknown
				|| startKnowledge == Attribute::KnowledgeState::Unknown
				|| endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(3), conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "TemporalOncePerMonth history/range form currently supports direct scalar boolean committed-history condition attributes only.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState argumentKnowledge =
				(anchorKnowledge == Attribute::KnowledgeState::Uncertain
					|| startKnowledge == Attribute::KnowledgeState::Uncertain
					|| endKnowledge == Attribute::KnowledgeState::Uncertain)
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			return EvaluateTemporalOncePerMonthHistoryPredicate(ruleSet,
			                                                    anchorDate,
			                                                    startDate,
			                                                    endDate,
			                                                    conditionReference,
			                                                    argumentKnowledge,
			                                                    evaluationContext);
		}

		if(IsTemporalScalarDatePredicateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 1)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "Temporal scalar date predicates require exactly one Date argument.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalInstant date;
			Attribute::KnowledgeState dateKnowledge = Attribute::KnowledgeState::Certain;
			ResolvedValue failureValue;

			if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, date, dateKnowledge, failureValue))
			{
				return TemporalDatePredicateFailureValue(failureValue);
			}

			if(dateKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			const Attribute::KnowledgeState resultKnowledge =
				dateKnowledge == Attribute::KnowledgeState::Uncertain
					? Attribute::KnowledgeState::Uncertain
					: Attribute::KnowledgeState::Certain;

			if(IsTemporalCurrentDateBoundaryPredicateFunctionName(functionName))
			{
				return EvaluateTemporalCurrentDateBoundaryPredicate(ruleSet, functionName, date, resultKnowledge);
			}

			if(IsTemporalCurrentDateMonthlyPredicateFunctionName(functionName))
			{
				return EvaluateTemporalOncePerMonthPredicate(ruleSet, date, resultKnowledge);
			}

			return EvaluateTemporalIsWeekdayFunction(date, resultKnowledge);
		}

		if(IsTemporalScalarDateComparisonFunctionName(functionName))
		{
			if(!arguments.is_array() || (arguments.size() != 1 && arguments.size() != 2))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "Temporal scalar comparison predicates require one Date argument or two Date/DateTime arguments.",
				              functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			if(arguments.size() == 1)
			{
				TemporalInstant date;
				Attribute::KnowledgeState dateKnowledge = Attribute::KnowledgeState::Certain;
				ResolvedValue failureValue;

				if(!TryResolveTemporalDateOnlyNodeArgument(ruleSet, context, arguments.at(0), functionName, evaluationContext, date, dateKnowledge, failureValue))
				{
					return TemporalDatePredicateFailureValue(failureValue);
				}

				if(dateKnowledge == Attribute::KnowledgeState::Unknown)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				const Attribute::KnowledgeState resultKnowledge =
					dateKnowledge == Attribute::KnowledgeState::Uncertain
						? Attribute::KnowledgeState::Uncertain
						: Attribute::KnowledgeState::Certain;

				return EvaluateTemporalCurrentDateComparisonPredicate(ruleSet, functionName, date, resultKnowledge);
			}

			std::vector<ResolvedValue> values;
			values.reserve(arguments.size());

			for(const json& argument : arguments)
			{
				values.push_back(ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext));
			}

			return EvaluateTemporalScalarDateComparisonFunction(functionName, values, evaluationContext);
		}

		if(IsTemporalConditionalAggregateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 4)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "Conditional interval extrema require start, end, value, and condition arguments.",
				              functionName);

				return UnknownValue(Attribute::UNKNOWN);
			}

			TemporalAttributeReference valueReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), valueReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_value_source",
				              "Conditional interval extrema currently support direct scalar committed-history value attributes only.",
				              functionName);

				return UnknownValue(Attribute::UNKNOWN);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(3), conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "Conditional interval extrema currently support direct scalar boolean committed-history condition attributes only.",
				              functionName);

				return UnknownValue(valueReference.valueType);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(valueReference.valueType);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(valueReference.valueType);
			}

			ResolvedValue value = EvaluateTemporalConditionalExtremaFunction(ruleSet, functionName, valueReference, conditionReference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalConditionalWeightedAggregateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 4)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              functionName + " requires start, end, value, and condition arguments.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference valueReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), valueReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_value_source",
				              "Conditional daily/time-weighted interval aggregates currently support direct scalar committed-history value attributes only.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(3), conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "Conditional daily/time-weighted interval aggregates currently support direct scalar boolean committed-history condition attributes only.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalConditionalWeightedAggregateFunction(ruleSet, functionName, valueReference, conditionReference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalConditionalCountDistinctAggregateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 4)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              functionName + " requires start, end, value, and condition arguments.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference valueReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), valueReference))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "IntervalCountDistinctIf currently supports start, end, a direct scalar committed-history value attribute, and a direct scalar boolean committed-history condition attribute.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference conditionReference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(3), conditionReference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_condition_source",
				              "IntervalCountDistinctIf currently supports direct scalar boolean committed-history condition attributes only.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalConditionalCountDistinctFunction(ruleSet, functionName, valueReference, conditionReference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalCountDistinctAggregateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              functionName + " requires start, end, and value arguments.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), reference))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "IntervalCountDistinct currently supports start, end, and a direct scalar committed-history value attribute.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalCountDistinctFunction(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalWeightedAggregateFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              functionName + " requires start, end, and value arguments.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), reference))
			{
				AddDiagnostic(evaluationContext,
				              SessionModelTemporalFunctions::UnsupportedShapeDiagnostic(functionName),
				              "Daily/time-weighted interval aggregates currently support start, end, and a direct scalar committed-history value attribute.",
				              functionName);

				return UnknownValue(Attribute::DOUBLE);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::DOUBLE);
			}

			ResolvedValue value = EvaluateTemporalWeightedAggregateFunction(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(IsTemporalExtremaFunctionName(functionName))
		{
			if(!arguments.is_array() || arguments.size() != 3)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", functionName + " requires start/end effective-time arguments and a scalar history value.", functionName);

				return ResolvedValue{false, false, ""};
			}

			TemporalAttributeReference reference;

			if(!ResolveTemporalAttributeArgumentNode(context, arguments.at(2), reference))
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_value_source",
				              "Interval extrema currently support direct scalar committed-history attributes only.",
				              functionName);

				return UnknownValue(Attribute::UNKNOWN);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(0), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(1), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(reference.valueType);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(reference.valueType);
			}

			ResolvedValue value = EvaluateTemporalExtremaFunction(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext);
			const Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		if(functionName == "IntervalAlways"
			|| functionName == "IntervalSometimes"
			|| functionName == "IntervalAtLeastDays"
			|| functionName == "IntervalConsecutiveDays")
		{
			const bool hasDays = functionName == "IntervalAtLeastDays" || functionName == "IntervalConsecutiveDays";
			const size_t expectedArgumentCount = hasDays ? 4 : 3;

			if(!arguments.is_array() || arguments.size() != expectedArgumentCount)
			{
				AddDiagnostic(evaluationContext, "unsupported_temporal_function_shape", functionName + " requires start/end effective-time arguments and a boolean condition timeline.", functionName);

				return ResolvedValue{false, false, ""};
			}

			TemporalIntervalCallShape shape;
			TemporalAttributeReference reference;

			if(!TrySelectTemporalIntervalShape(context, arguments, hasDays, shape, reference))
			{
				AddDiagnostic(evaluationContext, "temporal_attribute_not_found", functionName + " could not resolve its condition attribute argument.", functionName);

				return UnknownValue(Attribute::BOOLEAN);
			}

			TemporalInstant intervalStart;
			TemporalInstant intervalEnd;
			Attribute::KnowledgeState startKnowledge = Attribute::KnowledgeState::Certain;
			Attribute::KnowledgeState endKnowledge = Attribute::KnowledgeState::Certain;

			if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(shape.startIndex), evaluationContext, intervalStart, startKnowledge)
				|| !TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(shape.endIndex), evaluationContext, intervalEnd, endKnowledge))
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			if(startKnowledge == Attribute::KnowledgeState::Unknown || endKnowledge == Attribute::KnowledgeState::Unknown)
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			Attribute::KnowledgeState argumentKnowledge = CombineKnowledgeStates(startKnowledge, endKnowledge);
			ResolvedValue value;

			if(hasDays)
			{
				ResolvedValue daysValue = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(shape.daysIndex), evaluationContext);

				if(!daysValue.supported)
				{
					return daysValue;
				}

				if(!daysValue.known)
				{
					return UnknownValue(Attribute::BOOLEAN);
				}

				int requiredDays = 0;

				if(!TryParseIntegerValue(daysValue.value, requiredDays) || requiredDays < 0)
				{
					AddDiagnostic(evaluationContext,
					              "unsupported_expression_value",
					              "Day-count temporal functions require a non-negative integer number of days.",
					              functionName);

					return UnknownValue(Attribute::BOOLEAN);
				}

				argumentKnowledge = CombineKnowledgeStates(argumentKnowledge, daysValue.knowledgeState);
				value = EvaluateTemporalDayCountPredicate(ruleSet, functionName, reference, intervalStart, intervalEnd, requiredDays, evaluationContext);
			}
			else
			{
				value = EvaluateTemporalIntervalPredicate(ruleSet, functionName, reference, intervalStart, intervalEnd, evaluationContext);
			}

			if(argumentKnowledge == Attribute::KnowledgeState::Uncertain
				&& value.known
				&& value.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				value.knowledgeState = Attribute::KnowledgeState::Uncertain;
			}

			return value;
		}

		return ResolvedValue{false, false, ""};
	}

	long long InclusiveDifference(long long value)
	{
		if(value > 0)
		{
			return value + 1;
		}

		if(value < 0)
		{
			return value - 1;
		}

		return 1;
	}

	long long ExclusiveDifference(long long value)
	{
		if(value > 0)
		{
			return value - 1;
		}

		if(value < 0)
		{
			return value + 1;
		}

		return 0;
	}

	long long AbsoluteDifference(long long value)
	{
		return value < 0 ? -value : value;
	}

	long long InclusiveAbsoluteDifference(long long value)
	{
		return AbsoluteDifference(value) + 1;
	}

	long long ExclusiveAbsoluteDifference(long long value)
	{
		const long long magnitude = AbsoluteDifference(value);

		return magnitude > 0 ? magnitude - 1 : 0;
	}

	bool TryParseDayOfWeekName(const std::string& value, int& dayOfWeek)
	{
		const std::string normalized = NormalizeLookup(value);

		if(normalized == "monday" || normalized == "mon") { dayOfWeek = 0; return true; }
		if(normalized == "tuesday" || normalized == "tue" || normalized == "tues") { dayOfWeek = 1; return true; }
		if(normalized == "wednesday" || normalized == "wed") { dayOfWeek = 2; return true; }
		if(normalized == "thursday" || normalized == "thu" || normalized == "thur" || normalized == "thurs") { dayOfWeek = 3; return true; }
		if(normalized == "friday" || normalized == "fri") { dayOfWeek = 4; return true; }
		if(normalized == "saturday" || normalized == "sat") { dayOfWeek = 5; return true; }
		if(normalized == "sunday" || normalized == "sun") { dayOfWeek = 6; return true; }

		return false;
	}

	long long CountWeekdaysForward(long long startDays, long long endDays)
	{
		if(endDays <= startDays)
		{
			return 0;
		}

		const long long totalDays = endDays - startDays;
		const long long wholeWeeks = totalDays / 7;
		const int remainder = static_cast<int>(totalDays % 7);
		long long weekdays = wholeWeeks * 5;
		int dayOfWeek = DayOfWeekMondayZero(startDays);

		for(int index = 0; index < remainder; ++index)
		{
			if(dayOfWeek < 5)
			{
				++weekdays;
			}

			dayOfWeek = (dayOfWeek + 1) % 7;
		}

		return weekdays;
	}

	long long CountWeekdaysBetween(long long startDays, long long endDays)
	{
		if(startDays <= endDays)
		{
			return CountWeekdaysForward(startDays, endDays);
		}

		return CountWeekdaysForward(endDays, startDays);
	}

	Attribute::KnowledgeState CombineArgumentKnowledge(const std::vector<ResolvedValue>& values)
	{
		Attribute::KnowledgeState state = Attribute::KnowledgeState::Certain;

		for(const ResolvedValue& value : values)
		{
			state = CombineKnowledgeStates(state, value.knowledgeState);
		}

		return state;
	}

	bool ArgumentsKnown(const std::vector<ResolvedValue>& values, ResolvedValue& result)
	{
		for(const ResolvedValue& value : values)
		{
			if(!value.supported)
			{
				result = value;

				return false;
			}

			if(!value.known)
			{
				result = ResolvedValue{true, false, ""};

				return false;
			}
		}

		return true;
	}

	bool ExpectArgumentCount(const std::string& functionName,
	                         const std::vector<ResolvedValue>& arguments,
	                         size_t minimum,
	                         size_t maximum,
	                         EvaluationContext& evaluationContext)
	{
		if(arguments.size() >= minimum && arguments.size() <= maximum)
		{
			return true;
		}

		std::ostringstream message;

		message << functionName << " expects ";

		if(minimum == maximum)
		{
			message << minimum;
		}
		else
		{
			message << minimum << " to " << maximum;
		}

		message << " argument(s).";

		AddDiagnostic(evaluationContext, "unsupported_expression_shape", message.str(), functionName);

		return false;
	}

	bool IsScalarFunctionName(const std::string& name)
	{
		return name == "Known"
			|| name == "Certain"
			|| name == "Or"
			|| name == "If"
			|| name == "Default"
			|| name == "DefaultWithUnknown"
			|| name == "Value"
			|| name == "Contains"
			|| name == "EndsWith"
			|| name == "Find"
			|| name == "Length"
			|| name == "Lower"
			|| name == "NewLine"
			|| name == "Quote"
			|| name == "StartsWith"
			|| name == "Substring"
			|| name == "Text"
			|| name == "Upper"
			|| name == "Abs"
			|| name == "Acos"
			|| name == "Asin"
			|| name == "Atan"
			|| name == "Cos"
			|| name == "Ex"
			|| name == "Ln"
			|| name == "Log"
			|| name == "Maximum"
			|| name == "Minimum"
			|| name == "Number"
			|| name == "Remainder"
			|| name == "Round"
			|| name == "Sin"
			|| name == "Sqrt"
			|| name == "Tan"
			|| name == "Trunc"
			|| name == "Xy"
			|| name == "CurrentDate"
			|| name == "CurrentTime"
			|| name == "CurrentDateTime"
			|| name == "CurrentLocale"
			|| name == "DeploymentName"
			|| name == "DeploymentVersion"
			|| name == "Date"
			|| name == "DateTime"
			|| name == "TimeOfDay"
			|| name == "AddDays"
			|| name == "AddWeeks"
			|| name == "AddMonths"
			|| name == "AddYears"
			|| name == "AddHours"
			|| name == "AddMinutes"
			|| name == "AddSeconds"
			|| name == "ConcatenateDateTime"
			|| name == "ExtractDate"
			|| name == "ExtractTimeOfDay"
			|| name == "ExtractDay"
			|| name == "ExtractMonth"
			|| name == "ExtractYear"
			|| name == "ExtractHour"
			|| name == "ExtractMinute"
			|| name == "ExtractSecond"
			|| name == "MakeDate"
			|| name == "DayDifference"
			|| name == "DayDifferenceExclusive"
			|| name == "DayDifferenceInclusive"
			|| name == "WeekDifference"
			|| name == "MonthDifference"
			|| name == "YearDifference"
			|| name == "HourDifference"
			|| name == "MinuteDifference"
			|| name == "SecondDifference"
			|| name == "SecondDifferenceExclusive"
			|| name == "SecondDifferenceInclusive"
			|| name == "NextDate"
			|| name == "NextDayOfTheWeek"
			|| name == "WeekdayCount"
			|| name == "YearStart"
			|| name == "YearEnd";
	}

	Attribute::AttributeType StaticFunctionReturnType(const std::string& functionName, const std::vector<ResolvedValue>& arguments)
	{
		if(functionName == "Known"
			|| functionName == "Certain"
			|| functionName == "Unknown"
			|| functionName == "Uncertain"
			|| functionName == "Not"
			|| functionName == "And"
			|| functionName == "Or"
			|| functionName == "Contains"
			|| functionName == "StartsWith"
			|| functionName == "EndsWith"
			|| functionName == "Equals"
			|| functionName == "NotEquals"
			|| functionName == "GreaterThan"
			|| functionName == "GreaterThanEquals"
			|| functionName == "GreaterThanOrEquals"
			|| functionName == "greaterThanOrEqual"
			|| functionName == "LessThan"
			|| functionName == "LessThanEquals"
			|| functionName == "LessThanOrEquals"
			|| functionName == "IsNumber"
			|| functionName == "Exists"
			|| functionName == "ExistsScope"
			|| functionName == "ForAll"
			|| functionName == "ForAllScope"
			|| functionName == "IsMemberOf"
			|| functionName == "InstanceEquals"
			|| functionName == "InstanceNotEquals")
		{
			return Attribute::BOOLEAN;
		}

		if(functionName == "Count"
			|| functionName == "InstanceCount"
			|| functionName == "InstanceCountIf"
			|| functionName == "Find"
			|| functionName == "Length"
			|| functionName == "Number"
			|| functionName == "Abs"
			|| functionName == "Acos"
			|| functionName == "Asin"
			|| functionName == "Atan"
			|| functionName == "Cos"
			|| functionName == "Ex"
			|| functionName == "Ln"
			|| functionName == "Log"
			|| functionName == "Maximum"
			|| functionName == "Minimum"
			|| functionName == "Remainder"
			|| functionName == "Round"
			|| functionName == "Sin"
			|| functionName == "Sqrt"
			|| functionName == "Tan"
			|| functionName == "Trunc"
			|| functionName == "Xy"
			|| functionName == "Sum"
			|| functionName == "InstanceMaximum"
			|| functionName == "InstanceMaximumIf"
			|| functionName == "InstanceMinimum"
			|| functionName == "InstanceMinimumIf"
			|| functionName == "InstanceSum"
			|| functionName == "InstanceSumIf"
			|| functionName == "ExtractDay"
			|| functionName == "ExtractMonth"
			|| functionName == "ExtractYear"
			|| functionName == "ExtractHour"
			|| functionName == "ExtractMinute"
			|| functionName == "ExtractSecond"
			|| functionName == "DayDifference"
			|| functionName == "DayDifferenceExclusive"
			|| functionName == "DayDifferenceInclusive"
			|| functionName == "WeekDifference"
			|| functionName == "MonthDifference"
			|| functionName == "YearDifference"
			|| functionName == "HourDifference"
			|| functionName == "MinuteDifference"
			|| functionName == "SecondDifference"
			|| functionName == "SecondDifferenceExclusive"
			|| functionName == "SecondDifferenceInclusive"
			|| functionName == "WeekdayCount")
		{
			return Attribute::DOUBLE;
		}

		if(functionName == "Concatenate"
			|| functionName == "CurrentLocale"
			|| functionName == "DeploymentName"
			|| functionName == "DeploymentVersion"
			|| functionName == "Lower"
			|| functionName == "NewLine"
			|| functionName == "Quote"
			|| functionName == "Substring"
			|| functionName == "Text"
			|| functionName == "Upper")
		{
			return Attribute::STRING;
		}

		if(functionName == "CurrentDate"
			|| functionName == "Date"
			|| functionName == "MakeDate"
			|| functionName == "NextDate"
			|| functionName == "NextDayOfTheWeek"
			|| functionName == "ExtractDate"
			|| functionName == "YearStart"
			|| functionName == "YearEnd")
		{
			return Attribute::DATE;
		}

		if(functionName == "CurrentDateTime"
			|| functionName == "DateTime"
			|| functionName == "ConcatenateDateTime")
		{
			return Attribute::DATETIME;
		}

		if(functionName == "CurrentTime"
			|| functionName == "TimeOfDay"
			|| functionName == "ExtractTimeOfDay")
		{
			return Attribute::TIMEOFDAY;
		}

		if(functionName == "AddDays"
			|| functionName == "AddWeeks"
			|| functionName == "AddMonths"
			|| functionName == "AddYears")
		{
			return !arguments.empty() && arguments[0].valueType == Attribute::DATETIME ? Attribute::DATETIME : Attribute::DATE;
		}

		if(functionName == "AddHours" || functionName == "AddMinutes" || functionName == "AddSeconds")
		{
			return !arguments.empty() && arguments[0].valueType == Attribute::TIMEOFDAY ? Attribute::TIMEOFDAY : Attribute::DATETIME;
		}

		return Attribute::UNKNOWN;
	}

	ResolvedValue ApplyStaticFunctionReturnType(const std::string& functionName,
	                                            const std::vector<ResolvedValue>& arguments,
	                                            ResolvedValue value)
	{
		return WithValueType(value, StaticFunctionReturnType(functionName, arguments));
	}

	ResolvedValue EvaluateScalarFunction(const std::string& functionName,
	                                     const std::vector<ResolvedValue>& arguments,
	                                     EvaluationContext& evaluationContext)
	{
		if(functionName == "Known" || functionName == "Certain")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(functionName == "Certain")
			{
				return BooleanValue(arguments[0].known && arguments[0].knowledgeState == Attribute::KnowledgeState::Certain);
			}

			return BooleanValue(arguments[0].known);
		}

		if(functionName == "CurrentDate" || functionName == "CurrentDateTime" || functionName == "CurrentTime")
		{
			ParsedFunctionCall call;

			call.name = functionName;
			call.arguments.reserve(arguments.size());

			for(size_t index = 0; index < arguments.size(); ++index)
			{
				call.arguments.push_back("");
			}

			return EvaluateCurrentDateTimeExpression(call, evaluationContext);
		}

		if(functionName == "CurrentLocale" || functionName == "DeploymentName" || functionName == "DeploymentVersion")
		{
			if(!ExpectArgumentCount(functionName, arguments, 0, 0, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(evaluationContext.ruleSet == nullptr)
			{
				return ResolvedValue{true, true, functionName == "CurrentLocale" ? "en-GB" : ""};
			}

			if(functionName == "CurrentLocale")
			{
				return ResolvedValue{true, true, evaluationContext.ruleSet->sessionContext.currentLocale};
			}

			if(functionName == "DeploymentName")
			{
				return ResolvedValue{true, true, evaluationContext.ruleSet->sessionContext.deploymentName};
			}

			return ResolvedValue{true, true, evaluationContext.ruleSet->sessionContext.deploymentVersion};
		}

		if(functionName == "Or")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1024, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			bool sawUnknown = false;
			bool sawUncertain = false;
			bool sawKnown = false;

			for(const ResolvedValue& value : arguments)
			{
				if(!value.supported)
				{
					return value;
				}

				bool boolValue = false;

				if(!value.known)
				{
					sawUnknown = true;

					continue;
				}

				sawKnown = true;

				if(TryParseBoolean(value.value, boolValue) && boolValue)
				{
					return BooleanValue(true, value.knowledgeState);
				}

				if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					sawUncertain = true;
				}
			}

			if(sawUncertain || (sawKnown && sawUnknown))
			{
				return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
			}

			if(sawUnknown)
			{
				return ResolvedValue{true, false, ""};
			}

			return BooleanValue(false);
		}

		if(functionName == "If")
		{
			if(!ExpectArgumentCount(functionName, arguments, 3, 3, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!arguments[0].supported)
			{
				return arguments[0];
			}

			if(!arguments[0].known)
			{
				return ResolvedValue{true, false, ""};
			}

			bool condition = false;

			if(!TryParseBoolean(arguments[0].value, condition))
			{
				return ResolvedValue{false, false, ""};
			}

			return condition ? arguments[1] : arguments[2];
		}

		if(functionName == "Default" || functionName == "DefaultWithUnknown")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!arguments[0].supported)
			{
				return arguments[0];
			}

			if(arguments[0].known)
			{
				if(arguments[0].knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					return arguments[1];
				}

				return arguments[0];
			}

			return functionName == "DefaultWithUnknown" ? arguments[1] : arguments[0];
		}

		if(functionName == "Value")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			return arguments[0];
		}

		if(functionName == "NewLine")
		{
			if(!ExpectArgumentCount(functionName, arguments, 0, 0, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			return ResolvedValue{true, true, "\n"};
		}

		if(functionName == "Quote")
		{
			if(!ExpectArgumentCount(functionName, arguments, 0, 0, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			return ResolvedValue{true, true, "\""};
		}

		ResolvedValue knownResult;

		if(functionName == "Contains" || functionName == "StartsWith" || functionName == "EndsWith")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			const std::string text = ToLowerAscii(arguments[0].value);
			const std::string pattern = ToLowerAscii(arguments[1].value);

			if(functionName == "Contains")
			{
				return BooleanValue(text.find(pattern) != std::string::npos, CombineArgumentKnowledge(arguments));
			}

			if(functionName == "StartsWith")
			{
				return BooleanValue(text.rfind(pattern, 0) == 0, CombineArgumentKnowledge(arguments));
			}

			return BooleanValue(text.size() >= pattern.size() && text.compare(text.size() - pattern.size(), pattern.size(), pattern) == 0, CombineArgumentKnowledge(arguments));
		}

		if(functionName == "Find")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			const size_t position = arguments[0].value.find(arguments[1].value);

			return ResolvedValue{true, true, position == std::string::npos ? "0" : std::to_string(position + 1), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "Substring")
		{
			if(!ExpectArgumentCount(functionName, arguments, 3, 3, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			int start = 0;
			int length = 0;

			if(!TryParseIntegerValue(arguments[1].value, start) || !TryParseIntegerValue(arguments[2].value, length) || start < 1 || length < 0)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Substring requires a 1-based start and non-negative length.", functionName);

				return ResolvedValue{false, false, ""};
			}

			const size_t startIndex = static_cast<size_t>(start - 1);

			if(startIndex >= arguments[0].value.size())
			{
				return ResolvedValue{true, true, "", CombineArgumentKnowledge(arguments)};
			}

			return ResolvedValue{true, true, arguments[0].value.substr(startIndex, static_cast<size_t>(length)), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "Length" || functionName == "Lower" || functionName == "Text" || functionName == "Upper")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			std::string value = arguments[0].value;

			if(functionName == "Length")
			{
				return ResolvedValue{true, true, std::to_string(value.size()), arguments[0].knowledgeState};
			}

			if(functionName == "Lower")
			{
				std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
					return static_cast<char>(std::tolower(ch));
				});
			}
			else if(functionName == "Upper")
			{
				std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
					return static_cast<char>(std::toupper(ch));
				});
			}

			return ResolvedValue{true, true, value, arguments[0].knowledgeState};
		}

		if(functionName == "Number")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			double value = 0.0;

			if(!TryParseDoubleValue(arguments[0].value, value))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Number requires a numeric argument.", functionName);

				return ResolvedValue{false, false, ""};
			}

			return ResolvedValue{true, true, FormatDoubleValue(value), arguments[0].knowledgeState};
		}

		if(functionName == "Abs" || functionName == "Acos" || functionName == "Asin" || functionName == "Atan"
			|| functionName == "Cos" || functionName == "Ex" || functionName == "Ln" || functionName == "Log"
			|| functionName == "Round" || functionName == "Sin" || functionName == "Sqrt" || functionName == "Tan"
			|| functionName == "Trunc" || functionName == "Xy")
		{
			const size_t maxArguments = (functionName == "Round" || functionName == "Trunc" || functionName == "Xy") ? 2 : 1;

			if(!ExpectArgumentCount(functionName, arguments, functionName == "Xy" ? 2 : 1, maxArguments, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			double value = 0.0;

			if(!TryParseDoubleValue(arguments[0].value, value))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Numeric function requires numeric arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			double result = value;

			if(functionName == "Abs") result = std::fabs(value);
			else if(functionName == "Acos") result = std::acos(value);
			else if(functionName == "Asin") result = std::asin(value);
			else if(functionName == "Atan") result = std::atan(value);
			else if(functionName == "Cos") result = std::cos(value);
			else if(functionName == "Ex") result = std::exp(value);
			else if(functionName == "Ln") result = std::log(value);
			else if(functionName == "Log") result = std::log10(value);
			else if(functionName == "Round")
			{
				if(arguments.size() == 2)
				{
					int decimals = 0;

					if(!TryParseIntegerValue(arguments[1].value, decimals))
					{
						AddDiagnostic(evaluationContext, "unsupported_expression_value", "Round decimal places must be an integer.", functionName);

						return ResolvedValue{false, false, ""};
					}

					const double scale = std::pow(10.0, decimals);
					result = std::round(value * scale) / scale;
				}
				else
				{
					result = std::round(value);
				}
			}
			else if(functionName == "Sin") result = std::sin(value);
			else if(functionName == "Sqrt") result = std::sqrt(value);
			else if(functionName == "Tan") result = std::tan(value);
			else if(functionName == "Trunc")
			{
				if(arguments.size() == 2)
				{
					int decimals = 0;

					if(!TryParseIntegerValue(arguments[1].value, decimals))
					{
						AddDiagnostic(evaluationContext, "unsupported_expression_value", "Trunc decimal places must be an integer.", functionName);

						return ResolvedValue{false, false, ""};
					}

					const double scale = std::pow(10.0, decimals);
					result = std::trunc(value * scale) / scale;
				}
				else
				{
					result = std::trunc(value);
				}
			}
			else if(functionName == "Xy")
			{
				double exponent = 0.0;

				if(!TryParseDoubleValue(arguments[1].value, exponent))
				{
					AddDiagnostic(evaluationContext, "unsupported_expression_value", "Xy requires numeric arguments.", functionName);

					return ResolvedValue{false, false, ""};
				}

				result = std::pow(value, exponent);
			}

			return ResolvedValue{true, true, FormatDoubleValue(result), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "Maximum" || functionName == "Minimum")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1024, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			bool hasValue = false;
			double best = 0.0;

			for(const ResolvedValue& argument : arguments)
			{
				double value = 0.0;

				if(!TryParseDoubleValue(argument.value, value))
				{
					AddDiagnostic(evaluationContext, "unsupported_expression_value", "Maximum and Minimum require numeric arguments.", functionName);

					return ResolvedValue{false, false, ""};
				}

				if(!hasValue || (functionName == "Maximum" ? value > best : value < best))
				{
					hasValue = true;
					best = value;
				}
			}

			return ResolvedValue{true, true, FormatDoubleValue(best), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "Remainder")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			double left = 0.0;
			double right = 0.0;

			if(!TryParseDoubleValue(arguments[0].value, left) || !TryParseDoubleValue(arguments[1].value, right) || right == 0.0)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Remainder requires numeric arguments and a non-zero divisor.", functionName);

				return ResolvedValue{false, false, ""};
			}

			return ResolvedValue{true, true, FormatDoubleValue(std::fmod(left, right)), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "Date" || functionName == "DateTime" || functionName == "TimeOfDay")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts parts;

			if(!TryParseDateTimeParts(arguments[0].value, parts))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Date, DateTime, and TimeOfDay require ISO date/time text.", functionName);

				return ResolvedValue{false, false, ""};
			}

			if(functionName == "Date")
			{
				if(!parts.hasDate)
				{
					AddDiagnostic(evaluationContext, "unsupported_expression_value", "Date requires date or date-time text.", functionName);

					return ResolvedValue{false, false, ""};
				}

				return ResolvedValue{true, true, FormatDate(parts), arguments[0].knowledgeState};
			}

			if(functionName == "DateTime")
			{
				if(!parts.hasDate || !parts.hasTime)
				{
					AddDiagnostic(evaluationContext, "unsupported_expression_value", "DateTime requires date-time text.", functionName);

					return ResolvedValue{false, false, ""};
				}

				return ResolvedValue{true, true, FormatDateTime(parts), arguments[0].knowledgeState};
			}

			if(!parts.hasTime)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "TimeOfDay requires time or date-time text.", functionName);

				return ResolvedValue{false, false, ""};
			}

			return ResolvedValue{true, true, FormatTime(parts), arguments[0].knowledgeState};
		}

		if(functionName == "MakeDate")
		{
			if(!ExpectArgumentCount(functionName, arguments, 3, 3, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			int year = 0;
			int month = 0;
			int day = 0;

			if(!TryParseIntegerValue(arguments[0].value, year)
				|| !TryParseIntegerValue(arguments[1].value, month)
				|| !TryParseIntegerValue(arguments[2].value, day)
				|| !IsValidDate(year, month, day))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "MakeDate requires valid year, month, and day arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			DateTimeParts parts;

			parts.year = year;
			parts.month = month;
			parts.day = day;
			parts.hasDate = true;

			return ResolvedValue{true, true, FormatDate(parts), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "ConcatenateDateTime")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts date;
			DateTimeParts time;

			if(!TryParseDateTimeParts(arguments[0].value, date) || !date.hasDate
				|| !TryParseDateTimeParts(arguments[1].value, time) || !time.hasTime)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "ConcatenateDateTime requires date and time arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			date.hour = time.hour;
			date.minute = time.minute;
			date.second = time.second;
			date.hasTime = true;

			return ResolvedValue{true, true, FormatDateTime(date), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "AddDays" || functionName == "AddWeeks" || functionName == "AddMonths" || functionName == "AddYears")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts parts;
			int amount = 0;

			if(!TryParseDateTimeParts(arguments[0].value, parts) || !parts.hasDate || !TryParseIntegerValue(arguments[1].value, amount))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Date add functions require date and integer amount arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			if(functionName == "AddDays" || functionName == "AddWeeks")
			{
				const long long delta = functionName == "AddWeeks" ? static_cast<long long>(amount) * 7LL : amount;
				const long long days = DaysFromCivil(parts.year, parts.month, parts.day) + delta;
				DateTimeParts result = CivilFromDays(days);

				result.hour = parts.hour;
				result.minute = parts.minute;
				result.second = parts.second;
				result.hasTime = parts.hasTime;

				return ResolvedValue{true, true, result.hasTime ? FormatDateTime(result) : FormatDate(result), CombineArgumentKnowledge(arguments)};
			}

			DateTimeParts result = AddMonthsToDate(parts, functionName == "AddYears" ? amount * 12 : amount);

			return ResolvedValue{true, true, result.hasTime ? FormatDateTime(result) : FormatDate(result), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "NextDate")
		{
			if(!ExpectArgumentCount(functionName, arguments, 3, 3, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts start;
			int day = 0;
			int month = 0;

			if(!TryParseDateTimeParts(arguments[0].value, start) || !start.hasDate
				|| !TryParseIntegerValue(arguments[1].value, day)
				|| !TryParseIntegerValue(arguments[2].value, month)
				|| month < 1 || month > 12 || day < 1 || day > 31)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "NextDate requires a date, day of month, and month.", functionName);

				return ResolvedValue{false, false, ""};
			}

			const long long startDays = DaysFromCivil(start.year, start.month, start.day);

			for(int year = start.year; year < start.year + 12; ++year)
			{
				if(!IsValidDate(year, month, day))
				{
					continue;
				}

				const long long candidateDays = DaysFromCivil(year, month, day);

				if(candidateDays > startDays)
				{
					DateTimeParts result;
					result.year = year;
					result.month = month;
					result.day = day;
					result.hasDate = true;

					return ResolvedValue{true, true, FormatDate(result), CombineArgumentKnowledge(arguments)};
				}
			}

			AddDiagnostic(evaluationContext, "unsupported_expression_value", "NextDate could not find a valid future date.", functionName);

			return ResolvedValue{false, false, ""};
		}

		if(functionName == "NextDayOfTheWeek")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts start;
			int targetDay = 0;

			if(!TryParseDateTimeParts(arguments[0].value, start) || !start.hasDate || !TryParseDayOfWeekName(arguments[1].value, targetDay))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "NextDayOfTheWeek requires a date and weekday name.", functionName);

				return ResolvedValue{false, false, ""};
			}

			const long long startDays = DaysFromCivil(start.year, start.month, start.day);
			const int currentDay = DayOfWeekMondayZero(startDays);
			const int delta = (targetDay - currentDay + 7) % 7;

			DateTimeParts result = CivilFromDays(startDays + delta);

			return ResolvedValue{true, true, FormatDate(result), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "WeekdayCount")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts start;
			DateTimeParts end;

			if(!TryParseDateTimeParts(arguments[0].value, start) || !start.hasDate
				|| !TryParseDateTimeParts(arguments[1].value, end) || !end.hasDate)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "WeekdayCount requires start and end dates.", functionName);

				return ResolvedValue{false, false, ""};
			}

			const long long count = CountWeekdaysBetween(DaysFromCivil(start.year, start.month, start.day), DaysFromCivil(end.year, end.month, end.day));

			return ResolvedValue{true, true, std::to_string(count), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "AddHours" || functionName == "AddMinutes" || functionName == "AddSeconds")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts parts;
			int amount = 0;

			if(!TryParseDateTimeParts(arguments[0].value, parts) || !parts.hasTime || !TryParseIntegerValue(arguments[1].value, amount))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Time add functions require time/date-time and integer amount arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			const long long multiplier = functionName == "AddHours" ? 3600LL : functionName == "AddMinutes" ? 60LL : 1LL;
			DateTimeParts result = PartsFromSeconds(SecondsFromParts(parts) + static_cast<long long>(amount) * multiplier);

			if(!parts.hasDate)
			{
				return ResolvedValue{true, true, FormatTime(result), CombineArgumentKnowledge(arguments)};
			}

			return ResolvedValue{true, true, FormatDateTime(result), CombineArgumentKnowledge(arguments)};
		}

		if(functionName == "ExtractDate" || functionName == "ExtractTimeOfDay" || functionName == "ExtractDay"
			|| functionName == "ExtractMonth" || functionName == "ExtractYear" || functionName == "ExtractHour"
			|| functionName == "ExtractMinute" || functionName == "ExtractSecond" || functionName == "YearStart"
			|| functionName == "YearEnd")
		{
			if(!ExpectArgumentCount(functionName, arguments, 1, 1, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts parts;

			if(!TryParseDateTimeParts(arguments[0].value, parts))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Date/time extraction requires a valid date, time, or date-time argument.", functionName);

				return ResolvedValue{false, false, ""};
			}

			if(functionName == "ExtractDate")
			{
				if(!parts.hasDate)
				{
					return ResolvedValue{false, false, ""};
				}

				return ResolvedValue{true, true, FormatDate(parts), arguments[0].knowledgeState};
			}

			if(functionName == "ExtractTimeOfDay")
			{
				if(!parts.hasTime)
				{
					return ResolvedValue{false, false, ""};
				}

				return ResolvedValue{true, true, FormatTime(parts), arguments[0].knowledgeState};
			}

			if(functionName == "ExtractDay") return ResolvedValue{true, true, std::to_string(parts.day), arguments[0].knowledgeState};
			if(functionName == "ExtractMonth") return ResolvedValue{true, true, std::to_string(parts.month), arguments[0].knowledgeState};
			if(functionName == "ExtractYear") return ResolvedValue{true, true, std::to_string(parts.year), arguments[0].knowledgeState};
			if(functionName == "ExtractHour") return ResolvedValue{true, true, std::to_string(parts.hour), arguments[0].knowledgeState};
			if(functionName == "ExtractMinute") return ResolvedValue{true, true, std::to_string(parts.minute), arguments[0].knowledgeState};
			if(functionName == "ExtractSecond") return ResolvedValue{true, true, std::to_string(parts.second), arguments[0].knowledgeState};

			DateTimeParts result;
			result.year = parts.year;
			result.month = functionName == "YearStart" ? 1 : 12;
			result.day = functionName == "YearStart" ? 1 : 31;
			result.hasDate = true;

			return ResolvedValue{true, true, FormatDate(result), arguments[0].knowledgeState};
		}

		if(functionName == "DayDifference" || functionName == "DayDifferenceExclusive" || functionName == "DayDifferenceInclusive"
			|| functionName == "WeekDifference" || functionName == "MonthDifference" || functionName == "YearDifference"
			|| functionName == "HourDifference" || functionName == "MinuteDifference" || functionName == "SecondDifference"
			|| functionName == "SecondDifferenceExclusive" || functionName == "SecondDifferenceInclusive")
		{
			if(!ExpectArgumentCount(functionName, arguments, 2, 2, evaluationContext))
			{
				return ResolvedValue{false, false, ""};
			}

			if(!ArgumentsKnown(arguments, knownResult))
			{
				return knownResult;
			}

			DateTimeParts left;
			DateTimeParts right;

			if(!TryParseDateTimeParts(arguments[0].value, left) || !TryParseDateTimeParts(arguments[1].value, right))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Difference functions require valid date/time arguments.", functionName);

				return ResolvedValue{false, false, ""};
			}

			if(functionName == "DayDifference" || functionName == "DayDifferenceExclusive" || functionName == "DayDifferenceInclusive"
				|| functionName == "WeekDifference" || functionName == "MonthDifference" || functionName == "YearDifference")
			{
				if(!left.hasDate || !right.hasDate)
				{
					return ResolvedValue{false, false, ""};
				}

				const long long days = DaysFromCivil(right.year, right.month, right.day) - DaysFromCivil(left.year, left.month, left.day);
				const long long absoluteDays = AbsoluteDifference(days);

				if(functionName == "WeekDifference")
				{
					return ResolvedValue{true, true, std::to_string(absoluteDays / 7LL), CombineArgumentKnowledge(arguments)};
				}

				if(functionName == "DayDifferenceExclusive")
				{
					return ResolvedValue{true, true, std::to_string(ExclusiveAbsoluteDifference(days)), CombineArgumentKnowledge(arguments)};
				}

				if(functionName == "DayDifferenceInclusive")
				{
					return ResolvedValue{true, true, std::to_string(InclusiveAbsoluteDifference(days)), CombineArgumentKnowledge(arguments)};
				}

				if(functionName == "MonthDifference")
				{
					return ResolvedValue{true, true, std::to_string(WholeMonthDifference(left, right)), CombineArgumentKnowledge(arguments)};
				}

				if(functionName == "YearDifference")
				{
					return ResolvedValue{true, true, std::to_string(WholeYearDifference(left, right)), CombineArgumentKnowledge(arguments)};
				}

				return ResolvedValue{true, true, std::to_string(absoluteDays), CombineArgumentKnowledge(arguments)};
			}

			if(!left.hasTime || !right.hasTime)
			{
				return ResolvedValue{false, false, ""};
			}

			const long long seconds = SecondsFromParts(right) - SecondsFromParts(left);
			const long long divisor = functionName == "HourDifference" ? 3600LL : functionName == "MinuteDifference" ? 60LL : 1LL;

			if(functionName == "SecondDifferenceExclusive")
			{
				return ResolvedValue{true, true, std::to_string(ExclusiveDifference(seconds)), CombineArgumentKnowledge(arguments)};
			}

			if(functionName == "SecondDifferenceInclusive")
			{
				return ResolvedValue{true, true, std::to_string(InclusiveDifference(seconds)), CombineArgumentKnowledge(arguments)};
			}

			return ResolvedValue{true, true, std::to_string(seconds / divisor), CombineArgumentKnowledge(arguments)};
		}

		return ResolvedValue{false, false, ""};
	}

	std::vector<ResolvedValue> ResolveExpressionArguments(const RuleSet& ruleSet,
	                                                     const std::shared_ptr<Instance>& context,
	                                                     const ParsedFunctionCall& call,
	                                                     EvaluationContext& evaluationContext)
	{
		std::vector<ResolvedValue> values;

		values.reserve(call.arguments.size());

		for(const std::string& argument : call.arguments)
		{
			values.push_back(ResolveExpressionArgument(ruleSet, context, argument, evaluationContext));
		}

		return values;
	}

	ResolvedValue EvaluateAndExpression(const RuleSet& ruleSet,
	                                    const std::shared_ptr<Instance>& context,
	                                    const ParsedFunctionCall& call,
	                                    EvaluationContext& evaluationContext)
	{
		if(call.arguments.empty())
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "And expression requires at least one argument.", "And");

			return ResolvedValue{false, false, ""};
		}

		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawKnown = false;

		for(const std::string& argument : call.arguments)
		{
			ResolvedValue value = ResolveExpressionArgument(ruleSet, context, argument, evaluationContext);

			if(!value.supported)
			{
				return value;
			}

			bool boolValue = false;

			if(!value.known)
			{
				sawUnknown = true;

				continue;
			}

			sawKnown = true;

			if(!TryParseBoolean(value.value, boolValue))
			{
				return BooleanValue(false);
			}

			if(value.knowledgeState == Attribute::KnowledgeState::Certain && !boolValue)
			{
				return BooleanValue(false);
			}

			if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
			else if(!boolValue)
			{
				sawUncertain = true;
			}
		}

		if(sawUncertain || sawUnknown)
		{
			return BooleanValue(false, (sawUncertain || sawKnown) ? Attribute::KnowledgeState::Uncertain : Attribute::KnowledgeState::Unknown);
		}

		return BooleanValue(true);
	}

	ResolvedValue EvaluateUnknownExpression(const RuleSet& ruleSet,
	                                       const std::shared_ptr<Instance>& context,
	                                       const ParsedFunctionCall& call,
	                                       EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Unknown expression requires one argument.", call.name);

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue value = ResolveExpressionArgument(ruleSet, context, call.arguments[0], evaluationContext);

		if(!value.supported)
		{
			return value;
		}

		if(call.name == "Uncertain")
		{
			return BooleanValue(value.known && value.knowledgeState == Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(!value.known);
	}

	ResolvedValue EvaluateEqualsExpression(const RuleSet& ruleSet,
	                                      const std::shared_ptr<Instance>& context,
	                                      const ParsedFunctionCall& call,
	                                      bool invert,
	                                      EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Equals and NotEquals expressions require two arguments.", call.name);

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue left = ResolveExpressionArgument(ruleSet, context, call.arguments[0], evaluationContext);
		ResolvedValue right = ResolveExpressionArgument(ruleSet, context, call.arguments[1], evaluationContext);

		if(!left.supported)
		{
			return left;
		}

		if(!right.supported)
		{
			return right;
		}

		if(!left.known || !right.known)
		{
			return BooleanValue(false, (left.known || right.known) ? Attribute::KnowledgeState::Uncertain : Attribute::KnowledgeState::Unknown);
		}

		const bool equals = left.known && right.known && left.value == right.value;

		const Attribute::KnowledgeState knowledgeState =
			left.knowledgeState == Attribute::KnowledgeState::Certain && right.knowledgeState == Attribute::KnowledgeState::Certain
				? Attribute::KnowledgeState::Certain
				: Attribute::KnowledgeState::Uncertain;

		return BooleanValue(invert ? !equals : equals, knowledgeState);
	}

	ResolvedValue EvaluateComparisonExpression(const RuleSet& ruleSet,
	                                          const std::shared_ptr<Instance>& context,
	                                          const ParsedFunctionCall& call,
	                                          EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Comparison expressions require two arguments.", call.name);

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue left = ResolveExpressionArgument(ruleSet, context, call.arguments[0], evaluationContext);
		ResolvedValue right = ResolveExpressionArgument(ruleSet, context, call.arguments[1], evaluationContext);

		if(!left.supported)
		{
			return left;
		}

		if(!right.supported)
		{
			return right;
		}

		if(!left.known || !right.known)
		{
			return BooleanValue(false, (left.known || right.known) ? Attribute::KnowledgeState::Uncertain : Attribute::KnowledgeState::Unknown);
		}

		const int comparison = CompareKnownValues(left.value, right.value);
		const Attribute::KnowledgeState knowledgeState =
			left.knowledgeState == Attribute::KnowledgeState::Certain && right.knowledgeState == Attribute::KnowledgeState::Certain
				? Attribute::KnowledgeState::Certain
				: Attribute::KnowledgeState::Uncertain;

		if(call.name == "GreaterThan")
		{
			return BooleanValue(comparison > 0, knowledgeState);
		}

		if(call.name == "GreaterThanEquals" || call.name == "GreaterThanOrEquals" || call.name == "greaterThanOrEqual")
		{
			return BooleanValue(comparison >= 0, knowledgeState);
		}

		if(call.name == "LessThan")
		{
			return BooleanValue(comparison < 0, knowledgeState);
		}

		if(call.name == "LessThanEquals" || call.name == "LessThanOrEquals")
		{
			return BooleanValue(comparison <= 0, knowledgeState);
		}

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateIsNumberExpression(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& context,
	                                        const ParsedFunctionCall& call,
	                                        EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "IsNumber expression requires one argument.", call.name);

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue value = ResolveExpressionArgument(ruleSet, context, call.arguments[0], evaluationContext);

		if(!value.supported)
		{
			return value;
		}

		if(!value.known)
		{
			return ResolvedValue{true, false, ""};
		}

		double numericValue = 0.0;

		return BooleanValue(TryParseDoubleValue(value.value, numericValue), value.knowledgeState);
	}

	ResolvedValue EvaluateExistsExpression(const RuleSet& ruleSet,
	                                      const std::shared_ptr<Instance>& context,
	                                      const ParsedFunctionCall& call,
	                                      EvaluationContext& evaluationContext)
	{
		if(call.arguments.empty() || call.arguments.size() > 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Exists expression requires scope and optional condition arguments.", "Exists");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);

		return EvaluateScopedQuantifierOverInstances(instances,
		                                             context,
		                                             false,
		                                             call.arguments.size() > 1,
		                                             false,
		                                             evaluationContext,
		                                             [&](const std::shared_ptr<Instance>& instance) {
			                                             return ResolveExpressionArgument(ruleSet, instance, call.arguments[1], evaluationContext);
		                                             });
	}

	ResolvedValue EvaluateScopedQuantifierExpression(const RuleSet& ruleSet,
	                                                 const std::shared_ptr<Instance>& context,
	                                                 const ParsedFunctionCall& call,
	                                                 EvaluationContext& evaluationContext)
	{
		const bool isForAll = call.name == "ForAll" || call.name == "ForAllScope";
		const bool isExportedForAll = call.name == "ForAll";

		if(call.arguments.empty() || call.arguments.size() > 3 || (isExportedForAll && call.arguments.size() < 2))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Scoped quantifier expression requires scope, optional alias, and optional condition arguments.", call.name);

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, scope);
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);

		const size_t conditionIndex = call.arguments.size() >= 3 ? 2 : 1;
		ScopedQuantifierTraceSummary traceSummary;

		ResolvedValue value = EvaluateScopedQuantifierOverInstances(instances,
		                                                            context,
		                                                            isForAll,
		                                                            call.arguments.size() > 1,
		                                                            call.name == "ForAllScope",
		                                                            evaluationContext,
		                                                            [&](const std::shared_ptr<Instance>& instance) {
			                                                            return ResolveExpressionArgument(ruleSet, instance, call.arguments[conditionIndex], evaluationContext);
		                                                            },
		                                                            &traceSummary);

		return WithScopedQuantifierTrace(value, relationshipId, traceSummary);
	}

	std::string ResolveInstanceToken(const std::shared_ptr<Instance>& context, const std::string& argument)
	{
		const std::string value = RemoveMatchingQuotes(argument);
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty() || normalized == "scope-ref")
		{
			return context != nullptr ? context->instanceId : "";
		}

		return value;
	}

	ResolvedInstanceReference InstanceReferenceFromValue(const ResolvedValue& value)
	{
		if(!value.supported)
		{
			return ResolvedInstanceReference{false, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(!value.known)
		{
			return ResolvedInstanceReference{true, false, "", value.knowledgeState};
		}

		return ResolvedInstanceReference{true, !value.value.empty(), value.value, value.knowledgeState};
	}

	ResolvedInstanceReference ResolveTargetInstanceReference(const RuleSet& ruleSet,
	                                                        const std::shared_ptr<Instance>& context,
	                                                        const std::string& argument,
	                                                        EvaluationContext& evaluationContext)
	{
		if(IsQuotedString(argument))
		{
			const std::string instanceId = RemoveMatchingQuotes(argument);

			return ResolvedInstanceReference{true, !instanceId.empty(), instanceId, Attribute::KnowledgeState::Certain};
		}

		ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

		if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
		{
			if(!nestedCall.valid)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression syntax is invalid.", argument);

				return ResolvedInstanceReference{false, false, "", Attribute::KnowledgeState::Unknown};
			}

			return InstanceReferenceFromValue(EvaluateExpressionCallWithTrace(ruleSet, context, nestedCall, evaluationContext));
		}

		std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(context, argument);

		if(attribute != nullptr)
		{
			if(attribute->IsUnknown())
			{
				return ResolvedInstanceReference{true, false, "", attribute->GetKnowledgeState()};
			}

			return ResolvedInstanceReference{
				true,
				!attribute->GetStringValue().empty(),
				attribute->GetStringValue(),
				attribute->GetKnowledgeState()
			};
		}

		const std::string instanceId = ResolveInstanceToken(context, argument);

		return ResolvedInstanceReference{true, !instanceId.empty(), instanceId, Attribute::KnowledgeState::Certain};
	}

	ResolvedRelationshipScope RelationshipScopeFromValue(const RuleSet& ruleSet,
	                                                     const ResolvedValue& value,
	                                                     EvaluationContext& evaluationContext)
	{
		if(!value.supported)
		{
			return ResolvedRelationshipScope{false, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(!value.known)
		{
			return ResolvedRelationshipScope{true, false, "", value.knowledgeState};
		}

		if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return ResolvedRelationshipScope{true, true, value.value, Attribute::KnowledgeState::Uncertain};
		}

		std::shared_ptr<Relationship> relationship = FindRelationshipByScope(ruleSet, value.value);

		if(relationship == nullptr)
		{
			AddDiagnostic(evaluationContext, "is_member_of_relationship", "IsMemberOf relationship argument does not resolve to a known relationship.", value.value);

			return ResolvedRelationshipScope{false, false, "", Attribute::KnowledgeState::Unknown};
		}

		return ResolvedRelationshipScope{true, true, relationship->id, Attribute::KnowledgeState::Certain};
	}

	ResolvedRelationshipScope ResolveRelationshipScopeArgument(const RuleSet& ruleSet,
	                                                          const std::shared_ptr<Instance>& sourceContext,
	                                                          const std::string& argument,
	                                                          EvaluationContext& evaluationContext)
	{
		if(sourceContext == nullptr)
		{
			return ResolvedRelationshipScope{true, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(IsQuotedString(argument))
		{
			return RelationshipScopeFromValue(ruleSet,
			                                  ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
			                                  evaluationContext);
		}

		ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

		if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
		{
			if(!nestedCall.valid)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression syntax is invalid.", argument);

				return ResolvedRelationshipScope{false, false, "", Attribute::KnowledgeState::Unknown};
			}

			return RelationshipScopeFromValue(ruleSet, EvaluateExpressionCallWithTrace(ruleSet, sourceContext, nestedCall, evaluationContext), evaluationContext);
		}

		if(FindRelationshipByScope(ruleSet, argument) != nullptr)
		{
			return RelationshipScopeFromValue(ruleSet,
			                                  ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
			                                  evaluationContext);
		}

		std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(sourceContext, argument);

		if(attribute != nullptr)
		{
			if(attribute->IsUnknown())
			{
				return ResolvedRelationshipScope{true, false, "", attribute->GetKnowledgeState()};
			}

			return RelationshipScopeFromValue(ruleSet,
			                                  ResolvedValue{
				                                  true,
				                                  !attribute->GetStringValue().empty(),
				                                  attribute->GetStringValue(),
				                                  attribute->GetKnowledgeState(),
				                                  static_cast<Attribute::AttributeType>(attribute->GetType())
			                                  },
			                                  evaluationContext);
		}

		return RelationshipScopeFromValue(ruleSet,
		                                  ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
		                                  evaluationContext);
	}

	ResolvedInstanceReference ResolveTargetInstanceReferenceNode(const RuleSet& ruleSet,
	                                                            const std::shared_ptr<Instance>& context,
	                                                            const json& argument,
	                                                            EvaluationContext& evaluationContext)
	{
		if(argument.is_object()
			&& (argument.contains("nestedFunction")
				|| argument.contains("functionName")
				|| GetString(argument, "valueMode") == "Attribute"))
		{
			return InstanceReferenceFromValue(ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext));
		}

		const std::string instanceId = ResolveInstanceNodeToken(context, argument, 0, evaluationContext);

		return ResolvedInstanceReference{true, !instanceId.empty(), instanceId, Attribute::KnowledgeState::Certain};
	}

	ResolvedRelationshipScope ResolveRelationshipScopeArgumentNode(const RuleSet& ruleSet,
	                                                              const std::shared_ptr<Instance>& sourceContext,
	                                                              const json& argument,
	                                                              EvaluationContext& evaluationContext)
	{
		if(sourceContext == nullptr)
		{
			return ResolvedRelationshipScope{true, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(argument.is_object()
			&& (argument.contains("nestedFunction")
				|| argument.contains("functionName")
				|| GetString(argument, "valueMode") == "Attribute"))
		{
			return RelationshipScopeFromValue(ruleSet, ResolveFunctionArgumentNode(ruleSet, sourceContext, argument, evaluationContext), evaluationContext);
		}

		return RelationshipScopeFromValue(ruleSet,
		                                  ResolvedValue{true, true, RelationshipScopeValue(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
		                                  evaluationContext);
	}

	ResolvedValue EvaluateIsMemberOfResult(const std::shared_ptr<Instance>& sourceContext,
	                                       const ResolvedInstanceReference& target,
	                                       const ResolvedRelationshipScope& relationship)
	{
		if(!target.supported || !relationship.supported)
		{
			return ResolvedValue{false, false, ""};
		}

		if(!target.known)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(target.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		if(!relationship.known)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(relationship.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		if(sourceContext == nullptr)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		return BooleanValue(HasRelationshipLinkById(sourceContext, relationship.relationshipId, [&](const InstanceRelationshipLink& link) {
			return link.outgoing
				&& link.sourceInstanceId == sourceContext->instanceId
				&& link.targetInstanceId == target.instanceId;
		}));
	}

	ResolvedValue EvaluateIsMemberOfExpression(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const ParsedFunctionCall& call,
	                                           EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2 && call.arguments.size() != 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "IsMemberOf expression requires target and relationship arguments, with an optional alias argument.", "IsMemberOf");

			return ResolvedValue{false, false, ""};
		}

		const size_t relationshipIndex = call.arguments.size() == 3 ? 2 : 1;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedInstanceReference target = ResolveTargetInstanceReference(ruleSet, context, call.arguments[0], evaluationContext);
		ResolvedRelationshipScope relationship = ResolveRelationshipScopeArgument(ruleSet, sourceContext, call.arguments[relationshipIndex], evaluationContext);

		return EvaluateIsMemberOfResult(sourceContext, target, relationship);
	}

	ResolvedValue EvaluateIsMemberOfNode(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const json& arguments,
	                                     EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || (arguments.size() != 2 && arguments.size() != 3))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "IsMemberOf expression requires target and relationship arguments, with an optional alias argument.", "IsMemberOf");

			return ResolvedValue{false, false, ""};
		}

		const size_t relationshipIndex = arguments.size() == 3 ? 2 : 1;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedInstanceReference target = ResolveTargetInstanceReferenceNode(ruleSet, context, arguments.at(0), evaluationContext);
		ResolvedRelationshipScope relationship = ResolveRelationshipScopeArgumentNode(ruleSet, sourceContext, arguments.at(relationshipIndex), evaluationContext);

		return EvaluateIsMemberOfResult(sourceContext, target, relationship);
	}

	ResolvedRelationshipScope TemporalRelationshipScopeFromValue(const RuleSet& ruleSet,
	                                                            const ResolvedValue& value,
	                                                            EvaluationContext& evaluationContext)
	{
		if(!value.supported)
		{
			return ResolvedRelationshipScope{false, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(!value.known)
		{
			return ResolvedRelationshipScope{true, false, "", value.knowledgeState};
		}

		if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return ResolvedRelationshipScope{true, true, value.value, Attribute::KnowledgeState::Uncertain};
		}

		std::shared_ptr<Relationship> relationship = FindRelationshipByScope(ruleSet, value.value);

		if(relationship == nullptr)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_not_found",
			              "IsMemberOfAt relationship argument does not resolve to a known relationship.",
			              value.value);

			return ResolvedRelationshipScope{false, false, "", Attribute::KnowledgeState::Unknown};
		}

		return ResolvedRelationshipScope{true, true, relationship->id, Attribute::KnowledgeState::Certain};
	}

	ResolvedRelationshipScope ResolveTemporalRelationshipScopeArgument(const RuleSet& ruleSet,
	                                                                  const std::shared_ptr<Instance>& sourceContext,
	                                                                  const std::string& argument,
	                                                                  EvaluationContext& evaluationContext)
	{
		if(sourceContext == nullptr)
		{
			return ResolvedRelationshipScope{true, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(IsQuotedString(argument))
		{
			return TemporalRelationshipScopeFromValue(ruleSet,
			                                          ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
			                                          evaluationContext);
		}

		ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

		if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
		{
			if(!nestedCall.valid)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression syntax is invalid.", argument);

				return ResolvedRelationshipScope{false, false, "", Attribute::KnowledgeState::Unknown};
			}

			return TemporalRelationshipScopeFromValue(ruleSet, EvaluateExpressionCallWithTrace(ruleSet, sourceContext, nestedCall, evaluationContext), evaluationContext);
		}

		if(FindRelationshipByScope(ruleSet, argument) != nullptr)
		{
			return TemporalRelationshipScopeFromValue(ruleSet,
			                                          ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
			                                          evaluationContext);
		}

		std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(sourceContext, argument);

		if(attribute != nullptr)
		{
			if(attribute->IsUnknown())
			{
				return ResolvedRelationshipScope{true, false, "", attribute->GetKnowledgeState()};
			}

			return TemporalRelationshipScopeFromValue(ruleSet,
			                                          ResolvedValue{
				                                          true,
				                                          !attribute->GetStringValue().empty(),
				                                          attribute->GetStringValue(),
				                                          attribute->GetKnowledgeState(),
				                                          static_cast<Attribute::AttributeType>(attribute->GetType())
			                                          },
			                                          evaluationContext);
		}

		return TemporalRelationshipScopeFromValue(ruleSet,
		                                          ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
		                                          evaluationContext);
	}

	ResolvedRelationshipScope ResolveTemporalRelationshipScopeArgumentNode(const RuleSet& ruleSet,
	                                                                      const std::shared_ptr<Instance>& sourceContext,
	                                                                      const json& argument,
	                                                                      EvaluationContext& evaluationContext)
	{
		if(sourceContext == nullptr)
		{
			return ResolvedRelationshipScope{true, false, "", Attribute::KnowledgeState::Unknown};
		}

		if(argument.is_object()
			&& (argument.contains("nestedFunction")
				|| argument.contains("functionName")
				|| GetString(argument, "valueMode") == "Attribute"))
		{
			return TemporalRelationshipScopeFromValue(ruleSet, ResolveFunctionArgumentNode(ruleSet, sourceContext, argument, evaluationContext), evaluationContext);
		}

		return TemporalRelationshipScopeFromValue(ruleSet,
		                                          ResolvedValue{true, true, RelationshipScopeValue(argument), Attribute::KnowledgeState::Certain, Attribute::STRING},
		                                          evaluationContext);
	}

	bool BuildEntityExistenceCoverage(const RuleSet& ruleSet,
	                                  const std::string& entityId,
	                                  const std::string& instanceId,
	                                  const TemporalEffectiveWindow& window,
	                                  std::vector<TemporalCoverageSegment>& segments,
	                                  EvaluationContext& evaluationContext)
	{
		for(const RuleSet::EntityExistenceHistorySegment* segment : ruleSet.GetEntityExistenceHistorySegments(entityId, instanceId))
		{
			if(segment == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildHistoryCoverageSegment(segment->effectiveStart,
			                                   segment->effectiveEnd,
			                                   segment->knowledgeState,
			                                   "entity:" + entityId + ":" + instanceId,
			                                   window,
			                                   coverage,
			                                   evaluationContext))
			{
				return false;
			}

			if(coverage.endSeconds > coverage.startSeconds)
			{
				segments.push_back(coverage);
			}
		}

		return true;
	}

	bool BuildRelationshipCoverage(const RuleSet& ruleSet,
	                               const std::string& relationshipId,
	                               const std::string& sourceInstanceId,
	                               const TemporalEffectiveWindow& window,
	                               std::vector<TemporalCoverageSegment>& segments,
	                               EvaluationContext& evaluationContext)
	{
		for(const RuleSet::RelationshipCoverageHistorySegment* segment : ruleSet.GetRelationshipCoverageHistorySegments(relationshipId, sourceInstanceId))
		{
			if(segment == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildHistoryCoverageSegment(segment->effectiveStart,
			                                   segment->effectiveEnd,
			                                   segment->knowledgeState,
			                                   "relationshipCoverage:" + relationshipId + ":" + sourceInstanceId,
			                                   window,
			                                   coverage,
			                                   evaluationContext))
			{
				return false;
			}

			if(coverage.endSeconds > coverage.startSeconds)
			{
				segments.push_back(coverage);
			}
		}

		return true;
	}

	bool RelationshipCoverageHasConflict(const std::string& relationshipId,
	                                     const std::string& sourceInstanceId,
	                                     const std::vector<TemporalCoverageSegment>& coverageSegments,
	                                     EvaluationContext& evaluationContext)
	{
		for(size_t leftIndex = 0; leftIndex < coverageSegments.size(); ++leftIndex)
		{
			for(size_t rightIndex = leftIndex + 1; rightIndex < coverageSegments.size(); ++rightIndex)
			{
				const TemporalCoverageSegment& left = coverageSegments[leftIndex];
				const TemporalCoverageSegment& right = coverageSegments[rightIndex];

				if(left.knowledgeState != right.knowledgeState
					&& RawTemporalIntervalsOverlap(left.startSeconds, left.endSeconds, right.startSeconds, right.endSeconds))
				{
					AddDiagnostic(evaluationContext,
					              "temporal_relationship_coverage_conflict",
					              "IsMemberOfAt found contradictory relationship coverage history for the same source relationship.",
					              relationshipId + ":" + sourceInstanceId);

					return true;
				}
			}
		}

		return false;
	}

	ResolvedValue EvaluateNoLinkRelationshipCoverageResult(const std::vector<TemporalCoverageSegment>& coverageSegments,
	                                                       const TemporalEffectiveWindow& window)
	{
		if(coverageSegments.empty())
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		std::vector<TemporalCoverageSegment> certainCoverageSegments;
		bool sawUnknownCoverage = false;
		bool sawUncertainCoverage = false;

		for(const TemporalCoverageSegment& segment : coverageSegments)
		{
			if(segment.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				certainCoverageSegments.push_back(segment);
			}
			else if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknownCoverage = true;
			}
			else if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertainCoverage = true;
			}
		}

		if(HasCompleteCoverage(certainCoverageSegments, window.start.seconds, window.end.seconds))
		{
			return BooleanValue(false);
		}

		if(!HasCompleteCoverage(coverageSegments, window.start.seconds, window.end.seconds) || sawUnknownCoverage)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertainCoverage)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		return UnknownValue(Attribute::BOOLEAN);
	}

	bool EntityExistenceCoverageAllowsCertainOverlap(const RuleSet& ruleSet,
	                                                const std::string& sourceEntityId,
	                                                const std::string& sourceInstanceId,
	                                                const std::string& targetEntityId,
	                                                const std::string& targetInstanceId,
	                                                const TemporalCoverageSegment& linkCoverage,
	                                                bool& sawUnknown,
	                                                bool& sawUncertain,
	                                                EvaluationContext& evaluationContext)
	{
		TemporalEffectiveWindow linkWindow;

		linkWindow.start.parsed = true;
		linkWindow.start.seconds = linkCoverage.startSeconds;
		linkWindow.start.hasTime = true;
		linkWindow.end.parsed = true;
		linkWindow.end.seconds = linkCoverage.endSeconds;
		linkWindow.end.hasTime = true;
		linkWindow.instant = false;

		std::vector<TemporalCoverageSegment> sourceSegments;
		std::vector<TemporalCoverageSegment> targetSegments;

		if(!BuildEntityExistenceCoverage(ruleSet, sourceEntityId, sourceInstanceId, linkWindow, sourceSegments, evaluationContext)
			|| !BuildEntityExistenceCoverage(ruleSet, targetEntityId, targetInstanceId, linkWindow, targetSegments, evaluationContext))
		{
			sawUnknown = true;

			return false;
		}

		for(const TemporalCoverageSegment& source : sourceSegments)
		{
			if(source.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				continue;
			}

			for(const TemporalCoverageSegment& target : targetSegments)
			{
				if(target.knowledgeState == Attribute::KnowledgeState::Certain
					&& RawTemporalIntervalsOverlap(source.startSeconds, source.endSeconds, target.startSeconds, target.endSeconds))
				{
					return true;
				}
			}
		}

		auto summarize = [&](const std::vector<TemporalCoverageSegment>& segments) {
			if(!HasCompleteCoverage(segments, linkCoverage.startSeconds, linkCoverage.endSeconds))
			{
				sawUnknown = true;
			}

			for(const TemporalCoverageSegment& segment : segments)
			{
				if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
				{
					sawUnknown = true;
				}
				else if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					sawUncertain = true;
				}
			}
		};

		summarize(sourceSegments);
		summarize(targetSegments);

		return false;
	}

	ResolvedValue EvaluateIsMemberOfAtResult(const RuleSet& ruleSet,
	                                         const std::shared_ptr<Instance>& sourceContext,
	                                         const ResolvedInstanceReference& target,
	                                         const ResolvedRelationshipScope& relationship,
	                                         const TemporalEffectiveWindow& window,
	                                         EvaluationContext& evaluationContext)
	{
		if(!target.supported || !relationship.supported)
		{
			return ResolvedValue{false, false, ""};
		}

		if(!target.known)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(target.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		if(!relationship.known)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(relationship.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		if(sourceContext == nullptr)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_source_unresolved",
			              "IsMemberOfAt could not resolve the current source scope.",
			              relationship.relationshipId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		std::shared_ptr<Relationship> relationshipDefinition = FindRelationshipById(ruleSet, relationship.relationshipId);

		if(relationshipDefinition == nullptr)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_not_found",
			              "IsMemberOfAt relationship argument does not resolve to a known relationship.",
			              relationship.relationshipId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sourceContext->entityId != relationshipDefinition->sourceEntity)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_source_unresolved",
			              "IsMemberOfAt source scope does not match the relationship source entity.",
			              sourceContext->entityId + ":" + relationshipDefinition->sourceEntity);

			return UnknownValue(Attribute::BOOLEAN);
		}

		std::string targetEntityId;
		std::shared_ptr<Instance> targetInstance = FindInstanceInAnyEntity(ruleSet, target.instanceId, targetEntityId);

		if(targetInstance == nullptr)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_target_unresolved",
			              "IsMemberOfAt target argument does not resolve to an instance.",
			              target.instanceId);

			return UnknownValue(Attribute::BOOLEAN);
		}

		if(targetEntityId != relationshipDefinition->targetEntity)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_target_type",
			              "IsMemberOfAt target instance does not match the relationship target entity.",
			              targetEntityId + ":" + relationshipDefinition->targetEntity);

			return UnknownValue(Attribute::BOOLEAN);
		}

		std::vector<TemporalCoverageSegment> relationshipCoverageSegments;

		if(!BuildRelationshipCoverage(ruleSet, relationship.relationshipId, sourceContext->instanceId, window, relationshipCoverageSegments, evaluationContext))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(RelationshipCoverageHasConflict(relationship.relationshipId, sourceContext->instanceId, relationshipCoverageSegments, evaluationContext))
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		std::vector<TemporalCoverageSegment> linkCoverageSegments;

		for(const RuleSet::RelationshipLinkHistorySegment* segment : ruleSet.GetRelationshipLinkHistorySegments(relationship.relationshipId, sourceContext->instanceId, target.instanceId))
		{
			if(segment == nullptr)
			{
				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildHistoryCoverageSegment(segment->effectiveStart,
			                                   segment->effectiveEnd,
			                                   segment->knowledgeState,
			                                   "relationship:" + relationship.relationshipId + ":" + sourceContext->instanceId + ":" + target.instanceId,
			                                   window,
			                                   coverage,
			                                   evaluationContext))
			{
				return UnknownValue(Attribute::BOOLEAN);
			}

			if(coverage.endSeconds > coverage.startSeconds)
			{
				linkCoverageSegments.push_back(coverage);
			}
		}

		if(linkCoverageSegments.empty())
		{
			return EvaluateNoLinkRelationshipCoverageResult(relationshipCoverageSegments, window);
		}

		for(const TemporalCoverageSegment& left : linkCoverageSegments)
		{
			if(left.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				continue;
			}

			for(const TemporalCoverageSegment& right : linkCoverageSegments)
			{
				if(right.knowledgeState == Attribute::KnowledgeState::Unknown
					&& RawTemporalIntervalsOverlap(left.startSeconds, left.endSeconds, right.startSeconds, right.endSeconds))
				{
					AddDiagnostic(evaluationContext,
					              "temporal_relationship_conflict",
					              "IsMemberOfAt found contradictory certain and unknown link history for the same relationship member.",
					              relationship.relationshipId + ":" + sourceContext->instanceId + ":" + target.instanceId);

					return UnknownValue(Attribute::BOOLEAN);
				}
			}
		}

		bool sawUnknown = false;
		bool sawUncertain = false;

		for(const TemporalCoverageSegment& coverage : linkCoverageSegments)
		{
			if(coverage.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				bool entityUnknown = false;
				bool entityUncertain = false;

				if(EntityExistenceCoverageAllowsCertainOverlap(ruleSet,
				                                               relationshipDefinition->sourceEntity,
				                                               sourceContext->instanceId,
				                                               relationshipDefinition->targetEntity,
				                                               target.instanceId,
				                                               coverage,
				                                               entityUnknown,
				                                               entityUncertain,
				                                               evaluationContext))
				{
					return BooleanValue(true);
				}

				sawUnknown = sawUnknown || entityUnknown;
				sawUncertain = sawUncertain || entityUncertain;
			}
			else if(coverage.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;
			}
			else if(coverage.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(sawUnknown)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		return UnknownValue(Attribute::BOOLEAN);
	}

	ResolvedValue UnknownOrUncertainTemporalRelationshipTime(Attribute::KnowledgeState knowledgeState)
	{
		if(knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		return UnknownValue(Attribute::BOOLEAN);
	}

	ResolvedValue EvaluateIsMemberOfAtExpression(const RuleSet& ruleSet,
	                                             const std::shared_ptr<Instance>& context,
	                                             const ParsedFunctionCall& call,
	                                             EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 3 && call.arguments.size() != 4)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              "IsMemberOfAt requires target, relationship, and time arguments, with an optional alias argument.",
			              "IsMemberOfAt");

			return UnknownValue(Attribute::BOOLEAN);
		}

		const size_t relationshipIndex = call.arguments.size() == 4 ? 2 : 1;
		const size_t timeIndex = call.arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedInstanceReference target = ResolveTargetInstanceReference(ruleSet, context, call.arguments[0], evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgument(ruleSet, sourceContext, call.arguments[relationshipIndex], evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[timeIndex], evaluationContext, instant, timeKnowledge))
		{
			if(timeKnowledge == Attribute::KnowledgeState::Unknown || timeKnowledge == Attribute::KnowledgeState::Uncertain)
			{
				return UnknownOrUncertainTemporalRelationshipTime(timeKnowledge);
			}

			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_time",
			              "IsMemberOfAt time argument must resolve to a date or date-time value.",
			              call.arguments[timeIndex]);

			return UnknownValue(Attribute::BOOLEAN);
		}

		ResolvedValue value = EvaluateIsMemberOfAtResult(ruleSet,
		                                                 sourceContext,
		                                                 target,
		                                                 relationship,
		                                                 BuildTemporalEffectiveWindow(instant),
		                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain
			&& value.known
			&& value.knowledgeState == Attribute::KnowledgeState::Certain)
		{
			value.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}

		return value;
	}

	ResolvedValue EvaluateIsMemberOfAtNode(const RuleSet& ruleSet,
	                                       const std::shared_ptr<Instance>& context,
	                                       const json& arguments,
	                                       EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || (arguments.size() != 3 && arguments.size() != 4))
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              "IsMemberOfAt requires target, relationship, and time arguments, with an optional alias argument.",
			              "IsMemberOfAt");

			return UnknownValue(Attribute::BOOLEAN);
		}

		const size_t relationshipIndex = arguments.size() == 4 ? 2 : 1;
		const size_t timeIndex = arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedInstanceReference target = ResolveTargetInstanceReferenceNode(ruleSet, context, arguments.at(0), evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgumentNode(ruleSet, sourceContext, arguments.at(relationshipIndex), evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(timeIndex), evaluationContext, instant, timeKnowledge))
		{
			if(timeKnowledge == Attribute::KnowledgeState::Unknown || timeKnowledge == Attribute::KnowledgeState::Uncertain)
			{
				return UnknownOrUncertainTemporalRelationshipTime(timeKnowledge);
			}

			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_time",
			              "IsMemberOfAt time argument must resolve to a date or date-time value.",
			              FunctionArgumentValue(arguments.at(timeIndex)));

			return UnknownValue(Attribute::BOOLEAN);
		}

		ResolvedValue value = EvaluateIsMemberOfAtResult(ruleSet,
		                                                 sourceContext,
		                                                 target,
		                                                 relationship,
		                                                 BuildTemporalEffectiveWindow(instant),
		                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain
			&& value.known
			&& value.knowledgeState == Attribute::KnowledgeState::Certain)
		{
			value.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}

		return value;
	}

	struct TemporalForScopeTraversal
	{
		bool supported = true;
		bool known = true;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Certain;
		std::vector<std::shared_ptr<Instance>> instances;
	};

	void ApplyTemporalForScopeMembershipState(TemporalForScopeTraversal& traversal,
	                                         Attribute::KnowledgeState knowledgeState)
	{
		if(knowledgeState == Attribute::KnowledgeState::Unknown)
		{
			traversal.known = false;
			traversal.knowledgeState = Attribute::KnowledgeState::Unknown;
		}
		else if(knowledgeState == Attribute::KnowledgeState::Uncertain && traversal.known)
		{
			traversal.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}
	}

	bool EntityExistenceCoverageAllowsWindowOverlap(const RuleSet& ruleSet,
	                                               const std::string& entityId,
	                                               const std::string& instanceId,
	                                               const TemporalEffectiveWindow& window,
	                                               bool& sawUnknown,
	                                               bool& sawUncertain,
	                                               EvaluationContext& evaluationContext)
	{
		std::vector<TemporalCoverageSegment> segments;

		if(!BuildEntityExistenceCoverage(ruleSet, entityId, instanceId, window, segments, evaluationContext))
		{
			sawUnknown = true;

			return false;
		}

		for(const TemporalCoverageSegment& segment : segments)
		{
			if(segment.knowledgeState == Attribute::KnowledgeState::Certain
				&& RawTemporalIntervalsOverlap(segment.startSeconds, segment.endSeconds, window.start.seconds, window.end.seconds))
			{
				return true;
			}
		}

		if(!HasCompleteCoverage(segments, window.start.seconds, window.end.seconds))
		{
			sawUnknown = true;
		}

		for(const TemporalCoverageSegment& segment : segments)
		{
			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;
			}
			else if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		return false;
	}

	bool TemporalForScopeCoverageIsCertainlyComplete(const std::vector<TemporalCoverageSegment>& coverageSegments,
	                                                const TemporalEffectiveWindow& window,
	                                                bool& sawUnknown,
	                                                bool& sawUncertain)
	{
		std::vector<TemporalCoverageSegment> certainCoverageSegments;

		for(const TemporalCoverageSegment& segment : coverageSegments)
		{
			if(segment.knowledgeState == Attribute::KnowledgeState::Certain)
			{
				certainCoverageSegments.push_back(segment);
			}
			else if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;
			}
			else if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(HasCompleteCoverage(certainCoverageSegments, window.start.seconds, window.end.seconds))
		{
			return true;
		}

		if(coverageSegments.empty() || !HasCompleteCoverage(coverageSegments, window.start.seconds, window.end.seconds))
		{
			sawUnknown = true;
		}

		return false;
	}

	TemporalForScopeTraversal ResolveForScopeAtTraversal(const RuleSet& ruleSet,
	                                                     const std::shared_ptr<Instance>& sourceContext,
	                                                     const ResolvedRelationshipScope& relationship,
	                                                     const TemporalEffectiveWindow& window,
	                                                     EvaluationContext& evaluationContext)
	{
		TemporalForScopeTraversal traversal;

		if(!relationship.supported)
		{
			traversal.supported = false;
			traversal.known = false;

			return traversal;
		}

		if(!relationship.known)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

			return traversal;
		}

		if(relationship.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);

			return traversal;
		}

		if(sourceContext == nullptr)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_source_unresolved",
			              "ForScopeAt could not resolve the current source scope.",
			              relationship.relationshipId);
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

			return traversal;
		}

		std::shared_ptr<Relationship> relationshipDefinition = FindRelationshipById(ruleSet, relationship.relationshipId);

		if(relationshipDefinition == nullptr)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_not_found",
			              "ForScopeAt relationship argument does not resolve to a known relationship.",
			              relationship.relationshipId);
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

			return traversal;
		}

		if(sourceContext->entityId != relationshipDefinition->sourceEntity)
		{
			AddDiagnostic(evaluationContext,
			              "temporal_relationship_source_unresolved",
			              "ForScopeAt source scope does not match the relationship source entity.",
			              sourceContext->entityId + ":" + relationshipDefinition->sourceEntity);
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

			return traversal;
		}

		std::vector<TemporalCoverageSegment> relationshipCoverageSegments;

		if(!BuildRelationshipCoverage(ruleSet, relationship.relationshipId, sourceContext->instanceId, window, relationshipCoverageSegments, evaluationContext))
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

			return traversal;
		}

		if(RelationshipCoverageHasConflict(relationship.relationshipId, sourceContext->instanceId, relationshipCoverageSegments, evaluationContext))
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

			return traversal;
		}

		struct LinkCoverage
		{
			std::string targetInstanceId;
			TemporalCoverageSegment coverage;
		};

		std::vector<LinkCoverage> linkCoverages;

		for(const RuleSet::RelationshipLinkHistorySegment* segment : ruleSet.GetRelationshipLinkHistorySegments(relationship.relationshipId, sourceContext->instanceId))
		{
			if(segment == nullptr)
			{
				continue;
			}

			if(!segment->targetEntityId.empty() && segment->targetEntityId != relationshipDefinition->targetEntity)
			{
				AddDiagnostic(evaluationContext,
				              "temporal_relationship_target_type",
				              "ForScopeAt link history target entity does not match the relationship target entity.",
				              segment->targetEntityId + ":" + relationshipDefinition->targetEntity);
				ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

				continue;
			}

			TemporalCoverageSegment coverage;

			if(!TryBuildHistoryCoverageSegment(segment->effectiveStart,
			                                   segment->effectiveEnd,
			                                   segment->knowledgeState,
			                                   "relationship:" + relationship.relationshipId + ":" + sourceContext->instanceId + ":" + segment->targetInstanceId,
			                                   window,
			                                   coverage,
			                                   evaluationContext))
			{
				ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

				return traversal;
			}

			if(coverage.endSeconds > coverage.startSeconds)
			{
				linkCoverages.push_back(LinkCoverage{segment->targetInstanceId, coverage});
			}
		}

		for(size_t leftIndex = 0; leftIndex < linkCoverages.size(); ++leftIndex)
		{
			const LinkCoverage& left = linkCoverages.at(leftIndex);

			if(left.coverage.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				continue;
			}

			for(size_t rightIndex = 0; rightIndex < linkCoverages.size(); ++rightIndex)
			{
				const LinkCoverage& right = linkCoverages.at(rightIndex);

				if(left.targetInstanceId == right.targetInstanceId
					&& right.coverage.knowledgeState == Attribute::KnowledgeState::Unknown
					&& RawTemporalIntervalsOverlap(left.coverage.startSeconds,
					                               left.coverage.endSeconds,
					                               right.coverage.startSeconds,
					                               right.coverage.endSeconds))
				{
					AddDiagnostic(evaluationContext,
					              "temporal_relationship_conflict",
					              "ForScopeAt found contradictory certain and unknown link history for the same relationship member.",
					              relationship.relationshipId + ":" + sourceContext->instanceId + ":" + left.targetInstanceId);
					ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);

					return traversal;
				}
			}
		}

		bool sawUnknownMembership = false;
		bool sawUncertainMembership = false;

		for(const LinkCoverage& linkCoverage : linkCoverages)
		{
			if(linkCoverage.coverage.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknownMembership = true;

				continue;
			}

			if(linkCoverage.coverage.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertainMembership = true;

				continue;
			}

			bool entityUnknown = false;
			bool entityUncertain = false;

			if(!EntityExistenceCoverageAllowsCertainOverlap(ruleSet,
			                                                relationshipDefinition->sourceEntity,
			                                                sourceContext->instanceId,
			                                                relationshipDefinition->targetEntity,
			                                                linkCoverage.targetInstanceId,
			                                                linkCoverage.coverage,
			                                                entityUnknown,
			                                                entityUncertain,
			                                                evaluationContext))
			{
				sawUnknownMembership = sawUnknownMembership || entityUnknown;
				sawUncertainMembership = sawUncertainMembership || entityUncertain;

				continue;
			}

			std::shared_ptr<Instance> targetInstance = FindInstance(ruleSet, relationshipDefinition->targetEntity, linkCoverage.targetInstanceId);

			if(targetInstance == nullptr)
			{
				AddDiagnostic(evaluationContext,
				              "temporal_relationship_target_unresolved",
				              "ForScopeAt target link history does not resolve to an instance.",
				              linkCoverage.targetInstanceId);
				sawUnknownMembership = true;

				continue;
			}

			PushUniqueInstance(traversal.instances, targetInstance);
		}

		bool coverageUnknown = false;
		bool coverageUncertain = false;
		const bool relationshipCertainComplete = TemporalForScopeCoverageIsCertainlyComplete(relationshipCoverageSegments,
		                                                                                    window,
		                                                                                    coverageUnknown,
		                                                                                    coverageUncertain);

		if(!relationshipCertainComplete)
		{
			sawUnknownMembership = sawUnknownMembership || coverageUnknown;
			sawUncertainMembership = sawUncertainMembership || coverageUncertain;
		}

		bool sourceUnknown = false;
		bool sourceUncertain = false;

		if(!EntityExistenceCoverageAllowsWindowOverlap(ruleSet,
		                                               relationshipDefinition->sourceEntity,
		                                               sourceContext->instanceId,
		                                               window,
		                                               sourceUnknown,
		                                               sourceUncertain,
		                                               evaluationContext))
		{
			sawUnknownMembership = sawUnknownMembership || sourceUnknown;
			sawUncertainMembership = sawUncertainMembership || sourceUncertain;
		}

		if(sawUnknownMembership)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Unknown);
		}
		else if(sawUncertainMembership)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return traversal;
	}

	ResolvedValue CollectionFromTemporalTraversal(const TemporalForScopeTraversal& traversal)
	{
		ResolvedValue collection;

		collection.supported = traversal.supported;
		collection.known = traversal.known;
		collection.knowledgeState = traversal.knowledgeState;
		collection.isCollection = true;

		return collection;
	}

	ResolvedValue UnknownOrUncertainTemporalForScopeValue(Attribute::KnowledgeState knowledgeState)
	{
		if(knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::UNKNOWN};
		}

		return UnknownValue(Attribute::UNKNOWN);
	}

	ResolvedValue EvaluateForScopeAtTraversalValue(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const TemporalForScopeTraversal& traversal,
	                                              const std::string& scope,
	                                              const std::string& expression,
	                                              EvaluationContext& evaluationContext)
	{
		if(!traversal.supported)
		{
			return ResolvedValue{false, false, ""};
		}

		if(traversal.instances.size() > 1)
		{
			AddDiagnostic(evaluationContext, "for_scope_cardinality", "ForScopeAt produced multiple related instances for a scalar expression.", scope);

			return ResolvedValue{false, false, ""};
		}

		if(!traversal.known)
		{
			return UnknownValue(Attribute::UNKNOWN);
		}

		if(traversal.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::UNKNOWN};
		}

		if(traversal.instances.empty())
		{
			return UnknownValue(Attribute::UNKNOWN);
		}

		evaluationContext.scopeStack.push_back(context);

		ResolvedValue value = ResolveExpressionArgument(ruleSet, traversal.instances.front(), expression, evaluationContext);

		evaluationContext.scopeStack.pop_back();

		return value;
	}

	ResolvedValue EvaluateForScopeAtTraversalCollection(const RuleSet& ruleSet,
	                                                   const std::shared_ptr<Instance>& context,
	                                                   const TemporalForScopeTraversal& traversal,
	                                                   const std::string& expression,
	                                                   EvaluationContext& evaluationContext)
	{
		ResolvedValue collection = CollectionFromTemporalTraversal(traversal);

		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.known || collection.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return collection;
		}

		collection.collectionValues.reserve(traversal.instances.size());

		for(const std::shared_ptr<Instance>& instance : traversal.instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveExpressionArgument(ruleSet, instance, expression, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(collection.valueType == Attribute::UNKNOWN && value.valueType != Attribute::UNKNOWN)
			{
				collection.valueType = value.valueType;
			}

			collection.collectionValues.push_back(ScopedValue{
				instance,
				value.supported,
				value.known,
				value.value,
				value.knowledgeState,
				value.valueType
			});
		}

		return collection;
	}

	ResolvedValue EvaluateForScopeAtExpression(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const ParsedFunctionCall& call,
	                                           EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 3 && call.arguments.size() != 4)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              "ForScopeAt requires relationship, time, and expression arguments, with an optional alias argument.",
			              "ForScopeAt");

			return ResolvedValue{false, false, ""};
		}

		const size_t timeIndex = call.arguments.size() == 4 ? 2 : 1;
		const size_t expressionIndex = call.arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgument(ruleSet, sourceContext, call.arguments[0], evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[timeIndex], evaluationContext, instant, timeKnowledge))
		{
			if(timeKnowledge == Attribute::KnowledgeState::Unknown || timeKnowledge == Attribute::KnowledgeState::Uncertain)
			{
				return WithRelationshipTrace(UnknownOrUncertainTemporalForScopeValue(timeKnowledge), relationship.relationshipId);
			}

			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_time",
			              "ForScopeAt time argument must resolve to a date or date-time value.",
			              call.arguments[timeIndex]);

			return WithRelationshipTrace(UnknownValue(Attribute::UNKNOWN), relationship.relationshipId);
		}

		TemporalForScopeTraversal traversal = ResolveForScopeAtTraversal(ruleSet,
		                                                                 sourceContext,
		                                                                 relationship,
		                                                                 BuildTemporalEffectiveWindow(instant),
		                                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return WithRelationshipTrace(EvaluateForScopeAtTraversalValue(ruleSet,
		                                                              context,
		                                                              traversal,
		                                                              RemoveMatchingQuotes(call.arguments[0]),
		                                                              call.arguments[expressionIndex],
		                                                              evaluationContext),
		                             relationship.relationshipId);
	}

	ResolvedValue EvaluateForScopeAtCollectionExpression(const RuleSet& ruleSet,
	                                                     const std::shared_ptr<Instance>& context,
	                                                     const ParsedFunctionCall& call,
	                                                     EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 3 && call.arguments.size() != 4)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              "ForScopeAt collection expression requires relationship, time, and expression arguments, with an optional alias argument.",
			              "ForScopeAt");

			return ResolvedValue{false, false, ""};
		}

		const size_t timeIndex = call.arguments.size() == 4 ? 2 : 1;
		const size_t expressionIndex = call.arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgument(ruleSet, sourceContext, call.arguments[0], evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[timeIndex], evaluationContext, instant, timeKnowledge))
		{
			TemporalForScopeTraversal traversal;

			ApplyTemporalForScopeMembershipState(traversal,
			                                     timeKnowledge == Attribute::KnowledgeState::Uncertain
				                                     ? Attribute::KnowledgeState::Uncertain
				                                     : Attribute::KnowledgeState::Unknown);

			if(timeKnowledge != Attribute::KnowledgeState::Unknown && timeKnowledge != Attribute::KnowledgeState::Uncertain)
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_relationship_time",
				              "ForScopeAt time argument must resolve to a date or date-time value.",
				              call.arguments[timeIndex]);
			}

			return WithRelationshipTrace(CollectionFromTemporalTraversal(traversal), relationship.relationshipId);
		}

		TemporalForScopeTraversal traversal = ResolveForScopeAtTraversal(ruleSet,
		                                                                 sourceContext,
		                                                                 relationship,
		                                                                 BuildTemporalEffectiveWindow(instant),
		                                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return WithRelationshipTrace(EvaluateForScopeAtTraversalCollection(ruleSet,
		                                                                   context,
		                                                                   traversal,
		                                                                   call.arguments[expressionIndex],
		                                                                   evaluationContext),
		                             relationship.relationshipId);
	}

	ResolvedValue EvaluateForScopeAtNodeValue(const RuleSet& ruleSet,
	                                          const std::shared_ptr<Instance>& context,
	                                          const json& expressionNode,
	                                          const TemporalForScopeTraversal& traversal,
	                                          const std::string& scope,
	                                          EvaluationContext& evaluationContext)
	{
		if(!traversal.supported)
		{
			return ResolvedValue{false, false, ""};
		}

		if(traversal.instances.size() > 1)
		{
			AddDiagnostic(evaluationContext, "for_scope_cardinality", "ForScopeAt produced multiple related instances for a scalar expression.", scope);

			return ResolvedValue{false, false, ""};
		}

		if(!traversal.known)
		{
			return UnknownValue(Attribute::UNKNOWN);
		}

		if(traversal.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::UNKNOWN};
		}

		if(traversal.instances.empty())
		{
			return UnknownValue(Attribute::UNKNOWN);
		}

		evaluationContext.scopeStack.push_back(context);

		ResolvedValue value = ResolveScopedConditionArgumentNode(ruleSet, traversal.instances.front(), expressionNode, evaluationContext);

		evaluationContext.scopeStack.pop_back();

		return value;
	}

	ResolvedValue EvaluateForScopeAtNodeCollection(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const json& expressionNode,
	                                              const TemporalForScopeTraversal& traversal,
	                                              EvaluationContext& evaluationContext)
	{
		ResolvedValue collection = CollectionFromTemporalTraversal(traversal);

		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.known || collection.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return collection;
		}

		collection.collectionValues.reserve(traversal.instances.size());

		for(const std::shared_ptr<Instance>& instance : traversal.instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveScopedConditionArgumentNode(ruleSet, instance, expressionNode, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(collection.valueType == Attribute::UNKNOWN && value.valueType != Attribute::UNKNOWN)
			{
				collection.valueType = value.valueType;
			}

			collection.collectionValues.push_back(ScopedValue{
				instance,
				value.supported,
				value.known,
				value.value,
				value.knowledgeState,
				value.valueType
			});
		}

		return collection;
	}

	ResolvedValue EvaluateForScopeAtNode(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const json& arguments,
	                                     EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || (arguments.size() != 3 && arguments.size() != 4))
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              "ForScopeAt expression requires relationship, time, and expression arguments, with an optional alias argument.",
			              "ForScopeAt");

			return ResolvedValue{false, false, ""};
		}

		const size_t timeIndex = arguments.size() == 4 ? 2 : 1;
		const size_t expressionIndex = arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgumentNode(ruleSet, sourceContext, arguments.at(0), evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(timeIndex), evaluationContext, instant, timeKnowledge))
		{
			if(timeKnowledge == Attribute::KnowledgeState::Unknown || timeKnowledge == Attribute::KnowledgeState::Uncertain)
			{
				return WithRelationshipTrace(UnknownOrUncertainTemporalForScopeValue(timeKnowledge), relationship.relationshipId);
			}

			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_time",
			              "ForScopeAt time argument must resolve to a date or date-time value.",
			              FunctionArgumentValue(arguments.at(timeIndex)));

			return WithRelationshipTrace(UnknownValue(Attribute::UNKNOWN), relationship.relationshipId);
		}

		TemporalForScopeTraversal traversal = ResolveForScopeAtTraversal(ruleSet,
		                                                                 sourceContext,
		                                                                 relationship,
		                                                                 BuildTemporalEffectiveWindow(instant),
		                                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return WithRelationshipTrace(EvaluateForScopeAtNodeValue(ruleSet,
		                                                         context,
		                                                         arguments.at(expressionIndex),
		                                                         traversal,
		                                                         FunctionArgumentValue(arguments.at(0)),
		                                                         evaluationContext),
		                             relationship.relationshipId);
	}

	ResolvedValue EvaluateForScopeAtCollectionNode(const RuleSet& ruleSet,
	                                               const std::shared_ptr<Instance>& context,
	                                               const json& arguments,
	                                               EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || (arguments.size() != 3 && arguments.size() != 4))
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              "ForScopeAt collection expression requires relationship, time, and expression arguments, with an optional alias argument.",
			              "ForScopeAt");

			return ResolvedValue{false, false, ""};
		}

		const size_t timeIndex = arguments.size() == 4 ? 2 : 1;
		const size_t expressionIndex = arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgumentNode(ruleSet, sourceContext, arguments.at(0), evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(timeIndex), evaluationContext, instant, timeKnowledge))
		{
			TemporalForScopeTraversal traversal;

			ApplyTemporalForScopeMembershipState(traversal,
			                                     timeKnowledge == Attribute::KnowledgeState::Uncertain
				                                     ? Attribute::KnowledgeState::Uncertain
				                                     : Attribute::KnowledgeState::Unknown);

			if(timeKnowledge != Attribute::KnowledgeState::Unknown && timeKnowledge != Attribute::KnowledgeState::Uncertain)
			{
				AddDiagnostic(evaluationContext,
				              "unsupported_temporal_relationship_time",
				              "ForScopeAt time argument must resolve to a date or date-time value.",
				              FunctionArgumentValue(arguments.at(timeIndex)));
			}

			return WithRelationshipTrace(CollectionFromTemporalTraversal(traversal), relationship.relationshipId);
		}

		TemporalForScopeTraversal traversal = ResolveForScopeAtTraversal(ruleSet,
		                                                                 sourceContext,
		                                                                 relationship,
		                                                                 BuildTemporalEffectiveWindow(instant),
		                                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return WithRelationshipTrace(EvaluateForScopeAtNodeCollection(ruleSet,
		                                                              context,
		                                                              arguments.at(expressionIndex),
		                                                              traversal,
		                                                              evaluationContext),
		                             relationship.relationshipId);
	}

	bool IsTemporalUniversalQuantifierName(const std::string& functionName)
	{
		return SessionModelTemporalFunctions::IsTemporalUniversalQuantifierName(functionName);
	}

	ResolvedValue UnknownOrUncertainTemporalUniversalQuantifierValue(Attribute::KnowledgeState knowledgeState)
	{
		if(knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		return UnknownValue(Attribute::BOOLEAN);
	}

	ResolvedValue EvaluateTemporalUniversalQuantifierTraversal(const RuleSet& ruleSet,
	                                                          const std::shared_ptr<Instance>& context,
	                                                          const TemporalForScopeTraversal& traversal,
	                                                          const std::string& conditionExpression,
	                                                          EvaluationContext& evaluationContext)
	{
		if(!traversal.supported)
		{
			return ResolvedValue{false, false, ""};
		}

		bool sawUnknown = !traversal.known;
		bool sawUncertain = traversal.known && traversal.knowledgeState == Attribute::KnowledgeState::Uncertain;

		for(const std::shared_ptr<Instance>& instance : traversal.instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveExpressionArgument(ruleSet, instance, conditionExpression, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!value.supported)
			{
				return value;
			}

			if(!value.known)
			{
				if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					sawUncertain = true;
				}
				else
				{
					sawUnknown = true;
				}

				continue;
			}

			if(value.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			bool boolValue = false;

			if(!TryParseBoolean(value.value, boolValue))
			{
				boolValue = false;
			}

			if(!boolValue)
			{
				if(value.knowledgeState == Attribute::KnowledgeState::Certain)
				{
					return BooleanValue(false);
				}

				sawUncertain = true;

				continue;
			}

			if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(sawUnknown)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(true);
	}

	ResolvedValue EvaluateTemporalUniversalQuantifierNodeTraversal(const RuleSet& ruleSet,
	                                                              const std::shared_ptr<Instance>& context,
	                                                              const TemporalForScopeTraversal& traversal,
	                                                              const json& conditionNode,
	                                                              EvaluationContext& evaluationContext)
	{
		if(!traversal.supported)
		{
			return ResolvedValue{false, false, ""};
		}

		bool sawUnknown = !traversal.known;
		bool sawUncertain = traversal.known && traversal.knowledgeState == Attribute::KnowledgeState::Uncertain;

		for(const std::shared_ptr<Instance>& instance : traversal.instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveScopedConditionArgumentNode(ruleSet, instance, conditionNode, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!value.supported)
			{
				return value;
			}

			if(!value.known)
			{
				if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					sawUncertain = true;
				}
				else
				{
					sawUnknown = true;
				}

				continue;
			}

			if(value.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;

				continue;
			}

			bool boolValue = false;

			if(!TryParseBoolean(value.value, boolValue))
			{
				boolValue = false;
			}

			if(!boolValue)
			{
				if(value.knowledgeState == Attribute::KnowledgeState::Certain)
				{
					return BooleanValue(false);
				}

				sawUncertain = true;

				continue;
			}

			if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(sawUnknown)
		{
			return UnknownValue(Attribute::BOOLEAN);
		}

		if(sawUncertain)
		{
			return BooleanValue(false, Attribute::KnowledgeState::Uncertain);
		}

		return BooleanValue(true);
	}

	ResolvedValue EvaluateTemporalScopedQuantifierExpression(const RuleSet& ruleSet,
	                                                         const std::shared_ptr<Instance>& context,
	                                                         const ParsedFunctionCall& call,
	                                                         EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 3 && call.arguments.size() != 4)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              call.name + " requires relationship, time, and condition arguments, with an optional alias argument.",
			              call.name);

			return ResolvedValue{false, false, ""};
		}

		const size_t timeIndex = call.arguments.size() == 4 ? 2 : 1;
		const size_t conditionIndex = call.arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgument(ruleSet, sourceContext, call.arguments[0], evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgument(ruleSet, context, call.arguments[timeIndex], evaluationContext, instant, timeKnowledge))
		{
			if(timeKnowledge == Attribute::KnowledgeState::Unknown || timeKnowledge == Attribute::KnowledgeState::Uncertain)
			{
				return UnknownOrUncertainTemporalUniversalQuantifierValue(timeKnowledge);
			}

			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_time",
			              call.name + " time argument must resolve to a date or date-time value.",
			              call.arguments[timeIndex]);

			return UnknownValue(Attribute::BOOLEAN);
		}

		TemporalForScopeTraversal traversal = ResolveForScopeAtTraversal(ruleSet,
		                                                                 sourceContext,
		                                                                 relationship,
		                                                                 BuildTemporalEffectiveWindow(instant),
		                                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return EvaluateTemporalUniversalQuantifierTraversal(ruleSet,
		                                                    context,
		                                                    traversal,
		                                                    call.arguments[conditionIndex],
		                                                    evaluationContext);
	}

	ResolvedValue EvaluateTemporalScopedQuantifierNode(const RuleSet& ruleSet,
	                                                   const std::shared_ptr<Instance>& context,
	                                                   const std::string& functionName,
	                                                   const json& arguments,
	                                                   EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || (arguments.size() != 3 && arguments.size() != 4))
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_shape",
			              functionName + " requires relationship, time, and condition arguments, with an optional alias argument.",
			              functionName);

			return ResolvedValue{false, false, ""};
		}

		const size_t timeIndex = arguments.size() == 4 ? 2 : 1;
		const size_t conditionIndex = arguments.size() == 4 ? 3 : 2;
		std::shared_ptr<Instance> sourceContext = ResolveRelationshipSourceContext(context, evaluationContext);
		ResolvedRelationshipScope relationship = ResolveTemporalRelationshipScopeArgumentNode(ruleSet, sourceContext, arguments.at(0), evaluationContext);
		TemporalInstant instant;
		Attribute::KnowledgeState timeKnowledge = Attribute::KnowledgeState::Certain;

		if(!TryResolveTemporalInstantArgumentNode(ruleSet, context, arguments.at(timeIndex), evaluationContext, instant, timeKnowledge))
		{
			if(timeKnowledge == Attribute::KnowledgeState::Unknown || timeKnowledge == Attribute::KnowledgeState::Uncertain)
			{
				return UnknownOrUncertainTemporalUniversalQuantifierValue(timeKnowledge);
			}

			AddDiagnostic(evaluationContext,
			              "unsupported_temporal_relationship_time",
			              functionName + " time argument must resolve to a date or date-time value.",
			              FunctionArgumentValue(arguments.at(timeIndex)));

			return UnknownValue(Attribute::BOOLEAN);
		}

		TemporalForScopeTraversal traversal = ResolveForScopeAtTraversal(ruleSet,
		                                                                 sourceContext,
		                                                                 relationship,
		                                                                 BuildTemporalEffectiveWindow(instant),
		                                                                 evaluationContext);

		if(timeKnowledge == Attribute::KnowledgeState::Uncertain)
		{
			ApplyTemporalForScopeMembershipState(traversal, Attribute::KnowledgeState::Uncertain);
		}

		return EvaluateTemporalUniversalQuantifierNodeTraversal(ruleSet,
		                                                        context,
		                                                        traversal,
		                                                        arguments.at(conditionIndex),
		                                                        evaluationContext);
	}

	ResolvedValue EvaluateInstanceNotEqualsExpression(const std::shared_ptr<Instance>& context,
	                                                 const ParsedFunctionCall& call,
	                                                 EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceNotEquals expression requires two arguments.", "InstanceNotEquals");

			return ResolvedValue{false, false, ""};
		}

		const std::string left = ResolveInstanceToken(context, call.arguments[0]);
		const std::string right = ResolveInstanceToken(context, call.arguments[1]);

		return BooleanValue(!left.empty() && !right.empty() && left != right);
	}

	ResolvedValue EvaluateInstanceMaximumExpression(const RuleSet& ruleSet,
	                                                const std::shared_ptr<Instance>& context,
	                                                const ParsedFunctionCall& call,
	                                                EvaluationContext& evaluationContext)
	{
		if(call.arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceMaximum expression requires scope and attribute arguments.", "InstanceMaximum");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string attributeName = RemoveMatchingQuotes(call.arguments[1]);
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);
		bool hasMaximum = false;
		double maximum = 0.0;

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(instance, attributeName);

			if(attribute == nullptr || attribute->IsUnknown())
			{
				continue;
			}

			double value = 0.0;

			if(!TryParseDoubleValue(attribute->GetStringValue(), value))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "InstanceMaximum requires numeric attribute values.", attributeName);

				return ResolvedValue{false, false, ""};
			}

			if(!hasMaximum || value > maximum)
			{
				hasMaximum = true;
				maximum = value;
			}
		}

		if(!hasMaximum)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		return ResolvedValue{true, true, FormatDoubleValue(maximum), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
	}

	ResolvedValue EvaluateInstanceAggregateExpression(const RuleSet& ruleSet,
	                                                  const std::shared_ptr<Instance>& context,
	                                                  const ParsedFunctionCall& call,
	                                                  EvaluationContext& evaluationContext)
	{
		const bool hasCondition = call.name == "InstanceMaximumIf" || call.name == "InstanceMinimumIf" || call.name == "InstanceSumIf";
		const bool isMinimum = call.name == "InstanceMinimum" || call.name == "InstanceMinimumIf";
		const bool isSum = call.name == "InstanceSum" || call.name == "InstanceSumIf";
		const size_t expectedArguments = hasCondition ? 3 : 2;

		if(call.arguments.size() != expectedArguments)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Instance aggregate expression requires scope, attribute, and optional condition arguments.", call.name);

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = RemoveMatchingQuotes(call.arguments[0]);
		const std::string attributeName = RemoveMatchingQuotes(call.arguments[1]);
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);
		bool hasValue = false;
		double result = 0.0;

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			if(hasCondition)
			{
				evaluationContext.scopeStack.push_back(context);

				ResolvedValue condition = ResolveExpressionArgument(ruleSet, instance, call.arguments[2], evaluationContext);

				evaluationContext.scopeStack.pop_back();

				if(!condition.supported)
				{
					return condition;
				}

				bool conditionValue = false;

				if(!condition.known || !TryParseBoolean(condition.value, conditionValue) || !conditionValue)
				{
					continue;
				}
			}

			std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(instance, attributeName);

			if(attribute == nullptr || attribute->IsUnknown())
			{
				continue;
			}

			double value = 0.0;

			if(!TryParseDoubleValue(attribute->GetStringValue(), value))
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value", "Instance aggregate requires numeric attribute values.", attributeName);

				return ResolvedValue{false, false, ""};
			}

			if(isSum)
			{
				result += value;
			}
			else if(!hasValue || (isMinimum ? value < result : value > result))
			{
				result = value;
			}

			hasValue = true;
		}

		if(!hasValue)
		{
			return UnknownValue(Attribute::DOUBLE);
		}

		return ResolvedValue{true, true, FormatDoubleValue(result), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
	}

	ResolvedValue ResolveExpressionArgument(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& context,
	                                        const std::string& argument,
	                                        EvaluationContext& evaluationContext)
	{
		if(IsQuotedString(argument))
		{
			return ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING};
		}

		double numberLiteral = 0.0;

		if(TryParseDoubleValue(argument, numberLiteral))
		{
			return ResolvedValue{true, true, Trim(argument), Attribute::KnowledgeState::Certain, Attribute::DOUBLE};
		}

		bool boolLiteral = false;

		if(TryParseBoolean(argument, boolLiteral))
		{
			return BooleanValue(boolLiteral);
		}

		ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

		if(nestedCall.parsed && IsKnownExpressionName(nestedCall.name))
		{
			if(!nestedCall.valid)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression syntax is invalid.", argument);

				return ResolvedValue{false, false, ""};
			}

			return EvaluateExpressionCallWithTrace(ruleSet, context, nestedCall, evaluationContext);
		}

		std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(context, argument);
		AppendAttributeReadTrace(evaluationContext,
		                         context,
		                         context != nullptr ? context->entityId : std::string(),
		                         context != nullptr ? context->instanceId : std::string(),
		                         attribute != nullptr ? attribute->GetID() : argument,
		                         "",
		                         attribute);

		if(attribute == nullptr)
		{
			return ResolvedValue{true, false, ""};
		}

		if(attribute->IsUnknown())
		{
			return ResolvedValue{
				true,
				false,
				"",
				attribute->GetKnowledgeState(),
				static_cast<Attribute::AttributeType>(attribute->GetType())
			};
		}

		return ResolvedValue{
			true,
			true,
			attribute->GetStringValue(),
			attribute->GetKnowledgeState(),
			static_cast<Attribute::AttributeType>(attribute->GetType())
		};
	}

	ResolvedValue EvaluateConcatenateExpression(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const ParsedFunctionCall& call,
	                                           EvaluationContext& evaluationContext)
	{
		std::string result;

		for(const std::string& argument : call.arguments)
		{
			ResolvedValue value = ResolveExpressionArgument(ruleSet, context, argument, evaluationContext);

			if(!value.supported)
			{
				return value;
			}

			if(!value.known)
			{
				return ResolvedValue{true, false, ""};
			}

			result += value.value;
		}

		return ResolvedValue{true, true, result, Attribute::KnowledgeState::Certain, Attribute::STRING};
	}

	ResolvedValue EvaluateExpressionCall(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const ParsedFunctionCall& call,
	                                     EvaluationContext& evaluationContext)
	{
		if(call.name == "For")
		{
			return EvaluateForExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "ForScope")
		{
			return EvaluateForScopeExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "ForScopeAt")
		{
			return EvaluateForScopeAtExpression(ruleSet, context, call, evaluationContext);
		}

		if(IsTemporalUniversalQuantifierName(call.name))
		{
			return EvaluateTemporalScopedQuantifierExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Count")
		{
			return EvaluateCountExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "CountDistinct")
		{
			return EvaluateCountDistinctExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Sum" || call.name == "Average" || ((call.name == "Maximum" || call.name == "Minimum") && call.arguments.size() == 1 && IsCollectionExpressionName(ParseFunctionCall(call.arguments[0]).name)))
		{
			return EvaluateCollectionAggregateExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Concatenate")
		{
			return EvaluateConcatenateExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InstanceCount")
		{
			return EvaluateInstanceCountExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InstanceCountIf")
		{
			return EvaluateInstanceCountIfExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InstanceValueIf")
		{
			return EvaluateInstanceValueIfExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InferInstance" || call.name == "InferInstanceFor")
		{
			AddDiagnostic(evaluationContext, "infer_instance_invalid_context", "InferInstance and InferInstanceFor are valid only as conclusion-side instance-creation constructs.", call.name);

			return ResolvedValue{false, false, ""};
		}

		if(call.name == "Error" || call.name == "Warning")
		{
			AddDiagnostic(evaluationContext, "validation_invalid_context", "Error and Warning are valid only as conclusion-side validation constructs.", call.name);

			return ResolvedValue{false, false, ""};
		}

		if(IsTemporalFunctionName(call.name))
		{
			return EvaluateTemporalExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InstanceMaximumIf" || call.name == "InstanceMinimum" || call.name == "InstanceMinimumIf" || call.name == "InstanceSum" || call.name == "InstanceSumIf")
		{
			return EvaluateInstanceAggregateExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InstanceMaximum")
		{
			return EvaluateInstanceMaximumExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Not")
		{
			return EvaluateNotExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "CurrentDate" || call.name == "CurrentTime")
		{
			return EvaluateCurrentDateTimeExpression(call, evaluationContext);
		}

		if(call.name == "And")
		{
			return EvaluateAndExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Equals")
		{
			return EvaluateEqualsExpression(ruleSet, context, call, false, evaluationContext);
		}

		if(call.name == "NotEquals")
		{
			return EvaluateEqualsExpression(ruleSet, context, call, true, evaluationContext);
		}

		if(call.name == "Unknown" || call.name == "Uncertain")
		{
			return EvaluateUnknownExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "Exists")
		{
			return EvaluateExistsExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "ExistsScope" || call.name == "ForAll" || call.name == "ForAllScope")
		{
			return EvaluateScopedQuantifierExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "IsMemberOf")
		{
			return EvaluateIsMemberOfExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "InstanceEquals")
		{
			ResolvedValue notEquals = EvaluateInstanceNotEqualsExpression(context, call, evaluationContext);

			if(!notEquals.supported || !notEquals.known)
			{
				return notEquals;
			}

			bool boolValue = false;

			return BooleanValue(TryParseBoolean(notEquals.value, boolValue) && !boolValue, notEquals.knowledgeState);
		}

		if(call.name == "InstanceNotEquals")
		{
			return EvaluateInstanceNotEqualsExpression(context, call, evaluationContext);
		}

		if(call.name == "GreaterThan" || call.name == "GreaterThanEquals" || call.name == "GreaterThanOrEquals" || call.name == "greaterThanOrEqual" || call.name == "LessThan" || call.name == "LessThanEquals" || call.name == "LessThanOrEquals")
		{
			return EvaluateComparisonExpression(ruleSet, context, call, evaluationContext);
		}

		if(call.name == "IsNumber")
		{
			return EvaluateIsNumberExpression(ruleSet, context, call, evaluationContext);
		}

		if(IsScalarFunctionName(call.name))
		{
			std::vector<ResolvedValue> arguments = ResolveExpressionArguments(ruleSet, context, call, evaluationContext);

			return ApplyStaticFunctionReturnType(call.name, arguments, EvaluateScalarFunction(call.name, arguments, evaluationContext));
		}

		AddDiagnostic(evaluationContext, "unsupported_expression_function", "Expression function is not implemented yet.", call.name);

		return ResolvedValue{false, false, ""};
	}

	ExpressionResolution ResolveLiteralExpression(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const std::string& literalValue,
	                                              EvaluationContext& evaluationContext)
	{
		ParsedFunctionCall call = ParseFunctionCall(literalValue);

		if(!call.parsed || !IsKnownExpressionName(call.name))
		{
			return ExpressionResolution{false, ResolvedValue{}};
		}

		if(!call.valid)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression syntax is invalid.", literalValue);

			return ExpressionResolution{true, ResolvedValue{false, false, ""}};
		}

		ResolvedValue value = EvaluateExpressionCallWithTrace(ruleSet, context, call, evaluationContext);

		if(value.supported)
		{
			return ExpressionResolution{true, value};
		}

		return ExpressionResolution{true, ResolvedValue{false, false, ""}};
	}

	ResolvedValue ResolveConclusionValue(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const json& node,
	                                     const std::string& defaultEntityId,
	                                     const std::string& valueMode,
	                                     const std::string& literalValue,
	                                     bool allowAttributeIdFallback,
	                                     EvaluationContext& evaluationContext)
	{
		if((valueMode.empty() || valueMode == "Literal" || valueMode == "Function") && node.is_object())
		{
			if(node.contains("nestedFunction") && node.at("nestedFunction").is_object())
			{
				return EvaluateFunctionNodeWithTrace(ruleSet, context, node.at("nestedFunction"), evaluationContext);
			}

			if(node.contains("functionName"))
			{
				return EvaluateFunctionNodeWithTrace(ruleSet, context, node, evaluationContext);
			}
		}

		if(valueMode.empty() || valueMode == "Literal")
		{
			ExpressionResolution expression = ResolveLiteralExpression(ruleSet, context, literalValue, evaluationContext);

			if(expression.isExpression)
			{
				return expression.value;
			}
		}

		return ResolveValue(context, node, defaultEntityId, valueMode, literalValue, allowAttributeIdFallback, evaluationContext);
	}

	bool AttributeEqualsValue(const std::shared_ptr<Attribute>& attribute, const std::string& value)
	{
		if(attribute == nullptr || attribute->IsUnknown())
		{
			return false;
		}

		ValueVariant converted;

		if(!Attribute::TryConvertValue(value, static_cast<Attribute::AttributeType>(attribute->GetType()), converted))
		{
			return false;
		}

		ValueVariant current = attribute->GetValue();

		if(current.index() != converted.index())
		{
			return false;
		}

		return current == converted;
	}

	bool AttributeGreaterThanValue(const std::shared_ptr<Attribute>& attribute, const std::string& value)
	{
		if(attribute == nullptr || attribute->IsUnknown())
		{
			return false;
		}

		ValueVariant converted;

		if(!Attribute::TryConvertValue(value, static_cast<Attribute::AttributeType>(attribute->GetType()), converted))
		{
			return false;
		}

		ValueVariant current = attribute->GetValue();

		if(std::holds_alternative<int>(current) && std::holds_alternative<int>(converted))
		{
			return std::get<int>(current) > std::get<int>(converted);
		}

		if(std::holds_alternative<double>(current) && std::holds_alternative<double>(converted))
		{
			return std::get<double>(current) > std::get<double>(converted);
		}

		return false;
	}

	bool AttributeLessThanValue(const std::shared_ptr<Attribute>& attribute, const std::string& value)
	{
		if(attribute == nullptr || attribute->IsUnknown())
		{
			return false;
		}

		ValueVariant converted;

		if(!Attribute::TryConvertValue(value, static_cast<Attribute::AttributeType>(attribute->GetType()), converted))
		{
			return false;
		}

		ValueVariant current = attribute->GetValue();

		if(std::holds_alternative<int>(current) && std::holds_alternative<int>(converted))
		{
			return std::get<int>(current) < std::get<int>(converted);
		}

		if(std::holds_alternative<double>(current) && std::holds_alternative<double>(converted))
		{
			return std::get<double>(current) < std::get<double>(converted);
		}

		return false;
	}

	bool AttributeGreaterThanOrEqualValue(const std::shared_ptr<Attribute>& attribute, const std::string& value)
	{
		return AttributeGreaterThanValue(attribute, value) || AttributeEqualsValue(attribute, value);
	}

	NodeResult EvaluateCertainExpression(const std::shared_ptr<Instance>& context, const json& node, EvaluationContext& evaluationContext)
	{
		if(!node.contains("functionArguments") || !node.at("functionArguments").is_array())
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Certain expression requires functionArguments.", node.dump());

			return NodeResult::Unsupported;
		}

		for(const json& argument : node.at("functionArguments"))
		{
			if(!argument.is_object())
			{
				continue;
			}

			const std::string valueMode = GetString(argument, "valueMode");

			if(valueMode != "Attribute")
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_value_mode", "Certain currently supports Attribute arguments only.", valueMode);

				return NodeResult::Unsupported;
			}

			const std::string attributeId = GetString(argument, "attributeId");
			std::string entityId = GetString(argument, "entityId");
			const std::string relationshipId = GetString(argument, "relationshipId");

			if(entityId.empty() && context != nullptr)
			{
				entityId = context->entityId;
			}

			std::shared_ptr<Attribute> attribute = ResolveAttribute(context, entityId, attributeId, relationshipId);
			AppendAttributeReadTrace(evaluationContext,
			                         context,
			                         entityId,
			                         "",
			                         attributeId,
			                         relationshipId,
			                         attribute);

			if(attribute == nullptr || !attribute->IsCertain())
			{
				return NodeResult::False;
			}
		}

		return NodeResult::True;
	}

	ResolvedValue NodeResultValue(NodeResult result)
	{
		if(result == NodeResult::Unsupported)
		{
			return ResolvedValue{false, false, ""};
		}

		return BooleanValue(NodeTruthValue(result), NodeKnowledgeState(result));
	}

	ResolvedValue ResolveFunctionArgumentNode(const RuleSet& ruleSet,
	                                          const std::shared_ptr<Instance>& context,
	                                          const json& argument,
	                                          EvaluationContext& evaluationContext)
	{
		if(!argument.is_object())
		{
			const std::string value = FunctionArgumentValue(argument);

			return ResolvedValue{true, true, value, Attribute::KnowledgeState::Certain, LiteralValueType(value)};
		}

		if(argument.contains("nestedFunction") && argument.at("nestedFunction").is_object())
		{
			return EvaluateFunctionNodeWithTrace(ruleSet, context, argument.at("nestedFunction"), evaluationContext);
		}

		if(argument.contains("functionName"))
		{
			return EvaluateFunctionNodeWithTrace(ruleSet, context, argument, evaluationContext);
		}

		const std::string valueMode = GetString(argument, "valueMode");

		if(valueMode.empty() || valueMode == "Literal")
		{
			const std::string value = GetString(argument, "value");

			return ResolvedValue{true, true, value, Attribute::KnowledgeState::Certain, LiteralValueType(value)};
		}

		if(valueMode == "Attribute")
		{
			std::string attributeId = FirstNonEmpty(argument, {
				"attributeId",
				"valueAttributeId",
				"sourceAttributeId",
				"comparisonAttributeId",
				"attributeValueId",
				"value",
				"attributeName",
				"coreName"
			});

			std::string entityId = FirstNonEmpty(argument, {
				"entityId",
				"valueEntityId",
				"sourceEntityId",
				"comparisonEntityId"
			});

			if(entityId.empty() && context != nullptr)
			{
				entityId = context->entityId;
			}

			const std::string relationshipId = FirstNonEmpty(argument, {
				"relationshipId",
				"valueRelationshipId",
				"sourceRelationshipId",
				"comparisonRelationshipId"
			});

			std::shared_ptr<Attribute> attribute = ResolveAttribute(context, entityId, attributeId, relationshipId);
			AppendAttributeReadTrace(evaluationContext,
			                         context,
			                         entityId,
			                         "",
			                         attributeId,
			                         relationshipId,
			                         attribute);

			if(attribute == nullptr || attribute->IsUnknown())
			{
				if(attribute == nullptr)
				{
					return ResolvedValue{true, false, ""};
				}

				return ResolvedValue{
					true,
					false,
					"",
					attribute->GetKnowledgeState(),
					static_cast<Attribute::AttributeType>(attribute->GetType())
				};
			}

			return ResolvedValue{
				true,
				true,
				attribute->GetStringValue(),
				attribute->GetKnowledgeState(),
				static_cast<Attribute::AttributeType>(attribute->GetType())
			};
		}

		AddDiagnostic(evaluationContext, "unsupported_expression_value_mode", "Expression argument value mode is not implemented yet.", valueMode);

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue ResolveScopedConditionArgumentNode(const RuleSet& ruleSet,
	                                                 const std::shared_ptr<Instance>& context,
	                                                 const json& argument,
	                                                 EvaluationContext& evaluationContext)
	{
		if(argument.is_object())
		{
			if(argument.contains("condition") && argument.at("condition").is_object())
			{
				return NodeResultValue(EvaluateNode(ruleSet, context, argument.at("condition"), evaluationContext));
			}

			const std::string nodeType = GetString(argument, "nodeType");

			if(nodeType == "condition" || nodeType == "group" || nodeType == "expression" || !GetString(argument, "operator").empty())
			{
				return NodeResultValue(EvaluateNode(ruleSet, context, argument, evaluationContext));
			}
		}

		return ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext);
	}

	ResolvedValue EvaluateForScopeNode(const RuleSet& ruleSet,
	                                   const std::shared_ptr<Instance>& context,
	                                   const json& arguments,
	                                   EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() < 2 || arguments.size() > 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "ForScope expression requires scope, optional alias, and expression arguments.", "ForScope");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = FunctionArgumentValue(arguments.at(0));
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, arguments.at(0));
		const size_t expressionIndex = arguments.size() >= 3 ? 2 : 1;
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet,
		                                                                            context,
		                                                                            scope,
		                                                                            IsExplicitRelationshipScopeArgument(arguments.at(0)));

		if(instances.empty())
		{
			return WithRelationshipTrace(ResolvedValue{true, false, ""}, relationshipId);
		}

		if(instances.size() > 1)
		{
			AddDiagnostic(evaluationContext, "for_scope_cardinality", "ForScope produced multiple related instances for a scalar expression.", scope);

			return WithRelationshipTrace(ResolvedValue{false, false, ""}, relationshipId);
		}

		evaluationContext.scopeStack.push_back(context);

		ResolvedValue value = ResolveScopedConditionArgumentNode(ruleSet, instances.front(), arguments.at(expressionIndex), evaluationContext);

		evaluationContext.scopeStack.pop_back();

		return WithRelationshipTrace(value, relationshipId);
	}

	ResolvedValue EvaluateForScopeCollectionNode(const RuleSet& ruleSet,
	                                             const std::shared_ptr<Instance>& context,
	                                             const json& arguments,
	                                             EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() < 2 || arguments.size() > 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "ForScope collection expression requires scope, optional alias, and expression arguments.", "ForScope");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = FunctionArgumentValue(arguments.at(0));
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, arguments.at(0));
		const size_t expressionIndex = arguments.size() >= 3 ? 2 : 1;
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet,
		                                                                            context,
		                                                                            scope,
		                                                                            IsExplicitRelationshipScopeArgument(arguments.at(0)));
		ResolvedValue collection;

		collection.supported = true;
		collection.known = true;
		collection.knowledgeState = Attribute::KnowledgeState::Certain;
		collection.isCollection = true;
		collection.collectionValues.reserve(instances.size());

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue value = ResolveScopedConditionArgumentNode(ruleSet, instance, arguments.at(expressionIndex), evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(collection.valueType == Attribute::UNKNOWN && value.valueType != Attribute::UNKNOWN)
			{
				collection.valueType = value.valueType;
			}

			collection.collectionValues.push_back(ScopedValue{
				instance,
				value.supported,
				value.known,
				value.value,
				value.knowledgeState,
				value.valueType
			});
		}

		collection.traceRelationshipId = relationshipId;

		return collection;
	}

	const json* ResolveCollectionFunctionNode(const json& argument)
	{
		if(argument.is_object() && argument.contains("nestedFunction") && argument.at("nestedFunction").is_object())
		{
			return &argument.at("nestedFunction");
		}

		if(argument.is_object() && argument.contains("functionName"))
		{
			return &argument;
		}

		return nullptr;
	}

	bool IsCollectionFunctionArgument(const json& argument)
	{
		const json* collectionFunction = ResolveCollectionFunctionNode(argument);

		return collectionFunction != nullptr && IsCollectionExpressionName(GetString(*collectionFunction, "functionName"));
	}

	ResolvedValue FilterCollectionByNode(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const ResolvedValue& collection,
	                                     const json& conditionNode,
	                                     EvaluationContext& evaluationContext)
	{
		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.isCollection)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Filter requires a collection argument.", "Filter");

			return ResolvedValue{false, false, ""};
		}

		if(!collection.known)
		{
			return collection;
		}

		ResolvedValue filtered;

		filtered.supported = true;
		filtered.known = true;
		filtered.knowledgeState = collection.knowledgeState;
		filtered.valueType = collection.valueType;
		filtered.isCollection = true;
		filtered.traceRelationshipId = collection.traceRelationshipId;
		filtered.collectionValues.reserve(collection.collectionValues.size());

		bool sawUnknown = false;
		bool sawUncertain = collection.knowledgeState == Attribute::KnowledgeState::Uncertain;

		for(const ScopedValue& item : collection.collectionValues)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue predicate = ResolveScopedConditionArgumentNode(ruleSet, item.sourceInstance, conditionNode, evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!predicate.supported)
			{
				return predicate;
			}

			if(!predicate.known)
			{
				sawUnknown = true;

				continue;
			}

			bool include = false;

			if(!TryParseBoolean(predicate.value, include))
			{
				AddDiagnostic(evaluationContext, "filter_condition_type", "Filter condition must evaluate to a boolean value.", conditionNode.dump());

				return ResolvedValue{false, false, ""};
			}

			if(predicate.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;

				continue;
			}

			if(include)
			{
				filtered.collectionValues.push_back(item);
			}
		}

		if(sawUnknown)
		{
			filtered.known = false;
			filtered.knowledgeState = Attribute::KnowledgeState::Unknown;
			filtered.collectionValues.clear();

			return filtered;
		}

		if(sawUncertain)
		{
			filtered.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}

		return filtered;
	}

	ResolvedValue EvaluateFilterCollectionNode(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const json& arguments,
	                                           EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() != 2)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Filter expression requires collection and condition arguments.", "Filter");

			return ResolvedValue{false, false, ""};
		}

		const json* collectionFunction = ResolveCollectionFunctionNode(arguments.at(0));

		if(collectionFunction == nullptr || !IsCollectionExpressionName(GetString(*collectionFunction, "functionName")))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Filter currently supports ForScope, ForScopeAt, or Filter collection arguments.", arguments.at(0).dump());

			return ResolvedValue{false, false, ""};
		}

		return FilterCollectionByNode(ruleSet,
		                              context,
		                              EvaluateCollectionNodeWithTrace(ruleSet, context, arguments.at(0), evaluationContext),
		                              arguments.at(1),
		                              evaluationContext);
	}

	ResolvedValue EvaluateCollectionNode(const RuleSet& ruleSet,
	                                     const std::shared_ptr<Instance>& context,
	                                     const json& argument,
	                                     EvaluationContext& evaluationContext)
	{
		const json* collectionFunction = ResolveCollectionFunctionNode(argument);

		if(collectionFunction == nullptr)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Collection argument must be a ForScope, ForScopeAt, or Filter function.", argument.dump());

			return ResolvedValue{false, false, ""};
		}

		const std::string functionName = GetString(*collectionFunction, "functionName");
		const json emptyArguments = json::array();
		const json& arguments = collectionFunction->contains("functionArguments") && collectionFunction->at("functionArguments").is_array()
			? collectionFunction->at("functionArguments")
			: emptyArguments;

		if(functionName == "ForScope")
		{
			return EvaluateForScopeCollectionNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "ForScopeAt")
		{
			return EvaluateForScopeAtCollectionNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "Filter")
		{
			return EvaluateFilterCollectionNode(ruleSet, context, arguments, evaluationContext);
		}

		AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Collection consumers currently support only ForScope, ForScopeAt, or Filter collection arguments.", functionName);

		return ResolvedValue{false, false, ""};
	}

	ResolvedValue EvaluateCollectionNodeWithTrace(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const json& argument,
	                                              EvaluationContext& evaluationContext)
	{
		const json* collectionFunction = ResolveCollectionFunctionNode(argument);
		const std::string functionName = collectionFunction != nullptr
			? GetString(*collectionFunction, "functionName")
			: std::string();
		TraceSinkCapture traceCapture(evaluationContext);
		ResolvedValue value = EvaluateCollectionNode(ruleSet, context, argument, evaluationContext);
		std::vector<EvaluationTraceNode> children = traceCapture.Release();

		AppendFunctionResultTrace(evaluationContext, context, functionName, value, std::move(children));

		return value;
	}

	ResolvedValue EvaluateCountNode(const RuleSet& ruleSet,
	                                const std::shared_ptr<Instance>& context,
	                                const json& arguments,
	                                EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Count expression requires one collection argument.", "Count");

			return ResolvedValue{false, false, ""};
		}

		const json& argument = arguments.at(0);
		const json* collectionFunction = ResolveCollectionFunctionNode(argument);

		if(collectionFunction == nullptr || !IsCollectionExpressionName(GetString(*collectionFunction, "functionName")))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Count currently supports only ForScope, ForScopeAt, or Filter collection arguments.", argument.dump());

			return ResolvedValue{false, false, ""};
		}

		ResolvedValue collection = EvaluateCollectionNodeWithTrace(ruleSet, context, argument, evaluationContext);

		if(!collection.supported)
		{
			return collection;
		}

		if(!collection.known)
		{
			ResolvedValue result = UnknownValue(Attribute::DOUBLE);

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		if(collection.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			ResolvedValue result{true, true, "", Attribute::KnowledgeState::Uncertain, Attribute::DOUBLE};

			result.traceRelationshipId = collection.traceRelationshipId;
			result.contributingItems = collection.collectionValues;

			return result;
		}

		ResolvedValue result{true, true, std::to_string(collection.collectionValues.size()), collection.knowledgeState, Attribute::DOUBLE};

		result.traceRelationshipId = collection.traceRelationshipId;
		result.contributingItems = collection.collectionValues;

		return result;
	}

	ResolvedValue EvaluateCountDistinctNode(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& context,
	                                        const json& arguments,
	                                        EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "CountDistinct expression requires one collection argument.", "CountDistinct");

			return ResolvedValue{false, false, ""};
		}

		const json& argument = arguments.at(0);
		const json* collectionFunction = ResolveCollectionFunctionNode(argument);

		if(collectionFunction == nullptr || !IsCollectionExpressionName(GetString(*collectionFunction, "functionName")))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "CountDistinct currently supports only ForScope, ForScopeAt, or Filter collection arguments.", argument.dump());

			return ResolvedValue{false, false, ""};
		}

		return EvaluateCountDistinctCollection(EvaluateCollectionNodeWithTrace(ruleSet, context, argument, evaluationContext),
		                                       evaluationContext);
	}

	ResolvedValue EvaluateCollectionAggregateNode(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& context,
	                                              const std::string& functionName,
	                                              const json& arguments,
	                                              EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() != 1)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", functionName + " collection aggregate requires one collection argument.", functionName);

			return ResolvedValue{false, false, ""};
		}

		const json& argument = arguments.at(0);
		const json* collectionFunction = ResolveCollectionFunctionNode(argument);

		if(collectionFunction == nullptr || !IsCollectionExpressionName(GetString(*collectionFunction, "functionName")))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", functionName + " currently supports only ForScope, ForScopeAt, or Filter collection arguments.", argument.dump());

			return ResolvedValue{false, false, ""};
		}

		return EvaluateCollectionAggregate(functionName,
		                                   EvaluateCollectionNodeWithTrace(ruleSet, context, argument, evaluationContext),
		                                   evaluationContext);
	}

	ResolvedValue EvaluateScopedQuantifierNode(const RuleSet& ruleSet,
	                                           const std::shared_ptr<Instance>& context,
	                                           const std::string& functionName,
	                                           const json& arguments,
	                                           EvaluationContext& evaluationContext)
	{
		const bool isForAll = functionName == "ForAll" || functionName == "ForAllScope";
		const bool isExportedForAll = functionName == "ForAll";

		if(!arguments.is_array() || arguments.empty() || arguments.size() > 3 || (isExportedForAll && arguments.size() < 2))
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Scoped quantifier expression requires scope, optional alias, and optional condition arguments.", functionName);

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = FunctionArgumentValue(arguments.at(0));
		const std::string relationshipId = ResolveRelationshipScopeTraceId(ruleSet, arguments.at(0));
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet,
		                                                                            context,
		                                                                            scope,
		                                                                            IsExplicitRelationshipScopeArgument(arguments.at(0)));

		const size_t conditionIndex = arguments.size() >= 3 ? 2 : 1;
		ScopedQuantifierTraceSummary traceSummary;

		ResolvedValue value = EvaluateScopedQuantifierOverInstances(instances,
		                                                            context,
		                                                            isForAll,
		                                                            arguments.size() > 1,
		                                                            functionName == "ForAllScope",
		                                                            evaluationContext,
		                                                            [&](const std::shared_ptr<Instance>& instance) {
			                                                            return ResolveScopedConditionArgumentNode(ruleSet, instance, arguments.at(conditionIndex), evaluationContext);
		                                                            },
		                                                            &traceSummary);

		return WithScopedQuantifierTrace(value, relationshipId, traceSummary);
	}

	ResolvedValue EvaluateInstanceValueIfNode(const RuleSet& ruleSet,
	                                          const std::shared_ptr<Instance>& context,
	                                          const json& arguments,
	                                          EvaluationContext& evaluationContext)
	{
		if(!arguments.is_array() || arguments.size() != 3)
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceValueIf expression requires scope, value, and condition arguments.", "InstanceValueIf");

			return ResolvedValue{false, false, ""};
		}

		const std::string scope = FunctionArgumentValue(arguments.at(0));
		std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet,
		                                                                            context,
		                                                                            scope,
		                                                                            IsExplicitRelationshipScopeArgument(arguments.at(0)));
		std::shared_ptr<Instance> matchedInstance;

		for(const std::shared_ptr<Instance>& instance : instances)
		{
			evaluationContext.scopeStack.push_back(context);

			ResolvedValue conditionValue = ResolveScopedConditionArgumentNode(ruleSet, instance, arguments.at(2), evaluationContext);

			evaluationContext.scopeStack.pop_back();

			if(!conditionValue.supported)
			{
				return conditionValue;
			}

			if(!conditionValue.known || conditionValue.knowledgeState != Attribute::KnowledgeState::Certain)
			{
				return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain};
			}

			bool boolValue = false;

			if(!TryParseBoolean(conditionValue.value, boolValue) || !boolValue)
			{
				continue;
			}

			if(matchedInstance != nullptr)
			{
				return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain};
			}

			matchedInstance = instance;
		}

		if(matchedInstance == nullptr)
		{
			return ResolvedValue{true, true, "", Attribute::KnowledgeState::Uncertain};
		}

		evaluationContext.scopeStack.push_back(context);

		ResolvedValue matchedValue = ResolveScopedConditionArgumentNode(ruleSet, matchedInstance, arguments.at(1), evaluationContext);

		evaluationContext.scopeStack.pop_back();

		return matchedValue;
	}

	std::string ResolveInstanceNodeToken(const std::shared_ptr<Instance>& context,
	                                     const json& argument,
	                                     size_t argumentIndex,
	                                     const EvaluationContext& evaluationContext)
	{
		const std::string value = FunctionArgumentValue(argument);
		const std::string normalized = NormalizeLookup(value);

		if(normalized.empty() || normalized == "scope-ref")
		{
			if(argumentIndex == 0 && !evaluationContext.scopeStack.empty() && evaluationContext.scopeStack.back() != nullptr)
			{
				return evaluationContext.scopeStack.back()->instanceId;
			}

			return context != nullptr ? context->instanceId : "";
		}

		return value;
	}

	ResolvedValue EvaluateFunctionNodeValue(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& context,
	                                        const json& node,
	                                        EvaluationContext& evaluationContext)
	{
		if(!node.is_object())
		{
			AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression node must be an object.", node.dump());

			return ResolvedValue{false, false, ""};
		}

		const std::string functionName = GetString(node, "functionName");
		const json emptyArguments = json::array();
		const json& arguments = node.contains("functionArguments") && node.at("functionArguments").is_array()
			? node.at("functionArguments")
			: emptyArguments;

		if(functionName.empty())
		{
			return BooleanValue(true);
		}

		if(functionName == "Certain")
		{
			return NodeResultValue(EvaluateCertainExpression(context, node, evaluationContext));
		}

		if(functionName == "CurrentDate" || functionName == "CurrentTime")
		{
			return EvaluateCurrentDateTimeExpression(BuildCallFromNode(node, functionName), evaluationContext);
		}

		if(functionName == "For")
		{
			return EvaluateForExpression(ruleSet, context, BuildCallFromNode(node, functionName), evaluationContext);
		}

		if(functionName == "ForScope")
		{
			return EvaluateForScopeNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "ForScopeAt")
		{
			return EvaluateForScopeAtNode(ruleSet, context, arguments, evaluationContext);
		}

		if(IsTemporalUniversalQuantifierName(functionName))
		{
			return EvaluateTemporalScopedQuantifierNode(ruleSet, context, functionName, arguments, evaluationContext);
		}

		if(functionName == "Count")
		{
			return EvaluateCountNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "CountDistinct")
		{
			return EvaluateCountDistinctNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "Sum"
			|| functionName == "Average"
			|| ((functionName == "Maximum" || functionName == "Minimum")
				&& arguments.size() == 1
				&& IsCollectionFunctionArgument(arguments.at(0))))
		{
			return EvaluateCollectionAggregateNode(ruleSet, context, functionName, arguments, evaluationContext);
		}

		if(functionName == "Concatenate")
		{
			std::string result;

			for(const json& argument : arguments)
			{
				ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext);

				if(!value.supported)
				{
					return value;
				}

				if(!value.known)
				{
					return ResolvedValue{true, false, ""};
				}

				result += value.value;
			}

			return ResolvedValue{true, true, result, Attribute::KnowledgeState::Certain, Attribute::STRING};
		}

		if(functionName == "InstanceCount")
		{
			if(arguments.size() != 1)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceCount expression requires one relationship or entity scope argument.", "InstanceCount");

				return ResolvedValue{false, false, ""};
			}

			const std::string scope = FunctionArgumentValue(arguments.at(0));
			std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet,
			                                                                            context,
			                                                                            scope,
			                                                                            IsExplicitRelationshipScopeArgument(arguments.at(0)));

			return WithRelationshipTrace(
				ResolvedValue{true, true, std::to_string(instances.size()), Attribute::KnowledgeState::Certain, Attribute::DOUBLE},
				ResolveRelationshipScopeTraceId(ruleSet, arguments.at(0)));
		}

		if(functionName == "InstanceValueIf")
		{
			return EvaluateInstanceValueIfNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "InferInstance" || functionName == "InferInstanceFor")
		{
			AddDiagnostic(evaluationContext, "infer_instance_invalid_context", "InferInstance and InferInstanceFor are valid only as conclusion-side instance-creation constructs.", functionName);

			return ResolvedValue{false, false, ""};
		}

		if(functionName == "Error" || functionName == "Warning")
		{
			AddDiagnostic(evaluationContext, "validation_invalid_context", "Error and Warning are valid only as conclusion-side validation constructs.", functionName);

			return ResolvedValue{false, false, ""};
		}

		if(IsTemporalFunctionName(functionName))
		{
			return EvaluateTemporalFunctionNode(ruleSet, context, arguments, functionName, evaluationContext);
		}

		if(functionName == "InstanceMaximum")
		{
			return EvaluateInstanceMaximumExpression(ruleSet, context, BuildCallFromNode(node, functionName), evaluationContext);
		}

		if(functionName == "InstanceMaximumIf" || functionName == "InstanceMinimum" || functionName == "InstanceMinimumIf" || functionName == "InstanceSum" || functionName == "InstanceSumIf")
		{
			return EvaluateInstanceAggregateExpression(ruleSet, context, BuildCallFromNode(node, functionName), evaluationContext);
		}

		if(functionName == "InstanceCountIf")
		{
			if(arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceCountIf expression requires scope and condition arguments.", "InstanceCountIf");

				return ResolvedValue{false, false, ""};
			}

			const std::string scope = FunctionArgumentValue(arguments.at(0));
			std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet,
			                                                                            context,
			                                                                            scope,
			                                                                            IsExplicitRelationshipScopeArgument(arguments.at(0)));
			size_t matches = 0;

			for(const std::shared_ptr<Instance>& instance : instances)
			{
				evaluationContext.scopeStack.push_back(context);

				ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, instance, arguments.at(1), evaluationContext);

				evaluationContext.scopeStack.pop_back();

				if(!value.supported)
				{
					return value;
				}

				bool boolValue = false;

				if(TryResolvedBoolean(value, boolValue) && boolValue)
				{
					++matches;
				}
			}

			return WithRelationshipTrace(
				ResolvedValue{true, true, std::to_string(matches)},
				ResolveRelationshipScopeTraceId(ruleSet, arguments.at(0)));
		}

		if(functionName == "Not")
		{
			if(arguments.size() != 1)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Not expression requires one argument.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(0), evaluationContext);

			if(!value.supported)
			{
				return value;
			}

			bool boolValue = false;

			if(!value.known)
			{
				return ResolvedValue{true, false, ""};
			}

			if(!TryParseBoolean(value.value, boolValue))
			{
				return BooleanValue(false, value.knowledgeState);
			}

			return BooleanValue(!boolValue, value.knowledgeState);
		}

		if(functionName == "And")
		{
			if(arguments.empty())
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "And expression requires at least one argument.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			bool sawUnknown = false;
			bool sawUncertain = false;
			bool sawKnown = false;

			for(const json& argument : arguments)
			{
				ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext);

				if(!value.supported)
				{
					return value;
				}

				bool boolValue = false;

				if(!value.known)
				{
					sawUnknown = true;

					continue;
				}

				sawKnown = true;

				if(!TryParseBoolean(value.value, boolValue))
				{
					return BooleanValue(false);
				}

				if(value.knowledgeState == Attribute::KnowledgeState::Certain && !boolValue)
				{
					return BooleanValue(false);
				}

				if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
				{
					sawUncertain = true;
				}
				else if(!boolValue)
				{
					sawUncertain = true;
				}
			}

			if(sawUncertain || sawUnknown)
			{
				return BooleanValue(false, (sawUncertain || sawKnown) ? Attribute::KnowledgeState::Uncertain : Attribute::KnowledgeState::Unknown);
			}

			return BooleanValue(true);
		}

		if(functionName == "Unknown" || functionName == "Uncertain")
		{
			if(arguments.size() != 1)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Unknown and Uncertain expressions require one argument.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(0), evaluationContext);

			if(!value.supported)
			{
				return value;
			}

			if(functionName == "Uncertain")
			{
				return BooleanValue(value.known && value.knowledgeState == Attribute::KnowledgeState::Uncertain);
			}

			return BooleanValue(!value.known);
		}

		if(functionName == "Equals" || functionName == "NotEquals")
		{
			if(arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Equals and NotEquals expressions require two arguments.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			ResolvedValue left = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(0), evaluationContext);
			ResolvedValue right = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(1), evaluationContext);

			if(!left.supported)
			{
				return left;
			}

			if(!right.supported)
			{
				return right;
			}

			if(!left.known || !right.known)
			{
				return BooleanValue(false, (left.known || right.known) ? Attribute::KnowledgeState::Uncertain : Attribute::KnowledgeState::Unknown);
			}

			const bool equals = left.known && right.known && left.value == right.value;

			const Attribute::KnowledgeState knowledgeState =
				left.knowledgeState == Attribute::KnowledgeState::Certain && right.knowledgeState == Attribute::KnowledgeState::Certain
					? Attribute::KnowledgeState::Certain
					: Attribute::KnowledgeState::Uncertain;

			return BooleanValue(functionName == "NotEquals" ? !equals : equals, knowledgeState);
		}

		if(functionName == "GreaterThan" || functionName == "GreaterThanEquals" || functionName == "GreaterThanOrEquals" || functionName == "greaterThanOrEqual" || functionName == "LessThan" || functionName == "LessThanEquals" || functionName == "LessThanOrEquals")
		{
			if(arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Comparison expressions require two arguments.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			ResolvedValue left = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(0), evaluationContext);
			ResolvedValue right = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(1), evaluationContext);

			if(!left.supported)
			{
				return left;
			}

			if(!right.supported)
			{
				return right;
			}

			if(!left.known || !right.known)
			{
				return BooleanValue(false, (left.known || right.known) ? Attribute::KnowledgeState::Uncertain : Attribute::KnowledgeState::Unknown);
			}

			const int comparison = CompareKnownValues(left.value, right.value);
			const Attribute::KnowledgeState knowledgeState =
				left.knowledgeState == Attribute::KnowledgeState::Certain && right.knowledgeState == Attribute::KnowledgeState::Certain
					? Attribute::KnowledgeState::Certain
					: Attribute::KnowledgeState::Uncertain;

			if(functionName == "GreaterThan")
			{
				return BooleanValue(comparison > 0, knowledgeState);
			}

			if(functionName == "LessThan")
			{
				return BooleanValue(comparison < 0, knowledgeState);
			}

			if(functionName == "LessThanEquals" || functionName == "LessThanOrEquals")
			{
				return BooleanValue(comparison <= 0, knowledgeState);
			}

			return BooleanValue(comparison >= 0, knowledgeState);
		}

		if(functionName == "IsNumber")
		{
			if(arguments.size() != 1)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "IsNumber expression requires one argument.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			ResolvedValue value = ResolveFunctionArgumentNode(ruleSet, context, arguments.at(0), evaluationContext);

			if(!value.supported)
			{
				return value;
			}

			if(!value.known)
			{
				return ResolvedValue{true, false, ""};
			}

			double numericValue = 0.0;

			return BooleanValue(TryParseDoubleValue(value.value, numericValue), value.knowledgeState);
		}

		if(functionName == "Exists")
		{
			if(arguments.empty() || arguments.size() > 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Exists expression requires scope and optional condition arguments.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			const std::string scope = FunctionArgumentValue(arguments.at(0));
			std::vector<std::shared_ptr<Instance>> instances = ResolveForScopeInstances(ruleSet, context, scope);

			return EvaluateScopedQuantifierOverInstances(instances,
			                                             context,
			                                             false,
			                                             arguments.size() > 1,
			                                             false,
			                                             evaluationContext,
			                                             [&](const std::shared_ptr<Instance>& instance) {
				                                             return ResolveFunctionArgumentNode(ruleSet, instance, arguments.at(1), evaluationContext);
			                                             });
		}

		if(functionName == "ExistsScope" || functionName == "ForAll" || functionName == "ForAllScope")
		{
			return EvaluateScopedQuantifierNode(ruleSet, context, functionName, arguments, evaluationContext);
		}

		if(functionName == "IsMemberOf")
		{
			return EvaluateIsMemberOfNode(ruleSet, context, arguments, evaluationContext);
		}

		if(functionName == "InstanceNotEquals")
		{
			if(arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceNotEquals expression requires two arguments.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			const std::string left = ResolveInstanceNodeToken(context, arguments.at(0), 0, evaluationContext);
			const std::string right = ResolveInstanceNodeToken(context, arguments.at(1), 1, evaluationContext);

			return BooleanValue(!left.empty() && !right.empty() && left != right);
		}

		if(functionName == "InstanceEquals")
		{
			if(arguments.size() != 2)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "InstanceEquals expression requires two arguments.", node.dump());

				return ResolvedValue{false, false, ""};
			}

			const std::string left = ResolveInstanceNodeToken(context, arguments.at(0), 0, evaluationContext);
			const std::string right = ResolveInstanceNodeToken(context, arguments.at(1), 1, evaluationContext);

			return BooleanValue(!left.empty() && !right.empty() && left == right);
		}

		if(IsScalarFunctionName(functionName))
		{
			std::vector<ResolvedValue> values;

			values.reserve(arguments.size());

			for(const json& argument : arguments)
			{
				values.push_back(ResolveFunctionArgumentNode(ruleSet, context, argument, evaluationContext));
			}

			return ApplyStaticFunctionReturnType(functionName, values, EvaluateScalarFunction(functionName, values, evaluationContext));
		}

		AddDiagnostic(evaluationContext, "unsupported_expression_function", "Expression function is not implemented yet.", functionName);

		return ResolvedValue{false, false, ""};
	}

	NodeResult EvaluateExpressionNode(const RuleSet& ruleSet, const std::shared_ptr<Instance>& context, const json& node, EvaluationContext& evaluationContext)
	{
		const std::string functionName = GetString(node, "functionName");
		ResolvedValue value = EvaluateFunctionNodeWithTrace(ruleSet, context, node, evaluationContext);

		if(!value.supported)
		{
			return NodeResult::Unsupported;
		}

		if(!value.known)
		{
			return NodeResult::Unknown;
		}

		bool boolValue = false;

		if(TryParseBoolean(value.value, boolValue))
		{
			return BooleanNodeResult(boolValue, value.knowledgeState, evaluationContext.useConcreteUncertainValues);
		}

		if(functionName == "InstanceValueIf" && value.knowledgeState == Attribute::KnowledgeState::Uncertain)
		{
			return NodeResult::Uncertain;
		}

		if(functionName == "InstanceCount"
			|| functionName == "InstanceCountIf"
			|| functionName == "InstanceMaximum"
			|| functionName == "InstanceMaximumIf"
			|| functionName == "InstanceMinimum"
			|| functionName == "InstanceMinimumIf"
			|| functionName == "InstanceSum"
			|| functionName == "InstanceSumIf")
		{
			return NodeResult::True;
		}

		return NodeResult::False;
	}

	bool TryResolveFlowDerivedBoolean(const RuleSet& ruleSet,
	                                  const std::shared_ptr<Instance>& context,
	                                  const std::string& entityId,
	                                  const std::string& attributeId,
	                                  const std::shared_ptr<Attribute>& attribute,
	                                  EvaluationContext& evaluationContext,
	                                  bool& value)
	{
		if(!evaluationContext.resolveFlowDerivedBooleans || evaluationContext.ruleSet == nullptr || context == nullptr || attribute == nullptr
			|| (attribute->GetType() != Attribute::BOOLEAN) || entityId.empty() || attributeId.empty())
		{
			return false;
		}

		const Attribute* attributeKey = attribute.get();
		auto cachedValue = evaluationContext.flowDerivedBooleanValues.find(attributeKey);
		if(cachedValue != evaluationContext.flowDerivedBooleanValues.end())
		{
			value = cachedValue->second;

			return true;
		}

		if(evaluationContext.unresolvedFlowDerivedBooleans.find(attributeKey) != evaluationContext.unresolvedFlowDerivedBooleans.end()
			|| !evaluationContext.resolvingFlowDerivedBooleans.insert(attributeKey).second)
		{
			return false;
		}

		auto markUnresolved = [&]() {
			evaluationContext.resolvingFlowDerivedBooleans.erase(attributeKey);
			evaluationContext.unresolvedFlowDerivedBooleans.insert(attributeKey);
		};

		const std::vector<std::string> ruleIds = ruleSet.GetActiveSessionModelRulesConcludingAttribute(entityId, attributeId);
		if(ruleIds.size() != 1)
		{
			markUnresolved();

			return false;
		}

		const RuleSet::SessionModelRuleMetadata* rule = FindRule(ruleSet, ruleIds.front());
		const std::string valueMode = rule != nullptr && rule->conclusionValueMode.empty()
			? "Literal"
			: (rule != nullptr ? rule->conclusionValueMode : "");
		if(rule == nullptr || !rule->isActive || rule->conclusionMode != "Value" || valueMode != "Literal"
			|| rule->conclusionEntityId != entityId || rule->conclusionAttributeId != attributeId
			|| (rule->conclusionExpression.is_object() && !rule->conclusionExpression.empty()))
		{
			markUnresolved();

			return false;
		}

		std::shared_ptr<Instance> target;
		for(const std::shared_ptr<Instance>& candidate : ResolveConclusionTargets(ruleSet, context, *rule, RuleTrigger{"", "", ""}))
		{
			if(candidate != nullptr && candidate->GetAttributeByID(attributeId).get() == attributeKey)
			{
				target = candidate;

				break;
			}
		}

		if(target == nullptr)
		{
			markUnresolved();

			return false;
		}

		if(evaluationContext.stats != nullptr)
		{
			evaluationContext.stats->candidateRules++;
		}

		EvaluationContext ruleContext = evaluationContext;
		ruleContext.ruleId = rule->ruleId;
		ruleContext.resolveFlowDerivedBooleans = false;
		ruleContext.useConcreteUncertainValues = true;
		ruleContext.traceSink = nullptr;
		NodeResult conditionResult = EvaluateNode(ruleSet, target, rule->condition, ruleContext);

		if(conditionResult == NodeResult::Unsupported)
		{
			if(evaluationContext.stats != nullptr)
			{
				evaluationContext.stats->unsupportedRules++;
			}
			markUnresolved();

			return false;
		}

		if(evaluationContext.stats != nullptr)
		{
			evaluationContext.stats->evaluatedRules++;
		}

		if(conditionResult != NodeResult::True && conditionResult != NodeResult::False)
		{
			markUnresolved();

			return false;
		}

		const bool conditionPassed = conditionResult == NodeResult::True;
		std::string resolvedText = conditionPassed ? rule->conclusionValue : rule->elseValue;
		if(resolvedText.empty() && attribute->GetType() == Attribute::BOOLEAN && rule->conclusionValue.empty())
		{
			resolvedText = conditionPassed ? "true" : "false";
		}

		bool resolvedValue = false;
		if(resolvedText.empty() || !TryParseBoolean(resolvedText, resolvedValue))
		{
			markUnresolved();

			return false;
		}

		evaluationContext.resolvingFlowDerivedBooleans.erase(attributeKey);
		evaluationContext.flowDerivedBooleanValues.emplace(attributeKey, resolvedValue);
		value = resolvedValue;

		return true;
	}

	NodeResult EvaluateConditionNode(const std::shared_ptr<Instance>& context, const json& node, EvaluationContext& evaluationContext)
	{
		std::string entityId = GetString(node, "entityId");
		const std::string attributeId = GetString(node, "attributeId");
		const std::string relationshipId = GetString(node, "relationshipId");
		const std::string operatorName = GetString(node, "operator");
		const std::string valueMode = GetString(node, "valueMode");
		const std::string value = GetString(node, "value");

		if(operatorName == "any")
		{
			return NodeResult::True;
		}

		if(entityId.empty() && context != nullptr)
		{
			entityId = context->entityId;
		}

		std::shared_ptr<Attribute> attribute = ResolveAttribute(context, entityId, attributeId, relationshipId);
		AppendAttributeReadTrace(evaluationContext,
		                         context,
		                         entityId,
		                         "",
		                         attributeId,
		                         relationshipId,
		                         attribute);

		if(attribute == nullptr)
		{
			return NodeResult::Unknown;
		}

		if((operatorName == "isTrue" || operatorName == "isFalse") && (attribute->IsUnknown() || attribute->IsUncertain()))
		{
			bool derivedValue = false;
			if(TryResolveFlowDerivedBoolean(*evaluationContext.ruleSet,
			                                    context,
			                                    entityId,
			                                    attributeId,
			                                    attribute,
			                                    evaluationContext,
			                                    derivedValue))
			{
				return (operatorName == "isTrue") == derivedValue ? NodeResult::True : NodeResult::False;
			}
		}

		if(operatorName == "isKnown")
		{
			return attribute->IsUnknown() ? NodeResult::False : NodeResult::True;
		}

		if(operatorName == "isUnknown")
		{
			return attribute->IsUnknown() ? NodeResult::True : NodeResult::False;
		}

		if(operatorName == "isNotEmpty")
		{
			return AttributeBackedNodeResult(attribute, !attribute->GetStringValue().empty(), evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "isEmpty")
		{
			return AttributeBackedNodeResult(attribute, attribute->GetStringValue().empty(), evaluationContext.useConcreteUncertainValues);
		}

		if(attribute->IsUnknown())
		{
			return NodeResult::Unknown;
		}

		if(operatorName == "isTrue")
		{
			bool boolValue = false;

			return AttributeBackedNodeResult(attribute,
			                                 TryParseBoolean(attribute->GetStringValue(), boolValue) && boolValue,
			                                 evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "isFalse")
		{
			bool boolValue = false;

			return AttributeBackedNodeResult(attribute,
			                                 TryParseBoolean(attribute->GetStringValue(), boolValue) && !boolValue,
			                                 evaluationContext.useConcreteUncertainValues);
		}

		ResolvedValue comparisonValue = ResolveValue(context, node, entityId, valueMode, value, false, evaluationContext);

		if(!comparisonValue.supported)
		{
			return NodeResult::Unsupported;
		}

		if(!comparisonValue.known)
		{
			return BooleanNodeResult(false,
			                         CombineKnowledgeStates(attribute->GetKnowledgeState(), Attribute::KnowledgeState::Unknown),
			                         evaluationContext.useConcreteUncertainValues);
		}

		const Attribute::KnowledgeState comparisonKnowledge = CombineKnowledgeStates(attribute->GetKnowledgeState(), comparisonValue.knowledgeState);

		if(operatorName == "equals")
		{
			return BooleanNodeResult(AttributeEqualsValue(attribute, comparisonValue.value), comparisonKnowledge, evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "notEquals")
		{
			return BooleanNodeResult(!AttributeEqualsValue(attribute, comparisonValue.value), comparisonKnowledge, evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "in")
		{
			return BooleanNodeResult(ValueInLiteralList(attribute->GetStringValue(), comparisonValue.value), comparisonKnowledge, evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "greaterThan")
		{
			return BooleanNodeResult(AttributeGreaterThanValue(attribute, comparisonValue.value), comparisonKnowledge, evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "greaterThanOrEqual" || operatorName == "greaterThanEquals")
		{
			return BooleanNodeResult(AttributeGreaterThanOrEqualValue(attribute, comparisonValue.value), comparisonKnowledge, evaluationContext.useConcreteUncertainValues);
		}

		if(operatorName == "lessThan")
		{
			return BooleanNodeResult(AttributeLessThanValue(attribute, comparisonValue.value), comparisonKnowledge, evaluationContext.useConcreteUncertainValues);
		}

		AddDiagnostic(evaluationContext, "unsupported_condition_operator", "Condition operator is not implemented yet.", operatorName);

		return NodeResult::Unsupported;
	}

	bool IsBlankConditionPlaceholder(const json& node)
	{
		if(!node.is_object())
		{
			return false;
		}

		const std::string nodeType = ToLowerAscii(GetString(node, "nodeType"));
		if(nodeType != "condition")
		{
			return false;
		}

		if(node.value("isFunction", false))
		{
			return false;
		}

		if(node.contains("children") && node.at("children").is_array() && !node.at("children").empty())
		{
			return false;
		}

		return GetString(node, "entityId").empty()
			&& GetString(node, "attributeId").empty()
			&& GetString(node, "compareAttributeId").empty()
			&& GetString(node, "functionName").empty()
			&& GetString(node, "value").empty()
			&& GetString(node, "valueTo").empty();
	}

	NodeResult EvaluateNode(const RuleSet& ruleSet, const std::shared_ptr<Instance>& context, const json& node, EvaluationContext& evaluationContext)
	{
		if(!node.is_object())
		{
			AddDiagnostic(evaluationContext, "unsupported_condition_node", "Condition node must be an object.", node.dump());

			return NodeResult::Unsupported;
		}

		const std::string nodeType = ToLowerAscii(GetString(node, "nodeType"));

		if(nodeType == "condition")
		{
			return EvaluateConditionNode(context, node, evaluationContext);
		}

		if(nodeType == "expression")
		{
			return EvaluateExpressionNode(ruleSet, context, node, evaluationContext);
		}

		if(nodeType == "group")
		{
			std::string junctionValue = GetString(node, "junction");
			const std::string junction = Util::ToUpper(junctionValue);
			const bool hasChildren = node.contains("children") && !node.at("children").is_null();

			if(hasChildren && !node.at("children").is_array())
			{
				AddDiagnostic(evaluationContext, "unsupported_group_shape", "Group node requires a children array.", node.dump());

				return NodeResult::Unsupported;
			}

			const json emptyChildren = json::array();
			const json& children = hasChildren ? node.at("children") : emptyChildren;
			bool sawUnsupported = false;

			if(junction == "OR")
			{
				bool sawUnknown = false;
				bool sawUncertain = false;
				bool sawKnown = false;
				for(const json& child : children)
				{
					if(IsBlankConditionPlaceholder(child))
					{
						continue;
					}

					NodeResult result = EvaluateNode(ruleSet, context, child, evaluationContext);

					if(result == NodeResult::True)
					{
						return NodeResult::True;
					}

					if(result == NodeResult::Unsupported)
					{
						sawUnsupported = true;
					}

					if(result == NodeResult::Unknown)
					{
						sawUnknown = true;
					}
					else if(result == NodeResult::Uncertain)
					{
						sawUncertain = true;
						sawKnown = true;
					}
					else if(result == NodeResult::False)
					{
						sawKnown = true;
					}
				}

				if(sawUnsupported)
				{
					return NodeResult::Unsupported;
				}

				if(sawUncertain || (sawKnown && sawUnknown))
				{
					return NodeResult::Uncertain;
				}

				if(sawUnknown)
				{
					return NodeResult::Unknown;
				}

				return NodeResult::False;
			}

			bool sawUnknown = false;
			bool sawUncertain = false;
			bool sawKnown = false;

			for(const json& child : children)
			{
				if(IsBlankConditionPlaceholder(child))
				{
					continue;
				}

				NodeResult result = EvaluateNode(ruleSet, context, child, evaluationContext);

				if(result == NodeResult::False)
				{
					return NodeResult::False;
				}

				if(result == NodeResult::Unsupported)
				{
					sawUnsupported = true;
				}

				if(result == NodeResult::Unknown)
				{
					sawUnknown = true;
				}
				else if(result == NodeResult::Uncertain)
				{
					sawUncertain = true;
					sawKnown = true;
				}
				else if(result == NodeResult::True)
				{
					sawKnown = true;
				}
			}

			if(sawUnsupported)
			{
				return NodeResult::Unsupported;
			}

			if(sawUncertain || (sawKnown && sawUnknown))
			{
				return NodeResult::Uncertain;
			}

			if(sawUnknown)
			{
				return NodeResult::Unknown;
			}

			return NodeResult::True;
		}

		if(nodeType.empty() && !GetString(node, "operator").empty())
		{
			return EvaluateConditionNode(context, node, evaluationContext);
		}

		AddDiagnostic(evaluationContext, "unsupported_condition_node", "Condition node type is not implemented yet.", nodeType);

		return NodeResult::Unsupported;
	}

	bool SetAttributeResolvedValue(const std::shared_ptr<Attribute>& attribute, const ResolvedValue& value, bool skipEmptyKnownValue)
	{
		if(attribute == nullptr || !value.supported)
		{
			return false;
		}

		const bool wasUnknown = attribute->IsUnknown();
		const std::string previousValue = wasUnknown ? "" : attribute->GetStringValue();
		const Attribute::KnowledgeState previousKnowledgeState = attribute->GetKnowledgeState();

		if(!value.known)
		{
			attribute->MakeUnknown(false);

			return !wasUnknown || previousKnowledgeState != attribute->GetKnowledgeState();
		}

		if(skipEmptyKnownValue && value.value.empty())
		{
			if(value.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				attribute->SetKnowledgeState(Attribute::KnowledgeState::Uncertain, false);

				const bool isUnknown = attribute->IsUnknown();
				const std::string currentValue = isUnknown ? "" : attribute->GetStringValue();

				return wasUnknown != isUnknown || previousValue != currentValue || previousKnowledgeState != attribute->GetKnowledgeState();
			}

			return false;
		}

		if(!attribute->SetValueFromString(value.value, false))
		{
			return false;
		}

		attribute->SetKnowledgeState(value.knowledgeState, false);

		const bool isUnknown = attribute->IsUnknown();
		const std::string currentValue = isUnknown ? "" : attribute->GetStringValue();

		return wasUnknown != isUnknown || previousValue != currentValue || previousKnowledgeState != attribute->GetKnowledgeState();
	}

	struct InferInstanceConclusion
	{
		bool isInfer = false;
		bool shapeSupported = false;
		std::string functionName;
		std::string relationshipArgument;
		std::string conditionArgument;
		bool hasCondition = false;
	};

	struct InferInstanceApplyResult
	{
		bool applied = false;
		bool createdRelationshipLink = false;
		std::string relationshipId;
		std::string sourceEntityId;
		std::string sourceInstanceId;
	};

	struct ValidationConclusion
	{
		bool isValidation = false;
		bool shapeSupported = false;
		std::string functionName;
		std::string severity;
		std::string messageArgument;
	};

	bool IsInferInstanceFunctionName(const std::string& name)
	{
		return name == "InferInstance" || name == "InferInstanceFor";
	}

	bool IsValidationFunctionName(const std::string& name)
	{
		return name == "Error" || name == "Warning";
	}

	std::string ValidationSeverityFromFunctionName(const std::string& name)
	{
		return name == "Error" ? "error" : "warning";
	}

	InferInstanceConclusion ResolveInferInstanceConclusion(const RuleSet::SessionModelRuleMetadata& rule)
	{
		InferInstanceConclusion result;
		ParsedFunctionCall call = ParseFunctionCall(rule.conclusionValue);

		if(call.parsed && IsInferInstanceFunctionName(call.name))
		{
			result.isInfer = true;
			result.functionName = call.name;

			if(!call.valid)
			{
				return result;
			}

			if(call.name == "InferInstance" && call.arguments.size() == 1)
			{
				result.shapeSupported = true;
				result.relationshipArgument = call.arguments[0];

				return result;
			}

			if(call.name == "InferInstanceFor" && (call.arguments.size() == 2 || call.arguments.size() == 3))
			{
				result.shapeSupported = true;
				result.relationshipArgument = call.arguments[0];
				result.conditionArgument = call.arguments.size() == 3 ? call.arguments[2] : call.arguments[1];
				result.hasCondition = true;

				return result;
			}

			return result;
		}

		if(rule.conclusionMode == "InferInstance")
		{
			result.isInfer = true;
			result.functionName = "InferInstance";
			result.relationshipArgument = !rule.relationshipId.empty() ? rule.relationshipId : rule.conclusionValue;
			result.shapeSupported = !result.relationshipArgument.empty();

			return result;
		}

		if(rule.conclusionMode == "InferInstanceFor")
		{
			result.isInfer = true;
			result.functionName = "InferInstanceFor";
			result.relationshipArgument = rule.relationshipId;
			result.conditionArgument = rule.conclusionValue;
			result.hasCondition = !result.conditionArgument.empty();
			result.shapeSupported = !result.relationshipArgument.empty() && result.hasCondition;

			return result;
		}

		return result;
	}

	ValidationConclusion ResolveValidationConclusion(const RuleSet::SessionModelRuleMetadata& rule)
	{
		ValidationConclusion result;
		ParsedFunctionCall call = ParseFunctionCall(rule.conclusionValue);

		if(call.parsed && IsValidationFunctionName(call.name))
		{
			result.isValidation = true;
			result.functionName = call.name;
			result.severity = ValidationSeverityFromFunctionName(call.name);

			if(!call.valid)
			{
				return result;
			}

			if(call.arguments.size() == 1)
			{
				result.shapeSupported = true;
				result.messageArgument = call.arguments[0];
			}

			return result;
		}

		if(IsValidationFunctionName(rule.conclusionMode))
		{
			result.isValidation = true;
			result.functionName = rule.conclusionMode;
			result.severity = ValidationSeverityFromFunctionName(rule.conclusionMode);
			result.messageArgument = rule.conclusionValue;
			result.shapeSupported = !result.messageArgument.empty();

			return result;
		}

		return result;
	}

	bool IsInferInstanceConclusionRule(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return ResolveInferInstanceConclusion(rule).isInfer;
	}

	bool IsValidationConclusionRule(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return ResolveValidationConclusion(rule).isValidation;
	}

	std::string BuildValidationEventKey(const RuleSet::SessionModelRuleMetadata& rule,
	                                    const std::shared_ptr<Instance>& target,
	                                    const ValidationConclusion& conclusion)
	{
		return "validation:" + conclusion.severity + ":" + rule.ruleId + ":" + (target != nullptr ? target->entityId : "") + ":" + (target != nullptr ? target->instanceId : "");
	}

	SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange ValidationEventChangeFromEvent(const RuleSet::ValidationEvent& event)
	{
		return SessionModelRuleEvaluator::EvaluationStats::ValidationEventChange{
			event.eventId,
			event.severity,
			event.ruleId,
			event.ruleName,
			event.entityId,
			event.instanceId,
			event.message,
			event.messageKnown,
			event.messageKnowledgeState
		};
	}

	bool ValidationEventsEqual(const RuleSet::ValidationEvent& left, const RuleSet::ValidationEvent& right)
	{
		return left.eventId == right.eventId
			&& left.severity == right.severity
			&& left.ruleId == right.ruleId
			&& left.ruleName == right.ruleName
			&& left.entityId == right.entityId
			&& left.instanceId == right.instanceId
			&& left.message == right.message
			&& left.messageKnown == right.messageKnown
			&& left.messageKnowledgeState == right.messageKnowledgeState;
	}

	ResolvedValue ResolveValidationMessage(const RuleSet& ruleSet,
	                                       const std::shared_ptr<Instance>& target,
	                                       const std::string& argument,
	                                       EvaluationContext& evaluationContext)
	{
		if(IsQuotedString(argument))
		{
			return ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING};
		}

		ParsedFunctionCall messageCall = ParseFunctionCall(argument);

		if(messageCall.parsed && IsKnownExpressionName(messageCall.name))
		{
			if(!messageCall.valid)
			{
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Validation message expression syntax is invalid.", argument);

				return ResolvedValue{false, false, ""};
			}

			return EvaluateExpressionCallWithTrace(ruleSet, target, messageCall, evaluationContext);
		}

		std::shared_ptr<Attribute> attribute = FindAttributeByIdentifierOrName(target, argument);

		if(attribute != nullptr)
		{
			if(attribute->IsUnknown())
			{
				return ResolvedValue{true, false, "", attribute->GetKnowledgeState(), static_cast<Attribute::AttributeType>(attribute->GetType())};
			}

			return ResolvedValue{true, true, attribute->GetStringValue(), attribute->GetKnowledgeState(), static_cast<Attribute::AttributeType>(attribute->GetType())};
		}

		return ResolvedValue{true, true, RemoveMatchingQuotes(argument), Attribute::KnowledgeState::Certain, Attribute::STRING};
	}

	bool ApplyValidationConclusion(RuleSet& ruleSet,
	                               const std::shared_ptr<Instance>& target,
	                               const RuleSet::SessionModelRuleMetadata& rule,
	                               const ValidationConclusion& conclusion,
	                               EvaluationContext& evaluationContext,
	                               SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		if(target == nullptr || !conclusion.isValidation || !conclusion.shapeSupported)
		{
			return false;
		}

		ResolvedValue message = ResolveValidationMessage(ruleSet, target, conclusion.messageArgument, evaluationContext);

		if(!message.supported)
		{
			return false;
		}

		if(ruleSet.activeValidationEventsByKey == nullptr)
		{
			ruleSet.activeValidationEventsByKey = std::make_shared<std::unordered_map<std::string, RuleSet::ValidationEvent>>();
		}

		RuleSet::ValidationEvent event;

		event.eventId = BuildValidationEventKey(rule, target, conclusion);
		event.severity = conclusion.severity;
		event.ruleId = rule.ruleId;
		event.ruleName = rule.ruleName;
		event.entityId = target->entityId;
		event.instanceId = target->instanceId;
		event.message = message.known ? message.value : "";
		event.messageKnown = message.known;
		event.messageKnowledgeState = Attribute::KnowledgeStateToString(message.knowledgeState);

		auto existing = ruleSet.activeValidationEventsByKey->find(event.eventId);

		if(existing == ruleSet.activeValidationEventsByKey->end())
		{
			(*ruleSet.activeValidationEventsByKey)[event.eventId] = event;
			stats.validationEventsCreated++;
			stats.createdValidationEvents.push_back(ValidationEventChangeFromEvent(event));
		}
		else if(!ValidationEventsEqual(existing->second, event))
		{
			existing->second = event;
			stats.validationEventsUpdated++;
			stats.updatedValidationEvents.push_back(ValidationEventChangeFromEvent(event));
		}
		else
		{
			stats.validationEventsReused++;
			stats.reusedValidationEvents.push_back(ValidationEventChangeFromEvent(event));
		}

		if(conclusion.severity == "error")
		{
			stats.validationErrors++;
		}
		else
		{
			stats.validationWarnings++;
		}

		return true;
	}

	bool ClearValidationConclusion(RuleSet& ruleSet,
	                               const std::shared_ptr<Instance>& target,
	                               const RuleSet::SessionModelRuleMetadata& rule,
	                               const ValidationConclusion& conclusion,
	                               SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		if(target == nullptr || !conclusion.isValidation || !conclusion.shapeSupported || ruleSet.activeValidationEventsByKey == nullptr)
		{
			return false;
		}

		const std::string eventId = BuildValidationEventKey(rule, target, conclusion);
		auto existing = ruleSet.activeValidationEventsByKey->find(eventId);

		if(existing == ruleSet.activeValidationEventsByKey->end())
		{
			return false;
		}

		stats.validationEventsCleared++;
		stats.clearedValidationEvents.push_back(ValidationEventChangeFromEvent(existing->second));
		ruleSet.activeValidationEventsByKey->erase(existing);

		return true;
	}

	std::string BuildInferredInstanceKey(const RuleSet::SessionModelRuleMetadata& rule,
	                                     const std::shared_ptr<Instance>& source,
	                                     const std::shared_ptr<Instance>& drivingInstance,
	                                     const std::shared_ptr<Relationship>& relationship)
	{
		std::string key = "inferred:" + rule.ruleId + ":" + (source != nullptr ? source->instanceId : "") + ":";

		if(drivingInstance != nullptr)
		{
			key += drivingInstance->instanceId + ":";
		}

		key += (relationship != nullptr ? relationship->targetEntity : "") + ":" + (relationship != nullptr ? relationship->id : "");

		return key;
	}

	bool HasOutgoingRelationshipLink(const std::shared_ptr<Instance>& source, const std::string& relationshipId, const std::string& targetInstanceId)
	{
		if(source == nullptr)
		{
			return false;
		}

		return HasRelationshipLinkById(source, relationshipId, [&](const InstanceRelationshipLink& link) {
			return link.outgoing && link.targetInstanceId == targetInstanceId;
		});
	}

	bool HasDifferentOutgoingRelationshipTarget(const std::shared_ptr<Instance>& source, const std::string& relationshipId, const std::string& targetInstanceId)
	{
		if(source == nullptr)
		{
			return false;
		}

		return HasRelationshipLinkById(source, relationshipId, [&](const InstanceRelationshipLink& link) {
			return link.outgoing && link.targetInstanceId != targetInstanceId;
		});
	}

	bool RelationshipAllowsMultipleOutgoingTargets(const std::shared_ptr<Relationship>& relationship)
	{
		return relationship == nullptr || relationship->type == "1M" || relationship->type == "MM" || relationship->type.empty();
	}

	std::shared_ptr<Instance> FindInferredInstanceByKey(RuleSet& ruleSet, const std::shared_ptr<Entity>& entity, const std::string& inferredKey)
	{
		if(entity == nullptr || inferredKey.empty())
		{
			return nullptr;
		}

		if(ruleSet.inferredInstanceIdsByKey != nullptr)
		{
			auto it = ruleSet.inferredInstanceIdsByKey->find(inferredKey);

			if(it != ruleSet.inferredInstanceIdsByKey->end())
			{
				std::shared_ptr<Instance> instance = entity->GetInstanceById(it->second);

				if(instance != nullptr)
				{
					return instance;
				}
			}
		}

		for(const std::shared_ptr<Instance>& instance : entity->instances)
		{
			if(instance != nullptr && instance->isInferred && instance->inferredInstanceKey == inferredKey)
			{
				if(ruleSet.inferredInstanceIdsByKey != nullptr)
				{
					(*ruleSet.inferredInstanceIdsByKey)[inferredKey] = instance->instanceId;
				}

				return instance;
			}
		}

		return nullptr;
	}

	NodeResult EvaluateInferCondition(const RuleSet& ruleSet,
	                                  const std::shared_ptr<Instance>& drivingInstance,
	                                  const std::string& condition,
	                                  EvaluationContext& evaluationContext)
	{
		if(drivingInstance == nullptr)
		{
			return NodeResult::Unknown;
		}

		evaluationContext.scopeStack.push_back(drivingInstance);

		ParsedFunctionCall conditionCall = ParseFunctionCall(condition);
		ResolvedValue value;

		if(conditionCall.parsed && IsKnownExpressionName(conditionCall.name))
		{
			if(!conditionCall.valid)
			{
				evaluationContext.scopeStack.pop_back();
				AddDiagnostic(evaluationContext, "unsupported_expression_shape", "Expression syntax is invalid.", condition);

				return NodeResult::Unsupported;
			}

			value = EvaluateExpressionCallWithTrace(ruleSet, drivingInstance, conditionCall, evaluationContext);
		}
		else
		{
			value = ResolveExpressionArgument(ruleSet, drivingInstance, condition, evaluationContext);
		}

		evaluationContext.scopeStack.pop_back();

		if(!value.supported)
		{
			return NodeResult::Unsupported;
		}

		if(!value.known)
		{
			return NodeResult::Unknown;
		}

		bool boolValue = false;

		if(!TryParseBoolean(value.value, boolValue))
		{
			return NodeResult::Unsupported;
		}

		return BooleanNodeResult(boolValue, value.knowledgeState);
	}

	std::shared_ptr<Relationship> ResolveInferRelationship(const RuleSet& ruleSet,
	                                                       const std::shared_ptr<Instance>& source,
	                                                       const InferInstanceConclusion& conclusion,
	                                                       EvaluationContext& evaluationContext)
	{
		if(conclusion.relationshipArgument.empty())
		{
			return nullptr;
		}

		ResolvedRelationshipScope relationship = ResolveRelationshipScopeArgument(ruleSet, source, conclusion.relationshipArgument, evaluationContext);

		if(!relationship.supported || !relationship.known || relationship.relationshipId.empty())
		{
			return nullptr;
		}

		return FindRelationshipById(ruleSet, relationship.relationshipId);
	}

	std::vector<std::shared_ptr<Instance>> ResolveInferSourceTargets(const RuleSet& ruleSet,
	                                                                 const std::shared_ptr<Instance>& changedInstance,
	                                                                 const RuleSet::SessionModelRuleMetadata& rule,
	                                                                 const RuleTrigger& trigger,
	                                                                 EvaluationContext& evaluationContext)
	{
		std::vector<std::shared_ptr<Instance>> sources;

		if(changedInstance == nullptr)
		{
			return sources;
		}

		InferInstanceConclusion conclusion = ResolveInferInstanceConclusion(rule);
		std::shared_ptr<Relationship> relationship = ResolveInferRelationship(ruleSet, changedInstance, conclusion, evaluationContext);

		if(relationship == nullptr)
		{
			PushUniqueInstance(sources, changedInstance);

			return sources;
		}

		if(changedInstance->entityId == relationship->sourceEntity)
		{
			PushUniqueInstance(sources, changedInstance);

			return sources;
		}

		std::vector<std::shared_ptr<Instance>> relatedSources = ResolveRelatedInstances(changedInstance, relationship->sourceEntity, relationship->id);

		for(const std::shared_ptr<Instance>& source : relatedSources)
		{
			PushUniqueInstance(sources, source);
		}

		return sources;
	}

	InferInstanceApplyResult CreateOrReuseInferredInstance(RuleSet& ruleSet,
	                                                       const std::shared_ptr<Instance>& source,
	                                                       const std::shared_ptr<Instance>& drivingInstance,
	                                                       const RuleSet::SessionModelRuleMetadata& rule,
	                                                       const InferInstanceConclusion& conclusion,
	                                                       EvaluationContext& evaluationContext,
	                                                       SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		InferInstanceApplyResult result;

		if(source == nullptr)
		{
			AddDiagnostic(evaluationContext, "infer_instance_source_scope_unresolved", "InferInstance source scope could not be resolved.", rule.ruleId);

			return result;
		}

		std::shared_ptr<Relationship> relationship = ResolveInferRelationship(ruleSet, source, conclusion, evaluationContext);

		if(relationship == nullptr)
		{
			AddDiagnostic(evaluationContext, "infer_instance_relationship_not_found", "InferInstance relationship could not be resolved.", conclusion.relationshipArgument);

			return result;
		}

		if(source->entityId != relationship->sourceEntity)
		{
			AddDiagnostic(evaluationContext, "infer_instance_source_scope_unresolved", "InferInstance source scope does not match the relationship source entity.", relationship->id);

			return result;
		}

		std::shared_ptr<Entity> targetEntity = FindEntity(ruleSet, relationship->targetEntity);

		if(targetEntity == nullptr)
		{
			AddDiagnostic(evaluationContext, "infer_instance_relationship_not_found", "InferInstance relationship target entity could not be resolved.", relationship->id);

			return result;
		}

		const std::string inferredKey = BuildInferredInstanceKey(rule, source, drivingInstance, relationship);
		std::shared_ptr<Instance> inferredInstance = FindInferredInstanceByKey(ruleSet, targetEntity, inferredKey);
		const bool createdInstance = inferredInstance == nullptr;

		if(createdInstance)
		{
			if(!RelationshipAllowsMultipleOutgoingTargets(relationship) && HasDifferentOutgoingRelationshipTarget(source, relationship->id, inferredKey))
			{
				AddDiagnostic(evaluationContext, "infer_instance_relationship_cardinality", "InferInstance relationship cardinality does not allow another target.", relationship->id);

				return result;
			}

			inferredInstance = targetEntity->GetCleanInstance();
			inferredInstance->entityId = targetEntity->entityid;
			inferredInstance->SetInstanceId(inferredKey);
			inferredInstance->isInferred = true;
			inferredInstance->inferredInstanceKey = inferredKey;
			inferredInstance->createdByRuleId = rule.ruleId;
			inferredInstance->createdFromSourceInstanceId = source->instanceId;
			inferredInstance->createdFromDrivingInstanceId = drivingInstance != nullptr ? drivingInstance->instanceId : "";
			inferredInstance->createdThroughRelationshipId = relationship->id;
			inferredInstance->lastEvaluationState = "CertainTrue";

			targetEntity->instances.push_back(inferredInstance);
			inferredInstance->EvaluateFunctions();

			if(ruleSet.inferredInstanceIdsByKey != nullptr)
			{
				(*ruleSet.inferredInstanceIdsByKey)[inferredKey] = inferredInstance->instanceId;
			}

			stats.inferredInstancesCreated++;
			stats.createdInferredInstances.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange{
				inferredInstance->instanceId,
				inferredInstance->entityId,
				source->instanceId,
				drivingInstance != nullptr ? drivingInstance->instanceId : "",
				relationship->id,
				rule.ruleId
			});
		}
		else
		{
			if(!RelationshipAllowsMultipleOutgoingTargets(relationship) && HasDifferentOutgoingRelationshipTarget(source, relationship->id, inferredInstance->instanceId))
			{
				AddDiagnostic(evaluationContext, "infer_instance_relationship_cardinality", "InferInstance relationship cardinality does not allow another target.", relationship->id);

				return result;
			}

			inferredInstance->lastEvaluationState = "CertainTrue";

			stats.inferredInstancesReused++;
			stats.reusedInferredInstances.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange{
				inferredInstance->instanceId,
				inferredInstance->entityId,
				source->instanceId,
				drivingInstance != nullptr ? drivingInstance->instanceId : "",
				relationship->id,
				rule.ruleId
			});
		}

		const bool linkAlreadyExists = HasOutgoingRelationshipLink(source, relationship->id, inferredInstance->instanceId);

		source->AddLinkedInstance(inferredInstance);
		inferredInstance->AddLinkedInstance(source);
		source->AddRelationshipLink(inferredInstance, relationship, true);
		inferredInstance->AddRelationshipLink(source, relationship, false);

		if(linkAlreadyExists)
		{
			stats.inferredLinksReused++;
			stats.reusedInferredLinks.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange{
				relationship->id,
				source->instanceId,
				inferredInstance->instanceId,
				rule.ruleId
			});
		}
		else
		{
			stats.inferredLinksCreated++;
			stats.createdInferredLinks.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange{
				relationship->id,
				source->instanceId,
				inferredInstance->instanceId,
				rule.ruleId
			});

			result.createdRelationshipLink = true;
		}

		result.applied = true;
		result.relationshipId = relationship->id;
		result.sourceEntityId = source->entityId;
		result.sourceInstanceId = source->instanceId;

		return result;
	}

	std::vector<InferInstanceApplyResult> ApplyInferInstanceConclusion(RuleSet& ruleSet,
	                                                                   const std::shared_ptr<Instance>& source,
	                                                                   const RuleSet::SessionModelRuleMetadata& rule,
	                                                                   EvaluationContext& evaluationContext,
	                                                                   SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		std::vector<InferInstanceApplyResult> results;
		InferInstanceConclusion conclusion = ResolveInferInstanceConclusion(rule);

		if(!conclusion.isInfer || !conclusion.shapeSupported)
		{
			AddDiagnostic(evaluationContext, "unsupported_infer_instance_shape", "InferInstance supports InferInstance(relationship), InferInstanceFor(relationship, condition), and InferInstanceFor(relationship, alias, condition) as conclusion-side constructs.", rule.conclusionValue);

			return results;
		}

		if(conclusion.functionName == "InferInstance")
		{
			results.push_back(CreateOrReuseInferredInstance(ruleSet, source, nullptr, rule, conclusion, evaluationContext, stats));

			return results;
		}

		std::shared_ptr<Relationship> relationship = ResolveInferRelationship(ruleSet, source, conclusion, evaluationContext);

		if(relationship == nullptr)
		{
			AddDiagnostic(evaluationContext, "infer_instance_relationship_not_found", "InferInstanceFor relationship could not be resolved.", conclusion.relationshipArgument);

			return results;
		}

		std::vector<std::shared_ptr<Instance>> drivingInstances = ResolveRelatedInstances(source, relationship->targetEntity, relationship->id);

		for(const std::shared_ptr<Instance>& drivingInstance : drivingInstances)
		{
			if(drivingInstance == nullptr)
			{
				continue;
			}

			NodeResult condition = EvaluateInferCondition(ruleSet, drivingInstance, conclusion.conditionArgument, evaluationContext);

			if(condition == NodeResult::Unsupported)
			{
				continue;
			}

			if(condition != NodeResult::True)
			{
				continue;
			}

			results.push_back(CreateOrReuseInferredInstance(ruleSet, source, drivingInstance, rule, conclusion, evaluationContext, stats));
		}

		return results;
	}

	bool ApplyValueConclusion(const std::shared_ptr<Instance>& target,
	                          const RuleSet& ruleSet,
	                          const RuleSet::SessionModelRuleMetadata& rule,
	                          NodeResult conditionResult,
	                          EvaluationContext& evaluationContext)
	{
		if(target == nullptr || rule.conclusionMode != "Value")
		{
			return false;
		}

		std::shared_ptr<Attribute> attribute = target->GetAttributeByID(rule.conclusionAttributeId);

		if(conditionResult == NodeResult::Unknown)
		{
			return SetAttributeResolvedValue(attribute, ResolvedValue{true, false, ""}, true);
		}

		const bool conditionPassed = conditionResult == NodeResult::True;
		const bool conditionUncertain = conditionResult == NodeResult::Uncertain;
		const std::string valueMode = rule.conclusionValueMode.empty() ? "Literal" : rule.conclusionValueMode;
		const bool implicitBooleanLiteralConclusion = valueMode == "Literal"
			&& rule.conclusionValue.empty()
			&& attribute != nullptr
			&& attribute->GetType() == Attribute::BOOLEAN;

		if(!conditionPassed && rule.elseValue.empty() && !(conditionResult == NodeResult::False && implicitBooleanLiteralConclusion))
		{
			return false;
		}

		const json& valueNode = rule.conclusionExpression.is_object()
			? rule.conclusionExpression
			: json::object();
		std::string value = conditionPassed ? rule.conclusionValue : rule.elseValue;

		if(implicitBooleanLiteralConclusion)
		{
			value = conditionPassed ? "true" : "false";
		}

		ResolvedValue resolvedValue = ResolveConclusionValue(ruleSet, target, valueNode, target->entityId, valueMode, value, false, evaluationContext);

		if(conditionUncertain && resolvedValue.known)
		{
			resolvedValue.knowledgeState = Attribute::KnowledgeState::Uncertain;
		}

		return SetAttributeResolvedValue(attribute, resolvedValue, true);
	}

	bool IsDecisionTableRule(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return rule.hasExecutableDecisionTable || rule.conclusionMode == "DecisionTable";
	}

	bool IsRelationshipDecisionTableRule(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return rule.hasExecutableDecisionTable
			&& rule.conclusionMode == "Relationship"
			&& GetString(rule.executableDecisionTable, "matchPolicy") == "AllMatches";
	}

	std::string RuleTraceKind(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return IsDecisionTableRule(rule) ? "DecisionTable" : "Rule";
	}

	std::string RuleTraceLabel(const RuleSet::SessionModelRuleMetadata& rule)
	{
		if(!rule.ruleName.empty())
		{
			return rule.ruleName;
		}

		return IsDecisionTableRule(rule) ? "Decision table rule" : "Rule";
	}

	std::string RuleTraceAttributeId(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return rule.conclusionMode == "Value" ? rule.conclusionAttributeId : std::string();
	}

	std::string RuleTraceRelationshipId(const RuleSet::SessionModelRuleMetadata& rule, const RuleTrigger& trigger)
	{
		if(!trigger.relationshipId.empty())
		{
			return trigger.relationshipId;
		}

		return rule.conclusionMode == "Relationship" ? rule.relationshipId : std::string();
	}

	void AppendRuleTraceNode(SessionModelRuleEvaluator::EvaluationStats& stats,
	                         const RuleSet::SessionModelRuleMetadata& rule,
	                         const std::shared_ptr<Instance>& target,
	                         const RuleTrigger& trigger,
	                         NodeResult conditionResult,
	                         const std::string& conclusionDetail,
	                         const std::string& conclusionKnowledgeState,
	                         const std::vector<SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode>* conditionChildren = nullptr,
	                         const std::vector<SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode>* conclusionChildren = nullptr)
	{
		if(!stats.captureTrace)
		{
			return;
		}

		const std::string conditionText = NodeResultToTraceText(conditionResult);
		const std::string conditionKnowledgeState = NodeResultToTraceKnowledgeState(conditionResult);
		const std::string attributeId = RuleTraceAttributeId(rule);
		const std::string relationshipId = RuleTraceRelationshipId(rule, trigger);
		const std::string targetInstanceId = target != nullptr ? target->instanceId : std::string();
		const std::string targetDetail = targetInstanceId.empty() ? "default instance" : targetInstanceId;
		const std::string ruleDetail = "Target instance: " + targetDetail
			+ "; condition: " + conditionText
			+ "; conclusion: " + conclusionDetail;

		SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode ruleNode =
			CreateTraceNode(RuleTraceKind(rule),
			                RuleTraceLabel(rule),
			                ruleDetail,
			                conditionKnowledgeState,
			                rule,
			                target,
			                attributeId,
			                relationshipId);

		SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode conditionNode =
			CreateTraceNode("Condition",
			                "Condition",
			                "Result: " + conditionText,
			                conditionKnowledgeState,
			                rule,
			                target,
			                std::string(),
			                relationshipId);

		if(conditionChildren != nullptr)
		{
			conditionNode.children = *conditionChildren;
		}

		ruleNode.children.push_back(conditionNode);

		SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode conclusionNode =
			CreateTraceNode("Conclusion",
			                "Conclusion",
			                conclusionDetail,
			                conclusionKnowledgeState,
			                rule,
			                target,
			                attributeId,
			                relationshipId);

		if(conclusionChildren != nullptr)
		{
			conclusionNode.children = *conclusionChildren;
		}

		ruleNode.children.push_back(conclusionNode);

		stats.trace.push_back(ruleNode);
	}

	std::string DecisionTableRowTraceLabel(size_t rowIndex)
	{
		return "Row " + std::to_string(rowIndex + 1);
	}

	std::string DecisionTableRowTraceDetail(NodeResult result)
	{
		switch(result)
		{
			case NodeResult::True:
				return "Matched";
			case NodeResult::False:
				return "Did not match";
			case NodeResult::Unknown:
				return "Could not match: Unknown";
			case NodeResult::Uncertain:
				return "Could not match: Uncertain";
			case NodeResult::Unsupported:
				return "Unsupported";
			default:
				return "Not evaluated";
		}
	}

	std::string DecisionTableRowTraceKnowledgeState(NodeResult result)
	{
		return result == NodeResult::True
			? "Known"
			: result == NodeResult::False
				? "NotEvaluated"
				: NodeResultToTraceKnowledgeState(result);
	}

	EvaluationTraceNode CreateDecisionTableRowTraceNode(const RuleSet::SessionModelRuleMetadata& rule,
	                                                    const std::shared_ptr<Instance>& target,
	                                                    size_t rowIndex,
	                                                    NodeResult result)
	{
		return CreateTraceNode("DecisionRow",
		                       DecisionTableRowTraceLabel(rowIndex),
		                       DecisionTableRowTraceDetail(result),
		                       DecisionTableRowTraceKnowledgeState(result),
		                       rule,
		                       target,
		                       RuleTraceAttributeId(rule),
		                       rule.relationshipId);
	}

	EvaluationTraceNode CreateDecisionTableConditionCellsTraceNode(const RuleSet::SessionModelRuleMetadata& rule,
	                                                               const std::shared_ptr<Instance>& target,
	                                                               NodeResult result,
	                                                               std::vector<EvaluationTraceNode> children)
	{
		EvaluationTraceNode node = CreateTraceNode("Condition",
		                                           "Condition cells",
		                                           "Result: " + NodeResultToTraceText(result),
		                                           NodeResultToTraceKnowledgeState(result),
		                                           rule,
		                                           target,
		                                           std::string(),
		                                           rule.relationshipId);
		node.children = std::move(children);

		return node;
	}

	EvaluationTraceNode CreateDecisionTableConclusionCellsTraceNode(const RuleSet::SessionModelRuleMetadata& rule,
	                                                                const std::shared_ptr<Instance>& target,
	                                                                int changedCellCount,
	                                                                std::vector<EvaluationTraceNode> children)
	{
		EvaluationTraceNode node = CreateTraceNode("Conclusion",
		                                           "Conclusion cells",
		                                           changedCellCount > 0
			                                           ? "Applied conclusion cell changes: " + std::to_string(changedCellCount) + "."
			                                           : "Conclusion cells made no changes.",
		                                           changedCellCount > 0 ? "Known" : "NotEvaluated",
		                                           rule,
		                                           target,
		                                           RuleTraceAttributeId(rule),
		                                           rule.relationshipId);
		node.children = std::move(children);

		return node;
	}

	EvaluationTraceNode CreateDecisionTableConclusionCellsDiagnosticTraceNode(const RuleSet::SessionModelRuleMetadata& rule,
	                                                                          const std::shared_ptr<Instance>& target,
	                                                                          const std::string& detail)
	{
		return CreateTraceNode("Conclusion",
		                       "Conclusion cells",
		                       detail,
		                       "Unknown",
		                       rule,
		                       target,
		                       RuleTraceAttributeId(rule),
		                       rule.relationshipId);
	}

	EvaluationTraceNode CreateDecisionTableConclusionCellTraceNode(const RuleSet::SessionModelRuleMetadata& rule,
	                                                               const std::shared_ptr<Instance>& target,
	                                                               size_t cellIndex,
	                                                               const std::string& attributeId,
	                                                               bool changed,
	                                                               bool unsupported,
	                                                               std::vector<EvaluationTraceNode> children)
	{
		const std::string effectiveAttributeId = !attributeId.empty() ? attributeId : RuleTraceAttributeId(rule);
		std::string detail;
		std::string knowledgeState;

		if(unsupported)
		{
			detail = "Could not apply conclusion cell for attribute " + effectiveAttributeId + ".";
			knowledgeState = "Unknown";
		}
		else if(changed)
		{
			detail = "Changed attribute " + effectiveAttributeId + ".";
			knowledgeState = "Known";
		}
		else
		{
			detail = "No change for attribute " + effectiveAttributeId + ".";
			knowledgeState = "NotEvaluated";
		}

		EvaluationTraceNode node = CreateTraceNode("Conclusion",
		                                           "Conclusion cell " + std::to_string(cellIndex + 1),
		                                           detail,
		                                           knowledgeState,
		                                           rule,
		                                           target,
		                                           effectiveAttributeId,
		                                           rule.relationshipId);
		node.children = std::move(children);

		return node;
	}

	EvaluationTraceNode CreateRelationshipDecisionTableConclusionCellTraceNode(
		const RuleSet::SessionModelRuleMetadata& rule,
		const std::shared_ptr<Instance>& source,
		size_t cellIndex,
		const std::string& relationshipId,
		const std::string& detail,
		const std::string& knowledgeState,
		std::vector<EvaluationTraceNode> children)
	{
		EvaluationTraceNode node = CreateTraceNode("Conclusion",
		                                           "Conclusion cell " + std::to_string(cellIndex + 1),
		                                           detail,
		                                           knowledgeState,
		                                           rule,
		                                           source,
		                                           std::string(),
		                                           relationshipId);
		node.children = std::move(children);

		return node;
	}

	void AppendDecisionTableRowTrace(EvaluationContext& evaluationContext, EvaluationTraceNode rowTrace)
	{
		if(evaluationContext.traceSink != nullptr)
		{
			evaluationContext.traceSink->push_back(std::move(rowTrace));
		}
	}

	NodeResult EvaluateDecisionTableRow(const std::shared_ptr<Instance>& target,
	                                    const RuleSet& ruleSet,
	                                    const json& row,
	                                    EvaluationContext& evaluationContext)
	{
		if(!row.is_object())
		{
			AddDiagnostic(evaluationContext, "unsupported_decision_table_row", "Decision-table row must be an object.", row.dump());

			return NodeResult::Unsupported;
		}

		if(!row.contains("conditionCells") || !row.at("conditionCells").is_array() || row.at("conditionCells").empty())
		{
			return NodeResult::True;
		}

		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawKnown = false;

		for(const json& cell : row.at("conditionCells"))
		{
			NodeResult result = EvaluateNode(ruleSet, target, cell, evaluationContext);

			if(result == NodeResult::False)
			{
				return NodeResult::False;
			}

			if(result == NodeResult::Unsupported)
			{
				return NodeResult::Unsupported;
			}

			if(result == NodeResult::Unknown)
			{
				sawUnknown = true;
			}
			else if(result == NodeResult::Uncertain)
			{
				sawUncertain = true;
				sawKnown = true;
			}
			else if(result == NodeResult::True)
			{
				sawKnown = true;
			}
		}

		if(sawUncertain || (sawKnown && sawUnknown))
		{
			return NodeResult::Uncertain;
		}

		if(sawUnknown)
		{
			return NodeResult::Unknown;
		}

		return NodeResult::True;
	}

	const json* GetDecisionTableRowJson(const RuleSet::SessionModelRuleMetadata& rule,
	                                    const RuleSet::DecisionTableRowMetadata& rowMetadata)
	{
		if(!rule.executableDecisionTable.is_object()
			|| !rule.executableDecisionTable.contains("rows")
			|| !rule.executableDecisionTable.at("rows").is_array())
		{
			return nullptr;
		}

		const json& rows = rule.executableDecisionTable.at("rows");

		if(rowMetadata.rowIndex >= rows.size())
		{
			return nullptr;
		}

		return &rows.at(rowMetadata.rowIndex);
	}

	const json* GetDecisionTableConditionCellJson(const RuleSet::SessionModelRuleMetadata& rule,
	                                              const RuleSet::DecisionTableRowMetadata& rowMetadata,
	                                              size_t conditionCellIndex)
	{
		const json* row = GetDecisionTableRowJson(rule, rowMetadata);

		if(row == nullptr
			|| !row->is_object()
			|| !row->contains("conditionCells")
			|| !row->at("conditionCells").is_array())
		{
			return nullptr;
		}

		const json& conditionCells = row->at("conditionCells");

		if(conditionCellIndex >= conditionCells.size())
		{
			return nullptr;
		}

		return &conditionCells.at(conditionCellIndex);
	}

	const json* GetDecisionTableConclusionCellJson(const RuleSet::SessionModelRuleMetadata& rule,
	                                               const RuleSet::DecisionTableRowMetadata& rowMetadata,
	                                               const RuleSet::DecisionTableConclusionCellMetadata& cellMetadata)
	{
		const json* row = GetDecisionTableRowJson(rule, rowMetadata);

		if(row == nullptr
			|| !row->is_object()
			|| !row->contains("conclusionCells")
			|| !row->at("conclusionCells").is_array())
		{
			return nullptr;
		}

		const json& conclusionCells = row->at("conclusionCells");

		if(cellMetadata.cellIndex >= conclusionCells.size())
		{
			return nullptr;
		}

		return &conclusionCells.at(cellMetadata.cellIndex);
	}

	NodeResult EvaluateDecisionTableRow(const std::shared_ptr<Instance>& target,
	                                    const RuleSet& ruleSet,
	                                    const RuleSet::SessionModelRuleMetadata& rule,
	                                    const RuleSet::DecisionTableRowMetadata& rowMetadata,
	                                    EvaluationContext& evaluationContext)
	{
		const json* row = GetDecisionTableRowJson(rule, rowMetadata);

		if(row == nullptr || !rowMetadata.isObject)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_decision_table_row",
			              "Decision-table row must be an object.",
			              row != nullptr ? row->dump() : rule.ruleId);

			return NodeResult::Unsupported;
		}

		if(!rowMetadata.conditionCellsArray || rowMetadata.conditionCellsEmpty)
		{
			return NodeResult::True;
		}

		bool sawUnknown = false;
		bool sawUncertain = false;
		bool sawKnown = false;

		for(size_t conditionCellIndex = 0; conditionCellIndex < rowMetadata.conditionCellCount; ++conditionCellIndex)
		{
			const json* cell = GetDecisionTableConditionCellJson(rule, rowMetadata, conditionCellIndex);

			if(cell == nullptr)
			{
				AddDiagnostic(evaluationContext, "unsupported_decision_table_row", "Decision-table condition cell was not found.", rule.ruleId);

				return NodeResult::Unsupported;
			}

			NodeResult result = EvaluateNode(ruleSet, target, *cell, evaluationContext);

			if(result == NodeResult::False)
			{
				return NodeResult::False;
			}

			if(result == NodeResult::Unsupported)
			{
				return NodeResult::Unsupported;
			}

			if(result == NodeResult::Unknown)
			{
				sawUnknown = true;
			}
			else if(result == NodeResult::Uncertain)
			{
				sawUncertain = true;
				sawKnown = true;
			}
			else if(result == NodeResult::True)
			{
				sawKnown = true;
			}
		}

		if(sawUncertain || (sawKnown && sawUnknown))
		{
			return NodeResult::Uncertain;
		}

		if(sawUnknown)
		{
			return NodeResult::Unknown;
		}

		return NodeResult::True;
	}

	bool ApplyDecisionTableConclusionCell(const std::shared_ptr<Instance>& target,
	                                      const RuleSet& ruleSet,
	                                      const RuleSet::SessionModelRuleMetadata& rule,
	                                      const json& cell,
	                                      EvaluationContext& evaluationContext,
	                                      AttributeChange& changedAttribute)
	{
		if(target == nullptr || !cell.is_object())
		{
			AddDiagnostic(evaluationContext, "unsupported_decision_table_conclusion_cell", "Conclusion cell must be an object.", cell.dump());

			return false;
		}

		std::string attributeId = GetString(cell, "attributeId");

		if(attributeId.empty())
		{
			attributeId = rule.conclusionAttributeId;
		}

		const std::string valueMode = GetString(cell, "valueMode");
		const std::string value = GetString(cell, "value");
		ResolvedValue resolvedValue = ResolveConclusionValue(ruleSet, target, cell, target->entityId, valueMode, value, false, evaluationContext);
		std::shared_ptr<Attribute> attribute = target->GetAttributeByID(attributeId);

		if(attribute == nullptr)
		{
			AddDiagnostic(evaluationContext, "decision_table_conclusion_attribute_missing", "Decision-table conclusion attribute was not found on target instance.", attributeId);

			return false;
		}

		if(SetAttributeResolvedValue(attribute, resolvedValue, false))
		{
			changedAttribute = AttributeChange{target->entityId, target->instanceId, attributeId};

			return true;
		}

		return false;
	}

	bool ApplyDecisionTableConclusionCell(const std::shared_ptr<Instance>& target,
	                                      const RuleSet& ruleSet,
	                                      const RuleSet::SessionModelRuleMetadata& rule,
	                                      const RuleSet::DecisionTableRowMetadata& rowMetadata,
	                                      const RuleSet::DecisionTableConclusionCellMetadata& cellMetadata,
	                                      EvaluationContext& evaluationContext,
	                                      AttributeChange& changedAttribute)
	{
		const json* cell = GetDecisionTableConclusionCellJson(rule, rowMetadata, cellMetadata);

		if(target == nullptr || cell == nullptr || !cellMetadata.isObject)
		{
			AddDiagnostic(evaluationContext,
			              "unsupported_decision_table_conclusion_cell",
			              "Conclusion cell must be an object.",
			              cell != nullptr ? cell->dump() : rule.ruleId);

			return false;
		}

		std::string attributeId = cellMetadata.attributeId;

		if(attributeId.empty())
		{
			attributeId = rule.conclusionAttributeId;
		}

		ResolvedValue resolvedValue = ResolveConclusionValue(ruleSet, target, *cell, target->entityId, cellMetadata.valueMode, cellMetadata.value, false, evaluationContext);
		std::shared_ptr<Attribute> attribute = target->GetAttributeByID(attributeId);

		if(attribute == nullptr)
		{
			AddDiagnostic(evaluationContext, "decision_table_conclusion_attribute_missing", "Decision-table conclusion attribute was not found on target instance.", attributeId);

			return false;
		}

		if(SetAttributeResolvedValue(attribute, resolvedValue, false))
		{
			changedAttribute = AttributeChange{target->entityId, target->instanceId, attributeId};

			return true;
		}

		return false;
	}

	struct DecisionTableResult
	{
		bool unsupported = false;
		bool matched = false;
		int appliedConclusions = 0;
		std::vector<AttributeChange> changedAttributes;
		std::vector<RelationshipChange> changedRelationships;
	};

	void PushUniqueAttributeId(std::vector<std::string>& attributeIds, std::unordered_set<std::string>& seenAttributeIds, const std::string& attributeId)
	{
		if(!attributeId.empty() && seenAttributeIds.insert(attributeId).second)
		{
			attributeIds.push_back(attributeId);
		}
	}

	std::vector<std::string> CollectDecisionTableConclusionAttributeIds(const RuleSet::SessionModelRuleMetadata& rule)
	{
		std::vector<std::string> attributeIds;
		std::unordered_set<std::string> seenAttributeIds;

		PushUniqueAttributeId(attributeIds, seenAttributeIds, rule.conclusionAttributeId);

		if(rule.decisionTableMetadataBuilt)
		{
			for(const RuleSet::DecisionTableRowMetadata& rowMetadata : rule.decisionTableRowMetadata)
			{
				for(const RuleSet::DecisionTableConclusionCellMetadata& cellMetadata : rowMetadata.conclusionCells)
				{
					PushUniqueAttributeId(attributeIds, seenAttributeIds, cellMetadata.attributeId);
				}
			}

			return attributeIds;
		}

		if(!rule.executableDecisionTable.is_object()
			|| !rule.executableDecisionTable.contains("rows")
			|| !rule.executableDecisionTable.at("rows").is_array())
		{
			return attributeIds;
		}

		for(const json& row : rule.executableDecisionTable.at("rows"))
		{
			if(!row.is_object() || !row.contains("conclusionCells") || !row.at("conclusionCells").is_array())
			{
				continue;
			}

			for(const json& cell : row.at("conclusionCells"))
			{
				PushUniqueAttributeId(attributeIds, seenAttributeIds, GetString(cell, "attributeId"));
			}
		}

		return attributeIds;
	}

	void ClearDecisionTableConclusionAttributes(const std::shared_ptr<Instance>& target,
	                                            const RuleSet::SessionModelRuleMetadata& rule,
	                                            std::vector<AttributeChange>& changedAttributes)
	{
		if(target == nullptr)
		{
			return;
		}

		for(const std::string& attributeId : CollectDecisionTableConclusionAttributeIds(rule))
		{
			std::shared_ptr<Attribute> attribute = target->GetAttributeByID(attributeId);

			if(attribute == nullptr || attribute->IsUnknown())
			{
				continue;
			}

			attribute->MakeUnknown(false);
			changedAttributes.push_back(AttributeChange{target->entityId, target->instanceId, attributeId});
		}
	}

	std::string BuildRelationshipDecisionTableInferredKey(
		const RuleSet::SessionModelRuleMetadata& rule,
		const std::shared_ptr<Instance>& source,
		const std::shared_ptr<Relationship>& relationship,
		const ResolvedValue& identity)
	{
		const std::string sourceInstanceId = source != nullptr ? source->instanceId : std::string();
		const std::string relationshipId = relationship != nullptr ? relationship->id : std::string();
		const std::string typedIdentity = std::to_string(static_cast<int>(identity.valueType)) + ":" + identity.value;

		return "inferred-dt:"
			+ std::to_string(rule.ruleId.size()) + ":" + rule.ruleId + ":"
			+ std::to_string(sourceInstanceId.size()) + ":" + sourceInstanceId + ":"
			+ std::to_string(relationshipId.size()) + ":" + relationshipId + ":"
			+ std::to_string(typedIdentity.size()) + ":" + typedIdentity;
	}

	struct RelationshipDecisionTableCellPlan
	{
		size_t cellIndex = 0;
		std::shared_ptr<Relationship> relationship;
		std::shared_ptr<Entity> targetEntity;
		ResolvedValue identity;
		std::string inferredKey;
		std::vector<EvaluationTraceNode> valueTrace;
	};

	struct RelationshipDecisionTableRowPlan
	{
		bool matched = false;
		EvaluationTraceNode rowTrace;
		std::vector<RelationshipDecisionTableCellPlan> cells;
		std::vector<EvaluationTraceNode> conclusionTraces;
	};

	struct RelationshipDecisionTableApplyResult
	{
		bool applied = false;
		bool createdInstance = false;
		bool createdLink = false;
		bool identifierChanged = false;
		std::shared_ptr<Instance> instance;
	};

	RelationshipDecisionTableApplyResult ApplyRelationshipDecisionTableCell(
		RuleSet& ruleSet,
		const std::shared_ptr<Instance>& source,
		const RuleSet::SessionModelRuleMetadata& rule,
		const RelationshipDecisionTableCellPlan& plan,
		SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		RelationshipDecisionTableApplyResult result;
		std::shared_ptr<Instance> inferredInstance = FindInferredInstanceByKey(ruleSet, plan.targetEntity, plan.inferredKey);
		result.createdInstance = inferredInstance == nullptr;

		if(result.createdInstance)
		{
			inferredInstance = plan.targetEntity->GetCleanInstance();
			inferredInstance->entityId = plan.targetEntity->entityid;
			inferredInstance->SetInstanceId(plan.inferredKey);
			inferredInstance->isInferred = true;
			inferredInstance->inferredInstanceKey = plan.inferredKey;
			inferredInstance->createdByRuleId = rule.ruleId;
			inferredInstance->createdFromSourceInstanceId = source->instanceId;
			inferredInstance->createdThroughRelationshipId = plan.relationship->id;
			inferredInstance->lastEvaluationState = "CertainTrue";

			plan.targetEntity->instances.push_back(inferredInstance);

			if(ruleSet.inferredInstanceIdsByKey != nullptr)
			{
				(*ruleSet.inferredInstanceIdsByKey)[plan.inferredKey] = inferredInstance->instanceId;
			}

			stats.inferredInstancesCreated++;
			stats.createdInferredInstances.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange{
				inferredInstance->instanceId,
				inferredInstance->entityId,
				source->instanceId,
				std::string(),
				plan.relationship->id,
				rule.ruleId
			});
		}
		else
		{
			inferredInstance->lastEvaluationState = "CertainTrue";
			stats.inferredInstancesReused++;
			stats.reusedInferredInstances.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange{
				inferredInstance->instanceId,
				inferredInstance->entityId,
				source->instanceId,
				std::string(),
				plan.relationship->id,
				rule.ruleId
			});
		}

		if(plan.targetEntity->identifyingAttribute != nullptr)
		{
			const std::string identifierAttributeId = plan.targetEntity->identifyingAttribute->GetID();
			result.identifierChanged = SetAttributeResolvedValue(
				inferredInstance->GetAttributeByID(identifierAttributeId), plan.identity, false);
		}

		inferredInstance->EvaluateFunctions();
		const bool linkAlreadyExists = HasOutgoingRelationshipLink(source, plan.relationship->id, inferredInstance->instanceId);

		source->AddLinkedInstance(inferredInstance);
		inferredInstance->AddLinkedInstance(source);
		source->AddRelationshipLink(inferredInstance, plan.relationship, true);
		inferredInstance->AddRelationshipLink(source, plan.relationship, false);

		if(linkAlreadyExists)
		{
			stats.inferredLinksReused++;
			stats.reusedInferredLinks.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange{
				plan.relationship->id,
				source->instanceId,
				inferredInstance->instanceId,
				rule.ruleId
			});
		}
		else
		{
			result.createdLink = true;
			stats.inferredLinksCreated++;
			stats.createdInferredLinks.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange{
				plan.relationship->id,
				source->instanceId,
				inferredInstance->instanceId,
				rule.ruleId
			});
		}

		result.applied = true;
		result.instance = inferredInstance;

		return result;
	}

	int RetractRelationshipDecisionTableInstances(
		RuleSet& ruleSet,
		const std::shared_ptr<Instance>& source,
		const RuleSet::SessionModelRuleMetadata& rule,
		const std::shared_ptr<Relationship>& relationship,
		const std::unordered_set<std::string>& desiredKeys,
		SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		std::shared_ptr<Entity> targetEntity = relationship != nullptr ? FindEntity(ruleSet, relationship->targetEntity) : nullptr;

		if(source == nullptr || relationship == nullptr || targetEntity == nullptr)
		{
			return 0;
		}

		std::vector<std::shared_ptr<Instance>> removals;

		for(const std::shared_ptr<Instance>& instance : targetEntity->instances)
		{
			if(instance != nullptr
				&& instance->isInferred
				&& instance->createdByRuleId == rule.ruleId
				&& instance->createdFromSourceInstanceId == source->instanceId
				&& instance->createdThroughRelationshipId == relationship->id
				&& desiredKeys.find(instance->inferredInstanceKey) == desiredKeys.end())
			{
				removals.push_back(instance);
			}
		}

		for(const std::shared_ptr<Instance>& instance : removals)
		{
			const bool removedRuleLink = HasOutgoingRelationshipLink(source, relationship->id, instance->instanceId);
			const std::vector<std::shared_ptr<Instance>> linkedInstances = instance->linkedInstances;

			for(const std::shared_ptr<Instance>& linkedInstance : linkedInstances)
			{
				if(linkedInstance == nullptr)
				{
					continue;
				}

				linkedInstance->linkedInstances.erase(
					std::remove(linkedInstance->linkedInstances.begin(), linkedInstance->linkedInstances.end(), instance),
					linkedInstance->linkedInstances.end());
				linkedInstance->relationshipLinks.erase(
					std::remove_if(linkedInstance->relationshipLinks.begin(), linkedInstance->relationshipLinks.end(),
					               [&instance](const InstanceRelationshipLink& link) {
						               return link.sourceInstanceId == instance->instanceId
							               || link.targetInstanceId == instance->instanceId;
					               }),
					linkedInstance->relationshipLinks.end());
				linkedInstance->InvalidateRelationshipLinkIndex();
				linkedInstance->EvaluateFunctions();
			}

			instance->linkedInstances.clear();
			instance->relationshipLinks.clear();
			instance->InvalidateRelationshipLinkIndex();

			if(ruleSet.inferredInstanceIdsByKey != nullptr)
			{
				ruleSet.inferredInstanceIdsByKey->erase(instance->inferredInstanceKey);
			}

			stats.inferredInstancesRemoved++;
			stats.removedInferredInstances.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredInstanceChange{
				instance->instanceId,
				instance->entityId,
				source->instanceId,
				instance->createdFromDrivingInstanceId,
				relationship->id,
				rule.ruleId
			});

			if(removedRuleLink)
			{
				stats.inferredLinksRemoved++;
				stats.removedInferredLinks.push_back(SessionModelRuleEvaluator::EvaluationStats::InferredLinkChange{
					relationship->id,
					source->instanceId,
					instance->instanceId,
					rule.ruleId
				});
			}

			targetEntity->RemoveInstanceById(instance->instanceId);
		}

		return static_cast<int>(removals.size());
	}

	DecisionTableResult EvaluateRelationshipDecisionTable(
		const std::shared_ptr<Instance>& source,
		RuleSet& ruleSet,
		const RuleSet::SessionModelRuleMetadata& rule,
		NodeResult outerConditionResult,
		EvaluationContext& evaluationContext,
		SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		DecisionTableResult result;

		if(source == nullptr
			|| !rule.executableDecisionTable.is_object()
			|| !rule.executableDecisionTable.contains("rows")
			|| !rule.executableDecisionTable.at("rows").is_array())
		{
			AddDiagnostic(evaluationContext, "decision_table_payload_missing", "Relationship decision table has no executable rows payload.", rule.ruleId);
			result.unsupported = true;

			return result;
		}

		const json& rows = rule.executableDecisionTable.at("rows");
		std::vector<std::shared_ptr<Relationship>> tableRelationships;
		std::unordered_set<std::string> seenRelationshipIds;

		for(const json& row : rows)
		{
			if(!row.is_object() || !row.contains("conclusionCells") || !row.at("conclusionCells").is_array())
			{
				continue;
			}

			for(const json& cell : row.at("conclusionCells"))
			{
				std::string relationshipId = GetString(cell, "relationshipId");
				if(relationshipId.empty())
				{
					relationshipId = rule.relationshipId;
				}
				std::shared_ptr<Relationship> relationship = FindRelationshipById(ruleSet, relationshipId);

				if(relationship == nullptr || source->entityId != relationship->sourceEntity)
				{
					AddDiagnostic(evaluationContext,
					              "decision_table_relationship_not_found",
					              "Relationship decision-table conclusion could not resolve a relationship from the source entity.",
					              relationshipId);
					result.unsupported = true;

					return result;
				}

				if(seenRelationshipIds.insert(relationship->id).second)
				{
					tableRelationships.push_back(relationship);
				}
			}
		}

		if(tableRelationships.empty())
		{
			AddDiagnostic(evaluationContext, "decision_table_relationship_missing", "Relationship decision table has no relationship conclusion cells.", rule.ruleId);
			result.unsupported = true;

			return result;
		}

		std::vector<RelationshipDecisionTableRowPlan> rowPlans;
		std::unordered_set<std::string> desiredKeys;

		for(size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
		{
			const json& row = rows.at(rowIndex);
			NodeResult rowResult = outerConditionResult;
			std::vector<EvaluationTraceNode> rowConditionChildren;
			if(outerConditionResult == NodeResult::True)
			{
				TraceSinkCapture rowConditionCapture(evaluationContext);
				rowResult = EvaluateDecisionTableRow(source, ruleSet, row, evaluationContext);
				rowConditionChildren = rowConditionCapture.Release();
			}
			RelationshipDecisionTableRowPlan rowPlan;
			rowPlan.rowTrace = CreateDecisionTableRowTraceNode(rule, source, rowIndex, rowResult);
			rowPlan.rowTrace.children.push_back(CreateDecisionTableConditionCellsTraceNode(
				rule, source, rowResult, std::move(rowConditionChildren)));

			if(rowResult == NodeResult::Unsupported)
			{
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowPlan.rowTrace));
				result.unsupported = true;

				return result;
			}

			if(rowResult != NodeResult::True)
			{
				rowPlans.push_back(std::move(rowPlan));
				continue;
			}

			result.matched = true;
			rowPlan.matched = true;

			if(!row.is_object() || !row.contains("conclusionCells") || !row.at("conclusionCells").is_array())
			{
				rowPlan.rowTrace.children.push_back(CreateDecisionTableConclusionCellsDiagnosticTraceNode(
					rule, source, "Matching relationship decision-table row has no conclusionCells array."));
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowPlan.rowTrace));
				AddDiagnostic(evaluationContext,
				              "decision_table_conclusion_cells_missing",
				              "Matching relationship decision-table row has no conclusionCells array.",
				              row.dump());
				result.unsupported = true;

				return result;
			}

			const json& conclusionCells = row.at("conclusionCells");
			for(size_t cellIndex = 0; cellIndex < conclusionCells.size(); ++cellIndex)
			{
				const json& cell = conclusionCells.at(cellIndex);

				if(!cell.is_object())
				{
					AddDiagnostic(evaluationContext, "unsupported_decision_table_conclusion_cell", "Relationship conclusion cell must be an object.", cell.dump());
					result.unsupported = true;

					return result;
				}

				std::string relationshipId = GetString(cell, "relationshipId");
				if(relationshipId.empty())
				{
					relationshipId = rule.relationshipId;
				}
				std::shared_ptr<Relationship> relationship = FindRelationshipById(ruleSet, relationshipId);
				std::shared_ptr<Entity> targetEntity = relationship != nullptr ? FindEntity(ruleSet, relationship->targetEntity) : nullptr;
				const std::string declaredTargetEntityId = GetString(cell, "entityId");

				if(relationship == nullptr
					|| targetEntity == nullptr
					|| (!declaredTargetEntityId.empty() && declaredTargetEntityId != relationship->targetEntity))
				{
					AddDiagnostic(evaluationContext,
					              "decision_table_relationship_target_mismatch",
					              "Relationship decision-table conclusion target does not match the relationship target entity.",
					              relationshipId + ":" + declaredTargetEntityId);
					result.unsupported = true;

					return result;
				}

				TraceSinkCapture valueCapture(evaluationContext);
				ResolvedValue identity = ResolveConclusionValue(
					ruleSet,
					source,
					cell,
					source->entityId,
					GetString(cell, "valueMode"),
					GetString(cell, "value"),
					false,
					evaluationContext);
				std::vector<EvaluationTraceNode> valueTrace = valueCapture.Release();

				if(!identity.supported)
				{
					AddDiagnostic(evaluationContext,
					              "decision_table_relationship_identity_unsupported",
					              "Relationship decision-table instance identity could not be evaluated.",
					              relationshipId);
					result.unsupported = true;

					return result;
				}

				if(!identity.known)
				{
					rowPlan.conclusionTraces.push_back(CreateRelationshipDecisionTableConclusionCellTraceNode(
							rule,
							source,
							cellIndex,
							relationshipId,
							"Inferred instance identity is unknown; no instance was selected.",
							"Unknown",
							std::move(valueTrace)));
					continue;
				}

				if(targetEntity->identifyingAttribute != nullptr)
				{
					ValueVariant converted;
					if(!Attribute::TryConvertValue(
						identity.value,
						static_cast<Attribute::AttributeType>(targetEntity->identifyingAttribute->GetType()),
						converted))
					{
						AddDiagnostic(evaluationContext,
						              "decision_table_relationship_identity_type",
						              "Relationship decision-table identity does not match the target entity identifier type.",
						              identity.value);
						result.unsupported = true;

						return result;
					}
				}

				RelationshipDecisionTableCellPlan cellPlan;
				cellPlan.cellIndex = cellIndex;
				cellPlan.relationship = relationship;
				cellPlan.targetEntity = targetEntity;
				cellPlan.identity = identity;
				cellPlan.inferredKey = BuildRelationshipDecisionTableInferredKey(rule, source, relationship, identity);
				cellPlan.valueTrace = std::move(valueTrace);
				desiredKeys.insert(cellPlan.inferredKey);
				rowPlan.cells.push_back(std::move(cellPlan));
			}

			rowPlans.push_back(std::move(rowPlan));
		}

		for(const std::shared_ptr<Relationship>& relationship : tableRelationships)
		{
			if(RelationshipAllowsMultipleOutgoingTargets(relationship))
			{
				continue;
			}

			std::unordered_set<std::string> relationshipDesiredKeys;
			for(const RelationshipDecisionTableRowPlan& rowPlan : rowPlans)
			{
				for(const RelationshipDecisionTableCellPlan& cellPlan : rowPlan.cells)
				{
					if(cellPlan.relationship->id == relationship->id)
					{
						relationshipDesiredKeys.insert(cellPlan.inferredKey);
					}
				}
			}

			if(relationshipDesiredKeys.size() > 1)
			{
				AddDiagnostic(evaluationContext,
				              "decision_table_relationship_cardinality",
				              "Multiple relationship decision-table rows matched a relationship that allows only one target.",
				              relationship->id);
				result.unsupported = true;

				return result;
			}

			if(!relationshipDesiredKeys.empty())
			{
				std::shared_ptr<Entity> targetEntity = FindEntity(ruleSet, relationship->targetEntity);
				for(const InstanceRelationshipLink& link : source->relationshipLinks)
				{
					if(!link.outgoing
						|| link.relationshipId != relationship->id
						|| relationshipDesiredKeys.find(link.targetInstanceId) != relationshipDesiredKeys.end())
					{
						continue;
					}

					std::shared_ptr<Instance> existingTarget = targetEntity != nullptr
						? targetEntity->GetInstanceById(link.targetInstanceId)
						: nullptr;
					const bool isRetractableRuleTarget = existingTarget != nullptr
						&& existingTarget->isInferred
						&& existingTarget->createdByRuleId == rule.ruleId
						&& existingTarget->createdFromSourceInstanceId == source->instanceId
						&& existingTarget->createdThroughRelationshipId == relationship->id;

					if(!isRetractableRuleTarget)
					{
						AddDiagnostic(evaluationContext,
						              "decision_table_relationship_cardinality",
						              "Relationship decision-table result would replace an existing target on a relationship that allows only one target.",
						              relationship->id);
						result.unsupported = true;

						return result;
					}
				}
			}
		}

		std::unordered_set<std::string> appliedKeys;
		for(RelationshipDecisionTableRowPlan& rowPlan : rowPlans)
		{
			if(!rowPlan.matched)
			{
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowPlan.rowTrace));
				continue;
			}

			std::vector<EvaluationTraceNode> conclusionTraces = std::move(rowPlan.conclusionTraces);
			int rowAppliedCount = 0;

			for(const RelationshipDecisionTableCellPlan& cellPlan : rowPlan.cells)
			{
				if(!appliedKeys.insert(cellPlan.inferredKey).second)
				{
					conclusionTraces.push_back(CreateRelationshipDecisionTableConclusionCellTraceNode(
						rule,
						source,
						cellPlan.cellIndex,
						cellPlan.relationship->id,
						"Matched the same inferred instance identity as an earlier row.",
						"Known",
						cellPlan.valueTrace));
					result.appliedConclusions++;
					rowAppliedCount++;
					continue;
				}

				RelationshipDecisionTableApplyResult applyResult = ApplyRelationshipDecisionTableCell(
					ruleSet, source, rule, cellPlan, stats);
				if(!applyResult.applied)
				{
					result.unsupported = true;

					return result;
				}

				if(applyResult.identifierChanged && cellPlan.targetEntity->identifyingAttribute != nullptr)
				{
					result.changedAttributes.push_back(AttributeChange{
						cellPlan.targetEntity->entityid,
						applyResult.instance->instanceId,
						cellPlan.targetEntity->identifyingAttribute->GetID()
					});
				}

				if(applyResult.createdLink)
				{
					result.changedRelationships.push_back(RelationshipChange{
						source->entityId, source->instanceId, cellPlan.relationship->id, rule.ruleId
					});
				}

				conclusionTraces.push_back(CreateRelationshipDecisionTableConclusionCellTraceNode(
					rule,
					source,
					cellPlan.cellIndex,
					cellPlan.relationship->id,
					applyResult.createdInstance
						? "Created inferred instance for identity '" + cellPlan.identity.value + "'."
						: "Reused inferred instance for identity '" + cellPlan.identity.value + "'.",
					"Known",
					cellPlan.valueTrace));
				result.appliedConclusions++;
				rowAppliedCount++;
			}

			rowPlan.rowTrace.children.push_back(CreateDecisionTableConclusionCellsTraceNode(
				rule, source, rowAppliedCount, std::move(conclusionTraces)));
			AppendDecisionTableRowTrace(evaluationContext, std::move(rowPlan.rowTrace));
		}

		int removedCount = 0;
		for(const std::shared_ptr<Relationship>& relationship : tableRelationships)
		{
			const int relationshipRemoved = RetractRelationshipDecisionTableInstances(
				ruleSet, source, rule, relationship, desiredKeys, stats);
			removedCount += relationshipRemoved;

			if(relationshipRemoved > 0)
			{
				result.changedRelationships.push_back(RelationshipChange{
					source->entityId, source->instanceId, relationship->id, rule.ruleId
				});
			}
		}

		if(removedCount > 0)
		{
			result.appliedConclusions += removedCount;
			if(evaluationContext.traceSink != nullptr)
			{
				evaluationContext.traceSink->push_back(CreateTraceNode(
					"Conclusion",
					"Inferred instance retraction",
					"Removed inferred instances no longer selected by matching rows: " + std::to_string(removedCount) + ".",
					"Known",
					rule,
					source,
					std::string(),
					rule.relationshipId));
			}
		}

		return result;
	}

	DecisionTableResult EvaluateDecisionTable(const std::shared_ptr<Instance>& target,
	                                          const RuleSet& ruleSet,
	                                          const RuleSet::SessionModelRuleMetadata& rule,
	                                          EvaluationContext& evaluationContext)
	{
		DecisionTableResult result;

		if(!rule.executableDecisionTable.is_object() || !rule.executableDecisionTable.contains("rows") || !rule.executableDecisionTable.at("rows").is_array())
		{
			AddDiagnostic(evaluationContext, "decision_table_payload_missing", "Decision-table rule has no executable rows payload.", rule.ruleId);
			result.unsupported = true;

			return result;
		}

		if(rule.decisionTableMetadataBuilt)
		{
			for(const RuleSet::DecisionTableRowMetadata& rowMetadata : rule.decisionTableRowMetadata)
			{
				TraceSinkCapture rowConditionCapture(evaluationContext);
				NodeResult rowResult = EvaluateDecisionTableRow(target, ruleSet, rule, rowMetadata, evaluationContext);
				std::vector<EvaluationTraceNode> rowConditionChildren = rowConditionCapture.Release();
				EvaluationTraceNode rowTrace = CreateDecisionTableRowTraceNode(rule, target, rowMetadata.rowIndex, rowResult);
				rowTrace.children.push_back(CreateDecisionTableConditionCellsTraceNode(rule, target, rowResult, std::move(rowConditionChildren)));

				if(rowResult == NodeResult::Unsupported)
				{
					AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
					result.unsupported = true;

					return result;
				}

				if(rowResult == NodeResult::False || rowResult == NodeResult::Unknown || rowResult == NodeResult::Uncertain)
				{
					AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
					continue;
				}

				result.matched = true;

				if(!rowMetadata.conclusionCellsArray)
				{
					const json* row = GetDecisionTableRowJson(rule, rowMetadata);
					rowTrace.children.push_back(CreateDecisionTableConclusionCellsDiagnosticTraceNode(rule,
					                                                                                  target,
					                                                                                  "Matching decision-table row has no conclusionCells array."));
					AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));

					AddDiagnostic(evaluationContext,
					              "decision_table_conclusion_cells_missing",
					              "Matching decision-table row has no conclusionCells array.",
					              row != nullptr ? row->dump() : rule.ruleId);
					result.unsupported = true;

					return result;
				}

				std::vector<EvaluationTraceNode> conclusionCellTraces;
				int changedCellCount = 0;

				for(const RuleSet::DecisionTableConclusionCellMetadata& cellMetadata : rowMetadata.conclusionCells)
				{
					AttributeChange changedAttribute;
					const size_t diagnosticCountBefore = evaluationContext.diagnostics != nullptr ? evaluationContext.diagnostics->size() : 0;
					const std::string attributeId = cellMetadata.attributeId.empty() ? rule.conclusionAttributeId : cellMetadata.attributeId;
					TraceSinkCapture conclusionCellCapture(evaluationContext);
					const bool changed = ApplyDecisionTableConclusionCell(target, ruleSet, rule, rowMetadata, cellMetadata, evaluationContext, changedAttribute);
					std::vector<EvaluationTraceNode> conclusionCellChildren = conclusionCellCapture.Release();
					const bool unsupportedCell = !changed
						&& evaluationContext.diagnostics != nullptr
						&& evaluationContext.diagnostics->size() > diagnosticCountBefore;

					conclusionCellTraces.push_back(CreateDecisionTableConclusionCellTraceNode(rule,
					                                                                          target,
					                                                                          cellMetadata.cellIndex,
					                                                                          attributeId,
					                                                                          changed,
					                                                                          unsupportedCell,
					                                                                          std::move(conclusionCellChildren)));

					if(changed)
					{
						result.changedAttributes.push_back(changedAttribute);
						changedCellCount++;
					}
					else if(unsupportedCell)
					{
						rowTrace.children.push_back(CreateDecisionTableConclusionCellsTraceNode(rule, target, changedCellCount, std::move(conclusionCellTraces)));
						AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
						result.unsupported = true;

						return result;
					}
				}

				rowTrace.children.push_back(CreateDecisionTableConclusionCellsTraceNode(rule, target, changedCellCount, std::move(conclusionCellTraces)));
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));

				return result;
			}

			ClearDecisionTableConclusionAttributes(target, rule, result.changedAttributes);

			return result;
		}

		const json& rows = rule.executableDecisionTable.at("rows");

		for(size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
		{
			const json& row = rows.at(rowIndex);
			TraceSinkCapture rowConditionCapture(evaluationContext);
			NodeResult rowResult = EvaluateDecisionTableRow(target, ruleSet, row, evaluationContext);
			std::vector<EvaluationTraceNode> rowConditionChildren = rowConditionCapture.Release();
			EvaluationTraceNode rowTrace = CreateDecisionTableRowTraceNode(rule, target, rowIndex, rowResult);
			rowTrace.children.push_back(CreateDecisionTableConditionCellsTraceNode(rule, target, rowResult, std::move(rowConditionChildren)));

			if(rowResult == NodeResult::Unsupported)
			{
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
				result.unsupported = true;

				return result;
			}

			if(rowResult == NodeResult::False || rowResult == NodeResult::Unknown || rowResult == NodeResult::Uncertain)
			{
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
				continue;
			}

			result.matched = true;

			if(!row.contains("conclusionCells") || !row.at("conclusionCells").is_array())
			{
				rowTrace.children.push_back(CreateDecisionTableConclusionCellsDiagnosticTraceNode(rule,
				                                                                                  target,
				                                                                                  "Matching decision-table row has no conclusionCells array."));
				AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
				AddDiagnostic(evaluationContext, "decision_table_conclusion_cells_missing", "Matching decision-table row has no conclusionCells array.", row.dump());
				result.unsupported = true;

				return result;
			}

			std::vector<EvaluationTraceNode> conclusionCellTraces;
			int changedCellCount = 0;

			for(const json& cell : row.at("conclusionCells"))
			{
				AttributeChange changedAttribute;
				const size_t diagnosticCountBefore = evaluationContext.diagnostics != nullptr ? evaluationContext.diagnostics->size() : 0;
				std::string attributeId = GetString(cell, "attributeId");

				if(attributeId.empty())
				{
					attributeId = rule.conclusionAttributeId;
				}

				TraceSinkCapture conclusionCellCapture(evaluationContext);
				const bool changed = ApplyDecisionTableConclusionCell(target, ruleSet, rule, cell, evaluationContext, changedAttribute);
				std::vector<EvaluationTraceNode> conclusionCellChildren = conclusionCellCapture.Release();
				const bool unsupportedCell = !changed
					&& evaluationContext.diagnostics != nullptr
					&& evaluationContext.diagnostics->size() > diagnosticCountBefore;

				conclusionCellTraces.push_back(CreateDecisionTableConclusionCellTraceNode(rule,
				                                                                          target,
				                                                                          conclusionCellTraces.size(),
				                                                                          attributeId,
				                                                                          changed,
				                                                                          unsupportedCell,
				                                                                          std::move(conclusionCellChildren)));

				if(changed)
				{
					result.changedAttributes.push_back(changedAttribute);
					changedCellCount++;
				}
				else if(unsupportedCell)
				{
					rowTrace.children.push_back(CreateDecisionTableConclusionCellsTraceNode(rule, target, changedCellCount, std::move(conclusionCellTraces)));
					AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));
					result.unsupported = true;

					return result;
				}
			}

			rowTrace.children.push_back(CreateDecisionTableConclusionCellsTraceNode(rule, target, changedCellCount, std::move(conclusionCellTraces)));
			AppendDecisionTableRowTrace(evaluationContext, std::move(rowTrace));

			return result;
		}

		ClearDecisionTableConclusionAttributes(target, rule, result.changedAttributes);

		return result;
	}

	bool CanEvaluateRuleShape(const RuleSet::SessionModelRuleMetadata& rule, EvaluationContext& evaluationContext)
	{
		if(IsDecisionTableRule(rule))
		{
			if(!rule.executableDecisionTable.is_object())
			{
				AddDiagnostic(evaluationContext, "decision_table_payload_missing", "Decision-table rule has no executable payload.", rule.ruleId);

				return false;
			}

			if(rule.conclusionMode == "Relationship" && !IsRelationshipDecisionTableRule(rule))
			{
				AddDiagnostic(evaluationContext,
				              "decision_table_relationship_match_policy",
				              "Relationship decision tables require the AllMatches match policy.",
				              GetString(rule.executableDecisionTable, "matchPolicy"));

				return false;
			}

			return true;
		}

		InferInstanceConclusion inferConclusion = ResolveInferInstanceConclusion(rule);

		if(inferConclusion.isInfer)
		{
			if(!rule.conclusionValueMode.empty() && rule.conclusionValueMode != "Literal")
			{
				AddDiagnostic(evaluationContext, "unsupported_conclusion_value_mode", "InferInstance conclusion value mode is not implemented yet.", rule.conclusionValueMode);

				return false;
			}

			if(!inferConclusion.shapeSupported)
			{
				AddDiagnostic(evaluationContext, "unsupported_infer_instance_shape", "InferInstance supports InferInstance(relationship), InferInstanceFor(relationship, condition), and InferInstanceFor(relationship, alias, condition) as conclusion-side constructs.", rule.conclusionValue);

				return false;
			}

			return true;
		}

		ValidationConclusion validationConclusion = ResolveValidationConclusion(rule);

		if(validationConclusion.isValidation)
		{
			if(!rule.conclusionValueMode.empty() && rule.conclusionValueMode != "Literal")
			{
				AddDiagnostic(evaluationContext, "unsupported_conclusion_value_mode", "Validation conclusion value mode is not implemented yet.", rule.conclusionValueMode);

				return false;
			}

			if(!validationConclusion.shapeSupported)
			{
				AddDiagnostic(evaluationContext, "unsupported_validation_shape", "Validation conclusions support Error(message) and Warning(message) as conclusion-side constructs.", rule.conclusionValue);

				return false;
			}

			return true;
		}

		if(rule.conclusionMode == "None")
		{
			return true;
		}

		if(rule.conclusionMode == "Relationship")
		{
			if(!rule.conclusionValueMode.empty() && rule.conclusionValueMode != "Literal")
			{
				AddDiagnostic(evaluationContext, "unsupported_conclusion_value_mode", "Relationship conclusion value mode is not implemented yet.", rule.conclusionValueMode);

				return false;
			}

			return true;
		}

		if(rule.conclusionMode != "Value")
		{
			AddDiagnostic(evaluationContext, "unsupported_conclusion_mode", "Conclusion mode is not implemented yet.", rule.conclusionMode);

			return false;
		}

		if(!rule.conclusionValueMode.empty() && rule.conclusionValueMode != "Literal" && rule.conclusionValueMode != "Attribute")
		{
			AddDiagnostic(evaluationContext, "unsupported_conclusion_value_mode", "Conclusion value mode is not implemented yet.", rule.conclusionValueMode);

			return false;
		}

		return true;
	}

	void EnqueueAttributeChange(std::deque<AttributeChange>& queue, std::unordered_set<std::string>& queuedKeys, const AttributeChange& change)
	{
		const std::string key = QueueKey(change);

		if(queuedKeys.insert(key).second)
		{
			queue.push_back(change);
		}
	}

	void EnqueueRelationshipChange(std::deque<RelationshipChange>& queue, std::unordered_set<std::string>& queuedKeys, const RelationshipChange& change)
	{
		const std::string key = QueueKey(change);

		if(queuedKeys.insert(key).second)
		{
			queue.push_back(change);
		}
	}

	void EvaluateRuleForChangedInstance(RuleSet& ruleSet,
	                                    const std::shared_ptr<Instance>& changedInstance,
	                                    const RuleSet::SessionModelRuleMetadata* rule,
	                                    const RuleTrigger& trigger,
	                                    SessionModelRuleEvaluator::EvaluationStats& stats,
	                                    std::deque<AttributeChange>& queue,
	                                    std::unordered_set<std::string>& queuedKeys,
	                                    std::deque<RelationshipChange>& relationshipQueue,
	                                    std::unordered_set<std::string>& queuedRelationshipKeys)
	{
		if(rule == nullptr || !rule->isActive)
		{
			stats.skippedRules++;

			return;
		}

		EvaluationContext evaluationContext;

		evaluationContext.ruleId = rule->ruleId;
		evaluationContext.ruleSet = &ruleSet;
		evaluationContext.diagnostics = &stats.diagnostics;

		if(!CanEvaluateRuleShape(*rule, evaluationContext))
		{
			AppendRuleTraceNode(stats,
			                    *rule,
			                    nullptr,
			                    trigger,
			                    NodeResult::Unsupported,
			                    "Rule shape is not supported by the SessionModelRuleEvaluator.",
			                    "Unknown");
			stats.unsupportedRules++;

			return;
		}

		std::vector<std::shared_ptr<Instance>> targets;

		if(IsInferInstanceConclusionRule(*rule))
		{
			targets = ResolveInferSourceTargets(ruleSet, changedInstance, *rule, trigger, evaluationContext);
		}
		else if(IsValidationConclusionRule(*rule))
		{
			if(!rule->conclusionEntityId.empty())
			{
				targets = ResolveConclusionTargets(ruleSet, changedInstance, *rule, trigger);
			}
			else
			{
				PushUniqueInstance(targets, changedInstance);
			}
		}
		else if(rule->conclusionMode == "None" || (rule->conclusionMode == "Relationship" && !IsDecisionTableRule(*rule)))
		{
			PushUniqueInstance(targets, changedInstance);
		}
		else
		{
			targets = ResolveConclusionTargets(ruleSet, changedInstance, *rule, trigger);
		}

		if(targets.empty())
		{
			AppendRuleTraceNode(stats,
			                    *rule,
			                    nullptr,
			                    trigger,
			                    NodeResult::Unknown,
			                    "No target instances were resolved for this rule.",
			                    "NotEvaluated");
			stats.skippedRules++;

			return;
		}

		for(const std::shared_ptr<Instance>& target : targets)
		{
			std::vector<SessionModelRuleEvaluator::EvaluationStats::EvaluationTraceNode> conditionTrace;

			evaluationContext.traceSink = stats.captureTrace ? &conditionTrace : nullptr;
			NodeResult result = EvaluateNode(ruleSet, target, rule->condition, evaluationContext);
			evaluationContext.traceSink = nullptr;

			if(result == NodeResult::Unsupported)
			{
				AppendRuleTraceNode(stats,
				                    *rule,
				                    target,
				                    trigger,
				                    result,
				                    "Condition shape is not supported by the SessionModelRuleEvaluator.",
				                    "Unknown",
				                    &conditionTrace);
				stats.unsupportedRules++;

				continue;
			}

			stats.evaluatedRules++;

			if(IsInferInstanceConclusionRule(*rule))
			{
				if(result != NodeResult::True)
				{
					AppendRuleTraceNode(stats,
					                    *rule,
					                    target,
					                    trigger,
					                    result,
					                    "Condition did not match; infer-instance conclusion was not applied.",
					                    "NotEvaluated",
					                    &conditionTrace);
					continue;
				}

				std::vector<EvaluationTraceNode> conclusionTrace;

				evaluationContext.traceSink = stats.captureTrace ? &conclusionTrace : nullptr;
				std::vector<InferInstanceApplyResult> inferResults = ApplyInferInstanceConclusion(ruleSet, target, *rule, evaluationContext, stats);
				evaluationContext.traceSink = nullptr;

				int appliedCount = 0;

				for(const InferInstanceApplyResult& inferResult : inferResults)
				{
					if(!inferResult.applied)
					{
						continue;
					}

					stats.appliedConclusions++;
					appliedCount++;

					if(inferResult.createdRelationshipLink)
					{
						EnqueueRelationshipChange(relationshipQueue,
						                          queuedRelationshipKeys,
						                          RelationshipChange{inferResult.sourceEntityId, inferResult.sourceInstanceId, inferResult.relationshipId, rule->ruleId});
					}
				}

				AppendRuleTraceNode(stats,
				                    *rule,
				                    target,
				                    trigger,
				                    result,
				                    appliedCount > 0
					                    ? "Applied infer-instance conclusion changes: " + std::to_string(appliedCount) + "."
					                    : "Infer-instance conclusion made no changes.",
				                    appliedCount > 0 ? "Known" : "NotEvaluated",
				                    &conditionTrace,
				                    &conclusionTrace);
				continue;
			}

			if(IsValidationConclusionRule(*rule))
			{
				ValidationConclusion validationConclusion = ResolveValidationConclusion(*rule);
				std::vector<EvaluationTraceNode> conclusionTrace;
				bool appliedValidationChange = false;
				bool clearedValidationChange = false;

				if(result == NodeResult::True)
				{
					evaluationContext.traceSink = stats.captureTrace ? &conclusionTrace : nullptr;
					if(ApplyValidationConclusion(ruleSet, target, *rule, validationConclusion, evaluationContext, stats))
					{
						stats.appliedConclusions++;
						appliedValidationChange = true;
					}
					evaluationContext.traceSink = nullptr;
				}
				else if(ClearValidationConclusion(ruleSet, target, *rule, validationConclusion, stats))
				{
					stats.appliedConclusions++;
					clearedValidationChange = true;
				}

				std::string validationDetail;
				if(appliedValidationChange)
				{
					validationDetail = "Applied validation conclusion.";
				}
				else if(clearedValidationChange)
				{
					validationDetail = "Cleared validation conclusion.";
				}
				else if(result == NodeResult::True)
				{
					validationDetail = "Validation conclusion made no changes.";
				}
				else
				{
					validationDetail = "Condition did not match; validation conclusion was not applied.";
				}

				AppendRuleTraceNode(stats,
				                    *rule,
				                    target,
				                    trigger,
				                    result,
				                    validationDetail,
				                    (appliedValidationChange || clearedValidationChange) ? "Known" : "NotEvaluated",
				                    &conditionTrace,
				                    &conclusionTrace);
				continue;
			}

			if(rule->conclusionMode == "None")
			{
				AppendRuleTraceNode(stats,
				                    *rule,
				                    target,
				                    trigger,
				                    result,
				                    "Rule has no conclusion to apply.",
				                    "NotEvaluated",
				                    &conditionTrace);
				continue;
			}

			if(IsDecisionTableRule(*rule))
			{
				if(result == NodeResult::False && !IsRelationshipDecisionTableRule(*rule))
				{
					AppendRuleTraceNode(stats,
					                    *rule,
					                    target,
					                    trigger,
					                    result,
					                    "Condition did not match; decision table was not evaluated.",
					                    "NotEvaluated",
					                    &conditionTrace);
					continue;
				}

				std::vector<EvaluationTraceNode> conclusionTrace;

				evaluationContext.traceSink = stats.captureTrace ? &conclusionTrace : nullptr;
				DecisionTableResult tableResult = IsRelationshipDecisionTableRule(*rule)
					? EvaluateRelationshipDecisionTable(target, ruleSet, *rule, result, evaluationContext, stats)
					: EvaluateDecisionTable(target, ruleSet, *rule, evaluationContext);
				evaluationContext.traceSink = nullptr;

				if(tableResult.unsupported)
				{
					AppendRuleTraceNode(stats,
					                    *rule,
					                    target,
					                    trigger,
					                    NodeResult::Unsupported,
					                    "Decision table shape is not supported by the SessionModelRuleEvaluator.",
					                    "Unknown",
					                    &conditionTrace,
					                    &conclusionTrace);
					stats.unsupportedRules++;

					continue;
				}

				const int appliedCount = IsRelationshipDecisionTableRule(*rule)
					? tableResult.appliedConclusions
					: static_cast<int>(tableResult.changedAttributes.size());
				stats.appliedConclusions += appliedCount;

				for(const AttributeChange& inferredChange : tableResult.changedAttributes)
				{
					EnqueueAttributeChange(queue, queuedKeys, inferredChange);
				}

				for(const RelationshipChange& relationshipChange : tableResult.changedRelationships)
				{
					EnqueueRelationshipChange(relationshipQueue, queuedRelationshipKeys, relationshipChange);
				}

				AppendRuleTraceNode(stats,
				                    *rule,
				                    target,
				                    trigger,
				                    result,
				                    appliedCount > 0
					                    ? "Applied decision-table conclusions: " + std::to_string(appliedCount) + "."
					                    : "Decision table made no conclusion changes.",
				                    appliedCount > 0 ? "Known" : "NotEvaluated",
				                    &conditionTrace,
				                    &conclusionTrace);
				continue;
			}

			if(rule->conclusionMode == "Relationship")
			{
				AppendRuleTraceNode(stats,
				                    *rule,
				                    target,
				                    trigger,
				                    result,
				                    "Relationship conclusion did not change runtime state.",
				                    "NotEvaluated",
				                    &conditionTrace);
				continue;
			}

			std::vector<EvaluationTraceNode> conclusionTrace;

			evaluationContext.traceSink = stats.captureTrace ? &conclusionTrace : nullptr;
			const bool appliedValueConclusion = ApplyValueConclusion(target, ruleSet, *rule, result, evaluationContext);
			evaluationContext.traceSink = nullptr;

			if(appliedValueConclusion)
			{
				stats.appliedConclusions++;

				AttributeChange inferredChange{rule->conclusionEntityId, target->instanceId, rule->conclusionAttributeId};

				EnqueueAttributeChange(queue, queuedKeys, inferredChange);
			}

			AppendRuleTraceNode(stats,
			                    *rule,
			                    target,
			                    trigger,
			                    result,
			                    appliedValueConclusion
				                    ? "Applied value conclusion for attribute " + rule->conclusionAttributeId + "."
				                    : "Value conclusion made no changes.",
			                    appliedValueConclusion ? "Known" : "NotEvaluated",
			                    &conditionTrace,
			                    &conclusionTrace);
		}
	}

	bool TryBuildTemporalHistoryChangeRanges(const std::vector<std::pair<std::string, std::string>>& effectiveIntervals,
	                                         std::vector<TemporalHistoryChangeRange>& ranges)
	{
		for(const std::pair<std::string, std::string>& effectiveInterval : effectiveIntervals)
		{
			TemporalInstant start;

			if(effectiveInterval.first.empty() || !TryParseTemporalInstant(effectiveInterval.first, start))
			{
				return false;
			}

			TemporalHistoryChangeRange range;

			range.startSeconds = start.seconds;
			range.endOpen = effectiveInterval.second.empty();

			if(!range.endOpen)
			{
				TemporalInstant end;

				if(!TryParseTemporalInstant(effectiveInterval.second, end) || end.seconds <= start.seconds)
				{
					return false;
				}

				range.endSeconds = end.seconds;
			}

			ranges.push_back(range);
		}

		return !ranges.empty();
	}

	bool StringArgumentResolvesChangedTemporalAttribute(const std::shared_ptr<Instance>& changedInstance,
	                                                   const std::string& argument,
	                                                   const std::string& attributeId)
	{
		ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

		if(nestedCall.parsed)
		{
			return false;
		}

		std::shared_ptr<Attribute> attribute = FindTemporalAttributeOnInstance(changedInstance, RemoveMatchingQuotes(argument));

		if(attribute != nullptr)
		{
			return attribute->GetID() == attributeId;
		}

		return RemoveMatchingQuotes(argument) == attributeId;
	}

	bool StringArgumentResolvesChangedBooleanTemporalAttribute(const std::shared_ptr<Instance>& changedInstance,
	                                                          const std::string& argument,
	                                                          const std::string& attributeId)
	{
		ParsedFunctionCall nestedCall = ParseFunctionCall(argument);

		if(nestedCall.parsed)
		{
			return false;
		}

		std::shared_ptr<Attribute> attribute = FindTemporalAttributeOnInstance(changedInstance, RemoveMatchingQuotes(argument));

		if(attribute != nullptr)
		{
			return attribute->GetID() == attributeId && attribute->GetType() == Attribute::BOOLEAN;
		}

		return RemoveMatchingQuotes(argument) == attributeId;
	}

	bool NodeArgumentResolvesChangedTemporalAttribute(const std::shared_ptr<Instance>& changedInstance,
	                                                 const json& argument,
	                                                 const std::string& attributeId)
	{
		TemporalAttributeReference reference;

		return ResolveTemporalAttributeArgumentNode(changedInstance, argument, reference) && reference.attributeId == attributeId;
	}

	bool NodeArgumentResolvesChangedBooleanTemporalAttribute(const std::shared_ptr<Instance>& changedInstance,
	                                                        const json& argument,
	                                                        const std::string& attributeId)
	{
		TemporalAttributeReference reference;

		return ResolveTemporalAttributeArgumentNode(changedInstance, argument, reference)
			&& reference.attributeId == attributeId
			&& reference.valueType == Attribute::BOOLEAN;
	}

	bool TryParseTemporalArgumentLiteral(const std::string& argument, TemporalInstant& instant)
	{
		return TryParseTemporalInstant(argument, instant) || TryParseTemporalInstant(RemoveMatchingQuotes(argument), instant);
	}

	bool TryParseTemporalArgumentNodeLiteral(const json& argument, TemporalInstant& instant)
	{
		const std::string value = FunctionArgumentValue(argument);

		return TryParseTemporalInstant(value, instant) || TryParseTemporalInstant(RemoveMatchingQuotes(value), instant);
	}

	bool TemporalStringIntervalCallMayOverlapChangedRanges(const std::shared_ptr<Instance>& changedInstance,
	                                                       const ParsedFunctionCall& call,
	                                                       const std::string& attributeId,
	                                                       const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                       bool& sawMatchingTemporalReference)
	{
		if(!IsTemporalIntervalFunctionName(call.name))
		{
			return false;
		}

		const bool hasDays = IsTemporalDayCountFunctionName(call.name);

		if(call.arguments.size() != (hasDays ? 4 : 3))
		{
			return false;
		}

		const size_t exportedConditionIndex = hasDays ? 3 : 2;

		if(StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[exportedConditionIndex], attributeId))
		{
			sawMatchingTemporalReference = true;

			TemporalInstant start;
			TemporalInstant end;

			return !TryParseTemporalArgumentLiteral(call.arguments[0], start)
				|| !TryParseTemporalArgumentLiteral(call.arguments[1], end)
				|| AnyHistoryRangeOverlapsInterval(ranges, start, end);
		}

		if(StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[0], attributeId))
		{
			sawMatchingTemporalReference = true;

			TemporalInstant start;
			TemporalInstant end;

			return !TryParseTemporalArgumentLiteral(call.arguments[1], start)
				|| !TryParseTemporalArgumentLiteral(call.arguments[2], end)
				|| AnyHistoryRangeOverlapsInterval(ranges, start, end);
		}

		return false;
	}

	bool TemporalNodeIntervalCallMayOverlapChangedRanges(const std::shared_ptr<Instance>& changedInstance,
	                                                     const std::string& functionName,
	                                                     const json& arguments,
	                                                     const std::string& attributeId,
	                                                     const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                     bool& sawMatchingTemporalReference)
	{
		if(!IsTemporalIntervalFunctionName(functionName))
		{
			return false;
		}

		const bool hasDays = IsTemporalDayCountFunctionName(functionName);

		if(arguments.size() != (hasDays ? 4 : 3))
		{
			return false;
		}

		const size_t exportedConditionIndex = hasDays ? 3 : 2;

		if(NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(exportedConditionIndex), attributeId))
		{
			sawMatchingTemporalReference = true;

			TemporalInstant start;
			TemporalInstant end;

			return !TryParseTemporalArgumentNodeLiteral(arguments.at(0), start)
				|| !TryParseTemporalArgumentNodeLiteral(arguments.at(1), end)
				|| AnyHistoryRangeOverlapsInterval(ranges, start, end);
		}

		if(NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(0), attributeId))
		{
			sawMatchingTemporalReference = true;

			TemporalInstant start;
			TemporalInstant end;

			return !TryParseTemporalArgumentNodeLiteral(arguments.at(1), start)
				|| !TryParseTemporalArgumentNodeLiteral(arguments.at(2), end)
				|| AnyHistoryRangeOverlapsInterval(ranges, start, end);
		}

		return false;
	}

	bool TemporalStringExtremaCallMayOverlapChangedRanges(const std::shared_ptr<Instance>& changedInstance,
	                                                      const ParsedFunctionCall& call,
	                                                      const std::string& attributeId,
	                                                      const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                      bool& sawMatchingTemporalReference)
	{
		if(!IsTemporalExtremaFunctionName(call.name)
			&& !IsTemporalConditionalAggregateFunctionName(call.name)
			&& !IsTemporalConditionalWeightedAggregateFunctionName(call.name)
			&& !IsTemporalWeightedAggregateFunctionName(call.name)
			&& !IsTemporalCountDistinctAggregateFunctionName(call.name)
			&& !IsTemporalConditionalCountDistinctAggregateFunctionName(call.name))
		{
			return false;
		}

		const bool conditionalAggregate = IsTemporalConditionalAggregateFunctionName(call.name)
			|| IsTemporalConditionalWeightedAggregateFunctionName(call.name)
			|| IsTemporalConditionalCountDistinctAggregateFunctionName(call.name);
		const size_t minimumArgumentCount = conditionalAggregate ? 4 : 3;

		if(call.arguments.size() < minimumArgumentCount)
		{
			return false;
		}

		const bool valueMatches = StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[2], attributeId);
		const bool conditionMatches = conditionalAggregate && StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[3], attributeId);

		if(!valueMatches && !conditionMatches)
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		TemporalInstant start;
		TemporalInstant end;

		return !TryParseTemporalArgumentLiteral(call.arguments[0], start)
			|| !TryParseTemporalArgumentLiteral(call.arguments[1], end)
			|| AnyHistoryRangeOverlapsInterval(ranges, start, end);
	}

	bool TemporalNodeExtremaCallMayOverlapChangedRanges(const std::shared_ptr<Instance>& changedInstance,
	                                                    const std::string& functionName,
	                                                    const json& arguments,
	                                                    const std::string& attributeId,
	                                                    const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                    bool& sawMatchingTemporalReference)
	{
		if(!IsTemporalExtremaFunctionName(functionName)
			&& !IsTemporalConditionalAggregateFunctionName(functionName)
			&& !IsTemporalConditionalWeightedAggregateFunctionName(functionName)
			&& !IsTemporalWeightedAggregateFunctionName(functionName)
			&& !IsTemporalCountDistinctAggregateFunctionName(functionName)
			&& !IsTemporalConditionalCountDistinctAggregateFunctionName(functionName))
		{
			return false;
		}

		const bool conditionalAggregate = IsTemporalConditionalAggregateFunctionName(functionName)
			|| IsTemporalConditionalWeightedAggregateFunctionName(functionName)
			|| IsTemporalConditionalCountDistinctAggregateFunctionName(functionName);
		const size_t minimumArgumentCount = conditionalAggregate ? 4 : 3;

		if(arguments.size() < minimumArgumentCount)
		{
			return false;
		}

		const bool valueMatches = NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(2), attributeId);
		const bool conditionMatches = conditionalAggregate && NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(3), attributeId);

		if(!valueMatches && !conditionMatches)
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		TemporalInstant start;
		TemporalInstant end;

		return !TryParseTemporalArgumentNodeLiteral(arguments.at(0), start)
			|| !TryParseTemporalArgumentNodeLiteral(arguments.at(1), end)
			|| AnyHistoryRangeOverlapsInterval(ranges, start, end);
	}

	bool RelativeWindowMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                           const std::vector<TemporalHistoryChangeRange>& ranges,
	                                           int days,
	                                           const TemporalInstant* explicitAnchor = nullptr)
	{
		if(days < 1)
		{
			return true;
		}

		TemporalInstant anchor;

		if(explicitAnchor != nullptr)
		{
			anchor = *explicitAnchor;
		}
		else if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), anchor))
		{
			return true;
		}

		const long long anchorDayStart = LocalDayStartSeconds(anchor.seconds);
		TemporalInstant start;
		TemporalInstant end;

		start.parsed = true;
		start.seconds = anchorDayStart - static_cast<long long>(days - 1) * 86400LL;
		start.hasTime = false;
		end.parsed = true;
		end.seconds = anchorDayStart + 86400LL;
		end.hasTime = false;

		return AnyHistoryRangeOverlapsInterval(ranges, start, end);
	}

	bool TemporalOncePerMonthHistoryRangeMayOverlapChangedRanges(const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                             const TemporalInstant& anchorDate,
	                                                             const TemporalInstant& startDate,
	                                                             const TemporalInstant& endDate)
	{
		if(anchorDate.hasTime || startDate.hasTime || endDate.hasTime)
		{
			return true;
		}

		const std::vector<long long> recurrenceDays = MonthlyRecurrenceDayStarts(anchorDate, startDate, endDate);

		for(long long recurrenceDayStart : recurrenceDays)
		{
			TemporalInstant recurrenceStart;
			TemporalInstant recurrenceEnd;

			recurrenceStart.parsed = true;
			recurrenceStart.seconds = recurrenceDayStart;
			recurrenceStart.hasTime = false;
			recurrenceEnd.parsed = true;
			recurrenceEnd.seconds = recurrenceDayStart + 86400LL;
			recurrenceEnd.hasTime = false;

			if(AnyHistoryRangeOverlapsInterval(ranges, recurrenceStart, recurrenceEnd))
			{
				return true;
			}
		}

		return false;
	}

	bool TemporalOncePerMonthRangeConditionMayOverlapChangedRanges(const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                               const TemporalInstant& startDate,
	                                                               const TemporalInstant& endDate)
	{
		if(startDate.hasTime || endDate.hasTime)
		{
			return true;
		}

		if(startDate.seconds > endDate.seconds)
		{
			return false;
		}

		TemporalInstant intervalStart = startDate;
		TemporalInstant intervalEnd = endDate;

		intervalEnd.seconds += 86400LL;
		intervalEnd.hasTime = false;

		return AnyHistoryRangeOverlapsInterval(ranges, intervalStart, intervalEnd);
	}

	bool TemporalOncePerMonthAnchorConditionMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                                const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                                const TemporalInstant& anchorDate)
	{
		if(anchorDate.hasTime)
		{
			return true;
		}

		TemporalInstant currentDate;

		if(!TryParseTemporalInstant(ruleSet.GetSessionCurrentDate(), currentDate) || currentDate.hasTime)
		{
			return true;
		}

		const DateTimeParts currentParts = PartsFromSeconds(currentDate.seconds);
		const DateTimeParts anchorParts = PartsFromSeconds(anchorDate.seconds);
		const int daysInCurrentMonth = DaysInMonth(currentParts.year, currentParts.month);
		const int recurrenceDay = anchorParts.day < daysInCurrentMonth ? anchorParts.day : daysInCurrentMonth;
		DateTimeParts recurrenceParts;

		recurrenceParts.year = currentParts.year;
		recurrenceParts.month = currentParts.month;
		recurrenceParts.day = recurrenceDay;
		recurrenceParts.hasDate = true;
		recurrenceParts.hasTime = false;

		TemporalInstant recurrenceStart;
		TemporalInstant recurrenceEnd;

		recurrenceStart.parsed = true;
		recurrenceStart.seconds = SecondsFromParts(recurrenceParts);
		recurrenceStart.hasTime = false;
		recurrenceEnd.parsed = true;
		recurrenceEnd.seconds = recurrenceStart.seconds + 86400LL;
		recurrenceEnd.hasTime = false;

		return AnyHistoryRangeOverlapsInterval(ranges, recurrenceStart, recurrenceEnd);
	}

	bool TemporalStringOncePerMonthHistoryCallMayOverlapChangedRanges(const std::shared_ptr<Instance>& changedInstance,
	                                                                  const RuleSet& ruleSet,
	                                                                  const ParsedFunctionCall& call,
	                                                                  const std::string& attributeId,
	                                                                  const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                                  bool& sawMatchingTemporalReference)
	{
		if(call.name != "TemporalOncePerMonth" || (call.arguments.size() != 2 && call.arguments.size() != 3 && call.arguments.size() != 4))
		{
			return false;
		}

		const size_t conditionIndex = call.arguments.size() == 2 ? 1 : call.arguments.size() - 1;

		if(!StringArgumentResolvesChangedBooleanTemporalAttribute(changedInstance, call.arguments[conditionIndex], attributeId))
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		if(call.arguments.size() == 2)
		{
			TemporalInstant anchorDate;

			if(!TryParseTemporalArgumentLiteral(call.arguments[0], anchorDate))
			{
				return true;
			}

			return TemporalOncePerMonthAnchorConditionMayOverlapChangedRanges(ruleSet, ranges, anchorDate);
		}

		if(call.arguments.size() == 3)
		{
			TemporalInstant startDate;
			TemporalInstant endDate;

			if(!TryParseTemporalArgumentLiteral(call.arguments[0], startDate)
				|| !TryParseTemporalArgumentLiteral(call.arguments[1], endDate))
			{
				return true;
			}

			return TemporalOncePerMonthRangeConditionMayOverlapChangedRanges(ranges, startDate, endDate);
		}

		TemporalInstant anchorDate;
		TemporalInstant startDate;
		TemporalInstant endDate;

		if(!TryParseTemporalArgumentLiteral(call.arguments[0], anchorDate)
			|| !TryParseTemporalArgumentLiteral(call.arguments[1], startDate)
			|| !TryParseTemporalArgumentLiteral(call.arguments[2], endDate))
		{
			return true;
		}

		return TemporalOncePerMonthHistoryRangeMayOverlapChangedRanges(ranges, anchorDate, startDate, endDate);
	}

	bool TemporalNodeOncePerMonthHistoryCallMayOverlapChangedRanges(const std::shared_ptr<Instance>& changedInstance,
	                                                                const RuleSet& ruleSet,
	                                                                const std::string& functionName,
	                                                                const json& arguments,
	                                                                const std::string& attributeId,
	                                                                const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                                bool& sawMatchingTemporalReference)
	{
		if(functionName != "TemporalOncePerMonth"
			|| !arguments.is_array()
			|| (arguments.size() != 2 && arguments.size() != 3 && arguments.size() != 4))
		{
			return false;
		}

		const size_t conditionIndex = arguments.size() == 2 ? 1 : arguments.size() - 1;

		if(!NodeArgumentResolvesChangedBooleanTemporalAttribute(changedInstance, arguments.at(conditionIndex), attributeId))
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		if(arguments.size() == 2)
		{
			TemporalInstant anchorDate;

			if(!TryParseTemporalArgumentNodeLiteral(arguments.at(0), anchorDate))
			{
				return true;
			}

			return TemporalOncePerMonthAnchorConditionMayOverlapChangedRanges(ruleSet, ranges, anchorDate);
		}

		if(arguments.size() == 3)
		{
			TemporalInstant startDate;
			TemporalInstant endDate;

			if(!TryParseTemporalArgumentNodeLiteral(arguments.at(0), startDate)
				|| !TryParseTemporalArgumentNodeLiteral(arguments.at(1), endDate))
			{
				return true;
			}

			return TemporalOncePerMonthRangeConditionMayOverlapChangedRanges(ranges, startDate, endDate);
		}

		TemporalInstant anchorDate;
		TemporalInstant startDate;
		TemporalInstant endDate;

		if(!TryParseTemporalArgumentNodeLiteral(arguments.at(0), anchorDate)
			|| !TryParseTemporalArgumentNodeLiteral(arguments.at(1), startDate)
			|| !TryParseTemporalArgumentNodeLiteral(arguments.at(2), endDate))
		{
			return true;
		}

		return TemporalOncePerMonthHistoryRangeMayOverlapChangedRanges(ranges, anchorDate, startDate, endDate);
	}

	bool TemporalStringRelativeWindowCallMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                             const std::shared_ptr<Instance>& changedInstance,
	                                                             const ParsedFunctionCall& call,
	                                                             const std::string& attributeId,
	                                                             const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                             bool& sawMatchingTemporalReference)
	{
		if(!IsTemporalRelativeWindowFunctionName(call.name) || (call.arguments.size() != 2 && call.arguments.size() != 3))
		{
			return false;
		}

		const size_t conditionIndex = call.arguments.size() == 3 ? 2 : 1;

		if(!StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[conditionIndex], attributeId))
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		int days = 0;
		const size_t daysIndex = call.arguments.size() == 3 ? 1 : 0;

		if(!TryParseIntegerValue(RemoveMatchingQuotes(call.arguments[daysIndex]), days))
		{
			return true;
		}

		if(call.name == "TemporalConsecutiveDays" && call.arguments.size() == 3)
		{
			const std::string requiredArgument = RemoveMatchingQuotes(call.arguments[0]);
			const std::string searchArgument = RemoveMatchingQuotes(call.arguments[1]);
			double requiredNumber = 0.0;
			double searchNumber = 0.0;

			if(TryParseDoubleValue(requiredArgument, requiredNumber) && TryParseDoubleValue(searchArgument, searchNumber))
			{
				int requiredDays = 0;
				int searchDays = 0;

				if(!TryParseIntegerValue(requiredArgument, requiredDays)
					|| !TryParseIntegerValue(searchArgument, searchDays)
					|| requiredDays < 1
					|| searchDays < 1)
				{
					return false;
				}

				return RelativeWindowMayOverlapChangedRanges(ruleSet, ranges, searchDays);
			}
		}

		TemporalInstant anchor;

		if(call.arguments.size() == 3)
		{
			if(!TryParseTemporalArgumentLiteral(call.arguments[0], anchor))
			{
				return true;
			}

			return RelativeWindowMayOverlapChangedRanges(ruleSet, ranges, days, &anchor);
		}

		return RelativeWindowMayOverlapChangedRanges(ruleSet, ranges, days);
	}

	bool TemporalNodeRelativeWindowCallMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                           const std::shared_ptr<Instance>& changedInstance,
	                                                           const std::string& functionName,
	                                                           const json& arguments,
	                                                           const std::string& attributeId,
	                                                           const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                           bool& sawMatchingTemporalReference)
	{
		if(!IsTemporalRelativeWindowFunctionName(functionName) || (arguments.size() != 2 && arguments.size() != 3))
		{
			return false;
		}

		const size_t conditionIndex = arguments.size() == 3 ? 2 : 1;

		if(!NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(conditionIndex), attributeId))
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		int days = 0;
		const size_t daysIndex = arguments.size() == 3 ? 1 : 0;

		if(!TryParseIntegerValue(RemoveMatchingQuotes(FunctionArgumentValue(arguments.at(daysIndex))), days))
		{
			return true;
		}

		if(functionName == "TemporalConsecutiveDays" && arguments.size() == 3)
		{
			const std::string requiredArgument = RemoveMatchingQuotes(FunctionArgumentValue(arguments.at(0)));
			const std::string searchArgument = RemoveMatchingQuotes(FunctionArgumentValue(arguments.at(1)));
			double requiredNumber = 0.0;
			double searchNumber = 0.0;

			if(TryParseDoubleValue(requiredArgument, requiredNumber) && TryParseDoubleValue(searchArgument, searchNumber))
			{
				int requiredDays = 0;
				int searchDays = 0;

				if(!TryParseIntegerValue(requiredArgument, requiredDays)
					|| !TryParseIntegerValue(searchArgument, searchDays)
					|| requiredDays < 1
					|| searchDays < 1)
				{
					return false;
				}

				return RelativeWindowMayOverlapChangedRanges(ruleSet, ranges, searchDays);
			}
		}

		TemporalInstant anchor;

		if(arguments.size() == 3)
		{
			if(!TryParseTemporalArgumentNodeLiteral(arguments.at(0), anchor))
			{
				return true;
			}

			return RelativeWindowMayOverlapChangedRanges(ruleSet, ranges, days, &anchor);
		}

		return RelativeWindowMayOverlapChangedRanges(ruleSet, ranges, days);
	}

	std::string ResolveTemporalMembershipRelationshipId(const RuleSet& ruleSet, const std::string& relationshipArgument)
	{
		std::shared_ptr<Relationship> relationship = FindRelationshipByScope(ruleSet, RemoveMatchingQuotes(relationshipArgument));

		return relationship != nullptr ? relationship->id : "";
	}

	bool TemporalMembershipTimeArgumentMayOverlapChangedRanges(const std::string& timeArgument,
	                                                          const std::vector<TemporalHistoryChangeRange>& ranges)
	{
		TemporalInstant instant;

		if(!TryParseTemporalArgumentLiteral(timeArgument, instant))
		{
			return true;
		}

		return TemporalEffectiveWindowOverlapsRanges(ranges, BuildTemporalEffectiveWindow(instant));
	}

	bool TemporalMembershipTimeArgumentMayOverlapChangedRanges(const json& timeArgument,
	                                                          const std::vector<TemporalHistoryChangeRange>& ranges)
	{
		TemporalInstant instant;

		if(!TryParseTemporalArgumentNodeLiteral(timeArgument, instant))
		{
			return true;
		}

		return TemporalEffectiveWindowOverlapsRanges(ranges, BuildTemporalEffectiveWindow(instant));
	}

	bool TemporalStringRelationshipMembershipCallMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                                    const ParsedFunctionCall& call,
	                                                                    const std::string& relationshipId,
	                                                                    const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                                    bool& sawMatchingTemporalReference)
	{
		const bool isMemberOfAt = call.name == "IsMemberOfAt";
		const bool isTemporalScopeAt = call.name == "ForScopeAt" || IsTemporalUniversalQuantifierName(call.name);

		if((!isMemberOfAt && !isTemporalScopeAt) || (call.arguments.size() != 3 && call.arguments.size() != 4))
		{
			return false;
		}

		const size_t relationshipIndex = isTemporalScopeAt ? 0 : (call.arguments.size() == 4 ? 2 : 1);
		const size_t timeIndex = isTemporalScopeAt ? (call.arguments.size() == 4 ? 2 : 1) : (call.arguments.size() == 4 ? 3 : 2);
		const std::string callRelationshipId = ResolveTemporalMembershipRelationshipId(ruleSet, call.arguments[relationshipIndex]);

		if(!callRelationshipId.empty() && callRelationshipId != relationshipId)
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		return TemporalMembershipTimeArgumentMayOverlapChangedRanges(call.arguments[timeIndex], ranges);
	}

	bool TemporalNodeRelationshipMembershipCallMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                                  const std::string& functionName,
	                                                                  const json& arguments,
	                                                                  const std::string& relationshipId,
	                                                                  const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                                  bool& sawMatchingTemporalReference)
	{
		const bool isMemberOfAt = functionName == "IsMemberOfAt";
		const bool isTemporalScopeAt = functionName == "ForScopeAt" || IsTemporalUniversalQuantifierName(functionName);

		if((!isMemberOfAt && !isTemporalScopeAt) || !arguments.is_array() || (arguments.size() != 3 && arguments.size() != 4))
		{
			return false;
		}

		const size_t relationshipIndex = isTemporalScopeAt ? 0 : (arguments.size() == 4 ? 2 : 1);
		const size_t timeIndex = isTemporalScopeAt ? (arguments.size() == 4 ? 2 : 1) : (arguments.size() == 4 ? 3 : 2);
		const std::string callRelationshipId = ResolveTemporalMembershipRelationshipId(ruleSet, RelationshipScopeValue(arguments.at(relationshipIndex)));

		if(!callRelationshipId.empty() && callRelationshipId != relationshipId)
		{
			return false;
		}

		sawMatchingTemporalReference = true;

		return TemporalMembershipTimeArgumentMayOverlapChangedRanges(arguments.at(timeIndex), ranges);
	}

	bool TemporalRelationshipExpressionMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                          const std::string& expression,
	                                                          const std::string& relationshipId,
	                                                          const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                          bool& sawMatchingTemporalReference)
	{
		ParsedFunctionCall call = ParseFunctionCall(expression);

		if(!call.parsed || !call.valid)
		{
			return false;
		}

		if(TemporalStringRelationshipMembershipCallMayOverlapChangedRanges(ruleSet, call, relationshipId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		for(const std::string& argument : call.arguments)
		{
			if(TemporalRelationshipExpressionMayOverlapChangedRanges(ruleSet, argument, relationshipId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
		}

		return false;
	}

	bool TemporalRelationshipNodeMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                                    const json& node,
	                                                    const std::string& relationshipId,
	                                                    const std::vector<TemporalHistoryChangeRange>& ranges,
	                                                    bool& sawMatchingTemporalReference)
	{
		if(node.is_array())
		{
			for(const json& item : node)
			{
				if(TemporalRelationshipNodeMayOverlapChangedRanges(ruleSet, item, relationshipId, ranges, sawMatchingTemporalReference))
				{
					return true;
				}
			}

			return false;
		}

		if(!node.is_object())
		{
			return false;
		}

		const std::string functionName = GetString(node, "functionName");

		if(!functionName.empty() && node.contains("functionArguments") && node.at("functionArguments").is_array())
		{
			if(TemporalNodeRelationshipMembershipCallMayOverlapChangedRanges(ruleSet,
			                                                                 functionName,
			                                                                 node.at("functionArguments"),
			                                                                 relationshipId,
			                                                                 ranges,
			                                                                 sawMatchingTemporalReference))
			{
				return true;
			}
		}

		for(auto it = node.begin(); it != node.end(); ++it)
		{
			if(TemporalRelationshipNodeMayOverlapChangedRanges(ruleSet, it.value(), relationshipId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
		}

		return false;
	}

	bool TemporalExpressionMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                              const std::shared_ptr<Instance>& changedInstance,
	                                              const std::string& expression,
	                                              const std::string& attributeId,
	                                              const std::vector<TemporalHistoryChangeRange>& ranges,
	                                              bool& sawMatchingTemporalReference)
	{
		ParsedFunctionCall call = ParseFunctionCall(expression);

		if(!call.parsed || !call.valid)
		{
			return false;
		}

		if(call.name == "ValueAt" && call.arguments.size() == 2)
		{
			const bool firstIsAttribute = StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[0], attributeId);
			const bool secondIsAttribute = StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[1], attributeId);

			if(firstIsAttribute || secondIsAttribute)
			{
				sawMatchingTemporalReference = true;

				TemporalInstant point;
				const std::string& timeArgument = firstIsAttribute ? call.arguments[1] : call.arguments[0];

				return !TryParseTemporalArgumentLiteral(timeArgument, point) || AnyHistoryRangeOverlapsPoint(ranges, point);
			}
		}
		else if((call.name == "Earliest" || call.name == "Latest" || call.name == "WhenLast" || call.name == "WhenNext")
			&& !call.arguments.empty()
			&& StringArgumentResolvesChangedTemporalAttribute(changedInstance, call.arguments[0], attributeId))
		{
			sawMatchingTemporalReference = true;

			return true;
		}
		else if(TemporalStringIntervalCallMayOverlapChangedRanges(changedInstance, call, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}
		else if(TemporalStringExtremaCallMayOverlapChangedRanges(changedInstance, call, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}
		else if(TemporalStringRelativeWindowCallMayOverlapChangedRanges(ruleSet, changedInstance, call, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}
		else if(TemporalStringOncePerMonthHistoryCallMayOverlapChangedRanges(changedInstance, ruleSet, call, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		for(const std::string& argument : call.arguments)
		{
			if(TemporalExpressionMayOverlapChangedRanges(ruleSet, changedInstance, argument, attributeId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
		}

		return false;
	}

	bool TemporalNodeMayOverlapChangedRanges(const RuleSet& ruleSet,
	                                        const std::shared_ptr<Instance>& changedInstance,
	                                        const json& node,
	                                        const std::string& attributeId,
	                                        const std::vector<TemporalHistoryChangeRange>& ranges,
	                                        bool& sawMatchingTemporalReference)
	{
		if(node.is_array())
		{
			for(const json& item : node)
			{
				if(TemporalNodeMayOverlapChangedRanges(ruleSet, changedInstance, item, attributeId, ranges, sawMatchingTemporalReference))
				{
					return true;
				}
			}

			return false;
		}

		if(!node.is_object())
		{
			return false;
		}

		const std::string functionName = GetString(node, "functionName");

		if(!functionName.empty() && node.contains("functionArguments") && node.at("functionArguments").is_array())
		{
			const json& arguments = node.at("functionArguments");

			if(functionName == "ValueAt" && arguments.size() == 2)
			{
				const bool firstIsAttribute = NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(0), attributeId);
				const bool secondIsAttribute = NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(1), attributeId);

				if(firstIsAttribute || secondIsAttribute)
				{
					sawMatchingTemporalReference = true;

					TemporalInstant point;
					const json& timeArgument = firstIsAttribute ? arguments.at(1) : arguments.at(0);

					return !TryParseTemporalArgumentNodeLiteral(timeArgument, point) || AnyHistoryRangeOverlapsPoint(ranges, point);
				}
			}
			else if((functionName == "Earliest" || functionName == "Latest" || functionName == "WhenLast" || functionName == "WhenNext")
				&& !arguments.empty()
				&& NodeArgumentResolvesChangedTemporalAttribute(changedInstance, arguments.at(0), attributeId))
			{
				sawMatchingTemporalReference = true;

				return true;
			}
			else if(TemporalNodeIntervalCallMayOverlapChangedRanges(changedInstance, functionName, arguments, attributeId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
			else if(TemporalNodeExtremaCallMayOverlapChangedRanges(changedInstance, functionName, arguments, attributeId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
			else if(TemporalNodeRelativeWindowCallMayOverlapChangedRanges(ruleSet, changedInstance, functionName, arguments, attributeId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
			else if(TemporalNodeOncePerMonthHistoryCallMayOverlapChangedRanges(changedInstance, ruleSet, functionName, arguments, attributeId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
		}

		for(auto it = node.begin(); it != node.end(); ++it)
		{
			if(TemporalNodeMayOverlapChangedRanges(ruleSet, changedInstance, it.value(), attributeId, ranges, sawMatchingTemporalReference))
			{
				return true;
			}
		}

		return false;
	}

	bool RuleHasTemporalReferenceToAttribute(const RuleSet::SessionModelRuleMetadata& rule,
	                                        const std::string& entityId,
	                                        const std::string& attributeId)
	{
		for(const RuleSet::SessionModelRuleReference& reference : rule.references)
		{
			if(reference.entityId == entityId
				&& reference.attributeId == attributeId
				&& reference.source.find(".temporal") != std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	bool RuleHasScalarTemporalHistoryReference(const RuleSet::SessionModelRuleMetadata& rule)
	{
		for(const RuleSet::SessionModelRuleReference& reference : rule.references)
		{
			if(!reference.attributeId.empty()
				&& reference.source.find(".temporal") != std::string::npos
				&& reference.source.find(".temporalRelationship") == std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	bool RuleHasTemporalRelationshipHistoryReference(const RuleSet::SessionModelRuleMetadata& rule)
	{
		for(const RuleSet::SessionModelRuleReference& reference : rule.references)
		{
			if(!reference.relationshipId.empty()
				&& reference.source.find(".temporalRelationship") != std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	bool RuleHasTransactionSnapshotSensitiveTemporalReference(const RuleSet::SessionModelRuleMetadata& rule)
	{
		return RuleHasScalarTemporalHistoryReference(rule) || RuleHasTemporalRelationshipHistoryReference(rule);
	}

	bool RuleHasTemporalReferenceToRelationship(const RuleSet::SessionModelRuleMetadata& rule,
	                                           const std::string& relationshipId)
	{
		for(const RuleSet::SessionModelRuleReference& reference : rule.references)
		{
			if(reference.relationshipId == relationshipId
				&& reference.source.find(".temporalRelationship") != std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	bool RuleTemporalRelationshipDependencyMayOverlapChangedRanges(const RuleSet::SessionModelRuleMetadata& rule,
	                                                              const RuleSet& ruleSet,
	                                                              const std::string& relationshipId,
	                                                              const std::vector<TemporalHistoryChangeRange>& ranges)
	{
		if(!RuleHasTemporalReferenceToRelationship(rule, relationshipId))
		{
			return false;
		}

		bool sawMatchingTemporalReference = false;

		if((rule.conclusionValueMode.empty() || rule.conclusionValueMode == "Literal")
			&& TemporalRelationshipExpressionMayOverlapChangedRanges(ruleSet, rule.conclusionValue, relationshipId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		if(!rule.condition.is_null()
			&& TemporalRelationshipNodeMayOverlapChangedRanges(ruleSet, rule.condition, relationshipId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		if(rule.executableDecisionTable.is_object()
			&& TemporalRelationshipNodeMayOverlapChangedRanges(ruleSet, rule.executableDecisionTable, relationshipId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		return !sawMatchingTemporalReference;
	}

	bool RelationshipLinkHistorySegmentMayOverlapRanges(const RuleSet::RelationshipLinkHistorySegment& segment,
	                                                   const std::vector<TemporalHistoryChangeRange>& ranges)
	{
		if(ranges.empty())
		{
			return true;
		}

		TemporalInstant segmentStart;

		if(!TryParseTemporalInstant(segment.effectiveStart, segmentStart))
		{
			return true;
		}

		bool segmentEndOpen = segment.effectiveEnd.empty();
		TemporalInstant segmentEnd;

		if(!segmentEndOpen && (!TryParseTemporalInstant(segment.effectiveEnd, segmentEnd) || segmentEnd.seconds <= segmentStart.seconds))
		{
			return true;
		}

		for(const TemporalHistoryChangeRange& range : ranges)
		{
			const bool rangeEndsAfterSegmentStart = range.endOpen || segmentStart.seconds < range.endSeconds;
			const bool segmentEndsAfterRangeStart = segmentEndOpen || range.startSeconds < segmentEnd.seconds;

			if(rangeEndsAfterSegmentStart && segmentEndsAfterRangeStart)
			{
				return true;
			}
		}

		return false;
	}

	std::vector<std::shared_ptr<Relationship>> FindRelationshipsForEntityEndpoint(const RuleSet& ruleSet,
	                                                                              const std::string& entityId,
	                                                                              bool sourceEndpoint)
	{
		std::vector<std::shared_ptr<Relationship>> relationships;

		if(ruleSet.entities == nullptr)
		{
			return relationships;
		}

		for(const std::shared_ptr<Entity>& entity : *ruleSet.entities)
		{
			if(entity == nullptr)
			{
				continue;
			}

			for(const std::shared_ptr<Relationship>& relationship : entity->relationships)
			{
				if(relationship == nullptr)
				{
					continue;
				}

				const std::string& endpoint = sourceEndpoint ? relationship->sourceEntity : relationship->targetEntity;

				if(endpoint == entityId)
				{
					relationships.push_back(relationship);
				}
			}
		}

		return relationships;
	}

	bool RuleTemporalDependencyMayOverlapChangedRanges(const RuleSet::SessionModelRuleMetadata& rule,
	                                                   const RuleSet& ruleSet,
	                                                   const std::shared_ptr<Instance>& changedInstance,
	                                                   const std::string& entityId,
	                                                   const std::string& attributeId,
	                                                   const std::vector<TemporalHistoryChangeRange>& ranges)
	{
		if(!RuleHasTemporalReferenceToAttribute(rule, entityId, attributeId))
		{
			return false;
		}

		bool sawMatchingTemporalReference = false;

		if((rule.conclusionValueMode.empty() || rule.conclusionValueMode == "Literal")
			&& TemporalExpressionMayOverlapChangedRanges(ruleSet, changedInstance, rule.conclusionValue, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		if(!rule.condition.is_null()
			&& TemporalNodeMayOverlapChangedRanges(ruleSet, changedInstance, rule.condition, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		if(rule.executableDecisionTable.is_object()
			&& TemporalNodeMayOverlapChangedRanges(ruleSet, changedInstance, rule.executableDecisionTable, attributeId, ranges, sawMatchingTemporalReference))
		{
			return true;
		}

		return !sawMatchingTemporalReference;
	}

	void EvaluateTemporalRelationshipScopedAttributeChange(RuleSet& ruleSet,
	                                                       const std::string& entityId,
	                                                       const std::string& instanceId,
	                                                       const std::string& attributeId,
	                                                       SessionModelRuleEvaluator::EvaluationStats& stats,
	                                                       std::deque<AttributeChange>& queue,
	                                                       std::unordered_set<std::string>& queuedKeys,
	                                                       std::deque<RelationshipChange>& relationshipQueue,
	                                                       std::unordered_set<std::string>& queuedRelationshipKeys)
	{
		const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingAttribute(entityId, attributeId);

		if(ruleIds == nullptr)
		{
			return;
		}

		std::unordered_set<std::string> evaluatedKeys;

		for(const std::string& ruleId : *ruleIds)
		{
			const RuleSet::SessionModelRuleMetadata* rule = FindRule(ruleSet, ruleId);

			if(rule == nullptr)
			{
				continue;
			}

			for(const RuleSet::SessionModelRuleReference& reference : rule->references)
			{
				if(reference.entityId != entityId
					|| reference.attributeId != attributeId
					|| reference.relationshipId.empty()
					|| !RuleHasTemporalReferenceToRelationship(*rule, reference.relationshipId)
					|| (reference.source.find(".expression") == std::string::npos
						&& reference.source.find(".condition") == std::string::npos))
				{
					continue;
				}

				std::shared_ptr<Relationship> relationship = FindRelationshipById(ruleSet, reference.relationshipId);

				if(relationship == nullptr || relationship->targetEntity != entityId)
				{
					continue;
				}

				for(const RuleSet::RelationshipLinkHistorySegment* segment : ruleSet.GetRelationshipLinkHistorySegmentsByTarget(reference.relationshipId, instanceId))
				{
					if(segment == nullptr || segment->sourceInstanceId.empty())
					{
						continue;
					}

					std::vector<TemporalHistoryChangeRange> ranges;

					if(!TryBuildTemporalHistoryChangeRanges({{segment->effectiveStart, segment->effectiveEnd}}, ranges))
					{
						ranges.clear();
					}

					if(!RuleTemporalRelationshipDependencyMayOverlapChangedRanges(*rule, ruleSet, reference.relationshipId, ranges))
					{
						continue;
					}

					const std::string key = ruleId + "\x1f" + reference.relationshipId + "\x1f" + segment->sourceInstanceId;

					if(!evaluatedKeys.insert(key).second)
					{
						continue;
					}

					std::shared_ptr<Instance> sourceInstance = FindInstance(ruleSet, relationship->sourceEntity, segment->sourceInstanceId);

					if(sourceInstance == nullptr)
					{
						stats.skippedRules++;

						continue;
					}

					stats.candidateRules++;

					EvaluateRuleForChangedInstance(ruleSet,
					                               sourceInstance,
					                               rule,
					                               RuleTrigger{"", "", reference.relationshipId},
					                               stats,
					                               queue,
					                               queuedKeys,
					                               relationshipQueue,
					                               queuedRelationshipKeys);
				}
			}
		}
	}

	void ProcessEvaluationQueues(RuleSet& ruleSet,
	                             std::deque<AttributeChange>& queue,
	                             std::unordered_set<std::string>& queuedKeys,
	                             std::deque<RelationshipChange>& relationshipQueue,
	                             std::unordered_set<std::string>& queuedRelationshipKeys,
	                             SessionModelRuleEvaluator::EvaluationStats& stats)
	{
		const int maxSteps = 1000;

		for(int step = 0; !queue.empty() || !relationshipQueue.empty(); ++step)
		{
			if(step >= maxSteps)
			{
				stats.maxDepthStops++;

				break;
			}

			if(!queue.empty())
			{
				AttributeChange change = queue.front();

				queue.pop_front();
				stats.queuedAttributes++;

				std::shared_ptr<Instance> changedInstance = FindInstance(ruleSet, change.entityId, change.instanceId);

				if(changedInstance == nullptr)
				{
					stats.skippedRules++;

					continue;
				}

				const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingAttribute(change.entityId, change.attributeId);

				if(ruleIds == nullptr)
				{
					continue;
				}

				std::unordered_set<std::string> seenRuleIds;
				for(const std::string& ruleId : *ruleIds)
				{
					if(!MarkUniqueRuleId(seenRuleIds, ruleId))
					{
						continue;
					}

					stats.candidateRules++;
					EvaluateRuleForChangedInstance(ruleSet,
					                               changedInstance,
					                               FindRule(ruleSet, ruleId),
					                               RuleTrigger{change.entityId, change.attributeId, ""},
					                               stats,
					                               queue,
					                               queuedKeys,
					                               relationshipQueue,
					                               queuedRelationshipKeys);
				}

				continue;
			}

			RelationshipChange change = relationshipQueue.front();

			relationshipQueue.pop_front();
			stats.queuedRelationships++;

			std::shared_ptr<Instance> changedInstance = FindInstance(ruleSet, change.entityId, change.instanceId);

			if(changedInstance == nullptr)
			{
				stats.skippedRules++;

				continue;
			}

			const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingRelationship(change.relationshipId);

			if(ruleIds == nullptr)
			{
				continue;
			}

			std::unordered_set<std::string> seenRuleIds;
			for(const std::string& ruleId : *ruleIds)
			{
				if(!MarkUniqueRuleId(seenRuleIds, ruleId))
				{
					continue;
				}

				stats.candidateRules++;
				if(!change.sourceRuleId.empty() && ruleId == change.sourceRuleId)
				{
					stats.skippedRules++;

					continue;
				}

				EvaluateRuleForChangedInstance(ruleSet,
				                               changedInstance,
				                               FindRule(ruleSet, ruleId),
				                               RuleTrigger{"", "", change.relationshipId},
				                               stats,
				                               queue,
				                               queuedKeys,
				                               relationshipQueue,
				                               queuedRelationshipKeys);
			}
		}
	}

}

bool SessionModelRuleEvaluator::EvaluateCondition(const RuleSet& ruleSet,
                                                  const std::string& entityId,
                                                  const std::string& instanceId,
                                                  const json& condition,
                                                  EvaluationStats* stats) const
{
	if(condition.is_null())
	{
		return true;
	}

	json conditionNode = condition;

	if(condition.is_object() && condition.contains("ast"))
	{
		conditionNode = condition.at("ast");
	}

	if(conditionNode.is_null())
	{
		return true;
	}

	EvaluationStats localStats;
	EvaluationStats& targetStats = stats != nullptr ? *stats : localStats;
	std::shared_ptr<Instance> instance = FindInstance(ruleSet, entityId, instanceId);

	if(instance == nullptr)
	{
		targetStats.skippedRules++;

		return false;
	}

	EvaluationContext evaluationContext;
	evaluationContext.ruleId = "flow-transition";
	evaluationContext.ruleSet = &ruleSet;
	evaluationContext.diagnostics = &targetStats.diagnostics;

	targetStats.candidateRules++;

	NodeResult result = EvaluateNode(ruleSet, instance, conditionNode, evaluationContext);

	if(result == NodeResult::Unsupported)
	{
		targetStats.unsupportedRules++;

		return false;
	}

	targetStats.evaluatedRules++;

	return result == NodeResult::True;
}

bool SessionModelRuleEvaluator::EvaluateFlowCondition(RuleSet& ruleSet,
                                                      const std::string& entityId,
                                                      const std::string& instanceId,
                                                      const json& condition,
                                                      EvaluationStats* stats) const
{
	if(condition.is_null())
	{
		return true;
	}

	json conditionNode = condition;
	if(condition.is_object() && condition.contains("ast"))
	{
		conditionNode = condition.at("ast");
	}

	if(conditionNode.is_null())
	{
		return true;
	}

	EvaluationStats localStats;
	EvaluationStats& targetStats = stats != nullptr ? *stats : localStats;
	std::shared_ptr<Instance> instance = FindInstance(ruleSet, entityId, instanceId);

	if(instance == nullptr)
	{
		targetStats.skippedRules++;

		return false;
	}

	EvaluationContext evaluationContext;
	evaluationContext.ruleId = "flow-transition";
	evaluationContext.ruleSet = &ruleSet;
	evaluationContext.stats = &targetStats;
	evaluationContext.diagnostics = &targetStats.diagnostics;
	evaluationContext.resolveFlowDerivedBooleans = true;

	targetStats.candidateRules++;
	NodeResult result = EvaluateNode(ruleSet, instance, conditionNode, evaluationContext);
	if(result == NodeResult::Unsupported)
	{
		targetStats.unsupportedRules++;

		return false;
	}

	targetStats.evaluatedRules++;

	return result == NodeResult::True;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateInitialRules(RuleSet& ruleSet) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	for(const RuleSet::SessionModelRuleMetadata& rule : *ruleSet.sessionModelRules)
	{
		if(!rule.isActive || rule.conclusionEntityId.empty() || RuleHasReactiveReference(rule))
		{
			continue;
		}

		std::shared_ptr<Entity> entity = FindEntity(ruleSet, rule.conclusionEntityId);

		if(entity == nullptr)
		{
			stats.skippedRules++;

			continue;
		}

		for(const std::shared_ptr<Instance>& instance : entity->instances)
		{
			if(instance == nullptr)
			{
				continue;
			}

			stats.candidateRules++;

			EvaluateRuleForChangedInstance(ruleSet,
			                               instance,
			                               &rule,
			                               RuleTrigger{"", "", ""},
			                               stats,
			                               queue,
			                               queuedKeys,
			                               relationshipQueue,
			                               queuedRelationshipKeys);
		}
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateAllRules(RuleSet& ruleSet, bool captureTrace) const
{
	EvaluationStats stats;
	stats.captureTrace = captureTrace;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	for(const RuleSet::SessionModelRuleMetadata& rule : *ruleSet.sessionModelRules)
	{
		if(!rule.isActive || rule.conclusionEntityId.empty())
		{
			continue;
		}

		std::shared_ptr<Entity> entity = FindEntity(ruleSet, rule.conclusionEntityId);

		if(entity == nullptr)
		{
			stats.skippedRules++;

			continue;
		}

		for(const std::shared_ptr<Instance>& instance : entity->instances)
		{
			if(instance == nullptr)
			{
				continue;
			}

			stats.candidateRules++;

			EvaluateRuleForChangedInstance(ruleSet,
			                               instance,
			                               &rule,
			                               RuleTrigger{"", "", ""},
			                               stats,
			                               queue,
			                               queuedKeys,
			                               relationshipQueue,
			                               queuedRelationshipKeys);
		}
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedAttribute(RuleSet& ruleSet,
                                                                                               const std::string& entityId,
                                                                                               const std::string& instanceId,
                                                                                               const std::string& attributeId,
                                                                                               bool captureTrace) const

{
	return EvaluateChangedAttributes(
		ruleSet,
		{ChangedAttribute{entityId, instanceId, attributeId}},
		captureTrace);
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedAttributes(
	RuleSet& ruleSet,
	const std::vector<ChangedAttribute>& attributes,
	bool captureTrace) const
{
	EvaluationStats stats;

	stats.captureTrace = captureTrace;

	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;

	for(const ChangedAttribute& attribute : attributes)
	{
		EnqueueAttributeChange(
			queue,
			queuedKeys,
			AttributeChange{attribute.entityId, attribute.instanceId, attribute.attributeId});
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	for(const ChangedAttribute& attribute : attributes)
	{
		EvaluateTemporalRelationshipScopedAttributeChange(ruleSet,
		                                                  attribute.entityId,
		                                                  attribute.instanceId,
		                                                  attribute.attributeId,
		                                                  stats,
		                                                  queue,
		                                                  queuedKeys,
		                                                  relationshipQueue,
		                                                  queuedRelationshipKeys);
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedAttributeHistory(
	RuleSet& ruleSet,
	const std::string& entityId,
	const std::string& instanceId,
	const std::string& attributeId,
	const std::vector<std::pair<std::string, std::string>>& effectiveIntervals) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;
	std::shared_ptr<Instance> changedInstance = FindInstance(ruleSet, entityId, instanceId);

	if(changedInstance == nullptr)
	{
		stats.skippedRules++;

		return stats;
	}

	const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingAttribute(entityId, attributeId);

	if(ruleIds == nullptr)
	{
		return stats;
	}

	std::vector<TemporalHistoryChangeRange> ranges;

	if(!TryBuildTemporalHistoryChangeRanges(effectiveIntervals, ranges))
	{
		ranges.clear();
	}

	stats.queuedAttributes++;

	std::unordered_set<std::string> seenRuleIds;
	for(const std::string& ruleId : *ruleIds)
	{
		if(!MarkUniqueRuleId(seenRuleIds, ruleId))
		{
			continue;
		}

		const RuleSet::SessionModelRuleMetadata* rule = FindRule(ruleSet, ruleId);

		if(rule == nullptr || !RuleTemporalDependencyMayOverlapChangedRanges(*rule, ruleSet, changedInstance, entityId, attributeId, ranges))
		{
			continue;
		}

		stats.candidateRules++;

		EvaluateRuleForChangedInstance(ruleSet,
		                               changedInstance,
		                               rule,
		                               RuleTrigger{entityId, attributeId, ""},
		                               stats,
		                               queue,
		                               queuedKeys,
		                               relationshipQueue,
		                               queuedRelationshipKeys);
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedRelationshipHistory(
	RuleSet& ruleSet,
	const std::string& relationshipId,
	const std::string& sourceEntityId,
	const std::string& sourceInstanceId,
	const std::string& targetEntityId,
	const std::string& targetInstanceId,
	const std::vector<std::pair<std::string, std::string>>& effectiveIntervals) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;
	std::shared_ptr<Instance> changedInstance = FindInstance(ruleSet, sourceEntityId, sourceInstanceId);

	if(changedInstance == nullptr)
	{
		stats.skippedRules++;

		return stats;
	}

	const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingRelationship(relationshipId);

	if(ruleIds == nullptr)
	{
		return stats;
	}

	std::vector<TemporalHistoryChangeRange> ranges;

	if(!TryBuildTemporalHistoryChangeRanges(effectiveIntervals, ranges))
	{
		ranges.clear();
	}

	stats.queuedRelationships++;

	std::unordered_set<std::string> seenRuleIds;
	for(const std::string& ruleId : *ruleIds)
	{
		if(!MarkUniqueRuleId(seenRuleIds, ruleId))
		{
			continue;
		}

		const RuleSet::SessionModelRuleMetadata* rule = FindRule(ruleSet, ruleId);

		if(rule == nullptr || !RuleTemporalRelationshipDependencyMayOverlapChangedRanges(*rule, ruleSet, relationshipId, ranges))
		{
			continue;
		}

		stats.candidateRules++;

		EvaluateRuleForChangedInstance(ruleSet,
		                               changedInstance,
		                               rule,
		                               RuleTrigger{"", "", relationshipId},
		                               stats,
		                               queue,
		                               queuedKeys,
		                               relationshipQueue,
		                               queuedRelationshipKeys);
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedEntityExistenceHistory(
	RuleSet& ruleSet,
	const std::string& entityId,
	const std::string& instanceId,
	const std::vector<std::pair<std::string, std::string>>& effectiveIntervals) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;
	std::shared_ptr<Instance> changedInstance = FindInstance(ruleSet, entityId, instanceId);

	if(changedInstance == nullptr)
	{
		stats.skippedRules++;

		return stats;
	}

	std::vector<TemporalHistoryChangeRange> ranges;

	if(!TryBuildTemporalHistoryChangeRanges(effectiveIntervals, ranges))
	{
		ranges.clear();
	}

	std::unordered_set<std::string> evaluatedKeys;
	stats.queuedRelationships++;

	auto evaluateSourceForRelationship = [&](const std::shared_ptr<Relationship>& relationship,
	                                         const std::shared_ptr<Instance>& sourceInstance) {
		if(relationship == nullptr || sourceInstance == nullptr)
		{
			return;
		}

		const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingRelationship(relationship->id);

		if(ruleIds == nullptr)
		{
			return;
		}

		for(const std::string& ruleId : *ruleIds)
		{
			const std::string key = ruleId + "\x1f" + relationship->id + "\x1f" + sourceInstance->instanceId;

			if(!evaluatedKeys.insert(key).second)
			{
				continue;
			}

			const RuleSet::SessionModelRuleMetadata* rule = FindRule(ruleSet, ruleId);

			if(rule == nullptr || !RuleTemporalRelationshipDependencyMayOverlapChangedRanges(*rule, ruleSet, relationship->id, ranges))
			{
				continue;
			}

			stats.candidateRules++;

			EvaluateRuleForChangedInstance(ruleSet,
			                               sourceInstance,
			                               rule,
			                               RuleTrigger{"", "", relationship->id},
			                               stats,
			                               queue,
			                               queuedKeys,
			                               relationshipQueue,
			                               queuedRelationshipKeys);
		}
	};

	for(const std::shared_ptr<Relationship>& relationship : FindRelationshipsForEntityEndpoint(ruleSet, entityId, true))
	{
		evaluateSourceForRelationship(relationship, changedInstance);
	}

	for(const std::shared_ptr<Relationship>& relationship : FindRelationshipsForEntityEndpoint(ruleSet, entityId, false))
	{
		if(relationship == nullptr)
		{
			continue;
		}

		for(const RuleSet::RelationshipLinkHistorySegment* segment : ruleSet.GetRelationshipLinkHistorySegmentsByTarget(relationship->id, instanceId))
		{
			if(segment == nullptr || !RelationshipLinkHistorySegmentMayOverlapRanges(*segment, ranges))
			{
				continue;
			}

			evaluateSourceForRelationship(relationship, FindInstance(ruleSet, relationship->sourceEntity, segment->sourceInstanceId));
		}
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedHistorySnapshot(RuleSet& ruleSet) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;

	if(ruleSet.sessionModelRules == nullptr || ruleSet.sessionModelRules->empty())
	{
		return stats;
	}

	for(const RuleSet::SessionModelRuleMetadata& rule : *ruleSet.sessionModelRules)
	{
		if(!rule.isActive || rule.conclusionEntityId.empty() || !RuleHasTransactionSnapshotSensitiveTemporalReference(rule))
		{
			continue;
		}

		std::shared_ptr<Entity> entity = FindEntity(ruleSet, rule.conclusionEntityId);

		if(entity == nullptr)
		{
			stats.skippedRules++;

			continue;
		}

		for(const std::shared_ptr<Instance>& instance : entity->instances)
		{
			if(instance == nullptr)
			{
				continue;
			}

			stats.candidateRules++;

			EvaluateRuleForChangedInstance(ruleSet,
			                               instance,
			                               &rule,
			                               RuleTrigger{"", "", ""},
			                               stats,
			                               queue,
			                               queuedKeys,
			                               relationshipQueue,
			                               queuedRelationshipKeys);
		}
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedSessionContext(
	RuleSet& ruleSet,
	const std::vector<std::string>& functionNames) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;
	std::unordered_set<std::string> evaluatedKeys;
	std::unordered_set<std::string> seenFunctions;

	for(const std::string& functionName : functionNames)
	{
		if(functionName.empty() || !seenFunctions.insert(functionName).second)
		{
			continue;
		}

		const std::vector<std::string>* ruleIds = ruleSet.FindActiveSessionModelRulesReferencingFunction(functionName);

		if(ruleIds == nullptr)
		{
			continue;
		}

		for(const std::string& ruleId : *ruleIds)
		{
			const RuleSet::SessionModelRuleMetadata* rule = FindRule(ruleSet, ruleId);

			if(rule == nullptr || rule->conclusionEntityId.empty())
			{
				stats.skippedRules++;

				continue;
			}

			std::shared_ptr<Entity> entity = FindEntity(ruleSet, rule->conclusionEntityId);

			if(entity == nullptr)
			{
				stats.skippedRules++;

				continue;
			}

			for(const std::shared_ptr<Instance>& instance : entity->instances)
			{
				if(instance == nullptr)
				{
					continue;
				}

				const std::string key = ruleId + "\x1f" + instance->instanceId;

				if(!evaluatedKeys.insert(key).second)
				{
					continue;
				}

				stats.candidateRules++;

				EvaluateRuleForChangedInstance(ruleSet,
				                               instance,
				                               rule,
				                               RuleTrigger{"", "", ""},
				                               stats,
				                               queue,
				                               queuedKeys,
				                               relationshipQueue,
				                               queuedRelationshipKeys);
			}
		}
	}

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}

SessionModelRuleEvaluator::EvaluationStats SessionModelRuleEvaluator::EvaluateChangedRelationship(RuleSet& ruleSet,
                                                                                                  const std::string& entityId,
                                                                                                  const std::string& instanceId,
                                                                                                  const std::string& relationshipId) const
{
	EvaluationStats stats;
	std::deque<AttributeChange> queue;
	std::unordered_set<std::string> queuedKeys;
	std::deque<RelationshipChange> relationshipQueue;
	std::unordered_set<std::string> queuedRelationshipKeys;
	std::shared_ptr<Instance> changedInstance = FindInstance(ruleSet, entityId, instanceId);

	if(changedInstance == nullptr)
	{
		stats.skippedRules++;

		return stats;
	}

	EnqueueRelationshipChange(relationshipQueue, queuedRelationshipKeys, RelationshipChange{entityId, instanceId, relationshipId, ""});

	ProcessEvaluationQueues(ruleSet, queue, queuedKeys, relationshipQueue, queuedRelationshipKeys, stats);

	return stats;
}
