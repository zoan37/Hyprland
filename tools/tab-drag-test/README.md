# Tab drag test harness

End-to-end tests for modifier-free groupbar tab dragging, run against a nested
Hyprland driven by a real pointer through `zwlr_virtual_pointer_v1`. Events
traverse the compositor's normal input pipeline, so a plain left press reaches
`CInputManager::processMouseDownNormal` exactly as a physical mouse would —
which is the whole point, since that is the path the feature hooks.

This is deliberately outside `hyprtester`: that harness can already synthesise
clicks via `hl.plugin.test.click`, but its group tests depend on `kitty`.
Porting these cases to it would be the right move before any upstream PR.

## Build the pointer driver

```sh
XML=../../protocols/wlr-virtual-pointer-unstable-v1.xml
wayland-scanner client-header $XML wlr-virtual-pointer-unstable-v1-client-protocol.h
wayland-scanner private-code  $XML wlr-virtual-pointer-unstable-v1-protocol.c
gcc -O2 -o vpointer vpointer.c wlr-virtual-pointer-unstable-v1-protocol.c \
    $(pkg-config --cflags --libs wayland-client)
```

## Run

Start a nested instance on the build under test, then group three windows in it:

```sh
HYPRLAND_HEADLESS_ONLY=1 ../../build/Hyprland --config "$PWD/tabdrag.conf" &
sleep 10

# ALWAYS pin the instance explicitly. Without this, hyprctl talks to whatever
# instance it feels like — which during development means your real session, and
# dispatchers like togglegroup act on whatever window happens to be focused.
hyprctl instances -j | jq -r '.[] | select(.pid != <your real session pid>) |
  "SIG=\(.instance)\nSOCK=\(.wl_socket)"' > demo.env
source demo.env
export HYPRLAND_INSTANCE_SIGNATURE=$SIG

for c in one two three; do hyprctl dispatch exec "alacritty --class $c"; sleep 2; done
hyprctl dispatch focuswindow class:one; hyprctl dispatch togglegroup
for c in three two; do hyprctl dispatch focuswindow class:$c; hyprctl dispatch movewindoworgroup r; sleep 1; done

./run-tabdrag-tests.sh
```

The script derives tab coordinates from the live monitor and window box, so it
survives the nested instance coming up at a different size. Hardcoding them
silently tests the wrong pixels.

## Cases covered

```
drag tab[0] -> slot 2 moves it to the end
drag tab[2] -> slot 0 moves it to the front
drag tab[0] -> slot 1 swaps the first two
plain click does not reorder
sub-threshold jitter does not reorder
leaving the bar band pauses reordering
drag_tabs = false disables dragging
drag_tabs = true re-enables it
```

## Testing the SUPER+drag tear-out

The old gesture needs a modifier, which needs a virtual keyboard. Shortcut: bind
`movewindow` to an unmodified right-drag, which reaches the identical
`INPUT_TYPE_DRAG_START` path, and drive it with `vpointer ... right`.

```sh
hyprctl keyword bindm ", mouse:273, movewindow"
(WAYLAND_DISPLAY=$SOCK ./vpointer 575 344 drag 287 26 300 250 20 400 right &) ; sleep 4
hyprctl clients -j | jq -r '.[] | "\(.class) grouped=\(.grouped|length) floating=\(.floating)"'
```

Check the state **mid-drag**: the torn-out window shows `grouped=0 floating=true`
while the rest of the group carries on. After release it may rejoin the group,
because dropping it back over the group's own window re-merges it —
`drag_into_group` doing its normal job, not a bug.

Note that `hyprctl dispatch mouse 1movewindow` does **not** work as a substitute:
`Actions::mouse` only acts when `m_passPressed` was set by a real bind press, so
the dispatcher is a no-op and the test silently proves nothing.
