// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string>

namespace brightness {
// Read-only public Xiaomi UAPI: flag, display id, driver level, framework clone.
// https://github.com/MiCode/vendor_opensource_display-drivers/blob/popsicle-w-oss/include/uapi/display/drm/mi_disp.h
inline std::string readMainPanel() {
    struct Request {uint32_t flag, display, brightness, clone;};
    static_assert(sizeof(Request)==16);
    constexpr auto getBrightness=_IOR('D',0x0B,Request);
    static_assert(_IOC_DIR(getBrightness)==_IOC_READ);
    static_assert(_IOC_NR(getBrightness)==0x0B); // Never issue SET (0x0C).
    Request request{0,0,UINT32_MAX,UINT32_MAX};
    int fd=open("/dev/mi_display/disp_feature",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return "{\"ok\":false,\"error\":\"panel readback unavailable\"}";
    int result=ioctl(fd,getBrightness,&request);close(fd);
    if(result<0||request.brightness>1000000||request.clone>1000000)
        return "{\"ok\":false,\"error\":\"panel readback failed\"}";
    return "{\"ok\":true,\"driver_brightness\":"+std::to_string(request.brightness)+",\"brightness_clone\":"+std::to_string(request.clone)+"}";
}
}
