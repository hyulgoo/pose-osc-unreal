#pragma once

#include "CoreMinimal.h"

#include "PoseOscTypes.generated.h"

/** OSC schema /dstrict/hands (docs/DESIGN_DECISIONS.md D3). */
namespace PoseOsc
{
	inline const TCHAR* const MessageAddress = TEXT("/dstrict/hands");
	inline constexpr int32 MessageArgCount = 10; // ,iiffffffff
}

UENUM(BlueprintType)
enum class EPoseHand : uint8
{
	Left,
	Right,
};

/** Per-hand state (D6). */
UENUM(BlueprintType)
enum class EPoseHandState : uint8
{
	Tracking UMETA(ToolTip = "Confidence >= tracking threshold; target position follows the hand."),
	Holding UMETA(ToolTip = "Confidence < holding threshold; last valid position is held."),
	Lost UMETA(ToolTip = "Holding lasted too long (or Idle); this hand shows the default effect."),
};

/** Global state (D6). */
UENUM(BlueprintType)
enum class EPoseGlobalState : uint8
{
	Idle UMETA(ToolTip = "No packets for the idle timeout; non-interactive mode."),
	Interactive UMETA(ToolTip = "Packets arrived continuously for the enter streak."),
};

USTRUCT(BlueprintType)
struct FPoseHandSample
{
	GENERATED_BODY()

	/** Normalized: x, y in [0, 1] (x mirrored on the Python side, y = 0 at the top), z = MediaPipe relative depth. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	float Confidence = 0.f;
};

/** One decoded /dstrict/hands message. */
USTRUCT(BlueprintType)
struct FPoseHandsFrame
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Seq = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	bool bPersonPresent = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	FPoseHandSample Left;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	FPoseHandSample Right;

	const FPoseHandSample& GetHand(EPoseHand Hand) const
	{
		return Hand == EPoseHand::Left ? Left : Right;
	}
};
