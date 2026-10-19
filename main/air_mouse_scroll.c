#include "air_mouse_scroll.h"

#include <math.h>
#include <string.h>

void air_mouse_scroll_reset(air_mouse_scroll_t *scroll)
{
    memset(scroll, 0, sizeof(*scroll));
}

int8_t air_mouse_scroll_update(air_mouse_scroll_t *scroll, int32_t rate,
                              uint32_t elapsed_ms, uint16_t resolution)
{
    if (resolution != AIR_MOUSE_WHEEL_RESOLUTION) resolution = 1;
    if (scroll->resolution != resolution || elapsed_ms > 100) {
        air_mouse_scroll_reset(scroll);
        scroll->resolution = resolution;
        if (elapsed_ms > 100) return 0;
    }
    if (elapsed_ms == 0) return 0;
    if (rate == 0) {
        scroll->velocity = 0; // Stop immediately: no scroll after the hand stops.
        return 0;
    }
    if (scroll->velocity * rate < 0 || scroll->remainder * rate < 0) {
        scroll->velocity = 0;
        scroll->remainder = 0; // Reversal must not fight the preceding gesture.
    }
    // Preserve the existing 1760 LSB / detent at 10 ms baseline speed.
    // Smooth velocity over 25 ms; retain fractions below one HID unit.
    float blend = elapsed_ms / (25.0f + elapsed_ms);
    scroll->velocity += blend * (rate / 1760.0f - scroll->velocity);
    scroll->remainder += scroll->velocity * elapsed_ms / 10.0f * resolution;
    float whole = truncf(scroll->remainder);
    scroll->remainder -= whole;
    int limit = resolution == 1 ? 12 : 127;
    // Discard excess speed rather than queueing delayed scroll after release.
    if (whole > limit) whole = limit;
    if (whole < -limit) whole = -limit;
    return (int8_t)whole;
}
