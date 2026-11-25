// license:BSD-3-Clause
// CAP12: main-thread capture of copied blitter input words and screen views.
#ifndef MAME_EMU_LIVINGARCADE_CV1K_TRACE_H
#define MAME_EMU_LIVINGARCADE_CV1K_TRACE_H
#include "livingarcade_raster_command.h"
namespace livingarcade_cv1k_trace {
using namespace livingarcade_raster_command;
inline writer &output(){static writer w("LIVINGARCADE_CV1K_TRACE","LACV0001");return w;}
struct list;
inline thread_local list *active=nullptr;
struct list {
    writer *out=nullptr;
    std::vector<u16> words;
    int l,t,r,b;
    template<class Rect> explicit list(const Rect &clip)
      :l(clip.left()),t(clip.top()),r(clip.right()+1),b(clip.bottom()+1){
        auto &o=livingarcade_cv1k_trace::output();
        if(!o.f||o.failed||o.capped)return;
        if(active||o.count>=1000000){o.capped=true;return;}
        out=&o;active=this;
    }
    list(const list&)=delete;
    ~list(){
        if(!out)return;
        active=nullptr;
        if(out->capped||out->failed)return;
        if(out->bytes+24+2*u64(words.size())+12>max_file){out->capped=true;return;}
        out->write("LIST",4);
        for(int v:{l,t,r,b})out->integer(u32(v),4);
        out->integer(words.size(),4);
        for(u16 v:words)out->integer(v,2);
        ++out->count;
    }
};
inline void word(u16 v){
    if(!active||active->out->capped)return;
    if(active->words.size()>=max_payload/2){active->out->capped=true;return;}
    active->words.push_back(v);
}
template<class Rect> void view(u64 frame,int w,int h,const Rect &clip,int sx,int sy){
    auto &o=livingarcade_cv1k_trace::output();
    if(!o.f||o.failed||o.capped)return;
    if(active||o.count>=1000000||o.bytes+44+12>max_file){o.capped=true;return;}
    o.write("VIEW",4);o.integer(frame,8);
    for(int v:{w,h,clip.left(),clip.top(),clip.right()+1,clip.bottom()+1,sx,sy})o.integer(u32(v),4);
    ++o.count;
}
}
#endif
