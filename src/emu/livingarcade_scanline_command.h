// license:BSD-3-Clause
// LivingArcade CAP-5A: diagnostic RGB scanline commands, opt-in and private.
// Source writes are observed before touching the destination; final pixels
// are read ONLY for the independent witness. Not a shipping tile archive.
#ifndef MAME_EMU_LIVINGARCADE_SCANLINE_COMMAND_H
#define MAME_EMU_LIVINGARCADE_SCANLINE_COMMAND_H
#include "livingarcade_raster_command.h"
#include <memory>
#include <string>
#include <unordered_map>
namespace livingarcade_scanline_command {
using namespace livingarcade_raster_command;
inline constexpr u32 max_scanlines=262144, max_events=8192;
// Optional diagnostic frame window: inclusive start, exclusive end.
// Both values must be valid decimal frame numbers; malformed pairs record
// nothing, so the strict local verifier fails rather than accepting a prefix.
struct capture_window {
    u64 begin=0,end=0;
    bool enabled=false,valid=true;
    static bool parse(const char *p,u64 &value){
        if(!p||!*p)return false;
        value=0;
        for(;*p;++p){
            if(*p<'0'||*p>'9')return false;
            value=value*10+unsigned(*p-'0');
            if(value>1000000)return false;
        }
        return true;
    }
    capture_window(){
        const char *a=std::getenv("LIVINGARCADE_NEOGEO_START_FRAME");
        const char *b=std::getenv("LIVINGARCADE_NEOGEO_END_FRAME");
        enabled=a||b;
        if(enabled)valid=parse(a,begin)&&parse(b,end)&&begin<end;
    }
    bool contains(u64 frame)const{
        return !enabled||(valid&&frame>=begin&&frame<end);
    }
};
inline const capture_window &frame_window(){
    static const capture_window window;
    return window;
}
inline bool capture_frame(u64 frame){return frame_window().contains(frame);}

// CAP15.2: rotate each of the three independent streams by emulated frame
// number, not wall time. Each file retains the existing wire format/budget.
// The caller precreates a fresh private directory per chunk. No path is
// opened until its first frame; a failed/capped stream never opens a successor.
struct chunk_config {
    const char *root=std::getenv("LIVINGARCADE_NEOGEO_CHUNK_ROOT");
    u64 length=0;
    bool valid=false;
    chunk_config(){
        const auto &w=frame_window();
        valid=root&&*root&&w.enabled&&w.valid&&
            capture_window::parse(std::getenv("LIVINGARCADE_NEOGEO_CHUNK_FRAMES"),length)&&
            length>=1&&length<=296&&(w.end-w.begin)<=12*length;
    }
};
inline const chunk_config &chunks(){static const chunk_config c;return c;}
inline bool tile_mode(){
    static const bool enabled=[](){const char *p=std::getenv("LIVINGARCADE_NEOGEO_TILE_CAPTURE");return p&&std::strcmp(p,"1")==0;}();
    return enabled;
}
struct chunk_stream {
    const char *name,*magic;
    std::unique_ptr<writer> current;
    writer disabled{"","", ""};
    u64 index=0;
    bool stopped=false;
    chunk_stream(const char *n,const char *m):name(n),magic(m){}
    writer &get(u64 frame){
        const auto &c=chunks();
        if(stopped||!c.valid||!capture_frame(frame))return disabled;
        const u64 next=(frame-frame_window().begin)/c.length;
        if(current&&next==index)return *current;
        if((!current&&next!=0)||(current&&next!=index+1)){
            stopped=true;
            if(current)current->failed=true;
            return disabled;
        }
        if(current){
            current->close();
            if(current->failed||current->capped){stopped=true;return disabled;}
        }
        index=next;
        const std::string path=std::string(c.root)+"/chunk-"+std::to_string(index)+"/"+name;
        current=std::make_unique<writer>("",magic,path.c_str());
        if(!current->f||current->failed)stopped=true;
        return *current;
    }
};

inline writer &scanline_output(){
    static writer w("LIVINGARCADE_NEOGEO_COMMAND","LANE0001");
    return w;
}
inline writer &frame_output(){
    static writer w("LIVINGARCADE_NEOGEO_PRESENT","LANF0001");
    return w;
}
inline writer &command_for_frame(u64 frame){
    if(!chunks().root)return scanline_output();
    static chunk_stream stream("command.private.bin",tile_mode()?"LANT0001":"LANE0001");
    return stream.get(frame);
}
inline writer &publication_for_frame(u64 frame){
    if(!chunks().root)return frame_output();
    static chunk_stream stream("frame.private.bin","LANF0001");
    return stream.get(frame);
}
inline writer &exact_for_frame(u64 frame){
    if(!chunks().root)return exact_output();
    static chunk_stream stream("exact.private.bin","LARX0001");
    return stream.get(frame);
}
inline writer &secondary_frame_output(){static writer w("LIVINGARCADE_NEOGEO_PRESENT_SECONDARY","LANF0001");return w;}
// Complete RGB texture-source witness, independent of scanline commands.
template<class Bitmap,class Rect>
inline void frame_witness(const Bitmap &dest,u64 frame,const Rect &clip,writer *selected=nullptr){
    auto &o=selected?*selected:publication_for_frame(frame);
    if(!o.f||o.failed||o.capped)return;
    if(o.count>=max_updates){o.capped=true;return;}
    const int w=dest.width(),h=dest.height(),l=clip.left(),t=clip.top(),r=clip.right()+1,b=clip.bottom()+1;
    u32 status=0;
    if(!dest.valid()||w<=0||h<=0||w>1024||h>1024||l<0||t<0||r>w||b>h||l>=r||t>=b)status=1;
    u64 hash=offset;
    if(!status)for(int y=t;y<b;++y)for(int x=l;x<r;++x){
        const u32 c=dest.pix(y,x);
        for(unsigned i=0;i<4;++i)mix(hash,c>>(8*i));
    }
    if(o.bytes+48+12>max_file){o.capped=true;return;}
    o.write("FRN1",4);o.integer(frame,8);
    for(int v:{w,h,l,t,r,b})o.integer(u32(v),4);
    o.integer(status,4);o.integer(hash,8);
    ++o.count;if(std::fflush(o.f)!=0)o.failed=true;
}
// Per-chunk content-addressed indexed tiles and immutable palette snapshots.
// Definitions precede first use; no native pointers or final pixels in payload.
struct tile_dictionary {
    std::unordered_map<std::string,u16> tiles,palettes;
};
inline tile_dictionary &tile_dict(){static tile_dictionary d;return d;}
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
    std::vector<u8> tile_payload;
    unsigned draws=0;
    template<class Bitmap,class Rect>
    scanline_scope(const Bitmap &dest,u64 f,const Rect &clip,u32 background)
      :w(dest.width()),h(dest.height()),l(clip.left()),t(clip.top()),
       r(clip.right()+1),b(clip.bottom()+1),frame(f),bg(background){
        if(!capture_frame(f))return;
        auto &o=command_for_frame(f);
        if(!o.f||o.failed||o.capped||line_active)return;
        if(o.count>=max_scanlines){o.capped=true;return;}
        out=&o;line_active=this;
        if(tile_mode()&&!o.count){tile_dict().tiles.clear();tile_dict().palettes.clear();}
        if(!dest.valid()||w<=0||h<=0||w>1024||h>1024||
           l<0||r>w||l>=r||t<0||b>h||b!=t+1){status|=1;return;}
        row=reinterpret_cast<std::uintptr_t>(&dest.pix(t,0));
    }
    ~scanline_scope(){
        if(out){if(!finished)out->failed=true;line_active=nullptr;}
    }
    scanline_scope(const scanline_scope&)=delete;
    scanline_scope& operator=(const scanline_scope&)=delete;
    void append(u64 v,unsigned n){for(unsigned i=0;i<n;++i)tile_payload.push_back(u8(v>>(8*i)));}
    template<class Pen>
    void source_row(const u8 *src,unsigned side,unsigned sy,int x,u16 mask,bool flip,bool wrap,const Pen *palette){
        if(!out||status||!tile_mode())return;
        if(!src||!palette||(side!=8&&side!=16)||sy>=side||x<-16||x>383||
           !mask||(side==8&&(mask!=0xff00||flip||wrap))||++draws>136){status|=8;return;}
        std::string key(1,char(side));
        for(unsigned i=0;i<side*side;i+=2){
            if(src[i]>15||src[i+1]>15){status|=8;return;}
            key.push_back(char(src[i]|(src[i+1]<<4)));
        }
        auto &d=tile_dict();
        auto ti=d.tiles.find(key);
        if(ti==d.tiles.end()){
            if(d.tiles.size()>=65535){status|=8;return;}
            const u16 id=u16(d.tiles.size());
            ti=d.tiles.emplace(key,id).first;
            append(1,1);append(id,2);
            tile_payload.insert(tile_payload.end(),key.begin(),key.end());
        }
        std::string pk;pk.reserve(64);
        for(unsigned i=0;i<16;++i)for(unsigned j=0;j<4;++j)pk.push_back(char(u32(palette[i])>>(8*j)));
        auto pi=d.palettes.find(pk);
        if(pi==d.palettes.end()){
            if(d.palettes.size()>=65535){status|=8;return;}
            const u16 id=u16(d.palettes.size());
            pi=d.palettes.emplace(pk,id).first;
            append(2,1);append(id,2);tile_payload.insert(tile_payload.end(),pk.begin(),pk.end());
        }
        append(3,1);append(ti->second,2);append(pi->second,2);append(u16(x),2);
        append(mask,2);append(sy,1);append((flip?1:0)|(wrap?2:0),1);
    }
    void put(const u32 *dst,unsigned pen,u32 color,unsigned layer){
        if(status||tile_mode())return;
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
        const u64 payload_size=tile_mode()?tile_payload.size():8*u64(events.size());
        if(out->bytes+56+payload_size+12>max_file){out->capped=true;return;}
        out->write(tile_mode()?"TIL1":"LIN1",4);out->integer(frame,8);
        for(int v:{w,h,l,t,r,b})out->integer(u32(v),4);
        out->integer(status,4);out->integer(bg,4);out->integer(tile_mode()?tile_payload.size():events.size(),4);
        out->integer(hash,8);
        if(tile_mode())out->write(tile_payload.data(),tile_payload.size());
        else for(const auto &e:events){
            out->integer(e.x,2);out->integer(e.pen,1);
            out->integer(e.layer,1);out->integer(e.color,4);
        }
        ++out->count;
        if(std::fflush(out->f)!=0)out->failed=true;
    }
};
inline bool tile_active(){return tile_mode()&&line_active&&line_active->out&&!line_active->status;}
template<class Pen>
inline void source_tile_row(const u8 *src,unsigned side,unsigned sy,int x,u16 mask,bool flip,bool wrap,const Pen *palette){
    if(tile_active())line_active->source_row(src,side,sy,x,mask,flip,wrap,palette);
}
inline void source_pixel(const u32 *dst,unsigned pen,u32 color,unsigned layer){
    if(line_active)line_active->put(dst,pen,color,layer);
}
}
#endif
