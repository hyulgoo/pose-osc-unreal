#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "PoseOscGameMode.generated.h"

/** Spawns an APoseInteractionActor when the level has none, so the pipeline runs on an empty map. */
UCLASS()
class POSEOSCUNREAL_API APoseOscGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	virtual void StartPlay() override;

private:
	UPROPERTY(EditDefaultsOnly, Category = "PoseOsc")
	bool bSpawnInteractionActorIfMissing = true;
};
