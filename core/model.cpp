// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
namespace brightness {
namespace {
std::string trim(std::string s) {
    const auto a=s.find_first_not_of(" \r\t\n");
    if(a==std::string::npos) return {};
    return s.substr(a,s.find_last_not_of(" \r\t\n")-a+1);
}
double numeric(const std::string& s) {
    size_t used=0; double v=std::stod(s,&used);
    if(used!=s.size() || !std::isfinite(v)) throw std::runtime_error("non-finite or malformed number");
    return v;
}
void range(double x,double a,double b,const std::string& key) {
    if(!std::isfinite(x)||x<a||x>b) throw std::runtime_error("invalid range: "+key);
}
double interpolate(double x,const std::vector<double>& xs,const std::vector<double>& ys) {
    if(xs.size()<2||xs.size()!=ys.size()||!std::isfinite(x)) throw std::runtime_error("invalid calibration");
    if(x<=xs.front()) return ys.front();
    if(x>=xs.back()) return ys.back();
    size_t i=std::upper_bound(xs.begin(),xs.end(),x)-xs.begin();
    return ys[i-1]+(ys[i]-ys[i-1])*(x-xs[i-1])/(xs[i]-xs[i-1]);
}
}
Config Config::parse(const std::string& text) {
    Config c; std::map<std::string,double> values; std::istringstream in(text); std::string line;
    while(std::getline(in,line)) {
        line=trim(line.substr(0,line.find('#'))); if(line.empty()) continue;
        auto p=line.find('='); if(p==std::string::npos) throw std::runtime_error("expected key=value");
        auto key=trim(line.substr(0,p)); auto v=numeric(trim(line.substr(p+1)));
        if(!values.emplace(key,v).second) throw std::runtime_error("duplicate key: "+key);
    }
    auto take=[&](const char* key,double& dst,double lo,double hi) {
        auto it=values.find(key); if(it==values.end()) return;
        range(it->second,lo,hi,key); dst=it->second; values.erase(it);
    };
    auto integer=[&](const char* key,int& dst,int lo,int hi) {
        double v=dst; take(key,v,lo,hi); if(std::floor(v)!=v) throw std::runtime_error("expected integer"); dst=static_cast<int>(v);
    };
    auto boolean=[&](const char* key,bool& dst) {int v=dst;integer(key,v,0,1);dst=v;};
    take("alpha_fast",c.alpha_fast,.01,.9); take("alpha_mid",c.alpha_mid,.01,.9); take("alpha_slow",c.alpha_slow,.001,.9);
    take("fast_threshold",c.fast_threshold,.1,5); take("slow_threshold",c.slow_threshold,.001,.5);
    take("spike_up",c.spike_up,2,50); take("spike_down",c.spike_down,.001,.5);
    take("hyst_low",c.hyst_low,0,.2); take("hyst_high",c.hyst_high,0,.2);
    take("high_lux_threshold",c.high_lux_threshold,5000,100000);
    take("high_lux_boost_max",c.high_lux_boost_max,0,1); take("high_lux_min_pct",c.high_lux_min_pct,0,1);
    integer("high_lux_duration_ms",c.high_lux_duration_ms,1000,20000);
    integer("user_hold_ms",c.user_hold_ms,2000,30000);
    integer("log_max_bytes",c.log_max_bytes,65536,8388608); integer("log_max_backups",c.log_max_backups,1,8);
    boolean("learning_enabled",c.learning_enabled); boolean("enabled",c.enabled);
    if(!values.empty()) throw std::runtime_error("unknown key: "+values.begin()->first);
    if(c.alpha_slow>c.alpha_mid||c.alpha_mid>c.alpha_fast||c.slow_threshold>=c.fast_threshold) throw std::runtime_error("inconsistent response thresholds");
    return c;
}
std::string Config::serialize() const {
    std::ostringstream s; s<<std::setprecision(9);
#define FIELD(x) s<<#x<<'='<<x<<'\n'
    FIELD(enabled);FIELD(learning_enabled);FIELD(alpha_fast);FIELD(alpha_mid);FIELD(alpha_slow);
    FIELD(fast_threshold);FIELD(slow_threshold);FIELD(spike_up);FIELD(spike_down);FIELD(hyst_low);FIELD(hyst_high);
    FIELD(high_lux_threshold);FIELD(high_lux_boost_max);FIELD(high_lux_min_pct);FIELD(high_lux_duration_ms);
    FIELD(user_hold_ms);FIELD(log_max_bytes);FIELD(log_max_backups);
#undef FIELD
    return s.str();
}
Profile Profile::parse(const std::string& text) {
    std::istringstream s(text); Profile p; std::string magic; size_t count=0;
    if(!(s>>magic>>p.id>>count)||magic!="IAB_PROFILE_V1"||count<2||count>256||p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=std::string::npos) throw std::runtime_error("invalid profile header");
    for(size_t i=0;i<count;i++) {
        double n,b; if(!(s>>n>>b)) throw std::runtime_error("truncated profile");
        range(n,0,20000,"nits");range(b,0,1,"backlight");
        if(i&&(n<=p.nits.back()||b<=p.backlight.back())) throw std::runtime_error("profile is not strictly increasing");
        p.nits.push_back(n);p.backlight.push_back(b);
    }
    std::string extra; if(s>>extra) throw std::runtime_error("trailing profile data");
    return p;
}
double Profile::toBrightness(double x) const {return interpolate(x,nits,backlight);}
double Profile::toNits(double x) const {return interpolate(x,backlight,nits);}
double baseNits(double lux) {
    if(!std::isfinite(lux)||lux<0) throw std::runtime_error("invalid lux");
    // Continuous at 0.5 lux (1 nit) and 2 lux (3 nits).
    if(lux<=.5) return .222+lux*1.556;
    if(lux<=2) return 1+std::log(lux/.5)/std::log(4)*2;
    if(lux<=10) return 3+std::log(lux/2)/std::log(5)*12;
    if(lux<=100) return 15+std::log(lux/10)/std::log(10)*85;
    if(lux<=1000) return 100+std::log(lux/100)/std::log(10)*200;
    if(lux<=10000) return 300+std::log(lux/1000)/std::log(10)*200;
    return 500+std::log(std::min(lux,100000.0)/10000)/std::log(10)*38.5;
}
void Learning::add(double lux,double value,int64_t time) {
    range(lux,0,200000,"learn lux");range(value,0,1,"learn brightness");
    if(time<=0) throw std::runtime_error("invalid sample timestamp");
    auto it=std::min_element(points.begin(),points.end(),[&](const Point&a,const Point&b){return std::abs(std::log1p(a.lux)-std::log1p(lux))<std::abs(std::log1p(b.lux)-std::log1p(lux));});
    if(it!=points.end()&&std::abs(std::log1p(it->lux)-std::log1p(lux))<.25) *it={lux,value,time};
    else points.push_back({lux,value,time});
    if(points.size()>32) points.erase(std::min_element(points.begin(),points.end(),[](auto&a,auto&b){return a.time<b.time;}));
    std::sort(points.begin(),points.end(),[](auto&a,auto&b){return a.lux<b.lux;});
    // The latest explicit adjustment takes precedence; constrain conflicting neighbours.
    for(auto& p:points) {if(p.lux<lux)p.value=std::min(p.value,value); if(p.lux>lux)p.value=std::max(p.value,value);}
}
double Learning::target(double lux,const Profile& p) const {
    const auto base=[&](double l){return std::clamp(p.toBrightness(baseNits(l))+legacy_offset,0.0,1.0);};
    if(points.empty()) return base(lux);
    std::vector<double> xs,ys;
    const double left=std::max(0.0,std::expm1(std::log1p(points.front().lux)-std::log(10.0)));
    const double right=std::min(200000.0,std::expm1(std::log1p(points.back().lux)+std::log(10.0)));
    if(left<points.front().lux) {xs.push_back(std::log1p(left));ys.push_back(std::min(base(left),points.front().value));}
    for(auto& point:points) {xs.push_back(std::log1p(point.lux));ys.push_back(point.value);}
    if(right>points.back().lux) {xs.push_back(std::log1p(right));ys.push_back(std::max(base(right),points.back().value));}
    if(lux<left) return std::min(base(lux),ys.front());
    if(lux>right) return std::max(base(lux),ys.back());
    if(xs.size()==1) return ys.front();
    return interpolate(std::log1p(lux),xs,ys);
}
void Controller::reset() {frame_={};smooth_=-1;output_=-1;candidate_=-1;last_=0;hold_until_=0;high_until_=0;candidate_since_=0;wake_until_=0;}
void Controller::hold(int64_t now) {hold_until_=now+config.user_hold_ms;output_=-1;}
bool Controller::learn(double lux,double value,int64_t time) {
    if(!config.learning_enabled||!config.enabled||!frame_.automatic||!frame_.screen||frame_.blocked||frame_.near||smooth_<0||frame_.lux<0) return false;
    if(!std::isfinite(lux)||lux<0||std::abs(std::log1p(lux)-std::log1p(smooth_))>std::log(3.0)) return false;
    if(value<frame_.minimum||value>frame_.maximum) return false;
    learning.add(lux,value,time);hold(frame_.now);return true;
}
Output Controller::step(const Frame& f) {
    const bool waking=f.automatic&&f.screen&&(!frame_.automatic||!frame_.screen);
    if(waking){reset();wake_until_=f.now+2000;}
    frame_=f;
    auto release=[&](const char* reason){output_=-1;return Output{false,0,reason};};
    if(!config.enabled) return release("disabled");
    if(!f.automatic) {reset();return release("manual_or_unknown");}
    if(!f.screen) {reset();return release("screen_off");}
    if(f.blocked) return release("system_override");
    if(f.near) return release("proximity");
    if(!std::isfinite(f.lux)||f.lux<0||f.lux>200000) {smooth_=-1;return release("sensor_unavailable");}
    if(!std::isfinite(f.minimum)||!std::isfinite(f.maximum)||f.minimum<0||f.maximum>1||f.minimum>f.maximum||!std::isfinite(f.current)) return release("limits_unavailable");
    double lux=f.lux;
    // A covered rear sensor is normal on a desk. It can extend spike
    // confirmation, but must never cap a stable front-light measurement.
    double dt=last_?std::clamp((f.now-last_)/1000.0,.01,2.0):.25;last_=f.now;
    if(smooth_<0) smooth_=lux;
    else {
        double ratio=(lux+1)/(smooth_+1);
        if(ratio>config.spike_up||ratio<config.spike_down) {
            if(candidate_<0||std::abs(std::log1p(candidate_)-std::log1p(lux))>.4) {candidate_=lux;candidate_since_=f.now;}
            const bool rearDisagrees=ratio>config.spike_up&&std::isfinite(f.back)&&f.back>=0&&f.back<lux/config.spike_up;
            if(f.now-candidate_since_<(rearDisagrees?1500:750)) lux=smooth_;
        } else candidate_=-1;
        double diff=std::abs(lux-smooth_)/std::max(smooth_,1.0);
        double a=diff>=config.fast_threshold?config.alpha_fast:(diff<=config.slow_threshold?config.alpha_slow:config.alpha_mid);
        smooth_+=(1-std::pow(1-a,dt/.25))*(lux-smooth_);
    }
    if(f.now<hold_until_) return release("user_adjustment");
    // Let the display leave AOD and the light sensor settle before seeding
    // the output from current brightness. Never start a ramp from doze level.
    if(f.now<wake_until_) return release("wake_settling");
    double target=learning.target(smooth_,profile);
    const bool safe=std::isfinite(f.temp)&&f.temp>=0&&f.temp<=45&&f.battery>=0&&(f.battery>=30||f.charging);
    if(safe&&smooth_>=config.high_lux_threshold) high_until_=f.now+config.high_lux_duration_ms;
    if(!safe) high_until_=0;
    bool sunlight=safe&&high_until_>f.now;
    if(sunlight) {
        const double boost=std::clamp((smooth_-config.high_lux_threshold)/config.high_lux_threshold,0.0,1.0)*config.high_lux_boost_max;
        target=std::max(target*(1+boost),f.maximum*config.high_lux_min_pct);
    }
    target=std::clamp(target,f.minimum,f.maximum);
    if(output_<0) output_=std::clamp(f.current,f.minimum,f.maximum);
    double hysteresis=target>output_?config.hyst_high:config.hyst_low;
    if(std::abs(target-output_)>std::max(.00001,output_*hysteresis)) {
        const double a=1-std::exp(-dt/(target>output_?.45:1.2));
        output_+=a*(target-output_);
    }
    output_=std::clamp(output_,f.minimum,f.maximum);
    return {true,output_,sunlight?"sunlight":(!safe?"auto_boost_unavailable":"auto")};
}
std::string jsonString(const std::string& s) {
    std::ostringstream o;o<<'"';for(unsigned char c:s) {switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;default:if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;else o<<c;}}o<<'"';return o.str();
}
}
