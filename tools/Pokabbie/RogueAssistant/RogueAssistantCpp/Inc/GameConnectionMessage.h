#pragma once
#include "Defines.h"

enum class GameMessageChannel : u16
{
	Undefined,
	CommonRead,
};

union GameMessageID
{
	u32 CompactedID;
	struct
	{
		GameMessageChannel Channel;
		union
		{
			u16 Param16;
			u8 Param8[2];
		};
	};
};

inline GameMessageID CreateMessageId(GameMessageChannel channel, u16 param = 0)
{
	GameMessageID id;
	id.Channel = channel;
	id.Param16 = param;
	return id;
}
