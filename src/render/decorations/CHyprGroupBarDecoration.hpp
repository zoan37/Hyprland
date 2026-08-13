#pragma once

#include "IHyprWindowDecoration.hpp"
#include "../../devices/IPointer.hpp"
#include <vector>
#include "../Texture.hpp"
#include <string>
#include "../../helpers/memory/Memory.hpp"

class CTitleTex {
  public:
    CTitleTex(PHLWINDOW pWindow, const Vector2D& bufferSize, const float monitorScale);
    ~CTitleTex() = default;

    SP<Render::ITexture> m_texActive;
    SP<Render::ITexture> m_texInactive;
    SP<Render::ITexture> m_texLockedActive;
    SP<Render::ITexture> m_texLockedInactive;
    std::string          m_content;

    PHLWINDOWREF         m_windowOwner;
};

void refreshGroupBarGradients();

class CHyprGroupBarDecoration : public IHyprWindowDecoration {
  public:
    CHyprGroupBarDecoration(PHLWINDOW);
    virtual ~CHyprGroupBarDecoration() = default;

    virtual SDecorationPositioningInfo getPositioningInfo();

    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);

    virtual void                       draw(PHLMONITOR, float const& a);

    virtual eDecorationType            getDecorationType();

    virtual void                       updateWindow(PHLWINDOW);

    virtual void                       damageEntire();

    virtual bool                       onInputOnDeco(const eInputType, const Vector2D&, std::any = {});

    virtual eDecorationLayer           getDecorationLayer();

    virtual uint64_t                   getDecorationFlags();

    virtual std::string                getDisplayName();

    // A tab being dragged is drawn under the cursor rather than in its slot, so it
    // slides continuously instead of jumping a slot at a time. Fills `outAlong` with
    // its position along the bar, measured from the bar's origin, and returns false
    // for every tab that is not the one being dragged.
    static bool draggedTabAlong(PHLWINDOW w, double barLen, double tabLen, double& outAlong);

  private:
    // Modifier-free tab dragging, in the spirit of general:resize_on_border: press a
    // tab and drag it along the bar to reorder the group, no keybind involved. The
    // gesture takes the decoration pointer grab on arming, so motion and the release
    // reach it wherever the cursor goes.
    //
    // The state is static rather than per-instance because the decoration drawing the
    // bar changes mid-gesture: reordering moves the group's current window, and the
    // bar is drawn by whichever decoration belongs to it. The bar geometry is
    // snapshotted on press, which is valid for the whole gesture since reordering
    // permutes tabs within the bar but never moves the bar itself.
    //
    // armed  : a press on a tab is being tracked
    // active : the pointer has since moved past the threshold, so it is a drag and
    //          not a click
    static bool               tabDragArmed();
    static bool               tabDragActive();
    static void               updateTabDrag(const Vector2D& pos);
    static void               endTabDrag();

    CBox                      m_assignedBox = {0};

    PHLWINDOWREF              m_window;

    std::vector<PHLWINDOWREF> m_dwGroupMembers;

    float                     m_barWidth;
    float                     m_barHeight;

    std::optional<bool>       m_bLastVisibilityStatus;

    CTitleTex*                textureFromTitle(const std::string&);
    void                      invalidateTextures();

    CBox                      assignedBoxGlobal();
    bool                      visible();

    void                      armTabDrag(const Vector2D& pos, PHLWINDOW dragged);

    bool                      onBeginWindowDragOnDeco(const Vector2D&);
    bool                      onEndWindowDragOnDeco(const Vector2D&, PHLWINDOW);
    bool                      onMouseButtonOnDeco(const Vector2D&, const IPointer::SButtonEvent&);
    bool                      onScrollOnDeco(const Vector2D&, const IPointer::SAxisEvent);

    struct STitleTexs {
        // STitleTexs*                            overriden = nullptr; // TODO: make shit shared in-group to decrease VRAM usage.
        std::vector<UP<CTitleTex>> titleTexs;
    } m_titleTexs;
};
