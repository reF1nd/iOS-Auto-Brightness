// SPDX-License-Identifier: GPL-3.0-only
#include "storage.hpp"
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
namespace brightness {
namespace {
std::runtime_error error(const std::string& s) {return std::runtime_error(s+": "+std::strerror(errno));}
uint64_t checksum(const std::string& s) {uint64_t h=14695981039346656037ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return h;}
}
std::string readFile(const std::string& path,size_t limit) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)throw error("open "+path);
    struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||static_cast<uint64_t>(st.st_size)>limit){close(fd);throw std::runtime_error("invalid or oversized file");}
    std::string out;char b[4096];
    for(;;){ssize_t n=read(fd,b,sizeof b);if(n<0&&errno==EINTR)continue;if(n<0){close(fd);throw error("read");}if(!n)break;out.append(b,static_cast<size_t>(n));if(out.size()>limit){close(fd);throw std::runtime_error("file too large");}}
    close(fd);return out;
}
void atomicWrite(const std::string& path,const std::string& data) {
    auto p=path.find_last_of('/');std::string dir=p==std::string::npos?".":path.substr(0,p);
    int dfd=open(dir.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(dfd<0)throw error("open directory");
    std::string temp=path+".tmp.XXXXXX";std::vector<char> name(temp.begin(),temp.end());name.push_back(0);
    int fd=mkstemp(name.data());if(fd<0){close(dfd);throw error("create temporary file");}
    try {
        if(fchmod(fd,0600))throw error("chmod temporary file");
        size_t at=0;while(at<data.size()){ssize_t n=write(fd,data.data()+at,data.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)throw error("write");at+=static_cast<size_t>(n);}
        if(fsync(fd))throw error("sync file");
        int result=close(fd);fd=-1;if(result)throw error("close file");
        if(rename(name.data(),path.c_str()))throw error("replace file");
        if(fsync(dfd))throw error("sync directory");
        close(dfd);
    } catch(...) {if(fd>=0)close(fd);unlink(name.data());close(dfd);throw;}
}
std::string Store::path() const {return directory+"/learning-"+std::to_string(user)+"-"+profile+".dat";}
std::string Store::encode(const Learning& l) const {
    std::ostringstream s;s<<std::setprecision(17)<<(profile.find("-v68curve2")!=std::string::npos?"IAB_CURVE_OBSERVATIONS_V2 ":"IAB_LEARNING_V1 ")<<profile<<' '<<user<<' '<<l.legacy_offset<<' '<<l.points.size()<<'\n';
    for(auto& p:l.points)s<<p.lux<<' '<<p.value<<' '<<p.time<<'\n';
    const auto body=s.str();s<<"CHECKSUM "<<std::hex<<checksum(body)<<'\n';return s.str();
}
Learning Store::decode(const std::string& text) const {
    if(text.size()>65536)throw std::runtime_error("learning file too large");
    auto pos=text.rfind("CHECKSUM ");if(pos==std::string::npos)throw std::runtime_error("missing checksum");
    std::istringstream check(text.substr(pos+9));uint64_t hash;std::string extra;
    if(!(check>>std::hex>>hash)||(check>>extra)||hash!=checksum(text.substr(0,pos)))throw std::runtime_error("invalid checksum");
    std::istringstream s(text.substr(0,pos));Learning l;std::string magic,id;int64_t uid;size_t count;
    if(!(s>>magic>>id>>uid>>l.legacy_offset>>count)||magic!=(profile.find("-v68curve2")!=std::string::npos?"IAB_CURVE_OBSERVATIONS_V2":"IAB_LEARNING_V1")||id!=profile||uid!=user||count>32||!std::isfinite(l.legacy_offset)||std::abs(l.legacy_offset)>.3)throw std::runtime_error("invalid learning identity or format");
    for(size_t i=0;i<count;i++) {
        Point p{};if(!(s>>p.lux>>p.value>>p.time)||!std::isfinite(p.lux)||!std::isfinite(p.value)||p.lux<0||p.lux>200000||p.value<0||p.value>1||p.time<=0)throw std::runtime_error("invalid learning point");
        if(i&&(p.lux<=l.points.back().lux||(magic=="IAB_LEARNING_V1"&&p.value<l.points.back().value)))throw std::runtime_error("nonmonotonic learning data");
        l.points.push_back(p);
    }
    if(s>>extra)throw std::runtime_error("trailing learning data");
    return l;
}
Learning Store::load(std::string& status) const {
    const auto p=path();
    try {auto l=decode(readFile(p));status="loaded";return l;}catch(const std::exception& e){status=e.what();}
    try {auto l=decode(readFile(p+".bak"));status="recovered_backup";return l;}catch(...){}
    if(access(p.c_str(),F_OK)!=0&&access((p+".bak").c_str(),F_OK)!=0)status="new";
    else status="load_failed: "+status;
    return {};
}
void Store::save(const Learning& l,bool clearBackup) const {
    auto text=encode(l);decode(text); // Validate before touching any existing data.
    std::string previous;
    try {previous=readFile(path());decode(previous);}catch(...){previous.clear();}
    if(!previous.empty())atomicWrite(path()+".bak",previous);
    atomicWrite(path(),text);
    if(clearBackup||previous.empty())atomicWrite(path()+".bak",text);
}
}
