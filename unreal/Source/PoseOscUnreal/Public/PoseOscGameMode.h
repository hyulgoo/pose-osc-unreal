#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "PoseOscGameMode.generated.h"

/**
 * Spawns an APoseInteractionActor when the level has none, so the pipeline runs on an empty map. No default pawn:
 * the interaction actor's camera is the view (a free-flying pawn would only get in the way of the installation).
 */
UCLASS()
class POSEOSCUNREAL_API APoseOscGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	APoseOscGameMode();

	virtual void StartPlay() override;

private:
	UPROPERTY(EditDefaultsOnly, Category = "PoseOsc")
	bool bSpawnInteractionActorIfMissing = true;
};
