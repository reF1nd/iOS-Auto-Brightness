// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include "storage.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>
using namespace brightness;
static int count=0;
static void check(bool value,const char* message){++count;if(!value)throw std::runtime_error(message);}
template<class F> static void rejects(F fn){bool threw=false;try{fn();}catch(...){threw=true;}check(threw,"expected rejection");}
int main(){
    try {
        Profile p=Profile::parse("IAB_PROFILE_V1 test 3\n1 0.001\n100 0.1\n1000 1\n");
        double previous=-1;
        for(int i=0;i<=100000;i++){
            double lux=std::expm1(i*std::log(100001.0)/100000),n=baseNits(lux);
            check(std::isfinite(n)&&n>=previous,"base curve must be monotonic");previous=n;
        }
        check(std::abs(baseNits(.5)-baseNits(.500001))<.00001,"0.5 lux continuity");
        check(std::abs(baseNits(2)-baseNits(2.000001))<.00001,"2 lux continuity");
        rejects([]{baseNits(std::numeric_limits<double>::quiet_NaN());});
        rejects([]{Config::parse("alpha_fast=nan");});
        rejects([]{Config::parse("alpha_mid=0.6\nalpha_fast=.2");});
        rejects([]{Config::parse("enabled=2");});
        rejects([]{Config::parse("alpha_mid=.3\nalpha_mid=.4");});
        rejects([]{Config::parse("log_max_backups=1.5");});
        rejects([]{Config::parse("unrecognized=1");});
        check(Config::parse(Config{}.serialize()).high_lux_duration_ms==8000,"config roundtrip");
        rejects([]{Profile::parse("IAB_PROFILE_V1 bad 2\n1 .5\n2 .4");});
        Learning l;l.add(10,.05,1);l.add(100,.2,2);l.add(1000,.5,3);
        check(std::abs(l.target(100,p)-.2)<1e-9,"exact preference must be remembered");
        l.add(100,.01,4);previous=-1;
        for(int i=0;i<10000;i++){
            double lux=std::expm1(i*std::log(100001.0)/9999),v=l.target(lux,p);
            check(v>=previous-1e-12&&v>=0&&v<=1,"learned curve must remain monotonic");previous=v;
        }
        check(l.points.front().value<=.01,"new explicit preference constrains neighbours");
        for(int i=0;i<100;i++)l.add(std::expm1(i*.12),.5,i+10);
        check(l.points.size()<=32,"sample count bounded");
        Controller c;c.profile=p;
        Frame f;f.now=1000;f.automatic=true;f.screen=true;f.lux=100;f.current=.1;f.minimum=.001;f.maximum=.4;f.battery=80;f.temp=35;
        check(c.step(f).reason=="wake_settling","wake holds control while display and sensor settle");
        f.now+=2000;
        check(c.step(f).apply,"valid auto frame controls display");
        f.back=0;f.now+=250;c.step(f);
        check(c.smoothLux()==100,"covered rear sensor must not darken a stable front sensor");
        f.lux=2000;f.now+=250;c.step(f);
        check(c.smoothLux()==100,"rear disagreement confirms a front spike before applying");
        f.now+=1600;c.step(f);
        check(c.smoothLux()>100,"persistent front light must eventually override a covered rear sensor");
        c.reset();f.lux=100;c.step(f);
        check(c.learn(100,.15,10000),"explicit valid adjustment accepted");
        check(!c.step(f).apply,"user adjustment hold");f.now+=6000;
        check(c.step(f).apply,"control resumes after user hold");
        f.automatic=false;check(!c.step(f).apply,"manual mode releases control");check(!c.learn(100,.2,10001),"manual mode never learns");
        f.automatic=true;f.screen=false;check(!c.step(f).apply,"screen off releases control");
        f.screen=true;f.blocked=true;check(!c.step(f).apply,"window override respected");
        f.blocked=false;f.near=true;check(!c.step(f).apply,"proximity releases control");
        f.near=false;f.lux=-1;check(!c.step(f).apply,"invalid sensor releases control");
        c.reset();f.lux=50000;f.temp=-1;f.now+=1000;c.step(f);f.now+=2000;auto noTemp=c.step(f);
        check(noTemp.reason=="auto_boost_unavailable"&&noTemp.value<=f.maximum,"unknown temperature blocks boost");
        f.temp=46;f.now+=1000;check(c.step(f).reason=="auto_boost_unavailable","hot blocks boost");
        f.temp=35;f.battery=20;f.now+=1000;check(c.step(f).reason=="auto_boost_unavailable","low battery blocks boost");
        f.battery=80;f.now+=1000;check(c.step(f).reason!="sunlight","strong light needs repeated confirmation");
        f.now+=250;check(c.step(f).reason=="sunlight","confirmed safe strong light permits boost");
        f.maximum=.05;f.now+=1000;check(c.step(f).value<=.05,"system cap takes effect immediately");
        {
            Controller dark;dark.profile=p;
            Frame night=f;night.lux=0;night.current=.009;night.now=1000;
            dark.step(night);
            check(dark.learn(0,.009,20000),"zero-lux manual preference is learnable");
            night.now+=6000;
            check(std::abs(dark.step(night).value-.009)<1e-12,"dark preference controls the display");
            night.screen=false;night.current=.001;night.now+=1000;
            check(!dark.step(night).apply,"sleep releases even with a learned preference");
            night.screen=true;night.now+=30000;
            check(!dark.step(night).apply,"doze brightness must not seed an immediate wake ramp");
            night.now+=250;night.current=.009;
            check(!dark.step(night).apply,"wake grace remains while system restores user brightness");
            night.now+=2000;
            check(std::abs(dark.step(night).value-.009)<1e-12,"wake preserves the exact zero-lux preference");
            check(dark.learning.points.size()==1,"sleep and wake never invent or delete preferences");
        }
        char tmp[]="/tmp/iab-tests-XXXXXX";char* d=mkdtemp(tmp);check(d,"temporary directory");
        std::string dir=d;Store store{dir,"test",0};std::string status;
        check(store.load(status).points.empty()&&status=="new","first boot");
        Learning saved;saved.add(100,.2,1234);store.save(saved);auto bytes=readFile(store.path());
        Store restarted{dir,"test",0};auto restored=restarted.load(status);
        check(status=="loaded"&&restored.points.size()==1&&restored.target(100,p)==.2,"process restart preserves mapping");
        check(store.encode(restored)==bytes,"serialization roundtrip");
        Store other{dir,"test",10};rejects([&]{other.decode(bytes);});
        Store wrongPanel{dir,"other",0};rejects([&]{wrongPanel.decode(bytes);});
        auto corrupt=bytes;corrupt[20]^=1;rejects([&]{store.decode(corrupt);});
        atomicWrite(store.path(),"interrupted write");
        check(store.load(status).points.size()==1&&status=="recovered_backup","corrupt primary recovers backup");
        saved.add(1000,.4,2345);store.save(saved);
        check(store.load(status).points.size()==2,"save after recovery repairs primary");
        store.save({},true);atomicWrite(store.path(),"corrupt");
        check(store.load(status).points.empty(),"clear does not resurrect old backup");
        Store missing{dir+"/missing","test",0};rejects([&]{missing.save(saved);});
        auto link=dir+"/symlink";symlink(store.path().c_str(),link.c_str());rejects([&]{readFile(link);});
        rejects([&]{readFile(store.path(),1);});
        std::filesystem::remove_all(dir);
        std::cout<<count<<" contract checks passed\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
