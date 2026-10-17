// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-core.h"
#define PM_DECK_LABELS 64
#define PM_DECK_STYLES 5
#define PM_DECK_INKS 10
#define PM_DECK_PANELS 24
#define PM_DECK_BACKGROUNDS 17
#define PM_DECK_IMAGES 1
enum { PM_IMAGE_LOGO=0 };
enum { PM_INK_ACCENT, PM_INK_TEXT, PM_INK_MUTED, PM_INK_QUIET, PM_INK_ATTENTION };
// ProsperoMenu docked-layout colours (deck-layout-docked.c).
enum { PM_INK_TEAL=8 };
enum { PM_INK_TITLE=9 };      // the title at the top of the panel
enum { PM_BG_DOCK=11, PM_BG_EDGE, PM_BG_TEAL, PM_BG_SEPARATOR, PM_BG_SCROLL };
enum { PM_BG_LOGO=16 };       // the logo mark left of the title
typedef struct { float x,y; unsigned font,ink; char text[256]; } PmDeckText;
typedef struct { float x,y,width,height; unsigned ink,visible; } PmDeckPanel;
// A real picture widget (UI2.ImageBox): the replaceable header logo.
typedef struct { float x,y,width,height; unsigned visible; char path[192]; } PmDeckImage;
typedef struct { PmDeckText labels[PM_DECK_LABELS]; PmDeckPanel panels[PM_DECK_PANELS];
 PmDeckImage images[PM_DECK_IMAGES]; unsigned marquee;
 // Browser rows reuse the HUD label slots in the main scene. Their text must
 // move with the dock; the independent closed HUD retains its fixed position.
 unsigned browser; } PmDeckLayout;
extern const int pm_deck_font_sizes[PM_DECK_STYLES];
extern const float pm_deck_ink[PM_DECK_INKS][4];
extern const float pm_deck_background[PM_DECK_BACKGROUNDS][4];
// Pure presentation of an already validated frame. No managed calls, network,
// filesystem, allocations or clock reads. Native rendering and previews share it.
void pm_deck_layout_classic(const PmDeckFrame*,int open,unsigned selected,PmDeckLayout*);
