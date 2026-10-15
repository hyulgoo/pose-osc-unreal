#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PoseInteractionStateMachine.h"
#include "PoseOscTypes.h"

#include "PoseInteractionComponent.generated.h"

class UPoseOscReceiverComponent;

/**
 * Drives FPoseInteractionStateMachine from the receiver on the same actor: each tick it feeds the newest frame
 * (if any) and the current monotonic time, applies D3 rule 1 (seq baseline reset on Idle entry) and shows the
 * states with their transition reasons on screen. State logic lives in C++ only; Blueprint reads it through the
 * BlueprintPure getters.
 */
UCLASS(ClassGroup = (PoseOsc), meta = (BlueprintSpawnableComponent))
class POSEOSCUNREAL_API UPoseInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPoseInteractionComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	EPoseGlobalState GetGlobalState() const;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	EPoseHandState GetHandState(EPoseHand Hand) const;

	/** Normalized target (x, y in [0, 1], z relative depth); held while Holding. */
	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	FVector GetHandTargetNormalized(EPoseHand Hand) const;

	const FPoseInteractionStateMachine& GetStateMachine() const { return StateMachine; }

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void HandleTransitions();
	void DrawDebug(double Now) const;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|State Machine")
	FPoseStateMachineSettings Settings;

	/** On-screen state display (non-shipping builds). */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Debug")
	bool bShowDebug = true;

	UPROPERTY(EditAnywhere, Category = "PoseOsc|Debug")
	bool bLogTransitions = true;

	UPROPERTY(Transient)
	TObjectPtr<UPoseOscReceiverComponent> Receiver;

	FPoseInteractionStateMachine StateMachine;
};
