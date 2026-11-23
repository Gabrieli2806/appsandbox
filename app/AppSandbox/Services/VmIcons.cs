using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Storage.Streams;

namespace AppSandbox.Services;

/// <summary>
/// The pictures users give their VMs. The host sends one as a data URL when
/// asked (getVmIcon); it is decoded once per version and shared by the list,
/// the VM page and anything else that shows the VM.
/// </summary>
public static class VmIcons
{
    private static readonly Dictionary<string, (int Version, ImageSource? Image)> Cache = [];
    private static readonly HashSet<string> Asked = [];
    private static bool _listening;

    /// <summary>A VM's picture arrived or went away.</summary>
    public static event Action<string>? Changed;

    /// <summary>The picture for this version of the VM's icon, or null while it is on its way (or when there is none).</summary>
    public static ImageSource? Get(VmInfo vm)
    {
        if (!_listening)
        {
            _listening = true;
            AppState.Current.VmIcon += OnIcon;
        }

        if (vm.IconVersion == 0)
        {
            Cache.Remove(vm.Id);
            return null;
        }

        if (Cache.TryGetValue(vm.Id, out var known) && known.Version == vm.IconVersion) return known.Image;

        string request = vm.Id + ":" + vm.IconVersion;
        if (Asked.Add(request)) HostBridge.Send("getVmIcon", ("vmId", vm.Id));
        return known.Image;   // the previous picture, until the new one is here
    }

    private static async void OnIcon(string id, string data)
    {
        var vm = AppState.Current.Find(id);
        int version = vm?.IconVersion ?? 0;
        ImageSource? image = null;

        try
        {
            // data:image/png;base64,... or data:image/x-icon;base64,...
            int comma = data.IndexOf(',');
            if (data.StartsWith("data:image/", StringComparison.Ordinal) && comma > 0)
            {
                byte[] bytes = Convert.FromBase64String(data[(comma + 1)..]);
                using var stream = new InMemoryRandomAccessStream();
                using (var writer = new DataWriter(stream.GetOutputStreamAt(0)))
                {
                    writer.WriteBytes(bytes);
                    await writer.StoreAsync();
                    writer.DetachStream();
                }
                stream.Seek(0);
                var bitmap = new BitmapImage { DecodePixelWidth = 96 };
                await bitmap.SetSourceAsync(stream);
                image = bitmap;
            }
        }
        catch (Exception ex)
        {
            UiSettings.Log("VM icon: " + ex.Message);
        }

        Cache[id] = (version, image);
        Changed?.Invoke(id);
    }
}
