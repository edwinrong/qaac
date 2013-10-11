#ifndef SOXLPF_H
#define SOXLPF_H

#include "soxcmodule.h"
#include "iointer.h"
#include "util.h"

class SoxLowpassFilter: public FilterBase {
    int64_t m_position;
    uint64_t m_length;
    std::vector<uint8_t > m_pivot;
    DecodeBuffer<float> m_buffer;
    std::shared_ptr<lsx_convolver_t> m_convolver;
    AudioStreamBasicDescription m_asbd;
    SoXConvolverModule m_module;
public:
    SoxLowpassFilter(const SoXConvolverModule &module,
                     const std::shared_ptr<ISource> &src,
                     unsigned Fp);
    const AudioStreamBasicDescription &getSampleFormat() const
    {
        return m_asbd;
    }
    size_t readSamples(void *buffer, size_t nsamples);
    int64_t getPosition() { return m_position; }
};

#endif
