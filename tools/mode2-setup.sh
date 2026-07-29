#!/bin/sh
# 모드 2 오디오 세팅을 한 번에 끝낸다: 통합 기기 생성 → 앱 설정 파일 기록 → 앱 실행.
#
# 왜 있는가: 이 세팅을 손으로 하면 (1) 통합 기기 만들기 (2) 앱에서 기기 고르기
# (3) 기타/목소리 채널 번호 찾아서 고르기 세 단계를 매번 반복하게 되고, 채널 번호는
# 서브기기 순서에 따라 달라져서 결국 녹음해 보며 확인하게 된다. 채널 번호는 통합 기기를
# 만든 도구가 이미 알고 있으므로, 그 출력을 그대로 앱 설정 파일에 적어 넣으면 화면에서
# 고를 일이 없다.
#
# 사용법:
#   tools/mode2-setup.sh [프리셋]
#   tools/mode2-setup.sh --no-launch [프리셋]   앱은 실행하지 않는다
#
# 프리셋:
#   airpods   (기본) 출력=에어팟, 목소리=에어팟 마이크, 기타=Scarlett 악기 잭
#             전부 귀로만 듣는 구성. 하울링이 없다. 무선이라 왕복 80ms대라 연주감(타이밍)
#             평가에는 못 쓴다.
#   scarlett  출력=맥북 스피커, 목소리=Scarlett INPUT 1, 기타=Scarlett INPUT 2
#             불필요한 맥북 마이크를 통합 기기에서 빼서 지연을 줄인다. Rubber Band R3가
#             끊기지 않도록 버퍼는 안전한 256을 유지한다.
#   scarlett-youtube
#             scarlett과 같되 유튜브·영상 소리를 BlackHole로 **디지털로** 따로 받아 4채널로
#             녹음한다. 방 마이크와 달리 목소리·기타 생음이 안 섞여 반주만 깨끗하게 남는다.
#             시스템 기본 출력을 "Speakers + BlackHole"로 바꾸므로, 끝나면 시스템 설정 →
#             사운드에서 원래 출력으로 되돌리거나 다른 프리셋을 실행하면 된다.
#   scarlett-room
#             scarlett과 같되 맥북 내장 마이크를 방 마이크로 하나 더 붙여 4채널로 녹음한다.
#             스피커에서 실제로 나온 소리(앱 출력 + 유튜브 반주 + 기타 생음)가 한 채널에
#             통째로 남으므로, 노래를 부르지 않고 나중에 그대로 들어보며 판단할 수 있다.
#             내장 마이크가 들어오는 만큼 지연이 늘어나니 연주감 평가에는 scarlett을 쓴다.
#   builtin   출력=맥북 스피커, 목소리=맥북 내장 마이크, 기타=Scarlett 악기 잭
#             스피커로 들을 때 쓴다. 44.1kHz로 돌아 제어 갱신이 두 배 빠른 유일한 구성이다.
#             스피커+열린 마이크라 하울링 고리가 생긴다 — 앱의 "하울링 억제"를 켜고
#             출력 볼륨을 낮춰서 시작할 것.
#
# "출력=맥북 스피커 + 목소리=에어팟 마이크"는 만들 수 없다(speakers를 넣으면 이유를 찍고 멈춘다).
# 에어팟 마이크는 에어팟이 출력 기기이기도 할 때만 소리를 보낸다. 아니면 그 채널이 오류 없이
# 완전 무음이 된다 — 기기 목록에도 정상으로 보이고 채널 수도 맞아서 앱을 의심하게 된다.
#
# 에어팟 마이크를 쓰는 두 구성(airpods, speakers)은 **전체가 24kHz**로 돈다. 에어팟 마이크는
# HFP라 24kHz 전용이고, 통합 기기의 레이트는 마스터가 정하는데 블루투스는 레이트를 못 맞추면
# 오류 없이 무음이 되므로 마스터를 에어팟으로 잡기 때문이다(USB·내장 기기는 리샘플링으로
# 따라온다). 24kHz에서는 블록이 21.3ms로 길어져 제어 갱신이 절반이 된다.
# 마스터 선택은 채널 순서와 무관하다 — 출력 ch0-1은 여전히 맨 앞 기기가 차지한다.
#
# 자세한 배경은 docs/오디오-장치-설정.md 참고.

set -e
cd "$(dirname "$0")/.."

launch=1
if [ "$1" = "--no-launch" ]; then
    launch=0
    shift
fi

buffer_size=256
multiout=0
case "${1:-airpods}" in
    airpods)  device_name="Scarlett + AirPods";                  output="AirPods";     mic="AirPods";      room="MacBook Pro 마이크" ;;
    scarlett) device_name="Scarlett + Mac Speakers";             output="MacBook Pro"; mic="Scarlett Solo"; room=""; buffer_size=256 ;;
    scarlett-youtube)
              # 유튜브·영상 소리를 방 마이크가 아니라 **디지털로** 따로 받는다.
              # BlackHole(가상 오디오 기기)을 통합 기기의 입력으로 붙이고, 시스템 기본 출력을
              # "스피커 + BlackHole" 다중 출력 기기로 바꾼다. 그러면 유튜브 소리가 스피커로
              # 들리면서 동시에 BlackHole을 거쳐 앱 입력으로 들어온다.
              # 방 마이크와 달리 목소리·기타 생음이 안 섞여서 반주만 깨끗하게 남는다.
              # 맥북 스피커가 통합 기기와 다중 출력 기기 양쪽에 들어가지만 동시 사용된다(실측).
              device_name="Scarlett + Speakers + YouTube";       output="MacBook Pro 스피커"; mic="Scarlett Solo"; room="BlackHole"; multiout=1 ;;
    scarlett-room)
              # scarlett과 같되 맥북 내장 마이크를 방 마이크로 하나 더 붙인다. 처리에는 안 쓰고
              # 녹음에만 담긴다 — 앱 출력·유튜브 반주·기타 생음이 섞인 "실제로 들린 소리"를
              # 남겨야 나중에 노래하지 않고 그대로 다시 들어볼 수 있다.
              # 내장 마이크가 통합 기기에 들어오는 만큼 지연이 늘어나므로 연주감 평가에는 scarlett을 쓴다.
              device_name="Scarlett + Mac Speakers + Room";      output="MacBook Pro"; mic="Scarlett Solo"; room="MacBook Pro 마이크" ;;
    speakers)
        # 이 조합은 만들 수 없다. 앱 녹음으로 확인(2026-07-28): 출력이 맥북 스피커면
        # 목소리 채널이 정확히 0이고, 같은 기기에서 출력만 에어팟으로 바꾸면 -54dB로 살아난다.
        # 에어팟 마이크(HFP)는 에어팟이 출력 기기이기도 할 때만 소리를 보낸다.
        echo "이 조합은 동작하지 않습니다: 출력=맥북 스피커 + 목소리=에어팟 마이크"
        echo "  에어팟 마이크는 에어팟이 출력 기기일 때만 소리를 보냅니다(HFP)."
        echo "  출력이 다른 기기면 목소리 채널이 오류 없이 완전 무음(0)이 됩니다."
        echo
        echo "  스피커로 들으려면 : tools/mode2-setup.sh builtin   (목소리 = 내장 마이크)"
        echo "  에어팟 마이크를 쓰려면: tools/mode2-setup.sh airpods   (출력 = 에어팟)"
        exit 1 ;;
    # 내장 마이크가 이미 목소리 채널이라 같은 기기를 두 번 넣을 수 없다.
    builtin)  device_name="Guitar + Built-in Mic";               output="MacBook Pro"; mic="MacBook Pro";  room="" ;;
    *)
        echo "알 수 없는 프리셋: $1"
        echo "쓸 수 있는 값: airpods, scarlett, scarlett-room, scarlett-youtube, speakers, builtin"
        exit 1 ;;
esac
guitar="Scarlett Solo"

# 앱이 떠 있으면 먼저 닫는다. 앱은 종료할 때 자기 설정을 저장하므로, 켜 둔 채로 설정
# 파일을 고쳐 봐야 종료하는 순간 예전 값으로 덮어쓰인다.
if pgrep -qf "Vocal Guitar App"; then
    echo "실행 중인 앱을 닫습니다."
    osascript -e 'tell application "Vocal Guitar App" to quit' >/dev/null 2>&1 || true
    sleep 2
fi

cmake --build build --target Mode2AudioDevice >/dev/null
[ "$launch" -eq 1 ] && cmake --build build --target VocalGuitarApp >/dev/null

# 방 마이크(스피커 앞에서 실제 들리는 소리를 받는 마이크)를 입력으로 하나 더 붙인다.
# 처리에는 쓰지 않고 녹음에만 담긴다 — 앱 출력과 유튜브 반주가 섞인 "실제로 들린 소리"가
# 한 파일에 남아야 나중에 그대로 다시 들어볼 수 있다.
# 시스템(유튜브 등)의 소리를 갈라 받으려면 기본 출력을 "스피커 + BlackHole"로 바꿔야 한다.
# 이걸 안 하면 BlackHole 채널이 오류 없이 완전 무음이 된다 — 앱을 의심하게 되는 함정이다.
if [ "$multiout" -eq 1 ]; then
    build/Mode2AudioDevice create-multiout "Speakers + BlackHole" "$output" "BlackHole" >/dev/null
    build/Mode2AudioDevice default-output "Speakers + BlackHole"
    echo
fi

if [ "$mic" = "$guitar" ] && [ -n "$room" ]; then
    # Scarlett 하나에서 INPUT 1(마이크)과 INPUT 2(기타)를 받고, 방 마이크를 하나 더 붙인다.
    layout=$(build/Mode2AudioDevice create "$device_name" "$output" "$guitar" "$room")
elif [ "$mic" = "$guitar" ]; then
    # Scarlett 하나에서 INPUT 1(마이크)과 INPUT 2(기타)를 함께 받는다.
    # 같은 서브기기를 두 번 넣으면 안 되므로 한 번만 추가한다.
    layout=$(build/Mode2AudioDevice create "$device_name" "$output" "$guitar")
elif [ -n "$room" ]; then
    layout=$(build/Mode2AudioDevice create "$device_name" "$output" "$mic" "$guitar" "$room")
else
    layout=$(build/Mode2AudioDevice create "$device_name" "$output" "$mic" "$guitar")
fi
echo "$layout"

# 채널 번호는 도구가 찍어 준 배치에서 그대로 읽는다("입력 ch1-2 (앱: 채널 2-3) Scarlett...").
# 목소리는 마이크 기기의 첫 채널, 기타는 인터페이스의 마지막 채널이다
# (Scarlett Solo는 INPUT 1=XLR, INPUT 2=악기 잭이라 악기 잭이 뒤에 온다).
if [ "$mic" = "$guitar" ]; then
    vocal_ch=$(echo "$layout" | awk -v g="$guitar" '/입력 ch/ && index($0, g) > 0 { split($2, a, /ch|-/); print a[2]; exit }')
    guitar_ch=$(echo "$layout" | awk -v g="$guitar" '/입력 ch/ && index($0, g) > 0 { split($2, a, /ch|-/); print a[3]; exit }')
else
    vocal_ch=$(echo "$layout" | awk -v g="$guitar" '/입력 ch/ && index($0, g) == 0 { split($2, a, /ch|-/); print a[2]; exit }')
    guitar_ch=$(echo "$layout" | awk -v g="$guitar" '/입력 ch/ && index($0, g)  > 0 { split($2, a, /ch|-/); print a[3]; exit }')
fi
# 괄호로 필드를 자르면 안 된다 — 기기 이름 자체에 괄호가 들어간다("... (Mac Speakers)").
# "(48000Hz)" 형태만 정확히 집는다.
rate=$(echo "$layout" | sed -n 's/.*(\([0-9][0-9.]*\)Hz).*/\1/p' | head -1)
# 방 마이크는 이름으로 찾는다. 없으면 -1(앱에서 "없음").
if [ -n "$room" ]; then
    room_ch=$(echo "$layout" | awk -v r="$room" '/입력 ch/ && index($0, r) > 0 { split($2, a, /ch|-/); print a[2]; exit }')
fi
: "${room_ch:=-1}"

if [ -z "$vocal_ch" ] || [ -z "$guitar_ch" ] || [ -z "$rate" ]; then
    echo "채널 배치를 읽지 못했습니다. 위 출력을 보고 앱에서 직접 고르세요."
    exit 1
fi

settings_dir="$HOME/Library/VocalGuitarApp"
mkdir -p "$settings_dir"
cat > "$settings_dir/AudioDeviceSettings.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>

<DEVICESETUP deviceType="CoreAudio" audioOutputDeviceName="$device_name"
             audioInputDeviceName="$device_name" audioDeviceRate="$rate.0"
             audioDeviceBufferSize="$buffer_size"/>
XML
cat > "$settings_dir/Mode2ChannelMap.xml" <<XML
<?xml version="1.0" encoding="UTF-8"?>

<CHANNELMAP guitarChannel="$guitar_ch" vocalChannel="$vocal_ch" roomChannel="$room_ch"/>
XML

echo
echo "앱 설정을 적었습니다: 기기 \"$device_name\" ${rate}Hz, 버퍼 ${buffer_size},"
echo "  목소리 = 채널 $((vocal_ch + 1)), 기타 = 채널 $((guitar_ch + 1)) (화면 표시 기준)"
if [ "$room_ch" -ge 0 ] 2>/dev/null; then
    echo "  녹음에 함께 담을 채널 = 채널 $((room_ch + 1)) ($room)"
    echo "  → \"녹음 시작\"을 누르면 4채널로 남는다: 기타 / 목소리 / 앱 출력 / 스피커 앞 소리"
fi

if [ "$launch" -eq 1 ]; then
    open "build/VocalGuitarApp_artefacts/Debug/Vocal Guitar App.app"
    echo "앱을 실행했습니다. 화면 위쪽 기기 이름이 \"$device_name\"인지 확인하세요."
fi

# 음소거·볼륨 0 경고는 위쪽 기기 배치 출력에 섞여 있어서 놓치기 쉽다(실제로 놓쳐서
# "출력이 안 된다"를 앱 문제로 의심하며 한참 찾았다). 맨 끝에 다시 띄운다.
warnings=$(echo "$layout" | grep '!!' || true)
if [ -n "$warnings" ]; then
    echo
    echo "================ 소리가 안 날 이유가 이미 있습니다 ================"
    echo "$warnings"
    echo "시스템 설정 → 사운드에서 그 출력 기기를 골라 음소거를 풀고 볼륨을 올리세요."
    echo "메뉴 막대 볼륨은 *기본* 출력 기기에만 적용되므로 여기서는 소용없습니다."
    echo "=================================================================="
fi
