#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SessionModelTemporalFunctions
{
	enum class ArgumentRole
	{
		TimePoint,
		WindowStart,
		WindowEnd,
		WindowDays,
		AnchorDate,
		ValueHistorySource,
		ConditionHistorySource,
		DayOfMonth,
		RelationshipSource,
		TargetInstanceExpression,
		ScopedExpression,
		PlainExpression,
		LiteralOrScalarExpression
	};

	enum class DependencyDomain
	{
		ScalarAttributeHistory,
		EntityExistenceHistory,
		RelationshipLinkHistory,
		RelationshipCoverageHistory,
		ExpressionDependency
	};

	enum class ExtractionMode
	{
		RoleMetadata,
		Procedural
	};

	enum class FunctionKind
	{
		ValueAt,
		ObservedDate,
		BooleanInterval,
		DayCountInterval,
		RelativeWindow,
		DaysSince,
		ElapsedDateUnit,
		ScalarDatePredicate,
		IntervalExtrema,
		ConditionalIntervalExtrema,
		WeightedAggregate,
		ConditionalWeightedAggregate,
		CountDistinctAggregate,
		ConditionalCountDistinctAggregate,
		ScalarDateComparison,
		RelationshipPointInTime,
		RelationshipScopeTraversal,
		RelationshipUniversalQuantifier
	};

	enum class TemporalOncePerMonthShapeFamily
	{
		AnchorCurrentDate,
		AnchorConditionHistory,
		RangeDayCurrentDate,
		AnchorRangeCurrentDate,
		RangeConditionHistory,
		AnchorRangeConditionHistory
	};

	struct FunctionMetadata
	{
		std::string name;
		std::string canonicalName;
		bool implemented = false;
		FunctionKind kind = FunctionKind::ValueAt;
		ExtractionMode extractionMode = ExtractionMode::RoleMetadata;
		std::string unsupportedShapeDiagnostic;
		std::vector<ArgumentRole> argumentRoles;
		std::vector<DependencyDomain> dependencyDomains;
		size_t minimumArgumentCount = 0;
		size_t maximumArgumentCount = 0;
	};

	struct FunctionShapeMetadata
	{
		std::string functionName;
		std::string shapeName;
		bool implemented = false;
		TemporalOncePerMonthShapeFamily temporalOncePerMonthShapeFamily = TemporalOncePerMonthShapeFamily::AnchorCurrentDate;
		std::string unsupportedShapeDiagnostic;
		std::vector<ArgumentRole> argumentRoles;
		std::vector<DependencyDomain> dependencyDomains;
		size_t minimumArgumentCount = 0;
		size_t maximumArgumentCount = 0;
	};

	struct ObservedTemporalFunctionShapeMetadata
	{
		std::string functionName;
		std::string shapeName;
		size_t observedArity = 0;
		std::vector<ArgumentRole> argumentRoles;
		std::vector<DependencyDomain> dependencyDomains;
		std::string sourceFile;
		bool implemented = false;
		std::string status;
		std::string diagnostic;
		std::string confidence;
		std::string notes;
	};

	const std::vector<FunctionMetadata>& All();
	const FunctionMetadata* Find(const std::string& name);
	const std::vector<FunctionShapeMetadata>& TemporalOncePerMonthShapes();
	const std::vector<FunctionShapeMetadata>& DeferredTemporalOncePerMonthShapes();
	const std::vector<ObservedTemporalFunctionShapeMetadata>& DeferredObservedTemporalFunctionShapes();

	bool IsKnownName(const std::string& name);
	bool IsImplementedName(const std::string& name);
	bool IsIntervalFunctionName(const std::string& name);
	bool IsDayCountFunctionName(const std::string& name);
	bool IsRelativeWindowFunctionName(const std::string& name);
	bool IsExtremaFunctionName(const std::string& name);
	bool IsConditionalAggregateFunctionName(const std::string& name);
	bool IsWeightedAggregateFunctionName(const std::string& name);
	bool IsConditionalWeightedAggregateFunctionName(const std::string& name);
	bool IsCountDistinctAggregateFunctionName(const std::string& name);
	bool IsConditionalCountDistinctAggregateFunctionName(const std::string& name);
	bool IsScalarDateComparisonFunctionName(const std::string& name);
	bool IsElapsedDateUnitFunctionName(const std::string& name);
	bool IsScalarDatePredicateFunctionName(const std::string& name);
	bool IsTemporalUniversalQuantifierName(const std::string& name);
	bool UsesProceduralDependencyExtraction(const std::string& name);
	const std::string& UnsupportedShapeDiagnostic(const std::string& name);
	bool HasDependencyDomain(const FunctionMetadata& metadata, DependencyDomain domain);
	bool HasDependencyDomain(const FunctionShapeMetadata& metadata, DependencyDomain domain);
	bool HasDependencyDomain(const ObservedTemporalFunctionShapeMetadata& metadata, DependencyDomain domain);
}
