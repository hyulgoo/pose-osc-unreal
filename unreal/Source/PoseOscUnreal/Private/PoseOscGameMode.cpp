#include "PoseOscGameMode.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "PoseInteractionActor.h"
#include "PoseOscUnreal.h"

void APoseOscGameMode::StartPlay()
{
	Super::StartPlay();

	UWorld* World = GetWorld();
	if (!bSpawnInteractionActorIfMissing || !World)
	{
		return;
	}
	if (TActorIterator<APoseInteractionActor>(World))
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const APoseInteractionActor* Spawned = World->SpawnActor<APoseInteractionActor>(Params);
	UE_LOG(LogPoseOsc, Display, TEXT("No APoseInteractionActor in %s; spawned %s"), *World->GetMapName(), *GetNameSafe(Spawned));
}
