#include "PoseInteractionStateMachine.h"

#include "PoseOscUnreal.h"

namespace PoseStateMachinePrivate
{
	const TCHAR* LexToString(EPoseHandState State)
	{
		switch (State)
		{
		case EPoseHandState::Tracking: return TEXT("Tracking");
		case EPoseHandState::Holding: return TEXT("Holding");
		case EPoseHandState::Lost: return TEXT("Lost");
		}
		return TEXT("?");
	}

	const TCHAR* LexToString(EPoseGlobalState State)
	{
		return State == EPoseGlobalState::Interactive ? TEXT("Interactive") : TEXT("Idle");
	}

	const TCHAR* LexToString(EPoseHand Hand)
	{
		return Hand == EPoseHand::Left ? TEXT("Left") : TEXT("Right");
	}
}

FString FPoseStateTransition::Describe() const
{
	using namespace PoseStateMachinePrivate;
	if (bGlobal)
	{
		return FString::Printf(TEXT("Global %s -> %s (%s)"), LexToString(GlobalFrom), LexToString(GlobalTo), *Reason);
	}
	return FString::Printf(TEXT("%s hand %s -> %s (%s)"), LexToString(Hand), LexToString(HandFrom), LexToString(HandTo), *Reason);
}

FPoseInteractionStateMachine::FPoseInteractionStateMachine(const FPoseStateMachineSettings& InSettings)
{
	SetSettings(InSettings);
}

void FPoseInteractionStateMachine::SetSettings(const FPoseStateMachineSettings& InSettings)
{
	Settings = InSettings;
	Settings.TrackingEnterConfidence = FMath::Clamp(Settings.TrackingEnterConfidence, 0.f, 1.f);
	Settings.HoldingEnterConfidence = FMath::Clamp(Settings.HoldingEnterConfidence, 0.f, 1.f);
	if (Settings.HoldingEnterConfidence > Settings.TrackingEnterConfidence)
	{
		UE_LOG(LogPoseOsc, Warning, TEXT("HoldingEnterConfidence %.2f > TrackingEnterConfidence %.2f; clamped to %.2f (no hysteresis band)"),
			Settings.HoldingEnterConfidence, Settings.TrackingEnterConfidence, Settings.TrackingEnterConfidence);
		Settings.HoldingEnterConfidence = Settings.TrackingEnterConfidence;
	}
	Settings.HoldingToLostSeconds = FMath::Max(0.f, Settings.HoldingToLostSeconds);
	Settings.InteractiveEnterSeconds = FMath::Max(0.f, Settings.InteractiveEnterSeconds);
	Settings.ContinuityMaxGapSeconds = FMath::Max(0.01f, Settings.ContinuityMaxGapSeconds);
	Settings.IdleTimeoutSeconds = FMath::Max(0.01f, Settings.IdleTimeoutSeconds);
}

void FPoseInteractionStateMachine::OnFrame(const FPoseHandsFrame& Frame, double ReceivedAt)
{
	// Continuity streak for the Idle -> Interactive time hysteresis.
	if (LastFrameTime < 0.0 || ReceivedAt - LastFrameTime > Settings.ContinuityMaxGapSeconds)
	{
		StreakStart = ReceivedAt;
	}
	LastFrameTime = ReceivedAt;

	if (GlobalState == EPoseGlobalState::Idle)
	{
		const double Streak = ReceivedAt - StreakStart;
		if (Streak >= Settings.InteractiveEnterSeconds)
		{
			SetGlobalState(EPoseGlobalState::Interactive, ReceivedAt,
				FString::Printf(TEXT("packets continuous for %.3fs >= %.2fs"), Streak, Settings.InteractiveEnterSeconds));
		}
	}

	if (GlobalState == EPoseGlobalState::Interactive)
	{
		UpdateHand(EPoseHand::Left, Frame.Left, Frame.bPersonPresent, ReceivedAt);
		UpdateHand(EPoseHand::Right, Frame.Right, Frame.bPersonPresent, ReceivedAt);
	}
}

void FPoseInteractionStateMachine::Tick(double Now)
{
	if (GlobalState != EPoseGlobalState::Interactive)
	{
		return;
	}

	const double Silence = LastFrameTime < 0.0 ? TNumericLimits<double>::Max() : Now - LastFrameTime;
	if (Silence > Settings.IdleTimeoutSeconds)
	{
		SetGlobalState(EPoseGlobalState::Idle, Now,
			FString::Printf(TEXT("no packet for %.3fs > %.2fs"), Silence, Settings.IdleTimeoutSeconds));
		for (const EPoseHand Hand : { EPoseHand::Left, EPoseHand::Right })
		{
			SetHandState(Hand, EPoseHandState::Lost, Now, TEXT("global Idle"));
		}
		StreakStart = -1.0;
		return;
	}

	CheckHoldingTimeout(EPoseHand::Left, Now);
	CheckHoldingTimeout(EPoseHand::Right, Now);
}

double FPoseInteractionStateMachine::GetHandStateAge(EPoseHand Hand, double Now) const
{
	return FMath::Max(0.0, Now - GetHand(Hand).StateSince);
}

double FPoseInteractionStateMachine::GetGlobalStateAge(double Now) const
{
	return FMath::Max(0.0, Now - GlobalSince);
}

double FPoseInteractionStateMachine::GetStreakSeconds() const
{
	return StreakStart < 0.0 || LastFrameTime < 0.0 ? 0.0 : LastFrameTime - StreakStart;
}

TArray<FPoseStateTransition> FPoseInteractionStateMachine::DrainTransitions()
{
	TArray<FPoseStateTransition> Out = MoveTemp(PendingTransitions);
	PendingTransitions.Reset();
	return Out;
}

void FPoseInteractionStateMachine::UpdateHand(EPoseHand Hand, const FPoseHandSample& Sample, bool bPersonPresent, double Time)
{
	FHand& H = GetHand(Hand);
	// person = 0 means both hands have confidence 0 (D6), whatever the coordinate fields contain.
	const float Confidence = bPersonPresent ? Sample.Confidence : 0.f;
	H.Confidence = Confidence;

	const auto ConfidenceReason = [&](const TCHAR* Op, float Threshold)
	{
		return bPersonPresent
			? FString::Printf(TEXT("conf %.2f %s %.2f"), Confidence, Op, Threshold)
			: FString(TEXT("person = 0"));
	};

	switch (H.State)
	{
	case EPoseHandState::Tracking:
		if (Confidence < Settings.HoldingEnterConfidence)
		{
			SetHandState(Hand, EPoseHandState::Holding, Time, ConfidenceReason(TEXT("<"), Settings.HoldingEnterConfidence));
		}
		else
		{
			// Also inside the hysteresis band: still Tracking, so the target keeps following (D6).
			H.Target = Sample.Position;
		}
		break;

	case EPoseHandState::Holding:
		if (Confidence >= Settings.TrackingEnterConfidence)
		{
			SetHandState(Hand, EPoseHandState::Tracking, Time, ConfidenceReason(TEXT(">="), Settings.TrackingEnterConfidence));
			H.Target = Sample.Position;
		}
		else
		{
			CheckHoldingTimeout(Hand, Time);
		}
		break;

	case EPoseHandState::Lost:
		if (Confidence >= Settings.TrackingEnterConfidence)
		{
			SetHandState(Hand, EPoseHandState::Tracking, Time, ConfidenceReason(TEXT(">="), Settings.TrackingEnterConfidence));
			H.Target = Sample.Position;
		}
		break;
	}
}

void FPoseInteractionStateMachine::CheckHoldingTimeout(EPoseHand Hand, double Now)
{
	const FHand& H = GetHand(Hand);
	if (H.State != EPoseHandState::Holding)
	{
		return;
	}
	const double Held = Now - H.StateSince;
	if (Held > Settings.HoldingToLostSeconds)
	{
		SetHandState(Hand, EPoseHandState::Lost, Now,
			FString::Printf(TEXT("Holding %.3fs > %.2fs"), Held, Settings.HoldingToLostSeconds));
	}
}

void FPoseInteractionStateMachine::SetHandState(EPoseHand Hand, EPoseHandState NewState, double Time, FString Reason)
{
	FHand& H = GetHand(Hand);
	if (H.State == NewState)
	{
		return;
	}

	FPoseStateTransition& T = PendingTransitions.AddDefaulted_GetRef();
	T.bGlobal = false;
	T.Hand = Hand;
	T.HandFrom = H.State;
	T.HandTo = NewState;
	T.Time = Time;
	T.Reason = Reason;

	H.State = NewState;
	H.StateSince = Time;
	H.Reason = MoveTemp(Reason);
}

void FPoseInteractionStateMachine::SetGlobalState(EPoseGlobalState NewState, double Time, FString Reason)
{
	if (GlobalState == NewState)
	{
		return;
	}

	FPoseStateTransition& T = PendingTransitions.AddDefaulted_GetRef();
	T.bGlobal = true;
	T.GlobalFrom = GlobalState;
	T.GlobalTo = NewState;
	T.Time = Time;
	T.Reason = Reason;

	GlobalState = NewState;
	GlobalSince = Time;
	GlobalReason = MoveTemp(Reason);
}
