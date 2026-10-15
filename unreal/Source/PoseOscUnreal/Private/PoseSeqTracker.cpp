#include "PoseSeqTracker.h"

FPoseSeqTracker::FPoseSeqTracker(int32 InRegressThreshold)
{
	SetRegressThreshold(InRegressThreshold);
}

FPoseSeqResult FPoseSeqTracker::Classify(int32 Seq, const FString& Sender)
{
	if (!LastSeq.IsSet())
	{
		LastSeq = Seq;
		LockedSender = Sender;
		return { EPoseSeqDecision::Baseline, 0 };
	}
	if (Sender != LockedSender)
	{
		return { EPoseSeqDecision::OtherSender, 0 };
	}

	// Both values are non-negative int32, so these differences cannot overflow.
	const int32 Last = LastSeq.GetValue();
	if (Seq > Last)
	{
		LastSeq = Seq;
		const int32 Lost = Seq - Last - 1;
		return { Lost > 0 ? EPoseSeqDecision::Gap : EPoseSeqDecision::Next, Lost };
	}
	if (Seq == Last)
	{
		return { EPoseSeqDecision::Duplicate, 0 };
	}
	if (Last - Seq > RegressThreshold)
	{
		LastSeq = Seq;
		return { EPoseSeqDecision::RestartDetected, 0 };
	}
	return { EPoseSeqDecision::Stale, 0 };
}

void FPoseSeqTracker::ResetBaseline()
{
	LastSeq.Reset();
	LockedSender.Reset();
}

void FPoseSeqTracker::SetRegressThreshold(int32 InRegressThreshold)
{
	RegressThreshold = FMath::Max(1, InRegressThreshold);
}

const TCHAR* FPoseSeqTracker::LexToString(EPoseSeqDecision Decision)
{
	switch (Decision)
	{
	case EPoseSeqDecision::Baseline: return TEXT("Baseline");
	case EPoseSeqDecision::Next: return TEXT("Next");
	case EPoseSeqDecision::Gap: return TEXT("Gap");
	case EPoseSeqDecision::Duplicate: return TEXT("Duplicate");
	case EPoseSeqDecision::Stale: return TEXT("Stale");
	case EPoseSeqDecision::RestartDetected: return TEXT("RestartDetected");
	case EPoseSeqDecision::OtherSender: return TEXT("OtherSender");
	}
	return TEXT("Unknown");
}
