// Jogos FALSOS so para testar o botao PARAR do launcher (nunca use contra os jogos reais).
// Um unico exe, copiado com nomes diferentes; o comportamento vem do nome do arquivo:
//   ersc_launcher.exe : abre o eldenring.exe falso (mesma pasta) e sai depois de 1,5 s (como o Seamless)
//   eldenring.exe     : janela falsa; CloseMainWindow fecha (ou e ignorado com --stubborn=er|both)
//   prismlauncher.exe : abre o javaw.exe falso e fica vivo ate ele sair
//   javaw.exe         : escreve o bridge.shm (heartbeat) e atende o "quit" do mc_cmd.txt como o mod (ou ignora com --stubborn=mc|both)
// Compilar: tools\New-FakeGames.ps1
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class FakeGame
{
    [STAThread]
    static void Main(string[] args)
    {
        var me = Path.GetFileNameWithoutExtension(Process.GetCurrentProcess().MainModule.FileName).ToLowerInvariant();
        string stub = "";
        foreach (var a in args) if (a.StartsWith("--stubborn=")) stub = a.Substring(11);
        var dir = AppDomain.CurrentDomain.BaseDirectory;
        if (me == "ersc_launcher") { Spawn(Path.Combine(dir, "eldenring.exe"), args); Thread.Sleep(1500); return; }
        if (me == "prismlauncher")
        {
            var p = Spawn(Path.Combine(dir, "javaw.exe"), new[] { "-Derbridge.coop=true", "--gameDir", "fake" , stub.Length > 0 ? "--stubborn=" + stub : "--x" });
            p.WaitForExit(); return;
        }
        if (me == "javaw") { Mc(stub == "mc" || stub == "both"); return; }
        // eldenring
        bool stubborn = stub == "er" || stub == "both";
        var f = new Form { Text = "FAKE Elden Ring (teste)", Width = 360, Height = 120, ShowInTaskbar = true, StartPosition = FormStartPosition.Manual, Left = 20, Top = 20 };
        f.FormClosing += (s, e) => { if (stubborn) e.Cancel = true; };
        Application.Run(f);
    }

    static Process Spawn(string exe, string[] args)
    {
        var psi = new ProcessStartInfo(exe, string.Join(" ", args)) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe) };
        return Process.Start(psi);
    }

    static void Mc(bool stubborn)
    {
        var ipc = Path.Combine(Environment.GetEnvironmentVariable("USERPROFILE"), "Documents", "EldenMinecraft", "ipc");
        Directory.CreateDirectory(ipc);
        var shm = Path.Combine(ipc, "bridge.shm"); var cmd = Path.Combine(ipc, "mc_cmd.txt");
        ulong hb = 1; uint pid = (uint)Process.GetCurrentProcess().Id;
        while (true)
        {
            try
            {
                var b = new byte[40];
                BitConverter.GetBytes(0x434D484Du).CopyTo(b, 0); BitConverter.GetBytes(hb++).CopyTo(b, 0x18); BitConverter.GetBytes(pid).CopyTo(b, 0x24);
                using (var fs = new FileStream(shm, FileMode.OpenOrCreate, FileAccess.Write, FileShare.ReadWrite)) fs.Write(b, 0, b.Length);
                if (File.Exists(cmd))
                {
                    string t; using (var fs = new FileStream(cmd, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) t = new StreamReader(fs).ReadToEnd();
                    if (t.Trim() == "quit") { File.Delete(cmd); if (!stubborn) { try { File.Delete(shm); } catch { } return; } }
                }
            }
            catch { }
            Thread.Sleep(100);
        }
    }
}
