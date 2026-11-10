#include <napi/native_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
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

struct Engine {
    explicit Engine(const double sampleRate, const uint32_t bufferSize)
        : inputLeft(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          inputRight(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          outputLeft(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          outputRight(std::max<uint32_t>(bufferSize, 4096), 0.0f),
          maxFrames(std::max<uint32_t>(bufferSize, 4096))
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
        if (plugin != nullptr) {
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

    std::unique_ptr<HarmonyMasterMe> plugin;
    std::vector<float> inputLeft;
    std::vector<float> inputRight;
    std::vector<float> outputLeft;
    std::vector<float> outputRight;
    uint32_t maxFrames;
    std::mutex mutex;
    double inputLufs = -70.0;
    double outputLufs = -70.0;
    double inputPeak = -70.0;
    double outputPeak = -70.0;
};

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

napi_value MakeSnapshot(const napi_env env, const Engine& engine)
{
    napi_value object = nullptr;
    napi_create_object(env, &object);
    napi_set_named_property(env, object, "inputLufs", MakeNumber(env, engine.inputLufs));
    napi_set_named_property(env, object, "outputLufs", MakeNumber(env, engine.outputLufs));
    napi_set_named_property(env, object, "inputPeak", MakeNumber(env, engine.inputPeak));
    napi_set_named_property(env, object, "outputPeak", MakeNumber(env, engine.outputPeak));
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

    std::lock_guard<std::mutex> lock(engine->mutex);
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
    std::lock_guard<std::mutex> lock(engine->mutex);
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
    std::lock_guard<std::mutex> lock(engine->mutex);
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
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->resize(frames);

        const auto* const input = static_cast<const float*>(inputData);
        auto* const output = static_cast<float*>(outputData);
        for (uint32_t frame = 0; frame < frames; ++frame) {
            engine->inputLeft[frame] = input[frame * 2];
            engine->inputRight[frame] = input[frame * 2 + 1];
            engine->outputLeft[frame] = 0.0f;
            engine->outputRight[frame] = 0.0f;
        }

        const float* inputs[2] = {engine->inputLeft.data(), engine->inputRight.data()};
        float* outputs[2] = {engine->outputLeft.data(), engine->outputRight.data()};
        engine->plugin->run(inputs, outputs, frames);

        for (uint32_t frame = 0; frame < frames; ++frame) {
            output[frame * 2] = engine->outputLeft[frame];
            output[frame * 2 + 1] = engine->outputRight[frame];
        }

        engine->inputPeak = std::max(PeakDb(engine->inputLeft.data(), frames),
                                     PeakDb(engine->inputRight.data(), frames));
        engine->outputPeak = std::max(PeakDb(engine->outputLeft.data(), frames),
                                      PeakDb(engine->outputRight.data(), frames));
        engine->inputLufs = engine->plugin->getParameterValue(kParameter_lufs_in);
        engine->outputLufs = engine->plugin->getParameterValue(kParameter_lufs_out);
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
