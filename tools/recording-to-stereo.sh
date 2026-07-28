#!/bin/sh
# 진단 녹음을 그냥 들어볼 수 있는 스테레오 파일로 바꾼다.
#
# 왜 필요한가: 앱이 남기는 녹음은 3~4채널 24비트다(기타 / 목소리 / 앱 출력 / 스피커 앞 소리).
# 분석에는 그래야 하지만 QuickTime·Finder 미리보기는 스테레오까지만 제대로 다뤄서
# "wav가 재생이 안 된다"가 된다. 왼쪽=앱 출력, 오른쪽=마이크로 들린 소리로 접어 준다.
#
# 사용법:
#   tools/recording-to-stereo.sh                 가장 최근 녹음
#   tools/recording-to-stereo.sh <파일.wav>      특정 파일
#
# 결과는 원본 옆에 "<이름>_들어보기.wav"로 저장된다(16비트 스테레오).

set -e

dir="$HOME/Library/VocalGuitarApp/recordings"
src=${1:-$(ls -t "$dir"/*.wav 2>/dev/null | head -1)}

if [ -z "$src" ] || [ ! -f "$src" ]; then
    echo "녹음 파일을 찾지 못했습니다: ${1:-$dir}"
    exit 1
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg가 필요합니다: brew install ffmpeg"
    exit 1
fi

channels=$(ffprobe -v error -select_streams a:0 -show_entries stream=channels -of csv=p=0 "$src")
out="${src%.wav}_들어보기.wav"

# ch0 기타 / ch1 목소리 / ch2 앱 출력 / ch3 스피커 앞 소리(있을 때)
# 오른쪽에는 "마이크로 들린 것"을 둔다 — 4채널이면 스피커 앞 마이크, 3채널이면 목소리 마이크.
case "$channels" in
    4) pan="pan=stereo|c0=c2|c1=c3"; right="스피커 앞 마이크(ch3)" ;;
    3) pan="pan=stereo|c0=c2|c1=c1"; right="목소리 마이크(ch1)" ;;
    2|1) pan=""; right="원본 그대로" ;;
    *) echo "예상 못 한 채널 수: $channels"; exit 1 ;;
esac

if [ -n "$pan" ]; then
    ffmpeg -hide_banner -loglevel error -y -i "$src" -af "$pan" -c:a pcm_s16le "$out"
else
    ffmpeg -hide_banner -loglevel error -y -i "$src" -c:a pcm_s16le "$out"
fi

echo "$(basename "$src") (${channels}채널) → $(basename "$out")"
echo "  왼쪽: 앱 출력    오른쪽: $right"
echo "$out"
