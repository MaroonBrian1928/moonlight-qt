#pragma once

#include "renderer.h"
#include "SDL_compat.h"

class SdlAudioRenderer : public IAudioRenderer
{
public:
    explicit SdlAudioRenderer(int audioPlaybackThresholdMs = 30, int audioDropThresholdMs = 30);

    virtual ~SdlAudioRenderer();

    virtual bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig);

    virtual void* getAudioBuffer(int* size);

    virtual bool submitAudio(int bytesWritten);

    virtual AudioFormat getAudioBufferFormat();

private:
    SDL_AudioDeviceID m_AudioDevice;
    void* m_AudioBuffer;
    Uint32 m_FrameSize;
    Uint32 m_FrameDurationMs;
    Uint32 m_BytesPerMs;
    Uint32 m_AudioPlaybackThresholdMs;
    Uint32 m_AudioDropThresholdMs;
    bool m_WaitingForPlaybackThreshold;
};
