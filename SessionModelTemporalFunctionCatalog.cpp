#include "SessionModelTemporalFunctionCatalog.h"

#include <algorithm>

namespace SessionModelTemporalFunctions
{
	namespace
	{
		FunctionMetadata MakeFunction(const std::string& name,
		                              FunctionKind kind,
		                              std::vector<ArgumentRole> argumentRoles,
		                              std::vector<DependencyDomain> dependencyDomains,
		                              size_t minimumArgumentCount,
		                              size_t maximumArgumentCount,
		                              const std::string& unsupportedShapeDiagnostic,
		                              ExtractionMode extractionMode = ExtractionMode::RoleMetadata,
		                              const std::string& canonicalName = "")
		{
			FunctionMetadata metadata;

			metadata.name = name;
			metadata.canonicalName = canonicalName.empty() ? name : canonicalName;
			metadata.implemented = true;
			metadata.kind = kind;
			metadata.extractionMode = extractionMode;
			metadata.unsupportedShapeDiagnostic = unsupportedShapeDiagnostic;
			metadata.argumentRoles = argumentRoles;
			metadata.dependencyDomains = dependencyDomains;
			metadata.minimumArgumentCount = minimumArgumentCount;
			metadata.maximumArgumentCount = maximumArgumentCount;

			return metadata;
		}

		FunctionShapeMetadata MakeTemporalOncePerMonthShape(const std::string& shapeName,
		                                                    TemporalOncePerMonthShapeFamily shapeFamily,
		                                                    std::vector<ArgumentRole> argumentRoles,
		                                                    std::vector<DependencyDomain> dependencyDomains,
		                                                    size_t argumentCount)
		{
			FunctionShapeMetadata metadata;

			metadata.functionName = "TemporalOncePerMonth";
			metadata.shapeName = shapeName;
			metadata.implemented = true;
			metadata.temporalOncePerMonthShapeFamily = shapeFamily;
			metadata.unsupportedShapeDiagnostic = "unsupported_temporal_function_shape";
			metadata.argumentRoles = argumentRoles;
			metadata.dependencyDomains = dependencyDomains;
			metadata.minimumArgumentCount = argumentCount;
			metadata.maximumArgumentCount = argumentCount;

			return metadata;
		}

		ObservedTemporalFunctionShapeMetadata MakeDeferredObservedShape(const std::string& functionName,
		                                                                const std::string& shapeName,
		                                                                size_t observedArity,
		                                                                std::vector<ArgumentRole> argumentRoles,
		                                                                std::vector<DependencyDomain> dependencyDomains,
		                                                                const std::string& sourceFile,
		                                                                const std::string& status,
		                                                                const std::string& diagnostic,
		                                                                const std::string& notes)
		{
			ObservedTemporalFunctionShapeMetadata metadata;

			metadata.functionName = functionName;
			metadata.shapeName = shapeName;
			metadata.observedArity = observedArity;
			metadata.argumentRoles = argumentRoles;
			metadata.dependencyDomains = dependencyDomains;
			metadata.sourceFile = sourceFile;
			metadata.implemented = false;
			metadata.status = status;
			metadata.diagnostic = diagnostic;
			metadata.confidence = "observed";
			metadata.notes = notes;

			return metadata;
		}

		const std::vector<DependencyDomain>& ScalarHistoryDomains()
		{
			static const std::vector<DependencyDomain> domains{
				DependencyDomain::ScalarAttributeHistory,
				DependencyDomain::ExpressionDependency
			};

			return domains;
		}

		const std::vector<DependencyDomain>& RelationshipHistoryDomains()
		{
			static const std::vector<DependencyDomain> domains{
				DependencyDomain::RelationshipLinkHistory,
				DependencyDomain::RelationshipCoverageHistory,
				DependencyDomain::EntityExistenceHistory,
				DependencyDomain::ExpressionDependency
			};

			return domains;
		}

		bool HasKind(const std::string& name, FunctionKind kind)
		{
			const FunctionMetadata* metadata = Find(name);

			return metadata != nullptr && metadata->kind == kind;
		}

		const std::string EmptyDiagnostic;
		const std::string RuleFunctionsAuditSource = "C:\\Users\\evert\\Downloads\\RuleFunctions_20260704-193203.json";
	}

	const std::vector<FunctionMetadata>& All()
	{
		static const std::vector<FunctionMetadata> functions{
			MakeFunction("ValueAt",
			             FunctionKind::ValueAt,
			             {ArgumentRole::ValueHistorySource, ArgumentRole::TimePoint},
			             ScalarHistoryDomains(),
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("Earliest",
			             FunctionKind::ObservedDate,
			             {ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             1,
			             1,
			             "unsupported_temporal_function_shape"),
			MakeFunction("Latest",
			             FunctionKind::ObservedDate,
			             {ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             1,
			             1,
			             "unsupported_temporal_function_shape"),
			MakeFunction("WhenLast",
			             FunctionKind::ObservedDate,
			             {ArgumentRole::ValueHistorySource, ArgumentRole::AnchorDate},
			             ScalarHistoryDomains(),
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("WhenNext",
			             FunctionKind::ObservedDate,
			             {ArgumentRole::ValueHistorySource, ArgumentRole::AnchorDate},
			             ScalarHistoryDomains(),
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalAlways",
			             FunctionKind::BooleanInterval,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalSometimes",
			             FunctionKind::BooleanInterval,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalAtLeastDays",
			             FunctionKind::DayCountInterval,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::WindowDays, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalConsecutiveDays",
			             FunctionKind::DayCountInterval,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::WindowDays, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalDailySum",
			             FunctionKind::WeightedAggregate,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_aggregate_shape"),
			MakeFunction("IntervalWeightedAverage",
			             FunctionKind::WeightedAggregate,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_aggregate_shape"),
			MakeFunction("IntervalCountDistinct",
			             FunctionKind::CountDistinctAggregate,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_aggregate_shape"),
			MakeFunction("IntervalCountDistinctIf",
			             FunctionKind::ConditionalCountDistinctAggregate,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_conditional_aggregate_shape"),
			MakeFunction("IntervalDailySumIf",
			             FunctionKind::ConditionalWeightedAggregate,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_conditional_aggregate_shape"),
			MakeFunction("IntervalWeightedAverageIf",
			             FunctionKind::ConditionalWeightedAggregate,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_conditional_aggregate_shape"),
			MakeFunction("IntervalMaximum",
			             FunctionKind::IntervalExtrema,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalMinimum",
			             FunctionKind::IntervalExtrema,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource},
			             ScalarHistoryDomains(),
			             3,
			             3,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IntervalMaximumIf",
			             FunctionKind::ConditionalIntervalExtrema,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_conditional_aggregate_shape"),
			MakeFunction("IntervalMinimumIf",
			             FunctionKind::ConditionalIntervalExtrema,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ValueHistorySource, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             4,
			             4,
			             "unsupported_temporal_conditional_aggregate_shape"),
			MakeFunction("TemporalAlwaysDays",
			             FunctionKind::RelativeWindow,
			             {ArgumentRole::AnchorDate, ArgumentRole::WindowDays, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             2,
			             3,
			             "unsupported_temporal_aggregate_shape"),
			MakeFunction("TemporalConsecutiveDays",
			             FunctionKind::RelativeWindow,
			             {ArgumentRole::AnchorDate, ArgumentRole::WindowDays, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             2,
			             3,
			             "unsupported_temporal_aggregate_shape"),
			MakeFunction("TemporalSometimesDays",
			             FunctionKind::RelativeWindow,
			             {ArgumentRole::AnchorDate, ArgumentRole::WindowDays, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             2,
			             3,
			             "unsupported_temporal_aggregate_shape"),
			MakeFunction("TemporalDaysSince",
			             FunctionKind::DaysSince,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd},
			             {DependencyDomain::ExpressionDependency},
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalWeeksSince",
			             FunctionKind::ElapsedDateUnit,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd},
			             {DependencyDomain::ExpressionDependency},
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalMonthsSince",
			             FunctionKind::ElapsedDateUnit,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd},
			             {DependencyDomain::ExpressionDependency},
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalYearsSince",
			             FunctionKind::ElapsedDateUnit,
			             {ArgumentRole::WindowStart, ArgumentRole::WindowEnd},
			             {DependencyDomain::ExpressionDependency},
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalFromStartDate",
			             FunctionKind::ScalarDatePredicate,
			             {ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             1,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalFromEndDate",
			             FunctionKind::ScalarDatePredicate,
			             {ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             1,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalFromRange",
			             FunctionKind::ScalarDatePredicate,
			             {ArgumentRole::PlainExpression, ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             2,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalOncePerMonth",
			             FunctionKind::ScalarDatePredicate,
			             {ArgumentRole::AnchorDate, ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ConditionHistorySource},
			             ScalarHistoryDomains(),
			             1,
			             4,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalIsWeekday",
			             FunctionKind::ScalarDatePredicate,
			             {ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             1,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalOn",
			             FunctionKind::ScalarDateComparison,
			             {ArgumentRole::PlainExpression, ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalBefore",
			             FunctionKind::ScalarDateComparison,
			             {ArgumentRole::PlainExpression, ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalAfter",
			             FunctionKind::ScalarDateComparison,
			             {ArgumentRole::PlainExpression, ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalOnOrBefore",
			             FunctionKind::ScalarDateComparison,
			             {ArgumentRole::PlainExpression, ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("TemporalOnOrAfter",
			             FunctionKind::ScalarDateComparison,
			             {ArgumentRole::PlainExpression, ArgumentRole::PlainExpression},
			             {DependencyDomain::ExpressionDependency},
			             1,
			             2,
			             "unsupported_temporal_function_shape"),
			MakeFunction("IsMemberOfAt",
			             FunctionKind::RelationshipPointInTime,
			             {ArgumentRole::TargetInstanceExpression, ArgumentRole::RelationshipSource, ArgumentRole::TimePoint},
			             RelationshipHistoryDomains(),
			             3,
			             4,
			             "unsupported_temporal_relationship_shape",
			             ExtractionMode::Procedural),
			MakeFunction("ForScopeAt",
			             FunctionKind::RelationshipScopeTraversal,
			             {ArgumentRole::RelationshipSource, ArgumentRole::TimePoint, ArgumentRole::ScopedExpression},
			             RelationshipHistoryDomains(),
			             3,
			             4,
			             "unsupported_temporal_relationship_shape",
			             ExtractionMode::Procedural),
			MakeFunction("ForAllScopeAt",
			             FunctionKind::RelationshipUniversalQuantifier,
			             {ArgumentRole::RelationshipSource, ArgumentRole::TimePoint, ArgumentRole::ScopedExpression},
			             RelationshipHistoryDomains(),
			             3,
			             4,
			             "unsupported_temporal_relationship_shape",
			             ExtractionMode::Procedural),
			MakeFunction("ForAllAt",
			             FunctionKind::RelationshipUniversalQuantifier,
			             {ArgumentRole::RelationshipSource, ArgumentRole::TimePoint, ArgumentRole::ScopedExpression},
			             RelationshipHistoryDomains(),
			             3,
			             4,
			             "unsupported_temporal_relationship_shape",
			             ExtractionMode::Procedural,
			             "ForAllScopeAt")
		};

		return functions;
	}

	const FunctionMetadata* Find(const std::string& name)
	{
		const std::vector<FunctionMetadata>& functions = All();
		auto it = std::find_if(functions.begin(), functions.end(), [&name](const FunctionMetadata& metadata) {
			return metadata.name == name;
		});

		return it == functions.end() ? nullptr : &(*it);
	}

	const std::vector<FunctionShapeMetadata>& TemporalOncePerMonthShapes()
	{
		static const std::vector<FunctionShapeMetadata> shapes{
			MakeTemporalOncePerMonthShape("anchorDate",
			                              TemporalOncePerMonthShapeFamily::AnchorCurrentDate,
			                              {ArgumentRole::AnchorDate},
			                              {DependencyDomain::ExpressionDependency},
			                              1),
			MakeTemporalOncePerMonthShape("anchorDate_condition",
			                              TemporalOncePerMonthShapeFamily::AnchorConditionHistory,
			                              {ArgumentRole::AnchorDate, ArgumentRole::ConditionHistorySource},
			                              ScalarHistoryDomains(),
			                              2),
			MakeTemporalOncePerMonthShape("startDate_endDate_dayOfMonth",
			                              TemporalOncePerMonthShapeFamily::RangeDayCurrentDate,
			                              {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::DayOfMonth},
			                              {DependencyDomain::ExpressionDependency},
			                              3),
			MakeTemporalOncePerMonthShape("anchorDate_startDate_endDate",
			                              TemporalOncePerMonthShapeFamily::AnchorRangeCurrentDate,
			                              {ArgumentRole::AnchorDate, ArgumentRole::WindowStart, ArgumentRole::WindowEnd},
			                              {DependencyDomain::ExpressionDependency},
			                              3),
			MakeTemporalOncePerMonthShape("startDate_endDate_condition",
			                              TemporalOncePerMonthShapeFamily::RangeConditionHistory,
			                              {ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ConditionHistorySource},
			                              ScalarHistoryDomains(),
			                              3),
			MakeTemporalOncePerMonthShape("anchorDate_startDate_endDate_condition",
			                              TemporalOncePerMonthShapeFamily::AnchorRangeConditionHistory,
			                              {ArgumentRole::AnchorDate, ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ConditionHistorySource},
			                              ScalarHistoryDomains(),
			                              4)
		};

		return shapes;
	}

	const std::vector<FunctionShapeMetadata>& DeferredTemporalOncePerMonthShapes()
	{
		static const std::vector<FunctionShapeMetadata> shapes;

		return shapes;
	}

	const std::vector<ObservedTemporalFunctionShapeMetadata>& DeferredObservedTemporalFunctionShapes()
	{
		static const std::vector<ObservedTemporalFunctionShapeMetadata> shapes{
			MakeDeferredObservedShape("Earliest",
			                          "implicit_source",
			                          0,
			                          {},
			                          {},
			                          RuleFunctionsAuditSource,
			                          "observed_ambiguous",
			                          "unsupported_temporal_function_shape",
			                          "Source catalog lists no parameters; runtime requires a direct scalar history source."),
			MakeDeferredObservedShape("Latest",
			                          "implicit_source",
			                          0,
			                          {},
			                          {},
			                          RuleFunctionsAuditSource,
			                          "observed_ambiguous",
			                          "unsupported_temporal_function_shape",
			                          "Source catalog lists no parameters; runtime requires a direct scalar history source."),
			MakeDeferredObservedShape("WhenLast",
			                          "date_condition",
			                          2,
			                          {ArgumentRole::AnchorDate, ArgumentRole::ConditionHistorySource},
			                          ScalarHistoryDomains(),
			                          RuleFunctionsAuditSource,
			                          "observed_ambiguous",
			                          "temporal_attribute_not_found",
			                          "Source catalog lists date and condition parameters; runtime implements attribute plus optional anchor time."),
			MakeDeferredObservedShape("WhenNext",
			                          "date_condition",
			                          2,
			                          {ArgumentRole::AnchorDate, ArgumentRole::ConditionHistorySource},
			                          ScalarHistoryDomains(),
			                          RuleFunctionsAuditSource,
			                          "observed_ambiguous",
			                          "temporal_attribute_not_found",
			                          "Source catalog lists date and condition parameters; runtime implements attribute plus optional anchor time."),
			MakeDeferredObservedShape("TemporalFromStartDate",
			                          "relationship_date_value",
			                          3,
			                          {ArgumentRole::RelationshipSource, ArgumentRole::TimePoint, ArgumentRole::ScopedExpression},
			                          RelationshipHistoryDomains(),
			                          RuleFunctionsAuditSource,
			                          "deferred",
			                          "unsupported_temporal_function_shape",
			                          "Source catalog relationship-scoped form is observed; runtime currently implements only the scalar CurrentDate predicate form."),
			MakeDeferredObservedShape("TemporalFromEndDate",
			                          "relationship_date_value",
			                          3,
			                          {ArgumentRole::RelationshipSource, ArgumentRole::TimePoint, ArgumentRole::ScopedExpression},
			                          RelationshipHistoryDomains(),
			                          RuleFunctionsAuditSource,
			                          "deferred",
			                          "unsupported_temporal_function_shape",
			                          "Source catalog relationship-scoped form is observed; runtime currently implements only the scalar CurrentDate predicate form."),
			MakeDeferredObservedShape("TemporalFromRange",
			                          "relationship_startDate_endDate_value",
			                          4,
			                          {ArgumentRole::RelationshipSource, ArgumentRole::WindowStart, ArgumentRole::WindowEnd, ArgumentRole::ScopedExpression},
			                          RelationshipHistoryDomains(),
			                          RuleFunctionsAuditSource,
			                          "deferred",
			                          "unsupported_temporal_function_shape",
			                          "Source catalog relationship-scoped range form is observed; runtime currently implements only the scalar CurrentDate range predicate form."),
			MakeDeferredObservedShape("TemporalIsWeekday",
			                          "startDate_endDate",
			                          2,
			                          {ArgumentRole::WindowStart, ArgumentRole::WindowEnd},
			                          {DependencyDomain::ExpressionDependency},
			                          RuleFunctionsAuditSource,
			                          "observed_ambiguous",
			                          "unsupported_temporal_function_shape",
			                          "Source catalog lists start/end date parameters but no weekday-range semantics are contracted; runtime currently implements one Date argument.")
		};

		return shapes;
	}

	bool IsKnownName(const std::string& name)
	{
		return Find(name) != nullptr;
	}

	bool IsImplementedName(const std::string& name)
	{
		const FunctionMetadata* metadata = Find(name);

		return metadata != nullptr && metadata->implemented;
	}

	bool IsIntervalFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::BooleanInterval) || HasKind(name, FunctionKind::DayCountInterval);
	}

	bool IsDayCountFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::DayCountInterval);
	}

	bool IsRelativeWindowFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::RelativeWindow);
	}

	bool IsExtremaFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::IntervalExtrema);
	}

	bool IsConditionalAggregateFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::ConditionalIntervalExtrema);
	}

	bool IsWeightedAggregateFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::WeightedAggregate);
	}

	bool IsConditionalWeightedAggregateFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::ConditionalWeightedAggregate);
	}

	bool IsCountDistinctAggregateFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::CountDistinctAggregate);
	}

	bool IsConditionalCountDistinctAggregateFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::ConditionalCountDistinctAggregate);
	}

	bool IsScalarDateComparisonFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::ScalarDateComparison);
	}

	bool IsElapsedDateUnitFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::ElapsedDateUnit);
	}

	bool IsScalarDatePredicateFunctionName(const std::string& name)
	{
		return HasKind(name, FunctionKind::ScalarDatePredicate);
	}

	bool IsTemporalUniversalQuantifierName(const std::string& name)
	{
		return HasKind(name, FunctionKind::RelationshipUniversalQuantifier);
	}

	bool UsesProceduralDependencyExtraction(const std::string& name)
	{
		const FunctionMetadata* metadata = Find(name);

		return metadata != nullptr && metadata->extractionMode == ExtractionMode::Procedural;
	}

	const std::string& UnsupportedShapeDiagnostic(const std::string& name)
	{
		const FunctionMetadata* metadata = Find(name);

		return metadata == nullptr ? EmptyDiagnostic : metadata->unsupportedShapeDiagnostic;
	}

	bool HasDependencyDomain(const FunctionMetadata& metadata, DependencyDomain domain)
	{
		return std::find(metadata.dependencyDomains.begin(), metadata.dependencyDomains.end(), domain) != metadata.dependencyDomains.end();
	}

	bool HasDependencyDomain(const FunctionShapeMetadata& metadata, DependencyDomain domain)
	{
		return std::find(metadata.dependencyDomains.begin(), metadata.dependencyDomains.end(), domain) != metadata.dependencyDomains.end();
	}

	bool HasDependencyDomain(const ObservedTemporalFunctionShapeMetadata& metadata, DependencyDomain domain)
	{
		return std::find(metadata.dependencyDomains.begin(), metadata.dependencyDomains.end(), domain) != metadata.dependencyDomains.end();
	}
}
