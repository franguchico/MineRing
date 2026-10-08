// Apelidos (nick) e UUIDs publicos de Minecraft do anfitriao e do convidado.
// Ficam so no PC do usuario: %LOCALAPPDATA%\EldenMinecraftLauncher\players.json (nada pessoal no repositorio).
using System;
using System.IO;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace EldenMinecraftLauncher
{
    class Player
    {
        public string Nick = "", Uuid = "";
        public bool Ok { get { return PlayersCfg.ValidNick(Nick) && PlayersCfg.NormalizeUuid(Uuid) != null; } }
    }

    enum Lookup { Ok, NotFound, Offline }

    class PlayersCfg
    {
        public Player Host = new Player(), Guest = new Player();
        public bool Complete { get { return Host.Ok && Guest.Ok; } }
        // trocavel por --mojang-url (testes de erro/offline)
        public static string MojangBase = "https://api.mojang.com/users/profiles/minecraft/";
        static readonly Regex NickRx = new Regex("^[A-Za-z0-9_]{3,16}$");

        public static bool ValidNick(string n) { return n != null && NickRx.IsMatch(n); }
        public static string NormalizeUuid(string s)
        {
            if (s == null) return null;
            var h = Regex.Replace(s.Trim(), "[-{}\\s]", "").ToLowerInvariant();
            if (!Regex.IsMatch(h, "^[0-9a-f]{32}$")) return null;
            return h.Substring(0, 8) + "-" + h.Substring(8, 4) + "-" + h.Substring(12, 4) + "-" + h.Substring(16, 4) + "-" + h.Substring(20);
        }

        public static PlayersCfg Load(string path)
        {
            var c = new PlayersCfg();
            try
            {
                if (!File.Exists(path)) return c;
                var t = File.ReadAllText(path, Encoding.UTF8);
                c.Host = ReadBlock(t, "host"); c.Guest = ReadBlock(t, "guest");
            }
            catch { }
            return c;
        }
        static Player ReadBlock(string t, string key)
        {
            var p = new Player();
            var m = Regex.Match(t, "\"" + key + "\"\\s*:\\s*\\{([^}]*)\\}");
            if (!m.Success) return p;
            var n = Regex.Match(m.Groups[1].Value, "\"nick\"\\s*:\\s*\"([^\"]*)\""); if (n.Success) p.Nick = n.Groups[1].Value;
            var u = Regex.Match(m.Groups[1].Value, "\"uuid\"\\s*:\\s*\"([^\"]*)\""); if (u.Success) p.Uuid = u.Groups[1].Value;
            return p;
        }
        public void Save(string path)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
            var s = "{\r\n  \"host\": {\r\n    \"nick\": \"" + Host.Nick + "\",\r\n    \"uuid\": \"" + Host.Uuid + "\"\r\n  },\r\n  \"guest\": {\r\n    \"nick\": \"" + Guest.Nick + "\",\r\n    \"uuid\": \"" + Guest.Uuid + "\"\r\n  }\r\n}\r\n";
            File.WriteAllText(path, s, new UTF8Encoding(false));
        }

        // Consulta a API publica da Mojang. NotFound = nick nao existe; Offline = sem rede/erro do servico.
        public static Lookup Query(string nick, out string uuid, out string name, out string why)
        {
            uuid = null; name = null; why = null;
            try
            {
                try { ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; } catch { }
                var rq = (HttpWebRequest)WebRequest.Create(MojangBase + Uri.EscapeDataString(nick));
                rq.Timeout = 8000; rq.ReadWriteTimeout = 8000; rq.UserAgent = "EldenMinecraftLauncher"; rq.Proxy = WebRequest.DefaultWebProxy;
                using (var rs = (HttpWebResponse)rq.GetResponse())
                using (var sr = new StreamReader(rs.GetResponseStream(), Encoding.UTF8))
                {
                    var body = sr.ReadToEnd();
                    var id = Regex.Match(body, "\"id\"\\s*:\\s*\"([0-9a-fA-F]{32})\"");
                    if (!id.Success) return Lookup.NotFound;
                    uuid = NormalizeUuid(id.Groups[1].Value);
                    var nm = Regex.Match(body, "\"name\"\\s*:\\s*\"([A-Za-z0-9_]{1,16})\""); name = nm.Success ? nm.Groups[1].Value : nick;
                    return Lookup.Ok;
                }
            }
            catch (WebException ex)
            {
                var hr = ex.Response as HttpWebResponse;
                if (hr != null && (hr.StatusCode == HttpStatusCode.NotFound || hr.StatusCode == HttpStatusCode.NoContent || hr.StatusCode == HttpStatusCode.BadRequest)) return Lookup.NotFound;
                why = hr != null ? "HTTP " + (int)hr.StatusCode : ex.Status.ToString(); return Lookup.Offline;
            }
            catch (Exception ex) { why = ex.GetType().Name; return Lookup.Offline; }
        }
    }

    // Telinha (tema pixel) para informar/editar os apelidos. O UUID e resolvido pela Mojang.
    class PlayersDialog : Window
    {
        readonly Window owner; readonly PlayersCfg cur; readonly Action<string> log;
        TextBox nickH, uuidH, nickG, uuidG; TextBlock status; Button btnSave, btnCancel;
        string autoH = "", autoG = "", prevNickH = "", prevNickG = ""; bool busy;
        public PlayersCfg Result;

        Style S(string k) { return (Style)owner.FindResource(k); }
        Brush Br(string hex) { var b = (SolidColorBrush)new BrushConverter().ConvertFromString(hex); b.Freeze(); return b; }
        TextBlock Tx(string t, double size, string color, string style, Thickness m)
        {
            var tb = new TextBlock { Text = t, FontSize = size, Foreground = Br(color), TextWrapping = TextWrapping.Wrap, Margin = m };
            if (style != null) tb.Style = S(style); tb.FontSize = size; tb.Foreground = Br(color); return tb;
        }

        public PlayersDialog(Window owner, PlayersCfg current, bool first, Action<string> log)
        {
            this.owner = owner; this.cur = current; this.log = log;
            Title = "Apelidos"; WindowStyle = WindowStyle.None; ResizeMode = ResizeMode.NoResize; Width = 580; SizeToContent = SizeToContent.Height;
            Background = Br("#050403"); FontFamily = owner.FontFamily; UseLayoutRounding = true; SnapsToDevicePixels = true;
            if (owner.IsVisible) { Owner = owner; WindowStartupLocation = WindowStartupLocation.CenterOwner; } else WindowStartupLocation = WindowStartupLocation.CenterScreen;

            var sp = new StackPanel { Margin = new Thickness(26, 22, 26, 22) };
            sp.Children.Add(Tx(first ? "QUEM VAI JOGAR?" : "EDITAR APELIDOS", 14, "#EBCB7A", "Px", new Thickness(0)));
            sp.Children.Add(Tx("Digite o nick de Minecraft de cada um. O UUID público é buscado sozinho na Mojang e fica salvo só neste PC.", 13, "#A39883", null, new Thickness(0, 8, 0, 14)));

            sp.Children.Add(Tx("ANFITRIÃO", 9, "#C8A24A", "Px", new Thickness(0, 4, 0, 6)));
            nickH = Field(sp, "Nick do anfitrião", current.Host.Nick); uuidH = Field(sp, "UUID do anfitrião (automático)", current.Host.Uuid, 12, true);
            sp.Children.Add(Tx("CONVIDADO", 9, "#C8A24A", "Px", new Thickness(0, 12, 0, 6)));
            nickG = Field(sp, "Nick do convidado", current.Guest.Nick); uuidG = Field(sp, "UUID do convidado (automático)", current.Guest.Uuid, 12, true);
            autoH = current.Host.Uuid; autoG = current.Guest.Uuid; prevNickH = current.Host.Nick; prevNickG = current.Guest.Nick;
            nickH.TextChanged += (s, e) => { if (uuidH.Text == autoH && prevNickH != nickH.Text) { uuidH.Text = ""; autoH = ""; } prevNickH = nickH.Text; };
            nickG.TextChanged += (s, e) => { if (uuidG.Text == autoG && prevNickG != nickG.Text) { uuidG.Text = ""; autoG = ""; } prevNickG = nickG.Text; };

            sp.Children.Add(Tx("Sem internet? Deixe o UUID em branco, tente de novo depois, ou cole o UUID à mão (com ou sem hífens).", 11.5, "#8F8571", null, new Thickness(0, 10, 0, 0)));
            status = Tx("", 13, "#EDE5D2", null, new Thickness(0, 10, 0, 0)); sp.Children.Add(status);

            var row = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 16, 0, 0) };
            btnCancel = new Button { Content = first ? "Depois" : "Cancelar", Style = S("GhostBtn"), Margin = new Thickness(0, 0, 10, 0) };
            btnSave = new Button { Content = "Salvar", Style = S("GhostBtn") };
            btnSave.FontWeight = FontWeights.SemiBold; btnSave.Foreground = Br("#EBCB7A");
            row.Children.Add(btnCancel); row.Children.Add(btnSave); sp.Children.Add(row);

            Content = new Border { BorderBrush = Br("#000000"), BorderThickness = new Thickness(2), Child = new Border { BorderBrush = Br("#8C6D2C"), BorderThickness = new Thickness(2), Background = Br("#0B0908"), Child = sp } };
            MouseLeftButtonDown += (s, e) => { if (e.ButtonState == MouseButtonState.Pressed && !(e.OriginalSource is TextBox)) { try { DragMove(); } catch { } } };
            btnCancel.Click += (s, e) => { if (!busy) { DialogResult = false; } };
            btnSave.Click += async (s, e) => { if (await DoSave()) DialogResult = true; };
            KeyDown += (s, e) => { if (e.Key == Key.Escape && !busy) DialogResult = false; };
            Loaded += (s, e) => { (string.IsNullOrEmpty(nickH.Text) ? nickH : nickG).Focus(); };
        }

        TextBox Field(Panel p, string label, string val, double size = 13, bool mono = false)
        {
            p.Children.Add(Tx(label, 11.5, "#A39883", null, new Thickness(0, 6, 0, 4)));
            var t = new TextBox { Style = S("Field"), Text = val ?? "", FontSize = size };
            if (mono) t.FontFamily = new FontFamily("Consolas");
            System.Windows.Automation.AutomationProperties.SetName(t, label);
            p.Children.Add(t); return t;
        }

        void Say(string t, string color) { status.Text = t; status.Foreground = Br(color); }

        // devolve null se ok; senao a mensagem de erro
        async Task<string> Resolve(string who, TextBox nick, TextBox uuid, Player old, Player outp)
        {
            var n = nick.Text.Trim();
            if (!PlayersCfg.ValidNick(n)) return "Apelido do " + who + " inválido: use de 3 a 16 letras, números ou _ .";
            var typed = uuid.Text.Trim();
            if (typed.Length > 0)
            {
                var u = PlayersCfg.NormalizeUuid(typed);
                if (u == null) return "UUID do " + who + " inválido: precisa ter 32 caracteres hexadecimais (0-9, a-f).";
                outp.Nick = n; outp.Uuid = u; uuid.Text = u; return null;
            }
            Say("Consultando a Mojang para " + n + "...", "#E3A234");
            string id = null, nm = null, why = null; Lookup r = Lookup.Offline;
            await Task.Run(() => { r = PlayersCfg.Query(n, out id, out nm, out why); });
            if (r == Lookup.NotFound) return "O nick \"" + n + "\" não existe na Mojang. Confira a grafia do " + who + " (ou informe o UUID à mão).";
            if (r == Lookup.Offline) return "Não consegui falar com a Mojang (" + why + "). Verifique a internet ou informe o UUID do " + who + " à mão e salve de novo.";
            outp.Nick = nm; outp.Uuid = id; nick.Text = nm; uuid.Text = id;
            if (who == "anfitrião") autoH = id; else autoG = id;
            return null;
        }

        public async Task<bool> DoSave()
        {
            if (busy) return false; busy = true; btnSave.IsEnabled = false; btnCancel.IsEnabled = false;
            try
            {
                var res = new PlayersCfg();
                var err = await Resolve("anfitrião", nickH, uuidH, cur.Host, res.Host);
                if (err == null) err = await Resolve("convidado", nickG, uuidG, cur.Guest, res.Guest);
                if (err == null && string.Equals(res.Host.Nick, res.Guest.Nick, StringComparison.OrdinalIgnoreCase)) err = "Anfitrião e convidado precisam ser pessoas diferentes.";
                if (err != null) { Say(err, "#E5654A"); if (log != null) log("Apelidos: " + err); return false; }
                Result = res; Say("Pronto! Apelidos salvos.", "#7BC043"); return true;
            }
            finally { busy = false; btnSave.IsEnabled = true; btnCancel.IsEnabled = true; }
        }

        // ---- automacao de teste (--setup-test host,convidado,prefixo-do-png[,uuidHost,uuidConvidado]) ----
        public async Task RunTest(string spec)
        {
            var p = spec.Split(',');
            await Task.Delay(600);
            nickH.Text = p[0]; nickG.Text = p.Length > 1 ? p[1] : "";
            if (p.Length > 3) { uuidH.Text = p[3]; }
            if (p.Length > 4) { uuidG.Text = p[4]; }
            var prefix = p.Length > 2 ? p[2] : null;
            await Task.Delay(300); Shot(prefix, "1-preenchido");
            var ok = await DoSave();
            await Task.Delay(300); Shot(prefix, "2-resultado-" + (ok ? "ok" : "erro"));
            if (ok) { DialogResult = true; } else { DialogResult = false; }
        }
        void Shot(string prefix, string tag)
        {
            if (prefix == null) return;
            var el = (FrameworkElement)Content; int w = (int)ActualWidth, h = (int)ActualHeight;
            var rtb = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32); rtb.Render(el);
            var enc = new PngBitmapEncoder(); enc.Frames.Add(BitmapFrame.Create(rtb));
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(prefix)));
            using (var f = File.Create(prefix + "-" + tag + ".png")) enc.Save(f);
        }
    }
}
