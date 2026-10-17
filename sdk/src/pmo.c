/* SPDX-License-Identifier: MIT
 * ProsperoMenu overlay SDK, API 1 (see sdk/include/pmo.h).
 *
 * One TCP connection to 127.0.0.1, one JSON object per line. A reader thread
 * answers the menu's watchdog pings and queues events for pmo_wait(). */
#include "pmo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __PS5__
#include <sys/sysctl.h>
#include <sys/user.h>
#include <ps5/klog.h>
#endif

/* Connection problems go to stderr (overlay.log in the overlay folder) and,
 * on the console, to klog, so they can be read while debugging. */
static void report(const char *fmt, ...) {
  char text[300]; va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof text, fmt, ap); va_end(ap);
  fprintf(stderr, "pmo: %s\n", text); fflush(stderr);
#ifdef __PS5__
  klog_printf("pmo: %s\n", text);
#endif
}

#define QUEUE 64
struct pmo {
  int fd, closed, visible;
  char id[16], dir[160], root[128];
  pthread_t reader;
  pthread_mutex_t write_lock, queue_lock;
  pthread_cond_t queue_cond;
  pmo_event_t queue[QUEUE];
  unsigned head, count;
  char *page; size_t page_len, page_cap; int page_items;
  unsigned request_serial; char request_id[16]; char *reply; size_t reply_size; int reply_ready;
};

/* ---- small JSON writer ------------------------------------------------- */
typedef struct { char *data; size_t len, cap; int failed; } buf_t;
static void buf_put(buf_t *b, const char *s, size_t n) {
  if(b->failed) return;
  if(b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 1024; while(cap < b->len + n + 1) cap *= 2;
    char *next = realloc(b->data, cap); if(!next) { b->failed = 1; return; }
    b->data = next; b->cap = cap;
  }
  memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0;
}
static void buf_str(buf_t *b, const char *s) { buf_put(b, s, strlen(s)); }
/* JSON string, cut at `limit` bytes on a UTF-8 boundary. */
static void buf_json(buf_t *b, const char *s, size_t limit) {
  buf_put(b, "\"", 1);
  size_t n = s ? strlen(s) : 0; if(n > limit) { n = limit; while(n && ((unsigned char)s[n] & 0xc0) == 0x80) n--; }
  for(size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i]; char esc[8];
    if(c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; buf_put(b, esc, 2); }
    else if(c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", c); buf_put(b, esc, 6); }
    else buf_put(b, (const char *)&c, 1);
  }
  buf_put(b, "\"", 1);
}
static void buf_int(buf_t *b, long v) { char n[24]; snprintf(n, sizeof n, "%ld", v); buf_str(b, n); }

static int send_line(pmo_t *o, const char *line, size_t len) {
  if(!o || o->fd < 0) return -1;
  pthread_mutex_lock(&o->write_lock);
  int rc = 0; size_t done = 0;
  while(done < len) {
    ssize_t w = send(o->fd, line + done, len - done, 0);
    if(w < 0 && errno == EINTR) continue;
    if(w <= 0) { rc = -1; break; }
    done += (size_t)w;
  }
  if(!rc) { char nl = '\n'; if(send(o->fd, &nl, 1, 0) != 1) rc = -1; }
  pthread_mutex_unlock(&o->write_lock);
  return rc;
}
static int send_buf(pmo_t *o, buf_t *b) { int rc = b->failed ? -1 : send_line(o, b->data, b->len); free(b->data); return rc; }

/* ---- small JSON reader (flat objects from the menu) -------------------- */
static const char *json_find(const char *json, const char *key) {
  char pattern[40]; snprintf(pattern, sizeof pattern, "\"%s\"", key);
  const char *p = json;
  while((p = strstr(p, pattern))) {
    const char *q = p + strlen(pattern); while(*q == ' ') q++;
    if(*q == ':') { q++; while(*q == ' ') q++; return q; }
    p = q;
  }
  return NULL;
}
static void json_string(const char *json, const char *key, char *out, size_t size) {
  out[0] = 0; const char *p = json_find(json, key); if(!p || *p != '"') return;
  size_t n = 0; p++;
  while(*p && *p != '"' && n + 1 < size) { if(*p == '\\' && p[1]) p++; out[n++] = *p++; }
  out[n] = 0;
}
static long json_number(const char *json, const char *key, long fallback) {
  const char *p = json_find(json, key); if(!p) return fallback;
  if(!strncmp(p, "true", 4)) return 1;
  if(!strncmp(p, "false", 5)) return 0;
  char *end; long v = strtol(p, &end, 10); return end == p ? fallback : v;
}

/* ---- reader thread ------------------------------------------------------ */
static void enqueue(pmo_t *o, const pmo_event_t *ev) {
  pthread_mutex_lock(&o->queue_lock);
  if(o->count < QUEUE) { o->queue[(o->head + o->count) % QUEUE] = *ev; o->count++; }
  pthread_cond_signal(&o->queue_cond);
  pthread_mutex_unlock(&o->queue_lock);
}
static void handle(pmo_t *o, const char *line) {
  char type[16]; json_string(line, "type", type, sizeof type);
  pmo_event_t ev; memset(&ev, 0, sizeof ev);
  if(!strcmp(type, "result")) {
    char id[16]; json_string(line, "id", id, sizeof id);
    pthread_mutex_lock(&o->queue_lock);
    if(o->reply && !strcmp(id, o->request_id)) { snprintf(o->reply, o->reply_size, "%s", line); o->reply_ready = 1; pthread_cond_broadcast(&o->queue_cond); }
    pthread_mutex_unlock(&o->queue_lock);
    return;
  }
  if(!strcmp(type, "ping")) { static const char pong[] = "{\"type\":\"pong\"}"; send_line(o, pong, sizeof pong - 1); return; }
  if(!strcmp(type, "show")) { o->visible = 1; ev.kind = PMO_EV_SHOW; }
  else if(!strcmp(type, "hide")) { o->visible = 0; ev.kind = PMO_EV_HIDE; }
  else if(!strcmp(type, "back")) ev.kind = PMO_EV_BACK;
  else if(!strcmp(type, "quit")) ev.kind = PMO_EV_QUIT;
  else if(!strcmp(type, "event")) {
    char kind[16]; json_string(line, "kind", kind, sizeof kind); json_string(line, "id", ev.id, sizeof ev.id);
    if(!strcmp(kind, "select")) ev.kind = PMO_EV_SELECT;
    else if(!strcmp(kind, "toggle")) { ev.kind = PMO_EV_TOGGLE; ev.on = json_number(line, "value", 0) != 0; }
    else if(!strcmp(kind, "slide")) { ev.kind = PMO_EV_SLIDE; ev.value = (int)json_number(line, "value", 0); }
    else return;
  } else return; /* welcome and future messages */
  enqueue(o, &ev);
}
static void *reader(void *arg) {
  pmo_t *o = arg; char *line = malloc(65536); size_t len = 0;
  if(!line) goto done;
  for(;;) {
    char chunk[4096]; ssize_t r = recv(o->fd, chunk, sizeof chunk, 0);
    if(r < 0 && errno == EINTR) continue;
    if(r <= 0) break;
    for(ssize_t i = 0; i < r; i++) {
      if(chunk[i] == '\n') { line[len] = 0; if(len) handle(o, line); len = 0; }
      else if(len + 1 < 65536) line[len++] = chunk[i];
    }
  }
done:
  free(line);
  pthread_mutex_lock(&o->queue_lock); o->closed = 1; pthread_cond_broadcast(&o->queue_cond); pthread_mutex_unlock(&o->queue_lock);
  return NULL;
}

/* ---- connection ----------------------------------------------------------- */
static int own_name(char *id, size_t size) {
  const char *env = getenv("PMO_ID");
  if(env && *env) { snprintf(id, size, "%s", env); return 0; }
#ifdef __PS5__
  /* The console's kinfo_proc is larger than the SDK's struct: ask for the
   * size first, then read the fields the SDK layout shares (as ProsperoMenu's
   * system-probe.c does). A buffer of sizeof(struct kinfo_proc) fails. */
  int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  size_t len = 0;
  if(!sysctl(mib, 4, NULL, &len, NULL, 0) && len >= sizeof(int) && len < 65536) {
    size_t cap = len + len / 4 + 64; unsigned char *raw = calloc(1, cap);
    if(raw && !sysctl(mib, 4, raw, &cap, NULL, 0)) {
      struct kinfo_proc info; memset(&info, 0, sizeof info);
      memcpy(&info, raw, cap < sizeof info ? cap : sizeof info);
      info.ki_comm[COMMLEN] = 0; info.ki_tdname[TDNAMLEN] = 0;
      const char *names[] = {info.ki_tdname, info.ki_comm};
      for(int i = 0; i < 2; i++) if(!strncmp(names[i], "pmo_", 4) && names[i][4]) { snprintf(id, size, "%s", names[i] + 4); free(raw); return 0; }
      report("process name is \"%s\" / \"%s\"", info.ki_comm, info.ki_tdname);
    } else report("process table lookup failed (%s)", strerror(errno));
    free(raw);
  } else report("process table size lookup failed (%s)", strerror(errno));
#endif
  return -1;
}
pmo_t *pmo_connect(const char *id) {
  pmo_t *o = calloc(1, sizeof *o); if(!o) return NULL;
  o->fd = -1;
  const char *root = getenv("PMO_ROOT"); snprintf(o->root, sizeof o->root, "%s", root && *root ? root : PMO_ROOT);
  if(id && *id) snprintf(o->id, sizeof o->id, "%s", id);
  else if(own_name(o->id, sizeof o->id)) { report("no overlay id: the process name does not start with pmo_ (pid %d)", (int)getpid()); free(o); return NULL; }
  snprintf(o->dir, sizeof o->dir, "%s/overlays/%s", o->root, o->id);
  char path[256], token[80] = ""; int port = 0;
  snprintf(path, sizeof path, "%s/run/%s.session", o->root, o->id);
  FILE *f = fopen(path, "r");
  if(!f || fscanf(f, "%d %79s", &port, token) != 2 || port <= 0 || port > 65535) {
    report("%s: cannot read session file %s (%s)", o->id, path, f ? "bad contents" : strerror(errno));
    if(f) fclose(f);
    free(o); return NULL;
  }
  fclose(f);
  o->fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a; memset(&a, 0, sizeof a);
  a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if(o->fd < 0 || connect(o->fd, (struct sockaddr *)&a, sizeof a)) {
    report("%s: cannot connect to the menu on 127.0.0.1:%d (%s)", o->id, port, strerror(errno));
    if(o->fd >= 0) close(o->fd);
    free(o); return NULL;
  }
  pthread_mutex_init(&o->write_lock, NULL); pthread_mutex_init(&o->queue_lock, NULL); pthread_cond_init(&o->queue_cond, NULL);
  buf_t b = {0};
  buf_str(&b, "{\"type\":\"hello\",\"api\":1,\"id\":"); buf_json(&b, o->id, 15);
  buf_str(&b, ",\"token\":"); buf_json(&b, token, 79); buf_str(&b, ",\"pid\":"); buf_int(&b, (long)getpid()); buf_str(&b, "}");
  if(send_buf(o, &b) || pthread_create(&o->reader, NULL, reader, o)) { close(o->fd); free(o); return NULL; }
  return o;
}
const char *pmo_id(const pmo_t *o) { return o ? o->id : ""; }
const char *pmo_dir(const pmo_t *o) { return o ? o->dir : ""; }
int pmo_visible(const pmo_t *o) { return o && o->visible; }
void pmo_close(pmo_t *o) {
  if(!o) return;
  if(o->fd >= 0) { shutdown(o->fd, SHUT_RDWR); pthread_join(o->reader, NULL); close(o->fd); }
  free(o->page); free(o);
}
int pmo_wait(pmo_t *o, pmo_event_t *ev, int timeout_ms) {
  memset(ev, 0, sizeof *ev);
  pthread_mutex_lock(&o->queue_lock);
  if(timeout_ms < 0) { while(!o->count && !o->closed) pthread_cond_wait(&o->queue_cond, &o->queue_lock); }
  else if(!o->count && !o->closed && timeout_ms > 0) {
    struct timeval now; gettimeofday(&now, NULL);
    struct timespec until; long ns = now.tv_usec * 1000L + (timeout_ms % 1000) * 1000000L;
    until.tv_sec = now.tv_sec + timeout_ms / 1000 + ns / 1000000000L; until.tv_nsec = ns % 1000000000L;
    while(!o->count && !o->closed) if(pthread_cond_timedwait(&o->queue_cond, &o->queue_lock, &until) == ETIMEDOUT) break;
  }
  int result = 0;
  if(o->count) { *ev = o->queue[o->head]; o->head = (o->head + 1) % QUEUE; o->count--; result = 1; }
  else if(o->closed) result = -1;
  pthread_mutex_unlock(&o->queue_lock);
  return result;
}

/* ---- pages -------------------------------------------------------------- */
static void page_put(pmo_t *o, const char *s) {
  buf_t b = {o->page, o->page_len, o->page_cap, 0}; buf_str(&b, s);
  o->page = b.data; o->page_len = b.len; o->page_cap = b.cap;
}
static void page_json(pmo_t *o, const char *s, size_t limit) {
  buf_t b = {o->page, o->page_len, o->page_cap, 0}; buf_json(&b, s, limit);
  o->page = b.data; o->page_len = b.len; o->page_cap = b.cap;
}
static void page_field(pmo_t *o, const char *key, const char *value, size_t limit) {
  if(!value) return;
  char k[24]; snprintf(k, sizeof k, ",\"%s\":", key); page_put(o, k); page_json(o, value, limit);
}
static void item_begin(pmo_t *o, const char *kind, const char *id) {
  page_put(o, o->page_items++ ? ",{\"kind\":" : "{\"kind\":"); page_json(o, kind, 8);
  if(id) { page_put(o, ",\"id\":"); page_json(o, id, 48); }
}
void pmo_page_begin(pmo_t *o, const char *title, const char *subtitle) {
  o->page_len = 0; o->page_items = 0;
  page_put(o, "{\"type\":\"page\",\"title\":"); page_json(o, title ? title : "", 48);
  page_field(o, "subtitle", subtitle, 80); page_put(o, ",\"items\":[");
}
void pmo_header(pmo_t *o, const char *label) { item_begin(o, "header", NULL); page_field(o, "label", label, 95); page_put(o, "}"); }
static void item(pmo_t *o, const char *id, const char *label, const char *value, const char *detail, int enabled) {
  item_begin(o, "item", id); page_field(o, "label", label, 95); page_field(o, "value", value, 31); page_field(o, "detail", detail, 159);
  if(!enabled) page_put(o, ",\"enabled\":false");
  page_put(o, "}");
}
void pmo_item(pmo_t *o, const char *id, const char *label, const char *value, const char *detail) { item(o, id, label, value, detail, 1); }
void pmo_item_disabled(pmo_t *o, const char *id, const char *label, const char *value, const char *detail) { item(o, id, label, value, detail, 0); }
void pmo_toggle(pmo_t *o, const char *id, const char *label, int on, const char *detail) {
  item_begin(o, "toggle", id); page_field(o, "label", label, 95); page_field(o, "detail", detail, 159);
  page_put(o, on ? ",\"on\":true}" : ",\"on\":false}");
}
void pmo_slider(pmo_t *o, const char *id, const char *label, int min, int max, int step, int current, const char *unit, const char *detail) {
  char n[96]; item_begin(o, "slider", id); page_field(o, "label", label, 95); page_field(o, "detail", detail, 159); page_field(o, "unit", unit, 8);
  snprintf(n, sizeof n, ",\"min\":%d,\"max\":%d,\"step\":%d,\"current\":%d}", min, max, step, current); page_put(o, n);
}
static char footer_text[96];
void pmo_footer(pmo_t *o, const char *footer) { (void)o; snprintf(footer_text, sizeof footer_text, "%s", footer ? footer : ""); }
int pmo_page_end(pmo_t *o) {
  page_put(o, "]");
  if(footer_text[0]) { page_field(o, "footer", footer_text, 80); footer_text[0] = 0; }
  page_put(o, "}");
  return o->page ? send_line(o, o->page, o->page_len) : -1;
}

/* ---- patches and messages ----------------------------------------------- */
static int patch(pmo_t *o, const char *id, const char *key, const char *json_value) {
  buf_t b = {0}; buf_str(&b, "{\"type\":\"patch\",\"set\":{"); buf_json(&b, id, 48);
  buf_str(&b, ":{\""); buf_str(&b, key); buf_str(&b, "\":"); buf_str(&b, json_value); buf_str(&b, "}}}");
  return send_buf(o, &b);
}
static int patch_text(pmo_t *o, const char *id, const char *key, const char *text, size_t limit) {
  buf_t v = {0}; buf_json(&v, text ? text : "", limit); int rc = v.failed ? -1 : patch(o, id, key, v.data); free(v.data); return rc;
}
int pmo_patch_value(pmo_t *o, const char *id, const char *value) { return patch_text(o, id, "value", value, 31); }
int pmo_patch_label(pmo_t *o, const char *id, const char *label) { return patch_text(o, id, "label", label, 95); }
int pmo_patch_detail(pmo_t *o, const char *id, const char *detail) { return patch_text(o, id, "detail", detail, 159); }
int pmo_patch_toggle(pmo_t *o, const char *id, int on) { return patch(o, id, "on", on ? "true" : "false"); }
int pmo_patch_enabled(pmo_t *o, const char *id, int enabled) { return patch(o, id, "enabled", enabled ? "true" : "false"); }
int pmo_patch_slider(pmo_t *o, const char *id, int current) { char n[16]; snprintf(n, sizeof n, "%d", current); return patch(o, id, "current", n); }
int pmo_patch_subtitle(pmo_t *o, const char *subtitle) {
  buf_t b = {0}; buf_str(&b, "{\"type\":\"patch\",\"set\":{},\"subtitle\":"); buf_json(&b, subtitle ? subtitle : "", 80); buf_str(&b, "}");
  return send_buf(o, &b);
}
static int message(pmo_t *o, const char *type, const char *fmt, va_list ap) {
  char text[512]; vsnprintf(text, sizeof text, fmt, ap);
  buf_t b = {0}; buf_str(&b, "{\"type\":\""); buf_str(&b, type); buf_str(&b, "\",\"text\":"); buf_json(&b, text, 400); buf_str(&b, "}");
  return send_buf(o, &b);
}
int pmo_notice(pmo_t *o, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int rc = message(o, "notice", fmt, ap); va_end(ap); return rc; }
int pmo_log(pmo_t *o, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int rc = message(o, "log", fmt, ap); va_end(ap); return rc; }
int pmo_exit(pmo_t *o) { static const char m[] = "{\"type\":\"exit\"}"; return send_line(o, m, sizeof m - 1); }

/* ---- requests ------------------------------------------------------------ */
int pmo_json_string(const char *json, const char *key, char *out, size_t size) {
  json_string(json, key, out, size); return json_find(json, key) ? 0 : -1;
}
long pmo_json_number(const char *json, const char *key, long fallback) { return json_number(json, key, fallback); }
int pmo_request(pmo_t *o, const char *action, const char *key, const char *value, char *reply, size_t size, int timeout_ms) {
  if(!o || !reply || size < 2) return -1;
  reply[0] = 0;
  pthread_mutex_lock(&o->queue_lock);
  if(o->reply) { pthread_mutex_unlock(&o->queue_lock); snprintf(reply, size, "{\"error\":\"another request is running\"}"); return -1; }
  snprintf(o->request_id, sizeof o->request_id, "r%u", ++o->request_serial);
  o->reply = reply; o->reply_size = size; o->reply_ready = 0;
  pthread_mutex_unlock(&o->queue_lock);
  buf_t b = {0};
  buf_str(&b, "{\"type\":\"request\",\"id\":"); buf_json(&b, o->request_id, 15);
  buf_str(&b, ",\"action\":"); buf_json(&b, action, 32);
  if(key && value) { buf_str(&b, ","); buf_json(&b, key, 32); buf_str(&b, ":"); buf_json(&b, value, 1000); }
  buf_str(&b, "}");
  int sent = send_buf(o, &b);
  pthread_mutex_lock(&o->queue_lock);
  if(!sent) {
    struct timeval now; gettimeofday(&now, NULL);
    long ms = timeout_ms > 0 ? timeout_ms : 10000;
    long ns = now.tv_usec * 1000L + (ms % 1000) * 1000000L;
    struct timespec until = { now.tv_sec + ms / 1000 + ns / 1000000000L, ns % 1000000000L };
    while(!o->reply_ready && !o->closed) if(pthread_cond_timedwait(&o->queue_cond, &o->queue_lock, &until) == ETIMEDOUT) break;
  }
  int ok = o->reply_ready && json_number(reply, "ok", 0) == 1;
  if(!o->reply_ready) snprintf(reply, size, "{\"error\":\"%s\"}", sent ? "could not send the request" : "the menu did not answer");
  o->reply = NULL; o->reply_ready = 0;
  pthread_mutex_unlock(&o->queue_lock);
  return ok ? 0 : -1;
}

/* ---- helpers --------------------------------------------------------------- */
const char *pmo_fmt(char *buf, size_t size, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vsnprintf(buf, size, fmt, ap); va_end(ap); return buf;
}
/* settings.ini: key=value lines, rewritten whole on every set. */
static int config_path(pmo_t *o, char *path, size_t size) { return snprintf(path, size, "%s/settings.ini", o->dir) < (int)size ? 0 : -1; }
int pmo_config_get_str(pmo_t *o, const char *key, char *out, size_t size) {
  char path[200], line[512]; size_t k = strlen(key); if(config_path(o, path, sizeof path)) return -1;
  FILE *f = fopen(path, "r"); if(!f) return -1;
  int found = -1;
  while(fgets(line, sizeof line, f)) {
    if(strncmp(line, key, k) || line[k] != '=') continue;
    char *v = line + k + 1; v[strcspn(v, "\r\n")] = 0; snprintf(out, size, "%s", v); found = 0; break;
  }
  fclose(f); return found;
}
int pmo_config_get_int(pmo_t *o, const char *key, int fallback) {
  char v[32]; if(pmo_config_get_str(o, key, v, sizeof v)) return fallback;
  char *end; long n = strtol(v, &end, 10); return end == v ? fallback : (int)n;
}
int pmo_config_set_str(pmo_t *o, const char *key, const char *value) {
  char path[200], tmp[210], line[512]; size_t k = strlen(key); if(config_path(o, path, sizeof path)) return -1;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *in = fopen(path, "r"), *out = fopen(tmp, "w"); if(!out) { if(in) fclose(in); return -1; }
  if(in) { while(fgets(line, sizeof line, in)) if(strncmp(line, key, k) || line[k] != '=') fputs(line, out); fclose(in); }
  fprintf(out, "%s=%s\n", key, value); fclose(out);
  return rename(tmp, path);
}
int pmo_config_set_int(pmo_t *o, const char *key, int value) { char v[16]; snprintf(v, sizeof v, "%d", value); return pmo_config_set_str(o, key, v); }
