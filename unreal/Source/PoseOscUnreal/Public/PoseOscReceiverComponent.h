#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PoseOscTypes.h"
#include "PoseSeqTracker.h"

#include "PoseOscReceiverComponent.generated.h"

class UOSCServer;
struct FOSCMessage;

USTRUCT(BlueprintType)
struct FPoseOscReceiverStats
{
	GENERATED_BODY()

	/** Messages delivered by the OSC plugin (before validation). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Received = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Accepted = 0;

	/** Sum of seq gaps among accepted frames. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Lost = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Duplicate = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Stale = 0;

	/** Seq regressions larger than the threshold (sender restarted without the receiver going Idle). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Restarts = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	int32 Rejected = 0;

	/** Accepted frames per second over the last ~1 s window (0 while silent). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	float AcceptedRateHz = 0.f;

	/** Lost / (accepted + lost) over the last ~1 s window. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "PoseOsc")
	float LossRatio = 0.f;
};

/**
 * Transport layer (D5 option A): owns a UOSCServer, validates /dstrict/hands, applies the seq rules (D3) and keeps
 * only the newest accepted frame (latest value, D0). It knows nothing about tracking states; the state machine
 * reads frames through ConsumeLatestFrame().
 */
UCLASS(ClassGroup = (PoseOsc), Config = Game, meta = (BlueprintSpawnableComponent))
class POSEOSCUNREAL_API UPoseOscReceiverComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPoseOscReceiverComponent();

	/**
	 * Newest frame accepted since the previous call; frames that arrived in between were overwritten.
	 * Returns false when nothing new arrived.
	 */
	bool ConsumeLatestFrame(FPoseHandsFrame& OutFrame, double& OutReceivedAtSeconds);

	/** D3 rule 1: forget the seq baseline. The state machine owner calls this when it enters Idle. */
	void ResetSeqBaseline();

	/** True when the bind probe succeeded and the server is running. */
	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	bool IsListening() const;

	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	FPoseOscReceiverStats GetStats() const { return Stats; }

	/** "address:port" actually bound (after the plugin's loopback rewrite), or an error description. */
	UFUNCTION(BlueprintPure, Category = "PoseOsc")
	FString GetBindDescription() const { return BindDescription; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void StartListening();
	void StopListening();
	void HandleOscMessage(const FOSCMessage& Message, const FString& SenderAddress, uint16 SenderPort);
	void RollRateWindow(double Now);

	/**
	 * IPv4 address to bind. The engine OSC plugin rewrites a loopback address (127.x) to the local host address
	 * (LAN IP) before binding (OSCServerReceiver.cpp SetAsLocalHostIfLoopback); see DESIGN_DECISIONS D5.
	 * Network settings are Config=Game so an installation can change them in DefaultGame.ini or with
	 * -ini:Game:[/Script/PoseOscUnreal.PoseOscReceiverComponent]:ListenAddress=... without rebuilding.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "PoseOsc|Network")
	FString ListenAddress = TEXT("0.0.0.0");

	UPROPERTY(EditAnywhere, Config, Category = "PoseOsc|Network", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 ListenPort = 8000;

	/**
	 * Only packets from these sender IPv4 addresses are dispatched (empty = any sender). With the wildcard bind
	 * above this keeps the receiver localhost-only by default (D3 change); add the inference PC's IP to split PCs.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "PoseOsc|Network")
	TArray<FString> AllowedSenderAddresses = { TEXT("127.0.0.1") };

	/** A seq regression larger than this is treated as a sender restart (D11: 30 = ~1 s at 30 Hz). */
	UPROPERTY(EditAnywhere, Category = "PoseOsc|Sequence", meta = (ClampMin = "1"))
	int32 SeqRegressThreshold = 30;

	UPROPERTY(Transient)
	TObjectPtr<UOSCServer> Server;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "PoseOsc|Stats")
	FPoseOscReceiverStats Stats;

	UPROPERTY(VisibleInstanceOnly, Transient, Category = "PoseOsc|Stats")
	FString BindDescription;

	FDelegateHandle MessageHandle;
	FPoseSeqTracker SeqTracker;

	FPoseHandsFrame LatestFrame;
	double LatestReceivedAt = -1.0;
	bool bHasNewFrame = false;

	double RateWindowStart = 0.0;
	int32 RateWindowAccepted = 0;
	int32 RateWindowLost = 0;
	double LastRejectLogAt = -1.0e9;
};
