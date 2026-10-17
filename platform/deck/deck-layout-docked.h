// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoMenu: Tesla-style menu docked to the left edge.
#pragma once
#include "deck-layout.h"
#include "deck-motion.h"

// Geometry on ProsperoMenu's 1920x1080 canvas: libtesla's 448x720 frame scaled by 1.5.
#define PM_DOCK_WIDTH 672.0f
#define PM_DOCK_CONTENT_X 52.0f
#define PM_DOCK_CONTENT_Y 188.0f
#define PM_DOCK_CONTENT_WIDTH 545.0f
#define PM_DOCK_ROW_HEIGHT 105.0f
#define PM_DOCK_VISIBLE_ROWS 6u
#define PM_DOCK_FOOTER_Y 970.0f
#define PM_DOCK_SLIDE_MS 160u

// Panel slots. 9-11 stay reserved for ProsperoMenu's closed-menu HUD.
enum {
 PM_PANEL_BACKGROUND=0, PM_PANEL_EDGE=1, PM_PANEL_FOOTER_RULE=2, PM_PANEL_SCROLL_THUMB=3,
 PM_PANEL_FOCUS_OUTER=4, PM_PANEL_FOCUS_INNER=5,
 PM_PANEL_LOGO_FIRST=6, // 6..8: the logo mark left of the title
 PM_PANEL_SEPARATOR_FIRST=12 // PM_DOCK_VISIBLE_ROWS+1 separators: 12..18
};
// Label slots. 41-44 stay reserved for the HUD.
enum {
 PM_LABEL_TITLE=0, PM_LABEL_SUBTITLE=1, PM_LABEL_FOOTER=2,
 PM_LABEL_ROW_TEXT=12, PM_LABEL_ROW_VALUE=20, PM_LABEL_DESCRIPTION=28, PM_DOCK_DESCRIPTION_LINES=4
};

// First visible row for snapped (whole-row) scrolling. Like libtesla's List,
// the focused row sits about one third of the way down and the list clamps at
// both ends instead of wrapping.
unsigned pm_deck_dock_top(unsigned count,unsigned selected);

// Pure presentation, same contract as pm_deck_layout_classic(): no managed calls,
// network, filesystem, allocations or clock reads. The keyboard and closed
// HUD keep their working layout; the file browser shares the dock and palette.
void pm_deck_layout_docked(const PmDeckFrame* frame,int open,unsigned selected,PmDeckLayout* layout);

// Animated variant. marqueeMs is how long the focused row has been focused: a
// title or focused row that does not fit scrolls its own window of cells and
// raises layout->marquee so the renderer keeps drawing until it settles.
void pm_deck_layout_docked_at(const PmDeckFrame* frame,int open,unsigned selected,PmDeckLayout* layout,uint64_t marqueeMs);

// Finite slide-in from the left edge. The HUD never slides. Returns non-zero
// while the animation is still moving.
static inline int pm_deck_motion_docked(PmDeckMotion* m,PmDeckLayout* l,unsigned selected,uint64_t now){
 if(!m->active)*m=(PmDeckMotion){now,now,now,0,0,0,selected,1};
 float slide=-PM_DOCK_WIDTH*(1-pm_deck_ease(now,m->opened,PM_DOCK_SLIDE_MS));
 if(slide!=0){
  for(unsigned i=0;i<PM_DECK_LABELS;i++)if(l->browser||i<41||i>44)l->labels[i].x+=slide;
  for(unsigned i=0;i<PM_DECK_PANELS;i++)if(i<9||i>11)l->panels[i].x+=slide;
  for(unsigned i=0;i<PM_DECK_IMAGES;i++)l->images[i].x+=slide;
 }
 m->selected=selected;m->last=now;
 return slide!=0;
}

// The renderer's active layout. Build with -DPM_CLASSIC_DECK for ProsperoMenu's
// original centred window.
#ifdef PM_CLASSIC_DECK
#define pm_deck_layout(f,o,s,l,ms) pm_deck_layout_classic(f,o,s,l)
#define pm_deck_motion pm_deck_motion_classic
#else
#define pm_deck_layout(f,o,s,l,ms) pm_deck_layout_docked_at(f,o,s,l,ms)
#define pm_deck_motion pm_deck_motion_docked
#endif
