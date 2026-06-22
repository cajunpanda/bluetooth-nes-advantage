# Manual

How to use a Bluetooth NES Advantage. Everything is driven from the stick's own controls; there
are no added buttons.

## Charging

Plug a 5 V supply into the DC jack. The green LED blinks while charging and is solid when full.
You can play while charging.

![Charging](images/charging.jpg)

## Pairing

1. Charge it if needed.
2. Pick the mode with a gesture (see Gestures below): Switch / Receiver (default) or BLE.
3. Pair:
   - **Nintendo Switch:** open Controllers, then Change Grip/Order. The stick appears as a Pro
     Controller. The blue LED blinks while pairing and is solid once connected.
   - **8BitDo Retro Receiver:** put the receiver in pairing mode and wake the stick.
   - **BLE (PC, phone, emulator):** switch to BLE mode, then pair "NES Advantage" from the
     device's Bluetooth menu.
4. Play. The stick reconnects to the last host automatically on wake.

**Nintendo Switch 2:** direct pairing is not possible for any open controller emulation, because
the Switch 2 requires a Nintendo-signed device certificate. Use an 8BitDo USB Wireless Adapter in
the dock; it bridges the stick's Pro Controller emulation to the Switch 2.

## Gestures

Hold the listed buttons together for about 5 seconds, then release. The LED blinks to confirm.
Gestures work any time during play.

| Hold for 5 s | Does | Confirm |
|---|---|---|
| Start | Sleep (power down) | LEDs off |
| Select | Forget the paired host, re-pair from scratch | Blue blinks |
| Select + Start | Switch mode: Switch/Receiver or BLE (restarts) | Blue blinks |
| A + B + Up | Cycle button profile | Red blinks the profile number |
| A + B + Down | Cycle directional mode | Red blinks the mode number |
| A + B + Select | Enter config / firmware-update mode (restarts) | Blue blinks 3 times |

Hold **Start** to wake from sleep.

## LEDs

| LED | Meaning |
|---|---|
| Blue solid | Connected to a host |
| Blue blinking | Pairing, waiting for a host |
| Blue off | Idle, not connected |
| Green blinking | Charging |
| Green solid | Fully charged |
| Red blinking | Battery low (under 20%) |

## Button profiles and directional modes

The stick remembers your choices per connection mode.

Button profiles (how NES A/B map onto the host):

| Mode | Profile 1 | Profile 2 |
|---|---|---|
| Switch / Receiver | Literal: A to A, B to B | NSO NES: A to B, B to Y (matches Switch Online NES games) |
| BLE | Default | BlueRetro (alternate mapping for BlueRetro adapters) |

Directional modes (where the joystick goes):

| Mode | 1 | 2 | 3 |
|---|---|---|---|
| Switch / Receiver | D-Pad | Left stick | Both |
| BLE | D-Pad | Axes | Both |

Most NES games on Switch want the NSO NES profile with D-Pad. Use the stick/axes modes for games
or menus that expect an analog stick.

## Player 1 / Player 2 (take-turns play)

The original player-select slider still works. It is built for hot-seat games like Super Mario
Bros., where two players share one stick. What it can do depends on the host, because one Bluetooth
radio can only be one controller to a console receiver:

| Connected to | Take-turns? | How |
|---|---|---|
| PC / emulator (BLE) | Yes | The stick shows up as two gamepads. Flip the slider to hand off; map each gamepad to a player in the emulator. |
| BlueRetro | Yes | Leave the slider on P1 and double-map the buttons to both wired ports in BlueRetro's config. |
| Nintendo Switch | No | One Pro Controller is one player. The slider picks which player you report as. |
| 8BitDo Retro Receiver | No | One receiver occupies one NES port. Same as above. |

Set the slider before or while you pair.

## Sleep and battery

- Manual sleep: hold Start for 5 s. Hold Start to wake.
- Auto-sleep after 90 seconds with no host, or 5 minutes idle while connected. Stays awake while
  charging.
- Sleep is a deep sleep with the buttons still monitored; waking reconnects in about a second.

## Configuration and firmware updates

Hold **A + B + Select** for 5 s. The stick restarts into config mode and advertises as "NES
Advantage Config". Open the config page in Chrome or Edge (Web Bluetooth), connect, and change
settings or flash new firmware over the air. It returns to normal play when you hold Start for
3 s, or after 5 idle minutes. See [`../web/README.md`](../web/README.md).
