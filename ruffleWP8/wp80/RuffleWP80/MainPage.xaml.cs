using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Navigation;
using System.Windows.Threading;
using System.Xml.Linq;
using Microsoft.Phone.Controls;
using Microsoft.Phone.Shell;
using Microsoft.Phone.Storage;
using Windows.Phone.Storage.SharedAccess;
using Windows.Storage;
using RuffleWP80Native;

// Dùng chung cho Silverlight 8.0 (wp80) và 8.1 (wp81sl liên kết file này). Phần chỉ có trên 8.1: #if WP81SL.
namespace RuffleWP80
{
    // Một nút trong cây chọn file: thư mục (có Children) hoặc file .swf (có Open)
    public class TreeNode
    {
        public string Name { get; set; }
        public int Depth { get; set; }
        public bool IsFolder { get; set; }
        public bool Expanded { get; set; }
        public int SwfCount { get; set; }
        public string Note { get; set; }
        public List<TreeNode> Children = new List<TreeNode>();
        public Func<Task<Stream>> Open;

        public Thickness Indent { get { return new Thickness(Depth * 28, 6, 0, 6); } }
        public string Glyph { get { return IsFolder ? (Expanded ? "−" : "+") : "▶"; } }
        public string Detail
        {
            get
            {
                if (Note != null) return Note;
                return IsFolder ? SwfCount + " SWF file(s)" : "";
            }
        }
    }

    public partial class MainPage : PhoneApplicationPage
    {
        private readonly RufflePlayer80 player = new RufflePlayer80();
        private readonly ObservableCollection<TreeNode> treeItems = new ObservableCollection<TreeNode>();
        private readonly List<TreeNode> roots = new List<TreeNode>();
        private const string ReceivedFolder = "Received"; // LocalFolder\Received: .swf mở từ email/trình duyệt/picker
        private readonly DispatcherTimer statusTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        private AppSettings settings = AppSettings.Load();
        private bool providerSet;
        private bool scanned;
        private bool playing;

        public MainPage()
        {
            InitializeComponent();
            treeList.ItemsSource = treeItems;
            statusTimer.Tick += OnStatusTick;
            statusTimer.Start();
            Touch.FrameReported += OnTouchFrame;
            // App bị treo (về Start, giữ Back, cuộc gọi...) -> dừng âm thanh; quay lại -> chạy tiếp (game KHÔNG nạp lại)
            PhoneApplicationService.Current.Deactivated += (s, e) => player.SetSuspended(true);
            PhoneApplicationService.Current.Closing += (s, e) => player.SetSuspended(true);
            PhoneApplicationService.Current.Activated += async (s, e) =>
            {
                player.SetSuspended(false);
                // Socket server chết khi app bị treo -> mở lại nếu màn Wi-Fi đang hiện
                if (wifiPanel.Visibility == Visibility.Visible && wifi != null) { wifi.Stop(); await StartWifi(); }
            };
            versionText.Text = "Version " + AppVersion();
            ApplySettings();
#if WP81SL
            pickButton.Visibility = Visibility.Visible;
            // PickSingleFileAndContinue: app bị tạm dừng, chọn xong hệ thống kích hoạt lại qua ContractActivated
            PhoneApplicationService.Current.ContractActivated += OnContractActivated;
#endif
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            if (!providerSet)
            {
                DrawingSurfaceBackground.SetBackgroundContentProvider(player.CreateContentProvider());
                providerSet = true;
            }

            // Mở từ file .swf (email, trình duyệt, thẻ nhớ): /MainPage.xaml?fileToken=...
            string token;
            if (NavigationContext.QueryString.TryGetValue("fileToken", out token))
            {
                NavigationContext.QueryString.Remove("fileToken");
                await OpenSharedFile(token);
            }
            else if (!scanned && !playing)
            {
                await OpenFilePanel();
            }
        }

        // ---------------------------------------------------------------- mở file

        private async Task OpenSharedFile(string token)
        {
            try
            {
                // Giữ lại bản sao trong LocalFolder\Received với tên gốc -> lần sau chọn lại được trong cây
                string name = SafeName(SharedStorageAccessManager.GetSharedFileName(token));
                StorageFolder dir = await ApplicationData.Current.LocalFolder.CreateFolderAsync(
                    ReceivedFolder, CreationCollisionOption.OpenIfExists);
                IStorageFile copy = await SharedStorageAccessManager.CopySharedFileAsync(
                    dir, name, NameCollisionOption.ReplaceExisting, token);
                scanned = false; // cây cần quét lại để thấy file mới
                using (Stream s = await copy.OpenStreamForReadAsync())
                    Play(ReadAll(s));
            }
            catch (Exception ex)
            {
                hintText.Text = "Cannot open file: " + ex.Message;
            }
        }

        private static Task<StorageFolder> ReceivedDir()
        {
            return ApplicationData.Current.LocalFolder.CreateFolderAsync(ReceivedFolder, CreationCollisionOption.OpenIfExists).AsTask();
        }

        private static bool LooksLikeSwf(byte[] d)
        {
            return d.Length >= 8 && d[1] == 'W' && d[2] == 'S' && (d[0] == 'F' || d[0] == 'C' || d[0] == 'Z');
        }

        // ---------------------------------------------------------------- Open URL

        private void OnUrlClick(object sender, RoutedEventArgs e)
        {
            urlStatus.Text = "";
            urlPanel.Visibility = Visibility.Visible;
            urlBox.Focus();
        }

        private void CloseUrlPanel() { urlPanel.Visibility = Visibility.Collapsed; }
        private void OnUrlClose(object sender, RoutedEventArgs e) { CloseUrlPanel(); }

        private async void OnUrlGo(object sender, RoutedEventArgs e)
        {
            string text = (urlBox.Text ?? "").Trim();
            if (text.Length == 0) return;
            if (!text.Contains("://")) text = "http://" + text;
            Uri uri;
            if (!Uri.TryCreate(text, UriKind.Absolute, out uri)) { urlStatus.Text = "Invalid URL."; return; }

            urlGoButton.IsEnabled = false;
            urlStatus.Text = "Downloading...";
            try
            {
                var req = (System.Net.HttpWebRequest)System.Net.WebRequest.Create(uri);
                req.AllowReadStreamBuffering = false; // đọc dần để báo % (mặc định WP tải hết rồi mới trả)
                byte[] data;
                Uri finalUri;
                urlProgress.Value = 0;
                urlProgress.IsIndeterminate = true;
                urlProgress.Visibility = Visibility.Visible;
                using (var resp = (System.Net.HttpWebResponse)await Task.Factory.FromAsync<System.Net.WebResponse>(
                    req.BeginGetResponse, req.EndGetResponse, null))
                {
                    long total = resp.ContentLength;
                    urlProgress.IsIndeterminate = total <= 0;
                    // Stream không buffer không được đọc trên luồng UI -> đọc ở luồng nền, báo % qua Dispatcher
                    data = await Task.Run(() =>
                    {
                        using (Stream s = resp.GetResponseStream())
                        using (var ms = new MemoryStream())
                        {
                            var buf = new byte[65536];
                            int n;
                            DateTime last = DateTime.MinValue;
                            while ((n = s.Read(buf, 0, buf.Length)) > 0)
                            {
                                ms.Write(buf, 0, n);
                                if ((DateTime.UtcNow - last).TotalMilliseconds < 150) continue;
                                last = DateTime.UtcNow;
                                long got = ms.Length;
                                Dispatcher.BeginInvoke(() =>
                                {
                                    if (total > 0)
                                    {
                                        double pct = Math.Min(100, got * 100.0 / total);
                                        urlProgress.Value = pct;
                                        urlStatus.Text = "Downloading... " + (int)pct + "% (" + Size(got) + " / " + Size(total) + ")";
                                    }
                                    else urlStatus.Text = "Downloading... " + Size(got);
                                });
                            }
                            return ms.ToArray();
                        }
                    });
                    finalUri = resp.ResponseUri ?? uri;
                }
                urlProgress.IsIndeterminate = false;
                urlProgress.Value = 100;
                if (!LooksLikeSwf(data))
                {
                    urlStatus.Text = "That link is not a .swf file (got " + data.Length + " bytes of something else, maybe a web page).";
                    return;
                }
                string name = SafeName(Uri.UnescapeDataString(Path.GetFileName(finalUri.AbsolutePath)));
                StorageFolder dir = await ReceivedDir();
                StorageFile file = await dir.CreateFileAsync(name, CreationCollisionOption.ReplaceExisting);
                using (Stream fs = await file.OpenStreamForWriteAsync())
                    fs.Write(data, 0, data.Length);
                scanned = false;
                urlStatus.Text = "";
                CloseUrlPanel();
                Play(data);
            }
            catch (Exception ex)
            {
                urlStatus.Text = "Download failed: " + ex.Message;
            }
            finally
            {
                urlGoButton.IsEnabled = true;
                urlProgress.IsIndeterminate = false;
                urlProgress.Visibility = Visibility.Collapsed;
            }
        }

        private static string Size(long bytes)
        {
            if (bytes < 1024 * 1024) return (bytes / 1024.0).ToString("0.0") + " KB";
            return (bytes / 1048576.0).ToString("0.0") + " MB";
        }

        // ---------------------------------------------------------------- Nhận qua Wi-Fi

        private WifiReceiver wifi;
        private readonly ObservableCollection<string> wifiFiles = new ObservableCollection<string>();

        private async void OnWifiClick(object sender, RoutedEventArgs e)
        {
            wifiPanel.Visibility = Visibility.Visible;
            wifiList.ItemsSource = wifiFiles;
            await StartWifi();
        }

        private async Task StartWifi()
        {
            if (wifi == null)
            {
                wifi = new WifiReceiver(ReceivedDir);
                wifi.Progress += (name, got, total) => Dispatcher.BeginInvoke(() =>
                {
                    double pct = total > 0 ? Math.Min(100, got * 100.0 / total) : 0;
                    wifiProgress.Visibility = Visibility.Visible;
                    wifiProgress.Value = pct;
                    wifiStatus.Text = "Receiving " + name + "... " + (int)pct + "% (" + Size(got) + " / " + Size(total) + ")";
                });
                wifi.Received += name => Dispatcher.BeginInvoke(() =>
                {
                    wifiProgress.Visibility = Visibility.Collapsed;
                    scanned = false;
                    if (!wifiFiles.Contains(name)) wifiFiles.Insert(0, name);
                    wifiStatus.Text = "Received " + name + ". Tap a file to play it.";
                });
                wifi.Error += msg => Dispatcher.BeginInvoke(() =>
                {
                    wifiProgress.Visibility = Visibility.Collapsed;
                    wifiStatus.Text = "Transfer error: " + msg;
                });
            }
            List<string> addrs = WifiReceiver.Addresses();
            if (addrs.Count == 0)
            {
                wifiAddress.Text = "";
                wifiStatus.Text = "No network. Connect the phone to Wi-Fi, then open this screen again.";
                return;
            }
            try
            {
                await wifi.Start();
                wifiAddress.Text = string.Join("\n", addrs);
                wifiStatus.Text = "Waiting for files...";
                // Không cho màn hình tắt (app bị treo -> server dừng)
                PhoneApplicationService.Current.UserIdleDetectionMode = IdleDetectionMode.Disabled;
            }
            catch (Exception ex)
            {
                wifiStatus.Text = "Cannot start server on port " + WifiReceiver.Port + ": " + ex.Message;
            }
        }

        private void CloseWifiPanel()
        {
            wifiPanel.Visibility = Visibility.Collapsed;
            if (wifi != null) wifi.Stop();
            if (!playing) PhoneApplicationService.Current.UserIdleDetectionMode = IdleDetectionMode.Enabled;
        }

        private void OnWifiClose(object sender, RoutedEventArgs e) { CloseWifiPanel(); }

        private async void OnWifiFileSelected(object sender, SelectionChangedEventArgs e)
        {
            var name = wifiList.SelectedItem as string;
            if (name == null) return;
            wifiList.SelectedItem = null;
            try
            {
                StorageFile file = await (await ReceivedDir()).GetFileAsync(name);
                byte[] data;
                using (Stream s = await file.OpenStreamForReadAsync())
                    data = ReadAll(s);
                CloseWifiPanel();
                Play(data);
            }
            catch (Exception ex)
            {
                wifiStatus.Text = "Cannot open " + name + ": " + ex.Message;
            }
        }

        internal static string SafeName(string name)
        {
            if (string.IsNullOrEmpty(name)) name = "shared.swf";
            foreach (char c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
            if (!name.EndsWith(".swf", StringComparison.OrdinalIgnoreCase)) name += ".swf";
            return name;
        }

        // ---------------------------------------------------------------- popup cây thư mục

        private async Task OpenFilePanel()
        {
            filePanel.Visibility = Visibility.Visible;
            if (!scanned) await ScanAll();
        }

        private void CloseFilePanel()
        {
            filePanel.Visibility = Visibility.Collapsed;
        }

        private async void OnBrowseClick(object sender, RoutedEventArgs e) { await OpenFilePanel(); }
        private void OnFilePanelClose(object sender, RoutedEventArgs e) { CloseFilePanel(); }
        private async void OnRefreshClick(object sender, RoutedEventArgs e) { await ScanAll(); }

        // Quét mọi nguồn app đọc được thành các gốc của cây. Thư mục không chứa .swf bị lược bỏ.
        private async Task ScanAll()
        {
            scanned = true;
            refreshButton.IsEnabled = false;
            browseButton.IsEnabled = false;
            roots.Clear();
            treeItems.Clear();
            fileStatus.Text = "Scanning...";

            // 1) Thẻ nhớ (mọi thiết bị lưu trữ ngoài)
            try
            {
                int i = 0;
                foreach (ExternalStorageDevice sd in await ExternalStorage.GetExternalStorageDevicesAsync())
                {
                    i++;
                    roots.Add(await ScanExternal(sd.RootFolder, i == 1 ? "SD card" : "SD card " + i, 0));
                }
                if (i == 0) roots.Add(new TreeNode { Name = "SD card", IsFolder = true, Note = "No SD card inserted" });
            }
            catch (Exception ex)
            {
                roots.Add(new TreeNode { Name = "SD card", IsFolder = true, Note = "Error: " + ex.Message });
            }

            // 2) File đã mở từ email / trình duyệt / Downloads (bản sao trong bộ nhớ app)
            try
            {
                StorageFolder dir = await ApplicationData.Current.LocalFolder.CreateFolderAsync(
                    ReceivedFolder, CreationCollisionOption.OpenIfExists);
                roots.Add(await ScanLocal(dir, "Opened from email / browser", 0));
            }
            catch (Exception ex)
            {
                roots.Add(new TreeNode { Name = "Opened from email / browser", IsFolder = true, Note = "Error: " + ex.Message });
            }

            int total = 0;
            foreach (TreeNode r in roots)
            {
                total += r.SwfCount;
                r.Expanded = r.SwfCount > 0;
                if (r.Note == null && r.SwfCount == 0) r.Note = "No .swf files";
                // Chuỗi thư mục chỉ có đúng 1 thư mục con -> mở sẵn cho đỡ bấm
                TreeNode n = r;
                while (n.Children.Count == 1 && n.Children[0].IsFolder) { n = n.Children[0]; n.Expanded = true; }
            }
            fileStatus.Text = total == 0
                ? "No .swf files found. Copy .swf files to the SD card, or open one from email / the browser (a copy is kept here)."
                : total + " SWF file(s). Tap a folder to expand, tap a file to play.";
            RebuildTree();
            refreshButton.IsEnabled = true;
            browseButton.IsEnabled = true;
        }

        private async Task<TreeNode> ScanExternal(ExternalStorageFolder folder, string name, int depth)
        {
            var node = new TreeNode { Name = name, Depth = depth, IsFolder = true };
            if (depth < 8)
            {
                foreach (ExternalStorageFolder sub in await folder.GetFoldersAsync())
                {
                    TreeNode child = await ScanExternal(sub, sub.Name, depth + 1);
                    if (child.SwfCount > 0) { node.Children.Add(child); node.SwfCount += child.SwfCount; }
                }
            }
            // GetFilesAsync chỉ trả các loại file app đã đăng ký (.swf trong WMAppManifest)
            foreach (ExternalStorageFile f in await folder.GetFilesAsync())
            {
                ExternalStorageFile file = f;
                node.Children.Add(new TreeNode { Name = f.Name, Depth = depth + 1, Open = () => file.OpenForReadAsync() });
                node.SwfCount++;
            }
            return node;
        }

        private async Task<TreeNode> ScanLocal(StorageFolder folder, string name, int depth)
        {
            var node = new TreeNode { Name = name, Depth = depth, IsFolder = true };
            if (depth < 8)
            {
                foreach (StorageFolder sub in await folder.GetFoldersAsync())
                {
                    TreeNode child = await ScanLocal(sub, sub.Name, depth + 1);
                    if (child.SwfCount > 0) { node.Children.Add(child); node.SwfCount += child.SwfCount; }
                }
            }
            foreach (StorageFile f in await folder.GetFilesAsync())
            {
                if (!f.Name.EndsWith(".swf", StringComparison.OrdinalIgnoreCase)) continue;
                StorageFile file = f;
                node.Children.Add(new TreeNode { Name = f.Name, Depth = depth + 1, Open = () => file.OpenStreamForReadAsync() });
                node.SwfCount++;
            }
            return node;
        }

        private void RebuildTree()
        {
            treeItems.Clear();
            foreach (TreeNode r in roots) AddVisible(r);
        }

        private void AddVisible(TreeNode n)
        {
            treeItems.Add(n);
            if (n.IsFolder && n.Expanded)
                foreach (TreeNode c in n.Children) AddVisible(c);
        }

        private async void OnTreeSelected(object sender, SelectionChangedEventArgs e)
        {
            var node = treeList.SelectedItem as TreeNode;
            if (node == null) return;
            treeList.SelectedItem = null;
            if (node.IsFolder)
            {
                if (node.Children.Count == 0) return;
                node.Expanded = !node.Expanded;
                RebuildTree();
                treeList.ScrollIntoView(node);
                return;
            }
            try
            {
                byte[] data;
                using (Stream s = await node.Open())
                    data = ReadAll(s);
                CloseFilePanel();
                Play(data);
            }
            catch (Exception ex)
            {
                fileStatus.Text = "Cannot read " + node.Name + ": " + ex.Message;
            }
        }

        private void OnPickClick(object sender, RoutedEventArgs e)
        {
#if WP81SL
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            picker.FileTypeFilter.Add(".swf");
            picker.PickSingleFileAndContinue();
#endif
        }

#if WP81SL
        private async void OnContractActivated(object sender, Windows.ApplicationModel.Activation.IActivatedEventArgs e)
        {
            var args = e as Windows.ApplicationModel.Activation.FileOpenPickerContinuationEventArgs;
            if (args == null || args.Files.Count == 0) return;
            try
            {
                // Giữ bản sao vào Received để lần sau chọn lại trong cây
                StorageFolder dir = await ApplicationData.Current.LocalFolder.CreateFolderAsync(
                    ReceivedFolder, CreationCollisionOption.OpenIfExists);
                StorageFile copy = await args.Files[0].CopyAsync(dir, SafeName(args.Files[0].Name), NameCollisionOption.ReplaceExisting);
                scanned = false;
                CloseFilePanel();
                using (Stream s = await copy.OpenStreamForReadAsync())
                    Play(ReadAll(s));
            }
            catch (Exception ex)
            {
                hintText.Text = "Cannot open " + args.Files[0].Name + ": " + ex.Message;
            }
        }
#endif

        private static byte[] ReadAll(Stream s)
        {
            using (var ms = new MemoryStream())
            {
                s.CopyTo(ms);
                return ms.ToArray();
            }
        }

        // ---------------------------------------------------------------- chơi / dừng

        private void Play(byte[] swf)
        {
            player.LoadSwf(swf);
            playing = true;
            zoom = 1; panX = 0; panY = 0; // native cũng đặt lại zoom khi nạp
            menuPanel.Visibility = Visibility.Collapsed;
            settingsPanel.Visibility = Visibility.Collapsed;
            ApplicationBar.IsVisible = true;
            fpsText.Visibility = settings.ShowFps ? Visibility.Visible : Visibility.Collapsed;
            PhoneApplicationService.Current.UserIdleDetectionMode = IdleDetectionMode.Disabled; // không tắt màn khi chơi
        }

        private void Stop()
        {
            player.Unload();
            playing = false;
            pinching = false;
            touches.Clear();
            menuPanel.Visibility = Visibility.Visible;
            ApplicationBar.IsVisible = false;
            fpsText.Visibility = Visibility.Collapsed;
            PhoneApplicationService.Current.UserIdleDetectionMode = IdleDetectionMode.Enabled;
        }

        private void OnStatusTick(object sender, EventArgs e)
        {
            if (!playing) return;
            if (!player.IsPlaying())
            {
                // Nạp lỗi (không phải SWF...) -> về danh sách, hiện thông báo
                Stop();
                string msg = player.Status();
                if (!string.IsNullOrEmpty(msg)) hintText.Text = msg;
                return;
            }
            if (settings.ShowFps) fpsText.Text = player.FpsText();
        }

        protected override void OnBackKeyPress(CancelEventArgs e)
        {
            if (settingsPanel.Visibility == Visibility.Visible)
            {
                CloseSettings();
                e.Cancel = true;
                return;
            }
            if (urlPanel.Visibility == Visibility.Visible)
            {
                CloseUrlPanel();
                e.Cancel = true;
                return;
            }
            if (wifiPanel.Visibility == Visibility.Visible)
            {
                CloseWifiPanel();
                e.Cancel = true;
                return;
            }
            if (filePanel.Visibility == Visibility.Visible)
            {
                CloseFilePanel();
                e.Cancel = true;
                return;
            }
            if (playing)
            {
                Stop();
                e.Cancel = true; // Back khi đang chơi -> về danh sách
                return;
            }
            base.OnBackKeyPress(e);
        }

        private void OnAppBarSettings(object sender, EventArgs e) { OpenSettings(); }
        private void OnAppBarResetZoom(object sender, EventArgs e) { zoom = 1; panX = 0; panY = 0; player.SetView(1, 0, 0); }
        private void OnAppBarBack(object sender, EventArgs e) { if (playing) Stop(); }

        // ---------------------------------------------------------------- Settings

        private static string AppVersion()
        {
            try
            {
                XElement app = XDocument.Load("WMAppManifest.xml").Root.Element("App");
                return app != null ? (string)app.Attribute("Version") : "?";
            }
            catch (Exception)
            {
                return "?";
            }
        }

        private void ApplySettings()
        {
            player.SetUseGpu(settings.Gpu);
            player.SetMsaa(settings.Msaa);
            player.SetAudioEnabled(settings.Sound);
            player.SetRenderScale(AppSettings.Scales[settings.ScaleIndex]);
            player.SetQuality(settings.Quality);
            player.SetFrameLimit(AppSettings.Limits[settings.LimitIndex]);
            fpsText.Visibility = playing && settings.ShowFps ? Visibility.Visible : Visibility.Collapsed;
        }

        private void SyncSettingsUi()
        {
            gpuCheck.IsChecked = settings.Gpu;
            msaaCheck.IsChecked = settings.Msaa;
            msaaCheck.IsEnabled = settings.Gpu;
            soundCheck.IsChecked = settings.Sound;
            fpsCheck.IsChecked = settings.ShowFps;
            scaleButton.Content = "Render resolution: " + AppSettings.ScaleNames[settings.ScaleIndex];
            qualityButton.Content = "Quality: " + AppSettings.QualityNames[settings.Quality];
            limitButton.Content = "Frame limit: " + AppSettings.LimitNames[settings.LimitIndex];
        }

        private void OpenSettings()
        {
            SyncSettingsUi();
            settingsPanel.Visibility = Visibility.Visible;
        }

        private void CloseSettings()
        {
            settingsPanel.Visibility = Visibility.Collapsed;
        }

        private void OnSettingsClick(object sender, RoutedEventArgs e) { OpenSettings(); }
        private void OnSettingsClose(object sender, RoutedEventArgs e) { CloseSettings(); }

        private void SettingsChanged()
        {
            settings.Save();
            ApplySettings();
            SyncSettingsUi();
        }

        private void OnSettingChanged(object sender, RoutedEventArgs e)
        {
            settings.Gpu = gpuCheck.IsChecked == true;
            settings.Msaa = msaaCheck.IsChecked == true;
            settings.Sound = soundCheck.IsChecked == true;
            settings.ShowFps = fpsCheck.IsChecked == true;
            SettingsChanged();
        }

        private void OnScaleClick(object sender, RoutedEventArgs e)
        {
            settings.ScaleIndex = (settings.ScaleIndex + 1) % AppSettings.Scales.Length;
            SettingsChanged();
        }

        private void OnQualityClick(object sender, RoutedEventArgs e)
        {
            settings.Quality = (settings.Quality + 1) % AppSettings.QualityNames.Length;
            SettingsChanged();
        }

        private void OnLimitClick(object sender, RoutedEventArgs e)
        {
            settings.LimitIndex = (settings.LimitIndex + 1) % AppSettings.Limits.Length;
            SettingsChanged();
        }

        // ---------------------------------------------------------------- hướng màn hình

        // Grid nền không tự xoay -> báo native để Ruffle vẽ ngang và GPU xoay khi present
        protected override void OnOrientationChanged(OrientationChangedEventArgs e)
        {
            base.OnOrientationChanged(e);
            int rotation = 0;
            if (e.Orientation == PageOrientation.LandscapeLeft) rotation = 1;
            else if (e.Orientation == PageOrientation.LandscapeRight) rotation = 2;
            player.SetOrientation(rotation);
        }

        private void OnSurfaceSizeChanged(object sender, SizeChangedEventArgs e)
        {
            player.SetViewSize((float)e.NewSize.Width, (float)e.NewSize.Height);
        }

        // ---------------------------------------------------------------- chạm: 1 ngón = chuột, 2 ngón = zoom/kéo

        private readonly Dictionary<int, Point> touches = new Dictionary<int, Point>(); // DIP theo bề mặt vẽ
        private bool pinching;
        private bool mouseDown;
        private Point lastMouse;              // chuẩn hoá
        private Point pinchMid;               // chuẩn hoá
        private double pinchDist;             // DIP
        private float zoom = 1, panX, panY;

        private void OnTouchFrame(object sender, TouchFrameEventArgs e)
        {
            if (!playing || settingsPanel.Visibility == Visibility.Visible) return;
            double w = DrawingSurfaceBackground.ActualWidth, h = DrawingSurfaceBackground.ActualHeight;
            if (w <= 0 || h <= 0) return;

            TouchPointCollection pts = e.GetTouchPoints(DrawingSurfaceBackground);
            foreach (TouchPoint tp in pts)
            {
                if (tp.Action == TouchAction.Up) touches.Remove(tp.TouchDevice.Id);
                else touches[tp.TouchDevice.Id] = tp.Position;
            }

            if (pinching || touches.Count >= 2)
            {
                if (!pinching)
                {
                    // Ngón thứ 2 chạm -> huỷ thao tác chuột của ngón đầu, bắt đầu zoom
                    if (mouseDown) { player.Pointer(2, (float)lastMouse.X, (float)lastMouse.Y); mouseDown = false; }
                    pinching = true;
                    StartPinch(w, h);
                }
                else if (touches.Count >= 2) UpdatePinch(w, h);
                else if (touches.Count == 0) pinching = false; // nhấc hết ngón -> hết zoom
                if (touches.Count == 1) StartPinch(w, h); // còn 1 ngón: chờ ngón 2 hoặc nhấc
                return;
            }

            TouchPoint p = e.GetPrimaryTouchPoint(DrawingSurfaceBackground);
            if (p == null) return;
            lastMouse = new Point(p.Position.X / w, p.Position.Y / h);
            int kind = p.Action == TouchAction.Down ? 1 : p.Action == TouchAction.Up ? 2 : 0;
            if (kind == 1) mouseDown = true;
            if (kind == 2) mouseDown = false;
            player.Pointer(kind, (float)lastMouse.X, (float)lastMouse.Y);
        }

        private void PinchGeometry(double w, double h, out Point mid, out double dist)
        {
            var two = touches.Values.Take(2).ToArray();
            if (two.Length < 2) { mid = two.Length == 1 ? new Point(two[0].X / w, two[0].Y / h) : new Point(0.5, 0.5); dist = 0; return; }
            mid = new Point((two[0].X + two[1].X) / 2 / w, (two[0].Y + two[1].Y) / 2 / h);
            double dx = two[0].X - two[1].X, dy = two[0].Y - two[1].Y;
            dist = Math.Sqrt(dx * dx + dy * dy);
        }

        private void StartPinch(double w, double h)
        {
            PinchGeometry(w, h, out pinchMid, out pinchDist);
        }

        private void UpdatePinch(double w, double h)
        {
            Point mid;
            double dist;
            PinchGeometry(w, h, out mid, out dist);
            if (pinchDist < 1 || dist < 1) { pinchMid = mid; pinchDist = dist; return; }
            // Điểm nội dung dưới tâm cũ giữ nguyên dưới tâm mới (zoom quanh tâm 2 ngón + kéo theo tâm)
            float newZoom = (float)Math.Max(1.0, Math.Min(4.0, zoom * dist / pinchDist));
            double cx = panX + pinchMid.X / zoom, cy = panY + pinchMid.Y / zoom;
            float maxPan = 1 - 1 / newZoom;
            panX = (float)Math.Max(0, Math.Min(maxPan, cx - mid.X / newZoom));
            panY = (float)Math.Max(0, Math.Min(maxPan, cy - mid.Y / newZoom));
            zoom = newZoom;
            player.SetView(zoom, panX, panY);
            pinchMid = mid;
            pinchDist = dist;
        }
    }
}
