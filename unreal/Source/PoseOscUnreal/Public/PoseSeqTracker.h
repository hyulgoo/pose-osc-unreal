#pragma once

#include "CoreMinimal.h"

/** Outcome of one seq check (D3). Mirrors python/tools/osc_receive_check.py so both receivers agree. */
enum class EPoseSeqDecision : uint8
{
	Baseline,      // First packet after construction or ResetBaseline() (D3 rule 1: Idle entry).
	Next,          // seq == last + 1
	Gap,           // seq > last + 1; (seq - last - 1) packets lost
	Duplicate,     // seq == last -> drop
	Stale,         // small regression (reorder) -> drop
	RestartDetected, // regression larger than the threshold (sender restart or int32 wrap) -> re-baseline (D3 rule 2)
};

struct FPoseSeqResult
{
	EPoseSeqDecision Decision = EPoseSeqDecision::Baseline;
	int32 Lost = 0;

	bool IsAccepted() const
	{
		return Decision != EPoseSeqDecision::Duplicate && Decision != EPoseSeqDecision::Stale;
	}
};

/** Plain C++ seq rules; no engine time or UObject dependency so it can be unit-tested directly. */
class POSEOSCUNREAL_API FPoseSeqTracker
{
public:
	explicit FPoseSeqTracker(int32 InRegressThreshold = 30);

	/** Classifies Seq (expected >= 0) and, if accepted, makes it the new baseline. */
	FPoseSeqResult Classify(int32 Seq);

	/** Forget the baseline; the next packet is accepted as Baseline. */
	void ResetBaseline();

	void SetRegressThreshold(int32 InRegressThreshold);

	TOptional<int32> GetLastSeq() const { return LastSeq; }

	static const TCHAR* LexToString(EPoseSeqDecision Decision);

private:
	TOptional<int32> LastSeq;
	int32 RegressThreshold = 30;
};
