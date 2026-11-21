# PadUnlock

Unlock the PS Vita lockscreen **without touching the screen**, so you don't smudge it.

When the lockscreen shows, you can:

- **hold any button** (D-pad, ✕ ○ □ △, L, R, Start, Select) for about 0.5 s, or
- **push either analog stick** in any direction (this triggers right away),

and the plugin makes the "peel from the top-right corner" swipe for you. Your lockscreen and passcode still work as usual. Unlike [NoLockScreen](https://github.com/TheOfficialFloW/VitaTweaks), this plugin does not remove the lockscreen; it just lets you dismiss it with the controls.

## Install

1. Copy `padunlock.skprx` to `ur0:tai/`.
2. Add it under `*KERNEL` in `ur0:tai/config.txt` (or `ux0:tai/config.txt` if that's the one you use):
   ```
   *KERNEL
   ur0:tai/padunlock.skprx
   ```
3. Do a full reboot (Power Off, then on). If you don't use Ensō, run your HENkaku exploit again afterwards.

Don't use it together with NoLockScreen, which removes the lockscreen this plugin dismisses.

### Update

Overwrite `ur0:tai/padunlock.skprx` with the new file and do a full reboot. Kernel plugins are only loaded at boot, so replacing the file has no effect until then.

### Uninstall

Remove the `ur0:tai/padunlock.skprx` line from `config.txt`, delete the file, and reboot.

## How it works

PadUnlock is a kernel plugin. It hooks the SceTouch kernel read functions (`ksceTouchPeek`, `ksceTouchRead`, and their `Region` variants), the same method [reVita](https://github.com/MERLev/reVita) uses for touch emulation. When triggered, it replaces the front-panel data that **SceShell** reads with a fake finger that drags from the top-right corner toward the bottom-left. Each fake swipe uses a fresh touch ID, as a real touch panel does.

The plugin only acts while the lockscreen is on screen:

1. It **arms** at boot and every time the Vita wakes from sleep, because that's when the lockscreen appears. Wake-up is detected with a kernel power callback.
2. While armed, holding a button or pushing a stick starts one fake swipe.
3. It **disarms** after that swipe, or after you peel the screen with your own finger.

So holding a button on the LiveArea, where the same corner peel closes the app bubble, does nothing. Games and apps never see the fake finger, because injection only happens for the SceShell process.

### Wake-up detection

On a real Vita, the kernel power callback reports sleep and wake with these flags:

| Event | Flags | Meaning |
|---|---|---|
| Sleep (power button) | `0x80400000` | `BUTTON_POWER_PRESS` + `APP_SUSPEND` |
| Wake from sleep | `0x00820000`, then `0x00040000` | `APP_RESUMING` + `SYSTEM_RESUMING`, then `SYSTEM_RESUME` |
| Wake when the system never suspended | `0x00800000` | `APP_RESUMING` |

The plugin arms on `APP_RESUMING`, `APP_RESUME`, `SYSTEM_RESUMING` or `SYSTEM_RESUME`. As a fallback, it also arms if the real-time clock jumps forward by more than 3 seconds between two polls.

After the first real sleep, the console sets `AFTER_SYSTEM_RESUME` (`0x80`) in every later callback, including sleep and PS-button presses. The plugin ignores that flag, because arming on it would let a button hold swipe on the LiveArea.

Some plugins keep the Vita from fully suspending when you press power: the screen goes off, but the green LED stays on and the battery drains. For example, [vitacompanion](https://github.com/devnoname120/vitacompanion) does this while its `nosleep` option is on, which is the default at every boot; send `nosleep off` to its command port to turn it off. That isn't caused by PadUnlock, which still unlocks after either kind of sleep.

## Compatibility

- Tested on a real PS Vita with HENkaku/taiHEN: unlock at boot, unlock after sleep with a button or either stick, and no effect on the LiveArea after unlocking.
- No firmware-specific offsets are used, so it is expected to work on other firmwares supported by taiHEN, but these haven't been tested.
- PS TV has no touch screen, so the plugin does nothing there.

## Known limitations

- **Clock jumps:** if the system clock jumps forward by more than 3 seconds while the Vita is awake (for example, a network time sync), the clock fallback arms the plugin. Until the next swipe, holding a button or pushing a stick on the LiveArea would then close the app bubble. This should be rare.
- **Passcode:** the swipe only dismisses the lockscreen. If you've set a passcode, you still enter it as usual.

## Troubleshooting

### Nothing happens at all

Usually the plugin isn't being loaded. Check these first:

- **Is it in `config.txt`?** Copying the file into the `tai` folder isn't enough. It must be listed under `*KERNEL` (not `*main` or `*ALL`).
- **Is it the right `config.txt`?** If `ux0:tai/config.txt` exists, taiHEN uses it and ignores `ur0:tai/config.txt`. Add the line to the file that's actually used, and make sure the path in it matches where the `.skprx` is.
- **Did you do a full reboot?** Power Off, then On. Kernel plugins only load at boot.
- **Do you use Ensō?** Without it, HENkaku isn't running yet when the boot lockscreen shows, so the plugin can't unlock it. Run the exploit, then put the Vita to sleep and wake it. It should work on that lockscreen.
- **Is NoLockScreen installed?** Remove it. It removes the lockscreen this plugin dismisses.

### Debug log

If it still doesn't work, use the debug build. It writes a log to `ux0:data/padunlock.log`.

1. Download `padunlock-debug.skprx` from the [latest release](https://github.com/irxd/pad-unlock/releases/latest), rename it to `padunlock.skprx`, and overwrite your installed copy. Keep the same `config.txt` line.
2. Do a full reboot (and run the exploit again if you don't use Ensō).
3. At the boot lockscreen, hold ✕ for about 2 seconds, then push the left stick all the way.
4. Put the Vita to sleep with the power button, wait about 10 seconds, and wake it. At the lockscreen, hold ✕ for about 2 seconds, then push a stick.
5. **Before rebooting**, copy `ux0:data/padunlock.log` to your PC with VitaShell. The log is overwritten at every boot.
6. When reporting an issue, include the log (or say that it wasn't created), your `config.txt`, firmware version, whether you use Ensō, your other plugins, and what happened in steps 3 and 4.

If `padunlock.log` isn't created at all, the plugin isn't loading. Go back to the checklist above.

Switch back to the regular build afterwards. The debug build writes to the memory card while the lockscreen is up.

To build the debug version yourself, run `cmake -S . -B build-debug -DPADUNLOCK_DEBUG=ON && cmake --build build-debug`.

The log records power callbacks and their flags, arm/disarm changes, swipe start and end (with how many touch frames were replaced), ignored input with button and stick values, and how often SceShell reads the touch panel.

| Log shows | Meaning |
|---|---|
| No `armed 0 -> 1` after a `power cb` line | That wake-up flag isn't in `PWR_CB_ARM_MASK` yet; add it |
| `input ignored: armed=0` | The plugin wasn't armed when you pressed |
| `swipe end: injected=0` | SceShell didn't read the touch panel through the hooked functions |
| `swipe end: injected=N` but still locked | The swipe was delivered but not accepted; adjust the swipe path or duration |

## Build

You need [VitaSDK](https://vitasdk.org/). On Windows, use WSL or MSYS2.

```sh
./build.sh          # uses $VITASDK, defaults to /usr/local/vitasdk
# -> build/padunlock.skprx
```

From Windows with VitaSDK in WSL, run this from the project folder:

```sh
wsl -d Ubuntu -- bash build.sh
```

If you push the repo to GitHub, the included workflow (`.github/workflows/build.yml`) builds it in the `vitasdk/vitasdk` Docker image and uploads the `.skprx` as an artifact.

## Tuning

These settings are `#define`s at the top of `src/main.c`:

| Define | Default | Meaning |
|---|---|---|
| `HOLD_MS` | 500 | How long a button must be held |
| `STICK_THRESHOLD` | 90 | How far a stick must be pushed (0–127) |
| `SWIPE_MS` | 300 | Duration of the fake swipe |
| `SWIPE_TAIL_MS` | 60 | How long the fake finger rests at the end point before lifting |
| `SWIPE_X0/Y0 → X1/Y1` | (1900,20) → (500,900) | Swipe path on the 1920×1088 front panel |
| `MANUAL_SWIPE_DIST` | 400 | How far a real finger must move to count as a manual unlock (disarms the plugin) |
| `HOLD_BUTTONS` | all except PS/power/volume | Which buttons count |
| `PWR_CB_ARM_MASK` | resume flags | Which power callback flags arm the plugin |
| `DEBUG_LOG` | 0 | Set to 1 (or build with `-DPADUNLOCK_DEBUG=ON`) to write a diagnostic log to `ux0:data/padunlock.log` |

## Credits

- Touch hook NIDs and approach: [reVita](https://github.com/MERLev/reVita) by MERLev
- Plugin skeleton: [VitaTweaks](https://github.com/TheOfficialFloW/VitaTweaks) by TheFloW

## License

PadUnlock is licensed under the [GNU General Public License v3.0](LICENSE), the same license as reVita and VitaTweaks, which it builds on.
