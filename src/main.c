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
    - pushing either analog stick away from center
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
#include <stdio.h>
#include <stdarg.h>

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

/* Debug log written to LOG_PATH. Set to 0 to disable. */
#define DEBUG_LOG          0
#define LOG_PATH           "ux0:data/padunlock.log"

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

/* Power callback flags (psp2common/power.h). Any of these means the console
   is waking up, and the lockscreen will be shown.
   Observed on a real PS Vita (kernel callback): sleep via power button
   reports 0x80400000 (POWER_PRESS | APP_SUSPEND), wake reports 0x00800000
   (APP_RESUMING). The SYSTEM_* flags never arrive, but are kept in case
   other firmwares/models send them. */
#define PWR_CB_AFTER_SYSTEM_RESUME 0x00000080
#define PWR_CB_SYSTEM_RESUMING     0x00020000
#define PWR_CB_SYSTEM_RESUME       0x00040000
#define PWR_CB_APP_RESUME          0x00200000
#define PWR_CB_APP_RESUMING        0x00800000
#define PWR_CB_ARM_MASK (PWR_CB_AFTER_SYSTEM_RESUME | PWR_CB_SYSTEM_RESUMING | \
                         PWR_CB_SYSTEM_RESUME | PWR_CB_APP_RESUME |            \
                         PWR_CB_APP_RESUMING)

/* Fallback sleep detection: if wall-clock time jumps forward by more than
   this between two polls (~16 ms apart), the console was asleep. */
#define SLEEP_GAP_US   (3ULL * 1000 * 1000)

#define FRONT_PORT 0

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

/* Touch ID of the current fake finger. A real panel gives every new contact
   a new ID, so each swipe gets a fresh one; reusing an ID that SceShell has
   already seen lift off can make it ignore the new touch. */
static volatile uint8_t fake_id = 0x40;

/* Real-finger tracking (written by hooks, read by worker). */
static volatile int real_tracking = 0;
static volatile int real_x0, real_y0, real_x1, real_y1;
static volatile int real_manual_unlock = 0;

/* Diagnostics (written by hooks / power callback, logged by worker). */
static volatile uint32_t stat_shell_polls = 0;  /* front-panel reads by SceShell */
static volatile uint32_t stat_injected = 0;     /* buffers we overwrote          */
static volatile uint32_t pwr_cb_count = 0;
static volatile uint32_t pwr_cb_flags = 0;

/* ------------------------------------------------------------------ log */

#if DEBUG_LOG
/* Lines are buffered in memory and flushed by the worker thread, so nothing
   is lost while ux0: isn't mounted yet (early boot, right after resume). */
static char log_buf[2048];
static int log_len = 0;
static int log_started = 0;

static void log_printf(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  if (n > (int)sizeof(line) - 1)
    n = sizeof(line) - 1;

  uint32_t ms = (uint32_t)(ksceKernelGetSystemTimeWide() / 1000);
  char stamp[16];
  int sn = snprintf(stamp, sizeof(stamp), "[%7u] ", (unsigned)ms);

  if (log_len + sn + n + 1 > (int)sizeof(log_buf))
    return; /* buffer full, drop */
  memcpy(log_buf + log_len, stamp, sn);
  log_len += sn;
  memcpy(log_buf + log_len, line, n);
  log_len += n;
  log_buf[log_len++] = '\n';
}

static void log_flush(void) {
  if (log_len == 0)
    return;
  int flags = SCE_O_WRONLY | SCE_O_CREAT | (log_started ? SCE_O_APPEND : SCE_O_TRUNC);
  SceUID fd = ksceIoOpen(LOG_PATH, flags, 0666);
  if (fd < 0)
    return; /* ux0: not ready, retry later */
  ksceIoWrite(fd, log_buf, log_len);
  ksceIoClose(fd);
  log_started = 1;
  log_len = 0;
}
#else
/* Empty function (not an empty macro) so values that only exist for
   logging still count as used and don't trigger warnings. */
static inline void log_printf(const char *fmt, ...) { (void)fmt; }
#define log_flush()     do {} while (0)
#endif

/* ------------------------------------------------------------ touch hook */

static void process_touch(SceUInt32 port, SceTouchData *data, int count) {
  if (port != FRONT_PORT || count <= 0 || count > 64)
    return;
  if (shell_pid <= 0 || ksceKernelGetProcessId() != shell_pid)
    return;

  stat_shell_polls++;

  uint32_t fp = fake_point;

  if (fp & 0x80000000) {
    /* Replace whatever the panel reported with our single fake finger. */
    uint16_t x = (fp >> 16) & 0xFFF;
    uint16_t y = fp & 0xFFF;
    uint8_t id = fake_id;
    for (int i = 0; i < count; i++) {
      memset(&data[i].report[0], 0, sizeof(data[i].report[0]));
      data[i].report[0].id = id;
      data[i].report[0].force = 128;
      data[i].report[0].x = x;
      data[i].report[0].y = y;
      data[i].reportNum = 1;
    }
    stat_injected++;
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
  pwr_cb_flags |= (uint32_t)powerInfo; /* accumulate until the worker logs it */
  pwr_cb_count++;
  if (powerInfo & PWR_CB_ARM_MASK)
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

static void start_swipe(SceInt64 *swipe_start, SceInt64 now, const char *why,
                        const SceCtrlData *ctrl) {
  /* Fresh touch ID per gesture (1..126, never 0x00/0x7F). */
  uint8_t id = fake_id + 1;
  if (id < 1 || id > 126)
    id = 1;
  fake_id = id;

  *swipe_start = now;
  stat_injected = 0;
  log_printf("swipe start (%s) id=%u btns=%08X lx=%u ly=%u rx=%u ry=%u polls=%u",
             why, (unsigned)id, (unsigned)ctrl->buttons, ctrl->lx, ctrl->ly,
             ctrl->rx, ctrl->ry, (unsigned)stat_shell_polls);
}

static int worker_thread(SceSize args, void *argp) {
  SceUID cb = ksceKernelCreateCallback("padunlock_power_cb", 0, power_cb, NULL);
  int reg = cb >= 0 ? kscePowerRegisterCallback(cb) : -1;
  log_printf("worker start: cb=%08X reg=%08X", (unsigned)cb, (unsigned)reg);

  int smode = ksceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  log_printf("set analog mode: %08X", (unsigned)smode);

  SceInt64 hold_start = 0;      /* 0 = no qualifying input held           */
  int need_release = 0;         /* wait for all input released before next */
  SceInt64 swipe_start = 0;     /* 0 = no swipe in progress                */
  int was_armed = armed;
  int logged_disarmed_press = 0;
  uint32_t seen_pwr_cb = 0;
  uint32_t poll_mark = 0;
  SceInt64 poll_mark_time = 0;

  /* The system timer pauses during sleep, so use the wall clock (RTC) to
     notice that time passed while we weren't running. */
  SceRtcTick rtc_last = {0};
  int rtc_ret = ksceRtcGetCurrentTick(&rtc_last);
  log_printf("rtc first read: ret=%08X", (unsigned)rtc_ret);

  while (thread_run) {
    SceInt64 now = ksceKernelGetSystemTimeWide();

    /* ---- sleep detection ---- */
    SceRtcTick rtc_now = {0};
    if (ksceRtcGetCurrentTick(&rtc_now) >= 0) {
      if (rtc_last.tick && rtc_now.tick > rtc_last.tick + SLEEP_GAP_US) {
        log_printf("rtc gap %u s -> arm",
                   (unsigned)((rtc_now.tick - rtc_last.tick) / 1000000ULL));
        armed = 1;
      }
      rtc_last = rtc_now;
    }

    if (pwr_cb_count != seen_pwr_cb) {
      seen_pwr_cb = pwr_cb_count;
      uint32_t f = pwr_cb_flags;
      pwr_cb_flags = 0;
      log_printf("power cb #%u flags=%08X", (unsigned)seen_pwr_cb, (unsigned)f);
    }

    if (armed != was_armed) {
      log_printf("armed %d -> %d", was_armed, (int)armed);
      was_armed = armed;
    }

    if (shell_pid <= 0) {
      SceUID pid = ksceKernelSysrootGetShellPid();
      if (pid > 0) {
        shell_pid = pid;
        log_printf("shell pid %08X", (unsigned)pid);
      }
    }

    /* Log how often SceShell reads the touch panel (once every ~5 s while
       armed), to confirm the hooks still see it after resume. */
    if (armed && now - poll_mark_time > 5 * 1000 * 1000) {
      log_printf("shell touch polls: +%u", (unsigned)(stat_shell_polls - poll_mark));
      poll_mark = stat_shell_polls;
      poll_mark_time = now;
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
        log_printf("swipe end: injected=%u", (unsigned)stat_injected);
      }
      ksceKernelDelayThreadCB(4 * 1000);
      continue;
    }

    /* ---- manual unlock seen: stop listening ---- */
    if (real_manual_unlock) {
      real_manual_unlock = 0;
      armed = 0;
      log_printf("manual finger unlock seen");
    }

    /* ---- read input ---- */
    SceCtrlData ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    int got = ksceCtrlPeekBufferPositive(0, &ctrl, 1);

    int any_btn = got > 0 && (ctrl.buttons & HOLD_BUTTONS);
    int sticks  = got > 0 &&
                  (stick_out(ctrl.lx) || stick_out(ctrl.ly) ||
                   stick_out(ctrl.rx) || stick_out(ctrl.ry));

    if (!any_btn && !sticks) {
      hold_start = 0;
      need_release = 0;
      logged_disarmed_press = 0;
    } else if (!armed || shell_pid <= 0) {
      if (!logged_disarmed_press) {
        log_printf("input ignored: armed=%d btns=%08X lx=%u ly=%u rx=%u ry=%u",
                   (int)armed, (unsigned)ctrl.buttons,
                   ctrl.lx, ctrl.ly, ctrl.rx, ctrl.ry);
        logged_disarmed_press = 1;
      }
    } else if (!need_release) {
      if (sticks) {
        start_swipe(&swipe_start, now, "stick", &ctrl);
        need_release = 1;
      } else if (!hold_start) {
        hold_start = now;
      } else if (now - hold_start >= (SceInt64)HOLD_MS * 1000) {
        start_swipe(&swipe_start, now, "button", &ctrl);
        need_release = 1;
        hold_start = 0;
      }
    }

    log_flush();
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

  log_printf("PadUnlock start: hooks %08X %08X %08X %08X",
             (unsigned)hooks[0], (unsigned)hooks[1], (unsigned)hooks[2], (unsigned)hooks[3]);

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
