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
    check(referenceRamp(.01,true)==0&&referenceRamp(.04,true)==56&&referenceRamp(.2,false)==300,"reference transition timing");
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
    check(animation.animating()&&animationFirst.value==animationStart.value,"a new short ramp begins at the last output");
    animationFrame.now+=50;const auto animationMiddle=animation.step(animationFrame);
    check(animationMiddle.value>animationFirst.value&&animationMiddle.value<animationTarget,"50ms animation cadence produces an intermediate short-ramp output");
    check(animation.smoothLux()==animationLux&&animation.desired()==animationTarget,"animation leaves the ambient value and curve endpoint unchanged");
    animationFrame.now+=50;const auto animationEnd=animation.step(animationFrame);
    check(!animation.animating()&&std::abs(animationEnd.value-animationTarget)<1e-9,"short ramp reaches its endpoint without another environment update");
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
    Controller rear;rear.reference=true;rear.learning=dimPreference;rear.profile={"test",{1,3500},{.001,1}};
    Frame r;r.now=1000;r.automatic=true;r.screen=true;r.front=6.488;r.lux=6.488;r.back=0;r.temp=30;r.battery=80;r.minimum=.000366256;r.maximum=1;
    r.current=dimPreference.referenceTarget(r.lux);
    const double held=rear.step(r).value;
    for(int i=1;i<=300;i++) {
        r.now+=100;r.back=(i/20)%2?25:0;r.lux=std::max(r.front,r.back);
        auto out=rear.step(r);check(out.apply&&std::abs(out.value-held)<.00001,"stationary front and pulsing rear cannot flash output");
    }
    for(int i=0;i<600;i++){r.now+=100;r.back=25;r.lux=25;rear.step(r);}
    check(rear.smoothLux()>24,"stable rear still participates after confirmation");
    r.now+=100;r.front=.1;r.back=.1;r.lux=.1;rear.step(r);
    check(!rear.ambientGuard(),"real front change is not delayed by rear-only confirmation");
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
