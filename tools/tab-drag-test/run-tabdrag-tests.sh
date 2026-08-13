#!/usr/bin/env bash
# End-to-end tests for modifier-free groupbar tab dragging, driven through a real
# virtual pointer against a nested Hyprland running the patched build.
#
# Assumes the nested instance is already up with three grouped windows
# (classes tabA/tabB/tabC) and that sig.env holds its instance signature.

set -u
cd "$(dirname "$0")"
source demo.env
export HYPRLAND_INSTANCE_SIGNATURE=$SIG
export WAYLAND_DISPLAY_NESTED=${SOCK:-wayland-2}

# Geometry, derived at runtime from the monitor and the group's window box, so the
# tests survive the nested instance coming up at a different size.
EW=$(hyprctl monitors -j | jq -r '.[0] | (.width/.scale) | floor')
EH=$(hyprctl monitors -j | jq -r '.[0] | (.height/.scale) | floor')
WX=$(hyprctl clients -j | jq -r '[.[] | select((.grouped|length) > 1)][0].at[0]')
WY=$(hyprctl clients -j | jq -r '[.[] | select((.grouped|length) > 1)][0].at[1]')
WW=$(hyprctl clients -j | jq -r '[.[] | select((.grouped|length) > 1)][0].size[0]')
N=$(hyprctl clients -j | jq -r '[.[] | select((.grouped|length) > 1)][0].grouped | length')
GAPS_IN=2
# bar height = gaps_out*(1+keep_upper_gap) + indicator_height + indicator_gap + height
BAR_H=$(( 2*2 + 3 + 0 + 20 ))
BAR_Y=$(( WY - BAR_H/2 ))
BARW=$(( (WW - GAPS_IN*(N-1)) / N ))
TAB0=$(( WX + BARW/2 ))
TAB1=$(( WX + (BARW+GAPS_IN) + BARW/2 ))
TAB2=$(( WX + 2*(BARW+GAPS_IN) + BARW/2 ))
echo "geometry: extents=${EW}x${EH} bar_y=$BAR_Y tabs=$TAB0/$TAB1/$TAB2 (barw=$BARW n=$N)"

pass=0; fail=0

order() {
  hyprctl clients -j | jq -r '(.[0].grouped) as $o | [$o[] as $a | (map(select(.address==$a))[0].class)] | join(" ")'
}

setorder() { # force a known starting order by dragging, then assert it
  :
}

check() { # check <name> <expected> <actual>
  if [[ "$2" == "$3" ]]; then
    echo "PASS  $1"
    echo "        got: $3"
    pass=$((pass+1))
  else
    echo "FAIL  $1"
    echo "        expected: $2"
    echo "        got:      $3"
    fail=$((fail+1))
  fi
}

vp() { WAYLAND_DISPLAY=$WAYLAND_DISPLAY_NESTED ./vpointer $EW $EH "$@"; }

echo "starting order: $(order)"
echo

# 1. drag the first tab to the last slot
before=$(order)
first=$(echo "$before" | awk '{print $1}')
rest=$(echo "$before" | cut -d' ' -f2-)
vp drag $TAB0 $BAR_Y $TAB2 $BAR_Y 24 40 >/dev/null; sleep 1
check "drag tab[0] -> slot 2 moves it to the end" "$rest $first" "$(order)"

# 2. drag it back to the front
before=$(order)
last=$(echo "$before" | awk '{print $3}')
head2=$(echo "$before" | cut -d' ' -f1-2)
vp drag $TAB2 $BAR_Y $TAB0 $BAR_Y 24 40 >/dev/null; sleep 1
check "drag tab[2] -> slot 0 moves it to the front" "$last $head2" "$(order)"

# 3. drag the first tab one slot right
before=$(order)
read -r a b c <<< "$before"
vp drag $TAB0 $BAR_Y $TAB1 $BAR_Y 16 40 >/dev/null; sleep 1
check "drag tab[0] -> slot 1 swaps the first two" "$b $a $c" "$(order)"

# 4. a click, and a movement under the threshold, must not reorder
before=$(order)
vp drag $TAB0 $BAR_Y $TAB0 $BAR_Y 1 0 >/dev/null; sleep 1
check "plain click does not reorder" "$before" "$(order)"

before=$(order)
vp drag $TAB0 $BAR_Y $((TAB0+3)) $BAR_Y 3 30 >/dev/null; sleep 1
check "sub-threshold jitter does not reorder" "$before" "$(order)"

# 5. straying off the bar pauses the gesture: press on the bar, drop far below it,
#    then travel sideways. Nothing should move.
before=$(order)
vp drag2 $TAB0 $BAR_Y $TAB0 400 $TAB2 400 16 40 >/dev/null; sleep 1
check "leaving the bar band pauses reordering" "$before" "$(order)"

# 6. the config gate
hyprctl keyword group:groupbar:drag_tabs false >/dev/null; sleep 1
before=$(order)
vp drag $TAB0 $BAR_Y $TAB2 $BAR_Y 24 40 >/dev/null; sleep 1
check "drag_tabs = false disables dragging" "$before" "$(order)"

hyprctl keyword group:groupbar:drag_tabs true >/dev/null; sleep 1
before=$(order)
first=$(echo "$before" | awk '{print $1}')
rest=$(echo "$before" | cut -d' ' -f2-)
vp drag $TAB0 $BAR_Y $TAB2 $BAR_Y 24 40 >/dev/null; sleep 1
check "drag_tabs = true re-enables it" "$rest $first" "$(order)"

echo
echo "$pass passed, $fail failed"
exit $((fail > 0))
