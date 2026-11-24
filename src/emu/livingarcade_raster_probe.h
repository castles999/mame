// license:BSD-3-Clause
// LivingArcade research-only structural raster operation probe.
// Counts API entries during scoped CPS updates, NOT replay commands.
#ifndef MAME_EMU_LIVINGARCADE_RASTER_PROBE_H
#define MAME_EMU_LIVINGARCADE_RASTER_PROBE_H
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace livingarcade_raster_probe {
enum op : unsigned { tile, tile_aux, roz, gfx, gfx_aux, gfx_priority, gfx_zoom,
                     gfx_rgb, rows, cols, stars, palette, fill, priority_clear,
                     cps1_layers, cps2_layers, count };
inline constexpr const char *names[count] = {
    "tile","tile_aux","roz","gfx","gfx_aux","gfx_priority","gfx_zoom","gfx_rgb",
    "rows_configured","cols_configured","stars_enabled","palette_build_in_update",
    "fill","priority_clear","cps1_layers","cps2_layers"
};
struct update_scope;
inline thread_local update_scope *active = nullptr;
struct writer {
    std::FILE *file = nullptr;
    unsigned updates = 0;
    bool failed = false;
    writer() {
        const char *path = std::getenv("LIVINGARCADE_RASTER_PROBE");
        if (path && *path) {
            file = std::fopen(path, "wx");
            if (!file) std::fputs("LivingArcade raster probe: cannot create fresh receipt\n", stderr);
        }
    }
    ~writer() { if (file) std::fclose(file); }
    writer(const writer &) = delete;
    writer &operator=(const writer &) = delete;
};
inline writer &output() { static writer w; return w; }
inline constexpr unsigned max_updates = 16384;
inline constexpr std::uint64_t offset = 14695981039346656037ULL;
inline constexpr std::uint64_t prime = 1099511628211ULL;
inline void mix(std::uint64_t &h, unsigned byte) noexcept {
    h = (h ^ (byte & 255U)) * prime;
}
struct update_scope {
    writer *out = nullptr;
    update_scope *previous = nullptr;
    const void *target = nullptr;
    std::array<std::uint64_t,count> counters{};
    std::uint64_t order = offset;
    std::uint64_t frame;
    int l,t,r,b;
    bool finished = false;
    update_scope(const void *dest, std::uint64_t f, int left,int top,int right,int bottom)
        : frame(f),l(left),t(top),r(right),b(bottom) {
        auto &w = output();
        if (!w.file || w.failed || w.updates >= max_updates || active) return;
        out = &w; target = dest; previous = active; active = this;
    }
    ~update_scope() { if (out) active = previous; }
    update_scope(const update_scope &) = delete;
    update_scope &operator=(const update_scope &) = delete;
    void hit(op event) noexcept {
        ++counters[event]; mix(order, unsigned(event));
    }
    template<class Bitmap> void finish(const Bitmap &bitmap) noexcept {
        if (!out || finished) return;
        finished = true;
        bool valid = l>=0 && t>=0 && r>=l && b>=t &&
                     r<bitmap.width() && b<bitmap.height();
        std::uint64_t reference = offset;
        if (valid) for (int y=t;y<=b;++y) for (int x=l;x<=r;++x) {
            const auto v = std::uint16_t(bitmap.pix(y,x));
            mix(reference,v); mix(reference,v>>8);
        }
        auto *f = out->file;
        int rc = std::fprintf(f,
          "{\"schema\":1,\"update\":%u,\"frame\":%llu,\"clip\":[%d,%d,%d,%d],"
          "\"bitmap\":[%d,%d],\"valid\":%s,\"index_fnv64\":\"%016llx\","
          "\"order_fnv64\":\"%016llx\",\"counts\":{",
          out->updates,static_cast<unsigned long long>(frame),l,t,r,b,
          bitmap.width(),bitmap.height(),valid?"true":"false",
          static_cast<unsigned long long>(reference),static_cast<unsigned long long>(order));
        for (unsigned i=0;i<count && rc>=0;++i)
            rc = std::fprintf(f,"%s\"%s\":%llu",i?",":"",names[i],
                              static_cast<unsigned long long>(counters[i]));
        if (rc>=0) rc = std::fprintf(f,"}}\n");
        if (rc<0 || std::fflush(f)!=0) out->failed = true;
        ++out->updates;
    }
};
inline void hit(op event) noexcept { if (active) active->hit(event); }
inline void tile_entry(const void *dest,bool rotated,bool row,bool col) noexcept {
    if (!active) return;
    active->hit(tile);
    if (dest!=active->target) active->hit(tile_aux);
    if (rotated) active->hit(roz);
    if (row) active->hit(rows);
    if (col) active->hit(cols);
}
inline void gfx_entry(const void *dest,bool priority,bool zoom,bool rgb) noexcept {
    if (!active) return;
    active->hit(gfx);
    if (dest!=active->target) active->hit(gfx_aux);
    if (priority) active->hit(gfx_priority);
    if (zoom) active->hit(gfx_zoom);
    if (rgb) active->hit(gfx_rgb);
}
}
#endif
