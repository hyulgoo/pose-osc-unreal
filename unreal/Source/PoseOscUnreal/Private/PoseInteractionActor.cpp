#include "PoseInteractionActor.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NiagaraComponent.h"
#include "PoseFlowerFieldComponent.h"
#include "PoseInteractionComponent.h"
#include "PoseNiagaraDriverComponent.h"
#include "PoseOscReceiverComponent.h"
#include "TimerManager.h"

APoseInteractionActor::APoseInteractionActor()
{
	PrimaryActorTick.bCanEverTick = false;

	// 320 x 100 x 180 cm (16:9 face toward the viewer). Scale the actor to resize the mapped space.
	InteractionVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("InteractionVolume"));
	InteractionVolume->InitBoxExtent(FVector(50.0, 160.0, 90.0));
	InteractionVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	InteractionVolume->SetCanEverAffectNavigation(false);
	SetRootComponent(InteractionVolume);

	// Viewer stands on -X looking along +X (D7), far enough that the volume face fills most of a 90 degree view.
	ViewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ViewCamera"));
	ViewCamera->SetupAttachment(InteractionVolume);
	ViewCamera->SetRelativeLocation(FVector(-260.0, 0.0, 0.0));
	ViewCamera->SetFieldOfView(90.f);
	// The empty map is black; auto exposure would brighten emissive particles until they clip. Fix the exposure.
	FPostProcessSettings& PostProcess = ViewCamera->PostProcessSettings;
	PostProcess.bOverride_AutoExposureMethod = true;
	PostProcess.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	PostProcess.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	PostProcess.AutoExposureApplyPhysicalCameraExposure = false;
	PostProcess.bOverride_AutoExposureBias = true;
	PostProcess.AutoExposureBias = 0.f;
	ViewCamera->PostProcessBlendWeight = 1.f;

	Effects = CreateDefaultSubobject<UNiagaraComponent>(TEXT("Effects"));
	Effects->SetupAttachment(InteractionVolume);
	Effects->SetAutoActivate(true);

	// Flowers grow on the volume face; their local space is the volume's (D14).
	Flowers = CreateDefaultSubobject<UPoseFlowerFieldComponent>(TEXT("Flowers"));
	Flowers->SetupAttachment(InteractionVolume);

	Receiver = CreateDefaultSubobject<UPoseOscReceiverComponent>(TEXT("OscReceiver"));
	Interaction = CreateDefaultSubobject<UPoseInteractionComponent>(TEXT("Interaction"));
	Driver = CreateDefaultSubobject<UPoseNiagaraDriverComponent>(TEXT("NiagaraDriver"));
}

void APoseInteractionActor::BeginPlay()
{
	Super::BeginPlay();

	if (bUseAsViewTarget)
	{
		TakeOverView();
		// A player controller possessing a pawn later in the same frame resets its view target; claim it once more.
		GetWorldTimerManager().SetTimerForNextTick(this, &APoseInteractionActor::TakeOverView);
	}
}

void APoseInteractionActor::TakeOverView()
{
	if (APlayerController* Controller = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (Controller->GetViewTarget() != this)
		{
			Controller->SetViewTarget(this);
		}
	}
}
