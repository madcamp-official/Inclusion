#!/bin/sh
# 진단 녹음을 그냥 들어볼 수 있는 스테레오 파일로 바꾼다.
#
# 왜 필요한가: 앱이 남기는 녹음은 3~4채널 24비트다(기타 / 목소리 / 앱 출력 / 4번째 채널).
# 4번째 채널은 프리셋에 따라 다르다 — scarlett-room이면 스피커 앞 마이크, scarlett-youtube면
# 유튜브 소리다. 분석에는 4채널이 필요하지만 QuickTime·Finder는 스테레오까지만 제대로
# 다뤄서 "wav가 재생이 안 된다"가 된다.
#
# 기본은 **듣기용 믹스**다: 보정된 목소리(앱 출력) + 4번째 채널만 섞는다. 기타 라인과
# 목소리 원본은 빼므로, 스피커 앞에서 들리던 것에 가깝게 들린다.
#
# 사용법:
#   tools/recording-to-stereo.sh                 가장 최근 녹음 (믹스)
#   tools/recording-to-stereo.sh --split         왼쪽=앱 출력, 오른쪽=4번째 채널로 분리
#   tools/recording-to-stereo.sh <파일.wav>      특정 파일
#
# 결과는 원본 옆에 "<이름>_들어보기.wav"로 저장된다(16비트 스테레오).

set -e

split=0
if [ "$1" = "--split" ]; then
    split=1
    shift
fi

dir="$HOME/Library/VocalGuitarApp/recordings"
# 자동 선택에서 이 스크립트가 만든 결과물은 뺀다. 안 그러면 가장 최근 파일이 직전 변환
# 결과라서 그걸 또 변환하고("..._들어보기_들어보기.wav"), 원본 4채널은 건드리지도 않는다.
src=${1:-$(ls -t "$dir"/*.wav 2>/dev/null | grep -v '_들어보기\.wav$' | head -1)}

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

# ch0 기타 / ch1 목소리 / ch2 앱 출력 / ch3 4번째 채널(있을 때)
# 기본(믹스)은 들을 것만 남긴다: 앱 출력 + 4번째 채널. 기타 라인·목소리 원본은 넣지 않는다.
# 0.8을 곱하는 건 둘을 더할 때 피크가 넘치지 않게 하기 위함이다.
if [ "$split" -eq 1 ]; then
    case "$channels" in
        4) pan="pan=stereo|c0=c2|c1=c3"; desc="왼쪽: 앱 출력    오른쪽: 4번째 채널(ch3)" ;;
        3) pan="pan=stereo|c0=c2|c1=c1"; desc="왼쪽: 앱 출력    오른쪽: 목소리 마이크(ch1)" ;;
        2|1) pan=""; desc="원본 그대로" ;;
        *) echo "예상 못 한 채널 수: $channels"; exit 1 ;;
    esac
else
    case "$channels" in
        4) pan="pan=stereo|c0=0.8*c2+0.8*c3|c1=0.8*c2+0.8*c3"
           desc="앱 출력(보정된 목소리) + 4번째 채널을 섞음. 기타 라인·목소리 원본은 뺐음" ;;
        3) pan="pan=stereo|c0=c2|c1=c2"; desc="앱 출력만 (4번째 채널이 없는 녹음)" ;;
        2|1) pan=""; desc="원본 그대로" ;;
        *) echo "예상 못 한 채널 수: $channels"; exit 1 ;;
    esac
fi

if [ -n "$pan" ]; then
    ffmpeg -hide_banner -loglevel error -y -i "$src" -af "$pan" -c:a pcm_s16le "$out"
else
    ffmpeg -hide_banner -loglevel error -y -i "$src" -c:a pcm_s16le "$out"
fi

echo "$(basename "$src") (${channels}채널) → $(basename "$out")"
echo "  $desc"
echo "$out"
