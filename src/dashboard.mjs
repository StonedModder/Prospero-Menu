// SPDX-License-Identifier: GPL-3.0-or-later
// Original ProsperoMenu dashboard integration. Only local installed metadata is used.
import { GAME_TITLE, GAME_VERSION } from './game-metadata.mjs';
import { invariant } from './errors.mjs';
export class Dashboard {
  constructor(runtime){this.runtime=runtime;this.offset=0;this.epoch=1;this.signature='';this.ack=0;this.lastRead=0;this.pending=null;}
  frame(){
    const enabled=this.runtime.snapshot().settings.appearance.gameCardDetails;
    if(enabled&&this.runtime.library&&!this.pending&&Date.now()-this.lastRead>30000){
      this.lastRead=Date.now();this.pending=this.runtime.library.refresh().catch(()=>{}).finally(()=>{this.pending=null;});
    }
    const records=(this.runtime.libraryState?.games||[]).filter(g=>GAME_TITLE.test(g.titleId)&&GAME_VERSION.test(g.version||''));
    const signature=JSON.stringify(records.map(g=>[g.titleId,g.version]));
    if(signature!==this.signature){this.signature=signature;this.epoch=(this.epoch+1)>>>0||1;this.offset=0;}
    const items=enabled?records.slice(this.offset,this.offset+32):[];
    this.offset=enabled&&this.offset+items.length<records.length?this.offset+items.length:0;
    return {enabled,epoch:this.epoch,ack:this.ack,items};
  }
  open(input){
    invariant(input&&Number.isInteger(input.serial)&&input.serial>0&&input.serial<=0xffffffff&&GAME_TITLE.test(input.titleId),'DASHBOARD_REQUEST','Invalid dashboard game request.');
    if(input.serial===this.ack)return;
    invariant(!this.ack||((input.serial-this.ack)|0)>0,'DASHBOARD_REQUEST','Dashboard action is outdated.');
    this.cheats?.stop();this.cheats=new this.runtime.consoleDeck.constructor(this.runtime);
    this.cheats.nativeSettings=true;this.cheats.openGameCheats(input.titleId);this.ack=input.serial;
  }
}
