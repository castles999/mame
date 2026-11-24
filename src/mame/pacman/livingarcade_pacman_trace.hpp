// Living Arcade Pac-Man-family graphics state and ordered sprite-draw trace.
// Research-only opt-in local capture; dumps user-local decoded graphics once.
// Treat output as game-derived copyrighted data; NEVER distribute traces.
// Intended as a locally-added source file in src/mame/pacman/ of a MAME checkout.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace livingarcade_capture {
class Sink {
public:
    Sink() {
        const char* path = std::getenv("LIVINGARCADE_PACMAN_TRACE");
        if(!path || !path[0]) return;
        file_.open(path, std::ios::binary | std::ios::trunc);
        if(file_) {
            file_.write("LACP0001",8);
            active_=static_cast<bool>(file_);
        }
    }
    bool enabled() const noexcept { return active_; }
    template<class Gfx> void dictionary(uint8_t id,Gfx* gfx) {
        if(!active_ || !gfx || (dictionaryMask_ & (1u<<id)))return;
        const auto w=unsigned(gfx->width()),h=unsigned(gfx->height()),n=unsigned(gfx->elements());
        if(id>1 || w==0 || h==0 || w>64 || h>64 || n>4096 ||
           uint64_t(w)*h*n>16*1024*1024 || gfx->rowbytes()<w){ active_=false;return; }
        put8(4);put8(id);put16(static_cast<uint16_t>(w));put16(static_cast<uint16_t>(h));
        put32(n);put32(gfx->colorbase());put16(gfx->granularity());put16(gfx->depth());
        for(unsigned code=0;code<n;++code){
            const auto *pixels=gfx->get_data(code);
            for(unsigned row=0;row<h;++row)
                file_.write(reinterpret_cast<const char*>(pixels+row*gfx->rowbytes()),w);
        }
        dictionaryMask_|=uint8_t(1u<<id);active_=static_cast<bool>(file_);
    }
    template<class Palette> void palette(const Palette& palette) {
        if(!active_ || paletteWritten_)return;
        auto n=unsigned(palette.entries());
        if(n==0 || n>4096){active_=false;return;}
        put8(5);put16(static_cast<uint16_t>(n));
        for(unsigned i=0;i<n;++i){
            auto pen=palette.pen(i);
            auto rgb=palette.pen_color(pen);
            put16(static_cast<uint16_t>(pen));
            put32((uint32_t(rgb.r())<<16)|(uint32_t(rgb.g())<<8)|uint32_t(rgb.b()));
        }
        paletteWritten_=true;active_=static_cast<bool>(file_);
    }
    void begin(uint64_t frame, int x0, int y0, int x1, int y1,
               uint8_t cb, uint8_t sb, uint8_t pb, uint8_t tb, uint8_t flip, uint8_t priority,
               const uint8_t* video, const uint8_t* colors,
               const uint8_t* sprites, unsigned spriteLen,
               const uint8_t* spriteXY, unsigned spriteXYLen) {
        if(!active_)return;
        if(!video || spriteLen>65535 || spriteXYLen>65535 ||
           x0<-32768 || y0<-32768 || x1>32767 || y1>32767) { active_=false;return; }
        put8(1);put64(frame);
        put16(static_cast<uint16_t>(x0));put16(static_cast<uint16_t>(y0));
        put16(static_cast<uint16_t>(x1));put16(static_cast<uint16_t>(y1));
        put8(cb);put8(sb);put8(pb);put8(tb);put8(flip);put8(priority);
        put16(static_cast<uint16_t>(spriteLen));put16(static_cast<uint16_t>(spriteXYLen));
        file_.write(reinterpret_cast<const char*>(video),1024);
        if(colors) file_.write(reinterpret_cast<const char*>(colors),1024);
        else { const char zero[1024]={};file_.write(zero,1024); }
        if(sprites && spriteLen)file_.write(reinterpret_cast<const char*>(sprites),spriteLen);
        if(spriteXY && spriteXYLen)file_.write(reinterpret_cast<const char*>(spriteXY),spriteXYLen);
        active_=static_cast<bool>(file_);
    }
    void sprite(unsigned code,unsigned color,unsigned fx,unsigned fy,int sx,int sy,uint32_t transparentMask) {
        if(!active_)return;
        if(code>65535 || color>65535 || sx<-32768 || sx>32767 || sy<-32768 || sy>32767){active_=false;return;}
        put8(2);put16(static_cast<uint16_t>(code));put16(static_cast<uint16_t>(color));
        put8(static_cast<uint8_t>(fx));put8(static_cast<uint8_t>(fy));
        put16(static_cast<uint16_t>(sx));put16(static_cast<uint16_t>(sy));put32(transparentMask);
        active_=static_cast<bool>(file_);
    }
    template<class Pixel> void end(int x0,int y0,int x1,int y1,Pixel&& indexAt) {
        if(!active_)return;
        // FNV-1a of MAME's *indexed* output, in clip rectangle raster order.
        uint32_t crc=2166136261u;
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
            uint16_t v=static_cast<uint16_t>(indexAt(y,x));
            crc=(crc^(v&255u))*16777619u;crc=(crc^(v>>8))*16777619u;
        }
        put8(3);put32(crc);active_=static_cast<bool>(file_);
        file_.flush();
    }
private:
    void put8(uint8_t v) { char c=static_cast<char>(v);file_.write(&c,1); }
    void put16(uint16_t v) { put8(v&255u);put8(v>>8); }
    void put32(uint32_t v) { for(int i=0;i<4;++i)put8((v>>(8*i))&255u); }
    void put64(uint64_t v) { for(int i=0;i<8;++i)put8((v>>(8*i))&255u); }
    std::ofstream file_;
    bool active_=false;
    uint8_t dictionaryMask_=0;
    bool paletteWritten_=false;
};
inline Sink& trace() {static Sink s;return s;}
}  // namespace livingarcade_capture