#include <LovyanGFX.hpp>

class LGFX_Qualia : public lgfx::LGFX_Device {
    lgfx::Panel_RGB panel;
    lgfx::Bus_RGB bus;

    public:
        LGFX_Qualia(void) {
            // Rectangle Bar RGB TTL TFT Display - 4.58" 320x960 No Touchscreen - HD458002C40
            auto b = bus.config();
            
        }
}