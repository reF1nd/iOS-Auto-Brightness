// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace brightness;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static Profile panel(){return Profile::parse("IAB_PROFILE_V1 test 3\n0 0\n100 0.1\n1000 1\n");}
static Frame frame(){Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=100;f.back=100;f.current=.1;f.maximum=1;f.temp=30;f.battery=80;f.local_hour=12;return f;}
static Output run(Controller& c,Frame& f,int count){Output o;for(int i=0;i<count;i++){f.now+=250;o=c.step(f);}return o;}
int main(){try {
    check(!Config::parse("").settings_learning,"compatibility learning is opt-in");
    auto compatibility=Config::parse("settings_learning=1\n");
    check(Config::parse(compatibility.serialize()).settings_learning,"compatibility setting survives save/reload");
    Controller c;c.profile=panel();c.config.hyst_low=0;c.config.hyst_high=0;Frame f=frame();
    for(int hour:{0,5,6,17,18,23}) {
        c.reset();f.local_hour=hour;c.step(f);run(c,f,30);
        double scale=(hour<6||hour>=18)?.95:1;
        check(c.nightScale()==scale,"night boundary");
        check(std::abs(c.desired()-.1*scale)<.00001,"night scales default target");
    }
    c.learning.add(100,.2,1000);c.reset();f.local_hour=23;c.step(f);run(c,f,30);
    check(std::abs(c.desired()-.2)<.00001,"night preserves explicit preference");
    c.config.night_enabled=false;check(c.nightScale()==1,"night switch");
    c.learning.add(100,.9,2000);c.reset();f.current=.9;f.battery=14;c.step(f);auto o=run(c,f,10);
    check(o.reason=="low_battery_limit"&&o.value<=.7,"low battery caps an existing high output immediately");
    f.maximum=.4;o=run(c,f,1);check(o.value<=.4,"system cap remains authoritative");
    f.maximum=1;f.charging=true;o=run(c,f,40);check(o.value>.89,"charging removes extra low battery cap");
    f.charging=false;f.battery=15;o=run(c,f,10);check(o.value>.89,"15 percent boundary");
    f.battery=14;c.config.low_battery_enabled=false;o=run(c,f,10);check(o.value>.89,"cap switch");

    Controller solar;solar.profile=panel();solar.config.high_lux_duration_ms=1000;f=frame();
    solar.step(f);run(solar,f,10);f.lux=30000;f.back=30000;
    o=run(solar,f,1);check(o.reason!="sunlight","a single strong-light sample cannot engage boost");
    o=run(solar,f,50);check(o.reason=="sunlight","sustained safe light engages boost");
    f.lux=12000;f.back=12000;o=run(solar,f,100);check(o.reason=="sunlight","70 to 100 percent threshold band retains boost");
    f.lux=1000;f.back=1000;
    o=run(solar,f,1);check(o.reason=="sunlight","low light cannot cancel boost immediately");
    o=run(solar,f,50);check(o.reason!="sunlight","sustained light below exit threshold ends boost");
    f.lux=30000;f.back=30000;o=run(solar,f,50);check(o.reason=="sunlight","boost can re-enter");
    f.temp=46;o=run(solar,f,1);check(o.reason!="sunlight","unsafe temperature bypasses delayed exit");

    Controller dark;dark.profile=panel();f=frame();dark.step(f);run(dark,f,20);
    f.lux=50;f.back=50;run(dark,f,1);f.lux=20;f.back=20;run(dark,f,1);
    check(dark.fastDark(),"consistent front and rear decrease enables fast dark transition");
    dark.reset();f=frame();dark.step(f);run(dark,f,20);double before=dark.smoothLux();
    f.lux=20;f.back=300;run(dark,f,8);
    check(dark.smoothLux()==before&&!dark.fastDark()&&dark.ambientGuard(),"brief front-only drop is guarded");
    f.lux=100;run(dark,f,1);check(dark.smoothLux()==before,"recovered transient does not dim");
    f.lux=20;run(dark,f,40);check(dark.smoothLux()==before&&!dark.fastDark(),"time alone cannot validate persistent disagreement");

    Controller hysteresis;hysteresis.profile=panel();hysteresis.config.hyst_low=.1;hysteresis.config.hyst_high=0;
    f=frame();f.current=.1;hysteresis.learning.add(100,.105,10);hysteresis.step(f);o=run(hysteresis,f,20);
    check(o.value==.1,"brightening uses low-level hysteresis at low brightness");
    hysteresis.learning.add(100,.095,20);o=run(hysteresis,f,20);
    check(o.value==.1,"dimming uses the same level-dependent hysteresis");
    Controller idle;idle.profile=panel();f=frame();idle.step(f);run(idle,f,20);
    f.now+=8000;f.lux=1000;f.back=1000;o=idle.step(f);
    check(o.value<idle.desired(),"idle time before a new reading cannot skip its transition");
    auto cfg=Config::parse("manual_gamma=2.6\nalpha_fast=.5\n");
    check(cfg.manual_gamma==2.6&&Config::parse(cfg.serialize()).alpha_fast==.5,"legacy gamma and tuning survive roundtrip");
    std::cout<<"Night, low battery, sunlight hysteresis, transition and legacy config contracts passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
