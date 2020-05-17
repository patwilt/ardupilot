/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * AP_KDECAN.cpp
 *
 *      Author: Francisco Ferreira
 */

#include <AP_HAL/AP_HAL.h>

#if HAL_WITH_UAVCAN

#include <AP_BoardConfig/AP_BoardConfig.h>
#include <AP_CANManager/AP_CANManager.h>

#include <AP_Common/AP_Common.h>

#include <AP_HAL/utility/sparse-endian.h>

#include <SRV_Channel/SRV_Channel.h>
#include <GCS_MAVLink/GCS.h>
#include <AP_Scheduler/AP_Scheduler.h>
#include <AP_Math/AP_Math.h>
#include <AP_Motors/AP_Motors.h>
#include <AP_Logger/AP_Logger.h>

#include "AP_KDECAN.h"
#include <stdio.h>

extern const AP_HAL::HAL& hal;

#define debug_can(level_debug, fmt, args...) do { AP::can().log(level_debug, _driver_index, fmt, ##args); } while (0)

#define DEFAULT_NUM_POLES 14

// table of user settable CAN bus parameters
const AP_Param::GroupInfo AP_KDECAN::var_info[] = {
    // @Param: NPOLE
    // @DisplayName: Number of motor poles
    // @Description: Sets the number of motor poles to calculate the correct RPM value
    AP_GROUPINFO("NPOLE", 1, AP_KDECAN, _num_poles, DEFAULT_NUM_POLES),

    AP_GROUPEND
};

const uint16_t AP_KDECAN::SET_PWM_MIN_INTERVAL_US;

AP_KDECAN::AP_KDECAN()
{
    AP_Param::setup_object_defaults(this, var_info);

    debug_can(AP_CANManager::LOG_INFO, "KDECAN: constructed\n\r");
}

AP_KDECAN *AP_KDECAN::get_kdecan(uint8_t driver_index)
{
    if (driver_index >= AP::can().get_num_drivers() ||
        AP::can().get_driver_type(driver_index) != AP_CANManager::Driver_Type_KDECAN) {
        return nullptr;
    }
    return static_cast<AP_KDECAN*>(AP::can().get_driver(driver_index));
}

bool AP_KDECAN::add_interface(AP_HAL::CANIface* can_iface) {

    if (_can_iface != nullptr) {
        debug_can(AP_CANManager::LOG_ERROR, "AP_KDECAN: Multiple Interface not supported\n\r");
        return false;
    }

    _can_iface = can_iface;

    if (_can_iface == nullptr) {
        debug_can(AP_CANManager::LOG_ERROR, "AP_KDECAN: CAN driver not found\n\r");
        return false;
    }

    if (!_can_iface->is_initialized()) {
        debug_can(AP_CANManager::LOG_ERROR, "AP_KDECAN: Driver not initialized\n\r");
        return false;
    }

    if (!_can_iface->set_event_handle(&_event_handle)) {
        debug_can(AP_CANManager::LOG_ERROR, "AP_KDECAN: Cannot add event handle\n\r");
        return false;
    }
    return true;
}

void AP_KDECAN::init(uint8_t driver_index, bool enable_filters)
{
    _driver_index = driver_index;

    debug_can(AP_CANManager::LOG_INFO, "KDECAN: starting init\n\r");

    if (_initialized) {
        debug_can(AP_CANManager::LOG_ERROR, "KDECAN: already initialized\n\r");
        return;
    }

    if (_can_iface == nullptr) {
        debug_can(AP_CANManager::LOG_ERROR, "KDECAN: Interface not found\n\r");
        return;
    }

    // find available KDE ESCs
    frame_id_t id = { { .object_address = ESC_INFO_OBJ_ADDR,
                      .destination_id = BROADCAST_NODE_ID,
                      .source_id = AUTOPILOT_NODE_ID,
                      .priority = 0,
                      .unused = 0 } };

    AP_HAL::CanFrame frame { (id.value | AP_HAL::CanFrame::FlagEFF), nullptr, 0 };

    if(!_can_iface->send(frame, AP_HAL::micros() + 1000000, 0)) {
        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: couldn't send discovery message\n\r");
        return;
    }

    debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: discovery message sent\n\r");

    uint32_t start = AP_HAL::millis();

    // wait 1 second for answers
    while (AP_HAL::millis() - start < 1000) {
        AP_HAL::CanFrame esc_id_frame {};
        uint64_t rx_time;
        AP_HAL::CANIface::CanIOFlags flags = 0;

        int16_t n = _can_iface->receive(esc_id_frame, rx_time, flags);

        if (n != 1) {
            continue;
        }

        if (!esc_id_frame.isExtended()) {
            continue;
        }

        if (esc_id_frame.dlc != 5) {
            continue;
        }

        id.value = esc_id_frame.id & AP_HAL::CanFrame::MaskExtID;

        if (id.source_id == BROADCAST_NODE_ID ||
            id.source_id >= (KDECAN_MAX_NUM_ESCS + ESC_NODE_ID_FIRST) ||
            id.destination_id != AUTOPILOT_NODE_ID ||
            id.object_address != ESC_INFO_OBJ_ADDR) {
            continue;
        }

        _esc_present_bitmask |= (1 << (id.source_id - ESC_NODE_ID_FIRST));
        _esc_max_node_id = id.source_id - ESC_NODE_ID_FIRST + 1;

        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: found ESC id %u\n\r", id.source_id);
    }

    snprintf(_thread_name, sizeof(_thread_name), "kdecan_%u", driver_index);

    // start thread for receiving and sending CAN frames
    if (!hal.scheduler->thread_create(FUNCTOR_BIND_MEMBER(&AP_KDECAN::loop, void), _thread_name, 4096, AP_HAL::Scheduler::PRIORITY_CAN, 0)) {
        debug_can(AP_CANManager::LOG_ERROR, "KDECAN: couldn't create thread\n\r");
        return;
    }

    _initialized = true;

    debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: init done\n\r");

    return;
}

void AP_KDECAN::loop()
{
    uint64_t timeout;

    uint16_t output_buffer[KDECAN_MAX_NUM_ESCS] {};

    enumeration_state_t enumeration_state = _enumeration_state;
    uint64_t enumeration_start = 0;
    uint8_t enumeration_esc_num = 0;

    const uint32_t LOOP_INTERVAL_US = MIN(AP::scheduler().get_loop_period_us(), SET_PWM_MIN_INTERVAL_US);
    uint64_t pwm_last_sent = 0;
    uint8_t sending_esc_num = 0;

    uint64_t telemetry_last_request = 0;

    while (true) {
        if (!_initialized) {
            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: not initialized\n\r");
            hal.scheduler->delay_microseconds(2000);
            continue;
        }

        uint64_t now = AP_HAL::micros64();
        bool read_select;
        bool write_select;
        bool select_ret;
        // get latest enumeration state set from GCS
        if (_enum_sem.take(1)) {
            enumeration_state = _enumeration_state;
            _enum_sem.give();
        } else {
            debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get enumeration semaphore on loop\n\r");
        }

        if (enumeration_state != ENUMERATION_STOPPED) {
            // check if enumeration timed out
            if (enumeration_start != 0 && now - enumeration_start >= ENUMERATION_TIMEOUT_MS * 1000) {
                enumeration_start = 0;

                WITH_SEMAPHORE(_enum_sem);

                // check if enumeration state didn't change or was set to stop
                if (enumeration_state == _enumeration_state || _enumeration_state == ENUMERATION_STOP) {
                    enumeration_state = _enumeration_state = ENUMERATION_STOPPED;
                }

                continue;
            }

            timeout = now + 1000;

            switch (enumeration_state) {
                case ENUMERATION_START: {

                    // send broadcast frame to start enumeration
                    frame_id_t id = { { .object_address = ENUM_OBJ_ADDR,
                                      .destination_id = BROADCAST_NODE_ID,
                                      .source_id = AUTOPILOT_NODE_ID,
                                      .priority = 0,
                                      .unused = 0 } };
                    be16_t data = htobe16((uint16_t) ENUMERATION_TIMEOUT_MS);
                    AP_HAL::CanFrame frame { (id.value | AP_HAL::CanFrame::FlagEFF), (uint8_t*) &data, sizeof(data) };


                    // wait for write space to be available
                    read_select = false;
                    write_select = true;
                    select_ret = _can_iface->select(read_select, write_select, &frame, timeout);

                    if (select_ret && write_select) {
                        now = AP_HAL::micros64();
                        timeout = now + ENUMERATION_TIMEOUT_MS * 1000;

                        int8_t res = _can_iface->send(frame, timeout, 0);

                        if (res == 1) {
                            enumeration_start = now;
                            enumeration_esc_num = 0;
                            _esc_present_bitmask = 0;
                            _esc_max_node_id = 0;

                            WITH_SEMAPHORE(_enum_sem);

                            if (enumeration_state == _enumeration_state) {
                                enumeration_state = _enumeration_state = ENUMERATION_RUNNING;
                            }
                        } else if (res == 0) {
                            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: strange buffer full when starting ESC enumeration\n\r");
                            break;
                        } else {
                            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: error sending message to start ESC enumeration, result %d\n\r", res);
                            break;
                        }
                    } else {
                        break;
                    }
                    FALLTHROUGH;
                }
                case ENUMERATION_RUNNING: {
                    // wait for enumeration messages from ESCs

                    // wait for write space to be available
                    read_select = true;
                    write_select = false;
                    select_ret = _can_iface->select(read_select, write_select, nullptr, timeout);

                    if (select_ret && read_select) {
                        AP_HAL::CanFrame recv_frame;
                        uint64_t rx_time;
                        AP_HAL::CANIface::CanIOFlags flags {};

                        int16_t res = _can_iface->receive(recv_frame, rx_time, flags);

                        if (res == 1) {
                            if (rx_time < enumeration_start) {
                                // old message
                                break;
                            }

                            frame_id_t id { .value = recv_frame.id & AP_HAL::CanFrame::MaskExtID };

                            if (id.object_address == UPDATE_NODE_ID_OBJ_ADDR) {
                                // reply from setting new node ID
                                _esc_present_bitmask |= 1 << (id.source_id - ESC_NODE_ID_FIRST);
                                _esc_max_node_id = MAX(_esc_max_node_id, id.source_id - ESC_NODE_ID_FIRST + 1);
                                break;
                            } else if (id.object_address != ENUM_OBJ_ADDR) {
                                // discardable frame, only looking for enumeration
                                break;
                            }

                            // try to set node ID for the received ESC
                            while (AP_HAL::micros64() - enumeration_start < ENUMERATION_TIMEOUT_MS * 1000) {
                                // wait for write space to be available
                                id = { { .object_address = UPDATE_NODE_ID_OBJ_ADDR,
                                            .destination_id = uint8_t(enumeration_esc_num + ESC_NODE_ID_FIRST),
                                            .source_id = AUTOPILOT_NODE_ID,
                                            .priority = 0,
                                            .unused = 0 } };
                                AP_HAL::CanFrame send_frame { (id.value | AP_HAL::CanFrame::FlagEFF), (uint8_t*) &recv_frame.data, recv_frame.dlc };
                                read_select = false;
                                write_select = true;
                                select_ret = _can_iface->select(read_select, write_select, &send_frame, timeout);

                                if (select_ret && write_select) {
                                    timeout = enumeration_start + ENUMERATION_TIMEOUT_MS * 1000;

                                    res = _can_iface->send(send_frame, timeout, 0);

                                    if (res == 1) {
                                        enumeration_esc_num++;
                                        break;
                                    } else if (res == 0) {
                                        debug_can(AP_CANManager::LOG_ERROR, "KDECAN: strange buffer full when setting ESC node ID\n\r");
                                    } else {
                                        debug_can(AP_CANManager::LOG_ERROR, "KDECAN: error sending message to set ESC node ID, result %d\n\r", res);
                                    }
                                }
                            }
                        } else if (res == 0) {
                            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: strange failed read when getting ESC enumeration message\n\r");
                        } else {
                            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: error receiving ESC enumeration message, result %d\n\r", res);
                        }
                    }
                    break;
                }
                case ENUMERATION_STOP: {

                    // send broadcast frame to stop enumeration
                    frame_id_t id = { { .object_address = ENUM_OBJ_ADDR,
                                      .destination_id = BROADCAST_NODE_ID,
                                      .source_id = AUTOPILOT_NODE_ID,
                                      .priority = 0,
                                      .unused = 0 } };
                    le16_t data = htole16((uint16_t) ENUMERATION_TIMEOUT_MS);
                    AP_HAL::CanFrame frame { (id.value | AP_HAL::CanFrame::FlagEFF), (uint8_t*) &data, sizeof(data) };


                    // wait for write space to be available
                    read_select = false;
                    write_select = true;
                    select_ret = _can_iface->select(read_select, read_select, &frame, timeout);

                    if (select_ret && write_select) {
                        timeout = enumeration_start + ENUMERATION_TIMEOUT_MS * 1000;

                        int8_t res = _can_iface->send(frame, timeout, 0);

                        if (res == 1) {
                            enumeration_start = 0;

                            WITH_SEMAPHORE(_enum_sem);

                            if (enumeration_state == _enumeration_state) {
                                enumeration_state = _enumeration_state = ENUMERATION_STOPPED;
                            }
                        } else if (res == 0) {
                            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: strange buffer full when stop ESC enumeration\n\r");
                        } else {
                            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: error sending message to stop ESC enumeration, result %d\n\r", res);
                        }
                    }
                    break;
                }
                case ENUMERATION_STOPPED:
                default:
                    debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: something wrong happened, shouldn't be here, enumeration state: %u\n\r", enumeration_state);
                    break;
            }

            continue;
        }

        if (!_esc_present_bitmask) {
            debug_can(AP_CANManager::LOG_ERROR, "KDECAN: no valid ESC present");
            hal.scheduler->delay(1000);
            continue;
        }

        // always look for received frames
        timeout = now + LOOP_INTERVAL_US;

        // check if:
        //   - is currently sending throttle frames, OR
        //   - there are new output values and, a throttle frame was never sent or it's no longer in CAN queue, OR
        //   - it is time to send throttle frames again, regardless of new output values, OR
        //   - it is time to ask for telemetry information
        bool try_write = false;
        if (sending_esc_num > 0 ||
            (_new_output.load(std::memory_order_acquire) && (pwm_last_sent == 0 || now - pwm_last_sent > SET_PWM_TIMEOUT_US)) ||
            (pwm_last_sent != 0 && (now - pwm_last_sent > SET_PWM_MIN_INTERVAL_US)) ||
            (now - telemetry_last_request > TELEMETRY_INTERVAL_US)) {
            // wait for write space or receive frame
            try_write = true;
        } else {  // don't need to send frame, choose the maximum time we'll wait for receiving a frame
            uint64_t next_action = MIN(now + LOOP_INTERVAL_US, telemetry_last_request + TELEMETRY_INTERVAL_US);

            if (pwm_last_sent != 0) {
                next_action = MIN(next_action, pwm_last_sent + SET_PWM_MIN_INTERVAL_US);
            }
            timeout = next_action;
        }

        read_select = true;
        write_select = true;
        // Immediately check if rx buffer not empty
        select_ret = _can_iface->select(read_select, write_select, nullptr, 0);
        if (!read_select && !write_select) {
            //wait for any event
            _event_handle.wait(timeout);
        }
        read_select = true;
        write_select = true;
        select_ret = _can_iface->select(read_select, write_select, nullptr, 0);
        if (select_ret && read_select) {
            AP_HAL::CanFrame frame;
            uint64_t rx_time;
            AP_HAL::CANIface::CanIOFlags flags {};

            int16_t res = _can_iface->receive(frame, rx_time, flags);

            if (res == 1) {
                frame_id_t id { .value = frame.id & AP_HAL::CanFrame::MaskExtID };

                // check if frame is valid: directed at autopilot, doesn't come from broadcast and ESC was detected before
                if (id.destination_id == AUTOPILOT_NODE_ID &&
                    id.source_id != BROADCAST_NODE_ID &&
                    (1 << (id.source_id - ESC_NODE_ID_FIRST) & _esc_present_bitmask)) {
                    switch (id.object_address) {
                        case TELEMETRY_OBJ_ADDR: {
                            if (frame.dlc != 8) {
                                break;
                            }

                            if (!_telem_sem.take(1)) {
                                debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get telemetry semaphore on write\n\r");
                                break;
                            }

                            _telemetry[id.source_id - ESC_NODE_ID_FIRST].time = rx_time;
                            _telemetry[id.source_id - ESC_NODE_ID_FIRST].voltage = frame.data[0] << 8 | frame.data[1];
                            _telemetry[id.source_id - ESC_NODE_ID_FIRST].current = frame.data[2] << 8 | frame.data[3];
                            _telemetry[id.source_id - ESC_NODE_ID_FIRST].rpm = frame.data[4] << 8 | frame.data[5];
                            _telemetry[id.source_id - ESC_NODE_ID_FIRST].temp = frame.data[6];
                            _telemetry[id.source_id - ESC_NODE_ID_FIRST].new_data = true;
                            _telem_sem.give();
                            break;
                        }
                        default:
                            // discard frame
                            break;
                    }
                }
            }
        }

        if (try_write) {
            now = AP_HAL::micros64();

            bool new_output = _new_output.load(std::memory_order_acquire);

            if (sending_esc_num > 0) {
                // currently sending throttle frames, check it didn't timeout
                if (now - pwm_last_sent > SET_PWM_TIMEOUT_US) {
                    debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: timed-out after sending frame to ESC with ID %d\n\r", sending_esc_num - 1);
                    sending_esc_num = 0;
                }
            }

            if (sending_esc_num == 0 && new_output) {
                if (!_rc_out_sem.take(1)) {
                    debug_can(AP_CANManager::LOG_ERROR, "KDECAN: failed to get PWM semaphore on read\n\r");
                    continue;
                }

                memcpy(output_buffer, _scaled_output, KDECAN_MAX_NUM_ESCS * sizeof(uint16_t));

                _rc_out_sem.give();
            }

            // check if:
            //   - is currently sending throttle frames, OR
            //   - there are new output values and, a throttle frame was never sent or it's no longer in CAN queue, OR
            //   - it is time to send throttle frames again, regardless of new output values
            if (sending_esc_num > 0 ||
                (new_output && (pwm_last_sent == 0 || now - pwm_last_sent > SET_PWM_TIMEOUT_US)) ||
                (pwm_last_sent != 0 && (now - pwm_last_sent > SET_PWM_MIN_INTERVAL_US))) {

                for (uint8_t esc_num = sending_esc_num; esc_num < _esc_max_node_id; esc_num++) {

                    if ((_esc_present_bitmask & (1 << esc_num)) == 0) {
                        continue;
                    }

                    be16_t kde_pwm = htobe16(output_buffer[esc_num]);

                    if (hal.util->safety_switch_state() == AP_HAL::Util::SAFETY_DISARMED) {
                        kde_pwm = 0;
                    }

                    frame_id_t id = { { .object_address = SET_PWM_OBJ_ADDR,
                                      .destination_id = uint8_t(esc_num + ESC_NODE_ID_FIRST),
                                      .source_id = AUTOPILOT_NODE_ID,
                                      .priority = 0,
                                      .unused = 0 } };

                    AP_HAL::CanFrame frame { (id.value | AP_HAL::CanFrame::FlagEFF), (uint8_t*) &kde_pwm, sizeof(kde_pwm) };

                    if (esc_num == 0) {
                        timeout = now + SET_PWM_TIMEOUT_US;
                    } else {
                        timeout = pwm_last_sent + SET_PWM_TIMEOUT_US;
                    }
                    
                    write_select = true;
                    read_select = false;
                    //Check if we can transmit this frame
                    bool res = _can_iface->select(read_select, write_select, &frame, 0);
                    if (res && write_select) {
                        res = _can_iface->send(frame, timeout, 0);
                    } else {
                        res = false;
                    }

                    if (res) {
                        if (esc_num == 0) {
                            pwm_last_sent = now;

                            if (new_output) {
                                _new_output.store(false, std::memory_order_release);
                            }
                        }

                        sending_esc_num = (esc_num + 1) % _esc_max_node_id;
                    } else {
                        debug_can(AP_CANManager::LOG_ERROR, "KDECAN: error sending message to ESC with ID %d, result %d\n\r", esc_num + ESC_NODE_ID_FIRST, res);
                    }

                    break;
                }
            } else if (now - telemetry_last_request > TELEMETRY_INTERVAL_US) {
                // broadcast telemetry request frame
                frame_id_t id = { { .object_address = TELEMETRY_OBJ_ADDR,
                                  .destination_id = BROADCAST_NODE_ID,
                                  .source_id = AUTOPILOT_NODE_ID,
                                  .priority = 0,
                                  .unused = 0 } };

                AP_HAL::CanFrame frame { (id.value | AP_HAL::CanFrame::FlagEFF), nullptr, 0 };
                timeout = now + TELEMETRY_TIMEOUT_US;

                write_select = true;
                read_select = false;
                //Check if we can transmit this frame
                bool res = _can_iface->select(read_select, write_select, &frame, 0);
                if (res && write_select) {
                    res = _can_iface->send(frame, timeout, 0);
                } else {
                    res = 0;
                }
 
                if (res == 1) {
                    telemetry_last_request = now;
                } else if (res == 0) {
                    debug_can(AP_CANManager::LOG_ERROR, "KDECAN: strange buffer full when sending message requesting telemetry\n\r");
                } else {
                    debug_can(AP_CANManager::LOG_ERROR, "KDECAN: error sending message requesting telemetry, result %d\n\r", res);
                }
            }
        }
    }
}

void AP_KDECAN::update()
{
    if (_rc_out_sem.take(1)) {
        for (uint8_t i = 0; i < KDECAN_MAX_NUM_ESCS; i++) {
            if ((_esc_present_bitmask & (1 << i)) == 0) {
                continue;
            }

            SRV_Channel::Aux_servo_function_t motor_function = SRV_Channels::get_motor_function(i);

            if (SRV_Channels::function_assigned(motor_function)) {
                float norm_output = SRV_Channels::get_output_norm(motor_function);
                _scaled_output[i] = uint16_t((norm_output + 1.0f) / 2.0f * 2000.0f);
            } else {
                _scaled_output[i] = 0;
            }
        }

        _rc_out_sem.give();
        _new_output.store(true, std::memory_order_release);
    } else {
        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get PWM semaphore on write\n\r");
    }

    AP_Logger *logger = AP_Logger::get_singleton();

    if (logger == nullptr || !logger->should_log(0xFFFFFFFF)) {
        return;
    }

    if (!_telem_sem.take(1)) {
        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get telemetry semaphore on DF read\n\r");
        return;
    }

    telemetry_info_t telem_buffer[KDECAN_MAX_NUM_ESCS] {};

    for (uint8_t i = 0; i < _esc_max_node_id; i++) {
        if (_telemetry[i].new_data) {
            telem_buffer[i] = _telemetry[i];
            _telemetry[i].new_data = false;
        }
    }

    _telem_sem.give();

    uint8_t num_poles = _num_poles > 0 ? _num_poles : DEFAULT_NUM_POLES;

    // log ESC telemetry data
    for (uint8_t i = 0; i < _esc_max_node_id; i++) {
        if (telem_buffer[i].new_data) {
            logger->Write_ESC(i, telem_buffer[i].time,
                          int32_t(telem_buffer[i].rpm * 60UL * 2 / num_poles * 100),
                          telem_buffer[i].voltage,
                          telem_buffer[i].current,
                          int16_t(telem_buffer[i].temp * 100U), 0, 0);
        }
    }
}

bool AP_KDECAN::pre_arm_check(char* reason, uint8_t reason_len)
{
    if (!_enum_sem.take(1)) {
        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get enumeration semaphore on read\n\r");
        snprintf(reason, reason_len ,"Enumeration state unknown");
        return false;
    }

    if (_enumeration_state != ENUMERATION_STOPPED) {
        snprintf(reason, reason_len ,"Enumeration running");
        _enum_sem.give();
        return false;
    }

    _enum_sem.give();

    uint16_t motors_mask = 0;
    AP_Motors *motors = AP_Motors::get_singleton();

    if (motors) {
        motors_mask = motors->get_motor_mask();
    }

    uint8_t num_expected_motors = __builtin_popcount(motors_mask);
    uint8_t num_present_escs = __builtin_popcount(_esc_present_bitmask);

    if (num_present_escs < num_expected_motors) {
        snprintf(reason, reason_len, "Too few ESCs detected");
        return false;
    }

    if (num_present_escs > num_expected_motors) {
        snprintf(reason, reason_len, "Too many ESCs detected");
        return false;
    }

    if (_esc_max_node_id != num_expected_motors) {
        snprintf(reason, reason_len, "Wrong node IDs, run enumeration");
        return false;
    }

    return true;
}

void AP_KDECAN::send_mavlink(uint8_t chan)
{
    if (!_telem_sem.take(1)) {
        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get telemetry semaphore on MAVLink read\n\r");
        return;
    }

    telemetry_info_t telem_buffer[KDECAN_MAX_NUM_ESCS];
    memcpy(telem_buffer, _telemetry, sizeof(telemetry_info_t) * KDECAN_MAX_NUM_ESCS);
    _telem_sem.give();

    uint16_t voltage[4] {};
    uint16_t current[4] {};
    uint16_t rpm[4] {};
    uint8_t temperature[4] {};
    uint16_t totalcurrent[4] {};
    uint16_t count[4] {};
    uint8_t num_poles = _num_poles > 0 ? _num_poles : DEFAULT_NUM_POLES;
    uint64_t now = AP_HAL::micros64();

    for (uint8_t i = 0; i < _esc_max_node_id && i < 8; i++) {
        uint8_t idx = i % 4;
        if (telem_buffer[i].time && (now - telem_buffer[i].time < 1000000)) {
            voltage[idx]      = telem_buffer[i].voltage;
            current[idx]      = telem_buffer[i].current;
            rpm[idx]          = uint16_t(telem_buffer[i].rpm  * 60UL * 2 / num_poles);
            temperature[idx]  = telem_buffer[i].temp;
        } else {
            voltage[idx] = 0;
            current[idx] = 0;
            rpm[idx] = 0;
            temperature[idx] = 0;
        }

        if (idx == 3 || i == _esc_max_node_id - 1) {
            if (!HAVE_PAYLOAD_SPACE((mavlink_channel_t)chan, ESC_TELEMETRY_1_TO_4)) {
                return;
            }

            if (i < 4) {
                mavlink_msg_esc_telemetry_1_to_4_send((mavlink_channel_t)chan, temperature, voltage, current, totalcurrent, rpm, count);
            } else {
                mavlink_msg_esc_telemetry_5_to_8_send((mavlink_channel_t)chan, temperature, voltage, current, totalcurrent, rpm, count);
            }
        }
    }
}

bool AP_KDECAN::run_enumeration(bool start_stop)
{
    if (!_enum_sem.take(1)) {
        debug_can(AP_CANManager::LOG_DEBUG, "KDECAN: failed to get enumeration semaphore on write\n\r");
        return false;
    }

    if (start_stop) {
        _enumeration_state = ENUMERATION_START;
    } else if (_enumeration_state != ENUMERATION_STOPPED) {
        _enumeration_state = ENUMERATION_STOP;
    }

    _enum_sem.give();

    return true;
}

#endif // HAL_WITH_UAVCAN
