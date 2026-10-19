#include <assert.h>
#include <stdio.h>

#include "air_mouse_scroll.h"

int main(void)
{
    air_mouse_scroll_t scroll;
    air_mouse_scroll_reset(&scroll);
    int total = 0;
    // A small movement must produce sub-detent reports instead of waiting
    // for a complete wheel notch. 10 units per settled 10 ms sample.
    for (int i = 0; i < 100; ++i) {
        int step = air_mouse_scroll_update(&scroll, 147, 10, 120);
        assert(step >= 0 && step <= 11);
        if (i == 0) assert(step > 0);
        total += step;
    }
    assert(total > 960 && total < 1010);
    assert(air_mouse_scroll_update(&scroll, 0, 10, 120) == 0);
    assert(air_mouse_scroll_update(&scroll, -147, 10, 120) < 0);
    air_mouse_scroll_reset(&scroll);
    total = 0;
    for (int i = 0; i < 100; ++i) total += air_mouse_scroll_update(&scroll, 147, 10, 1);
    assert(total >= 7 && total <= 8); // Unsupported host retains normal speed.
    assert(air_mouse_scroll_update(&scroll, 147, 10, 120) < 11); // No accumulated jump.
    assert(air_mouse_scroll_update(&scroll, 30000, 10, 120) == 127);
    assert(air_mouse_scroll_update(&scroll, 0, 10, 120) == 0); // No queued tail.
    assert(air_mouse_scroll_update(&scroll, 30000, 200, 120) == 0);
    assert(air_mouse_scroll_update(&scroll, 30000, 0, 120) == 0);
    assert(air_mouse_scroll_update(&scroll, 30000, 10, 1) == 4);
    // Equal wall-clock duration at 10 and 20 ms sampling has similar distance.
    air_mouse_scroll_t fast, slow;
    air_mouse_scroll_reset(&fast);
    air_mouse_scroll_reset(&slow);
    int fast_total = 0, slow_total = 0;
    for (int i = 0; i < 100; ++i) fast_total += air_mouse_scroll_update(&fast, 147, 10, 120);
    for (int i = 0; i < 50; ++i) slow_total += air_mouse_scroll_update(&slow, 147, 20, 120);
    assert(fast_total - slow_total >= -2 && fast_total - slow_total <= 2);
    puts("PASS: fractional scroll, stop, reverse, legacy host, mode switch, saturation, gaps and sample timing");
    return 0;
}
