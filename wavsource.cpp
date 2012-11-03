#include <cstring>
#include <limits>
#include <assert.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "wavsource.h"
#include "util.h"
#include "win32util.h"
#include "chanmap.h"

#define FOURCCR(a,b,c,d) ((a)|((b)<<8)|((c)<<16)|((d)<<24))

namespace wave {
    const GUID ksFormatSubTypePCM = {
	0x1, 0x0, 0x10, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
    };
    const GUID ksFormatSubTypeFloat = {
	0x3, 0x0, 0x10, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
    };
}

WaveSource::WaveSource(const std::shared_ptr<FILE> &fp, bool ignorelength)
    : m_fp(fp)
{
    std::memset(&m_asbd, 0, sizeof m_asbd);
    m_seekable = util::is_seekable(fileno(m_fp.get()));
    int64_t data_length = parse();
    if (ignorelength || !data_length ||
	data_length % m_asbd.mBytesPerPacket)
	setRange(0, -1LL);
    else
	setRange(0, data_length / m_asbd.mBytesPerPacket);
}

size_t WaveSource::readSamples(void *buffer, size_t nsamples)
{
    nsamples = adjustSamplesToRead(nsamples);
    if (nsamples) {
	ssize_t nbytes = nsamples * m_asbd.mBytesPerFrame;
	nbytes = read(fd(), buffer, nbytes);
	nsamples = nbytes > 0 ? nbytes / m_asbd.mBytesPerFrame: 0;
	addSamplesRead(nsamples);
    }
    return nsamples;
}
void WaveSource::skipSamples(int64_t count)
{
    skip(count * m_asbd.mBytesPerFrame);
}

int64_t WaveSource::parse()
{
    int64_t data_length = 0;

    uint32_t fcc = nextChunk(0);
    if (fcc != FOURCCR('R','I','F','F') && fcc != FOURCCR('R','F','6','4'))
	throw std::runtime_error("WaveSource: not a wav file");

    uint32_t wave;
    read32le(&wave);
    if (wave != FOURCCR('W','A','V','E'))
	throw std::runtime_error("WaveSource: not a wav file");

    if (fcc == FOURCCR('R','F','6','4'))
	data_length = ds64();

    uint32_t size;
    while (nextChunk(&size) != FOURCCR('f','m','t',' '))
	skip(size);
    fmt(size);

    while (nextChunk(&size) != FOURCCR('d','a','t','a'))
	skip(size);
    if (fcc != FOURCCR('R','F','6','4'))
	data_length = size;

    return data_length;
}

inline void WaveSource::read16le(void *n)
{
    util::check_eof(read(fd(), n, 2) == 2);
}

inline void WaveSource::read32le(void *n)
{
    util::check_eof(read(fd(), n, 4) == 4);
}

inline void WaveSource::read64le(void *n)
{
    util::check_eof(read(fd(), n, 8) == 8);
}

void WaveSource::skip(int64_t n)
{
    if (m_seekable)
	_lseeki64(fd(), n, SEEK_CUR);
    else {
	char buf[8192];
	while (n > 0) {
	    int nn = static_cast<int>(std::min(n, 8192LL));
	    util::check_eof(read(fd(), buf, nn) == nn);
	    n -= nn;
	}
    }
}

uint32_t WaveSource::nextChunk(uint32_t *size)
{
    uint32_t fcc, n;
    read32le(&fcc);
    read32le(&n);
    if (size) *size = n;
    return fcc;
}

int64_t WaveSource::ds64()
{
    uint32_t size;
    int64_t data_length;

    if (nextChunk(&size)!= FOURCCR('d','s','6','4'))
	throw std::runtime_error("WaveSource: ds64 is expected in RF64 file");
    if (size != 28)
	throw std::runtime_error("WaveSource: RF64 with non empty chunk table "
				 "is not supported");
    skip(8); // RIFF size
    read64le(&data_length);
    skip(12); // sample count + chunk table size
    return data_length;
}

void WaveSource::fmt(size_t size)
{
    uint16_t wFormatTag, nChannels, nBlockAlign, wBitsPerSample, cbSize;
    uint32_t nSamplesPerSec, nAvgBytesPerSec, dwChannelMask = 0;
    uint16_t wValidBitsPerSample;
    wave::GUID guid;
    bool isfloat = false;

    if (size < 16)
	throw std::runtime_error("WaveSource: fmt chunk too small");

    read16le(&wFormatTag);
    if (wFormatTag != 1 && wFormatTag != 3 && wFormatTag != 0xfffe)
	throw std::runtime_error("WaveSource: not supported wave file");
    if (wFormatTag == 3)
	isfloat = true;

    read16le(&nChannels);
    read32le(&nSamplesPerSec);
    read32le(&nAvgBytesPerSec);
    read16le(&nBlockAlign);
    read16le(&wBitsPerSample);
    wValidBitsPerSample = wBitsPerSample;
    if (wFormatTag != 0xfffe)
	skip(size - 16);

    if (!nChannels || !nSamplesPerSec || !nAvgBytesPerSec || !nBlockAlign)
	throw std::runtime_error("WaveSource: invalid wave fmt");
    if (!wBitsPerSample || wBitsPerSample & 0x7)
	throw std::runtime_error("WaveSource: invalid wave fmt");
    if (nBlockAlign != nChannels * wBitsPerSample / 8)
	throw std::runtime_error("WaveSource: invalid wave fmt");
    if (nAvgBytesPerSec != nSamplesPerSec * nBlockAlign)
	throw std::runtime_error("WaveSource: invalid wave fmt");
    if (nChannels > 8)
	throw std::runtime_error("WaveSource: too many number of channels");

    if (wFormatTag == 0xfffe) {
	if (size < 40)
	    throw std::runtime_error("WaveSource: fmt chunk too small");
	read16le(&cbSize);
	read16le(&wValidBitsPerSample);
	read32le(&dwChannelMask);
	if (dwChannelMask > 0 && util::bitcount(dwChannelMask) >= nChannels)
	    chanmap::getChannels(dwChannelMask, &m_chanmap, nChannels);

	util::check_eof(read(fd(), &guid, sizeof guid) == sizeof guid);
	skip(size - 40);

	if (!std::memcmp(&guid, &wave::ksFormatSubTypeFloat, sizeof guid))
	    isfloat = true;
	else if (std::memcmp(&guid, &wave::ksFormatSubTypePCM, sizeof guid))
	    throw std::runtime_error("WaveSource: not supported wave file");

	if (!wValidBitsPerSample || wValidBitsPerSample > wBitsPerSample)
	    throw std::runtime_error("WaveSource: invalid wave fmt");
    }
    m_asbd.mFormatID = FOURCC('l','p','c','m');
    m_asbd.mSampleRate = nSamplesPerSec;
    m_asbd.mBytesPerPacket = nBlockAlign;
    m_asbd.mFramesPerPacket = 1;
    m_asbd.mBytesPerFrame = nBlockAlign;
    m_asbd.mChannelsPerFrame = nChannels;
    m_asbd.mBitsPerChannel = wValidBitsPerSample;
    if (isfloat)
	m_asbd.mFormatFlags |= kAudioFormatFlagIsFloat;
    else if (wBitsPerSample > 8)
	m_asbd.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    if (wBitsPerSample == wValidBitsPerSample)
	m_asbd.mFormatFlags |= kAudioFormatFlagIsPacked;
    else
	m_asbd.mFormatFlags |= kAudioFormatFlagIsAlignedHigh;
}
