#include "compressor.h"
#include "cautil.h"

namespace {
    template <typename T>
    inline T frame_amplitude(const T *frame, unsigned nchannels)
    {
        T x = 0;
        for (unsigned i = 0; i < nchannels; ++i) {
            T y = std::abs(frame[i]);
            if (y > x) x = y;
        }
        return x;
    }
}

Compressor::Compressor(const std::shared_ptr<ISource> &src,
                       double threshold, double ratio, double knee_width,
                       double attack, double release)
    : FilterBase(src),
      m_threshold(threshold),
      m_slope((1.0 - ratio) / ratio),
      m_knee_width(knee_width),
      m_attack(attack / 1000.0),
      m_release(release / 1000.0),
      m_Tlo(threshold - knee_width / 2.0),
      m_Thi(threshold + knee_width / 2.0),
      m_knee_factor(m_slope / (knee_width * 2.0)),
      m_yR(0.0),
      m_yA(0.0)
{
    const AudioStreamBasicDescription &asbd = src->getSampleFormat();
    unsigned bits = 32;
    if (asbd.mBitsPerChannel > 32
        || (asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) &&
           asbd.mBitsPerChannel > 24)
        bits = 64;
    m_asbd = cautil::buildASBDForPCM(asbd.mSampleRate, asbd.mChannelsPerFrame,
                                     bits, kAudioFormatFlagIsFloat);
}

size_t Compressor::readSamples(void *buffer, size_t nsamples)
{
    if (m_asbd.mBitsPerChannel == 64)
        return readSamplesT(static_cast<double*>(buffer), nsamples);
    else
        return readSamplesT(static_cast<float*>(buffer), nsamples);
}

template <typename T>
size_t Compressor::readSamplesT(T *buffer, size_t nsamples)
{
    const double Fs = m_asbd.mSampleRate;
    unsigned nchannels = m_asbd.mChannelsPerFrame;
    const double alphaA = 
        m_attack > 0.0 ? std::exp(-1.0 / (m_attack * Fs)) : 0.0;
    const double alphaR =
        m_release > 0.0 ? std::exp(-1.0 / (m_release * Fs)) : 0.0;

    nsamples = readSamplesAsFloat(source(), &m_pivot, buffer, nsamples);

    for (size_t i = 0; i < nsamples; ++i) {
        T *frame = &buffer[i * nchannels];
        double xL = frame_amplitude(frame, nchannels);
        double xG = util::scale_to_dB(xL);
        double yG = computeGain(xG);
        double cG = smoothAverage(yG, alphaA, alphaR);
        T cL = static_cast<T>(util::dB_to_scale(cG));
        for (unsigned n = 0; n < nchannels; ++n)
            frame[n] *= cL;
    }
    return nsamples;
}
