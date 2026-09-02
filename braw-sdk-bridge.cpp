// Minimal Blackmagic RAW SDK bridge for ffmpeg-proxygen.
//
// It writes either packed RGBA frames or native little-endian PCM samples to
// stdout; diagnostics are always written to stderr so stdout is pipe-safe.

#include <CoreFoundation/CoreFoundation.h>

#include "BlackmagicRawAPI.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kDefaultLibraries =
    "/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries";
constexpr BlackmagicRawResourceFormat kResourceFormat = blackmagicRawResourceFormatRGBAU8;

std::string cf_string(CFStringRef value) {
    if (value == nullptr) {
        return "";
    }
    const CFIndex length = CFStringGetLength(value);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string output(static_cast<size_t>(capacity), '\0');
    if (!CFStringGetCString(value, output.data(), capacity, kCFStringEncodingUTF8)) {
        return "";
    }
    output.resize(std::strlen(output.c_str()));
    return output;
}

CFStringRef cf_string_create(const std::string& value) {
    return CFStringCreateWithCString(nullptr, value.c_str(), kCFStringEncodingUTF8);
}

std::string json_escape(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                    output += escaped;
                } else {
                    output += static_cast<char>(ch);
                }
        }
    }
    return output;
}

void release_if(IUnknown* object) {
    if (object != nullptr) {
        object->Release();
    }
}

class ClipSession {
public:
    ClipSession(const std::string& libraries, const std::string& source) {
        CFStringRef libraries_path = cf_string_create(libraries);
        factory_ = CreateBlackmagicRawFactoryInstanceFromPath(libraries_path);
        CFRelease(libraries_path);
        if (factory_ == nullptr) {
            throw std::runtime_error("could not load BlackmagicRawAPI.framework from " + libraries);
        }
        check(factory_->CreateCodec(&codec_), "could not create Blackmagic RAW codec");
        CFStringRef source_path = cf_string_create(source);
        const HRESULT result = codec_->OpenClip(source_path, &clip_);
        CFRelease(source_path);
        check(result, "could not open BRAW source");
    }

    ~ClipSession() {
        release_if(clip_);
        release_if(codec_);
        release_if(factory_);
    }

    IBlackmagicRaw* codec() const { return codec_; }
    IBlackmagicRawClip* clip() const { return clip_; }

    static void check(HRESULT result, const std::string& message) {
        if (FAILED(result)) {
            char code[32];
            std::snprintf(code, sizeof(code), " (HRESULT 0x%08x)", static_cast<unsigned int>(result));
            throw std::runtime_error(message + code);
        }
    }

private:
    IBlackmagicRawFactory* factory_ = nullptr;
    IBlackmagicRaw* codec_ = nullptr;
    IBlackmagicRawClip* clip_ = nullptr;
};

std::optional<std::string> string_metadata(IBlackmagicRawClip* clip, const char* key) {
    Variant value;
    if (FAILED(VariantInit(&value))) {
        return std::nullopt;
    }
    CFStringRef metadata_key = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    const HRESULT result = clip->GetMetadata(metadata_key, &value);
    CFRelease(metadata_key);
    if (FAILED(result)) {
        VariantClear(&value);
        return std::nullopt;
    }

    std::optional<std::string> output;
    switch (value.vt) {
        case blackmagicRawVariantTypeString:
            output = cf_string(value.bstrVal);
            break;
        case blackmagicRawVariantTypeS16:
            output = std::to_string(value.iVal);
            break;
        case blackmagicRawVariantTypeU16:
            output = std::to_string(value.uiVal);
            break;
        case blackmagicRawVariantTypeS32:
            output = std::to_string(value.intVal);
            break;
        case blackmagicRawVariantTypeU32:
            output = std::to_string(value.uintVal);
            break;
        case blackmagicRawVariantTypeFloat32:
            output = std::to_string(value.fltVal);
            break;
        default:
            break;
    }
    VariantClear(&value);
    return output;
}

struct AudioInfo {
    uint32_t bit_depth = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    uint64_t samples = 0;
};

std::optional<AudioInfo> audio_info(IBlackmagicRawClip* clip) {
    IBlackmagicRawClipAudio* audio = nullptr;
    if (FAILED(clip->QueryInterface(IID_IBlackmagicRawClipAudio, reinterpret_cast<void**>(&audio)))) {
        return std::nullopt;
    }
    AudioInfo info;
    BlackmagicRawAudioFormat format;
    const bool success =
        SUCCEEDED(audio->GetAudioFormat(&format)) &&
        format == blackmagicRawAudioFormatPCMLittleEndian &&
        SUCCEEDED(audio->GetAudioBitDepth(&info.bit_depth)) &&
        SUCCEEDED(audio->GetAudioChannelCount(&info.channels)) &&
        SUCCEEDED(audio->GetAudioSampleRate(&info.sample_rate)) &&
        SUCCEEDED(audio->GetAudioSampleCount(&info.samples));
    audio->Release();
    if (!success) {
        return std::nullopt;
    }
    return info;
}

struct DecodeScale {
    BlackmagicRawResolutionScale value = blackmagicRawResolutionScaleFull;
    const char* name = "full";
    uint32_t width = 0;
    uint32_t height = 0;
};

DecodeScale choose_scale(IBlackmagicRawClip* clip, uint32_t target_height) {
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    ClipSession::check(clip->GetWidth(&source_width), "could not read BRAW width");
    ClipSession::check(clip->GetHeight(&source_height), "could not read BRAW height");

    std::vector<DecodeScale> scales = {
        {blackmagicRawResolutionScaleFull, "full", source_width, source_height},
        {blackmagicRawResolutionScaleHalf, "half", source_width, source_height},
        {blackmagicRawResolutionScaleQuarter, "quarter", source_width, source_height},
        {blackmagicRawResolutionScaleEighth, "eighth", source_width, source_height},
    };
    IBlackmagicRawClipResolutions* resolutions = nullptr;
    if (SUCCEEDED(clip->QueryInterface(IID_IBlackmagicRawClipResolutions,
                                       reinterpret_cast<void**>(&resolutions)))) {
        for (auto& scale : scales) {
            uint32_t width = 0;
            uint32_t height = 0;
            if (SUCCEEDED(resolutions->GetClosestResolutionForScale(scale.value, &width, &height)) &&
                width > 0 && height > 0) {
                scale.width = width;
                scale.height = height;
            }
        }
        resolutions->Release();
    } else {
        for (size_t index = 1; index < scales.size(); ++index) {
            const uint32_t divisor = 1U << index;
            scales[index].width = std::max(1U, source_width / divisor);
            scales[index].height = std::max(1U, source_height / divisor);
        }
    }

    if (target_height == 0) {
        return scales.front();
    }

    // Decode at the least expensive BRAW scale that still has the requested
    // vertical resolution. FFmpeg performs the final exact-aspect conversion.
    DecodeScale selected = scales.front();
    for (const auto& candidate : scales) {
        if (candidate.height >= target_height &&
            static_cast<uint64_t>(candidate.width) * candidate.height <=
                static_cast<uint64_t>(selected.width) * selected.height) {
            selected = candidate;
        }
    }
    return selected;
}

DecodeScale scale_from_name(IBlackmagicRawClip* clip, const std::string& name) {
    const DecodeScale selected = choose_scale(clip, 0);
    if (name == "auto") {
        return selected;
    }

    uint32_t source_width = 0;
    uint32_t source_height = 0;
    ClipSession::check(clip->GetWidth(&source_width), "could not read BRAW width");
    ClipSession::check(clip->GetHeight(&source_height), "could not read BRAW height");
    const std::map<std::string, BlackmagicRawResolutionScale> named_scales = {
        {"full", blackmagicRawResolutionScaleFull},
        {"half", blackmagicRawResolutionScaleHalf},
        {"quarter", blackmagicRawResolutionScaleQuarter},
        {"eighth", blackmagicRawResolutionScaleEighth},
    };
    const auto found = named_scales.find(name);
    if (found == named_scales.end()) {
        throw std::runtime_error("unknown BRAW scale: " + name);
    }

    const char* scale_name =
        found->second == blackmagicRawResolutionScaleFull ? "full" :
        found->second == blackmagicRawResolutionScaleHalf ? "half" :
        found->second == blackmagicRawResolutionScaleQuarter ? "quarter" : "eighth";
    DecodeScale output {found->second, scale_name, source_width, source_height};
    IBlackmagicRawClipResolutions* resolutions = nullptr;
    if (SUCCEEDED(clip->QueryInterface(IID_IBlackmagicRawClipResolutions,
                                       reinterpret_cast<void**>(&resolutions)))) {
        ClipSession::check(resolutions->GetClosestResolutionForScale(output.value, &output.width,
                                                                       &output.height),
                           "could not read BRAW decode resolution");
        resolutions->Release();
    } else if (output.value != blackmagicRawResolutionScaleFull) {
        const uint32_t divisor = output.value == blackmagicRawResolutionScaleHalf ? 2 :
                                 output.value == blackmagicRawResolutionScaleQuarter ? 4 : 8;
        output.width = std::max(1U, source_width / divisor);
        output.height = std::max(1U, source_height / divisor);
    }
    return output;
}

class DecodeCallback final : public IBlackmagicRawCallback {
public:
    void begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        complete_ = false;
        result_ = E_FAIL;
    }

    HRESULT wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return complete_; });
        return result_;
    }

    void ReadComplete(IBlackmagicRawJob* read_job, HRESULT result,
                      IBlackmagicRawFrame* frame) override {
        IBlackmagicRawJob* process_job = nullptr;
        if (SUCCEEDED(result)) {
            result = frame->SetResourceFormat(kResourceFormat);
        }
        if (SUCCEEDED(result)) {
            result = frame->SetResolutionScale(scale_);
        }
        if (SUCCEEDED(result)) {
            result = frame->CreateJobDecodeAndProcessFrame(nullptr, nullptr, &process_job);
        }
        if (SUCCEEDED(result)) {
            result = process_job->Submit();
        }
        if (FAILED(result)) {
            release_if(process_job);
            signal(result);
        }
        read_job->Release();
    }

    void ProcessComplete(IBlackmagicRawJob* process_job, HRESULT result,
                         IBlackmagicRawProcessedImage* image) override {
        void* data = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t size = 0;
        if (SUCCEEDED(result)) result = image->GetWidth(&width);
        if (SUCCEEDED(result)) result = image->GetHeight(&height);
        if (SUCCEEDED(result)) result = image->GetResourceSizeBytes(&size);
        if (SUCCEEDED(result)) result = image->GetResource(&data);
        const uint64_t expected_size = static_cast<uint64_t>(width) * height * 4U;
        if (SUCCEEDED(result) && (size < expected_size || width != width_ || height != height_)) {
            std::fprintf(stderr,
                         "braw-sdk-bridge: unexpected decoded frame layout: got %ux%u (%u bytes), "
                         "expected %ux%u\n",
                         width, height, size, width_, height_);
            result = E_FAIL;
        }
        // The SDK may allocate a buffer slightly larger than its packed image.
        // It always stores RGBAU8 pixels tightly at the beginning of the buffer.
        if (SUCCEEDED(result) && std::fwrite(data, 1, expected_size, stdout) != expected_size) {
            result = E_FAIL;
        }
        if (SUCCEEDED(result) && std::fflush(stdout) != 0) {
            result = E_FAIL;
        }
        process_job->Release();
        signal(result);
    }

    void DecodeComplete(IBlackmagicRawJob*, HRESULT) override {}
    void TrimProgress(IBlackmagicRawJob*, float) override {}
    void TrimComplete(IBlackmagicRawJob*, HRESULT) override {}
    void SidecarMetadataParseWarning(IBlackmagicRawClip*, CFStringRef, uint32_t, CFStringRef) override {}
    void SidecarMetadataParseError(IBlackmagicRawClip*, CFStringRef, uint32_t, CFStringRef) override {}
    void PreparePipelineComplete(void*, HRESULT) override {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOTIMPL; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    BlackmagicRawResolutionScale scale_ = blackmagicRawResolutionScaleFull;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

private:
    void signal(HRESULT result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result;
        complete_ = true;
        condition_.notify_one();
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    bool complete_ = false;
    HRESULT result_ = E_FAIL;
};

void write_probe(const std::string& libraries, const std::string& source, uint32_t target_height) {
    ClipSession session(libraries, source);
    IBlackmagicRawClip* clip = session.clip();
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame_count = 0;
    float frame_rate = 0;
    ClipSession::check(clip->GetWidth(&width), "could not read BRAW width");
    ClipSession::check(clip->GetHeight(&height), "could not read BRAW height");
    ClipSession::check(clip->GetFrameCount(&frame_count), "could not read BRAW frame count");
    ClipSession::check(clip->GetFrameRate(&frame_rate), "could not read BRAW frame rate");

    CFStringRef timecode_ref = nullptr;
    std::string timecode;
    if (frame_count > 0 && SUCCEEDED(clip->GetTimecodeForFrame(0, &timecode_ref))) {
        timecode = cf_string(timecode_ref);
        CFRelease(timecode_ref);
    }
    CFStringRef camera_type_ref = nullptr;
    std::string camera_type;
    if (SUCCEEDED(clip->GetCameraType(&camera_type_ref))) {
        camera_type = cf_string(camera_type_ref);
        CFRelease(camera_type_ref);
    }
    const DecodeScale scale = choose_scale(clip, target_height);
    const auto audio = audio_info(clip);

    const std::vector<const char*> metadata_keys = {
        "manufacturer", "camera_id", "camera_type", "firmware_version", "lens_type",
        "production_name", "clip_number", "reel_name", "scene", "take", "camera_number",
        "camera_operator", "date_recorded", "viewing_gamma", "viewing_gamut",
    };
    std::cout << "{\"width\":" << width
              << ",\"height\":" << height
              << ",\"decode_width\":" << scale.width
              << ",\"decode_height\":" << scale.height
              << ",\"decode_scale\":\"" << scale.name << "\""
              << ",\"frame_count\":" << frame_count
              << ",\"frame_rate\":" << std::setprecision(12) << frame_rate
              << ",\"timecode\":\"" << json_escape(timecode) << "\""
              << ",\"camera_type\":\"" << json_escape(camera_type) << "\""
              << ",\"audio\":";
    if (audio) {
        std::cout << "{\"bit_depth\":" << audio->bit_depth
                  << ",\"channels\":" << audio->channels
                  << ",\"sample_rate\":" << audio->sample_rate
                  << ",\"samples\":" << audio->samples << "}";
    } else {
        std::cout << "null";
    }
    std::cout << ",\"metadata\":{";
    bool first = true;
    for (const char* key : metadata_keys) {
        const auto value = string_metadata(clip, key);
        if (!value || value->empty()) {
            continue;
        }
        if (!first) {
            std::cout << ',';
        }
        first = false;
        std::cout << '"' << json_escape(key) << "\":\"" << json_escape(*value) << '"';
    }
    std::cout << "}}\n";
}

void write_video(const std::string& libraries, const std::string& source,
                 const std::string& requested_scale, uint64_t max_frames) {
    ClipSession session(libraries, source);
    IBlackmagicRawClip* clip = session.clip();
    const DecodeScale scale = scale_from_name(clip, requested_scale);
    uint64_t frame_count = 0;
    ClipSession::check(clip->GetFrameCount(&frame_count), "could not read BRAW frame count");
    frame_count = std::min(frame_count, max_frames);

    DecodeCallback callback;
    callback.scale_ = scale.value;
    callback.width_ = scale.width;
    callback.height_ = scale.height;
    ClipSession::check(session.codec()->SetCallback(&callback), "could not configure BRAW callback");

    for (uint64_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        callback.begin();
        IBlackmagicRawJob* read_job = nullptr;
        ClipSession::check(clip->CreateJobReadFrame(frame_index, &read_job),
                           "could not create BRAW frame-read job");
        const HRESULT submit_result = read_job->Submit();
        if (FAILED(submit_result)) {
            read_job->Release();
            ClipSession::check(submit_result, "could not submit BRAW frame-read job");
        }
        ClipSession::check(callback.wait(), "could not decode BRAW frame " + std::to_string(frame_index));
    }
    session.codec()->FlushJobs();
}

void write_audio(const std::string& libraries, const std::string& source) {
    ClipSession session(libraries, source);
    IBlackmagicRawClipAudio* audio = nullptr;
    ClipSession::check(session.clip()->QueryInterface(IID_IBlackmagicRawClipAudio,
                                                       reinterpret_cast<void**>(&audio)),
                       "BRAW source has no readable audio stream");
    AudioInfo info;
    BlackmagicRawAudioFormat format;
    HRESULT result = audio->GetAudioFormat(&format);
    if (SUCCEEDED(result) && format != blackmagicRawAudioFormatPCMLittleEndian) result = E_FAIL;
    if (SUCCEEDED(result)) result = audio->GetAudioBitDepth(&info.bit_depth);
    if (SUCCEEDED(result)) result = audio->GetAudioChannelCount(&info.channels);
    if (SUCCEEDED(result)) result = audio->GetAudioSampleRate(&info.sample_rate);
    if (SUCCEEDED(result)) result = audio->GetAudioSampleCount(&info.samples);
    if (FAILED(result) || info.bit_depth == 0 || info.bit_depth % 8 != 0) {
        audio->Release();
        ClipSession::check(FAILED(result) ? result : E_FAIL, "could not read BRAW PCM audio format");
    }

    const uint32_t bytes_per_sample_frame = info.channels * (info.bit_depth / 8);
    constexpr uint32_t kSamplesPerChunk = 48'000;
    std::vector<uint8_t> buffer(static_cast<size_t>(kSamplesPerChunk) * bytes_per_sample_frame);
    uint64_t position = 0;
    while (position < info.samples) {
        uint32_t samples_read = 0;
        uint32_t bytes_read = 0;
        const uint32_t requested = static_cast<uint32_t>(std::min<uint64_t>(
            kSamplesPerChunk, info.samples - position));
        result = audio->GetAudioSamples(static_cast<int64_t>(position), buffer.data(),
                                        static_cast<uint32_t>(buffer.size()), requested,
                                        &samples_read, &bytes_read);
        if (FAILED(result) || samples_read == 0 ||
            std::fwrite(buffer.data(), 1, bytes_read, stdout) != bytes_read) {
            audio->Release();
            ClipSession::check(FAILED(result) ? result : E_FAIL, "could not read BRAW PCM audio");
        }
        position += samples_read;
    }
    if (std::fflush(stdout) != 0) {
        audio->Release();
        throw std::runtime_error("could not write BRAW PCM audio to stdout");
    }
    audio->Release();
}

void usage() {
    std::cerr << "Usage:\n"
              << "  braw-sdk-bridge [--sdk-libraries DIR] probe INPUT.braw [--target-height N]\n"
              << "  braw-sdk-bridge [--sdk-libraries DIR] video INPUT.braw SCALE [--frames N]\n"
              << "  braw-sdk-bridge [--sdk-libraries DIR] audio INPUT.braw\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        std::string libraries = kDefaultLibraries;
        int argument = 1;
        if (argument + 1 < argc && std::string(argv[argument]) == "--sdk-libraries") {
            libraries = argv[argument + 1];
            argument += 2;
        }
        if (argc - argument < 2) {
            usage();
            return 2;
        }
        const std::string mode = argv[argument++];
        const std::string source = argv[argument++];
        if (mode == "probe") {
            uint32_t target_height = 0;
            if (argument + 1 < argc && std::string(argv[argument]) == "--target-height") {
                const unsigned long parsed_height = std::stoul(argv[argument + 1]);
                if (parsed_height == 0 || parsed_height > std::numeric_limits<uint32_t>::max()) {
                    throw std::runtime_error("invalid BRAW target height");
                }
                target_height = static_cast<uint32_t>(parsed_height);
                argument += 2;
            }
            if (argument == argc) {
                write_probe(libraries, source, target_height);
                return 0;
            }
        }
        if (mode == "video" && argument < argc) {
            const std::string scale = argv[argument++];
            uint64_t frame_limit = std::numeric_limits<uint64_t>::max();
            if (argument + 1 < argc && std::string(argv[argument]) == "--frames") {
                frame_limit = std::stoull(argv[argument + 1]);
                argument += 2;
            }
            if (argument == argc && frame_limit > 0) {
                write_video(libraries, source, scale, frame_limit);
                return 0;
            }
        }
        if (mode == "audio" && argument == argc) {
            write_audio(libraries, source);
            return 0;
        }
        usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "braw-sdk-bridge: " << error.what() << '\n';
        return 1;
    }
}
