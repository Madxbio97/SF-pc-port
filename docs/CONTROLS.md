# Keyboard and mouse controls

The launcher exposes all 31 native gameplay actions. Bindings are captured as
physical keyboard scancodes or mouse inputs and are saved in
`%LOCALAPPDATA%\SyphonFilterPC\launcher.ini`.

| Action | Default | Runtime meaning |
| --- | --- | --- |
| Move forward / backward | W / S | Walk in chase and first-person; crouch-walk while kneeling |
| Turn left / right | A / D | Tank turn in chase mode; strafe in first-person |
| Strafe left / right | Q / E | Lateral movement in chase, manual-aim corner peek and side-roll direction |
| Run | Left Shift | Run while moving forward |
| Roll | Space | Forward roll, or left/right with a strafe direction; no backward roll |
| Reload | R | Reload when the current weapon can accept reserve ammunition |
| Aim | Mouse Right | Hold for manual first-person aim; WASD moves, mouse/right stick controls sight |
| Fire | Mouse Left | Fire the current weapon |
| Crouch / stealth | C | Toggle kneeling; movement becomes stealth crouch-walk |
| Action / interact | F | Contextual doors, switches, pickups and mission actions |
| Target lock | Tab | Press to select/cycle; hold to retain automatic target lock |
| Quick turn | Backspace | Retail 180-degree turn |
| Quick weapon switch | Mouse Middle | Short Select-style weapon switch |
| Previous / next weapon | [ / ] | Direct previous/next selection without the wheel ribbon |
| Weapon menu previous / next | Wheel Down / Wheel Up | Open and scroll the retail weapon ribbon |
| Pause menu | Escape | Open or close pause |
| Quick weapon 1..10 | 1..9, 0 | Equip the corresponding available quick slot |

Mouse movement controls the sight while Aim is held. The optional **Mouse look
in chase mode** checkbox in Input Configuration also captures the mouse during
normal gameplay: horizontal motion turns Gabe through the common retail
actor/camera facing path, while vertical motion pitches the unlocked chase
camera. Scripted and locked cameras retain ownership. Separate horizontal and
vertical sensitivity values
are available beside the checkbox and accept values from 0.10 to 20.00. Crouch
plus movement is the stealth locomotion path; Roll plus Strafe selects a side
roll. These are composed states, not separate bindable actions.
