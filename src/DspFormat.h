#pragma once

namespace SaneAudioRenderer
{
    enum class DspFormat
    {
        Pcm8,
        Pcm16,
        Pcm24,
        Pcm32,
        Float,
        Double,
    };

    template<DspFormat OutputFormat>
    struct DspFormatTraits;

    template<>
    struct DspFormatTraits<DspFormat::Pcm8>
    {
        typedef int8_t SampleType;
    };

    template<>
    struct DspFormatTraits<DspFormat::Pcm16>
    {
        typedef int16_t SampleType;
    };

    template<>
    struct DspFormatTraits<DspFormat::Pcm24>
    {
        typedef int32_t SampleType;
    };

    template<>
    struct DspFormatTraits<DspFormat::Pcm32>
    {
        typedef int32_t SampleType;
    };

    template<>
    struct DspFormatTraits<DspFormat::Float>
    {
        typedef float SampleType;
    };

    template<>
    struct DspFormatTraits<DspFormat::Double>
    {
        typedef double SampleType;
    };

    inline uint32_t DspFormatSize(DspFormat format)
    {
        return (format == DspFormat::Pcm8) ? 1 :
               (format == DspFormat::Pcm16) ? 2 :
               (format == DspFormat::Double) ? 8 : 4;
    }
}