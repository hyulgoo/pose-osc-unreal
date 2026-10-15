#include "PoseInteractionActor.h"

#include "Components/SceneComponent.h"
#include "PoseInteractionComponent.h"
#include "PoseOscReceiverComponent.h"

APoseInteractionActor::APoseInteractionActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Receiver = CreateDefaultSubobject<UPoseOscReceiverComponent>(TEXT("OscReceiver"));
	Interaction = CreateDefaultSubobject<UPoseInteractionComponent>(TEXT("Interaction"));
}
