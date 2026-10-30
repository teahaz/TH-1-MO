# Trigger view, 2026-10-25

Found while trying the trigger view (built in a copy, `~/scratchpad/trigger-view/host`, then moved here).

- **Fixed: zoom in didn't seem to work, at least not after zooming out.** Zooming now
  starts from what the view shows (the stored zoom clamped between the whole packet and
  a tick over 8 columns), so every press changes the view even when the zoom was set on
  another packet; from FIT, zoom in starts at the whole packet. Zoom in also moved off
  Ctrl+I, which most terminals send as Tab (it never reached the button there); it is
  Ctrl+N now, and + and - as before. Checked on the bench in trigger and roll mode.
- **Fixed: arrow key navigation was not great.** The arrows now move by position: a lane,
  its settings cell to the right, the bands above and below, the controls along the
  bottom and back up to what is above them. The focus is CEL's own selection, drawn with
  CEL's defaults, and Enter and QuickStrike stay CEL's. CEL moves the selection only one
  widget up or down and a script can't set it, so for an arrow the script makes every
  other widget unselectable for one frame (`has_selection`): CEL's next frame can only
  select the target, and that frame's layout makes them selectable again (`nav` in
  `ui/cel/bus_replay.cel`). CEL is unchanged. Lanes take the selection too. On a lane,
  Enter or Space drives the cursor from it (left and right move it); Esc gives it back.
- **Fixed: Space works the same as Enter** on buttons and toggles, and on a lane. Pause
  is p or Ctrl+P (Space never was pause: CEL names the key "space", and the old binding
  was " ").
