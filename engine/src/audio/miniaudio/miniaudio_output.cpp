// miniaudio playback backend (the only file that includes miniaudio.h).
#include <memory>
#include <string>
#include "miniaudio.h"
#include "litholattice/audio/audio_engine.h"
#include "litholattice/audio/audio_output.h"

namespace litholattice {
namespace {

class MiniaudioOutput final : public AudioOutput {
 public:
  explicit MiniaudioOutput(AudioEngine& engine) : engine_(engine) {}
  MiniaudioOutput(const MiniaudioOutput&) = delete;
  MiniaudioOutput& operator=(const MiniaudioOutput&) = delete;
  ~MiniaudioOutput() override {
    if (open_) ma_device_uninit(&device_);
  }

  bool open(std::string* err) {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate = ma_uint32(engine_.sampleRate());
    cfg.dataCallback = &MiniaudioOutput::callback;
    cfg.pUserData = &engine_;
    if (ma_result r = ma_device_init(nullptr, &cfg, &device_); r != MA_SUCCESS) return fail(err, r);
    open_ = true;
    if (ma_result r = ma_device_start(&device_); r != MA_SUCCESS) return fail(err, r);
    return true;
  }

  std::string description() const override {
    return std::string("miniaudio ") + MA_VERSION_STRING + " | " + device_.playback.name + " | " +
           ma_get_backend_name(device_.pContext->backend);
  }

 private:
  static bool fail(std::string* err, ma_result r) {
    if (err) *err = ma_result_description(r);
    return false;
  }
  static void callback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
    static_cast<AudioEngine*>(dev->pUserData)->render(static_cast<float*>(out), frames, int(dev->playback.channels));
  }

  AudioEngine& engine_;
  ma_device device_{};
  bool open_ = false;
};

}  // namespace

std::unique_ptr<AudioOutput> openAudioOutput(AudioEngine& engine, std::string* err) {
  auto out = std::make_unique<MiniaudioOutput>(engine);
  if (!out->open(err)) return nullptr;
  return out;
}

}  // namespace litholattice
