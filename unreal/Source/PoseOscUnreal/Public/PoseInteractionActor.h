#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "PoseInteractionActor.generated.h"

class UPoseInteractionComponent;
class UPoseOscReceiverComponent;

/** Drop-in actor carrying the receiver (transport) and the interaction (state machine) components. */
UCLASS()
class POSEOSCUNREAL_API APoseInteractionActor : public AActor
{
	GENERATED_BODY()

public:
	APoseInteractionActor();

	UPoseOscReceiverComponent* GetReceiver() const { return Receiver; }
	UPoseInteractionComponent* GetInteraction() const { return Interaction; }

private:
	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UPoseOscReceiverComponent> Receiver;

	UPROPERTY(VisibleAnywhere, Category = "PoseOsc")
	TObjectPtr<UPoseInteractionComponent> Interaction;
};
