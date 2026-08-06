#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "../pad/PsyX_pad.h"

#include "PsyX/PsyX_public.h"

#include <string.h>

int g_padCommEnable = 0;

static unsigned char g_padActAlign[MAX_CONTROLLERS][6] = {
	{ 0, 1, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0, 1, 0xFF, 0xFF, 0xFF, 0xFF }
};

static int PadDecodePort(int port, int* mtap, int* slot)
{
	const int decodedMtap = port & 3;
	const int decodedSlot = (port >> 4) & 1;
	if (decodedMtap != 0 || decodedSlot >= MAX_CONTROLLERS)
		return 0;

	*mtap = decodedMtap;
	*slot = decodedSlot;
	return 1;
}

static void PadResetActAlign(int slot)
{
	g_padActAlign[slot][0] = 0;
	g_padActAlign[slot][1] = 1;
	memset(&g_padActAlign[slot][2], 0xFF, 4);
}

void PadInitDirect(unsigned char* pad1, unsigned char* pad2)
{
	PadResetActAlign(0);
	PadResetActAlign(1);
	PsyX_Pad_InitPad(0, pad1);
	PsyX_Pad_InitPad(1, pad2);
}

void PadInitMtap(unsigned char* slot1, unsigned char* slot2)
{
	PSYX_UNIMPLEMENTED();
	PadResetActAlign(0);
	PadResetActAlign(1);
	PsyX_Pad_InitPad(0, slot1);
	PsyX_Pad_InitPad(1, slot2);
}

void PadInitGun(unsigned char* unk00, int unk01)
{
	PSYX_UNIMPLEMENTED();
}

int PadChkVsync()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void PadStartCom()
{
	g_padCommEnable = 1;
}

void PadStopCom()
{
	PsyX_Pad_StopAllRumble();
	g_padCommEnable = 0;
}

unsigned int PadEnableCom(unsigned int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void PadEnableGun(unsigned char unk00)
{
	PSYX_UNIMPLEMENTED();
}

void PadRemoveGun()
{
	PSYX_UNIMPLEMENTED();
}

int PadGetState(int port)
{
	int mtap;
	int slot;
	if (!PadDecodePort(port, &mtap, &slot))
		return PadStateDiscon;

	return PsyX_Pad_GetStatus(mtap, slot) ? PadStateStable : PadStateDiscon;
}

int PadInfoMode(int port, int term, int offs)
{
	int mtap;
	int slot;
	if (!PadDecodePort(port, &mtap, &slot) || !PsyX_Pad_GetStatus(mtap, slot))
		return 0;

	switch (term)
	{
	case InfoModeCurID:
		return PsyX_Pad_GetModeId(mtap, slot);
	case InfoModeCurExID:
		return 7;
	case InfoModeCurExOffs:
		return PsyX_Pad_GetModeId(mtap, slot) == 7 ? 1 : 0;
	case InfoModeIdTable:
		if (offs < 0)
			return 2;
		if (offs == 0)
			return 4;
		if (offs == 1)
			return 7;
		return 0;
	default:
		return 0;
	}
}

int PadInfoAct(int port, int acno, int term)
{
	static const unsigned char actuatorInfo[2][5] = {
		{ 1, 2, 0, 10, 0 },
		{ 1, 1, 1, 20, 0 }
	};
	int mtap;
	int slot;
	if (!PadDecodePort(port, &mtap, &slot) || !PsyX_Pad_GetStatus(mtap, slot))
		return 0;
	if (acno == -1)
		return 2;
	if (acno < 0 || acno >= 2 || term < InfoActFunc || term > InfoActSign)
		return 0;
	return actuatorInfo[acno][term - InfoActFunc];
}

int PadInfoComb(int unk00, int unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int PadSetActAlign(int port, unsigned char* table)
{
	int mtap;
	int slot;
	int seen[2] = { 0, 0 };
	if (!table || !PadDecodePort(port, &mtap, &slot) ||
		!PsyX_Pad_GetStatus(mtap, slot))
		return 0;

	for (int offset = 0; offset < 6; ++offset)
	{
		const unsigned char actuator = table[offset];
		if (actuator == 0xFF)
			continue;
		if (actuator > 1 || seen[actuator])
			return 0;
		seen[actuator] = 1;
	}

	memcpy(g_padActAlign[slot], table, 6);
	return 1;
}

int PadSetMainMode(int socket, int offs, int lock)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void PadSetAct(int port, unsigned char* table, int len)
{
	int mtap;
	int slot;
	unsigned char actuatorValues[2] = { 0, 0 };
	if (!PadDecodePort(port, &mtap, &slot))
		return;

	if (!table || len <= 0)
	{
		PsyX_Pad_Vibrate(mtap, slot, NULL, 0);
		return;
	}

	const int alignedLength = len < 6 ? len : 6;
	for (int offset = 0; offset < alignedLength; ++offset)
	{
		const unsigned char actuator = g_padActAlign[slot][offset];
		if (actuator < 2)
			actuatorValues[actuator] = table[offset];
	}

	PsyX_Pad_Vibrate(mtap, slot, actuatorValues, 2);
}