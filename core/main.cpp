// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include "storage.hpp"
#include "panel_read.hpp"
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <chrono>
#include <unistd.h>
using namespace brightness;
int main(int argc,char**argv) {
    if(argc==2&&std::string(argv[1])=="--panel-read"){std::cout<<readMainPanel()<<std::endl;return 0;}
    if(argc==4 && std::string(argv[1])=="--config") {
        try {
            const auto text=std::string(argv[3]);
            if(std::string(argv[2])=="check") std::cout<<Config::parse(readFile(text)).serialize();
            else if(std::string(argv[2])=="save") {
                std::ostringstream input;input<<std::cin.rdbuf();
                if(input.str().size()>32768)throw std::runtime_error("config too large");
                atomicWrite(text,Config::parse(input.str()).serialize());
            } else return 2;
            return 0;
        }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
    }
    if(argc!=3){std::cerr<<"usage: ios_brightness_daemon STATE_DIRECTORY PROFILE\n";return 2;}
    umask(0077);
    try {
        const std::string dir=argv[1];struct stat st{};
        if(lstat(dir.c_str(),&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0022))throw std::runtime_error("state directory must be owned and private");
        const int lock=open((dir+"/core.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(lock<0||flock(lock,LOCK_EX|LOCK_NB))throw std::runtime_error("another core owns this state directory");
        Controller core;core.profile=Profile::parse(readFile(argv[2]));
        const std::string conf=dir+"/config.conf";std::string configStatus="default",saveStatus="waiting_for_user";
        if(access(conf.c_str(),F_OK)==0) {try{core.config=Config::parse(readFile(conf));configStatus="loaded";}catch(const std::exception&e){core.config.enabled=false;configStatus=std::string("invalid: ")+e.what();}}
        else {
            try {
                core.config=Config::parse(readFile(dir+"/legacy-config.conf"));
                configStatus="migrated_legacy";
            }catch(const std::exception& e) {configStatus=access((dir+"/legacy-config.conf").c_str(),F_OK)==0?std::string("legacy_rejected: ")+e.what():"default";}
            atomicWrite(conf,core.config.serialize());
        }
        Store store{dir,core.profile.id,0};bool hasUser=false;Output last;
        std::cout<<std::setprecision(10);
        std::string line;
        rusage initialUsage{};getrusage(RUSAGE_SELF,&initialUsage);
        const double initialCpu=initialUsage.ru_utime.tv_sec+initialUsage.ru_utime.tv_usec/1e6+initialUsage.ru_stime.tv_sec+initialUsage.ru_stime.tv_usec/1e6;
        const auto started=std::chrono::steady_clock::now();
        auto status=[&]() {
            rusage usage{};getrusage(RUSAGE_SELF,&usage);
            const double cpu=usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
            const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
            std::ostringstream o;o<<std::setprecision(10)<<"{\"ok\":true,\"apply\":"<<(last.apply?"true":"false")<<",\"target\":"<<last.value<<",\"reason\":"<<jsonString(last.reason)<<",\"lux\":"<<core.smoothLux()<<",\"user\":"<<store.user<<",\"profile\":"<<jsonString(store.profile)<<",\"storage\":"<<jsonString(saveStatus)<<",\"config_status\":"<<jsonString(configStatus)<<",\"settled\":"<<(core.settled()?"true":"false")<<",\"ambient_guard\":"<<(core.ambientGuard()?"true":"false")<<",\"fast_dark\":"<<(core.fastDark()?"true":"false")<<",\"alpha\":"<<core.alpha()<<",\"desired\":"<<core.desired()<<",\"night_scale\":"<<core.nightScale()<<",\"points\":[";
            bool comma=false;for(auto&p:core.learning.points){if(comma)o<<',';comma=true;o<<"{\"lux\":"<<p.lux<<",\"brightness\":"<<p.value<<",\"time\":"<<p.time<<'}';}
            o<<"],\"legacy_offset\":"<<core.learning.legacy_offset<<",\"local_hour\":"<<core.frame().local_hour<<",\"curve\":[";
            for(int i=0;i<=80;i++){double lux=std::expm1(i*std::log(100001.0)/80);if(i)o<<',';o<<'['<<lux<<','<<core.base(lux)<<','<<core.target(lux)<<']';}
            o<<"],\"enabled\":"<<(core.config.enabled?"true":"false")<<",\"learning_enabled\":"<<(core.config.learning_enabled?"true":"false")<<",\"settings_learning\":"<<(core.config.settings_learning?"true":"false")<<",\"core_pid\":"<<getpid()<<",\"cpu_core_percent\":"<<(elapsed>0?100*(cpu-initialCpu)/elapsed:0)<<"}";return o.str();
        };
        while(std::getline(std::cin,line)) {
            try {
                if(line.size()>65536)throw std::runtime_error("command too large");
                std::istringstream in(line);std::string cmd;in>>cmd;
                auto end=[&](){std::string extra;if(in>>extra)throw std::runtime_error("trailing command arguments");};
                if(cmd=="PANEL_READ") {end();std::cout<<readMainPanel()<<std::endl;continue;}
                if(cmd=="POLICY") {
                    std::string policy;in>>policy;end();
                    if(policy!="v68"||hasUser||core.reference)throw std::runtime_error("policy must precede user and be set once");
                    core.reference=true;core.profile.id+="-v68curve2";store.profile=core.profile.id;core.reset();
                } else if(cmd=="SLIDER") {
                    int value;int64_t wall;if(!(in>>value>>wall)||!hasUser)throw std::runtime_error("invalid slider");end();
                    auto previous=core;
                    if(core.slider(value,wall)) {
                        try{store.save(core.learning);saveStatus="saved_reference_curve";}
                        catch(...){core=previous;throw;}
                    }
                } else if(cmd=="USER") {
                    int64_t user=-1;if(!(in>>user)||user<0)throw std::runtime_error("invalid user");end();
                    if(!hasUser||store.user!=user){store.user=user;hasUser=true;core.reset();core.learning=store.load(saveStatus);last={false,0,"user_changed"};}
                } else if(cmd=="SOURCE") {
                    std::string source;if(!(in>>source)||!hasUser||source.empty()||source.size()>40||source.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789")!=std::string::npos)throw std::runtime_error("invalid source");end();
                    auto identity=core.profile.id+(source=="default"?"":"-"+source);
                    if(store.profile!=identity){store.profile=identity;core.reset();core.learning=store.load(saveStatus);last={false,0,"source_changed"};}
                } else if(cmd=="FRAME"||cmd=="FRAME2"||cmd=="FRAME3") {
                    Frame f;if(!(in>>f.now>>f.automatic>>f.screen>>f.lux>>f.back>>f.near>>f.temp>>f.battery>>f.charging>>f.current>>f.minimum>>f.maximum>>f.blocked))throw std::runtime_error("invalid frame");
                    if(cmd!="FRAME"&&(!(in>>f.local_hour)||f.local_hour<0||f.local_hour>23))throw std::runtime_error("invalid local hour");
                    if(cmd=="FRAME3"&&(!(in>>f.front)||!std::isfinite(f.front)||f.front < -1||f.front>200000))throw std::runtime_error("invalid raw front");
                    end();
                    if(!hasUser)throw std::runtime_error("user required");
                    last=core.step(f);
                } else if(cmd=="HOLD") {int64_t now;if(!(in>>now))throw std::runtime_error("missing time");end();core.hold(now);last={false,0,"user_adjustment"};
                } else if(cmd=="EVENT") {
                    int64_t time;double lux,value;if(!(in>>time>>lux>>value)||!hasUser)throw std::runtime_error("invalid event");end();
                    auto previous=core.learning;
                    if(core.learn(lux,value,time)) {
                        try{store.save(core.learning);saveStatus="saved";}catch(const std::exception&e){core.learning=previous;saveStatus=std::string("save_failed: ")+e.what();throw;}
                    }else {std::cout<<"{\"ok\":true,\"accepted\":false}"<<std::endl;continue;}
                } else if(cmd=="CLEAR") {end();
                    if(!hasUser)throw std::runtime_error("user required");
                    store.save({},true);core.learning={};saveStatus="cleared";
                } else if(cmd=="CONFIG") {
                    std::string path;if(!(in>>std::quoted(path)))throw std::runtime_error("missing config file");end();
                    auto proposed=Config::parse(readFile(path));atomicWrite(conf,proposed.serialize());core.config=proposed;core.reset();configStatus="applied";
                } else if(cmd=="RELOAD") {end();
                    try {
                        auto proposed=Config::parse(readFile(conf));
                        if(proposed.serialize()!=core.config.serialize()){core.config=proposed;core.reset();}
                        configStatus="applied";
                    }catch(const std::exception& e){configStatus=std::string("invalid: ")+e.what();throw;}
                } else if(cmd=="CONFIG_GET") {end();std::cout<<"{\"ok\":true,\"data\":"<<jsonString(core.config.serialize())<<'}'<<std::endl;continue;
                } else if(cmd=="EXPORT") {end();if(!hasUser)throw std::runtime_error("user required");std::cout<<"{\"ok\":true,\"data\":"<<jsonString(store.encode(core.learning))<<'}'<<std::endl;continue;
                } else if(cmd=="IMPORT") {
                    std::string path;if(!(in>>std::quoted(path))||!hasUser)throw std::runtime_error("missing import file");end();auto l=store.decode(readFile(path));store.save(l,true);core.learning=l;saveStatus="imported";core.reset();
                } else if(cmd=="MIGRATE") {
                    std::string path;if(!(in>>std::quoted(path))||!hasUser)throw std::runtime_error("missing migration file");end();
                    if(saveStatus!="new")throw std::runtime_error("migration requires new state");
                    auto text=readFile(path,256);std::istringstream old(text);std::string record;old>>record;
                    if(record.rfind("user_offset=",0)!=0)throw std::runtime_error("invalid legacy format");
                    size_t used=0;double v=std::stod(record.substr(12),&used);
                    if(used!=record.size()-12||!std::isfinite(v)||std::abs(v)>.3)throw std::runtime_error("invalid legacy offset");
                    Learning l;l.legacy_offset=v;store.save(l);core.learning=l;saveStatus="migrated_offset";
                } else if(cmd!="STATUS") throw std::runtime_error("unknown command");
                end();
                std::cout<<status()<<std::endl;
            }catch(const std::exception& e){std::cout<<"{\"ok\":false,\"error\":"<<jsonString(e.what())<<'}'<<std::endl;}
        }
        close(lock);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
