// macOS 통합 오디오 기기(Aggregate Device) 생성·삭제 도구.
//
// 왜 필요한가: 모드 2는 기타와 목소리를 서로 다른 하드웨어에서 동시에 받아야 한다.
// 기타는 오디오 인터페이스(예: Scarlett Solo의 악기 잭), 목소리는 다른 마이크(에어팟 등)로
// 들어오는 게 보통인데, CoreAudio는 한 번에 기기 하나만 열 수 있다. 입력과 출력을 다른
// 기기로 잡으면 클럭이 달라 JUCE가 아예 열지 못하고 기본 기기로 폴백한다(실측 확인).
// 그래서 필요한 기기들을 통합 기기 하나로 묶어야 한다.
//
// 이 작업은 "오디오 MIDI 설정" 앱에서 손으로도 되지만, 손으로 만들면 기기 구성이 사람과
// 시점마다 달라져 채널 번호가 어긋난다. 여기에 남겨서 같은 구성을 언제든 똑같이 재현한다.
//
// 서브기기 "순서가 곧 채널 순서"라는 게 핵심이다. 출력 기기를 맨 앞에 둬야 출력 ch0-1이
// 그 기기가 된다. 오디오 인터페이스가 앞에 오면 인터페이스의 출력이 ch0-1을 차지해서,
// 스피커로 내보내려던 소리가 인터페이스의 헤드폰 단자로 나간다.
//
// 사용법:
//   Mode2AudioDevice list
//   Mode2AudioDevice create <이름> <출력기기> <입력기기...>
//   Mode2AudioDevice remove <이름>
//
// 기기는 이름 일부만 적어도 찾는다(대소문자 무시). 예: "Scarlett", "AirPods".
//
// 이 저장소의 기준 구성(기타 + 에어팟 마이크 → 맥북 스피커):
//   Mode2AudioDevice create "Guitar + AirPods Mic (Mac Speakers)" \
//       "MacBook Pro 스피커" "AirPods Pro" "Scarlett Solo"
//
// 에어팟은 마이크를 쓰면 HFP로 내려가 24kHz가 되는데, 통합 기기 안에서는 드리프트 보정으로
// 48kHz로 올라온다(실측). 에어팟을 단독 기기로 쓸 때 겪던 24kHz 저하가 사라진다.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UID_PREFIX "com.MadCamp.VocalGuitarApp."

static CFStringRef cf(const char *s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

static int cfToUtf8(CFStringRef s, char *buf, size_t size)
{
    if (s == NULL)
    {
        buf[0] = '\0';
        return 0;
    }
    return CFStringGetCString(s, buf, (CFIndex) size, kCFStringEncodingUTF8);
}

static UInt32 channelCount(AudioObjectID device, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyStreamConfiguration, scope,
                                        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, NULL, &size) != noErr || size == 0)
        return 0;

    AudioBufferList *list = (AudioBufferList *) malloc(size);
    UInt32 total = 0;
    if (AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, list) == noErr)
        for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
            total += list->mBuffers[i].mNumberChannels;
    free(list);
    return total;
}

static AudioObjectID *allDevices(UInt32 *countOut)
{
    AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr)
    {
        *countOut = 0;
        return NULL;
    }
    AudioObjectID *devices = (AudioObjectID *) malloc(size);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices) != noErr)
    {
        free(devices);
        *countOut = 0;
        return NULL;
    }
    *countOut = size / sizeof(AudioObjectID);
    return devices;
}

static int deviceName(AudioObjectID device, char *buf, size_t size)
{
    AudioObjectPropertyAddress addr = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 dataSize = sizeof(s);
    if (AudioObjectGetPropertyData(device, &addr, 0, NULL, &dataSize, &s) != noErr)
        return 0;
    int ok = cfToUtf8(s, buf, size);
    if (s) CFRelease(s);
    return ok;
}

static int deviceUID(AudioObjectID device, char *buf, size_t size)
{
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 dataSize = sizeof(s);
    if (AudioObjectGetPropertyData(device, &addr, 0, NULL, &dataSize, &s) != noErr)
        return 0;
    int ok = cfToUtf8(s, buf, size);
    if (s) CFRelease(s);
    return ok;
}

static Float64 sampleRate(AudioObjectID device)
{
    AudioObjectPropertyAddress addr = { kAudioDevicePropertyNominalSampleRate,
                                        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &rate);
    return rate;
}

// 출력 기기가 음소거되어 있거나 볼륨이 0이면 통합 기기는 정상인데 소리만 안 난다.
// 시스템 출력을 다른 기기(에어팟 등)로 쓰는 동안 내장 스피커가 음소거·볼륨 0으로 남아 있는
// 일이 흔하다. 실제로 이걸 못 보고 라우팅을 한참 의심한 적이 있어서 만들 때 같이 확인한다.
static void warnIfSilenced(AudioObjectID device, const char *name)
{
    UInt32 mute = 0;
    UInt32 size = sizeof(mute);
    AudioObjectPropertyAddress muteAddr = { kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput,
                                            kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(device, &muteAddr, 0, NULL, &size, &mute) == noErr && mute)
        printf("  !! %s 가 음소거 상태다 — 소리가 안 난다.\n", name);

    Float32 volume = -1.0f;
    size = sizeof(volume);
    AudioObjectPropertyAddress volAddr = { kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput,
                                           kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(device, &volAddr, 0, NULL, &size, &volume) != noErr)
    {
        volAddr.mElement = 1;
        size = sizeof(volume);
        if (AudioObjectGetPropertyData(device, &volAddr, 0, NULL, &size, &volume) != noErr)
            return;  // 볼륨 제어가 없는 기기(인터페이스 등)는 확인할 게 없다.
    }
    if (volume <= 0.001f)
        printf("  !! %s 의 볼륨이 0이다 — 소리가 안 난다.\n", name);
}

static int containsNoCase(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    for (const char *p = haystack; *p; ++p)
        if (strncasecmp(p, needle, nlen) == 0)
            return 1;
    return 0;
}

// 이름 일부로 기기를 찾는다. 입력이 필요한 자리인지(wantInput) 알려주면 같은 이름의
// 입력/출력 기기가 따로 있는 경우(에어팟이 그렇다)를 올바르게 가른다.
static AudioObjectID findDevice(const char *nameFragment, int wantInput, char *uidOut, size_t uidSize)
{
    UInt32 count = 0;
    AudioObjectID *devices = allDevices(&count);
    AudioObjectID found = kAudioObjectUnknown;

    for (UInt32 i = 0; i < count; ++i)
    {
        char name[512], uid[512];
        if (!deviceName(devices[i], name, sizeof(name))) continue;
        if (!containsNoCase(name, nameFragment)) continue;

        const UInt32 ins = channelCount(devices[i], kAudioObjectPropertyScopeInput);
        const UInt32 outs = channelCount(devices[i], kAudioObjectPropertyScopeOutput);
        if (wantInput ? (ins == 0) : (outs == 0)) continue;

        // 통합 기기는 후보에서 뺀다. 통합 기기를 다시 통합할 수는 없다.
        deviceUID(devices[i], uid, sizeof(uid));
        if (strstr(uid, "AMS2_Aggregate") != NULL || strncmp(uid, UID_PREFIX, strlen(UID_PREFIX)) == 0)
            continue;

        found = devices[i];
        snprintf(uidOut, uidSize, "%s", uid);
        break;
    }
    free(devices);
    return found;
}

static void listDevices(void)
{
    UInt32 count = 0;
    AudioObjectID *devices = allDevices(&count);
    printf("%-4s %-4s %9s  %s\n", "입력", "출력", "샘플레이트", "이름 / UID");
    for (UInt32 i = 0; i < count; ++i)
    {
        char name[512], uid[512];
        deviceName(devices[i], name, sizeof(name));
        deviceUID(devices[i], uid, sizeof(uid));
        printf("%-4u %-4u %8.0fHz  %s\n%29s%s\n",
               channelCount(devices[i], kAudioObjectPropertyScopeInput),
               channelCount(devices[i], kAudioObjectPropertyScopeOutput),
               sampleRate(devices[i]), name, "", uid);
    }
    free(devices);
}

static AudioObjectID findAggregateByUID(const char *uid)
{
    UInt32 count = 0;
    AudioObjectID *devices = allDevices(&count);
    AudioObjectID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < count; ++i)
    {
        char existing[512];
        if (deviceUID(devices[i], existing, sizeof(existing)) && strcmp(existing, uid) == 0)
        {
            found = devices[i];
            break;
        }
    }
    free(devices);
    return found;
}

// 이름에서 UID를 만든다. 같은 이름으로 다시 만들면 같은 UID가 나오므로, 재실행이
// 기기를 늘리지 않고 덮어쓴다.
static void uidForName(const char *name, char *out, size_t size)
{
    size_t written = snprintf(out, size, "%s", UID_PREFIX);
    for (const char *p = name; *p && written + 1 < size; ++p)
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))
            out[written++] = *p;
    out[written] = '\0';
}

static CFDictionaryRef subDeviceDict(const char *uid, int driftCompensation)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef u = cf(uid);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceUIDKey), u);
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &driftCompensation);
    CFDictionarySetValue(d, CFSTR(kAudioSubDeviceDriftCompensationKey), n);
    CFRelease(u);
    CFRelease(n);
    return d;
}

static int createAggregate(const char *name, int deviceCount, char **deviceNames)
{
    char uid[512];
    uidForName(name, uid, sizeof(uid));

    AudioObjectID existing = findAggregateByUID(uid);
    if (existing != kAudioObjectUnknown && AudioHardwareDestroyAggregateDevice(existing) == noErr)
        printf("같은 이름의 기존 기기를 제거하고 다시 만듭니다.\n");

    CFDictionaryRef subs[16];
    char uids[16][512];
    char names[16][512];
    UInt32 ins[16], outs[16];
    if (deviceCount > 16) deviceCount = 16;

    for (int i = 0; i < deviceCount; ++i)
    {
        // 첫 번째는 출력 기기, 나머지는 입력 기기로 찾는다.
        AudioObjectID dev = findDevice(deviceNames[i], i > 0, uids[i], sizeof(uids[i]));
        if (dev == kAudioObjectUnknown)
        {
            printf("기기를 찾지 못했습니다: \"%s\" (%s)\n", deviceNames[i], i == 0 ? "출력" : "입력");
            printf("`Mode2AudioDevice list`로 실제 이름을 확인하세요.\n");
            return 1;
        }
        deviceName(dev, names[i], sizeof(names[i]));
        ins[i] = channelCount(dev, kAudioObjectPropertyScopeInput);
        outs[i] = channelCount(dev, kAudioObjectPropertyScopeOutput);
        if (outs[i] > 0)
            warnIfSilenced(dev, names[i]);
        // 마스터(첫 번째)만 자기 클럭을 쓰고 나머지는 드리프트 보정을 켠다.
        subs[i] = subDeviceDict(uids[i], i == 0 ? 0 : 1);
    }

    CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, (const void **) subs, deviceCount,
                                    &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef cfName = cf(name), cfUID = cf(uid), cfMaster = cf(uids[0]);
    int isPrivate = 0;
    CFNumberRef cfPrivate = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &isPrivate);

    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), cfName);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), cfUID);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), list);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMasterSubDeviceKey), cfMaster);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), cfPrivate);

    AudioObjectID created = kAudioObjectUnknown;
    OSStatus err = AudioHardwareCreateAggregateDevice(desc, &created);

    CFRelease(cfName); CFRelease(cfUID); CFRelease(cfMaster); CFRelease(cfPrivate);
    CFRelease(list);
    for (int i = 0; i < deviceCount; ++i) CFRelease(subs[i]);
    CFRelease(desc);

    if (err != noErr)
    {
        printf("생성 실패: OSStatus %d\n", (int) err);
        return 1;
    }

    printf("통합 기기를 만들었습니다: %s  (%.0fHz)\n", name, sampleRate(created));
    printf("UID: %s\n\n", uid);

    // 앱에서 어느 채널을 골라야 하는지 그대로 출력한다. 이게 없으면 매번 녹음해서
    // 채널을 찾아내야 한다(실제로 그렇게 한 적이 있다).
    printf("채널 배치 — 앱의 채널 선택은 1부터 세므로 괄호 안 번호를 고르면 된다:\n");
    UInt32 inBase = 0, outBase = 0;
    for (int i = 0; i < deviceCount; ++i)
    {
        if (outs[i] > 0)
        {
            printf("  출력 ch%u-%u  %s\n", outBase, outBase + outs[i] - 1, names[i]);
            outBase += outs[i];
        }
        if (ins[i] > 0)
        {
            printf("  입력 ch%u-%u (앱: 채널 %u-%u)  %s\n", inBase, inBase + ins[i] - 1,
                   inBase + 1, inBase + ins[i], names[i]);
            inBase += ins[i];
        }
    }
    printf("\n오디오 인터페이스의 입력 순서는 하드웨어를 따른다(Scarlett Solo: 1=XLR, 2=악기 잭).\n");
    return 0;
}

static int removeAggregate(const char *name)
{
    char uid[512];
    uidForName(name, uid, sizeof(uid));
    AudioObjectID device = findAggregateByUID(uid);

    // 이름으로 못 찾으면 인자를 UID 자체로 보고 한 번 더 찾는다. `list`에 찍힌 UID를
    // 그대로 붙여 넣어 지울 수 있어야, 예전에 다른 이름으로 만들어 둔 기기도 정리된다.
    if (device == kAudioObjectUnknown)
        device = findAggregateByUID(name);

    if (device == kAudioObjectUnknown)
    {
        printf("그런 이름이나 UID의 기기가 없습니다: %s\n", name);
        return 1;
    }
    OSStatus err = AudioHardwareDestroyAggregateDevice(device);
    if (err != noErr)
    {
        printf("삭제 실패: OSStatus %d\n", (int) err);
        return 1;
    }
    printf("삭제했습니다: %s\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "list") == 0)
    {
        listDevices();
        return 0;
    }
    if (argc >= 5 && strcmp(argv[1], "create") == 0)
        return createAggregate(argv[2], argc - 3, argv + 3);
    if (argc == 3 && strcmp(argv[1], "remove") == 0)
        return removeAggregate(argv[2]);

    printf("사용법:\n"
           "  Mode2AudioDevice list\n"
           "  Mode2AudioDevice create <이름> <출력기기> <입력기기...>\n"
           "  Mode2AudioDevice remove <이름>\n"
           "\n"
           "기기는 이름 일부만 적어도 찾는다(대소문자 무시).\n"
           "첫 번째 기기가 출력이고 맨 앞에 와야 출력 ch0-1을 차지한다.\n"
           "\n"
           "기준 구성(기타 + 에어팟 마이크 -> 맥북 스피커):\n"
           "  Mode2AudioDevice create \"Guitar + AirPods Mic (Mac Speakers)\" \\\n"
           "      \"MacBook Pro\" \"AirPods\" \"Scarlett Solo\"\n");
    return 1;
}
