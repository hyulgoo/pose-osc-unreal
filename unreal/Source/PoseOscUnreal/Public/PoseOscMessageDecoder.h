#pragma once

#include "CoreMinimal.h"
#include "PoseOscTypes.h"

struct FOSCMessage;

enum class EPoseOscDecodeResult : uint8
{
	Ok,
	WrongAddress,   // not exactly /dstrict/hands (OSC addresses are case-sensitive)
	WrongArgCount,  // != 10
	WrongArgType,   // not ,iiffffffff
	InvalidValue,   // seq < 0, person not 0/1, or a non-finite float
};

namespace PoseOsc
{
	/**
	 * Strict decode of one /dstrict/hands message (D3). On Ok, x / y / confidence are clamped to [0, 1] as a safe
	 * fallback (the Python sender already clamps them); z is passed through.
	 */
	POSEOSCUNREAL_API EPoseOscDecodeResult DecodeHandsMessage(const FOSCMessage& Message, FPoseHandsFrame& OutFrame);

	POSEOSCUNREAL_API const TCHAR* LexToString(EPoseOscDecodeResult Result);
}
