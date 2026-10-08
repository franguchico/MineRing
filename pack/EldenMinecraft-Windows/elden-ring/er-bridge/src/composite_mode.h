#pragma once
#include "bridge_protocol.h"

namespace mb {
inline bool passive_composite_control(const ErmcControl& control) {
    constexpr uint32_t ownership = ERMC_CTRL_OVERRIDE_CAMERA | ERMC_CTRL_MOVE_HUNTER |
        ERMC_CTRL_HIDE_HUNTER | ERMC_CTRL_HOLD_HUNTER | ERMC_CTRL_FREE_FLIGHT;
    constexpr uint32_t required = ERMC_CTRL_PASSIVE_COMPOSITE | ERMC_CTRL_COMPOSITE;
    return (control.flags & required) == required && !(control.flags & ownership) && control.mcFrame != 0;
}
inline bool composite_control_valid(const ErmcControl& control) {
    return !(control.flags & ERMC_CTRL_PASSIVE_COMPOSITE) || passive_composite_control(control);
}
inline uint64_t composite_frame_target(const ErmcControl& control, uint64_t appliedPose) {
    if (!composite_control_valid(control)) return 0;
    return passive_composite_control(control) ? control.mcFrame : appliedPose;
}
}
