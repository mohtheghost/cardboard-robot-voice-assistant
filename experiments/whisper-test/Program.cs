using System;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading.Tasks;

class Program
{
    static async Task Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;
        // Read YOUR OpenAI API key from the environment (never put it in the code)
        string apiKey = Environment.GetEnvironmentVariable("OPENAI_API_KEY");
        if (string.IsNullOrEmpty(apiKey))
        {
            Console.WriteLine("OPENAI_API_KEY is not set. Create your own key at https://platform.openai.com/api-keys and run:");
            Console.WriteLine("  Windows PowerShell:  $env:OPENAI_API_KEY = \"sk-...\"      Linux/macOS:  export OPENAI_API_KEY=sk-...");
            return;
        }

        // Path to the WAV file: first command-line argument, or audio.wav in the current folder
        string filePath = args.Length > 0 ? args[0] : "audio.wav";
        if (!File.Exists(filePath))
        {
            Console.WriteLine($"WAV file not found: {filePath}");
            Console.WriteLine("Usage: dotnet run -- path\\to\\recording.wav   (16 kHz mono WAV works best)");
            return;
        }

        byte[] audioBytes = File.ReadAllBytes(filePath);

        using (var client = new HttpClient())
        {
            client.DefaultRequestHeaders.Authorization =
                new AuthenticationHeaderValue("Bearer", apiKey);

            using (var content = new MultipartFormDataContent())
            {
                // Add required fields
                content.Add(new StringContent("whisper-1"), "model");

                // Use plain text response to avoid JSON parsing
                content.Add(new StringContent("text"), "response_format");

                // Add audio file content
                var audioContent = new ByteArrayContent(audioBytes);
                audioContent.Headers.ContentType = MediaTypeHeaderValue.Parse("audio/wav");
                content.Add(audioContent, "file", Path.GetFileName(filePath));

                try
                {
                    // Send request
                    HttpResponseMessage response =
                        await client.PostAsync("https://api.openai.com/v1/audio/transcriptions", content);

                    response.EnsureSuccessStatusCode(); // Throws if not 2xx

                    // Read transcription as plain text
                    string transcription = await response.Content.ReadAsStringAsync();
                    Console.WriteLine("Transcription: " + transcription);
                }
                catch (HttpRequestException e)
                {
                    Console.WriteLine("Request error: " + e.Message);
                }
            }
        }
    }
}