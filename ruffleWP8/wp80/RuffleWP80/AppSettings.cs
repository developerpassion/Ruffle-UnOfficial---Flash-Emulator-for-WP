using System.IO.IsolatedStorage;

namespace RuffleWP80
{
    // Cài đặt lưu trong IsolatedStorageSettings (dùng chung Silverlight 8.0 / 8.1)
    public class AppSettings
    {
        public static readonly float[] Scales = { 0f, 1f, 0.75f, 0.5f };       // 0 = Auto
        public static readonly string[] ScaleNames = { "Auto", "100%", "75%", "50%" };
        public static readonly string[] QualityNames = { "Low", "Medium", "High", "Best" };
        public static readonly int[] Limits = { 0, 30, 20 };
        public static readonly string[] LimitNames = { "Unlimited", "30 FPS", "20 FPS" };

        public bool Gpu = true;
        public bool Msaa = false;
        public bool Sound = true;
        public bool ShowFps = false;
        public int ScaleIndex = 0;
        public int Quality = 2;
        public int LimitIndex = 0;

        public static AppSettings Load()
        {
            var s = new AppSettings();
            var store = IsolatedStorageSettings.ApplicationSettings;
            s.Gpu = Get(store, "gpu", s.Gpu);
            s.Msaa = Get(store, "msaa", s.Msaa);
            s.Sound = Get(store, "sound", s.Sound);
            s.ShowFps = Get(store, "showFps", s.ShowFps);
            s.ScaleIndex = Clamp(Get(store, "scale", s.ScaleIndex), Scales.Length);
            s.Quality = Clamp(Get(store, "quality", s.Quality), QualityNames.Length);
            s.LimitIndex = Clamp(Get(store, "limit", s.LimitIndex), Limits.Length);
            return s;
        }

        public void Save()
        {
            var store = IsolatedStorageSettings.ApplicationSettings;
            store["gpu"] = Gpu;
            store["msaa"] = Msaa;
            store["sound"] = Sound;
            store["showFps"] = ShowFps;
            store["scale"] = ScaleIndex;
            store["quality"] = Quality;
            store["limit"] = LimitIndex;
            store.Save();
        }

        private static T Get<T>(IsolatedStorageSettings store, string key, T def)
        {
            T v;
            return store.TryGetValue(key, out v) ? v : def;
        }

        private static int Clamp(int i, int n)
        {
            return i < 0 || i >= n ? 0 : i;
        }
    }
}
