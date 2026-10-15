#include "PoseInteractionComponent.h"

#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "PoseOscReceiverComponent.h"
#include "PoseOscUnreal.h"

namespace PoseInteractionPrivate
{
	FColor HandColor(EPoseHandState State)
	{
		switch (State)
		{
		case EPoseHandState::Tracking: return FColor::Green;
		case EPoseHandState::Holding: return FColor::Yellow;
		case EPoseHandState::Lost: return FColor::Red;
		}
		return FColor::White;
	}

	const TCHAR* HandStateText(EPoseHandState State)
	{
		switch (State)
		{
		case EPoseHandState::Tracking: return TEXT("TRACKING");
		case EPoseHandState::Holding: return TEXT("HOLDING");
		case EPoseHandState::Lost: return TEXT("LOST");
		}
		return TEXT("?");
	}
}

UPoseInteractionComponent::UPoseInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

EPoseGlobalState UPoseInteractionComponent::GetGlobalState() const
{
	return StateMachine.GetGlobalState();
}

EPoseHandState UPoseInteractionComponent::GetHandState(EPoseHand Hand) const
{
	return StateMachine.GetHandState(Hand);
}

FVector UPoseInteractionComponent::GetHandTargetNormalized(EPoseHand Hand) const
{
	return StateMachine.GetHandTarget(Hand);
}

void UPoseInteractionComponent::BeginPlay()
{
	Super::BeginPlay();

	StateMachine.SetSettings(Settings);

	const AActor* Owner = GetOwner();
	Receiver = Owner ? Owner->FindComponentByClass<UPoseOscReceiverComponent>() : nullptr;
	if (!Receiver)
	{
		UE_LOG(LogPoseOsc, Error, TEXT("%s: no UPoseOscReceiverComponent on the same actor; state machine stays Idle"), *GetPathName());
	}
}

#if WITH_EDITOR
void UPoseInteractionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// Live tuning: edits on the PIE instance apply immediately.
	StateMachine.SetSettings(Settings);
}
#endif

void UPoseInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Monotonic wall clock, not world time: pause and time dilation must not delay the timeout (D3).
	const double Now = FPlatformTime::Seconds();

	if (Receiver)
	{
		FPoseHandsFrame Frame;
		double ReceivedAt = 0.0;
		if (Receiver->ConsumeLatestFrame(Frame, ReceivedAt))
		{
			StateMachine.OnFrame(Frame, ReceivedAt);
		}
	}
	StateMachine.Tick(Now);
	HandleTransitions();

	if (bShowDebug)
	{
		DrawDebug(Now);
	}
}

void UPoseInteractionComponent::HandleTransitions()
{
	for (const FPoseStateTransition& Transition : StateMachine.DrainTransitions())
	{
		if (bLogTransitions)
		{
			UE_LOG(LogPoseOsc, Log, TEXT("%s"), *Transition.Describe());
		}
		if (Transition.bGlobal && Transition.GlobalTo == EPoseGlobalState::Idle && Receiver)
		{
			// D3 rule 1: after Idle the next packet starts a new seq baseline (Python restart begins at 0).
			Receiver->ResetSeqBaseline();
		}
	}
}

void UPoseInteractionComponent::DrawDebug(double Now) const
{
#if !UE_BUILD_SHIPPING
	using namespace PoseInteractionPrivate;

	if (!GEngine)
	{
		return;
	}

	const uint64 KeyBase = static_cast<uint64>(GetUniqueID()) << 4;
	constexpr float Duration = 0.5f; // Re-sent every tick with the same key; lingers briefly if ticking stops.
	const FVector2D Scale(1.2, 1.2);

	const bool bInteractive = StateMachine.GetGlobalState() == EPoseGlobalState::Interactive;
	const double LastFrame = StateMachine.GetLastFrameTime();
	const FString LastPacket = LastFrame < 0.0 ? FString(TEXT("none yet")) : FString::Printf(TEXT("%.2fs ago"), Now - LastFrame);

	FString Network = TEXT("receiver missing");
	if (Receiver)
	{
		const FPoseOscReceiverStats Stats = Receiver->GetStats();
		Network = FString::Printf(TEXT("%s %s | %.1f Hz, loss %.1f%% | accepted %d lost %d dup %d stale %d restarts %d rejected %d"),
			Receiver->IsListening() ? TEXT("listening") : TEXT("NOT LISTENING"), *Receiver->GetBindDescription(),
			Stats.AcceptedRateHz, Stats.LossRatio * 100.f, Stats.Accepted, Stats.Lost, Stats.Duplicate, Stats.Stale,
			Stats.Restarts, Stats.Rejected);
	}

	GEngine->AddOnScreenDebugMessage(KeyBase + 0, Duration, bInteractive ? FColor::Cyan : FColor(255, 140, 0),
		FString::Printf(TEXT("[PoseOsc] %s %.1fs | last packet %s | streak %.2fs | %s"),
			bInteractive ? TEXT("INTERACTIVE") : TEXT("IDLE"), StateMachine.GetGlobalStateAge(Now), *LastPacket,
			StateMachine.GetStreakSeconds(), *StateMachine.GetGlobalReason()),
		false, Scale);
	GEngine->AddOnScreenDebugMessage(KeyBase + 1, Duration, FColor::Silver, Network, false, Scale);
	for (const EPoseHand Hand : { EPoseHand::Left, EPoseHand::Right })
	{
		const EPoseHandState State = StateMachine.GetHandState(Hand);
		const FVector Target = StateMachine.GetHandTarget(Hand);
		GEngine->AddOnScreenDebugMessage(KeyBase + (Hand == EPoseHand::Left ? 2 : 3), Duration, HandColor(State),
			FString::Printf(TEXT("%s: %-8s %.1fs | conf %.2f | target (%.3f, %.3f) | %s"),
				Hand == EPoseHand::Left ? TEXT("L") : TEXT("R"), HandStateText(State), StateMachine.GetHandStateAge(Hand, Now),
				StateMachine.GetHandConfidence(Hand), Target.X, Target.Y, *StateMachine.GetHandReason(Hand)),
			false, Scale);
	}
#endif
}
