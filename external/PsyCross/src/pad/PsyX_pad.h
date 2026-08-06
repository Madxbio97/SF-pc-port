#ifndef PSYX_PAD_H
#define PSYX_PAD_H

#include "PsyX/PsyX_public.h"

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
extern "C" {
#endif

extern int PsyX_Pad_InitSystem(void);
extern void PsyX_Pad_ShutdownSystem(void);
extern void PsyX_Pad_InitPad(int slot, unsigned char* padData);
extern void PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len);
extern int PsyX_Pad_GetStatus(int mtap, int slot);
extern int PsyX_Pad_GetModeId(int mtap, int slot);
extern void PsyX_Pad_InternalPadUpdates(void);

extern void PsyX_Pad_DeviceAdded(int deviceIndex);
extern void PsyX_Pad_DeviceRemoved(int instanceId);
extern void PsyX_Pad_DeviceRemapped(int instanceId);
extern void PsyX_Pad_StopAllRumble(void);
extern void PsyX_Pad_SetFocus(int focused);

#if defined(_LANGUAGE_C_PLUS_PLUS)||defined(__cplusplus)||defined(c_plusplus)
}
#endif

#endif // PSYX_PAD_H