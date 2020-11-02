#pragma once

#include <AP_Logger/LogStructure.h>

#include <AP_VisualOdom/AP_VisualOdom.h>

#include <AP_Vehicle/AP_Vehicle_Type.h>

class AP_DAL_VisualOdom {
public:

    // return VisualOdom health
    bool healthy() const {
        return RVOH.healthy;
    }

    bool enabled() const {
        return RVOH.enabled;
    }

    bool get_delay_ms() const {
        return RVOH.delay_ms;
    }

    // return a 3D vector defining the position offset of the camera in meters relative to the body frame origin
    const Vector3f &get_pos_offset() const {
        return RVOH.pos_offset;
    }

    // AP_DAL methods:
    AP_DAL_VisualOdom();

    AP_DAL_VisualOdom *visualodom() {
        if (RVOH.ptr_is_nullptr) {
            return nullptr;
        }
        return this;
    }

    void start_frame();

#if APM_BUILD_TYPE(APM_BUILD_Replay)
    void handle_message(const log_RVOH &msg) {
        RVOH = msg;
   }
#endif

private:

    struct log_RVOH RVOH;
};
