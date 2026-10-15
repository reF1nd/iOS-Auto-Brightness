// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include "reference.hpp"
#include "storage.hpp"
#include <iostream>
#include <stdexcept>
using namespace brightness;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    check(std::abs(referenceBrightness(referenceNits(10))-.068271013)<.000001,"reference 10 lux curve");
    check(std::abs(referenceBrightness(referenceNits(100))-.2071)<.0001,"reference 100 lux curve");
    check(referenceNits(.5)==1&&referenceNits(1)==1&&referenceNits(2)==3,"reference low-lux branches retained");
    check(referenceSlider(0,2.2)==0&&referenceSlider(255,2.2)>.9999,"reference slider endpoints");
    check(referenceRamp(.01,true)==0&&referenceRamp(.04,true)==56&&referenceRamp(.2,false)==300,"reference transition timing");
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
    const double held=rear.step(r).value;
    for(int i=1;i<=300;i++) {
        r.now+=100;r.back=(i/20)%2?25:0;r.lux=std::max(r.front,r.back);
        auto out=rear.step(r);check(out.apply&&std::abs(out.value-held)<.00001,"stationary front and pulsing rear cannot flash output");
    }
    for(int i=0;i<100;i++){r.now+=100;r.back=25;r.lux=25;rear.step(r);}
    check(rear.smoothLux()>24,"stable rear still participates after confirmation");
    r.now+=100;r.front=.1;r.back=.1;r.lux=.1;rear.step(r);
    check(!rear.ambientGuard(),"real front change is not delayed by rear-only confirmation");
    Controller c;c.reference=true;c.profile={"test",{1,3500},{.001,1}};
    Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=10;f.current=.8;f.minimum=.001;f.maximum=1;f.temp=30;f.battery=80;
    auto o=c.step(f);check(o.apply&&o.value<.08&&o.reason=="reference_warm_start","wake uses current curve, not stale bright framework");
    check(!c.slider(100,10000),"initial slider snapshot cannot teach");
    check(c.slider(120,11000)&&c.learning.points.size()==1,"slider change adds a preference observation");
    f.now+=100;f.blocked=true;c.step(f);
    check(!c.slider(160,12000)&&c.learning.points.size()==1,"system protection prevents learning");
    f.blocked=false;f.screen=false;f.now+=100;check(!c.step(f).apply,"no writes while screen off");
    f.screen=true;f.now+=100;f.current=.9;o=c.step(f);check(o.apply&&o.value<.2,"wake does not replay framework brightness");
    f.maximum=.01;f.now+=100;check(c.step(f).value<=.01,"live system cap retained");
    std::cout<<"Reference curve, slider conversion, wake, multi-sample fitting and persistence passed\n";
}
