#include "shell/assistant/assistant_voice.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "shell/assistant/assistant_audio_level.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <md4c.h>
#include <sndfile.h>
#include <unistd.h>

namespace {
  std::string path(const std::string& value) { return FileUtils::expandUserPath(value).string(); }

  bool modelExists(const std::string& value) {
    std::error_code error;
    return !value.empty() && std::filesystem::is_regular_file(path(value), error);
  }

  bool executable(const std::string& value) { return !value.empty() && process::commandExists(path(value).c_str()); }

  std::string speechText(const std::string& markdown) {
    struct Context {
      std::string text;
      bool code = false;
    } context;
    MD_PARSER parser{};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
    parser.enter_block = [](MD_BLOCKTYPE type, void*, void* data) {
      if (type == MD_BLOCK_CODE)
        static_cast<Context*>(data)->code = true;
      return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void*, void* data) {
      auto& ctx = *static_cast<Context*>(data);
      if (type == MD_BLOCK_CODE)
        ctx.code = false;
      ctx.text += '\n';
      return ctx.text.size() > 4000 ? 1 : 0;
    };
    parser.enter_span = [](MD_SPANTYPE, void*, void*) { return 0; };
    parser.leave_span = [](MD_SPANTYPE, void*, void*) { return 0; };
    parser.text = [](MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* data) {
      auto& ctx = *static_cast<Context*>(data);
      if (!ctx.code) {
        if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR)
          ctx.text += ' ';
        else if (type != MD_TEXT_HTML)
          ctx.text.append(text, size);
      }
      return ctx.text.size() > 4000 ? 1 : 0;
    };
    md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &parser, &context);
    return StringUtils::trim(StringUtils::truncateUtf8(context.text, 4000));
  }

  // pw-record supplies native float PCM. The worker writes the same samples to Whisper's WAV
  // and meters them, so visual feedback never opens a second microphone stream.
  struct Capture {
    SNDFILE* file = nullptr;
    std::shared_ptr<AssistantAudioLevel> level;
    std::string pending;
    std::size_t frames = 0;
    bool failed = false;
    ~Capture() { finish(); }
    void finish() {
      if (file) {
        failed |= sf_close(file) != 0;
        file = nullptr;
      }
    }
    void append(std::string_view bytes) {
      if (!file || failed)
        return;
      pending.append(bytes);
      std::array<float, 1024> samples{};
      std::size_t offset = 0;
      while (pending.size() - offset >= sizeof(float) && frames < 960000) {
        const auto count = std::min({samples.size(), (pending.size() - offset) / sizeof(float), 960000 - frames});
        std::memcpy(samples.data(), pending.data() + offset, count * sizeof(float));
        for (std::size_t i = 0; i < count; ++i)
          samples[i] = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0F, 1.0F) : 0.0F;
        if (sf_writef_float(file, samples.data(), static_cast<sf_count_t>(count)) != static_cast<sf_count_t>(count)) {
          failed = true;
          break;
        }
        level->capture(std::span(samples.data(), count));
        frames += count;
        offset += count * sizeof(float);
      }
      pending.erase(0, offset);
      if (frames == 960000 || failed)
        pending.clear();
    }
  };

  bool usableAudio(const std::filesystem::path& file) {
    SF_INFO info{};
    auto* audio = sf_open(file.c_str(), SFM_READ, &info);
    if (!audio)
      return false;
    sf_close(audio);
    return info.samplerate > 0
        && info.channels == 1
        && info.frames >= info.samplerate / 10
        && info.frames <= static_cast<sf_count_t>(info.samplerate) * 300;
  }
} // namespace

AssistantVoice::Workspace::~Workspace() {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

AssistantVoice::~AssistantVoice() {
  m_changed = {};
  m_transcript = {};
  m_alive.reset();
  cancel();
}

void AssistantVoice::changed() {
  if (m_changed)
    m_changed();
}

void AssistantVoice::cancel() {
  ++m_generation;
  if (m_cancel)
    *m_cancel = true;
  m_cancel.reset();
  // The worker retains the private files until its process has actually exited.
  m_workspace.reset();
  m_level.reset();
  m_state = State::Idle;
  m_error.clear();
  changed();
}

void AssistantVoice::fail(std::string key) {
  m_level.reset();
  m_state = State::Idle;
  m_workspace.reset();
  m_error = i18n::tr(key);
  changed();
}

bool AssistantVoice::workspace() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  std::string name = std::string(runtime && *runtime ? runtime : "/tmp") + "/noctalia-voice-XXXXXX";
  if (!::mkdtemp(name.data())) {
    fail("assistant.voice.files-error");
    return false;
  }
  m_workspace = std::make_shared<Workspace>();
  m_workspace->path = name;
  return true;
}

float AssistantVoice::audioLevel() const {
  return m_level && (m_state == State::Recording || m_state == State::Speaking) ? m_level->level() : 0.0F;
}

void AssistantVoice::run(
    std::vector<std::string> args, int seconds, std::function<void(process::RunResult)> done,
    process::OutputCallback output, process::OutputCallback diagnostic
) {
  const std::weak_ptr<void> alive = m_alive;
  const auto generation = m_generation;
  const auto files = m_workspace;
  m_cancel = std::make_shared<std::atomic<bool>>(false);
  const bool capturesAudio = static_cast<bool>(output);
  const bool started = process::runAsync(
      args,
      {.stdOut = std::move(output),
       .stdErr = std::move(diagnostic),
       .onExit =
           [this, alive, generation, files, done = std::move(done)](process::RunResult result) {
             DeferredCall::callLater([this, alive, generation, files, done, result = std::move(result)]() mutable {
               if (alive.expired() || generation != m_generation)
                 return;
               m_cancel.reset();
               done(std::move(result));
             });
           }},
      {.timeout = std::chrono::seconds(seconds),
       .maxOutputBytes = capturesAudio ? 0U : 64U * 1024U,
       .cancel = m_cancel,
       .terminateWithParent = true}
  );
  if (!started)
    fail("assistant.voice.process-error");
}

void AssistantVoice::dictate(const Config& config) {
  cancel();
  m_config = config;
  if (!config.enabled
      || !executable(config.whisperCommand)
      || !modelExists(config.whisperModel)
      || !executable("pw-record")) {
    fail("assistant.voice.setup-dictation");
    return;
  }
  if (!workspace())
    return;
  m_level = std::make_shared<AssistantAudioLevel>();
  const auto capture = std::make_shared<Capture>();
  capture->level = m_level;
  SF_INFO info{};
  info.samplerate = 16000;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  capture->file = sf_open((m_workspace->path / "input.wav").c_str(), SFM_WRITE, &info);
  if (!capture->file) {
    fail("assistant.voice.files-error");
    return;
  }
  m_state = State::Recording;
  changed();
  run(
      {"pw-record", "--rate", "16000", "--channels", "1", "--format", "f32", "--raw", "--latency", "20ms",
       "--sample-count", "960000", "--properties",
       "application.name=Noctalia media.name=Dictation media.role=Communication", "-"},
      65,
      [this, capture](process::RunResult result) {
        capture->finish();
        // pw-record returns 1 when SIGTERM stops capture before its sample limit.
        // Only accept that exit for a deliberate stop, and still validate the recorded audio.
        const bool stopped =
            result.timedOut && m_state == State::Transcribing && (result.exitCode == 0 || result.exitCode == 1);
        if ((!result && !stopped) || capture->failed || !usableAudio(m_workspace->path / "input.wav")) {
          fail("assistant.voice.capture-error");
          return;
        }
        transcribe();
      },
      [capture](std::string_view bytes) { capture->append(bytes); }
  );
}

void AssistantVoice::finishRecording() {
  if (m_state != State::Recording || !m_cancel)
    return;
  m_state = State::Transcribing;
  *m_cancel = true;
  changed();
}

void AssistantVoice::transcribe() {
  m_state = State::Transcribing;
  changed();
  run({path(m_config.whisperCommand), "-m", path(m_config.whisperModel), "-f",
       (m_workspace->path / "input.wav").string(), "-l", "auto", "-t", "4", "--no-gpu", "-nt", "-otxt", "-of",
       (m_workspace->path / "transcript").string()},
      120, [this](process::RunResult result) {
        if (!result) {
          fail("assistant.voice.transcribe-error");
          return;
        }
        std::ifstream input(m_workspace->path / "transcript.txt");
        std::string text(12000, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(input.gcount()));
        text = StringUtils::trim(StringUtils::truncateUtf8(text, 11996));
        if (text.empty()) {
          fail("assistant.voice.no-speech");
          return;
        }
        m_state = State::Idle;
        m_workspace.reset();
        if (m_transcript)
          m_transcript(std::move(text));
        changed();
      });
}

void AssistantVoice::speak(const Config& config, std::string text, bool markdown) {
  cancel();
  m_config = config;
  if (!config.enabled
      || !executable(config.piperCommand)
      || !modelExists(config.piperModel)
      || !modelExists(config.piperModel + ".json")
      || !executable("pw-play")) {
    fail("assistant.voice.setup-speech");
    return;
  }
  // Keep a spoken reply bounded; the full answer remains readable and copyable.
  text = markdown ? speechText(text) : StringUtils::trim(StringUtils::truncateUtf8(text, 4000));
  if (text.empty()) {
    fail("assistant.voice.no-readable-text");
    return;
  }
  if (!workspace())
    return;
  std::ofstream input(m_workspace->path / "reply.txt");
  input << text;
  input.close();
  if (!input) {
    fail("assistant.voice.files-error");
    return;
  }
  m_state = State::PreparingSpeech;
  changed();
  run({path(config.piperCommand), "-m", path(config.piperModel), "--input-file",
       (m_workspace->path / "reply.txt").string(), "-f", (m_workspace->path / "reply.wav").string()},
      120, [this](process::RunResult result) {
        if (!result || !usableAudio(m_workspace->path / "reply.wav")) {
          fail("assistant.voice.speech-error");
          return;
        }
        m_level = std::make_shared<AssistantAudioLevel>();
        m_level->loadPlayback(m_workspace->path / "reply.wav");
        const auto meter = m_level;
        m_state = State::Speaking;
        changed();
        run(
            {"pw-play", "--verbose", "--latency", "40ms", "--properties",
             "application.name=Noctalia media.name=Assistant media.role=Communication",
             (m_workspace->path / "reply.wav").string()},
            300,
            [this](process::RunResult played) {
              if (!played) {
                fail("assistant.voice.playback-error");
                return;
              }
              m_state = State::Idle;
              m_level.reset();
              m_workspace.reset();
              changed();
            },
            {}, [meter](std::string_view bytes) { meter->playbackEvent(bytes); }
        );
      });
}
