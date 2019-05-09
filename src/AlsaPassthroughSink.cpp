/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AlsaPassthroughSink.h"

#include <stdint.h>
#include <limits.h>
#include <sys/utsname.h>
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

#define ALSA_MAX_CHANNELS 16
static enum AudioChannel LegacyALSAChannelMap[ALSA_MAX_CHANNELS + 1] = {
    AE_CH_FL      , AE_CH_FR      , AE_CH_BL      , AE_CH_BR      , AE_CH_FC      , AE_CH_LFE     , AE_CH_SL      , AE_CH_SR      ,
    AE_CH_UNKNOWN1, AE_CH_UNKNOWN2, AE_CH_UNKNOWN3, AE_CH_UNKNOWN4, AE_CH_UNKNOWN5, AE_CH_UNKNOWN6, AE_CH_UNKNOWN7, AE_CH_UNKNOWN8, // for p16v devices
    AE_CH_NULL
};

static enum AudioChannel LegacyALSAChannelMap51Wide[ALSA_MAX_CHANNELS + 1] = {
    AE_CH_FL      , AE_CH_FR      , AE_CH_SL      , AE_CH_SR      , AE_CH_FC      , AE_CH_LFE     , AE_CH_BL      , AE_CH_BR      ,
    AE_CH_UNKNOWN1, AE_CH_UNKNOWN2, AE_CH_UNKNOWN3, AE_CH_UNKNOWN4, AE_CH_UNKNOWN5, AE_CH_UNKNOWN6, AE_CH_UNKNOWN7, AE_CH_UNKNOWN8, // for p16v devices
    AE_CH_NULL
};

static const std::vector<uint32_t> s_sampleRates {
    32000,
    44100,
    48000,
    96000,
    192000
};

AlsaPassthroughSink::AlsaPassthroughSink() :
    m_pcm(NULL)
{
    // ensure that ALSA has been initialized
    if (!snd_config) {
        snd_config_update();
    }
}

AlsaPassthroughSink::~AlsaPassthroughSink()
{
    deinit();
}

AlsaPassthroughSink* AlsaPassthroughSink::create(std::string& device, AudioFormat& desiredFormat)
{
    auto* sink = new AlsaPassthroughSink();
    if (sink->init(device, desiredFormat))
        return sink;

    delete sink;
    return nullptr;
}

AlsaPassthroughSink* AlsaPassthroughSink::createPassthrough(std::string& device, AudioFormat& desiredFormat)
{
    auto* sink = new AlsaPassthroughSink();

}

inline AudioChannelLayout AlsaPassthroughSink::channelLayoutPassthrough(const AudioFormat& format)
{
    unsigned int count = 0;

    switch (format.streamInfo.type)
    {
    case StreamInfo::StreamType::DtsHdMaster:
    case StreamInfo::StreamType::TrueHd:
        count = 8;
        break;
    case StreamInfo::StreamType::STREAM_TYPE_DTSHD_CORE:
    case StreamInfo::StreamType::Dts512:
    case StreamInfo::StreamType::STREAM_TYPE_DTS_1024:
    case StreamInfo::StreamType::STREAM_TYPE_DTS_2048:
    case StreamInfo::StreamType::Ac3:
    case StreamInfo::StreamType::Eac3:
    case StreamInfo::StreamType::DtsHd:
        count = 2;
        break;
    default:
        count = 0;
        break;
    }

    AudioChannelLayout info;
    for (unsigned int i = 0; i < count; ++i) {
        info += AE_CH_RAW;
    }

    return info;
}

inline AudioChannelLayout AlsaPassthroughSink::GetChannelLayoutLegacy(const AudioFormat& format, unsigned int minChannels, unsigned int maxChannels)
{
    enum AudioChannel* channelMap = LegacyALSAChannelMap;
    unsigned int count = 0;

    if (format.sampleFormat == AudioSampleFormat::Bitstream) {
        return channelLayoutPassthrough(format);
    }

    // According to CEA-861-D only RL and RR are known. In case of a format having SL and SR channels
    // but no BR BL channels, we use the wide map in order to open only the num of channels really
    // needed.
    if (format.channelLayout.HasChannel(AE_CH_SL) && !format.channelLayout.HasChannel(AE_CH_BL))
    {
        channelMap = LegacyALSAChannelMap51Wide;
    }
    for (unsigned int c = 0; c < 8; ++c)
    {
        for (unsigned int i = 0; i < format.channelLayout.count(); ++i)
        {
            if (format.channelLayout[i] == channelMap[c])
            {
                count = c + 1;
                break;
            }
        }
    }
    count = std::max(count, minChannels);
    count = std::min(count, maxChannels);

    AudioChannelLayout info;
    for (unsigned int i = 0; i < count; ++i)
        info += channelMap[i];

    return info;
}

inline AudioChannelLayout AlsaPassthroughSink::GetChannelLayout(const AudioFormat& format, unsigned int channels)
{
    AudioChannelLayout info;
    std::string alsaMapStr("none");

    if (format.sampleFormat == AudioSampleFormat::Bitstream)
    {
        info = channelLayoutPassthrough(format);
    }
    else
    {
        // ask for the actual map
        snd_pcm_chmap_t* actualMap = snd_pcm_get_chmap(m_pcm);
        if (actualMap)
        {
            alsaMapStr = ALSAchmapToString(actualMap);

            info = ALSAchmapToAEChannelMap(actualMap);

            // "fake" a compatible map if it is more suitable for AE
            if (!info.ContainsChannels(format.channelLayout))
            {
                AudioChannelLayout infoAlternate = GetAlternateLayoutForm(info);
                if (infoAlternate.count())
                {
                    std::vector<AudioChannelLayout> alts;
                    alts.push_back(info);
                    alts.push_back(infoAlternate);
                    if (format.channelLayout.BestMatch(alts) == 1)
                        info = infoAlternate;
                }
            }

            // add empty channels as needed (with e.g. FL,FR,LFE in 4ch)
            while (info.count() < channels)
                info += AE_CH_UNKNOWN1;

            free(actualMap);
        }
        else
        {
            info = GetChannelLayoutLegacy(format, channels, channels);
        }
    }

    return info;
}

AudioChannel AlsaPassthroughSink::ALSAChannelToAEChannel(unsigned int alsaChannel)
{
    AudioChannel aeChannel;
    switch (alsaChannel)
    {
    case SND_CHMAP_FL:   aeChannel = AE_CH_FL; break;
    case SND_CHMAP_FR:   aeChannel = AE_CH_FR; break;
    case SND_CHMAP_FC:   aeChannel = AE_CH_FC; break;
    case SND_CHMAP_LFE:  aeChannel = AE_CH_LFE; break;
    case SND_CHMAP_RL:   aeChannel = AE_CH_BL; break;
    case SND_CHMAP_RR:   aeChannel = AE_CH_BR; break;
    case SND_CHMAP_FLC:  aeChannel = AE_CH_FLOC; break;
    case SND_CHMAP_FRC:  aeChannel = AE_CH_FROC; break;
    case SND_CHMAP_RC:   aeChannel = AE_CH_BC; break;
    case SND_CHMAP_SL:   aeChannel = AE_CH_SL; break;
    case SND_CHMAP_SR:   aeChannel = AE_CH_SR; break;
    case SND_CHMAP_TFL:  aeChannel = AE_CH_TFL; break;
    case SND_CHMAP_TFR:  aeChannel = AE_CH_TFR; break;
    case SND_CHMAP_TFC:  aeChannel = AE_CH_TFC; break;
    case SND_CHMAP_TC:   aeChannel = AE_CH_TC; break;
    case SND_CHMAP_TRL:  aeChannel = AE_CH_TBL; break;
    case SND_CHMAP_TRR:  aeChannel = AE_CH_TBR; break;
    case SND_CHMAP_TRC:  aeChannel = AE_CH_TBC; break;
    case SND_CHMAP_RLC:  aeChannel = AE_CH_BLOC; break;
    case SND_CHMAP_RRC:  aeChannel = AE_CH_BROC; break;
    default:             aeChannel = AE_CH_UNKNOWN1; break;
    }
    return aeChannel;
}

unsigned int AlsaPassthroughSink::AEChannelToALSAChannel(AudioChannel aeChannel)
{
    unsigned int alsaChannel;
    switch (aeChannel)
    {
    case AE_CH_FL:    alsaChannel = SND_CHMAP_FL; break;
    case AE_CH_FR:    alsaChannel = SND_CHMAP_FR; break;
    case AE_CH_FC:    alsaChannel = SND_CHMAP_FC; break;
    case AE_CH_LFE:   alsaChannel = SND_CHMAP_LFE; break;
    case AE_CH_BL:    alsaChannel = SND_CHMAP_RL; break;
    case AE_CH_BR:    alsaChannel = SND_CHMAP_RR; break;
    case AE_CH_FLOC:  alsaChannel = SND_CHMAP_FLC; break;
    case AE_CH_FROC:  alsaChannel = SND_CHMAP_FRC; break;
    case AE_CH_BC:    alsaChannel = SND_CHMAP_RC; break;
    case AE_CH_SL:    alsaChannel = SND_CHMAP_SL; break;
    case AE_CH_SR:    alsaChannel = SND_CHMAP_SR; break;
    case AE_CH_TFL:   alsaChannel = SND_CHMAP_TFL; break;
    case AE_CH_TFR:   alsaChannel = SND_CHMAP_TFR; break;
    case AE_CH_TFC:   alsaChannel = SND_CHMAP_TFC; break;
    case AE_CH_TC:    alsaChannel = SND_CHMAP_TC; break;
    case AE_CH_TBL:   alsaChannel = SND_CHMAP_TRL; break;
    case AE_CH_TBR:   alsaChannel = SND_CHMAP_TRR; break;
    case AE_CH_TBC:   alsaChannel = SND_CHMAP_TRC; break;
    case AE_CH_BLOC:  alsaChannel = SND_CHMAP_RLC; break;
    case AE_CH_BROC:  alsaChannel = SND_CHMAP_RRC; break;
    default:          alsaChannel = SND_CHMAP_UNKNOWN; break;
    }
    return alsaChannel;
}

AudioChannelLayout AlsaPassthroughSink::ALSAchmapToAEChannelMap(snd_pcm_chmap_t* alsaMap)
{
    AudioChannelLayout info;

    for (unsigned int i = 0; i < alsaMap->channels; i++)
        info += ALSAChannelToAEChannel(alsaMap->pos[i]);

    return info;
}

snd_pcm_chmap_t* AlsaPassthroughSink::AEChannelMapToALSAchmap(const AudioChannelLayout& info)
{
    int AECount = info.count();
    snd_pcm_chmap_t* alsaMap = (snd_pcm_chmap_t*)malloc(sizeof(snd_pcm_chmap_t) + AECount * sizeof(int));

    alsaMap->channels = AECount;

    for (int i = 0; i < AECount; i++)
        alsaMap->pos[i] = AEChannelToALSAChannel(info[i]);

    return alsaMap;
}

snd_pcm_chmap_t* AlsaPassthroughSink::CopyALSAchmap(snd_pcm_chmap_t* alsaMap)
{
    snd_pcm_chmap_t* copyMap = (snd_pcm_chmap_t*)malloc(sizeof(snd_pcm_chmap_t) + alsaMap->channels * sizeof(int));

    copyMap->channels = alsaMap->channels;
    memcpy(copyMap->pos, alsaMap->pos, alsaMap->channels * sizeof(int));

    return copyMap;
}

std::string AlsaPassthroughSink::ALSAchmapToString(snd_pcm_chmap_t* alsaMap)
{
    char buf[128] = { 0 };
    //! @bug ALSA bug - buffer overflow by a factor of 2 is possible
    //! http://mailman.alsa-project.org/pipermail/alsa-devel/2014-December/085815.html
    int err = snd_pcm_chmap_print(alsaMap, sizeof(buf) / 2, buf);
    if (err < 0)
        return "Error";
    return std::string(buf);
}

AudioChannelLayout AlsaPassthroughSink::GetAlternateLayoutForm(const AudioChannelLayout& info)
{
    AudioChannelLayout altLayout;

    // only handle symmetrical layouts
    if (info.HasChannel(AE_CH_BL) == info.HasChannel(AE_CH_BR) &&
            info.HasChannel(AE_CH_SL) == info.HasChannel(AE_CH_SR) &&
            info.HasChannel(AE_CH_BLOC) == info.HasChannel(AE_CH_BROC))
    {
        /* CEA-861-D used by HDMI 1.x has 7.1 as back+back-x-of-center, not
     * side+back. Mangle it here. */
        if (info.HasChannel(AE_CH_SL) && info.HasChannel(AE_CH_BL) && !info.HasChannel(AE_CH_BLOC))
        {
            altLayout = info;
            altLayout.ReplaceChannel(AE_CH_BL, AE_CH_BLOC);
            altLayout.ReplaceChannel(AE_CH_BR, AE_CH_BROC);
            altLayout.ReplaceChannel(AE_CH_SL, AE_CH_BL);
            altLayout.ReplaceChannel(AE_CH_SR, AE_CH_BR);
        }
        // same in reverse
        else if (!info.HasChannel(AE_CH_SL) && info.HasChannel(AE_CH_BL) && info.HasChannel(AE_CH_BLOC))
        {
            altLayout = info;
            altLayout.ReplaceChannel(AE_CH_BL, AE_CH_SL);
            altLayout.ReplaceChannel(AE_CH_BR, AE_CH_SR);
            altLayout.ReplaceChannel(AE_CH_BLOC, AE_CH_BL);
            altLayout.ReplaceChannel(AE_CH_BROC, AE_CH_BR);
        }
        /* We have side speakers but no back speakers, allow map to back
     * speakers. */
        else if (info.HasChannel(AE_CH_SL) && !info.HasChannel(AE_CH_BL))
        {
            altLayout = info;
            altLayout.ReplaceChannel(AE_CH_SL, AE_CH_BL);
            altLayout.ReplaceChannel(AE_CH_SR, AE_CH_BR);
        }
        // reverse
        else if (!info.HasChannel(AE_CH_SL) && info.HasChannel(AE_CH_BL))
        {
            altLayout = info;
            altLayout.ReplaceChannel(AE_CH_BL, AE_CH_SL);
            altLayout.ReplaceChannel(AE_CH_BR, AE_CH_SR);
        }
    }
    return altLayout;
}

snd_pcm_chmap_t* AlsaPassthroughSink::SelectALSAChannelMap(const AudioChannelLayout& info)
{
    snd_pcm_chmap_t* chmap = NULL;
    snd_pcm_chmap_query_t** supportedMaps;

    supportedMaps = snd_pcm_query_chmaps(m_pcm);

    if (!supportedMaps)
        return NULL;

    AudioChannelLayout infoAlternate = GetAlternateLayoutForm(info);

    /* for efficiency, first try to find an exact match, and only then fallback
   * to searching for less perfect matches */
    int i = 0;
    for (snd_pcm_chmap_query_t* supportedMap = supportedMaps[i++];
         supportedMap; supportedMap = supportedMaps[i++])
    {
        if (supportedMap->map.channels == info.count())
        {
            AudioChannelLayout candidate = ALSAchmapToAEChannelMap(&supportedMap->map);
            const AudioChannelLayout* selectedInfo = &info;

            if (!candidate.ContainsChannels(info) || !info.ContainsChannels(candidate))
            {
                selectedInfo = &infoAlternate;
                if (!candidate.ContainsChannels(infoAlternate) || !infoAlternate.ContainsChannels(candidate))
                    continue;
            }

            if (supportedMap->type == SND_CHMAP_TYPE_VAR)
            {
                // device supports the AE map directly
                chmap = AEChannelMapToALSAchmap(*selectedInfo);
                break;
            }
            else
            {
                // device needs 1:1 remapping
                chmap = CopyALSAchmap(&supportedMap->map);
                break;
            }
        }
    }

    // if no exact chmap was found, fallback to best-effort
    if (!chmap)
    {
        AudioChannelLayout allChannels;
        std::vector<AudioChannelLayout> supportedMapsAE;

        // Convert the ALSA maps to AE maps.
        int i = 0;
        for (snd_pcm_chmap_query_t* supportedMap = supportedMaps[i++];
             supportedMap; supportedMap = supportedMaps[i++])
            supportedMapsAE.push_back(ALSAchmapToAEChannelMap(&supportedMap->map));

        int score = 0;
        int best = info.BestMatch(supportedMapsAE, &score);

        // see if we find a better result with the alternate form
        if (infoAlternate.count() && score < 0)
        {
            int scoreAlt = 0;
            int bestAlt = infoAlternate.BestMatch(supportedMapsAE, &scoreAlt);
            if (scoreAlt > score)
                best = bestAlt;
        }

        if (best > 0)
            chmap = CopyALSAchmap(&supportedMaps[best]->map);
    }

    snd_pcm_free_chmaps(supportedMaps);
    return chmap;
}

std::string AlsaPassthroughSink::aesParameters(const AudioFormat& format)
{
    std::string params;

    if (m_passthrough)
        params = "AES0=0x06";
    else
        params = "AES0=0x04";

    params += ",AES1=0x82,AES2=0x00";

    if (m_passthrough && format.channelLayout.count() == 8) params += ",AES3=0x09";
    else if (format.sampleRate == 192000) params += ",AES3=0x0e";
    else if (format.sampleRate == 176400) params += ",AES3=0x0c";
    else if (format.sampleRate ==  96000) params += ",AES3=0x0a";
    else if (format.sampleRate ==  88200) params += ",AES3=0x08";
    else if (format.sampleRate ==  48000) params += ",AES3=0x02";
    else if (format.sampleRate ==  44100) params += ",AES3=0x00";
    else if (format.sampleRate ==  32000) params += ",AES3=0x03";
    else params += ",AES3=0x01";

    return params;
}

bool AlsaPassthroughSink::init(std::string& device, AudioFormat& format)
{
    m_initDevice = device;
    m_initFormat = format;
    ALSAConfig inconfig, outconfig;
    inconfig.format = format.sampleFormat;
    inconfig.sampleRate = format.sampleRate;

    /*
   * We can't use the better GetChannelLayout() at this point as the device
   * is not opened yet, and we need inconfig.channels to select the correct
   * device... Legacy layouts should be accurate enough for device selection
   * in all cases, though.
   */
    inconfig.channels = GetChannelLayoutLegacy(format, 2, 8).count();

    // if we are raw, correct the data format
    if (format.sampleFormat == AudioSampleFormat::Bitstream)
    {
        inconfig.format   = AudioSampleFormat::S16NE;
        m_passthrough     = true;
    }
    else
    {
        m_passthrough   = false;
    }

    if (inconfig.channels == 0)
    {
        std::cerr << "CAESinkALSA::Initialize - Unable to open the requested channel layout";
        return false;
    }

    AudioDeviceType devType = AEDeviceTypeFromName(device);

    std::string AESParams;
    /* digital interfaces should have AESx set, though in practice most
   * receivers don't care */
    if (m_passthrough || devType == AudioDeviceType::Hdmi || devType == AudioDeviceType::Spdif)
        AESParams = aesParameters(format);

    std::cout << "CAESinkALSA::Initialize - Attempting to open device " << device.c_str();

    // get the sound config
    snd_config_t *config;
    snd_config_copy(&config, snd_config);

    if (!openAudioDevice(device, AESParams, inconfig.channels, &m_pcm, config))
    {
        std::cerr << "CAESinkALSA::Initialize - failed to initialize device " << device.c_str();
        snd_config_delete(config);
        return false;
    }

    // get the actual device name that was used
    m_device = snd_pcm_name(m_pcm);
    std::cout << "CAESinkALSA::Initialize - Opened device " << m_device;

    // free the sound config
    snd_config_delete(config);

    snd_pcm_chmap_t* selectedChmap = NULL;
    if (!m_passthrough)
    {
        selectedChmap = SelectALSAChannelMap(format.channelLayout);
        if (selectedChmap)
        {
            // update wanted channel count according to the selected map
            inconfig.channels = selectedChmap->channels;
        }
    }

    if (!InitializeHW(inconfig, outconfig) || !InitializeSW(outconfig))
    {
        free(selectedChmap);
        return false;
    }

    if (selectedChmap)
    {
        // failure is OK, that likely just means the selected chmap is fixed already
        snd_pcm_set_chmap(m_pcm, selectedChmap);
        free(selectedChmap);
    }

    // we want it blocking
    snd_pcm_nonblock(m_pcm, 0);
    snd_pcm_prepare (m_pcm);

    if (m_passthrough && inconfig.channels != outconfig.channels)
    {
        std::cout << "CAESinkALSA::Initialize - could not open required number of channels";
        return false;
    }
    // adjust format to the configuration we got
    format.channelLayout = GetChannelLayout(format, outconfig.channels);
    // we might end up with an unusable channel layout that contains only UNKNOWN
    // channels, let's do a sanity check.
    if (!format.channelLayout.IsLayoutValid())
        return false;

    format.sampleRate = outconfig.sampleRate;
    format.m_frames = outconfig.periodSize;
    format.m_frameSize = outconfig.frameSize;
    format.sampleFormat = outconfig.format;

    m_format              = format;
    m_formatSampleRateMul = 1.0 / (double)m_format.sampleRate;

    return true;
}


bool AlsaPassthroughSink::initPassthrough(AudioDeviceInfo& device, AudioFormat& format)
{
    m_initDevice = device.deviceName;
    m_initFormat = format;
    ALSAConfig inconfig, outconfig;
    inconfig.format = format.sampleFormat;
    inconfig.sampleRate = format.sampleRate;
    inconfig.channels = channelLayoutPassthrough(format).count();
    inconfig.format   = AudioSampleFormat::S16NE;
    m_passthrough     = true;

    if (inconfig.channels == 0) {
        std::cerr << "CAESinkALSA::Initialize - Unable to open the requested channel layout";
        return false;
    }
    std::cout << "CAESinkALSA::Initialize - Attempting to open device " << device.deviceName << std::endl;

    // get the sound config
    snd_config_t *config;
    snd_config_copy(&config, snd_config);

    if (!openAudioDevice(device.deviceName, aesParameters(format), inconfig.channels, &m_pcm, config))
    {
        std::cerr << "CAESinkALSA::Initialize - failed to initialize device " << device << std::endl;
        snd_config_delete(config);
        return false;
    }

    // get the actual device name that was used
    m_device = snd_pcm_name(m_pcm);
    std::cout << "CAESinkALSA::Initialize - Opened device " << m_device;

    // free the sound config
    snd_config_delete(config);

    if (!InitializeHW(inconfig, outconfig) || !InitializeSW(outconfig)) {
        return false;
    }

    // we want it blocking
    snd_pcm_nonblock(m_pcm, 0);
    snd_pcm_prepare (m_pcm);

    if (inconfig.channels != outconfig.channels) {
        std::cout << "CAESinkALSA::Initialize - could not open required number of channels";
        return false;
    }

    // adjust format to the configuration we got
    format.channelLayout = channelLayoutPassthrough(format);
    // we might end up with an unusable channel layout that contains only UNKNOWN
    // channels, let's do a sanity check.
    if (!format.channelLayout.IsLayoutValid())
        return false;

    format.sampleRate = outconfig.sampleRate;
    format.m_frames = outconfig.periodSize;
    format.m_frameSize = outconfig.frameSize;
    format.sampleFormat = outconfig.format;

    m_format              = format;
    m_formatSampleRateMul = 1.0 / (double)m_format.sampleRate;

    return true;
}

snd_pcm_format_t AlsaPassthroughSink::toAlsa(AudioSampleFormat format)
{
    switch (format) {
    case AudioSampleFormat::S16NE:  return SND_PCM_FORMAT_S16;
    case AudioSampleFormat::S16LE:  return SND_PCM_FORMAT_S16_LE;
    case AudioSampleFormat::S16BE:  return SND_PCM_FORMAT_S16_BE;
    case AudioSampleFormat::S32NE:  return SND_PCM_FORMAT_S32;
    case AudioSampleFormat::Float:  return SND_PCM_FORMAT_FLOAT;
    case AudioSampleFormat::Bitstream:    return SND_PCM_FORMAT_S16;
    default:
        return SND_PCM_FORMAT_UNKNOWN;
    }
}

bool AlsaPassthroughSink::InitializeHW(const ALSAConfig &inconfig, ALSAConfig &outconfig)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    memset(hw_params, 0, snd_pcm_hw_params_sizeof());

    snd_pcm_hw_params_any(m_pcm, hw_params);
    snd_pcm_hw_params_set_access(m_pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

    unsigned int sampleRate   = inconfig.sampleRate;
    snd_pcm_hw_params_set_rate_near    (m_pcm, hw_params, &sampleRate, NULL);

    unsigned int channelCount = inconfig.channels;
    // select a channel count >=wanted, or otherwise the highest available
    if (snd_pcm_hw_params_set_channels_min(m_pcm, hw_params, &channelCount) == 0)
        snd_pcm_hw_params_set_channels_first(m_pcm, hw_params, &channelCount);
    else
        snd_pcm_hw_params_set_channels_last(m_pcm, hw_params, &channelCount);

    // ensure we opened X channels or more
    if (inconfig.channels > channelCount)
    {
        std::cout << "CAESinkALSA::InitializeHW - Unable to open the required number of channels";
    }

    // update outconfig
    outconfig.channels = channelCount;

    snd_pcm_format_t fmt = toAlsa(inconfig.format);
    outconfig.format = inconfig.format;

    if (fmt == SND_PCM_FORMAT_UNKNOWN)
    {
        // if we dont support the requested format, fallback to float
        fmt = SND_PCM_FORMAT_FLOAT;
        outconfig.format = AudioSampleFormat::Float;
    }

    snd_pcm_hw_params_t *hw_params_copy;
    snd_pcm_hw_params_alloca(&hw_params_copy);
    snd_pcm_hw_params_copy(hw_params_copy, hw_params); // copy what we have

    // try the data format
    if (snd_pcm_hw_params_set_format(m_pcm, hw_params, fmt) < 0)
    {
        // if the chosen format is not supported, try each one in descending order
        std::cout << "CAESinkALSA::InitializeHW - Your hardware does not support: " << outconfig.format;
        for (enum AudioSampleFormat i = AudioSampleFormat::Max; i > AudioSampleFormat::Invalid; i = (enum AudioSampleFormat)((int)i - 1))
        {
            if (i == AudioSampleFormat::Bitstream || i == AudioSampleFormat::Max)
                continue;

            if (m_passthrough && i != AudioSampleFormat::S16BE && i != AudioSampleFormat::S16LE)
                continue;

            fmt = toAlsa(i);
            if (fmt == SND_PCM_FORMAT_UNKNOWN)
                continue;

            snd_pcm_hw_params_copy(hw_params, hw_params_copy); // restore from copy
            if (snd_pcm_hw_params_set_format(m_pcm, hw_params, fmt) < 0)
            {
                fmt = SND_PCM_FORMAT_UNKNOWN;
                continue;
            }

            int fmtBits = CAEUtil::numBits(i);
            int bits    = snd_pcm_hw_params_get_sbits(hw_params);

            // skip bits check when alsa reports invalid sbits value
            if (bits > 0 && bits != fmtBits) {
                continue;
            }

            // record that the format fell back to X
            outconfig.format = i;
            std::cout << "CAESinkALSA::InitializeHW - Using data format " << outconfig.format;
            break;
        }

        // if we failed to find a valid output format
        if (fmt == SND_PCM_FORMAT_UNKNOWN)
        {
            std::cerr << "CAESinkALSA::InitializeHW - Unable to find a suitable output format";
            return false;
        }
    }

    snd_pcm_uframes_t periodSize, bufferSize;
    snd_pcm_hw_params_get_buffer_size_max(hw_params, &bufferSize);
    snd_pcm_hw_params_get_period_size_max(hw_params, &periodSize, NULL);

    /*
   We want to make sure, that we have max 200 ms Buffer with
   a periodSize of approx 50 ms. Choosing a higher bufferSize
   will cause problems with menu sounds. Buffer will be increased
   after those are fixed.
  */
    periodSize  = std::min(periodSize, (snd_pcm_uframes_t) sampleRate / 20);
    bufferSize  = std::min(bufferSize, (snd_pcm_uframes_t) sampleRate / 5);

    /*
   According to upstream we should set buffer size first - so make sure it is always at least
   4x period size to not get underruns (some systems seem to have issues with only 2 periods)
  */
    periodSize = std::min(periodSize, bufferSize / 4);

    std::cout << "CAESinkALSA::InitializeHW - Request: periodSize:" << periodSize << ", bufferSize: " << bufferSize;

    snd_pcm_hw_params_copy(hw_params_copy, hw_params); // copy what we have and is already working

    // Make sure to not initialize too large to not cause underruns
    snd_pcm_uframes_t periodSizeMax = bufferSize / 3;
    if(snd_pcm_hw_params_set_period_size_max(m_pcm, hw_params_copy, &periodSizeMax, NULL) != 0)
    {
        snd_pcm_hw_params_copy(hw_params_copy, hw_params); // restore working copy
        std::cout << "CAESinkALSA::InitializeHW - Request: Failed to limit periodSize to: " << periodSizeMax;
    }

    // first trying bufferSize, PeriodSize
    // for more info see here:
    // http://mailman.alsa-project.org/pipermail/alsa-devel/2009-September/021069.html
    // the last three tries are done as within pulseaudio

    // backup periodSize and bufferSize first. Restore them after every failed try
    snd_pcm_uframes_t periodSizeTemp, bufferSizeTemp;
    periodSizeTemp = periodSize;
    bufferSizeTemp = bufferSize;
    if (snd_pcm_hw_params_set_buffer_size_near(m_pcm, hw_params_copy, &bufferSize) != 0
            || snd_pcm_hw_params_set_period_size_near(m_pcm, hw_params_copy, &periodSize, NULL) != 0
            || snd_pcm_hw_params(m_pcm, hw_params_copy) != 0)
    {
        bufferSize = bufferSizeTemp;
        periodSize = periodSizeTemp;
        // retry with PeriodSize, bufferSize
        snd_pcm_hw_params_copy(hw_params_copy, hw_params); // restore working copy
        if (snd_pcm_hw_params_set_period_size_near(m_pcm, hw_params_copy, &periodSize, NULL) != 0
                || snd_pcm_hw_params_set_buffer_size_near(m_pcm, hw_params_copy, &bufferSize) != 0
                || snd_pcm_hw_params(m_pcm, hw_params_copy) != 0)
        {
            // try only periodSize
            periodSize = periodSizeTemp;
            snd_pcm_hw_params_copy(hw_params_copy, hw_params); // restore working copy
            if(snd_pcm_hw_params_set_period_size_near(m_pcm, hw_params_copy, &periodSize, NULL) != 0
                    || snd_pcm_hw_params(m_pcm, hw_params_copy) != 0)
            {
                // try only BufferSize
                bufferSize = bufferSizeTemp;
                snd_pcm_hw_params_copy(hw_params_copy, hw_params); // restore working copy
                if (snd_pcm_hw_params_set_buffer_size_near(m_pcm, hw_params_copy, &bufferSize) != 0
                        || snd_pcm_hw_params(m_pcm, hw_params_copy) != 0)
                {
                    // set default that Alsa would choose
                    std::cerr << "CAESinkAlsa::InitializeHW - Using default alsa values - set failed";
                    if (snd_pcm_hw_params(m_pcm, hw_params) != 0)
                    {
                        std::cout << "CAESinkALSA::InitializeHW - Could not init a valid sink";
                        return false;
                    }
                }
            }
            // reread values when alsa default was kept
            snd_pcm_get_params(m_pcm, &bufferSize, &periodSize);
        }
    }

    std::cout << "CAESinkALSA::InitializeHW - Got: periodSize: " << periodSize << ", bufferSize: " << bufferSize;

    // set the format parameters
    outconfig.sampleRate   = sampleRate;

    // if periodSize is too small Audio Engine might starve
    m_fragmented = false;
    unsigned int fragments = 1;
    if (periodSize < AE_MIN_PERIODSIZE)
    {
        fragments = std::ceil((double) AE_MIN_PERIODSIZE / periodSize);
        std::cout << "Audio Driver reports too low periodSize: " << periodSize << ", will use: " << fragments;
        m_fragmented = true;
    }

    m_originalPeriodSize   = periodSize;
    outconfig.periodSize   = fragments * periodSize;
    outconfig.frameSize    = snd_pcm_frames_to_bytes(m_pcm, 1);

    m_bufferSize = (unsigned int)bufferSize;
    m_timeout    = std::ceil((double)(bufferSize * 1000) / (double)sampleRate);

    std::cout << "CAESinkALSA::InitializeHW - Setting timeout to " << m_timeout;

    return true;
}

bool AlsaPassthroughSink::InitializeSW(const ALSAConfig &inconfig)
{
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_uframes_t boundary;

    snd_pcm_sw_params_alloca(&sw_params);
    memset(sw_params, 0, snd_pcm_sw_params_sizeof());

    snd_pcm_sw_params_current              (m_pcm, sw_params);
    snd_pcm_sw_params_set_start_threshold  (m_pcm, sw_params, INT_MAX);
    snd_pcm_sw_params_set_silence_threshold(m_pcm, sw_params, 0);
    snd_pcm_sw_params_get_boundary         (sw_params, &boundary);
    snd_pcm_sw_params_set_silence_size     (m_pcm, sw_params, boundary);
    snd_pcm_sw_params_set_avail_min        (m_pcm, sw_params, inconfig.periodSize);

    if (snd_pcm_sw_params(m_pcm, sw_params) < 0)
    {
        std::cerr << "CAESinkALSA::InitializeSW - Failed to set the parameters";
        return false;
    }

    return true;
}

void AlsaPassthroughSink::deinit()
{
    if (m_pcm)
    {
        Stop();
        snd_pcm_close(m_pcm);
        m_pcm = NULL;
    }
}

void AlsaPassthroughSink::Stop()
{
    if (!m_pcm)
        return;
    snd_pcm_drop(m_pcm);
}

double AlsaPassthroughSink::GetCacheTotal()
{
    return (double)m_bufferSize * m_formatSampleRateMul;
}

unsigned int AlsaPassthroughSink::AddPackets(uint8_t **data, unsigned int frames, unsigned int offset)
{
    if (!m_pcm)
    {
        std::cerr << "CAESinkALSA - Tried to add packets without a sink";
        return INT_MAX;
    }

    void *buffer = data[0]+offset*m_format.m_frameSize;
    unsigned int amount = 0;
    int64_t data_left = (int64_t) frames;
    int frames_written = 0;

    while (data_left > 0)
    {
        if (m_fragmented)
            amount = std::min((unsigned int) data_left, m_originalPeriodSize);
        else // take care as we can come here a second time if the sink does not eat all data
            amount = (unsigned int) data_left;

        int ret = snd_pcm_writei(m_pcm, buffer, amount);
        if (ret < 0)
        {
            std::cerr << "CAESinkALSA - snd_pcm_writei, trying to recover" << snd_strerror(ret);
            ret = snd_pcm_recover(m_pcm, ret, 1);
            if(ret < 0)
            {
                HandleError("snd_pcm_writei(1)", ret);
                ret = snd_pcm_writei(m_pcm, buffer, amount);
                if (ret < 0)
                {
                    HandleError("snd_pcm_writei(2)", ret);
                    ret = 0;
                }
            }
        }

        if ( ret > 0 && snd_pcm_state(m_pcm) == SND_PCM_STATE_PREPARED)
            snd_pcm_start(m_pcm);

        if (ret <= 0)
            break;

        frames_written += ret;
        data_left -= ret;
        buffer = data[0]+offset*m_format.m_frameSize + frames_written*m_format.m_frameSize;
    }
    return frames_written;
}

void AlsaPassthroughSink::HandleError(const char* name, int err)
{
    switch(err)
    {
    case -EPIPE:
        std::cerr << "CAESinkALSA::HandleError underrun" << name;
        if ((err = snd_pcm_prepare(m_pcm)) < 0)
            std::cerr << "CAESinkALSA::HandleError " << name << "snd_pcm_prepare returned: " << snd_strerror(err);
        break;

    case -ESTRPIPE:
        std::cerr << "CAESinkALSA::HandleError " << name << "Resuming after suspend";

        // try to resume the stream
        while((err = snd_pcm_resume(m_pcm)) == -EAGAIN) {
            usleep(1000);
        }

        // if the hardware doesnt support resume, prepare the stream
        if (err == -ENOSYS)
            if ((err = snd_pcm_prepare(m_pcm)) < 0)
                std::cerr << "CAESinkALSA::HandleError " << name << "snd_pcm_prepare returned: " << snd_strerror(err);
        break;

    default:
        std::cerr << "CAESinkALSA::HandleError " << name << "snd_pcm_writei returned: " << snd_strerror(err);
        break;
    }
}

void AlsaPassthroughSink::Drain()
{
    if (!m_pcm)
        return;

    snd_pcm_drain(m_pcm);
    snd_pcm_prepare(m_pcm);
}

void AlsaPassthroughSink::AppendParams(std::string &device, const std::string &params)
{
    /* Note: escaping, e.g. "plug:'something:X=y'" isn't handled,
   * but it is not normally encountered at this point. */

    device += (device.find(':') == std::string::npos) ? ':' : ',';
    device += params;
}

bool AlsaPassthroughSink::TryDevice(const std::string &name, snd_pcm_t **pcmp, snd_config_t *lconf)
{
    /* Check if this device was already open (e.g. when checking for supported
   * channel count in EnumerateDevice()) */
    if (*pcmp)
    {
        if (name == snd_pcm_name(*pcmp))
            return true;

        snd_pcm_close(*pcmp);
        *pcmp = NULL;
    }

    int err = snd_pcm_open_lconf(pcmp, name.c_str(), SND_PCM_STREAM_PLAYBACK, (SND_PCM_NO_AUTO_FORMAT | SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_RESAMPLE), lconf);
    if (err < 0)
    {
        std::cout << "CAESinkALSA - Unable to open device: " << name;
    }

    return err == 0;
}

bool AlsaPassthroughSink::TryDeviceWithParams(const std::string &name, const std::string &params, snd_pcm_t **pcmp, snd_config_t *lconf)
{
    if (!params.empty())
    {
        std::string nameWithParams = name;
        AppendParams(nameWithParams, params);
        if (TryDevice(nameWithParams, pcmp, lconf))
            return true;
    }

    /* Try the variant without extra parameters.
   * Custom devices often do not take the AESx parameters, for example.
   */
    return TryDevice(name, pcmp, lconf);
}

bool AlsaPassthroughSink::openAudioDevice(const std::string &name, const std::string &params, int channels, snd_pcm_t **pcmp, snd_config_t *lconf)
{
    /* Special name denoting surroundXX mangling. This is needed for some
   * devices for multichannel to work. */
    if (name == "@" || name.substr(0, 2) == "@:")
    {
        std::string openName = name.substr(1);

        /* These device names allow alsa-lib to perform special routing if needed
     * for multichannel to work with the audio hardware.
     * Fall through in switch() so that devices with more channels are
     * added as fallback. */
        switch (channels)
        {
        case 3:
        case 4:
            if (TryDeviceWithParams("surround40" + openName, params, pcmp, lconf))
                return true;
        case 5:
        case 6:
            if (TryDeviceWithParams("surround51" + openName, params, pcmp, lconf))
                return true;
        case 7:
        case 8:
            if (TryDeviceWithParams("surround71" + openName, params, pcmp, lconf))
                return true;
        }

        /* Try "sysdefault" and "default" (they provide dmix if needed, and route
     * audio to all extra channels on subdeviced cards),
     * unless the selected devices is not DEV=0 of the card, in which case
     * "sysdefault" and "default" would point to another device.
     * "sysdefault" is a newish device name that won't be overwritten in case
     * system configuration redefines "default". "default" is still tried
     * because "sysdefault" is rather new. */
        size_t devPos = openName.find(",DEV=");
        if (devPos == std::string::npos || (devPos + 5 < openName.size() && openName[devPos+5] == '0'))
        {
            // "sysdefault" and "default" do not have "DEV=0", drop it
            std::string nameWithoutDev = openName;
            if (devPos != std::string::npos)
                nameWithoutDev.erase(nameWithoutDev.begin() + devPos, nameWithoutDev.begin() + devPos + 6);

            if (TryDeviceWithParams("sysdefault" + nameWithoutDev, params, pcmp, lconf)
                    || TryDeviceWithParams("default" + nameWithoutDev, params, pcmp, lconf))
                return true;
        }

        // Try "front" (no dmix, no audio in other channels on subdeviced cards)
        if (TryDeviceWithParams("front" + openName, params, pcmp, lconf))
            return true;

    }
    else
    {
        // Non-surroundXX device, just add it
        if (TryDeviceWithParams(name, params, pcmp, lconf))
            return true;
    }

    return false;
}

AudioDeviceInfos AlsaPassthroughSink::enumerateDevices()
{
    AudioDeviceInfos list;

    // ensure that ALSA has been initialized
    snd_lib_error_set_handler(sndLibErrorHandler);
    if (!snd_config) {
        snd_config_update();
    }

    snd_config_t *config;
    snd_config_copy(&config, snd_config);

    /* Always enumerate the default device.
   * Note: If "default" is a stereo device, EnumerateDevice()
   * will automatically add "@" instead to enable surroundXX mangling.
   * We don't want to do that if "default" can handle multichannel
   * itself (e.g. in case of a pulseaudio server). */
    //enumerateDevice(list, "default", "", config);

    void **hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        std::cout << "CAESinkALSA - Unable to get a list of devices";
        return {};
    }

    std::string defaultDescription;

    for (void** hint = hints; *hint != NULL; ++hint)
    {
        char *io = snd_device_name_get_hint(*hint, "IOID");
        char *name = snd_device_name_get_hint(*hint, "NAME");
        char *desc = snd_device_name_get_hint(*hint, "DESC");
        if ((!io || strcmp(io, "Output") == 0) && name
                && strcmp(name, "null") != 0)
        {
            std::string baseName = std::string(name);
            baseName = baseName.substr(0, baseName.find(':'));

            if (strcmp(name, "default") == 0)
            {
                // added already, but lets get the description if we have one
                if (desc)
                    defaultDescription = desc;
            }
            else if (baseName == "front")
            {
                // Enumerate using the surroundXX mangling
                /* do not enumerate basic "front", it is already handled
         * by the default "@" entry added in the very beginning */
                if (strcmp(name, "front") != 0)
                    enumerateDevice(list, std::string("@") + (name+5), desc ? desc : name, config);
            }

            // Do not enumerate "default", it is already enumerated above.

            /* Do not enumerate the surroundXX devices, those are always accompanied
       * with a "front" device and it is handled above as "@". The below
       * devices plus sysdefault will be automatically used if available
       * for a "@" device.
       * sysdefault devices are enumerated as not all cards have front/surround
       * devices. For cards with front/surround devices the sysdefault
       * entry will be removed in a second pass after enumeration.
       */

            /* Ubuntu has patched their alsa-lib so that "defaults.namehint.extended"
       * defaults to "on" instead of upstream "off", causing lots of unwanted
       * extra devices (many of which are not actually routed properly) to be
       * found by the enumeration process. Skip them as well ("hw", "dmix",
       * "plughw", "dsnoop"). */

            else if (baseName != "default"
                     && baseName != "surround40"
                     && baseName != "surround41"
                     && baseName != "surround50"
                     && baseName != "surround51"
                     && baseName != "surround71"
                     && baseName != "hw"
                     && baseName != "dmix"
                     && baseName != "plughw"
                     && baseName != "dsnoop")
            {
                enumerateDevice(list, name, desc ? desc : name, config);
            }
        }
        free(io);
        free(name);
        free(desc);
    }
    snd_device_name_free_hint(hints);

    // set the displayname for default device
    if (!list.empty() && list[0].deviceName == "default")
    {
        // If we have one from a hint (DESC), use it
        if (!defaultDescription.empty())
            list[0].m_displayName = defaultDescription;
        // Otherwise use the discovered name or (unlikely) "Default"
        else if (list[0].m_displayName.empty())
            list[0].m_displayName = "Default";
    }

    // cards with surround entries where sysdefault should be removed
    std::set<std::string> cardsWithSurround;

    for (AudioDeviceInfos::iterator it1 = list.begin(); it1 != list.end(); ++it1)
    {
        std::string baseName = it1->deviceName.substr(0, it1->deviceName.find(':'));
        std::string card = GetParamFromName(it1->deviceName, "CARD");
        if (baseName == "@" && !card.empty())
            cardsWithSurround.insert(card);
    }

    if (!cardsWithSurround.empty())
    {
        // remove sysdefault entries where we already have a surround entry
        AudioDeviceInfos::iterator iter = list.begin();
        while (iter != list.end())
        {
            std::string baseName = iter->deviceName.substr(0, iter->deviceName.find(':'));
            std::string card = GetParamFromName(iter->deviceName, "CARD");
            if (baseName == "sysdefault" && cardsWithSurround.find(card) != cardsWithSurround.end())
                iter = list.erase(iter);
            else
                iter++;
        }
    }

    // lets check uniqueness, we may need to append DEV or CARD to DisplayName
    /* If even a single device of card/dev X clashes with Y, add suffixes to
   * all devices of both them, for clarity. */

    // clashing card names, e.g. "NVidia", "NVidia_2"
    std::set<std::string> cardsToAppend;

    // clashing basename + cardname combinations, e.g. ("hdmi","Nvidia")
    std::set<std::pair<std::string, std::string> > devsToAppend;

    for (AudioDeviceInfos::iterator it1 = list.begin(); it1 != list.end(); ++it1)
    {
        for (AudioDeviceInfos::iterator it2 = it1+1; it2 != list.end(); ++it2)
        {
            if (it1->m_displayName == it2->m_displayName
                    && it1->m_displayNameExtra == it2->m_displayNameExtra)
            {
                // something needs to be done
                std::string cardString1 = GetParamFromName(it1->deviceName, "CARD");
                std::string cardString2 = GetParamFromName(it2->deviceName, "CARD");

                if (cardString1 != cardString2)
                {
                    // card name differs, add identifiers to all devices
                    cardsToAppend.insert(cardString1);
                    cardsToAppend.insert(cardString2);
                    continue;
                }

                std::string devString1 = GetParamFromName(it1->deviceName, "DEV");
                std::string devString2 = GetParamFromName(it2->deviceName, "DEV");

                if (devString1 != devString2)
                {
                    // device number differs, add identifiers to all such devices
                    devsToAppend.insert(std::make_pair(it1->deviceName.substr(0, it1->deviceName.find(':')), cardString1));
                    devsToAppend.insert(std::make_pair(it2->deviceName.substr(0, it2->deviceName.find(':')), cardString2));
                    continue;
                }

                // if we got here, the configuration is really weird, just append the whole device string
                it1->m_displayName += " (" + it1->deviceName + ")";
                it2->m_displayName += " (" + it2->deviceName + ")";
            }
        }
    }

    for (std::set<std::string>::iterator it = cardsToAppend.begin();
         it != cardsToAppend.end(); ++it)
    {
        for (AudioDeviceInfos::iterator itl = list.begin(); itl != list.end(); ++itl)
        {
            std::string cardString = GetParamFromName(itl->deviceName, "CARD");
            if (cardString == *it)
                // "HDA NVidia (NVidia)", "HDA NVidia (NVidia_2)", ...
                itl->m_displayName += " (" + cardString + ")";
        }
    }

    for (std::set<std::pair<std::string, std::string> >::iterator it = devsToAppend.begin();
         it != devsToAppend.end(); ++it)
    {
        for (AudioDeviceInfos::iterator itl = list.begin(); itl != list.end(); ++itl)
        {
            std::string baseName = itl->deviceName.substr(0, itl->deviceName.find(':'));
            std::string cardString = GetParamFromName(itl->deviceName, "CARD");
            if (baseName == it->first && cardString == it->second)
            {
                std::string devString = GetParamFromName(itl->deviceName, "DEV");
                // "HDMI #0", "HDMI #1" ...
                itl->m_displayNameExtra += " #" + devString;
            }
        }
    }

    return list;
}

AudioDeviceType AlsaPassthroughSink::AEDeviceTypeFromName(const std::string &name)
{
    if (name.substr(0, 4) == "hdmi")
        return AudioDeviceType::Hdmi;
    else if (name.substr(0, 6) == "iec958" || name.substr(0, 5) == "spdif")
        return AudioDeviceType::Spdif;

    return AudioDeviceType::Pcm;
}

std::string AlsaPassthroughSink::GetParamFromName(const std::string &name, const std::string &param)
{
    // name = "hdmi:CARD=x,DEV=y" param = "CARD" => return "x"
    size_t parPos = name.find(param + '=');
    if (parPos != std::string::npos)
    {
        parPos += param.size() + 1;
        return name.substr(parPos, name.find_first_of(",'\"", parPos)-parPos);
    }

    return "";
}

void AlsaPassthroughSink::enumerateDevice(AudioDeviceInfos &list, const std::string &device, const std::string &description, snd_config_t *config)
{
    snd_pcm_t *pcmhandle = NULL;
    if (!openAudioDevice(device, "", ALSA_MAX_CHANNELS, &pcmhandle, config))
        return;

    snd_pcm_info_t *pcminfo;
    snd_pcm_info_alloca(&pcminfo);
    memset(pcminfo, 0, snd_pcm_info_sizeof());

    int err = snd_pcm_info(pcmhandle, pcminfo);
    if (err < 0)
    {
        std::cout << "CAESinkALSA - Unable to get pcm_info for: " << device.c_str();
        snd_pcm_close(pcmhandle);
    }

    int cardNr = snd_pcm_info_get_card(pcminfo);

    AudioDeviceInfo info;
    info.deviceName = device;
    info.deviceType = AEDeviceTypeFromName(device);

    if (cardNr >= 0)
    {
        // "HDA NVidia", "HDA Intel", "HDA ATI HDMI", "SB Live! 24-bit External", ...
        char *cardName;
        if (snd_card_get_name(cardNr, &cardName) == 0)
            info.m_displayName = cardName;

        if (info.deviceType == AudioDeviceType::Hdmi && info.m_displayName.size() > 5 &&
                info.m_displayName.substr(info.m_displayName.size()-5) == " HDMI")
        {
            // We already know this is HDMI, strip it
            info.m_displayName.erase(info.m_displayName.size()-5);
        }

        // "CONEXANT Analog", "USB Audio", "HDMI 0", "ALC889 Digital" ...
        std::string pcminfoName = snd_pcm_info_get_name(pcminfo);

        /*
     * Filter "USB Audio", in those cases snd_card_get_name() is more
     * meaningful already
     */
        if (pcminfoName != "USB Audio")
            info.m_displayNameExtra = pcminfoName;

        if (info.deviceType == AudioDeviceType::Hdmi)
        {
            // @TODO(mawe): to be implemented
        }
        else if (info.deviceType == AudioDeviceType::Spdif)
        {
            // append instead of replace, pcminfoName is useful for S/PDIF
            if (!info.m_displayNameExtra.empty())
                info.m_displayNameExtra += ' ';
            info.m_displayNameExtra += "S/PDIF";

            info.streamTypes.push_back(StreamInfo::StreamType::Ac3);
            info.streamTypes.push_back(StreamInfo::StreamType::STREAM_TYPE_DTSHD_CORE);
            info.streamTypes.push_back(StreamInfo::StreamType::STREAM_TYPE_DTS_1024);
            info.streamTypes.push_back(StreamInfo::StreamType::STREAM_TYPE_DTS_2048);
            info.streamTypes.push_back(StreamInfo::StreamType::Dts512);
            info.sampleFormat.push_back(AudioSampleFormat::Bitstream);
        }
        else if (info.m_displayNameExtra.empty())
        {
            /* for USB audio, it gets a bit confusing as there is
       * - "SB Live! 24-bit External"
       * - "SB Live! 24-bit External, S/PDIF"
       * so add "Analog" qualifier to the first one */
            info.m_displayNameExtra = "Analog";
        }

        /* "default" is a device that will be used for all inputs, while
     * "@" will be mangled to front/default/surroundXX as necessary */
        if (device == "@" || device == "default")
        {
            // Make it "Default (whatever)"
            info.m_displayName = "Default (" + info.m_displayName + (info.m_displayNameExtra.empty() ? "" : " " + info.m_displayNameExtra + ")");
            info.m_displayNameExtra = "";
        }

    }
    else
    {
        // virtual devices: "default", "pulse", ...
        /* description can be e.g. "PulseAudio Sound Server" - for hw devices it is
     * normally uninteresting, like "HDMI Audio Output" or "Default Audio Device",
     * so we only use it for virtual devices that have no better display name */
        info.m_displayName = description;
    }

    snd_pcm_hw_params_t *hwparams;
    snd_pcm_hw_params_alloca(&hwparams);
    memset(hwparams, 0, snd_pcm_hw_params_sizeof());

    // ensure we can get a playback configuration for the device
    if (snd_pcm_hw_params_any(pcmhandle, hwparams) < 0)
    {
        std::cout << "CAESinkALSA - No playback configurations available for device: " << device << std::endl;
        snd_pcm_close(pcmhandle);
        return;
    }

    // detect the available sample rates
    for (const auto rate : s_sampleRates) {
        if (snd_pcm_hw_params_test_rate(pcmhandle, hwparams, rate, 0) >= 0)
            info.sampleRates.push_back(rate);
    }

    // detect the channels available
    int channels = 0;
    for (int i = ALSA_MAX_CHANNELS; i >= 1; --i)
    {
        // Reopen the device if needed on the special "surroundXX" cases
        if (info.deviceType == AudioDeviceType::Pcm && (i == 8 || i == 6 || i == 4))
            openAudioDevice(device, "", i, &pcmhandle, config);

        if (snd_pcm_hw_params_test_channels(pcmhandle, hwparams, i) >= 0)
        {
            channels = i;
            break;
        }
    }

    if (device == "default" && channels == 2)
    {
        /* This looks like the ALSA standard default stereo dmix device, we
     * probably want to use "@" instead to get surroundXX. */
        snd_pcm_close(pcmhandle);
        enumerateDevice(list, "@", description, config);
        return;
    }

    AudioChannelLayout alsaChannels;
    snd_pcm_chmap_query_t** alsaMaps = snd_pcm_query_chmaps(pcmhandle);
    bool useEldChannels = (info.channels.count() > 0);
    if (alsaMaps)
    {
        int i = 0;
        for (snd_pcm_chmap_query_t* alsaMap = alsaMaps[i++];
             alsaMap; alsaMap = alsaMaps[i++])
        {
            AudioChannelLayout AEmap = ALSAchmapToAEChannelMap(&alsaMap->map);
            alsaChannels.AddMissingChannels(AEmap);
            if (!useEldChannels)
                info.channels.AddMissingChannels(AEmap);
        }
        snd_pcm_free_chmaps(alsaMaps);
    }
    else
    {
        for (int i = 0; i < channels; ++i)
        {
            if (!info.channels.HasChannel(LegacyALSAChannelMap[i]))
                info.channels += LegacyALSAChannelMap[i];
            alsaChannels += LegacyALSAChannelMap[i];
        }
    }

    // remove the channels from m_channels that we cant use
    info.channels.ResolveChannels(alsaChannels);

    // detect the PCM sample formats that are available
    for (enum AudioSampleFormat i = AudioSampleFormat::Max; i > AudioSampleFormat::Invalid; i = (enum AudioSampleFormat)((int)i - 1))
    {
        if (i == AudioSampleFormat::Bitstream || i == AudioSampleFormat::Max)
            continue;
        snd_pcm_format_t fmt = toAlsa(i);
        if (fmt == SND_PCM_FORMAT_UNKNOWN)
            continue;

        if (snd_pcm_hw_params_test_format(pcmhandle, hwparams, fmt) >= 0)
            info.sampleFormat.push_back(i);
    }

    if (info.deviceType == AudioDeviceType::Hdmi)
    {
        // we don't trust ELD information and push back our supported formats explicitly
        info.streamTypes.push_back(StreamInfo::StreamType::Ac3);
        info.streamTypes.push_back(StreamInfo::StreamType::DtsHd);
        info.streamTypes.push_back(StreamInfo::StreamType::DtsHdMaster);
        info.streamTypes.push_back(StreamInfo::StreamType::STREAM_TYPE_DTSHD_CORE);
        info.streamTypes.push_back(StreamInfo::StreamType::STREAM_TYPE_DTS_1024);
        info.streamTypes.push_back(StreamInfo::StreamType::STREAM_TYPE_DTS_2048);
        info.streamTypes.push_back(StreamInfo::StreamType::Dts512);
        info.streamTypes.push_back(StreamInfo::StreamType::Eac3);
        info.streamTypes.push_back(StreamInfo::StreamType::TrueHd);

        // indicate that we can do AudioSampleFormat::AE_FMT_RAW
        info.sampleFormat.push_back(AudioSampleFormat::Bitstream);
    }

    snd_pcm_close(pcmhandle);
    info.m_wantsIECPassthrough = true;
    list.push_back(info);
}

void AlsaPassthroughSink::sndLibErrorHandler(const char *file, int line, const char *function, int err, const char *fmt, ...)
{
    //std::cerr << "ALSA error: " << err;
}