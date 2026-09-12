/* Independent, headless PUAE diagnostic. It links no Benefactor product code:
 * libretro callbacks record presentation/audio timing from the emulator. */
#include "common/log.h"
#include "harness/puae_options.h"

#include <libretro.h>

#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint64_t kHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

void hash_bytes(std::uint64_t &hash, const std::uint8_t *bytes, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ bytes[index]) * kHashPrime;
    }
}

struct DlCloser final {
    void operator()(void *handle) const noexcept {
        if (handle != nullptr) {
            dlclose(handle);
        }
    }
};

class PuaeOracle final {
  public:
    PuaeOracle(const std::filesystem::path &core, std::filesystem::path game,
               std::filesystem::path activity, std::uint32_t fire_start, std::uint32_t fire_end)
        : library_(dlopen(core.c_str(), RTLD_NOW | RTLD_LOCAL)), game_(std::move(game)),
          activity_(std::move(activity)), fire_start_(fire_start), fire_end_(fire_end) {
        if (!library_) {
            throw std::runtime_error("cannot load PUAE core: " + std::string(dlerror()));
        }
        api_version_ = symbol<decltype(&retro_api_version)>("retro_api_version");
        set_environment_ = symbol<decltype(&retro_set_environment)>("retro_set_environment");
        set_video_ = symbol<decltype(&retro_set_video_refresh)>("retro_set_video_refresh");
        set_audio_ = symbol<decltype(&retro_set_audio_sample)>("retro_set_audio_sample");
        set_audio_batch_ =
            symbol<decltype(&retro_set_audio_sample_batch)>("retro_set_audio_sample_batch");
        set_input_poll_ = symbol<decltype(&retro_set_input_poll)>("retro_set_input_poll");
        set_input_state_ = symbol<decltype(&retro_set_input_state)>("retro_set_input_state");
        init_ = symbol<decltype(&retro_init)>("retro_init");
        deinit_ = symbol<decltype(&retro_deinit)>("retro_deinit");
        load_game_ = symbol<decltype(&retro_load_game)>("retro_load_game");
        unload_game_ = symbol<decltype(&retro_unload_game)>("retro_unload_game");
        run_ = symbol<decltype(&retro_run)>("retro_run");
        memory_data_ = symbol<decltype(&retro_get_memory_data)>("retro_get_memory_data");
        memory_size_ = symbol<decltype(&retro_get_memory_size)>("retro_get_memory_size");
    }

    PuaeOracle(const PuaeOracle &) = delete;
    PuaeOracle &operator=(const PuaeOracle &) = delete;

    ~PuaeOracle() {
        if (loaded_) {
            unload_game_();
        }
        if (initialized_) {
            deinit_();
        }
        active_ = nullptr;
    }

    void initialize() {
        if (active_ != nullptr) {
            throw std::logic_error("only one PUAE oracle may run at a time");
        }
        if (api_version_() != RETRO_API_VERSION) {
            throw std::runtime_error("PUAE core has an incompatible libretro API version");
        }
        std::filesystem::create_directories(activity_ / "save");
        std::filesystem::create_directories(activity_ / "system");
        system_dir_ = (activity_ / "system").string();
        save_dir_ = (activity_ / "save").string();
        game_path_ = std::filesystem::absolute(game_).string();
        output_.open(activity_ / "puae_frames.csv", std::ios::trunc);
        if (!output_) {
            throw std::runtime_error("cannot open PUAE frame report");
        }
        capture_rgb_.resize(std::size_t{2048} * 2048U * 3U);
        output_ << "frame,video_hash,video_calls,pcm_hash,pcm_frames,nonzero_pcm_frames,"
                   "chip_003e,chip_0184\n";
        active_ = this;
        set_environment_(&environment_callback);
        set_video_(&video_callback);
        set_audio_(&audio_callback);
        set_audio_batch_(&audio_batch_callback);
        set_input_poll_(&input_poll_callback);
        set_input_state_(&input_state_callback);
        init_();
        initialized_ = true;
        const retro_game_info info{game_path_.c_str(), nullptr, 0, nullptr};
        if (!load_game_(&info)) {
            throw std::runtime_error("PUAE refused the supplied WHDLoad game directory");
        }
        loaded_ = true;
    }

    void run_frames(std::uint32_t count) {
        capture_frame_ = count - 1U;
        for (frame_ = 0; frame_ < count; ++frame_) {
            video_hash_ = kHashOffset;
            pcm_hash_ = kHashOffset;
            video_calls_ = 0;
            pcm_frames_ = 0;
            nonzero_pcm_frames_ = 0;
            run_();
            if (invalid_callback_) {
                throw std::runtime_error("PUAE supplied an invalid video or audio buffer");
            }
            const auto *chip =
                static_cast<const std::uint8_t *>(memory_data_(RETRO_MEMORY_SYSTEM_RAM));
            const std::size_t chip_size = memory_size_(RETRO_MEMORY_SYSTEM_RAM);
            const std::uint32_t word_003e = chip != nullptr && chip_size > 0x3FU
                                                ? (std::uint32_t(chip[0x3E]) << 8U) | chip[0x3F]
                                                : 0U;
            const std::uint32_t word_0184 = chip != nullptr && chip_size > 0x185U
                                                ? (std::uint32_t(chip[0x184]) << 8U) | chip[0x185]
                                                : 0U;
            if (chip != nullptr) {
                ++chip_frames_;
            }
            if (video_calls_ != 0) {
                ++video_frames_;
                video_hashes_.insert(video_hash_);
            }
            if (pcm_frames_ != 0) {
                ++audio_frames_;
                if (nonzero_pcm_frames_ != 0) {
                    ++audible_frames_;
                }
            }
            output_ << frame_ << ',' << video_hash_ << ',' << video_calls_ << ',' << pcm_hash_
                    << ',' << pcm_frames_ << ',' << nonzero_pcm_frames_ << ',' << word_003e << ','
                    << word_0184 << '\n';
        }
        output_.flush();
        if (!output_) {
            throw std::runtime_error("cannot finish PUAE frame report");
        }
        if (capture_width_ != 0) {
            std::ofstream image(activity_ / "puae_last.ppm", std::ios::binary | std::ios::trunc);
            image << "P6\n" << capture_width_ << ' ' << capture_height_ << "\n255\n";
            image.write(
                reinterpret_cast<const char *>(capture_rgb_.data()),
                static_cast<std::streamsize>(std::size_t{capture_width_} * capture_height_ * 3U));
            if (!image) {
                throw std::runtime_error("cannot finish PUAE screenshot");
            }
        }
        benefactor_log_write(BENEFACTOR_LOG_INFO, "oracle",
                             "scanned %u frames: video=%u (%zu distinct hashes), audio=%u, "
                             "nonzero-audio=%u, chip-memory=%u; report=%s; screenshot=%s",
                             count, video_frames_, video_hashes_.size(), audio_frames_,
                             audible_frames_, chip_frames_, (activity_ / "puae_frames.csv").c_str(),
                             capture_width_ != 0 ? (activity_ / "puae_last.ppm").c_str() : "none");
        if (video_frames_ == 0 || chip_frames_ == 0) {
            throw std::runtime_error(
                "PUAE did not expose video and chip memory; no comparison is valid");
        }
    }

  private:
    template <typename Function> Function symbol(const char *name) const {
        dlerror();
        void *address = dlsym(library_.get(), name);
        if (const char *failure = dlerror(); failure != nullptr) {
            throw std::runtime_error(std::string("PUAE core lacks ") + name + ": " + failure);
        }
        return reinterpret_cast<Function>(address);
    }

    static bool environment_callback(unsigned command, void *data) {
        PuaeOracle &oracle = *active_;
        switch (command) {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback *>(data)->log = &log_callback;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *static_cast<const char **>(data) = oracle.system_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char **>(data) = oracle.save_dir_.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto *variable = static_cast<retro_variable *>(data);
            variable->value = puae_option_value(variable->key);
            return variable->value != nullptr;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            *static_cast<bool *>(data) = false;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            oracle.pixel_format_ = *static_cast<retro_pixel_format *>(data);
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned *>(data) = RETRO_LANGUAGE_ENGLISH;
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
            return true;
        default:
            return false;
        }
    }

    static void log_callback(retro_log_level level, const char *format, ...) {
        BenefactorLogLevel mapped = BENEFACTOR_LOG_INFO;
        if (level == RETRO_LOG_DEBUG) {
            mapped = BENEFACTOR_LOG_DEBUG;
        } else if (level == RETRO_LOG_WARN) {
            mapped = BENEFACTOR_LOG_WARNING;
        } else if (level == RETRO_LOG_ERROR) {
            mapped = BENEFACTOR_LOG_ERROR;
        }
        std::va_list arguments;
        va_start(arguments, format);
        benefactor_log_write_va(mapped, "puae", format, arguments);
        va_end(arguments);
    }

    static void video_callback(const void *data, unsigned width, unsigned height,
                               std::size_t pitch) {
        PuaeOracle &oracle = *active_;
        if (data == nullptr) {
            return;
        }
        const std::size_t bpp = oracle.pixel_format_ == RETRO_PIXEL_FORMAT_XRGB8888 ? 4U : 2U;
        if (width == 0 || width > 2048 || height == 0 || height > 2048 || pitch < width * bpp ||
            pitch > 8192) {
            oracle.invalid_callback_ = true;
            return;
        }
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        for (unsigned row = 0; row < height; ++row) {
            hash_bytes(oracle.video_hash_, bytes + row * pitch, width * bpp);
        }
        ++oracle.video_calls_;
        if (oracle.frame_ == oracle.capture_frame_) {
            oracle.capture_width_ = width;
            oracle.capture_height_ = height;
            for (unsigned y = 0; y < height; ++y) {
                const auto *source = bytes + y * pitch;
                auto *target = oracle.capture_rgb_.data() + std::size_t{y} * width * 3U;
                for (std::size_t x = 0; x < width; ++x) {
                    if (oracle.pixel_format_ == RETRO_PIXEL_FORMAT_XRGB8888) {
                        target[x * 3U] = source[x * 4U + 2U];
                        target[x * 3U + 1U] = source[x * 4U + 1U];
                        target[x * 3U + 2U] = source[x * 4U];
                    } else {
                        const std::uint16_t pixel = std::uint16_t(source[x * 2U]) |
                                                    std::uint16_t(source[x * 2U + 1U]) << 8U;
                        const unsigned red = oracle.pixel_format_ == RETRO_PIXEL_FORMAT_RGB565
                                                 ? (pixel >> 11U) & 31U
                                                 : (pixel >> 10U) & 31U;
                        const unsigned green = oracle.pixel_format_ == RETRO_PIXEL_FORMAT_RGB565
                                                   ? (pixel >> 5U) & 63U
                                                   : (pixel >> 5U) & 31U;
                        const unsigned blue = pixel & 31U;
                        target[x * 3U] = static_cast<std::uint8_t>(red * 255U / 31U);
                        target[x * 3U + 1U] = static_cast<std::uint8_t>(
                            green * 255U /
                            (oracle.pixel_format_ == RETRO_PIXEL_FORMAT_RGB565 ? 63U : 31U));
                        target[x * 3U + 2U] = static_cast<std::uint8_t>(blue * 255U / 31U);
                    }
                }
            }
        }
    }

    void record_audio(const std::int16_t *samples, std::size_t frames) {
        if (samples == nullptr) {
            invalid_callback_ = true;
            return;
        }
        hash_bytes(pcm_hash_, reinterpret_cast<const std::uint8_t *>(samples),
                   frames * 2U * sizeof(std::int16_t));
        pcm_frames_ += frames;
        for (std::size_t index = 0; index < frames; ++index) {
            if (samples[index * 2U] != 0 || samples[index * 2U + 1U] != 0) {
                ++nonzero_pcm_frames_;
            }
        }
    }

    static void audio_callback(std::int16_t left, std::int16_t right) {
        const std::int16_t frame[]{left, right};
        active_->record_audio(frame, 1U);
    }

    static std::size_t audio_batch_callback(const std::int16_t *samples, std::size_t frames) {
        active_->record_audio(samples, frames);
        return frames;
    }

    static void input_poll_callback() {}

    static std::int16_t input_state_callback(unsigned port, unsigned device, unsigned index,
                                             unsigned id) {
        const PuaeOracle &oracle = *active_;
        const bool fire = oracle.frame_ >= oracle.fire_start_ && oracle.frame_ < oracle.fire_end_;
        const bool joypad_fire = (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD &&
                                 (id == RETRO_DEVICE_ID_JOYPAD_B || id == RETRO_DEVICE_ID_JOYPAD_A);
        const bool mouse_fire =
            (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_MOUSE && id == RETRO_DEVICE_ID_MOUSE_LEFT;
        return fire && port == 0U && index == 0U && (joypad_fire || mouse_fire) ? 1 : 0;
    }

    static PuaeOracle *active_;
    std::unique_ptr<void, DlCloser> library_;
    std::filesystem::path game_;
    std::filesystem::path activity_;
    std::string game_path_;
    std::string system_dir_;
    std::string save_dir_;
    std::ofstream output_;
    std::uint32_t fire_start_;
    std::uint32_t fire_end_;
    std::uint32_t frame_{};
    retro_pixel_format pixel_format_{RETRO_PIXEL_FORMAT_RGB565};
    std::uint64_t video_hash_{kHashOffset};
    std::uint64_t pcm_hash_{kHashOffset};
    std::size_t video_calls_{};
    std::size_t pcm_frames_{};
    std::size_t nonzero_pcm_frames_{};
    std::uint32_t video_frames_{};
    std::uint32_t audio_frames_{};
    std::uint32_t audible_frames_{};
    std::uint32_t chip_frames_{};
    std::unordered_set<std::uint64_t> video_hashes_;
    std::vector<std::uint8_t> capture_rgb_;
    std::uint32_t capture_frame_{};
    unsigned capture_width_{};
    unsigned capture_height_{};
    bool invalid_callback_{};
    bool initialized_{};
    bool loaded_{};
    decltype(&retro_api_version) api_version_{};
    decltype(&retro_set_environment) set_environment_{};
    decltype(&retro_set_video_refresh) set_video_{};
    decltype(&retro_set_audio_sample) set_audio_{};
    decltype(&retro_set_audio_sample_batch) set_audio_batch_{};
    decltype(&retro_set_input_poll) set_input_poll_{};
    decltype(&retro_set_input_state) set_input_state_{};
    decltype(&retro_init) init_{};
    decltype(&retro_deinit) deinit_{};
    decltype(&retro_load_game) load_game_{};
    decltype(&retro_unload_game) unload_game_{};
    decltype(&retro_run) run_{};
    decltype(&retro_get_memory_data) memory_data_{};
    decltype(&retro_get_memory_size) memory_size_{};
};

PuaeOracle *PuaeOracle::active_ = nullptr;

std::uint32_t parse_frame(const char *text) {
    std::uint32_t value{};
    const char *end = text + std::char_traits<char>::length(text);
    const auto [next, error] = std::from_chars(text, end, value);
    if (error != std::errc{} || next != end) {
        throw std::invalid_argument("oracle frame arguments must be unsigned decimal integers");
    }
    return value;
}

} // namespace

int main(int argc, char **argv) {
    benefactor_log_set_category_level("puae", BENEFACTOR_LOG_WARNING);
    if (argc != 5 && argc != 7) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "oracle",
                             "usage: puae_oracle CORE GAME ACTIVITY FRAMES [FIRE_START FIRE_END]");
        return 2;
    }
    try {
        const auto frames = parse_frame(argv[4]);
        const auto fire_start = argc == 7 ? parse_frame(argv[5]) : 0U;
        const auto fire_end = argc == 7 ? parse_frame(argv[6]) : 0U;
        if (frames == 0 || frames > 20000 || fire_end > frames || fire_start > fire_end) {
            throw std::invalid_argument("invalid oracle frame or fire range");
        }
        PuaeOracle oracle(argv[1], argv[2], argv[3], fire_start, fire_end);
        oracle.initialize();
        oracle.run_frames(frames);
        return 0;
    } catch (const std::exception &error) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "oracle", "%s", error.what());
        return 1;
    }
}
