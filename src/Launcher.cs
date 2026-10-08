// MineRing Launcher - WPF nativo, C# 5, compilado com o csc.exe que ja vem no Windows.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using Path = System.IO.Path;
using ShapePath = System.Windows.Shapes.Path;
using System.Windows.Threading;
using Microsoft.Win32;
using WinForms = System.Windows.Forms;

namespace EldenMinecraftLauncher
{
    enum St { Loading, Ok, Warn, Err }

    class Inst { public string Id; public string Name; public string Role; public string Dir; }

    class Det
    {
        public string SteamDir; public bool SteamRunning;
        public string GameDir; public string GameVer; public string GameProblem;
        public bool Seamless;
        public string PrismExe; public string PrismData; public string InstancesDir;
        public List<Inst> Insts = new List<Inst>();
        public string InstallDir; public bool UpToDate;
        public bool McFound; public string JavaWhere; public bool PrismAccount;
        public bool BridgeInstalled, CoopManifest;
    }

    class Row { public St State; public string Title; public string Detail; public string ActionText; public Action Action; public string ActionText2; public Action Action2; }

    static class Program
    {
        [STAThread]
        public static void Main(string[] args)
        {
            var app = new Application();
            app.ShutdownMode = ShutdownMode.OnMainWindowClose;
            var win = new MainWin(args);
            app.Run(win.Window);
        }
    }

    partial class MainWin
    {
        // Repositorio do projeto (unico lugar para trocar): o botao da barra de titulo abre esta pagina e pede uma estrela.
        const string RepoUrl = "https://github.com/franguchico/MineRing";

        public Window Window;
        readonly string[] args;
        bool dry, fakeMissing, busy;
        string shotPath, shotTab; bool autoPlay;
        readonly string baseDir, packDir, stateDir, logFile, cfgFile;
        // --sandbox <pasta>: todo estado, Prism, instancias, pack instalado e ambiente dos scripts ficam la (Steam/Elden = pasta falsa).
        readonly string sandbox, homeDir, appDataDir, localDir;
        string playersFile, setupTest; PlayersCfg players = new PlayersCfg();
        Dictionary<string, string> cfg = new Dictionary<string, string>();
        Det det = new Det();
        bool scanning = true;
        string role = "host"; string instId;
        readonly Dictionary<string, string> manual = new Dictionary<string, string>();
        int stepIndex = -1; string stepFail;
        DispatcherTimer monitor;
        // ---- camada visual/sonora ----
        string assetDir; FontFamily pixelFF;
        readonly Music music = new Music(); readonly Sfx sfx = new Sfx();
        double volume = 0.6; bool muted, lowFx, noIntro, noMusic;
        PixelScene introScene, bgScene; bool introActive, bgActive;
        readonly Stopwatch clock = Stopwatch.StartNew();
        double introT0, lastBg = -1, fpsT0; int fpsN; bool fpsLogged, musLog1, musLog2;
        string introShotsPrefix; readonly Queue<double> shotTimes = new Queue<double>(new[] { 0.4, 3.0, 6.0, 10.0 });
        int pxScale = 4; double skipAt;

        // cores
        static Brush B(string hex) { var b = (SolidColorBrush)new BrushConverter().ConvertFromString(hex); b.Freeze(); return b; }
        static readonly Brush cGold = B("#C8A24A"), cGoldB = B("#EBCB7A"), cText = B("#EDE5D2"), cMuted = B("#A39883"),
            cOk = B("#7BC043"), cWarn = B("#E3A234"), cErr = B("#E5654A"), cLine = B("#3A2F1C"), cOff = B("#5A5242");

        T F<T>(string n) where T : class { return (T)Window.FindName(n); }

        public MainWin(string[] a)
        {
            args = a;
            dry = a.Contains("--dry-run"); fakeMissing = a.Contains("--fake-missing"); autoPlay = a.Contains("--auto-play");
            for (int i = 0; i < a.Length - 1; i++) { if (a[i] == "--shot") shotPath = a[i + 1]; if (a[i] == "--tab") shotTab = a[i + 1]; if (a[i] == "--intro-shots") introShotsPrefix = a[i + 1]; }
            for (int i = 0; i < a.Length - 1; i++) if (a[i] == "--skip-intro-at") double.TryParse(a[i + 1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out skipAt);
            noIntro = a.Contains("--no-intro"); noMusic = a.Contains("--no-music"); if (a.Contains("--low-fx")) lowFx = true;
            baseDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
            packDir = Path.Combine(baseDir, "pack");
            for (int i = 0; i < a.Length - 1; i++) { if (a[i] == "--sandbox") sandbox = Path.GetFullPath(a[i + 1]); if (a[i] == "--offline-fixtures") fixturesDir = Path.GetFullPath(a[i + 1]); if (a[i] == "--install-shots") installShotsPrefix = a[i + 1]; if (a[i] == "--throttle-kbps") int.TryParse(a[i + 1], out throttleKbps); if (a[i] == "--simulate-games") simDir = Path.GetFullPath(a[i + 1]); if (a[i] == "--sim-stubborn") simStubborn = a[i + 1]; }
            // modo de teste: so vale com --sandbox e com a pasta dos jogos FALSOS dentro da sandbox (nunca toca jogo real)
            if (simDir != null && (sandbox == null || !simDir.StartsWith(sandbox.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))) simDir = null;
            if (sandbox != null)
            {
                homeDir = Path.Combine(sandbox, "userprofile"); appDataDir = Path.Combine(sandbox, "appdata"); localDir = Path.Combine(sandbox, "localappdata");
                stateDir = Path.Combine(sandbox, "state");
                foreach (var d0 in new[] { homeDir, appDataDir, localDir, Path.Combine(homeDir, "Downloads"), Path.Combine(homeDir, "Documents") }) try { Directory.CreateDirectory(d0); } catch { }
            }
            else
            {
                homeDir = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile); appDataDir = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
                localDir = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
                stateDir = Path.Combine(localDir, "EldenMinecraftLauncher");
            }
            try { Directory.CreateDirectory(stateDir); } catch { }
            logFile = Path.Combine(stateDir, "launcher.log"); cfgFile = Path.Combine(stateDir, fakeMissing ? "config.fake.txt" : "config.txt");
            LoadCfg();
            playersFile = Path.Combine(stateDir, "players.json");
            for (int i = 0; i < a.Length - 1; i++)
            {
                if (a[i] == "--players-file") playersFile = a[i + 1];
                if (a[i] == "--mojang-url") PlayersCfg.MojangBase = a[i + 1];
                if (a[i] == "--setup-test") setupTest = a[i + 1];
            }
            players = PlayersCfg.Load(playersFile);

            using (var s = typeof(MainWin).Assembly.GetManifestResourceStream("Ui.xaml"))
                Window = (Window)XamlReader.Load(s);
            SetupAssets();
            Window.Loaded += OnLoaded;
            var wa = SystemParameters.WorkArea;
            double tw = 1040, th = 780;
            for (int i = 0; i < a.Length - 1; i++) if (a[i] == "--size") { var z = a[i + 1].Split('x'); double.TryParse(z[0], out tw); double.TryParse(z[1], out th); }
            Window.Width = Math.Min(tw, wa.Width); Window.Height = Math.Min(th, wa.Height);
            Build();
        }

        // ---------- config ----------
        void LoadCfg()
        {
            try { if (File.Exists(cfgFile)) foreach (var l in File.ReadAllLines(cfgFile)) { int i = l.IndexOf('='); if (i > 0) cfg[l.Substring(0, i)] = l.Substring(i + 1); } } catch { }
            string r; if (cfg.TryGetValue("Role", out r) && (r == "host" || r == "guest")) role = r;
            cfg.TryGetValue("InstanceId", out instId);
            string v; double vv;
            if (cfg.TryGetValue("Volume", out v) && double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out vv)) volume = Math.Max(0, Math.Min(1, vv));
            if (cfg.TryGetValue("Muted", out v)) muted = v == "1";
            if (cfg.TryGetValue("Fx", out v) && v == "low") lowFx = true;
        }
        void SaveAudioCfg()
        {
            cfg["Volume"] = volume.ToString("0.00", System.Globalization.CultureInfo.InvariantCulture); cfg["Muted"] = muted ? "1" : "0"; cfg["Fx"] = lowFx ? "low" : "high"; SaveCfg();
        }
        void SaveCfg()
        {
            try { cfg["Role"] = role; cfg["InstanceId"] = instId ?? ""; File.WriteAllLines(cfgFile, cfg.Select(k => k.Key + "=" + k.Value).ToArray()); } catch { }
        }
        string Cfg(string k) { string v; return cfg.TryGetValue(k, out v) && !string.IsNullOrEmpty(v) ? v : null; }

        void Log(string s)
        {
            var line = DateTime.Now.ToString("HH:mm:ss") + "  " + s;
            try { File.AppendAllText(logFile, line + Environment.NewLine); } catch { }
            Window.Dispatcher.BeginInvoke(new Action(() => { var t = F<TextBox>("LogText"); t.AppendText(line + "\n"); t.ScrollToEnd(); var t2 = F<TextBox>("InstLogText"); if (t2 != null) { t2.AppendText(line + "\n"); t2.ScrollToEnd(); } }));
        }

        // ---------- UI base ----------
        void Build()
        {
            F<Button>("BtnClose").Click += (s, e) => Window.Close();
            F<Button>("BtnMin").Click += (s, e) => Window.WindowState = WindowState.Minimized;
            F<Grid>("TitleBar").MouseLeftButtonDown += (s, e) => { if (e.ButtonState == MouseButtonState.Pressed) { try { Window.DragMove(); } catch { } } };
            F<RadioButton>("TabPlay").Checked += (s, e) => ShowPage("play");
            F<RadioButton>("TabInstall").Checked += (s, e) => ShowPage("install");
            F<RadioButton>("TabHelp").Checked += (s, e) => ShowPage("help");
            F<Button>("BtnPlay").Click += async (s, e) => { if (runMode) await StopAll(); else await PlayClicked(); };
            F<Button>("BtnConfYes").Click += (s, e) => ResolveConfirm(true);
            F<Button>("BtnConfNo").Click += (s, e) => ResolveConfirm(false);
            F<Button>("BtnStop").Click += async (s, e) => await StopAll();
            F<Button>("BtnRescan").Click += async (s, e) => await Rescan();
            F<Button>("BtnLog").Click += (s, e) => { var b = F<Border>("LogBox"); b.Visibility = b.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible; };
            F<Button>("BtnPlayers").Click += (s, e) => EditPlayers(false);
            F<RadioButton>("RoleHost").Checked += (s, e) => SetRole("host");
            F<RadioButton>("RoleGuest").Checked += (s, e) => SetRole("guest");
            F<TextBox>("ServerBox").TextChanged += (s, e) => F<TextBlock>("ServerHint").Visibility = F<TextBox>("ServerBox").Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
            if (dry) F<Border>("DryBadge").Visibility = Visibility.Visible;
            Window.KeyDown += (s, e) => { if (e.Key == Key.Escape && Window.WindowState != WindowState.Minimized && !busy) { } };

            Window.PreviewKeyDown += OnPreviewKey;
            Window.PreviewMouseLeftButtonDown += OnPreviewMouse;
            F<Button>("BtnVol").Click += (s, e) => ToggleMute();
            BuildHeart();
            LoadAvatar();
            Window.SizeChanged += (s, e) =>
            {
                F<TextBlock>("StarText").Visibility = Window.ActualWidth < 1240 ? Visibility.Collapsed : Visibility.Visible;
                F<Button>("BtnSupport").Visibility = Window.ActualWidth < 880 ? Visibility.Collapsed : Visibility.Visible;
            };
            F<Button>("BtnSupport").Click += (s, e) => { OpenUrl(RepoUrl); Log("GitHub: abrindo o repositorio no navegador."); };
            F<Button>("BtnFx").Click += (s, e) => { lowFx = !lowFx; ApplyFx(); SaveAudioCfg(); Log("Efeitos: " + (lowFx ? "reduzidos" : "completos")); };
            F<Canvas>("VolBar").MouseLeftButtonDown += (s, e) =>
            {
                double x = e.GetPosition(F<Canvas>("VolBar")).X; int l = (int)Math.Max(1, Math.Min(5, Math.Floor((x - 1) / 8) + 1));
                volume = l / 5.0; muted = false; ApplyAudio(); sfx.Play("volume");
            };
            EventManager.RegisterClassHandler(typeof(ButtonBase), Mouse.MouseEnterEvent, new MouseEventHandler((s, e) => { if (((ButtonBase)s).IsEnabled) sfx.Play("hover"); }));
            EventManager.RegisterClassHandler(typeof(ButtonBase), ButtonBase.ClickEvent, new RoutedEventHandler((s, e) =>
            {
                var n = ((FrameworkElement)s).Name; sfx.Play((n == "BtnPlay" || n == "BtnInstall") ? "confirm" : n == "BtnVol" ? "volume" : (s is RadioButton && (n == "TabPlay" || n == "TabHelp" || n == "TabInstall")) ? "tab" : "click");
            }));
            BuildLogo(); BuildBlocks(); BuildRings(); BuildHelp(); BuildSteps(); BuildInstall(); BuildSenha();
            ApplyAudio(); ApplyFx();
            (role == "guest" ? F<RadioButton>("RoleGuest") : F<RadioButton>("RoleHost")).IsChecked = true;
            ApplyRoleUi(); ApplyPlayerLabels();
        }

        string curPage = "play";
        void ShowPage(string page)
        {
            var order = new[] { "play", "install", "help" };
            int from = Array.IndexOf(order, curPage), to = Array.IndexOf(order, page); curPage = page;
            F<Grid>("PagePlay").Visibility = page == "play" ? Visibility.Visible : Visibility.Collapsed;
            F<Grid>("PageInstall").Visibility = page == "install" ? Visibility.Visible : Visibility.Collapsed;
            F<ScrollViewer>("PageHelp").Visibility = page == "help" ? Visibility.Visible : Visibility.Collapsed;
            var el = page == "play" ? (UIElement)F<Grid>("PagePlay") : page == "install" ? (UIElement)F<Grid>("PageInstall") : F<ScrollViewer>("PageHelp");
            el.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(240)));
            var tt = new TranslateTransform(); el.RenderTransform = tt;
            tt.BeginAnimation(TranslateTransform.XProperty, new DoubleAnimation(to >= from ? 36 : -36, 0, TimeSpan.FromMilliseconds(280)) { EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut } });
        }
        void GoInstall() { F<RadioButton>("TabInstall").IsChecked = true; }

        // ---------- audio / efeitos / intro ----------
        void Extract(string res, string dest)
        {
            try
            {
                using (var s = typeof(MainWin).Assembly.GetManifestResourceStream(res))
                {
                    if (s == null) return;
                    if (File.Exists(dest) && new FileInfo(dest).Length == s.Length) return;
                    using (var f = File.Create(dest)) s.CopyTo(f);
                }
            }
            catch { }
        }

        void SetupAssets()
        {
            assetDir = Path.Combine(baseDir, "assets");
            if (!File.Exists(Path.Combine(assetDir, "fonts", "PressStart2P-Regular.ttf")))
            {
                assetDir = Path.Combine(stateDir, "assets");
                try { Directory.CreateDirectory(Path.Combine(assetDir, "fonts")); } catch { }
                Extract("musica-8bit.mp3", Path.Combine(assetDir, "musica-8bit.mp3"));
                Extract("PressStart2P-Regular.ttf", Path.Combine(assetDir, "fonts", "PressStart2P-Regular.ttf"));
            }
            try
            {
                var uri = new Uri("file:///" + Path.Combine(assetDir, "fonts").Replace('\\', '/') + "/");
                var ff = new FontFamily(uri, "./#Press Start 2P");
                GlyphTypeface gt; bool ok = new Typeface(ff, FontStyles.Normal, FontWeights.Normal, FontStretches.Normal).TryGetGlyphTypeface(out gt);
                if (ok) { pixelFF = ff; Window.Resources["Pixel"] = ff; }
                Log("Fonte pixel (Press Start 2P, OFL): " + (ok ? "carregada de " + assetDir : "NAO carregada, usando Consolas"));
            }
            catch (Exception ex) { Log("Fonte pixel: erro - " + ex.Message); }
        }

        // Musica opcional: procura musica-8bit.mp3 em assets\ ao lado do .exe, depois na pasta de estado. Sem arquivo = sem musica, sem erro.
        string FindMusic()
        {
            foreach (var d in new[] { Path.Combine(baseDir, "assets"), assetDir, Path.Combine(stateDir, "assets") })
            { try { var f = Path.Combine(d, "musica-8bit.mp3"); if (File.Exists(f)) return f; } catch { } }
            return null;
        }

        void ApplyAudio()
        {
            music.Vol = volume; music.Muted = muted; sfx.Vol = volume; sfx.Muted = muted;
            DrawVolUi(); SaveAudioCfg();
        }
        void ToggleMute() { muted = !muted; if (!muted && volume < 0.05) volume = 0.6; ApplyAudio(); if (!muted) sfx.Play("volume"); Log("Musica: " + (muted ? "silenciada" : "ligada (volume " + (int)Math.Round(volume * 100) + "%)")); }
        void ApplyFx()
        {
            F<TextBlock>("FxText").Text = lowFx ? "fx-" : "FX";
            F<TextBlock>("FxText").Foreground = lowFx ? cOff : cGoldB;
            if (introScene != null) introScene.Low = lowFx; if (bgScene != null) bgScene.Low = lowFx;
        }

        void DrawVolUi()
        {
            var ic = F<Canvas>("VolIcon"); ic.Children.Clear();
            Action<double, double, double, double, Brush> r = (x, y, w, h, b) => { var q = new Rectangle { Width = w, Height = h, Fill = b }; Canvas.SetLeft(q, x); Canvas.SetTop(q, y); ic.Children.Add(q); };
            Brush c = muted ? cMuted : cGoldB;
            r(1, 6, 4, 6, c); r(5, 4, 3, 10, c); r(8, 1, 3, 16, c);
            if (!muted) { r(14, 6, 2, 6, c); if (volume > 0.45) r(18, 3, 2, 12, c); }
            else for (int i = 0; i < 5; i++) { r(13 + i * 2, 3 + i * 3, 2, 3, cErr); r(21 - i * 2, 3 + i * 3, 2, 3, cErr); }
            var vb = F<Canvas>("VolBar"); vb.Children.Clear();
            int lvl = (int)Math.Round(volume * 5);
            for (int i = 0; i < 5; i++)
            {
                double h = 6 + i * 4;
                var q = new Rectangle { Width = 6, Height = h, Fill = (!muted && i < lvl) ? cGoldB : cOff };
                Canvas.SetLeft(q, 1 + i * 8); Canvas.SetTop(q, 34 - h); vb.Children.Add(q);
            }
        }

        bool InTitleBar(object src)
        {
            var tb = F<Grid>("TitleBar");
            for (var d = src as DependencyObject; d != null; d = (d is Visual) ? VisualTreeHelper.GetParent(d) : LogicalTreeHelper.GetParent(d)) if (d == tb) return true;
            return false;
        }
        void OnPreviewKey(object s, KeyEventArgs e)
        {
            if (e.Key == Key.Escape && confTcs != null) { ResolveConfirm(false); e.Handled = true; return; }
            if (e.Key == Key.M && !(Keyboard.FocusedElement is TextBox) && confTcs == null) { ToggleMute(); e.Handled = true; return; }
            if (!introActive) return;
            var k = e.Key == Key.System ? e.SystemKey : e.Key;
            if (k == Key.LeftAlt || k == Key.RightAlt || k == Key.LeftCtrl || k == Key.RightCtrl || k == Key.LeftShift || k == Key.RightShift || k == Key.F4 || e.Key == Key.System) return;
            SkipIntro(); e.Handled = true;
        }
        void OnPreviewMouse(object s, MouseButtonEventArgs e)
        {
            if (introActive && !InTitleBar(e.OriginalSource)) { SkipIntro(); e.Handled = true; }
        }

        void StartScenes()
        {
            var ww = Window.ActualWidth - 8; var hh = Window.ActualHeight - 8;
            pxScale = ww >= 1500 ? 5 : ww >= 900 ? 4 : 3;
            int w = (int)Math.Ceiling(ww / pxScale), h = (int)Math.Ceiling(hh / pxScale);
            bgScene = new PixelScene(w, h, pxScale) { Low = lowFx, EmberTarget = 60 };
            var bg = F<Image>("BgImg"); bg.Source = bgScene.Bmp; bg.Width = w * pxScale; bg.Height = h * pxScale;
            if (!noIntro && (shotPath == null || introShotsPrefix != null) && !autoPlay)
            {
                introScene = new PixelScene(w, h, pxScale) { Low = lowFx };
                introScene.OnCue = (n) => sfx.Play(n);
                var im = F<Image>("IntroImg"); im.Source = introScene.Bmp; im.Width = w * pxScale; im.Height = h * pxScale;
                F<Grid>("IntroLayer").Visibility = Visibility.Visible; introActive = true; introT0 = clock.Elapsed.TotalSeconds;
                F<StackPanel>("TitleLeft").Opacity = 0; F<StackPanel>("TitleTabs").Opacity = 0; F<StackPanel>("TitleLeft").IsHitTestVisible = false; F<StackPanel>("TitleTabs").IsHitTestVisible = false;
                F<Grid>("TitleBar").Background = Brushes.Transparent;
                Window.Activate(); Window.Focus();
                F<Grid>("IntroLayer").Focusable = true; Keyboard.Focus(F<Grid>("IntroLayer"));
                F<TextBlock>("SkipHint").BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromSeconds(1)) { BeginTime = TimeSpan.FromSeconds(1) });
                Log("Intro iniciada (" + w + "x" + h + " pixels logicos, escala " + pxScale + ").");
            }
            else bgActive = true;
            fpsT0 = clock.Elapsed.TotalSeconds; fpsN = 0;
            CompositionTarget.Rendering += OnFrame;
        }

        void SkipIntro()
        {
            if (!introActive) return; introActive = false; bgActive = true; sfx.Play("confirm");
            Log("Intro concluida/pulada.");
            var layer = F<Grid>("IntroLayer");
            var an = new DoubleAnimation(1, 0, TimeSpan.FromMilliseconds(800)); an.Completed += (s, e) => { layer.Visibility = Visibility.Collapsed; introScene = null; };
            layer.BeginAnimation(UIElement.OpacityProperty, an);
            foreach (var n in new[] { "TitleLeft", "TitleTabs" })
            {
                var sp = F<StackPanel>(n); sp.IsHitTestVisible = true;
                sp.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(700)) { BeginTime = TimeSpan.FromMilliseconds(250) });
            }
            F<Grid>("TitleBar").Background = B("#E0100D0A");
        }

        void OnFrame(object s, EventArgs ev)
        {
            double t = clock.Elapsed.TotalSeconds;
            music.Update();
            if (!musLog1 && t > 3.2 && music.Playing) { musLog1 = true; Log("Musica: tocando, posicao " + music.Position.ToString("0.0") + " s, volume " + (muted ? "mudo" : ((int)Math.Round(volume * 100)) + "%") + "."); }
            if (!musLog2 && t > 9.0 && music.Playing) { musLog2 = true; Log("Musica: posicao " + music.Position.ToString("0.0") + " s (avancando = tocando)."); }
            if (introActive && skipAt > 0 && t - introT0 >= skipAt) { skipAt = 0; SkipIntro(); if (introShotsPrefix != null) { var tm = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(2600) }; tm.Tick += (q, w) => { tm.Stop(); Shot(introShotsPrefix + "-apos-pular.png"); Log("Captura: apos pular"); if (args.Contains("--exit-after-intro")) Window.Close(); }; tm.Start(); } }
            if (introScene != null)
            {
                double it = t - introT0; if (!introActive) it = Math.Max(it, 0);
                var sw = Stopwatch.StartNew();
                introScene.Render(t, it, Cx.Ease(it / 1.6));
                var pt = F<TextBlock>("PressTxt");
                if (introActive && it > 15 * PixelScene.Beat) pt.Opacity = ((int)(t * 2.2) % 2 == 0) ? 1.0 : 0.25; else if (!introActive) pt.Opacity = 0;
                if (introShotsPrefix != null && shotTimes.Count > 0 && it >= shotTimes.Peek())
                {
                    double st = shotTimes.Dequeue(); var path = introShotsPrefix + "-intro-" + st.ToString("0") + "s.png";
                    Dispatcher.CurrentDispatcher.BeginInvoke(new Action(() => { try { Shot(path); Log("Captura: " + path); } catch (Exception ex) { Log("Captura falhou: " + ex.Message); } if (shotTimes.Count == 0 && skipAt <= 0 && !args.Contains("--skip-intro-at") && args.Contains("--exit-after-intro")) Window.Close(); }), DispatcherPriority.ApplicationIdle);
                }
            }
            if (bgActive && bgScene != null)
            {
                double iv = lowFx ? 1.0 / 12 : 1.0 / 30;
                if (t - lastBg >= iv) { lastBg = t; bgScene.Render(t, -1, 0.47); }
            }
            fpsN++;
            if (t - fpsT0 >= 5)
            {
                double fps = fpsN / (t - fpsT0);
                if (!fpsLogged) { fpsLogged = true; Log("Desempenho: " + fps.ToString("0") + " quadros/s medidos de quadros da janela (" + (introActive ? "intro" : "fundo") + ")."); }
                if (!introActive && !lowFx && bgActive && fps < 24) { lowFx = true; ApplyFx(); SaveAudioCfg(); Log("Maquina lenta (" + fps.ToString("0") + " fps): efeitos reduzidos automaticamente."); }
                fpsT0 = t; fpsN = 0;
            }
        }

        // ---------- apelidos (players.json) ----------
        void ApplyPlayerLabels()
        {
            F<TextBlock>("HostNickText").Text = players.Host.Nick.Length > 0 ? players.Host.Nick : "definir apelido";
            F<TextBlock>("GuestNickText").Text = players.Guest.Nick.Length > 0 ? players.Guest.Nick : "definir apelido";
        }
        // true se o usuario salvou
        bool EditPlayers(bool first)
        {
            var dlg = new PlayersDialog(Window, players, first, Log);
            if (setupTest != null) { var spec = setupTest; setupTest = null; dlg.Loaded += async (s, e) => { try { await dlg.RunTest(spec); } catch (Exception ex) { Log("setup-test: " + ex.Message); dlg.DialogResult = false; } }; }
            var ok = dlg.ShowDialog() == true;
            if (ok && dlg.Result != null)
            {
                players = dlg.Result;
                try { players.Save(playersFile); Log("Apelidos salvos (anfitrião: " + players.Host.Nick + ", convidado: " + players.Guest.Nick + ")."); } catch (Exception ex) { Log("Não consegui salvar os apelidos: " + ex.Message); }
                ApplyPlayerLabels();
                var hp = F<StackPanel>("HelpPanel"); hp.Children.Clear(); BuildHelp();
            }
            return ok;
        }

        void SetRole(string r)
        {
            role = r; instId = null; ApplyRoleUi(); SaveCfg(); RenderChecks();
        }
        void ApplyRoleUi()
        {
            F<StackPanel>("ServerPanel").Visibility = role == "guest" ? Visibility.Visible : Visibility.Collapsed;
        }

        void BuildHeart()
        {
            // estrela pixel (pede uma estrela no GitHub)
            var c = F<Canvas>("HeartIcon");
            string[] rows = { "....#....", "....#....", "...###...", "#########", ".#######.", "..#####..", ".###.###.", ".##...##." };
            for (int y = 0; y < rows.Length; y++) for (int x = 0; x < 9; x++)
            {
                if (rows[y][x] != '#') continue;
                var r = new Rectangle { Width = 1.5, Height = 1.5, Fill = B(y < 3 ? "#FFE48A" : "#F0B93A"), SnapsToDevicePixels = true };
                Canvas.SetLeft(r, 0.25 + x * 1.5); Canvas.SetTop(r, y * 1.5); c.Children.Add(r);
            }
        }

        void LoadAvatar()
        {
            try
            {
                using (var st = typeof(MainWin).Assembly.GetManifestResourceStream("avatar.jpg"))
                {
                    if (st == null) { F<Image>("AvatarImg").Visibility = Visibility.Collapsed; return; }
                    var bi = new System.Windows.Media.Imaging.BitmapImage();
                    bi.BeginInit(); bi.CacheOption = System.Windows.Media.Imaging.BitmapCacheOption.OnLoad; bi.StreamSource = st; bi.EndInit(); bi.Freeze();
                    F<Image>("AvatarImg").Source = bi;
                }
            }
            catch (Exception ex) { Log("Avatar: " + ex.Message); F<Image>("AvatarImg").Visibility = Visibility.Collapsed; }
        }

        void BuildLogo()
        {
            var c = F<Canvas>("Logo");
            var d = new Rectangle { Width = 20, Height = 20, Stroke = cGold, StrokeThickness = 1.6, RenderTransformOrigin = new Point(0.5, 0.5), RenderTransform = new RotateTransform(45) };
            Canvas.SetLeft(d, 5); Canvas.SetTop(d, 5); c.Children.Add(d);
            string[] cols = { "#6B9E3A", "#8A5A2B", "#6B9E3A", "#8A5A2B", "#EBCB7A", "#8A5A2B", "#7A7A7A", "#8A5A2B", "#7A7A7A" };
            for (int i = 0; i < 9; i++)
            {
                var r = new Rectangle { Width = 5, Height = 5, Fill = B(cols[i]), SnapsToDevicePixels = true };
                Canvas.SetLeft(r, 7.5 + (i % 3) * 5); Canvas.SetTop(r, 7.5 + (i / 3) * 5); c.Children.Add(r);
            }
        }

        void BuildBlocks()
        {
            var c = F<Canvas>("BlockStrip"); var rnd = new Random(7);
            string[][] pal = { new[] { "#4F7F2A", "#5E9433", "#456F25" }, new[] { "#6B4A2A", "#7A5530", "#5E4025" }, new[] { "#4A4640", "#58534B", "#3E3A35" } };
            for (int x = 0; x < 260; x++)
            {
                for (int y = 0; y < 2; y++)
                {
                    var set = y == 0 ? pal[0] : pal[1];
                    if (y == 1 && rnd.Next(7) == 0) set = pal[2];
                    var r = new Rectangle { Width = 10, Height = 10, Fill = B(set[rnd.Next(3)]), Opacity = 0.55 };
                    Canvas.SetLeft(r, x * 10); Canvas.SetTop(r, y * 10); c.Children.Add(r);
                }
            }
        }

        void BuildRings()
        {
            var c = F<Canvas>("RingsCanvas");
            Action place = () => { };
            for (int i = 0; i < 2; i++)
            {
                double size = i == 0 ? 470 : 380;
                var e = new Ellipse { Width = size, Height = size, Stroke = cGold, StrokeThickness = 1, Opacity = i == 0 ? 0.20 : 0.13,
                    StrokeDashArray = i == 0 ? new DoubleCollection(new double[] { 1, 7 }) : new DoubleCollection(new double[] { 40, 10 }),
                    RenderTransformOrigin = new Point(0.5, 0.5) };
                var rt = new RotateTransform(0); e.RenderTransform = rt;
                rt.BeginAnimation(RotateTransform.AngleProperty, new DoubleAnimation(i == 0 ? 0 : 360, i == 0 ? 360 : 0, TimeSpan.FromSeconds(i == 0 ? 90 : 140)) { RepeatBehavior = RepeatBehavior.Forever });
                c.Children.Add(e);
                var ee = e; double sz = size;
                c.SizeChanged += (s, a) => { Canvas.SetLeft(ee, (c.ActualWidth - sz) / 2); Canvas.SetTop(ee, c.ActualHeight * 0.37 - sz / 2); };
            }
        }

        // ---------- passos ----------
        readonly string[] stepNames = { "Verificar", "Elden Ring", "Minecraft", "Pronto" };
        void BuildSteps()
        {
            var p = F<StackPanel>("StepsPanel"); p.Children.Clear();
            for (int i = 0; i < stepNames.Length; i++)
            {
                var col = new StackPanel { Width = 84 };
                bool done = stepIndex > i || (stepIndex == 3 && i == 3 && stepFail == null);
                bool active = stepIndex == i && stepFail == null && i < 3;
                bool failed = stepIndex == i && stepFail != null;
                var circle = new Border { Width = 26, Height = 26, CornerRadius = new CornerRadius(0), BorderThickness = new Thickness(2.5), HorizontalAlignment = HorizontalAlignment.Center };
                var tx = new TextBlock { Text = (i + 1).ToString(), FontSize = 11, FontFamily = pixelFF ?? new FontFamily("Consolas"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                if (done) { circle.Background = cOk; circle.BorderBrush = cOk; tx.Text = "\u2713"; tx.Foreground = B("#10200A"); }
                else if (failed) { circle.Background = cErr; circle.BorderBrush = cErr; tx.Text = "!"; tx.Foreground = B("#2A0E08"); }
                else if (active) { circle.BorderBrush = cGoldB; circle.Background = B("#2E2312"); tx.Foreground = cGoldB; circle.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(1, 0.45, TimeSpan.FromMilliseconds(700)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever }); }
                else { circle.BorderBrush = cLine; tx.Foreground = cMuted; }
                circle.Child = tx; col.Children.Add(circle);
                col.Children.Add(new TextBlock { Text = stepNames[i], FontSize = 11.5, Foreground = (done || active) ? cText : cMuted, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 6, 0, 0) });
                p.Children.Add(col);
                if (i < stepNames.Length - 1)
                    p.Children.Add(new Rectangle { Width = 36, Height = 2, Fill = stepIndex > i ? cOk : (Brush)cLine, Margin = new Thickness(-4, 12, -4, 0), VerticalAlignment = VerticalAlignment.Top });
            }
        }
        void SetStep(int i, string fail = null) { Window.Dispatcher.Invoke(new Action(() => { stepIndex = i; stepFail = fail; BuildSteps(); })); }
        void Say(string s) { Window.Dispatcher.Invoke(new Action(() => { F<TextBlock>("StatusText").Text = s; })); Log(s); }
        void ShowError(string title, string body)
        {
            sfx.Play("error");
            Window.Dispatcher.Invoke(new Action(() =>
            {
                F<TextBlock>("ErrTitle").Text = title; F<TextBlock>("ErrBody").Text = body;
                var b = F<Border>("ErrBox"); b.Visibility = Visibility.Visible;
                b.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(250)));
            }));
            Log("ERRO: " + title + " - " + body);
        }
        void HideError() { F<Border>("ErrBox").Visibility = Visibility.Collapsed; }

        // ---------- deteccao ----------
        async void OnLoaded(object s, RoutedEventArgs e)
        {
            // presenca do JOGAR: halo pulsando em degraus + "pisca" do APERTE PARA COMECAR + balanco de pixel
            var ha = new DoubleAnimation(0.25, 1, TimeSpan.FromMilliseconds(1100)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever };
            F<Border>("Halo1").BeginAnimation(UIElement.OpacityProperty, ha);
            F<Border>("Halo2").BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(1, 0.2, TimeSpan.FromMilliseconds(1100)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
            var blink = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromMilliseconds(1500), RepeatBehavior = RepeatBehavior.Forever };
            blink.KeyFrames.Add(new DiscreteDoubleKeyFrame(1, KeyTime.FromPercent(0))); blink.KeyFrames.Add(new DiscreteDoubleKeyFrame(0.1, KeyTime.FromPercent(0.55)));
            F<TextBlock>("PressHint").BeginAnimation(UIElement.OpacityProperty, blink);
            var bob = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromMilliseconds(1200), RepeatBehavior = RepeatBehavior.Forever };
            bob.KeyFrames.Add(new DiscreteDoubleKeyFrame(0, KeyTime.FromPercent(0))); bob.KeyFrames.Add(new DiscreteDoubleKeyFrame(-3, KeyTime.FromPercent(0.5)));
            var bt = new TranslateTransform(); ((FrameworkElement)F<Button>("BtnPlay").Parent).RenderTransform = bt; bt.BeginAnimation(TranslateTransform.YProperty, bob);
            Log("Launcher iniciado" + (dry ? " (modo simulacao)" : "") + ".");
            if (!noMusic) { var mp = FindMusic(); if (mp != null) music.Start(mp, Log); else Log("Musica: nenhum musica-8bit.mp3 encontrado (opcional). Nada vai tocar."); } else Log("Musica desligada (--no-music).");
            if (!players.Complete || setupTest != null) EditPlayers(!players.Complete);
            StartScenes();
            monitor = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            monitor.Tick += (a, b) => { PollGames(); InstallTick(); }; monitor.Start(); PollGames();
            if (simDir != null) Log("[teste] --simulate-games ativo: JOGAR abre processos FALSOS de " + simDir + " (sandbox).");
            if (shotTab == "help") F<RadioButton>("TabHelp").IsChecked = true;
            if (shotTab == "install") F<RadioButton>("TabInstall").IsChecked = true;
            await Rescan();
            PollGames();   // jogos ja abertos (launcher reaberto): reconhece e mostra RODANDO/PARAR
            // quem ainda nao tem tudo cai direto na aba INSTALAR (quem ja tem tudo nao ve diferenca)
            if (shotPath == null && !autoPlay && shotTab == null && !args.Contains("--auto-install") && NeedsInstall()) { GoInstall(); Log("Faltam itens: abrindo a aba INSTALAR."); }
            if (args.Contains("--auto-install")) { GoInstall(); await InstallAll(0, true); if (args.Contains("--exit-after-install")) { await Task.Delay(1200); Window.Close(); } }
            if (autoPlay) { await PlayClicked(); }
            if (shotPath != null) { await Task.Delay(1400); Shot(shotPath); Window.Close(); }
        }

        async Task Rescan()
        {
            scanning = true; RenderChecks();
            await Task.Delay(450);
            var d = await Task.Run(() => Detect());
            det = d; scanning = false;
            // escolher instancia
            var cands = det.Insts.Where(i => i.Role == role).ToList();
            if (instId == null || !cands.Any(i => i.Id == instId))
            {
                var best = cands.FirstOrDefault(i => i.Id.IndexOf("teste", StringComparison.OrdinalIgnoreCase) < 0) ?? cands.FirstOrDefault();
                instId = best != null ? best.Id : null;
            }
            RenderChecks(); RenderInstPicker();
            Log("Verificacao: jogo=" + (det.GameDir ?? "nao achado") + "; prism=" + (det.PrismExe ?? "nao achado") + "; perfis=" + det.Insts.Count + "; pacote=" + (det.InstallDir ?? "nao achado"));
        }

        string Hash(string p) { using (var f = File.OpenRead(p)) return BitConverter.ToString(SHA256.Create().ComputeHash(f)).Replace("-", ""); }

        Det Detect()
        {
            var d = new Det();
            if (fakeMissing) return d;
            // Steam
            string steam = null;
            if (sandbox != null) { var fs = Path.Combine(sandbox, "steam"); if (Directory.Exists(fs)) steam = fs; }   // sandbox: Steam falsa, nunca o registro real
            else foreach (var k in new[] { @"HKEY_CURRENT_USER\Software\Valve\Steam|SteamPath", @"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam|InstallPath", @"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam|InstallPath" })
            {
                try { var v = Registry.GetValue(k.Split('|')[0], k.Split('|')[1], null) as string; if (!string.IsNullOrEmpty(v) && Directory.Exists(v)) { steam = Path.GetFullPath(v); break; } } catch { }
            }
            d.SteamDir = steam; d.SteamRunning = sandbox != null ? true : Process.GetProcessesByName("steam").Length > 0;
            // Elden Ring
            var libs = new List<string>();
            if (steam != null)
            {
                libs.Add(steam);
                try
                {
                    var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
                    if (File.Exists(vdf)) foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\"")) libs.Add(m.Groups[1].Value.Replace("\\\\", "\\"));
                }
                catch { }
            }
            string gm = Cfg("GameDir");
            if (gm != null && File.Exists(Path.Combine(gm, "eldenring.exe"))) d.GameDir = gm;
            else foreach (var l in libs.Distinct())
                {
                    var g = Path.Combine(l, "steamapps", "common", "ELDEN RING", "Game");
                    if (File.Exists(Path.Combine(l, "steamapps", "appmanifest_1245620.acf")) && File.Exists(Path.Combine(g, "eldenring.exe"))) { d.GameDir = g; break; }
                }
            if (d.GameDir != null)
            {
                try { d.GameVer = FileVersionInfo.GetVersionInfo(Path.Combine(d.GameDir, "eldenring.exe")).FileVersion; } catch { }
                if (d.GameVer != null) d.GameVer = d.GameVer.Replace(',', '.').Trim();
                d.Seamless = File.Exists(Path.Combine(d.GameDir, "ersc_launcher.exe")) && File.Exists(Path.Combine(d.GameDir, "SeamlessCoop", "ersc.dll"));
            }
            // Prism
            var loc = localDir;
            var pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            var appdata = appDataDir;
            var pcands = new List<string>();
            if (Cfg("PrismExe") != null) pcands.Add(Cfg("PrismExe"));
            pcands.Add(Path.Combine(PrismInstallDir, "prismlauncher.exe"));   // Prism portatil instalado pelo proprio launcher
            if (sandbox == null)
            {
                pcands.Add(Path.Combine(baseDir, "PrismLauncher", "prismlauncher.exe"));
                pcands.Add(Path.Combine(loc, "Programs", "PrismLauncher", "prismlauncher.exe"));
                pcands.Add(Path.Combine(pf, "PrismLauncher", "prismlauncher.exe"));
            }
            d.PrismExe = pcands.FirstOrDefault(File.Exists);
            if (d.PrismExe != null)
            {
                var dir = Path.GetDirectoryName(d.PrismExe);
                // Prism 11 usa portable.txt (ou a pasta UserData); versoes antigas usavam prismlauncher_portable.txt.
                bool portable = File.Exists(Path.Combine(dir, "portable.txt")) || File.Exists(Path.Combine(dir, "prismlauncher_portable.txt")) || Directory.Exists(Path.Combine(dir, "UserData"));
                d.PrismData = portable ? (Directory.Exists(Path.Combine(dir, "UserData")) ? Path.Combine(dir, "UserData") : dir) : Path.Combine(appdata, "PrismLauncher");
                d.PrismAccount = HasPrismAccount(d.PrismData);
                string inst = Cfg("InstancesDir");
                if (inst == null)
                {
                    inst = Path.Combine(d.PrismData, "instances");
                    try
                    {
                        var cf = Path.Combine(d.PrismData, "prismlauncher.cfg");
                        if (File.Exists(cf))
                        {
                            var m = Regex.Match(File.ReadAllText(cf), @"^InstanceDir=(.+)$", RegexOptions.Multiline);
                            if (m.Success) { var v = m.Groups[1].Value.Trim(); inst = Path.IsPathRooted(v) ? v : Path.Combine(d.PrismData, v); }
                        }
                    }
                    catch { }
                }
                d.InstancesDir = inst;
                try
                {
                    if (Directory.Exists(inst))
                        foreach (var idir in Directory.GetDirectories(inst))
                        {
                            var icfg = Path.Combine(idir, "instance.cfg");
                            if (!File.Exists(icfg)) continue;
                            var txt = File.ReadAllText(icfg);
                            var ja = Regex.Match(txt, @"^JvmArgs=(.*)$", RegexOptions.Multiline).Groups[1].Value;
                            if (ja.IndexOf("-Derbridge.coop=true", StringComparison.OrdinalIgnoreCase) < 0) continue;
                            var rm = Regex.Match(ja, @"-Derbridge\.coop\.role=(host|guest)", RegexOptions.IgnoreCase);
                            if (!rm.Success) continue;
                            var nm = Regex.Match(txt, @"^name=(.*)$", RegexOptions.Multiline).Groups[1].Value.Trim();
                            d.Insts.Add(new Inst { Id = Path.GetFileName(idir), Name = nm.Length > 0 ? nm : Path.GetFileName(idir), Role = rm.Groups[1].Value.ToLowerInvariant(), Dir = idir });
                        }
                }
                catch { }
            }
            // Pacote instalado (instalacao anterior com bridge-backups)
            var inc = new List<string>();
            if (Cfg("InstallDir") != null) inc.Add(Cfg("InstallDir"));
            inc.Add(PackInstallDir);   // pack instalado pelo proprio launcher (estado do launcher; na sandbox, dentro da sandbox)
            if (sandbox == null)
            {
                var parent = Directory.GetParent(baseDir); if (parent != null) inc.Add(Path.Combine(parent.FullName, "EldenMinecraft-Windows"));
                var docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
                var dl = Path.Combine(homeDir, "Downloads");
                foreach (var root in new[] { docs, dl })
                    try { foreach (var sub in Directory.GetDirectories(root, "EldenMinecraft-Windows*", SearchOption.TopDirectoryOnly)) inc.Add(sub); } catch { }
            }
            // prefere o pacote cujo manifesto e dono da ponte instalada NESTE jogo; se nenhum candidato for, procura o pacote anterior
            // em outras pastas (launcher movido de lugar). O achado fica salvo no config, que e o mesmo para qualquer copia do .exe.
            d.InstallDir = inc.FirstOrDefault(p => OwnsGame(p, d.GameDir)) ?? FindOwningInstall(d.GameDir)
                ?? inc.FirstOrDefault(p => Directory.Exists(Path.Combine(p, "bridge-backups")) && File.Exists(Path.Combine(p, "elden-ring", "windows", "Start-Coop.ps1")));
            if (d.InstallDir != null && OwnsGame(d.InstallDir, d.GameDir) && !string.Equals(Cfg("InstallDir"), d.InstallDir, StringComparison.OrdinalIgnoreCase) && !string.Equals(d.InstallDir, PackInstallDir, StringComparison.OrdinalIgnoreCase))
            {
                var found = d.InstallDir;
                Window.Dispatcher.BeginInvoke(new Action(() => { cfg["InstallDir"] = found; SaveCfg(); }));
            }
            // Minecraft Java e Java 21 (informativo: o Prism baixa o jogo e o Java sozinho)
            d.McFound = FindMinecraft(); d.JavaWhere = FindJava21(d.PrismData);
            var pk = d.InstallDir ?? PackInstallDir;
            d.BridgeInstalled = ManifestInstalled(Path.Combine(pk, "bridge-backups"), "manifest.json");
            d.CoopManifest = ManifestInstalled(Path.Combine(pk, "coop-backups"), "coop-manifest.json");
            if (d.InstallDir != null)
            {
                try
                {
                    var rel = File.ReadAllText(Path.Combine(packDir, "EldenMinecraft-Windows", "elden-ring", "windows", "coop-v2-release.json"));
                    var want = Regex.Match(rel, @"""CoreSha256""\s*:\s*""([0-9a-fA-F]{64})""").Groups[1].Value;
                    var core = d.GameDir == null ? null : Path.Combine(d.GameDir, "erbridge", "erbridge_core.dll");
                    d.UpToDate = want.Length == 64 && core != null && File.Exists(core) && string.Equals(Hash(core), want, StringComparison.OrdinalIgnoreCase);
                    // release que so troca o jar (core igual) tambem precisa atualizar os perfis
                    var wantJar = Regex.Match(rel, @"""JarSha256""\s*:\s*""([0-9a-fA-F]{64})""").Groups[1].Value;
                    if (d.UpToDate && wantJar.Length == 64)
                        foreach (var i in d.Insts)
                        {
                            var mods = Path.Combine(i.Dir, ".minecraft", "mods");
                            if (!Directory.Exists(mods)) continue;
                            foreach (var jar in Directory.GetFiles(mods, "er-bridge*.jar"))
                                if (!string.Equals(Hash(jar), wantJar, StringComparison.OrdinalIgnoreCase)) d.UpToDate = false;
                        }
                }
                catch { }
            }
            return d;
        }

        // ---------- checklist ----------
        List<Row> MakeRows()
        {
            var rows = new List<Row>();
            if (scanning)
            {
                foreach (var t in new[] { "Steam", "Elden Ring", "Seamless Co-op", "Prism Launcher", "Perfil Minecraft", "Pacote co-op", "Senha do co-op" })
                    rows.Add(new Row { State = St.Loading, Title = t, Detail = "Procurando\u2026" });
                return rows;
            }
            var d = det;
            rows.Add(d.SteamDir == null ? new Row { State = St.Err, Title = "Steam", Detail = "Não encontrei a Steam. Instale e entre na sua conta." }
                : d.SteamRunning ? new Row { State = St.Ok, Title = "Steam", Detail = "Aberta e pronta." }
                : new Row { State = St.Warn, Title = "Steam", Detail = "Fechada. Abra a Steam e faça login antes de jogar." });

            if (d.GameDir == null) rows.Add(new Row { State = St.Err, Title = "Elden Ring", Detail = "Não achei o jogo. Aponte a pasta ELDEN RING\\Game.", ActionText = "Escolher pasta", Action = PickGame });
            else if (d.GameVer != "2.7.1.0") rows.Add(new Row { State = St.Err, Title = "Elden Ring", Detail = "Versão " + (d.GameVer ?? "desconhecida") + " não suportada. Precisa ser 2.7.1.0 (não deixe a Steam atualizar)." });
            else rows.Add(new Row { State = St.Ok, Title = "Elden Ring", Detail = "Versão 2.7.1.0 encontrada." });

            rows.Add(d.GameDir == null ? new Row { State = St.Loading, Title = "Seamless Co-op", Detail = "Depende do Elden Ring." }
                : d.Seamless ? new Row { State = St.Ok, Title = "Seamless Co-op", Detail = "Instalado." }
                : new Row { State = St.Err, Title = "Seamless Co-op", Detail = "Falta o Seamless v2.0.1. A aba INSTALAR baixa do autor e instala.", ActionText = "Instalar", Action = GoInstall, ActionText2 = "Abrir página", Action2 = () => OpenUrl("https://www.nexusmods.com/eldenring/mods/510") });
            if (rows[2].State == St.Loading) rows[2].State = St.Warn;

            rows.Add(d.PrismExe == null ? new Row { State = St.Err, Title = "Prism Launcher", Detail = "Não achei o Prism. A aba INSTALAR baixa o Prism portátil oficial.", ActionText = "Instalar", Action = GoInstall, ActionText2 = "Escolher arquivo", Action2 = PickPrism }
                : new Row { State = St.Ok, Title = "Prism Launcher", Detail = "Encontrado." });

            var mine = d.Insts.Where(i => i.Role == role).ToList();
            if (d.PrismExe == null) rows.Add(new Row { State = St.Warn, Title = "Perfil Minecraft", Detail = "Depende do Prism." });
            else if (mine.Count == 0) rows.Add(new Row { State = St.Err, Title = "Perfil Minecraft", Detail = "Nenhum perfil de " + (role == "host" ? "anfitrião" : "convidado") + " com o mod. A aba INSTALAR cria o perfil. Se o Prism usa outra pasta, aponte a pasta instances.", ActionText = "Instalar", Action = GoInstall, ActionText2 = "Escolher pasta", Action2 = PickInstances });
            else { var cur = mine.FirstOrDefault(i => i.Id == instId) ?? mine[0]; rows.Add(new Row { State = St.Ok, Title = "Perfil Minecraft", Detail = cur.Name }); }
            // aviso (nao bloqueia o JOGAR): sem conta Microsoft no Prism o Minecraft nao abre
            if (d.PrismExe != null && mine.Count > 0 && !d.PrismAccount) rows.Add(new Row { State = St.Warn, Title = "Conta Microsoft", Detail = "Não achei uma conta no Prism. Sem ela o Minecraft não abre. A aba INSTALAR explica como entrar.", ActionText = "Entrar", Action = GoInstall });

            if (d.InstallDir == null) rows.Add(new Row { State = St.Err, Title = "Pacote co-op", Detail = "Ainda não instalado. A aba INSTALAR copia e instala a ponte (ou aponte uma instalação anterior com bridge-backups).", ActionText = "Instalar", Action = GoInstall, ActionText2 = "Escolher pasta", Action2 = PickInstall });
            else if (!d.UpToDate) rows.Add(new Row { State = St.Warn, Title = "Pacote co-op", Detail = "Desatualizado. O JOGAR atualiza sozinho, com backup.", ActionText = "Atualizar agora", Action = async () => await UpdateClicked() });
            else rows.Add(new Row { State = St.Ok, Title = "Pacote co-op", Detail = "windows.6, atualizado." });
            rows.Add(PassRow());
            return rows;
        }

        void RenderChecks()
        {
            if (passReady) { ImportIniPassIfNeeded(); UpdatePassUi(); }
            RenderRowList(F<StackPanel>("ChecksPanel"), MakeRows());
            UpdatePlayHints();
            RenderInstallPrereqs();
        }

        void UpdatePlayHints()
        {
            if (runMode || stopping) { F<TextBlock>("PressHint").Visibility = Visibility.Collapsed; if (runMode && !stopping) F<TextBlock>("PlaySub").Text = "fecha Elden Ring e Minecraft com segurança"; return; }
            var ready = !scanning && Missing().Count == 0;
            F<TextBlock>("PlaySub").Text = scanning ? "verificando seu PC…" : ready ? "abre Elden Ring e Minecraft" : "resolva os itens ao lado";
            F<TextBlock>("PressHint").Visibility = ready ? Visibility.Visible : Visibility.Hidden;
        }

        void RenderRowList(StackPanel p, List<Row> rows)
        {
            p.Children.Clear();
            for (int i = 0; i < rows.Count; i++)
            {
                var r = rows[i];
                var g = new Grid { Margin = new Thickness(0, 0, 0, 9) };
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(34) });
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                Brush c = r.State == St.Ok ? cOk : r.State == St.Warn ? cWarn : r.State == St.Err ? cErr : cGold;
                var ic = new Border { Width = 24, Height = 24, CornerRadius = new CornerRadius(0), BorderBrush = c, BorderThickness = new Thickness(2.5), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 1, 0, 0) };
                if (r.State == St.Loading)
                {
                    var sq = new Rectangle { Width = 8, Height = 8, Fill = cGoldB, RenderTransformOrigin = new Point(0.5, 0.5) };
                    var rt = new RotateTransform(); sq.RenderTransform = rt;
                    var spin = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromMilliseconds(800), RepeatBehavior = RepeatBehavior.Forever };
                    spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(0, KeyTime.FromPercent(0))); spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(45, KeyTime.FromPercent(0.25))); spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(90, KeyTime.FromPercent(0.5))); spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(135, KeyTime.FromPercent(0.75)));
                    rt.BeginAnimation(RotateTransform.AngleProperty, spin);
                    ic.Child = new Canvas { Width = 8, Height = 8, Children = { sq } };
                    ic.BorderBrush = cLine;
                }
                else ic.Child = new TextBlock { Text = r.State == St.Ok ? "\u2713" : r.State == St.Warn ? "!" : "\u2715", Foreground = c, FontSize = 12, FontWeight = FontWeights.Bold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                g.Children.Add(ic);
                var sp = new StackPanel(); Grid.SetColumn(sp, 1);
                sp.Children.Add(new TextBlock { Text = r.Title, FontSize = 14, FontWeight = FontWeights.SemiBold, Foreground = cText });
                sp.Children.Add(new TextBlock { Text = r.Detail, FontSize = 12, Foreground = r.State == St.Err ? B("#F0A090") : cMuted, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0) });
                if (r.Action != null)
                {
                    var act = r.Action;
                    var bp = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 7, 0, 0), HorizontalAlignment = HorizontalAlignment.Left };
                    var b = new Button { Content = r.ActionText, Style = (Style)Window.FindResource("GhostBtn"), HorizontalAlignment = HorizontalAlignment.Left, FontSize = 9 };
                    b.Padding = new Thickness(0); b.Click += (s, e) => act();
                    bp.Children.Add(b);
                    if (r.Action2 != null)
                    {
                        var act2 = r.Action2;
                        var b2 = new Button { Content = r.ActionText2, Style = (Style)Window.FindResource("GhostBtn"), Margin = new Thickness(8, 0, 0, 0), HorizontalAlignment = HorizontalAlignment.Left, FontSize = 9 };
                        b2.Padding = new Thickness(0); b2.Click += (s, e) => act2();
                        bp.Children.Add(b2);
                    }
                    sp.Children.Add(bp);
                }
                g.Children.Add(sp);
                g.Opacity = 0; g.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(220)) { BeginTime = TimeSpan.FromMilliseconds(i * 60) });
                p.Children.Add(g);
            }
        }

        void RenderInstPicker()
        {
            var p = F<StackPanel>("InstPanel"); p.Children.Clear();
            var mine = det.Insts.Where(i => i.Role == role).ToList();
            p.Visibility = mine.Count > 1 ? Visibility.Visible : Visibility.Collapsed;
            if (mine.Count <= 1) return;
            p.Children.Add(new TextBlock { Text = "Mais de um perfil encontrado. Escolha:", FontSize = 12, Foreground = cMuted, Margin = new Thickness(0, 0, 0, 6) });
            foreach (var i in mine)
            {
                var id = i.Id;
                var rb = new RadioButton { Content = i.Name, GroupName = "inst", Style = (Style)Window.FindResource("Chip"), Margin = new Thickness(0, 0, 0, 5), IsChecked = id == instId };
                rb.Checked += (s, e) => { instId = id; SaveCfg(); RenderChecks(); };
                p.Children.Add(rb);
            }
        }

        // ---------- seletores graficos ----------
        void PickGame()
        {
            using (var f = new WinForms.FolderBrowserDialog { Description = "Escolha a pasta ELDEN RING\\Game (onde fica o eldenring.exe)" })
            {
                if (f.ShowDialog() != WinForms.DialogResult.OK) return;
                var g = f.SelectedPath;
                var cand = new[] { g, Path.Combine(g, "Game"), Path.Combine(g, "ELDEN RING", "Game") }.FirstOrDefault(x => File.Exists(Path.Combine(x, "eldenring.exe")));
                if (cand == null) { ShowError("Pasta errada", "Não achei o eldenring.exe nessa pasta. Escolha a pasta \"Game\" dentro de ELDEN RING."); return; }
                cfg["GameDir"] = cand; SaveCfg(); var _ = Rescan();
            }
        }
        void PickPrism()
        {
            var o = new OpenFileDialog { Title = "Escolha o prismlauncher.exe", Filter = "Prism Launcher|prismlauncher.exe" };
            if (o.ShowDialog() == true) { cfg["PrismExe"] = o.FileName; SaveCfg(); var _ = Rescan(); }
        }
        void PickInstances()
        {
            using (var f = new WinForms.FolderBrowserDialog { Description = "Escolha a pasta \"instances\" do Prism (ela contém as pastas dos seus perfis)" })
                if (f.ShowDialog() == WinForms.DialogResult.OK) { cfg["InstancesDir"] = f.SelectedPath; SaveCfg(); var _ = Rescan(); }
        }
        void PickInstall()
        {
            using (var f = new WinForms.FolderBrowserDialog { Description = "Escolha a pasta EldenMinecraft-Windows da instalação anterior (a que contém bridge-backups)" })
            {
                if (f.ShowDialog() != WinForms.DialogResult.OK) return;
                if (!Directory.Exists(Path.Combine(f.SelectedPath, "bridge-backups"))) { ShowError("Pasta errada", "Essa pasta não tem \"bridge-backups\". Escolha a pasta EldenMinecraft-Windows da instalação anterior."); return; }
                cfg["InstallDir"] = f.SelectedPath; SaveCfg(); var _ = Rescan();
            }
        }
        void OpenUrl(string u) { try { Process.Start(new ProcessStartInfo(u) { UseShellExecute = true }); } catch { } }

        // ---------- processos ----------
        class PsResult { public int Code; public List<string> Lines = new List<string>(); }

        Task<PsResult> RunPs(string script, string argLine, int timeoutMs, Action<string> onLine = null)
        {
            return Task.Run(() =>
            {
                var res = new PsResult();
                var psi = new ProcessStartInfo("powershell.exe", "-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script + "\" " + argLine)
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8 };
                psi.EnvironmentVariables.Remove("PSModulePath");
                if (sandbox != null)
                {   // sandbox: os scripts nunca enxergam saves, players.json nem Documentos reais
                    psi.EnvironmentVariables["APPDATA"] = appDataDir; psi.EnvironmentVariables["LOCALAPPDATA"] = localDir; psi.EnvironmentVariables["USERPROFILE"] = homeDir;
                }
                using (var p = Process.Start(psi))
                {
                    DataReceivedEventHandler h = (s, e) => { if (e.Data != null) { lock (res.Lines) res.Lines.Add(e.Data); if (onLine != null) onLine(e.Data); else Log("  " + e.Data); } };
                    p.OutputDataReceived += h; p.ErrorDataReceived += h; p.BeginOutputReadLine(); p.BeginErrorReadLine();
                    if (!p.WaitForExit(timeoutMs)) { try { p.Kill(); } catch { } res.Code = -1; res.Lines.Add("Tempo esgotado."); }
                    else { p.WaitForExit(); res.Code = p.ExitCode; }
                }
                return res;
            });
        }

        // barras finais precisam ser dobradas: "D:\" sem isso vira D:" e engole o resto da linha de comando (biblioteca Steam na raiz do disco)
        static string Q(string s) { s = s.Replace("\"", ""); int n = s.Length - s.TrimEnd('\\').Length; return "\"" + s + new string('\\', n) + "\""; }
        static string FirstMsg(List<string> ls)
        {
            string m; lock (ls) m = ls.FirstOrDefault(l => l.Trim().Length > 0 && !l.StartsWith("+") && !l.StartsWith("At ") && !l.StartsWith("No ") && !l.StartsWith("   ")) ?? "Erro desconhecido.";
            if (m.IndexOf("Start Steam", StringComparison.OrdinalIgnoreCase) >= 0) return "A Steam precisa estar aberta e com login feito (online).";
            return m;
        }
        // ---------- validacao ----------
        List<string> Missing()
        {
            var m = new List<string>();
            if (det.SteamDir == null) m.Add("Steam não encontrada.");
            else if (!det.SteamRunning && !dry) m.Add("A Steam está fechada. Abra a Steam, faça login e clique em JOGAR de novo.");
            if (det.GameDir == null) m.Add("Elden Ring não encontrado. Use \"Escolher pasta\" ao lado.");
            else if (det.GameVer != "2.7.1.0") m.Add("Elden Ring na versão " + (det.GameVer ?? "?") + ". O co-op só funciona na 2.7.1.0.");
            else if (!det.Seamless) m.Add("Seamless Co-op v2.0.1 não instalado. Use a aba INSTALAR.");
            if (det.PrismExe == null) m.Add("Prism Launcher não encontrado. Use a aba INSTALAR.");
            else if (!det.Insts.Any(i => i.Role == role)) m.Add("Nenhum perfil Minecraft de " + (role == "host" ? "anfitrião" : "convidado") + " encontrado no Prism. Use a aba INSTALAR.");
            if (det.InstallDir == null) m.Add("Pacote co-op não instalado. Use a aba INSTALAR (ou aponte uma instalação anterior).");
            return m;
        }

        // ---------- atualizar ----------
        async Task UpdateClicked()
        {
            if (busy) return; busy = true; HideError();
            try { await DoUpdate(); } finally { busy = false; }
        }

        async Task<bool> DoUpdate()
        {
            if (det.InstallDir == null) { ShowError("Sem instalação anterior", "Não achei a pasta EldenMinecraft-Windows com bridge-backups. Escolha-a no item \"Pacote co-op\"."); return false; }
            Say(dry ? "Simulando atualização (nenhum arquivo será alterado)\u2026" : "Atualizando o pacote com backup\u2026");
            var script = Path.Combine(packDir, "launcher", "Run-Update.ps1");
            var a = "-InstalledPackage " + Q(det.InstallDir) + (det.InstancesDir != null ? " -InstancesDirectory " + Q(det.InstancesDir) : "") + (dry ? " -DryRun" : "");
            if (sandbox != null && det.GameDir != null) a += " -GameDir " + Q(det.GameDir) + " -SteamPath " + Q(SteamRoot());
            var r = await RunPs(script, a, 180000);
            var err = r.Lines.FirstOrDefault(l => l.StartsWith("ERROR|"));
            var ok = r.Lines.FirstOrDefault(l => l.StartsWith("RESULT|"));
            if (r.Code != 0 || err != null || ok == null)
            {
                var parts = (err ?? "ERROR||" + (r.Lines.LastOrDefault() ?? "falha desconhecida")).Split(new[] { '|' }, 3);
                ShowError("Não consegui atualizar", Explain(parts.Length > 1 ? parts[1] : "", parts.Length > 2 ? parts[2] : "") + "\nNada foi quebrado: o atualizador desfaz tudo se algo falha. Veja o Registro.");
                return false;
            }
            Say(dry ? "Simulação concluída: a atualização seria aplicada com backup." : "Pacote atualizado. Backup guardado em bridge-backups.");
            if (!dry) await Rescan();
            return true;
        }

        string Explain(string code, string msg)
        {
            if (code == "BridgeProfilesNotFound") return "Nenhum perfil Minecraft com o mod foi encontrado. Use \"Escolher pasta\" no item Perfil Minecraft e aponte a pasta instances do Prism.";
            if (msg.IndexOf("abert", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("running", StringComparison.OrdinalIgnoreCase) >= 0) return "Feche o Elden Ring e o Minecraft antes (botão PARAR ou \"Fechar tudo\") e tente de novo.";
            if (msg.IndexOf("hash", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("SHA", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("conhecid", StringComparison.OrdinalIgnoreCase) >= 0) return "Há arquivos modificados ou de uma versão antiga demais. Por segurança o launcher não força a troca. Detalhe: " + msg;
            return msg.Length > 0 ? msg : "Erro desconhecido.";
        }

        // ---------- jogar ----------
        async Task PlayClicked()
        {
            if (busy) return; busy = true;
            var btn = F<Button>("BtnPlay"); btn.IsEnabled = false; HideError();
            try { await DoPlay(); }
            catch (Exception ex) { SetStep(Math.Max(stepIndex, 0), "x"); ShowError("Algo deu errado", ex.Message); }
            finally { busy = false; btn.IsEnabled = true; }
            PollGames();
        }

        async Task DoPlay()
        {
            if (simDir != null) { await DoPlaySim(); return; }
            SetStep(0); Say("Verificando seu PC\u2026");
            det = await Task.Run(() => Detect());
            Window.Dispatcher.Invoke(new Action(() => { RenderChecks(); RenderInstPicker(); }));
            var miss = Missing();
            if (miss.Count > 0) { SetStep(0, "x"); Say("Faltam itens para jogar."); ShowError("Ainda não dá para jogar", string.Join("\n", miss.ToArray())); return; }
            var open0 = await Task.Run(() => FindGames());
            if (open0.Any(g => g.Kind == "er"))
            { SetStep(0, "x"); ShowError("Elden Ring já está aberto", "Clique em PARAR (ou Fechar tudo) e depois em JOGAR de novo. O co-op precisa abrir os dois jogos do zero."); return; }
            if ((!dry || sandbox != null) && open0.Any(g => g.Kind == "mc"))
            { SetStep(0, "x"); ShowError("Já existe um Java aberto", "Pode ser o Minecraft. Use \"Fechar tudo\" (ele salva o mundo) e tente de novo."); return; }
            if (!det.UpToDate && dry && args.Contains("--skip-update")) Log("[simulação] atualização ignorada (--skip-update)."); else if (!det.UpToDate) { Say("Pacote desatualizado. Atualizando antes de jogar\u2026"); if (!await DoUpdate()) { SetStep(0, "x"); return; } }
            await Task.Delay(500);

            // senha da sessao: o ini do Seamless precisa estar com a senha SALVA antes de abrir (senao os dois jogadores ficam com senhas diferentes)
            if (PassDirty()) { SetStep(0, "x"); ShowError("Senha ainda n\u00e3o salva", "Voc\u00ea mudou a senha no painel SENHA DO CO-OP mas n\u00e3o clicou em SALVAR. Salve (ou apague o campo) e clique em JOGAR de novo, sen\u00e3o voc\u00eas dois ficariam com senhas diferentes."); return; }
            if (savedPass != null && IniState() != IniSt.Igual)
            {
                SetStep(1); Say("Gravando a senha salva no Seamless\u2026");
                var pr = await ApplyPassToIni();
                if (pr.Kind == "err") { SetStep(1, "x"); ShowError("N\u00e3o consegui gravar a senha no Seamless", pr.Msg + "\nO Elden Ring n\u00e3o foi aberto para voc\u00eas n\u00e3o ficarem com senhas diferentes."); return; }
                Window.Dispatcher.Invoke(new Action(() => RenderChecks()));
            }
            else if (savedPass == null) Log("Aviso: nenhuma senha do co-op salva. Os dois jogadores precisam combinar a mesma senha (painel SENHA DO CO-OP).");

            // 2) Elden Ring (Seamless, sem EAC)
            SetStep(1); Say("Abrindo o Elden Ring (Seamless Co-op)\u2026");
            var startCoop = Path.Combine(det.InstallDir, "elden-ring", "windows", "Start-Coop.ps1");
            var sargs = "-GameDir " + Q(det.GameDir);
            if (dry) { Log("[simulação] powershell -File " + Q(startCoop) + " " + sargs); await Task.Delay(1100); }
            else
            {
                var r = await RunPs(startCoop, sargs, 90000);
                if (r.Code != 0) { SetStep(1, "x"); ShowError("O Elden Ring não abriu", Explain("", FirstMsg(r.Lines)) + "\nO Elden Ring é aberto pelo Seamless (sem anti-cheat). Veja o Registro."); return; }
                lock (r.Lines) foreach (var ln in r.Lines) { var pm = Regex.Match(ln, @"ProcessId\s*:?\s*(\d+)"); if (pm.Success) lock (tracked) tracked.Add(int.Parse(pm.Groups[1].Value)); }   // ersc_launcher aberto pelo Start-Coop
                Say("Esperando o Elden Ring subir\u2026");
                bool up = false;
                for (int i = 0; i < 120 && !up; i++) { up = (await Task.Run(() => FindGames())).Any(g => g.Kind == "er"); if (!up) await Task.Delay(1000); }
                if (!up) { SetStep(1, "x"); ShowError("O Elden Ring demorou demais", "Não vi o jogo abrir em 2 minutos. Confira a Steam e tente de novo."); return; }
            }

            // 3) Minecraft
            SetStep(2);
            var inst = det.Insts.First(i => i.Id == instId || (instId == null && i.Role == role));
            Say("Abrindo o Minecraft (" + inst.Name + ")\u2026");
            var server = F<TextBox>("ServerBox").Text.Trim();
            var pa = "--dir " + Q(det.PrismData) + " --launch " + Q(inst.Id) + (role == "guest" && Regex.IsMatch(server, @"^[A-Za-z0-9\.\-:]+$") ? " --server " + server : "");
            if (dry) { Log("[simulação] " + Q(det.PrismExe) + " " + pa); await Task.Delay(1100); }
            else { var pp = Process.Start(new ProcessStartInfo(det.PrismExe, pa) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(det.PrismExe) }); if (pp != null) lock (tracked) tracked.Add(pp.Id); }

            SetStep(3);
            Say(role == "host"
                ? "Tudo aberto. No Seamless escolha o personagem e inicie a sessão com a senha combinada. Depois abra o mundo e passe o endereço e4mc."
                : "Tudo aberto. Escolha o personagem \"Steve\", entre na sessão do anfitrião e conecte o Minecraft pelo endereço dele.");
            UpdatePills();
        }

        // ---------- jogos em execucao (RODANDO / PARAR) ----------
        class GProc { public int Pid; public int Ppid; public string Name; public string Kind; public string Path; public long Start; public string Cmd; }
        string simDir, simStubborn;                               // --simulate-games <pasta com os jogos FALSOS, dentro da sandbox>, --sim-stubborn er|mc|both
        readonly HashSet<int> tracked = new HashSet<int>();       // PIDs que o launcher abriu (Start-Coop, Prism); os filhos entram pela arvore
        List<GProc> live = new List<GProc>();
        bool runMode, polling, stopping;
        TaskCompletionSource<bool> confTcs;
        const int ErGraceMs = 10000, McGraceMs = 20000;           // espera pelo fechamento gracioso antes de escalar

        bool InSandbox(string p) { return sandbox != null && p != null && p.StartsWith(sandbox.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase); }

        // Unico detector de jogos abertos (pills, JOGAR, adocao ao abrir o launcher, PARAR).
        // Real: eldenring.exe + ersc_launcher.exe, e java/javaw com o criterio do Minecraft/Prism/bridge.
        // --sandbox: SO processos cujo executavel fica dentro da pasta da sandbox (nunca enxerga os jogos reais).
        List<GProc> FindGames()
        {
            var all = new List<GProc>();
            try
            {
                using (var q = new System.Management.ManagementObjectSearcher("SELECT ProcessId, ParentProcessId, Name, CommandLine, ExecutablePath, CreationDate FROM Win32_Process WHERE Name='eldenring.exe' OR Name='ersc_launcher.exe' OR Name='java.exe' OR Name='javaw.exe' OR Name='prismlauncher.exe'"))
                    foreach (System.Management.ManagementObject o in q.Get())
                    {
                        var g = new GProc { Pid = Convert.ToInt32(o["ProcessId"]), Ppid = Convert.ToInt32(o["ParentProcessId"]), Name = ((o["Name"] as string) ?? "").ToLowerInvariant(), Cmd = (o["CommandLine"] as string) ?? "", Path = o["ExecutablePath"] as string };
                        try { var cd = o["CreationDate"] as string; if (cd != null) g.Start = System.Management.ManagementDateTimeConverter.ToDateTime(cd).ToUniversalTime().Ticks; } catch { }
                        all.Add(g);
                    }
            }
            catch (Exception ex) { Log("Aviso: não consegui listar os processos (" + ex.Message + ")."); }
            if (sandbox != null) all = all.Where(g => InSandbox(g.Path)).ToList();
            HashSet<int> trk; lock (tracked) trk = new HashSet<int>(tracked);
            var res = new List<GProc>();
            foreach (var g in all)
            {
                var c = g.Cmd.ToLowerInvariant();
                if (g.Name == "eldenring.exe" || g.Name == "ersc_launcher.exe") g.Kind = "er";
                else if ((g.Name == "java.exe" || g.Name == "javaw.exe") && (c.Contains("net.minecraft") || c.Contains("prismlauncher") || c.Contains("-derbridge") || c.Contains("--gamedir") || trk.Contains(g.Pid) || trk.Contains(g.Ppid))) g.Kind = "mc";
                if (g.Kind != null) res.Add(g);
            }
            return res;
        }

        // monitor leve: WMI fora da thread da interface, a cada 2 s
        async void PollGames()
        {
            if (polling) return; polling = true;
            try { var l = await Task.Run(() => FindGames()); ApplyGames(l); }
            catch (Exception ex) { Log("Monitor: " + ex.Message); }
            finally { polling = false; }
        }
        void UpdatePills() { PollGames(); }

        void ApplyGames(List<GProc> l)
        {
            live = l;
            bool er = l.Any(g => g.Kind == "er"), mc = l.Any(g => g.Kind == "mc");
            F<Rectangle>("DotEr").Fill = er ? cOk : cOff; F<TextBlock>("TxtEr").Text = "Elden Ring · " + (er ? "aberto" : "fechado");
            F<Rectangle>("DotMc").Fill = mc ? cOk : cOff; F<TextBlock>("TxtMc").Text = "Minecraft · " + (mc ? "aberto" : "fechado");
            if (!busy && !stopping) SetRunMode(er, mc);
        }

        void SetRunMode(bool er, bool mc)
        {
            bool any = er || mc;
            if (any)
            {
                F<TextBlock>("RunText").Text = er && mc ? "RODANDO" : er ? "RODANDO: Elden Ring" : "RODANDO: Minecraft";
                if (!runMode)
                {
                    runMode = true; ApplyRunUi();
                    if (stepIndex != 3 || stepFail != null) { SetStep(3); Say(er && mc ? "Elden Ring e Minecraft estão rodando. Para encerrar os dois, clique em PARAR." : er ? "O Elden Ring está rodando. Para encerrar, clique em PARAR." : "O Minecraft está rodando. Para encerrar, clique em PARAR."); }
                    Log("Jogos rodando: " + (er ? "Elden Ring " : "") + (mc ? "Minecraft" : ""));
                }
            }
            else if (runMode)
            {
                runMode = false; lock (tracked) tracked.Clear(); ApplyRunUi(); SetStep(-1);
                Say("Os jogos foram fechados. Pronto para jogar de novo."); Log("Jogos fechados: botão voltou a JOGAR.");
            }
        }

        // aparencia do botao grande: JOGAR (dourado) <-> PARAR (perigo) + selo RODANDO pulsando no lugar do APERTE PARA COMECAR
        void ApplyRunUi()
        {
            var b = F<Button>("BtnPlay");
            b.Style = (Style)Window.FindResource(runMode ? "DangerBtn" : "GoldBtn");
            var pt = F<TextBlock>("PlayText"); pt.Text = runMode ? "PARAR" : "JOGAR"; pt.FontSize = 32;
            System.Windows.Automation.AutomationProperties.SetName(b, runMode ? "Parar: fechar Elden Ring e Minecraft" : "Jogar");
            var badge = F<StackPanel>("RunBadge"); badge.Visibility = runMode ? Visibility.Visible : Visibility.Collapsed;
            if (runMode)
            {
                var pulse = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromMilliseconds(1200), RepeatBehavior = RepeatBehavior.Forever };
                pulse.KeyFrames.Add(new DiscreteDoubleKeyFrame(1, KeyTime.FromPercent(0))); pulse.KeyFrames.Add(new DiscreteDoubleKeyFrame(0.3, KeyTime.FromPercent(0.55)));
                badge.BeginAnimation(UIElement.OpacityProperty, pulse);
            }
            else { badge.BeginAnimation(UIElement.OpacityProperty, null); badge.Opacity = 1; }
            F<Border>("Halo1").Background = B(runMode ? "#33E5654A" : "#33E2BC5C"); F<Border>("Halo2").Background = B(runMode ? "#22A63A28" : "#22C8A24A");
            UpdatePlayHints();
        }

        // ---------- confirmacao no tema (overlay dentro da janela) ----------
        Task<bool> Confirm(string title, string body, string yes, string no)
        {
            ResolveConfirm(false);
            confTcs = new TaskCompletionSource<bool>();
            F<TextBlock>("ConfTitle").Text = title; F<TextBlock>("ConfBody").Text = body;
            F<Button>("BtnConfYes").Content = yes; F<Button>("BtnConfNo").Content = no;
            var l = F<Grid>("ConfirmLayer"); l.Visibility = Visibility.Visible;
            l.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(160)));
            F<Button>("BtnConfNo").Focus();
            return confTcs.Task;
        }
        void ResolveConfirm(bool v)
        {
            var t = confTcs; if (t == null) return; confTcs = null;
            F<Grid>("ConfirmLayer").Visibility = Visibility.Collapsed; t.TrySetResult(v);
        }

        // ---------- fechar os jogos ----------
        Process OpenProc(GProc g)
        {
            try
            {
                if (sandbox != null && !InSandbox(g.Path)) return null;   // em teste nunca toca em processo fora da sandbox
                var p = Process.GetProcessById(g.Pid);
                if (p.HasExited) { p.Dispose(); return null; }
                if (g.Start != 0 && Math.Abs(p.StartTime.ToUniversalTime().Ticks - g.Start) > 3 * TimeSpan.TicksPerSecond) { p.Dispose(); return null; }   // PID reaproveitado por outro programa
                return p;
            }
            catch { return null; }
        }
        bool AnyAlive(List<GProc> l) { foreach (var g in l) using (var p = OpenProc(g)) if (p != null) return true; return false; }
        async Task<bool> WaitGone(List<GProc> l, int ms)
        {
            var t0 = Environment.TickCount;
            while (true)
            {
                if (!AnyAlive(l)) return true;
                if (unchecked(Environment.TickCount - t0) >= ms) return false;
                await Task.Delay(250);
            }
        }
        void CloseWins(List<GProc> l) { foreach (var g in l) using (var p = OpenProc(g)) if (p != null) { try { p.CloseMainWindow(); } catch { } } }
        void KillAll(List<GProc> l)
        {
            foreach (var g in l) using (var p = OpenProc(g)) if (p != null)
            {
                try { p.Kill(); Log("  Forçado a fechar: " + g.Name + " (PID " + g.Pid + ")."); }
                catch (Exception ex) { Log("  Não consegui forçar " + g.Name + " (PID " + g.Pid + "): " + ex.Message); }
            }
        }
        static string Names(List<GProc> l) { return string.Join(", ", l.Select(g => g.Name.Replace(".exe", "") + " (PID " + g.Pid + ")").ToArray()); }

        // PARAR (botao grande) e "Fechar tudo" (rodape) caem aqui: o mesmo fluxo, com confirmacao.
        async Task StopAll()
        {
            if (busy) return;
            var snap = await Task.Run(() => FindGames());
            bool er0 = snap.Any(g => g.Kind == "er"), mc0 = snap.Any(g => g.Kind == "mc");
            if (!er0 && !mc0) { ApplyGames(snap); Say("Não há Elden Ring nem Minecraft abertos."); return; }
            busy = true; HideError();
            bool cancelled = false;
            try
            {
                var what = er0 && mc0 ? "Elden Ring e Minecraft" : er0 ? "Elden Ring" : "Minecraft";
                var ok = await Confirm("FECHAR TUDO?", "Fechar " + what + "? Progresso não salvo se perde. O Elden Ring salva sozinho, mas saia no jogo se puder.", "Fechar tudo", "Cancelar");
                if (!ok) { cancelled = true; Say("Cancelado. Nada foi fechado."); }
                else { stopping = true; await DoStop(); }
            }
            catch (Exception ex) { ShowError("Algo deu errado ao fechar", ex.Message); }
            var fin = await Task.Run(() => FindGames());
            busy = false; stopping = false;
            F<Button>("BtnPlay").IsEnabled = true;
            if (!cancelled) ApplyRunUi();
            ApplyGames(fin);
        }

        async Task DoStop()
        {
            F<Button>("BtnPlay").IsEnabled = false;
            var pt = F<TextBlock>("PlayText"); var ps = F<TextBlock>("PlaySub");
            pt.FontSize = 22; pt.Text = "FECHANDO…"; F<TextBlock>("PressHint").Visibility = Visibility.Collapsed;
            if (dry && sandbox == null) { Say("[simulação] Fecharia o Minecraft (Close-Minecraft.ps1) e depois o Elden Ring (nada foi fechado)."); await Task.Delay(900); return; }
            var snap = await Task.Run(() => FindGames());
            var mcs = snap.Where(g => g.Kind == "mc").ToList();
            var ers = snap.Where(g => g.Kind == "er").ToList();

            // 1) Minecraft: gracioso (salva o mundo) -> janela -> forca
            if (mcs.Count > 0)
            {
                ps.Text = "fechando o Minecraft…"; Say("Fechando o Minecraft (o mundo é salvo)…");
                var script = Path.Combine(packDir, "EldenMinecraft-Windows", "elden-ring", "windows", "Close-Minecraft.ps1");
                var ca = sandbox != null ? "-IpcDir " + Q(Path.Combine(homeDir, "Documents", "EldenMinecraft", "ipc")) : "";
                var r = await RunPs(script, ca, 30000);
                if (r.Code != 0) Log("Close-Minecraft: " + FirstMsg(r.Lines));
                bool gone = await WaitGone(mcs, McGraceMs);
                if (!gone) { Say("O Minecraft não respondeu ao pedido de salvar e sair. Fechando a janela…"); CloseWins(mcs); gone = await WaitGone(mcs, 10000); }
                if (!gone) { Say("O Minecraft não fechou. Forçando o encerramento…"); KillAll(mcs); gone = await WaitGone(mcs, 4000); }
                Log(gone ? "Minecraft fechado." : "Minecraft continua aberto: " + Names(mcs.Where(g => { using (var p = OpenProc(g)) return p != null; }).ToList()));
            }

            // 2) Elden Ring: CloseMainWindow, espera ate ~10 s, so entao Kill
            ers = (await Task.Run(() => FindGames())).Where(g => g.Kind == "er").ToList();
            if (ers.Count > 0)
            {
                ps.Text = "fechando o Elden Ring…"; Say("Fechando o Elden Ring…");
                CloseWins(ers);
                var main = ers.Where(g => g.Name == "eldenring.exe").ToList();
                var t0 = Environment.TickCount; long mainGoneAt = main.Count == 0 ? t0 : 0;
                bool gone = false;
                while (!gone)
                {
                    gone = !AnyAlive(ers);
                    if (gone) break;
                    int el = unchecked(Environment.TickCount - t0);
                    if (mainGoneAt == 0 && !AnyAlive(main)) mainGoneAt = Environment.TickCount;
                    if (mainGoneAt != 0 && unchecked(Environment.TickCount - (int)mainGoneAt) > 2500) break;   // o jogo ja saiu; sobra so o launcher do Seamless sem janela
                    if (el >= ErGraceMs) break;
                    await Task.Delay(250);
                }
                if (!gone) { Say("O Elden Ring não fechou sozinho. Forçando o encerramento…"); ps.Text = "forçando o Elden Ring…"; KillAll(ers); gone = await WaitGone(ers, 4000); }
                Log(gone ? "Elden Ring fechado." : "Elden Ring continua aberto.");
            }

            // 3) confirma de verdade
            ps.Text = "conferindo…"; await Task.Delay(500);
            var left = (await Task.Run(() => FindGames())).Where(g => g.Kind == "er" || g.Kind == "mc").ToList();
            if (left.Count == 0) { lock (tracked) tracked.Clear(); SetStep(-1); Say("Tudo fechado. Pronto para jogar de novo."); }
            else
            {
                ShowError("Algo não quis fechar", "Ainda abertos: " + Names(left) + ".\nFeche pelo menu do jogo (Sair do jogo; no Minecraft, Salvar e sair) e clique em PARAR de novo se precisar. O launcher não derruba o que não consegue fechar com segurança.");
                Say("Alguns processos continuam abertos. Veja o aviso.");
            }
        }

        // ---------- teste: abre jogos FALSOS (so com --sandbox --simulate-games) ----------
        void SpawnFake(string exe)
        {
            var psi = new ProcessStartInfo(Path.Combine(simDir, exe), simStubborn != null ? "--stubborn=" + simStubborn : "") { UseShellExecute = false, WorkingDirectory = simDir, CreateNoWindow = true };
            psi.EnvironmentVariables["USERPROFILE"] = homeDir; psi.EnvironmentVariables["APPDATA"] = appDataDir; psi.EnvironmentVariables["LOCALAPPDATA"] = localDir;
            var p = Process.Start(psi); lock (tracked) tracked.Add(p.Id);
            Log("[teste] " + exe + " falso aberto (PID " + p.Id + ").");
        }
        async Task DoPlaySim()
        {
            SetStep(0); Say("[teste] Verificando…");
            var cur = await Task.Run(() => FindGames());
            if (cur.Any(g => g.Kind == "er")) { SetStep(0, "x"); ShowError("Elden Ring já está aberto", "Clique em PARAR (ou Fechar tudo) e depois em JOGAR de novo."); return; }
            if (cur.Any(g => g.Kind == "mc")) { SetStep(0, "x"); ShowError("Já existe um Java aberto", "Use \"Fechar tudo\" e tente de novo."); return; }
            await Task.Delay(500);
            SetStep(1); Say("[teste] Abrindo o Elden Ring falso…"); SpawnFake("ersc_launcher.exe");
            bool up = false;
            for (int i = 0; i < 50 && !up; i++) { up = (await Task.Run(() => FindGames())).Any(g => g.Name == "eldenring.exe"); if (!up) await Task.Delay(200); }
            SetStep(2); Say("[teste] Abrindo o Minecraft falso…"); SpawnFake("prismlauncher.exe");
            await Task.Delay(800);
            SetStep(3);
            Say(role == "host"
                ? "Tudo aberto. No Seamless escolha o personagem e inicie a sessão com a senha combinada. Depois abra o mundo e passe o endereço e4mc."
                : "Tudo aberto. Escolha o personagem \"Steve\", entre na sessão do anfitrião e conecte o Minecraft pelo endereço dele.");
        }

        // ---------- como funciona ----------
        TextBlock Tb(string t, double size, Brush fg, FontWeight? w = null, string font = null, Thickness? m = null)
        {
            var x = new TextBlock { Text = t, FontSize = size, Foreground = fg, TextWrapping = TextWrapping.Wrap };
            if (w.HasValue) x.FontWeight = w.Value; if (font == "Pixel") { x.FontFamily = pixelFF ?? new FontFamily("Consolas"); TextOptions.SetTextFormattingMode(x, TextFormattingMode.Display); } else if (font != null) x.FontFamily = new FontFamily(font); if (m.HasValue) x.Margin = m.Value; return x;
        }

        UIElement Heading(string t)
        {
            var sp = new StackPanel { Margin = new Thickness(0, 28, 0, 12) };
            sp.Children.Add(Tb(t, 14, cGoldB, FontWeights.Normal, "Pixel"));
            sp.Children.Add(new Rectangle { Height = 3, Fill = cLine, Margin = new Thickness(0, 10, 0, 0) });
            return sp;
        }

        UIElement StepCard(int n, string title, string body)
        {
            var b = new Border { Background = B("#C8140F0A"), BorderBrush = B("#5A4824"), BorderThickness = new Thickness(2, 2, 4, 4), CornerRadius = new CornerRadius(0), Padding = new Thickness(16), Margin = new Thickness(0, 0, 0, 10) };
            var g = new Grid(); g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(52) }); g.ColumnDefinitions.Add(new ColumnDefinition());
            g.Children.Add(Tb(n.ToString(), 32, cGold, FontWeights.Normal, "Pixel"));
            var sp = new StackPanel(); Grid.SetColumn(sp, 1);
            sp.Children.Add(Tb(title, 15, cText, FontWeights.SemiBold)); sp.Children.Add(Tb(body, 13, cMuted, null, null, new Thickness(0, 4, 0, 0)));
            g.Children.Add(sp); b.Child = g; return b;
        }

        UIElement KeyRow(string key, string title, string body)
        {
            var g = new Grid { Margin = new Thickness(0, 0, 0, 10) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(76) }); g.ColumnDefinitions.Add(new ColumnDefinition());
            var cap = new Border { Width = 56, Height = 50, CornerRadius = new CornerRadius(0), Background = B("#2A2114"), BorderBrush = cGold, BorderThickness = new Thickness(2, 2, 2, 6), HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
            cap.Child = new TextBlock { Text = key, FontFamily = pixelFF ?? new FontFamily("Consolas"), FontSize = 16, Foreground = cGoldB, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
            g.Children.Add(cap);
            var sp = new StackPanel { VerticalAlignment = VerticalAlignment.Center }; Grid.SetColumn(sp, 1);
            sp.Children.Add(Tb(title, 15, cText, FontWeights.SemiBold)); sp.Children.Add(Tb(body, 13, cMuted, null, null, new Thickness(0, 2, 0, 0)));
            g.Children.Add(sp); return g;
        }

        UIElement Fix(string sym, string fix)
        {
            var g = new Grid { Margin = new Thickness(0, 0, 0, 9) };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(4) }); g.ColumnDefinitions.Add(new ColumnDefinition());
            g.Children.Add(new Rectangle { Fill = cWarn, Margin = new Thickness(0, 2, 0, 2) });
            var sp = new StackPanel { Margin = new Thickness(14, 0, 0, 0) }; Grid.SetColumn(sp, 1);
            sp.Children.Add(Tb(sym, 14, cText, FontWeights.SemiBold)); sp.Children.Add(Tb(fix, 13, cMuted, null, null, new Thickness(0, 2, 0, 0)));
            g.Children.Add(sp); return g;
        }

        void BuildHelp()
        {
            var p = F<StackPanel>("HelpPanel");
            p.Children.Add(Tb("COMO FUNCIONA", 24, cGoldB, FontWeights.Normal, "Pixel", new Thickness(0, 22, 0, 0)));
            p.Children.Add(Tb("Você controla o corpo do Minecraft dentro do Elden Ring, junto com um amigo. O launcher abre tudo e confere se está certo.", 14, cMuted, null, null, new Thickness(0, 6, 0, 0)));

            p.Children.Add(Heading("Em 4 passos"));
            p.Children.Add(StepCard(1, "Confira os Preparativos", "Tudo verde na aba Jogar. Se algo estiver vermelho, abra a aba INSTALAR: ela instala Prism, perfil, ponte e Seamless sozinha, passo a passo. Quem já tem tudo pode ignorar essa aba."));
            p.Children.Add(StepCard(2, "Escolha seu papel", "Um jogador é o Anfitrião nos dois jogos e o outro é o Convidado. Os dois PCs precisam estar na mesma versão (windows.6)."));
            p.Children.Add(StepCard(3, "Clique em JOGAR", "O launcher atualiza o pacote se precisar (com backup), abre o Elden Ring pelo Seamless (sem anti-cheat) e depois o Minecraft no perfil certo. Enquanto os jogos estiverem abertos, o botão mostra RODANDO e vira PARAR."));
            p.Children.Add(StepCard(4, "Entrem na partida", "Anfitrião: escolhe o personagem de teste no Seamless, inicia a sessão com a senha combinada, abre o mundo no Minecraft e passa o endereço e4mc para o amigo.\nConvidado: escolhe só o personagem \"Steve\", entra na sessão e, no Minecraft, usa Multiplayer > Conexão direta com esse endereço (pode colar no launcher antes de jogar).\nComecem juntos, em chão firme de Limgrave."));

            p.Children.Add(Heading("Controles"));
            p.Children.Add(KeyRow("F8", "Alterna o jogo controlado", "Troca entre Minecraft e Elden Ring. Use para menus, missões, inventário e mapa do Elden Ring."));
            p.Children.Add(KeyRow("R", "Interagir (modo Minecraft)", "Aciona a interação do Elden Ring enquanto você joga no corpo do Minecraft."));
            p.Children.Add(KeyRow("E", "Inventário do Minecraft", "Abre o inventário normal do Minecraft."));

            p.Children.Add(Heading("Regras de ouro"));
            p.Children.Add(Tb("\u2022  Os dois PCs na mesma versão. Misturar versões é recusado.\n\u2022  Alcance cooperativo de 64 m, na mesma zona do jogo.\n\u2022  O comando /kill é destrutivo: perde itens e runas. Só para teste deliberado.\n\u2022  Mundos, saves e contas nunca são tocados pelo launcher. Só os arquivos do mod são trocados, sempre com backup. A única senha que ele grava é a da sessão do co-op (painel SENHA DO CO-OP), no ini do Seamless, com o jogo fechado.\n\u2022  Vocês dois precisam salvar a MESMA senha do co-op. Senhas diferentes = o Seamless não conecta.", 13.5, cText));

            p.Children.Add(Heading("Se der problema"));
            p.Children.Add(Fix("Botão JOGAR diz que faltam itens", "Leia o quadro vermelho: ele lista exatamente o que falta (Steam fechada, jogo em versão errada, perfil não achado...)."));
            p.Children.Add(Fix("Não achou meu Elden Ring ou Prism", "Use o botão \"Escolher pasta\" no item vermelho. O launcher lembra da escolha."));
            p.Children.Add(Fix("Atualização recusou o pacote", "Arquivos modificados ou de versão antiga demais não são trocados à força. Feche os dois jogos e veja o Registro na aba Jogar."));
            p.Children.Add(Fix("Elden Ring já aberto", "Clique em PARAR (o botão grande vira PARAR enquanto os jogos estão abertos) ou feche pelo menu do jogo (Sair do jogo). O co-op precisa abrir os dois jogos do zero pelo launcher."));
            p.Children.Add(Fix("Quero parar tudo", "Clique em PARAR (o botão grande, enquanto os jogos estão abertos) ou em \"Fechar tudo\" no rodapé: os dois fazem o mesmo. O launcher pede confirmação, fecha o Minecraft primeiro (salvando o mundo) e depois o Elden Ring, esperando até 10 s antes de forçar. Se puder, saia pelo menu do Elden Ring antes: o jogo salva sozinho, mas assim você escolhe o momento. Só fecha o Elden Ring, o Seamless e o Java do Minecraft; o Prism e a Steam continuam abertos."));
            p.Children.Add(Heading("Sobre e créditos"));
            p.Children.Add(Tb("\u2022  Música (opcional): \"(8bit) ELDEN RING - Main Theme / Short Version (Chiptune Cover)\", do canal \"I'm GearRabbit, make Chiptune / 8bit cover\" no YouTube (youtube.com/watch?v=Kl4-HAepMtM). Direitos dos autores originais; o arquivo não é distribuído com o projeto.\n\u2022  Fonte: Press Start 2P (SIL Open Font License 1.1).\n\u2022  Co-op: Seamless Co-op, de LukeYui, que o launcher baixa do release oficial do autor no seu PC (não é redistribuído aqui). Código-base da ponte: justbustin/minecraft-crossover-bridge (MIT). Veja THIRD-PARTY-NOTICES.txt.\n\u2022  Projeto de fã, não afiliado a FromSoftware, Bandai Namco, Mojang ou Microsoft. Elden Ring e Minecraft pertencem aos seus donos.", 13, cText));
            p.Children.Add(Tb("Registro de atividade: %LOCALAPPDATA%\\EldenMinecraftLauncher\\launcher.log", 11.5, cMuted, null, "Consolas", new Thickness(0, 10, 0, 0)));
        }

        // ---------- screenshot ----------
        void Shot(string path)
        {
            var el = (FrameworkElement)Window.Content;
            var w = (int)Window.ActualWidth; var h = (int)Window.ActualHeight;
            var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
            var dv = new DrawingVisual();
            using (var dc = dv.RenderOpen()) { dc.DrawRectangle(B("#050403"), null, new Rect(0, 0, w, h)); dc.DrawRectangle(new VisualBrush(el), null, new Rect(0, 0, w, h)); }
            rtb.Render(dv);
            var enc = new PngBitmapEncoder(); enc.Frames.Add(BitmapFrame.Create(rtb));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
            using (var f = File.Create(path)) enc.Save(f);
        }
    }
}
