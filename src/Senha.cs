// MineRing Launcher - senha da sessao do Seamless Co-op (painel SENHA DO CO-OP na aba JOGAR). C# 5.
// Um unico lugar de verdade: savedPass (guardada com DPAPI/CurrentUser em senha-coop.dat, fora do repo e fora do config.txt).
// JOGAR, INSTALAR e o painel leem so dela. Quem grava no ersc_settings.ini e o passo "password" do Run-Install.ps1, que
// tambem acerta o hash do arquivo no coop-manifest.json (o Start-Coop confere esse hash).
// REGRA: a senha nunca vai para launcher.log, linha de comando, mensagem de erro, README ou captura de tela.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using Path = System.IO.Path;

namespace EldenMinecraftLauncher
{
    class PassRes { public string Kind; public string Msg; public PassRes(string k, string m) { Kind = k; Msg = m; } }

    partial class MainWin
    {
        string savedPass;                 // so em memoria; no disco fica protegida pelo Windows (DPAPI, usuario atual)
        bool passShown, passSync, passReady;
        string passFlash; Brush passFlashBr;
        string copiedPass; DispatcherTimer clipTimer;

        static readonly byte[] PassEntropy = Encoding.UTF8.GetBytes("EldenMinecraftLauncher/senha-coop/v1");
        string PassStoreFile { get { return Path.Combine(stateDir, fakeMissing ? "senha-coop.fake.dat" : "senha-coop.dat"); } }
        string IniPath() { return det == null || det.GameDir == null ? null : Path.Combine(det.GameDir, "SeamlessCoop", "ersc_settings.ini"); }

        static bool PassOk(string p) { return p != null && Regex.IsMatch(p, "^[A-Za-z0-9_-]{12,128}$"); }

        // ---------- armazenamento (DPAPI) ----------
        string LoadSavedPass()
        {
            try
            {
                if (!File.Exists(PassStoreFile)) return null;
                var plain = ProtectedData.Unprotect(File.ReadAllBytes(PassStoreFile), PassEntropy, DataProtectionScope.CurrentUser);
                var s = Encoding.UTF8.GetString(plain); Array.Clear(plain, 0, plain.Length);
                return PassOk(s) ? s : null;
            }
            catch (Exception ex) { Log("Senha salva ilegivel (" + ex.GetType().Name + "). Defina de novo no painel SENHA DO CO-OP."); return null; }
        }

        void StoreSavedPass(string p)
        {
            var enc = ProtectedData.Protect(Encoding.UTF8.GetBytes(p), PassEntropy, DataProtectionScope.CurrentUser);
            var tmp = PassStoreFile + ".novo";
            File.WriteAllBytes(tmp, enc);
            if (File.Exists(PassStoreFile)) File.Replace(tmp, PassStoreFile, null); else File.Move(tmp, PassStoreFile);
        }

        // ---------- ini do Seamless (so leitura aqui; quem escreve e o passo "password" do PowerShell) ----------
        enum IniSt { Sem, Vazia, Igual, Diferente }

        string ReadIniPass()
        {
            try
            {
                var f = IniPath(); if (f == null || !File.Exists(f)) return null;
                var m = Regex.Match(File.ReadAllText(f), @"(?m)^[ \t]*cooppassword[ \t]*=[ \t]*([^\r\n;]*)");
                return m.Success ? m.Groups[1].Value.Trim() : null;
            }
            catch { return null; }
        }

        IniSt IniState()
        {
            if (det == null || det.GameDir == null || !det.Seamless) return IniSt.Sem;
            var v = ReadIniPass();
            if (v == null || !PassOk(v)) return IniSt.Vazia;
            return savedPass != null && string.Equals(v, savedPass, StringComparison.Ordinal) ? IniSt.Igual : IniSt.Diferente;
        }

        // quem ja tinha o Seamless instalado: a senha que ja esta no ini vira a senha salva (nada muda no jogo)
        void ImportIniPassIfNeeded()
        {
            if (savedPass != null || scanning || det == null || !det.Seamless) return;
            var v = ReadIniPass(); if (!PassOk(v)) return;
            try { StoreSavedPass(v); savedPass = v; if (Draft().Length == 0) SetDraft(v); Log("Senha do co-op importada do Seamless já instalado (valor não registrado)."); }
            catch (Exception ex) { Log("Não consegui guardar a senha importada (" + ex.GetType().Name + ")."); }
        }

        // ---------- gerador: 4 palavras + 4 numeros, facil de ditar (cerca de 44 bits) ----------
        static readonly string[] Words = (
            "lobo urso gato pato foca cabra zebra tigre onca arara tucano sapo peixe polvo baleia cobra jacare tatu anta lontra " +
            "raposa coruja falcao pombo cisne garca ganso galo vaca boi cavalo burro ovelha porco coelho rato macaco girafa " +
            "rio mar lago ilha praia areia pedra rocha serra monte vale campo bosque mata selva folha raiz galho flor " +
            "rosa lirio cravo trigo milho arroz feijao uva pera maca manga limao caju coco figo ameixa ponte torre " +
            "muro porta janela telhado mesa cadeira cama tapete espelho relogio vela lampada chave faca espada escudo arco flecha lanca " +
            "martelo machado capa botas anel coroa trono castelo reino azul verde preto branco cinza dourado prata bronze sol lua " +
            "estrela nuvem chuva vento trovao raio neve gelo fogo brasa fumaca sombra luz aurora noite pao queijo leite " +
            "mel cafe sopa bolo doce sal pimenta alho cebola tomate batata jogo festa danca canto musica livro carta mapa " +
            "trilha estrada barco navio remo ancora porto farol nobre guerreiro mago arqueiro ladino bardo monge druida paladino runa " +
            "fogueira chama brisa onda duna gruta ravina colina planalto cascata nascente pantano deserto floresta tundra vulcao cratera ilhota recife " +
            "coral concha perola tesouro bussola tocha escada tunel poco moinho forja bigorna enxada picareta balde sino tambor flauta harpa " +
            "violao gaita apito bandeira estandarte pergaminho tinta pena selo brasao elmo manto luva adaga cajado amuleto pocao cristal diamante esmeralda rubi safira topazio ambar jade").Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);

        static int RandInt(RNGCryptoServiceProvider r, int n)
        {
            var b = new byte[4]; uint lim = uint.MaxValue - (uint.MaxValue % (uint)n); uint v;
            do { r.GetBytes(b); v = BitConverter.ToUInt32(b, 0); } while (v >= lim);
            return (int)(v % (uint)n);
        }

        static string GeneratePassphrase()
        {
            var words = Words.Distinct().ToArray();
            using (var r = new RNGCryptoServiceProvider())
            {
                var pick = new List<string>();
                while (pick.Count < 4) { var w = words[RandInt(r, words.Length)]; if (!pick.Contains(w)) pick.Add(w); }
                var sb = new StringBuilder();
                foreach (var w in pick) { sb.Append(char.ToUpperInvariant(w[0])); sb.Append(w.Substring(1)); sb.Append('-'); }
                sb.Append(RandInt(r, 10000).ToString("D4"));
                return sb.ToString();
            }
        }

        // ---------- painel (aba JOGAR) ----------
        TextBox PassTx { get { return F<TextBox>("PassTx"); } }
        PasswordBox PassPw { get { return F<PasswordBox>("PassPw"); } }
        string Draft() { return (passShown ? PassTx.Text : PassPw.Password).Trim(); }
        bool PassDirty() { var d = Draft(); return d.Length > 0 && !string.Equals(d, savedPass, StringComparison.Ordinal); }

        void SetDraft(string v)
        {
            passSync = true;
            try { PassTx.Text = v ?? ""; PassPw.Password = v ?? ""; } finally { passSync = false; }
            UpdatePassUi();
        }

        void Flash(string msg, Brush br) { passFlash = msg; passFlashBr = br; UpdatePassUi(); }

        void BuildSenha()
        {
            savedPass = LoadSavedPass();
            passReady = true;
            SetDraft(savedPass ?? "");
            PassPw.PasswordChanged += (s, e) => { if (passSync) return; passSync = true; try { PassTx.Text = PassPw.Password; } finally { passSync = false; } passFlash = null; UpdatePassUi(); };
            PassTx.TextChanged += (s, e) => { if (passSync) return; passSync = true; try { PassPw.Password = PassTx.Text; } finally { passSync = false; } passFlash = null; UpdatePassUi(); };
            F<Button>("BtnPassGen").Click += (s, e) =>
            {
                SetDraft(GeneratePassphrase()); SetPassShown(true);
                Flash("Senha nova gerada, ainda não salva. Clique em SALVAR e passe a MESMA para seu amigo (use COPIAR).", cWarn);
            };
            F<Button>("BtnPassCopy").Click += (s, e) => CopyPass();
            F<Button>("BtnPassShow").Click += (s, e) => { SetPassShown(!passShown); };
            F<Button>("BtnPassSave").Click += async (s, e) => await SavePassClicked(false);
            F<Button>("BtnInstPass").Click += (s, e) => { F<RadioButton>("TabPlay").IsChecked = true; try { (passShown ? (Control)PassTx : PassPw).Focus(); } catch { } };
            UpdatePassUi();
        }

        void SetPassShown(bool show)
        {
            passShown = show;
            PassTx.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            PassPw.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
            F<Button>("BtnPassShow").Content = show ? "Ocultar" : "Mostrar";
            F<Button>("BtnPassShow").SetValue(System.Windows.Automation.AutomationProperties.NameProperty, show ? "Ocultar a senha na tela" : "Mostrar a senha na tela");
        }

        void CopyPass()
        {
            var d = Draft();
            if (d.Length == 0) { Flash("Não há senha para copiar. Gere uma ou digite a combinada.", cWarn); return; }
            try
            {
                var o = new DataObject(); o.SetText(d);
                try { o.SetData("ExcludeClipboardContentFromMonitorProcessing", new MemoryStream(new byte[] { 1, 0, 0, 0 })); } catch { }   // fora do histórico/nuvem da área de transferência
                Clipboard.SetDataObject(o, true);
                copiedPass = d;
                if (clipTimer != null) clipTimer.Stop();
                clipTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(45) };
                clipTimer.Tick += (s, e) =>
                {
                    clipTimer.Stop();
                    try { if (copiedPass != null && Clipboard.ContainsText() && Clipboard.GetText() == copiedPass) Clipboard.Clear(); } catch { }
                    copiedPass = null; passFlash = null; UpdatePassUi();
                };
                clipTimer.Start();
                Flash("Copiada. Cole no chat com seu amigo. A área de transferência é limpa em 45 segundos.", cOk);
            }
            catch (Exception) { Flash("O Windows não deixou usar a área de transferência agora. Mostre a senha e copie à mão.", cErr); }
        }

        // linha de estado do painel + o espelho na aba INSTALAR
        void UpdatePassUi()
        {
            if (!passReady) return;
            var d = Draft(); string msg; Brush br;
            F<TextBlock>("PassHint").Visibility = d.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
            if (passFlash != null) { msg = passFlash; br = passFlashBr; }
            else if (d.Length == 0) { if (savedPass == null) { msg = "Nenhuma senha definida ainda. Gere uma ou digite a que vocês combinaram."; br = cWarn; } else { msg = "Campo vazio. A senha salva continua valendo."; br = cMuted; } }
            else if (!PassOk(d)) { msg = "Precisa de 12 a 128 caracteres: letras sem acento, números, _ ou -."; br = cWarn; }
            else if (!string.Equals(d, savedPass, StringComparison.Ordinal)) { msg = "Alterada, ainda não salva. Clique em SALVAR."; br = cWarn; }
            else
            {
                switch (IniState())
                {
                    case IniSt.Igual: msg = "Salva e gravada no Seamless. Seu amigo precisa ter a mesma."; br = cOk; break;
                    case IniSt.Diferente: msg = "Salva neste PC, mas o Seamless ainda tem outra. O JOGAR grava antes de abrir (ou clique em SALVAR)."; br = cWarn; break;
                    case IniSt.Vazia: msg = "Salva neste PC. O ini do Seamless está sem senha válida: clique em SALVAR para gravar."; br = cWarn; break;
                    default: msg = "Salva neste PC. Vai para o Seamless quando ele for instalado (aba INSTALAR)."; br = cMuted; break;
                }
            }
            var t = F<TextBlock>("PassStatus"); t.Text = msg; t.Foreground = br;
            var it = F<TextBlock>("InstPassState");
            if (it != null) { it.Text = savedPass != null ? "Já definida. O launcher usa a senha salva (aba JOGAR) e grava no Seamless." : "Vazio: o launcher gera uma senha forte, guarda e grava no Seamless. Veja e copie na aba JOGAR."; }
        }

        void SetPassButtons(bool on) { foreach (var n in new[] { "BtnPassGen", "BtnPassCopy", "BtnPassShow", "BtnPassSave" }) F<Button>(n).IsEnabled = on; }

        // useSaved: "Gravar agora" do checklist (regrava a senha ja salva, ignora o campo)
        async Task SavePassClicked(bool useSaved)
        {
            var d = useSaved && savedPass != null ? savedPass : Draft();
            if (d.Length == 0) { Flash("Digite ou gere uma senha antes de salvar.", cWarn); return; }
            if (!PassOk(d)) { Flash("A senha precisa ter de 12 a 128 caracteres: letras sem acento, números, _ ou -.", cWarn); return; }
            if (busy || installBusy) { Flash("Espere o launcher terminar o que está fazendo e clique em SALVAR de novo.", cWarn); return; }
            busy = true; SetPassButtons(false);
            try
            {
                bool changed = !string.Equals(d, savedPass, StringComparison.Ordinal);
                try { StoreSavedPass(d); savedPass = d; Log("Senha do co-op " + (changed ? "salva" : "confirmada") + " (protegida pelo Windows; valor não registrado)."); }
                catch (Exception ex) { Log("Não consegui salvar a senha (" + ex.GetType().Name + ")."); Flash("Não consegui salvar a senha neste PC (sem permissão na pasta do launcher). O Seamless não foi alterado.", cErr); return; }
                Flash("Gravando no Seamless…", cMuted);
                var r = await ApplyPassToIni();
                switch (r.Kind)
                {
                    case "ok": Flash("Salva e gravada no Seamless. Passe a MESMA senha para seu amigo (COPIAR).", cOk); break;
                    case "sim": Flash("Simulação: senha salva; a gravação no Seamless foi só simulada.", cWarn); break;
                    case "pend": Flash(r.Msg, cWarn); break;
                    default: Flash("Salva neste PC, mas NÃO gravada no Seamless: " + r.Msg, cErr); break;
                }
            }
            finally { busy = false; SetPassButtons(true); RenderChecks(); }
        }

        // ---------- gravar a senha salva no ini (tambem usado pelo JOGAR e pelo INSTALAR) ----------
        async Task<PassRes> ApplyPassToIni()
        {
            var p = savedPass;
            if (p == null) return new PassRes("pend", "Nenhuma senha salva ainda.");
            if (det == null || det.GameDir == null || det.GameVer != "2.7.1.0" || !det.Seamless || det.InstallDir == null)
                return new PassRes("pend", "Salva neste PC. Vai para o Seamless quando ele for instalado (aba INSTALAR).");
            if (dry && sandbox == null) { Log("[simulação] gravaria a senha salva no ersc_settings.ini (valor não registrado)."); return new PassRes("sim", ""); }
            try
            {
                var st = await WithPassFile(p, pf => RunPassStep(pf));
                if (st == "NoSeamless") return new PassRes("pend", "Esse Seamless não foi instalado pelo launcher, então não mexi no ini. A senha ficou salva; digite a mesma dentro do Seamless.");
                Log("Senha do co-op no Seamless: " + (st == "Unchanged" ? "já estava igual" : "gravada") + ".");
                return new PassRes("ok", "");
            }
            catch (InstallEx ex) { Log("Senha do co-op: não gravada (" + ex.Code + "): " + ex.Message); return new PassRes("err", ex.Message); }
        }

        // escreve a senha em arquivo temporario (nunca em argumento), roda o passo e apaga o arquivo sobrescrito
        async Task<T> WithPassFile<T>(string pass, Func<string, Task<T>> run)
        {
            var pf = Path.Combine(stateDir, ".senha-sessao.tmp");
            try { File.WriteAllText(pf, pass, new UTF8Encoding(false)); return await run(pf); }
            finally { try { if (File.Exists(pf)) { File.WriteAllBytes(pf, new byte[Math.Max(64, (int)new FileInfo(pf).Length)]); File.Delete(pf); } } catch { } }
        }

        async Task<string> RunPassStep(string passFile)
        {
            var script = Path.Combine(packDir, "launcher", "Run-Install.ps1");
            if (!File.Exists(script)) throw new InstallEx("SemScript", "Falta o arquivo pack\\launcher\\Run-Install.ps1 ao lado do launcher. Use o ZIP completo da Release.");
            string result = null, err = null;
            var r = await RunPs(script, "-Step password -PackageRoot " + Q(PackTarget()) + " " + GameArgs() + " -PasswordFile " + Q(passFile), 120000, line =>
            {
                if (line.StartsWith("RESULT|")) result = line; else if (line.StartsWith("ERROR|")) err = line;
                if (!line.StartsWith("PROGRESS|")) Log("  " + line);
            });
            if (err != null) { var q = err.Split(new[] { '|' }, 3); var code = q.Length > 1 ? q[1] : ""; throw new InstallEx(code, ExplainInstall(code, q.Length > 2 ? q[2] : "")); }
            if (result == null) throw new InstallEx("Falha", ExplainInstall("", FirstMsg(r.Lines)));
            return result.Split('|')[1];
        }

        // checklist da aba JOGAR: estado da senha (aviso quando vazia)
        Row PassRow()
        {
            if (savedPass == null)
                return new Row { State = St.Warn, Title = "Senha do co-op", Detail = "Ainda sem senha. Defina em SENHA DO CO-OP, no painel acima: vocês dois precisam da mesma.", ActionText = "Gerar uma", Action = () => { SetDraft(GeneratePassphrase()); SetPassShown(true); Flash("Senha nova gerada, ainda não salva. Clique em SALVAR e passe a MESMA para seu amigo (use COPIAR).", cWarn); } };
            switch (IniState())
            {
                case IniSt.Igual: return new Row { State = St.Ok, Title = "Senha do co-op", Detail = "Definida e gravada no Seamless. Seu amigo precisa da mesma." };
                case IniSt.Sem: return new Row { State = St.Warn, Title = "Senha do co-op", Detail = "Salva neste PC. Vai para o Seamless quando ele for instalado." };
                default: return new Row { State = St.Warn, Title = "Senha do co-op", Detail = "Salva, mas o Seamless ainda tem outra. O JOGAR grava antes de abrir.", ActionText = "Gravar agora", Action = async () => { await SavePassClicked(true); } };
            }
        }
    }
}
