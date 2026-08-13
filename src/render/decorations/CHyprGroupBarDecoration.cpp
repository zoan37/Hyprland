#include "CHyprGroupBarDecoration.hpp"
#include "../../Compositor.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../desktop/state/WindowState.hpp"
#include "../../desktop/view/Group.hpp"
#include <algorithm>
#include <ranges>
#include <pango/pangocairo.h>
#include "../pass/TexPassElement.hpp"
#include "../pass/RectPassElement.hpp"
#include "../Renderer.hpp"
#include "../../managers/input/InputManager.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"
#include "../../layout/LayoutManager.hpp"
#include "../../layout/supplementary/DragController.hpp"

using namespace Render;

// shared things to conserve VRAM
static SP<ITexture> m_tGradientActive;
static SP<ITexture> m_tGradientInactive;
static SP<ITexture> m_tGradientLockedActive;
static SP<ITexture> m_tGradientLockedInactive;

constexpr int       BAR_TEXT_PAD = 2;

// linux/input-event-codes.h BTN_LEFT; the middle-click path above uses 274 (BTN_MIDDLE) the same way
constexpr uint32_t BTN_LEFT_CODE = 272;

// --- modifier-free tab dragging ------------------------------------------------
// See the comment on the static members in the header.
//
// Only the gesture itself is kept here. Bar geometry is read live from the
// decoration each time, never snapshotted: a member closing or joining, or the
// tile being resized by something else entirely, changes the tab dimensions
// mid-drag, and a stale snapshot would map the pointer to the wrong slot.

struct STabDragState {
    PHLWINDOWREF window;         // the tab being dragged
    Vector2D     pressPos;       // where the press landed, for the threshold
    Vector2D     pointer;        // latest pointer position, for drawing the tab under it
    double       grabOffset = 0; // where inside the tab the press landed, so it does not jump
    bool         armed      = false;
    bool         active     = false;
};

static STabDragState g_tabDrag;

// Enough to not reorder on the jitter of an ordinary click, small enough that a
// deliberate drag feels immediate.
static constexpr double TAB_DRAG_THRESHOLD = 4.0;

// How far off the bar, as a multiple of its thickness, the pointer may stray and
// still reorder. Without this, pressing a tab and moving down into the window would
// keep reordering on the horizontal component alone, which is a surprising way to
// lose your tab order. Straying past the band only pauses the gesture — coming back
// resumes it, and nothing is reverted.
static constexpr double TAB_DRAG_BAND = 3.0;

bool                    CHyprGroupBarDecoration::tabDragArmed() {
    return g_tabDrag.armed && !g_tabDrag.window.expired();
}

bool CHyprGroupBarDecoration::tabDragActive() {
    return tabDragArmed() && g_tabDrag.active;
}

void CHyprGroupBarDecoration::endTabDrag() {
    // Release only a grab a groupbar holds: this is static state shared by every
    // groupbar, and some other decoration's gesture is not ours to cancel.
    if (const auto GRAB = pointerGrab(); GRAB && GRAB->getDecorationType() == DECORATION_GROUPBAR) {
        // The tab snaps back into its slot here, so the bar has to be repainted. If
        // the pointer sat still for the last frame nothing else would schedule it and
        // the tab would stay drawn where it was dropped.
        if (g_tabDrag.active)
            GRAB->damageEntire();

        GRAB->ungrabPointer();
    }

    g_tabDrag = STabDragState{};
}

CHyprGroupBarDecoration::~CHyprGroupBarDecoration() {
    // The base destructor drops the grab, but the gesture state is shared and static,
    // so it would stay armed with nothing able to end it: the release can no longer
    // reach us, and the dragged window — still alive, merely moved out of the group —
    // would render at stale drag coordinates if it were grouped again.
    if (hasPointerGrab())
        endTabDrag();
}

void CHyprGroupBarDecoration::onPointerGrabCancelled() {
    // cancelPointerGrab() drops the grab before calling this, so endTabDrag can no
    // longer reach a decoration to repaint through. The tab snaps back into its slot
    // here, and on a cancel path nothing else is going to damage the bar.
    if (g_tabDrag.active)
        damageEntire();

    endTabDrag();
}

void CHyprGroupBarDecoration::armTabDrag(const Vector2D& pos, PHLWINDOW dragged) {
    static auto PSTACKED  = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto PDRAGTABS = CConfigValue<Config::INTEGER>("group:groupbar:drag_tabs");

    if (!*PDRAGTABS || !dragged || !dragged->m_group || dragged->m_group->size() < 2)
        return;

    // A layout move or resize already owns this press — pass_mouse_when_bound can let
    // both through. Reordering the group while the window is being dragged around is
    // not a gesture anyone asked for.
    if (g_layoutManager->dragController()->target())
        return;

    g_tabDrag          = STabDragState{};
    g_tabDrag.window   = dragged;
    g_tabDrag.pressPos = pos;
    g_tabDrag.pointer  = pos;
    g_tabDrag.armed    = true;

    // The grab goes on the dragged tab's own decoration, not on `this`: `this`
    // belongs to whichever window was current before the press, and if that window
    // closed mid-gesture its destructor would drop the grab and strand the drag.
    const auto DECO = dragged->getDecorationByType(DECORATION_GROUPBAR);
    if (!DECO) {
        g_tabDrag = STabDragState{};
        return;
    }

    DECO->grabPointer(BTN_LEFT_CODE);

    // Where inside the grabbed tab the press landed. Drawing the tab at
    // pointer - grabOffset keeps it under the same point of the cursor for the whole
    // gesture, instead of snapping its edge to the pointer on the first motion.
    const auto BARBOX = assignedBoxGlobal();
    double     tabLen = 0, STEP = 0;
    tabMetrics(BARBOX, dragged->m_group->size(), tabLen, STEP);
    const double RELATIVE = *PSTACKED ? pos.y - BARBOX.y : pos.x - BARBOX.x;
    if (STEP > 0 && RELATIVE >= 0)
        g_tabDrag.grabOffset = RELATIVE - sc<int>(RELATIVE / STEP) * STEP;
}

// Tab size and slot pitch, from the bar box and member count rather than from
// whatever the last draw() left behind: a gesture can begin and move before the next
// frame, and the count can change under it.
void CHyprGroupBarDecoration::tabMetrics(const CBox& barBox, size_t count, double& outTabLen, double& outStep) {
    static auto PSTACKED      = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto POUTERGAP     = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
    static auto PINNERGAP     = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");
    static auto PKEEPUPPERGAP = CConfigValue<Config::INTEGER>("group:groupbar:keep_upper_gap");

    if (count < 1) {
        outTabLen = 0;
        outStep   = 0;
        return;
    }

    if (*PSTACKED) {
        outTabLen = ((barBox.h - *POUTERGAP * *PKEEPUPPERGAP) - *POUTERGAP * count) / count;
        outStep   = outTabLen + *POUTERGAP;
    } else {
        outTabLen = (barBox.w - *PINNERGAP * (count - 1)) / count;
        outStep   = outTabLen + *PINNERGAP;
    }
}

bool CHyprGroupBarDecoration::draggedTabAlong(PHLWINDOW w, const CBox& barBox, double tabLen, double& outAlong) {
    static auto PSTACKED = CConfigValue<Config::INTEGER>("group:groupbar:stacked");

    // Stacked bars keep the old snap behaviour: the arithmetic differs and it is not
    // covered by any test.
    if (!tabDragActive() || *PSTACKED || !w || g_tabDrag.window.lock() != w)
        return false;

    const double DESIRED = (g_tabDrag.pointer.x - g_tabDrag.grabOffset) - barBox.x;
    outAlong             = std::clamp(DESIRED, 0.0, std::max(0.0, barBox.w - tabLen));
    return true;
}

void CHyprGroupBarDecoration::updateTabDrag(const Vector2D& pos) {
    static auto PSTACKED  = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto PINNERGAP = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");

    if (!tabDragArmed()) {
        // Nothing is being dragged any more — most likely the window went away
        // mid-gesture. Drop the grab rather than keep taking motion forever.
        endTabDrag();
        return;
    }

    const auto WINDOW = g_tabDrag.window.lock();
    if (!WINDOW || !WINDOW->m_group) {
        endTabDrag();
        return;
    }

    if (!g_tabDrag.active) {
        if (pos.distance(g_tabDrag.pressPos) < TAB_DRAG_THRESHOLD)
            return;

        g_tabDrag.active = true;
    }

    // Live geometry: a member closing or the tile resizing mid-drag changes these.
    const auto BARBOX = assignedBoxGlobal();

    // The tab is drawn under the cursor, so every motion has to redraw the bar, not
    // just the ones that change the order.
    g_tabDrag.pointer = pos;
    g_pHyprRenderer->damageBox(BARBOX);

    const auto   GROUP = WINDOW->m_group;
    const size_t SIZE  = GROUP->size();
    if (SIZE < 2) {
        endTabDrag();
        return;
    }

    // Which slot the cursor is over. Same geometry the press and drop paths use,
    // just evaluated continuously instead of once.
    double tabLen = 0, STEP = 0;
    tabMetrics(BARBOX, SIZE, tabLen, STEP);
    if (STEP <= 0)
        return;

    // Off-axis distance from the bar. Beyond the band the gesture pauses rather than
    // ending, so overshooting and coming back behaves the way a person expects.
    const double ACROSS    = *PSTACKED ? pos.x - BARBOX.x : pos.y - BARBOX.y;
    const double THICKNESS = *PSTACKED ? BARBOX.w : BARBOX.h;
    if (THICKNESS > 0 && (ACROSS < -THICKNESS * TAB_DRAG_BAND || ACROSS > THICKNESS * (1 + TAB_DRAG_BAND)))
        return;

    const double RELATIVE = *PSTACKED ? pos.y - BARBOX.y : pos.x - BARBOX.x;
    const int    HOVERED  = RELATIVE < 0 ? 0 : sc<int>(RELATIVE / STEP);
    const size_t TARGET   = std::clamp(HOVERED, 0, sc<int>(SIZE) - 1);

    // swapWithNext/swapWithLast move whichever window is current and follow it, so
    // the dragged tab has to be the current one. It normally is — the press made it
    // current — but a group can be re-pointed from elsewhere mid-gesture.
    if (GROUP->current() != WINDOW)
        GROUP->setCurrent(WINDOW);

    if (GROUP->getCurrentIdx() == TARGET)
        return;

    GROUP->moveCurrentToIndex(TARGET);
}

CHyprGroupBarDecoration::CHyprGroupBarDecoration(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow), m_window(pWindow) {
    static auto PENABLED   = CConfigValue<Config::INTEGER>("group:groupbar:enabled");
    static auto PGRADIENTS = CConfigValue<Config::INTEGER>("group:groupbar:gradients");

    if (*PENABLED && *PGRADIENTS)
        refreshGroupBarGradients();
}

SDecorationPositioningInfo CHyprGroupBarDecoration::getPositioningInfo() {
    static auto                PHEIGHT          = CConfigValue<Config::INTEGER>("group:groupbar:height");
    static auto                PINDICATORGAP    = CConfigValue<Config::INTEGER>("group:groupbar:indicator_gap");
    static auto                PINDICATORHEIGHT = CConfigValue<Config::INTEGER>("group:groupbar:indicator_height");
    static auto                PRENDERTITLES    = CConfigValue<Config::INTEGER>("group:groupbar:render_titles");
    static auto                PGRADIENTS       = CConfigValue<Config::INTEGER>("group:groupbar:gradients");
    static auto                PPRIORITY        = CConfigValue<Config::INTEGER>("group:groupbar:priority");
    static auto                PSTACKED         = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto                POUTERGAP        = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
    static auto                PKEEPUPPERGAP    = CConfigValue<Config::INTEGER>("group:groupbar:keep_upper_gap");

    SDecorationPositioningInfo info;
    info.policy   = DECORATION_POSITION_STICKY;
    info.edges    = DECORATION_EDGE_TOP;
    info.priority = *PPRIORITY;
    info.reserved = true;

    if (visible()) {
        if (*PSTACKED) {
            const auto ONEBARHEIGHT = *POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0);
            info.desiredExtents     = {{0, (ONEBARHEIGHT * m_dwGroupMembers.size()) + (*PKEEPUPPERGAP * *POUTERGAP)}, {0, 0}};
        } else
            info.desiredExtents = {{0, *POUTERGAP * (1 + *PKEEPUPPERGAP) + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0)}, {0, 0}};
    } else
        info.desiredExtents = {{0, 0}, {0, 0}};
    return info;
}

void CHyprGroupBarDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {
    m_assignedBox = reply.assignedGeometry;
}

eDecorationType CHyprGroupBarDecoration::getDecorationType() {
    return DECORATION_GROUPBAR;
}

//

void CHyprGroupBarDecoration::updateWindow(PHLWINDOW pWindow) {
    if (!m_window->m_group) {
        m_window->removeWindowDeco(this);
        return;
    }

    m_dwGroupMembers.clear();
    for (const auto& w : m_window->m_group->windows()) {
        m_dwGroupMembers.emplace_back(w);
    }

    damageEntire();

    if (m_dwGroupMembers.empty()) {
        m_window->removeWindowDeco(this);
        return;
    }
}

void CHyprGroupBarDecoration::damageEntire() {
    auto box = assignedBoxGlobal();
    box.translate(m_window->m_floatingOffset);
    g_pHyprRenderer->damageBox(box);
}

void CHyprGroupBarDecoration::draw(PHLMONITOR pMonitor, float const& a) {
    // get how many bars we will draw
    int        barsToDraw = m_dwGroupMembers.size();

    const bool VISIBLE = visible();

    if (!m_bLastVisibilityStatus.has_value() || VISIBLE != *m_bLastVisibilityStatus) {
        g_pDecorationPositioner->repositionDeco(this);
        m_bLastVisibilityStatus = VISIBLE;
    }

    if (!VISIBLE)
        return;

    static auto PRENDERTITLES              = CConfigValue<Config::INTEGER>("group:groupbar:render_titles");
    static auto PTITLEFONTSIZE             = CConfigValue<Config::INTEGER>("group:groupbar:font_size");
    static auto PHEIGHT                    = CConfigValue<Config::INTEGER>("group:groupbar:height");
    static auto PINDICATORGAP              = CConfigValue<Config::INTEGER>("group:groupbar:indicator_gap");
    static auto PINDICATORHEIGHT           = CConfigValue<Config::INTEGER>("group:groupbar:indicator_height");
    static auto PGRADIENTS                 = CConfigValue<Config::INTEGER>("group:groupbar:gradients");
    static auto PSTACKED                   = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto PROUNDING                  = CConfigValue<Config::INTEGER>("group:groupbar:rounding");
    static auto PROUNDINGPOWER             = CConfigValue<Config::FLOAT>("group:groupbar:rounding_power");
    static auto PGRADIENTROUNDING          = CConfigValue<Config::INTEGER>("group:groupbar:gradient_rounding");
    static auto PGRADIENTROUNDINGPOWER     = CConfigValue<Config::FLOAT>("group:groupbar:gradient_rounding_power");
    static auto PGRADIENTROUNDINGONLYEDGES = CConfigValue<Config::INTEGER>("group:groupbar:gradient_round_only_edges");
    static auto PROUNDONLYEDGES            = CConfigValue<Config::INTEGER>("group:groupbar:round_only_edges");
    static auto PGROUPCOLACTIVE            = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.active");
    static auto PGROUPCOLINACTIVE          = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.inactive");
    static auto PGROUPCOLACTIVELOCKED      = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.locked_active");
    static auto PGROUPCOLINACTIVELOCKED    = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.locked_inactive");
    static auto POUTERGAP                  = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
    static auto PINNERGAP                  = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");
    static auto PKEEPUPPERGAP              = CConfigValue<Config::INTEGER>("group:groupbar:keep_upper_gap");
    static auto PTEXTOFFSET                = CConfigValue<Config::INTEGER>("group:groupbar:text_offset");
    static auto PTEXTPADDING               = CConfigValue<Config::INTEGER>("group:groupbar:text_padding");
    static auto PBLUR                      = CConfigValue<Config::INTEGER>("group:groupbar:blur");
    auto* const GROUPCOLACTIVE             = sc<Config::CGradientValueData*>((PGROUPCOLACTIVE.ptr()));
    auto* const GROUPCOLINACTIVE           = sc<Config::CGradientValueData*>((PGROUPCOLINACTIVE.ptr()));
    auto* const GROUPCOLACTIVELOCKED       = sc<Config::CGradientValueData*>((PGROUPCOLACTIVELOCKED.ptr()));
    auto* const GROUPCOLINACTIVELOCKED     = sc<Config::CGradientValueData*>((PGROUPCOLINACTIVELOCKED.ptr()));

    const auto  ASSIGNEDBOX = assignedBoxGlobal();

    const auto  ONEBARHEIGHT = *POUTERGAP + *PINDICATORHEIGHT + *PINDICATORGAP + (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0);
    m_barWidth               = *PSTACKED ? ASSIGNEDBOX.w : (ASSIGNEDBOX.w - *PINNERGAP * (barsToDraw - 1)) / barsToDraw;
    m_barHeight              = *PSTACKED ? ((ASSIGNEDBOX.h - *POUTERGAP * *PKEEPUPPERGAP) - *POUTERGAP * (barsToDraw)) / barsToDraw : ASSIGNEDBOX.h - *POUTERGAP * *PKEEPUPPERGAP;

    const auto DESIREDHEIGHT = *PSTACKED ? (ONEBARHEIGHT * m_dwGroupMembers.size()) + *POUTERGAP * *PKEEPUPPERGAP : *POUTERGAP * (1 + *PKEEPUPPERGAP) + ONEBARHEIGHT;
    if (DESIREDHEIGHT != ASSIGNEDBOX.h)
        g_pDecorationPositioner->repositionDeco(this);

    bool blur = *PBLUR != 0;

    // Draw order is slot order, except that a tab being dragged goes last so it stays
    // on top of the ones it slides past. Kept as an index mapping rather than a list:
    // this runs for every visible group on every frame, and the overwhelmingly common
    // case has nothing being dragged at all.
    int dragged = -1;
    if (tabDragActive()) {
        for (int i = 0; i < barsToDraw; ++i) {
            const auto MEMBER    = *PSTACKED ? m_dwGroupMembers.size() - i - 1 : i;
            double     dragAlong = 0;
            if (draggedTabAlong(m_dwGroupMembers[MEMBER].lock(), ASSIGNEDBOX, m_barWidth, dragAlong))
                dragged = i;
        }
    }

    for (int n = 0; n < barsToDraw; ++n) {
        const int  i           = dragged < 0 ? n : (n == barsToDraw - 1 ? dragged : (n < dragged ? n : n + 1));
        const auto WINDOWINDEX = *PSTACKED ? m_dwGroupMembers.size() - i - 1 : i;

        // Offsets come from the slot index rather than accumulating across the loop,
        // because the loop no longer runs in slot order.
        const float yoff = *PSTACKED ? i * ONEBARHEIGHT : 0;

        // A tab being dragged is drawn wherever the cursor is rather than in its
        // slot, so it slides with the pointer while the others hold their places.
        // Only the drawing offset changes; the slot arithmetic is untouched.
        float  xoffDraw  = *PSTACKED ? 0 : i * (*PINNERGAP + m_barWidth);
        double dragAlong = 0;
        if (draggedTabAlong(m_dwGroupMembers[WINDOWINDEX].lock(), ASSIGNEDBOX, m_barWidth, dragAlong))
            xoffDraw = dragAlong;

        CBox rect = {ASSIGNEDBOX.x + xoffDraw - pMonitor->m_position.x + m_window->m_floatingOffset.x,
                     ASSIGNEDBOX.y + ASSIGNEDBOX.h - floor(yoff) - *PINDICATORHEIGHT - *POUTERGAP - pMonitor->m_position.y + m_window->m_floatingOffset.y, m_barWidth,
                     *PINDICATORHEIGHT};

        rect.scale(pMonitor->m_scale).round();

        const bool        GROUPLOCKED  = m_window->m_group->locked() || g_pKeybindManager->m_groupsLocked;
        const auto* const PCOLACTIVE   = GROUPLOCKED ? GROUPCOLACTIVELOCKED : GROUPCOLACTIVE;
        const auto* const PCOLINACTIVE = GROUPLOCKED ? GROUPCOLINACTIVELOCKED : GROUPCOLINACTIVE;

        CHyprColor        color = m_dwGroupMembers[WINDOWINDEX].lock() == Desktop::focusState()->window() ? PCOLACTIVE->m_colors[0] : PCOLINACTIVE->m_colors[0];
        color.a *= a;

        if (!rect.empty()) {
            CRectPassElement::SRectData rectdata;
            rectdata.color = color;
            rectdata.blur  = blur;
            rectdata.box   = rect;
            if (*PROUNDING) {
                rectdata.round         = *PROUNDING;
                rectdata.roundingPower = *PROUNDINGPOWER;
                if (*PROUNDONLYEDGES && barsToDraw > 1) {
                    rectdata.round      = 0;
                    const double offset = *PROUNDING * 2;
                    if (i == 0) {
                        rectdata.round   = *PROUNDING;
                        rectdata.clipBox = rect;
                        rectdata.box     = CBox{rect.pos(), Vector2D{rect.w + offset, rect.h}};
                    } else if (i == barsToDraw - 1) {
                        rectdata.round   = *PROUNDING;
                        rectdata.clipBox = rect;
                        rectdata.box     = CBox{rect.pos() - Vector2D{offset, 0.F}, Vector2D{rect.w + offset, rect.h}};
                    }
                }
            }
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(rectdata));
        }

        rect = {ASSIGNEDBOX.x + xoffDraw - pMonitor->m_position.x + m_window->m_floatingOffset.x,
                ASSIGNEDBOX.y + ASSIGNEDBOX.h - floor(yoff) - ONEBARHEIGHT - pMonitor->m_position.y + m_window->m_floatingOffset.y, m_barWidth,
                (*PGRADIENTS || *PRENDERTITLES ? *PHEIGHT : 0)};
        rect.scale(pMonitor->m_scale);

        if (!rect.empty()) {
            if (*PGRADIENTS) {
                const auto GRADIENTTEX = (m_dwGroupMembers[WINDOWINDEX] == Desktop::focusState()->window() ? (GROUPLOCKED ? m_tGradientLockedActive : m_tGradientActive) :
                                                                                                             (GROUPLOCKED ? m_tGradientLockedInactive : m_tGradientInactive));
                if (GRADIENTTEX && GRADIENTTEX->ok()) {
                    CTexPassElement::SRenderData data;
                    data.tex  = GRADIENTTEX;
                    data.blur = blur;
                    data.box  = rect;
                    data.a    = a;
                    if (*PGRADIENTROUNDING) {
                        data.round         = *PGRADIENTROUNDING;
                        data.roundingPower = *PGRADIENTROUNDINGPOWER;
                        if (*PGRADIENTROUNDINGONLYEDGES && barsToDraw > 1) {
                            data.round          = 0;
                            const double offset = *PGRADIENTROUNDING * 2;
                            if (i == 0) {
                                data.round   = *PGRADIENTROUNDING;
                                data.clipBox = rect;
                                data.box     = CBox{rect.pos(), Vector2D{rect.w + offset, rect.h}};
                            } else if (i == barsToDraw - 1) {
                                data.round   = *PGRADIENTROUNDING;
                                data.clipBox = rect;
                                data.box     = CBox{rect.pos() - Vector2D{offset, 0.F}, Vector2D{rect.w + offset, rect.h}};
                            }
                        }
                    }
                    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(data));
                }
            }

            if (*PRENDERTITLES) {
                CTitleTex* pTitleTex = textureFromTitle(m_dwGroupMembers[WINDOWINDEX]->m_title);

                if (!pTitleTex)
                    pTitleTex =
                        m_titleTexs.titleTexs
                            .emplace_back(makeUnique<CTitleTex>(
                                m_dwGroupMembers[WINDOWINDEX].lock(),
                                Vector2D{(m_barWidth - (*PTEXTPADDING * 2)) * pMonitor->m_scale, (*PTITLEFONTSIZE + 2L * BAR_TEXT_PAD) * pMonitor->m_scale}, pMonitor->m_scale))
                            .get();

                SP<ITexture> titleTex;
                if (m_dwGroupMembers[WINDOWINDEX] == Desktop::focusState()->window())
                    titleTex = GROUPLOCKED ? pTitleTex->m_texLockedActive : pTitleTex->m_texActive;
                else
                    titleTex = GROUPLOCKED ? pTitleTex->m_texLockedInactive : pTitleTex->m_texInactive;

                rect.y += std::ceil(((rect.height - titleTex->m_size.y) / 2.0) - (*PTEXTOFFSET * pMonitor->m_scale));
                rect.height = titleTex->m_size.y;
                rect.width  = titleTex->m_size.x;
                rect.x += std::round((((m_barWidth + *PTEXTPADDING) * pMonitor->m_scale) / 2.0) - ((titleTex->m_size.x + *PTEXTPADDING) / 2.0));
                rect.round();

                CTexPassElement::SRenderData data;
                data.tex = titleTex;
                data.box = rect;
                data.a   = a;
                g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(std::move(data)));
            }
        }
    }

    if (*PRENDERTITLES)
        invalidateTextures();
}

CTitleTex* CHyprGroupBarDecoration::textureFromTitle(const std::string& title) {
    for (auto const& tex : m_titleTexs.titleTexs) {
        if (tex->m_content == title)
            return tex.get();
    }

    return nullptr;
}

void CHyprGroupBarDecoration::invalidateTextures() {
    m_titleTexs.titleTexs.clear();
}

CTitleTex::CTitleTex(PHLWINDOW pWindow, const Vector2D& bufferSize, const float monitorScale) : m_content(pWindow->m_title), m_windowOwner(pWindow) {
    static auto      FALLBACKFONT             = CConfigValue<std::string>("misc:font_family");
    static auto      PTITLEFONTFAMILY         = CConfigValue<std::string>("group:groupbar:font_family");
    static auto      PTITLEFONTSIZE           = CConfigValue<Config::INTEGER>("group:groupbar:font_size");
    static auto      PTEXTCOLORACTIVE         = CConfigValue<Config::INTEGER>("group:groupbar:text_color");
    static auto      PTEXTCOLORINACTIVE       = CConfigValue<Config::INTEGER>("group:groupbar:text_color_inactive");
    static auto      PTEXTCOLORLOCKEDACTIVE   = CConfigValue<Config::INTEGER>("group:groupbar:text_color_locked_active");
    static auto      PTEXTCOLORLOCKEDINACTIVE = CConfigValue<Config::INTEGER>("group:groupbar:text_color_locked_inactive");

    static auto      PTITLEFONTWEIGHTACTIVE   = CConfigValue<Config::IComplexConfigValue>("group:groupbar:font_weight_active");
    static auto      PTITLEFONTWEIGHTINACTIVE = CConfigValue<Config::IComplexConfigValue>("group:groupbar:font_weight_inactive");

    const auto       FONTWEIGHTACTIVE   = sc<Config::CFontWeightConfigValueData*>((PTITLEFONTWEIGHTACTIVE.ptr()));
    const auto       FONTWEIGHTINACTIVE = sc<Config::CFontWeightConfigValueData*>((PTITLEFONTWEIGHTINACTIVE.ptr()));

    const CHyprColor COLORACTIVE         = CHyprColor(*PTEXTCOLORACTIVE);
    const CHyprColor COLORINACTIVE       = *PTEXTCOLORINACTIVE == -1 ? COLORACTIVE : CHyprColor(*PTEXTCOLORINACTIVE);
    const CHyprColor COLORLOCKEDACTIVE   = *PTEXTCOLORLOCKEDACTIVE == -1 ? COLORACTIVE : CHyprColor(*PTEXTCOLORLOCKEDACTIVE);
    const CHyprColor COLORLOCKEDINACTIVE = *PTEXTCOLORLOCKEDINACTIVE == -1 ? COLORINACTIVE : CHyprColor(*PTEXTCOLORLOCKEDINACTIVE);

    const auto       FONTFAMILY = *PTITLEFONTFAMILY != STRVAL_EMPTY ? *PTITLEFONTFAMILY : *FALLBACKFONT;

#define RENDER_TEXT(color, weight) g_pHyprRenderer->renderText(pWindow->m_title, (color), *PTITLEFONTSIZE* monitorScale, false, FONTFAMILY, bufferSize.x - 2, (weight));
    m_texActive         = RENDER_TEXT(COLORACTIVE, FONTWEIGHTACTIVE->m_value);
    m_texInactive       = RENDER_TEXT(COLORINACTIVE, FONTWEIGHTINACTIVE->m_value);
    m_texLockedActive   = RENDER_TEXT(COLORLOCKEDACTIVE, FONTWEIGHTACTIVE->m_value);
    m_texLockedInactive = RENDER_TEXT(COLORLOCKEDINACTIVE, FONTWEIGHTINACTIVE->m_value);
#undef RENDER_TEXT
}

static SP<ITexture> renderGradient(Config::CGradientValueData* grad) {

    if (!Desktop::focusState()->monitor())
        return nullptr;

    const Vector2D& bufferSize = Desktop::focusState()->monitor()->m_pixelSize;

    const auto      CAIROSURFACE = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bufferSize.x, bufferSize.y);
    const auto      CAIRO        = cairo_create(CAIROSURFACE);

    // clear the pixmap
    cairo_save(CAIRO);
    cairo_set_operator(CAIRO, CAIRO_OPERATOR_CLEAR);
    cairo_paint(CAIRO);
    cairo_restore(CAIRO);

    cairo_pattern_t* pattern;
    pattern = cairo_pattern_create_linear(0, 0, 0, bufferSize.y);

    for (unsigned long i = 0; i < grad->m_colors.size(); i++) {
        cairo_pattern_add_color_stop_rgba(pattern, 1 - sc<double>(i + 1) / (grad->m_colors.size() + 1), grad->m_colors[i].r, grad->m_colors[i].g, grad->m_colors[i].b,
                                          grad->m_colors[i].a);
    }

    cairo_rectangle(CAIRO, 0, 0, bufferSize.x, bufferSize.y);
    cairo_set_source(CAIRO, pattern);
    cairo_fill(CAIRO);
    cairo_pattern_destroy(pattern);

    cairo_surface_flush(CAIROSURFACE);

    // copy the data to an OpenGL texture we have
    auto tex = g_pHyprRenderer->createTexture(CAIROSURFACE);

    // delete cairo
    cairo_destroy(CAIRO);
    cairo_surface_destroy(CAIROSURFACE);

    return tex;
}

void refreshGroupBarGradients() {
    static auto PENABLED   = CConfigValue<Config::BOOL>("group:groupbar:enabled");
    static auto PGRADIENTS = CConfigValue<Config::BOOL>("group:groupbar:gradients");

    static auto PGROUPCOLACTIVE         = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.active");
    static auto PGROUPCOLINACTIVE       = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.inactive");
    static auto PGROUPCOLACTIVELOCKED   = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.locked_active");
    static auto PGROUPCOLINACTIVELOCKED = CConfigValue<Config::IComplexConfigValue>("group:groupbar:col.locked_inactive");
    auto* const GROUPCOLACTIVE          = sc<Config::CGradientValueData*>((PGROUPCOLACTIVE.ptr()));
    auto* const GROUPCOLINACTIVE        = sc<Config::CGradientValueData*>((PGROUPCOLINACTIVE.ptr()));
    auto* const GROUPCOLACTIVELOCKED    = sc<Config::CGradientValueData*>((PGROUPCOLACTIVELOCKED.ptr()));
    auto* const GROUPCOLINACTIVELOCKED  = sc<Config::CGradientValueData*>((PGROUPCOLINACTIVELOCKED.ptr()));

    if (m_tGradientActive && m_tGradientActive->ok()) {
        m_tGradientActive.reset();
        m_tGradientInactive.reset();
        m_tGradientLockedActive.reset();
        m_tGradientLockedInactive.reset();
    }

    if (!*PENABLED || !*PGRADIENTS)
        return;

    m_tGradientActive         = renderGradient(GROUPCOLACTIVE);
    m_tGradientInactive       = renderGradient(GROUPCOLINACTIVE);
    m_tGradientLockedActive   = renderGradient(GROUPCOLACTIVELOCKED);
    m_tGradientLockedInactive = renderGradient(GROUPCOLINACTIVELOCKED);
}

bool CHyprGroupBarDecoration::onBeginWindowDragOnDeco(const Vector2D& pos) {
    static auto PSTACKED  = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto POUTERGAP = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
    static auto PINNERGAP = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");
    if (m_window->m_group->size() == 1)
        return false;

    const float BARRELATIVEX = pos.x - assignedBoxGlobal().x;
    const float BARRELATIVEY = pos.y - assignedBoxGlobal().y;
    const int   WINDOWINDEX  = *PSTACKED ? (BARRELATIVEY / (m_barHeight + *POUTERGAP)) : (BARRELATIVEX) / (m_barWidth + *PINNERGAP);

    if (!*PSTACKED && (BARRELATIVEX - (m_barWidth + *PINNERGAP) * WINDOWINDEX > m_barWidth))
        return false;

    if (*PSTACKED && (BARRELATIVEY - (m_barHeight + *POUTERGAP) * WINDOWINDEX < *POUTERGAP))
        return false;

    PHLWINDOW   pWindow = m_window->m_group->fromIndex(WINDOWINDEX);

    const auto& GROUP = m_window->m_group;

    // remove the window from the group
    GROUP->remove(pWindow);

    // start a move drag on it
    g_layoutManager->dragController()->dragBegin(pWindow->layoutTarget(), MBIND_MOVE);

    if (!Desktop::focusState()->isWindowActive(pWindow))
        Desktop::focusState()->rawWindowFocus(pWindow, Desktop::FOCUS_REASON_CLICK);

    return true;
}

bool CHyprGroupBarDecoration::onEndWindowDragOnDeco(const Vector2D& pos, PHLWINDOW pDraggedWindow) {
    static auto PDRAGINTOGROUP                   = CConfigValue<Config::INTEGER>("group:drag_into_group");
    static auto PMERGEFLOATEDINTOTILEDONGROUPBAR = CConfigValue<Config::INTEGER>("group:merge_floated_into_tiled_on_groupbar");
    static auto PMERGEGROUPSONGROUPBAR           = CConfigValue<Config::INTEGER>("group:merge_groups_on_groupbar");
    const bool  FLOATEDINTOTILED                 = !m_window->m_isFloating && !g_layoutManager->dragController()->draggingTiled();

    if (!pDraggedWindow->canBeGroupedInto(m_window->m_group) || (*PDRAGINTOGROUP != 1 && *PDRAGINTOGROUP != 2) || (FLOATEDINTOTILED && !*PMERGEFLOATEDINTOTILEDONGROUPBAR) ||
        (!*PMERGEGROUPSONGROUPBAR && pDraggedWindow->m_group))
        return false;

    m_window->m_group->add(pDraggedWindow);

    if (!pDraggedWindow->getDecorationByType(DECORATION_GROUPBAR))
        pDraggedWindow->addWindowDeco(makeUnique<CHyprGroupBarDecoration>(pDraggedWindow));

    return true;
}

bool CHyprGroupBarDecoration::onMouseButtonOnDeco(const Vector2D& pos, const IPointer::SButtonEvent& e) {
    static auto PSTACKED          = CConfigValue<Config::INTEGER>("group:groupbar:stacked");
    static auto POUTERGAP         = CConfigValue<Config::INTEGER>("group:groupbar:gaps_out");
    static auto PINNERGAP         = CConfigValue<Config::INTEGER>("group:groupbar:gaps_in");
    static auto PMIDDLECLICKCLOSE = CConfigValue<Config::INTEGER>("group:groupbar:middle_click_close");

    // The release that ends a tab drag. It arrives here through the pointer grab, so
    // it reaches us wherever the pointer ended up. A press that never passed the
    // threshold was a click, not a drag, so it is not consumed and falls through to
    // the normal handling below.
    if (e.button == BTN_LEFT_CODE && e.state == WL_POINTER_BUTTON_STATE_RELEASED && hasPointerGrab()) {
        const bool WAS_DRAG = tabDragActive();
        endTabDrag();
        return WAS_DRAG;
    }

    if (Fullscreen::controller()->getFullscreenModes(m_window.lock()).internal == Fullscreen::FSMODE_FULLSCREEN)
        return true;

    const float BARRELATIVEX = pos.x - assignedBoxGlobal().x;
    const float BARRELATIVEY = pos.y - assignedBoxGlobal().y;
    const int   WINDOWINDEX  = *PSTACKED ? (BARRELATIVEY / (m_barHeight + *POUTERGAP)) : (BARRELATIVEX) / (m_barWidth + *PINNERGAP);
    static auto PFOLLOWMOUSE = CConfigValue<Config::INTEGER>("input:follow_mouse");

    // close window on middle click
    if (e.button == 274) {
        if (!*PMIDDLECLICKCLOSE)
            return true;

        static Vector2D pressedCursorPos;

        if (e.state == WL_POINTER_BUTTON_STATE_PRESSED)
            pressedCursorPos = pos;
        else if (e.state == WL_POINTER_BUTTON_STATE_RELEASED && pressedCursorPos == pos)
            g_pXWaylandManager->sendCloseWindow(m_window->m_group->fromIndex(WINDOWINDEX));

        return true;
    }

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED)
        return true;

    // click on padding
    const auto TABPAD   = !*PSTACKED && (BARRELATIVEX - (m_barWidth + *PINNERGAP) * WINDOWINDEX > m_barWidth);
    const auto STACKPAD = *PSTACKED && (BARRELATIVEY - (m_barHeight + *POUTERGAP) * WINDOWINDEX < *POUTERGAP);
    if (TABPAD || STACKPAD) {
        if (!Desktop::focusState()->isWindowActive(m_window.lock()))
            Desktop::focusState()->rawWindowFocus(m_window.lock(), Desktop::FOCUS_REASON_CLICK);
        return true;
    }

    PHLWINDOW pWindow = m_window->m_group->fromIndex(WINDOWINDEX);

    if (pWindow != m_window)
        pWindow->m_group->setCurrent(pWindow);

    if (!Desktop::focusState()->isWindowActive(pWindow) && *PFOLLOWMOUSE != 3)
        Desktop::focusState()->rawWindowFocus(pWindow, Desktop::FOCUS_REASON_CLICK);

    if (pWindow->m_isFloating)
        Desktop::windowState()->raise(pWindow);

    // A press on a tab is also the start of a possible drag. Nothing moves until
    // the pointer passes the threshold, so an ordinary click still just focuses.
    if (e.button == BTN_LEFT_CODE)
        armTabDrag(pos, pWindow);

    return true;
}

bool CHyprGroupBarDecoration::onScrollOnDeco(const Vector2D& pos, const IPointer::SAxisEvent e) {
    static auto PGROUPBARSCROLLING = CConfigValue<Config::INTEGER>("group:groupbar:scrolling");

    if (!*PGROUPBARSCROLLING || !m_window->m_group)
        return false;

    if (e.delta > 0)
        m_window->m_group->moveCurrent(true);
    else
        m_window->m_group->moveCurrent(false);

    return true;
}

bool CHyprGroupBarDecoration::onInputOnDeco(const eInputType type, const Vector2D& mouseCoords, std::any data) {
    switch (type) {
        case INPUT_TYPE_AXIS: return onScrollOnDeco(mouseCoords, std::any_cast<const IPointer::SAxisEvent>(data));
        case INPUT_TYPE_BUTTON: return onMouseButtonOnDeco(mouseCoords, std::any_cast<const IPointer::SButtonEvent&>(data));
        case INPUT_TYPE_DRAG_START: return onBeginWindowDragOnDeco(mouseCoords);
        case INPUT_TYPE_DRAG_END: return onEndWindowDragOnDeco(mouseCoords, std::any_cast<PHLWINDOW>(data));
        case INPUT_TYPE_MOTION: updateTabDrag(mouseCoords); return true;
        default: return false;
    }
}

eDecorationLayer CHyprGroupBarDecoration::getDecorationLayer() {
    return DECORATION_LAYER_OVER;
}

uint64_t CHyprGroupBarDecoration::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT;
}

std::string CHyprGroupBarDecoration::getDisplayName() {
    return "GroupBar";
}

CBox CHyprGroupBarDecoration::assignedBoxGlobal() {
    CBox box = m_assignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_window));

    const auto PWORKSPACE = m_window->m_workspace;

    if (PWORKSPACE && !m_window->m_pinned)
        box.translate(PWORKSPACE->m_renderOffset->value());

    return box.round();
}

bool CHyprGroupBarDecoration::visible() {
    static auto PENABLED = CConfigValue<Config::BOOL>("group:groupbar:enabled");
    static auto PDISABLE = CConfigValue<Config::BOOL>("group:groupbar:disable_when_only");
    return *PENABLED && (!*PDISABLE || m_dwGroupMembers.size() > 1) && m_window->m_ruleApplicator->decorate().valueOrDefault();
}
