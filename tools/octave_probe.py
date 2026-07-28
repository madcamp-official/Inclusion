"""출력 음정이 목소리 옥타브를 따라가는지 재는 합성 실험.

같은 기타 반주(E3 G3 A3 C4 반복)를 깔아 두고, 목소리 음역만 파일마다 바꿔 부른다.
출력의 절대 목표는 파일과 무관하게 같으므로, 어떤 음역에서 ±1200 cents 오차가
나타나면 그 음역에서 출력이 목소리를 따라간 것이다. 실제 노래로는 "같은 기타 음을
여러 옥타브로 부른다"를 통제할 수 없어서 합성으로 고정한다.

판정은 Mode2Offline이 출력하는 **출력 음정 정확도**(Harvest + StoneMask로 출력을 다시
분석해 기타 목표와 cents 비교)를 그대로 쓴다. 여기서 F0를 다시 구현하지 않는다 —
직접 짠 NSDF 분석기는 하향 시프트로 기음이 약해진 출력에서 배음을 잡아 옥타브 오류를
과다/과소 보고했다.

사용법:
  python3 tools/octave_probe.py /tmp/probe            # 음역별 WAV 생성
  for f in /tmp/probe/vocal_*.wav; do
      build/Mode2Offline_artefacts/Debug/Mode2Offline "$f" /tmp/out.wav \
          --octave=0 --gate=off --bleed=off | grep -E "입력 |±50|옥타브 오류"
  done
"""
import numpy as np
import os
import sys
import wave

SR = 48000
SECONDS = 4.0
# 파일별 목소리 중심음(MIDI). 저음부터 고음까지 훑어 어느 음역에서 무너지는지 본다.
VOCAL_CENTRES = [45, 50, 55, 60, 64, 69, 72, 76]
VOCAL_MELODY = [0, 2, 3, 2]          # 중심음 기준 반음 오프셋
VOCAL_NOTE_SEC = 0.5
# 기타는 음이 자주 바뀌어야 Mode2Offline이 정렬 지연을 실측할 수 있다.
GUITAR_PATTERN = [52, 55, 57, 60]    # E3 G3 A3 C4
GUITAR_NOTE_SEC = 0.5


def midi_to_hz(m):
    return 440.0 * 2.0 ** ((np.asarray(m, dtype=float) - 69.0) / 12.0)


def midi_name(m):
    names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    return f"{names[int(m) % 12]}{int(m) // 12 - 1}"


def make_guitar(total):
    out = np.zeros(total)
    n_note = int(GUITAR_NOTE_SEC * SR)
    for start in range(0, total, n_note):
        end = min(start + n_note, total)
        note = GUITAR_PATTERN[(start // n_note) % len(GUITAR_PATTERN)]
        t = np.arange(end - start) / SR
        f0 = float(midi_to_hz(note))
        env = np.exp(-3.0 * t)
        sig = sum(a * np.sin(2 * np.pi * f0 * h * t)
                  for h, a in [(1, 1.0), (2, 0.5), (3, 0.3), (4, 0.15), (5, 0.08)])
        out[start:end] = 0.25 * env * sig
    return out


def make_vocal(total, centre):
    """음절 엔벨로프 + 비브라토가 있는 멜로디.

    한 음을 길게 끌면 안 된다: 기타도 고정음이면 두 신호가 모두 완전 주기적이라
    분석이 현실과 다르게 낙관적으로 나온다.
    """
    n_note = int(VOCAL_NOTE_SEC * SR)
    i = np.arange(total)
    note = centre + np.array(VOCAL_MELODY)[(i // n_note) % len(VOCAL_MELODY)]
    t = i / SR
    f0 = midi_to_hz(note + 0.15 * np.sin(2 * np.pi * 5.0 * t))
    phase = np.cumsum(2 * np.pi * f0 / SR)
    syllable = 0.5 * (1 - np.cos(2 * np.pi * (t % 0.25) / 0.25))
    harmonics = (np.sin(phase) + 0.6 * np.sin(2 * phase) + 0.35 * np.sin(3 * phase)
                 + 0.2 * np.sin(4 * phase))
    return 0.18 * syllable * harmonics


def write_wav(path, guitar, vocal):
    data = np.stack([guitar, vocal, np.zeros(len(guitar))], axis=1)
    pcm = (np.clip(data, -0.99, 0.99) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(3)   # ch0 기타 / ch1 목소리 / ch2 (앱 녹음의 출력 자리)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    total = int(SECONDS * SR)
    guitar = make_guitar(total)
    for centre in VOCAL_CENTRES:
        name = f"vocal_{centre:02d}_{midi_name(centre)}.wav"
        path = os.path.join(out_dir, name)
        write_wav(path, guitar, make_vocal(total, centre))
        low, high = centre, centre + max(VOCAL_MELODY)
        print(f"{path}  목소리 {midi_name(centre)}~{midi_name(high)} "
              f"({float(midi_to_hz(low)):.0f}~{float(midi_to_hz(high)):.0f}Hz)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1])
