// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stddef.h>
#define PM_DECK_MAGIC 0x54414344u
// ProsperoMenu frame protocol v9: the frame carries a moving
// window of up to 32 rows around the focused item. The deck owns focus and the
// scroll position, and asks for a new window with a PM_DECK_WINDOW event
// whenever focus comes near either end of the current one.
#define PM_DECK_ROWS 32
#define PM_DECK_FOCUS_NONE 0xFFFFu // frame.focus: the deck keeps its position
#define PM_DECK_WINDOW_LEAD 14u    // focus row inside a freshly sent window
#define PM_DECK_VIEWPORT_ROWS 6u   // L1/R1 jump; matches the docked viewport
#define PM_DECK_STALE_MS 3000u
#define PM_DECK_DISCONNECTED_MS 15000u
typedef struct { char token[32], label[96], detail[160]; uint32_t flags; } PmDeckRow;
// Row flags. 1 enabled and 2 on are ProsperoMenu's. ProsperoMenu (frame v10) adds:
// PM_ROW_ADJUST: Left/Right send PREVIOUS/NEXT for this row (sliders, pickers).
// PM_ROW_HEADER: a category header; never focused, drawn small and muted.
// PM_ROW_VALUE: label holds "text\0value\0"; the value is drawn right-aligned.
enum { PM_ROW_ENABLED=1, PM_ROW_ON=2, PM_ROW_ADJUST=4, PM_ROW_HEADER=8, PM_ROW_VALUE=16, PM_ROW_FLAGS=31 };
// ProsperoSettings theme (frame v11): RGBA8 colours applied to the shared ink
// and background tables. serial 0 keeps the built-in palette.
enum { PM_THEME_ACCENT, PM_THEME_BACKGROUND, PM_THEME_TEXT, PM_THEME_MUTED,
 PM_THEME_SEPARATOR, PM_THEME_SCROLLBAR, PM_THEME_EDGE, PM_THEME_FOCUS_FILL,
 PM_THEME_TITLE, PM_THEME_LOGO, PM_THEME_COLORS };
typedef struct { uint32_t serial; uint32_t rgba[PM_THEME_COLORS]; uint32_t reserved; } PmDeckTheme;
typedef struct {uint32_t buttons;int32_t handle;uint32_t connection,sequence;uint64_t at;} PmDeckPad;
typedef struct {
 uint32_t epoch,serial,active,captured;int32_t owner;uint32_t connection,count,reserved;
 uint64_t deadline,captureAt;PmDeckPad events[128];
} PmDeckInput;
typedef struct {uint32_t epoch,serial,open;int32_t owner;uint32_t connection,ready;uint64_t renderedAt;uint32_t menuOpen,dashboardSerial;char dashboardTitle[10];uint32_t routeResets[4];} PmDeckFeedback;
typedef struct {char title[10],version[16];uint16_t reserved;} PmDashboardRecord;
typedef struct {uint32_t enabled,epoch,ack,count;PmDashboardRecord records[32];} PmDashboardFrame;
typedef struct {uint32_t sequence,remainingMs,kind,count;char title[96],detail[192];} PmDeckNotice;
#define PM_BROWSER_ROWS 12
typedef struct {char label[96],detail[64];uint32_t flags;} PmBrowserRow;
typedef struct {uint32_t active;char paths[2][160],status[160];PmBrowserRow entries[24];uint32_t reserved;} PmDeckBrowser;
typedef struct {
  uint32_t magic; uint16_t version,flags; uint32_t sequence;
  uint16_t page,count; uint16_t windowStart,total; uint16_t focus,reserved0;
  char title[64],subtitle[160],footer[160],ip[64]; PmDeckRow rows[PM_DECK_ROWS];
  uint32_t metricsSequence; char metrics[192];
  uint32_t reserved;PmDeckInput input;PmDeckNotice notice;PmDeckBrowser browser;PmDashboardFrame dashboard;
  PmDeckTheme theme;
  // v12: the replaceable header logo (a path the UI2 ImageBox loads). Empty
  // keeps the drawn mark, so the deck only depends on the managed image ABI
  // when the coordinator actually ships a picture.
  char logoPath[192];
  // v13: boot animation frame counter. The coordinator writes each decoded
  // frame to a fixed BMP path and bumps this counter; the deck reloads the
  // image whenever the counter changes (frame-accurate, no timing guesswork).
  uint32_t videoSerial;
  uint32_t videoPad;
} PmDeckFrame;
#define PM_DECK_VERSION 13
_Static_assert(sizeof(PmDeckFrame)==19024,"Deck wire size");
_Static_assert(offsetof(PmDeckFrame,videoSerial)==19016,"Video serial wire offset");
_Static_assert(offsetof(PmDeckFrame,logoPath)==18824,"Logo path wire offset");
_Static_assert(offsetof(PmDeckFrame,theme)==18776,"Theme wire offset");
_Static_assert(offsetof(PmDeckFrame,dashboard)==17864,"Dashboard wire offset");
_Static_assert(offsetof(PmDeckFrame,browser)==13440,"Browser wire offset");
_Static_assert(offsetof(PmDeckFrame,notice)==13136,"Independent notice offset");
_Static_assert(offsetof(PmDeckFrame,input)==10016,"Independent game input offset");
_Static_assert(offsetof(PmDeckFrame,metricsSequence)==9816,"Independent telemetry sequence offset");
_Static_assert(offsetof(PmDeckFrame,rows)==472,"Deck row offset");
typedef struct { uint32_t kind,sequence,row,serial; } PmDeckEvent;
enum { PM_DECK_SELECT=1,PM_DECK_NEXT=2,PM_DECK_PREVIOUS=3,PM_DECK_BACK=4,PM_DECK_WINDOW=5 };
enum { PM_LEFT=1,PM_UP=2,PM_RIGHT=4,PM_DOWN=8,PM_CIRCLE=64,PM_CROSS=128,
 PM_L1=1024,PM_R1=2048,PM_L3=16384,PM_R3=32768 };
typedef struct {
  PmDeckFrame frame; uint64_t received,chordAt[8],repeatAt,noticeDeadline; uint32_t previous[8],serial;
  // Absolute index of the focused item (windowStart+selected); survives window
  // refills and list rebuilds that keep the focus inside the current window.
  uint32_t focus;
  // Last explicit focus the coordinator asked for. An explicit focus is a
  // one-shot command: re-applying a stale value on every frame rebuild would
  // undo the user's scrolling.
  uint32_t focusCommand;
  uint8_t chordActive[8],chordLatched[8],drain[8],open,valid,selected,owner,pending,browserPane;
 uint32_t repeatDirection,presentedSequence;uint8_t presentedOpen,presentedSelected;PmDeckEvent event;
} PmDeck;
int pm_deck_decode(const void* bytes,size_t length,PmDeckFrame* frame);
void pm_deck_metrics_focus(PmDeckFrame* frame,int gameFocused);
void pm_deck_init(PmDeck* deck);
int pm_deck_receive(PmDeck* deck,const void* bytes,size_t length,uint64_t now);
void pm_deck_tick(PmDeck* deck,uint64_t now);
int pm_deck_stalled(const PmDeck* deck,uint64_t now);
uint32_t pm_deck_notice_remaining(const PmDeck* deck,uint64_t now);
void pm_deck_present(PmDeck* deck,uint32_t sequence,int open,unsigned selected);
// Return 1 when the renderer owns this sample; caller clears all controller
// fields, including analog values and button edges, before returning it.
int pm_deck_input(PmDeck* deck,unsigned device,uint32_t buttons,uint64_t now);
int pm_deck_input_at(PmDeck*,unsigned,uint32_t,uint64_t sampleAt,uint64_t now);
int pm_deck_event(const PmDeck* deck,PmDeckEvent* event);
void pm_deck_ack(PmDeck* deck,uint32_t serial);
