#ifndef EMULATOR_PUBLIC_H
#define EMULATOR_PUBLIC_H

#define CONTROLLER_MAP_FLAG_AXIS 0x4000
#define CONTROLLER_MAP_FLAG_INVERSE 0x8000

typedef struct
{
	int id;

	int kc_square, kc_circle, kc_triangle, kc_cross;

	int kc_l1, kc_l2, kc_l3;
	int kc_r1, kc_r2, kc_r3;

	int kc_start, kc_select;

	int kc_dpad_left, kc_dpad_right, kc_dpad_up, kc_dpad_down;
} PsyXKeyboardMapping;

typedef struct
{
	int id;

	// you can bind axis by adding CONTROLLER_MAP_AXIS_FLAG
	int gc_square, gc_circle, gc_triangle, gc_cross;

	int gc_l1, gc_l2, gc_l3;
	int gc_r1, gc_r2, gc_r3;

	int gc_start, gc_select;

	int gc_dpad_left, gc_dpad_right, gc_dpad_up, gc_dpad_down;

	int gc_axis_left_x, gc_axis_left_y;
	int gc_axis_right_x, gc_axis_right_y;
} PsyXControllerMapping;

typedef void (*GameDebugKeysHandlerFunc)(int nKey, char down);
typedef void (*GameDebugMouseHandlerFunc)(int x, int y, int dx, int dy);
typedef void (*GameOnTextInputHandler)(const char* buf);

typedef enum
{
	PSYX_ASPECT_ORIGINAL_4_3 = 0,
	PSYX_ASPECT_ADAPTIVE = 1,
} PsyXAspectMode;

typedef struct
{
	int x;
	int y;
	int w;
	int h;
} PsyXPresentationViewport;

typedef struct
{
	float x;
	float y;
} PsyXPresentationScale;

//------------------------------------------------------------------------

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) || \
	defined(c_plusplus)
extern "C"
{
#endif
	/* Mapped inputs */
	extern PsyXControllerMapping g_cfg_controllerMapping;
	extern PsyXKeyboardMapping g_cfg_keyboardMapping;
	extern int g_cfg_controllerToSlotMapping[2];

	/* Game inputs */
	extern GameOnTextInputHandler g_cfg_gameOnTextInput;

	/* Graphics configuration */
	extern int g_cfg_swapInterval;
	extern int g_cfg_pgxpZBuffer;
	extern int g_cfg_bilinearFiltering;
	extern int g_cfg_anisotropicFiltering;
	extern int g_cfg_pgxpTextureCorrection;
	extern int g_cfg_msaaSamples;
	extern int g_cfg_aspectMode;
	/* Native clients can opt out of legacy display-to-VRAM feedback. */
	extern int g_cfg_framebufferFeedback;
	/* Native clients with their own fixed-step clock do not need the PSX VBlank
	 * thread. */
	extern int g_cfg_vblankThread;

	/* Debug inputs */
	extern GameDebugKeysHandlerFunc g_dbg_gameDebugKeys;
	extern GameDebugMouseHandlerFunc g_dbg_gameDebugMouse;

	/* Usually called at the beginning of main function */
	extern void PsyX_Initialise(char* windowName, int screenWidth, int screenHeight,
								int fullscreen);

	/* Cleans all resources and closes open instances */
	extern void PsyX_Shutdown(void);

	/* Returns the screen size dimensions */
	extern void PsyX_GetScreenSize(int* screenWidth, int* screenHeight);

	/* Resolves original 4:3 letterboxing or a full adaptive output viewport. */
	extern PsyXPresentationViewport
	PsyX_CalculatePresentationViewport(int drawableWidth, int drawableHeight,
									   int aspectMode);

	/* Preserves the authored 4:3 pixel aspect: Hor+ for wide adaptive outputs,
 *
	 * Vert+ for narrow adaptive outputs, and identity for original mode. */
	extern PsyXPresentationScale PsyX_CalculatePresentationScale(int drawableWidth,
																 int drawableHeight,
																 int aspectMode);

	/* Sets mouse cursor position */
	extern void PsyX_SetCursorPosition(int x, int y);

	/* Sets mouse relative movement */
	extern void PsyX_SetCursorRelative(int enable);

	/* Usually called after ClearOTag/ClearOTagR */
	extern char PsyX_BeginScene(void);

	/* Usually called after DrawOTag/DrawOTagEnv */
	extern void PsyX_EndScene(void);

	/* Counts calls and returns the most recent one-second frame-rate sample. */
	extern unsigned int PsyX_CalcFPS(void);

	/* Explicitly updates emulator input loop */
	extern void PsyX_UpdateInput(void);

	/* Returns keyboard mapping index */
	extern int PsyX_LookupKeyboardMapping(const char* str, int default_value);

	/* Returns controller mapping index */
	extern int PsyX_LookupGameControllerMapping(const char* str, int default_value);

	/* Screen size of emulated PSX viewport with widescreen offsets */
	extern void PsyX_GetPSXWidescreenMappedViewport(struct _RECT16* rect);

	/* Waits for timer */
	extern void PsyX_WaitForTimestep(int count);

	/* Changes swap interval state */
	extern void PsyX_EnableSwapInterval(int enable);

	/* Changes the OpenGL swap interval used when synchronization is enabled. */
	extern void PsyX_SetSwapInterval(int interval);

	/* Caps completed presents. Zero disables the software frame limiter. */
	extern void PsyX_SetFrameLimit(int framesPerSecond);

	/* Re-anchors pacing after a blocking load, movie or display-mode change. */
	extern void PsyX_ResetFrameLimiter(void);

#if defined(_LANGUAGE_C_PLUS_PLUS) || defined(__cplusplus) || \
	defined(c_plusplus)
}
#endif

#endif
