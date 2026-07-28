# GPU 보컬 변환 작업 인수인계

작성일: 2026-07-28  
대상: 다음 작업자(Claude 포함)  
프로젝트: `C:\Users\User\kaist_madcamp\week4`

## 1. 핵심 요약

이 프로젝트의 사용자 보컬 생성은 두 방식을 시험했다.

| 방식 | 사용자별 학습 | 현재 판단 |
|---|---:|---|
| Seed-VC | 없음(Zero-shot) | 빠른 프로필 시험용. 25/50 step을 비교했지만 현재 사용자 음색 재현은 RVC보다 불리했음 |
| RVC | 있음 | 현재 Mode 1의 승인된 방식. 약 3분 13초 사용자 녹음으로 `rvc_user_long.pth`를 학습함 |

현재 Mode 1의 기준 모델은 서버의 다음 파일이다.

```text
/root/rvc-webui/assets/weights/rvc_user_long.pth
/root/rvc-webui/logs/rvc_user_long/added_IVF165_Flat_nprobe_1_rvc_user_long_v2.index
```

현재 승인된 결과는 RVC 입력 전에 로컬에서 원곡 가수의 표현을 약화하고, 사용자 음역·비브라토·에너지 특성을 넣은 뒤 GPU에서 음색 변환하는 구조다.

```text
원곡 분리 보컬
  -> 로컬 스타일/음역 전처리
  -> RVC용 WAV
  -> GPU 서버 RVC 추론
  -> 48 kHz 변환
  -> 키 앵커 및 표현 강도 변형
  -> Mode 1 song_package.json
```

Seed-VC는 사용자 모델을 “학습시킨” 것이 아니다. 사용자 녹음을 reference로 매번 넣는 zero-shot 추론이다. 실제 사용자별 학습을 수행한 것은 RVC뿐이다.

## 2. 보안 원칙

- 비밀번호, PEM/PPK 파일 내용, 개인 키를 이 문서나 Git에 넣지 않는다.
- 자동화 스크립트는 `ssh -o BatchMode=yes`를 사용하므로 SSH 키 인증이 준비되어 있어야 한다.
- 비밀번호 로그인이 필요하면 사람이 PowerShell에서 직접 입력하되, 코드나 명령 인자에 비밀번호를 쓰지 않는다.
- 사용자 원본 녹음, 분리한 원곡 보컬, 학습 모델과 대용량 렌더 WAV는 기본적으로 Git에 올리지 않는다.

## 3. GPU 서버 접속

### 3.1 확인된 서버

```text
KVPN 프로필: CAMP-49
SSH 주소: root@172.10.5.154
SSH 포트: 22
호스트명: camp-13
GPU: NVIDIA GeForce RTX 3090, 24 GB
드라이버: 580.173.02
```

`172.10.5.149`는 이 작업의 서버가 아니며 접속이 timeout 났다. 실제로 사용한 주소는 `172.10.5.154`다.

### 3.2 Windows PowerShell에서 접속

1. KCloudVPN을 실행하고 `CAMP-49`에 연결한다.
2. 일반 Windows PowerShell에서 다음 명령을 실행한다. 별도의 VM 내부 터미널이 아니다.

```powershell
ssh root@172.10.5.154 -p 22
```

키 파일을 별도로 받았다면:

```powershell
ssh -i "C:\path\to\given-key.pem" root@172.10.5.154 -p 22
```

자동화 가능 여부와 GPU 상태 확인:

```powershell
ssh -o BatchMode=yes root@172.10.5.154 "hostname && nvidia-smi"
```

정상이라면 `camp-13`과 RTX 3090 정보가 출력되어야 한다.

### 3.3 파일 전송

로컬에서 서버로:

```powershell
scp "C:\path\to\input.wav" root@172.10.5.154:/root/rvc-webui/input/input.wav
```

서버에서 로컬로:

```powershell
scp root@172.10.5.154:/root/rvc-webui/output.wav "C:\path\to\output.wav"
```

### 3.4 접속 문제 복구

#### `Connection timed out`

- KVPN `CAMP-49` 연결을 확인한다.
- IP가 `172.10.5.154`인지 확인한다.
- `ipconfig`에서 VPN TAP 어댑터가 활성화됐는지 확인한다.

#### 재부팅 직후 `Connection refused`

서버가 아직 올라오는 중이다. 1~3분 뒤 다음 명령을 다시 실행한다.

```powershell
ssh root@172.10.5.154 "hostname"
```

#### `Failed to initialize NVML: Driver/library version mismatch`

이 문제가 한 번 발생했고 서버 재부팅으로 해결했다.

```powershell
ssh root@172.10.5.154 "reboot"
```

연결이 끊기는 것은 정상이다. 서버가 다시 올라온 후:

```powershell
ssh root@172.10.5.154 "nvidia-smi"
```

재부팅은 다른 사용자가 GPU 작업 중이지 않은지 확인한 뒤 수행한다.

## 4. 서버 디렉터리

```text
/root/seed-vc       Seed-VC 설치 및 시험 결과
/root/rvc-webui     RVC 설치, 학습 데이터, 모델, 인덱스, 변환 결과
```

빠른 점검:

```powershell
ssh root@172.10.5.154 "du -sh /root/seed-vc /root/rvc-webui; nvidia-smi"
```

## 5. Seed-VC 작업

### 5.1 작업 성격

Seed-VC는 zero-shot voice conversion으로 사용했다. 별도 사용자 모델 학습 없이:

- `source`: 가이드 보컬
- `target`: 사용자 음성 reference
- `diffusion_steps`: 25 또는 50
- `f0_condition`: true

를 넣어 변환했다.

처음 사용한 짧은 사용자 녹음은 약 22초였고, 이후 약 3분 13초 녹음도 시험했다. 긴 reference 전체를 그대로 사용하지 않고 음질이 좋은 구간을 자동 선택하도록 로직을 추가했다.

### 5.2 로컬/서버 코드

로컬:

```text
external/seed-vc/server.py
external/seed-vc/reference_selector.py
external/seed-vc/seed_vc_wrapper.py
```

서버:

```text
/root/seed-vc/server.py
/root/seed-vc/reference_selector.py
/root/seed-vc/seed_vc_wrapper.py
```

`reference_selector.py`는 긴 사용자 녹음에서 비교적 깨끗한 약 20초 구간을 고른다.

- 최소 허용 길이: 5초
- 선호 길이: 20초
- 최대 길이: 25초
- 44.1 kHz로 정규화
- peak가 0.9를 넘지 않도록 정규화

### 5.3 직접 CLI 추론

서버에 이미 입력 파일이 올라가 있다는 가정:

```bash
cd /root/seed-vc

/usr/bin/time -v .venv/bin/python inference.py \
  --source input/guide_excerpt.wav \
  --target input/user_voice.wav \
  --output output/test25 \
  --diffusion-steps 25 \
  --f0-condition true \
  --auto-f0-adjust false \
  --fp16 true
```

50 step 비교:

```bash
cd /root/seed-vc

/usr/bin/time -v .venv/bin/python inference.py \
  --source input/guide_excerpt.wav \
  --target input/user_voice.wav \
  --output output/test50 \
  --diffusion-steps 50 \
  --f0-condition true \
  --auto-f0-adjust false \
  --fp16 true
```

실제 로그 기준으로 짧은 시험 구간은 다음 정도였다.

| 설정 | GPU 서버 wall time | RTF |
|---|---:|---:|
| 25 step | 약 18초 | 0.381 |
| 50 step | 약 20초 | 0.597 |

관련 서버 로그:

```text
/root/seed-vc/inference-25.log
/root/seed-vc/inference-50.log
/root/seed-vc/inference-new-profile.log
/root/seed-vc/inference-new-m6.log
/root/seed-vc/inference-new-m11.log
/root/seed-vc/cfg07.log
/root/seed-vc/cfg10.log
/root/seed-vc/cfg13.log
```

### 5.4 API 서버 실행

현재 프로세스가 항상 실행 중인 구조는 아니다. 필요할 때 서버 SSH 세션에서:

```bash
cd /root/seed-vc
.venv/bin/uvicorn server:app --host 0.0.0.0 --port 8000
```

다른 PowerShell에서:

```powershell
curl.exe http://172.10.5.154:8000/health
```

변환 예:

```powershell
curl.exe -X POST "http://172.10.5.154:8000/convert" `
  -F "source=@C:\path\to\guide.wav" `
  -F "target=@C:\path\to\user.wav" `
  -F "diffusion_steps=25" `
  -F "f0_condition=true" `
  --output "C:\path\to\seedvc_25.wav"
```

KVPN 내부에서 8000 포트 접근이 차단되면 SSH 포트 포워딩을 사용한다.

```powershell
ssh -L 8000:127.0.0.1:8000 root@172.10.5.154
```

그 뒤 URL은 `http://127.0.0.1:8000`을 사용한다.

### 5.5 Seed-VC 시험 결론

- 25/50 step 차이보다 reference 품질과 가이드 보컬의 성별·키·표현 영향이 더 크게 들렸다.
- 남성 사용자 녹음인데도 결과가 너무 여리거나 여성적으로 들리는 문제가 있었다.
- 원곡 가수/가이드의 표현 잔재가 강했다.
- 현재 Mode 1 기본 경로에서는 Seed-VC 대신 RVC + 스타일 전처리를 사용한다.
- 다만 신규 사용자의 “학습 없이 즉시 미리 듣기” 기능에는 여전히 후보가 될 수 있다.

## 6. RVC 사용자 모델 학습

### 6.1 사용한 사용자 녹음

두 데이터셋을 시험했다.

| 구분 | 원래 로컬 파일 | 서버 파일 | 길이 |
|---|---|---|---:|
| 짧은 녹음 | `C:\Users\User\Downloads\QR-[2026.07.27]-152029.wav` | `/root/rvc-webui/datasets/user_voice/user_voice.wav` | 약 21.96초 |
| 긴 녹음 | `C:\Users\User\Downloads\QR-[2026.07.27]-200755.wav` | `/root/rvc-webui/datasets/user_voice_long/user_voice_long.wav` | 약 192.96초 |

둘 다 서버에 48 kHz mono WAV로 저장됐다. 현재 승인된 모델은 긴 녹음으로 학습한 `rvc_user_long`이다.

원본 로컬 파일은 Git 관리 대상이 아니다. Claude가 재현할 때 해당 파일이 아직 존재하는지 먼저 확인해야 한다.

### 6.2 실제 학습 설정

RVC WebUI의 전처리 → F0/feature 추출 → 학습 → 인덱스 생성 순서로 실행했다.

```text
Experiment name: rvc_user_long
Version: v2
Target sample rate: 40k
Pitch guidance (F0): enabled
F0 method: RMVPE
GPU: 0
Precision: fp16
Batch size: 8
Total epochs: 100
Save every: 25 epochs
Pretrained G: assets/pretrained_v2/f0G40k.pth
Pretrained D: assets/pretrained_v2/f0D40k.pth
Cache dataset in GPU: enabled
```

주의: 저장된 설정 스냅샷 일부에는 batch size 4가 보이지만, 실제 `train-console.log`의 실행 기록은 batch size 8이다. 재현 기준은 8로 잡되 VRAM 문제가 생기면 4로 내린다.

실제 학습 시간:

```text
시작: 2026-07-27 11:15:50
완료: 2026-07-27 11:20:23
총 약 4분 33초 (RTX 3090)
```

체크포인트:

```text
25 epoch  -> rvc_user_long_e25_s200.pth
50 epoch  -> rvc_user_long_e50_s400.pth
75 epoch  -> rvc_user_long_e75_s600.pth
100 epoch -> rvc_user_long_e100_s800.pth
최종      -> rvc_user_long.pth
```

### 6.3 전처리 결과

- 192.96초 단일 WAV를 48개 학습 segment로 분할했다.
- RMVPE F0 추출: 47개 성공, 1개는 전 구간 pitch 0이라 skip.
- HuBERT feature 추출: 48개 성공.
- feature shape: `(6462, 768)`
- IVF cluster 수: 165
- pitch 0 segment는 `/root/rvc-webui/logs/rvc_user_long/0_gt_wavs/0_22.wav` 계열이었다.
- 한 segment skip은 치명적 오류가 아니며 최종 모델과 index가 정상 생성됐다.

### 6.4 학습 산출물

```text
/root/rvc-webui/assets/weights/rvc_user_long.pth
/root/rvc-webui/assets/weights/rvc_user_long_e25_s200.pth
/root/rvc-webui/assets/weights/rvc_user_long_e50_s400.pth
/root/rvc-webui/assets/weights/rvc_user_long_e75_s600.pth
/root/rvc-webui/assets/weights/rvc_user_long_e100_s800.pth

/root/rvc-webui/logs/rvc_user_long/added_IVF165_Flat_nprobe_1_rvc_user_long_v2.index
/root/rvc-webui/logs/rvc_user_long/trained_IVF165_Flat_nprobe_1_rvc_user_long_v2.index
/root/rvc-webui/logs/rvc_user_long/G_2333333.pth
/root/rvc-webui/logs/rvc_user_long/D_2333333.pth
```

짧은 데이터로 만든 구형 모델도 남아 있다.

```text
/root/rvc-webui/assets/weights/rvc_user.pth
/root/rvc-webui/assets/weights/rvc_user_e100_s400.pth
```

Mode 1에서 실수로 구형 `rvc_user.pth`를 선택하지 말고 `rvc_user_long.pth`를 사용한다.

### 6.5 동일한 방식으로 새 사용자를 학습하는 절차

RVC WebUI를 쓸 경우 서버에서 `/root/rvc-webui`의 기존 환경을 사용한다. 설치를 다시 할 필요는 없다.

1. 사용자 녹음을 mono WAV로 준비한다. 권장 3~5분, 무반주, 무리하지 않은 음역, clipping과 큰 잡음 없이 여러 모음/자음을 포함한다.
2. 서버에 사용자별 디렉터리로 업로드한다.

```powershell
ssh root@172.10.5.154 "mkdir -p /root/rvc-webui/datasets/USER_ID"
scp "C:\path\to\user.wav" root@172.10.5.154:/root/rvc-webui/datasets/USER_ID/user.wav
```

3. RVC WebUI의 Training 탭에서 다음 순서로 실행한다.

```text
실험명: 사용자별 고유 ID
40k / v2 / F0 사용
전처리
RMVPE F0 + HuBERT feature 추출 (GPU 0)
100 epoch, save every 25, batch 8
f0G40k.pth / f0D40k.pth 사용
feature index 생성
```

4. 25/50/75/100 epoch 음질을 같은 가이드 구간으로 비교한다.
5. 최종 선택한 `.pth`와 `added_*.index` 경로를 앱/manifest에 기록한다.

정확한 WebUI 내부 학습 shell 명령은 별도 스크립트로 보존되지 않았다. 위 값은 서버 학습 로그에서 복원한 실제 설정이다. 재현 시 RVC WebUI 단계를 사용하는 것이 가장 안전하다.

## 7. RVC 추론

### 7.1 서버의 추론 CLI

사용법:

```text
rvc_infer.py MODEL INPUT INDEX OUTPUT [PITCH_SHIFT]
```

현재 자동화에서 사용하는 실제 형태:

```bash
cd /root/rvc-webui

.venv/bin/python rvc_infer.py \
  rvc_user_long.pth \
  input/mode1_style_adapted.wav \
  logs/rvc_user_long/added_IVF165_Flat_nprobe_1_rvc_user_long_v2.index \
  output_mode1_style_adapted.wav \
  0
```

현재 파이프라인은 키를 RVC 전에 로컬 전처리하므로 마지막 pitch shift는 `0`이다. RVC CLI와 로컬 전처리에서 동시에 transpose하면 키가 이중 적용된다.

내부 주요 설정:

```text
F0 method: RMVPE
Index rate: 0.75
Output model sample rate: 40 kHz
```

앱용 파일은 추론 후 48 kHz로 resample한다.

### 7.2 로컬에서 단일 파일 자동 변환

프로젝트 루트의 Windows PowerShell에서:

```powershell
python tools\mode1_song_package\run_rvc_remote.py `
  --host root@172.10.5.154 `
  --input build\mode1\bansanka\style_adaptation\rvc_input_style_adapted.wav `
  --output build\mode1\bansanka\style_adaptation\bansanka_style_rvc_user.wav
```

이 스크립트는:

1. `scp`로 입력을 서버에 업로드하고,
2. 서버의 `rvc_infer.py`를 실행하고,
3. 결과를 다시 로컬로 다운로드한다.

기본 모델과 index는 이미 `rvc_user_long`으로 지정되어 있다. SSH key 인증이 안 되어 있으면 `BatchMode=yes`에서 실패한다.

### 7.3 표현 강도 변형 일괄 변환

```powershell
python tools\mode1_song_package\run_rvc_variants_remote.py `
  --host root@172.10.5.154 `
  --manifest build\mode1\bansanka\style_adaptation\style_variants.json
```

manifest의 각 `prepared_rvc_input`을 업로드하고 0/25/50/75/100 표현 강도 결과를 내려받는다. 실행 후 각 항목에 `converted_vocal` 경로가 추가된다.

## 8. 현재 승인된 Mode 1 보컬 제작 파이프라인

### 8.1 원곡 가수의 잔재를 줄이는 전처리

단순히 원곡 분리 보컬을 RVC에 넣으면 원곡 가수의 발성, 비브라토, 강세, 성별감이 많이 남았다. 그래서 `prepare_rvc_guide.py`를 추가했다.

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\prepare_rvc_guide.py `
  --user-voice "C:\path\to\user.wav" `
  --source-vocal "C:\path\to\isolated_vocal.wav" `
  --output-dir build\mode1\bansanka\style_adaptation `
  --style-strengths 0,25,50,75,100
```

이 단계는 로컬 CPU에서 다음을 한다.

- 사용자 편한 음역 분석 및 자동 key shift
- 사용자 녹음의 평균 비브라토 추출
- WORLD 기반 F0/energy 재합성
- 원곡의 미세 pitch 표현 약화
- 고음에서 energy 자동 감소
- 사용자 안전 음역 밖 음표 경고
- 표현 강도 0/25/50/75/100별 RVC 입력 생성

생성 파일:

```text
voice_profile.json
style_plan.json
adapted_f0.csv
style_variants.json
rvc_input_*.wav
```

### 8.2 현재 승인값

만찬가 기준:

```text
자동 base key: -17 semitones
기본 표현 강도: 25%
원곡 micro-pitch 유지량: 0.18
사용자 평균 vibrato rate: 4.688 Hz
적용 vibrato depth 상한: 30 cents
안전 음역 초과 시 감쇠: 0.8 dB/semitone, 최대 6 dB
사용자 안전 음역: F2-F3, 중심 A#2
최종 적응 보컬 음역: F2-F#3, 중심 C#3
```

키 앵커:

```text
-17, -12, -6, 0 semitones
```

각 키 앵커마다 표현 강도:

```text
0, 25, 50, 75, 100%
```

앱에서는 가장 가까운 사전 렌더 key anchor를 선택하고, 최대 약 3 semitone의 잔여 차이만 실시간 SoundTouch로 처리한다. 기본 UI 값은 25%다.

### 8.3 키 앵커 생성 예

예를 들어 -12 semitone 앵커:

```powershell
& .\external\seed-vc\venv\Scripts\python.exe `
  tools\mode1_song_package\prepare_rvc_guide.py `
  --user-voice "C:\path\to\user.wav" `
  --source-vocal "C:\path\to\isolated_vocal.wav" `
  --output-dir build\mode1\bansanka\key_anchors\m12 `
  --key-shift -12 `
  --style-strengths 0,25,50,75,100

python tools\mode1_song_package\run_rvc_variants_remote.py `
  --host root@172.10.5.154 `
  --manifest build\mode1\bansanka\key_anchors\m12\style_variants.json

python tools\mode1_song_package\resample_manifest_audio.py `
  --manifest build\mode1\bansanka\key_anchors\m12\style_variants.json `
  --sample-rate 48000
```

그 뒤 `add_key_anchor.py`로 `song_package.json`에 연결한다. -17/-12/-6/0을 같은 방식으로 만든다.

## 9. 현재 주요 로컬 산출물

```text
build/mode1/bansanka/style_adaptation/bansanka_style_rvc_user.wav
build/mode1/bansanka/style_adaptation/voice_profile.json
build/mode1/bansanka/style_adaptation/style_plan.json
build/mode1/bansanka/style_adaptation/adapted_f0.csv
build/mode1/bansanka/style_adaptation/style_variants.json
build/mode1/bansanka/key_anchors/
build/mode1/bansanka/song_package.json
```

대용량 WAV가 Git ignore되어 있거나 로컬에만 있을 수 있다. Claude는 코드가 있다고 산출물이 자동으로 존재한다고 가정하면 안 된다.

관련 문서/코드:

```text
docs/MODE1_STATUS_2026-07-28.md
tools/mode1_song_package/README.md
tools/mode1_song_package/prepare_rvc_guide.py
tools/mode1_song_package/run_rvc_remote.py
tools/mode1_song_package/run_rvc_variants_remote.py
tools/mode1_song_package/resample_manifest_audio.py
tools/mode1_song_package/add_key_anchor.py
external/seed-vc/server.py
external/seed-vc/reference_selector.py
```

## 10. 빠른 재현 순서

Claude가 이어서 작업할 때는 다음 순서가 가장 안전하다.

1. KVPN `CAMP-49` 연결.
2. GPU/SSH 확인.

```powershell
ssh -o BatchMode=yes root@172.10.5.154 "hostname && nvidia-smi"
```

3. 로컬 사용자 녹음과 원곡 분리 보컬 경로 확인.
4. `prepare_rvc_guide.py`로 키/스타일 변형 생성.
5. 생성된 `voice_profile.json`, `style_plan.json`을 먼저 검토.
6. `run_rvc_variants_remote.py`로 GPU RVC 변환.
7. 48 kHz로 resample.
8. `add_key_anchor.py`로 song package에 연결.
9. 앱에서 기본 표현 강도 25%, 자동 키 -17 결과 확인.
10. 원곡 가수 잔재, 자음 명료도, 고음의 얇아짐, 잡음/호흡 artifact를 청취 비교.

## 11. 문제 해결

### SSH 자동화가 즉시 실패

```powershell
ssh -o BatchMode=yes root@172.10.5.154 "echo ok"
```

이 명령이 실패하면 키 인증부터 해결한다. Python 스크립트 문제가 아니다.

### RVC 결과 키가 두 번 바뀜

`prepare_rvc_guide.py --key-shift`를 썼다면 `rvc_infer.py` 마지막 pitch 값은 `0`이어야 한다.

### RVC 결과가 원곡 가수를 너무 닮음

- 원곡 분리 보컬을 바로 넣지 않는다.
- `prepare_rvc_guide.py` 결과를 입력으로 쓴다.
- 표현 강도 기본 25%부터 비교한다.
- 사용자의 안전 음역으로 key를 낮춘다.

### 남성 음성이 너무 얇거나 여성적으로 들림

- 가이드의 원래 키가 사용자 음역보다 지나치게 높은지 확인한다.
- 현재 사용자 기준 자동값 `-17`과 `-12`를 우선 비교한다.
- 단순 RVC pitch shift보다 로컬 WORLD 전처리 후 RVC 변환 결과를 사용한다.
- 사용자 녹음에 힘 있는 중저음과 다양한 모음이 충분한지 확인한다.

### RVC 출력 sample rate가 앱과 다름

모델 출력은 40 kHz이고 앱은 48 kHz를 기대한다. `resample_manifest_audio.py --sample-rate 48000`을 반드시 거친다.

### CUDA out of memory

- 다른 GPU 프로세스를 `nvidia-smi`로 확인한다.
- 학습 batch size를 8에서 4로 내린다.
- 동시에 Seed-VC 서버와 RVC 학습을 실행하지 않는다.

### 학습 중 F0 segment 하나가 skip됨

전체가 무음/무성음이라 pitch가 0인 segment일 수 있다. 다른 feature 단계와 최종 index가 정상 생성됐다면 치명적이지 않다. skip 비율이 높으면 원본 녹음의 무음 제거와 노이즈 정리를 다시 한다.

## 12. 현재 결정과 다음 작업자 주의사항

- 현재 승인 기준은 `rvc_user_long.pth` + `added_IVF165...index`다.
- 최종 음질 판단은 단순 모델 epoch보다 가이드 전처리와 key 선택의 영향이 컸다.
- 현재 사용자에게는 만찬가 자동 key `-17`, 표현 강도 25%가 가장 좋다고 승인받았다.
- Seed-VC를 다시 시험하더라도 “사용자 학습”이라고 부르지 않는다.
- 같은 사용자의 RVC를 재학습하기 전에 현재 모델과 똑같은 평가 구간으로 A/B 비교한다.
- 원곡 분리 보컬이나 사용자 녹음을 Git에 추가하지 않는다.
- 서버 재부팅 전에 다른 작업자가 GPU를 쓰는지 확인한다.
- `docs/MODE1_STATUS_2026-07-28.md`의 기타 scheduler 관련 일부 내용은 이후 앱 수정으로 바뀌었을 수 있다. GPU 보컬 생성 상태는 이 문서를 기준으로 하고, 기타 진행 로직은 최신 C++ 코드와 테스트를 기준으로 판단한다.

## 13. 인수인계 체크리스트

- [ ] KVPN `CAMP-49`가 연결됨
- [ ] `root@172.10.5.154`에 SSH 접속 가능
- [ ] `hostname`이 `camp-13`
- [ ] `nvidia-smi`에서 RTX 3090 정상
- [ ] `/root/rvc-webui/assets/weights/rvc_user_long.pth` 존재
- [ ] `added_IVF165_Flat_nprobe_1_rvc_user_long_v2.index` 존재
- [ ] 로컬 사용자 원본 녹음 경로 확인
- [ ] 원곡 분리 보컬 경로 확인
- [ ] RVC 전처리 결과의 key와 사용자 안전 음역 확인
- [ ] RVC 입력에 pitch가 중복 적용되지 않음
- [ ] 출력이 48 kHz로 변환됨
- [ ] 기본 표현 강도 25% A/B 청취
- [ ] 대용량 음원과 개인 음성이 Git stage에서 제외됨

