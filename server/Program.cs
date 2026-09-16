using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

// ═══════════════════════════════════════════════════════════════
//  CARDBOARD ROBOT - VOICE ASSISTANT SERVER
//  Architecture: ESP32 <-> this server (runs on a VPS or on your PC)
//
//  Pipeline for every sentence the robot hears:
//    ESP32 mic audio (16 kHz PCM16, base64 in JSON over WebSocket)
//      -> Voice Activity Detection (dynamic threshold, calibrated at startup)
//      -> WAV file -> OpenAI Whisper (speech-to-text)
//      -> wake-word check ("assistant", "hello", "robot")
//      -> ChatGPT (gpt-4o-mini, short answers)
//      -> OpenAI TTS (mp3) -> ffmpeg -> 16 kHz PCM16
//      -> streamed back to the ESP32 in 2 KB chunks
//
//  Feedback messages to the ESP32: wake_beep, speech_ended, listen
//
//  Configuration comes from environment variables (see README.md):
//    OPENAI_API_KEY (required), ROBOT_PORT, ROBOT_AUTH_TOKEN,
//    ROBOT_WAKE_WORDS, ROBOT_TTS_VOICE, ROBOT_LANGUAGE, ROBOT_KEEP_RECORDINGS
// ═══════════════════════════════════════════════════════════════

static class Config
{
    static string Env(string name, string fallback) =>
        Environment.GetEnvironmentVariable(name) is { Length: > 0 } v ? v : fallback;

    // 🔑 Never hard-code the key: export OPENAI_API_KEY=sk-... before starting the server
    public static readonly string OpenAiApiKey = Env("OPENAI_API_KEY", "");

    // Optional shared secret the ESP32 must send in its registration message
    // ({"id":"esp32","token":"..."}). Empty = accept any client. Set it on a public VPS!
    public static readonly string AuthToken = Env("ROBOT_AUTH_TOKEN", "");

    // The robot beeps right after speech_ended / wake_beep and its mic hears that beep.
    // Ignore incoming audio for this long after sending those messages (ms).
    public const int IgnoreAudioAfterBeepMs = 700;

    // Keep recording_*.wav files after transcription (default: delete them)
    public static readonly bool KeepRecordings = Env("ROBOT_KEEP_RECORDINGS", "0") == "1";

    // Audio settings (must match ESP32)
    public const int SampleRate = 16000;
    public const int Channels = 1;
    public const int BitsPerSample = 16;

    // ✅ DYNAMIC VAD - Background noise + offset
    public const double VadNoiseOffset = 1000.0;
    public const int VadCalibrationSeconds = 3;
    public const int SpeechChunksToStart = 2;
    public const int SilenceChunksToStop = 10;
    public const int MinSpeechDurationMs = 500;

    // Wake words (comma separated in ROBOT_WAKE_WORDS). The sentence must contain one of them.
    public static readonly string[] WakeWords =
        Env("ROBOT_WAKE_WORDS", "assistant,hello,robot").Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
    public const bool EnableWakeWordDetection = true;

    // Language for Whisper (ISO code, e.g. "en", "ar") and the OpenAI TTS voice
    // (alloy, echo, fable, onyx, nova, shimmer)
    public static readonly string SttLanguage = Env("ROBOT_LANGUAGE", "en");
    public static readonly string TtsVoice = Env("ROBOT_TTS_VOICE", "echo");

    // Server settings
    public static readonly int ServerPort = int.TryParse(Env("ROBOT_PORT", "8080"), out var p) ? p : 8080;
    public const string TranscriptionsFolder = "transcriptions";
}

// ═══════════════════════════════════════════════════════════════
//  VOICE ACTIVITY DETECTION (VAD) - DYNAMIC WITH CALIBRATION
// ═══════════════════════════════════════════════════════════════
class VoiceActivityDetector
{
    private double _speechThreshold;
    private bool _isCalibrated = false;
    private List<double> _calibrationSamples = new();
    private DateTime _calibrationStart = DateTime.Now;

    public bool IsSpeaking { get; private set; } = false;
    private int _speechCounter = 0;
    private int _silenceCounter = 0;
    private Stopwatch _utteranceTimer = new();

    // Real-time monitoring
    private int _chunkCounter = 0;
    private const int LogEveryNChunks = 10; // Show VAD every 10 chunks (~1 s: 1536 samples per chunk at 16 kHz)

    public event Action? OnSpeechStarted;
    public event Action? OnSpeechEnded;

    public VoiceActivityDetector()
    {
        Log($"[VAD] 🎯 Dynamic VAD - calibrating background noise for {Config.VadCalibrationSeconds}s...");
    }

    public void ProcessChunk(byte[] pcmBytes)
    {
        double rms = ComputeRms(pcmBytes);

        // Calibration phase
        if (!_isCalibrated)
        {
            _calibrationSamples.Add(rms);

            if ((DateTime.Now - _calibrationStart).TotalSeconds >= Config.VadCalibrationSeconds)
            {
                double avgNoise = _calibrationSamples.Average();
                _speechThreshold = avgNoise + Config.VadNoiseOffset;
                _isCalibrated = true;

                Log($"[VAD] ✅ Calibration complete!");
                Log($"[VAD] 📊 Background noise: {avgNoise:F1}");
                Log($"[VAD] 📊 Speech threshold: {_speechThreshold:F1} (noise + {Config.VadNoiseOffset})");
                Log($"[VAD] 👁️  Real-time monitoring enabled");

                _calibrationSamples.Clear();
            }
            return;
        }

        // Real-time monitoring display
        _chunkCounter++;
        if (_chunkCounter >= LogEveryNChunks)
        {
            _chunkCounter = 0;
            string indicator = rms >= _speechThreshold ? "🟢 SPEECH" : "⚪ silence";
            string bar = GenerateBar(rms, _speechThreshold);
            Log($"[VAD] {indicator} | RMS: {rms,6:F1} | Threshold: {_speechThreshold,6:F1} | {bar}");
        }

        // Normal VAD operation
        if (rms >= _speechThreshold)
        {
            _speechCounter++;
            _silenceCounter = 0;

            if (!IsSpeaking && _speechCounter >= Config.SpeechChunksToStart)
            {
                IsSpeaking = true;
                _utteranceTimer.Restart();
                Log("[VAD] 🎙️  SPEECH STARTED");
                OnSpeechStarted?.Invoke();
            }
        }
        else
        {
            _silenceCounter++;
            _speechCounter = 0;

            if (IsSpeaking && _silenceCounter >= Config.SilenceChunksToStop)
            {
                long durationMs = _utteranceTimer.ElapsedMilliseconds;

                if (durationMs >= Config.MinSpeechDurationMs)
                {
                    Log($"[VAD] 🔇 SPEECH ENDED ({durationMs}ms)");
                    OnSpeechEnded?.Invoke();
                }

                IsSpeaking = false;
            }
        }
    }

    private string GenerateBar(double rms, double threshold)
    {
        // Create a simple ASCII bar graph
        int maxWidth = 40;
        double maxValue = threshold * 2; // Show up to 2x threshold

        int rmsWidth = (int)Math.Min((rms / maxValue) * maxWidth, maxWidth);
        int thresholdPos = (int)((threshold / maxValue) * maxWidth);

        string bar = new string('█', rmsWidth);
        string spaces = new string(' ', Math.Max(0, maxWidth - rmsWidth));

        // Insert threshold marker
        if (thresholdPos < maxWidth)
        {
            string result = bar + spaces;
            if (thresholdPos < result.Length)
            {
                result = result.Substring(0, thresholdPos) + "|" +
                        (thresholdPos + 1 < result.Length ? result.Substring(thresholdPos + 1) : "");
            }
            return result;
        }

        return bar + spaces;
    }

    private double ComputeRms(byte[] pcmBytes)
    {
        int sampleCount = pcmBytes.Length / 2;
        if (sampleCount == 0) return 0;

        double sumSq = 0;
        for (int i = 0; i < sampleCount; i++)
        {
            short s = BitConverter.ToInt16(pcmBytes, i * 2);
            sumSq += (double)s * s;
        }
        return Math.Sqrt(sumSq / sampleCount);
    }

    private void Log(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {msg}");
}

// ═══════════════════════════════════════════════════════════════
//  OPENAI API CLIENT
// ═══════════════════════════════════════════════════════════════
class OpenAIClient
{
    private readonly HttpClient _httpClient = new();
    private readonly string _apiKey;

    public OpenAIClient(string apiKey)
    {
        _apiKey = apiKey;
        _httpClient.DefaultRequestHeaders.Authorization =
            new AuthenticationHeaderValue("Bearer", _apiKey);
    }

    // Turn a failed OpenAI response into an exception whose message says what to check
    private static async Task ThrowIfFailedAsync(HttpResponseMessage response, string what)
    {
        if (response.IsSuccessStatusCode) return;
        string body = await response.Content.ReadAsStringAsync();
        string hint = (int)response.StatusCode switch
        {
            401 => " -> check OPENAI_API_KEY: it must be your own valid key from https://platform.openai.com/api-keys",
            429 => " -> rate limit or no credit left on your OpenAI account",
            _ => ""
        };
        throw new HttpRequestException($"{what} failed with HTTP {(int)response.StatusCode}{hint}. Details: {body}");
    }

    // Speech-to-Text (Whisper)
    public async Task<string?> TranscribeAsync(string wavFilePath)
    {
        try
        {
            if (!File.Exists(wavFilePath))
                return null;

            byte[] audioBytes = await File.ReadAllBytesAsync(wavFilePath);
            Log($"[STT] 🔄 Transcribing {Path.GetFileName(wavFilePath)}...");

            using var content = new MultipartFormDataContent();
            content.Add(new StringContent("whisper-1"), "model");
            content.Add(new StringContent("text"), "response_format");
            content.Add(new StringContent(Config.SttLanguage), "language");

            var audioContent = new ByteArrayContent(audioBytes);
            audioContent.Headers.ContentType = MediaTypeHeaderValue.Parse("audio/wav");
            content.Add(audioContent, "file", Path.GetFileName(wavFilePath));

            var response = await _httpClient.PostAsync(
                "https://api.openai.com/v1/audio/transcriptions",
                content);

            await ThrowIfFailedAsync(response, "Whisper");
            string transcription = (await response.Content.ReadAsStringAsync()).Trim();

            if (!string.IsNullOrEmpty(transcription))
            {
                Log($"[STT] ✅ Transcription: {transcription}");
            }

            return transcription;
        }
        catch (Exception ex)
        {
            Log($"[STT] ❌ Error: {ex.Message}");
            return null;
        }
    }

    // ChatGPT
    public async Task<string?> GetChatResponseAsync(string userMessage)
    {
        try
        {
            Log($"[ChatGPT] 🤖 Sending to gpt-4o-mini...");

            var payload = new
            {
                model = "gpt-4o-mini",
                messages = new[]
                {
                    new { role = "system", content = "You are a helpful and intelligent voice assistant. Keep your responses short and clear (2-3 sentences). Always respond in English." },
                    new { role = "user", content = userMessage }
                }
            };

            var jsonContent = new StringContent(
                JsonSerializer.Serialize(payload),
                Encoding.UTF8,
                "application/json");

            var response = await _httpClient.PostAsync(
                "https://api.openai.com/v1/chat/completions",
                jsonContent);

            await ThrowIfFailedAsync(response, "ChatGPT");
            string result = await response.Content.ReadAsStringAsync();
            var doc = JsonDocument.Parse(result);

            string answer = doc.RootElement
                .GetProperty("choices")[0]
                .GetProperty("message")
                .GetProperty("content")
                .GetString() ?? "";

            if (!string.IsNullOrEmpty(answer))
            {
                Log($"[ChatGPT] ✅ Response: {answer}");
            }

            return answer;
        }
        catch (Exception ex)
        {
            Log($"[ChatGPT] ❌ Error: {ex.Message}");
            return null;
        }
    }

    // Text-to-Speech, resampled to Config.SampleRate PCM16 with ffmpeg
    public async Task<byte[]?> GenerateSpeechAsync(string text, string voice = "alloy")
    {
        try
        {
            Log($"[TTS] 🔊 Generating speech...");

            // Request MP3 format for best quality
            var payload = new
            {
                model = "tts-1",
                voice = voice,
                input = text,
                response_format = "mp3"
            };

            var jsonContent = new StringContent(
                JsonSerializer.Serialize(payload),
                Encoding.UTF8,
                "application/json");

            var response = await _httpClient.PostAsync(
                "https://api.openai.com/v1/audio/speech",
                jsonContent);

            await ThrowIfFailedAsync(response, "Text-to-speech");
            byte[] mp3Data = await response.Content.ReadAsByteArrayAsync();

            Log($"[TTS] ✅ Generated {mp3Data.Length / 1024}KB MP3");

            // Save MP3 temporarily
            string tempMp3 = $"temp_tts_{Guid.NewGuid()}.mp3";
            string tempPcm = $"temp_tts_{Guid.NewGuid()}.pcm";

            try
            {
                await File.WriteAllBytesAsync(tempMp3, mp3Data);

                // Convert MP3 to raw PCM16 mono at Config.SampleRate using FFmpeg
                var processInfo = new ProcessStartInfo
                {
                    FileName = "ffmpeg",
                    Arguments = $"-hide_banner -loglevel error -nostats -i {tempMp3} -f s16le -ar {Config.SampleRate} -ac 1 -y {tempPcm}",
                    RedirectStandardOutput = false,
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true
                };

                using (var process = Process.Start(processInfo))
                {
                    if (process == null)
                    {
                        Log($"[TTS] ❌ FFmpeg not available");
                        return null;
                    }

                    // Read stderr while ffmpeg runs, otherwise a full pipe can deadlock WaitForExitAsync
                    var errorTask = process.StandardError.ReadToEndAsync();
                    await process.WaitForExitAsync();
                    string error = await errorTask;

                    if (process.ExitCode != 0)
                    {
                        Log($"[TTS] ❌ FFmpeg error: {error}");
                        return null;
                    }
                }

                // Read converted PCM
                byte[] pcmData = await File.ReadAllBytesAsync(tempPcm);
                Log($"[TTS] ✅ Converted to {Config.SampleRate} Hz PCM: {pcmData.Length / 1024}KB");

                return pcmData;
            }
            finally
            {
                // Clean up temp files
                try { File.Delete(tempMp3); } catch { }
                try { File.Delete(tempPcm); } catch { }
            }
        }
        catch (Exception ex)
        {
            Log($"[TTS] ❌ Error: {ex.Message}");
            return null;
        }
    }

    private void Log(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {msg}");
}

// ═══════════════════════════════════════════════════════════════
//  AUDIO PROCESSOR (Handles ESP32 audio stream)
// ═══════════════════════════════════════════════════════════════
class AudioProcessor : IDisposable
{
    private readonly VoiceActivityDetector _vad = new();
    private DateTime _ignoreAudioUntil = DateTime.MinValue;
    private readonly OpenAIClient _openAI;
    private readonly WebSocket _esp32Socket;

    private readonly Channel<byte[]> _audioQueue = Channel.CreateBounded<byte[]>(
        new BoundedChannelOptions(500)
        {
            FullMode = BoundedChannelFullMode.DropOldest
        });

    // .NET WebSocket allows only ONE outstanding SendAsync per socket; every sender goes through this lock
    private readonly SemaphoreSlim _sendLock = new(1, 1);

    private string? _currentWavFile = null;
    private BinaryWriter? _wavWriter = null;
    private FileStream? _wavFile = null;
    private long _dataChunkSizeOffset = 0;

    public AudioProcessor(OpenAIClient openAI, WebSocket esp32Socket)
    {
        _openAI = openAI;
        _esp32Socket = esp32Socket;

        _vad.OnSpeechStarted += OnSpeechStarted;
        _vad.OnSpeechEnded += OnSpeechEnded;

        _ = Task.Run(AudioWriterLoop);
    }

    public void ProcessAudioChunk(byte[] pcmBytes)
    {
        // The robot is beeping (or about to): what its mic hears now is not the user
        if (DateTime.Now < _ignoreAudioUntil)
            return;

        // Feed to VAD
        _vad.ProcessChunk(pcmBytes);

        // Queue for WAV writing
        _audioQueue.Writer.TryWrite(pcmBytes);
    }

    private void OnSpeechStarted()
    {
        _currentWavFile = $"recording_{DateTime.Now:yyyyMMdd_HHmmss}.wav";

        _wavWriter?.Dispose();
        _wavFile?.Dispose();

        _wavFile = new FileStream(_currentWavFile, FileMode.Create, FileAccess.ReadWrite);
        _wavWriter = new BinaryWriter(_wavFile);
        _dataChunkSizeOffset = WriteWavHeader(_wavWriter);
    }

    private void OnSpeechEnded()
    {
        if (_currentWavFile != null && _wavWriter != null && _wavFile != null)
        {
            _wavWriter.Flush();
            FinalizeWav(_wavWriter, _wavFile, _dataChunkSizeOffset);

            _wavWriter.Dispose();
            _wavFile.Dispose();
            _wavWriter = null;
            _wavFile = null;

            string wavPath = _currentWavFile;
            _currentWavFile = null;

            // ✅ SEND SPEECH-ENDED BEEP SIGNAL TO ESP32
            _ = Task.Run(async () => await SendSpeechEndedBeepSignalAsync());

            // Process transcription and response
            _ = Task.Run(async () => await ProcessTranscriptionAsync(wavPath));
        }
    }

    private async Task ProcessTranscriptionAsync(string wavPath)
    {
        bool robotMuted = false;   // true once we sent wake_beep (the ESP32 mutes its mic on it)
        bool answerSent = false;   // true once TTS audio was streamed (the ESP32 un-mutes by itself)
        try
        {
            // Transcribe
            string? transcription;
            try
            {
                transcription = await _openAI.TranscribeAsync(wavPath);
            }
            finally
            {
                if (!Config.KeepRecordings) { try { File.Delete(wavPath); } catch { } }
            }
            if (string.IsNullOrEmpty(transcription))
                return;

            // Save transcription
            Directory.CreateDirectory(Config.TranscriptionsFolder);
            string txtFile = Path.Combine(Config.TranscriptionsFolder,
                Path.GetFileNameWithoutExtension(wavPath) + ".txt");
            await File.WriteAllTextAsync(txtFile, transcription, Encoding.UTF8);

            // Check for wake words
            bool wakeWordDetected = false;
            string? detectedWakeWord = null;

            if (Config.EnableWakeWordDetection)
            {
                string transcriptionLower = transcription.ToLowerInvariant();
                foreach (string wakeWord in Config.WakeWords)
                {
                    if (transcriptionLower.Contains(wakeWord.ToLowerInvariant()))
                    {
                        wakeWordDetected = true;
                        detectedWakeWord = wakeWord;
                        break;
                    }
                }
            }

            if (!wakeWordDetected)
            {
                Log($"[WAKE] No wake word detected, ignoring");
                return;
            }

            Log($"[WAKE] 🚨 Wake word detected: {detectedWakeWord}");

            // ✅ SEND WAKE WORD BEEP SIGNAL TO ESP32 (it mutes its mic until the answer has played)
            await SendWakeWordBeepSignalAsync();
            robotMuted = true;

            // Remove wake word from message
            string userMessage = transcription;
            foreach (string word in Config.WakeWords)
            {
                int index = userMessage.IndexOf(word, StringComparison.OrdinalIgnoreCase);
                if (index >= 0)
                {
                    userMessage = userMessage.Remove(index, word.Length).Trim();
                    break;
                }
            }

            if (string.IsNullOrWhiteSpace(userMessage))
            {
                Log($"[WAKE] No command after wake word");
                return;
            }

            // Get ChatGPT response
            string? response = await _openAI.GetChatResponseAsync(userMessage);
            if (string.IsNullOrEmpty(response))
                return;

            // Generate speech
            byte[]? audioData = await _openAI.GenerateSpeechAsync(response, Config.TtsVoice);
            if (audioData == null)
                return;

            // Send to ESP32
            answerSent = true;
            await SendAudioToEsp32Async(audioData);
        }
        catch (Exception ex)
        {
            Log($"[PROCESS] ❌ Error: {ex.Message}");
        }
        finally
        {
            // Wake word heard but nothing to play (empty command, API error...): un-mute the robot
            if (robotMuted && !answerSent)
                await SendListenSignalAsync();
        }
    }

    // ✅ NEW: Send wake word beep signal to ESP32
    private async Task SendWakeWordBeepSignalAsync()
    {
        try
        {
            var message = new
            {
                target = "esp32",
                type = "wake_beep"
            };

            _ignoreAudioUntil = DateTime.Now.AddMilliseconds(Config.IgnoreAudioAfterBeepMs);
            await SendJsonAsync(message);

            Log($"[WAKE] 🔔 Sent beep signal to ESP32");
        }
        catch (Exception ex)
        {
            Log($"[WAKE] ❌ Failed to send beep signal: {ex.Message}");
        }
    }

    // ✅ NEW: Send speech-ended beep signal to ESP32
    private async Task SendSpeechEndedBeepSignalAsync()
    {
        try
        {
            var message = new
            {
                target = "esp32",
                type = "speech_ended"
            };

            _ignoreAudioUntil = DateTime.Now.AddMilliseconds(Config.IgnoreAudioAfterBeepMs);
            await SendJsonAsync(message);

            Log($"[VAD] 🔔 Sent speech-ended signal to ESP32");
        }
        catch (Exception ex)
        {
            Log($"[VAD] ❌ Failed to send speech-ended signal: {ex.Message}");
        }
    }

    private async Task SendAudioToEsp32Async(byte[] pcmData)
    {
        try
        {
            const int chunkSize = 2048;
            int totalChunks = (int)Math.Ceiling(pcmData.Length / (double)chunkSize);

            Log($"[SEND] 🚀 Sending {pcmData.Length / 1024}KB audio in {totalChunks} chunks");

            for (int i = 0; i < totalChunks; i++)
            {
                int offset = i * chunkSize;
                int length = Math.Min(chunkSize, pcmData.Length - offset);

                byte[] chunk = new byte[length];
                Array.Copy(pcmData, offset, chunk, 0, length);

                string base64Chunk = Convert.ToBase64String(chunk);

                var message = new
                {
                    target = "esp32",
                    type = "tts",
                    chunk = i,
                    total = totalChunks,
                    data = base64Chunk
                };

                await SendJsonAsync(message);

                await Task.Delay(15);
            }

            Log($"[SEND] ✅ Audio transmission complete");
        }
        catch (Exception ex)
        {
            Log($"[SEND] ❌ Error: {ex.Message}");
        }
    }

    private async Task SendJsonAsync(object message)
    {
        byte[] jsonBytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(message));
        await _sendLock.WaitAsync();
        try
        {
            await _esp32Socket.SendAsync(
                new ArraySegment<byte>(jsonBytes),
                WebSocketMessageType.Text,
                true,
                CancellationToken.None);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    // Tell the ESP32 to un-mute its microphone (sent when a wake word was heard but no answer follows)
    private async Task SendListenSignalAsync()
    {
        try
        {
            await SendJsonAsync(new { target = "esp32", type = "listen" });
            Log($"[MIC] 🎙️  Sent listen signal to ESP32");
        }
        catch (Exception ex)
        {
            Log($"[MIC] ❌ Failed to send listen signal: {ex.Message}");
        }
    }

    private async Task AudioWriterLoop()
    {
        try
        {
            await foreach (var pcmBytes in _audioQueue.Reader.ReadAllAsync())
            {
                if (_currentWavFile != null && _wavWriter != null)
                {
                    _wavWriter.Write(pcmBytes);
                }
            }
        }
        catch { }
        finally
        {
            _wavWriter?.Dispose();
            _wavFile?.Dispose();
        }
    }

    // Called when the ESP32 disconnects or registers again: stop the writer task, close any open WAV
    public void Dispose()
    {
        _vad.OnSpeechStarted -= OnSpeechStarted;
        _vad.OnSpeechEnded -= OnSpeechEnded;
        _audioQueue.Writer.TryComplete();
        try { _wavWriter?.Dispose(); } catch { }
        try { _wavFile?.Dispose(); } catch { }
        _wavWriter = null;
        _wavFile = null;
    }

    private long WriteWavHeader(BinaryWriter w)
    {
        int byteRate = Config.SampleRate * Config.Channels * (Config.BitsPerSample / 8);
        int blockAlign = Config.Channels * (Config.BitsPerSample / 8);

        w.Write(Encoding.ASCII.GetBytes("RIFF"));
        w.Write(0);
        w.Write(Encoding.ASCII.GetBytes("WAVE"));
        w.Write(Encoding.ASCII.GetBytes("fmt "));
        w.Write(16);
        w.Write((short)1);
        w.Write((short)Config.Channels);
        w.Write(Config.SampleRate);
        w.Write(byteRate);
        w.Write((short)blockAlign);
        w.Write((short)Config.BitsPerSample);
        w.Write(Encoding.ASCII.GetBytes("data"));
        long offset = w.BaseStream.Position;
        w.Write(0);
        return offset;
    }

    private void FinalizeWav(BinaryWriter w, FileStream f, long dataChunkSizeOffset)
    {
        long dataSize = f.Length - 44;
        f.Seek(dataChunkSizeOffset, SeekOrigin.Begin);
        w.Write((int)dataSize);
        f.Seek(4, SeekOrigin.Begin);
        w.Write((int)(dataSize + 36));
        w.Flush();
    }

    private void Log(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {msg}");
}

// ═══════════════════════════════════════════════════════════════
//  MAIN SERVER
// ═══════════════════════════════════════════════════════════════
class Program
{
    private static OpenAIClient? _openAI;
    private static WebSocket? _activeEsp32;   // only one robot at a time; a new registration replaces the old socket

    static bool FfmpegAvailable()
    {
        try
        {
            using var p = Process.Start(new ProcessStartInfo("ffmpeg", "-version")
            {
                RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true
            });
            if (p == null) return false;
            p.StandardOutput.ReadToEnd();
            p.WaitForExit(5000);
            return p.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    static async Task Main()
    {
        Console.OutputEncoding = Encoding.UTF8;

        Console.WriteLine("╔════════════════════════════════════════════════════════════╗");
        Console.WriteLine("║   CARDBOARD ROBOT - VOICE ASSISTANT SERVER               ║");
        Console.WriteLine("║   ESP32 mic -> Whisper -> ChatGPT -> TTS -> ESP32 speaker ║");
        Console.WriteLine("╚════════════════════════════════════════════════════════════╝");
        Console.WriteLine();

        if (string.IsNullOrWhiteSpace(Config.OpenAiApiKey) || Config.OpenAiApiKey == "sk-REPLACE-ME" || !Config.OpenAiApiKey.StartsWith("sk-"))
        {
            Console.WriteLine("❌ OPENAI_API_KEY is missing or still a placeholder. This server uses YOUR OWN OpenAI key.");
            Console.WriteLine("   Create one at https://platform.openai.com/api-keys (calls are billed to your OpenAI account), then:");
            Console.WriteLine("   Linux/macOS:  export OPENAI_API_KEY=sk-...   Windows: $env:OPENAI_API_KEY=\"sk-...\"");
            Environment.Exit(1);
        }
        if (string.IsNullOrEmpty(Config.AuthToken))
            Console.WriteLine("⚠️  ROBOT_AUTH_TOKEN is not set: any client that can reach this port may use your OpenAI credits.");
        if (!FfmpegAvailable())
        {
            Console.WriteLine("❌ ffmpeg was not found on the PATH. Install it (apt install ffmpeg / winget install ffmpeg) and start again.");
            Environment.Exit(1);
        }

        _openAI = new OpenAIClient(Config.OpenAiApiKey);

        Console.WriteLine("✅ OpenAI API initialized (key from OPENAI_API_KEY)");
        Console.WriteLine($"✅ Wake words: {string.Join(", ", Config.WakeWords)}");
        Console.WriteLine($"✅ TTS voice: {Config.TtsVoice}");
        Console.WriteLine($"✅ Sample rate: {Config.SampleRate} Hz");
        Console.WriteLine($"✅ VAD: Dynamic (background + {Config.VadNoiseOffset} offset, {Config.VadCalibrationSeconds}s calibration)");
        Console.WriteLine($"✅ Language: {Config.SttLanguage}");
        Console.WriteLine();

        // Start HTTP listener
        var listener = new HttpListener();
        listener.Prefixes.Add($"http://+:{Config.ServerPort}/");
        listener.Start();

        Console.WriteLine($"🚀 Server listening on port {Config.ServerPort}");
        Console.WriteLine("📡 Waiting for ESP32 to connect...");
        Console.WriteLine();

        while (true)
        {
            var context = await listener.GetContextAsync();

            if (!context.Request.IsWebSocketRequest)
            {
                context.Response.StatusCode = 400;
                context.Response.Close();
                continue;
            }

            var wsContext = await context.AcceptWebSocketAsync(null);
            var ws = wsContext.WebSocket;
            string remote = context.Request.RemoteEndPoint?.ToString() ?? "?";

            Log($"[CONNECT] New WebSocket from {remote}");
            _ = HandleClientAsync(ws, remote);
        }
    }

    static async Task HandleClientAsync(WebSocket ws, string remote)
    {
        byte[] buffer = new byte[256 * 1024];
        string clientId = "";
        AudioProcessor? audioProcessor = null;

        try
        {
            while (ws.State == WebSocketState.Open)
            {
                using var messageStream = new MemoryStream();
                WebSocketReceiveResult result;

                do
                {
                    result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer), CancellationToken.None);
                    if (result.MessageType == WebSocketMessageType.Close)
                        goto disconnected;

                    messageStream.Write(buffer, 0, result.Count);
                    if (messageStream.Length > 64 * 1024)   // a real mic frame is ~4 KB
                    {
                        Log($"[ERROR] {clientId}: message too big ({messageStream.Length} bytes), closing");
                        await ws.CloseAsync(WebSocketCloseStatus.MessageTooBig, "too big", CancellationToken.None);
                        goto disconnected;
                    }
                } while (!result.EndOfMessage);

                byte[] fullMessage = messageStream.ToArray();
                string msg = Encoding.UTF8.GetString(fullMessage);

                JsonDocument? json = null;
                try
                {
                    json = JsonDocument.Parse(msg);
                }
                catch
                {
                    continue;
                }

                var root = json.RootElement;

                // Client registration
                if (root.TryGetProperty("id", out var idProp))
                {
                    clientId = idProp.GetString() ?? "";
                    Log($"[REGISTER] Client ID: {clientId} from {remote}");

                    if (!string.IsNullOrEmpty(Config.AuthToken))
                    {
                        string? token = root.TryGetProperty("token", out var tokenProp) ? tokenProp.GetString() : null;
                        if (token != Config.AuthToken)
                        {
                            Log($"[REGISTER] ❌ Rejected {remote}: bad or missing token");
                            await ws.CloseAsync(WebSocketCloseStatus.PolicyViolation, "bad token", CancellationToken.None);
                            return;
                        }
                    }

                    // If ESP32 connected, create audio processor (and drop any previous robot socket)
                    if (clientId == "esp32" && _openAI != null)
                    {
                        var previous = Interlocked.Exchange(ref _activeEsp32, ws);
                        if (previous != null && previous != ws)
                        {
                            Log("[ESP32] 🔁 New robot connection replaces the previous one");
                            try { previous.Abort(); } catch { }
                        }
                        audioProcessor?.Dispose();
                        audioProcessor = new AudioProcessor(_openAI, ws);
                        Log($"[ESP32] ✅ Audio processor initialized ({Config.SampleRate} Hz)");
                    }

                    continue;
                }

                // Audio data from ESP32
                if (clientId == "esp32" && root.TryGetProperty("data", out var dataProp))
                {
                    string? b64 = dataProp.GetString();
                    if (string.IsNullOrEmpty(b64))
                        continue;

                    byte[] pcmBytes;
                    try { pcmBytes = Convert.FromBase64String(b64); }
                    catch { continue; }

                    if (pcmBytes.Length % 2 != 0)
                        Array.Resize(ref pcmBytes, pcmBytes.Length - 1);

                    audioProcessor?.ProcessAudioChunk(pcmBytes);
                }
            }

        disconnected:;
        }
        catch (Exception ex)
        {
            Log($"[ERROR] {clientId}: {ex.Message}");
        }
        finally
        {
            Log($"[DISCONNECT] {clientId} from {remote}");
            audioProcessor?.Dispose();
            Interlocked.CompareExchange(ref _activeEsp32, null, ws);

            if (ws.State != WebSocketState.Closed)
            {
                try
                {
                    await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None);
                }
                catch { }
            }
        }
    }

    private static void Log(string msg) => Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {msg}");
}