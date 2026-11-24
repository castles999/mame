// license:BSD-3-Clause
// LivingArcade CAP-5A: diagnostic RGB scanline commands, opt-in and private.
// Source writes are observed before touching the destination; final pixels
// are read ONLY for the independent witness. Not a shipping tile archive.
#ifndef MAME_EMU_LIVINGARCADE_SCANLINE_COMMAND_H
#define MAME_EMU_LIVINGARCADE_SCANLINE_COMMAND_H
#include "livingarcade_raster_command.h"
namespace livingarcade_scanline_command {
using namespace livingarcade_raster_command;
inline constexpr u32 max_scanlines=262144, max_events=8192;
inline writer &scanline_output(){
    static writer w("LIVINGARCADE_NEOGEO_COMMAND","LANE0001");
    return w;
}
struct scanline_scope;
inline thread_local scanline_scope *line_active=nullptr;
struct event { u16 x; u8 pen,layer; u32 color; };
struct scanline_scope {
    writer *out=nullptr;
    std::uintptr_t row=0;
    int w,h,l,t,r,b;
    u64 frame;
    u32 bg,status=0;
    bool finished=false;
    std::vector<event> events;
    template<class Bitmap,class Rect>
    scanline_scope(const Bitmap &dest,u64 f,const Rect &clip,u32 background)
      :w(dest.width()),h(dest.height()),l(clip.left()),t(clip.top()),
       r(clip.right()+1),b(clip.bottom()+1),frame(f),bg(background){
        auto &o=scanline_output();
        if(!o.f||o.failed||o.capped||line_active)return;
        if(o.count>=max_scanlines){o.capped=true;return;}
        out=&o;line_active=this;
        if(!dest.valid()||w<=0||h<=0||w>1024||h>1024||
           l<0||r>w||l>=r||t<0||b>h||b!=t+1){status|=1;return;}
        row=reinterpret_cast<std::uintptr_t>(&dest.pix(t,0));
    }
    ~scanline_scope(){
        if(out){if(!finished)out->failed=true;line_active=nullptr;}
    }
    scanline_scope(const scanline_scope&)=delete;
    scanline_scope& operator=(const scanline_scope&)=delete;
    void put(const u32 *dst,unsigned pen,u32 color,unsigned layer){
        if(status)return;
        const auto addr=reinterpret_cast<std::uintptr_t>(dst);
        if(addr<row||addr-row>=u64(w)*sizeof(u32)||(addr-row)%sizeof(u32)||
           pen==0||pen>15||layer<1||layer>2){status|=2;return;}
        const auto x=(addr-row)/sizeof(u32);
        if(x<unsigned(l)||x>=unsigned(r))return; // outside the observed clip
        if(events.size()>=max_events){status|=4;return;}
        events.push_back({u16(x),u8(pen),u8(layer),color});
    }
    template<class Bitmap> void finish(const Bitmap &dest){
        if(!out||finished)return;
        finished=true;
        u64 hash=offset;
        if(!status)for(int x=l;x<r;++x){
            const u32 c=dest.pix(t,x);
            for(unsigned i=0;i<4;++i)mix(hash,c>>(8*i));
        }
        if(out->bytes+56+8*u64(events.size())+12>max_file){out->capped=true;return;}
        out->write("LIN1",4);out->integer(frame,8);
        for(int v:{w,h,l,t,r,b})out->integer(u32(v),4);
        out->integer(status,4);out->integer(bg,4);out->integer(events.size(),4);
        out->integer(hash,8);
        for(const auto &e:events){
            out->integer(e.x,2);out->integer(e.pen,1);
            out->integer(e.layer,1);out->integer(e.color,4);
        }
        ++out->count;
        if(std::fflush(out->f)!=0)out->failed=true;
    }
};
inline void source_pixel(const u32 *dst,unsigned pen,u32 color,unsigned layer){
    if(line_active)line_active->put(dst,pen,color,layer);
}
}
#endif
