#!/usr/bin/env bash
set -e

IN="${1:-data/input.mp4}"
OUTDIR="${2:-data/segments}"
SEG="${3:-1}"   # seconds per segment

mkdir -p "$OUTDIR/hq" "$OUTDIR/lq"

# HQ
ffmpeg -y -i "$IN" -an -c:v libx264 -preset veryfast -crf 18 \
  -force_key_frames "expr:gte(t,n_forced*$SEG)" \
  -f segment -segment_time "$SEG" -reset_timestamps 1 \
  "$OUTDIR/hq/seg_%05d.mp4"

# LQ
ffmpeg -y -i "$IN" -an -c:v libx264 -preset veryfast -crf 32 -vf "scale=iw/2:ih/2" \
  -force_key_frames "expr:gte(t,n_forced*$SEG)" \
  -f segment -segment_time "$SEG" -reset_timestamps 1 \
  "$OUTDIR/lq/seg_%05d.mp4"

echo "[OK] Segments in $OUTDIR/{hq,lq}"
