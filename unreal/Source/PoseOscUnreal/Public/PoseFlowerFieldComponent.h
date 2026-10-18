#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"

#include "PoseFlowerFieldComponent.generated.h"

class UBoxComponent;
class UMaterialInterface;
class UPoseNiagaraDriverComponent;
class UStaticMesh;

/** Where the flowers grow: a jittered grid over the interaction volume face (D14). */
USTRUCT(BlueprintType)
struct FPoseFlowerLayoutSettings
{
	GENERATED_BODY()

	/** Field size relative to the volume face (local Y/Z). Above 1 fills the camera view beyond the hands' reach. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "0.1"))
	float FieldScale = 1.6f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "1", ClampMax = "64"))
	int32 Columns = 20;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "1", ClampMax = "64"))
	int32 Rows = 11;

	/** Random offset inside each grid cell, as a fraction of the cell (0 = regular grid, below 1 stays in the cell). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "0", ClampMax = "0.9"))
	float Jitter = 0.8f;

	/** Random offset along the viewing axis (local X) for parallax. Kept small because bloom ignores depth. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "0", Units = "Centimeters"))
	float DepthJitter = 15.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "0.1"))
	float SizeMin = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "0.1"))
	float SizeMax = 1.3f;

	/** Maximum tilt of a flower's axis away from the viewer, so some buds show their side. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout", meta = (ClampMin = "0", ClampMax = "80", Units = "Degrees"))
	float TiltMaxDegrees = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	int32 Seed = 2026;
};

/** One flower's fixed placement in volume local space (viewer on -X, D7). */
struct FPoseFlowerSeed
{
	FVector Location = FVector::ZeroVector;
	float Size = 1.f;
	float TiltRadians = 0.f;   // Axis tilt away from -X.
	float TiltDirection = 0.f; // Which way the axis tilts, around -X.
	float Spin = 0.f;          // Petal ring rotation.
	float Phase = 0.f;         // Breathing / sway phase.
	float ColorPick = 0.f;     // [0, 1) -> palette entry.
};

namespace PoseFlower
{
	/** Columns x Rows seeds inside the field; the same settings always give the same field. */
	TArray<FPoseFlowerSeed> MakeLayout(const FPoseFlowerLayoutSettings& Settings, const FVector& VolumeExtent);
}

/**
 * Presentation layer next to the Niagara driver: a field of glowing flowers on the volume face that open while a
 * wrist is near them and close after it leaves. It reads only the driver's smoothed hand positions and crossfaded
 * weights, so the D6 fallbacks carry over without state logic here (Holding keeps nearby flowers partly open,
 * Lost / Idle close them). Petals and cores are instances of one mesh; color and intensity go to per-instance
 * custom data read by M_PoseFlower (unreal/Scripts/create_flower_material.py).
 */
UCLASS(ClassGroup = (PoseOsc), Config = Game, meta = (BlueprintSpawnableComponent))
class POSEOSCUNREAL_API UPoseFlowerFieldComponent : public UInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	UPoseFlowerFieldComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	int32 GetFlowerCount() const { return Flowers.Num(); }

	/** 0 = closed bud, 1 = fully open; 0 for an invalid index. */
	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	float GetFlowerBloom(int32 FlowerIndex) const;

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	struct FFlower
	{
		FPoseFlowerSeed Seed;
		FLinearColor Color = FLinearColor::White;
		FQuat Facing = FQuat::Identity; // Turns the -X axis toward the view point.
		float Bloom = 0.f;
	};

	bool ResolveAssets();
	void BuildField();
	void UpdateBlooms(double DeltaSeconds);
	void ComputeInstances();
	void SetInstanceLight(int32 InstanceIndex, const FLinearColor& Color, float Intensity);
	void DrawBloomDebug() const;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Layout")
	FPoseFlowerLayoutSettings Layout;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Layout", meta = (ClampMin = "3", ClampMax = "12"))
	int32 PetalCount = 6;

	/** Petal length of a fully open flower at size 1. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Layout", meta = (ClampMin = "1", Units = "Centimeters"))
	float PetalLength = 14.f;

	/** Flowers within this distance of a wrist (on the volume face) open fully. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Bloom", meta = (ClampMin = "0", Units = "Centimeters"))
	float BloomInnerRadius = 20.f;

	/** Beyond this distance a wrist has no effect; smoothstep in between. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Bloom", meta = (ClampMin = "0", Units = "Centimeters"))
	float BloomOuterRadius = 55.f;

	/** Opening time constant. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Bloom", meta = (ClampMin = "0", Units = "Seconds"))
	float BloomRiseTau = 0.25f;

	/** Closing time constant. Slower than opening so a moving wrist leaves a trail of flowers. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Bloom", meta = (ClampMin = "0", Units = "Seconds"))
	float BloomFallTau = 1.2f;

	/** Open petal colors (linear); each flower picks one. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look")
	TArray<FLinearColor> Palette;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look")
	FLinearColor BudColor = FLinearColor(0.04f, 0.12f, 0.32f);

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look")
	FLinearColor CoreColor = FLinearColor(1.f, 0.8f, 0.45f);

	/** Per petal; closed petals overlap and add up (additive material). */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0"))
	float BudIntensity = 0.3f;

	/** Above ~2 the tonemapper washes the palette out toward white. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0"))
	float BloomIntensity = 1.5f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0"))
	float CoreIntensity = 3.f;

	/** Bud brightness swing (0..1) so the closed field looks alive. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0", ClampMax = "1"))
	float BreathAmount = 0.4f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0.1", Units = "Seconds"))
	float BreathPeriod = 4.f;

	/** Petal angle from the flower axis when closed / open. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0", ClampMax = "90", Units = "Degrees"))
	float ClosedAngle = 15.f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0", ClampMax = "120", Units = "Degrees"))
	float OpenAngle = 75.f;

	/** Petal ring rotation while opening. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (Units = "Degrees"))
	float OpenTwist = 30.f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Look", meta = (ClampMin = "0", Units = "Degrees"))
	float SwayAngle = 3.f;

	/** Config=Game (DefaultGame.ini). Without it the flowers use the mesh's own material and do not glow. */
	UPROPERTY(EditAnywhere, Config, Category = "PoseOsc|Assets")
	TSoftObjectPtr<UMaterialInterface> FlowerMaterial;

	/** Stretched into petals; any mesh works (scale follows its bounds). */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Assets")
	TSoftObjectPtr<UStaticMesh> PetalMesh;

	/** Bloom radii around each weighted hand on the volume face. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Debug")
	bool bDrawBloomDebug = false;

	UPROPERTY(Transient)
	TObjectPtr<UPoseNiagaraDriverComponent> Driver;

	UPROPERTY(Transient)
	TObjectPtr<UBoxComponent> Volume;

	TArray<FFlower> Flowers;
	TArray<FTransform> InstanceTransforms;
	TArray<float> InstanceCustomData;
	FVector MeshSize = FVector(100.0);
	int32 InstancesPerFlower = 0;
	double ElapsedSeconds = 0.0;
	bool bFieldReady = false;
};
