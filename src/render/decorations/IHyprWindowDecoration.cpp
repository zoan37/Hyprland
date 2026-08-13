#include "IHyprWindowDecoration.hpp"

// The decoration currently holding the pointer grab, if any. A raw pointer is safe
// here only because the destructor below clears it.
static IHyprWindowDecoration* g_pointerGrab = nullptr;

IHyprWindowDecoration::IHyprWindowDecoration(PHLWINDOW pWindow) : m_window(pWindow) {
    ;
}

IHyprWindowDecoration::~IHyprWindowDecoration() {
    ungrabPointer();
}

void IHyprWindowDecoration::grabPointer() {
    g_pointerGrab = this;
}

void IHyprWindowDecoration::ungrabPointer() {
    if (g_pointerGrab == this)
        g_pointerGrab = nullptr;
}

bool IHyprWindowDecoration::hasPointerGrab() const {
    return g_pointerGrab == this;
}

IHyprWindowDecoration* IHyprWindowDecoration::pointerGrab() {
    return g_pointerGrab;
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
