# MineRing — guia para agentes (launcher)

Instruções curtas para quem for mexer neste repo. O site de download fica em outro repo privado e tem o guia dele.

## O que é

**MineRing** liga Minecraft e Elden Ring em co-op. Este repo tem o **launcher/instalador** (C# WPF, compilado com o `csc.exe` do Windows, sem instalar nada) e o **pack** (scripts PowerShell, ponte nativa e mod Fabric). Projeto de fã, MIT.

## Estrutura

- `src/` — `Launcher.cs` (janela, JOGAR/PARAR, detecção), `Installer.cs` (aba INSTALAR), `Players.cs` (nicks/UUIDs), `Senha.cs` (senha do co-op), `Fx.cs` (intro, fundo, música, sons), `Ui.xaml.txt` (interface).
- `pack/` — scripts e binários que o launcher chama (`pack/launcher/Run-*.ps1`, `pack/EldenMinecraft-Windows/...`).
- `assets/` — fonte Press Start 2P (OFL), `icon.ico`, `avatar.jpg`, `musica-8bit.mp3` (embutidos no `.exe` pelo `Build.ps1`).
- `tools/` — `New-Sandbox.ps1`, `Test-StopFlow.ps1`, `FakeGame.cs`/`New-FakeGames.ps1`, `New-Icon.ps1`, `Make-Dist.ps1`, `Publish-Release.ps1`.

## Compilar e testar

```powershell
powershell -File Build.ps1                       # gera MineRing-Launcher.exe
powershell -File tools\New-Sandbox.ps1 -Dir $env:TEMP\sb   # Steam/Elden FALSOS, nicks de teste
.\MineRing-Launcher.exe --dry-run --sandbox $env:TEMP\sb --no-intro --no-music --size 1040x780 --tab jogar --shot out.png
powershell -File tools\Test-StopFlow.ps1        # JOGAR/PARAR com processos falsos
```

Flags úteis: `--dry-run`, `--sandbox <pasta>`, `--no-intro`, `--no-music`, `--low-fx`, `--size LxA`, `--tab jogar|install|help`, `--shot <png>`, `--intro-shots <prefixo>`, `--skip-intro-at <s>`, `--exit-after-intro`, `--players-file <json>`, `--offline-fixtures <pasta>`.

## Regras importantes

- **Nunca** encerre processos por nome (`eldenring`, `java`, `javaw`, `prismlauncher`): o dono pode estar jogando. Em testes, só PIDs de processos falsos criados na sandbox.
- **Nunca** leia, imprima ou altere o `players.json` / `senha-coop.dat` reais do dono (`%LOCALAPPDATA%\EldenMinecraftLauncher`). Nicks, UUIDs e senhas em testes são fictícios.
- **Não renomeie** a pasta de estado `%LOCALAPPDATA%\EldenMinecraftLauncher`, o namespace interno, as pastas do pack nem o perfil Prism "EldenMinecraft Coop": quebraria instalações existentes.
- Senhas nunca vão para log, argumento de linha de comando, commit ou screenshot.
- O Seamless Co-op **não pode** ser redistribuído (o autor proíbe); o launcher baixa do autor. Mods (Fabric API, e4mc) são baixados do Modrinth com hash conferido.
- Fonte pixel só em múltiplos de 8px quando possível (nitidez).
- Commits: PT-BR, autor `chico <337442102+franguchico@users.noreply.github.com>`, e a linha final `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. A branch `main` é protegida contra push forçado e exclusão.

## Publicar nova versão

1. Mude o código, compile e teste (acima). Faça commit e push em `main`.
2. `powershell -File tools\Publish-Release.ps1 -Version vX.Y.Z [-Notes notas.md]`.
   O zip tem **sempre o nome `MineRing-Launcher.zip`**. É isso que mantém o link fixo `https://github.com/franguchico/MineRing/releases/latest/download/MineRing-Launcher.zip` sempre apontando para a última versão (o botão do site usa esse link; não precisa mexer no site).
3. Confirme: `curl -sIL <link fixo>` termina em `200` com `filename=MineRing-Launcher.zip`.
4. Se for criar a release pela interface do GitHub, anexe o zip com **exatamente** esse nome.

O `gh` precisa estar na conta **franguchico** (`gh auth status`; `gh auth switch --user franguchico`).

## O que ainda não foi testado de verdade

JOGAR/PARAR com os jogos reais, instalação em PC limpo, login Microsoft no Prism e a música audível. Só rodaram em sandbox com jogos falsos. Diga isso ao dono ao entregar mudanças nessas áreas.
