# Native groupbar tab dragging

Notes for the `tab-drag-native` branch, based on `v0.56.0`.

## The problem

Dragging a groupbar tab was implemented as *dragging the window*:

`CHyprGroupBarDecoration::onBeginWindowDragOnDeco()`

1. works out which tab was grabbed from the cursor x,
2. `GROUP->remove(pWindow)` — tears it out of the group immediately,
3. `dragController()->dragBegin(..., MBIND_MOVE)` — starts a normal window drag.

So touching a tab made the window leave the group, float at 84.89% of its tiled
size, and follow the cursor. It also required holding SUPER, because that path is
only reachable from the `movewindow` mouse bind. Consequences:

- the dragged window covers the groupbar you are aiming at;
- the group relayouts twice for what should be a reordering of a list;
- no feedback about where the tab will land;
- on `v0.56.0` the drop ignores position entirely and re-inserts after the active
  tab (`group:insert_after_current`).

**Dragging a tab is not dragging a window.** Chrome models them as two gestures,
and the tear-off happens only when the pointer leaves the strip.

## What this branch does

Modifier-free tab dragging, in the spirit of `general:resize_on_border`: press a
tab and drag it along the bar, no keybind. The window never leaves the group and
never floats — the group's order is permuted as the pointer crosses tab
boundaries, so the tab visibly travels with the cursor and the reordering is its
own feedback. No separate insertion indicator is needed.

Gated by **`group:groupbar:drag_tabs`** (bool, default true).

### Why the entry point is the button path, not the drag path

`INPUT_TYPE_DRAG_START` only fires from `CKeybindManager::changeMouseBindMode`,
i.e. from a mouse *bind*, which by definition needs a modifier. Plain presses
already reach decorations through `CInputManager::processMouseDownNormal` →
`checkInputOnDecos(INPUT_TYPE_BUTTON, ...)` — that is how click-to-focus-a-tab
works today, and it is the same path `resize_on_border` uses. So the gesture is
armed from `onMouseButtonOnDeco` instead.

This leaves the existing SUPER+drag tear-out untouched: when a mouse bind
consumes the press, `processMouseDownNormal` returns before reaching the
decoration, so the two gestures cannot both arm.

### Shape

- **State** is static (`g_tabDrag` in the .cpp). The pointer is a singleton, so at
  most one tab can be dragged at a time. The bar geometry is *snapshotted* on
  press — reordering permutes tabs within the bar but never moves the bar — so no
  pointer to a decoration has to outlive the gesture. A window in the group
  closing mid-drag would otherwise be a use-after-free; instead the weak window
  ref simply expires and the gesture is dropped.
- **Arming** happens on left press over a tab (not padding), when
  `drag_tabs` is on and the group has more than one member. Nothing moves yet.
- **Motion** arrives via a new call in `CInputManager::mouseMoveUnified`, guarded
  by a static `tabDragArmed()` check so the common case costs one branch on the
  hottest path in the compositor. Past `TAB_DRAG_THRESHOLD` (4px) the gesture
  becomes a drag; below it, it stays a click.
- **Reordering** reuses `CGroup::swapWithNext/swapWithLast`, the same primitives
  behind the `movegroupwindow` dispatcher, stepping one slot at a time toward the
  hovered index. They move whichever window is *current* and follow it, so the
  dragged tab is made current first (the press already does this).
- **Off-axis band.** Straying further than `TAB_DRAG_BAND` (3×) the bar's
  thickness pauses the gesture rather than ending it — coming back resumes, and
  nothing is reverted. Without this, pressing a tab and moving down into the
  window would keep reordering on the horizontal component alone.
- **Release** is handled in `processMouseDownNormal` rather than in the
  decoration, because by then the pointer may have left the bar and the
  decoration would never see the event. A release that ended a real drag is
  swallowed; a release that was only ever a click falls through.

### Files

| file | change |
| --- | --- |
| `src/render/decorations/CHyprGroupBarDecoration.hpp` | static gesture API, `armTabDrag` |
| `src/render/decorations/CHyprGroupBarDecoration.cpp` | state, arm/update/end, press hook |
| `src/managers/input/InputManager.cpp` | motion hook, release hook |
| `src/config/values/ConfigValues.cpp` | `group:groupbar:drag_tabs` |

## Verification

Compile-clean, and `--verify-config` accepts an existing hyprlang config (this
branch is on `v0.56.0`, which still ships `src/config/legacy/`; note that
upstream `main` has removed legacy config support in `a9902ea6`, so a `main`-based
build cannot read a hyprlang config at all).

Behaviour was tested end to end against a nested Hyprland running this build,
driven by a real pointer through `zwlr_virtual_pointer_v1` — so events traverse
the compositor's normal input pipeline exactly as a physical mouse would. Three
windows were grouped and the group order read back over IPC after each gesture:

```
PASS  drag tab[0] -> slot 2 moves it to the end
PASS  drag tab[2] -> slot 0 moves it to the front
PASS  drag tab[0] -> slot 1 swaps the first two
PASS  plain click does not reorder
PASS  sub-threshold jitter does not reorder
PASS  leaving the bar band pauses reordering
PASS  drag_tabs = false disables dragging
PASS  drag_tabs = true re-enables it

8 passed, 0 failed
```

The harness (a small `zwlr_virtual_pointer_v1` client plus a driver script) lives
outside this tree. Porting it to `hyprtester` would be the right move before any
upstream submission; that harness can already synthesise clicks via
`hl.plugin.test.click`, but its group tests depend on `kitty`.

## Not done

- **Tear-off.** Dragging a tab off the bar does not detach the window into a
  floating drag the way Chrome does; the gesture just pauses. Tear-off still
  works the old way, with SUPER+drag, which this branch leaves alone.
- **Smooth motion.** The tab jumps a slot at a time rather than sliding under the
  cursor with the others parting around it.
- **Stacked bars** (`groupbar:stacked`) are handled in the arithmetic — the
  off-axis and along-axis roles swap — but were not tested.

## Relationship to the other branch

`groupbar-tab-drag-0.56` holds the cherry-picked upstream positional-drop fix and
`binds:drag_scale`. Neither is needed here: if the window never floats there is
nothing to shrink, and the position is decided continuously rather than on
release. `drag_scale` remains worth keeping as an independent change for ordinary
window drags.
