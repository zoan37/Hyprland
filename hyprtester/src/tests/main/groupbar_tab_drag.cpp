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

// Geometry of the groupbar under the config this test pins below, on the standard
// 1920x1080 test output. A single tiled window sits at 22,22 sized 1876x1036 (see
// the groups test); the bar reserves BAR_H off the top of that, and the window is
// pushed down by the same amount.
//
//   BAR_H = gaps_out * (1 + keep_upper_gap) + indicator_height + indicator_gap + height
//         = 2 * 2 + 3 + 0 + 20 = 27
static constexpr int BAR_X     = 22;
static constexpr int BAR_Y     = 22;
static constexpr int BAR_W     = 1876;
static constexpr int BAR_H     = 27;
static constexpr int GAPS_IN   = 2;
static constexpr int TAB_COUNT = 3;
static constexpr int TAB_W     = (BAR_W - GAPS_IN * (TAB_COUNT - 1)) / TAB_COUNT;

// vertical middle of the bar, and the horizontal middle of each tab
static constexpr int BAR_MID_Y = BAR_Y + BAR_H / 2;

static int           tabMidX(int index) {
    return BAR_X + index * (TAB_W + GAPS_IN) + TAB_W / 2;
}

static bool moveCursor(int x, int y) {
    return getFromSocket(std::format("/dispatch hl.dsp.cursor.move({{ x = {}, y = {} }})", x, y)) == "ok";
}

static bool button(bool pressed) {
    return getFromSocket(std::format("/eval hl.plugin.test.click(272, {})", pressed ? 1 : 0)) == "ok";
}

// A press, some motion, then a release — the gesture as a user performs it. The
// motion has to arrive in steps: the reorder is driven by the pointer crossing tab
// boundaries, so a single jump would exercise nothing in between.
static void dragAlongBar(int fromX, int toX, int steps = 12) {
    moveCursor(fromX, BAR_MID_Y);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 1; i <= steps; ++i) {
        moveCursor(fromX + (toX - fromX) * i / steps, BAR_MID_Y);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    button(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

// Reads back which window occupies a slot, by clicking that tab and asking what is
// focused. Uses only behaviour that already existed before tab dragging, so a
// failure here cannot be the assertion itself being wrong.
static std::string classInSlot(int index) {
    moveCursor(tabMidX(index), BAR_MID_Y);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    button(true);
    button(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return classOf(getFromSocket("/activewindow"));
}

TEST_CASE(groupbar_tab_drag) {
    NLog::log("{}Dispatching workspace `groupbar_tab_drag`", Colors::YELLOW);
    getFromSocket("/dispatch hl.dsp.focus({ workspace = 'name:groupbar_tab_drag' })");

    // Pin the groupbar geometry the constants above assume, and make sure the
    // feature under test is on.
    OK(getFromSocket("/eval hl.config({ group = { auto_group = false, groupbar = { enabled = true, drag_tabs = true, stacked = false, render_titles = true, "
                     "gradients = false, height = 20, indicator_height = 3, indicator_gap = 0, gaps_in = 2, gaps_out = 2, keep_upper_gap = true } } })"));

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

    // The tab coordinates are derived, not measured, so check the assumption rather
    // than silently clicking the wrong pixels if the layout ever changes.
    NLog::log("{}Check the group's geometry matches what the tab maths assumes", Colors::YELLOW);
    {
        const auto STR = getFromSocket("/clients");
        EXPECT_COUNT_STRING(STR, std::format("at: {},{}", BAR_X, BAR_Y + BAR_H), TAB_COUNT);
    }

    // Whatever the grouping left behind, put a known window in slot 0 to drag.
    const std::string SLOT0_BEFORE = classInSlot(0);

    EXPECT_NOT(SLOT0_BEFORE, "");

    NLog::log("{}Drag the first tab to the last slot", Colors::YELLOW);
    dragAlongBar(tabMidX(0), tabMidX(2));
    EXPECT(classInSlot(2), SLOT0_BEFORE);

    NLog::log("{}Drag it back to the first slot", Colors::YELLOW);
    dragAlongBar(tabMidX(2), tabMidX(0));
    EXPECT(classInSlot(0), SLOT0_BEFORE);

    NLog::log("{}A click must not reorder", Colors::YELLOW);
    {
        const auto BEFORE = classInSlot(1);
        moveCursor(tabMidX(1), BAR_MID_Y);
        button(true);
        button(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT(classInSlot(1), BEFORE);
    }

    NLog::log("{}drag_tabs = false disables the gesture", Colors::YELLOW);
    {
        OK(getFromSocket("/eval hl.config({ group = { groupbar = { drag_tabs = false } } })"));
        const auto BEFORE = classInSlot(0);
        dragAlongBar(tabMidX(0), tabMidX(2));
        EXPECT(classInSlot(0), BEFORE);
        OK(getFromSocket("/eval hl.config({ group = { groupbar = { drag_tabs = true } } })"));
    }

    // cleanup
    NLog::log("{}Killing all windows", Colors::YELLOW);
    Tests::killAllWindows();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
