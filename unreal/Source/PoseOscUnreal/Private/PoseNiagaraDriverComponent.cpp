#include "PoseNiagaraDriverComponent.h"

#include "Components/BoxComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "PoseInteractionComponent.h"
#include "PoseInteractionStateMachine.h"
#include "PoseOscUnreal.h"

namespace PoseNiagaraDriverPrivate
{
	FColor StateColor(EPoseHandState State)
	{
		switch (State)
		{
		case EPoseHandState::Tracking: return FColor::Green;
		case EPoseHandState::Holding: return FColor::Yellow;
		case EPoseHandState::Lost: return FColor::Red;
		}
		return FColor::White;
	}
}

UPoseNiagaraDriverComponent::UPoseNiagaraDriverComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

FVector UPoseNiagaraDriverComponent::GetHandWorldPosition(EPoseHand Hand) const
{
	return GetHand(Hand).WorldPosition;
}

float UPoseNiagaraDriverComponent::GetHandWeight(EPoseHand Hand) const
{
	return GetHand(Hand).Weight;
}

void UPoseNiagaraDriverComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	Interaction = Owner ? Owner->FindComponentByClass<UPoseInteractionComponent>() : nullptr;
	Volume = Owner ? Owner->FindComponentByClass<UBoxComponent>() : nullptr;
	Niagara = Owner ? Owner->FindComponentByClass<UNiagaraComponent>() : nullptr;

	if (!Interaction || !Volume)
	{
		UE_LOG(LogPoseOsc, Error, TEXT("%s: needs a UPoseInteractionComponent and a UBoxComponent (interaction volume) on the same actor; driver disabled"),
			*GetPathName());
		SetComponentTickEnabled(false);
		return;
	}
	// Read the state machine after it consumed this frame's packet.
	AddTickPrerequisiteComponent(Interaction);

	ResolveNiagaraSystem();
}

void UPoseNiagaraDriverComponent::ResolveNiagaraSystem()
{
	bNiagaraReady = false;
	if (!Niagara)
	{
		UE_LOG(LogPoseOsc, Display, TEXT("%s: no UNiagaraComponent; debug drawing only"), *GetPathName());
		return;
	}

	if (!Niagara->GetAsset() && !DefaultSystem.IsNull())
	{
		if (UNiagaraSystem* System = DefaultSystem.LoadSynchronous())
		{
			Niagara->SetAsset(System);
			Niagara->Activate(true);
		}
		else
		{
			UE_LOG(LogPoseOsc, Display, TEXT("Niagara system %s not found (not created yet?); debug drawing only"), *DefaultSystem.ToString());
		}
	}

	if (Niagara->GetAsset())
	{
		ValidateNiagaraParameters();
		bNiagaraReady = true;
		UE_LOG(LogPoseOsc, Display, TEXT("Driving Niagara system %s"), *GetNameSafe(Niagara->GetAsset()));
	}
}

void UPoseNiagaraDriverComponent::ValidateNiagaraParameters() const
{
	TArray<FNiagaraVariable> UserParameters;
	Niagara->GetAsset()->GetExposedParameters().GetUserParameters(UserParameters);

	const auto Check = [&UserParameters](FName Name, bool bPosition)
	{
		// GetUserParameters returns names without the "User." prefix.
		const FName Plain = Name.ToString().StartsWith(TEXT("User.")) ? FName(*Name.ToString().RightChop(5)) : Name;
		const FNiagaraVariable* Found = UserParameters.FindByPredicate([Plain](const FNiagaraVariable& Var) { return Var.GetName() == Plain; });
		if (!Found)
		{
			UE_LOG(LogPoseOsc, Warning, TEXT("Niagara User parameter '%s' is missing; the system will not react to it"), *Plain.ToString());
			return;
		}
		const FNiagaraTypeDefinition& Type = Found->GetType();
		const bool bTypeOk = bPosition
			? (Type == FNiagaraTypeDefinition::GetPositionDef() || Type == FNiagaraTypeDefinition::GetVec3Def())
			: Type == FNiagaraTypeDefinition::GetFloatDef();
		if (!bTypeOk)
		{
			UE_LOG(LogPoseOsc, Warning, TEXT("Niagara User parameter '%s' has type %s; expected %s"),
				*Plain.ToString(), *Type.GetName(), bPosition ? TEXT("Position (or Vector)") : TEXT("Float"));
		}
	};

	Check(ParameterNames.LeftHandPos, true);
	Check(ParameterNames.RightHandPos, true);
	Check(ParameterNames.LeftHandWeight, false);
	Check(ParameterNames.RightHandWeight, false);
	Check(ParameterNames.InteractiveWeight, false);
}

void UPoseNiagaraDriverComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Visual smoothing uses the real (undilated) frame time; state timeouts already use the monotonic clock.
	const double DeltaSeconds = FApp::GetDeltaTime();

	UpdateHand(EPoseHand::Left, DeltaSeconds);
	UpdateHand(EPoseHand::Right, DeltaSeconds);

	const bool bInteractive = Interaction->GetGlobalState() == EPoseGlobalState::Interactive;
	InteractiveWeight = PosePresentation::ApproachWeight(InteractiveWeight, bInteractive ? 1.f : 0.f, DeltaSeconds, InteractiveRiseTau, InteractiveFallTau);

	if (bNiagaraReady)
	{
		PushToNiagara();
	}
	if (bDrawDebug)
	{
		DrawDebug();
	}
}

void UPoseNiagaraDriverComponent::UpdateHand(EPoseHand Hand, double DeltaSeconds)
{
	FHandVisual& H = Hands[Hand == EPoseHand::Left ? 0 : 1];
	const EPoseHandState State = Interaction->GetHandState(Hand);

	const FVector Local = PosePresentation::NormalizedToVolumeLocal(Interaction->GetHandTargetNormalized(Hand), Volume->GetUnscaledBoxExtent(), Mapping);
	H.TargetWorldPosition = Volume->GetComponentTransform().TransformPosition(Local);

	// Re-acquisition snaps instead of sweeping from where the hand was lost (§8 #4); the Python filter resets too.
	const bool bReacquired = State == EPoseHandState::Tracking && H.LastState == EPoseHandState::Lost;
	if (!H.bHasPosition || bReacquired)
	{
		H.WorldPosition = H.TargetWorldPosition;
		H.bHasPosition = true;
	}
	else if (State != EPoseHandState::Lost)
	{
		H.WorldPosition = FMath::Lerp(H.WorldPosition, H.TargetWorldPosition, PosePresentation::ExpAlpha(DeltaSeconds, PositionTau));
	}
	// Lost: keep the last position while the weight fades out.

	H.Weight = PosePresentation::ApproachWeight(H.Weight, PosePresentation::HandWeightTarget(State, HoldingWeight), DeltaSeconds,
		HandWeightRiseTau, HandWeightFallTau);
	H.LastState = State;
}

void UPoseNiagaraDriverComponent::PushToNiagara() const
{
	// SetVariableVec3 forwards to SetVariablePosition when the User parameter is a Position (NiagaraComponent.cpp).
	Niagara->SetVariableVec3(ParameterNames.LeftHandPos, Hands[0].WorldPosition);
	Niagara->SetVariableVec3(ParameterNames.RightHandPos, Hands[1].WorldPosition);
	Niagara->SetVariableFloat(ParameterNames.LeftHandWeight, Hands[0].Weight);
	Niagara->SetVariableFloat(ParameterNames.RightHandWeight, Hands[1].Weight);
	Niagara->SetVariableFloat(ParameterNames.InteractiveWeight, InteractiveWeight);
}

void UPoseNiagaraDriverComponent::DrawDebug() const
{
#if ENABLE_DRAW_DEBUG
	using namespace PoseNiagaraDriverPrivate;

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Box color follows the crossfaded global weight: dim gray (Idle) -> cyan (Interactive). A hue lerp from orange
	// passed through green mid-fade and read as "Tracking", so the fade only changes saturation and brightness.
	const FColor BoxColor = FMath::Lerp(FLinearColor(0.25f, 0.25f, 0.25f), FLinearColor(0.f, 0.9f, 1.f), InteractiveWeight).ToFColor(true);
	DrawDebugBox(World, Volume->GetComponentLocation(), Volume->GetScaledBoxExtent(), Volume->GetComponentQuat(), BoxColor, false, -1.f, 0, 1.5f);

	for (const EPoseHand Hand : { EPoseHand::Left, EPoseHand::Right })
	{
		const FHandVisual& H = GetHand(Hand);
		if (!H.bHasPosition)
		{
			continue;
		}
		// Sphere: smoothed position, radius ~ crossfaded weight, color ~ state. Point: raw (unsmoothed) target.
		DrawDebugSphere(World, H.WorldPosition, 3.f + 12.f * H.Weight, 16, StateColor(Interaction->GetHandState(Hand)), false, -1.f, 0, 1.f);
		DrawDebugPoint(World, H.TargetWorldPosition, 6.f, FColor::White, false, -1.f);
	}
#endif
}
