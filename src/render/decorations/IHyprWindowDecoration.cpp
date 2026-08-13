#include "IHyprWindowDecoration.hpp"
#include "../../devices/IPointer.hpp"

// The decoration currently holding the pointer grab, if any. A raw pointer is safe
// here only because the destructor below clears it.
static IHyprWindowDecoration* g_pointerGrab       = nullptr;
static uint32_t               g_pointerGrabButton = 0;
static WP<IPointer>           g_pointerGrabDevice;

// Bumped by every acquisition. The input manager binds the device by watching this
// rather than the holder address: the same decoration re-arming is a new grab, with
// its device reset, and an address comparison cannot see that.
static uint64_t g_pointerGrabGeneration = 0;

IHyprWindowDecoration::IHyprWindowDecoration(PHLWINDOW pWindow) : m_window(pWindow) {
    ;
}

IHyprWindowDecoration::~IHyprWindowDecoration() {
    ungrabPointer();
}

void IHyprWindowDecoration::grabPointer(uint32_t button) {
    // A second grab must not silently strand the first: its owner would keep gesture
    // state with nothing able to end it, having lost the grab it ends through. This
    // includes re-arming on the decoration already holding it — that is a new
    // gesture, and the old one still has to repaint where it left its tab.
    if (g_pointerGrab)
        cancelPointerGrab();

    g_pointerGrab       = this;
    g_pointerGrabButton = button;
    g_pointerGrabDevice.reset();
    ++g_pointerGrabGeneration;
}

uint64_t IHyprWindowDecoration::pointerGrabGeneration() {
    return g_pointerGrabGeneration;
}

void IHyprWindowDecoration::bindPointerGrabDevice(const WP<IPointer>& pointer) {
    if (g_pointerGrab == this)
        g_pointerGrabDevice = pointer;
}

void IHyprWindowDecoration::ungrabPointer() {
    if (g_pointerGrab != this)
        return;

    g_pointerGrab = nullptr;
    g_pointerGrabDevice.reset();
}

bool IHyprWindowDecoration::hasPointerGrab() const {
    return g_pointerGrab == this;
}

bool IHyprWindowDecoration::pointerGrabWants(uint32_t button, const SP<IPointer>& from) {
    if (!g_pointerGrab || button != g_pointerGrabButton)
        return false;

    // Unbound means the device was never attached, which only happens if the grab was
    // taken outside the normal press path; accept it rather than stranding it.
    return g_pointerGrabDevice.expired() || g_pointerGrabDevice.lock() == from;
}

bool IHyprWindowDecoration::pointerGrabHeldBy(const SP<IPointer>& pointer) {
    return g_pointerGrab && !g_pointerGrabDevice.expired() && g_pointerGrabDevice.lock() == pointer;
}

void IHyprWindowDecoration::onPointerGrabCancelled() {
    ;
}

IHyprWindowDecoration* IHyprWindowDecoration::pointerGrab() {
    return g_pointerGrab;
}

void IHyprWindowDecoration::cancelPointerGrab() {
    if (!g_pointerGrab)
        return;

    const auto GRAB = g_pointerGrab;
    GRAB->ungrabPointer();
    GRAB->onPointerGrabCancelled();
}

bool IHyprWindowDecoration::onInputOnDeco(const eInputType, const Vector2D&, std::any) {
    return false;
}

eDecorationLayer IHyprWindowDecoration::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t IHyprWindowDecoration::getDecorationFlags() {
    return 0;
}

std::string IHyprWindowDecoration::getDisplayName() {
    return "Unknown Decoration";
}
