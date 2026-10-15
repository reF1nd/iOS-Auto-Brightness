// SPDX-License-Identifier: GPL-3.0-only
#include "model.hpp"
#include "reference.hpp"
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
    take("manual_gamma",c.manual_gamma,1.2,3.5);
    take("hyst_low",c.hyst_low,0,.2); take("hyst_high",c.hyst_high,0,.2);
    take("high_lux_threshold",c.high_lux_threshold,5000,100000);
    take("high_lux_boost_max",c.high_lux_boost_max,0,1); take("high_lux_min_pct",c.high_lux_min_pct,0,1);
    integer("high_lux_duration_ms",c.high_lux_duration_ms,1000,20000);
    integer("user_hold_ms",c.user_hold_ms,2000,30000);
    integer("log_max_bytes",c.log_max_bytes,65536,8388608); integer("log_max_backups",c.log_max_backups,1,8);
    boolean("night_enabled",c.night_enabled); boolean("low_battery_enabled",c.low_battery_enabled);
    boolean("settings_learning",c.settings_learning); boolean("learning_enabled",c.learning_enabled); boolean("enabled",c.enabled);
    if(!values.empty()) throw std::runtime_error("unknown key: "+values.begin()->first);
    if(c.alpha_slow>c.alpha_mid||c.alpha_mid>c.alpha_fast||c.slow_threshold>=c.fast_threshold) throw std::runtime_error("inconsistent response thresholds");
    return c;
}
std::string Config::serialize() const {
    std::ostringstream s; s<<std::setprecision(9);
#define FIELD(x) s<<#x<<'='<<x<<'\n'
    FIELD(enabled);FIELD(learning_enabled);FIELD(settings_learning);FIELD(night_enabled);FIELD(low_battery_enabled);FIELD(manual_gamma);FIELD(alpha_fast);FIELD(alpha_mid);FIELD(alpha_slow);
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
void Learning::add(double lux,double value,int64_t time,bool constrain) {
    range(lux,0,200000,"learn lux");range(value,0,1,"learn brightness");
    if(time<=0) throw std::runtime_error("invalid sample timestamp");
    auto it=std::min_element(points.begin(),points.end(),[&](const Point&a,const Point&b){return std::abs(std::log1p(a.lux)-std::log1p(lux))<std::abs(std::log1p(b.lux)-std::log1p(lux));});
    if(it!=points.end()&&std::abs(std::log1p(it->lux)-std::log1p(lux))<.25) *it={lux,value,time};
    else points.push_back({lux,value,time});
    if(points.size()>32) points.erase(std::min_element(points.begin(),points.end(),[](auto&a,auto&b){return a.time<b.time;}));
    std::sort(points.begin(),points.end(),[](auto&a,auto&b){return a.lux<b.lux;});
    // The latest explicit adjustment takes precedence; constrain conflicting neighbours.
    if(constrain)for(auto& p:points) {if(p.lux<lux)p.value=std::min(p.value,value); if(p.lux>lux)p.value=std::max(p.value,value);}
}
double Learning::target(double lux,const Profile& p,double baseScale) const {
    const auto base=[&](double l){return std::clamp((p.toBrightness(baseNits(l))+legacy_offset)*baseScale,0.0,1.0);};
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
double Learning::referenceTarget(double lux) const {
    const auto base=[](double l){return referenceBrightness(referenceNits(l));};
    if(points.empty())return std::clamp(base(lux)+legacy_offset,0.0,1.0);
    bool cached=fitted_offset==legacy_offset&&fitted_from.size()==points.size();
    for(size_t i=0;cached&&i<points.size();i++)cached=points[i].lux==fitted_from[i].lux&&points[i].value==fitted_from[i].value;
    if(cached)return interpolate(std::log1p(lux),fitted_x,fitted_y);
    // All samples contribute bounded residuals. A unit-weight zero-offset prior
    // keeps the preset curve authoritative; no sample is an interpolation anchor.
    std::vector<double> px,delta,xs,ys;
    for(const auto& p:points){px.push_back(std::log1p(p.lux));delta.push_back(std::clamp(p.value-base(p.lux),-.3,.3));}
    constexpr int count=129;
    for(int i=0;i<count;i++) {
        double x=i*std::log1p(200000.0)/(count-1),sum=0,weight=1;
        for(size_t j=0;j<px.size();j++) {
            double distance=(x-px[j])/std::log(10.0),w=1/(1+distance*distance);
            sum+=w*delta[j];weight+=w;
        }
        xs.push_back(x);ys.push_back(std::clamp(base(std::expm1(x))+std::clamp(legacy_offset+sum/weight,-.3,.3),0.0,1.0));
    }
    // Isotonic projection prevents contradictory observations from producing a
    // falling brightness curve. It does not force the curve through observations.
    std::vector<double> means;std::vector<int> sizes;
    for(double y:ys) {
        means.push_back(y);sizes.push_back(1);
        while(means.size()>1&&means[means.size()-2]>means.back()) {
            auto n=means.size();int total=sizes[n-2]+sizes[n-1];
            means[n-2]=(means[n-2]*sizes[n-2]+means[n-1]*sizes[n-1])/total;
            sizes[n-2]=total;means.pop_back();sizes.pop_back();
        }
    }
    size_t at=0;for(size_t i=0;i<means.size();i++)for(int j=0;j<sizes[i];j++)ys[at++]=means[i];
    fitted_from=points;fitted_offset=legacy_offset;fitted_x=xs;fitted_y=ys;
    return interpolate(std::log1p(lux),xs,ys);
}
double Controller::nightScale() const {
    return config.night_enabled && frame_.local_hour>=0 && frame_.local_hour<24 &&
        (frame_.local_hour>=18 || frame_.local_hour<6) ? .95 : 1;
}
double Controller::base(double lux) const {return reference?referenceBrightness(referenceNits(lux)):profile.toBrightness(baseNits(lux));}
double Controller::target(double lux) const {
    return reference?std::clamp(learning.referenceTarget(lux)*nightScale(),0.0,1.0):learning.target(lux,profile,nightScale());
}
bool Controller::slider(int value,int64_t wallTime) {
    if(value<0||value>255||wallTime<=0)throw std::runtime_error("slider outside reference range");
    const int before=last_slider_;
    if(before<0){last_slider_=value;return false;}
    if(!reference||!config.enabled||!config.learning_enabled||!frame_.automatic||!frame_.screen||frame_.blocked||frame_.near||proximity_confirmed_||dark_pending_||smooth_<0||output_<0) {
        last_slider_=value;return false;
    }
    if(std::abs(value-before)<10)return false;
    last_slider_=value;
    learning.add(smooth_,std::clamp(referenceSlider(value,config.manual_gamma)/nightScale(),0.0,1.0),wallTime,false);
    return true;
}
void Controller::reset() {
    last_slider_=-1;frame_={};smooth_=-1;output_=-1;candidate_=-1;last_=0;hold_until_=0;
    candidate_since_=0;wake_until_=0;trend_.clear();fast_dark_=false;sunlight_=false;solar_decay_=false;dark_pending_=false;dark_since_=-1;
    near_since_=-1;far_since_=-1;proximity_confirmed_=false;front_baseline_=-1;
    guard_front_=0;guard_back_=0;guard_confirmed_=false;guard_missing_=false;ambient_history_.clear();
    high_confirm_=0;high_exit_at_=-1;last_high_confirm_=-1;strong_since_=-1;alpha_=0;desired_=0;
    sunlight_target_=0;ramp_from_=0;ramp_target_=-1;ramp_at_=0;ramp_duration_=0;fast_until_=0;
}
void Controller::hold(int64_t now) {hold_until_=now+config.user_hold_ms;output_=-1;ramp_target_=-1;ramp_duration_=0;}
bool Controller::learn(double lux,double value,int64_t time) {
    if(!config.learning_enabled||!config.enabled||!frame_.automatic||!frame_.screen||frame_.blocked||frame_.near||proximity_confirmed_||dark_pending_||smooth_<0||frame_.lux<0) return false;
    if(!std::isfinite(lux)||lux<0||std::abs(std::log1p(lux)-std::log1p(smooth_))>std::log(3.0)) return false;
    if(value<frame_.minimum||value>frame_.maximum) return false;
    learning.add(lux,value,time,!reference);if(!reference)hold(frame_.now);return true;
}
Output Controller::step(const Frame& f) {
    const bool waking=f.automatic&&f.screen&&(!frame_.automatic||!frame_.screen);
    if(waking){reset();wake_until_=f.now+(reference?0:2000);}
    frame_=f;
    auto clearStrong=[&](){high_confirm_=0;last_high_confirm_=-1;strong_since_=-1;};
    auto release=[&](const char* reason){output_=-1;ramp_duration_=0;ramp_target_=-1;return Output{false,0,reason};};
    if(!config.enabled) return release("disabled");
    if(!f.automatic) {reset();return release("manual_or_unknown");}
    if(!f.screen) {reset();return release("screen_off");}
    if(f.blocked) {clearStrong();sunlight_=false;high_exit_at_=-1;return release("system_override");}

    if(!std::isfinite(f.lux)||f.lux<0||f.lux>200000) {clearStrong();sunlight_=false;smooth_=-1;dark_pending_=false;dark_since_=-1;guard_confirmed_=false;ambient_history_.clear();return release("sensor_unavailable");}
    if(!std::isfinite(f.minimum)||!std::isfinite(f.maximum)||f.minimum<0||f.maximum>1||f.minimum>f.maximum||!std::isfinite(f.current)) {clearStrong();return release("limits_unavailable");}
    // Brief proximity pulses must not expose a different system brightness.
    // Freeze an existing output (and learning) while confirming obstruction.
    // State/safety exits above always win, including a newly lowered cap.
    if(f.near) {
        clearStrong();
        far_since_=-1;
        if(near_since_<0||f.now<near_since_)near_since_=f.now;
        if(f.now-near_since_>=600)proximity_confirmed_=true;
        last_=f.now;
        if(output_<0||f.now<hold_until_)return release(proximity_confirmed_?"proximity_hold":"proximity_pending");
        const double cap=config.low_battery_enabled&&f.battery>=0&&f.battery<15&&!f.charging?
            std::max(f.minimum,std::min(f.maximum,.7)):f.maximum;
        output_=std::clamp(output_,f.minimum,cap);desired_=output_;
        ramp_duration_=0;ramp_target_=-1;
        return {true,output_,proximity_confirmed_?"proximity_hold":"proximity_pending"};
    }
    near_since_=-1;
    if(proximity_confirmed_) {
        if(far_since_<0||f.now<far_since_)far_since_=f.now;
        if(f.now-far_since_<300) {
            if(output_<0||f.now<hold_until_)return release("proximity_recovery");
            const double cap=config.low_battery_enabled&&f.battery>=0&&f.battery<15&&!f.charging?
                std::max(f.minimum,std::min(f.maximum,.7)):f.maximum;
            output_=std::clamp(output_,f.minimum,cap);desired_=output_;last_=f.now;
            return {true,output_,"proximity_recovery"};
        }
        proximity_confirmed_=false;far_since_=-1;
    }
    double lux=f.lux;
    const bool dual=std::isfinite(f.front)&&f.front>=0&&f.front<=200000;
    const double frontLux=dual?f.front:lux;
    const double frontBaseline=dual?front_baseline_:smooth_;

    // A covered rear sensor is normal on a desk. It can extend spike
    // confirmation, but must never cap a stable front-light measurement.
    double dt=last_?std::clamp((f.now-last_)/1000.0,.01,8.0):.25;last_=f.now;
    // Keep a bounded recent rear baseline, including the value carried into
    // the window. Rear events can precede a front event when entering darkness.
    while(ambient_history_.size()>1&&ambient_history_[1].now<f.now-5000)ambient_history_.erase(ambient_history_.begin());
    double rearBaseline=f.back;
    for(const auto& previous:ambient_history_)if(std::isfinite(previous.back))rearBaseline=std::max(rearBaseline,previous.back);
    ambient_history_.push_back(f);
    if(ambient_history_.size()>256)ambient_history_.erase(ambient_history_.begin());
    // Compare sensors even in dim rooms. The +1 regularizer avoids treating
    // sub-lux noise or a covered rear sensor as strong contrary evidence; an
    // absolute 20-lux gate previously discarded a stable 19-lux rear reading.
    const bool rearValid=std::isfinite(f.back)&&f.back>=0&&f.back<=200000;
    const bool rearBright=rearValid&&(f.back+1)/(frontLux+1)>2;
    if(guard_confirmed_&&frontLux>=frontBaseline*.8)guard_confirmed_=false;
    if(!dark_pending_&&!guard_confirmed_&&frontBaseline>=1&&(frontLux+1)/(frontBaseline+1)<.65&&rearBright&&(!dual||lux<smooth_*.8)) {
        dark_pending_=true;dark_since_=-1;guard_front_=frontBaseline;guard_back_=rearBaseline;
    }
    if(dark_pending_) {
        double frontRatio=(frontLux+1)/(guard_front_+1),rearRatio=(f.back+1)/(guard_back_+1);
        // Crossing an absolute/contrast threshold is not proof of darkness.
        // Require both sensors near darkness, or comparable relative declines
        // from the pre-drop baselines, continuously for the confirmation period.
        bool corroborated=rearValid&&((frontLux<=1&&f.back<=1)||(rearRatio<=.5&&rearRatio<=frontRatio*1.5));
        if(frontLux>=guard_front_*.8||(dual&&lux>=smooth_*.8)) {dark_pending_=false;dark_since_=-1;}
        else if(corroborated||!rearValid) {
            if(dark_since_<0||guard_missing_!=!rearValid)dark_since_=f.now;
            guard_missing_=!rearValid;
            // Missing rear readings cannot hold the screen indefinitely.
            if(f.now-dark_since_>=(rearValid?2000:5000)){dark_pending_=false;dark_since_=-1;guard_confirmed_=true;}
        } else dark_since_=-1;
    }
    if(dual&&!dark_pending_)front_baseline_=front_baseline_<0?frontLux:
        front_baseline_+(1-std::pow(1-config.alpha_mid,dt/.25))*(frontLux-front_baseline_);
    if(dark_pending_)lux=smooth_;
    // Confirm the selected lux coordinate before bypassing slow filters.
    // Two illuminated sides need 250 ms; a single side needs 750 ms so a
    // brief flash or direction change cannot trigger the fast path.
    const bool safe=std::isfinite(f.temp)&&f.temp>=0&&f.temp<=45&&f.battery>=0&&(f.battery>=30||f.charging);
    const bool strong=safe&&!dark_pending_&&f.now>=hold_until_&&lux>=config.high_lux_threshold;
    if(strong) {
        if(strong_since_<0||f.now<strong_since_){clearStrong();strong_since_=f.now;}
        if(last_high_confirm_<0||f.now-last_high_confirm_>=250) {
            last_high_confirm_=f.now;high_confirm_=std::min(2,high_confirm_+1);
        }
    } else clearStrong();
    const bool bothBright=dual&&rearValid&&f.back>=config.high_lux_threshold&&frontLux>=config.high_lux_threshold;
    const bool strongConfirmed=strong&&high_confirm_>=2&&f.now-strong_since_>=(bothBright?250:750);
    if(strongConfirmed)smooth_=std::max(smooth_,lux);
    trend_.push_back(lux);if(trend_.size()>5)trend_.erase(trend_.begin());
    if(trend_.size()>=3) {
        const size_t n=trend_.size();const double first=trend_[n-3],middle=trend_[n-2];
        if(!rearBright && first>middle && middle>lux && (first-lux)/std::max(first,.01)>.35)
            fast_until_=f.now+800;
    }
    fast_dark_=!rearBright&&f.now<fast_until_;
    if(smooth_<0) smooth_=lux;
    else {
        double ratio=(lux+1)/(smooth_+1);
        if(ratio>config.spike_up||ratio<config.spike_down) {
            if(candidate_<0||std::abs(std::log1p(candidate_)-std::log1p(lux))>.4) {candidate_=lux;candidate_since_=f.now;}
            const bool rearDisagrees=ratio>config.spike_up&&std::isfinite(f.back)&&f.back>=0&&f.back<lux/config.spike_up;
            if(!fast_dark_ && f.now-candidate_since_<(rearDisagrees?1500:750)) lux=smooth_;
        } else candidate_=-1;
        double diff=std::abs(lux-smooth_)/std::max(smooth_,1.0);
        double blend=std::clamp((diff-config.slow_threshold)/(config.fast_threshold-config.slow_threshold),0.0,1.0);
        alpha_=diff<=config.slow_threshold?config.alpha_slow:config.alpha_mid+blend*(config.alpha_fast-config.alpha_mid);
        if(trend_.size()==5) {
            double slope=0;for(size_t i=0;i<5;i++)slope+=(static_cast<double>(i)-2)*trend_[i];
            if(std::abs(slope/10)>50)alpha_=std::max(alpha_,.3);
        }
        if(fast_dark_)alpha_=std::max(alpha_,.7);
        smooth_+=(1-std::pow(1-alpha_,dt/.25))*(lux-smooth_);
    }
    if(f.now<hold_until_) return release("user_adjustment");
    // Let the display leave AOD and the light sensor settle before seeding
    // the output from current brightness. Never start a ramp from doze level.
    const bool fastWake=f.now<wake_until_&&strongConfirmed;
    if(f.now<wake_until_&&!fastWake) return release("wake_settling");
    // Explicit learned points take precedence over the time-of-day default.
    double target=this->target(smooth_);
    const bool wasSunlight=sunlight_;
    if(!safe) {sunlight_=false;clearStrong();high_exit_at_=-1;}
    else if(!sunlight_&&strongConfirmed) {sunlight_=true;high_exit_at_=-1;}
    if(sunlight_) {
        if(smooth_>=config.high_lux_threshold) {
            high_exit_at_=-1;
            const double progress=std::clamp((smooth_-config.high_lux_threshold)/config.high_lux_threshold,0.0,1.0);
            const double floor=reference?f.maximum*config.high_lux_min_pct:target+progress*std::max(0.0,f.maximum*config.high_lux_min_pct-target);
            sunlight_target_=std::max(target*(1+progress*config.high_lux_boost_max),floor);
        } else if(smooth_<config.high_lux_threshold*.7) {
            if(high_exit_at_<0)high_exit_at_=f.now;
            if(f.now-high_exit_at_>=config.high_lux_duration_ms){sunlight_=false;clearStrong();}
        } else high_exit_at_=-1;
        if(sunlight_)target=sunlight_target_;
    }
    // Normalized 1.0 is the panel maximum; do not multiply the system cap by .7.
    bool lowBattery=config.low_battery_enabled&&f.battery>=0&&f.battery<15&&!f.charging;
    double cap=lowBattery?std::max(f.minimum,std::min(f.maximum,.7)):f.maximum;
    // Hold actual output rather than recomputing a different point on the curve.
    // A manual adjustment still takes effect, but cannot teach a shaded sample.
    if(dark_pending_)target=output_>=0?output_:f.current;
    target=std::clamp(target,f.minimum,cap);
    if(fastWake) {
        // Do not dim a bright OEM wake frame or ramp from an AOD-level frame.
        output_=std::clamp(std::max(f.current,target),f.minimum,cap);desired_=output_;
        ramp_duration_=0;ramp_target_=-1;
        return {true,output_,"sunlight"};
    }
    if(reference&&output_<0) {
        // Reference warm start applies the current curve instead of replaying a
        // stale framework level. Screen-off frames never reach this write path.
        output_=target;desired_=target;ramp_duration_=0;ramp_target_=target;
        return {true,output_,"reference_warm_start"};
    }
    if(output_<0)output_=std::clamp(f.current,f.minimum,cap);
    const bool fastBright=strongConfirmed&&target>output_;
    const bool solarExit=wasSunlight&&!sunlight_;
    if(solarExit){solar_decay_=true;ramp_duration_=0;ramp_target_=-1;}
    if(strongConfirmed)solar_decay_=false;
    // low/high refer to brightness level, not the direction of the change.
    double hysteresis=config.hyst_low+(config.hyst_high-config.hyst_low)*std::clamp(output_,0.0,1.0);
    bool changed=std::abs(target-output_)>std::max(.00001,output_*hysteresis);
    if(changed) {
        bool reverse=ramp_target_>=0 && (target-output_)*(ramp_target_-output_)<0;
        if(ramp_duration_==0 || reverse || (fast_dark_&&!solar_decay_&&ramp_duration_>200) || (fastBright&&ramp_duration_>400)) {
            // Time spent asleep preceded this target; it must not consume the new ramp.
            ramp_from_=output_;ramp_at_=reference?f.now:f.now-static_cast<int64_t>(std::min(dt,.25)*1000);
            double distance=std::abs(target-output_);
            ramp_duration_=solar_decay_&&target<output_?3000:(fastBright?400:(rearBright&&target<output_?3000:(fast_dark_&&target<output_?200:(sunlight_?1000:static_cast<int64_t>(std::clamp(distance*8000,400.0,3000.0))))));
            if(reference)ramp_duration_=sunlight_?1000:(fast_dark_&&target<output_?200:referenceRamp(distance,target>output_));
        }
        ramp_target_=target;
        double t=ramp_duration_==0?1:std::clamp(static_cast<double>(f.now-ramp_at_)/ramp_duration_,0.0,1.0);
        double eased=t<.5?4*t*t*t:1-std::pow(-2*t+2,3)/2;
        double previous=output_;
        output_=std::clamp(ramp_from_+(ramp_target_-ramp_from_)*eased,std::min(previous,target),std::max(previous,target));
        // Moving endpoints must not bypass the downward slew limit. Ignore
        // sleep time before a new target; system/low-battery caps below win.
        if(!reference&&output_<previous)output_=std::max(output_,previous*std::pow(.8,std::min(dt,.25)));
        if(t>=1&&std::abs(output_-target)<.00001)ramp_duration_=0;
        desired_=target;
    } else {ramp_duration_=0;ramp_target_=output_;desired_=output_;solar_decay_=false;}
    output_=std::clamp(output_,f.minimum,cap);
    return {true,output_,lowBattery?"low_battery_limit":(dark_pending_?"ambient_guard":(sunlight_?"sunlight":(!safe?"auto_boost_unavailable":(nightScale()<1?"night":"auto"))))};
}
std::string jsonString(const std::string& s) {
    std::ostringstream o;o<<'"';for(unsigned char c:s) {switch(c){case '"':o<<"\\\"";break;case '\\':o<<"\\\\";break;case '\n':o<<"\\n";break;case '\r':o<<"\\r";break;case '\t':o<<"\\t";break;default:if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;else o<<c;}}o<<'"';return o.str();
}
}
