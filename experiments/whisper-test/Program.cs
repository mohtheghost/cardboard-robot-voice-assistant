using System;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading.Tasks;

class Program
{
    static async Task Main()
    {
        Console.OutputEncoding = Encoding.UTF8;
        // Read the API key from environment variable
        string apiKey = Environment.GetEnvironmentVariable("OPENAI_API_KEY");
        if (string.IsNullOrEmpty(apiKey))
        {
            Console.WriteLine("API key not set in environment.");
            return;
        }

        // Path to your WAV file
        string filePath = @"C:\Users\moh the ghost\Desktop\7\bin\Debug\net9.0\audio.wav";
        if (!File.Exists(filePath))
        {
            Console.WriteLine("WAV file not found.");
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