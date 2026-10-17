// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck-layout.h"
#include <string.h>
#include <stdio.h>
const int pm_deck_font_sizes[PM_DECK_STYLES]={48,36,25,24,17};
// Monochrome native chrome. Warning text retains its semantic amber color.
const float pm_deck_ink[PM_DECK_INKS][4]={
 {1,1,1,1},{0.97f,0.97f,0.97f,1},{0.70f,0.70f,0.70f,1},
 {0.53f,0.53f,0.53f,1},{1,0.76f,0.38f,1},
 {1,1,1,1},{1,1,1,1},{0.035f,0.035f,0.035f,1},
 {0,1,0.867f,1},              // ProsperoMenu: Tesla highlight teal
 {0.97f,0.97f,0.97f,1}        // ProsperoMenu: title text (PM_INK_TITLE)
};
const float pm_deck_background[PM_DECK_BACKGROUNDS][4]={
 {0,0,0,0.60f},               // local panel shadow
 {0.008f,0.008f,0.008f,0.68f},    // smoked-glass surface
 {0.008f,0.008f,0.008f,0.68f},    // matching header
 {0.04f,0.04f,0.04f,0.62f},       // quiet status surface
 {0.045f,0.045f,0.045f,1},    // selection interior
 {1,1,1,1},                  // white outline
 {0.68f,0.68f,0.68f,0.28f},       // fine gray-white divider
 {0.045f,0.045f,0.045f,1},
 {1,1,1,1},
 {0.045f,0.045f,0.045f,1},
 {1,1,1,1},
 // ProsperoMenu docked layout (libtesla style colours)
 {0,0,0,0.95f},               // dock background: 0.85 (Tesla) read too light over ShellUI on hardware
 {0.133f,0.133f,0.133f,1},    // right edge line (0xF222)
 {0,1,0.867f,1},              // focus outline (ColorHighlight)
 {1,1,1,0.13f},               // row separator
 {0.333f,0.333f,0.333f,1},    // scrollbar handle (ColorHandle)
 {0,1,0.867f,1}               // ProsperoMenu: logo mark (PM_BG_LOGO)
};
// Conservative display-cell accounting; CJK/emoji occupy two cells. Native
// UIFont remains responsible for shaping. Input comes from UTF-8 checked frames.
static unsigned glyph(const char* s,unsigned* cells){
 const unsigned char* p=(const unsigned char*)s;unsigned n=1,c=p[0];
 if(c>=0xf0){n=4;c&=7;}else if(c>=0xe0){n=3;c&=15;}else if(c>=0xc0){n=2;c&=31;}
 for(unsigned i=1;i<n;i++)c=(c<<6)|(p[i]&63);
 *cells=(c>=0x1100&&(c<=0x115f||c==0x2329||c==0x232a||
  (c>=0x2e80&&c<=0xa4cf)||(c>=0xac00&&c<=0xd7a3)||(c>=0xf900&&c<=0xfaff)||
  (c>=0xfe10&&c<=0xfe6f)||(c>=0xff01&&c<=0xff60)||(c>=0xffe0&&c<=0xffe6)||c>=0x1f000))?2:1;
 return n;
}
static unsigned cells_left(const char* text){
 unsigned total=0;while(*text){unsigned cells,n=glyph(text,&cells);total+=cells;text+=n;}return total;
}
static void lines(PmDeckLayout* l,unsigned first,unsigned count,unsigned width,const char* input){
 const char* p=input;
 for(unsigned line=0;line<count&&*p;line++){
  while(*p==' ')p++;const char* end=p;const char* space=NULL;unsigned cells=0;
  while(*end){unsigned cell,n=glyph(end,&cell);if(cells+cell>width)break;
   if(*end==' ')space=end;cells+=cell;end+=n;}
  // Prefer word wrapping only when the remaining lines can still hold the
  // received text. Otherwise long words could waste enough space to hide detail.
  if(*end&&line+1<count&&space&&space>p&&cells_left(space)<=width*(count-line-1))end=space;
  size_t length=(size_t)(end-p);while(length&&p[length-1]==' ')length--;
  if(*end&&line+1==count){
   // Leave room for the ellipsis without splitting a code point.
   const char* cut=p;unsigned used=0;
   while(cut<p+length){unsigned cell,n=glyph(cut,&cell);if(used+cell>width-3)break;used+=cell;cut+=n;}
   length=(size_t)(cut-p);memcpy(l->labels[first+line].text,p,length);
   memcpy(l->labels[first+line].text+length,"...",4);
  }else{memcpy(l->labels[first+line].text,p,length);l->labels[first+line].text[length]=0;}
  p=end;
 }
}
static void place(PmDeckLayout* l,unsigned index,float x,float y,unsigned font,unsigned ink){
 l->labels[index].x=x;l->labels[index].y=y;l->labels[index].font=font;l->labels[index].ink=ink;
}
static void panel(PmDeckLayout* l,unsigned index,float x,float y,float width,float height,unsigned ink,int visible){
 l->panels[index]=(PmDeckPanel){x,y,width,height,ink,!!visible};
}
void pm_deck_layout_classic(const PmDeckFrame* f,int open,unsigned selected,PmDeckLayout* l){
 memset(l,0,sizeof *l);
 unsigned accent=(f->flags>>3)&3; if(accent>2)accent=0;
 const unsigned accents[]={5,8,10},inks[]={0,5,6};
 int menu=open&&(f->flags&1),focus=menu&&selected<f->count&&selected<8;
 unsigned selectedRow=selected<8?selected:0;
 // Centered smoked-glass panel. Alpha applies only to our widgets; scene
 // occlusion stays zero so the dashboard continues rendering behind it.
 panel(l,0,255,155,1410,770,6,menu);
 panel(l,1,256,156,1408,768,1,menu);
 panel(l,2,288,296,1344,1,6,menu);
 panel(l,3,976,328,1,484,6,menu);
 panel(l,4,1008,328,624,484,3,menu);
 panel(l,5,288,328+selectedRow*58,656,54,accents[accent],focus);
 panel(l,6,288,876,1344,1,6,menu);
 panel(l,7,288,840,1344,1,6,menu&&f->metrics[0]);
 panel(l,8,278,336+selectedRow*58,2,38,accents[accent],focus);
 panel(l,9,72,60,1776,112,1,!open&&(f->flags&1)&&f->notice.kind&&f->notice.remainingMs);
 panel(l,10,288,846,1032,28,1,(f->flags&1)&&f->metrics[0]);
 panel(l,11,1392,200,240,36,3,(f->flags&2)&&f->ip[0]);
 panel(l,12,288,200,48,8,5,menu);panel(l,13,308,216,8,36,5,menu);
 panel(l,14,288,216,12,8,6,menu);panel(l,15,324,216,12,8,6,menu);
 for(unsigned i=0;i<8;i++)panel(l,16+i,288+(i%4)*336,328+(i/4)*236,312,208,3,0);
 place(l,0,360,200,1,PM_INK_TEXT);place(l,1,600,196,0,PM_INK_TEXT);
 place(l,2,288,260,4,PM_INK_MUTED);place(l,3,288,280,4,PM_INK_MUTED);
 for(unsigned i=4;i<12;i++)place(l,i,288,260,4,PM_INK_QUIET);
 for(unsigned i=0;i<8;i++){
  unsigned ink=i==selected?7:(i<f->count&&(f->rows[i].flags&1)?PM_INK_TEXT:PM_INK_MUTED);
  place(l,12+i,308,342+i*58,3,ink);place(l,20+i,880,346+i*58,4,i==selected?7:PM_INK_ACCENT);
 }
 place(l,28,1040,360,4,PM_INK_QUIET);
 for(unsigned i=0;i<4;i++)place(l,29+i,1040,404+i*40,1,PM_INK_TEXT);
 for(unsigned i=0;i<6;i++)place(l,33+i,1040,510+i*30,3,PM_INK_MUTED);
 place(l,39,1040,748,3,PM_INK_ACCENT);
 place(l,40,288,888,4,PM_INK_MUTED);place(l,41,1408,208,4,PM_INK_MUTED);
 place(l,42,288,850,4,PM_INK_MUTED);
 place(l,43,96,76,2,f->notice.kind==2?PM_INK_ATTENTION:PM_INK_ACCENT);
 place(l,44,96,122,3,PM_INK_TEXT);
 if(!open){
  // Size the small HUD to its readout, with padding and bounded display cells.
  unsigned metricCells=cells_left(f->metrics);if(metricCells>62)metricCells=62;
  unsigned metricWidth=32+metricCells*10;if(metricWidth<104)metricWidth=104;
  metricWidth=(metricWidth+15)&~15u;
  panel(l,10,60,1000,(float)metricWidth,32,1,(f->flags&1)&&f->metrics[0]);
  panel(l,11,1600,1000,260,32,1,(f->flags&2)&&f->ip[0]);
  place(l,42,76,1006,4,PM_INK_TEXT);place(l,41,1616,1006,4,PM_INK_TEXT);
 }
 if(open&&(f->flags&1)){
  strcpy(l->labels[0].text,"ProsperoMenu");
  const char* title=f->title;const char* slash=strstr(title," / ");if(slash)title=slash+3;
  lines(l,1,1,27,title);lines(l,2,1,118,f->subtitle);
  for(unsigned i=0;i<f->count&&i<8;i++){
   lines(l,12+i,1,38,f->rows[i].label);
   if(f->rows[i].flags&2)strcpy(l->labels[20+i].text,"ON");
   else if(selected==i)strcpy(l->labels[20+i].text,"<");
  }
  if(selected<f->count&&selected<8){
   const PmDeckRow* row=&f->rows[selected];
   strcpy(l->labels[28].text,(row->flags&1)?"SELECTED ACTION":"INFORMATION");lines(l,29,2,28,row->label);lines(l,33,6,42,row->detail);
   strcpy(l->labels[39].text,(row->flags&1)?"X  Select":"");
   if(!(row->flags&1))l->labels[39].ink=PM_INK_ATTENTION;
  }else{strcpy(l->labels[28].text,"DETAILS");strcpy(l->labels[29].text,"No actions here yet");}
  lines(l,40,1,64,f->footer);
 }
 if(menu&&(f->flags&32)){
  for(unsigned i=3;i<=5;i++)l->panels[i].visible=0;
  l->panels[8].visible=0;
  for(unsigned i=12;i<40;i++)l->labels[i].text[0]=0;
  for(unsigned i=0;i<f->count&&i<8;i++){
   unsigned ink=i==selected?7:PM_INK_TEXT;
   PmDeckPanel* card=&l->panels[16+i];card->visible=1;card->ink=i==selected?accents[accent]:3;
   place(l,12+i,card->x+20,card->y+28,1,ink);
   place(l,20+i,card->x+20,card->y+104,4,i==selected?7:PM_INK_MUTED);
   place(l,29+i,card->x+20,card->y+132,4,i==selected?7:PM_INK_MUTED);
   lines(l,12+i,1,16,f->rows[i].label);
   lines(l,20+i,2,28,f->rows[i].detail);
   strcpy(l->labels[29+i].text,l->labels[21+i].text);

  }
  l->labels[28].text[0]=0;
 }
 if(menu&&(f->flags&128)){
  // Reuse the existing bounded label pool; no new ShellUI objects or hooks.
  for(unsigned i=0;i<PM_DECK_LABELS;i++)l->labels[i].text[0]=0;
  for(unsigned i=2;i<PM_DECK_PANELS;i++)l->panels[i].visible=0;
  place(l,40,288,204,1,PM_INK_TEXT);lines(l,40,1,62,f->title);
  place(l,41,288,270,3,PM_INK_MUTED);place(l,42,288,304,3,PM_INK_MUTED);lines(l,41,2,86,f->rows[0].detail);
  place(l,43,288,852,4,PM_INK_MUTED);lines(l,43,1,120,f->footer);
  place(l,44,288,810,4,PM_INK_ATTENTION);lines(l,44,1,120,f->subtitle);
  for(unsigned i=0;i<40;i++){
   float x=304+(i%10)*132,y=400+(i/10)*96;
   place(l,i,x+18,y+20,i<38?1:4,i==selected?7:PM_INK_TEXT);
   if(i<36){l->labels[i].text[0]=f->rows[0].label[i];l->labels[i].text[1]=0;}
   else strcpy(l->labels[i].text,i==36?".":i==37?"/":i==38?"Delete":"Done");
   if(i==selected)panel(l,5,x,y,124,72,5,1);
  }
  return;
 }
 if(menu&&(f->flags&256)){
  // Two real file lists share the existing scene and bounded widget pool.
  for(unsigned i=0;i<PM_DECK_LABELS;i++)l->labels[i].text[0]=0;
  for(unsigned i=2;i<PM_DECK_PANELS;i++)l->panels[i].visible=0;
  place(l,0,288,186,1,PM_INK_TEXT);strcpy(l->labels[0].text,"File browser");
  for(unsigned side=0;side<2;side++){
   float x=288+side*686;
   place(l,1+side,x,248,3,PM_INK_TEXT);lines(l,1+side,1,49,f->browser.paths[side]);
   place(l,6+side,x,294,4,PM_INK_QUIET);strcpy(l->labels[6+side].text,"NAME");
   for(unsigned row=0;row<12;row++){
    unsigned i=side*12+row;const PmBrowserRow* entry=&f->browser.entries[i];float y=328+row*34;
    unsigned ink=selected==i?7:entry->flags&1?PM_INK_TEXT:PM_INK_QUIET;
    place(l,8+i,x+10,y,4,ink);place(l,32+i,x+492,y,4,ink);
    char name[104];snprintf(name,sizeof name,"%s%s%s",entry->flags&2?"* ":"",entry->flags&4?"/ ":"",entry->label);
    lines(l,8+i,1,44,name);lines(l,32+i,1,14,entry->detail);
    if(selected==i)panel(l,5,x,y-4,648,32,5,1);
   }
   place(l,32+side*12,x+492,294,4,PM_INK_QUIET);strcpy(l->labels[32+side*12].text,"SIZE / TYPE");
  }
  panel(l,2,288,281,1344,1,6,1);panel(l,3,958,248,1,494,6,1);
  panel(l,6,288,744,1344,1,6,1);panel(l,7,288,830,1344,1,6,1);
  place(l,3,288,758,4,PM_INK_MUTED);lines(l,3,1,116,f->subtitle);
  place(l,4,288,796,4,PM_INK_ATTENTION);lines(l,4,1,116,f->browser.status);
  for(unsigned i=0;i<8;i++){
   float x=288+i*170;int focus=selected==24+i;
   panel(l,16+i,x,844,154,36,focus?5:3,1);
   place(l,56+i,x+10,850,4,focus?7:f->rows[i].flags&1?PM_INK_TEXT:PM_INK_QUIET);lines(l,56+i,1,14,f->rows[i].label);
  }
  place(l,5,288,892,4,PM_INK_MUTED);lines(l,5,1,116,f->footer);
  return;
 }
 if(f->flags&1){
  lines(l,42,1,62,f->metrics);
  if(!open&&f->notice.kind&&f->notice.remainingMs){lines(l,43,1,50,f->notice.title);lines(l,44,1,82,f->notice.detail);}
 }
 if(f->flags&2)lines(l,41,1,19,f->ip);
 for(unsigned i=0;i<PM_DECK_LABELS;i++)if(l->labels[i].ink==PM_INK_ACCENT)l->labels[i].ink=inks[accent];
}
