// Minimal wlr-virtual-pointer client, used to drive a real pointer through a
// headless Hyprland so groupbar tab dragging can be tested end to end.
//
//   vpointer <extentW> <extentH> drag <x1> <y1> <x2> <y2> <steps> <stepDelayMs>
//   vpointer <extentW> <extentH> move <x> <y>
//
// Events go through the compositor's normal pointer pipeline, which is the point:
// a plain left press has to reach CInputManager::processMouseDownNormal exactly as
// a physical mouse would.

#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111

static uint32_t g_button = BTN_LEFT;

static struct zwlr_virtual_pointer_manager_v1* g_mgr = NULL;
static struct zwlr_virtual_pointer_v1*         g_ptr = NULL;

static uint32_t nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void sleepMs(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void handleGlobal(void* data, struct wl_registry* reg, uint32_t name, const char* iface, uint32_t ver) {
    if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0)
        g_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, ver < 2 ? ver : 2);
}

static void handleGlobalRemove(void* data, struct wl_registry* reg, uint32_t name) {}

static const struct wl_registry_listener REGISTRY_LISTENER = {handleGlobal, handleGlobalRemove};

static void moveTo(struct wl_display* dpy, uint32_t x, uint32_t y, uint32_t ew, uint32_t eh) {
    zwlr_virtual_pointer_v1_motion_absolute(g_ptr, nowMs(), x, y, ew, eh);
    zwlr_virtual_pointer_v1_frame(g_ptr);
    wl_display_flush(dpy);
}

static void button(struct wl_display* dpy, int pressed) {
    zwlr_virtual_pointer_v1_button(g_ptr, nowMs(), g_button, pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(g_ptr);
    wl_display_flush(dpy);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: vpointer <extentW> <extentH> move <x> <y>\n"
                        "       vpointer <extentW> <extentH> drag <x1> <y1> <x2> <y2> <steps> <stepDelayMs>\n");
        return 2;
    }

    const uint32_t EW  = (uint32_t)atoi(argv[1]);
    const uint32_t EH  = (uint32_t)atoi(argv[2]);
    const char*    CMD = argv[3];

    // optional trailing "right" switches the button, so the movewindow bind can be
    // exercised without needing a virtual keyboard to hold a modifier
    for (int i = 4; i < argc; ++i)
        if (strcmp(argv[i], "right") == 0)
            g_button = BTN_RIGHT;

    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "vpointer: cannot connect to WAYLAND_DISPLAY\n");
        return 1;
    }

    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &REGISTRY_LISTENER, NULL);
    wl_display_roundtrip(dpy);

    if (!g_mgr) {
        fprintf(stderr, "vpointer: compositor does not offer zwlr_virtual_pointer_manager_v1\n");
        return 1;
    }

    g_ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(g_mgr, NULL);
    wl_display_roundtrip(dpy);

    // The compositor registers the new device on its own event loop tick and only
    // then connects the listeners that turn these requests into input. Sending
    // immediately after the roundtrip races that, and the events are dropped.
    sleepMs(600);

    if (strcmp(CMD, "move") == 0 && argc >= 6) {
        moveTo(dpy, (uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]), EW, EH);
        wl_display_roundtrip(dpy);
    } else if (strcmp(CMD, "drag") == 0 && argc >= 10) {
        const int X1 = atoi(argv[4]), Y1 = atoi(argv[5]);
        const int X2 = atoi(argv[6]), Y2 = atoi(argv[7]);
        const int STEPS = atoi(argv[8]) < 1 ? 1 : atoi(argv[8]);
        const long DELAY = atol(argv[9]);

        // settle on the press point before pressing, so the press lands where we think
        moveTo(dpy, (uint32_t)X1, (uint32_t)Y1, EW, EH);
        sleepMs(120);

        button(dpy, 1);
        sleepMs(120);

        for (int i = 1; i <= STEPS; ++i) {
            const int X = X1 + (X2 - X1) * i / STEPS;
            const int Y = Y1 + (Y2 - Y1) * i / STEPS;
            moveTo(dpy, (uint32_t)X, (uint32_t)Y, EW, EH);
            sleepMs(DELAY);
        }

        sleepMs(120);
        button(dpy, 0);
        wl_display_roundtrip(dpy);
        sleepMs(120);
    } else if (strcmp(CMD, "drag2") == 0 && argc >= 12) {
        // Two-segment drag: press at 1, travel to 2, then to 3, release. Needed to
        // leave the bar before moving along it, which a single interpolated segment
        // cannot express.
        const int PX[3] = {atoi(argv[4]), atoi(argv[6]), atoi(argv[8])};
        const int PY[3] = {atoi(argv[5]), atoi(argv[7]), atoi(argv[9])};
        const int STEPS = atoi(argv[10]) < 1 ? 1 : atoi(argv[10]);
        const long DELAY = atol(argv[11]);

        moveTo(dpy, (uint32_t)PX[0], (uint32_t)PY[0], EW, EH);
        sleepMs(120);
        button(dpy, 1);
        sleepMs(120);

        for (int seg = 0; seg < 2; ++seg) {
            for (int i = 1; i <= STEPS; ++i) {
                const int X = PX[seg] + (PX[seg + 1] - PX[seg]) * i / STEPS;
                const int Y = PY[seg] + (PY[seg + 1] - PY[seg]) * i / STEPS;
                moveTo(dpy, (uint32_t)X, (uint32_t)Y, EW, EH);
                sleepMs(DELAY);
            }
        }

        sleepMs(120);
        button(dpy, 0);
        wl_display_roundtrip(dpy);
        sleepMs(120);
    } else {
        fprintf(stderr, "vpointer: bad arguments\n");
        return 2;
    }

    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
