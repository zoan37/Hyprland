#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include <format>
#include <thread>
#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include "../shared.hpp"

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;

#define UP CUniquePointer
#define SP CSharedPointer

// Geometry of the groupbar under the config these tests pin, on the standard
// 1920x1080 test output. A single tiled window sits at 22,22 sized 1876x1036 (see the
// groups test), and the bar reserves space off the top of it.
//
// Horizontal, one row of tabs:
//   reserved = gaps_out * (1 + keep_upper_gap) + indicator_height + indicator_gap + height
//            = 2 * 2 + 3 + 0 + 20 = 27
//   tab width = (bar width - gaps_in * (n - 1)) / n
//
// Stacked, one tab per row:
//   row      = gaps_out + indicator_height + indicator_gap + height = 25
//   reserved = row * n + gaps_out * keep_upper_gap = 25 * 3 + 2 = 77
//   tab step = row
static constexpr int BAR_X      = 22;
static constexpr int BAR_Y      = 22;
static constexpr int BAR_W      = 1876;
static constexpr int TAB_COUNT  = 3;
static constexpr int GAPS_IN    = 2;
static constexpr int H_RESERVED = 27;
static constexpr int H_TAB_W    = (BAR_W - GAPS_IN * (TAB_COUNT - 1)) / TAB_COUNT;
static constexpr int S_ROW      = 25;
static constexpr int S_RESERVED = S_ROW * TAB_COUNT + 2;

// hyprtester does not pull in Hyprland's math headers, so a plain pair will do.
struct SPoint {
    int x = 0;
    int y = 0;
};

struct SBarLayout {
    bool        stacked  = false;
    int         reserved = H_RESERVED;
    const char* name     = "horizontal";
};

static constexpr SBarLayout HORIZONTAL{.stacked = false, .reserved = H_RESERVED, .name = "horizontal"};
static constexpr SBarLayout STACKED{.stacked = true, .reserved = S_RESERVED, .name = "stacked"};

// Middle of the tab occupying a slot. Both axes come from the same arithmetic the
// implementation uses to turn a cursor position back into a slot.
static SPoint tabMid(const SBarLayout& layout, int index) {
    if (layout.stacked)
        return SPoint{.x = BAR_X + BAR_W / 2, .y = BAR_Y + index * S_ROW + S_ROW / 2};

    return SPoint{.x = BAR_X + index * (H_TAB_W + GAPS_IN) + H_TAB_W / 2, .y = BAR_Y + layout.reserved / 2};
}

static bool moveCursor(const SPoint& pos) {
    return getFromSocket(std::format("/dispatch hl.dsp.cursor.move({{ x = {}, y = {} }})", pos.x, pos.y)) == "ok";
}

static bool button(bool pressed) {
    return getFromSocket(std::format("/eval hl.plugin.test.click(272, {})", pressed ? 1 : 0)) == "ok";
}

// "\tclass: kitty_A\n" -> "kitty_A"
static std::string classOf(const std::string& activeWindow) {
    const auto POS = activeWindow.find("class: ");
    if (POS == std::string::npos)
        return "";

    const auto START = POS + std::string_view{"class: "}.length();
    const auto END   = activeWindow.find('\n', START);
    return activeWindow.substr(START, END == std::string::npos ? END : END - START);
}

// A press, some motion, then a release — the gesture as a user performs it. The motion
// has to arrive in steps: the reorder is driven by the pointer crossing tab
// boundaries, so a single jump would exercise nothing in between.
static void dragAlongBar(const SPoint& from, const SPoint& to, int steps = 12) {
    moveCursor(from);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 1; i <= steps; ++i) {
        moveCursor(SPoint{.x = from.x + (to.x - from.x) * i / steps, .y = from.y + (to.y - from.y) * i / steps});
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    button(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// Reads back which window occupies a slot, by clicking that tab and asking what is
// focused. Uses only behaviour that already existed before tab dragging, so a failure
// here cannot be the assertion itself being wrong.
static std::string classInSlot(const SBarLayout& layout, int index) {
    moveCursor(tabMid(layout, index));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    button(true);
    button(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return classOf(getFromSocket("/activewindow"));
}

static std::string applyGroupbarConfig(bool stacked, bool dragTabs) {
    const auto RESULT = getFromSocket(std::format("/eval hl.config({{ group = {{ auto_group = false, groupbar = {{ enabled = true, drag_tabs = {}, stacked = {}, "
                                                  "render_titles = true, gradients = false, height = 20, indicator_height = 3, indicator_gap = 0, gaps_in = 2, "
                                                  "gaps_out = 2, keep_upper_gap = true }} }} }})",
                                                  dragTabs ? "true" : "false", stacked ? "true" : "false"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return RESULT;
}

TEST_CASE(groupbar_tab_drag) {
    NLog::log("{}Dispatching workspace `groupbar_tab_drag`", Colors::YELLOW);
    getFromSocket("/dispatch hl.dsp.focus({ workspace = 'name:groupbar_tab_drag' })");

    OK(applyGroupbarConfig(false, true));

    auto kittyA = Tests::spawnKitty("kitty_A");
    auto kittyB = Tests::spawnKitty("kitty_B");
    auto kittyC = Tests::spawnKitty("kitty_C");
    if (!kittyA || !kittyB || !kittyC)
        FAIL_TEST("Could not spawn kitty");

    NLog::log("{}Grouping the three windows", Colors::YELLOW);
    OK(getFromSocket("/dispatch hl.dsp.focus({ window = 'class:kitty_A' })"));
    OK(getFromSocket("/dispatch hl.dsp.group.toggle()"));
    for (const auto& CLASS : {"kitty_B", "kitty_C"}) {
        OK(getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'class:{}' }})", CLASS)));
        OK(getFromSocket("/dispatch hl.dsp.window.move({ into_or_create_group = 'left' })"));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // The two orientations index tabs along different axes, so both are exercised.
    for (const auto& LAYOUT : {HORIZONTAL, STACKED}) {
        NLog::log("{}Tab dragging, {} bar", Colors::YELLOW, LAYOUT.name);
        OK(applyGroupbarConfig(LAYOUT.stacked, true));

        // The tab coordinates are derived, not measured, so check the assumption
        // rather than silently clicking the wrong pixels if the layout ever changes.
        EXPECT_COUNT_STRING(getFromSocket("/clients"), std::format("at: {},{}", BAR_X, BAR_Y + LAYOUT.reserved), TAB_COUNT);

        const std::string SLOT0_BEFORE = classInSlot(LAYOUT, 0);
        EXPECT_NOT(SLOT0_BEFORE, "");

        NLog::log("{}Drag the first tab to the last slot", Colors::YELLOW);
        dragAlongBar(tabMid(LAYOUT, 0), tabMid(LAYOUT, 2));
        EXPECT(classInSlot(LAYOUT, 2), SLOT0_BEFORE);

        NLog::log("{}Drag it back to the first slot", Colors::YELLOW);
        dragAlongBar(tabMid(LAYOUT, 2), tabMid(LAYOUT, 0));
        EXPECT(classInSlot(LAYOUT, 0), SLOT0_BEFORE);
    }

    // back to a horizontal bar for the remaining cases
    OK(applyGroupbarConfig(false, true));

    NLog::log("{}A click must not reorder", Colors::YELLOW);
    {
        const auto BEFORE = classInSlot(HORIZONTAL, 1);
        moveCursor(tabMid(HORIZONTAL, 1));
        button(true);
        button(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT(classInSlot(HORIZONTAL, 1), BEFORE);
    }

    NLog::log("{}drag_tabs = false disables the gesture", Colors::YELLOW);
    {
        OK(applyGroupbarConfig(false, false));
        const auto BEFORE = classInSlot(HORIZONTAL, 0);
        dragAlongBar(tabMid(HORIZONTAL, 0), tabMid(HORIZONTAL, 2));
        EXPECT(classInSlot(HORIZONTAL, 0), BEFORE);
        OK(applyGroupbarConfig(false, true));
    }

    // cleanup
    NLog::log("{}Killing all windows", Colors::YELLOW);
    Tests::killAllWindows();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
