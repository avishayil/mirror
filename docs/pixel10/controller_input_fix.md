# Controller input dead on Android — root cause & fix (2026-10-28)

## Symptom
After the boot fix (FIX2), Pokémon: Let's Go, Pikachu! reaches its own controller-select
applet ("Press a button on the controller you want to use."), but nothing — on-screen
overlay taps or a physical pad — advances it. The in-app Controls screen shows **no player
connected to a controller**. The same game on the user's Mac (Eden v0.2.1) connects a
controller at launch and works.

## Root cause (verified in code)
Android never applies the player **connection** config at startup.

- Desktop `Config::ReadControlValues` (`src/frontend_common/config.cpp`) calls
  `ReadPlayerValues` for all 10 players, which reads `player_N_connected` and `player_N_type`.
  `KernelCore`/`HIDCore::ReloadInputDevices` then connects each npad whose `player.connected`
  is true (`EmulatedController::ReloadFromSettings`: `Disconnect(); if (player.connected) Connect();`).
- Android `AndroidConfig::ReadAndroidPlayerValues` (`src/android/app/src/main/jni/android_config.cpp`)
  read **only** buttons/analogs/motions/use_system_vibrator — never `connected` or `type`.
  `SaveAndroidPlayerValues` likewise never wrote them. So `player.connected` stayed at its
  value-initialized default (**false** for every player), `ReloadInputDevices` connected
  nothing, and the game's controller applet waited forever.

The only Android code paths that connect an npad were the in-app Controls UI
(`NativeInput.connectControllers` → `ConnectController`) — all user-driven, never run at boot.
That's why it works on the Mac (config honored at startup) but not on Android.

The on-screen overlay routes to **port 8** (the Handheld npad) when player 1's style is
Handheld (`InputOverlay.kt`: `getStyleIndex(0) == Handheld -> 8`); the Handheld npad's
virtual-gamepad binding lives at port 8 (`EmulatedController::LoadVirtualGamepadParams`).
So for overlay input to land, the **Handheld npad (index 8)** must be connected — the Player 1
npad (index 0) is intentionally left disconnected (see `ConnectController`'s handheld branch).

## Fix (`src/android/app/src/main/jni/android_config.cpp`)
1. `ReadAndroidPlayerValues` now reads `connected` (default `player_index == 0`) and
   `controller_type` (default ProController), mirroring desktop `ReadPlayerValues`.
2. `SaveAndroidPlayerValues` now writes both, so the state round-trips durably.
3. `ReadAndroidControlValues`: when player 1's type is Handheld, set `use_docked_mode = Handheld`
   (mirrors desktop) and, if player 1 is connected, copy player 1 → Handheld slot (index 8),
   mark index 8 connected, and clear player 1's own `connected` flag. This reproduces the exact
   npad state the in-app Controls path (`ConnectController`) produces: Handheld npad connected,
   Player 1 npad off. The derivation is idempotent across save/read cycles.

Net effect: the user's existing `config.ini` (`player_0_connected=true`, `player_0_type=4`
Handheld) is now authoritative at startup — the Handheld npad connects, and overlay/physical
input reaches the game. No config.ini edit is required; the frontend simply honors it now.

Removed the temporary `[P10DBG]` overlay logging from `native_input.cpp`.
