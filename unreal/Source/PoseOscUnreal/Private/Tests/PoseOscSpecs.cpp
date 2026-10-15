// Automation specs for the plain C++ parts (seq rules, message decode, state machine).
// Run: UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests PoseOscUnreal;Quit" -unattended -nullrhi -nosplash

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "OSCAddress.h"
#include "OSCMessage.h"
#include "OSCTypes.h"
#include "PoseInteractionStateMachine.h"
#include "PoseOscMessageDecoder.h"
#include "PosePresentationMath.h"
#include "PoseSeqTracker.h"

#include <limits>

namespace PoseOscSpecPrivate
{
	constexpr double Dt = 1.0 / 30.0;

	FPoseHandsFrame MakeFrame(int32 Seq, float LeftConf, float RightConf, bool bPerson = true,
		const FVector& LeftPos = FVector(0.3, 0.5, 0.0), const FVector& RightPos = FVector(0.7, 0.5, 0.0))
	{
		FPoseHandsFrame Frame;
		Frame.Seq = Seq;
		Frame.bPersonPresent = bPerson;
		Frame.Left.Position = LeftPos;
		Frame.Left.Confidence = LeftConf;
		Frame.Right.Position = RightPos;
		Frame.Right.Confidence = RightConf;
		return Frame;
	}

	TArray<UE::OSC::FOSCData> ValidArgs(int32 Seq = 7, int32 Person = 1)
	{
		using UE::OSC::FOSCData;
		return {
			FOSCData(Seq), FOSCData(Person),
			FOSCData(0.25f), FOSCData(0.5f), FOSCData(-0.125f), FOSCData(0.875f),
			FOSCData(0.75f), FOSCData(0.0625f), FOSCData(0.25f), FOSCData(0.5f),
		};
	}

	FOSCMessage MakeMessage(const FString& Address, TArray<UE::OSC::FOSCData> Args)
	{
		return FOSCMessage(FOSCAddress(Address), MoveTemp(Args));
	}

	/** Feeds frames every Dt from Start for Duration seconds, ticking at each frame. Returns the time after. */
	double Feed(FPoseInteractionStateMachine& Machine, double Start, double Duration, float LeftConf, float RightConf, bool bPerson = true)
	{
		double T = Start;
		int32 Seq = 0;
		while (T < Start + Duration - 1e-9)
		{
			Machine.OnFrame(MakeFrame(Seq++, LeftConf, RightConf, bPerson), T);
			Machine.Tick(T);
			T += Dt;
		}
		return T;
	}
}

// ----- seq rules (D3) ----------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FPoseSeqTrackerSpec, "PoseOscUnreal.SeqTracker", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FPoseSeqTrackerSpec)

void FPoseSeqTrackerSpec::Define()
{
	It("accepts sequential packets and counts gaps", [this]()
	{
		FPoseSeqTracker Tracker;
		TestTrue(TEXT("first is baseline"), Tracker.Classify(0).Decision == EPoseSeqDecision::Baseline);
		TestTrue(TEXT("next"), Tracker.Classify(1).Decision == EPoseSeqDecision::Next);
		const FPoseSeqResult Gap = Tracker.Classify(4);
		TestTrue(TEXT("gap"), Gap.Decision == EPoseSeqDecision::Gap);
		TestEqual(TEXT("lost"), Gap.Lost, 2);
	});

	It("drops duplicates and small regressions", [this]()
	{
		FPoseSeqTracker Tracker(30);
		for (int32 Seq = 0; Seq < 50; ++Seq)
		{
			Tracker.Classify(Seq);
		}
		TestTrue(TEXT("duplicate"), Tracker.Classify(49).Decision == EPoseSeqDecision::Duplicate);
		const FPoseSeqResult Stale = Tracker.Classify(40);
		TestTrue(TEXT("stale"), Stale.Decision == EPoseSeqDecision::Stale);
		TestFalse(TEXT("stale not accepted"), Stale.IsAccepted());
		TestEqual(TEXT("baseline unchanged"), Tracker.GetLastSeq().Get(-1), 49);
	});

	It("treats a large regression as a sender restart", [this]()
	{
		FPoseSeqTracker Tracker(30);
		for (int32 Seq = 0; Seq < 100; ++Seq)
		{
			Tracker.Classify(Seq);
		}
		TestTrue(TEXT("restart"), Tracker.Classify(0).Decision == EPoseSeqDecision::RestartDetected);
		TestTrue(TEXT("then next"), Tracker.Classify(1).Decision == EPoseSeqDecision::Next);
	});

	It("accepts seq 0 after ResetBaseline (Idle entry, scenario 2)", [this]()
	{
		FPoseSeqTracker Tracker(30);
		for (int32 Seq = 0; Seq < 10; ++Seq)
		{
			Tracker.Classify(Seq);
		}
		TestTrue(TEXT("small regression is stale before reset"), Tracker.Classify(0).Decision == EPoseSeqDecision::Stale);
		Tracker.ResetBaseline();
		TestTrue(TEXT("baseline after reset"), Tracker.Classify(0).Decision == EPoseSeqDecision::Baseline);
	});

	It("handles int32 wrap-around as a restart", [this]()
	{
		FPoseSeqTracker Tracker(30);
		Tracker.Classify(MAX_int32 - 1);
		Tracker.Classify(MAX_int32);
		TestTrue(TEXT("wrap"), Tracker.Classify(0).Decision == EPoseSeqDecision::RestartDetected);
	});
}

// ----- message decode (D3 schema) -----------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FPoseOscDecoderSpec, "PoseOscUnreal.Decoder", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FPoseOscDecoderSpec)

void FPoseOscDecoderSpec::Define()
{
	using namespace PoseOscSpecPrivate;
	using UE::OSC::FOSCData;

	It("decodes a valid message", [this]()
	{
		FPoseHandsFrame Frame;
		TestTrue(TEXT("ok"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), ValidArgs()), Frame) == EPoseOscDecodeResult::Ok);
		TestEqual(TEXT("seq"), Frame.Seq, 7);
		TestTrue(TEXT("person"), Frame.bPersonPresent);
		TestEqual(TEXT("left x"), Frame.Left.Position.X, 0.25);
		TestEqual(TEXT("left z"), Frame.Left.Position.Z, -0.125);
		TestEqual(TEXT("left conf"), Frame.Left.Confidence, 0.875f);
		TestEqual(TEXT("right y"), Frame.Right.Position.Y, 0.0625);
	});

	It("rejects a different address, including case", [this]()
	{
		FPoseHandsFrame Frame;
		TestTrue(TEXT("other"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hand"), ValidArgs()), Frame) == EPoseOscDecodeResult::WrongAddress);
		TestTrue(TEXT("case"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/Hands"), ValidArgs()), Frame) == EPoseOscDecodeResult::WrongAddress);
	});

	It("rejects a wrong argument count", [this]()
	{
		TArray<FOSCData> Args = ValidArgs();
		Args.Pop();
		FPoseHandsFrame Frame;
		TestTrue(TEXT("9 args"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), MoveTemp(Args)), Frame) == EPoseOscDecodeResult::WrongArgCount);
	});

	It("rejects wrong argument types", [this]()
	{
		FPoseHandsFrame Frame;
		TArray<FOSCData> IntCoord = ValidArgs();
		IntCoord[2] = FOSCData(1);
		TestTrue(TEXT("int coordinate"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), MoveTemp(IntCoord)), Frame) == EPoseOscDecodeResult::WrongArgType);

		TArray<FOSCData> DoubleCoord = ValidArgs();
		DoubleCoord[5] = FOSCData(0.5);
		TestTrue(TEXT("double coordinate"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), MoveTemp(DoubleCoord)), Frame) == EPoseOscDecodeResult::WrongArgType);

		TArray<FOSCData> FloatSeq = ValidArgs();
		FloatSeq[0] = FOSCData(1.f);
		TestTrue(TEXT("float seq"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), MoveTemp(FloatSeq)), Frame) == EPoseOscDecodeResult::WrongArgType);
	});

	It("rejects invalid values", [this]()
	{
		FPoseHandsFrame Frame;
		TestTrue(TEXT("person 2"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), ValidArgs(1, 2)), Frame) == EPoseOscDecodeResult::InvalidValue);
		TestTrue(TEXT("seq -1"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), ValidArgs(-1, 1)), Frame) == EPoseOscDecodeResult::InvalidValue);
		TArray<FOSCData> NaN = ValidArgs();
		NaN[3] = FOSCData(std::numeric_limits<float>::quiet_NaN());
		TestTrue(TEXT("NaN"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), MoveTemp(NaN)), Frame) == EPoseOscDecodeResult::InvalidValue);
	});

	It("clamps out-of-range x, y and confidence", [this]()
	{
		TArray<FOSCData> Args = ValidArgs();
		Args[2] = FOSCData(1.5f);
		Args[3] = FOSCData(-0.25f);
		Args[5] = FOSCData(1.25f);
		FPoseHandsFrame Frame;
		TestTrue(TEXT("ok"), PoseOsc::DecodeHandsMessage(MakeMessage(TEXT("/dstrict/hands"), MoveTemp(Args)), Frame) == EPoseOscDecodeResult::Ok);
		TestEqual(TEXT("x"), Frame.Left.Position.X, 1.0);
		TestEqual(TEXT("y"), Frame.Left.Position.Y, 0.0);
		TestEqual(TEXT("conf"), Frame.Left.Confidence, 1.f);
	});
}

// ----- state machine (D6) --------------------------------------------------------------------------------------

BEGIN_DEFINE_SPEC(FPoseStateMachineSpec, "PoseOscUnreal.StateMachine", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FPoseStateMachineSpec)

void FPoseStateMachineSpec::Define()
{
	using namespace PoseOscSpecPrivate;

	It("starts Idle with both hands Lost", [this]()
	{
		const FPoseInteractionStateMachine Machine;
		TestTrue(TEXT("idle"), Machine.GetGlobalState() == EPoseGlobalState::Idle);
		TestTrue(TEXT("left lost"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Lost);
	});

	It("enters Interactive after 0.2 s of continuous packets, not before", [this]()
	{
		FPoseInteractionStateMachine Machine;
		Feed(Machine, 10.0, 0.15, 0.9f, 0.9f);
		TestTrue(TEXT("still idle at 0.15 s"), Machine.GetGlobalState() == EPoseGlobalState::Idle);
		Feed(Machine, 10.15, 0.1, 0.9f, 0.9f);
		TestTrue(TEXT("interactive after 0.25 s"), Machine.GetGlobalState() == EPoseGlobalState::Interactive);
		TestTrue(TEXT("hand tracking"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Tracking);
	});

	It("restarts the streak after a gap longer than ContinuityMaxGap", [this]()
	{
		FPoseInteractionStateMachine Machine;
		Machine.OnFrame(MakeFrame(0, 0.9f, 0.9f), 0.00);
		Machine.OnFrame(MakeFrame(1, 0.9f, 0.9f), 0.10);
		Machine.OnFrame(MakeFrame(2, 0.9f, 0.9f), 0.30); // gap 0.2 s > 0.15 s: streak restarts here
		TestTrue(TEXT("idle"), Machine.GetGlobalState() == EPoseGlobalState::Idle);
		TestEqual(TEXT("streak"), Machine.GetStreakSeconds(), 0.0);
	});

	It("goes Idle after 1.0 s of silence and forces hands to Lost (scenario 2)", [this]()
	{
		FPoseInteractionStateMachine Machine;
		const double End = Feed(Machine, 0.0, 1.0, 0.9f, 0.9f);
		Machine.DrainTransitions();
		const double LastFrame = Machine.GetLastFrameTime();

		Machine.Tick(LastFrame + 0.99);
		TestTrue(TEXT("still interactive at 0.99 s"), Machine.GetGlobalState() == EPoseGlobalState::Interactive);
		Machine.Tick(LastFrame + 1.01);
		TestTrue(TEXT("idle after 1.01 s"), Machine.GetGlobalState() == EPoseGlobalState::Idle);
		TestTrue(TEXT("left lost"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Lost);

		const TArray<FPoseStateTransition> Transitions = Machine.DrainTransitions();
		TestTrue(TEXT("first transition is global Idle"), Transitions.Num() == 3 && Transitions[0].bGlobal && Transitions[0].GlobalTo == EPoseGlobalState::Idle);

		// Python restarts: back to Interactive 0.2 s after packets resume.
		Feed(Machine, End + 3.0, 0.25, 0.9f, 0.9f);
		TestTrue(TEXT("interactive again"), Machine.GetGlobalState() == EPoseGlobalState::Interactive);
	});

	It("holds the last position, then Lost after 0.5 s, then recovers (scenario 1)", [this]()
	{
		FPoseInteractionStateMachine Machine;
		double T = Feed(Machine, 0.0, 0.5, 0.9f, 0.9f);
		const FVector Before = Machine.GetHandTarget(EPoseHand::Right);

		// Covered: right confidence 0.1 with a different reported position.
		Machine.OnFrame(MakeFrame(100, 0.9f, 0.1f, true, FVector(0.3, 0.5, 0.0), FVector(0.1, 0.1, 0.0)), T);
		TestTrue(TEXT("holding"), Machine.GetHandState(EPoseHand::Right) == EPoseHandState::Holding);
		TestTrue(TEXT("position held"), Machine.GetHandTarget(EPoseHand::Right).Equals(Before));
		TestTrue(TEXT("left unaffected"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Tracking);

		T = Feed(Machine, T + Dt, 0.45, 0.9f, 0.1f);
		TestTrue(TEXT("still holding before 0.5 s"), Machine.GetHandState(EPoseHand::Right) == EPoseHandState::Holding);
		T = Feed(Machine, T, 0.1, 0.9f, 0.1f);
		TestTrue(TEXT("lost after 0.5 s"), Machine.GetHandState(EPoseHand::Right) == EPoseHandState::Lost);

		Machine.OnFrame(MakeFrame(200, 0.9f, 0.7f), T);
		TestTrue(TEXT("tracking again"), Machine.GetHandState(EPoseHand::Right) == EPoseHandState::Tracking);
	});

	It("keeps the current state inside the 0.4-0.6 band", [this]()
	{
		FPoseInteractionStateMachine Machine;
		double T = Feed(Machine, 0.0, 0.5, 0.9f, 0.9f);
		const FVector Moved(0.42, 0.42, 0.0);
		Machine.OnFrame(MakeFrame(50, 0.5f, 0.9f, true, Moved), T);
		TestTrue(TEXT("tracking kept"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Tracking);
		TestTrue(TEXT("target follows in band"), Machine.GetHandTarget(EPoseHand::Left).Equals(Moved));

		T = Feed(Machine, T + Dt, 1.0, 0.1f, 0.9f); // -> Holding -> Lost
		Machine.OnFrame(MakeFrame(300, 0.5f, 0.9f), T);
		TestTrue(TEXT("lost kept in band"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Lost);
	});

	It("treats person = 0 as confidence 0 for both hands", [this]()
	{
		FPoseInteractionStateMachine Machine;
		const double T = Feed(Machine, 0.0, 0.5, 0.9f, 0.9f);
		Machine.OnFrame(MakeFrame(10, 0.9f, 0.9f, false), T);
		TestTrue(TEXT("left holding"), Machine.GetHandState(EPoseHand::Left) == EPoseHandState::Holding);
		TestTrue(TEXT("right holding"), Machine.GetHandState(EPoseHand::Right) == EPoseHandState::Holding);
		TestTrue(TEXT("still interactive (heartbeat)"), Machine.GetGlobalState() == EPoseGlobalState::Interactive);
		TestEqual(TEXT("reason"), Machine.GetHandReason(EPoseHand::Left), FString(TEXT("person = 0")));
	});

	It("clamps HoldingEnter above TrackingEnter", [this]()
	{
		FPoseStateMachineSettings Settings;
		Settings.TrackingEnterConfidence = 0.5f;
		Settings.HoldingEnterConfidence = 0.7f;
		AddExpectedMessage(TEXT("HoldingEnterConfidence"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
		const FPoseInteractionStateMachine Machine(Settings);
		TestEqual(TEXT("clamped"), Machine.GetSettings().HoldingEnterConfidence, 0.5f);
	});
}

// ----- presentation math (D4 render interpolation, D6 crossfade, D7 volume mapping) ---------------------------

BEGIN_DEFINE_SPEC(FPosePresentationSpec, "PoseOscUnreal.Presentation", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
END_DEFINE_SPEC(FPosePresentationSpec)

void FPosePresentationSpec::Define()
{
	using namespace PosePresentation;

	It("maps normalized x/y onto the volume face seen from -X", [this]()
	{
		const FVector Extent(50.0, 160.0, 90.0);
		const FPoseVolumeMappingSettings Flat;
		TestTrue(TEXT("center"), NormalizedToVolumeLocal(FVector(0.5, 0.5, 0.0), Extent, Flat).Equals(FVector::ZeroVector));
		// Mirrored x = 0 is the viewer's left (-Y in UE's left-handed frame); y = 0 is the top (+Z).
		TestTrue(TEXT("top-left"), NormalizedToVolumeLocal(FVector(0.0, 0.0, 0.0), Extent, Flat).Equals(FVector(0.0, -160.0, 90.0)));
		TestTrue(TEXT("bottom-right"), NormalizedToVolumeLocal(FVector(1.0, 1.0, 0.0), Extent, Flat).Equals(FVector(0.0, 160.0, -90.0)));
		TestTrue(TEXT("clamped"), NormalizedToVolumeLocal(FVector(1.5, -0.5, 0.0), Extent, Flat).Equals(FVector(0.0, 160.0, 90.0)));
	});

	It("ignores depth at weight 0 and moves toward the viewer for negative z", [this]()
	{
		const FVector Extent(50.0, 160.0, 90.0);
		FPoseVolumeMappingSettings Settings;
		TestEqual(TEXT("flat"), NormalizedToVolumeLocal(FVector(0.5, 0.5, -0.4), Extent, Settings).X, 0.0);
		Settings.DepthWeight = 1.f;
		Settings.DepthRange = 0.5f;
		TestEqual(TEXT("closer"), NormalizedToVolumeLocal(FVector(0.5, 0.5, -0.25), Extent, Settings).X, -25.0);
		TestEqual(TEXT("clamped depth"), NormalizedToVolumeLocal(FVector(0.5, 0.5, -2.0), Extent, Settings).X, -50.0);
	});

	It("smooths independently of frame rate", [this]()
	{
		const double Tau = 0.05;
		double At60 = 0.0;
		for (int32 Step = 0; Step < 2; ++Step)
		{
			At60 += (1.0 - At60) * ExpAlpha(1.0 / 60.0, Tau);
		}
		const double At30 = ExpAlpha(1.0 / 30.0, Tau);
		TestEqual(TEXT("two 60 Hz steps == one 30 Hz step"), At60, At30, 1e-9);
		TestEqual(TEXT("tau 0 = no smoothing"), ExpAlpha(0.016, 0.0), 1.0);
		TestEqual(TEXT("negative dt"), ExpAlpha(-1.0, Tau), 0.0);
	});

	It("crossfades weights with separate rise and fall time constants", [this]()
	{
		const double Dt = 1.0 / 60.0;
		float Up = 0.f;
		float Down = 1.f;
		for (int32 Step = 0; Step < 9; ++Step) // 0.15 s
		{
			Up = ApproachWeight(Up, 1.f, Dt, 0.15, 0.4);
			Down = ApproachWeight(Down, 0.f, Dt, 0.15, 0.4);
		}
		TestEqual(TEXT("rise after one tau"), Up, 1.f - FMath::Exp(-1.f), 0.01f);
		TestTrue(TEXT("fall is slower"), 1.f - Down < Up);
		float Settled = 0.f;
		for (int32 Step = 0; Step < 600; ++Step)
		{
			Settled = ApproachWeight(Settled, 1.f, Dt, 0.15, 0.4);
		}
		TestEqual(TEXT("reaches exactly 1"), Settled, 1.f);
	});

	It("uses Tracking 1 / Holding HoldingWeight / Lost 0 as weight targets", [this]()
	{
		TestEqual(TEXT("tracking"), HandWeightTarget(EPoseHandState::Tracking, 0.6f), 1.f);
		TestEqual(TEXT("holding"), HandWeightTarget(EPoseHandState::Holding, 0.6f), 0.6f);
		TestEqual(TEXT("lost"), HandWeightTarget(EPoseHandState::Lost, 0.6f), 0.f);
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
