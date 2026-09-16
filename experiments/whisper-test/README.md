# whisper-test (February 2026)

The very first experiment of the project, one week before the server existed: a
68-line console program that posts a WAV file to OpenAI Whisper and prints the
transcription. The `TranscribeAsync` method in the server is a near-verbatim copy
of it. It already used the `OPENAI_API_KEY` environment variable.

The project was created with Visual Studio's default name "7" (hence `7.csproj`).
The `.csproj` references the `OpenAI` and `Newtonsoft.Json` packages from an
earlier text-to-speech test whose code did not survive; the current code uses
neither.

To run it: set `OPENAI_API_KEY`, edit the `filePath` line in `Program.cs` to point
at a 16 kHz mono WAV, then `dotnet run`. It is kept only as a record of where the
project started; the real code is in `../../server`.
