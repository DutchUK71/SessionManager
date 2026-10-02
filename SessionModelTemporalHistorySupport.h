#pragma once

#include "Attribute.h"
#include "RuleSet.h"

#include <vector>

namespace SessionModelTemporalHistorySupport
{
	struct TemporalInstant
	{
		bool parsed = false;
		long long seconds = 0;
		bool hasTime = false;
	};

	struct TemporalCoverageSegment
	{
		long long startSeconds = 0;
		long long endSeconds = 0;
		Attribute::KnowledgeState knowledgeState = Attribute::KnowledgeState::Unknown;
		bool boolValue = false;
		const RuleSet::AttributeHistorySegment* source = nullptr;
	};

	struct TemporalHistoryChangeRange
	{
		long long startSeconds = 0;
		long long endSeconds = 0;
		bool endOpen = true;
	};

	enum class TemporalDayState
	{
		CertainFalse,
		CertainTrue,
		Unknown,
		Uncertain
	};

	struct TemporalCoverageSummary
	{
		std::vector<TemporalCoverageSegment> coverageSegments;
		std::vector<TemporalCoverageSegment> certainTrueSegments;
		std::vector<TemporalCoverageSegment> certainFalseSegments;
		bool sawUnknownSegment = false;
		bool sawUncertainSegment = false;
	};

	struct TemporalEffectiveWindow
	{
		TemporalInstant start;
		TemporalInstant end;
		bool instant = false;
	};

	TemporalEffectiveWindow BuildTemporalEffectiveWindow(const TemporalInstant& instant);

	bool TemporalEffectiveWindowOverlapsRanges(const std::vector<TemporalHistoryChangeRange>& ranges,
	                                           const TemporalEffectiveWindow& window);

	bool RawTemporalIntervalsOverlap(long long leftStart, long long leftEnd, long long rightStart, long long rightEnd);

	bool TemporalCoverageSegmentsOverlap(const TemporalCoverageSegment& left, const TemporalCoverageSegment& right);

	bool TemporalCoverageSegmentOverlapsRange(const TemporalCoverageSegment& segment, long long rangeStart, long long rangeEnd);

	bool TemporalCoverageCoversSlice(const TemporalCoverageSegment& coverage, long long sliceStart, long long sliceEnd);

	bool HasCompleteCoverage(std::vector<TemporalCoverageSegment> segments, long long intervalStart, long long intervalEnd);

	long long LocalDayStartSeconds(long long seconds);

	double TemporalDayFraction(long long startSeconds, long long endSeconds);

	TemporalDayState ClassifyTemporalDay(const TemporalCoverageSummary& summary, long long dayStartSeconds);

	bool AnyHistoryRangeOverlapsPoint(const std::vector<TemporalHistoryChangeRange>& ranges, const TemporalInstant& point);

	bool AnyHistoryRangeOverlapsInterval(const std::vector<TemporalHistoryChangeRange>& ranges,
	                                     const TemporalInstant& start,
	                                     const TemporalInstant& end);
}
