# Controls

The single-window launcher's **Controls / Управление** tab configures keyboard,
mouse and controller input without opening a separate dialog. Changes remain
staged until **Play / Играть** and are then saved in
`%LOCALAPPDATA%\SyphonFilterPC\launcher.ini`.

## Keyboard and mouse

The launcher exposes all 31 native gameplay actions. Bindings are captured as
physical keyboard scancodes or mouse inputs.

| Action | Default | Runtime meaning |
| --- | --- | --- |
| Move forward / backward | W / S | Walk in chase; crouch-walk while kneeling |
| Turn left / right | A / D | Tank turn in chase mode |
| Strafe left / right | Q / E | Lateral movement in chase, manual-aim corner peek and side-roll direction |
| Run | Left Shift | Run while moving forward |
| Roll | Space | Forward roll, or left/right with a strafe direction; no backward roll |
| Reload | R | Reload when the current weapon can accept reserve ammunition |
| Aim | Mouse Right | Hold for manual first-person aim; mouse/right stick controls sight |
| Fire | Mouse Left | Fire the current weapon |
| Crouch / stealth | C | Hold to kneel; movement becomes stealth crouch-walk |
| Action / interact | F | Contextual doors, switches, pickups and mission actions |
| Target lock | Tab | Press to select/cycle; hold to retain automatic target lock |
| Quick turn | Backspace | Retail 180-degree turn |
| Quick weapon switch | Mouse Middle | Short Select-style weapon switch |
| Previous / next weapon | [ / ] | Direct previous/next selection without the wheel ribbon |
| Weapon menu previous / next | Wheel Down / Wheel Up | Open and scroll the retail weapon ribbon |
| Pause menu | Escape | Open or close pause |
| Quick weapon 1..10 | 1..9, 0 | Equip the corresponding available quick slot |
| Performance counter | F6 | Show or hide presentation FPS and frame time |

Mouse movement controls the sight only while Aim is held. First-person aim
keeps Gabe's collision root stationary; Q/E retains the original corner peek.
Releasing aim transfers the final sight heading to chase control. Hold C plus
movement for stealth crouch-walk; Roll plus Strafe selects a side roll. In rifle
optics, the original Triangle/Circle actions zoom in/out (F or R / Space with
default keyboard bindings). These are composed states, not separate bindable
actions.

## Controller

Select **Controller** as the input device to edit the nine retail-style actions.
Button names adapt to Xbox, PlayStation, Nintendo or generic devices. Assigning
a button that is already in use swaps the two actions, so the controller layout
always remains valid.

| Action | Default physical control |
| --- | --- |
| Change weapon | Select / Back / Share |
| Fire | Square / X (west face button) |
| Kneel | Cross / A (south face button) |
| Roll / zoom out | Circle / B (east face button) |
| Step right | R2 / right trigger |
| Step left | L2 / left trigger |
| Target lock | R1 / right shoulder |
| Use / zoom in | Triangle / Y (north face button) |
| Aim | L1 / left shoulder |

The stick-layout row cycles through three schemes:

- **Character left, camera right** is the default modern two-stick layout.
- **Character right, camera left** swaps the two stick roles.
- **Original one stick** keeps character movement and camera control on the
  original shared scheme.

Controller capture accepts face buttons, shoulders, triggers and Select-style
buttons. Press Escape or the controller's Start button while capture is active
to cancel without changing the binding.

### Controller backend

The same tab selects the input transport:

- **Automatic** enables SDL's compatible backends and is recommended for most
  XInput, DualSense/DualShock and generic controllers.
- **XInput** restricts discovery to the Windows XInput path.
- **DirectInput** selects legacy DirectInput devices.
- **Raw Input** selects SDL's Windows Raw Input path.

Changing the backend restarts launcher-side controller discovery while keeping
the staged bindings. Hot-plugged controllers are detected while the Controls
tab is visible.

**Controller vibration / Вибрация** enables or disables runtime rumble. The
toggle and every binding use the same staged settings as the in-game controller
menu and are persisted together on Play.
