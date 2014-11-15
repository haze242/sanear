#include "pch.h"
#include "DspChunk.h"

namespace SaneAudioRenderer
{
    static_assert((int32_t{-1} >> 31) == -1 && (int64_t{-1} >> 63) == -1, "Code relies on right signed shift UB");

    namespace
    {
        template<DspFormat InputFormat, DspFormat OutputFormat>
        inline void ConvertSample(const typename DspFormatTraits<InputFormat>::SampleType& input,
                                  typename DspFormatTraits<OutputFormat>::SampleType& output);

        template<>
        inline void ConvertSample<DspFormat::Pcm8, DspFormat::Pcm8>(const int8_t& input, int8_t& output)
        {
            output = input;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm8, DspFormat::Pcm16>(const int8_t& input, int16_t& output)
        {
            output = (int16_t)input << 8;;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm8, DspFormat::Pcm24>(const int8_t& input, int32_t& output)
        {
            output = (int32_t)input << 16;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm8, DspFormat::Pcm32>(const int8_t& input, int32_t& output)
        {
            output = (int32_t)input << 24;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm8, DspFormat::Float>(const int8_t& input, float& output)
        {
            output = (float)input / -INT8_MIN;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm8, DspFormat::Double>(const int8_t& input, double& output)
        {
            output = (double)input / -INT8_MIN;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm16, DspFormat::Pcm16>(const int16_t& input, int16_t& output)
        {
            output = input;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm16, DspFormat::Pcm24>(const int16_t& input, int32_t& output)
        {
            output = (int32_t)input << 8;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm16, DspFormat::Pcm32>(const int16_t& input, int32_t& output)
        {
            output = (int32_t)input << 16;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm16, DspFormat::Float>(const int16_t& input, float& output)
        {
            output = (float)input / -INT16_MIN;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm16, DspFormat::Double>(const int16_t& input, double& output)
        {
            output = (double)input / -INT16_MIN;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm24, DspFormat::Pcm16>(const int32_t& input, int16_t& output)
        {
            output = (int16_t)(input >> 8);
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm24, DspFormat::Pcm24>(const int32_t& input, int32_t& output)
        {
            output = input;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm24, DspFormat::Pcm32>(const int32_t& input, int32_t& output)
        {
            output = input << 8;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm24, DspFormat::Float>(const int32_t& input, float& output)
        {
            output = (float)input / (-INT32_MIN >> 8);
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm24, DspFormat::Double>(const int32_t& input, double& output)
        {
            output = (double)input / (-INT32_MIN >> 8);
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm32, DspFormat::Pcm16>(const int32_t& input, int16_t& output)
        {
            output = (int16_t)(input >> 16);
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm32, DspFormat::Pcm24>(const int32_t& input, int32_t& output)
        {
            output = input >> 8;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm32, DspFormat::Pcm32>(const int32_t& input, int32_t& output)
        {
            output = input;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm32, DspFormat::Float>(const int32_t& input, float& output)
        {
            output = (float)input / -INT32_MIN;
        }

        template<>
        inline void ConvertSample<DspFormat::Pcm32, DspFormat::Double>(const int32_t& input, double& output)
        {
            output = (double)input / -INT32_MIN;
        }

        template<>
        inline void ConvertSample<DspFormat::Float, DspFormat::Pcm16>(const float& input, int16_t& output)
        {
            output = (int16_t)(input * INT16_MAX);
        }

        template<>
        inline void ConvertSample<DspFormat::Float, DspFormat::Pcm24>(const float& input, int32_t& output)
        {
            output = (int32_t)(input * (INT32_MAX >> 8));
        }

        template<>
        inline void ConvertSample<DspFormat::Float, DspFormat::Pcm32>(const float& input, int32_t& output)
        {
            output = (int32_t)(input * INT32_MAX);
        }

        template<>
        inline void ConvertSample<DspFormat::Float, DspFormat::Float>(const float& input, float& output)
        {
            output = input;
        }

        template<>
        inline void ConvertSample<DspFormat::Float, DspFormat::Double>(const float& input, double& output)
        {
            output = input;
        }

        template<>
        inline void ConvertSample<DspFormat::Double, DspFormat::Pcm16>(const double& input, int16_t& output)
        {
            output = (int16_t)(input * INT16_MAX);
        }

        template<>
        inline void ConvertSample<DspFormat::Double, DspFormat::Pcm24>(const double& input, int32_t& output)
        {
            output = (int32_t)(input * (INT32_MAX >> 8));
        }

        template<>
        inline void ConvertSample<DspFormat::Double, DspFormat::Pcm32>(const double& input, int32_t& output)
        {
            output = (int32_t)(input * INT32_MAX);
        }

        template<>
        inline void ConvertSample<DspFormat::Double, DspFormat::Float>(const double& input, float& output)
        {
            output = (float)input;
        }

        template<>
        inline void ConvertSample<DspFormat::Double, DspFormat::Double>(const double& input, double& output)
        {
            output = input;
        }

        template<DspFormat InputFormat, DspFormat OutputFormat>
        void ConvertSamples(const char* input, typename DspFormatTraits<OutputFormat>::SampleType* output, size_t samples)
        {
            const DspFormatTraits<InputFormat>::SampleType* inputData;
            inputData = (decltype(inputData))input;

            for (size_t i = 0; i < samples; i++)
                ConvertSample<InputFormat, OutputFormat>(inputData[i], output[i]);
        }

        template<DspFormat OutputFormat>
        void ConvertChunk(DspChunk& chunk)
        {
            const DspFormat inputFormat = chunk.GetFormat();

            assert(!chunk.IsEmpty() && OutputFormat != inputFormat);

            DspChunk output(OutputFormat, chunk.GetChannelCount(), chunk.GetFrameCount(), chunk.GetRate());
            auto outputData = (DspFormatTraits<OutputFormat>::SampleType*)output.GetData();

            switch (inputFormat)
            {
                case DspFormat::Pcm8:
                    ConvertSamples<DspFormat::Pcm8, OutputFormat>(chunk.GetConstData(), outputData, chunk.GetSampleCount());
                    break;

                case DspFormat::Pcm16:
                    ConvertSamples<DspFormat::Pcm16, OutputFormat>(chunk.GetConstData(), outputData, chunk.GetSampleCount());
                    break;

                case DspFormat::Pcm24:
                    ConvertSamples<DspFormat::Pcm24, OutputFormat>(chunk.GetConstData(), outputData, chunk.GetSampleCount());
                    break;

                case DspFormat::Pcm32:
                    ConvertSamples<DspFormat::Pcm32, OutputFormat>(chunk.GetConstData(), outputData, chunk.GetSampleCount());
                    break;

                case DspFormat::Float:
                    ConvertSamples<DspFormat::Float, OutputFormat>(chunk.GetConstData(), outputData, chunk.GetSampleCount());
                    break;

                case DspFormat::Double:
                    ConvertSamples<DspFormat::Double, OutputFormat>(chunk.GetConstData(), outputData, chunk.GetSampleCount());
                    break;
            }

            chunk = std::move(output);
        }
    }

    void DspChunk::ToFormat(DspFormat format, DspChunk& chunk)
    {
        assert(format != DspFormat::Pcm8);

        if (chunk.IsEmpty() || format == chunk.GetFormat())
            return;

        switch (format)
        {
            case DspFormat::Pcm16:
                ConvertChunk<DspFormat::Pcm16>(chunk);
                break;

            case DspFormat::Pcm24:
                ConvertChunk<DspFormat::Pcm24>(chunk);
                break;

            case DspFormat::Pcm32:
                ConvertChunk<DspFormat::Pcm32>(chunk);
                break;

            case DspFormat::Float:
                ConvertChunk<DspFormat::Float>(chunk);
                break;

            case DspFormat::Double:
                ConvertChunk<DspFormat::Double>(chunk);
                break;
        }
    }

    DspChunk::DspChunk()
        : m_format(DspFormat::Pcm16)
        , m_channels(1)
        , m_rate(1)
        , m_dataSize(0)
        , m_constData(nullptr)
        , m_delayedCopy(false)
    {
    }

    DspChunk::DspChunk(DspFormat format, uint32_t channels, size_t frames, uint32_t rate)
        : m_format(format)
        , m_channels(channels)
        , m_rate(rate)
        , m_dataSize(DspFormatSize(format) * channels * frames)
        , m_constData(nullptr)
        , m_delayedCopy(false)
    {
        Allocate();
    }

    DspChunk::DspChunk(IMediaSample* pSample, const AM_SAMPLE2_PROPERTIES& sampleProps, const WAVEFORMATEX& sampleFormat)
        : m_mediaSample(pSample)
        , m_channels(sampleFormat.nChannels)
        , m_rate(sampleFormat.nSamplesPerSec)
        , m_dataSize(sampleProps.lActual)
        , m_constData((char*)sampleProps.pbBuffer)
        , m_delayedCopy(true)
    {
        assert(m_mediaSample);
        assert(m_constData);

        if (sampleFormat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        {
            m_format = (sampleFormat.wBitsPerSample == 32) ? DspFormat::Float : DspFormat::Double;
        }
        else if (sampleFormat.wFormatTag == WAVE_FORMAT_PCM)
        {
            m_format = (sampleFormat.wBitsPerSample == 8) ? DspFormat::Pcm8 :
                       (sampleFormat.wBitsPerSample == 16) ? DspFormat::Pcm16 :
                       (sampleFormat.wBitsPerSample == 24) ? DspFormat::Pcm24 : DspFormat::Pcm32;
        }
        else if (sampleFormat.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const WAVEFORMATEXTENSIBLE& sampleFormatExtensible = (const WAVEFORMATEXTENSIBLE&)sampleFormat;

            if (sampleFormatExtensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            {
                m_format = (sampleFormat.wBitsPerSample == 32) ? DspFormat::Float : DspFormat::Double;
            }
            else if (sampleFormatExtensible.SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            {
                m_format = (sampleFormat.wBitsPerSample == 8) ? DspFormat::Pcm8 :
                           (sampleFormat.wBitsPerSample == 16) ? DspFormat::Pcm16 :
                           (sampleFormat.wBitsPerSample == 24) ? DspFormat::Pcm24 : DspFormat::Pcm32;
            }
        }

        // Unpack pcm24 samples right away.
        if (m_format == DspFormat::Pcm24)
        {
            m_dataSize = m_dataSize / 3 * 4;
            Allocate();

            for (size_t i = 0, n = GetSampleCount(); i < n; i++)
            {
                int32_t x = *(uint16_t*)(m_constData + i * 3);
                int32_t h = *(uint8_t*)(m_constData + i * 3 + 2);
                x |= ((h << 24) >> 8);
                ((int32_t*)m_data.get())[i] = x;
            }

            m_delayedCopy = false;
        }
    }

    DspChunk::DspChunk(DspChunk&& other)
        : m_mediaSample(other.m_mediaSample)
        , m_format(other.m_format)
        , m_channels(other.m_channels)
        , m_rate(other.m_rate)
        , m_dataSize(other.m_dataSize)
        , m_constData(other.m_constData)
        , m_delayedCopy(other.m_delayedCopy)
    {
        other.m_mediaSample = nullptr;
        other.m_dataSize = 0;
        std::swap(m_data, other.m_data);
    }

    DspChunk& DspChunk::operator=(DspChunk&& other)
    {
        if (this != &other)
        {
            m_mediaSample = other.m_mediaSample; other.m_mediaSample = nullptr;
            m_format = other.m_format;
            m_channels = other.m_channels;
            m_rate = other.m_rate;
            m_dataSize = other.m_dataSize; other.m_dataSize = 0;
            m_constData = other.m_constData;
            m_delayedCopy = other.m_delayedCopy;
            m_data = nullptr; std::swap(m_data, other.m_data);
        }
        return *this;
    }

    char* DspChunk::GetData()
    {
        InvokeDelayedCopy();
        return m_data.get();
    }

    void DspChunk::Shrink(size_t toFrames)
    {
        if (toFrames < GetFrameCount())
        {
            InvokeDelayedCopy();
            m_dataSize = GetFormatSize() * GetChannelCount() * toFrames;
        }
    }

    void DspChunk::Allocate()
    {
        if (m_dataSize > 0)
        {
            m_data.reset((char*)_aligned_malloc(m_dataSize, 16));

            if (!m_data.get())
                throw std::bad_alloc();
        }
    }

    void DspChunk::InvokeDelayedCopy()
    {
        if (m_delayedCopy)
        {
            Allocate();
            assert(m_constData);
            memcpy(m_data.get(), m_constData, m_dataSize);
            m_delayedCopy = false;
        }
    }
}