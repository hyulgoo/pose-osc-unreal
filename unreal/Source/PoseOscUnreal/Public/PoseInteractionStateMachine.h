#pragma once

#include "CoreMinimal.h"
#include "PoseOscTypes.h"

#include "PoseInteractionStateMachine.generated.h"

/** D6 numbers. All are initial values meant to be tuned in the Details panel. */
USTRUCT(BlueprintType)
struct FPoseStateMachineSettings
{
	GENERATED_BODY()

	/** Holding / Lost -> Tracking at or above this confidence (D6: 0.6). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand", meta = (ClampMin = "0", ClampMax = "1"))
	float TrackingEnterConfidence = 0.6f;

	/**
	 * Tracking -> Holding below this confidence (D6: 0.4). Keep the Python filter gate (--filter-min-conf) at or
	 * below this value so every coordinate used in Tracking has been filtered (D4).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand", meta = (ClampMin = "0", ClampMax = "1"))
	float HoldingEnterConfidence = 0.4f;

	/** Holding -> Lost after holding this long (D6: 0.5 s). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hand", meta = (ClampMin = "0", Units = "Seconds"))
	float HoldingToLostSeconds = 0.5f;

	/** Idle -> Interactive once packets have arrived continuously for this long (D6: 0.2 s). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Global", meta = (ClampMin = "0", Units = "Seconds"))
	float InteractiveEnterSeconds = 0.2f;

	/** Largest gap between packets that still counts as continuous (D12: 0.15 s, about 4 frames at 30 Hz). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Global", meta = (ClampMin = "0.01", Units = "Seconds"))
	float ContinuityMaxGapSeconds = 0.15f;

	/** Interactive -> Idle when no packet arrived for this long (D6: 1.0 s). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Global", meta = (ClampMin = "0.01", Units = "Seconds"))
	float IdleTimeoutSeconds = 1.0f;
};

/** One state change with the reason shown on screen and in the log. */
struct FPoseStateTransition
{
	bool bGlobal = true;
	EPoseHand Hand = EPoseHand::Left; // Valid when !bGlobal.
	EPoseGlobalState GlobalFrom = EPoseGlobalState::Idle;
	EPoseGlobalState GlobalTo = EPoseGlobalState::Idle;
	EPoseHandState HandFrom = EPoseHandState::Lost;
	EPoseHandState HandTo = EPoseHandState::Lost;
	double Time = 0.0;
	FString Reason;

	FString Describe() const;
};

/**
 * Interpretation layer (D5, D6): consumes "frame + receive time" and "current time" only, so it does not depend on
 * the transport (engine OSC plugin today, a custom UDP receiver later) or on the engine clock. Unit-tested in
 * Private/Tests/PoseInteractionStateMachineSpec.cpp.
 *
 * Per hand: Tracking (conf >= TrackingEnter) -> Holding (conf < HoldingEnter, last position held) -> Lost (Holding
 * longer than HoldingToLost). Between the two thresholds the current state is kept (value hysteresis).
 * Global: Idle -> Interactive after InteractiveEnter of continuous packets; Interactive -> Idle after IdleTimeout of
 * silence (time hysteresis). Entering Idle forces both hands to Lost. Hands are only evaluated while Interactive.
 */
class POSEOSCUNREAL_API FPoseInteractionStateMachine
{
public:
	explicit FPoseInteractionStateMachine(const FPoseStateMachineSettings& InSettings = FPoseStateMachineSettings());

	/** Applies settings after sanitizing them (HoldingEnter is clamped to <= TrackingEnter). */
	void SetSettings(const FPoseStateMachineSettings& InSettings);
	const FPoseStateMachineSettings& GetSettings() const { return Settings; }

	/** One accepted frame; ReceivedAt is in the same clock as Tick (FPlatformTime::Seconds in the game). */
	void OnFrame(const FPoseHandsFrame& Frame, double ReceivedAt);

	/** Time-based transitions (Holding -> Lost, Interactive -> Idle). */
	void Tick(double Now);

	EPoseGlobalState GetGlobalState() const { return GlobalState; }
	EPoseHandState GetHandState(EPoseHand Hand) const { return GetHand(Hand).State; }

	/** Last position seen while Tracking (normalized). Held during Holding; meaningless while Lost. */
	FVector GetHandTarget(EPoseHand Hand) const { return GetHand(Hand).Target; }
	float GetHandConfidence(EPoseHand Hand) const { return GetHand(Hand).Confidence; }

	/** Seconds spent in the current hand state (0 if Now precedes the change). */
	double GetHandStateAge(EPoseHand Hand, double Now) const;
	double GetGlobalStateAge(double Now) const;

	const FString& GetHandReason(EPoseHand Hand) const { return GetHand(Hand).Reason; }
	const FString& GetGlobalReason() const { return GlobalReason; }

	/** Receive time of the last frame, or a negative value if none yet. */
	double GetLastFrameTime() const { return LastFrameTime; }

	/** Length of the current continuous-packet streak (0 when there is none). */
	double GetStreakSeconds() const;

	/** State changes since the previous call, oldest first. */
	TArray<FPoseStateTransition> DrainTransitions();

private:
	struct FHand
	{
		EPoseHandState State = EPoseHandState::Lost;
		FVector Target = FVector::ZeroVector;
		float Confidence = 0.f;
		double StateSince = 0.0;
		FString Reason = TEXT("initial");
	};

	const FHand& GetHand(EPoseHand Hand) const { return Hands[Hand == EPoseHand::Left ? 0 : 1]; }
	FHand& GetHand(EPoseHand Hand) { return Hands[Hand == EPoseHand::Left ? 0 : 1]; }

	void UpdateHand(EPoseHand Hand, const FPoseHandSample& Sample, bool bPersonPresent, double Time);
	void CheckHoldingTimeout(EPoseHand Hand, double Now);
	void SetHandState(EPoseHand Hand, EPoseHandState NewState, double Time, FString Reason);
	void SetGlobalState(EPoseGlobalState NewState, double Time, FString Reason);

	FPoseStateMachineSettings Settings;

	EPoseGlobalState GlobalState = EPoseGlobalState::Idle;
	double GlobalSince = 0.0;
	FString GlobalReason = TEXT("initial");

	FHand Hands[2];

	double LastFrameTime = -1.0;
	double StreakStart = -1.0;

	TArray<FPoseStateTransition> PendingTransitions;
};
