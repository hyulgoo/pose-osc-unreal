#include "PoseOscReceiverComponent.h"

#include "HAL/PlatformTime.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "IPAddress.h"
#include "OSCManager.h"
#include "OSCMessage.h"
#include "OSCServer.h"
#include "PoseOscMessageDecoder.h"
#include "PoseOscUnreal.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/UObjectGlobals.h"

namespace PoseOscReceiverPrivate
{
	constexpr double RateWindowSeconds = 1.0;
	constexpr double RejectLogIntervalSeconds = 2.0;

	/** Same rewrite the OSC plugin applies before binding (OSCServerReceiver.cpp, SetAsLocalHostIfLoopback). */
	FIPv4Endpoint ResolveEffectiveBindEndpoint(const FIPv4Address& Requested, int32 Port)
	{
		FIPv4Endpoint Endpoint(Requested, static_cast<uint16>(Port));
		if (Requested.IsLoopbackAddress())
		{
			if (ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				bool bCanBindAll = false;
				const TSharedRef<FInternetAddr> LocalHost = SocketSubsystem->GetLocalHostAddr(*GLog, bCanBindAll);
				uint32 LocalIp = 0;
				LocalHost->GetIp(LocalIp);
				Endpoint.Address = FIPv4Address(LocalIp);
			}
		}
		return Endpoint;
	}

	/**
	 * UOSCServer::IsActive() stays true even when the plugin fails to bind (FServerReceiver::Launch returns a
	 * receiver without a socket), so a port conflict would otherwise only show up as a LogOSC warning.
	 */
	bool ProbeUdpBind(const FIPv4Endpoint& Endpoint, FString& OutError)
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			OutError = TEXT("no socket subsystem");
			return false;
		}

		FUniqueSocket Socket = SocketSubsystem->CreateUniqueSocket(NAME_DGram, TEXT("PoseOscBindProbe"), FNetworkProtocolTypes::IPv4);
		if (!Socket)
		{
			OutError = TEXT("socket creation failed");
			return false;
		}

		Socket->SetReuseAddr(false);
		const bool bBound = Socket->Bind(*Endpoint.ToInternetAddr());
		if (!bBound)
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			OutError = FString::Printf(TEXT("%s (%d)"), SocketSubsystem->GetSocketError(Error), static_cast<int32>(Error));
		}
		Socket->Close();
		return bBound;
	}
}

UPoseOscReceiverComponent::UPoseOscReceiverComponent()
{
	// Ticks only to roll the rate window; frames are pushed by the OSC delegate.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;
}

bool UPoseOscReceiverComponent::ConsumeLatestFrame(FPoseHandsFrame& OutFrame, double& OutReceivedAtSeconds)
{
	if (!bHasNewFrame)
	{
		return false;
	}
	OutFrame = LatestFrame;
	OutReceivedAtSeconds = LatestReceivedAt;
	bHasNewFrame = false;
	return true;
}

void UPoseOscReceiverComponent::ResetSeqBaseline()
{
	SeqTracker.ResetBaseline();
}

bool UPoseOscReceiverComponent::IsListening() const
{
	return Server != nullptr && Server->IsActive();
}

void UPoseOscReceiverComponent::BeginPlay()
{
	Super::BeginPlay();

	SeqTracker.SetRegressThreshold(SeqRegressThreshold);
	RateWindowStart = FPlatformTime::Seconds();
	StartListening();
}

void UPoseOscReceiverComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopListening();
	Super::EndPlay(EndPlayReason);
}

void UPoseOscReceiverComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	RollRateWindow(FPlatformTime::Seconds());
}

void UPoseOscReceiverComponent::StartListening()
{
	using namespace PoseOscReceiverPrivate;

	FIPv4Address RequestedAddress;
	if (!FIPv4Address::Parse(ListenAddress, RequestedAddress))
	{
		BindDescription = FString::Printf(TEXT("invalid ListenAddress '%s'"), *ListenAddress);
		UE_LOG(LogPoseOsc, Error, TEXT("Receiver disabled: %s (expected IPv4, e.g. 0.0.0.0)"), *BindDescription);
		return;
	}

	const FIPv4Endpoint BindEndpoint = ResolveEffectiveBindEndpoint(RequestedAddress, ListenPort);
	if (BindEndpoint.Address != RequestedAddress)
	{
		UE_LOG(LogPoseOsc, Warning,
			TEXT("ListenAddress %s is a loopback address: the OSC plugin binds the local host address %s instead, ")
			TEXT("so packets sent to %s will not arrive (DESIGN_DECISIONS D5)."),
			*RequestedAddress.ToString(), *BindEndpoint.Address.ToString(), *RequestedAddress.ToString());
	}

	FString BindError;
	if (!ProbeUdpBind(BindEndpoint, BindError))
	{
		BindDescription = FString::Printf(TEXT("%s bind failed: %s"), *BindEndpoint.ToString(), *BindError);
		UE_LOG(LogPoseOsc, Error,
			TEXT("Receiver disabled: %s. Another process may hold the port (python tools/osc_receive_check.py, a previous PIE)."),
			*BindDescription);
		return;
	}

	const FString ServerName = MakeUniqueObjectName(this, UOSCServer::StaticClass(), TEXT("PoseOscServer")).ToString();
	Server = UOSCManager::CreateOSCServer(ListenAddress, ListenPort, /*bMulticastLoopback=*/false, /*bStartListening=*/false, ServerName, this);
	if (!Server)
	{
		BindDescription = TEXT("UOSCServer creation failed");
		UE_LOG(LogPoseOsc, Error, TEXT("Receiver disabled: %s"), *BindDescription);
		return;
	}

	TArray<FString> AppliedAllowList;
	for (const FString& Address : AllowedSenderAddresses)
	{
		FIPv4Address Parsed;
		if (FIPv4Address::Parse(Address, Parsed))
		{
			Server->AddAllowlistedClient(Address);
			AppliedAllowList.Add(Address);
		}
		else
		{
			UE_LOG(LogPoseOsc, Warning, TEXT("Ignoring invalid AllowedSenderAddresses entry '%s'"), *Address);
		}
	}
	Server->SetAllowlistClientsEnabled(AppliedAllowList.Num() > 0);

	// Native delegate: no reflection call per message (D5). Listen() only once per server: calling it twice
	// without Stop() registers the pump ticker twice (OSCServer.cpp).
	MessageHandle = Server->OnOscMessageReceivedNative.AddUObject(this, &UPoseOscReceiverComponent::HandleOscMessage);
	Server->Listen();

	BindDescription = BindEndpoint.ToString();
	UE_LOG(LogPoseOsc, Display, TEXT("Listening on %s for %s (requested %s:%d, allowed senders: %s)"),
		*BindDescription, PoseOsc::MessageAddress, *ListenAddress, ListenPort,
		AppliedAllowList.Num() > 0 ? *FString::Join(AppliedAllowList, TEXT(", ")) : TEXT("any"));
}

void UPoseOscReceiverComponent::StopListening()
{
	if (!Server)
	{
		return;
	}

	Server->OnOscMessageReceivedNative.Remove(MessageHandle);
	MessageHandle.Reset();
	// Close the socket now; waiting for GC to collect the server could keep the port bound across a fast PIE
	// restart (DESIGN_DECISIONS D5, §8 #2).
	Server->Stop();
	Server = nullptr;

	UE_LOG(LogPoseOsc, Display, TEXT("Stopped listening on %s (accepted %d, lost %d, rejected %d, restarts %d)"),
		*BindDescription, Stats.Accepted, Stats.Lost, Stats.Rejected, Stats.Restarts);
}

void UPoseOscReceiverComponent::HandleOscMessage(const FOSCMessage& Message, const FString& SenderAddress, uint16 SenderPort)
{
	using namespace PoseOscReceiverPrivate;

	// UOSCServer::PumpPacketQueue broadcasts on the game thread, so no locking is needed here.
	check(IsInGameThread());

	const double Now = FPlatformTime::Seconds();
	++Stats.Received;

	FPoseHandsFrame Frame;
	const EPoseOscDecodeResult DecodeResult = PoseOsc::DecodeHandsMessage(Message, Frame);
	if (DecodeResult != EPoseOscDecodeResult::Ok)
	{
		++Stats.Rejected;
		if (Now - LastRejectLogAt >= RejectLogIntervalSeconds)
		{
			LastRejectLogAt = Now;
			UE_LOG(LogPoseOsc, Warning, TEXT("Rejected '%s' from %s:%u: %s (rejected so far: %d)"),
				*Message.GetAddress().GetFullPath(), *SenderAddress, SenderPort, PoseOsc::LexToString(DecodeResult), Stats.Rejected);
		}
		return;
	}

	const TOptional<int32> PreviousSeq = SeqTracker.GetLastSeq();
	const FPoseSeqResult SeqResult = SeqTracker.Classify(Frame.Seq);
	switch (SeqResult.Decision)
	{
	case EPoseSeqDecision::Duplicate:
		++Stats.Duplicate;
		return;
	case EPoseSeqDecision::Stale:
		++Stats.Stale;
		return;
	case EPoseSeqDecision::Baseline:
		UE_LOG(LogPoseOsc, Log, TEXT("Seq baseline %d from %s:%u"), Frame.Seq, *SenderAddress, SenderPort);
		break;
	case EPoseSeqDecision::RestartDetected:
		++Stats.Restarts;
		UE_LOG(LogPoseOsc, Log, TEXT("Seq %d after %d: sender restart, re-baselined (D3 rule 2)"),
			Frame.Seq, PreviousSeq.Get(-1));
		break;
	default:
		break;
	}

	++Stats.Accepted;
	Stats.Lost += SeqResult.Lost;
	++RateWindowAccepted;
	RateWindowLost += SeqResult.Lost;

	// Latest value only: overwrite, never queue (D0).
	LatestFrame = Frame;
	LatestReceivedAt = Now;
	bHasNewFrame = true;
}

void UPoseOscReceiverComponent::RollRateWindow(double Now)
{
	const double Elapsed = Now - RateWindowStart;
	if (Elapsed < PoseOscReceiverPrivate::RateWindowSeconds)
	{
		return;
	}
	Stats.AcceptedRateHz = static_cast<float>(RateWindowAccepted / Elapsed);
	const int32 Total = RateWindowAccepted + RateWindowLost;
	Stats.LossRatio = Total > 0 ? static_cast<float>(RateWindowLost) / Total : 0.f;
	RateWindowAccepted = 0;
	RateWindowLost = 0;
	RateWindowStart = Now;
}
