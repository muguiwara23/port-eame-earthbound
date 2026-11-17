#include "eb/audio_output.hpp"
#include "eb/frame_pacer.hpp"
#include "eb/snes_audio_dsp.hpp"
#include <SDL3/SDL.h>
#include <cmath>
#include <stdexcept>

namespace eb {
WaveFileWriter::WaveFileWriter(const std::string &path)
    : path_(path), output_(path, std::ios::binary | std::ios::trunc) {
    if (!output_)
        throw std::runtime_error("Cannot create WAV file: " + path);
    header();
}

WaveFileWriter::~WaveFileWriter() {
    if (!finished_) {
        try {
            finish();
        } catch (...) {
        }
    }
}

void WaveFileWriter::append(std::span<const std::int16_t> samples) {
    if (samples.size() > (0xffffffffu - 36 - bytes_) / 2)
        throw std::runtime_error("WAV recording exceeds the RIFF size limit");
    for (auto sample : samples)
        little16(static_cast<std::uint16_t>(sample));
    bytes_ += static_cast<std::uint32_t>(samples.size() * 2);
    if (!output_)
        throw std::runtime_error("Cannot write WAV audio: " + path_);
}

void WaveFileWriter::finish() {
    if (finished_)
        return;
    // RIFF lengths are unknown until recording ends; rewrite the placeholder
    // header after all interleaved stereo samples have been appended.
    output_.seekp(0);
    header();
    output_.close();
    finished_ = true;
    if (!output_)
        throw std::runtime_error("Cannot finalize WAV file: " + path_);
}

void WaveFileWriter::little16(std::uint16_t value) {
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
    output_.write(bytes, sizeof bytes);
}

void WaveFileWriter::little32(std::uint32_t value) {
    little16(static_cast<std::uint16_t>(value));
    little16(value >> 16);
}

void WaveFileWriter::header() {
    output_.write("RIFF", 4);
    little32(bytes_ + 36);
    output_.write("WAVEfmt ", 8);
    little32(16);
    little16(1);
    little16(2);
    little32(eb::SnesAudioDsp::output_sample_rate);
    little32(eb::SnesAudioDsp::output_sample_rate * 4);
    little16(4);
    little16(16);
    output_.write("data", 4);
    little32(bytes_);
}

// Port/eame: SDL3 plays through an audio stream. Its callback asks for bytes
// and is given them from the same queue SDL2's device callback consumed;
// the stream converts this source rate to the device's, as SDL2 did.
DeviceAudioQueue::DeviceAudioQueue(double frame_rate) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        throw std::runtime_error(std::string("SDL audio initialization: ") + SDL_GetError());
    SDL_AudioSpec desired{};
    // SDL converts this source rate to the physical device rate. The DSP
    // and WAV remain native; only playback follows the selected host cadence.
    desired.freq =
        int(std::lround(eb::SnesAudioDsp::output_sample_rate * frame_rate / eb::FramePacer::frame_rate));
    desired.format = SDL_AUDIO_S16;
    desired.channels = 2;
    // SDL2's 1024-sample device buffer.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    // Keep the DSP's sample format; SDL resamples to the hardware device.
    // Playback remains a consumer and never controls the simulation.
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired, consume, this);
    if (!stream_) {
        const std::string error = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw std::runtime_error("SDL audio device: " + error + " (use --no-audio to disable playback)");
    }
    SDL_ResumeAudioStreamDevice(stream_);
}

DeviceAudioQueue::~DeviceAudioQueue() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void DeviceAudioQueue::append(std::span<const std::int16_t> samples) {
    if (samples.empty())
        return;
    SDL_LockAudioStream(stream_);
    try {
        buffer_.append(samples);
    } catch (...) {
        SDL_UnlockAudioStream(stream_);
        throw;
    }
    SDL_UnlockAudioStream(stream_);
}

void DeviceAudioQueue::clear() {
    SDL_LockAudioStream(stream_);
    buffer_.clear();
    SDL_UnlockAudioStream(stream_);
}

// Runs on SDL's audio thread with the stream locked, so the queue is not
// changed under it by append() or clear().
void DeviceAudioQueue::consume(void* context, SDL_AudioStream* stream, int additional, int) {
    auto& self = *static_cast<DeviceAudioQueue*>(context);
    if (additional <= 0)
        return;
    self.scratch_.resize(std::size_t(additional) / sizeof(std::int16_t));
    self.buffer_.consume({self.scratch_.data(), self.scratch_.size()});
    SDL_PutAudioStreamData(stream, self.scratch_.data(), int(self.scratch_.size() * sizeof(std::int16_t)));
}

} // namespace eb
