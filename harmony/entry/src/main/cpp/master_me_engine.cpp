#include <napi/native_api.h>
#include <ohaudio/native_audiocapturer.h>
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dpf/distrho/src/DistrhoPluginInternal.hpp"

// Compile the original processor in this translation unit. The wrapper below
// promotes only the host-facing operations needed by the HarmonyOS bridge.
#include "../../../../../plugin/MasterMePlugin.cpp"

namespace {

using namespace DISTRHO;

class HarmonyMasterMe final : public MasterMePlugin
{
public:
    using MasterMePlugin::activate;
    using MasterMePlugin::deactivate;
    using MasterMePlugin::getParameterValue;
    using MasterMePlugin::loadProgram;
    using MasterMePlugin::run;
    using MasterMePlugin::setParameterValue;
};

class AudioRingBuffer
{
public:
    explicit AudioRingBuffer(const size_t capacity)
        : data(capacity, 0.0f)
    {
    }

    void Clear()
    {
        std::lock_guard<std::mutex> lock(mutex);
        readIndex = 0;
        writeIndex = 0;
        available = 0;
    }

    size_t Write(const std::vector<float>& samples)
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t dropped = 0;
        for (const float sample : samples) {
            if (available == data.size()) {
                readIndex = (readIndex + 1) % data.size();
                --available;
                ++dropped;
            }
            data[writeIndex] = sample;
            writeIndex = (writeIndex + 1) % data.size();
            ++available;
        }
        return dropped;
    }

    size_t Read(float* destination, const size_t requested)
    {
        size_t copied = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            copied = std::min(requested, available);
            for (size_t index = 0; index < copied; ++index) {
                destination[index] = data[readIndex];
                readIndex = (readIndex + 1) % data.size();
            }
            available -= copied;
        }
        return copied;
    }

private:
    std::mutex mutex;
    std::vector<float> data;
    size_t readIndex = 0;
    size_t writeIndex = 0;
    size_t available = 0;
};

struct Engine;
double PeakDb(const float* samples, const uint32_t frames);

void OnCaptureData(OH_AudioCapturer* capturer, void* userData, void* audioData, int32_t audioDataSize);
OH_AudioData_Callback_Result OnRenderData(OH_AudioRenderer* renderer, void* userData, void* audioData,
    int32_t audioDataSize);
void OnCaptureDeviceChange(OH_AudioCapturer* capturer, void* userData,
    OH_AudioDeviceDescriptorArray* deviceArray);
void OnRenderDeviceChange(OH_AudioRenderer* renderer, void* userData,
    OH_AudioStream_DeviceChangeReason reason);
void OnCaptureInterrupt(OH_AudioCapturer* capturer, void* userData,
    OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint);
void OnRenderInterrupt(OH_AudioRenderer* renderer, void* userData,
    OH_AudioInterrupt_ForceType type, OH_AudioInterrupt_Hint hint);
void OnCaptureError(OH_AudioCapturer* capturer, void* userData, OH_AudioStream_Result error);
void OnRenderError(OH_AudioRenderer* renderer, void* userData, OH_AudioStream_Result error);

struct Engine {
    static constexpr int32_t kSampleRate = 48000;
    static constexpr int32_t kChannelCount = 2;

    explicit Engine(const double sampleRate, const uint32_t bufferSize)
        : inputLeft(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          inputRight(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          outputLeft(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          outputRight(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          maxFrames(std::max<uint32_t>(bufferSize, 4096)),
          renderRing(16384)
    {
        DISTRHO::d_nextBufferSize = std::max<uint32_t>(bufferSize, 1);
        DISTRHO::d_nextSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        DISTRHO::d_nextPluginIsDummy = false;
        DISTRHO::d_nextPluginIsSelfTest = false;
        plugin = std::make_unique<HarmonyMasterMe>();
        plugin->activate();
    }

    ~Engine()
    {
        StopAudio();
        if (plugin != nullptr) {
            std::lock_guard<std::mutex> lock(processorMutex);
            plugin->deactivate();
        }
    }

    void resize(const uint32_t frames)
    {
        if (frames <= maxFrames) {
            return;
        }

        maxFrames = frames;
        inputLeft.resize(maxFrames);
        inputRight.resize(maxFrames);
        outputLeft.resize(maxFrames);
        outputRight.resize(maxFrames);
    }

    void ProcessLocked(const float* interleavedInput, float* interleavedOutput, const uint32_t frames)
    {
        if (frames == 0) {
            return;
        }

        resize(frames);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            inputLeft[frame] = interleavedInput[frame * 2];
            inputRight[frame] = interleavedInput[frame * 2 + 1];
            outputLeft[frame] = 0.0f;
            outputRight[frame] = 0.0f;
        }

        const float* inputs[2] = {inputLeft.data(), inputRight.data()};
        float* outputs[2] = {outputLeft.data(), outputRight.data()};
        plugin->run(inputs, outputs, frames);

        for (uint32_t frame = 0; frame < frames; ++frame) {
            interleavedOutput[frame * 2] = outputLeft[frame];
            interleavedOutput[frame * 2 + 1] = outputRight[frame];
        }

        inputPeak.store(std::max(PeakDb(inputLeft.data(), frames), PeakDb(inputRight.data(), frames)));
        outputPeak.store(std::max(PeakDb(outputLeft.data(), frames), PeakDb(outputRight.data(), frames)));
        inputLufs.store(plugin->getParameterValue(kParameter_lufs_in));
        outputLufs.store(plugin->getParameterValue(kParameter_lufs_out));
    }

    void ResetAudioStatistics()
    {
        callbackFrames.store(0);
        captureCallbacks.store(0);
        renderCallbacks.store(0);
        underruns.store(0);
        overflows.store(0);
        deviceChangeCount.store(0);
        recoveryCount.store(0);
        audioErrorCount.store(0);
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            lastError.clear();
        }
    }

    void SetError(const OH_AudioStream_Result error, const char* const stage)
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        lastError = std::string(stage) + " error " + std::to_string(static_cast<int32_t>(error));
        audioErrorCount.fetch_add(1);
        recoveryPending.store(true);
    }

    bool StartAudio()
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (audioRequested.load() && audioRunning.load()) {
            return true;
        }

        StopStreamsLocked();
        ResetAudioStatistics();
        recoveryPending.store(false);
        renderRing.Clear();
        audioRequested.store(true);
        if (!StartStreamsLocked()) {
            audioRequested.store(false);
            audioRunning.store(false);
            return false;
        }
        audioRunning.store(true);
        return true;
    }

    void StopAudio()
    {
        audioRequested.store(false);
        std::lock_guard<std::mutex> lock(audioMutex);
        StopStreamsLocked();
        audioRunning.store(false);
        recoveryPending.store(false);
        renderRing.Clear();
    }

    void RefreshAudioStatus()
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        if (!audioRequested.load() || captureStream == nullptr || renderStream == nullptr) {
            audioRunning.store(false);
            return;
        }

        OH_AudioStream_State captureState = AUDIOSTREAM_STATE_INVALID;
        OH_AudioStream_State renderState = AUDIOSTREAM_STATE_INVALID;
        OH_AudioCapturer_GetCurrentState(captureStream, &captureState);
        OH_AudioRenderer_GetCurrentState(renderStream, &renderState);
        const bool streamsRunning = captureState == AUDIOSTREAM_STATE_RUNNING &&
                                    renderState == AUDIOSTREAM_STATE_RUNNING;
        audioRunning.store(streamsRunning);
        if (streamsRunning && !recoveryPending.load()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastRecoveryAttempt < std::chrono::seconds(2)) {
            return;
        }

        lastRecoveryAttempt = now;
        recoveryCount.fetch_add(1);
        StopStreamsLocked();
        renderRing.Clear();
        if (!StartStreamsLocked()) {
            audioRequested.store(false);
            audioRunning.store(false);
        } else {
            recoveryPending.store(false);
            audioRunning.store(true);
        }
    }

    void HandleCapture(const int16_t* input, const size_t frames)
    {
        if (!audioRequested.load() || frames == 0) {
            return;
        }

        captureCallbacks.fetch_add(1);
        captureOutput.resize(frames * 2);
        for (size_t frame = 0; frame < frames; ++frame) {
            captureOutput[frame * 2] = static_cast<float>(input[frame * 2]) / 32768.0f;
            captureOutput[frame * 2 + 1] = static_cast<float>(input[frame * 2 + 1]) / 32768.0f;
        }
        if (renderRing.Write(captureOutput) > 0) {
            overflows.fetch_add(1);
        }
    }

    void HandleRender(int16_t* output, const size_t frames)
    {
        if (!audioRunning.load()) {
            std::memset(output, 0, frames * kChannelCount * sizeof(int16_t));
            return;
        }

        renderCallbacks.fetch_add(1);
        const size_t samples = frames * kChannelCount;
        if (renderScratch.size() < samples) {
            renderScratch.resize(samples);
        }

        const size_t copied = renderRing.Read(renderScratch.data(), samples);
        if (copied < samples) {
            if (captureCallbacks.load() > 0) {
                underruns.fetch_add(1);
            }
            std::fill(renderScratch.begin() + copied, renderScratch.begin() + samples, 0.0f);
        }

        std::lock_guard<std::mutex> processLock(processorMutex);
        resize(static_cast<uint32_t>(frames));
        for (size_t frame = 0; frame < frames; ++frame) {
            inputLeft[frame] = renderScratch[frame * 2];
            inputRight[frame] = renderScratch[frame * 2 + 1];
            outputLeft[frame] = 0.0f;
            outputRight[frame] = 0.0f;
        }

        const float* inputs[2] = {inputLeft.data(), inputRight.data()};
        float* outputs[2] = {outputLeft.data(), outputRight.data()};
        plugin->run(inputs, outputs, static_cast<uint32_t>(frames));
        inputPeak.store(std::max(PeakDb(inputLeft.data(), static_cast<uint32_t>(frames)),
                                 PeakDb(inputRight.data(), static_cast<uint32_t>(frames))));
        outputPeak.store(std::max(PeakDb(outputLeft.data(), static_cast<uint32_t>(frames)),
                                  PeakDb(outputRight.data(), static_cast<uint32_t>(frames))));
        inputLufs.store(plugin->getParameterValue(kParameter_lufs_in));
        outputLufs.store(plugin->getParameterValue(kParameter_lufs_out));

        for (size_t frame = 0; frame < frames; ++frame) {
            output[frame * 2] = ToS16(outputLeft[frame]);
            output[frame * 2 + 1] = ToS16(outputRight[frame]);
        }
        callbackFrames.fetch_add(frames);
    }

    static int16_t ToS16(const float sample)
    {
        const float clamped = std::max(-1.0f, std::min(1.0f, sample));
        return static_cast<int16_t>(std::lrint(clamped * 32767.0f));
    }

    bool StartStreamsLocked()
    {
        OH_AudioStreamBuilder* captureBuilder = nullptr;
        OH_AudioStreamBuilder* renderBuilder = nullptr;
        if (OH_AudioStreamBuilder_Create(&captureBuilder, AUDIOSTREAM_TYPE_CAPTURER) != AUDIOSTREAM_SUCCESS ||
            OH_AudioStreamBuilder_Create(&renderBuilder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS) {
            if (captureBuilder != nullptr) {
                OH_AudioStreamBuilder_Destroy(captureBuilder);
            }
            if (renderBuilder != nullptr) {
                OH_AudioStreamBuilder_Destroy(renderBuilder);
            }
            SetError(AUDIOSTREAM_ERROR_SYSTEM, "builder");
            return false;
        }

        auto configureCapture = [&]() {
            return OH_AudioStreamBuilder_SetSamplingRate(captureBuilder, kSampleRate) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetChannelCount(captureBuilder, kChannelCount) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetSampleFormat(captureBuilder, AUDIOSTREAM_SAMPLE_S16LE) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetLatencyMode(captureBuilder, AUDIOSTREAM_LATENCY_MODE_FAST) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetCapturerInfo(captureBuilder, AUDIOSTREAM_SOURCE_TYPE_MIC) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetCapturerReadDataCallback(captureBuilder, OnCaptureData, this) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetCapturerDeviceChangeCallback(captureBuilder,
                       OnCaptureDeviceChange, this) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetCapturerInterruptCallback(captureBuilder,
                       OnCaptureInterrupt, this) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetCapturerErrorCallback(captureBuilder, OnCaptureError, this) ==
                       AUDIOSTREAM_SUCCESS;
        };

        auto configureRender = [&]() {
            return OH_AudioStreamBuilder_SetSamplingRate(renderBuilder, kSampleRate) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetChannelCount(renderBuilder, kChannelCount) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetSampleFormat(renderBuilder, AUDIOSTREAM_SAMPLE_S16LE) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetLatencyMode(renderBuilder, AUDIOSTREAM_LATENCY_MODE_FAST) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetRendererInfo(renderBuilder, AUDIOSTREAM_USAGE_MUSIC) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetRendererWriteDataCallback(renderBuilder, OnRenderData, this) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetRendererOutputDeviceChangeCallback(renderBuilder,
                       OnRenderDeviceChange, this) == AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetRendererInterruptCallback(renderBuilder, OnRenderInterrupt, this) ==
                       AUDIOSTREAM_SUCCESS &&
                   OH_AudioStreamBuilder_SetRendererErrorCallback(renderBuilder, OnRenderError, this) ==
                       AUDIOSTREAM_SUCCESS;
        };

        const bool configured = configureCapture() && configureRender();
        const OH_AudioStream_Result captureResult = configured ?
            OH_AudioStreamBuilder_GenerateCapturer(captureBuilder, &captureStream) : AUDIOSTREAM_ERROR_INVALID_PARAM;
        const OH_AudioStream_Result renderResult = configured ?
            OH_AudioStreamBuilder_GenerateRenderer(renderBuilder, &renderStream) : AUDIOSTREAM_ERROR_INVALID_PARAM;
        OH_AudioStreamBuilder_Destroy(captureBuilder);
        OH_AudioStreamBuilder_Destroy(renderBuilder);

        if (!configured || captureResult != AUDIOSTREAM_SUCCESS || renderResult != AUDIOSTREAM_SUCCESS) {
            if (!configured) {
                SetError(AUDIOSTREAM_ERROR_INVALID_PARAM, "stream configuration");
            } else {
                SetError(captureResult != AUDIOSTREAM_SUCCESS ? captureResult : renderResult, "stream creation");
            }
            StopStreamsLocked();
            return false;
        }

        OH_AudioStream_Result startResult = OH_AudioCapturer_Start(captureStream);
        if (startResult == AUDIOSTREAM_SUCCESS) {
            startResult = OH_AudioRenderer_Start(renderStream);
        }
        if (startResult != AUDIOSTREAM_SUCCESS) {
            SetError(startResult, "stream start");
            StopStreamsLocked();
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex);
            lastError.clear();
        }
        lastRecoveryAttempt = std::chrono::steady_clock::now();
        return true;
    }

    void StopStreamsLocked()
    {
        if (captureStream != nullptr) {
            OH_AudioCapturer_Stop(captureStream);
            OH_AudioCapturer_Release(captureStream);
            captureStream = nullptr;
        }
        if (renderStream != nullptr) {
            OH_AudioRenderer_Stop(renderStream);
            OH_AudioRenderer_Release(renderStream);
            renderStream = nullptr;
        }
    }

    std::unique_ptr<HarmonyMasterMe> plugin;
    std::vector<float> inputLeft;
    std::vector<float> inputRight;
    std::vector<float> outputLeft;
    std::vector<float> outputRight;
    std::vector<float> captureOutput;
    std::vector<float> renderScratch;
    uint32_t maxFrames;
    std::mutex processorMutex;
    std::mutex audioMutex;
    std::mutex statusMutex;
    AudioRingBuffer renderRing;
    OH_AudioCapturer* captureStream = nullptr;
    OH_AudioRenderer* renderStream = nullptr;
    std::atomic<double> inputLufs{-70.0};
    std::atomic<double> outputLufs{-70.0};
    std::atomic<double> inputPeak{-70.0};
    std::atomic<double> outputPeak{-70.0};
    std::atomic<bool> audioRequested{false};
    std::atomic<bool> audioRunning{false};
    std::atomic<bool> recoveryPending{false};
    std::atomic<uint64_t> callbackFrames{0};
    std::atomic<uint64_t> captureCallbacks{0};
    std::atomic<uint64_t> renderCallbacks{0};
    std::atomic<uint64_t> underruns{0};
    std::atomic<uint64_t> overflows{0};
    std::atomic<uint64_t> deviceChangeCount{0};
    std::atomic<uint64_t> recoveryCount{0};
    std::atomic<uint64_t> audioErrorCount{0};
    std::string lastError;
    std::chrono::steady_clock::time_point lastRecoveryAttempt{};
};

void OnCaptureData(OH_AudioCapturer*, void* userData, void* audioData, const int32_t audioDataSize)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine == nullptr || audioData == nullptr || audioDataSize <= 0) {
        return;
    }
    const size_t frames = static_cast<size_t>(audioDataSize) /
                          (sizeof(int16_t) * Engine::kChannelCount);
    engine->HandleCapture(static_cast<const int16_t*>(audioData), frames);
}

OH_AudioData_Callback_Result OnRenderData(OH_AudioRenderer*, void* userData, void* audioData,
    const int32_t audioDataSize)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine == nullptr || audioData == nullptr || audioDataSize <= 0) {
        return AUDIO_DATA_CALLBACK_RESULT_INVALID;
    }
    const size_t frames = static_cast<size_t>(audioDataSize) /
                          (sizeof(int16_t) * Engine::kChannelCount);
    engine->HandleRender(static_cast<int16_t*>(audioData), frames);
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

void OnCaptureDeviceChange(OH_AudioCapturer*, void* userData, OH_AudioDeviceDescriptorArray*)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine != nullptr) {
        engine->deviceChangeCount.fetch_add(1);
        engine->recoveryPending.store(true);
    }
}

void OnRenderDeviceChange(OH_AudioRenderer*, void* userData,
    const OH_AudioStream_DeviceChangeReason reason)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine == nullptr) {
        return;
    }
    engine->deviceChangeCount.fetch_add(1);
    if (reason == REASON_OLD_DEVICE_UNAVAILABLE || reason == REASON_OVERRODE ||
        reason == REASON_STREAM_PRIORITY_CHANGED) {
        engine->recoveryPending.store(true);
    }
}

void OnCaptureInterrupt(OH_AudioCapturer*, void* userData, OH_AudioInterrupt_ForceType,
    const OH_AudioInterrupt_Hint hint)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine != nullptr &&
        (hint == AUDIOSTREAM_INTERRUPT_HINT_PAUSE || hint == AUDIOSTREAM_INTERRUPT_HINT_STOP)) {
        engine->recoveryPending.store(true);
    }
}

void OnRenderInterrupt(OH_AudioRenderer*, void* userData, OH_AudioInterrupt_ForceType,
    const OH_AudioInterrupt_Hint hint)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine != nullptr &&
        (hint == AUDIOSTREAM_INTERRUPT_HINT_PAUSE || hint == AUDIOSTREAM_INTERRUPT_HINT_STOP)) {
        engine->recoveryPending.store(true);
    }
}

void OnCaptureError(OH_AudioCapturer*, void* userData, const OH_AudioStream_Result error)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine != nullptr) {
        engine->SetError(error, "capture");
    }
}

void OnRenderError(OH_AudioRenderer*, void* userData, const OH_AudioStream_Result error)
{
    auto* const engine = static_cast<Engine*>(userData);
    if (engine != nullptr) {
        engine->SetError(error, "render");
    }
}

Engine* GetEngine(napi_env env, napi_value value)
{
    void* data = nullptr;
    napi_get_value_external(env, value, &data);
    return static_cast<Engine*>(data);
}

double ReadNumber(const napi_env env, const napi_value value, const double fallback)
{
    double result = fallback;
    if (napi_get_value_double(env, value, &result) != napi_ok) {
        return fallback;
    }
    return result;
}

napi_value MakeNumber(const napi_env env, const double value)
{
    napi_value result = nullptr;
    napi_create_double(env, value, &result);
    return result;
}

napi_value MakeBoolean(const napi_env env, const bool value)
{
    napi_value result = nullptr;
    napi_get_boolean(env, value, &result);
    return result;
}

napi_value MakeSnapshot(const napi_env env, const Engine& engine)
{
    napi_value object = nullptr;
    napi_create_object(env, &object);
    napi_set_named_property(env, object, "inputLufs", MakeNumber(env, engine.inputLufs.load()));
    napi_set_named_property(env, object, "outputLufs", MakeNumber(env, engine.outputLufs.load()));
    napi_set_named_property(env, object, "inputPeak", MakeNumber(env, engine.inputPeak.load()));
    napi_set_named_property(env, object, "outputPeak", MakeNumber(env, engine.outputPeak.load()));
    return object;
}

napi_value MakeAudioStatus(const napi_env env, Engine& engine)
{
    engine.RefreshAudioStatus();

    std::string error;
    {
        std::lock_guard<std::mutex> lock(engine.statusMutex);
        error = engine.lastError;
    }

    napi_value object = nullptr;
    napi_create_object(env, &object);
    napi_set_named_property(env, object, "running", MakeBoolean(env, engine.audioRunning.load()));
    napi_set_named_property(env, object, "requested", MakeBoolean(env, engine.audioRequested.load()));
    napi_set_named_property(env, object, "sampleRate", MakeNumber(env, Engine::kSampleRate));
    napi_set_named_property(env, object, "channels", MakeNumber(env, Engine::kChannelCount));
    napi_set_named_property(env, object, "callbackFrames",
        MakeNumber(env, static_cast<double>(engine.callbackFrames.load())));
    napi_set_named_property(env, object, "captureCallbacks",
        MakeNumber(env, static_cast<double>(engine.captureCallbacks.load())));
    napi_set_named_property(env, object, "renderCallbacks",
        MakeNumber(env, static_cast<double>(engine.renderCallbacks.load())));
    napi_set_named_property(env, object, "underruns",
        MakeNumber(env, static_cast<double>(engine.underruns.load())));
    napi_set_named_property(env, object, "overflows",
        MakeNumber(env, static_cast<double>(engine.overflows.load())));
    napi_set_named_property(env, object, "deviceChangeCount",
        MakeNumber(env, static_cast<double>(engine.deviceChangeCount.load())));
    napi_set_named_property(env, object, "recoveryCount",
        MakeNumber(env, static_cast<double>(engine.recoveryCount.load())));
    napi_set_named_property(env, object, "errorCount",
        MakeNumber(env, static_cast<double>(engine.audioErrorCount.load())));

    napi_value errorValue = nullptr;
    napi_create_string_utf8(env, error.c_str(), NAPI_AUTO_LENGTH, &errorValue);
    napi_set_named_property(env, object, "lastError", errorValue);
    return object;
}

double PeakDb(const float* samples, const uint32_t frames)
{
    double peak = 0.0;
    for (uint32_t index = 0; index < frames; ++index) {
        peak = std::max(peak, static_cast<double>(std::fabs(samples[index])));
    }
    return peak > 0.000001 ? 20.0 * std::log10(peak) : -70.0;
}

void FinalizeEngine(napi_env, void* finalizeData, void*)
{
    delete static_cast<Engine*>(finalizeData);
}

napi_value CreateEngine(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);

    const double sampleRate = argc > 0 ? ReadNumber(env, argv[0], 48000.0) : 48000.0;
    const double requestedBufferSize = argc > 1 ? ReadNumber(env, argv[1], 512.0) : 512.0;
    const auto bufferSize = static_cast<uint32_t>(std::max(64.0, requestedBufferSize));

    auto engine = new Engine(sampleRate, bufferSize);
    napi_value result = nullptr;
    napi_create_external(env, engine, FinalizeEngine, nullptr, &result);
    return result;
}

napi_value DestroyEngine(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    if (argc > 0) {
        Engine* engine = GetEngine(env, argv[0]);
        delete engine;
    }
    return nullptr;
}

napi_value LoadPreset(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr || argc < 2) {
        return nullptr;
    }

    const int32_t index = static_cast<int32_t>(ReadNumber(env, argv[1], 0.0));
    if (index < 0 || index >= static_cast<int32_t>(ARRAY_SIZE(kEasyPresets))) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(engine->processorMutex);
    engine->plugin->loadProgram(static_cast<uint32_t>(index));
    return nullptr;
}

napi_value SetParameter(napi_env env, napi_callback_info info)
{
    size_t argc = 3;
    napi_value argv[3] = {nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr || argc < 3) {
        return nullptr;
    }

    const auto index = static_cast<uint32_t>(std::max(0.0, ReadNumber(env, argv[1], 0.0)));
    const float value = static_cast<float>(ReadNumber(env, argv[2], 0.0));
    std::lock_guard<std::mutex> lock(engine->processorMutex);
    engine->plugin->setParameterValue(index, value);
    return nullptr;
}

napi_value GetParameter(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value argv[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr || argc < 2) {
        return MakeNumber(env, 0.0);
    }

    const auto index = static_cast<uint32_t>(std::max(0.0, ReadNumber(env, argv[1], 0.0)));
    std::lock_guard<std::mutex> lock(engine->processorMutex);
    return MakeNumber(env, engine->plugin->getParameterValue(index));
}

napi_value ProcessInterleaved(napi_env env, napi_callback_info info)
{
    size_t argc = 4;
    napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr || argc < 4) {
        return MakeNumber(env, 0.0);
    }

    napi_typedarray_type inputType = napi_float32_array;
    napi_typedarray_type outputType = napi_float32_array;
    size_t inputElements = 0;
    size_t outputElements = 0;
    void* inputData = nullptr;
    void* outputData = nullptr;
    napi_value inputBuffer = nullptr;
    napi_value outputBuffer = nullptr;
    size_t inputOffset = 0;
    size_t outputOffset = 0;

    if (napi_get_typedarray_info(env, argv[1], &inputType, &inputElements, &inputData,
                                 &inputBuffer, &inputOffset) != napi_ok ||
        napi_get_typedarray_info(env, argv[2], &outputType, &outputElements, &outputData,
                                 &outputBuffer, &outputOffset) != napi_ok ||
        inputType != napi_float32_array || outputType != napi_float32_array) {
        return MakeSnapshot(env, *engine);
    }

    const auto requestedFrames = static_cast<uint32_t>(std::max(0.0, ReadNumber(env, argv[3], 0.0)));
    const auto frames = std::min<uint32_t>(requestedFrames,
        static_cast<uint32_t>(std::min(inputElements, outputElements) / 2));

    {
        std::lock_guard<std::mutex> lock(engine->processorMutex);
        const auto* const input = static_cast<const float*>(inputData);
        auto* const output = static_cast<float*>(outputData);
        engine->ProcessLocked(input, output, frames);
    }

    return MakeSnapshot(env, *engine);
}

napi_value GetMeterSnapshot(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr) {
        return nullptr;
    }
    return MakeSnapshot(env, *engine);
}

napi_value StartAudio(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr) {
        return nullptr;
    }
    engine->StartAudio();
    return MakeAudioStatus(env, *engine);
}

napi_value StopAudio(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr) {
        return nullptr;
    }
    engine->StopAudio();
    return MakeAudioStatus(env, *engine);
}

napi_value GetAudioStatus(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value argv[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    Engine* const engine = argc > 0 ? GetEngine(env, argv[0]) : nullptr;
    if (engine == nullptr) {
        return nullptr;
    }
    return MakeAudioStatus(env, *engine);
}

napi_value Initialize(napi_env env, napi_value exports)
{
    const napi_property_descriptor properties[] = {
        {"createEngine", nullptr, CreateEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyEngine", nullptr, DestroyEngine, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"loadPreset", nullptr, LoadPreset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setParameter", nullptr, SetParameter, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getParameter", nullptr, GetParameter, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"processInterleaved", nullptr, ProcessInterleaved, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getMeterSnapshot", nullptr, GetMeterSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startAudio", nullptr, StartAudio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopAudio", nullptr, StopAudio, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getAudioStatus", nullptr, GetAudioStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
    };

    napi_define_properties(env, exports, sizeof(properties) / sizeof(properties[0]), properties);
    return exports;
}

} // namespace

static napi_module masterMeModule = {
    1,
    0,
    nullptr,
    Initialize,
    "masterme",
    nullptr,
    {0},
};

extern "C" __attribute__((constructor)) void RegisterMasterMeModule()
{
    napi_module_register(&masterMeModule);
}
