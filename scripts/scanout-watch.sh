#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Live per-second view of how frames reached the screen, from wp_presentation
# flags in the newest framegen-vulkan pacing profile log.
#   direct = zero-copy: the compositor scanned the game's buffer out directly
#   copied = the compositor copied the frame first
#   torn   = no vsync bit (flipped immediately), vsync = waited for vblank
# Needs FGVK_PACING_PROFILE=1 (fgvk).
# Usage: scanout-watch.sh [log]
set -euo pipefail
# Newest log over 1 MB: the small ones written next to the game's are Wine helpers.
newest() {
    find /tmp -maxdepth 1 -name "fgvk-pacing-$(id -u)-*.log" \
        "$@" -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-
}
log="${1:-$(newest -size +1M)}"
[[ -n "$log" ]] || log="$(newest)"
[[ -n "$log" && -f "$log" ]] || { echo "no framegen-vulkan log found in /tmp" >&2; exit 1; }
echo "watching $log (Ctrl+C to stop)"
tail -n0 -F "$log" 2>/dev/null | gawk '
    /event=presented/ && match($0, /flags=([0-9]+)/, f) {
        flags = f[1] + 0
        n++
        if (and(flags, 8)) direct++
        if (and(flags, 1)) vsync++
    }
    {
        now = systime()
        if (now != last) {
            if (last && n)
                printf "%s  frames %4d  direct %3d%%  copied %3d%%  torn %3d%%  vsync %3d%%\n",
                    strftime("%H:%M:%S", last), n, 100*direct/n, 100*(n-direct)/n,
                    100*(n-vsync)/n, 100*vsync/n
            else if (last)
                printf "%s  no presented frames (window not visible, or probe off)\n", strftime("%H:%M:%S", last)
            fflush(); last = now; n = direct = vsync = 0
        }
    }'
