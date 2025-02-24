#include "pch.h"
#include "audiostream.h"
#include "soundsystem.h"
#include "bass.h"

CAudioStream::CAudioStream(const char* filepath)
{
    if (isNetworkSource(filepath) && !CSoundSystem::allowNetworkSources)
    {
        gLogger->error("Loading of audiostream '{}' failed. Support of network sources was disabled in SA.Audio.ini", filepath);
        return;
    }

    unsigned flags = BASS_SAMPLE_SOFTWARE;
    if (CSoundSystem::useFloatAudio) flags |= BASS_SAMPLE_FLOAT;

    if (!(streamInternal = BASS_StreamCreateFile(FALSE, filepath, 0, 0, flags)) &&
        !(streamInternal = BASS_StreamCreateURL(filepath, 0, flags, 0, nullptr)))
    {
        gLogger->warn("Loading of audiostream '{}' failed. Error code: {}", filepath, BASS_ErrorGetCode());
        return;
    }

    SetType(CSoundSystem::defaultStreamType);
    BASS_ChannelGetAttribute(streamInternal, BASS_ATTRIB_FREQ, &rate);
    ok = true;
}

CAudioStream::~CAudioStream()
{
    if (streamInternal) BASS_StreamFree(streamInternal);
}

void CAudioStream::Play()
{    
    if (state == Stopped) BASS_ChannelSetPosition(streamInternal, 0, BASS_POS_BYTE); // rewind
    state = PlayingInactive; // needs to be processed
}

void CAudioStream::Pause(bool changeState)
{
    if (GetState() == Playing)
    {
        BASS_ChannelPause(streamInternal);
        state = changeState ? Paused : PlayingInactive;
    }
}

void CAudioStream::Stop()
{
    BASS_ChannelPause(streamInternal);
    state = Stopped;

    // cancel ongoing transitions
    speed = speedTarget;
    volume = volumeTarget;
}

void CAudioStream::Resume()
{
    Play();
}

float CAudioStream::GetLength() const
{
    return (float)BASS_ChannelBytes2Seconds(streamInternal, BASS_ChannelGetLength(streamInternal, BASS_POS_BYTE));
}

void CAudioStream::SetProgress(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    auto total = BASS_ChannelGetLength(streamInternal, BASS_POS_BYTE);
    auto bytePos = QWORD(value * total);
    BASS_ChannelSetPosition(streamInternal, bytePos, BASS_POS_BYTE);
}

float CAudioStream::GetProgress() const
{
    auto total = BASS_ChannelGetLength(streamInternal, BASS_POS_BYTE); // returns -1 on error
    auto bytePos = BASS_ChannelGetPosition(streamInternal, BASS_POS_BYTE); // returns -1 on error

    if (bytePos == -1) bytePos = 0; // error or not available yet

    float progress = (float)bytePos / total;
    progress = std::clamp(progress, 0.0f, 1.0f);
    return progress;
}

CAudioStream::eStreamState CAudioStream::GetState() const
{
    return (state == PlayingInactive) ? Playing : state;
}

void CAudioStream::SetLooping(bool enable)
{
    BASS_ChannelFlags(streamInternal, enable ? BASS_SAMPLE_LOOP : 0, BASS_SAMPLE_LOOP);
}

bool CAudioStream::GetLooping() const
{
    return (BASS_ChannelFlags(streamInternal, 0, 0) & BASS_SAMPLE_LOOP) != 0;
}

void CAudioStream::SetVolume(float value, float transitionTime)
{
    if (transitionTime > 0.0f) Resume();

    value = std::max(value, 0.0f);
    volumeTarget = value;

    if (transitionTime <= 0.0)
        volume = value; // instant
    else
        volumeTransitionStep = (volumeTarget - volume) / (1000.0 * transitionTime);
}

float CAudioStream::GetVolume() const
{
    return (float)volume;
}

void CAudioStream::SetSpeed(float value, float transitionTime)
{
    if (transitionTime > 0.0f) Resume();

    value = std::max(value, 0.0f);
    speedTarget = value;

    if (transitionTime <= 0.0)
        speed = value; // instant
    else
        speedTransitionStep = (speedTarget - speed) / (1000.0 * transitionTime);
}

float CAudioStream::GetSpeed() const
{
    return (float)speed;
}

void CAudioStream::SetType(eStreamType value)
{
    switch(value)
    {
        case eStreamType::SoundEffect:
        case eStreamType::Music:
            type = value;
            break;

        default:
            type = None;
    }
}

eStreamType CAudioStream::GetType() const
{
    return type;
}

void CAudioStream::UpdateVolume()
{
    if (volume != volumeTarget)
    {
        auto timeDelta = CTimer::m_snTimeInMillisecondsNonClipped - CTimer::m_snPreviousTimeInMillisecondsNonClipped;
        volume += volumeTransitionStep * (double)timeDelta; // animate the transition

        // check progress
        auto remaining = volumeTarget - volume;
        remaining *= (volumeTransitionStep > 0.0) ? 1.0 : -1.0;
        if (remaining < 0.0) // overshoot
        {
            volume = volumeTarget;
            if (volume <= 0.0f) Pause();
        }
    }

    float masterVolume = 1.0f;
    switch(type)
    {
        case SoundEffect: masterVolume = CSoundSystem::masterVolumeSfx; break;
        case Music: masterVolume = CSoundSystem::masterVolumeMusic; break;
    }

    BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_VOL, (float)volume * masterVolume);
}

void CAudioStream::UpdateSpeed()
{
    if (speed != speedTarget)
    {
        auto timeDelta = CTimer::m_snTimeInMillisecondsNonClipped - CTimer::m_snPreviousTimeInMillisecondsNonClipped;
        speed += speedTransitionStep * (double)timeDelta; // animate the transition

        // check progress
        auto remaining = speedTarget - speed;
        remaining *= (speedTransitionStep > 0.0) ? 1.0 : -1.0;
        if (remaining < 0.0) // overshoot
        {
            speed = speedTarget; // done
            if (speed <= 0.0f) Pause();
        }
    }

    float freq = rate * (float)speed * CSoundSystem::masterSpeed;
    freq = std::max(freq, 0.000001f); // 0 results in original speed
    BASS_ChannelSetAttribute(streamInternal, BASS_ATTRIB_FREQ, freq);
}

bool CAudioStream::IsOk() const
{
    return ok;
}

HSTREAM CAudioStream::GetInternal()
{
    return streamInternal;
}

void CAudioStream::Process()
{
    if (state == PlayingInactive)
    {
        BASS_ChannelPlay(streamInternal, FALSE);
        state = Playing;
    }

    if (!GetLooping() && GetProgress() >= 1.0f) // end reached
    {
        state = Stopped;
    }

    if (state != Playing) return; // done

    UpdateSpeed();
    UpdateVolume();
}

void CAudioStream::Set3dPosition(const CVector& pos)
{
    // not applicable for 2d audio
}

void CAudioStream::Set3dSize(float radius)
{
    // not applicable for 2d audio
}

void CAudioStream::Link(CPlaceable* placable)
{
    // not applicable for 2d audio
}
