#include "psx/libpad.h"
#include "psx/libetc.h"

#include "../PsyX_main.h"
#include "PsyX_pad.h"

#include <stdlib.h>
#include <string.h>

extern "C"
{
extern int g_padCommEnable;
}

typedef struct
{
	int deviceIndex;
	SDL_JoystickID instanceId;
	SDL_GameController* gameController;
	SDL_Joystick* joystick;
	SDL_Haptic* haptic;
	unsigned char* padData;
	bool ownsJoystick;
	bool switchingAnalog;
	bool hapticRumbleInitialized;
	PsyXControllerSnapshot snapshot;
} PsyXController;

int g_cfg_controllerToSlotMapping[MAX_CONTROLLERS] = { -1, -1 };

static PsyXController g_controllers[MAX_CONTROLLERS];
static const unsigned char* g_sdlKeyboardState = NULL;
static bool g_padSystemInitialized = false;
static bool g_inputSubsystemInitialized = false;
static bool g_hapticSubsystemInitialized = false;
static bool g_padOutputFocused = true;

static void PsyX_Pad_ResetSnapshot(PsyXControllerSnapshot* snapshot)
{
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->buttons[0] = 0xFF;
	snapshot->buttons[1] = 0xFF;
	snapshot->analog[0] = 128;
	snapshot->analog[1] = 128;
	snapshot->analog[2] = 128;
	snapshot->analog[3] = 128;
	snapshot->family = PSYX_CONTROLLER_FAMILY_UNKNOWN;
	snapshot->instanceId = -1;
}

static void PsyX_Pad_ResetController(PsyXController* controller)
{
	memset(controller, 0, sizeof(*controller));
	controller->deviceIndex = -1;
	controller->instanceId = -1;
	PsyX_Pad_ResetSnapshot(&controller->snapshot);
}

static bool PsyX_Pad_IsAttached(const PsyXController* controller)
{
	if (controller->gameController)
		return SDL_GameControllerGetAttached(controller->gameController) == SDL_TRUE;

	return controller->joystick &&
		SDL_JoystickGetAttached(controller->joystick) == SDL_TRUE;
}

static unsigned char PsyX_Pad_AxisToAnalog(int value)
{
	if (value < -32768)
		value = -32768;
	else if (value > 32767)
		value = 32767;

	return (unsigned char)((value / 256) + 128);
}

static void PsyX_Pad_SetSnapshotButtons(PsyXControllerSnapshot* snapshot,
										   Uint16 buttons)
{
	snapshot->buttons[0] = (unsigned char)(buttons & 0xFF);
	snapshot->buttons[1] = (unsigned char)(buttons >> 8);
}

static Uint16 PsyX_Pad_GetPadButtons(const LPPADRAW pad)
{
	return (Uint16)(pad->buttons[0] | ((Uint16)pad->buttons[1] << 8));
}

static void PsyX_Pad_SetPadButtons(LPPADRAW pad, Uint16 buttons)
{
	pad->buttons[0] = (unsigned char)(buttons & 0xFF);
	pad->buttons[1] = (unsigned char)(buttons >> 8);
}

static int PsyX_Pad_FamilyFromControllerType(int type)
{
#if SDL_VERSION_ATLEAST(2, 0, 12)
	switch ((SDL_GameControllerType)type)
	{
	case SDL_CONTROLLER_TYPE_XBOX360:
	case SDL_CONTROLLER_TYPE_XBOXONE:
		return PSYX_CONTROLLER_FAMILY_XBOX;

	case SDL_CONTROLLER_TYPE_PS3:
	case SDL_CONTROLLER_TYPE_PS4:
#if SDL_VERSION_ATLEAST(2, 0, 14)
	case SDL_CONTROLLER_TYPE_PS5:
#endif
		return PSYX_CONTROLLER_FAMILY_PLAYSTATION;

	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
		return PSYX_CONTROLLER_FAMILY_NINTENDO;

	case SDL_CONTROLLER_TYPE_UNKNOWN:
		return PSYX_CONTROLLER_FAMILY_UNKNOWN;

	default:
		return PSYX_CONTROLLER_FAMILY_GENERIC;
	}
#else
	(void)type;
	return PSYX_CONTROLLER_FAMILY_GENERIC;
#endif
}

static int PsyX_Pad_GetMappedControl(SDL_GameController* controller,
									int buttonOrAxis)
{
	if (!controller || buttonOrAxis < 0)
		return 0;

	if (buttonOrAxis & CONTROLLER_MAP_FLAG_AXIS)
	{
		const int axisIndex = buttonOrAxis &
			~(CONTROLLER_MAP_FLAG_AXIS | CONTROLLER_MAP_FLAG_INVERSE);
		if (axisIndex < 0 || axisIndex >= SDL_CONTROLLER_AXIS_MAX)
			return 0;

		int value = SDL_GameControllerGetAxis(
			controller, (SDL_GameControllerAxis)axisIndex);
		if (buttonOrAxis & CONTROLLER_MAP_FLAG_INVERSE)
			value = -value;
		return value;
	}

	if (buttonOrAxis >= SDL_CONTROLLER_BUTTON_MAX)
		return 0;

	return SDL_GameControllerGetButton(
		controller, (SDL_GameControllerButton)buttonOrAxis) * 32767;
}

static void PsyX_Pad_SetPressed(Uint16* buttons, Uint16 mask, bool pressed)
{
	if (pressed)
		*buttons &= (Uint16)~mask;
}

static void PsyX_Pad_SampleGameController(PsyXController* controller)
{
	SDL_GameController* gameController = controller->gameController;
	PsyXControllerSnapshot* snapshot = &controller->snapshot;
	const PsyXControllerMapping& mapping = g_cfg_controllerMapping;
	Uint16 buttons = 0xFFFF;

	PsyX_Pad_SetPressed(&buttons, 0x8000,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_square) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x2000,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_circle) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x1000,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_triangle) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x4000,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_cross) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0400,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_l1) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0800,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_r1) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0100,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_l2) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0200,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_r2) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0010,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_dpad_up) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0040,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_dpad_down) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0080,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_dpad_left) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0020,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_dpad_right) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0002,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_l3) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0004,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_r3) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0001,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_select) > 16384);
	PsyX_Pad_SetPressed(&buttons, 0x0008,
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_start) > 16384);

	PsyX_Pad_SetSnapshotButtons(snapshot, buttons);
	snapshot->analog[0] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_axis_right_x));
	snapshot->analog[1] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_axis_right_y));
	snapshot->analog[2] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_axis_left_x));
	snapshot->analog[3] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_GetMappedControl(gameController, mapping.gc_axis_left_y));
}

static bool PsyX_Pad_JoystickButton(SDL_Joystick* joystick, int button)
{
	return button >= 0 && button < SDL_JoystickNumButtons(joystick) &&
		SDL_JoystickGetButton(joystick, button) != 0;
}

static int PsyX_Pad_JoystickAxis(SDL_Joystick* joystick, int axis)
{
	if (axis < 0 || axis >= SDL_JoystickNumAxes(joystick))
		return 0;
	return SDL_JoystickGetAxis(joystick, axis);
}

static void PsyX_Pad_SampleJoystick(PsyXController* controller)
{
	SDL_Joystick* joystick = controller->joystick;
	PsyXControllerSnapshot* snapshot = &controller->snapshot;
	const int numAxes = SDL_JoystickNumAxes(joystick);
	const int numButtons = SDL_JoystickNumButtons(joystick);
	const int numHats = SDL_JoystickNumHats(joystick);
	const bool buttonTriggers = numAxes < 5 && numButtons >= 12;
	Uint16 buttons = 0xFFFF;

	// Unmapped joystick buttons are exposed to the host as BUTTON 1, BUTTON 2,
	// ... . Keep the physical order identical to the generic prompt order:
	// Cross, Circle, Square, Triangle.
	PsyX_Pad_SetPressed(&buttons, 0x4000, PsyX_Pad_JoystickButton(joystick, 0));
	PsyX_Pad_SetPressed(&buttons, 0x2000, PsyX_Pad_JoystickButton(joystick, 1));
	PsyX_Pad_SetPressed(&buttons, 0x8000, PsyX_Pad_JoystickButton(joystick, 2));
	PsyX_Pad_SetPressed(&buttons, 0x1000, PsyX_Pad_JoystickButton(joystick, 3));
	PsyX_Pad_SetPressed(&buttons, 0x0400, PsyX_Pad_JoystickButton(joystick, 4));
	PsyX_Pad_SetPressed(&buttons, 0x0800, PsyX_Pad_JoystickButton(joystick, 5));
	PsyX_Pad_SetPressed(&buttons, 0x0001, PsyX_Pad_JoystickButton(joystick, 6));
	PsyX_Pad_SetPressed(&buttons, 0x0008, PsyX_Pad_JoystickButton(joystick, 7));
	PsyX_Pad_SetPressed(&buttons, 0x0002, PsyX_Pad_JoystickButton(joystick, 8));
	PsyX_Pad_SetPressed(&buttons, 0x0004, PsyX_Pad_JoystickButton(joystick, 9));

	if (buttonTriggers)
	{
		PsyX_Pad_SetPressed(&buttons, 0x0100, PsyX_Pad_JoystickButton(joystick, 10));
		PsyX_Pad_SetPressed(&buttons, 0x0200, PsyX_Pad_JoystickButton(joystick, 11));
	}
	else
	{
		if (numAxes == 5)
		{
			const int triggerAxis = PsyX_Pad_JoystickAxis(joystick, 2);
			PsyX_Pad_SetPressed(&buttons, 0x0100, triggerAxis < -16384);
			PsyX_Pad_SetPressed(&buttons, 0x0200, triggerAxis > 16384);
		}
		else
		{
			PsyX_Pad_SetPressed(&buttons, 0x0100,
				PsyX_Pad_JoystickAxis(joystick, 4) > 16384);
			PsyX_Pad_SetPressed(&buttons, 0x0200,
				PsyX_Pad_JoystickAxis(joystick, 5) > 16384);
		}
	}

	if (numHats > 0)
	{
		const Uint8 hat = SDL_JoystickGetHat(joystick, 0);
		PsyX_Pad_SetPressed(&buttons, 0x0010, (hat & SDL_HAT_UP) != 0);
		PsyX_Pad_SetPressed(&buttons, 0x0040, (hat & SDL_HAT_DOWN) != 0);
		PsyX_Pad_SetPressed(&buttons, 0x0080, (hat & SDL_HAT_LEFT) != 0);
		PsyX_Pad_SetPressed(&buttons, 0x0020, (hat & SDL_HAT_RIGHT) != 0);
	}
	else if (buttonTriggers && numButtons == 16)
	{
		// Button-trigger layouts reserve 12..15 exclusively for the D-pad.
		PsyX_Pad_SetPressed(&buttons, 0x0010,
			PsyX_Pad_JoystickButton(joystick, 12));
		PsyX_Pad_SetPressed(&buttons, 0x0040,
			PsyX_Pad_JoystickButton(joystick, 13));
		PsyX_Pad_SetPressed(&buttons, 0x0080,
			PsyX_Pad_JoystickButton(joystick, 14));
		PsyX_Pad_SetPressed(&buttons, 0x0020,
			PsyX_Pad_JoystickButton(joystick, 15));
	}

	int rightAxisX = 2;
	int rightAxisY = 3;
	if (numAxes == 5)
	{
		rightAxisX = 3;
		rightAxisY = 4;
	}

	PsyX_Pad_SetSnapshotButtons(snapshot, buttons);
	snapshot->analog[0] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_JoystickAxis(joystick, rightAxisX));
	snapshot->analog[1] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_JoystickAxis(joystick, rightAxisY));
	snapshot->analog[2] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_JoystickAxis(joystick, 0));
	snapshot->analog[3] = PsyX_Pad_AxisToAnalog(
		PsyX_Pad_JoystickAxis(joystick, 1));
}

static bool PsyX_Pad_QueryRumbleSupport(const PsyXController* controller)
{
	if (!PsyX_Pad_IsAttached(controller))
		return false;

#if SDL_VERSION_ATLEAST(2, 0, 18)
	if (controller->gameController &&
		SDL_GameControllerHasRumble(controller->gameController) == SDL_TRUE)
		return true;

	if (controller->joystick &&
		SDL_JoystickHasRumble(controller->joystick) == SDL_TRUE)
		return true;
#endif

	return controller->haptic && controller->hapticRumbleInitialized;
}

static void PsyX_Pad_RefreshSnapshot(PsyXController* controller)
{
	const SDL_JoystickID instanceId = controller->instanceId;
	PsyX_Pad_ResetSnapshot(&controller->snapshot);

	if (!PsyX_Pad_IsAttached(controller))
		return;

	controller->snapshot.connected = 1;
	controller->snapshot.instanceId = (int)instanceId;
	controller->snapshot.rumbleSupported =
		PsyX_Pad_QueryRumbleSupport(controller) ? 1 : 0;

	if (controller->gameController)
	{
#if SDL_VERSION_ATLEAST(2, 0, 12)
		controller->snapshot.type =
			(int)SDL_GameControllerGetType(controller->gameController);
#else
		controller->snapshot.type = 0;
#endif
		controller->snapshot.family =
			PsyX_Pad_FamilyFromControllerType(controller->snapshot.type);
		PsyX_Pad_SampleGameController(controller);
	}
	else
	{
		controller->snapshot.family = PSYX_CONTROLLER_FAMILY_GENERIC;
		controller->snapshot.type = 0;
		PsyX_Pad_SampleJoystick(controller);
	}
}

static void PsyX_Pad_OpenHaptic(PsyXController* controller)
{
	controller->haptic = NULL;
	controller->hapticRumbleInitialized = false;

	if (!g_hapticSubsystemInitialized || !controller->joystick ||
		SDL_JoystickIsHaptic(controller->joystick) != SDL_TRUE)
		return;

	SDL_Haptic* haptic = SDL_HapticOpenFromJoystick(controller->joystick);
	if (!haptic)
		return;

	if (SDL_HapticRumbleSupported(haptic) != SDL_TRUE ||
		SDL_HapticRumbleInit(haptic) < 0)
	{
		SDL_HapticClose(haptic);
		return;
	}

	controller->haptic = haptic;
	controller->hapticRumbleInitialized = true;
}

static int PsyX_Pad_FindSlotByInstanceId(SDL_JoystickID instanceId)
{
	if (instanceId < 0)
		return -1;

	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
	{
		// SDL marks the handle detached before delivering DEVICE_REMOVED, so the
		// stored instance ID is the authoritative lookup key during teardown.
		if (g_controllers[slot].instanceId == instanceId)
			return slot;
	}

	return -1;
}

static int PsyX_Pad_FindDeviceIndex(SDL_JoystickID instanceId)
{
	for (int deviceIndex = 0; deviceIndex < SDL_NumJoysticks(); ++deviceIndex)
	{
		if (SDL_JoystickGetDeviceInstanceID(deviceIndex) == instanceId)
			return deviceIndex;
	}

	return -1;
}

static void PsyX_Pad_StopControllerRumble(PsyXController* controller)
{
#if SDL_VERSION_ATLEAST(2, 0, 9)
	if (controller->gameController)
		SDL_GameControllerRumble(controller->gameController, 0, 0, 0);
	if (controller->joystick)
		SDL_JoystickRumble(controller->joystick, 0, 0, 0);
#endif

	if (controller->haptic && controller->hapticRumbleInitialized)
		SDL_HapticRumbleStop(controller->haptic);
}

static void PsyX_Pad_NeutralizeLegacyPad(int slot)
{
	PsyXController* controller = &g_controllers[slot];
	if (!controller->padData)
		return;

	LPPADRAW pad = (LPPADRAW)controller->padData;
	PsyX_Pad_SetPadButtons(pad, 0xFFFF);
	pad->analog[0] = 128;
	pad->analog[1] = 128;
	pad->analog[2] = 128;
	pad->analog[3] = 128;
	if (slot != 0)
		pad->id = 0xFF;
}

static void PsyX_Pad_CloseController(int slot)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;

	PsyXController* controller = &g_controllers[slot];
	PsyX_Pad_StopControllerRumble(controller);

	if (controller->haptic)
		SDL_HapticClose(controller->haptic);
	controller->haptic = NULL;
	controller->hapticRumbleInitialized = false;

	if (controller->gameController)
		SDL_GameControllerClose(controller->gameController);
	else if (controller->ownsJoystick && controller->joystick)
		SDL_JoystickClose(controller->joystick);

	controller->gameController = NULL;
	controller->joystick = NULL;
	controller->ownsJoystick = false;
	controller->deviceIndex = -1;
	controller->instanceId = -1;
	controller->switchingAnalog = false;
	PsyX_Pad_ResetSnapshot(&controller->snapshot);
	PsyX_Pad_NeutralizeLegacyPad(slot);
}

static int PsyX_Pad_OpenController(int deviceIndex, int slot)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS || deviceIndex < 0 ||
		deviceIndex >= SDL_NumJoysticks())
		return 0;

	PsyXController* controller = &g_controllers[slot];
	if (controller->gameController || controller->joystick)
		return 0;

	SDL_GameController* gameController = NULL;
	SDL_Joystick* joystick = NULL;
	bool ownsJoystick = false;

	if (SDL_IsGameController(deviceIndex))
	{
		gameController = SDL_GameControllerOpen(deviceIndex);
		if (gameController)
			joystick = SDL_GameControllerGetJoystick(gameController);
	}
	else
	{
		joystick = SDL_JoystickOpen(deviceIndex);
		ownsJoystick = joystick != NULL;
	}

	if (!joystick)
	{
		if (gameController)
			SDL_GameControllerClose(gameController);
		eprintwarn("Unable to open input device %d: %s\n", deviceIndex,
			SDL_GetError());
		return 0;
	}

	controller->deviceIndex = deviceIndex;
	controller->instanceId = SDL_JoystickInstanceID(joystick);
	controller->gameController = gameController;
	controller->joystick = joystick;
	controller->ownsJoystick = ownsJoystick;
	controller->switchingAnalog = false;
	PsyX_Pad_OpenHaptic(controller);
	PsyX_Pad_RefreshSnapshot(controller);

	const char* name = gameController ? SDL_GameControllerName(gameController) :
		SDL_JoystickName(joystick);
	eprintinfo("Input slot %d: %s '%s' (instance %d)\n", slot + 1,
		gameController ? "GameController" : "Joystick/DInput",
		name ? name : "Unknown", (int)controller->instanceId);
	return 1;
}

static int PsyX_Pad_SelectSlot(int deviceIndex)
{
	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
	{
		if (!g_controllers[slot].gameController && !g_controllers[slot].joystick &&
			g_cfg_controllerToSlotMapping[slot] == deviceIndex)
			return slot;
	}

	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
	{
		if (!g_controllers[slot].gameController && !g_controllers[slot].joystick &&
			g_cfg_controllerToSlotMapping[slot] == -1)
			return slot;
	}

	return -1;
}

static void PsyX_Pad_AssignAvailableControllers(void)
{
	// SDL device indices are compacted after removal. Scan the current list in
	// index order so a previously unassigned controller is promoted
	// deterministically when a slot becomes free.
	const int numJoysticks = SDL_NumJoysticks();
	for (int deviceIndex = 0; deviceIndex < numJoysticks; ++deviceIndex)
	{
		const SDL_JoystickID instanceId =
			SDL_JoystickGetDeviceInstanceID(deviceIndex);
		if (instanceId < 0 || PsyX_Pad_FindSlotByInstanceId(instanceId) >= 0)
			continue;

		const int slot = PsyX_Pad_SelectSlot(deviceIndex);
		if (slot >= 0)
			PsyX_Pad_OpenController(deviceIndex, slot);
	}
}

static void PsyX_Pad_ReopenController(int slot, int deviceIndex)
{
	PsyX_Pad_CloseController(slot);
	PsyX_Pad_OpenController(deviceIndex, slot);
}

static void PsyX_Pad_DebugListControllers(void)
{
	const int numJoysticks = SDL_NumJoysticks();
	if (numJoysticks <= 0)
	{
		eprintwarn("No SDL input controllers found.\n");
		return;
	}

	eprintf("SDL input controller list:\n");
	for (int deviceIndex = 0; deviceIndex < numJoysticks; ++deviceIndex)
	{
		const bool mapped = SDL_IsGameController(deviceIndex) == SDL_TRUE;
		const char* name = mapped ? SDL_GameControllerNameForIndex(deviceIndex) :
			SDL_JoystickNameForIndex(deviceIndex);
		eprintinfo("  %d %s '%s'\n", deviceIndex,
			mapped ? "GameController" : "Joystick/DInput",
			name ? name : "Unknown");
	}
}

int PsyX_Pad_InitSystem(void)
{
	if (g_padSystemInitialized)
		return 1;

	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
		PsyX_Pad_ResetController(&g_controllers[slot]);

	if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
	{
		eprinterr("Failed to initialise SDL controller subsystems: %s\n",
			SDL_GetError());
		return 0;
	}
	g_inputSubsystemInitialized = true;

	if (SDL_InitSubSystem(SDL_INIT_HAPTIC) == 0)
		g_hapticSubsystemInitialized = true;
	else
		eprintwarn("SDL haptic subsystem is unavailable: %s\n", SDL_GetError());

	SDL_JoystickEventState(SDL_ENABLE);
	SDL_GameControllerEventState(SDL_ENABLE);
	SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
	g_sdlKeyboardState = SDL_GetKeyboardState(NULL);
	g_padSystemInitialized = true;
	g_padOutputFocused = true;

	PsyX_Pad_DebugListControllers();
	const int numJoysticks = SDL_NumJoysticks();
	for (int deviceIndex = 0; deviceIndex < numJoysticks; ++deviceIndex)
		PsyX_Pad_DeviceAdded(deviceIndex);

	return 1;
}

void PsyX_Pad_StopAllRumble(void)
{
	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
		PsyX_Pad_StopControllerRumble(&g_controllers[slot]);
}

void PsyX_Pad_SetFocus(int focused)
{
	g_padOutputFocused = focused != 0;
	if (!g_padOutputFocused)
		PsyX_Pad_StopAllRumble();
}

void PsyX_Pad_ShutdownSystem(void)
{
	g_padOutputFocused = false;
	PsyX_Pad_StopAllRumble();
	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
	{
		// The guest receive buffer can already be gone during process shutdown.
		// Hotplug removal still keeps the pointer and neutralizes it immediately.
		g_controllers[slot].padData = NULL;
		PsyX_Pad_CloseController(slot);
	}

	if (g_hapticSubsystemInitialized)
		SDL_QuitSubSystem(SDL_INIT_HAPTIC);
	if (g_inputSubsystemInitialized)
		SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);

	g_hapticSubsystemInitialized = false;
	g_inputSubsystemInitialized = false;
	g_padSystemInitialized = false;
	g_sdlKeyboardState = NULL;
}

void PsyX_Pad_InitPad(int slot, unsigned char* padData)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;

	PsyXController* controller = &g_controllers[slot];
	controller->padData = padData;
	if (!padData)
		return;

	LPPADRAW pad = (LPPADRAW)padData;
	const bool wasConnected = pad->id == 0x41 || pad->id == 0x73;
	pad->status = 0;
	if (!wasConnected)
		pad->id = (slot == 0 || controller->snapshot.connected) ? 0x41 : 0xFF;
	PsyX_Pad_SetPadButtons(pad, 0xFFFF);
	pad->analog[0] = 128;
	pad->analog[1] = 128;
	pad->analog[2] = 128;
	pad->analog[3] = 128;
}

void PsyX_Pad_DeviceAdded(int deviceIndex)
{
	if (!g_padSystemInitialized || deviceIndex < 0 ||
		deviceIndex >= SDL_NumJoysticks())
		return;

	const SDL_JoystickID instanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);
	int existingSlot = PsyX_Pad_FindSlotByInstanceId(instanceId);
	if (existingSlot < 0 && instanceId < 0)
	{
		for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
		{
			if (g_controllers[slot].deviceIndex == deviceIndex &&
				PsyX_Pad_IsAttached(&g_controllers[slot]))
			{
				existingSlot = slot;
				break;
			}
		}
	}

	if (existingSlot >= 0)
	{
		if (!g_controllers[existingSlot].gameController &&
			SDL_IsGameController(deviceIndex))
			PsyX_Pad_ReopenController(existingSlot, deviceIndex);
		return;
	}

	const int slot = PsyX_Pad_SelectSlot(deviceIndex);
	if (slot >= 0)
		PsyX_Pad_OpenController(deviceIndex, slot);
}

void PsyX_Pad_DeviceRemoved(int instanceId)
{
	const int slot = PsyX_Pad_FindSlotByInstanceId((SDL_JoystickID)instanceId);
	if (slot < 0)
		return;

	eprintinfo("Input slot %d disconnected (instance %d).\n", slot + 1,
		instanceId);
	PsyX_Pad_CloseController(slot);
	PsyX_Pad_AssignAvailableControllers();
}

void PsyX_Pad_DeviceRemapped(int instanceId)
{
	const int slot = PsyX_Pad_FindSlotByInstanceId((SDL_JoystickID)instanceId);
	if (slot < 0)
		return;

	const int deviceIndex = PsyX_Pad_FindDeviceIndex((SDL_JoystickID)instanceId);
	if (deviceIndex < 0)
		return;

	const bool mapped = SDL_IsGameController(deviceIndex) == SDL_TRUE;
	const bool openedAsController = g_controllers[slot].gameController != NULL;
	if (mapped != openedAsController)
		PsyX_Pad_ReopenController(slot, deviceIndex);
	else
		PsyX_Pad_RefreshSnapshot(&g_controllers[slot]);
}

static Uint16 PsyX_Pad_UpdateKeyboardInput(void)
{
	Uint16 buttons = 0xFFFF;
	if (!g_sdlKeyboardState)
		return buttons;

	const PsyXKeyboardMapping& mapping = g_cfg_keyboardMapping;
	PsyX_Pad_SetPressed(&buttons, 0x8000, g_sdlKeyboardState[mapping.kc_square] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x2000, g_sdlKeyboardState[mapping.kc_circle] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x1000, g_sdlKeyboardState[mapping.kc_triangle] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x4000, g_sdlKeyboardState[mapping.kc_cross] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0400, g_sdlKeyboardState[mapping.kc_l1] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0100, g_sdlKeyboardState[mapping.kc_l2] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0002, g_sdlKeyboardState[mapping.kc_l3] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0800, g_sdlKeyboardState[mapping.kc_r1] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0200, g_sdlKeyboardState[mapping.kc_r2] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0004, g_sdlKeyboardState[mapping.kc_r3] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0010, g_sdlKeyboardState[mapping.kc_dpad_up] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0040, g_sdlKeyboardState[mapping.kc_dpad_down] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0080, g_sdlKeyboardState[mapping.kc_dpad_left] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0020, g_sdlKeyboardState[mapping.kc_dpad_right] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0001, g_sdlKeyboardState[mapping.kc_select] != 0);
	PsyX_Pad_SetPressed(&buttons, 0x0008, g_sdlKeyboardState[mapping.kc_start] != 0);
	return buttons;
}

static void PsyX_Pad_CopySnapshotToPad(const PsyXControllerSnapshot* snapshot,
										LPPADRAW pad)
{
	pad->buttons[0] = snapshot->buttons[0];
	pad->buttons[1] = snapshot->buttons[1];
	memcpy(pad->analog, snapshot->analog, sizeof(snapshot->analog));
}

void PsyX_Pad_InternalPadUpdates(void)
{
	if (!g_padSystemInitialized)
		return;

	SDL_PumpEvents();
	SDL_GameControllerUpdate();
	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
		PsyX_Pad_RefreshSnapshot(&g_controllers[slot]);

	if (!g_padCommEnable)
		return;

	const Uint16 keyboardInputs = PsyX_Pad_UpdateKeyboardInput();
	for (int slot = 0; slot < MAX_CONTROLLERS; ++slot)
	{
		PsyXController* controller = &g_controllers[slot];
		Uint16 controllerButtons = (Uint16)(controller->snapshot.buttons[0] |
			((Uint16)controller->snapshot.buttons[1] << 8));
		const bool selectPressed = (controllerButtons & 0x0001) == 0;
		const bool startPressed = (controllerButtons & 0x0008) == 0;
		bool toggleAnalogMode = false;

		if (selectPressed && startPressed)
		{
			toggleAnalogMode = !controller->switchingAnalog;
			controller->switchingAnalog = true;
		}
		else if (controller->switchingAnalog && !selectPressed && !startPressed)
		{
			controller->switchingAnalog = false;
		}

		if (controller->switchingAnalog)
		{
			// Keep both buttons consumed until they are both released. The host
			// reads this snapshot directly, so masking only PADRAW would leak a
			// Start press into the pause handler.
			controllerButtons |= 0x0009;
			PsyX_Pad_SetSnapshotButtons(&controller->snapshot, controllerButtons);
		}

		if (!controller->padData)
			continue;

		LPPADRAW pad = (LPPADRAW)controller->padData;
		PsyX_Pad_CopySnapshotToPad(&controller->snapshot, pad);
		if (controller->snapshot.connected && pad->id != 0x41 && pad->id != 0x73)
			pad->id = 0x41;
		else if (!controller->snapshot.connected && slot != 0 &&
			!(g_activeKeyboardControllers & (1 << slot)))
			pad->id = 0xFF;

		if (toggleAnalogMode)
		{
			if (pad->id == 0x41)
			{
				eprintf("Port %d ANALOG: ON\n", slot + 1);
				pad->id = 0x73;
			}
			else if (pad->id == 0x73)
			{
				eprintf("Port %d ANALOG: OFF\n", slot + 1);
				pad->id = 0x41;
			}
		}

		if ((g_activeKeyboardControllers & (1 << slot)) &&
			keyboardInputs != 0xFFFF)
		{
			pad->status = 0;
			if (pad->id != 0x41)
			{
				if (pad->id != 0x73)
					eprintf("Port %d ANALOG: OFF\n", slot + 1);
				pad->id = 0x41;
			}
			PsyX_Pad_SetPadButtons(pad,
				(Uint16)(PsyX_Pad_GetPadButtons(pad) & keyboardInputs));
		}
	}
}

int PsyX_Pad_GetControllerSnapshot(int slot, PsyXControllerSnapshot* snapshot)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS || !snapshot)
		return 0;

	if (!g_padSystemInitialized)
		PsyX_Pad_ResetSnapshot(snapshot);
	else
		*snapshot = g_controllers[slot].snapshot;
	return 1;
}

int PsyX_Pad_GetStatus(int mtap, int slot)
{
	(void)mtap;
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return 0;
	if (slot == 0)
		return 1;
	return g_controllers[slot].snapshot.connected ? 1 : 0;
}

int PsyX_Pad_GetModeId(int mtap, int slot)
{
	(void)mtap;
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return 0;

	const PsyXController* controller = &g_controllers[slot];
	if (controller->padData)
	{
		const LPPADRAW pad = (LPPADRAW)controller->padData;
		if (pad->id == 0x41)
			return 4;
		if (pad->id == 0x73)
			return 7;
	}

	if (controller->snapshot.connected)
		return 7;
	return slot == 0 ? 4 : 0;
}

int PsyX_Pad_HasRumble(int slot)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS || !g_padSystemInitialized)
		return 0;

	PsyXController* controller = &g_controllers[slot];
	controller->snapshot.rumbleSupported =
		PsyX_Pad_QueryRumbleSupport(controller) ? 1 : 0;
	return controller->snapshot.rumbleSupported ? 1 : 0;
}

int PsyX_Pad_SetRumble(int slot, unsigned short low16, unsigned short high16,
						   unsigned int duration_ms)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS || !g_padSystemInitialized)
		return 0;

	PsyXController* controller = &g_controllers[slot];
	if (!PsyX_Pad_IsAttached(controller))
		return 0;

	if (!g_padOutputFocused)
	{
		PsyX_Pad_StopControllerRumble(controller);
		return low16 == 0 && high16 == 0 ? 1 : 0;
	}

	if (low16 == 0 && high16 == 0)
	{
		PsyX_Pad_StopControllerRumble(controller);
		return 1;
	}

	const Uint32 duration = duration_ms ? (Uint32)duration_ms : 1;
#if SDL_VERSION_ATLEAST(2, 0, 9)
	if (controller->gameController &&
		SDL_GameControllerRumble(controller->gameController, low16, high16,
			duration) == 0)
	{
		controller->snapshot.rumbleSupported = 1;
		return 1;
	}

	if (controller->joystick &&
		SDL_JoystickRumble(controller->joystick, low16, high16, duration) == 0)
	{
		controller->snapshot.rumbleSupported = 1;
		return 1;
	}
#endif

	if (controller->haptic && controller->hapticRumbleInitialized)
	{
		const Uint16 strongest = low16 > high16 ? low16 : high16;
		const float strength = (float)strongest / 65535.0f;
		if (SDL_HapticRumblePlay(controller->haptic, strength, duration) == 0)
		{
			controller->snapshot.rumbleSupported = 1;
			return 1;
		}
	}

	controller->snapshot.rumbleSupported = 0;
	return 0;
}

void PsyX_Pad_StopRumble(int slot)
{
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;
	PsyX_Pad_StopControllerRumble(&g_controllers[slot]);
}

void PsyX_Pad_Vibrate(int mtap, int slot, unsigned char* table, int len)
{
	(void)mtap;
	if (slot < 0 || slot >= MAX_CONTROLLERS)
		return;

	if (!table || len <= 0)
	{
		PsyX_Pad_StopRumble(slot);
		return;
	}

	const unsigned short high16 = table[0] ? 0xFFFF : 0;
	const unsigned short low16 =
		len > 1 ? (unsigned short)(table[1] * 257U) : 0;
	PsyX_Pad_SetRumble(slot, low16, high16, 200);
}