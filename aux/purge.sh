#!/bin/sh
set -eu
# Ask once unless already confirmed
if [ "${CONFIRMED:-0}" != "1" ] && [ -t 0 ]; then
    printf "This will delete installed files. Continue? [y/N] "
    read ans
    case "$ans" in
        y|Y|yes|YES)
            CONFIRMED=1
            export CONFIRMED
            ;;
        *)
            echo "Aborted."
            exit 1
            ;;
    esac
fi

: "${radioconfdir:?}"
: "${statedir:?}"
: "${hfdllibdir:?}"
: "${sysusersdir:?}"
: "${logrotatedir:?}"
: "${sbindir:?}"
: "${crondir:?}"
: "${bindir:?}"

case "$radioconfdir" in
    ""|"/"|".") echo "bad radioconfdir: '$radioconfdir'" >&2; exit 1 ;;
esac

case "$statedir" in
    ""|"/"|".") echo "bad statedir: '$statedir'" >&2; exit 1 ;;
esac

case "$hfdllibdir" in
    ""|"/"|".") echo "bad hfdllibdir: '$hfdllibdir'" >&2; exit 1 ;;
esac

rm -rf "$DESTDIR$radioconfdir" "$DESTDIR$statedir" "$DESTDIR$hfdllibdir"

d="$DESTDIR$sysusersdir"
[ -d "$d" ] && rm -f -- "$d/radio.conf"

d="$DESTDIR$logrotatedir"
if [ -d "$d" ]; then
   for f in $LOGROTATE_FILES; do
       rm -f -- "$d/$$f";
   done
fi   
d="$DESTDIR$hfdllibdir"
[ -d "$d" ] && rm -f -- "$d/systable.conf"

d="$DESTDIR$sbindir"
[ -d "$d" ] && rm -f -- "$d/start-hfdl"

d="$DESTDIR$crondir"
[ -d "$d" ] && rm -f -- "$d/ka9q-cleanups"

d="$DESTDIR$bindir"
if [ -d "$d" ]; then
    for f in $SCRIPTS; do
	rm -f -- "$d/$$f";
    done
fi

