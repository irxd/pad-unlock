/*
  PadUnlock - unlock the PS Vita lockscreen without touching the screen.

  How it works
  ------------
  The lockscreen is dismissed by SceShell when it sees a finger "peel" the
  screen from the top-right corner. Instead of patching SceShell (which needs
  per-firmware offsets, see VitaTweaks/NoLockScreen), this kernel plugin hooks
  the SceTouch kernel read functions and, on request, feeds SceShell a
  synthetic swipe gesture. The lockscreen itself (and passcode, if set) keeps
  working exactly as before.

  The plugin is "armed" at boot and every time the console resumes from
  sleep (that is when the lockscreen is shown). While armed:
    - holding any face/d-pad/shoulder/Start/Select button for HOLD_MS, or
    - pushing BOTH analog sticks away from center at the same time
  triggers the swipe. After the swipe (or after you peel the screen with your
  finger) the plugin disarms itself, so it never interferes with LiveArea
  (where the same corner-peel gesture closes the app bubble) or with games.

  Injection is limited to the SceShell process, so games/apps never see the
  fake finger.

  Licensed under GPLv3 (touch-hook approach adapted from reVita by MERLev,
  plugin skeleton inspired by VitaTweaks by TheFloW).
*/

#include <vitasdkkern.h>
#include <taihen.h>
#include <string.h>

/* ---------------------------------------------------------------- config */

#define HOLD_MS            500     /* how long a button must be held        */
#define STICK_THRESHOLD    90      /* 0..127 deflection from center (128)   */
#define SWIPE_MS           300     /* duration of the synthetic swipe       */
#define SWIPE_TAIL_MS      60      /* keep finger at end point before lift  */

/* Front panel is 1920x1088 (0..1919, 0..1087). Start in top-right corner and
   drag diagonally toward the bottom-left, like peeling a sticker. */
#define SWIPE_X0           1900
#define SWIPE_Y0           20
#define SWIPE_X1           500
#define SWIPE_Y1           900

/* A real-finger gesture longer than this (in panel units) is considered a
   manual unlock and disarms the plugin. */
#define MANUAL_SWIPE_DIST  400

/* Buttons that count for "hold any button" (raw SceCtrl bits so this works
   with old and new vitasdk headers). PS / power / volume are excluded. */
#define BTN_SELECT   0x00000001
#define BTN_START    0x00000008
#define BTN_UP       0x00000010
#define BTN_RIGHT    0x00000020
#define BTN_DOWN     0x00000040
#define BTN_LEFT     0x00000080
#define BTN_L        0x00000100
#define BTN_R        0x00000200
#define BTN_TRIANGLE 0x00001000
#define BTN_CIRCLE   0x00002000
#define BTN_CROSS    0x00004000
#define BTN_SQUARE   0x00008000
#define HOLD_BUTTONS (BTN_SELECT | BTN_START | BTN_UP | BTN_RIGHT | BTN_DOWN | \
                      BTN_LEFT | BTN_L | BTN_R | BTN_TRIANGLE | BTN_CIRCLE |   \
                      BTN_CROSS | BTN_SQUARE)

/* Power callback flags (same values as on PSP). */
#define PWR_CB_RESUMING        0x00020000
#define PWR_CB_RESUME_COMPLETE 0x00040000

#define FRONT_PORT 0
#define FAKE_TOUCH_ID 0x70

/* ------------------------------------------------------- touch structs */
/* psp2kern/touch.h doesn't define these; the layout matches psp2/touch.h
   (0x10 / 0x90 bytes). Prefixed to avoid clashing with newer SDK headers. */

typedef struct LsTouchReport {
  SceUInt8  id;
  SceUInt8  force;
  SceInt16  x;
  SceInt16  y;
  SceUInt8  reserved[8];
  SceUInt16 info;
} LsTouchReport;

typedef struct LsTouchData {
  SceUInt64     timeStamp;
  SceUInt32     status;
  SceUInt32     reportNum;
  LsTouchReport report[8];
} LsTouchData;

_Static_assert(sizeof(LsTouchReport) == 0x10, "LsTouchReport size");
_Static_assert(sizeof(LsTouchData) == 0x90, "LsTouchData size");

#define SceTouchData LsTouchData

/* ----------------------------------------------------------------- state */

enum {
  HK_PEEK,
  HK_READ,
  HK_PEEK_REGION,
  HK_READ_REGION,
  HK_NUM
};

static SceUID hooks[HK_NUM];
static tai_hook_ref_t refs[HK_NUM];

static SceUID thread_uid = -1;
static volatile int thread_run = 1;

static volatile SceUID shell_pid = -1;

/* armed: lockscreen is (probably) visible and we're allowed to swipe */
static volatile int armed = 0;

/* Published by the worker thread, consumed by touch hooks.
   bit31 = active, bits 16..27 = x, bits 0..11 = y. One 32-bit word so hooks
   always see a consistent point. */
static volatile uint32_t fake_point = 0;

/* Real-finger tracking (written by hooks, read by worker). */
static volatile int real_tracking = 0;
static volatile int real_x0, real_y0, real_x1, real_y1;
static volatile int real_manual_unlock = 0;

/* ------------------------------------------------------------ touch hook */

static void process_touch(SceUInt32 port, SceTouchData *data, int count) {
  if (port != FRONT_PORT || count <= 0 || count > 64)
    return;
  if (shell_pid <= 0 || ksceKernelGetProcessId() != shell_pid)
    return;

  uint32_t fp = fake_point;

  if (fp & 0x80000000) {
    /* Replace whatever the panel reported with our single fake finger. */
    uint16_t x = (fp >> 16) & 0xFFF;
    uint16_t y = fp & 0xFFF;
    for (int i = 0; i < count; i++) {
      memset(&data[i].report[0], 0, sizeof(data[i].report[0]));
      data[i].report[0].id = FAKE_TOUCH_ID;
      data[i].report[0].force = 128;
      data[i].report[0].x = x;
      data[i].report[0].y = y;
      data[i].reportNum = 1;
    }
    return;
  }

  if (!armed)
    return;

  /* Watch for the user peeling the lockscreen by hand: disarm afterwards. */
  SceTouchData *last = &data[count - 1];
  if (last->reportNum > 0) {
    if (!real_tracking) {
      real_x0 = real_x1 = last->report[0].x;
      real_y0 = real_y1 = last->report[0].y;
      real_tracking = 1;
    } else {
      real_x1 = last->report[0].x;
      real_y1 = last->report[0].y;
    }
  } else if (real_tracking) {
    int dx = real_x1 - real_x0;
    int dy = real_y1 - real_y0;
    if (dx * dx + dy * dy > MANUAL_SWIPE_DIST * MANUAL_SWIPE_DIST)
      real_manual_unlock = 1;
    real_tracking = 0;
  }
}

static int ksceTouchPeek_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs) {
  int ret = TAI_CONTINUE(int, refs[HK_PEEK], port, pData, nBufs);
  process_touch(port, pData, ret);
  return ret;
}

static int ksceTouchRead_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs) {
  int ret = TAI_CONTINUE(int, refs[HK_READ], port, pData, nBufs);
  process_touch(port, pData, ret);
  return ret;
}

static int ksceTouchPeekRegion_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region) {
  int ret = TAI_CONTINUE(int, refs[HK_PEEK_REGION], port, pData, nBufs, region);
  process_touch(port, pData, ret);
  return ret;
}

static int ksceTouchReadRegion_patched(SceUInt32 port, SceTouchData *pData, SceUInt32 nBufs, int region) {
  int ret = TAI_CONTINUE(int, refs[HK_READ_REGION], port, pData, nBufs, region);
  process_touch(port, pData, ret);
  return ret;
}

/* -------------------------------------------------------- power callback */

static int power_cb(int notifyId, int notifyCount, int powerInfo, void *common) {
  if (powerInfo & (PWR_CB_RESUMING | PWR_CB_RESUME_COMPLETE))
    armed = 1;
  return 0;
}

/* ---------------------------------------------------------- worker thread */

static inline uint32_t pack_point(int x, int y) {
  return 0x80000000 | ((uint32_t)(x & 0xFFF) << 16) | (uint32_t)(y & 0xFFF);
}

static int stick_out(uint8_t v) {
  int d = (int)v - 128;
  return d > STICK_THRESHOLD || d < -STICK_THRESHOLD;
}

static int worker_thread(SceSize args, void *argp) {
  SceUID cb = ksceKernelCreateCallback("padunlock_power_cb", 0, power_cb, NULL);
  if (cb >= 0)
    kscePowerRegisterCallback(cb);

  ksceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  SceInt64 hold_start = 0;      /* 0 = no qualifying input held           */
  int need_release = 0;         /* wait for all input released before next */
  SceInt64 swipe_start = 0;     /* 0 = no swipe in progress                */
  SceInt64 last_tick = ksceKernelGetSystemTimeWide();

  while (thread_run) {
    SceInt64 now = ksceKernelGetSystemTimeWide();

    /* Fallback resume detection: a big gap means we were asleep. */
    if (now - last_tick > 3 * 1000 * 1000)
      armed = 1;
    last_tick = now;

    if (shell_pid <= 0) {
      SceUID pid = ksceKernelSysrootGetShellPid();
      if (pid > 0)
        shell_pid = pid;
    }

    /* ---- run an active swipe ---- */
    if (swipe_start) {
      uint32_t elapsed_ms = (uint32_t)(now - swipe_start) / 1000;
      if (elapsed_ms < SWIPE_MS) {
        int x = SWIPE_X0 + (SWIPE_X1 - SWIPE_X0) * (int)elapsed_ms / SWIPE_MS;
        int y = SWIPE_Y0 + (SWIPE_Y1 - SWIPE_Y0) * (int)elapsed_ms / SWIPE_MS;
        fake_point = pack_point(x, y);
      } else if (elapsed_ms < SWIPE_MS + SWIPE_TAIL_MS) {
        fake_point = pack_point(SWIPE_X1, SWIPE_Y1);
      } else {
        fake_point = 0;   /* lift finger */
        swipe_start = 0;
        armed = 0;
      }
      ksceKernelDelayThreadCB(4 * 1000);
      continue;
    }

    /* ---- manual unlock seen: stop listening ---- */
    if (real_manual_unlock) {
      real_manual_unlock = 0;
      armed = 0;
    }

    /* ---- read input ---- */
    SceCtrlData ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    int got = ksceCtrlPeekBufferPositive(0, &ctrl, 1);

    int any_btn = got > 0 && (ctrl.buttons & HOLD_BUTTONS);
    int sticks  = got > 0 &&
                  (stick_out(ctrl.lx) || stick_out(ctrl.ly)) &&
                  (stick_out(ctrl.rx) || stick_out(ctrl.ry));

    if (!any_btn && !sticks) {
      hold_start = 0;
      need_release = 0;
    } else if (armed && !need_release && shell_pid > 0) {
      if (sticks) {
        swipe_start = now;
        need_release = 1;
      } else if (!hold_start) {
        hold_start = now;
      } else if (now - hold_start >= (SceInt64)HOLD_MS * 1000) {
        swipe_start = now;
        need_release = 1;
        hold_start = 0;
      }
    }

    ksceKernelDelayThreadCB(16 * 1000);
  }

  fake_point = 0;
  if (cb >= 0) {
    kscePowerUnregisterCallback(cb);
    ksceKernelDeleteCallback(cb);
  }
  return 0;
}

/* ------------------------------------------------------------ module glue */

static SceUID hook_touch(const char *module, uint32_t nid, int idx, const void *func) {
  return taiHookFunctionExportForKernel(KERNEL_PID, &refs[idx], module,
                                        TAI_ANY_LIBRARY, nid, func);
}

void _start() __attribute__ ((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
  for (int i = 0; i < HK_NUM; i++)
    hooks[i] = -1;

  tai_module_info_t info;
  info.size = sizeof(info);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceTouch", &info) < 0)
    return SCE_KERNEL_START_SUCCESS; /* PS TV: no touch screen, nothing to do */

  hooks[HK_PEEK]        = hook_touch("SceTouch", 0xBAD1960B, HK_PEEK,        ksceTouchPeek_patched);
  hooks[HK_READ]        = hook_touch("SceTouch", 0x70C8AACE, HK_READ,        ksceTouchRead_patched);
  hooks[HK_PEEK_REGION] = hook_touch("SceTouch", 0x9B3F7207, HK_PEEK_REGION, ksceTouchPeekRegion_patched);
  hooks[HK_READ_REGION] = hook_touch("SceTouch", 0x9A91F624, HK_READ_REGION, ksceTouchReadRegion_patched);

  /* Lockscreen is shown at boot too. */
  armed = 1;

  thread_uid = ksceKernelCreateThread("padunlock_thread", worker_thread,
                                      0x3C, 0x2000, 0, 0x10000, NULL);
  if (thread_uid >= 0)
    ksceKernelStartThread(thread_uid, 0, NULL);

  return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
  if (thread_uid >= 0) {
    thread_run = 0;
    ksceKernelWaitThreadEnd(thread_uid, NULL, NULL);
    ksceKernelDeleteThread(thread_uid);
  }
  for (int i = 0; i < HK_NUM; i++)
    if (hooks[i] >= 0)
      taiHookReleaseForKernel(hooks[i], refs[i]);
  return SCE_KERNEL_STOP_SUCCESS;
}
