#include "IHyprWindowDecoration.hpp"

// The decoration currently holding the pointer grab, if any. A raw pointer is safe
// here only because the destructor below clears it.
static IHyprWindowDecoration* g_pointerGrab       = nullptr;
static uint32_t               g_pointerGrabButton = 0;

IHyprWindowDecoration::IHyprWindowDecoration(PHLWINDOW pWindow) : m_window(pWindow) {
    ;
}

IHyprWindowDecoration::~IHyprWindowDecoration() {
    ungrabPointer();
}

void IHyprWindowDecoration::grabPointer(uint32_t button) {
    g_pointerGrab       = this;
    g_pointerGrabButton = button;
}

void IHyprWindowDecoration::ungrabPointer() {
    if (g_pointerGrab == this)
        g_pointerGrab = nullptr;
}

bool IHyprWindowDecoration::hasPointerGrab() const {
    return g_pointerGrab == this;
}

uint32_t IHyprWindowDecoration::pointerGrabButton() const {
    return g_pointerGrabButton;
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
