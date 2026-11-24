// license:BSD-3-Clause
// LivingArcade CAP-4B1: private, bounded, diagnostic indexed command capture.
// Tile runs contain intermediate layer data, not final framebuffer pixels.
// Research-only: do not distribute ROM-derived source pixels.
#ifndef MAME_EMU_LIVINGARCADE_RASTER_COMMAND_H
#define MAME_EMU_LIVINGARCADE_RASTER_COMMAND_H
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <limits>
namespace livingarcade_raster_command {
using u8 = std::uint8_t; using u16 = std::uint16_t;
using u32 = std::uint32_t; using u64 = std::uint64_t;
inline constexpr unsigned max_payload = 8*1024*1024;
inline constexpr u64 max_file = 512ULL*1024*1024;
inline constexpr unsigned max_updates = 4096;
inline constexpr u64 offset = 14695981039346656037ULL, prime = 1099511628211ULL;
inline void mix(u64 &h,unsigned b){h=(h^(b&255))*prime;}
struct writer {
    std::FILE *f=nullptr; u64 bytes=0; u32 count=0; bool failed=false,capped=false;
    explicit writer(const char *env="LIVINGARCADE_RASTER_COMMAND",const char *magic="LARC0001"){
        const char *p=std::getenv(env);
        if(!p||!*p)return;
        f=std::fopen(p,"wbx");
        if(!f){std::fputs("LivingArcade command capture: fresh output required\n",stderr);return;}
        write(magic,8);
    }
    void write(const void *p,std::size_t n){
        if(!f||failed)return;
        if(std::fwrite(p,1,n,f)!=n)failed=true; else bytes+=n;
    }
    void integer(u64 v,unsigned n){u8 b[8];for(unsigned i=0;i<n;++i)b[i]=u8(v>>(8*i));write(b,n);}
    ~writer(){
        if(!f)return;
        if(!failed){write("END1",4);integer(count,4);integer(capped?1:0,4);}
        std::fclose(f);
    }
};
inline writer &output(){static writer w;return w;}
inline writer &palette_output(){static writer w("LIVINGARCADE_RASTER_PALETTE","LARP0001");return w;}
inline writer &present_output(){static writer w("LIVINGARCADE_RASTER_PRESENT","LARF0001");return w;}
// MAME appends reserved black and white entries after the ordinary colors.
// entry_adjusted_color() returns black for either out-of-range special index;
// the renderer instead reads entry_list_adjusted(), which includes both.
// Keep shadow/highlight groups fail-closed until their replay is qualified.
template<class Palette>
inline bool snapshot_colors(const Palette &palette,std::vector<u32> &colors){
    const u32 entries=palette.entries();
    if(!palette.palette()||!entries||entries>65536||palette.shadows_enabled()||palette.highlights_enabled())
        return false;
    const u32 n=std::min<u32>(65536,entries+2);
    const auto *adjusted=palette.palette()->entry_list_adjusted();
    colors.reserve(n);
    for(u32 i=0;i<n;++i)colors.push_back(u32(adjusted[i])&0xffffff);
    return true;
}
// Independent witness of the indexed texture source just before set_bitmap.
// Not the post-OSD, scaled, CRT-filtered, brightness-modulated display image.
template<class Bitmap,class Palette,class Rect>
inline void present_witness(const Bitmap &dest,const Palette &palette,u64 frame,const Rect &clip){
    auto &p=present_output();
    if(!p.f||p.failed||p.capped)return;
    if(p.count>=max_updates){p.capped=true;return;}
    const int w=dest.width(),h=dest.height(),l=clip.left(),t=clip.top(),r=clip.right()+1,b=clip.bottom()+1;
    u32 status=0;
    if(!dest.valid()||w<=0||h<=0||w>1024||h>1024||l<0||t<0||r>w||b>h||l>=r||t>=b)status|=1;
    u64 indexed=offset,rgb=offset;
    std::vector<u32> colors;
    if(!status&&!snapshot_colors(palette,colors))status|=32;
    if(!status){
        for(int y=t;y<b&&!status;++y)for(int x=l;x<r;++x){
            const auto pen=u32(dest.pix(y,x));
            if(pen>=colors.size()){status|=32;break;}
            mix(indexed,pen);mix(indexed,pen>>8);
            const u32 c=colors[pen];mix(rgb,c>>16);mix(rgb,c>>8);mix(rgb,c);
        }
    }
    if(status)colors.clear();
    if(p.bytes+64+4*u64(colors.size())+12>max_file){p.capped=true;return;}
    p.write("FRM1",4);p.integer(frame,8);
    for(int v:{w,h,l,t,r,b})p.integer(u32(v),4);
    p.integer(p.count,4);p.integer(status,4);p.integer(colors.size(),4);
    p.integer(indexed,8);p.integer(rgb,8);
    for(u32 c:colors)p.integer(c,4);
    ++p.count;if(std::fflush(p.f)!=0)p.failed=true;
}

struct scope;
inline thread_local scope *active=nullptr;
struct scope {
    writer *out=nullptr; const void *target=nullptr,*priority=nullptr;
    std::vector<u8> payload; u32 status=0,ops=0; u64 frame;
    int l,t,r,b,w,h;
    unsigned sprite_depth=0;bool finished=false;
    template<class Bitmap,class Priority,class Rect>
    scope(const Bitmap &dest,const Priority &pri,u64 f,const Rect &clip)
      :frame(f),l(clip.left()),t(clip.top()),r(clip.right()+1),b(clip.bottom()+1),
       w(dest.width()),h(dest.height()){
        auto &o=output();
        if(!o.f||o.failed||o.capped||active)return;
        if(o.count>=max_updates){o.capped=true;return;}
        out=&o;target=&dest;priority=&pri;active=this;
        if(w<=0||h<=0||w>1024||h>1024||l<0||t<0||r>w||b>h||l>=r||t>=b||
           !pri.valid()||pri.width()<r||pri.height()<b)status|=1;
    }
    ~scope(){if(out){if(!finished)out->failed=true;active=nullptr;}}
    scope(const scope&)=delete;scope& operator=(const scope&)=delete;
    void put(u64 v,unsigned n){for(unsigned i=0;i<n;++i)payload.push_back(u8(v>>(8*i)));}
    bool room(u64 n){
        if(status)return false;
        if(n>max_payload-payload.size()||ops>=65536){status|=2;return false;}
        ++ops;return true;
    }
    bool bounds(int x,int y,int x2,int y2)const{return x>=l&&y>=t&&x2<=r&&y2<=b&&x<x2&&y<y2;}
    void rect(int x,int y,int x2,int y2){put(x,4);put(y,4);put(x2,4);put(y2,4);}
    void clear(unsigned tag,unsigned value){
        if(!room(19))return;
        put(tag,1);rect(l,t,r,b);put(value,2);
    }
    // Exact run geometry after MAME has updated dirty tiles and lowered scroll.
    // Auxiliary indexed targets only contribute priority in the current CPS scope.
    template<class Dest,class Pri,class Pix,class Flags>
    void tile_run(const Dest &dest,const Pri &pri,const Pix &pix,const Flags &flags,
                  int sx,int sy,int ex,int ey,int dx,int dy,unsigned mask,unsigned value,
                  u32 pcode,bool opaque,unsigned alpha){
        if(status)return;
        if(sizeof(typename Dest::pixel_t)!=2||!dest.valid()||&pri!=priority||alpha!=255||
           sx<0||sy<0||ex>pix.width()||ey>pix.height()||sx>=ex||sy>=ey||
           !bounds(dx,dy,dx+ex-sx,dy+ey-sy)){status|=4;return;}
        u64 n=u64(ex-sx)*(ey-sy);
        if(!room(24+3*n))return;
        put(3,1);put(&dest==target?0:1,1);rect(dx,dy,dx+ex-sx,dy+ey-sy);
        put(pcode,4);put(opaque?0:mask,1);put(opaque?0:value,1);
        for(int y=sy;y<ey;++y)for(int x=sx;x<ex;++x){put(pix.pix(y,x),2);put(flags.pix(y,x),1);}
    }
    template<class Gfx,class Dest,class Pri,class Rect>
    void sprite(Gfx &gfx,const Dest &dest,const Pri &pri,const Rect &clip,
                u32 code,u32 color,int fx,int fy,int x,int y,u32 pmask,u32 trans){
        if(status)return;
        unsigned sw=gfx.width(),sh=gfx.height();
        if(&dest!=target||&pri!=priority||!gfx.elements()||!gfx.colors()||
           !sw||!sh||sw>64||sh>64||trans>=32||x<-4096||x>4096||y<-4096||y>4096){
            status|=8;return;
        }
        int cl=std::max(l,clip.left()),ct=std::max(t,clip.top());
        int cr=std::min(r,clip.right()+1),cb=std::min(b,clip.bottom()+1);
        if(cl>=cr||ct>=cb)return;
        u64 n=u64(sw)*sh;
        if(!room(39+n))return;
        const auto *data=gfx.get_data(code%gfx.elements());
        put(4,1);rect(cl,ct,cr,cb);put(x,4);put(y,4);put(sw,2);put(sh,2);
        put(fx?1:0,1);put(fy?1:0,1);put(gfx.colorbase()+gfx.granularity()*(color%gfx.colors()),2);
        put(pmask,4);put(trans,1);
        for(unsigned yy=0;yy<sh;++yy)for(unsigned xx=0;xx<sw;++xx){
            const auto p=data[yy*gfx.rowbytes()+xx];if(p>=32)status|=8;put(p,1);
        }
    }
    // Tag 5 declares undefined initial color: replay must prove every pixel
    // was written, never silently substitute a black framebuffer.
    void undefined_color(){clear(5,0);}
    // Tag 6 preserves initial priority only when hardware does not clear it.
    // This is input state, not a captured final color plane.
    template<class Pri> void seed_priority(const Pri &pri){
        if(status)return;
        if(&pri!=priority){status|=4;return;}
        const u64 n=u64(r-l)*(b-t);
        if(!room(17+n))return;
        put(6,1);rect(l,t,r,b);
        for(int y=t;y<b;++y)for(int x=l;x<r;++x)put(pri.pix(y,x),1);
    }
    // Tag 7: ordered source gfx draw, no priority writes, 32-bit transparent mask.
    template<class Gfx,class Dest,class Rect>
    void plain_sprite(Gfx &gfx,const Dest &dest,const Rect &clip,
                      u32 code,u32 color,int fx,int fy,int x,int y,u32 mask,unsigned tag=7,u32 trans=0){
        if(status)return;
        unsigned sw=gfx.width(),sh=gfx.height();
        if(&dest!=target||!gfx.elements()||!gfx.colors()||!sw||!sh||sw>64||sh>64||
           x<-4096||x>4096||y<-4096||y>4096){status|=8;return;}
        int cl=std::max(l,clip.left()),ct=std::max(t,clip.top());
        int cr=std::min(r,clip.right()+1),cb=std::min(b,clip.bottom()+1);
        if(cl>=cr||ct>=cb)return;
        if(!room(38+u64(sw)*sh))return;
        const auto *data=gfx.get_data(code%gfx.elements());
        put(tag,1);rect(cl,ct,cr,cb);put(x,4);put(y,4);put(sw,2);put(sh,2);
        put(fx?1:0,1);put(fy?1:0,1);put(gfx.colorbase()+gfx.granularity()*(color%gfx.colors()),2);
        put(mask,4);put(trans,1);
        for(unsigned yy=0;yy<sh;++yy)for(unsigned xx=0;xx<sw;++xx){
            const auto p=data[yy*gfx.rowbytes()+xx];if(p>=32)status|=8;put(p,1);
        }
    }
    // Tag 8: direct source pen (e.g. generated star), not a framebuffer read.
    template<class Dest> void point(const Dest &dest,int x,int y,u32 pen){
        if(status)return;
        if(&dest!=target||!bounds(x,y,x+1,y+1)||pen>65535){status|=16;return;}
        if(!room(11))return;
        put(8,1);put(x,4);put(y,4);put(pen,2);
    }
    // Tag 9: explicit ordered destination-to-destination pixel move.
    template<class Dest> void move_pixel(const Dest &dest,int sx,int sy,int dx,int dy){
        if(status)return;
        if(&dest!=target||!bounds(sx,sy,sx+1,sy+1)||!bounds(dx,dy,dx+1,dy+1)){
            status|=16;return;
        }
        if(!room(17))return;
        put(9,1);put(sx,4);put(sy,4);put(dx,4);put(dy,4);
    }
    // Optional companion stream, correlated one-to-one with committed UPD1 records.
    // This samples the effective adjusted palette at callback completion, NOT final
    // OSD scanout. Palette changes between callbacks remain a separate gate.
    template<class Bitmap,class Palette>
    void capture_palette(const Bitmap &dest,const Palette &palette){
        if(!out||!finished||out->failed||out->capped)return;
        auto &p=palette_output();
        if(!p.f||p.failed||p.capped)return;
        if(p.count+1!=out->count){p.failed=true;return;}
        u32 flags=status;
        u64 rgb_hash=offset;
        std::vector<u32> colors;
        if(!flags&&!snapshot_colors(palette,colors))flags|=32;
        if(!flags){
            for(int y=t;y<b&&!flags;++y)for(int x=l;x<r;++x){
                const auto pen=u32(dest.pix(y,x));
                if(pen>=colors.size()){flags|=32;break;}
                const u32 c=colors[pen];
                mix(rgb_hash,c>>16);mix(rgb_hash,c>>8);mix(rgb_hash,c);
            }
        }
        if(flags)colors.clear();
        if(p.bytes+56+4*u64(colors.size())+12>max_file){p.capped=true;return;}
        p.write("PAL1",4);p.integer(frame,8);
        for(int v:{w,h,l,t,r,b})p.integer(u32(v),4);
        p.integer(p.count,4);p.integer(flags,4);p.integer(colors.size(),4);p.integer(rgb_hash,8);
        for(u32 c:colors)p.integer(c,4);
        ++p.count;if(std::fflush(p.f)!=0)p.failed=true;
    }
    template<class Bitmap,class Pri>
    void finish(const Bitmap &dest,const Pri &pri){
        if(!out||finished)return;
        finished=true;u64 ih=offset,ph=offset;
        if(!(status&1))for(int y=t;y<b;++y)for(int x=l;x<r;++x){
            u16 p=dest.pix(y,x);mix(ih,p);mix(ih,p>>8);mix(ph,pri.pix(y,x));
        }
        if(out->bytes+64+payload.size()+12>max_file){out->capped=true;return;}
        out->write("UPD1",4);out->integer(frame,8);
        for(int v:{w,h,l,t,r,b})out->integer(u32(v),4);
        out->integer(status,4);out->integer(ops,4);out->integer(payload.size(),4);
        out->integer(ih,8);out->integer(ph,8);out->write(payload.data(),payload.size());
        ++out->count;if(std::fflush(out->f)!=0)out->failed=true;
    }
};
inline void unsupported(){if(active&&!active->sprite_depth)active->status|=16;}
struct sprite_guard {
    scope *s=active;
    template<class... Args> explicit sprite_guard(Args&&...args){
        if(s){if(!s->sprite_depth)s->sprite(args...);++s->sprite_depth;}
    }
    ~sprite_guard(){if(s)--s->sprite_depth;}
    sprite_guard(const sprite_guard&)=delete;
};
struct plain_sprite_guard {
    scope *s=active;
    template<class... Args> explicit plain_sprite_guard(Args&&...args){
        if(s){if(!s->sprite_depth)s->plain_sprite(args...);++s->sprite_depth;}
    }
    ~plain_sprite_guard(){if(s)--s->sprite_depth;}
    plain_sprite_guard(const plain_sprite_guard&)=delete;
};
template<class Dest> inline void point(const Dest &dest,int x,int y,u32 pen){
    if(active)active->point(dest,x,y,pen);
}
template<class Dest> inline void move_pixel(const Dest &dest,int sx,int sy,int dx,int dy){
    if(active)active->move_pixel(dest,sx,sy,dx,dy);
}
inline void clear(unsigned tag,unsigned value){if(active)active->clear(tag,value);}
}
#endif
