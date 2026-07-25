# Gameplay menu parity

## Authority

The gameplay menu follows the USA v1.1 `MENU.OVL` state topology and its
384x240 ACD coordinate system. Native code consumes guest mission state; it
does not invent objectives, parameters, map locations or inventory state.

The retail root callback order is fixed:

1. Map
2. Objectives
3. Parameters
4. Briefing
5. Weapons
6. Options

Objectives and Parameters remain root previews, as in retail. Map, Briefing,
Weapons and Options enter a detail state. The PC port keeps two opt-in
extensions on a repeated confirm: an enlarged map and the two-page weapon
information view. These extensions do not replace or reorder retail states.

## ACD layout contract

- Virtual canvas: `384x240`
- Left grid: `36,25 190x184`
- Left content window: `52,32 165x155`
- Information grid: `233,35 116x81`
- Information content: `236,40 109x70`
- Section selectors: `252,134 100x100`
- Bottom hint window: `52,200 165x10`

Every render command has an explicit semantic panel. Panel ownership must
never be inferred from an X coordinate, and a missing information block must
never be replaced with synthesized text.

## Behaviour

- Opening animation draws the ACD frame and grid over 12 ticks.
- Selector and list focus movement uses the four-frame interpolation recovered
  from `difference >> 2` in `MENU.OVL`.
- A section change keeps the selector rail fixed while left content, information
  and the hint enter over four frames.
- Section input is paced for ten ticks after a transition.
- Options root preview shows the four retail configuration categories. Session
  actions appear only in the Options detail list.
- Weapon art is in the left display; status and ammunition are in the right
  information window. Repeated confirm opens ratings/description pages.
- Map pages start on the guest-authored current layer. Player and active
  objective markers are filtered by that layer.

## Text rules

Text is localized before measurement. A line may be condensed to preserve the
retail box, but it may not be clipped. Paragraphs wrap on glyph metrics. When a
translation is denser than the English copy, the native high-resolution atlas
is sampled at a smaller vertical size instead of discarding trailing lines.
Briefings use authored pagination. Objectives and parameters scroll by entry in
the root ACD when their complete localized list does not fit.

## Regression coverage

`sf_pause_menu_tests` validates the recovered rectangles, fixed section order,
explicit panel ownership, transition timing, map-marker filtering, briefing
pagination, weapon detail pages, Options nesting and dense objective scrolling.
