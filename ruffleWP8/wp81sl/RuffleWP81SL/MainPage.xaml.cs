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

// Bản riêng cho Silverlight 8.1 (wp81sl). Chọn file qua file picker hệ thống (đã gồm thẻ nhớ).
namespace RuffleWP80
{
    public partial class MainPage : PhoneApplicationPage
    {
        private readonly RufflePlayer80 player = new RufflePlayer80();
        private readonly DispatcherTimer statusTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        private AppSettings settings = AppSettings.Load();
        private bool providerSet;
        private bool playing;

        public MainPage()
        {
            InitializeComponent();
            statusTimer.Tick += OnStatusTick;
            statusTimer.Start();
            Touch.FrameReported += OnTouchFrame;
            // App bị treo (về Start, giữ Back, cuộc gọi...) -> dừng âm thanh; quay lại -> chạy tiếp (game KHÔNG nạp lại)
            PhoneApplicationService.Current.Deactivated += (s, e) => player.SetSuspended(true);
            PhoneApplicationService.Current.Closing += (s, e) => player.SetSuspended(true);
            PhoneApplicationService.Current.Activated += (s, e) => player.SetSuspended(false);
            versionText.Text = "Version " + AppVersion();
            ApplySettings();
#if WP81SL
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
        }

        // ---------------------------------------------------------------- mở file

        private async Task OpenSharedFile(string token)
        {
            try
            {
                IStorageFile copy = await SharedStorageAccessManager.CopySharedFileAsync(
                    ApplicationData.Current.LocalFolder, "shared.swf", NameCollisionOption.ReplaceExisting, token);
                using (Stream s = await copy.OpenStreamForReadAsync())
                    Play(ReadAll(s));
            }
            catch (Exception ex)
            {
                hintText.Text = "Cannot open file: " + ex.Message;
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
                using (Stream s = await args.Files[0].OpenStreamForReadAsync())
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
