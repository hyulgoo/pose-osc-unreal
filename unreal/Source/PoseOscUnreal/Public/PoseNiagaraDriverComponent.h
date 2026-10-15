#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PoseOscTypes.h"
#include "PosePresentationMath.h"

#include "PoseNiagaraDriverComponent.generated.h"

class UBoxComponent;
class UNiagaraComponent;
class UNiagaraSystem;
class UPoseInteractionComponent;

/** Niagara User parameter names (§9 proposal). The "User." prefix is optional (Niagara redirects both forms). */
USTRUCT(BlueprintType)
struct FPoseNiagaraParameterNames
{
	GENERATED_BODY()

	/** Position (or Vector) — smoothed world position of the left hand inside the volume. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Niagara")
	FName LeftHandPos = TEXT("LeftHandPos");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Niagara")
	FName RightHandPos = TEXT("RightHandPos");

	/** Float 0..1 — crossfaded hand presence (Tracking 1, Holding HoldingWeight, Lost 0). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Niagara")
	FName LeftHandWeight = TEXT("LeftHandWeight");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Niagara")
	FName RightHandWeight = TEXT("RightHandWeight");

	/** Float 0..1 — crossfaded global state (Interactive 1, Idle 0); the default effect uses 1 - this. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Niagara")
	FName InteractiveWeight = TEXT("InteractiveWeight");
};

/**
 * Presentation layer: reads the state machine (via UPoseInteractionComponent on the same actor), maps hand targets
 * into the interaction volume (D7), smooths positions frame-rate independently (D4), crossfades weights (D6) and
 * writes Niagara User parameters. No state decisions are made here.
 */
UCLASS(ClassGroup = (PoseOsc), Config = Game, meta = (BlueprintSpawnableComponent))
class POSEOSCUNREAL_API UPoseNiagaraDriverComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPoseNiagaraDriverComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	FVector GetHandWorldPosition(EPoseHand Hand) const;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	float GetHandWeight(EPoseHand Hand) const;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	float GetInteractiveWeight() const { return InteractiveWeight; }

protected:
	virtual void BeginPlay() override;

private:
	struct FHandVisual
	{
		FVector WorldPosition = FVector::ZeroVector;
		FVector TargetWorldPosition = FVector::ZeroVector;
		float Weight = 0.f;
		bool bHasPosition = false;
		EPoseHandState LastState = EPoseHandState::Lost;
	};

	const FHandVisual& GetHand(EPoseHand Hand) const { return Hands[Hand == EPoseHand::Left ? 0 : 1]; }
	void UpdateHand(EPoseHand Hand, double DeltaSeconds);
	void ResolveNiagaraSystem();
	void ValidateNiagaraParameters() const;
	void PushToNiagara() const;
	void DrawDebug() const;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Mapping")
	FPoseVolumeMappingSettings Mapping;

	/** Render interpolation time constant (D4): alpha = 1 - exp(-dt / tau). 0 = no smoothing. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Smoothing", meta = (ClampMin = "0", Units = "Seconds"))
	float PositionTau = 0.05f;

	/** Hand weight time constant when it rises (appearing). */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Crossfade", meta = (ClampMin = "0", Units = "Seconds"))
	float HandWeightRiseTau = 0.15f;

	/** Hand weight time constant when it falls (Holding / Lost fade). Slower so fallbacks read as transitions. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Crossfade", meta = (ClampMin = "0", Units = "Seconds"))
	float HandWeightFallTau = 0.4f;

	/** Hand weight while Holding (last valid position held, D6). Below 1 so the held state is visible. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Crossfade", meta = (ClampMin = "0", ClampMax = "1"))
	float HoldingWeight = 0.6f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Crossfade", meta = (ClampMin = "0", Units = "Seconds"))
	float InteractiveRiseTau = 0.2f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Crossfade", meta = (ClampMin = "0", Units = "Seconds"))
	float InteractiveFallTau = 0.5f;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Niagara")
	FPoseNiagaraParameterNames ParameterNames;

	/**
	 * Assigned to the Niagara component at BeginPlay when it has no asset, so the actor spawned by the game mode on
	 * an empty map still finds the project's system. Config=Game (DefaultGame.ini).
	 */
	UPROPERTY(EditAnywhere, Config, Category = "PoseOsc|Niagara")
	TSoftObjectPtr<UNiagaraSystem> DefaultSystem;

	/** Volume box, hand spheres (radius ~ weight, color ~ state) and raw targets. Works without a Niagara asset. */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Debug")
	bool bDrawDebug = true;

	UPROPERTY(Transient)
	TObjectPtr<UPoseInteractionComponent> Interaction;

	UPROPERTY(Transient)
	TObjectPtr<UBoxComponent> Volume;

	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> Niagara;

	FHandVisual Hands[2];
	float InteractiveWeight = 0.f;
	bool bNiagaraReady = false;
};
