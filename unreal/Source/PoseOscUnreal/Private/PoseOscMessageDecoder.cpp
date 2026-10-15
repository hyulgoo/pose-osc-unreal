#include "PoseOscMessageDecoder.h"

#include "OSCMessage.h"
#include "OSCTypes.h"

namespace PoseOsc
{
	namespace
	{
		FPoseHandSample MakeHand(float X, float Y, float Z, float Confidence)
		{
			FPoseHandSample Hand;
			Hand.Position = FVector(FMath::Clamp(X, 0.f, 1.f), FMath::Clamp(Y, 0.f, 1.f), Z);
			Hand.Confidence = FMath::Clamp(Confidence, 0.f, 1.f);
			return Hand;
		}
	}

	EPoseOscDecodeResult DecodeHandsMessage(const FOSCMessage& Message, FPoseHandsFrame& OutFrame)
	{
		using UE::OSC::FOSCData;

		// FString::operator== ignores case; OSC address matching does not.
		if (!Message.GetAddress().GetFullPath().Equals(MessageAddress, ESearchCase::CaseSensitive))
		{
			return EPoseOscDecodeResult::WrongAddress;
		}

		const TArray<FOSCData>& Args = Message.GetArgumentsChecked();
		if (Args.Num() != MessageArgCount)
		{
			return EPoseOscDecodeResult::WrongArgCount;
		}
		if (!Args[0].IsInt32() || !Args[1].IsInt32())
		{
			return EPoseOscDecodeResult::WrongArgType;
		}

		float Floats[MessageArgCount - 2];
		for (int32 Index = 2; Index < MessageArgCount; ++Index)
		{
			if (!Args[Index].IsFloat())
			{
				return EPoseOscDecodeResult::WrongArgType;
			}
			Floats[Index - 2] = Args[Index].GetFloat();
			if (!FMath::IsFinite(Floats[Index - 2]))
			{
				return EPoseOscDecodeResult::InvalidValue;
			}
		}

		const int32 Seq = Args[0].GetInt32();
		const int32 Person = Args[1].GetInt32();
		if (Seq < 0 || (Person != 0 && Person != 1))
		{
			return EPoseOscDecodeResult::InvalidValue;
		}

		OutFrame.Seq = Seq;
		OutFrame.bPersonPresent = Person == 1;
		OutFrame.Left = MakeHand(Floats[0], Floats[1], Floats[2], Floats[3]);
		OutFrame.Right = MakeHand(Floats[4], Floats[5], Floats[6], Floats[7]);
		return EPoseOscDecodeResult::Ok;
	}

	const TCHAR* LexToString(EPoseOscDecodeResult Result)
	{
		switch (Result)
		{
		case EPoseOscDecodeResult::Ok: return TEXT("Ok");
		case EPoseOscDecodeResult::WrongAddress: return TEXT("WrongAddress");
		case EPoseOscDecodeResult::WrongArgCount: return TEXT("WrongArgCount");
		case EPoseOscDecodeResult::WrongArgType: return TEXT("WrongArgType");
		case EPoseOscDecodeResult::InvalidValue: return TEXT("InvalidValue");
		}
		return TEXT("Unknown");
	}
}
