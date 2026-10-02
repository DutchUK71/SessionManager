#include "SessionModelTemporalHistorySupport.h"

#include <algorithm>

namespace
{
	long long PositiveModulo(long long value, long long divisor)
	{
		long long result = value % divisor;

		return result < 0 ? result + divisor : result;
	}

	bool TemporalHistoryRangeOverlapsPoint(const SessionModelTemporalHistorySupport::TemporalHistoryChangeRange& range,
	                                       long long pointSeconds)
	{
		return range.startSeconds <= pointSeconds && (range.endOpen || pointSeconds < range.endSeconds);
	}

	bool TemporalHistoryRangeOverlapsInterval(const SessionModelTemporalHistorySupport::TemporalHistoryChangeRange& range,
	                                          long long startSeconds,
	                                          long long endSeconds)
	{
		if(endSeconds <= startSeconds)
		{
			return true;
		}

		return range.startSeconds < endSeconds && (range.endOpen || startSeconds < range.endSeconds);
	}
}

namespace SessionModelTemporalHistorySupport
{
	TemporalEffectiveWindow BuildTemporalEffectiveWindow(const TemporalInstant& instant)
	{
		TemporalEffectiveWindow window;

		window.start = instant;
		window.end.parsed = true;
		window.end.seconds = instant.seconds + (instant.hasTime ? 1LL : 86400LL);
		window.end.hasTime = instant.hasTime;
		window.instant = instant.hasTime;

		return window;
	}

	bool TemporalEffectiveWindowOverlapsRanges(const std::vector<TemporalHistoryChangeRange>& ranges,
	                                           const TemporalEffectiveWindow& window)
	{
		return window.instant
			? AnyHistoryRangeOverlapsPoint(ranges, window.start)
			: AnyHistoryRangeOverlapsInterval(ranges, window.start, window.end);
	}

	bool RawTemporalIntervalsOverlap(long long leftStart, long long leftEnd, long long rightStart, long long rightEnd)
	{
		const long long overlapStart = leftStart > rightStart ? leftStart : rightStart;
		const long long overlapEnd = leftEnd < rightEnd ? leftEnd : rightEnd;

		return overlapStart < overlapEnd;
	}

	bool TemporalCoverageSegmentsOverlap(const TemporalCoverageSegment& left, const TemporalCoverageSegment& right)
	{
		return RawTemporalIntervalsOverlap(left.startSeconds, left.endSeconds, right.startSeconds, right.endSeconds);
	}

	bool TemporalCoverageSegmentOverlapsRange(const TemporalCoverageSegment& segment, long long rangeStart, long long rangeEnd)
	{
		return RawTemporalIntervalsOverlap(segment.startSeconds, segment.endSeconds, rangeStart, rangeEnd);
	}

	bool TemporalCoverageCoversSlice(const TemporalCoverageSegment& coverage, long long sliceStart, long long sliceEnd)
	{
		return coverage.startSeconds <= sliceStart && coverage.endSeconds >= sliceEnd;
	}

	bool HasCompleteCoverage(std::vector<TemporalCoverageSegment> segments, long long intervalStart, long long intervalEnd)
	{
		if(intervalStart >= intervalEnd)
		{
			return true;
		}

		std::sort(segments.begin(), segments.end(), [](const TemporalCoverageSegment& left, const TemporalCoverageSegment& right) {
			if(left.startSeconds != right.startSeconds)
			{
				return left.startSeconds < right.startSeconds;
			}

			return left.endSeconds < right.endSeconds;
		});

		long long cursor = intervalStart;

		for(const TemporalCoverageSegment& segment : segments)
		{
			if(segment.endSeconds <= cursor)
			{
				continue;
			}

			if(segment.startSeconds > cursor)
			{
				return false;
			}

			cursor = cursor > segment.endSeconds ? cursor : segment.endSeconds;

			if(cursor >= intervalEnd)
			{
				return true;
			}
		}

		return cursor >= intervalEnd;
	}

	long long LocalDayStartSeconds(long long seconds)
	{
		return seconds - PositiveModulo(seconds, 86400LL);
	}

	double TemporalDayFraction(long long startSeconds, long long endSeconds)
	{
		return static_cast<double>(endSeconds - startSeconds) / 86400.0;
	}

	TemporalDayState ClassifyTemporalDay(const TemporalCoverageSummary& summary, long long dayStartSeconds)
	{
		const long long dayEndSeconds = dayStartSeconds + 86400LL;

		for(const TemporalCoverageSegment& segment : summary.certainFalseSegments)
		{
			if(segment.endSeconds > dayStartSeconds && segment.startSeconds < dayEndSeconds)
			{
				return TemporalDayState::CertainFalse;
			}
		}

		if(HasCompleteCoverage(summary.certainTrueSegments, dayStartSeconds, dayEndSeconds))
		{
			return TemporalDayState::CertainTrue;
		}

		if(!HasCompleteCoverage(summary.coverageSegments, dayStartSeconds, dayEndSeconds))
		{
			return TemporalDayState::Unknown;
		}

		bool sawUnknown = false;
		bool sawUncertain = false;

		for(const TemporalCoverageSegment& segment : summary.coverageSegments)
		{
			if(segment.endSeconds <= dayStartSeconds || segment.startSeconds >= dayEndSeconds)
			{
				continue;
			}

			if(segment.knowledgeState == Attribute::KnowledgeState::Unknown)
			{
				sawUnknown = true;
			}
			else if(segment.knowledgeState == Attribute::KnowledgeState::Uncertain)
			{
				sawUncertain = true;
			}
		}

		if(sawUnknown)
		{
			return TemporalDayState::Unknown;
		}

		if(sawUncertain)
		{
			return TemporalDayState::Uncertain;
		}

		return TemporalDayState::CertainFalse;
	}

	bool AnyHistoryRangeOverlapsPoint(const std::vector<TemporalHistoryChangeRange>& ranges, const TemporalInstant& point)
	{
		if(ranges.empty())
		{
			return true;
		}

		for(const TemporalHistoryChangeRange& range : ranges)
		{
			if(TemporalHistoryRangeOverlapsPoint(range, point.seconds))
			{
				return true;
			}
		}

		return false;
	}

	bool AnyHistoryRangeOverlapsInterval(const std::vector<TemporalHistoryChangeRange>& ranges,
	                                     const TemporalInstant& start,
	                                     const TemporalInstant& end)
	{
		if(ranges.empty())
		{
			return true;
		}

		for(const TemporalHistoryChangeRange& range : ranges)
		{
			if(TemporalHistoryRangeOverlapsInterval(range, start.seconds, end.seconds))
			{
				return true;
			}
		}

		return false;
	}
}
