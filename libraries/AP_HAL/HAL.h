#pragma once

class AP_Param;

#include "AP_HAL_Namespace.h"

#include "AnalogIn.h"
#include "GPIO.h"
#include "RCInput.h"
#include "RCOutput.h"
#include "SPIDevice.h"
#include "Storage.h"
#include "UARTDriver.h"
#include "system.h"
#include "OpticalFlow.h"
#include "DSP.h"
#include "CANIface.h"


class AP_HAL::HAL {
public:
    HAL(AP_HAL::UARTDriver* _uartA, // console
        AP_HAL::UARTDriver* _uartB, // 1st GPS
        AP_HAL::UARTDriver* _uartC, // telem1
        AP_HAL::UARTDriver* _uartD, // telem2
        AP_HAL::UARTDriver* _uartE, // 2nd GPS
        AP_HAL::UARTDriver* _uartF, // extra1
        AP_HAL::UARTDriver* _uartG, // extra2
        AP_HAL::UARTDriver* _uartH, // extra3
        AP_HAL::I2CDeviceManager* _i2c_mgr,
        AP_HAL::SPIDeviceManager* _spi,
        AP_HAL::AnalogIn*   _analogin,
        AP_HAL::Storage*    _storage,
        AP_HAL::UARTDriver* _console,
        AP_HAL::GPIO*       _gpio,
        AP_HAL::RCInput*    _rcin,
        AP_HAL::RCOutput*   _rcout,
        AP_HAL::Scheduler*  _scheduler,
        AP_HAL::Util*       _util,
        AP_HAL::OpticalFlow*_opticalflow,
        AP_HAL::Flash*      _flash,
        AP_HAL::DSP*        _dsp,
#if MAX_NUMBER_OF_CAN_INTERFACES > 0
        AP_HAL::CANIface* _can_ifaces[MAX_NUMBER_OF_CAN_INTERFACES])
#else
        AP_HAL::CANIface** _can_ifaces)
#endif
        :
        uartA(_uartA),
        uartB(_uartB),
        uartC(_uartC),
        uartD(_uartD),
        uartE(_uartE),
        uartF(_uartF),
        uartG(_uartG),
        uartH(_uartH),
        i2c_mgr(_i2c_mgr),
        spi(_spi),
        analogin(_analogin),
        storage(_storage),
        console(_console),
        gpio(_gpio),
        rcin(_rcin),
        rcout(_rcout),
        scheduler(_scheduler),
        util(_util),
        opticalflow(_opticalflow),
        flash(_flash),
        dsp(_dsp)
    {
#if MAX_NUMBER_OF_CAN_INTERFACES > 0
        if (_can_ifaces == nullptr) {
            for (uint8_t i = 0; i < MAX_NUMBER_OF_CAN_INTERFACES; i++)
                can[i] = nullptr;
        } else {
            for (uint8_t i = 0; i < MAX_NUMBER_OF_CAN_INTERFACES; i++)
                can[i] = _can_ifaces[i];
        }
#endif

        AP_HAL::init();
    }

    struct Callbacks {
        virtual void setup() = 0;
        virtual void loop() = 0;
    };

    struct FunCallbacks : public Callbacks {
        FunCallbacks(void (*setup_fun)(void), void (*loop_fun)(void));

        void setup() override { _setup(); }
        void loop() override { _loop(); }

    private:
        void (*_setup)(void);
        void (*_loop)(void);
    };

    virtual void run(int argc, char * const argv[], Callbacks* callbacks) const = 0;

    AP_HAL::UARTDriver* uartA;
    AP_HAL::UARTDriver* uartB;
    AP_HAL::UARTDriver* uartC;
    AP_HAL::UARTDriver* uartD;
    AP_HAL::UARTDriver* uartE;
    AP_HAL::UARTDriver* uartF;
    AP_HAL::UARTDriver* uartG;
    AP_HAL::UARTDriver* uartH;
    AP_HAL::I2CDeviceManager* i2c_mgr;
    AP_HAL::SPIDeviceManager* spi;
    AP_HAL::AnalogIn*   analogin;
    AP_HAL::Storage*    storage;
    AP_HAL::UARTDriver* console;
    AP_HAL::GPIO*       gpio;
    AP_HAL::RCInput*    rcin;
    AP_HAL::RCOutput*   rcout;
    AP_HAL::Scheduler*  scheduler;
    AP_HAL::Util        *util;
    AP_HAL::OpticalFlow *opticalflow;
    AP_HAL::Flash       *flash;
    AP_HAL::DSP         *dsp;
#if MAX_NUMBER_OF_CAN_INTERFACES > 0
    AP_HAL::CANIface* can[MAX_NUMBER_OF_CAN_INTERFACES];
#else
    AP_HAL::CANIface** can;
#endif
};
