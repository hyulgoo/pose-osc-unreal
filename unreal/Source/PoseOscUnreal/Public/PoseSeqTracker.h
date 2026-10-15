#pragma once

#include "CoreMinimal.h"

/** Outcome of one seq check (D3). Mirrors python/tools/osc_receive_check.py so both receivers agree. */
enum class EPoseSeqDecision : uint8
{
	Baseline,      // First packet after construction or ResetBaseline() (D3 rule 1: Idle entry). Locks the sender.
	Next,          // seq == last + 1
	Gap,           // seq > last + 1; (seq - last - 1) packets lost
	Duplicate,     // seq == last -> drop
	Stale,         // small regression (reorder) -> drop
	RestartDetected, // regression larger than the threshold (sender restart or int32 wrap) -> re-baseline (D3 rule 2)
	OtherSender,   // a second sender while one is locked -> drop until the baseline is reset (T-007)
};

struct FPoseSeqResult
{
	EPoseSeqDecision Decision = EPoseSeqDecision::Baseline;
	int32 Lost = 0;

	bool IsAccepted() const
	{
		return Decision != EPoseSeqDecision::Duplicate && Decision != EPoseSeqDecision::Stale
			&& Decision != EPoseSeqDecision::OtherSender;
	}
};

/** Plain C++ seq rules; no engine time or UObject dependency so it can be unit-tested directly. */
class POSEOSCUNREAL_API FPoseSeqTracker
{
public:
	explicit FPoseSeqTracker(int32 InRegressThreshold = 30);

	/**
	 * Classifies Seq (expected >= 0) from Sender ("ip:port") and, if accepted, makes it the new baseline.
	 * The seq rules assume one sender: two interleaved streams would look like endless restarts, so the sender of
	 * the Baseline packet is locked until ResetBaseline(). A restarted sender has a new source port and is picked
	 * up after the next Idle (D3 rule 1), i.e. within the idle timeout.
	 */
	FPoseSeqResult Classify(int32 Seq, const FString& Sender = FString());

	/** Forget the baseline and the locked sender; the next packet is accepted as Baseline. */
	void ResetBaseline();

	void SetRegressThreshold(int32 InRegressThreshold);

	TOptional<int32> GetLastSeq() const { return LastSeq; }
	const FString& GetLockedSender() const { return LockedSender; }

	static const TCHAR* LexToString(EPoseSeqDecision Decision);

private:
	TOptional<int32> LastSeq;
	FString LockedSender;
	int32 RegressThreshold = 30;
};
