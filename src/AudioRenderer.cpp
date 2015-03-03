#include "pch.h"
#include "AudioRenderer.h"

namespace SaneAudioRenderer
{
    AudioRenderer::AudioRenderer(ISettings* pSettings, IMyClock* pClock, CAMEvent& bufferFilled, HRESULT& result)
        : m_deviceManager(result)
        , m_myClock(pClock)
        , m_flush(TRUE/*manual reset*/)
        , m_dspVolume(*this)
        , m_dspBalance(*this)
        , m_bufferFilled(bufferFilled)
        , m_settings(pSettings)
    {
        if (FAILED(result))
            return;

        try
        {
            if (!m_settings || !m_myClock)
                throw E_UNEXPECTED;

            ThrowIfFailed(m_myClock->QueryInterface(IID_PPV_ARGS(&m_myGraphClock)));

            if (static_cast<HANDLE>(m_flush) == NULL ||
                static_cast<HANDLE>(m_bufferFilled) == NULL)
            {
                throw E_OUTOFMEMORY;
            }
        }
        catch (HRESULT ex)
        {
            result = ex;
        }
    }

    AudioRenderer::~AudioRenderer()
    {
        // Just in case.
        if (m_state != State_Stopped)
            Stop();
    }

    void AudioRenderer::SetClock(IReferenceClock* pClock)
    {
        CAutoLock objectLock(this);

        m_graphClock = pClock;

        if (m_graphClock && m_graphClock != m_myGraphClock)
        {
            if (!m_externalClock)
                ClearDevice();

            m_externalClock = true;
        }
        else
        {
            if (m_externalClock)
                ClearDevice();

            m_externalClock = false;
        }
    }

    bool AudioRenderer::OnExternalClock()
    {
        CAutoLock objectLock(this);

        return m_externalClock;
    }

    bool AudioRenderer::Enqueue(IMediaSample* pSample, AM_SAMPLE2_PROPERTIES& sampleProps)
    {
        DspChunk chunk;

        {
            CAutoLock objectLock(this);
            assert(m_inputFormat);
            assert(m_state != State_Stopped);

            try
            {
                // Clear the device if related settings were changed.
                CheckDeviceSettings();

                // Create the device if needed.
                if (!m_device)
                    CreateDevice();

                // Apply sample corrections (pad, crop, guess timings).
                chunk = m_sampleCorrection.ProcessSample(pSample, sampleProps);

                // Apply clock corrections (graph clock and rate dsp).
                if (m_device && m_state == State_Running)
                    ApplyClockCorrection();

                // Apply dsp chain.
                if (m_device && !m_device->bitstream)
                {
                    auto f = [&](DspBase* pDsp)
                    {
                        pDsp->Process(chunk);
                    };

                    EnumerateProcessors(f);

                    DspChunk::ToFormat(m_device->dspFormat, chunk);
                }
            }
            catch (std::bad_alloc&)
            {
                ClearDevice();
                chunk = DspChunk();
            }
        }

        // Send processed sample to the device.
        return Push(chunk);
    }

    bool AudioRenderer::Finish(bool blockUntilEnd)
    {
        DspChunk chunk;

        {
            CAutoLock objectLock(this);
            assert(m_state != State_Stopped);

            // No device - nothing to block on.
            if (!m_device)
                blockUntilEnd = false;

            try
            {
                // Apply dsp chain.
                if (m_device && !m_device->bitstream)
                {
                    auto f = [&](DspBase* pDsp)
                    {
                        pDsp->Finish(chunk);
                    };

                    EnumerateProcessors(f);

                    DspChunk::ToFormat(m_device->dspFormat, chunk);
                }
            }
            catch (std::bad_alloc&)
            {
                chunk = DspChunk();
                assert(chunk.IsEmpty());
            }
        }

        auto doBlock = [this]
        {
            // Increase system timer resolution.
            TimePeriodHelper timePeriodHelper(1);

            // Unslave the clock because no more samples are going to be pushed.
            m_myClock->UnslaveClockFromAudio();

            for (;;)
            {
                int64_t actual = INT64_MAX;
                int64_t target;

                {
                    CAutoLock objectLock(this);

                    if (!m_device)
                        return true;

                    UINT64 deviceClockFrequency, deviceClockPosition;

                    try
                    {
                        ThrowIfFailed(m_device->audioClock->GetFrequency(&deviceClockFrequency));
                        ThrowIfFailed(m_device->audioClock->GetPosition(&deviceClockPosition, nullptr));
                    }
                    catch (HRESULT)
                    {
                        ClearDevice();
                        return true;
                    }

                    const auto previous = actual;
                    actual = llMulDiv(deviceClockPosition, OneSecond, deviceClockFrequency, 0);
                    target = llMulDiv(m_pushedFrames, OneSecond, m_device->waveFormat->nSamplesPerSec, 0);

                    // Return if the end of stream is reached.
                    if (actual == target)
                        return true;

                    // Stalling protection.
                    if (actual == previous && m_state == State_Running)
                        return true;
                }

                // Sleep until predicted end of stream.
                if (m_flush.Wait(std::max(1, (int32_t)((target - actual) * 1000 / OneSecond))))
                    return false;
            }
        };

        // Send processed sample to the device, and block until the buffer is drained (if requested).
        return Push(chunk) && (!blockUntilEnd || doBlock());
    }

    void AudioRenderer::BeginFlush()
    {
        m_flush.Set();
    }

    void AudioRenderer::EndFlush()
    {
        CAutoLock objectLock(this);
        assert(m_state != State_Running);

        if (m_device)
        {
            m_device->audioClient->Reset();
            m_bufferFilled.Reset();
            m_sampleCorrection.NewBuffer();
            m_pushedFrames = 0;
        }

        m_flush.Reset();
    }

    bool AudioRenderer::CheckFormat(SharedWaveFormat inputFormat)
    {
        assert(inputFormat);

        if (DspFormatFromWaveFormat(*inputFormat) != DspFormat::Unknown)
            return true;

        BOOL exclusive;
        m_settings->GetOuputDevice(nullptr, &exclusive, nullptr);
        BOOL bitstreamingAllowed;
        m_settings->GetAllowBitstreaming(&bitstreamingAllowed);

        if (!exclusive || !bitstreamingAllowed)
            return false;

        CAutoLock objectLock(this);

        return m_deviceManager.BitstreamFormatSupported(inputFormat, m_settings);
    }

    void AudioRenderer::SetFormat(SharedWaveFormat inputFormat)
    {
        CAutoLock objectLock(this);

        m_inputFormat = inputFormat;

        m_sampleCorrection.NewFormat(inputFormat);

        ClearDevice();
    }

    void AudioRenderer::NewSegment(double rate)
    {
        CAutoLock objectLock(this);

        m_startClockOffset = 0;
        m_rate = rate;

        m_sampleCorrection.NewSegment(m_rate);

        assert(m_inputFormat);
        if (m_device)
            InitializeProcessors();
    }

    void AudioRenderer::Play(REFERENCE_TIME startTime)
    {
        CAutoLock objectLock(this);
        assert(m_state != State_Running);
        m_state = State_Running;

        m_startTime = startTime;
        StartDevice();
    }

    void AudioRenderer::Pause()
    {
        CAutoLock objectLock(this);
        m_state = State_Paused;

        if (m_device)
        {
            m_myClock->UnslaveClockFromAudio();
            m_device->audioClient->Stop();
        }
    }

    void AudioRenderer::Stop()
    {
        CAutoLock objectLock(this);
        m_state = State_Stopped;

        ClearDevice();
    }

    SharedWaveFormat AudioRenderer::GetInputFormat()
    {
        CAutoLock objectLock(this);

        return m_inputFormat;
    }

    SharedAudioDevice AudioRenderer::GetAudioDevice()
    {
        CAutoLock objectLock(this);

        return m_device;
    }

    std::vector<std::wstring> AudioRenderer::GetActiveProcessors()
    {
        CAutoLock objectLock(this);

        std::vector<std::wstring> ret;

        if (m_inputFormat && m_device && !m_device->bitstream)
        {
            auto f = [&](DspBase* pDsp)
            {
                if (pDsp->Active())
                    ret.emplace_back(pDsp->Name());
            };

            EnumerateProcessors(f);
        }

        return ret;
    }

    void AudioRenderer::CheckDeviceSettings()
    {
        CAutoLock objectLock(this);

        UINT32 serial = m_settings->GetSerial();

        if (m_device && m_deviceSettingsSerial != serial)
        {
            LPWSTR pDeviceName = nullptr;
            BOOL exclusive;
            UINT32 buffer;
            if (SUCCEEDED(m_settings->GetOuputDevice(&pDeviceName, &exclusive, &buffer)))
            {
                if (m_device->exclusive != !!exclusive ||
                    m_device->bufferDuration != buffer ||
                    (pDeviceName && *pDeviceName && wcscmp(pDeviceName, m_device->friendlyName->c_str())) ||
                    ((!pDeviceName || !*pDeviceName) && !m_device->default))
                {
                    ClearDevice();
                    assert(!m_device);
                }
                else
                {
                    m_deviceSettingsSerial = serial;
                }
                CoTaskMemFree(pDeviceName);
            }
        }
    }

    void AudioRenderer::StartDevice()
    {
        CAutoLock objectLock(this);
        assert(m_state == State_Running);

        if (m_device)
        {
            m_myClock->SlaveClockToAudio(m_device->audioClock, m_startTime + m_startClockOffset);
            m_startClockOffset = 0;
            m_device->audioClient->Start();
            //assert(m_bufferFilled.Check());
        }
    }

    void AudioRenderer::CreateDevice()
    {
        CAutoLock objectLock(this);

        assert(!m_device);
        assert(m_inputFormat);

        m_deviceSettingsSerial = m_settings->GetSerial();
        m_device = m_deviceManager.CreateDevice(m_inputFormat, m_settings);

        if (m_device)
        {
            m_sampleCorrection.NewBuffer();
            m_pushedFrames = 0;

            InitializeProcessors();

            m_startClockOffset = m_sampleCorrection.GetLastSampleEnd();

            if (m_state == State_Running)
                StartDevice();
        }
    }

    void AudioRenderer::ClearDevice()
    {
        CAutoLock objectLock(this);

        if (m_device)
        {
            m_myClock->UnslaveClockFromAudio();
            m_device->audioClient->Stop();
            m_bufferFilled.Reset();
            m_sampleCorrection.NewBuffer();
            m_pushedFrames = 0;
            m_device = nullptr;
            m_deviceManager.ReleaseDevice();
        }
    }

    void AudioRenderer::ApplyClockCorrection()
    {
        CAutoLock objectLock(this);
        assert(m_inputFormat);
        assert(m_device);
        assert(m_state == State_Running);

        // Apply corrections to internal clock.
        {
            REFERENCE_TIME offset = m_sampleCorrection.GetTimingsError() - m_myClock->GetSlavedClockOffset();
            if (std::abs(offset) > 1000)
            {
                m_myClock->OffsetSlavedClock(offset);
                DebugOut("AudioRenderer offset internal clock by", offset / 10000., "ms");
            }
        }

        /*
        // Try to match internal clock with graph clock if they're different.
        // We do it in the roundabout way by dynamically changing audio sampling rate.
        if (m_externalClock && !m_device->bitstream)
        {
            assert(m_dspRate.Active());
            REFERENCE_TIME graphTime, myTime, myStartTime;
            if (SUCCEEDED(m_myClock->GetAudioClockStartTime(&myStartTime)) &&
                SUCCEEDED(m_myClock->GetAudioClockTime(&myTime, nullptr)) &&
                SUCCEEDED(GetGraphTime(graphTime)) &&
                myTime > myStartTime)
            {
                REFERENCE_TIME offset = graphTime - myTime - m_correctedWithRateDsp;
                if (std::abs(offset) > MILLISECONDS_TO_100NS_UNITS(2))
                {
                    m_dspRate.Adjust(offset);
                    m_correctedWithRateDsp += offset;
                    DebugOut("AudioRenderer offset internal clock indirectly by", offset / 10000., "ms");
                }
            }
        }
        */
    }

    HRESULT AudioRenderer::GetGraphTime(REFERENCE_TIME& time)
    {
        CAutoLock objectLock(this);

        return m_graphClock ?
                   m_graphClock->GetTime(&time) :
                   m_myGraphClock->GetTime(&time);
    }

    void AudioRenderer::InitializeProcessors()
    {
        CAutoLock objectLock(this);
        assert(m_inputFormat);
        assert(m_device);

        m_correctedWithRateDsp = 0;

        if (m_device->bitstream)
            return;

        const auto inRate = m_inputFormat->nSamplesPerSec;
        const auto inChannels = m_inputFormat->nChannels;
        const auto inMask = DspMatrix::GetChannelMask(*m_inputFormat);
        const auto outRate = m_device->waveFormat->nSamplesPerSec;
        const auto outChannels = m_device->waveFormat->nChannels;
        const auto outMask = DspMatrix::GetChannelMask(*m_device->waveFormat);

        m_dspMatrix.Initialize(inChannels, inMask, outChannels, outMask);
        m_dspRate.Initialize(m_externalClock, inRate, outRate, outChannels);
        m_dspVariableRate.Initialize(m_externalClock, inRate, outRate, outChannels);
        m_dspTempo.Initialize(m_rate, outRate, outChannels);
        m_dspCrossfeed.Initialize(m_settings, outRate, outChannels, outMask);
        m_dspVolume.Initialize(m_device->exclusive);
        m_dspLimiter.Initialize(m_settings, outRate, m_device->exclusive);
        m_dspDither.Initialize(m_device->dspFormat);
    }

    bool AudioRenderer::Push(DspChunk& chunk)
    {
        if (chunk.IsEmpty())
            return true;

        const uint32_t frameSize = chunk.GetFrameSize();
        const size_t chunkFrames = chunk.GetFrameCount();

        uint32_t sleepDuration = 0;
        bool firstIteration = true;
        for (size_t doneFrames = 0; doneFrames < chunkFrames;)
        {
            // The device buffer is full or almost full at the beginning of the second and subsequent iterations.
            // Sleep until the buffer may have significant amount of free space. Unless interrupted.
            if (!firstIteration && m_flush.Wait(sleepDuration))
                return false;

            firstIteration = false;

            CAutoLock objectLock(this);

            assert(m_state != State_Stopped);

            if (m_device)
            {
                try
                {
                    // Get up-to-date information on the device buffer.
                    UINT32 bufferFrames, bufferPadding;
                    ThrowIfFailed(m_device->audioClient->GetBufferSize(&bufferFrames));
                    ThrowIfFailed(m_device->audioClient->GetCurrentPadding(&bufferPadding));

                    // Find out how many frames we can write this time.
                    const UINT32 doFrames = std::min(bufferFrames - bufferPadding, (UINT32)(chunkFrames - doneFrames));

                    sleepDuration = m_device->bufferDuration / 4;

                    if (doFrames == 0)
                        continue;

                    // Write frames to the device buffer.
                    BYTE* deviceBuffer;
                    ThrowIfFailed(m_device->audioRenderClient->GetBuffer(doFrames, &deviceBuffer));
                    assert(frameSize == (m_device->waveFormat->wBitsPerSample / 8 * m_device->waveFormat->nChannels));
                    memcpy(deviceBuffer, chunk.GetConstData() + doneFrames * frameSize, doFrames * frameSize);
                    ThrowIfFailed(m_device->audioRenderClient->ReleaseBuffer(doFrames, 0));

                    // If the buffer is fully filled, set the corresponding event
                    // and stop delaying Paused->Running transition.
                    if (bufferPadding + doFrames == bufferFrames &&
                        !m_bufferFilled.Check())
                    {
                        m_bufferFilled.Set();
                    }

                    doneFrames += doFrames;
                    m_pushedFrames += doFrames;

                    continue;
                }
                catch (HRESULT)
                {
                    ClearDevice();
                }
            }

            // The code below emulates null audio device.
            assert(!m_device);

            // Don't delay Paused->Running transition, because we don't have a buffer to fill anyway.
            m_bufferFilled.Set();

            sleepDuration = 1;

            // Loop until the graph time passes the current sample end.
            REFERENCE_TIME graphTime;
            if (m_state == State_Running &&
                SUCCEEDED(GetGraphTime(graphTime)) &&
                graphTime > m_startTime + m_sampleCorrection.GetLastSampleEnd())
            {
                break;
            }
        }

        return true;
    }
}
