/* SPDX-License-Identifier: MIT
 * ProsperoMenu example overlay: Console Tools.
 *
 * A read-only dashboard of the console: firmware, network address, uptime,
 * temperatures and free space on console storage and on every connected USB
 * or extended drive. Values refresh every 2 seconds while the page is open.
 * Selecting a drive opens a page with its details.
 *
 * Shows how an overlay builds a second page and handles Circle itself (its
 * own page stack), and how to keep a page live with patches.
 */
#include "pmo.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#ifdef __PS5__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __PS5__
#include <sys/sysctl.h>
#include <ps5/kernel.h>
int sceNetCtlInit(void);
int sceNetCtlGetInfo(int code, void *info);
int sceKernelGetCpuTemperature(int *celsius);
int sceKernelGetSocSensorTemperature(int sensor, int *celsius);
#else
static uint32_t kernel_get_fw_version(void) { return 0x13400002; }
static int sceNetCtlInit(void) { return 0; }
static int sceNetCtlGetInfo(int code, void *info) { (void)code; strcpy(info, "127.0.0.1"); return 0; }
static int sceKernelGetCpuTemperature(int *c) { *c = 52; return 0; }
static int sceKernelGetSocSensorTemperature(int s, int *c) { (void)s; *c = 48; return 0; }
#endif

typedef struct { const char *path, *label; } drive_t;
static const drive_t drives[] = {
  {"/data", "Console storage"}, {"/user", "System storage"},
  {"/mnt/ext0", "Extended storage 1"}, {"/mnt/ext1", "Extended storage 2"},
  {"/mnt/usb0", "USB 1"}, {"/mnt/usb1", "USB 2"}, {"/mnt/usb2", "USB 3"}, {"/mnt/usb3", "USB 4"},
  {"/mnt/usb4", "USB 5"}, {"/mnt/usb5", "USB 6"}, {"/mnt/usb6", "USB 7"}, {"/mnt/usb7", "USB 8"},
};
#define DRIVES (int)(sizeof drives / sizeof drives[0])

static const char *bytes_text(char *out, size_t size, double bytes) {
  const char *unit[] = {"B", "KB", "MB", "GB", "TB"}; int u = 0;
  while(bytes >= 1024 && u < 4) { bytes /= 1024; u++; }
  snprintf(out, size, u >= 3 ? "%.1f %s" : "%.0f %s", bytes, unit[u]);
  return out;
}
/* 1: mounted (fills free/total), 0: not present. USB folders exist even when
 * nothing is plugged in, so a drive counts only when it is its own mount. */
static int drive_space(const char *path, double *free_bytes, double *total) {
  struct statfs st;
  if(statfs(path, &st)) return 0;
#ifdef __PS5__
  if(strncmp(path, "/mnt/", 5) == 0 && strcmp(st.f_mntonname, path)) return 0;
#else
  if(strncmp(path, "/mnt/", 5) == 0) return 0; /* PC build: no console drives */
#endif
  *free_bytes = (double)st.f_bavail * st.f_bsize; *total = (double)st.f_blocks * st.f_bsize;
  return *total > 0;
}
static void firmware(char *out, size_t size) {
  uint32_t v = kernel_get_fw_version();
  snprintf(out, size, "%x.%02x", (v >> 24) & 0xff, (v >> 16) & 0xff);
}
static void address(char *out, size_t size) {
  char info[512]; memset(info, 0, sizeof info);
  sceNetCtlInit();
  if(sceNetCtlGetInfo(14, info) < 0 || !info[0]) snprintf(out, size, "Not connected");
  else snprintf(out, size, "%.31s", info);
}
static void uptime(char *out, size_t size) {
#ifdef __PS5__
  struct timeval boot; size_t len = sizeof boot; int mib[] = {CTL_KERN, KERN_BOOTTIME};
  if(sysctl(mib, 2, &boot, &len, NULL, 0)) { snprintf(out, size, "--"); return; }
  long s = (long)(time(NULL) - boot.tv_sec);
#else
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); long s = (long)ts.tv_sec;
#endif
  if(s < 0) s = 0;
  if(s >= 86400) snprintf(out, size, "%ldd %ldh %ldm", s / 86400, s % 86400 / 3600, s % 3600 / 60);
  else snprintf(out, size, "%ldh %ldm", s / 3600, s % 3600 / 60);
}
static void temperature(int soc, char *out, size_t size) {
  int c = -1, rc = soc ? sceKernelGetSocSensorTemperature(0, &c) : sceKernelGetCpuTemperature(&c);
  if(rc || c < 0 || c > 130) snprintf(out, size, "--"); else snprintf(out, size, "%d C", c);
}
static void drive_value(int i, char *out, size_t size) {
  double f, t; char a[16], b[16];
  if(!drive_space(drives[i].path, &f, &t)) { snprintf(out, size, "--"); return; }
  snprintf(out, size, "%s free of %s", bytes_text(a, sizeof a, f), bytes_text(b, sizeof b, t));
}

static int detail = -1; /* drive shown on the second page, -1 = main page */

static void main_page(pmo_t *o) {
  char v[64], id[16];
  pmo_page_begin(o, "Console Tools", "Read-only console information");
  pmo_header(o, "System");
  firmware(v, sizeof v); pmo_item(o, "fw", "Firmware", v, "The system software version.");
  uptime(v, sizeof v); pmo_item(o, "uptime", "Uptime", v, "Time since the console last started.");
  address(v, sizeof v); pmo_item(o, "ip", "Network address", v, "Use this address for FTP and the web interface.");
  pmo_header(o, "Temperatures");
  temperature(0, v, sizeof v); pmo_item(o, "cpu", "CPU", v, "Updated every 2 seconds.");
  temperature(1, v, sizeof v); pmo_item(o, "soc", "SoC", v, "System-on-chip sensor 0.");
  pmo_header(o, "Storage");
  for(int i = 0; i < DRIVES; i++) {
    double f, t; if(!drive_space(drives[i].path, &f, &t)) continue;
    char detail_text[96]; snprintf(id, sizeof id, "drive%d", i); drive_value(i, v, sizeof v);
    snprintf(detail_text, sizeof detail_text, "%s. Select for details.", drives[i].path);
    pmo_item(o, id, drives[i].label, v, detail_text);
  }
  pmo_page_end(o);
}
static void drive_page(pmo_t *o, int i) {
  double f = 0, t = 0; char a[16], b[16], c[16], pct[16], dev[96] = "--";
  int present = drive_space(drives[i].path, &f, &t);
#ifdef __PS5__
  struct statfs st; if(present && !statfs(drives[i].path, &st)) snprintf(dev, sizeof dev, "%.40s (%.16s)", st.f_mntfromname, st.f_fstypename);
#endif
  snprintf(pct, sizeof pct, "%.0f%%", t > 0 ? (t - f) * 100.0 / t : 0.0);
  pmo_page_begin(o, drives[i].label, drives[i].path);
  if(!present) pmo_item_disabled(o, "gone", "Drive not connected", NULL, "Press Circle to go back.");
  else {
    pmo_item(o, "free", "Free", bytes_text(a, sizeof a, f), "Space available to apps and copies.");
    pmo_item(o, "used", "Used", bytes_text(b, sizeof b, t - f), "Space in use.");
    pmo_item(o, "total", "Size", bytes_text(c, sizeof c, t), "Total size of this file system.");
    pmo_item(o, "pct", "In use", pct, NULL);
    pmo_item(o, "device", "Device", NULL, dev);
  }
  pmo_item(o, "back", "Back", NULL, "Return to Console Tools.");
  pmo_page_end(o);
}
static void show(pmo_t *o) { if(detail < 0) main_page(o); else drive_page(o, detail); }

int main(void) {
  pmo_t *o = pmo_connect(NULL);
  if(!o) return 1;
  show(o);
  time_t last = time(NULL);
  for(;;) {
    pmo_event_t ev;
    int r = pmo_wait(o, &ev, 1000);
    if(r < 0) break;
    if(r > 0) {
      if(ev.kind == PMO_EV_QUIT) break;
      if(ev.kind == PMO_EV_SHOW) show(o);
      else if(ev.kind == PMO_EV_BACK) { if(detail >= 0) { detail = -1; show(o); } else pmo_exit(o); }
      else if(ev.kind == PMO_EV_SELECT) {
        if(detail < 0 && !strncmp(ev.id, "drive", 5)) { detail = atoi(ev.id + 5); if(detail < 0 || detail >= DRIVES) detail = -1; show(o); }
        else if(detail >= 0 && !strcmp(ev.id, "back")) { detail = -1; show(o); }
      }
    }
    if(pmo_visible(o) && time(NULL) - last >= 2) {
      last = time(NULL);
      char v[64];
      if(detail < 0) {
        uptime(v, sizeof v); pmo_patch_value(o, "uptime", v);
        temperature(0, v, sizeof v); pmo_patch_value(o, "cpu", v);
        temperature(1, v, sizeof v); pmo_patch_value(o, "soc", v);
      }
    }
  }
  pmo_close(o);
  return 0;
}
