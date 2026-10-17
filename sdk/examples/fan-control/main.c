/* SPDX-License-Identifier: MIT
 * ProsperoMenu example overlay: Fan Control.
 *
 * Shows CPU and SoC temperatures and holds a fan threshold: the temperature at
 * which the console's fan controller starts ramping up. Lower runs the fan
 * earlier and cooler; the firmware's own value returns when the hold is off.
 *
 * Method learned from Elf Arsenal (src/fan.c) and OnionHEN
 * (daemon_fan.cpp): the threshold is byte 5 of a zeroed 28-byte request to
 * /dev/icc_fan (ioctl 0xC01C8F07). The firmware puts its own curve back on
 * events like game launches, so the hold re-applies it every 15 seconds.
 * The overlay declares "keepAlive" and "autostart" so a held threshold
 * survives leaving the menu and is restored when ProsperoMenu starts.
 */
#include "pmo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define FAN_DEVICE "/dev/icc_fan"
#define FAN_SET_THRESHOLD 0xC01C8F07ul
#define FAN_MIN_C 40
#define FAN_MAX_C 85
#define FAN_DEFAULT_C 65
#define FAN_FIRMWARE_C 77     /* what OnionHEN restores for "automatic" */
#define REAPPLY_S 15
#define REFRESH_S 2

#ifdef __PS5__
int sceKernelGetCpuTemperature(int *celsius);
int sceKernelGetSocSensorTemperature(int sensor, int *celsius);
#else
/* PC development build: plausible fake readings. */
static int sceKernelGetCpuTemperature(int *c) { *c = 52 + (int)(time(NULL) % 5); return 0; }
static int sceKernelGetSocSensorTemperature(int s, int *c) { (void)s; *c = 48 + (int)(time(NULL) % 3); return 0; }
#endif

static int hold, threshold;
static char status[96] = "Not applied yet";

static int write_threshold(int celsius) {
#ifdef __PS5__
  int fd = open(FAN_DEVICE, O_RDONLY);
  if(fd < 0) return -errno;
  /* The request is read and written back in full: always pass all 28 bytes,
   * zeroed, or stack garbage reaches the fan controller (see Elf Arsenal). */
  unsigned char request[28];
  memset(request, 0, sizeof request);
  request[5] = (unsigned char)celsius;
  int rc = ioctl(fd, FAN_SET_THRESHOLD, request);
  int err = errno;
  close(fd);
  return rc < 0 ? -err : 0;
#else
  (void)celsius; return 0;
#endif
}

static int failed;  /* the last write failed: show the status as a warning */

static void apply(pmo_t *o, int celsius, const char *why) {
  int rc = write_threshold(celsius);
  failed = rc != 0;
  if(rc) pmo_fmt(status, sizeof status, "Could not set %d C: %s. Is kstuff loaded?", celsius, strerror(-rc));
  else pmo_fmt(status, sizeof status, "%s %d C", why, celsius);
  pmo_log(o, "fan: %s", status);
}

static void temperatures(char *cpu, char *soc, size_t size) {
  int c = -1, s = -1;
  if(sceKernelGetCpuTemperature(&c) || c < 0 || c > 130) snprintf(cpu, size, "--");
  else snprintf(cpu, size, "%d C", c);
  if(sceKernelGetSocSensorTemperature(0, &s) || s < 0 || s > 130) snprintf(soc, size, "--");
  else snprintf(soc, size, "%d C", s);
}

static void page(pmo_t *o) {
  char cpu[16], soc[16];
  temperatures(cpu, soc, sizeof cpu);
  pmo_page_begin(o, "Fan Control", hold ? "Holding threshold" : "Firmware fan curve");
  pmo_header(o, "Temperatures");
  pmo_item(o, "cpu", "CPU", cpu, "Updated every 2 seconds while this page is open.");
  pmo_item(o, "soc", "SoC", soc, "System-on-chip sensor 0.");
  pmo_header(o, "Fan");
  pmo_toggle(o, "hold", "Hold fan threshold", hold,
             "On: keep the threshold below, re-applied every 15 seconds and after restarts. Off: give control back to the firmware.");
  pmo_slider(o, "threshold", "Threshold", FAN_MIN_C, FAN_MAX_C, 1, threshold, " C",
             "The fan ramps up from this temperature. Lower means a cooler, louder console.");
  /* Plain status text; only a failed write is drawn as a warning. */
  if(failed) pmo_item_disabled(o, "status", "Status", NULL, status);
  else pmo_item(o, "status", "Status", NULL, status);
  pmo_footer(o, "X Select   Circle Back   Left/Right Adjust");
  pmo_page_end(o);
}

int main(void) {
  pmo_t *o = pmo_connect(NULL);
  if(!o) return 1;
  hold = pmo_config_get_int(o, "hold", 0) != 0;
  threshold = pmo_config_get_int(o, "threshold", FAN_DEFAULT_C);
  if(threshold < FAN_MIN_C || threshold > FAN_MAX_C) threshold = FAN_DEFAULT_C;
  if(hold) apply(o, threshold, "Restored");
  page(o);

  time_t last_apply = time(NULL), last_refresh = 0;
  for(;;) {
    pmo_event_t ev;
    int r = pmo_wait(o, &ev, 1000);
    if(r < 0) break;
    if(r > 0) {
      if(ev.kind == PMO_EV_QUIT) break;
      if(ev.kind == PMO_EV_BACK) { pmo_exit(o); continue; }
      if(ev.kind == PMO_EV_SHOW) { page(o); continue; }
      if(ev.kind == PMO_EV_TOGGLE && !strcmp(ev.id, "hold")) {
        hold = ev.on;
        pmo_config_set_int(o, "hold", hold);
        if(hold) apply(o, threshold, "Holding");
        else apply(o, FAN_FIRMWARE_C, "Released to the firmware at");
        last_apply = time(NULL);
        page(o);
      } else if(ev.kind == PMO_EV_SLIDE && !strcmp(ev.id, "threshold")) {
        threshold = ev.value;
        pmo_config_set_int(o, "threshold", threshold);
        if(hold) { apply(o, threshold, "Holding"); last_apply = time(NULL); }
        else { failed = 0; pmo_fmt(status, sizeof status, "Saved %d C. Turn on the hold to use it.", threshold); }
        pmo_patch_detail(o, "status", status);
        pmo_patch_enabled(o, "status", !failed);
      }
    }
    time_t now = time(NULL);
    if(hold && now - last_apply >= REAPPLY_S) {
      if(write_threshold(threshold)) apply(o, threshold, "Holding");
      last_apply = now;
    }
    if(pmo_visible(o) && now - last_refresh >= REFRESH_S) {
      char cpu[16], soc[16];
      temperatures(cpu, soc, sizeof cpu);
      pmo_patch_value(o, "cpu", cpu);
      pmo_patch_value(o, "soc", soc);
      pmo_patch_detail(o, "status", status);
      pmo_patch_enabled(o, "status", !failed);
      last_refresh = now;
    }
  }
  pmo_close(o);
  return 0;
}
