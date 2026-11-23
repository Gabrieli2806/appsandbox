using System.Buffers;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Microsoft.UI.Dispatching;

namespace AppSandbox.Services;

/// <summary>
/// The native host (appsandbox_host.dll, src\app_win\host.h): everything that
/// touches VMs. The shell sends it JSON commands and receives JSON messages;
/// both are listed in AGENTS.md section 7.
/// </summary>
public static class HostBridge
{
    private const string Dll = "appsandbox_host.dll";

    public enum BeginResult { Run = 0, RunForVm = 1, Headless = 2, HandedOver = 3, ServiceRunning = 4 }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void MessageCallback(nint json);

    [DllImport(Dll)] private static extern int asb_host_begin();
    [DllImport(Dll)] private static extern int asb_host_run_headless();
    [DllImport(Dll)] private static extern int asb_host_start(nint shellWindow, MessageCallback onMessage);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int asb_host_command(string json);

    // Kept for the life of the process: the host calls it until it has shut down.
    private static MessageCallback? _callback;
    private static DispatcherQueue? _queue;

    /// <summary>One message from the host, already on the UI thread and outside the host's own call.</summary>
    public static event Action<JsonElement>? Message;

    public static bool Started { get; private set; }

    public static BeginResult Begin() => (BeginResult)asb_host_begin();

    public static int RunHeadless() => asb_host_run_headless();

    public static bool Start(nint shellWindow, DispatcherQueue queue)
    {
        _queue = queue;
#if DEBUG
        // Debug builds only: ASB_UI_DEMO names a file with one host message per
        // line. They are played to the pages instead of starting the host, so
        // the shell can be looked at without administrator rights or VMs.
        if (Environment.GetEnvironmentVariable("ASB_UI_DEMO") is { Length: > 0 } demo && File.Exists(demo))
        {
            _demo = true;
            Started = true;
            foreach (string line in File.ReadAllLines(demo))
            {
                if (line.Trim().Length == 0) continue;
                string text = line;
                queue.TryEnqueue(() =>
                {
                    using var document = JsonDocument.Parse(text);
                    Message?.Invoke(document.RootElement);
                });
            }
            return true;
        }
#endif
        _callback = OnMessage;
        Started = asb_host_start(shellWindow, _callback) != 0;
        return Started;
    }

    /// <summary>
    /// The host calls this from inside its own message handling, sometimes
    /// while it waits on a VM operation. Nothing is done here but copying the
    /// text: the shell reacts from its own queue.
    /// </summary>
    private static void OnMessage(nint json)
    {
        string? text = Marshal.PtrToStringUni(json);
        if (string.IsNullOrEmpty(text)) return;

        _queue?.TryEnqueue(() =>
        {
            try
            {
                using var document = JsonDocument.Parse(text);
                Message?.Invoke(document.RootElement);
            }
            catch (Exception ex)
            {
                UiSettings.Log("Host message: " + ex.Message + " :: " + (text.Length > 300 ? text[..300] : text));
            }
        });
    }

    /// <summary>
    /// Sends {"action": action, field: value, ...}. Values are strings,
    /// integers or booleans; the host reads them by name. Returns at once: the
    /// host handles the command from its message queue and answers with messages.
    /// </summary>
    public static void Send(string action, params (string Key, object? Value)[] fields)
    {
        if (!Started) return;

        var buffer = new ArrayBufferWriter<byte>();
        using (var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
        {
            writer.WriteStartObject();
            writer.WriteString("action", action);
            foreach (var (key, value) in fields)
            {
                switch (value)
                {
                    case null: break;
                    case string s: writer.WriteString(key, s); break;
                    case bool b: writer.WriteBoolean(key, b); break;
                    case int i: writer.WriteNumber(key, i); break;
                    case long l: writer.WriteNumber(key, l); break;
                    default: writer.WriteString(key, value.ToString()); break;
                }
            }
            writer.WriteEndObject();
        }

#if DEBUG
        if (_demo)
        {
            UiSettings.Log("demo send: " + Encoding.UTF8.GetString(buffer.WrittenSpan));
            return;
        }
#endif
        asb_host_command(Encoding.UTF8.GetString(buffer.WrittenSpan));
    }

#if DEBUG
    private static bool _demo;
#endif
}
