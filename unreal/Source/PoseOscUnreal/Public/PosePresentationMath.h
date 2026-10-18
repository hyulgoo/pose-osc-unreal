#pragma once

#include "CoreMinimal.h"
#include "PoseOscTypes.h"

#include "PosePresentationMath.generated.h"

/** Normalized pose space -> interaction volume local space (D7). */
USTRUCT(BlueprintType)
struct FPoseVolumeMappingSettings
{
	GENERATED_BODY()

	/**
	 * How much MediaPipe's relative depth z moves the target along the volume's local X (viewing direction).
	 * 0 = flat plane through the volume center (D3/D7: z is noisy, start at 0).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mapping", meta = (ClampMin = "0", ClampMax = "1"))
	float DepthWeight = 0.f;

	/** z value that maps to the volume's X extent before DepthWeight (MediaPipe z is roughly within +-0.5). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mapping", meta = (ClampMin = "0.01"))
	float DepthRange = 0.5f;
};

/** Pure functions for the presentation layer, unit-tested in Private/Tests/PoseOscSpecs.cpp. */
namespace PosePresentation
{
	/**
	 * Volume local axes (viewer looks along +X, D7): normalized x (0 = left of the mirrored view) -> local Y,
	 * normalized y (0 = top) -> local Z (inverted), z (negative = closer to the camera) -> local X.
	 */
	inline FVector NormalizedToVolumeLocal(const FVector& Normalized, const FVector& BoxExtent, const FPoseVolumeMappingSettings& Settings)
	{
		const double Depth = FMath::Clamp(Normalized.Z / FMath::Max(Settings.DepthRange, 0.01f), -1.0, 1.0) * Settings.DepthWeight;
		return FVector(
			Depth * BoxExtent.X,
			(FMath::Clamp(Normalized.X, 0.0, 1.0) - 0.5) * 2.0 * BoxExtent.Y,
			(0.5 - FMath::Clamp(Normalized.Y, 0.0, 1.0)) * 2.0 * BoxExtent.Z);
	}

	/**
	 * Frame-rate independent exponential smoothing factor: applying it twice with Dt equals once with 2 * Dt.
	 * (FMath::VInterpTo uses Clamp(Dt * Speed, 0, 1), which is not frame-rate independent; D4.)
	 */
	inline double ExpAlpha(double DeltaSeconds, double TauSeconds)
	{
		if (TauSeconds <= 0.0)
		{
			return 1.0;
		}
		return 1.0 - FMath::Exp(-FMath::Max(DeltaSeconds, 0.0) / TauSeconds);
	}

	/** Moves Current toward Target with separate time constants for rising and falling. */
	inline float ApproachWeight(float Current, float Target, double DeltaSeconds, double RiseTau, double FallTau)
	{
		const double Alpha = ExpAlpha(DeltaSeconds, Target > Current ? RiseTau : FallTau);
		const float Next = static_cast<float>(Current + (Target - Current) * Alpha);
		// Snap the last bit so weights actually reach 0 / 1 (Niagara spawn rates scale with them).
		return FMath::Abs(Target - Next) < 1.e-3f ? Target : Next;
	}

	/** Crossfade target for a hand state (D6: Tracking shows the hand, Lost hands the area to the default effect). */
	inline float HandWeightTarget(EPoseHandState State, float HoldingWeight)
	{
		switch (State)
		{
		case EPoseHandState::Tracking: return 1.f;
		case EPoseHandState::Holding: return FMath::Clamp(HoldingWeight, 0.f, 1.f);
		case EPoseHandState::Lost: return 0.f;
		}
		return 0.f;
	}

	/**
	 * How much a hand at Distance opens a flower (D14): 1 up to InnerRadius, smoothstep down to 0 at OuterRadius.
	 * OuterRadius <= InnerRadius gives a hard edge at InnerRadius.
	 */
	inline float BloomInfluence(double Distance, double InnerRadius, double OuterRadius)
	{
		if (Distance <= InnerRadius)
		{
			return 1.f;
		}
		if (Distance >= OuterRadius)
		{
			return 0.f;
		}
		const double T = (Distance - InnerRadius) / (OuterRadius - InnerRadius);
		return static_cast<float>(1.0 - T * T * (3.0 - 2.0 * T));
	}
}
