// Minimal command-line recorder for testing the C434T microphones:
//   rec out.wav [seconds]
// Records 48 kHz 16-bit stereo from the default input device with AudioQueue.
// Build: clang -framework AudioToolbox -framework CoreFoundation -o rec rec.c

#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static AudioFileID gFile;
static SInt64 gPacket;

static void Callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf, const AudioTimeStamp *t,
                     UInt32 n, const AudioStreamPacketDescription *d) {
    (void)user; (void)t; (void)d;
    UInt32 packets = buf->mAudioDataByteSize / 4;
    if (packets && AudioFileWritePackets(gFile, false, buf->mAudioDataByteSize, NULL, gPacket,
                                         &packets, buf->mAudioData) == noErr)
        gPacket += packets;
    (void)n;
    AudioQueueEnqueueBuffer(q, buf, 0, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.wav [seconds]\n", argv[0]);
        return 1;
    }
    double seconds = argc > 2 ? atof(argv[2]) : 3.0;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = 48000;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1],
                                                           strlen(argv[1]), false);
    OSStatus err = AudioFileCreateWithURL(url, kAudioFileWAVEType, &f, kAudioFileFlags_EraseFile, &gFile);
    if (err) {
        fprintf(stderr, "AudioFileCreateWithURL: %d\n", (int)err);
        return 1;
    }
    AudioQueueRef q;
    err = AudioQueueNewInput(&f, Callback, NULL, NULL, kCFRunLoopCommonModes, 0, &q);
    if (err) {
        fprintf(stderr, "AudioQueueNewInput: %d\n", (int)err);
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b;
        AudioQueueAllocateBuffer(q, 19200, &b);
        AudioQueueEnqueueBuffer(q, b, 0, NULL);
    }
    err = AudioQueueStart(q, NULL);
    if (err) {
        fprintf(stderr, "AudioQueueStart: %d\n", (int)err);
        return 1;
    }
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    AudioQueueStop(q, true);
    AudioQueueDispose(q, true);
    AudioFileClose(gFile);
    printf("recorded %lld frames\n", (long long)gPacket);
    return 0;
}
