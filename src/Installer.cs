// MineRing Launcher - instalacao guiada do zero (aba INSTALAR). C# 5, compilado com o csc.exe do Windows.
// Regras: nada de segredo em log, nada de saves/mundos tocados, tudo com hash conferido e idempotente.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using Path = System.IO.Path;

namespace EldenMinecraftLauncher
{
    // Arquivo baixavel com tamanho e hash fixados (nada e aceito sem conferir).
    class Pin { public string Name, FileName, Url, Sha256, Sha512; public long Size; }

    static class Pins
    {
        // Prism Launcher portatil 11.1.1 (GPL-3.0), release oficial do GitHub. SHA-256 igual ao campo digest da API do GitHub.
        public static readonly Pin Prism = new Pin
        {
            Name = "Prism Launcher 11.1.1", FileName = "PrismLauncher-Windows-MSVC-Portable-11.1.1.zip", Size = 20396629,
            Url = "https://github.com/PrismLauncher/PrismLauncher/releases/download/11.1.1/PrismLauncher-Windows-MSVC-Portable-11.1.1.zip",
            Sha256 = "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"
        };
        // Mods do Modrinth (versao fixa para Minecraft 1.21.1 + Fabric). SHA-512 vem da API do Modrinth; SHA-256 dos perfis ja validados.
        public static readonly Pin FabricApi = new Pin
        {
            Name = "Fabric API 0.116.17", FileName = "fabric-api-0.116.17+1.21.1.jar", Size = 2452735,
            Url = "https://cdn.modrinth.com/data/P7dR8mSH/versions/Mys3P7lK/fabric-api-0.116.17%2B1.21.1.jar",
            Sha256 = "79ac44b40780acbd884b34c50be1e39af682847e5f5cb3b1fddeeaa768dce800",
            Sha512 = "98c478217da19181f0e4df3ea6c2da6bdbda271c7544a4aea048513a49b444f1ff5b71dc0df6303017c48e8ff69b7bb6af6179bdf0d0e09ab3c1ee08411857a8"
        };
        public static readonly Pin E4mc = new Pin
        {
            Name = "e4mc 6.2.3", FileName = "e4mc-fabric-6.2.3.jar", Size = 663705,
            Url = "https://cdn.modrinth.com/data/qANg5Jrr/versions/8wsUJ306/e4mc-fabric-6.2.3.jar",
            Sha256 = "27a889a01c44d54cd05a2cec942b0e76f2fd05988060c1107f55bcdcf42818b5",
            Sha512 = "de52773df77c5653500b139781173386e4e779bcfae79a91c93e4c3cd31ba412c3b1110360701efa6433733a6f6cf7738c11cbdafc88fccad786ef7412eabf1a"
        };
        // Seamless Co-op v2.0.1 (LukeYui). NAO e redistribuido: baixado do release oficial do autor na maquina do usuario.
        public static readonly Pin Seamless = new Pin
        {
            Name = "Seamless Co-op v2.0.1", FileName = "Seamless.Co-op.v2.0.1.zip", Size = 8078349,
            Url = "https://github.com/LukeYui/EldenRingSeamlessCoopRelease/releases/download/v2.0.1/Seamless.Co-op.v2.0.1.zip",
            Sha256 = "848ae27e1c77217590dac401c012a17d9b4f05b9b9a24aa33af54d8d7b8ba2a1"
        };
        public const string NexusUrl = "https://www.nexusmods.com/eldenring/mods/510";
        public const string JavaUrl = "https://adoptium.net/temurin/releases/?version=21&os=windows&arch=x64&package=jre";
    }

    class InstallEx : Exception { public string Code; public InstallEx(string code, string msg) : base(msg) { Code = code; } }

    enum SS { Pending, Running, Ok, Skip, Warn, Err }

    class SAct { public string Text; public Action Do; public SS When; }

    class IStep
    {
        public string Id, Title, Desc, Detail = "", ErrCode;
        public SS State = SS.Pending; public double Pct = -1; public bool Interruptible = true;
        public List<SAct> Acts = new List<SAct>();
        public Func<IStep, CancellationToken, Task> Run;
        public ColumnDefinition BarA, BarB; public TextBlock DetailTx; public FrameworkElement Row;
    }

    partial class MainWin
    {
        string fixturesDir, installShotsPrefix; int installShotN, throttleKbps;   // throttleKbps: so para testes (--throttle-kbps), simula rede lenta
        readonly List<IStep> steps = new List<IStep>();
        CancellationTokenSource cts; bool installBusy, installDone; Mutex installMutex; int closeTries;
        readonly HashSet<string> seamlessSeen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        string PrismInstallDir { get { return Path.Combine(stateDir, "PrismLauncher"); } }
        string PackInstallDir { get { return Path.Combine(stateDir, "EldenMinecraft-Windows"); } }
        string DlDir { get { return Path.Combine(stateDir, "downloads"); } }
        string PackTarget() { return det.InstallDir ?? PackInstallDir; }

        // ---------- deteccao auxiliar (roda em thread de fundo; nao toca na UI) ----------
        static string Hex(byte[] b) { return BitConverter.ToString(b).Replace("-", "").ToLowerInvariant(); }

        // So olha o comeco do accounts.json (existe um array "accounts" com pelo menos 1 item). Nunca le/loga tokens.
        bool HasPrismAccount(string data)
        {
            try
            {
                if (data == null) return false;
                var f = Path.Combine(data, "accounts.json"); if (!File.Exists(f)) return false;
                using (var fs = File.OpenRead(f)) { var buf = new byte[512]; int n = fs.Read(buf, 0, buf.Length); return Regex.IsMatch(Encoding.UTF8.GetString(buf, 0, n), "\"accounts\"\\s*:\\s*\\[\\s*\\{"); }
            }
            catch { return false; }
        }

        bool FindMinecraft()
        {
            try
            {
                var mc = Path.Combine(appDataDir, ".minecraft");
                if (File.Exists(Path.Combine(mc, "launcher_profiles.json")) || Directory.Exists(Path.Combine(mc, "versions"))) return true;
                if (sandbox != null) return false;
                if (File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Minecraft Launcher", "MinecraftLauncher.exe"))) return true;
                var pk = Path.Combine(localDir, "Packages");
                if (Directory.Exists(pk) && Directory.GetDirectories(pk, "Microsoft.4297127D64EC6_*").Length > 0) return true;
            }
            catch { }
            return false;
        }

        string FindJava21(string prismData)
        {
            try
            {
                var roots = new List<string>();
                if (prismData != null) roots.Add(Path.Combine(prismData, "java"));
                if (sandbox == null)
                {
                    foreach (var pf in new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86) })
                        foreach (var v in new[] { "Eclipse Adoptium", "Java", "Microsoft", "Zulu", "Amazon Corretto" }) roots.Add(Path.Combine(pf, v));
                    var jh = Environment.GetEnvironmentVariable("JAVA_HOME");
                    if (!string.IsNullOrEmpty(jh) && File.Exists(Path.Combine(jh, "release")) && Regex.IsMatch(File.ReadAllText(Path.Combine(jh, "release")), "JAVA_VERSION=\"2[1-9]")) return "JAVA_HOME";
                }
                foreach (var r in roots)
                {
                    if (!Directory.Exists(r)) continue;
                    foreach (var d in Directory.GetDirectories(r))
                    {
                        var n = Path.GetFileName(d).ToLowerInvariant();
                        if (!(n.Contains("21") || n.Contains("delta"))) continue;
                        if (Directory.GetFiles(d, "java*.exe", SearchOption.AllDirectories).Length > 0) return Path.GetFileName(r);
                    }
                }
            }
            catch { }
            return null;
        }

        bool ManifestInstalled(string root, string name)
        {
            try
            {
                if (!Directory.Exists(root)) return false;
                foreach (var d in Directory.GetDirectories(root))
                {
                    var f = Path.Combine(d, name);
                    if (File.Exists(f) && Regex.IsMatch(File.ReadAllText(f), "\"Status\"\\s*:\\s*\"Installed\"")) return true;
                }
            }
            catch { }
            return false;
        }

        // manifesto da ponte "Installed" que aponta para ESTE jogo (o dono dos arquivos da ponte na pasta do jogo)
        static bool OwnsGame(string pkg, string gameDir)
        {
            try
            {
                var root = Path.Combine(pkg, "bridge-backups");
                if (gameDir == null || !Directory.Exists(root) || !File.Exists(Path.Combine(pkg, "elden-ring", "windows", "Start-Coop.ps1"))) return false;
                var want = Path.GetFullPath(gameDir).TrimEnd('\\');
                foreach (var d in Directory.GetDirectories(root))
                {
                    var f = Path.Combine(d, "manifest.json");
                    if (!File.Exists(f)) continue;
                    var t = File.ReadAllText(f);
                    if (!Regex.IsMatch(t, "\"Status\"\\s*:\\s*\"Installed\"")) continue;
                    var g = Regex.Match(t, "\"GameDir\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
                    if (g.Success && string.Equals(Regex.Unescape(g.Groups[1].Value).TrimEnd('\\'), want, StringComparison.OrdinalIgnoreCase)) return true;
                }
            }
            catch { }
            return false;
        }

        // Instalacao anterior fora dos lugares de sempre (ex.: launcher movido para outro disco e o pacote antigo numa subpasta).
        // So roda se o jogo ja tem a ponte (erbridge_core.dll); busca limitada por profundidade, quantidade e tempo.
        string discoveredFor;
        string FindOwningInstall(string gameDir)
        {
            if (gameDir == null || !File.Exists(Path.Combine(gameDir, "erbridge", "erbridge_core.dll"))) return null;
            if (discoveredFor == gameDir) return null;   // ja procurou nesta sessao e nao achou
            var roots = new List<KeyValuePair<string, int>>();
            foreach (var r in new[] { "Documents", "Desktop", "Downloads" }) roots.Add(new KeyValuePair<string, int>(Path.Combine(homeDir, r), 7));
            if (sandbox == null)
            {
                roots.Add(new KeyValuePair<string, int>(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), 7));
                roots.Add(new KeyValuePair<string, int>(homeDir, 3));
                try { foreach (var dr in DriveInfo.GetDrives()) if (dr.DriveType == DriveType.Fixed && dr.IsReady) roots.Add(new KeyValuePair<string, int>(dr.RootDirectory.FullName, 3)); } catch { }
            }
            var skip = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "node_modules", ".git", "AppData", "Windows", "Program Files", "Program Files (x86)", "ProgramData", "$Recycle.Bin", "System Volume Information", "steamapps", "bridge-backups", "coop-backups", "instances" };
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var sw = Stopwatch.StartNew(); int visited = 0;
            foreach (var root in roots)
            {
                var q = new Queue<KeyValuePair<string, int>>(); q.Enqueue(new KeyValuePair<string, int>(root.Key, 0));
                while (q.Count > 0 && visited < 60000 && sw.ElapsedMilliseconds < 6000)
                {
                    var cur = q.Dequeue();
                    if (!seen.Add(cur.Key)) continue;
                    visited++;
                    if (OwnsGame(cur.Key, gameDir)) { Log("Pacote co-op achado fora dos lugares de sempre: " + cur.Key); return cur.Key; }
                    if (cur.Value >= root.Value) continue;
                    try
                    {
                        foreach (var sub in new DirectoryInfo(cur.Key).EnumerateDirectories())
                        {
                            if ((sub.Attributes & (FileAttributes.ReparsePoint | FileAttributes.System)) != 0 || skip.Contains(sub.Name) || sub.Name.StartsWith(".")) continue;
                            q.Enqueue(new KeyValuePair<string, int>(sub.FullName, cur.Value + 1));
                        }
                    }
                    catch { }
                }
            }
            Log("Busca do pacote co-op anterior: nada achado (" + visited + " pastas, " + sw.ElapsedMilliseconds + " ms).");
            discoveredFor = gameDir;
            return null;
        }

        bool NeedsInstall()
        {
            return det.GameDir != null && det.GameVer == "2.7.1.0" && (det.PrismExe == null || det.InstallDir == null || !det.Seamless || !det.Insts.Any(i => i.Role == role));
        }

        // raiz da biblioteca Steam que contem o jogo (para -SteamPath dos scripts)
        string SteamRoot()
        {
            if (sandbox != null) return Path.Combine(sandbox, "steam");
            try
            {
                if (det.GameDir != null)
                {
                    var g = new DirectoryInfo(det.GameDir);   // ...\steamapps\common\ELDEN RING\Game
                    var lib = g.Parent != null && g.Parent.Parent != null && g.Parent.Parent.Parent != null ? g.Parent.Parent.Parent.Parent : null;
                    if (lib != null && string.Equals(g.Parent.Parent.Name, "common", StringComparison.OrdinalIgnoreCase) && Directory.Exists(Path.Combine(lib.FullName, "steamapps"))) return lib.FullName;
                }
            }
            catch { }
            return det.SteamDir;
        }
        string GameArgs() { return "-GameDir " + Q(det.GameDir) + " -SteamPath " + Q(SteamRoot()); }

        string ReleaseVal(string key)
        {
            try
            {
                var rel = File.ReadAllText(Path.Combine(packDir, "EldenMinecraft-Windows", "elden-ring", "windows", "coop-v2-release.json"));
                return Regex.Match(rel, "\"" + key + "\"\\s*:\\s*\"([^\"]+)\"").Groups[1].Value;
            }
            catch { return ""; }
        }

        // ---------- download com tamanho + hash ----------
        static bool VerifyFile(string path, Pin p, out string why)
        {
            why = null;
            try
            {
                if (new FileInfo(path).Length != p.Size) { why = "tamanho " + new FileInfo(path).Length + " bytes, esperado " + p.Size; return false; }
                using (var f = File.OpenRead(path)) { if (Hex(SHA256.Create().ComputeHash(f)) != p.Sha256) { why = "SHA-256 diferente do esperado"; return false; } }
                if (p.Sha512 != null) using (var f = File.OpenRead(path)) { if (Hex(SHA512.Create().ComputeHash(f)) != p.Sha512) { why = "SHA-512 diferente do esperado"; return false; } }
                return true;
            }
            catch (Exception ex) { why = ex.Message; return false; }
        }

        static string Mb(long b) { return (b / 1048576.0).ToString("0.0", CultureInfo.GetCultureInfo("pt-BR")); }

        // Baixa (ou copia do --offline-fixtures) para dir, confere e so entao promove. Cache valido e reaproveitado.
        string Fetch(Pin p, string dir, Action<double, string> prog, CancellationToken ct)
        {
            Directory.CreateDirectory(dir);
            var dest = Path.Combine(dir, p.FileName); string why;
            if (File.Exists(dest))
            {
                if (VerifyFile(dest, p, out why)) { prog(1, p.Name + ": já baixado e conferido."); return dest; }
                try { File.Delete(dest); } catch { }
            }
            var part = dest + ".part";
            try { if (File.Exists(part)) File.Delete(part); } catch { }
            try
            {
                if (fixturesDir != null)
                {
                    var src = Path.Combine(fixturesDir, p.FileName);
                    if (!File.Exists(src)) throw new InstallEx("SemFixture", "Modo offline: faltou " + p.FileName + " em " + fixturesDir + ".");
                    using (var i = File.OpenRead(src)) using (var o = File.Create(part))
                    {
                        var buf = new byte[262144]; int n; long got = 0;
                        while ((n = i.Read(buf, 0, buf.Length)) > 0) { ct.ThrowIfCancellationRequested(); o.Write(buf, 0, n); got += n; prog(Math.Min(1.0, (double)got / p.Size), "Copiando " + p.Name + " (modo offline)"); }
                    }
                }
                else
                {
                    try { ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; } catch { }
                    var rq = (HttpWebRequest)WebRequest.Create(p.Url);
                    rq.UserAgent = "EldenMinecraftLauncher/windows.6 (projeto de fa, sem fins lucrativos)"; rq.Timeout = 30000; rq.ReadWriteTimeout = 30000; rq.Proxy = WebRequest.DefaultWebProxy;
                    using (var rs = (HttpWebResponse)rq.GetResponse())
                    {
                        if (rs.ContentLength > 0 && rs.ContentLength != p.Size) throw new InstallEx("Tamanho", p.Name + ": o servidor anunciou " + rs.ContentLength + " bytes, esperado " + p.Size + ". Por segurança não baixei.");
                        using (var i = rs.GetResponseStream()) using (var o = File.Create(part))
                        {
                            var buf = new byte[81920]; int n; long got = 0; var t0 = DateTime.UtcNow;
                            while ((n = i.Read(buf, 0, buf.Length)) > 0)
                            {
                                ct.ThrowIfCancellationRequested(); o.Write(buf, 0, n); got += n; if (throttleKbps > 0) Thread.Sleep((int)(n * 1000L / (throttleKbps * 1024L)));
                                if (got > p.Size + 4096) throw new InstallEx("Tamanho", p.Name + ": o arquivo passou do tamanho esperado. Por segurança parei.");
                                prog(Math.Min(1.0, (double)got / p.Size), "Baixando " + p.Name + ": " + Mb(got) + " de " + Mb(p.Size) + " MB");
                            }
                            if (got < p.Size) throw new InstallEx("Rede", "A conexão caiu no meio do download de " + p.Name + " (" + Mb(got) + " de " + Mb(p.Size) + " MB). Tente de novo.");
                        }
                    }
                }
            }
            catch (OperationCanceledException) { try { File.Delete(part); } catch { } throw; }
            catch (InstallEx) { try { File.Delete(part); } catch { } throw; }
            catch (Exception ex)
            {
                try { File.Delete(part); } catch { }
                // erro LOCAL (disco/permissao/arquivo em uso) nao pode virar "Rede": a etapa do Seamless daria o conselho errado (baixar no Nexus)
                if (ex is UnauthorizedAccessException) throw new InstallEx("Disco", "O Windows negou gravar em " + dir + ". Confira a permissão da pasta do launcher (ou o antivírus / acesso controlado a pastas) e tente de novo.");
                var io = ex as IOException;
                if (io != null)
                {
                    int hr = io.HResult & 0xFFFF;
                    if (hr == 112 || hr == 39) throw new InstallEx("Disco", "O disco está cheio ao baixar " + p.Name + ". Libere espaço (uns 300 MB) e tente de novo.");
                    if (hr == 32 || hr == 33) throw new InstallEx("EmUso", "O arquivo de " + p.Name + " está em uso por outro programa ou por outra janela do launcher. Feche a outra janela e tente de novo.");
                }
                throw new InstallEx("Rede", "Não consegui baixar " + p.Name + " (" + ex.Message + ").");
            }
            prog(1, "Conferindo " + p.Name + "…");
            if (!VerifyFile(part, p, out why)) { try { File.Delete(part); } catch { } throw new InstallEx("HashInvalido", p.Name + " baixado não confere (" + why + "). Apaguei o arquivo e não instalei nada."); }
            File.Move(part, dest);
            return dest;
        }

        static void ExtractZipSafe(string zip, string dest, Action<double> prog, CancellationToken ct)
        {
            Directory.CreateDirectory(dest);
            var root = Path.GetFullPath(dest).TrimEnd('\\') + "\\";
            using (var za = ZipFile.OpenRead(zip))
            {
                int i = 0, n = za.Entries.Count;
                foreach (var e in za.Entries)
                {
                    ct.ThrowIfCancellationRequested();
                    var target = Path.GetFullPath(Path.Combine(dest, e.FullName.Replace('/', '\\')));
                    if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InstallEx("ZipInseguro", "O ZIP tem um caminho fora da pasta de destino. Recusei.");
                    if (e.FullName.EndsWith("/")) Directory.CreateDirectory(target);
                    else { Directory.CreateDirectory(Path.GetDirectoryName(target)); e.ExtractToFile(target, true); }
                    i++; if (i % 20 == 0) prog((double)i / n);
                }
            }
            prog(1);
        }

        static void MoveDirRetry(string from, string to)
        {
            for (int i = 0; ; i++)
            {
                try { Directory.Move(from, to); return; }
                catch (IOException) { if (i >= 8) throw; Thread.Sleep(600); }   // antivirus costuma segurar a pasta recem extraida
                catch (UnauthorizedAccessException) { if (i >= 8) throw; Thread.Sleep(600); }
            }
        }

        // ---------- UI da aba INSTALAR ----------
        void BuildInstall()
        {
            // fechar a janela no meio de uma etapa que mexe nos arquivos do jogo deixaria o PowerShell orfao rodando sozinho: avisa e so deixa fechar na 2a tentativa
            Window.Closing += (s, e) =>
            {
                if (!installBusy) return;
                var cur = steps.FirstOrDefault(x => x.State == SS.Running);
                if (cur != null && !cur.Interruptible && ++closeTries < 2) { e.Cancel = true; InstStatus("Esta etapa está mexendo nos arquivos do jogo e termina em instantes. Espere, ou clique em fechar de novo para sair mesmo assim."); }
                else if (cts != null) { try { cts.Cancel(); } catch { } }
            };
            F<Button>("BtnInstall").Click += async (s, e) => await InstallAll(0, false);
            F<Button>("BtnInstCancel").Click += (s, e) => CancelInstall();
            F<Button>("BtnInstRescan").Click += async (s, e) => { if (!installBusy) await Rescan(); };
            F<Button>("BtnInstGoPlay").Click += (s, e) => F<RadioButton>("TabPlay").IsChecked = true;
            F<Button>("BtnInstLog").Click += (s, e) => { var b = F<Border>("InstLogBox"); b.Visibility = b.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible; };
            if (Cfg("Consent") == "1") F<CheckBox>("InstConsent").IsChecked = true;
            F<CheckBox>("InstConsent").Click += (s, e) => { cfg["Consent"] = F<CheckBox>("InstConsent").IsChecked == true ? "1" : "0"; SaveCfg(); };
            F<Border>("InstHalo").BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0.25, 1, TimeSpan.FromMilliseconds(1100)) { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
            BuildInstallSteps();
        }

        void BuildInstallSteps()
        {
            steps.Clear();
            steps.Add(new IStep { Id = "check", Title = "Conferir seu PC", Desc = "Steam, Elden Ring 2.7.1.0 e Minecraft Java.", Run = StepCheck });
            steps.Add(new IStep { Id = "prism", Title = "Prism Launcher", Desc = "Baixa o Prism portátil oficial (20 MB) e liga o download automático do Java 21.", Run = StepPrism });
            steps.Add(new IStep { Id = "pack", Title = "Pacote co-op", Desc = "Copia os arquivos da ponte para uma pasta do launcher.", Run = StepPack });
            steps.Add(new IStep { Id = "bridge", Title = "Ponte no Elden Ring", Desc = "Instala a ponte (dinput8 + core) com backup dos seus saves.", Run = StepBridge });
            steps.Add(new IStep { Id = "seamless", Title = "Seamless Co-op", Desc = "Baixa do autor (8 MB, conferido por hash) e instala.", Run = StepSeamless });
            steps.Add(new IStep { Id = "mods", Title = "Mods do Minecraft", Desc = "Fabric API e e4mc, direto do Modrinth, com hash conferido.", Run = StepMods });
            steps.Add(new IStep { Id = "profile", Title = "Perfil do Minecraft", Desc = "Cria o perfil Fabric 1.21.1 do seu papel no Prism.", Run = StepProfile });
            var login = new IStep { Id = "login", Title = "Conta Microsoft no Prism", Desc = "Você entra no Prism pelo navegador. O launcher nunca vê sua senha.", Run = StepLogin };
            login.Acts.Add(new SAct { Text = "Abrir o Prism", When = SS.Warn, Do = OpenPrismForLogin });
            login.Acts.Add(new SAct { Text = "Já entrei, verificar", When = SS.Warn, Do = () => { var _ = RecheckLogin(); } });
            steps.Add(login);
            steps.Add(new IStep { Id = "validate", Title = "Validar tudo", Desc = "Confere o resultado e libera o JOGAR.", Run = StepValidate });
            var sm = steps.First(x => x.Id == "seamless");
            sm.Acts.Add(new SAct { Text = "Abrir página do autor", When = SS.Err, Do = () => OpenUrl(Pins.NexusUrl) });
            sm.Acts.Add(new SAct { Text = "Escolher o ZIP baixado", When = SS.Err, Do = PickSeamlessZip });
            var pr = steps.First(x => x.Id == "prism");
            pr.Acts.Add(new SAct { Text = "Já tenho o Prism: escolher", When = SS.Err, Do = PickPrism });
            RenderSteps();
        }

        void PickSeamlessZip()
        {
            var o = new Microsoft.Win32.OpenFileDialog { Title = "Escolha o ZIP do Seamless Co-op v2.0.1", Filter = "ZIP|*.zip" };
            if (o.ShowDialog() == true)
            {
                var p = Pins.Seamless; string why;
                if (!VerifyFile(o.FileName, p, out why)) { ShowInstError("Esse ZIP não é o esperado", "O arquivo escolhido não confere com o Seamless Co-op v2.0.1 oficial (" + why + "). Baixe a versão 2.0.1 na página do autor."); return; }
                cfg["SeamlessZip"] = o.FileName; SaveCfg(); HideInstError();
                var _ = InstallAll(steps.FindIndex(x => x.Id == "seamless"), false);
            }
        }

        void OpenPrismForLogin()
        {
            try
            {
                if (det.PrismExe == null) return;
                Log("Abrindo o Prism para você entrar com a conta Microsoft (o launcher não vê sua senha).");
                Process.Start(new ProcessStartInfo(det.PrismExe, "--dir " + Q(det.PrismData)) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(det.PrismExe) });
            }
            catch (Exception ex) { ShowInstError("Não consegui abrir o Prism", ex.Message); }
        }

        async Task RecheckLogin()
        {
            if (installBusy || busy) return;
            det = await Task.Run(() => Detect());
            await InstallAll(steps.FindIndex(x => x.Id == "login"), false);
        }

        void RenderInstallPrereqs()
        {
            var p = F<StackPanel>("InstChecksPanel"); if (p == null) return;
            var rows = new List<Row>();
            if (scanning) { foreach (var t in new[] { "Steam", "Elden Ring", "Minecraft Java", "Java 21" }) rows.Add(new Row { State = St.Loading, Title = t, Detail = "Procurando…" }); }
            else
            {
                var d = det;
                rows.Add(d.SteamDir == null ? new Row { State = St.Err, Title = "Steam", Detail = "Não encontrei a Steam. Instale a Steam e entre na sua conta." }
                    : d.SteamRunning ? new Row { State = St.Ok, Title = "Steam", Detail = "Aberta e pronta." }
                    : new Row { State = St.Warn, Title = "Steam", Detail = "Fechada. Para instalar não precisa; para jogar abra a Steam e faça login." });
                if (d.GameDir == null) rows.Add(new Row { State = St.Err, Title = "Elden Ring", Detail = "Não achei o jogo na Steam. Instale o Elden Ring ou aponte a pasta ELDEN RING\\Game.", ActionText = "Escolher pasta", Action = PickGame });
                else if (d.GameVer != "2.7.1.0") rows.Add(new Row { State = St.Err, Title = "Elden Ring", Detail = "Versão " + (d.GameVer ?? "desconhecida") + " não suportada. O co-op precisa da 2.7.1.0 e o launcher não troca a versão do jogo." });
                else rows.Add(new Row { State = St.Ok, Title = "Elden Ring", Detail = "Versão 2.7.1.0 encontrada." });
                rows.Add(d.McFound ? new Row { State = St.Ok, Title = "Minecraft Java", Detail = "Encontrado." }
                    : new Row { State = St.Warn, Title = "Minecraft Java", Detail = "Não achei. Dá para instalar mesmo assim, mas para jogar você precisa de uma conta Microsoft com Minecraft Java.", ActionText = "Abrir minecraft.net", Action = () => OpenUrl("https://www.minecraft.net/download") });
                rows.Add(d.JavaWhere != null ? new Row { State = St.Ok, Title = "Java 21", Detail = "Encontrado." }
                    : new Row { State = St.Warn, Title = "Java 21", Detail = "Não achei. Sem problema: o Prism baixa o Java 21 sozinho (precisa de internet na primeira abertura).", ActionText = "Instalador oficial", Action = () => OpenUrl(Pins.JavaUrl) });
            }
            RenderRowList(p, rows);
            var nick = role == "host" ? players.Host.Nick : players.Guest.Nick;
            F<TextBlock>("InstRoleText").Text = (role == "host" ? "Anfitrião" : "Convidado") + (nick.Length > 0 ? " · " + nick : "");
        }

        void ShowInstError(string title, string body)
        {
            sfx.Play("error");
            Window.Dispatcher.Invoke(new Action(() =>
            {
                F<TextBlock>("InstErrTitle").Text = title; F<TextBlock>("InstErrBody").Text = body;
                var b = F<Border>("InstErrBox"); b.Visibility = Visibility.Visible; b.BeginAnimation(UIElement.OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(250)));
            }));
            Log("ERRO: " + title + " - " + body);
        }
        void HideInstError() { F<Border>("InstErrBox").Visibility = Visibility.Collapsed; }

        void InstStatus(string t) { Window.Dispatcher.Invoke(new Action(() => { F<TextBlock>("InstSub").Text = t; })); }

        Brush StateBrush(SS s) { return s == SS.Ok || s == SS.Skip ? cOk : s == SS.Warn ? cWarn : s == SS.Err ? cErr : s == SS.Running ? cGold : cLine; }

        void RenderSteps()
        {
            var p = F<StackPanel>("InstStepsPanel"); p.Children.Clear();
            for (int i = 0; i < steps.Count; i++)
            {
                var s = steps[i];
                var g = new Grid { Margin = new Thickness(0, 0, 0, 12) };
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(38) });
                g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                var c = StateBrush(s.State);
                var ic = new Border { Width = 26, Height = 26, BorderBrush = c, BorderThickness = new Thickness(2.5), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 1, 0, 0) };
                if (s.State == SS.Running)
                {
                    var sq = new Rectangle { Width = 8, Height = 8, Fill = cGoldB, RenderTransformOrigin = new Point(0.5, 0.5) };
                    var rt = new RotateTransform(); sq.RenderTransform = rt;
                    var spin = new DoubleAnimationUsingKeyFrames { Duration = TimeSpan.FromMilliseconds(800), RepeatBehavior = RepeatBehavior.Forever };
                    spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(0, KeyTime.FromPercent(0))); spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(45, KeyTime.FromPercent(0.25))); spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(90, KeyTime.FromPercent(0.5))); spin.KeyFrames.Add(new DiscreteDoubleKeyFrame(135, KeyTime.FromPercent(0.75)));
                    rt.BeginAnimation(RotateTransform.AngleProperty, spin);
                    ic.Child = new Canvas { Width = 8, Height = 8, Children = { sq } }; ic.Background = B("#2E2312");
                }
                else if (s.State == SS.Pending) ic.Child = new TextBlock { Text = (i + 1).ToString(), Foreground = cMuted, FontSize = 11, FontFamily = pixelFF ?? new FontFamily("Consolas"), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                else ic.Child = new TextBlock { Text = s.State == SS.Ok || s.State == SS.Skip ? "✓" : s.State == SS.Warn ? "!" : "✕", Foreground = c, FontSize = 13, FontWeight = FontWeights.Bold, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
                g.Children.Add(ic);
                var sp = new StackPanel { Margin = new Thickness(2, 0, 0, 0) }; Grid.SetColumn(sp, 1);
                var head = new StackPanel { Orientation = Orientation.Horizontal };
                head.Children.Add(new TextBlock { Text = s.Title, FontSize = 14, FontWeight = FontWeights.SemiBold, Foreground = s.State == SS.Pending ? cMuted : cText });
                if (s.State == SS.Skip) head.Children.Add(new TextBlock { Text = "  já estava pronto", FontSize = 11.5, Foreground = cOk, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(0, 0, 0, 1) });
                sp.Children.Add(head);
                var det0 = string.IsNullOrEmpty(s.Detail) ? s.Desc : s.Detail;
                s.DetailTx = new TextBlock { Text = det0, FontSize = 12, Foreground = s.State == SS.Err ? B("#F0A090") : s.State == SS.Warn ? B("#EBC27A") : cMuted, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 2, 0, 0) };
                sp.Children.Add(s.DetailTx);
                s.BarA = null; s.BarB = null;
                if (s.State == SS.Running && s.Pct >= 0)
                {
                    var bg = new Grid { Height = 8 }; s.BarA = new ColumnDefinition(); s.BarB = new ColumnDefinition();
                    s.BarA.Width = new GridLength(Math.Max(0.0001, s.Pct), GridUnitType.Star); s.BarB.Width = new GridLength(Math.Max(0.0001, 1 - s.Pct), GridUnitType.Star);
                    bg.ColumnDefinitions.Add(s.BarA); bg.ColumnDefinitions.Add(s.BarB);
                    bg.Children.Add(new Rectangle { Fill = cGold });
                    var ob = new Border { Background = Brushes.Black, Padding = new Thickness(2), Margin = new Thickness(0, 6, 0, 0), Child = new Border { Background = B("#0F0C09"), Child = bg } };
                    sp.Children.Add(ob);
                }
                var btns = new WrapPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(0, 7, 0, 0) };
                foreach (var a in s.Acts.Where(x => x.When == s.State && !installBusy))
                {
                    var act = a.Do; var b = new Button { Content = a.Text, Style = (Style)Window.FindResource("GhostBtn"), FontSize = 9, Margin = new Thickness(0, 0, 8, 4) }; b.Padding = new Thickness(0); b.Click += (x, y) => act(); btns.Children.Add(b);
                }
                if (s.State == SS.Err && !installBusy)
                {
                    int idx = i; var rb = new Button { Content = "Tentar de novo", Style = (Style)Window.FindResource("GhostBtn"), FontSize = 9 }; rb.Padding = new Thickness(0);
                    rb.Click += async (x, y) => { HideInstError(); await InstallAll(idx, false); };
                    btns.Children.Add(rb);
                }
                if (btns.Children.Count > 0) sp.Children.Add(btns);
                g.Children.Add(sp); s.Row = g;
                p.Children.Add(g);
            }
        }

        void SetSS(IStep s, SS st, string detail)
        {
            Window.Dispatcher.Invoke(new Action(() =>
            {
                s.State = st; if (detail != null) s.Detail = detail; if (st != SS.Running) s.Pct = -1; else if (s.Pct < 0) s.Pct = -1;
                RenderSteps();
                if (st == SS.Running && s.Row != null) s.Row.BringIntoView();
                InstallShot(s.Id + "-" + st.ToString().ToLowerInvariant());
            }));
            Log("Etapa \"" + s.Title + "\": " + st + (detail != null && detail.Length > 0 ? " - " + detail.Replace("\n", " ") : ""));
        }

        // progresso 0..1 (<0 = indeterminado). Atualiza so a barra e o texto, sem refazer a lista.
        void Prog(IStep s, double pct, string text)
        {
            Window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (s.State != SS.Running) return;
                bool had = s.Pct >= 0; s.Pct = pct; if (text != null) s.Detail = text;
                if (pct >= 0 && !had) { RenderSteps(); return; }
                if (s.BarA != null && pct >= 0) { s.BarA.Width = new GridLength(Math.Max(0.0001, Math.Min(1, pct)), GridUnitType.Star); s.BarB.Width = new GridLength(Math.Max(0.0001, 1 - Math.Min(1, pct)), GridUnitType.Star); }
                if (s.DetailTx != null && text != null) s.DetailTx.Text = text;
            }));
        }

        void InstallShot(string tag)
        {
            if (installShotsPrefix == null) return;
            var path = installShotsPrefix + "-" + (++installShotN).ToString("00") + "-" + tag + ".png";
            Window.Dispatcher.BeginInvoke(new Action(() => { try { Shot(path); } catch (Exception ex) { Log("Captura falhou: " + ex.Message); } }), DispatcherPriority.ApplicationIdle);
        }

        void CancelInstall()
        {
            if (cts == null || cts.IsCancellationRequested) return;
            cts.Cancel();
            var cur = steps.FirstOrDefault(x => x.State == SS.Running);
            InstStatus(cur != null && !cur.Interruptible ? "Cancelamento pedido. Esta etapa mexe em arquivos do jogo e termina primeiro (não dá para interromper com segurança)." : "Cancelando…");
            Log("Cancelamento pedido pelo usuário.");
        }

        // ---------- motor ----------
        async Task InstallAll(int start, bool auto)
        {
            if (busy || installBusy) return;
            var consent = F<CheckBox>("InstConsent");
            if (auto) consent.IsChecked = true;   // so --auto-install (teste)
            if (consent.IsChecked != true) { ShowInstError("Falta o seu OK", "Marque a caixa de confirmação abaixo (Elden Ring original e downloads) e clique em INSTALAR TUDO de novo."); return; }
            if (!players.Complete) { EditPlayers(true); if (!players.Complete) { ShowInstError("Faltam os apelidos", "O perfil do Minecraft e a whitelist usam o apelido do anfitrião e do convidado. Preencha em \"Editar apelidos\" (aba JOGAR) e tente de novo."); return; } }
            // uma instalacao por vez (duas janelas brigariam pelo mesmo .part e pela mesma pasta do jogo)
            var gotLock = false;
            try
            {
                var key = BitConverter.ToString(SHA1.Create().ComputeHash(Encoding.UTF8.GetBytes(Path.GetFullPath(stateDir).ToLowerInvariant()))).Replace("-", "");
                installMutex = new Mutex(false, "Local\\ErmcInstall-" + key);
                try { gotLock = installMutex.WaitOne(0); } catch (AbandonedMutexException) { gotLock = true; }
            }
            catch { gotLock = true; }
            if (!gotLock) { try { installMutex.Dispose(); } catch { } installMutex = null; ShowInstError("Outra janela do launcher já está instalando", "Existe outra janela do MineRing Launcher instalando agora. Espere ela terminar (ou feche-a) e clique em INSTALAR TUDO de novo."); return; }
            busy = true; installBusy = true; installDone = false; cts = new CancellationTokenSource();
            HideInstError(); F<Button>("BtnInstall").IsEnabled = false; F<Button>("BtnInstCancel").IsEnabled = true; F<Button>("BtnInstGoPlay").Visibility = Visibility.Collapsed; F<Button>("BtnPlay").IsEnabled = false;
            F<TextBlock>("InstallText").Text = "INSTALANDO…";
            bool failed = false, canceled = false;
            Log("Instalação iniciada (a partir da etapa " + (start + 1) + ")" + (sandbox != null ? " [sandbox]" : "") + (fixturesDir != null ? " [offline-fixtures]" : "") + ".");
            try
            {
                for (int i = start; i < steps.Count; i++)
                {
                    var s = steps[i];
                    if (cts.IsCancellationRequested) { canceled = true; break; }
                    InstStatus("Etapa " + (i + 1) + " de " + steps.Count + ": " + s.Title);
                    s.ErrCode = null; SetSS(s, SS.Running, s.Desc);
                    try
                    {
                        await s.Run(s, cts.Token);
                        if (s.State == SS.Running) SetSS(s, SS.Ok, s.Detail.Length > 0 ? s.Detail : "Pronto.");
                    }
                    catch (OperationCanceledException) { SetSS(s, SS.Pending, "Cancelado. Nada foi deixado pela metade."); canceled = true; break; }
                    catch (InstallEx ex)
                    {
                        s.ErrCode = ex.Code; SetSS(s, SS.Err, ex.Message.Split(new[] { (char)10 })[0]); ShowInstError("Não deu certo: " + s.Title, ex.Message + "\nNada foi quebrado. Corrija e use \"Tentar de novo\" na etapa (ou INSTALAR TUDO: o que já está pronto é pulado)."); failed = true; break;
                    }
                    catch (Exception ex)
                    {
                        SetSS(s, SS.Err, ex.Message); ShowInstError("Algo deu errado: " + s.Title, ex.Message + "\nVeja o Registro. Dá para tentar de novo."); failed = true; break;
                    }
                }
            }
            finally
            {
                busy = false; installBusy = false; var c0 = cts; cts = null; try { c0.Dispose(); } catch { }
                try { if (installMutex != null) { installMutex.ReleaseMutex(); installMutex.Dispose(); } } catch { }
                installMutex = null;
                F<Button>("BtnInstall").IsEnabled = true; F<Button>("BtnInstCancel").IsEnabled = false; F<Button>("BtnPlay").IsEnabled = true; F<TextBlock>("InstallText").Text = "INSTALAR TUDO";
            }
            RenderSteps();
            try { await Rescan(); } catch { }
            var v = steps.First(x => x.Id == "validate"); var lg = steps.First(x => x.Id == "login");
            if (canceled) InstStatus("Cancelado com segurança. Clique em INSTALAR TUDO para continuar de onde parou.");
            else if (failed) InstStatus("Parou numa etapa. Veja o aviso vermelho abaixo.");
            else if (v.State == SS.Ok) { installDone = true; InstStatus("Tudo pronto! Pode ir para a aba JOGAR."); F<Button>("BtnInstGoPlay").Visibility = Visibility.Visible; }
            else if (v.State == SS.Warn) { InstStatus("Tudo instalado. Falta só entrar com sua conta Microsoft no Prism."); F<Button>("BtnInstGoPlay").Visibility = Visibility.Visible; }
            var last = steps.LastOrDefault(x => x.State != SS.Pending) ?? steps.Last();
            Window.Dispatcher.BeginInvoke(new Action(() => { if (last.Row != null) last.Row.BringIntoView(); InstallShot("fim"); }), DispatcherPriority.Background);
        }

        // vigia: conta Microsoft apareceu no Prism / ZIP do Seamless apareceu em Downloads
        void InstallTick()
        {
            if (installBusy || busy || curPage != "install" || steps.Count == 0 || det == null) return;
            if (F<CheckBox>("InstConsent").IsChecked != true) return;
            var lg = steps.First(x => x.Id == "login");
            if (lg.State == SS.Warn && det.PrismData != null && HasPrismAccount(det.PrismData)) { Log("Conta Microsoft detectada no Prism."); var _ = RecheckLogin(); return; }
            var sm = steps.First(x => x.Id == "seamless");
            if (sm.State == SS.Err && sm.ErrCode == "SeamlessManual" && FindSeamlessZip() != null) { Log("ZIP do Seamless encontrado em Downloads."); var _ = InstallAll(steps.FindIndex(x => x.Id == "seamless"), false); }
        }

        // ---------- erros em portugues claro ----------
        string ExplainInstall(string code, string msg)
        {
            switch (code)
            {
                case "BinariosAusentes": return "Os binários da ponte (dinput8.dll, erbridge_core.dll e o jar) não estão na pasta pack. Use o ZIP da aba Releases, não o código-fonte do GitHub.";
                case "BinarioHash": return "Um arquivo da ponte não bate com a versão windows.6 esperada. Baixe o ZIP da Release de novo. Detalhe: " + msg;
                case "BridgeIncompleto": return "Existe uma instalação anterior da ponte incompleta ou com falha. O launcher não mexe nela. Rode Remove-Bridge.ps1 do pacote e tente de novo. Detalhe: " + msg;
                case "ErAberto": return "O Elden Ring está aberto. Feche o jogo pelo menu (Sair do jogo) e tente de novo.";
                case "PlayersInvalido": return "O arquivo de apelidos está incompleto. Edite os apelidos na aba JOGAR (o UUID precisa ser achado na Mojang ou colado a mão).";
                case "SeamlessForeign": return "Já existem arquivos do Seamless que não foram instalados por este launcher. Por segurança não sobrescrevo. Se ele funciona para você, ok; senão apague a pasta SeamlessCoop e o ersc_launcher.exe do jogo e tente de novo.";
                case "PerfilExiste": return "Já existe um perfil com esse nome no Prism. Não sobrescrevo perfis nem mundos.";
                case "SemMods": return "Faltam os arquivos dos mods baixados. Tente de novo a etapa dos mods.";
                case "SenhaInvalida": return "A senha da sessão precisa ter de 12 a 128 caracteres (letras sem acento, números, _ ou -). Troque no painel SENHA DO CO-OP (aba JOGAR) ou gere uma nova.";
                case "IniAlterado": return "O ersc_settings.ini foi editado fora do launcher (ele não bate mais com o registro da instalação), então o launcher não sobrescreve. Se você mudou a senha dentro do Seamless, a do launcher fica diferente: restaure o ini original ou reinstale o Seamless pelo launcher.";
                case "SeamlessIncompleto": return "A instalação do Seamless está incompleta ou com falha. O launcher não mexe nela. Reinstale pela aba INSTALAR.";
                case "SeamlessMultiplo": return "Há mais de um registro de instalação do Seamless para este jogo. Por segurança não mexi em nenhum.";
                case "ManifestoInvalido": case "IniAusente": case "BackupAusente": case "IniInvalido": return "O registro ou o ersc_settings.ini do Seamless não está como o launcher instalou, então não gravei a senha. Reinstale o Seamless pela aba INSTALAR. Detalhe: " + msg;
            }
            if (msg.IndexOf("Access to the path", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("acesso ao caminho", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("foi negado", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("is denied", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("UnauthorizedAccess", StringComparison.OrdinalIgnoreCase) >= 0)
                return "O Windows negou a gravação numa pasta (provavelmente a pasta do jogo ou a do launcher). Confira se a pasta não é protegida, se o antivírus / \"Acesso controlado a pastas\" do Windows não está bloqueando, ou rode o launcher como administrador. Nada foi quebrado: a instalação desfez o que fez. Detalhe: " + msg;
            if (msg.IndexOf("not enough space", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("espaço suficiente", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("espaco suficiente", StringComparison.OrdinalIgnoreCase) >= 0)
                return "O disco está cheio. Libere espaço (uns 300 MB) e tente de novo. Detalhe: " + msg;
            if (msg.IndexOf("unrecognized/non-bridge", StringComparison.OrdinalIgnoreCase) >= 0)
                return "Já existe um dinput8.dll na pasta do jogo que não foi instalado por esta pasta do launcher: ou a ponte foi instalada antes por outra pasta do MineRing (um pacote EldenMinecraft-Windows antigo), ou é de outro mod. Se for do MineRing, use \"Escolher pasta\" no item Pacote co-op (aba JOGAR) e aponte a pasta EldenMinecraft-Windows antiga, a que tem bridge-backups. Nada foi alterado no jogo. Detalhe: " + msg;
            if (msg.IndexOf("Unsupported eldenring.exe FileVersion",StringComparison.OrdinalIgnoreCase) >= 0) return "O Elden Ring não está na versão 2.7.1.0. O co-op só funciona nela e o launcher não troca a versão do jogo.";
            if (msg.IndexOf("Select exactly one Steam", StringComparison.OrdinalIgnoreCase) >= 0) return "Não achei exatamente uma instalação do Elden Ring na Steam. Use \"Escolher pasta\" nos preparativos e confira se o jogo terminou de instalar/atualizar.";
            if (msg.IndexOf("incomplete/updating", StringComparison.OrdinalIgnoreCase) >= 0) return "A Steam ainda está instalando ou atualizando o Elden Ring. Espere terminar e tente de novo.";
            if (msg.IndexOf("is running", StringComparison.OrdinalIgnoreCase) >= 0 || msg.IndexOf("already running", StringComparison.OrdinalIgnoreCase) >= 0) return "O Elden Ring ou o Seamless está aberto. Feche e tente de novo.";
            if (msg.IndexOf("archive SHA256 mismatch", StringComparison.OrdinalIgnoreCase) >= 0) return "O ZIP do Seamless não é o v2.0.1 oficial (o hash não bate). Baixe de novo na página do autor.";
            if (msg.IndexOf("Unowned Seamless destination", StringComparison.OrdinalIgnoreCase) >= 0) return "Já existem arquivos do Seamless que não foram instalados por este launcher. Por segurança não sobrescrevo.";
            if (msg.IndexOf("installed bridge manifest is required", StringComparison.OrdinalIgnoreCase) >= 0) return "A ponte precisa ser instalada antes do Seamless. Rode a etapa \"Ponte no Elden Ring\".";
            if (msg.IndexOf("ja existe", StringComparison.OrdinalIgnoreCase) >= 0 && msg.IndexOf("instancia", StringComparison.OrdinalIgnoreCase) >= 0) return "Já existe um perfil com esse nome no Prism. Não sobrescrevo perfis nem mundos.";
            return msg.Length > 0 ? msg : "Erro desconhecido.";
        }

        // roda pack\launcher\Run-Install.ps1 -Step <passo>; protocolo PROGRESS|pct|texto, RESULT|status|info, ERROR|codigo|detalhe
        async Task<string> RunInstallPs(IStep s, string step, string extra)
        {
            var script = Path.Combine(packDir, "launcher", "Run-Install.ps1");
            if (!File.Exists(script)) throw new InstallEx("SemScript", "Falta o arquivo pack\\launcher\\Run-Install.ps1 ao lado do launcher. Use o ZIP completo da Release.");
            string result = null, err = null;
            s.Interruptible = false;
            var r = await RunPs(script, "-Step " + step + " -PackageRoot " + Q(PackTarget()) + " " + extra, 900000, line =>
            {
                if (line.StartsWith("PROGRESS|"))
                {
                    var q = line.Split(new[] { '|' }, 3); double pc;
                    if (double.TryParse(q[1], NumberStyles.Float, CultureInfo.InvariantCulture, out pc)) Prog(s, Math.Max(0, Math.Min(1, pc / 100.0)), q.Length > 2 ? q[2] : null);
                }
                else { if (line.StartsWith("RESULT|")) result = line; else if (line.StartsWith("ERROR|")) err = line; Log("  " + line); }
            });
            s.Interruptible = true;
            if (err != null) { var q = err.Split(new[] { '|' }, 3); var code = q.Length > 1 ? q[1] : ""; throw new InstallEx(code, ExplainInstall(code, q.Length > 2 ? q[2] : "")); }
            if (result == null) throw new InstallEx("Falha", ExplainInstall("", FirstMsg(r.Lines)));            return result.Split('|')[1];
        }

        // ---------- etapas ----------
        async Task StepCheck(IStep s, CancellationToken ct)
        {
            det = await Task.Run(() => Detect());
            Window.Dispatcher.Invoke(new Action(() => { RenderChecks(); RenderInstPicker(); }));
            if (det.SteamDir == null) throw new InstallEx("SemSteam", "Não encontrei a Steam. Instale a Steam, entre na sua conta e instale o Elden Ring.");
            if (det.GameDir == null) throw new InstallEx("SemElden", "Não achei o Elden Ring na Steam. Instale o jogo ou use \"Escolher pasta\" nos preparativos.");
            if (det.GameVer != "2.7.1.0") throw new InstallEx("VersaoElden", "O Elden Ring está na versão " + (det.GameVer ?? "desconhecida") + ". O co-op precisa da 2.7.1.0 e o launcher não troca a versão do jogo (e não deixe a Steam atualizar).");
            try
            {   // espaco livre onde o launcher guarda tudo (Prism ~60 MB + downloads + pacote) e no disco do jogo (Seamless + backups de saves)
                foreach (var pth in new[] { stateDir, det.GameDir })
                {
                    var root = Path.GetPathRoot(Path.GetFullPath(pth));
                    if (string.IsNullOrEmpty(root) || root.StartsWith("\\\\")) continue;
                    var di = new DriveInfo(root);
                    if (di.IsReady && di.AvailableFreeSpace < 400L * 1048576) throw new InstallEx("Disco", "Pouco espaço livre em " + root + " (" + Mb(di.AvailableFreeSpace) + " MB). Libere uns 400 MB e tente de novo.");
                }
            }
            catch (InstallEx) { throw; }
            catch { }
            s.Detail = "Steam e Elden Ring 2.7.1.0 encontrados. Minecraft Java: " + (det.McFound ? "encontrado." : "não achei (o Prism baixa o jogo; você precisa da conta).");
        }

        async Task StepPrism(IStep s, CancellationToken ct)
        {
            if (det.PrismExe != null) { SetSS(s, SS.Skip, "Prism já encontrado. Não mexi nele."); return; }
            // sobras de uma extracao interrompida (janela fechada / queda de energia): so pastas temporarias que o proprio launcher cria
            try { foreach (var old in Directory.GetDirectories(stateDir, "PrismLauncher.novo-*")) try { Directory.Delete(old, true); } catch { } } catch { }
            Prog(s, 0, "Preparando o download do Prism…");
            await Task.Run(() =>
            {
                var zip = Fetch(Pins.Prism, DlDir, (p, t) => Prog(s, p * 0.8, t), ct);
                var tmp = PrismInstallDir + ".novo-" + Guid.NewGuid().ToString("N").Substring(0, 8);
                try
                {
                    Prog(s, 0.8, "Extraindo o Prism…");
                    ExtractZipSafe(zip, tmp, p => Prog(s, 0.8 + p * 0.17, "Extraindo o Prism…"), ct);
                    var exe = File.Exists(Path.Combine(tmp, "prismlauncher.exe")) ? tmp : Directory.GetFiles(tmp, "prismlauncher.exe", SearchOption.AllDirectories).Select(Path.GetDirectoryName).FirstOrDefault();
                    if (exe == null) throw new InstallEx("PrismSemExe", "O ZIP do Prism não trouxe o prismlauncher.exe. Nada foi instalado.");
                    if (!File.Exists(Path.Combine(exe, "portable.txt")) && !Directory.Exists(Path.Combine(exe, "UserData"))) File.WriteAllText(Path.Combine(exe, "portable.txt"), "");
                    Directory.CreateDirectory(Path.Combine(exe, "instances"));
                    var cfgp = Path.Combine(exe, "prismlauncher.cfg");
                    if (!File.Exists(cfgp)) File.WriteAllText(cfgp, "[General]\r\nAutomaticJavaDownload=true\r\nAutomaticJavaSwitch=true\r\nIgnoreJavaWizard=true\r\n", new UTF8Encoding(false));
                    ct.ThrowIfCancellationRequested();
                    try { File.Delete(zip); } catch { }   // 20 MB que o launcher baixou e nao precisa mais
                    if (Directory.Exists(PrismInstallDir)) MoveDirRetry(PrismInstallDir, PrismInstallDir + ".antigo-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));   // sobra incompleta: guarda, nunca apaga
                    MoveDirRetry(exe, PrismInstallDir);
                    if (exe != tmp) try { Directory.Delete(tmp, true); } catch { }
                }
                catch { try { if (Directory.Exists(tmp)) Directory.Delete(tmp, true); } catch { } throw; }
                Prog(s, 1, "Prism instalado.");
            });
            det = await Task.Run(() => Detect());
            if (det.PrismExe == null) throw new InstallEx("PrismSumiu", "O Prism foi extraído mas o launcher não o encontrou. Veja o Registro.");
            s.Detail = "Prism " + "11.1.1" + " portátil instalado na pasta do launcher, com download automático do Java 21 ligado.";
        }

        async Task StepPack(IStep s, CancellationToken ct)
        {
            if (det.InstallDir != null) { SetSS(s, SS.Skip, "Pacote já instalado em " + det.InstallDir + ". Não copiei nada por cima (atualizações são do JOGAR, com backup)."); return; }
            var st = await RunInstallPs(s, "pack", GameArgs());
            det = await Task.Run(() => Detect());
            if (st == "AlreadyInstalled") SetSS(s, SS.Skip, "Os arquivos da ponte já estavam copiados e conferidos.");
            else s.Detail = "Arquivos da ponte copiados e conferidos (SHA-256) para a pasta do launcher.";
        }

        async Task StepBridge(IStep s, CancellationToken ct)
        {
            if (Process.GetProcessesByName("eldenring").Length > 0) throw new InstallEx("ErAberto", ExplainInstall("ErAberto", ""));
            var st = await RunInstallPs(s, "bridge", GameArgs());
            det = await Task.Run(() => Detect());
            if (st == "AlreadyInstalled") SetSS(s, SS.Skip, "A ponte já estava instalada e conferida.");
            else s.Detail = "Ponte instalada. Backup dos saves e manifesto guardados em bridge-backups.";
        }

        string FindSeamlessZip()
        {
            var cands = new List<string>();
            var c = Cfg("SeamlessZip"); if (c != null) cands.Add(c);
            var dirs = new List<string> { Path.Combine(homeDir, "Downloads"), Path.Combine(homeDir, "Desktop"), DlDir };
            if (sandbox == null) dirs.Add(Path.GetTempPath());
            foreach (var d in dirs)
                try { foreach (var f in Directory.GetFiles(d, "*.zip")) if (Path.GetFileName(f).IndexOf("seamless", StringComparison.OrdinalIgnoreCase) >= 0) cands.Add(f); } catch { }
            foreach (var f in cands.Distinct())
            {
                try
                {
                    if (!File.Exists(f) || new FileInfo(f).Length != Pins.Seamless.Size) continue;
                    var key = f + "|" + File.GetLastWriteTimeUtc(f).Ticks; string why;
                    if (seamlessSeen.Contains(key)) continue;
                    if (VerifyFile(f, Pins.Seamless, out why)) return f;
                    seamlessSeen.Add(key);
                }
                catch { }
            }
            return null;
        }

        static string RandomPass()
        {
            const string al = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
            var b = new byte[16]; using (var r = new RNGCryptoServiceProvider()) r.GetBytes(b);
            var sb = new StringBuilder(); foreach (var x in b) sb.Append(al[x % al.Length]); return sb.ToString();
        }

        async Task StepSeamless(IStep s, CancellationToken ct)
        {
            if (det.Seamless)
            {
                if (det.CoopManifest) SetSS(s, SS.Skip, "O Seamless Co-op já estava instalado e registrado.");
                else SetSS(s, SS.Warn, "Já há um Seamless no jogo que não foi instalado por este launcher. Não mexi nele. Se o JOGAR reclamar, apague a pasta SeamlessCoop e o ersc_launcher.exe do jogo e rode de novo.");
                return;
            }
            var zip = FindSeamlessZip(); bool mine = false;
            if (zip == null)
            {
                Prog(s, 0, "Baixando o Seamless Co-op v2.0.1 do release oficial do autor (LukeYui)…");
                try { zip = await Task.Run(() => Fetch(Pins.Seamless, DlDir, (p, t) => Prog(s, p * 0.6, t), ct)); mine = true; }
                catch (InstallEx ex)
                {
                    if (ex.Code != "Rede" && ex.Code != "SemFixture") throw;
                    throw new InstallEx("SeamlessManual","Não consegui baixar o Seamless Co-op do release do autor (" + ex.Message + ")\nBaixe o ZIP da versão v2.0.1 na página do autor (precisa de login no Nexus): o launcher vigia a pasta Downloads e continua sozinho. Ou use \"Escolher o ZIP baixado\".");
                }
            }
            else mine = zip.StartsWith(DlDir, StringComparison.OrdinalIgnoreCase);
            Prog(s, 0.6, "Instalando o Seamless Co-op (backup dos saves antes)…");
            // senha: um unico lugar de verdade (savedPass, painel SENHA DO CO-OP na aba JOGAR). Vazia = gera uma forte e guarda ANTES de instalar.
            if (PassDirty()) throw new InstallEx("SenhaNaoSalva", "Você digitou uma senha na aba JOGAR mas não clicou em SALVAR. Salve (ou apague o campo) e tente de novo.");
            var pass = savedPass;
            if (pass == null)
            {
                pass = RandomPass();
                try { StoreSavedPass(pass); savedPass = pass; Window.Dispatcher.Invoke(new Action(() => { SetDraft(pass); UpdatePassUi(); })); Log("Senha do co-op gerada e salva (valor não registrado)."); }
                catch (Exception ex) { throw new InstallEx("SenhaSalvar", "Não consegui guardar a senha da sessão neste PC (" + ex.GetType().Name + "). Confira a permissão da pasta do launcher e tente de novo."); }
            }
            if (!PassOk(pass)) throw new InstallEx("SenhaInvalida", ExplainInstall("SenhaInvalida", ""));
            var st = await WithPassFile(pass, pf => RunInstallPs(s, "seamless", GameArgs() + " -ArchivePath " + Q(zip) + " -PasswordFile " + Q(pf)));
            if (st == "AlreadyInstalled") { await WithPassFile(pass, pf => RunPassStep(pf)); s.Detail = "O Seamless já estava registrado. A senha salva foi conferida no ini."; }
            else s.Detail = "Seamless Co-op v2.0.1 instalado (saves separados em .co2, invasões desligadas). A senha da sessão está salva no launcher: veja e copie em SENHA DO CO-OP (aba JOGAR) e passe a mesma para seu amigo.";
            if (mine) try { File.Delete(zip); } catch { }   // so apaga o que o launcher baixou; o ZIP do usuario fica
            det = await Task.Run(() => Detect());
        }

        async Task StepMods(IStep s, CancellationToken ct)
        {
            var dir = Path.Combine(DlDir, "mods");
            var list = new List<Pin> { Pins.FabricApi }; if (role == "host") list.Add(Pins.E4mc);
            for (int i = 0; i < list.Count; i++)
            {
                var pin = list[i]; int k = i;
                await Task.Run(() => Fetch(pin, dir, (p, t) => Prog(s, (k + p) / list.Count, t), ct));
            }
            s.Detail = (role == "host" ? "Fabric API e e4mc baixados" : "Fabric API baixado") + " do Modrinth e conferidos (SHA-256 e SHA-512).";
        }

        async Task StepProfile(IStep s, CancellationToken ct)
        {
            if (det.PrismExe == null) throw new InstallEx("SemPrism", "O Prism ainda não está instalado.");
            var have = det.Insts.FirstOrDefault(i => i.Role == role);
            if (have != null) { SetSS(s, SS.Skip, "Perfil já existe no Prism: " + have.Name + ". Não mexi nele."); return; }
            if (PrismRunning()) throw new InstallEx("PrismAberto", "O Prism está aberto. Feche o Prism (ele recarrega a lista de perfis ao abrir) e tente de novo.");
            var inst = det.InstancesDir ?? Path.Combine(det.PrismData, "instances");
            Directory.CreateDirectory(inst);
            var fab = Path.Combine(DlDir, "mods", Pins.FabricApi.FileName); var e4 = Path.Combine(DlDir, "mods", Pins.E4mc.FileName);
            if (!File.Exists(fab) || (role == "host" && !File.Exists(e4))) throw new InstallEx("SemMods", ExplainInstall("SemMods", ""));
            var extra = "-Role " + role + " -InstancesDirectory " + Q(inst) + " -PlayersFile " + Q(playersFile) + " -FabricApiJar " + Q(fab) + (role == "host" ? " -E4mcJar " + Q(e4) : "");
            await RunInstallPs(s, "profile", extra);
            det = await Task.Run(() => Detect());
            if (!det.Insts.Any(i => i.Role == role)) throw new InstallEx("PerfilSumiu", "O perfil foi criado mas o launcher não o enxerga. Veja o Registro.");
            s.Detail = "Perfil criado no Prism (Minecraft 1.21.1, Fabric 0.19.5, ponte windows.6" + (role == "host" ? ", e4mc e whitelist dos dois jogadores" : "") + ").";
        }

        bool PrismRunning()
        {
            try
            {
                foreach (var p in Process.GetProcessesByName("prismlauncher"))
                {
                    try { if (det.PrismExe == null || string.Equals(p.MainModule.FileName, det.PrismExe, StringComparison.OrdinalIgnoreCase)) return true; } catch { return true; }
                }
            }
            catch { }
            return false;
        }

        async Task StepLogin(IStep s, CancellationToken ct)
        {
            det = await Task.Run(() => Detect());
            if (det.PrismAccount) { SetSS(s, SS.Ok, "Conta Microsoft encontrada no Prism."); return; }
            SetSS(s, SS.Warn, "Falta entrar com sua conta Microsoft (com Minecraft Java). Clique em \"Abrir o Prism\", depois em Conta (canto superior direito) > Gerenciar contas > Adicionar Microsoft (em inglês: Account > Manage Accounts > Add Microsoft) e entre pelo navegador. O launcher só abre o Prism: não vê sua senha. Ele continua sozinho quando a conta aparecer.");
        }

        async Task StepValidate(IStep s, CancellationToken ct)
        {
            det = await Task.Run(() => Detect());
            Window.Dispatcher.Invoke(new Action(() => { RenderChecks(); RenderInstPicker(); }));
            var probs = Missing().Where(m => !m.StartsWith("A Steam está fechada")).ToList();
            if (probs.Count > 0) throw new InstallEx("Validacao", "Ainda falta:\n" + string.Join("\n", probs.ToArray()));
            await RunInstallPs(s, "validate", GameArgs());
            string extra = det.UpToDate ? "" : " O pacote será atualizado pelo JOGAR (com backup).";
            if (!det.PrismAccount) SetSS(s, SS.Warn, "Tudo instalado e conferido. Falta só a conta Microsoft no Prism (etapa acima)." + extra);
            else SetSS(s, SS.Ok, "Tudo pronto e conferido. Pode jogar." + extra);
        }
    }
}
