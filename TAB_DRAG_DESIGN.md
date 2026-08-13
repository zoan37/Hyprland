# Native groupbar tab dragging

Working notes for the `tab-drag-native` branch, based on `v0.56.0`.

## The problem with the current behaviour

Dragging a groupbar tab is implemented as *dragging the window*:

`CHyprGroupBarDecoration::onBeginWindowDragOnDeco()`
(`src/render/decorations/CHyprGroupBarDecoration.cpp`)

1. works out which tab was grabbed from the cursor x,
2. `GROUP->remove(pWindow)` — tears it out of the group immediately,
3. `dragController()->dragBegin(..., MBIND_MOVE)` — starts a normal window drag.

So the moment you touch a tab, the window leaves the group, gets floated at
84.89% of its tiled size, and follows the cursor. Consequences:

- the dragged window covers the groupbar you are aiming at, so the drop
  position has to be guessed;
- the group relayouts twice (on tear-out and on drop) for what should be a
  reordering of a list;
- there is no feedback about where the tab will land, because the bar is
  hidden behind the window that left it;
- on `v0.56.0` the drop ignores position entirely and re-inserts after the
  active tab (`group:insert_after_current`). Upstream `9f777b13` makes the
  drop positional, but that only helps if you can see the bar.

None of this is what a tab drag should be. **Dragging a tab is not dragging a
window.** Chrome models these as two different gestures, and the tear-off only
happens when the pointer leaves the tab strip.

## Target behaviour

Follow the Chrome model:

- **Inside the bar** — the window never leaves the group and never floats.
  The tab order updates live as the cursor crosses the midpoint of a
  neighbouring tab. Releasing just ends the gesture. Because the bar redraws
  with the new order, the reordering *is* the feedback; no separate insertion
  indicator is needed.
- **Leaving the bar** — once the cursor moves outside the bar's band by more
  than a threshold, fall back to today's behaviour: remove from the group and
  hand off to a real window drag. That is the tear-off, and it is the only
  case where the window should float.

## Hooks that already exist

Verified against `v0.56.0`:

- **Claiming the gesture.** `CKeybindManager::changeMouseBindMode()`
  (`src/managers/KeybindManager.cpp:961`) calls
  `checkInputOnDecos(INPUT_TYPE_DRAG_START, ...)` and, if the decoration
  returns `true`, returns early **without** calling `beginDragTarget()`. So a
  decoration can already take a drag over from the window-drag machinery.
  Today the groupbar returns `true` and then starts a window drag itself;
  it can just as well start a tab drag instead.
- **Release.** Button events already reach decorations —
  `src/managers/input/InputManager.cpp:872` dispatches `INPUT_TYPE_BUTTON`,
  which the groupbar handles in `onMouseButtonOnDeco()`. The release edge of
  that event is the natural end of the gesture.
- **Reordering.** `CGroup` (`src/desktop/view/Group.hpp`) exposes
  `moveCurrent(bool next)`, `swapWithNext()`, `fromIndex(size_t)`, `size()`
  and holds the order in `m_windows`.
- **Hit geometry.** `onBeginWindowDragOnDeco()` already contains the maths for
  turning a cursor position into a tab index, for both horizontal and
  `groupbar:stacked` bars. Reuse it rather than reinventing it.

## The missing piece

`INPUT_TYPE_MOTION` exists in `eInputType` (`src/SharedDefs.hpp:38`) but is
**never dispatched** — nothing in `src/` passes it to `checkInputOnDecos`. It
is a stub. Live reordering needs pointer motion to reach the decoration, so
this branch has to add that dispatch in the pointer-motion path in
`CInputManager` (alongside the existing `INPUT_TYPE_BUTTON` and
`INPUT_TYPE_AXIS` sites), then handle it in
`CHyprGroupBarDecoration::onInputOnDeco()`, which currently falls through to
`default: return false`.

Adding the dispatch is a change other decorations can benefit from, and is
plausibly upstreamable on its own.

## Sketch

State on `CHyprGroupBarDecoration`:

```
struct {
    bool   active     = false;
    size_t index      = 0;   // tab currently being dragged
    double grabOffset = 0;   // cursor offset within that tab, to avoid jumping
} m_tabDrag;
```

- `onBeginWindowDragOnDeco(pos)` — if the group has more than one member,
  record `m_tabDrag` and return `true`. Do **not** remove from the group, do
  **not** call `dragBegin`.
- `onMotion(pos)` (new) — if `m_tabDrag.active`:
  - if the cursor is still within the bar band: compute the hovered index; if
    it differs from `m_tabDrag.index`, reorder `m_windows` and damage the
    decoration, then update `m_tabDrag.index`;
  - if it has left the band by more than a threshold: end the tab drag,
    `GROUP->remove()` and `dragBegin(MBIND_MOVE)` — the existing tear-off.
- `onMouseButtonOnDeco()` — on release with `m_tabDrag.active`, clear it.

Order of work:

1. dispatch `INPUT_TYPE_MOTION` and prove it arrives (log it);
2. live reorder inside the bar;
3. tear-off threshold;
4. optional polish: render the grabbed tab following the cursor rather than
   only reordering underneath it. Chrome does this; reordering alone may
   already feel right.

## Relationship to the other branch

`groupbar-tab-drag-0.56` holds two commits: the cherry-picked upstream
positional-drop fix and `binds:drag_scale`. Neither is needed for this design
— if the window never floats, there is nothing to shrink, and the drop
position is decided continuously rather than on release. `drag_scale` remains
worth keeping as an independent change for ordinary window drags.
