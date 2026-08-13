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

    // Modifier-free tab dragging, in the spirit of general:resize_on_border: press
    // a tab and drag it along the bar to reorder the group, no keybind involved.
    //
    // The state is static because the pointer is a singleton — at most one tab can
    // be dragged at a time — and the bar geometry is snapshotted when the press
    // happens, so nothing here has to hold a pointer to a decoration that may be
    // destroyed mid-gesture (a window in the group closing would do it).
    //
    // armed  : a press on a tab is being tracked
    // active : the pointer has since moved past the threshold, so it is a drag and
    //          not a click
    static bool tabDragArmed();
    static bool tabDragActive();
    static void updateTabDrag(const Vector2D& pos);
    static void endTabDrag();

    // While a tab is being dragged it is drawn under the cursor rather than in its
    // slot, so it slides continuously instead of jumping a slot at a time. Fills
    // `outAlong` with its position along the bar, measured from the bar's origin,
    // and returns false for every tab that is not the one being dragged.
    static bool draggedTabAlong(PHLWINDOW w, double barLen, double tabLen, double& outAlong);

  private:
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
