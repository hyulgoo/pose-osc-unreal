#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PoseInteractionActor.generated.h"

class UBoxComponent;
class UCameraComponent;
class UNiagaraComponent;
class UPoseFlowerFieldComponent;
class UPoseInteractionComponent;
class UPoseNiagaraDriverComponent;
class UPoseOscReceiverComponent;

/**
 * Drop-in actor: the root box is the interaction volume (D7), so moving / rotating / scaling the actor places the
 * mapped space in the level. Carries the transport (receiver), interpretation (state machine) and presentation
 * (Niagara driver, flower field) components, plus an optional view camera looking along the volume's +X.
 */
UCLASS()
class POSEOSCUNREAL_API APoseInteractionActor : public AActor
{
	GENERATED_BODY()

public:
	APoseInteractionActor();

	UPoseOscReceiverComponent* GetReceiver() const { return Receiver; }
	UPoseInteractionComponent* GetInteraction() const { return Interaction; }
	UPoseNiagaraDriverComponent* GetDriver() const { return Driver; }
	UPoseFlowerFieldComponent* GetFlowers() const { return Flowers; }

protected:
	virtual void BeginPlay() override;

private:
	void TakeOverView();

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UBoxComponent> InteractionVolume;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UCameraComponent> ViewCamera;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UNiagaraComponent> Effects;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UPoseOscReceiverComponent> Receiver;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UPoseInteractionComponent> Interaction;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UPoseNiagaraDriverComponent> Driver;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UPoseFlowerFieldComponent> Flowers;

	/** Make this actor's camera the first player's view target at BeginPlay (turn off to use a level camera). */
	UPROPERTY(EditAnywhere, Category = "PoseOsc")
	bool bUseAsViewTarget = true;
};
