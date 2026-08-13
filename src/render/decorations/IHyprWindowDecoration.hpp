#pragma once

#include <any>
#include "../../defines.hpp"
#include "../../helpers/math/Math.hpp"
#include "DecorationPositioner.hpp"

enum eDecorationType : int8_t {
    DECORATION_NONE = -1,
    DECORATION_GROUPBAR,
    DECORATION_SHADOW,
    DECORATION_INNER_GLOW,
    DECORATION_BORDER,
    DECORATION_CUSTOM
};

enum eDecorationLayer : uint8_t {
    DECORATION_LAYER_BOTTOM = 0, /* lowest. */
    DECORATION_LAYER_UNDER,      /* under the window, but above BOTTOM */
    DECORATION_LAYER_OVER,       /* above the window, but below its popups */
    DECORATION_LAYER_OVERLAY     /* above everything of the window, including popups */
};

enum eDecorationFlags : uint8_t {
    DECORATION_ALLOWS_MOUSE_INPUT  = 1 << 0, /* this decoration accepts mouse input */
    DECORATION_PART_OF_MAIN_WINDOW = 1 << 1, /* this decoration is a *seamless* part of the main window, so stuff like shadows will include it */
    DECORATION_NON_SOLID           = 1 << 2, /* this decoration is not solid. Other decorations should draw on top of it. Example: shadow */
};

class CDecorationPositioner;
class IPointer;

class IHyprWindowDecoration {
  public:
    IHyprWindowDecoration(PHLWINDOW);
    virtual ~IHyprWindowDecoration();

    virtual SDecorationPositioningInfo getPositioningInfo() = 0;

    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply) = 0;

    virtual void                       draw(PHLMONITOR, float const& a) = 0;

    virtual eDecorationType            getDecorationType() = 0;

    virtual void                       updateWindow(PHLWINDOW) = 0;

    virtual void                       damageEntire() = 0; // should be ignored by non-absolute decos

    virtual bool                       onInputOnDeco(const eInputType, const Vector2D&, std::any = {});

    virtual eDecorationLayer           getDecorationLayer();

    virtual uint64_t                   getDecorationFlags();

    virtual std::string                getDisplayName();

    // Pointer grab.
    //
    // checkInputOnDecos only delivers to decorations whose box contains the cursor,
    // which is right for clicks and wrong for gestures: a drag that begins on a
    // decoration has to keep receiving motion after the pointer has left it, and has
    // to see the release wherever it happens. A decoration that claims a press takes
    // the grab, and from then until it releases it receives INPUT_TYPE_MOTION and the
    // matching button release regardless of where the pointer is.
    //
    // Only one grab exists at a time, because the pointer is a singleton. The grab is
    // dropped automatically if the holding decoration is destroyed, which a window
    // closing mid-gesture would otherwise turn into a dangling pointer.
    // The device is bound separately: a button event carries no pointer, so the input
    // manager attaches it once the decoration has taken the grab.
    void grabPointer(uint32_t button);
    void bindPointerGrabDevice(const WP<IPointer>& pointer);
    void ungrabPointer();
    bool hasPointerGrab() const;

    // True when an event from this device and button belongs to the current grab.
    static uint64_t pointerGrabGeneration();
    static bool     pointerGrabWants(uint32_t button, const SP<IPointer>& from);
    static bool     pointerGrabHeldBy(const SP<IPointer>& pointer);

    // Called when the grab is taken away rather than ended by a release — the
    // compositor force-releases held buttons on things like a workspace change, and
    // a gesture that kept running after that would be acting on a button nobody is
    // holding.
    virtual void                  onPointerGrabCancelled();

    static IHyprWindowDecoration* pointerGrab();
    static void                   cancelPointerGrab();

  private:
    PHLWINDOWREF m_window;

    friend class CDecorationPositioner;
};
