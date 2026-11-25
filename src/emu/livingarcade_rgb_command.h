// license:BSD-3-Clause
// LivingArcade CAP7: bounded RGB source-layer diagnostic stream.
// Reference framebuffer reads are restricted to finish(), never replay inputs.
#ifndef MAME_EMU_LIVINGARCADE_RGB_COMMAND_H
#define MAME_EMU_LIVINGARCADE_RGB_COMMAND_H
#include "livingarcade_raster_command.h"
namespace livingarcade_rgb_command {
using namespace livingarcade_raster_command;
inline writer &rgb_output(){static writer w("LIVINGARCADE_RGB_COMMAND","LARR0001");return w;}
struct scope;
inline thread_local scope *current=nullptr;
struct scope {
    writer *out=nullptr;
    const void *target=nullptr;
    u64 frame;
    int w,h,l,t,r,b;
    u32 status=0,ops=0;
    unsigned depth=0;
    bool finished=false;
    std::vector<u8> payload;
    template<class Bitmap,class Rect>
    scope(const Bitmap &dest,u64 f,const Rect &clip)
      :frame(f),w(dest.width()),h(dest.height()),l(clip.left()),t(clip.top()),r(clip.right()+1),b(clip.bottom()+1){
        auto &o=rgb_output();
        if(!o.f||o.failed||o.capped)return;
        if(current){current->status|=16;return;}
        if(o.count>=max_updates){o.capped=true;return;}
        out=&o;target=&dest;current=this;
        if(sizeof(typename Bitmap::pixel_t)!=4||!dest.valid()||w<=0||h<=0||w>1024||h>1024||
           l<0||t<0||r>w||b>h||l>=r||t>=b)status|=1;
    }
    ~scope(){if(out){if(!finished)out->failed=true;current=nullptr;}}
    scope(const scope&)=delete;
    void put(u64 v,unsigned n){for(unsigned i=0;i<n;++i)payload.push_back(u8(v>>(8*i)));}
    bool room(u64 n){
        if(status)return false;
        if(n>max_payload-payload.size()||ops>=65536){status|=2;return false;}
        ++ops;return true;
    }
    bool bounds(int x,int y,int x2,int y2)const{return x>=l&&y>=t&&x2<=r&&y2<=b&&x<x2&&y<y2;}
    void rect(int x,int y,int x2,int y2){put(x,4);put(y,4);put(x2,4);put(y2,4);}
    template<class Dest,class Rect> void fill(const Dest &dest,const Rect &clip,u32 color){
        if(status)return;
        if(&dest!=target){status|=16;return;}
        int a=std::max(l,clip.left()),c=std::max(t,clip.top());
        int d=std::min(r,clip.right()+1),e=std::min(b,clip.bottom()+1);
        if(a>=d||c>=e)return;
        if(!room(21))return;
        put(1,1);rect(a,c,d,e);put(color,4);
    }
    template<class Dest> void point(const Dest &dest,int x,int y,u32 color){
        if(status)return;
        if(&dest!=target){status|=16;return;}
        if(!bounds(x,y,x+1,y+1))return;
        if(!room(13))return;
        put(2,1);put(x,4);put(y,4);put(color,4);
    }
    // Source tile samples are resolved with the tile chip's own palette,
    // including its palette offset, BEFORE the destination is composed.
    template<class Dest,class Pix,class Flags,class Color>
    void tile(const Dest &dest,const Pix &pix,const Flags &flags,const Color *clut,
              int sx,int sy,int ex,int ey,int dx,int dy,u32 pcode,
              unsigned mask,unsigned value,bool opaque,unsigned alpha){
        if(status)return;
        if(&dest!=target||sizeof(typename Dest::pixel_t)!=4||!clut||
           (pcode&0xffff)!=0xff00||alpha!=255||sx<0||sy<0||ex>pix.width()||ey>pix.height()||
           sx>=ex||sy>=ey||!bounds(dx,dy,dx+ex-sx,dy+ey-sy)){status|=4;return;}
        const u64 n=u64(ex-sx)*(ey-sy);
        if(!room(17+5*n))return;
        put(3,1);rect(dx,dy,dx+ex-sx,dy+ey-sy);
        for(int y=sy;y<ey;++y)for(int x=sx;x<ex;++x){
            const bool selected=opaque||((flags.pix(y,x)&mask)==value);
            put(selected?1:0,1);
            put(selected?u32(clut[(pcode>>16)+pix.pix(y,x)]):0,4);
        }
    }
    // Keep source pens and the sprite's own effective palette separate.
    // mode: 0 opaque, 1 single transparent pen, 2 transparent mask.
    template<class Gfx,class Dest,class Rect>
    void sprite(Gfx &gfx,const Dest &dest,const Rect &clip,u32 code,u32 color,
                int fx,int fy,int x,int y,unsigned mode,u32 trans){
        if(status)return;
        const unsigned sw=gfx.width(),sh=gfx.height(),n=gfx.granularity();
        if(&dest!=target||!gfx.elements()||!gfx.colors()||!sw||!sh||sw>64||sh>64||
           !n||n>256||mode>2||x<-4096||x>4096||y<-4096||y>4096){status|=8;return;}
        int a=std::max(l,clip.left()),c=std::max(t,clip.top());
        int d=std::min(r,clip.right()+1),e=std::min(b,clip.bottom()+1);
        if(a>=d||c>=e)return;
        if(!room(38+4*u64(n)+u64(sw)*sh))return;
        const auto *data=gfx.get_data(code%gfx.elements());
        const auto *pal=gfx.palette().pens()+gfx.colorbase()+n*(color%gfx.colors());
        put(4,1);rect(a,c,d,e);put(x,4);put(y,4);put(sw,2);put(sh,2);
        put(fx?1:0,1);put(fy?1:0,1);put(mode,1);put(trans,4);put(n,2);
        for(unsigned i=0;i<n;++i)put(u32(pal[i]),4);
        for(unsigned yy=0;yy<sh;++yy)for(unsigned xx=0;xx<sw;++xx){
            const auto p=data[yy*gfx.rowbytes()+xx];
            if(p>=n||(mode==2&&p>=32))status|=8;
            put(p,1);
        }
    }
    // Tag 5: two independently selected mixer operands and fixed-point
    // contributions. No final RGB pixels are used as inputs.
    template<class Dest,class Mix,class Color>
    void blend_row(const Dest &dest,int y,int start,int end,const Mix &z,const Color *clut){
        if(status)return;
        if(&dest!=target||!clut||y<t||y>=b){status|=4;return;}
        start=std::max(start,l);end=std::min(end,r);
        if(start>=end)return;
        if(!room(17+10*u64(end-start)))return;
        put(5,1);rect(start,y,end,y+1);
        for(int x=start;x<end;++x){
            if(z.src_blend[x]>8||z.dst_blend[x]>8)status|=8;
            put(u32(clut[z.src_pal[x]]),4);put(u32(clut[z.dst_pal[x]]),4);
            put(z.src_blend[x],1);put(z.dst_blend[x],1);
        }
    }
    // Tag 6: persistent PPU row source events, before background/sprite
    // visibility and priority composition. Events are published at copy time.
    template<class Dest,class Events>
    void ppu_row(const Dest &dest,int y,const Events &events){
        if(status)return;
        if(&dest!=target||y<t||y>=b||events.empty()||events.size()>1024){status|=8;return;}
        if(!room(25+8*u64(events.size())))return;
        put(6,1);rect(l,y,r,y+1);put(events.size(),4);
        for(const auto &event:events){
            put(event.x,2);put(event.kind,1);put(event.pen,1);put(event.color,4);
        }
    }
    template<class Bitmap> void finish(const Bitmap &dest){
        if(!out||finished)return;
        finished=true;u64 hash=offset;
        if(!(status&1))for(int y=t;y<b;++y)for(int x=l;x<r;++x){
            const u32 p=dest.pix(y,x);
            for(unsigned i=0;i<4;++i)mix(hash,p>>(8*i));
        }
        if(out->bytes+56+payload.size()+12>max_file){out->capped=true;return;}
        out->write("RGB1",4);out->integer(frame,8);
        for(int v:{w,h,l,t,r,b})out->integer(u32(v),4);
        out->integer(status,4);out->integer(ops,4);out->integer(payload.size(),4);
        out->integer(hash,8);out->write(payload.data(),payload.size());
        ++out->count;if(std::fflush(out->f)!=0)out->failed=true;
    }
};
inline void unsupported(){if(current&&!current->depth)current->status|=16;}
struct sprite_guard {
    scope *s=current;
    template<class... Args> explicit sprite_guard(Args&&...args){
        if(s){if(!s->depth)s->sprite(args...);++s->depth;}
    }
    ~sprite_guard(){if(s)--s->depth;}
    sprite_guard(const sprite_guard&)=delete;
};
template<class Dest,class Rect> inline void fill(Dest &dest,u32 color,const Rect &clip){
    if(current)current->fill(dest,clip,color);
    dest.fill(color,clip);
}
template<class Dest> inline void point(Dest &dest,int x,int y,u32 color){
    if(current)current->point(dest,x,y,color);
    dest.pix(y,x)=color;
}
}
#endif
