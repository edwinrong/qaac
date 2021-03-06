#include <algorithm>
#include "strutil.h"
#include "sink.h"
#include "util.h"
#include "bitstream.h"
#if defined(_MSC_VER) || defined(__MINGW32__)
#include "win32util.h"
#include <io.h>
#include <fcntl.h>
#endif
#include <sys/stat.h>

static
bool getDescripterHeader(const uint8_t **p, const uint8_t *end,
                         int *tag, uint32_t *size)
{
    *size = 0;
    if (*p < end) {
        *tag = *(*p)++;
        while (*p < end) {
            int n = *(*p)++;
            *size = (*size << 7) | (n & 0x7f);
            if (!(n & 0x80)) return true;
        }
    }
    return false;
}

static
void parseMagicCookieAAC(const std::vector<uint8_t> &cookie,
                         std::vector<uint8_t> *decSpecificConfig)
{
    /*
     * QT's "Magic Cookie" for AAC is just a esds descripter.
     * We obtain only decSpecificConfig from it, and discard others.
     */
    const uint8_t *p = &cookie[0];
    const uint8_t *end = p + cookie.size();
    int tag;
    uint32_t size;
    while (getDescripterHeader(&p, end, &tag, &size)) {
        switch (tag) {
        case 3: // esds
            /*
             * ES_ID: 16
             * streamDependenceFlag: 1
             * URLFlag: 1
             * OCRstreamFlag: 1
             * streamPriority: 5
             *
             * (flags are all zero, so other atttributes are not present)
             */
            p += 3;
            break;
        case 4: // decConfig
            /*
             * objectTypeId: 8
             * streamType: 6
             * upStream: 1
             * reserved: 1
             * bufferSizeDB: 24
             * maxBitrate: 32
             * avgBitrate: 32
             *
             * QT gives constant value for bufferSizeDB, max/avgBitrate
             * depending on encoder settings.
             * On the other hand, mp4v2 calculates and sets them
             * using "real" values, when finished media writing;
             * Therefore, these values will be different from QT.
             */
            p += 13;
            break;
        case 5: // decSpecificConfig
            {
                std::vector<uint8_t> vec(size);
                std::memcpy(&vec[0], p, size);
                decSpecificConfig->swap(vec);
            }
            return;
        default:
            p += size;
        }
    }
    throw std::runtime_error(
            "Magic cookie format is different from expected!!");
}

static
void parseDecSpecificConfig(const std::vector<uint8_t> &config,
                            unsigned *sampling_rate_index,
                            unsigned *sampling_rate, unsigned *channel_config)
{
    static const unsigned tab[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 
        16000, 12000, 11025, 8000, 7350, 0, 0, 0
    };
    BitStream bs(const_cast<uint8_t*>(&config[0]), config.size());
    unsigned objtype = bs.get(5);
    *sampling_rate_index = bs.get(4);
    if (*sampling_rate_index == 15)
        *sampling_rate = bs.get(24);
    else
        *sampling_rate = tab[*sampling_rate_index];
    *channel_config = bs.get(4);
}

static
void parseMagicCookieALAC(const std::vector<uint8_t> &cookie,
                          std::vector<uint8_t> *alac,
                          std::vector<uint8_t> *chan)
{
    const uint8_t *cp = &cookie[0];
    const uint8_t *endp = cp + cookie.size();
    if (std::memcmp(cp + 4, "frmaalac", 8) == 0)
        cp += 24;
    if (endp - cp >= 24) {
        alac->resize(24);
        std::memcpy(&(*alac)[0], cp, 24);
        cp += 24;
        if (endp - cp >= 24 && !std::memcmp(cp + 4, "chan", 4)) {
            chan->resize(12);
            std::memcpy(&(*chan)[0], cp + 12, 12);
        }
    }
}

using mp4v2::impl::MP4Atom;

MP4SinkBase::MP4SinkBase(const std::wstring &path, bool temp)
        : m_filename(path), m_closed(false)
{
    static const char * const compatibleBrands[] = { "M4A ", "mp42", "isom" };
    void (MP4FileX::*create)(const char *, uint32_t, int, int,
            char*, uint32_t, char **, uint32_t);
    if (temp) m_filename = L"qaac.int";
    try {
        create = temp ? &MP4FileX::CreateTemp : &MP4FileX::Create;
        (m_mp4file.*create)(
                    strutil::w2us(m_filename).c_str(),
                    0, // flags
                    1, // add_ftypes
                    0, // add_iods
                    "M4A ", // majorBrand
                    0, // minorVersion
                    const_cast<char**>(compatibleBrands), 
                    util::sizeof_array(compatibleBrands));
    } catch (mp4v2::impl::Exception *e) {
        m_mp4file.ResetFile();
        handle_mp4error(e);
    }
}

void MP4SinkBase::close()
{
    if (!m_closed) {
        m_closed = true;
        try {
            m_mp4file.Close();
        } catch (mp4v2::impl::Exception *e) {
            handle_mp4error(e);
        }
    }
}

MP4Sink::MP4Sink(const std::wstring &path,
                 const std::vector<uint8_t> &cookie,
                 uint32_t fcc, uint32_t trim, bool temp)
        : MP4SinkBase(path, temp), m_sample_id(0), m_trim(trim)
{
    std::vector<uint8_t> config;
    parseMagicCookieAAC(cookie, &config);
    try {
        unsigned index, rate, chconfig;
        parseDecSpecificConfig(config, &index, &rate, &chconfig);
        m_mp4file.SetTimeScale(rate);
        m_track_id = m_mp4file.AddAudioTrack(rate, 1024, MP4_MPEG4_AUDIO_TYPE);
        /*
         * According to ISO 14496-12 8.16.3, 
         * ChannelCount of AusioSampleEntry is either 1 or 2.
         */
        m_mp4file.SetIntegerProperty(
                "moov.trak.mdia.minf.stbl.stsd.mp4a.channels",
                chconfig == 1 ? 1 : 2);
        /* Looks like iTunes sets upsampled scale here */
        if (fcc == 'aach') { 
            uint64_t scale = static_cast<uint64_t>(rate) << 17;
            m_mp4file.SetIntegerProperty(
                "moov.trak.mdia.minf.stbl.stsd.mp4a.timeScale",
                scale);
        }
        m_mp4file.SetTrackESConfiguration(m_track_id, &config[0],
                                          config.size());
    } catch (mp4v2::impl::Exception *e) {
        handle_mp4error(e);
    }
}

ALACSink::ALACSink(const std::wstring &path,
        const std::vector<uint8_t> &magicCookie, bool temp)
        : MP4SinkBase(path, temp)
{
    try {
        std::vector<uint8_t> alac, chan;
        parseMagicCookieALAC(magicCookie, &alac, &chan);
        if (alac.size() != 24)
            throw std::runtime_error("Invalid ALACSpecificConfig!");
        if (chan.size() && chan.size() != 12)
            throw std::runtime_error("Invalid ALACChannelLayout!");

        m_track_id = m_mp4file.AddAlacAudioTrack(&alac[0],
                                                 chan.size() ? &chan[0] : 0);
    } catch (mp4v2::impl::Exception *e) {
        handle_mp4error(e);
    }
}

ADTSSink::ADTSSink(const std::wstring &path, const std::vector<uint8_t> &cookie)
    : m_fp(win32::fopen(path, L"wb"))
{
    init(cookie);
}

ADTSSink::ADTSSink(const std::shared_ptr<FILE> &fp,
                   const std::vector<uint8_t> &cookie)
    : m_fp(fp)
{
    init(cookie);
}

void ADTSSink::writeSamples(const void *data, size_t length, size_t nsamples)
{
    BitStream bs;
    bs.put(0xfff, 12); // syncword
    bs.put(0, 1);  // ID(MPEG identifier). 0 for MPEG4, 1 for MPEG2
    bs.put(0, 2);  // layer. always 0
    bs.put(1, 1);  // protection absent. 1 means no CRC information
    bs.put(1, 2);  // profile, (MPEG-4 object type) - 1. 1 for AAC LC
    bs.put(m_sample_rate_index, 4); // sampling rate index
    bs.put(0, 1); // private bit
    bs.put(m_channel_config, 3); // channel configuration
    bs.put(0, 4); /*
                   * originaly/copy: 1
                   * home: 1
                   * copyright_identification_bit: 1
                   * copyright_identification_start: 1
                   */
    bs.put(length + 7, 13); // frame_length
    bs.put(0x7ff, 11); // adts_buffer_fullness, 0x7ff for VBR
    bs.put(0, 2); // number_of_raw_data_blocks_in_frame
    bs.byteAlign();

    if (write(fileno(m_fp.get()), bs.data(), 7) < 0 ||
        write(fileno(m_fp.get()), data, length) < 0)
    {
        win32::throw_error("write failed", _doserrno);
    }
}

void ADTSSink::init(const std::vector<uint8_t> &cookie)
{
    m_seekable = util::is_seekable(fileno(m_fp.get()));
    std::vector<uint8_t> config;
    parseMagicCookieAAC(cookie, &config);
    unsigned rate;
    parseDecSpecificConfig(config, &m_sample_rate_index, &rate,
                           &m_channel_config);
}
