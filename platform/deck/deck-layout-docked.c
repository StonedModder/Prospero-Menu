// SPDX-License-Identifier: GPL-3.0-or-later
// ProsperoMenu: Tesla-style list docked to the left edge of the screen.
// Geometry follows libtesla (448x720) scaled by 1.5 onto ProsperoMenu's 1920x1080
// canvas. This file only arranges ProsperoMenu's existing bounded widget pool; it
// creates no ShellUI objects and changes no input or protocol behaviour.
#include "deck-layout-docked.h"
#include <stdio.h>
#include <string.h>

// Established menu-row UTF-8 fitting is unchanged. Compact header/browser
// fields use wider ASCII/Unicode budgets in wide_glyph() below; every cut
// remains on a code-point boundary and truncated text ends in "...".
static unsigned glyph(const char* s,unsigned* cells){
 const unsigned char* p=(const unsigned char*)s;unsigned n=1,c=p[0];
 if(c>=0xf0){n=4;c&=7;}else if(c>=0xe0){n=3;c&=15;}else if(c>=0xc0){n=2;c&=31;}
 for(unsigned i=1;i<n&&p[i];i++)c=(c<<6)|(p[i]&63);
 *cells=(c>=0x1100&&(c<=0x115f||c==0x2329||c==0x232a||
  (c>=0x2e80&&c<=0xa4cf)||(c>=0xac00&&c<=0xd7a3)||(c>=0xf900&&c<=0xfaff)||
  (c>=0xfe10&&c<=0xfe6f)||(c>=0xff01&&c<=0xff60)||(c>=0xffe0&&c<=0xffe6)||c>=0x1f000))?2:1;
 return n;
}
static unsigned wide_glyph(const char* s,unsigned* cells){
 unsigned n=glyph(s,cells),c=(unsigned char)s[0];
 if(n==1){if((c>='A'&&c<='Z')||c=='m'||c=='w'||strchr("@%&+<=>~",(int)c))*cells=2;}
 else{
  // Accent/ligature/Greek/Cyrillic glyphs can be wider than an ASCII cell.
  // Emoji and other fallback-font symbols also need more than two cells.
  if(n==2)*cells=3;else if(n==4||*cells==1)*cells=4;
  unsigned cp=c&(n==2?31u:n==3?15u:7u);
  for(unsigned i=1;i<n;i++)cp=(cp<<6)|((unsigned char)s[i]&63u);
  // Arabic presentation-form phrase ligatures can occupy many ems as one
  // code point. A compact field shows an ellipsis rather than spilling it.
  if(cp>=0xfdf0&&cp<=0xfdff)*cells=32;
 }
 return n;
}
// Word-wraps text into `count` consecutive labels of at most `width` cells.
static void fit(PmDeckLayout* l,unsigned first,unsigned count,unsigned width,const char* input){
 const char* p=input;
 for(unsigned line=0;line<count;line++){
  char* out=l->labels[first+line].text;out[0]=0;
  while(*p==' ')p++;if(!*p)continue;
  const char* end=p;const char* space=NULL;unsigned used=0;
  while(*end){unsigned c,n=glyph(end,&c);if(used+c>width)break;if(*end==' ')space=end;used+=c;end+=n;}
  if(*end&&line+1<count&&space&&space>p)end=space;
  size_t length=(size_t)(end-p);while(length&&p[length-1]==' ')length--;
  if(*end&&line+1==count){
   const char* cut=p;unsigned kept=0;
   while(cut<p+length){unsigned c,n=glyph(cut,&c);if(kept+c>width-3)break;kept+=c;cut+=n;}
   length=(size_t)(cut-p);memcpy(out,p,length);memcpy(out+length,"...",4);return;
  }
  memcpy(out,p,length);out[length]=0;p=end;
 }
}
// Conservative display cells of a UTF-8 string, using the same fit budget.
static unsigned cells(const char* s){
 unsigned total=0;
 while(*s){unsigned c,n=glyph(s,&c);total+=c;for(unsigned i=0;i<n&&*s;i++)s++;}
 return total;
}
static void place(PmDeckLayout* l,unsigned i,float x,float y,unsigned font,unsigned ink){
 l->labels[i].x=x;l->labels[i].y=y;l->labels[i].font=font;l->labels[i].ink=ink;l->labels[i].text[0]=0;
}
static void panel(PmDeckLayout* l,unsigned i,float x,float y,float w,float h,unsigned ink,int visible){
 l->panels[i]=(PmDeckPanel){x,y,w,h,ink,!!visible};
}

unsigned pm_deck_dock_top(unsigned count,unsigned selected){
 if(count<=PM_DOCK_VISIBLE_ROWS)return 0;
 unsigned last=count-PM_DOCK_VISIBLE_ROWS,lead=PM_DOCK_VISIBLE_ROWS/3;
 if(selected>=count)selected=count-1;
 unsigned top=selected>lead?selected-lead:0;
 return top>last?last:top;
}

// Font styles from pm_deck_font_sizes: 0=48 title, 1=36 rows, 2=25 values and
// hints, 3=24 subtitle and description. Reusing them adds no ShellUI fonts.
enum { FONT_TITLE=0, FONT_ROW=1, FONT_VALUE=2, FONT_SMALL=3 };

// Marquee. A label that does not fit scrolls its own window of display cells:
// the first cell drops one at a time until the tail is visible, holds, then the
// window restarts. Labels draw above panels, so sliding a label out of the dock
// would spill over the game; changing the window keeps every cell inside the
// field instead. libtesla's ListItem scrolls long focused text the same way.
#define PM_MARQUEE_HOLD_MS 1500u       // read the start before it moves
#define PM_MARQUEE_MS_PER_CELL 140u
#define PM_MARQUEE_END_MS 900u         // pause with the tail visible
static const char* skip_cells(const char* s,unsigned skip){
 while(*s&&skip){unsigned c,n=glyph(s,&c);if(c>skip)break;skip-=c;s+=n;}
 return s;
}
static unsigned take_cells(const char* s,unsigned width){
 const char* p=s;unsigned used=0;
 while(*p){unsigned c,n=glyph(p,&c);if(used+c>width)break;used+=c;p+=n;}
 return (unsigned)(p-s);
}
// Fits text into one label. Overflowing text scrolls once marqueeMs passes the
// hold; *active is raised whenever the text overflows so the renderer keeps
// drawing until the scroll has looped.
static void fit_window(PmDeckLayout* l,unsigned slot,unsigned width,const char* input,uint64_t ms,unsigned* active){
 const char* p=input;unsigned total=0;while(*p){unsigned c,n=glyph(p,&c);total+=c;p+=n;}
 if(total<=width||!width){fit(l,slot,1,width,input);return;}
 *active=1;
 if(ms<PM_MARQUEE_HOLD_MS){fit(l,slot,1,width,input);return;}
 unsigned maxOffset=total-width;
 unsigned cycle=(unsigned)(PM_MARQUEE_HOLD_MS+PM_MARQUEE_MS_PER_CELL*(maxOffset+1)+PM_MARQUEE_END_MS);
 unsigned phase=(unsigned)(ms%cycle),offset=0;
 if(phase>=PM_MARQUEE_HOLD_MS){
  unsigned moved=(phase-PM_MARQUEE_HOLD_MS)/PM_MARQUEE_MS_PER_CELL;
  offset=moved>maxOffset?maxOffset:moved;
 }
 const char* window=skip_cells(input,offset);
 unsigned keep=take_cells(window,width);
 char* out=l->labels[slot].text;
 if(keep>sizeof(l->labels[slot].text)-1)keep=(unsigned)sizeof(l->labels[slot].text)-1;
 memcpy(out,window,keep);out[keep]=0;
}

// Single-line compact fields use conservative proportional-glyph budgets.
// A NULL activity pointer keeps an unfocused field static. Ordinary menu rows
// retain their established fitting/scrolling behaviour above.
static void field(PmDeckLayout* l,unsigned slot,unsigned width,const char* input,uint64_t ms,unsigned* active){
 unsigned total=0;const char* p=input;
 while(*p){unsigned c,n=wide_glyph(p,&c);total+=c;p+=n;}
 const char* window=input;int ellipsis=total>width&&(!active||ms<PM_MARQUEE_HOLD_MS);
 if(total>width&&active)*active=1;
 if(total>width&&active&&!ellipsis){
  unsigned maxOffset=total-width;
  unsigned cycle=PM_MARQUEE_HOLD_MS+PM_MARQUEE_MS_PER_CELL*(maxOffset+1)+PM_MARQUEE_END_MS;
  unsigned phase=(unsigned)(ms%cycle),skip=0;
  if(phase>=PM_MARQUEE_HOLD_MS){skip=(phase-PM_MARQUEE_HOLD_MS)/PM_MARQUEE_MS_PER_CELL;if(skip>maxOffset)skip=maxOffset;}
  int tail=skip==maxOffset;
  while(*window&&skip){unsigned c,n=wide_glyph(window,&c);if(c>skip){if(tail)window+=n;break;}skip-=c;window+=n;}
 }
 unsigned limit=ellipsis?width-3:width,used=0;const char* end=window;
 while(*end){unsigned c,n=wide_glyph(end,&c);if(used+c>limit)break;used+=c;end+=n;}
 if(*end){unsigned c;wide_glyph(end,&c);if(c>width){
  ellipsis=1;limit=width-3;used=0;end=window;
  while(*end){unsigned cells,n=wide_glyph(end,&cells);if(used+cells>limit)break;used+=cells;end+=n;}
 }}
 size_t bytes=(size_t)(end-window);char* out=l->labels[slot].text;
 if(!bytes&&total){strcpy(out,"...");return;}
 memcpy(out,window,bytes);if(ellipsis)memcpy(out+bytes,"...",4);else out[bytes]=0;
}

// Compact two-pane browser in the same dock and shared ProsperoSettings inks.
// Keep all 24 file rows and eight action IDs: this changes presentation only.
static void browser_layout(const PmDeckFrame* f,unsigned selected,PmDeckLayout* l,uint64_t ms,unsigned* marquee){
 for(unsigned i=1;i<PM_DECK_LABELS;i++)place(l,i,0,0,4,PM_INK_MUTED);
 for(unsigned i=9;i<=11;i++)l->panels[i].visible=0;
 l->browser=1;
 unsigned active=selected<24?selected/12:f->browser.active;
 for(unsigned side=0;side<2;side++){
  float x=28+side*320;
  place(l,1+side,x+8,166,4,side==active?PM_INK_TEAL:PM_INK_MUTED);
  field(l,1+side,31,f->browser.paths[side],ms,side==active?marquee:NULL);
  place(l,6+side,x+8,208,4,PM_INK_MUTED);strcpy(l->labels[6+side].text,"NAME / SIZE");
  for(unsigned row=0;row<12;row++){
   unsigned i=side*12+row;const PmBrowserRow* entry=&f->browser.entries[i];float y=244+row*40;
   unsigned ink=entry->flags&1?PM_INK_TEXT:PM_INK_MUTED;
   place(l,8+i,x+8,y,4,ink);place(l,32+i,x+208,y,4,ink);
   char name[104];snprintf(name,sizeof name,"%s%s%s",entry->flags&2?"* ":"",entry->flags&4?"/ ":"",entry->label);
   field(l,8+i,21,name,ms,selected==i?marquee:NULL);
   field(l,32+i,9,entry->detail,0,NULL);
   if(selected==i){
    panel(l,PM_PANEL_FOCUS_OUTER,x,y-4,296,36,PM_BG_TEAL,1);
    panel(l,PM_PANEL_FOCUS_INNER,x+3,y-1,290,30,4,1);
   }
  }
 }
 panel(l,PM_PANEL_SCROLL_THUMB,335,156,1,556,PM_BG_SEPARATOR,1);
 panel(l,PM_PANEL_SEPARATOR_FIRST,28,226,616,1,PM_BG_SEPARATOR,1);
 panel(l,PM_PANEL_SEPARATOR_FIRST+1,28,712,616,1,PM_BG_SEPARATOR,1);
 const char* detail=selected>=24&&selected<32?f->rows[selected-24].detail:f->subtitle;
 // New folder acts on the controller-owned pane, which may have changed
 // without a new coordinator frame. Show its live path rather than stale help.
 if(selected==28)detail=f->browser.paths[active];
 place(l,3,36,734,4,PM_INK_MUTED);field(l,3,66,detail,ms,marquee);
 place(l,4,36,780,4,PM_INK_ATTENTION);field(l,4,66,f->browser.status,ms,marquee);
 for(unsigned i=0;i<8;i++){
  float x=28+(i%4)*156,y=850+(i/4)*54;
  panel(l,16+i,x,y+40,140,1,PM_BG_SEPARATOR,1);
  place(l,56+i,x+8,y+10,4,f->rows[i].flags&1?PM_INK_TEXT:PM_INK_MUTED);
  field(l,56+i,14,f->rows[i].label,0,NULL);
  if(selected==24+i){
   panel(l,PM_PANEL_FOCUS_OUTER,x,y,140,40,PM_BG_TEAL,1);
   panel(l,PM_PANEL_FOCUS_INNER,x+3,y+3,134,34,4,1);
  }
 }
 place(l,5,28,1012,4,PM_INK_TEXT);
 strcpy(l->labels[5].text,"D-pad Browse   L1/R1 Pane   X Select   Circle Up   L3+R3 Close");
}

void pm_deck_layout_docked_at(const PmDeckFrame* f,int open,unsigned selected,PmDeckLayout* l,uint64_t marqueeMs){
 int menu=open&&(f->flags&1);
 unsigned marquee=0;
 // Closed state, HUD, notices and the keyboard keep their working presentation.
 // The file browser shares this dock instead of falling back to the old window.
 if(!menu||(f->flags&128)){pm_deck_layout_classic(f,open,selected,l);return;}
 // Start from ProsperoMenu's closed layout so the metrics and IP readouts keep their
 // bounded sizing, then move the metrics readout clear of the dock.
 pm_deck_layout_classic(f,0,selected,l);
 l->panels[9].visible=0;l->labels[43].text[0]=0;l->labels[44].text[0]=0;
 if(l->panels[10].x<PM_DOCK_WIDTH){l->panels[10].x+=PM_DOCK_WIDTH-32;l->labels[42].x+=PM_DOCK_WIDTH-32;}
 for(unsigned i=0;i<PM_DECK_PANELS;i++)if(i<9||i>11)l->panels[i].visible=0;
 for(unsigned i=0;i<PM_DECK_IMAGES;i++)l->images[i].visible=0;
 for(unsigned i=0;i<PM_DECK_LABELS;i++)if(i<41||i>44)place(l,i,0,0,FONT_SMALL,PM_INK_QUIET);

 const float x=PM_DOCK_CONTENT_X,w=PM_DOCK_CONTENT_WIDTH,h=PM_DOCK_ROW_HEIGHT;
 panel(l,PM_PANEL_BACKGROUND,0,0,PM_DOCK_WIDTH,1080,PM_BG_DOCK,1);
 panel(l,PM_PANEL_EDGE,PM_DOCK_WIDTH-1,0,1,1080,PM_BG_EDGE,1);
 panel(l,PM_PANEL_FOOTER_RULE,22,PM_DOCK_FOOTER_Y,PM_DOCK_WIDTH-44,1,5,1);

 // Header: a logo left of the title, then the page name (the root page shows
 // the product name) and the coordinator's subtitle, as Tesla shows name and
 // version. A real image (the user's replaceable logo) is preferred; without
 // one the mark is drawn from the same validated panel widget as the rest of
 // the dock: three bars in the Logo colour.
 if(f->logoPath[0]){
  PmDeckImage* logo=&l->images[PM_IMAGE_LOGO];
  logo->x=30;logo->y=22;logo->width=92;logo->height=92;logo->visible=1;
  snprintf(logo->path,sizeof logo->path,"%s",f->logoPath);
 }else{
  panel(l,PM_PANEL_LOGO_FIRST+0,46,30,60,14,PM_BG_LOGO,1);
  panel(l,PM_PANEL_LOGO_FIRST+1,46,62,60,14,PM_BG_LOGO,1);
  panel(l,PM_PANEL_LOGO_FIRST+2,46,94,60,14,PM_BG_LOGO,1);
 }
 place(l,PM_LABEL_TITLE,140,27,FONT_TITLE,PM_INK_TITLE);
 // ProsperoMenu names its own pages; a "Product / Page" title shows the page.
 const char* title=f->title;const char* slash=strstr(title," / ");
 if(!title[0])title="ProsperoMenu";else if(slash)title=slash+3;
 field(l,PM_LABEL_TITLE,18,title,marqueeMs,&marquee);
 place(l,PM_LABEL_SUBTITLE,140,92,FONT_SMALL,PM_INK_MUTED);fit(l,PM_LABEL_SUBTITLE,1,36,f->subtitle);
 if(f->flags&256){browser_layout(f,selected,l,marqueeMs,&marquee);l->marquee=marquee;return;}

 unsigned count=f->count<PM_DECK_ROWS?f->count:PM_DECK_ROWS;
 unsigned top=pm_deck_dock_top(count,selected);
 unsigned shown=count-top<PM_DOCK_VISIBLE_ROWS?count-top:PM_DOCK_VISIBLE_ROWS;
 int focus=selected<count;
 if(!count){
  place(l,PM_LABEL_ROW_TEXT,x+30,PM_DOCK_CONTENT_Y+32,FONT_ROW,PM_INK_MUTED);
  strcpy(l->labels[PM_LABEL_ROW_TEXT].text,"Nothing here yet");
 }
 for(unsigned s=0;s<shown;s++){
  unsigned i=top+s;const PmDeckRow* row=&f->rows[i];float y=PM_DOCK_CONTENT_Y+s*h;
  int on=!!(row->flags&PM_ROW_ON),enabled=!!(row->flags&PM_ROW_ENABLED);
  if(row->flags&PM_ROW_HEADER){
   // Tesla CategoryHeader: small muted text sitting on the row's lower edge.
   place(l,PM_LABEL_ROW_TEXT+s,x+8,y+58,FONT_VALUE,PM_INK_MUTED);
   fit(l,PM_LABEL_ROW_TEXT+s,1,34,row->label);
   panel(l,PM_PANEL_SEPARATOR_FIRST+s,x,y,w,1,PM_BG_SEPARATOR,0);
   continue;
  }
  // Labels have no alignment control: right-align the value by its width in
  // cells (about 15 px each at the value size) and give the label the rest.
  const char* value=(row->flags&PM_ROW_VALUE)?row->label+strlen(row->label)+1:on?"On":"";
  unsigned valueCells=cells(value);if(valueCells>18)valueCells=18;
  unsigned labelCells=valueCells?26-(valueCells*15+30+18)/19:26;
  place(l,PM_LABEL_ROW_TEXT+s,x+30,y+32,FONT_ROW,enabled?PM_INK_TEXT:PM_INK_QUIET);
  // Only the focused row scrolls; the rest stay truncated, as libtesla does.
  if(i==selected)fit_window(l,PM_LABEL_ROW_TEXT+s,labelCells,row->label,marqueeMs,&marquee);
  else fit(l,PM_LABEL_ROW_TEXT+s,1,labelCells,row->label);
  if(valueCells){
   place(l,PM_LABEL_ROW_VALUE+s,x+w-30-valueCells*15.0f,y+40,FONT_VALUE,on?PM_INK_TEAL:enabled?PM_INK_MUTED:PM_INK_QUIET);
   fit(l,PM_LABEL_ROW_VALUE+s,1,18,value);
  }
  // A separator above every row and one under the last; the focused row's
  // outline covers its own two.
  int touches=focus&&(i==selected||i==selected+1);
  panel(l,PM_PANEL_SEPARATOR_FIRST+s,x,y,w,1,PM_BG_SEPARATOR,!touches);
 }
 if(shown){
  int touches=focus&&top+shown==selected+1;
  panel(l,PM_PANEL_SEPARATOR_FIRST+shown,x,PM_DOCK_CONTENT_Y+shown*h,w,1,PM_BG_SEPARATOR,!touches);
 }
 if(focus){
  float y=PM_DOCK_CONTENT_Y+(selected-top)*h;
  panel(l,PM_PANEL_FOCUS_OUTER,x,y,w,h,PM_BG_TEAL,1);
  panel(l,PM_PANEL_FOCUS_INNER,x+4,y+4,w-8,h-8,4,1);
 }
 // The scrollbar reflects the whole list, not just the shipped window.
 if(f->total>PM_DOCK_VISIBLE_ROWS){
  float view=PM_DOCK_VISIBLE_ROWS*h,list=f->total*h,thumb=view*view/list;
  float offset=(f->windowStart+top)*h/(list-view)*(view-thumb);
  panel(l,PM_PANEL_SCROLL_THUMB,x+w+15,PM_DOCK_CONTENT_Y+offset,7,thumb,PM_BG_SCROLL,1);
 }

 // The focused row's explanation sits between the list and the footer, so
 // every ProsperoMenu action keeps its help text in the narrower panel.
 for(unsigned i=0;i<PM_DOCK_DESCRIPTION_LINES;i++)
  place(l,PM_LABEL_DESCRIPTION+i,x,PM_DOCK_CONTENT_Y+PM_DOCK_VISIBLE_ROWS*h+20+i*30,FONT_SMALL,PM_INK_MUTED);
 if(focus){
  const PmDeckRow* row=&f->rows[selected];
  fit(l,PM_LABEL_DESCRIPTION,PM_DOCK_DESCRIPTION_LINES,40,row->detail);
  if(!(row->flags&1))
   for(unsigned i=0;i<PM_DOCK_DESCRIPTION_LINES;i++)l->labels[PM_LABEL_DESCRIPTION+i].ink=PM_INK_ATTENTION;
 }
 place(l,PM_LABEL_FOOTER,45,1012,FONT_VALUE,PM_INK_TEXT);fit(l,PM_LABEL_FOOTER,1,50,f->footer);
 l->marquee=marquee;
}
// Static entry point: no animation phase, used by the layout tests and the
// classic build. The renderer calls pm_deck_layout_docked_at directly.
void pm_deck_layout_docked(const PmDeckFrame* f,int open,unsigned selected,PmDeckLayout* l){
 pm_deck_layout_docked_at(f,open,selected,l,0);
}
