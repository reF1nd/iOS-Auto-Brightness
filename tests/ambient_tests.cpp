// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace brightness;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static Frame initial(){Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=9.3;f.back=114;f.current=.08925;f.minimum=.000366;f.maximum=.4;f.temp=34;f.battery=67;f.local_hour=12;return f;}
static Controller controller(){Controller c;c.profile=Profile::parse("IAB_PROFILE_V1 test 3\n0 0\n100 0.1\n1000 1\n");c.learning.add(0,.0097,1);c.learning.add(2.052,.01355,2);c.learning.add(9.3,.08925,3);return c;}
static Output run(Controller& c,Frame& f,int count){Output o;for(int i=0;i<count;i++){f.now+=250;o=c.step(f);if(o.apply)f.current=o.value;}return o;}
int main(){try {
    auto c=controller();auto f=initial();c.step(f);run(c,f,40);
    const auto points=c.learning.points.size();const double before=f.current;
    f.lux=1.825;f.back=89;auto o=run(c,f,240);
    check(c.ambientGuard()&&o.reason=="ambient_guard"&&o.value==before,"one minute of front-only darkness cannot dim");
    check(c.settled(),"stable conflict permits low-frequency polling");
    check(!c.learn(f.lux,.04,10000)&&c.learning.points.size()==points,"shade cannot teach a preference");
    f.lux=0;f.back=114;o=run(c,f,40);
    check(c.ambientGuard()&&o.value==before&&!c.fastDark(),"zero front with bright rear stays guarded");
    f.back=0;run(c,f,4);check(c.ambientGuard()&&!c.settled(),"short rear occlusion is not darkness confirmation");
    f.back=114;o=run(c,f,40);check(c.ambientGuard()&&o.value==before,"rear recovery cancels confirmation");
    c.hold(f.now);f.current=.07;run(c,f,24);
    check(c.ambientGuard()&&std::abs(f.current-.07)<1e-12,"manual adjustment works while learning is paused");
    f.maximum=.03;o=run(c,f,1);check(o.value<=.03,"system cap bypasses guard and slew limit");
    f.lux=9.3;run(c,f,1);check(!c.ambientGuard(),"front recovery releases guard");

    c=controller();f=initial();c.step(f);run(c,f,40);f.lux=0;run(c,f,20);f.back=0;
    run(c,f,9);check(!c.ambientGuard(),"both dark for two seconds releases guard");
    run(c,f,100);check(f.current<.0101,"real darkness eventually follows dark preference");
    f.screen=false;run(c,f,1);f.screen=true;f.lux=0;f.back=100;run(c,f,40);
    check(!c.ambientGuard()&&f.current<.0101,"wake starts a new baseline, not an old guard");

    c=controller();f=initial();f.lux=100;f.back=1000;c.step(f);run(c,f,40);
    // Rear changed first; comparable relative drops are meaningful even when
    // the different sensor orientations retain very different absolute lux.
    f.back=100;run(c,f,4);f.lux=10;run(c,f,9);
    check(!c.ambientGuard(),"matching relative drops confirm real environmental change");
    run(c,f,40);check(!c.ambientGuard()&&c.smoothLux()<11,"confirmed drop does not repeatedly rearm the guard");

    c=controller();f=initial();c.step(f);run(c,f,40);f.lux=0;run(c,f,20);f.back=-1;
    run(c,f,19);check(c.ambientGuard(),"rear loss has a grace period");
    run(c,f,3);check(!c.ambientGuard(),"missing rear cannot freeze brightness forever");
    run(c,f,100);check(f.current<.0101,"front-only fallback still adapts");

    c=controller();f=initial();f.back=0;c.step(f);run(c,f,40);
    double previous=f.current;
    for(int i=0;i<140;i++) {
        f.lux=i<30?(30-i)*.04:0;
        auto out=run(c,f,1);
        check(out.value+1e-12>=previous*std::pow(.8,.25),"moving dim targets obey relative slew bound");
        previous=out.value;
    }
    check(f.current<.0101&&!c.ambientGuard(),"covered rear must not prevent normal front adaptation");
    c=controller();f=initial();f.back=0;c.step(f);run(c,f,40);previous=f.current;
    f.lux=1;f.now+=8000;o=c.step(f);
    check(o.value+1e-12>=previous*std::pow(.8,.25),"idle interval cannot spend a new dim ramp in advance");
    // Replay 18:54: front 8.825 -> 0 for ten seconds, rear unchanged at 19.
    // Include the old gate boundary and smaller positive rear measurements.
    for(double rear: {2.0, 4.0, 19.0, 20.0, 21.0}) {
        c=controller();f=initial();f.lux=8.825;f.back=rear;c.step(f);run(c,f,40);
        const double held=f.current;
        f.lux=0;o=run(c,f,40);
        check(c.ambientGuard()&&o.value==held&&!c.fastDark(),"dim-room front dropout must not dim with stable rear");
        check(!c.learn(0,.01,10000),"dim-room dropout must not create a learning point");
        f.lux=8.825;run(c,f,1);check(!c.ambientGuard(),"dim-room front recovery releases guard");
        f.lux=0;run(c,f,8);f.back=0;
        run(c,f,4);check(c.ambientGuard(),"brief dual darkness needs confirmation");
        run(c,f,6);check(!c.ambientGuard(),"sustained dual darkness releases low-lux guard");
        run(c,f,100);check(f.current<held,"confirmed dim room still adapts downward");
    }
    c=controller();f=initial();f.lux=8.825;f.back=19;c.step(f);run(c,f,40);
    const double held=f.current;f.lux=0;run(c,f,12);
    f.back=18;run(c,f,40);check(c.ambientGuard()&&f.current==held,"small rear variation is not corroboration");
    f.lux=3;f.back=5;run(c,f,10);check(!c.ambientGuard(),"relative dual decline confirms darkness below 20 lux");

    c=controller();f=initial();f.lux=8.825;f.back=19;c.step(f);run(c,f,40);f.lux=0;run(c,f,12);
    f.maximum=.003;o=run(c,f,1);check(o.apply&&o.value<=.003,"low-lux guard cannot override system cap");
    f.blocked=true;o=run(c,f,1);check(!o.apply,"thermal/system protection must release control during low-lux guard");
    f.blocked=false;f.back=-1;run(c,f,19);check(c.ambientGuard(),"low-lux rear loss retains grace period");
    run(c,f,3);check(!c.ambientGuard(),"low-lux rear loss eventually permits front-only control");

    for(double rear: {0.0, .2, 1.0}) {
        c=controller();f=initial();f.back=rear;c.step(f);run(c,f,40);const double start=f.current;
        f.lux=0;run(c,f,100);
        check(!c.ambientGuard()&&f.current<start,"covered/noise-level rear cannot freeze normal dimming");
    }
    std::cout<<"Orientation guard, corroborated darkness, learning exclusion and downward slew passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
