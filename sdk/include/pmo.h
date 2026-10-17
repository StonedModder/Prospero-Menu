/* SPDX-License-Identifier: MIT
 * ProsperoMenu overlay SDK, API 1. https://github.com/StonedModder/prospero-menu
 *
 * An overlay is a small PS5 payload ELF that lives in
 * /data/ProsperoMenu/overlays/<id>/ next to its overlay.json. ProsperoMenu
 * starts it when the user opens it and draws the pages it sends. The overlay
 * never touches the system UI: it describes rows, the menu renders them.
 *
 *   int main(void) {
 *     pmo_t *o = pmo_connect(NULL);          // id from the process name
 *     if(!o) return 1;
 *     int on = 0;
 *     for(;;) {
 *       pmo_page_begin(o, "Hello", "v1.0");
 *       pmo_header(o, "Example");
 *       pmo_toggle(o, "power", "Power", on, "Turns the thing on.");
 *       pmo_page_end(o);
 *       pmo_event_t ev;
 *       int r = pmo_wait(o, &ev, -1);
 *       if(r < 0 || ev.kind == PMO_EV_QUIT) break;
 *       if(ev.kind == PMO_EV_TOGGLE) on = ev.on;
 *       if(ev.kind == PMO_EV_BACK) pmo_exit(o);   // root page: leave
 *     }
 *     pmo_close(o);
 *     return 0;
 *   }
 *
 * Wire protocol (for other languages): docs/OVERLAYS.md.
 * This header and sdk/src/pmo.c are MIT licensed so overlays may use any
 * licence. Builds as a PS5 payload, or on a PC for development (it then talks
 * to `npm run dev`'s overlay host on 127.0.0.1).
 */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMO_API 1
#define PMO_ROOT "/data/ProsperoMenu"

typedef struct pmo pmo_t;

typedef enum {
  PMO_EV_NONE = 0,
  PMO_EV_SELECT,  /* X on an item: id                                        */
  PMO_EV_TOGGLE,  /* X on a toggle: id, on (the new state, already shown)    */
  PMO_EV_SLIDE,   /* Left/Right on a slider: id, value (already shown)       */
  PMO_EV_BACK,    /* Circle: pop your own page, or pmo_exit() at your root   */
  PMO_EV_SHOW,    /* the menu is showing this overlay                        */
  PMO_EV_HIDE,    /* the menu closed or the user left this overlay           */
  PMO_EV_QUIT     /* stop now; the process is killed shortly after           */
} pmo_event_kind;

typedef struct {
  pmo_event_kind kind;
  char id[49];
  int on;          /* PMO_EV_TOGGLE */
  int value;       /* PMO_EV_SLIDE  */
} pmo_event_t;

/* Connect to ProsperoMenu. id NULL: use the id from the process name the
 * menu gave this overlay ("pmo_<id>"). Returns NULL when the menu is not
 * reachable; the overlay should then exit. */
pmo_t *pmo_connect(const char *id);
const char *pmo_id(const pmo_t *o);
/* Overlay folder, e.g. "/data/ProsperoMenu/overlays/fan-control". */
const char *pmo_dir(const pmo_t *o);
void pmo_close(pmo_t *o);

/* Next event. timeout_ms < 0 waits forever, 0 polls. Returns 1 with an event,
 * 0 on timeout, -1 when the menu disconnected (exit then). Pings from the
 * menu's watchdog are answered by the SDK's own thread, so long work in the
 * main loop is fine; keep the menu informed with pmo_patch_*(). */
int pmo_wait(pmo_t *o, pmo_event_t *ev, int timeout_ms);
/* Non-zero while the menu shows this overlay (between SHOW and HIDE). */
int pmo_visible(const pmo_t *o);

/* Pages. Build one page and send it in one go; it replaces the shown page.
 * Text is UTF-8. Labels are cut at 95 bytes, values at 31, details at 159.
 * id: 1-48 characters of A-Z a-z 0-9 _ . : - (unique within the page). */
void pmo_page_begin(pmo_t *o, const char *title, const char *subtitle);
void pmo_header(pmo_t *o, const char *label);
void pmo_item(pmo_t *o, const char *id, const char *label, const char *value, const char *detail);
void pmo_item_disabled(pmo_t *o, const char *id, const char *label, const char *value, const char *detail);
void pmo_toggle(pmo_t *o, const char *id, const char *label, int on, const char *detail);
void pmo_slider(pmo_t *o, const char *id, const char *label, int min, int max, int step, int current,
                const char *unit, const char *detail);
void pmo_footer(pmo_t *o, const char *footer); /* optional button hints   */
int pmo_page_end(pmo_t *o);                     /* 0 sent, -1 failed      */

/* Small live updates to the shown page (values, labels, toggles). */
int pmo_patch_value(pmo_t *o, const char *id, const char *value);
int pmo_patch_label(pmo_t *o, const char *id, const char *label);
int pmo_patch_detail(pmo_t *o, const char *id, const char *detail);
int pmo_patch_toggle(pmo_t *o, const char *id, int on);
int pmo_patch_slider(pmo_t *o, const char *id, int current);
int pmo_patch_enabled(pmo_t *o, const char *id, int enabled);
int pmo_patch_subtitle(pmo_t *o, const char *subtitle);

int pmo_notice(pmo_t *o, const char *fmt, ...);   /* system notification   */
int pmo_log(pmo_t *o, const char *fmt, ...);      /* menu's runtime log    */
int pmo_exit(pmo_t *o);                           /* back to the menu list */

/* Helpers for console overlays ------------------------------------------- */
/* Ask the menu to do something on this overlay's behalf. The action may need
 * a permission listed in overlay.json ("permissions"). Sends
 * {"type":"request","action":action,key:value} and waits for the answer.
 * Returns 0 with the reply object in `reply`, -1 with an error text in `reply`.
 * API 1 defines no actions yet; unknown actions answer with an error.     */
int pmo_request(pmo_t *o, const char *action, const char *key, const char *value,
                char *reply, size_t size, int timeout_ms);
/* Read one field of a flat JSON object, e.g. a pmo_request() reply. */
int pmo_json_string(const char *json, const char *key, char *out, size_t size);
long pmo_json_number(const char *json, const char *key, long fallback);

/* printf into a fixed buffer, always terminated. */
const char *pmo_fmt(char *buf, size_t size, const char *fmt, ...);
/* Read/write a small key=value file in the overlay folder ("settings.ini"). */
int pmo_config_get_int(pmo_t *o, const char *key, int fallback);
int pmo_config_get_str(pmo_t *o, const char *key, char *out, size_t size);
int pmo_config_set_int(pmo_t *o, const char *key, int value);
int pmo_config_set_str(pmo_t *o, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
