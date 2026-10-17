// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace brightness {
// Numeric reference data recovered from v6.8; values are not a panel calibration.
inline double referenceBrightness(double nits) {
    static constexpr double table[]={
  0.222324, 4.444427, 6.897454, 11.746254, 18.499263,
  27.198215, 37.076188, 47.788082, 60.855153, 75.885078,
  91.581012, 106.566015, 118.267195, 135.453849, 153.958282,
  173.797341, 194.969823, 218.68604, 243.586065, 270.287649,
  296.118024, 324.675573, 353.026839, 383.425091, 415.169813,
  446.740448, 465.361556, 468.12312, 471.112089, 473.581759,
  476.674673, 479.022195, 482.37416, 485.223322, 487.664441,
  490.827235, 492.951404, 496.036411, 499.129524, 502.195445,
  505.346186, 507.992082, 511.172582, 514.392001, 516.955388,
  520.755191, 523.192138, 526.367738, 529.734164, 532.000526,
  535.32172, 538.537659,
};
    if(nits<=table[0])return 0;
    if(nits>=table[51])return 1;
    for(int i=0;i<51;i++)if(nits<table[i+1])return (i+(nits-table[i])/(table[i+1]-table[i]))/51;
    return 1;
}
inline double referenceNits(double lux) {
    if(!std::isfinite(lux)||lux<0)throw std::runtime_error("invalid reference lux");
    if(lux<=.5)return .222+lux*1.556;
    // Keep both neighbouring endpoints while repairing v6.8's discontinuity:
    // its log(lux)/log(2) branch fell below zero just above 0.5 lux.
    if(lux<=2)return 1+std::log(lux/.5)/std::log(4)*2;
    if(lux<=10)return 3+std::log(lux/2)/std::log(5)*12;
    if(lux<=100)return 15+std::log(lux/10)/std::log(10)*85;
    if(lux<=1000)return 100+std::log(lux/100)/std::log(10)*200;
    if(lux<=10000)return 300+std::log(lux/1000)/std::log(10)*200;
    return 500+std::log(std::min(lux,100000.0)/10000)/std::log(10)*38.5;
}
inline double referenceSlider(int slider,double gamma) {
    if(slider<0||slider>255||!std::isfinite(gamma)||gamma<1.2||gamma>3.5)throw std::runtime_error("invalid reference slider");
    return referenceBrightness(.222+std::pow(slider/255.0,gamma)*538.315659);
}
inline int referenceRamp(double distance,bool increasing) {
    int ms=distance<.02?0:distance<.05?80:distance<.15?180:distance<.3?300:400;
    return increasing?(ms*45878)/65536:ms;
}
}
