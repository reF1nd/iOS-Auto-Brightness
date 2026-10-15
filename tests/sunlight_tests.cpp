// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace brightness;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static Controller controller(){Controller c;c.profile=Profile::parse("IAB_PROFILE_V1 test 3\n0 0\n100 0.1\n1000 1\n");c.config.hyst_low=0;c.config.hyst_high=0;return c;}
static Frame frame(){Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=f.front=f.back=30000;f.current=.001;f.minimum=.001;f.maximum=1;f.temp=30;f.battery=80;f.local_hour=12;return f;}
static Output advance(Controller& c,Frame& f,int ms=250){f.now+=ms;auto o=c.step(f);if(o.apply)f.current=o.value;return o;}
int main(){try {
    auto c=controller();auto f=frame();
    check(!c.step(f).apply,"first strong wake sample leaves OEM in control");
    auto o=advance(c,f);
    check(o.apply&&o.reason=="sunlight"&&o.value>=.9,"confirmed dual-side sunny wake skips long dim ramp");
    c=controller();f=frame();f.current=.98;c.step(f);o=advance(c,f);
    check(o.value>=.98,"bright OEM wake frame is not pulled down during settling");
    c=controller();f=frame();f.maximum=.2;c.step(f);o=advance(c,f);
    check(o.apply&&o.value<=.2,"fast wake obeys current system cap");
    f.maximum=.05;o=advance(c,f);check(o.value<=.05,"cap reductions bypass fast wake preservation");
    f.blocked=true;o=advance(c,f);check(!o.apply,"system thermal override immediately wins");
    f.blocked=false;o=advance(c,f);check(!o.apply,"protection interruption invalidates fast-path evidence");
    for(double front: {-1.0,0.0}) {
        c=controller();f=frame();f.front=front;c.step(f);
        check(!advance(c,f).apply,"rear-only illumination needs longer confirmation");
        check(!advance(c,f).apply,"no early single-side takeover");
        check(advance(c,f).apply,"persistent single-side sun still responds within 750 ms of first sample");
    }
    for(int failure=0;failure<5;failure++) {
        c=controller();f=frame();
        if(failure==0)f.temp=-1;
        if(failure==1)f.temp=46;
        if(failure==2)f.battery=20;
        if(failure==3)f.near=true;
        if(failure==4)f.blocked=true;
        c.step(f);check(!advance(c,f).apply,"unsafe or obscured wake cannot use fast boost");
    }
    c=controller();f=frame();c.step(f);c.hold(f.now);o=advance(c,f,1000);
    check(!o.apply&&o.reason=="user_adjustment","manual adjustment hold cannot be bypassed");
    c=controller();f=frame();f.lux=f.front=f.back=100;c.step(f);
    check(!advance(c,f).apply,"ordinary indoor wake keeps settling protection");
    advance(c,f,2000);
    f.lux=f.front=f.back=30000;advance(c,f);f.lux=f.front=f.back=100;o=advance(c,f);
    check(o.reason!="sunlight","one-frame flash cannot engage sunlight");
    f.lux=f.front=f.back=30000;advance(c,f);advance(c,f);o=advance(c,f);
    check(o.reason=="sunlight"&&o.value>=.9,"confirmed sun while already awake reaches target promptly");

    // A fixed learned normal target isolates the enhancement curve itself.
    double previous=0;
    for(double lux: {15000.0,15001.0,22500.0,30000.0}) {
        c=controller();f=frame();f.lux=f.front=f.back=lux;c.learning.add(lux,.1,1);
        c.step(f);o=advance(c,f);
        const double progress=std::clamp((lux-15000)/15000,0.0,1.0);
        const double expected=std::max(.1*(1+progress*.35),.1+progress*.8);
        check(std::abs(o.value-expected)<1e-8,"enhancement is continuous from threshold to full-sun floor");
        check(o.value>=previous,"sunlight enhancement remains monotonic");previous=o.value;
    }
    c=controller();c.config.high_lux_duration_ms=1000;f=frame();c.step(f);advance(c,f);
    f.lux=f.front=f.back=12000;
    for(int i=0;i<100;i++)o=advance(c,f);
    check(o.reason=="sunlight","hysteresis band retains enhancement");
    f.lux=f.front=f.back=0;o=advance(c,f);
    check(o.reason=="sunlight","entering shade cannot immediately remove enhancement");
    bool exited=false;
    for(int i=0;i<120;i++) {
        double before=f.current;o=advance(c,f);
        check(o.value+1e-10>=before*std::pow(.8,.25),"sunlight exit always obeys slow downward slew");
        if(o.reason!="sunlight")exited=true;
    }
    check(exited&&f.current<.01,"sustained darkness eventually dims");
    f.maximum=.002;o=advance(c,f);check(o.value<=.002,"exit smoothing never delays a safety cap");
    std::cout<<"Fast sunny wake, continuous enhancement, protected handoff and slow exit passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
