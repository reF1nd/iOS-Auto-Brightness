// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include "storage.hpp"
#include <cmath>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace brightness;
int main(int argc,char**argv) {
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
        else atomicWrite(conf,core.config.serialize());
        Store store{dir,core.profile.id,0};bool hasUser=false;Output last;
        std::cout<<std::setprecision(10);
        std::string line;
        auto status=[&]() {
            std::ostringstream o;o<<std::setprecision(10)<<"{\"ok\":true,\"apply\":"<<(last.apply?"true":"false")<<",\"target\":"<<last.value<<",\"reason\":"<<jsonString(last.reason)<<",\"lux\":"<<core.smoothLux()<<",\"user\":"<<store.user<<",\"profile\":"<<jsonString(store.profile)<<",\"storage\":"<<jsonString(saveStatus)<<",\"config_status\":"<<jsonString(configStatus)<<",\"points\":[";
            bool comma=false;for(auto&p:core.learning.points){if(comma)o<<',';comma=true;o<<"{\"lux\":"<<p.lux<<",\"brightness\":"<<p.value<<",\"time\":"<<p.time<<'}';}
            o<<"],\"curve\":[";
            for(int i=0;i<=80;i++){double lux=std::expm1(i*std::log(100001.0)/80);if(i)o<<',';o<<'['<<lux<<','<<core.profile.toBrightness(baseNits(lux))<<','<<core.learning.target(lux,core.profile)<<']';}
            o<<"]}";return o.str();
        };
        while(std::getline(std::cin,line)) {
            try {
                if(line.size()>65536)throw std::runtime_error("command too large");
                std::istringstream in(line);std::string cmd;in>>cmd;
                if(cmd=="USER") {
                    int64_t user=-1;if(!(in>>user)||user<0)throw std::runtime_error("invalid user");
                    if(!hasUser||store.user!=user){store.user=user;hasUser=true;core.reset();core.learning=store.load(saveStatus);last={false,0,"user_changed"};}
                } else if(cmd=="FRAME") {
                    Frame f;if(!(in>>f.now>>f.automatic>>f.screen>>f.lux>>f.back>>f.near>>f.temp>>f.battery>>f.charging>>f.current>>f.minimum>>f.maximum>>f.blocked))throw std::runtime_error("invalid frame");
                    if(!hasUser)throw std::runtime_error("user required");
                    last=core.step(f);
                } else if(cmd=="HOLD") {int64_t now;if(!(in>>now))throw std::runtime_error("missing time");core.hold(now);last={false,0,"user_adjustment"};
                } else if(cmd=="EVENT") {
                    int64_t time;double lux,value;if(!(in>>time>>lux>>value)||!hasUser)throw std::runtime_error("invalid event");
                    auto previous=core.learning;
                    if(core.learn(lux,value,time)) {
                        try{store.save(core.learning);saveStatus="saved";}catch(const std::exception&e){core.learning=previous;saveStatus=std::string("save_failed: ")+e.what();throw;}
                    }else {std::cout<<"{\"ok\":true,\"accepted\":false}"<<std::endl;continue;}
                } else if(cmd=="CLEAR") {
                    if(!hasUser)throw std::runtime_error("user required");
                    store.save({},true);core.learning={};saveStatus="cleared";
                } else if(cmd=="CONFIG") {
                    std::string path;if(!(in>>std::quoted(path)))throw std::runtime_error("missing config file");
                    auto proposed=Config::parse(readFile(path));atomicWrite(conf,proposed.serialize());core.config=proposed;core.reset();configStatus="applied";
                } else if(cmd=="RELOAD") {
                    try {
                        auto proposed=Config::parse(readFile(conf));
                        if(proposed.serialize()!=core.config.serialize()){core.config=proposed;core.reset();}
                        configStatus="applied";
                    }catch(const std::exception& e){configStatus=std::string("invalid: ")+e.what();throw;}
                } else if(cmd=="CONFIG_GET") {std::cout<<"{\"ok\":true,\"data\":"<<jsonString(core.config.serialize())<<'}'<<std::endl;continue;
                } else if(cmd=="EXPORT") {if(!hasUser)throw std::runtime_error("user required");std::cout<<"{\"ok\":true,\"data\":"<<jsonString(store.encode(core.learning))<<'}'<<std::endl;continue;
                } else if(cmd=="IMPORT") {
                    std::string path;if(!(in>>std::quoted(path))||!hasUser)throw std::runtime_error("missing import file");auto l=store.decode(readFile(path));store.save(l,true);core.learning=l;saveStatus="imported";core.reset();
                } else if(cmd=="MIGRATE") {
                    std::string path;if(!(in>>std::quoted(path))||!hasUser)throw std::runtime_error("missing migration file");
                    if(saveStatus!="new")throw std::runtime_error("migration requires new state");
                    auto text=readFile(path,256);std::istringstream old(text);std::string record;old>>record;
                    if(record.rfind("user_offset=",0)!=0)throw std::runtime_error("invalid legacy format");
                    size_t used=0;double v=std::stod(record.substr(12),&used);
                    if(used!=record.size()-12||!std::isfinite(v)||std::abs(v)>.3)throw std::runtime_error("invalid legacy offset");
                    Learning l;l.legacy_offset=v;store.save(l);core.learning=l;saveStatus="migrated_offset";
                } else if(cmd!="STATUS") throw std::runtime_error("unknown command");
                std::string extra;if(in>>extra)throw std::runtime_error("trailing command arguments");
                std::cout<<status()<<std::endl;
            }catch(const std::exception& e){std::cout<<"{\"ok\":false,\"error\":"<<jsonString(e.what())<<'}'<<std::endl;}
        }
        close(lock);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
