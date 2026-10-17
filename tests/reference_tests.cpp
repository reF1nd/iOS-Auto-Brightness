// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include "reference.hpp"
#include "storage.hpp"
#include <iostream>
#include <stdexcept>
using namespace brightness;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Frame referenceFrame(double lux,double current) {
    Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=lux;f.current=current;
    f.minimum=.000366256;f.maximum=1;f.temp=30;f.battery=80;
    return f;
}
void checkAlpha(double initial,double next,double expected) {
    Controller c;c.reference=true;
    auto f=referenceFrame(initial,referenceBrightness(referenceNits(initial)));
    c.step(f);f.now+=800;f.lux=next;c.step(f);
    check(std::abs(c.alpha()-expected)<1e-9,"reference EMA coefficient matches its range and direction");
}
void checkRise(double before,double after,int64_t elapsed) {
    const double limit=(before+.005)*std::pow(1.6,std::min<int64_t>(100,elapsed)/1000.0)-.005;
    check(after<=limit+1e-12,"ordinary output rise respects the relative rate and bounded frame time");
}
// Input fixture for the bridge's front-led coordinate; the controller receives
// both this coordinate and the unmodified front value through FRAME3.
double weightedLux(double front,double rear) {
    return std::expm1(std::log1p(front)+std::min(.25*std::max(0.0,std::log1p(rear)-std::log1p(front)),std::log(3.0)));
}
int main(){
    check(std::abs(referenceBrightness(referenceNits(10))-.068271013)<.000001,"reference 10 lux curve");
    check(std::abs(referenceBrightness(referenceNits(100))-.2071)<.0001,"reference 100 lux curve");
    check(referenceNits(.5)==1&&referenceNits(1)==2&&referenceNits(2)==3,"dark curve joins the reference endpoints continuously");
    check(std::abs(referenceNits(.5-1e-8)-referenceNits(.5+1e-8))<1e-6,"no jump at the former half-lux discontinuity");
    Learning empty;double darkPrevious=empty.referenceTarget(0);
    for(int i=1;i<=400;i++) {
        const double value=empty.referenceTarget(i*.005);
        check(value>=darkPrevious,"empty learning has a nondecreasing dark curve");darkPrevious=value;
    }
    for(int i=399;i>=0;i--) {
        const double value=empty.referenceTarget(i*.005);
        check(value<=darkPrevious,"returning through the dark curve cannot reverse brightness");darkPrevious=value;
    }
    check(referenceSlider(0,2.2)==0&&referenceSlider(255,2.2)>.9999,"reference slider endpoints");
    check(referenceRamp(.01,true)==0&&referenceRamp(.04,true)==56&&referenceRamp(.2,false)==300,"recovered reference timing remains distinct from the safer applied timing");
    checkAlpha(10,10.4,.06);
    checkAlpha(10,10.50001,.06+(.050001-.05)/.95*(.20-.06));
    checkAlpha(10,15,.06+(.5-.05)/.95*(.20-.06));
    checkAlpha(10,20,.45);
    checkAlpha(.2,.3,.06+(.5-.05)/.95*(.20-.06));
    checkAlpha(10,5,.06+(.7-.06)*.5);
    Controller fine,coarse;fine.reference=true;coarse.reference=true;
    auto timing=referenceFrame(10,referenceBrightness(referenceNits(10)));
    fine.step(timing);coarse.step(timing);timing.lux=10.4;
    for(int i=0;i<8;i++) {
        timing.now+=100;fine.step(timing);
        if(i<7)check(fine.smoothLux()==10,"animation polls cannot advance the environmental filter early");
    }
    coarse.step(timing);
    check(std::abs(fine.smoothLux()-coarse.smoothLux())<1e-9,"100ms driver polling preserves the 800ms slow filter response");
    const double sameTime=fine.smoothLux();fine.step(timing);
    check(fine.smoothLux()==sameTime,"duplicate timestamps do not advance the filter");
    Controller animation;animation.reference=true;
    auto animationFrame=referenceFrame(10,referenceBrightness(referenceNits(10)));
    const auto animationStart=animation.step(animationFrame);
    check(!animation.animating(),"matching the initial live curve needs no animation");
    animationFrame.now+=800;animationFrame.lux=20;
    const auto animationFirst=animation.step(animationFrame);
    const double animationTarget=animation.desired(),animationLux=animation.smoothLux();
    check(animation.animating()&&animationFirst.value==animationStart.value,"an ordinary rise begins at the last output");
    animationFrame.lux=animationLux; // Keep the endpoint fixed at later filter ticks.
    animationFrame.now+=50;const auto animationMiddle=animation.step(animationFrame);
    check(animationMiddle.value>animationFirst.value&&animationMiddle.value<animationTarget,"50ms animation cadence produces an intermediate output");
    check(animation.smoothLux()==animationLux&&animation.desired()==animationTarget,"animation leaves the ambient value and curve endpoint unchanged");
    animationFrame.now+=50;const auto animationEnd=animation.step(animationFrame);
    check(animation.animating()&&animationEnd.value<animationTarget,"ordinary rise cannot finish in the old 56 or 125ms window");
    double animationPrevious=animationEnd.value;
    for(int i=0;i<60;i++) {
        animationFrame.now+=50;const auto out=animation.step(animationFrame);
        checkRise(animationPrevious,out.value,50);animationPrevious=out.value;
    }
    check(!animation.animating()&&std::abs(animationPrevious-animationTarget)<1e-9,"bounded ordinary ramp eventually reaches the unchanged endpoint");
    Controller small;small.reference=true;small.config.hyst_low=small.config.hyst_high=0;
    auto smallFrame=referenceFrame(10,referenceBrightness(referenceNits(10)));
    const double smallStart=small.step(smallFrame).value;
    smallFrame.now+=800;smallFrame.lux=12;auto smallOut=small.step(smallFrame);
    check(small.animating()&&smallOut.value==smallStart&&small.desired()-smallStart<.02,"small ordinary rise no longer takes the zero-duration shortcut");
    smallFrame.now+=100;smallOut=small.step(smallFrame);
    check(smallOut.value>smallStart&&smallOut.value<small.desired(),"small rise has a real intermediate frame");
    checkRise(smallStart,smallOut.value,100);

    Controller wakeRise;wakeRise.reference=true;
    auto wakeFrame=referenceFrame(24.479,865.0/16383);
    wakeFrame.local_hour=0;wakeFrame.battery=18;
    auto wakeOut=wakeRise.step(wakeFrame);const double wakeStart=wakeOut.value;
    check(wakeOut.value==wakeFrame.current&&std::abs(wakeRise.desired()*16383-2142.272372139098)<.001,"recorded wake case starts at raw865 with the unchanged raw2142 curve target");
    double wakePrevious=wakeOut.value;
    for(int i=1;i<=80;i++) {
        wakeFrame.now+=50;wakeOut=wakeRise.step(wakeFrame);
        checkRise(wakePrevious,wakeOut.value,50);
        check(wakeOut.value>=wakePrevious&&wakeOut.reason!="sunlight","dim-room wake rises monotonically without the sunlight shortcut");
        if(i==8)check(wakeRise.animating()&&wakeOut.value<(wakeStart+.005)*std::pow(1.6,.4)-.005+1e-12,"400ms cannot complete a 2.5x dark-room handover");
        wakePrevious=wakeOut.value;
    }
    check(!wakeRise.animating()&&std::abs(wakeOut.value-wakeRise.desired())<1e-9,"recorded wake rise converges without a brightness step");
    wakeFrame.blocked=true;wakeFrame.now+=50;wakeRise.step(wakeFrame);
    wakeFrame.blocked=false;wakeFrame.current=wakeStart;wakeFrame.now+=100000;wakeOut=wakeRise.step(wakeFrame);
    check(wakeOut.value==wakeStart&&wakeOut.reason=="reference_resume","a long pause restarts a 2.5x rise at actual output");
    wakeFrame.now+=5000;wakeOut=wakeRise.step(wakeFrame);checkRise(wakeStart,wakeOut.value,5000);
    check(wakeRise.animating(),"a delayed first resume frame cannot finish the handover");

    Controller moving;moving.reference=true;
    auto movingFrame=referenceFrame(10,.02);movingFrame.now=0;
    double movingPrevious=moving.step(movingFrame).value;
    movingFrame.now=50;auto movingOut=moving.step(movingFrame);
    check(movingOut.value>movingPrevious,"timestamp zero is a valid output-clock baseline");
    checkRise(movingPrevious,movingOut.value,50);movingPrevious=movingOut.value;
    for(int64_t elapsed:{50,5000,20,800,10000,50}) {
        movingFrame.now+=elapsed;movingOut=moving.step(movingFrame);
        checkRise(movingPrevious,movingOut.value,elapsed);movingPrevious=movingOut.value;
    }
    movingFrame.now+=800;movingFrame.lux=1000;movingOut=moving.step(movingFrame);
    check(movingOut.value>movingPrevious&&moving.desired()>movingOut.value,"a same-direction endpoint update preserves progress without a target jump");
    checkRise(movingPrevious,movingOut.value,800);movingPrevious=movingOut.value;
    movingFrame.now+=50;movingOut=moving.step(movingFrame);checkRise(movingPrevious,movingOut.value,50);
    movingPrevious=movingOut.value;
    movingOut=moving.step(movingFrame);check(movingOut.value==movingPrevious,"duplicate output timestamps cannot advance a rise");
    movingFrame.minimum=.2;movingFrame.now+=50;movingOut=moving.step(movingFrame);
    check(movingOut.value>=.2,"a new mandatory display minimum bypasses ordinary rise pacing");

    Controller fineRise,coarseRise;fineRise.reference=coarseRise.reference=true;
    auto rateFrame=referenceFrame(10,.02);rateFrame.now=0;
    fineRise.step(rateFrame);coarseRise.step(rateFrame);
    for(int i=1;i<=16;i++){rateFrame.now=i*50;fineRise.step(rateFrame);}
    const auto coarseOut=coarseRise.step(rateFrame);
    checkRise(.02,coarseOut.value,800);
    check(coarseOut.value<fineRise.step(rateFrame).value&&coarseRise.animating(),"coarse polling slows animation instead of catching up in one large step");
    Controller evolving;evolving.reference=true;
    auto evolvingFrame=referenceFrame(10,referenceBrightness(referenceNits(10)));
    const double evolvingStart=evolving.step(evolvingFrame).value;
    double evolvingPrevious=evolvingStart;evolvingFrame.lux=20;
    for(int i=0;i<80;i++) {
        evolvingFrame.now+=800;const auto out=evolving.step(evolvingFrame);
        checkRise(evolvingPrevious,out.value,800);evolvingPrevious=out.value;
    }
    check(evolvingPrevious>evolvingStart+.01&&std::abs(evolvingPrevious-evolving.desired())<.005,"filter updates at every coarse poll cannot repeatedly reset the ramp and stall convergence");
    Controller black;black.reference=true;
    auto blackFrame=referenceFrame(0,.02);double blackPrevious=black.step(blackFrame).value;
    for(int i=0;i<20;i++) {
        blackFrame.now+=100;const auto out=black.step(blackFrame);
        check(out.value>=blackFrame.minimum&&out.value<=blackPrevious,"100ms pure-black frames dim without crossing the display minimum");
        blackPrevious=out.value;
    }
    check(blackPrevious==blackFrame.minimum,"pure-black baseline settles exactly at the active minimum");
    black.sliderBaseline(0);check(black.slider(64,10000),"pure-black user adjustment produces a preference sample");
    check(black.learning.points[0].value==referenceSlider(64,black.config.manual_gamma),"slider preferences keep the normalized reference-backlight unit");
    for(int i=0;i<40;i++) {
        blackFrame.now+=100;const auto out=black.step(blackFrame);
        checkRise(blackPrevious,out.value,100);blackPrevious=out.value;
    }
    check(blackPrevious>blackFrame.minimum,"pure-black learned preference rises gradually above the minimum");
    Controller syncedController;syncedController.reference=true;
    auto syncFrame=referenceFrame(11.720,2138.0/16383);syncFrame.local_hour=0;syncFrame.battery=18;
    const auto staleOutput=syncedController.step(syncFrame);
    const double syncAmbient=syncedController.smoothLux(),syncTarget=syncedController.desired();
    check(staleOutput.value==syncFrame.current&&std::abs(syncTarget*16383-1304)<1,"recorded conflict has cached raw2138 and a curve target near1304");
    syncFrame.current=499.0/16383;syncedController.syncOutput(syncFrame.now,syncFrame.current);
    check(syncedController.smoothLux()==syncAmbient&&syncedController.desired()==syncTarget&&syncedController.learning.points.empty(),"output synchronization preserves ambient, curve target and observations");
    auto synced=syncedController.step(syncFrame);
    check(synced.apply&&synced.value==syncFrame.current&&syncedController.animating(),"the first synchronized frame stays at actual raw499 instead of jumping to1304");
    double syncPrevious=synced.value;
    for(int i=0;i<80;i++) {
        syncFrame.now+=50;synced=syncedController.step(syncFrame);
        checkRise(syncPrevious,synced.value,50);syncPrevious=synced.value;
    }
    check(std::abs(synced.value-syncTarget)<1e-9,"continuous control converges from synchronized output to the unchanged curve");
    for(int raw:{510,520,490}) {
        syncFrame.now+=50;syncFrame.current=raw/16383.0;
        syncedController.syncOutput(syncFrame.now,syncFrame.current);synced=syncedController.step(syncFrame);
        check(synced.value==syncFrame.current&&syncedController.learning.points.empty()&&syncedController.smoothLux()==syncAmbient,"repeated system output changes only rebase the output ramp");
    }
    syncedController.pauseOutput();syncFrame.now+=50;syncFrame.current=.02;
    syncedController.syncOutput(syncFrame.now,syncFrame.current);synced=syncedController.step(syncFrame);
    check(synced.reason=="reference_resume"&&synced.value==syncFrame.current,"synchronization preserves an existing paused-resume handover");
    Controller failedWrite;failedWrite.reference=true;
    auto raceFrame=referenceFrame(10,.8);
    failedWrite.step(raceFrame);raceFrame.now+=50;const auto unwritten=failedWrite.step(raceFrame);
    check(unwritten.value<.8&&failedWrite.animating(),"race setup advances a proposed but unwritten animation value");
    const double raceAmbient=failedWrite.smoothLux();
    failedWrite.pauseOutput();check(!failedWrite.animating(),"failed writer pauses animation without a user hold");
    raceFrame.now+=50;raceFrame.current=.8;const auto raceResume=failedWrite.step(raceFrame);
    check(raceResume.apply&&raceResume.value==raceFrame.current&&raceResume.reason=="reference_resume","write-race recovery starts from the last actual hardware output");
    check(failedWrite.smoothLux()==raceAmbient&&failedWrite.animating(),"write-race recovery preserves ambient state and resumes a ramp");
    Controller spike;spike.reference=true;
    auto spikeFrame=referenceFrame(10,referenceBrightness(referenceNits(10)));
    spike.step(spikeFrame);spikeFrame.now+=800;spikeFrame.lux=10000;spike.step(spikeFrame);
    check(std::abs(spike.smoothLux()-73)<1e-9,"reference spike is clamped before the EMA update");
    Learning l;const double base=referenceBrightness(referenceNits(10));
    l.add(10,base+.1,1000,false);
    double one=l.referenceTarget(10);
    check(one>base&&one<base+.09,"one sample shifts curve without pinning output");
    l.add(100,.4,2000,false);
    check(l.referenceTarget(10)>one,"second sample contributes to combined curve");
    l.add(1000,.05,3000,false); // Contradictory observations are not rewritten.
    check(l.points[1].value==.4&&l.points[2].value==.05,"retain observations, constrain fitted curve instead");
    double previous=-1;
    for(int i=0;i<=500;i++){
        double lux=std::expm1(i*std::log1p(200000.0)/500),value=l.referenceTarget(lux);
        check(value>=previous-1e-12&&value>=0&&value<=1,"fitted curve stays monotonic and bounded");previous=value;
    }
    Store store{"unused","test-v68curve2",0};
    auto text=store.encode(l);auto restored=store.decode(text);
    check(text.rfind("IAB_CURVE_OBSERVATIONS_V2",0)==0&&restored.points.size()==3,"versioned observation storage");
    check(restored.referenceTarget(10)==l.referenceTarget(10),"restart reconstructs identical preference curve");
    Learning dimPreference;dimPreference.add(70.51677506,.0006866859953,1000,false);
    check(dimPreference.referenceTarget(6.488)>.01,"dark preference must not collapse adjacent lux to zero");
    Learning blackPreference;blackPreference.add(0,.01,1000,false);
    check(blackPreference.referenceTarget(0)>.001,"pure black can still learn a brighter preference");
    Controller rear;rear.reference=true;rear.profile={"test",{1,3500},{.001,1}};
    Frame r;r.now=1000;r.automatic=true;r.screen=true;r.front=6.488;r.lux=6.488;r.back=0;r.temp=30;r.battery=80;r.minimum=.000366256;r.maximum=1;
    r.current=rear.target(r.lux);
    const double held=rear.step(r).value;
    for(int i=1;i<=300;i++) {
        r.now+=100;r.back=(i/20)%2?25:0;r.lux=weightedLux(r.front,r.back);
        auto out=rear.step(r);check(out.apply&&std::abs(out.value-held)<.00001,"stationary front and pulsing rear cannot flash output");
    }
    r.back=25;r.lux=weightedLux(r.front,r.back);
    for(int i=0;i<49;i++){r.now+=100;rear.step(r);}
    check(std::abs(rear.smoothLux()-r.front)<1e-9&&rear.ambientGuard(),"large rear-only change waits for five continuous seconds");
    for(int i=0;i<600;i++){r.now+=100;rear.step(r);}
    check(rear.smoothLux()>r.front&&rear.smoothLux()<r.back&&std::abs(rear.smoothLux()-r.lux)<.1,"a stable rear lamp contributes the bounded weighted coordinate, not max lux");
    check(rear.desired()>held&&rear.desired()<rear.target(r.back),"sustained rear light raises output by less than full rear control");
    const double lampAmbient=rear.smoothLux();
    r.now+=800;r.front=.1;r.back=25;r.lux=weightedLux(r.front,r.back);rear.step(r);
    check(!rear.ambientGuard()&&rear.smoothLux()<lampAmbient,"real front decrease bypasses rear-only confirmation without the old permanent dark hold");
    const double beforeDark=rear.smoothLux();
    r.now+=800;r.front=.01;r.back=.01;r.lux=weightedLux(r.front,r.back);rear.step(r);
    check(!rear.ambientGuard()&&rear.smoothLux()<beforeDark,"both sides becoming dark resumes ordinary dimming");
    // SOURCE resets the controller when the bridge falls back to another sensor.
    rear.reset();r.now+=100;r.front=-1;r.back=25;r.lux=r.back;r.current=.04;
    const auto fallbackStart=rear.step(r);
    check(fallbackStart.value==r.current&&rear.smoothLux()==25,"selected rear fallback seeds its own coordinate and begins at live output");
    Controller c;c.reference=true;c.profile={"test",{1,3500},{.001,1}};
    Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=10;f.current=.8;f.minimum=.001;f.maximum=1;f.temp=30;f.battery=80;
    auto o=c.step(f);check(o.apply&&o.value==f.current&&o.reason=="reference_start","wake starts from the supplied live output");
    f.now+=100;o=c.step(f);
    check(o.value<.8&&o.value>c.desired(),"wake moves toward the curve without an immediate jump");
    check(!c.slider(100,10000),"initial slider snapshot cannot teach");
    for(int value=101;value<=119;value++)check(!c.slider(value,10000+value),"small adjacent changes do not accumulate into a preference");
    c.sliderBaseline(100);
    check(c.learning.points.empty(),"baseline updates cannot teach a preference");
    check(c.slider(120,11000)&&c.learning.points.size()==1,"slider change adds a preference observation");
    const double beforePause=o.value,ambientBeforePause=c.smoothLux();
    f.now+=100;f.blocked=true;o=c.step(f);
    check(!o.apply&&o.value==beforePause,"temporary output gate preserves the last output without writing");
    check(!c.slider(160,12000)&&c.learning.points.size()==1,"system protection prevents learning");
    f.blocked=false;f.now+=100000;f.current=.03;f.lux=10.4;o=c.step(f);
    check(c.smoothLux()==ambientBeforePause,"paused wall time does not consume ambient filter progress");
    check(o.apply&&o.value==f.current&&o.reason=="reference_resume","resume begins at the live node instead of the previous curve output");
    f.now+=100;o=c.step(f);
    check(o.value>.03&&o.value<c.desired(),"resume makes a continuous intermediate step");
    f.blocked=false;f.screen=false;f.now+=100;check(!c.step(f).apply,"no writes while screen off");
    f.screen=true;f.now+=100;f.current=.9;o=c.step(f);check(o.apply&&o.value==f.current,"new wake keeps its first live frame");
    f.maximum=.01;f.now+=100;check(c.step(f).value<=.01,"live system cap retained");
    std::cout<<"Reference curve, slider conversion, wake, multi-sample fitting and persistence passed\n";
}
