# PadUnlock

Unlock the PS Vita lockscreen **without touching the screen**, so you don't smudge it.

When the lockscreen shows, you can:

- **hold any button** (D-pad, ✕ ○ □ △, L, R, Start, Select) for about 0.5 s, or
- **push either analog stick** in any direction,

and the plugin makes the "peel from the top-right corner" swipe for you. Your lockscreen and passcode still work as usual. Unlike [NoLockScreen](https://github.com/TheOfficialFloW/VitaTweaks), this plugin does not remove the lockscreen.

## How it works

PadUnlock is a kernel plugin. It hooks the SceTouch kernel read functions (`ksceTouchPeek/Read[Region]`), the same method [reVita](https://github.com/MERLev/reVita) uses for touch emulation. On request, it replaces the front-panel data that **SceShell** reads with a fake finger that drags from the top-right corner toward the bottom-left.

The plugin only acts when the lockscreen is on screen:

1. It **arms** at boot and every time the Vita wakes from sleep, because that's when the lockscreen appears.
2. While armed, the button hold or a stick push starts one fake swipe.
3. It **disarms** after that swipe, or after you peel the screen with your own finger.

So holding a button on the LiveArea, where the same corner peel closes the app bubble, does nothing. Games and apps never see the fake finger, because injection only happens for the SceShell process. The plugin doesn't use firmware-specific offsets, so it should work on 3.60–3.74.

## Install

1. Copy `padunlock.skprx` to `ur0:tai/`.
2. Add it under `*KERNEL` in `ur0:tai/config.txt` (or `ux0:tai/config.txt` if that's the one you use):
   ```
   *KERNEL
   ur0:tai/padunlock.skprx
   ```
3. Reboot.

Don't use it together with NoLockScreen, which removes the lockscreen this plugin dismisses.

## Build

You need [VitaSDK](https://vitasdk.org/). On Windows, use WSL or MSYS2.

```sh
./build.sh          # uses $VITASDK, defaults to /usr/local/vitasdk
# -> build/padunlock.skprx
```

From Windows with VitaSDK in WSL: `wsl -d Ubuntu -- bash build.sh` (run it from the project folder).

If you push the repo to GitHub, the included workflow (`.github/workflows/build.yml`) builds it in the `vitasdk/vitasdk` Docker image and uploads the `.skprx` as an artifact.

## Tuning

These settings are `#define`s at the top of `src/main.c`:

| Define | Default | Meaning |
|---|---|---|
| `HOLD_MS` | 500 | How long a button must be held |
| `STICK_THRESHOLD` | 90 | How far a stick must be pushed (0–127) |
| `SWIPE_MS` | 300 | Duration of the fake swipe |
| `SWIPE_X0/Y0 → X1/Y1` | (1900,20) → (500,900) | Swipe path on the 1920×1088 front panel |
| `HOLD_BUTTONS` | all except PS/power/volume | Which buttons count |
| `DEBUG_LOG` | 0 | Set to 1 to write a diagnostic log to `ux0:data/padunlock.log` (power callbacks, arm/disarm, swipes, ignored input) |

## Credits

- Touch hook NIDs and approach: reVita by MERLev
- Plugin skeleton: VitaTweaks by TheFloW

License: GPLv3
