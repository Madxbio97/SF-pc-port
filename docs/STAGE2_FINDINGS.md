# Stage 2 findings: title and menu

All names in this document are descriptive clean-room names. They are based on
control flow, referenced strings and data access; they are not claimed original
source identifiers.

## TITLE.OVL function map

`TITLE.OVL` is loaded at `0x80146630`. Ghidra identifies 37 functions:

| Address | Descriptive name | Recovered role |
| --- | --- | --- |
| `0x801473a4` | `Title_SetViewportTarget` | Starts an eight-frame viewport interpolation. |
| `0x8014748c` | `Title_UpdateViewport` | Advances viewport coordinates toward their targets. |
| `0x8014775c` | `Title_PushMenu` | Saves the current menu and selection on a ten-entry stack. |
| `0x8014785c` | `Title_PopMenu` | Restores a prior menu and selection. |
| `0x80147924` | `Title_Initialize` | Allocates title data and initializes render/UI services. |
| `0x80147a70` | `Title_Shutdown` | Detaches UI objects and frees title data. |
| `0x80147b0c` | `Title_QueueOperation` | Appends an item to a ten-entry asynchronous-operation ring. |
| `0x80147b90` | `Title_DequeueOperation` | Removes an item from that ring. |
| `0x80147c08` | `Title_ResetOperations` | Clears the operation ring and callbacks. |
| `0x80147c34` | `Title_OpenDialog` | Opens a message/choice dialog. |
| `0x80147f28` | `Title_SaveGame` | Starts or completes a memory-card save. |
| `0x80148070` | `Title_DispatchOperation` | Maps an operation result to title state or follow-up UI. |
| `0x80148230` | `Title_ProcessSaveOperation` | Dispatches memory-card load/save operations. |
| `0x80148474` | `Title_LoadMenuSprites` | Loads four named TIMs and places their sprites. |
| `0x80148610` | `Title_FocusTextItem` | Expands the focused text bounds and moves the viewport. |
| `0x801486a8` | `Title_DetachMenuSprites` | Removes the four title sprites from the render list. |
| `0x80148740` | `Title_ConfirmNewGame` | Completes the New Game transition after memory-card checks. |
| `0x80148770` | `Title_AcceptMainMenuSelection` | Dispatches New Game, Load Game or Training Video. |
| `0x801489d8` | `Title_AcceptDialogSelection` | Returns a dialog selection to its callback. |
| `0x80148a54` | `Title_AcceptSaveMenuSelection` | Dispatches save-menu actions. |
| `0x80148b1c` | `Title_AdvanceLogoSequence` | Advances the startup logo/movie sequence. |
| `0x80148c00` | `Title_AcceptLoadMenuSelection` | Opens a selected save slot or cancels the load menu. |
| `0x80148cb4` | `Title_CloseTrainingMovie` | Pops the Training Video state. |
| `0x80148cd4` | `Title_SetSelection` | Bounds-checks and changes the current menu item. |
| `0x80148d88` | `Title_SetTextMenuSelection` | Recolors and focuses a standard text-menu item. |
| `0x80148e70` | `Title_SetLoadMenuSelection` | Wraps and focuses the load-menu cursor. |
| `0x80148f78` | `Title_StartTitleMovie` | Starts `SOL/TITLE.STR` and configures title playback. |
| `0x80149048` | `Title_UpdateTextMenuColors` | Applies selected and idle text colors. |
| `0x80149154` | `Title_StartLogoSequence` | Starts or advances the three startup movies. |
| `0x801491b8` | `Title_UpdateLoadMenu` | Builds save-time labels after card enumeration. |
| `0x80149300` | `Title_StartTrainingMovie` | Starts `SOL/TRAINING.STR`. |
| `0x80149328` | `Title_UpdateMenuSprites` | Animates selected/unselected sprite brightness. |
| `0x80149558` | `Title_RebuildMenu` | Rebuilds menu text and invokes per-menu callbacks. |
| `0x80149830` | `Title_AcceptSelection` | Emits selection feedback. |
| `0x80149880` | `Title_HandleInput` | Edge-detects pad input and changes/accepts selections. |
| `0x80149bec` | `Title_BuildLoadMenu` | Builds five save-slot rows plus Cancel. |
| `0x80149cf4` | `Title_Update` | Top-level title state update and exit dispatch. |

## HOG container

`COMMON/TITLE.HOG` has SHA-256
`921216c03521e04985e8dbc7abfe2b42817db836793efd4fdd85b1535aa6d98a`.

The recovered container layout is:

```text
u32 identifier
u32 file_count
u32 header_constant
u32 names_offset
u32 data_offset
u32 relative_file_offsets[file_count]
char zero_terminated_names[file_count]
padding
byte file_data[]
```

Offsets are relative to `data_offset`; the final file extends to archive end.
The new parser validates all table, name and payload bounds.

## Original title sprites

`Title_LoadMenuSprites` gives the exact asset names and screen coordinates:

| File | Mode | Size | VRAM | Screen |
| --- | --- | --- | --- | --- |
| `NEW.TIM` | 8-bit indexed | 102 x 66 | 896, 68 | 42, 157 |
| `LOAD.TIM` | 8-bit indexed | 108 x 66 | 896, 0 | 131, 157 |
| `VIDEO.TIM` | 8-bit indexed | 112 x 66 | 896, 136 | 233, 157 |
| `SEARCH.TIM` | 8-bit indexed | 108 x 49 | 896, 204 | 133, 166 |

The shared graphics reset at `0x8001629c` selects the original 384 x 240 draw
buffer before title initialization. `MOVIE.OVL` then switches DISP/DRAW to the
retail 320 x 240 movie mode for the title state and restores 384 x 240 when it
returns. PsyCross rasterizes each mode at that exact resolution and presents
both through a nearest-neighbour 4:3 viewport, matching the PS1 pixel aspect.

All four share the 256-color CLUT at VRAM `(768, 490)`. `Title_LoadMenuSprites`
sets additive blend mode 1 on every sprite. `Title_UpdateMenuSprites` ramps the
selected item toward brightness 200, visible idle items toward 70 and hidden
items toward zero in steps of 10. During the card search, New Game, Training
Video and Search are visible together; Load Game is hidden and navigation skips
it. Search fades out while Load Game fades in when the search completes. All
four fade out after title-movie frame `0x274` and fade back in on the next pass.

The native path reads the HOG and TIM data directly from the supplied disc and
uploads the original words to PsyCross VRAM. For host composition it also
derives a transient RGBA mask from the same indexed pixels and CLUT. No extracted
or replacement game asset is committed.

The title TIM pixel blocks reserve more bytes than their VRAM rectangles use.
The parser follows the rectangle dimensions, validates that the declared block
contains them and ignores only the verified trailing workspace/padding bytes.

## Current native slice

The Stage 2 executable first reads the complete raw 2352-byte sectors for
`989LOGO.STR`, `EIDETIC.STR`, `INTRO.STR`, `TITLE.STR` and `TRAINING.STR`. The
native media layer demultiplexes PSX STR, decodes MDEC video to 16-bit PsyCross
VRAM and streams the XA audio through OpenAL. All five streams are 15 FPS with
stereo audio; `989LOGO`, `EIDETIC` and `TITLE` are 320 x 240, while `INTRO` and
`TRAINING` are 320 x 160. The 160-line movies use the original 40-line top and
bottom letterbox inside the 320 x 240 movie framebuffer.

The three startup movies play once. `TITLE.STR` then loops as the title
background while all four recovered menu sprites retain their original
placement and brightness animation. PsyCross cannot isolate the green ramp from
high-chroma movie pixels in its whole-primitive additive path, so the title-only
compatibility pass maps the original CLUT's continuous green intensity to RGBA
alpha and composites it once in pure green. This preserves the smooth glow and
selected-item highlight without thresholding or replacement artwork.
Keyboard/gamepad navigation follows the overlay's bounds and unavailable-Load
skip rule. Training Video plays the original `TRAINING.STR` and returns to the
looping title menu; New Game keeps its original mission-opening transition.
Memory-card persistence remains in a later milestone, so Load Game currently
acknowledges an empty native save set. No movie, audio or menu asset is extracted
into the repository.
