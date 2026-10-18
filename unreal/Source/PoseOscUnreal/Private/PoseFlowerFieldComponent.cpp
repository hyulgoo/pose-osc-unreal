#include "PoseFlowerFieldComponent.h"

#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "Misc/App.h"
#include "PoseNiagaraDriverComponent.h"
#include "PoseOscUnreal.h"
#include "PosePresentationMath.h"

namespace PoseFlowerFieldPrivate
{
	/** Per-instance custom data read by M_PoseFlower: linear R, G, B, intensity. */
	constexpr int32 CustomDataFloats = 4;

	/** Petal proportions relative to its length (width) and width (thickness). */
	constexpr float PetalWidthRatio = 0.45f;
	constexpr float PetalThicknessRatio = 0.15f;

	/** Closed petals are shorter and relatively wider than open ones, so a bud reads as a plump shape, not a needle. */
	constexpr float ClosedLengthScale = 0.55f;
	constexpr float ClosedWidthScale = 0.7f;
}

TArray<FPoseFlowerSeed> PoseFlower::MakeLayout(const FPoseFlowerLayoutSettings& Settings, const FVector& VolumeExtent)
{
	const int32 Columns = FMath::Max(Settings.Columns, 1);
	const int32 Rows = FMath::Max(Settings.Rows, 1);
	const double HalfY = VolumeExtent.Y * Settings.FieldScale;
	const double HalfZ = VolumeExtent.Z * Settings.FieldScale;
	const double CellY = 2.0 * HalfY / Columns;
	const double CellZ = 2.0 * HalfZ / Rows;
	const float HalfJitter = 0.5f * FMath::Clamp(Settings.Jitter, 0.f, 0.9f);
	const float SizeMin = FMath::Min(Settings.SizeMin, Settings.SizeMax);
	const float SizeMax = FMath::Max(Settings.SizeMin, Settings.SizeMax);
	const float TiltMax = FMath::DegreesToRadians(FMath::Clamp(Settings.TiltMaxDegrees, 0.f, 80.f));

	FRandomStream Random(Settings.Seed);
	TArray<FPoseFlowerSeed> Seeds;
	Seeds.Reserve(Columns * Rows);
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		for (int32 Column = 0; Column < Columns; ++Column)
		{
			// One draw per statement: argument evaluation order is unspecified and would make the field compiler-dependent.
			const float OffsetY = Random.FRandRange(-HalfJitter, HalfJitter);
			const float OffsetZ = Random.FRandRange(-HalfJitter, HalfJitter);
			const float Depth = Random.FRandRange(-Settings.DepthJitter, Settings.DepthJitter);

			FPoseFlowerSeed& Seed = Seeds.AddDefaulted_GetRef();
			Seed.Location = FVector(Depth, -HalfY + (Column + 0.5 + OffsetY) * CellY, HalfZ - (Row + 0.5 + OffsetZ) * CellZ);
			Seed.Size = Random.FRandRange(SizeMin, SizeMax);
			// Square root spreads tilts evenly over the cone instead of crowding them near the axis.
			Seed.TiltRadians = TiltMax * FMath::Sqrt(Random.GetFraction());
			Seed.TiltDirection = Random.FRandRange(0.f, UE_TWO_PI);
			Seed.Spin = Random.FRandRange(0.f, UE_TWO_PI);
			Seed.Phase = Random.FRandRange(0.f, UE_TWO_PI);
			Seed.ColorPick = Random.GetFraction();
		}
	}
	return Seeds;
}

UPoseFlowerFieldComponent::UPoseFlowerFieldComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	SetMobility(EComponentMobility::Movable);
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCanEverAffectNavigation(false);
	CastShadow = false;

	PetalMesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	Palette = {
		FLinearColor(1.f, 0.15f, 0.45f),  // pink
		FLinearColor(0.85f, 0.1f, 1.f),   // magenta
		FLinearColor(0.45f, 0.2f, 1.f),   // violet
		FLinearColor(1.f, 0.35f, 0.2f),   // coral
		FLinearColor(1.f, 0.7f, 0.15f),   // gold
		FLinearColor(0.15f, 0.8f, 1.f),   // cyan
	};
}

float UPoseFlowerFieldComponent::GetFlowerBloom(int32 FlowerIndex) const
{
	return Flowers.IsValidIndex(FlowerIndex) ? Flowers[FlowerIndex].Bloom : 0.f;
}

void UPoseFlowerFieldComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	Driver = Owner ? Owner->FindComponentByClass<UPoseNiagaraDriverComponent>() : nullptr;
	Volume = Owner ? Owner->FindComponentByClass<UBoxComponent>() : nullptr;

	if (!Driver || !Volume)
	{
		UE_LOG(LogPoseOsc, Error, TEXT("%s: needs a UPoseNiagaraDriverComponent and a UBoxComponent (interaction volume) on the same actor; flower field disabled"),
			*GetPathName());
		SetComponentTickEnabled(false);
		return;
	}
	// Read the hands after the driver smoothed this frame's values.
	AddTickPrerequisiteComponent(Driver);

	if (!ResolveAssets())
	{
		SetComponentTickEnabled(false);
		return;
	}
	BuildField();
}

bool UPoseFlowerFieldComponent::ResolveAssets()
{
	UStaticMesh* Mesh = PetalMesh.LoadSynchronous();
	if (!Mesh)
	{
		UE_LOG(LogPoseOsc, Error, TEXT("%s: petal mesh %s not found; flower field disabled"), *GetPathName(), *PetalMesh.ToString());
		return false;
	}
	SetStaticMesh(Mesh);
	MeshSize = Mesh->GetBoundingBox().GetSize().ComponentMax(FVector(UE_KINDA_SMALL_NUMBER));

	if (UMaterialInterface* Material = FlowerMaterial.LoadSynchronous())
	{
		SetMaterial(0, Material);
		UE_LOG(LogPoseOsc, Display, TEXT("Flower field uses %s"), *GetNameSafe(Material));
	}
	else
	{
		UE_LOG(LogPoseOsc, Warning, TEXT("Flower material %s not found; flowers use the mesh's own material and will not glow (run unreal/Scripts/create_flower_material.py)"),
			*FlowerMaterial.ToString());
	}
	return true;
}

void UPoseFlowerFieldComponent::BuildField()
{
	using namespace PoseFlowerFieldPrivate;

	bFieldReady = false;
	ClearInstances();
	SetNumCustomDataFloats(CustomDataFloats);

	// Flowers face the actor's camera: with parallel axes, buds near the edges of the 90 degree view are seen from the
	// side and read as streaks. Without a camera they face -X (the D7 viewing direction).
	const AActor* Owner = GetOwner();
	const UCameraComponent* Camera = Owner ? Owner->FindComponentByClass<UCameraComponent>() : nullptr;
	const TOptional<FVector> ViewPoint = Camera ? TOptional<FVector>(GetComponentTransform().InverseTransformPosition(Camera->GetComponentLocation())) : TOptional<FVector>();

	const TArray<FPoseFlowerSeed> Seeds = PoseFlower::MakeLayout(Layout, Volume->GetUnscaledBoxExtent());
	Flowers.Reset(Seeds.Num());
	for (const FPoseFlowerSeed& Seed : Seeds)
	{
		FFlower& Flower = Flowers.AddDefaulted_GetRef();
		Flower.Seed = Seed;
		if (!Palette.IsEmpty())
		{
			Flower.Color = Palette[FMath::Min(FMath::FloorToInt32(Seed.ColorPick * Palette.Num()), Palette.Num() - 1)];
		}
		if (ViewPoint)
		{
			const FVector ToViewer = (*ViewPoint - Seed.Location).GetSafeNormal(UE_SMALL_NUMBER, -FVector::XAxisVector);
			Flower.Facing = FQuat::FindBetweenNormals(-FVector::XAxisVector, ToViewer);
		}
	}

	InstancesPerFlower = PetalCount + 1; // petals + core
	InstanceTransforms.SetNum(Flowers.Num() * InstancesPerFlower);
	InstanceCustomData.SetNumZeroed(InstanceTransforms.Num() * CustomDataFloats);
	if (InstanceTransforms.IsEmpty())
	{
		return;
	}

	ComputeInstances();
	AddInstances(InstanceTransforms, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false, /*bUpdateNavigation*/ false);
	SetCustomData(0, InstanceTransforms.Num() - 1, InstanceCustomData, /*bMarkRenderStateDirty*/ true);
	bFieldReady = true;

	UE_LOG(LogPoseOsc, Display, TEXT("Flower field: %d flowers x (%d petals + core) = %d instances"), Flowers.Num(), PetalCount, InstanceTransforms.Num());
}

void UPoseFlowerFieldComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bFieldReady)
	{
		return;
	}

	// Same clock as the driver: real frame time, not dilated game time (D13).
	const double DeltaSeconds = FApp::GetDeltaTime();
	ElapsedSeconds += DeltaSeconds;

	UpdateBlooms(DeltaSeconds);
	ComputeInstances();
	BatchUpdateInstancesTransforms(0, InstanceTransforms, /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
	SetCustomData(0, InstanceTransforms.Num() - 1, InstanceCustomData, /*bMarkRenderStateDirty*/ true);

	if (bDrawBloomDebug)
	{
		DrawBloomDebug();
	}
}

void UPoseFlowerFieldComponent::UpdateBlooms(double DeltaSeconds)
{
	struct FHandOnFace
	{
		FVector2D Position;
		float Weight;
	};

	// Distance is measured on the volume face (local Y/Z): the hands are mapped flat (DepthWeight 0, D7) and the
	// flowers' depth jitter is only for parallax.
	TArray<FHandOnFace, TInlineAllocator<2>> Hands;
	const FTransform& ToWorld = GetComponentTransform();
	for (const EPoseHand Hand : { EPoseHand::Left, EPoseHand::Right })
	{
		const float Weight = Driver->GetHandWeight(Hand);
		if (Weight > 0.f)
		{
			const FVector Local = ToWorld.InverseTransformPosition(Driver->GetHandWorldPosition(Hand));
			Hands.Add({ FVector2D(Local.Y, Local.Z), Weight });
		}
	}

	for (FFlower& Flower : Flowers)
	{
		const FVector2D Position(Flower.Seed.Location.Y, Flower.Seed.Location.Z);
		float Target = 0.f;
		for (const FHandOnFace& Hand : Hands)
		{
			const float Influence = PosePresentation::BloomInfluence(FVector2D::Distance(Position, Hand.Position), BloomInnerRadius, BloomOuterRadius);
			Target = FMath::Max(Target, Hand.Weight * Influence);
		}
		Flower.Bloom = PosePresentation::ApproachWeight(Flower.Bloom, Target, DeltaSeconds, BloomRiseTau, BloomFallTau);
	}
}

void UPoseFlowerFieldComponent::ComputeInstances()
{
	using namespace PoseFlowerFieldPrivate;

	const float ClosedRadians = FMath::DegreesToRadians(ClosedAngle);
	const float OpenRadians = FMath::DegreesToRadians(OpenAngle);
	const float TwistRadians = FMath::DegreesToRadians(OpenTwist);
	const float SwayRadians = FMath::DegreesToRadians(SwayAngle);
	const double BreathPhase = UE_TWO_PI * ElapsedSeconds / FMath::Max(BreathPeriod, 0.1f);

	for (int32 FlowerIndex = 0; FlowerIndex < Flowers.Num(); ++FlowerIndex)
	{
		const FFlower& Flower = Flowers[FlowerIndex];
		const FPoseFlowerSeed& Seed = Flower.Seed;
		const float Open = FMath::SmoothStep(0.f, 1.f, Flower.Bloom);
		const float Wave = static_cast<float>(FMath::Sin(BreathPhase + Seed.Phase));

		// Flower frame: axis toward the viewer, tilted by the seed plus a slow sway.
		const FVector TiltAxis(0.0, FMath::Cos(Seed.TiltDirection), FMath::Sin(Seed.TiltDirection));
		const FQuat Frame = Flower.Facing * FQuat(TiltAxis, Seed.TiltRadians + SwayRadians * Wave);
		const FVector Axis = Frame.RotateVector(-FVector::XAxisVector);
		const FVector U = Frame.RotateVector(FVector::YAxisVector);
		const FVector V = Frame.RotateVector(FVector::ZAxisVector);

		const float Size = Seed.Size * PetalLength;
		const float Length = Size * FMath::Lerp(ClosedLengthScale, 1.f, Open);
		const float Width = Size * PetalWidthRatio * FMath::Lerp(ClosedWidthScale, 1.f, Open);
		const float Theta = FMath::Lerp(ClosedRadians, OpenRadians, Open);
		const float Spin = Seed.Spin + TwistRadians * Open;
		const FVector PetalScale = FVector(Length, Width, Width * PetalThicknessRatio) / MeshSize;

		const FLinearColor PetalColor = FMath::Lerp(BudColor, Flower.Color, Open);
		const float PetalIntensity = FMath::Lerp(BudIntensity * (1.f + BreathAmount * Wave), BloomIntensity, Open);

		const int32 First = FlowerIndex * InstancesPerFlower;
		for (int32 Petal = 0; Petal < PetalCount; ++Petal)
		{
			const float Phi = Spin + UE_TWO_PI * Petal / PetalCount;
			const FVector Radial = FMath::Cos(Phi) * U + FMath::Sin(Phi) * V;
			const FVector Tangent = -FMath::Sin(Phi) * U + FMath::Cos(Phi) * V;
			const FVector Direction = FMath::Cos(Theta) * Axis + FMath::Sin(Theta) * Radial;

			// Mesh X = petal length (outward), Y = width (along the ring), Z = thickness. The base overlaps the core.
			const FQuat Rotation = FRotationMatrix::MakeFromXY(Direction, Tangent).ToQuat();
			InstanceTransforms[First + Petal] = FTransform(Rotation, Seed.Location + Direction * (0.45f * Length), PetalScale);
			SetInstanceLight(First + Petal, PetalColor, PetalIntensity);
		}

		// Core: hidden inside the bud, a warm light once open.
		const float CoreDiameter = Size * FMath::Lerp(0.2f, 0.35f, Open);
		InstanceTransforms[First + PetalCount] = FTransform(FQuat::Identity, Seed.Location + Axis * (0.1f * Length), FVector(CoreDiameter) / MeshSize);
		SetInstanceLight(First + PetalCount, CoreColor, CoreIntensity * Open);
	}
}

void UPoseFlowerFieldComponent::SetInstanceLight(int32 InstanceIndex, const FLinearColor& Color, float Intensity)
{
	float* Data = &InstanceCustomData[InstanceIndex * PoseFlowerFieldPrivate::CustomDataFloats];
	Data[0] = Color.R;
	Data[1] = Color.G;
	Data[2] = Color.B;
	Data[3] = Intensity;
}

void UPoseFlowerFieldComponent::DrawBloomDebug() const
{
#if ENABLE_DRAW_DEBUG
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FTransform& ToWorld = GetComponentTransform();
	const double Scale = ToWorld.GetMaximumAxisScale();
	for (const EPoseHand Hand : { EPoseHand::Left, EPoseHand::Right })
	{
		if (Driver->GetHandWeight(Hand) <= 0.f)
		{
			continue;
		}
		const FVector Center = Driver->GetHandWorldPosition(Hand);
		const FVector Y = ToWorld.GetUnitAxis(EAxis::Y);
		const FVector Z = ToWorld.GetUnitAxis(EAxis::Z);
		DrawDebugCircle(World, Center, BloomInnerRadius * Scale, 48, FColor::Magenta, false, -1.f, 0, 1.f, Y, Z, false);
		DrawDebugCircle(World, Center, BloomOuterRadius * Scale, 48, FColor(120, 60, 160), false, -1.f, 0, 1.f, Y, Z, false);
	}
#endif
}

#if WITH_EDITOR
void UPoseFlowerFieldComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// During PIE, layout / petal count / palette edits regrow the field; the other values are read every tick.
	const FName Member = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	const bool bRebuild = Member == GET_MEMBER_NAME_CHECKED(UPoseFlowerFieldComponent, Layout)
		|| Member == GET_MEMBER_NAME_CHECKED(UPoseFlowerFieldComponent, PetalCount)
		|| Member == GET_MEMBER_NAME_CHECKED(UPoseFlowerFieldComponent, Palette);
	if (bRebuild && bFieldReady)
	{
		BuildField();
	}
}
#endif
