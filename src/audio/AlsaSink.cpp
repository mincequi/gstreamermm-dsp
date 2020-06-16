/*
 * Copyright (C) 2020 Manuel Weichselbaumer <mincequi@web.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "audio/AlsaSink.h"

#include "audio/SpdifTypes.h"

#include <cstdint>
#include <cstring>
#include <alsa/asoundlib.h>
#include <loguru/loguru.hpp>

namespace coro {
namespace audio {

AlsaSink::AlsaSink()
{
}

AlsaSink::~AlsaSink()
{
}

void AlsaSink::start(const AudioConf& conf)
{
    //open(conf);
    openSimple(conf);
    //if (!setDelay(64)) {
    //    LOG_F(WARNING, "Delay not accepted: %d ms", 16);
    //}


    // Get current sw params
    snd_pcm_sw_params_t* params;
    snd_pcm_sw_params_alloca(&params);
    int err = snd_pcm_sw_params_current(m_pcm, params);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params_current() failed.\n");
        return;
    }

    snd_pcm_uframes_t frames = 0;
    snd_pcm_sw_params_get_start_threshold(params, &frames);
    snd_pcm_uframes_t min = 0;
    snd_pcm_sw_params_get_avail_min(params, &min);
    LOG_F(INFO, "Device opened. start threshold: %zu, min avail: %zu", frames, min);

    snd_pcm_prepare(m_pcm);
}

void AlsaSink::setDevice(const std::string& device)
{
    if (device == m_device) {
        return;
    }

    m_device = device;
    onStop();
    start(m_conf);
}

AudioConf AlsaSink::onProcess(const AudioConf& conf, AudioBuffer& buffer)
{
    if (m_conf != conf) {
        onStop();
        start(conf);
        m_conf = conf;
    }

    if (!m_pcm) {
        start(conf);
    }

    if (conf.codec == AudioCodec::Ac3) {
        doAc3Payload(buffer);
    }

    /*
    if (!write(buffer.data(), buffer.size())) {
        stop();
        start(conf);
    }
    */

    //
    //snd_pcm_sframes_t delay = 0;
    //snd_pcm_delay(m_pcm, &delay);
    //LOG_F(1, "Device delay: %zu ms", delay * 1000 / toInt(m_conf.rate));

    writeSimple(buffer.data(), buffer.size());

    buffer.clear();

    return conf;
}

void AlsaSink::onStop()
{
    if (m_pcm) {
        snd_pcm_drain(m_pcm);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
    LOG_F(INFO, "Device flushed");
}

bool AlsaSink::open(const AudioConf& conf)
{
    if (m_pcm) {
        LOG_F(INFO, "Device already opened");
        return false;
    }

    int err = snd_pcm_open(&m_pcm, m_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err) {
        LOG_F(INFO, "Unable to open ALSA device '%s'\n", m_device.c_str());
        return false;
    }

    /* try to set up hw params */
    if (!setHwParams(conf)) {
        LOG_F(INFO, "Unable to set HW params for device '%s'\n", m_device.c_str());
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    /* try to set up sw params */
    if (!setSwParams()) {
        LOG_F(INFO, "Unable to set SW params for device '%s'\n", m_device.c_str());
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    return true;
}

bool AlsaSink::openSimple(const AudioConf& conf)
{
    if (m_pcm) {
        LOG_F(WARNING, "Device already opened");
        return false;
    }

    int err = snd_pcm_open(&m_pcm, m_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err) {
        LOG_F(ERROR, "Unable to open ALSA device '%s'\n", m_device.c_str());
        return false;
    }

    unsigned int rate = toInt(conf.rate);
    err = snd_pcm_set_params(m_pcm,
                             SND_PCM_FORMAT_S16,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             2,
                             rate,
                             0,
                             40000);
    if (err) {
        LOG_F(WARNING, "snd_pcm_set_params() failed.");
        return false;
    }

    return true;
}

bool AlsaSink::setHwParams(const AudioConf& conf)
{
    snd_pcm_hw_params_t   *params;
    snd_pcm_hw_params_alloca(&params);

    /* fetch all possible hardware parameters */
    int err = snd_pcm_hw_params_any(m_pcm, params);
    if (err < 0) {
        LOG_F(INFO, "snd_pcm_hw_params_any() failed.\n");
        return false;
    }

    /* set the access type */
    err = snd_pcm_hw_params_set_access(m_pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOG_F(INFO, "snd_pcm_hw_params_set_access() failed.\n");
        return false;
    }

    /* set the sample bitformat */
    err = snd_pcm_hw_params_set_format(m_pcm, params, SND_PCM_FORMAT_S16);
    if (err < 0) {
        LOG_F(INFO, "snd_pcm_hw_params_set_format() failed.\n");
        return false;
    }

    /* set the number of channels */
    err = snd_pcm_hw_params_set_channels(m_pcm, params, 2);
    if (err < 0) {
        LOG_F(INFO, "snd_pcm_hw_params_set_channels() failed.\n");
        return false;
    }

    /* set the sample rate */
    unsigned int rate = toInt(conf.rate);
    err = snd_pcm_hw_params_set_rate_near(m_pcm, params, &rate, 0);
    if (err < 0) {
        LOG_F(INFO, "snd_pcm_hw_params_set_rate_near() failed.\n");
        return false;
    }
    if (rate > 1.05 * toInt(conf.rate) || rate < 0.95 * toInt(conf.rate)) {
        LOG_F(INFO, "sample rate %i not supported by the hardware, using %u\n", toInt(conf.rate), rate);
    }

    /*
    // set the time per hardware sample transfer
    if (internal->period_time == 0)
        internal->period_time = internal->buffer_time/4;

    err = snd_pcm_hw_params_set_period_time_near(m_pcm, params, &(internal->period_time), 0);
    if (err < 0){
        LOG_F(INFO, "snd_pcm_hw_params_set_period_time_near() failed.\n");
        return err;
    }

    // set the length of the hardware sample buffer in microseconds
    // some plug devices have very high minimum periods; don't allow a buffer
    // size small enough that it's ~ guaranteed to skip
    if(internal->buffer_time<internal->period_time*3)
        internal->buffer_time=internal->period_time*3;
    err = snd_pcm_hw_params_set_buffer_time_near(m_pcm, params, &(internal->buffer_time), 0);
    if (err < 0) {
        LOG_F(INFO, "snd_pcm_hw_params_set_buffer_time_near() failed.\n");
        return err;
    }
    */

    /* Set buffer size and period size manually for SPDIF */
    //if (self->passthrough) {
    snd_pcm_uframes_t buffer_size = spdif::ac3BufferSize;
    snd_pcm_uframes_t period_size = spdif::ac3PeriodSize;

    err = snd_pcm_hw_params_set_buffer_size_near(m_pcm, params, &buffer_size);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_hw_params_set_buffer_size_near() failed.\n");
        return false;
    }
    err = snd_pcm_hw_params_set_period_size_near(m_pcm, params, &period_size, NULL);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_hw_params_set_period_size_near() failed.\n");
        return false;
    }
    //}

    /* commit the params structure to the hardware via ALSA */
    err = snd_pcm_hw_params(m_pcm, params);
    if (err < 0){
        LOG_F(ERROR, "snd_pcm_hw_params() failed.\n");
        return false;
    }

    return true;
}

bool AlsaSink::setSwParams()
{
    snd_pcm_sw_params_t *params;
    snd_pcm_sw_params_alloca(&params);

    /* fetch the current software parameters */
    int err = snd_pcm_sw_params_current(m_pcm, params);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params_current() failed.\n");
        return false;
    }

    err = snd_pcm_sw_params_set_start_threshold(m_pcm, params, spdif::ac3BufferSize);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params_set_start_threshold() failed.\n");
        return false;
    }

    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(m_pcm, params, spdif::ac3PeriodSize);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params_set_avail_min() failed.\n");
        return false;
    }

    /* commit the params structure to ALSA */
    err = snd_pcm_sw_params(m_pcm, params);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params() failed.\n");
        return false;
    }

    return true;
}

bool AlsaSink::setDelay(uint16_t ms)
{
    snd_pcm_sw_params_t* params;
    snd_pcm_sw_params_alloca(&params);

    // get current sw params
    int err = snd_pcm_sw_params_current(m_pcm, params);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params_current() failed.\n");
        return false;
    }

    // set start threshold
    err = snd_pcm_sw_params_set_start_threshold(m_pcm, params, 44100 * ms / 1000);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params_set_start_threshold() failed.\n");
        return false;
    }

    // commit the params to ALSA */
    err = snd_pcm_sw_params(m_pcm, params);
    if (err < 0) {
        LOG_F(ERROR, "snd_pcm_sw_params() failed.\n");
        return false;
    }

    return true;
}

void AlsaSink::writeSimple(const char* samples, uint32_t bytesCount)
{
    snd_pcm_sframes_t frameCount = bytesCount / 4;
    char* ptr = (char*)samples;

    while (frameCount > 0) {
        auto ret = snd_pcm_writei(m_pcm, ptr, frameCount);
        // If all ok
        if (ret == 0) {
            return;
        }
        // If failed, try to recover
        if (ret < 0) {
            LOG_F(WARNING, "Write failed: %s", snd_strerror(ret));
            ret = snd_pcm_recover(m_pcm, ret, 0);
        }
        // If recover failed
        if (ret < 0) {
            LOG_F(WARNING, "Recovery failed: %s", snd_strerror(ret));
            break;
        }
        // If less frames written
        if (ret > 0 && ret < frameCount) {
            LOG_F(WARNING, "Frames written: %zd. expected: %zd.", ret, frameCount);
        }

        frameCount -= ret;
        ptr += ret * 4;
    }
}

bool AlsaSink::recover(int err)
{
    // underrun
    if (err == -EPIPE) {
        LOG_F(WARNING, "AlsaSink underrun");
        err = snd_pcm_prepare(m_pcm);
        if (err < 0) {
             LOG_F(ERROR, "AlsaSink cannot be recovered from underrun");
            // Cannot recover from underrun
            return false;
        }
        return true;
    } else if (err == -ESTRPIPE) {
        LOG_F(WARNING, "AlsaSink suspended");
        // wait until suspend flag clears
        while ((err = snd_pcm_resume(m_pcm)) == -EAGAIN)
            sleep(1);

        if (err < 0) {
            err = snd_pcm_prepare(m_pcm);
            if (err < 0) {
                LOG_F(ERROR, "AlsaSink cannot be recovered from suspend");
                // Cannot recover from suspend
                return false;
            }
        }
        return true;
    }

    LOG_F(WARNING, "AlsaSink unrecoverable");
    // unrecoverable
    return false;
}

void AlsaSink::doAc3Payload(AudioBuffer& buffer)
{
    if (buffer.size() > (spdif::ac3FrameSize - spdif::SpdifAc3Header::size())) {
        LOG_F(WARNING, "Frame too big, droppping it.");
        buffer.clear();
        return;
    }

    spdif::SpdifAc3Header ac3Header(buffer.data(), buffer.size());
    buffer.prepend((char*)&ac3Header, 8);

#if __BYTE_ORDER == __LITTLE_ENDIAN
    auto ac3Data = buffer.data() + ac3Header.size();
    const auto size = buffer.size() - ac3Header.size();

    for (uint32_t i = 0; i < size; i += 2) {
        *(uint16_t*)(ac3Data+i) = __bswap_16(*(uint16_t*)(ac3Data+i));
    }
#endif

    buffer.grow(spdif::ac3FrameSize);
}

} // namespace audio
} // namespace coro
