# l2ui.sh - shared terminal UI for l2 and l2install.
#
# POSIX sh (sourced by bash *and* dash).  Works three ways:
#   gum present + tty   -> gum menus/prompts, themed to the L2 palette
#   tty without gum     -> the same layout in plain ANSI (numbered menus)
#   pipe / CI / dumb    -> uncoloured, line oriented
# TERM=linux (the real console) or a non-UTF-8 locale switches every glyph to
# ASCII, because the Linux VT font cannot draw most box/bullet characters.
#
# Env: NO_COLOR=1, L2_PLAIN=1 (disable gum), L2_ASCII=1 (force ASCII glyphs)

UI_VERSION=2

ui_have() { command -v "$1" >/dev/null 2>&1; }

# ------------------------------------------------------------ capabilities
UI_COLOR=0
if { [ -t 1 ] || [ -t 2 ]; } && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
    UI_COLOR=1
fi
USE_GUM=0
if [ -t 0 ] && [ -t 1 ] && ui_have gum && [ -z "${L2_PLAIN:-}" ]; then USE_GUM=1; fi

UI_ASCII=0
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) ;;
    *) UI_ASCII=1 ;;
esac
[ "${TERM:-}" = linux ] && UI_ASCII=1
[ -n "${L2_ASCII:-}" ] && UI_ASCII=1

if [ "$UI_ASCII" = 1 ]; then
    G_OK='+' G_ERR='x' G_WRN='!' G_DOT='*' G_ARR='>' G_RUN='-' G_OFF='.'
    G_BF='#' G_BE='-' G_H='-' G_V='|' G_TL='+' G_TR='+' G_BL='+' G_BR='+'
    G_CUR='>' G_SEL='x' G_UNS=' ' G_SPIN='| / - \'
else
    G_OK='✓' G_ERR='✗' G_WRN='!' G_DOT='●' G_ARR='▶' G_RUN='◌' G_OFF='○'
    G_BF='█' G_BE='░' G_H='─' G_V='│' G_TL='╭' G_TR='╮' G_BL='╰' G_BR='╯'
    G_CUR='❯' G_SEL='✓' G_UNS='·' G_SPIN='⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏'
fi

# palette (xterm-256): L2 sky, accent pink, dim, ok, warn, err, text
C_ACC=117 C_PNK=212 C_DIM=244 C_OK=114 C_WRN=214 C_ERR=203 C_FG=252

fg()  { if [ "$UI_COLOR" = 1 ]; then printf '\033[38;5;%sm' "$1"; fi; return 0; }
bld() { if [ "$UI_COLOR" = 1 ]; then printf '\033[1m'; fi; return 0; }
rst() { if [ "$UI_COLOR" = 1 ]; then printf '\033[0m'; fi; return 0; }
paint() { fg "$1"; printf '%s' "$2"; rst; }

ui_width() {
    _w=${COLUMNS:-}
    if [ -z "$_w" ] && ui_have tput; then _w=$(tput cols 2>/dev/null || true); fi
    case "$_w" in ''|*[!0-9]*) _w=80 ;; esac
    [ "$_w" -gt 100 ] && _w=100
    [ "$_w" -lt 40 ] && _w=40
    printf '%s' "$_w"
}

# ---------------------------------------------------------------- messages
say()  { printf '  %s %s\n' "$(paint $C_ACC "$G_DOT")" "$*" >&2; }
note() { printf '    %s\n' "$(paint $C_DIM "$*")" >&2; }
ok()   { printf '  %s %s\n' "$(paint $C_OK "$G_OK")" "$*" >&2; }
warn() { printf '  %s %s\n' "$(paint $C_WRN "$G_WRN")" "$(paint $C_WRN "$*")" >&2; }
step() { printf '\n%s %s\n' "$(paint $C_PNK "$G_ARR")" "$(bld; paint $C_FG "$*"; rst)" >&2; }
die()  {
    if command -v job_kill >/dev/null 2>&1; then job_kill; fi
    printf '  %s %s\n' "$(paint $C_ERR "$G_ERR")" "$(paint $C_ERR "$*")" >&2
    exit 1
}

ui_rule() {
    _n=$(ui_width); _n=$((_n - 4)); _s=""
    while [ "$_n" -gt 0 ]; do _s="$_s$G_H"; _n=$((_n - 1)); done
    printf '  %s\n' "$(paint $C_DIM "$_s")"
}

# key / value line:  ui_kv "kernel" "7.3.3"
ui_kv() { printf '  %s %s\n' "$(paint $C_DIM "$(printf '%-12s' "$1")")" "$2"; }

# state glyph + label:  ui_state ok|warn|off|err "text"
ui_state() {
    case "$1" in
        ok)   printf '%s %s' "$(paint $C_OK "$G_OK")" "$2" ;;
        warn) printf '%s %s' "$(paint $C_WRN "$G_WRN")" "$2" ;;
        err)  printf '%s %s' "$(paint $C_ERR "$G_ERR")" "$2" ;;
        *)    printf '%s %s' "$(paint $C_DIM "$G_OFF")" "$(paint $C_DIM "$2")" ;;
    esac
}

# ui_bar VALUE MAX [WIDTH]  ->  ▕█████░░░░░▏
ui_bar() {
    _v=$1; _m=$2; _bw=${3:-24}
    [ "$_m" -gt 0 ] 2>/dev/null || _m=1
    _f=$((_v * _bw / _m)); [ "$_f" -gt "$_bw" ] && _f=$_bw
    _col=$C_OK
    [ $((_v * 100 / _m)) -ge 80 ] && _col=$C_WRN
    [ "$_v" -gt "$_m" ] && _col=$C_ERR
    _out=""; _i=0
    while [ "$_i" -lt "$_bw" ]; do
        if [ "$_i" -lt "$_f" ]; then _out="$_out$G_BF"; else _out="$_out$G_BE"; fi
        _i=$((_i + 1))
    done
    fg "$_col"; printf '%s' "$_out"; rst
}

# bytes -> "6.8 MiB"
ui_size() {
    _b=${1:-0}
    if [ "$_b" -ge 1048576 ]; then
        _t=$((_b * 10 / 1048576)); printf '%s.%s MiB' $((_t / 10)) $((_t % 10))
    elif [ "$_b" -ge 1024 ]; then
        _t=$((_b * 10 / 1024)); printf '%s.%s KiB' $((_t / 10)) $((_t % 10))
    else printf '%s B' "$_b"; fi
}

ui_secs() {
    _s=${1:-0}
    if [ "$_s" -ge 60 ]; then printf '%sm%02ds' $((_s / 60)) $((_s % 60)); else printf '%ss' "$_s"; fi
}

# -------------------------------------------------------------------- logo
# The L2 mark with a blue -> pink gradient.  Extra lines (arguments) are
# printed to the right of it, so the banner doubles as an info header.
ui_logo() {
    if [ "$UI_ASCII" = 1 ]; then
        _l1=' _      ____  '; _l2='| |    |___ \ '; _l3='| |      __) |'
        _l4='| |___  / __/ '; _l5='|_____||_____|'; _l6=''
    else
        _l1=' ██╗     ██████╗ '; _l2=' ██║     ╚════██╗'; _l3=' ██║      █████╔╝'
        _l4=' ██║     ██╔═══╝ '; _l5=' ███████╗███████╗'; _l6=' ╚══════╝╚══════╝'
    fi
    _i=1
    for _g in 117 111 105 141 177 212; do
        eval "_ln=\$_l$_i"
        if [ "$UI_ASCII" = 1 ]; then _pad=14; else _pad=17; fi
        _txt=""
        if [ $# -ge "$_i" ]; then
            eval "_txt=\${$_i}"
        fi
        printf '%s%s  %s\n' "$(paint "$_g" "$(printf "%-${_pad}s" "$_ln")")" "" "$_txt"
        _i=$((_i + 1))
    done
}

# ----------------------------------------------------------------- panels
ui_panel() {
    if [ "$USE_GUM" = 1 ]; then
        gum style --border rounded --border-foreground "$C_ACC" --padding "0 2" \
            --margin "0 1" "$@"
        return
    fi
    _pw=0
    for _l in "$@"; do [ "${#_l}" -gt "$_pw" ] && _pw=${#_l}; done
    _h=""; _k=$((_pw + 2))
    while [ "$_k" -gt 0 ]; do _h="$_h$G_H"; _k=$((_k - 1)); done
    printf '  %s%s%s\n' "$(paint $C_ACC "$G_TL")" "$(paint $C_ACC "$_h")" "$(paint $C_ACC "$G_TR")"
    for _l in "$@"; do
        printf '  %s %-*s %s\n' "$(paint $C_ACC "$G_V")" "$_pw" "$_l" "$(paint $C_ACC "$G_V")"
    done
    printf '  %s%s%s\n' "$(paint $C_ACC "$G_BL")" "$(paint $C_ACC "$_h")" "$(paint $C_ACC "$G_BR")"
}

# CSV on stdin ("A,B,C" header first) -> table
ui_table() {
    if [ "$USE_GUM" = 1 ]; then
        gum table --separator ',' --print --border rounded \
            --border.foreground "$C_DIM" --header.foreground "$C_PNK"
    else
        _hdr=1
        while IFS=, read -r _a _b _c _d; do
            if [ "$_hdr" = 1 ]; then
                printf '  %s\n' "$(paint $C_PNK "$(printf '%-16s %-12s %-14s %s' "$_a" "$_b" "$_c" "$_d")")"
                _hdr=0
            else
                printf '  %-16s %-12s %-14s %s\n' "$_a" "$_b" "$_c" "$_d"
            fi
        done
    fi
}

ui_pager() {
    if [ "$USE_GUM" = 1 ] && [ -t 1 ]; then gum pager < "$1" || cat "$1"
    elif ui_have less && [ -t 1 ]; then less -R "$1"
    else cat "$1"; fi
}

# ---------------------------------------------------------------- prompts
ui_choose() {
    _h=$1; shift
    if [ "$USE_GUM" = 1 ]; then
        gum choose --header "$_h" --height 16 --cursor "$G_CUR " \
            --cursor.foreground "$C_PNK" --header.foreground "$C_ACC" \
            --item.foreground "$C_FG" --selected.foreground "$C_PNK" "$@"
    else
        _i=1
        printf '\n  %s\n' "$(paint $C_ACC "$_h")" >&2
        for _x in "$@"; do printf '   %s %s\n' "$(paint $C_PNK "$(printf '%2d' "$_i")")" "$_x" >&2; _i=$((_i + 1)); done
        printf '  %s ' "$(paint $C_PNK "$G_CUR")" >&2
        read -r _n || return 1
        _i=1
        for _x in "$@"; do
            [ "$_i" = "$_n" ] && { printf '%s\n' "$_x"; return 0; }
            _i=$((_i + 1))
        done
        return 1
    fi
}

# ui_menu HEADER "key|label" ...   -> prints the chosen key
ui_menu() {
    _h=$1; shift
    _labels=""
    # labels are passed to ui_choose one per argument; keep a key lookup list
    _nl='
'
    for _e in "$@"; do _labels="$_labels${_e#*|}$_nl"; done
    _pick=$(
        IFS=$_nl
        # shellcheck disable=SC2086
        set -- $_labels
        ui_choose "$_h" "$@"
    ) || return 1
    for _e in "$@"; do
        if [ "${_e#*|}" = "$_pick" ]; then printf '%s\n' "${_e%%|*}"; return 0; fi
    done
    return 1
}

ui_multi() {
    _h=$1; shift
    if [ "${YES:-0}" = 1 ]; then printf '%s\n' "$@"; return 0; fi
    if [ "$USE_GUM" = 1 ]; then
        gum choose --no-limit --header "$_h" --height 16 \
            --cursor-prefix "$G_UNS " --selected-prefix "$G_SEL " --unselected-prefix "$G_UNS " \
            --cursor.foreground "$C_PNK" --header.foreground "$C_ACC" \
            --selected.foreground "$C_PNK" "$@"
    else
        _i=1
        printf '\n  %s %s\n' "$(paint $C_ACC "$_h")" "$(paint $C_DIM '(numbers, space separated; empty = all)')" >&2
        for _x in "$@"; do printf '   %s %s\n' "$(paint $C_PNK "$(printf '%2d' "$_i")")" "$_x" >&2; _i=$((_i + 1)); done
        printf '  %s ' "$(paint $C_PNK "$G_CUR")" >&2
        read -r _n || { printf '%s\n' "$@"; return 0; }
        if [ -z "$_n" ]; then printf '%s\n' "$@"; return 0; fi
        for _k in $_n; do
            _i=1
            for _x in "$@"; do
                [ "$_i" = "$_k" ] && printf '%s\n' "$_x"
                _i=$((_i + 1))
            done
        done
    fi
}

ui_confirm() {
    if [ "${YES:-0}" = 1 ]; then return 0; fi
    if [ "$USE_GUM" = 1 ]; then
        gum confirm --affirmative "Yes" --negative "No" \
            --prompt.foreground "$C_ACC" --selected.background "$C_PNK" \
            --selected.foreground 0 "$1"
    else
        printf '  %s %s [y/N] ' "$(paint $C_PNK '?')" "$1" >&2
        read -r _a || return 1
        case "$_a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
    fi
}

# destructive confirmation: default answer is No
ui_confirm_danger() {
    if [ "${YES:-0}" = 1 ]; then return 0; fi
    if [ "$USE_GUM" = 1 ]; then
        gum confirm --default=false --affirmative "Erase" --negative "Cancel" \
            --prompt.foreground "$C_ERR" --selected.background "$C_ERR" \
            --selected.foreground 0 "$1"
    else
        printf '  %s %s [y/N] ' "$(paint $C_ERR '!')" "$1" >&2
        read -r _a || return 1
        case "$_a" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
    fi
}

ui_input() {  # header placeholder [value]
    if [ "$USE_GUM" = 1 ]; then
        gum input --header "$1" --placeholder "${2:-}" --value "${3:-}" \
            --header.foreground "$C_ACC" --prompt.foreground "$C_PNK" \
            --cursor.foreground "$C_PNK" --width 60
    else
        printf '  %s %s%s: ' "$(paint $C_PNK '?')" "$1" "${3:+ [$3]}" >&2
        read -r _v || return 1
        printf '%s\n' "${_v:-${3:-}}"
    fi
}

ui_password() {
    if [ "$USE_GUM" = 1 ]; then
        gum input --password --header "$1" --header.foreground "$C_ACC" \
            --prompt.foreground "$C_PNK" --width 60
    else
        printf '  %s %s: ' "$(paint $C_PNK '?')" "$1" >&2
        if [ -t 0 ]; then stty -echo 2>/dev/null || true; fi
        read -r _v || true
        if [ -t 0 ]; then stty echo 2>/dev/null || true; fi
        printf '\n' >&2
        printf '%s\n' "$_v"
    fi
}

# gum spin only wraps external commands; used for short downloads
ui_spin() {
    _t=$1; shift
    if [ "$USE_GUM" = 1 ]; then
        gum spin --spinner dot --title "$_t" --show-error \
            --spinner.foreground "$C_PNK" --title.foreground "$C_FG" -- "$@"
    else
        say "$_t"
        "$@"
    fi
}

# ------------------------------------------------------------ job ticker
# job_begin "title" [logfile]   ...foreground work...   job_end rc
# A background ticker draws a spinner, the elapsed time and the last line of
# the log, so a 20 minute kernel build is not a silent screen.
UI_JOB_PID="" UI_JOB_T0=0 UI_JOB_TITLE=""

job_begin() {
    UI_JOB_TITLE=$1; _log=${2:-}; UI_JOB_T0=$(date +%s)
    if [ -t 2 ] && [ "$UI_COLOR" = 1 ]; then
        (
            _fr=$G_SPIN; _cols=$(ui_width)
            while :; do
                for _f in $_fr; do
                    _now=$(date +%s); _el=$((_now - UI_JOB_T0))
                    _last=""
                    if [ -n "$_log" ] && [ -f "$_log" ]; then
                        _cw=$((_cols - ${#UI_JOB_TITLE} - 18))
                        [ "$_cw" -lt 1 ] && _cw=1
                        _last=$(tail -n 1 "$_log" 2>/dev/null | tr -cd '[:print:]' | cut -c1-"$_cw")
                    fi
                    printf '\r\033[K  %s %s %s %s' "$(paint $C_PNK "$_f")" "$UI_JOB_TITLE" \
                        "$(paint $C_DIM "$(ui_secs "$_el")")" "$(paint $C_DIM "$_last")" >&2
                    sleep 0.2 2>/dev/null || sleep 1
                done
            done
        ) &
        UI_JOB_PID=$!
    else
        printf '  %s %s ...\n' "$(paint $C_PNK "$G_RUN")" "$UI_JOB_TITLE" >&2
    fi
}

job_kill() {
    if [ -n "${UI_JOB_PID:-}" ]; then
        kill "$UI_JOB_PID" 2>/dev/null || true
        wait "$UI_JOB_PID" 2>/dev/null || true
        UI_JOB_PID=""
        printf '\r\033[K' >&2
    fi
}

job_end() {  # rc [detail]
    job_kill
    _el=$(( $(date +%s) - UI_JOB_T0 ))
    if [ "${1:-0}" = 0 ]; then
        printf '  %s %s %s %s\n' "$(paint $C_OK "$G_OK")" "$UI_JOB_TITLE" \
            "$(paint $C_DIM "$(ui_secs "$_el")")" "$(paint $C_DIM "${2:-}")" >&2
    else
        printf '  %s %s %s\n' "$(paint $C_ERR "$G_ERR")" "$UI_JOB_TITLE" \
            "$(paint $C_DIM "$(ui_secs "$_el")")" >&2
    fi
}

# last N lines of a log in a red-bordered box, for failed builds
ui_logtail() {
    [ -f "$1" ] || return 0
    printf '\n  %s %s\n' "$(paint $C_ERR "$G_ERR")" "$(paint $C_ERR "last ${2:-40} lines of $1")" >&2
    tail -n "${2:-40}" "$1" | sed 's/^/    /' >&2
    printf '\n' >&2
}
