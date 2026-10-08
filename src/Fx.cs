// Camada visual/sonora do MineRing Launcher: cena pixel-art por codigo (sem imagens externas), sons 8-bit sintetizados e musica.
// C# 5 (csc do Windows). Nada aqui toca na logica de detectar/atualizar/jogar.
using System;
using System.Collections.Generic;
using System.IO;
using System.Media;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace EldenMinecraftLauncher
{
    static class Cx
    {
        public static int Rgb(int r, int g, int b) { return unchecked((int)0xFF000000) | (r << 16) | (g << 8) | b; }
        public static int Hex(string s) { int v = Convert.ToInt32(s.TrimStart('#'), 16); return unchecked((int)0xFF000000) | v; }
        public static int Cl(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
        public static double Cl01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
        public static int Mix(int a, int b, double t)
        {
            t = Cl01(t);
            int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
            int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
            int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
            return Rgb(r, g, bl);
        }
        public static int Mul(int c, double f)
        {
            return Rgb(Cl((int)(((c >> 16) & 255) * f)), Cl((int)(((c >> 8) & 255) * f)), Cl((int)((c & 255) * f)));
        }
        public static int Add(int c, int r, int g, int b)
        {
            return Rgb(Cl(((c >> 16) & 255) + r), Cl(((c >> 8) & 255) + g), Cl((c & 255) + b));
        }
        public static double Hash(int x, int y, int s)
        {
            unchecked
            {
                uint h = (uint)(x * 374761393 + y * 668265263 + s * 362437);
                h = (h ^ (h >> 13)) * 1274126177u; h ^= h >> 16;
                return (h & 0xFFFFFF) / (double)0x1000000;
            }
        }
        public static double Smooth(double t) { t = Cl01(t); return t * t * (3 - 2 * t); }
        public static double Ease(double t) { t = Cl01(t); return 1 - (1 - t) * (1 - t) * (1 - t); }
    }

    class Ember { public double x, y, vx, vy, life, max, ph; public int sz; }

    class PixelScene
    {
        public const double Beat = 60.0 / 92.0;   // BPM medido do mp3 (~92)
        public int W, H; public WriteableBitmap Bmp;
        public bool Low; public int Px;
        int[] frame, sky, glowAmt, vig, mist;
        int hy, pl; double cx;
        // arvore
        int[] tx, ty, tc; double[] tph; int tn;
        // sprite
        const int SW = 64, SH = 50, OY = 6;
        int[] sx, sy, sc; int sn;             // corpo (x,y,cor)
        int[] cpx, cpy, cpc; double[] cpw; int cpn;   // capa dinamica
        double sprX; int sprY;
        // particulas
        List<Ember> embers = new List<Ember>(); Random rnd = new Random(5);
        public int EmberTarget = 90;
        // logo
        static readonly Dictionary<char, string[]> Font = MakeFont();
        double lastT = -1; int burstIdx = -1;
        public Action<string> OnCue;          // "tick", "logo"
        bool logoCue;

        static Dictionary<char, string[]> MakeFont()
        {
            var f = new Dictionary<char, string[]>();
            f['E'] = new[] { "11111", "10000", "10000", "11110", "10000", "10000", "11111" };
            f['L'] = new[] { "10000", "10000", "10000", "10000", "10000", "10000", "11111" };
            f['D'] = new[] { "11110", "10001", "10001", "10001", "10001", "10001", "11110" };
            f['N'] = new[] { "10001", "11001", "10101", "10101", "10011", "10001", "10001" };
            f['R'] = new[] { "11110", "10001", "10001", "11110", "10100", "10010", "10001" };
            f['I'] = new[] { "11111", "00100", "00100", "00100", "00100", "00100", "11111" };
            f['G'] = new[] { "01111", "10000", "10000", "10011", "10001", "10001", "01111" };
            f['M'] = new[] { "10001", "11011", "10101", "10101", "10001", "10001", "10001" };
            f['C'] = new[] { "01111", "10000", "10000", "10000", "10000", "10000", "01111" };
            f['A'] = new[] { "01110", "10001", "10001", "11111", "10001", "10001", "10001" };
            f['F'] = new[] { "11111", "10000", "10000", "11110", "10000", "10000", "10000" };
            f['T'] = new[] { "11111", "00100", "00100", "00100", "00100", "00100", "00100" };
            f['X'] = new[] { "10001", "10001", "01010", "00100", "01010", "10001", "10001" };
            return f;
        }

        public PixelScene(int w, int h, int px)
        {
            W = w; H = h; Px = px;
            frame = new int[W * H];
            Bmp = new WriteableBitmap(W, H, 96, 96, PixelFormats.Bgr32, null);
            hy = (int)(H * 0.60); pl = (int)(H * 0.60); cx = W * 0.62;
            BuildSky(); BuildTree(); BuildSprite(); BuildMist(); BuildVig();
            for (int i = 0; i < 40; i++) { var e = NewEmber(true); embers.Add(e); }
        }

        static readonly int[] Bayer = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
        double Dith(int x, int y) { return (Bayer[(y & 3) * 4 + (x & 3)] + 0.5) / 16.0 - 0.5; }

        void BuildSky()
        {
            sky = new int[W * H]; glowAmt = new int[W * H];
            int top = Cx.Rgb(4, 3, 2), hor = Cx.Rgb(54, 32, 12);
            double ccx = cx, ccy = hy - H * 0.30, R = W * 0.70;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    double t = Math.Pow(Cx.Cl01((double)y / hy), 1.6);
                    double q = Math.Floor(t * 9 + Dith(x, y) + 0.5) / 9.0;
                    sky[y * W + x] = Cx.Mix(top, hor, q);
                    double dx = (x - ccx) / R, dy = (y - ccy) / (R * 0.8);
                    double d = Math.Sqrt(dx * dx + dy * dy);
                    double a = Math.Pow(Cx.Cl01(1 - d), 2.1);
                    double hz = Math.Exp(-Math.Pow((y - hy) / (H * 0.14), 2)) * 0.55 * Cx.Cl01(1 - Math.Abs(x - ccx) / (W * 0.95));
                    a = Math.Max(a, hz);
                    double qa = Math.Floor(a * 8 + Dith(x, y) + 0.5) / 8.0;
                    glowAmt[y * W + x] = (int)(Cx.Cl01(qa) * 255);
                }
        }

        void Leaf(List<int> L, int x, int y, int c, double ph, int type) { }

        void BuildTree()
        {
            var lx = new List<int>(); var ly = new List<int>(); var lc = new List<int>(); var lp = new List<double>();
            int trunkTop = (int)(hy - H * 0.31); int baseY = hy + 8;
            // tronco
            for (int y = trunkTop; y <= baseY; y++)
            {
                double t = (double)(y - trunkTop) / (baseY - trunkTop);
                double hw = 2.6 + 10.5 * Math.Pow(t, 3.4);
                for (int x = (int)(cx - hw); x <= (int)(cx + hw); x++)
                {
                    double u = (x - cx) / hw;
                    double sh = 0.62 - 0.5 * u + (Cx.Hash(x, y / 9, 3) - 0.5) * 0.34;
                    int c = sh > 0.78 ? Cx.Hex("#FFE9A0") : sh > 0.56 ? Cx.Hex("#E8B545") : sh > 0.34 ? Cx.Hex("#B77F1F") : Cx.Hex("#6B430E");
                    if (Cx.Hash(x, y / 7, 9) > 0.88) c = Cx.Mul(c, 0.72);
                    lx.Add(x); ly.Add(y); lc.Add(c); lp.Add(-1);
                }
            }
            // galhos
            var r = new Random(21);
            double ccy = trunkTop - H * 0.05;
            for (int k = 0; k < 7; k++)
            {
                double ang = Math.PI * (0.12 + 0.76 * k / 6.0);
                Branch(lx, ly, lc, lp, r, cx + (k - 3) * 1.0, trunkTop + 10, ang, W * 0.20, 2.8, 3);
            }
            // copa
            double rx = W * 0.37, ry = H * 0.20, ccx = cx - W * 0.03;
            ccy = trunkTop - H * 0.075;
            int[] pal = { Cx.Hex("#FFF6C4"), Cx.Hex("#FFDE7E"), Cx.Hex("#EDB846"), Cx.Hex("#BD8421"), Cx.Hex("#7C4F10") };
            for (int y = (int)(ccy - ry * 1.4); y < (int)(ccy + ry * 1.1); y++)
                for (int x = (int)(ccx - rx * 1.2); x < (int)(ccx + rx * 1.2); x++)
                {
                    if (x < 0 || x >= W || y < 0) continue;
                    double dx = (x - ccx) / rx, dy = (y - ccy) / ry;
                    if (dy > 0) dy *= 1.7;
                    double ang = Math.Atan2(dy, dx);
                    double edge = 1.0 + 0.16 * (Cx.Hash((int)(ang * 6), 1, 4) - 0.5) * 2 + 0.07 * Math.Sin(ang * 9);
                    double r2 = Math.Sqrt(dx * dx + dy * dy);
                    if (r2 > edge) continue;
                    double rr = r2 / edge;
                    double n = Cx.Hash(x / 3, y / 3, 5) * 0.5 + Cx.Hash(x / 6, y / 5, 15) * 0.3 + Cx.Hash(x, y, 6) * 0.2;
                    if (n > 0.93 - rr * 0.60) continue;
                    double v = 1 - rr * 0.78 + (Cx.Hash(x, y, 7) - 0.5) * 0.32 + (-dy * 0.12);
                    int idx = v > 0.82 ? 0 : v > 0.60 ? 1 : v > 0.40 ? 2 : v > 0.22 ? 3 : 4;
                    lx.Add(x); ly.Add(y); lc.Add(pal[idx]); lp.Add(Cx.Hash(x, y, 8) > 0.55 ? Cx.Hash(x, y, 2) * 6.28 : -1);
                }
            tn = lx.Count; tx = lx.ToArray(); ty = ly.ToArray(); tc = lc.ToArray(); tph = lp.ToArray();
        }

        void Branch(List<int> lx, List<int> ly, List<int> lc, List<double> lp, Random r, double x, double y, double ang, double len, double wid, int depth)
        {
            int steps = (int)(len * 1.4);
            double px = x, py = y;
            for (int i = 0; i < steps; i++)
            {
                double f = (double)i / steps;
                ang += (r.NextDouble() - 0.5) * 0.12;
                px += Math.Cos(ang) * 0.75; py -= Math.Sin(ang) * 0.75;
                double w = Math.Max(0.6, wid * (1 - f * 0.8));
                for (int dx = (int)(-w / 2); dx <= (int)(w / 2); dx++)
                {
                    int X = (int)px + dx, Y = (int)py;
                    if (X < 0 || X >= W || Y < 0 || Y >= H) continue;
                    lx.Add(X); ly.Add(Y); lc.Add(dx < 0 ? Cx.Hex("#D9A23E") : Cx.Hex("#7A4C10")); lp.Add(-1);
                }
            }
            if (depth > 0)
            {
                int kids = 2 + r.Next(2);
                for (int k = 0; k < kids; k++)
                    Branch(lx, ly, lc, lp, r, px, py, ang + (k - (kids - 1) / 2.0) * 0.6 + (r.NextDouble() - 0.5) * 0.3, len * 0.55, wid * 0.62, depth - 1);
            }
        }

        // ---------- sprite: Tarnished + Torrent (silhueta com luz de borda) ----------
        void Ell(byte[] m, double cx_, double cy, double rx, double ry, byte v)
        {
            cy += OY;
            for (int y = (int)(cy - ry - 1); y <= cy + ry + 1; y++)
                for (int x = (int)(cx_ - rx - 1); x <= cx_ + rx + 1; x++)
                {
                    if (x < 0 || y < 0 || x >= SW || y >= SH) continue;
                    double dx = (x - cx_) / rx, dy = (y - cy) / ry;
                    if (dx * dx + dy * dy <= 1) m[y * SW + x] = v;
                }
        }
        void Poly(byte[] m, double[] p, byte v)
        {
            int n = p.Length / 2;
            for (int y = 0; y < SH; y++)
                for (int x = 0; x < SW; x++)
                {
                    bool inside = false; double yy = y - OY + 0.5, xx = x + 0.5;
                    for (int i = 0, j = n - 1; i < n; j = i++)
                    {
                        double xi = p[i * 2], yi = p[i * 2 + 1], xj = p[j * 2], yj = p[j * 2 + 1];
                        if (((yi > yy) != (yj > yy)) && (xx < (xj - xi) * (yy - yi) / (yj - yi) + xi)) inside = !inside;
                    }
                    if (inside) m[y * SW + x] = v;
                }
        }
        void Ln(byte[] m, double x0, double y0, double x1, double y1, double th, byte v)
        {
            int n = (int)(Math.Max(Math.Abs(x1 - x0), Math.Abs(y1 - y0)) * 2) + 1;
            for (int i = 0; i <= n; i++)
            {
                double t = (double)i / n; Ell(m, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, th / 2, th / 2, v);
            }
        }

        void BuildSprite()
        {
            var m = new byte[SW * SH];
            // cavalo (Torrent)
            Ell(m, 30, 26, 15, 6.5, 1);
            Poly(m, new double[] { 36, 22, 43, 11, 48, 12, 43, 28 }, 1);                           // pescoco
            Poly(m, new double[] { 44, 11, 50, 9, 52, 12, 59, 20, 57, 23, 50, 19, 45, 16 }, 1);   // cabeca
            Poly(m, new double[] { 47, 10, 48, 5, 50, 10 }, 1);                                      // orelha
            Ln(m, 38, 30, 41, 43, 2.4, 1); Ln(m, 35, 31, 33, 42, 2.2, 1);                            // patas dianteiras
            Ln(m, 21, 31, 17, 43, 2.6, 1); Ln(m, 25, 32, 26, 43, 2.2, 1);                            // traseiras
            // Tarnished (capuz, tronco, espada)
            Ell(m, 31, 16, 4.2, 7, 1);
            Ell(m, 30, 6, 4.2, 4.2, 1);
            Poly(m, new double[] { 26, 7, 29, -1, 34, 8 }, 1);                                       // ponta do capuz
            Ln(m, 35, 14, 41, 5, 1.6, 1);                                                             // braco
            Ln(m, 41, 5, 48, -3, 1.2, 1);                                                             // espada
            Ln(m, 38, 8, 44, 11, 1.2, 1);                                                             // guarda
            // capa (dinamica)
            var cm = new byte[SW * SH];
            Poly(cm, new double[] { 28, 10, 22, 13, 8, 23, 4, 30, 12, 29, 20, 27, 31, 23, 35, 17, 34, 10 }, 2);
            // combinar para detectar bordas
            Func<int, int, bool> occ = (x, y) => x >= 0 && y >= 0 && x < SW && y < SH && (m[y * SW + x] != 0 || cm[y * SW + x] != 0);
            int dark = Cx.Hex("#0A0705"), dark2 = Cx.Hex("#14100A"), rim = Cx.Hex("#F0C257"), rimH = Cx.Hex("#FFF0B0");
            var a = new List<int>(); var b = new List<int>(); var c = new List<int>();
            var ca = new List<int>(); var cb = new List<int>(); var cc = new List<int>(); var cw = new List<double>();
            for (int y = 0; y < SH; y++)
                for (int x = 0; x < SW; x++)
                {
                    bool body = m[y * SW + x] != 0, cape = cm[y * SW + x] != 0;
                    if (!body && !cape) continue;
                    bool edge = !occ(x + 1, y) || !occ(x, y - 1) || (!occ(x + 1, y - 1));
                    int col = edge ? (Cx.Hash(x, y, 1) > 0.5 ? rim : rimH) : (Cx.Hash(x, y, 2) > 0.8 ? dark2 : dark);
                    if (edge && !(cape && !body) && (!occ(x + 1, y - 1)) && !occ(x, y - 1) && !occ(x + 1, y)) col = rimH;
                    if (body) { a.Add(x); b.Add(y); c.Add(col); }
                    else { ca.Add(x); cb.Add(y); cc.Add(col); cw.Add(Math.Max(0, (32 - x) / 28.0)); }
                }
            sx = a.ToArray(); sy = b.ToArray(); sc = c.ToArray(); sn = sx.Length;
            cpx = ca.ToArray(); cpy = cb.ToArray(); cpc = cc.ToArray(); cpw = cw.ToArray(); cpn = cpx.Length;
        }

        void BuildMist()
        {
            mist = new int[256 * 24];
            for (int y = 0; y < 24; y++)
                for (int x = 0; x < 256; x++)
                {
                    double v = 0;
                    for (int o = 0; o < 3; o++)
                    {
                        int cell = 8 << o; int gx = x / cell, gy = y / Math.Max(2, cell / 3);
                        double fx = (double)(x % cell) / cell;
                        double a = Cx.Hash(gx % (256 / cell), gy, 40 + o), b = Cx.Hash((gx + 1) % (256 / cell), gy, 40 + o);
                        v += (a + (b - a) * Cx.Smooth(fx)) / (1 << o);
                    }
                    mist[y * 256 + x] = (int)(Cx.Cl01(v / 1.75) * 255);
                }
        }

        void BuildVig()
        {
            vig = new int[W * H];
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    double dx = (x - W / 2.0) / (W / 2.0), dy = (y - H / 2.0) / (H / 2.0);
                    double v = 1 - 0.50 * (dx * dx * 0.7 + dy * dy * 0.9);
                    vig[y * W + x] = (int)(Cx.Cl01(v) * 256);
                }
        }

        Ember NewEmber(bool anywhere)
        {
            var e = new Ember();
            e.x = rnd.NextDouble() * W; e.y = anywhere ? rnd.NextDouble() * H : H + 2 + rnd.NextDouble() * 6;
            e.vy = -(7 + rnd.NextDouble() * 20); e.vx = (rnd.NextDouble() - 0.4) * 4; e.max = 5 + rnd.NextDouble() * 9; e.life = anywhere ? rnd.NextDouble() * e.max : 0;
            e.ph = rnd.NextDouble() * 6.28; e.sz = rnd.NextDouble() > 0.88 ? 2 : 1;
            return e;
        }

        void Burst(double x, double y, int n)
        {
            for (int i = 0; i < n; i++)
            {
                var e = new Ember(); double a = rnd.NextDouble() * 6.28, s = 10 + rnd.NextDouble() * 40;
                e.x = x; e.y = y; e.vx = Math.Cos(a) * s; e.vy = Math.Sin(a) * s - 18; e.max = 0.7 + rnd.NextDouble() * 1.0; e.life = 0; e.ph = rnd.NextDouble() * 6; e.sz = rnd.NextDouble() > 0.7 ? 2 : 1;
                embers.Add(e);
            }
        }

        // ---------- render ----------
        int EmberColor(double f)
        {
            if (f < 0.25) return Cx.Rgb(255, 238, 170);
            if (f < 0.55) return Cx.Rgb(240, 176, 56);
            if (f < 0.8) return Cx.Rgb(200, 100, 28);
            return Cx.Rgb(120, 48, 16);
        }

        void Put(int x, int y, int c) { if ((uint)x < (uint)W && (uint)y < (uint)H) frame[y * W + x] = c; }
        void Blend(int x, int y, int c, double a)
        {
            if ((uint)x >= (uint)W || (uint)y >= (uint)H || a <= 0) return;
            frame[y * W + x] = a >= 1 ? c : Cx.Mix(frame[y * W + x], c, a);
        }
        void Rect(int x, int y, int w, int h, int c, double a)
        {
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) Blend(x + i, y + j, c, a);
        }

        double HillTop(int x, double off, double baseY, double amp, double f)
        {
            double a = x + off;
            return baseY + amp * (0.55 * Math.Sin(a * 0.041 * f) + 0.30 * Math.Sin(a * 0.097 * f + 2) + 0.15 * Math.Sin(a * 0.23 * f + 1));
        }

        void DrawHills(double off, double baseY, double amp, double f, int col, int rimCol, double lit)
        {
            for (int x = 0; x < W; x++)
            {
                int top = (int)HillTop(x, off, baseY, amp, f);
                if (top < 0) top = 0;
                for (int y = top; y < H; y++) frame[y * W + x] = col;
                if (top < H) frame[top * W + x] = Cx.Mix(col, rimCol, lit);
            }
        }

        void DrawMist(double t, int y0, int rows, double speed, double alpha, int col)
        {
            int off = (int)(t * speed);
            for (int j = 0; j < rows; j++)
            {
                double vf = Math.Sin(Math.PI * (j + 0.5) / rows);
                int y = y0 + j; if (y < 0 || y >= H) continue;
                for (int x = 0; x < W; x++)
                {
                    int m = mist[(j % 24) * 256 + (((x + off) % 256) + 256) % 256];
                    double a = Math.Max(0, (m / 255.0 - 0.38)) * alpha * vf * 2.2;
                    a = Math.Floor(a * 5 + Dith(x, y) + 0.5) / 5.0;
                    if (a > 0) frame[y * W + x] = Cx.Mix(frame[y * W + x], col, a);
                }
            }
        }

        void DrawGlyph(char ch, int x, int y, int s, int cTop, int cBot, double a, double flash)
        {
            string[] g;
            if (!Font.TryGetValue(ch, out g)) return;
            int dark = Cx.Hex("#140A02"), ext = Cx.Hex("#4A3009");
            if (cTop == Cx.Hex("#E6E6E6") || cTop == Cx.Hex("#D4D4D4")) ext = Cx.Hex("#2A2A2E");
            int ex = Math.Max(1, s / 2 + 1);
            Func<int, int, bool> on = (cx_, cy_) => cx_ >= 0 && cy_ >= 0 && cx_ < 5 && cy_ < 7 && g[cy_][cx_] == '1';
            for (int cy_ = 0; cy_ < 7; cy_++)
                for (int cx_ = 0; cx_ < 5; cx_++)
                {
                    if (!on(cx_, cy_)) continue;
                    for (int d = 1; d <= ex; d++) Rect(x + cx_ * s + d, y + cy_ * s + d, s, s, ext, a);
                }
            for (int cy_ = 0; cy_ < 7; cy_++)
                for (int cx_ = 0; cx_ < 5; cx_++)
                {
                    if (!on(cx_, cy_)) continue;
                    Rect(x + cx_ * s - 1, y + cy_ * s - 1, s + 2, s + 2, dark, a);
                }
            for (int cy_ = 0; cy_ < 7; cy_++)
                for (int cx_ = 0; cx_ < 5; cx_++)
                {
                    if (!on(cx_, cy_)) continue;
                    int baseC = Cx.Mix(cTop, cBot, cy_ / 6.0);
                    bool tUp = !on(cx_, cy_ - 1), tLf = !on(cx_ - 1, cy_), tDn = !on(cx_, cy_ + 1), tRt = !on(cx_ + 1, cy_);
                    for (int j = 0; j < s; j++)
                        for (int i = 0; i < s; i++)
                        {
                            int c = baseC;
                            if ((tUp && j == 0) || (tLf && i == 0)) c = Cx.Mix(c, Cx.Rgb(255, 255, 255), 0.45);
                            else if ((tDn && j == s - 1) || (tRt && i == s - 1)) c = Cx.Mul(c, 0.62);
                            else if (Cx.Hash(x + cx_ * s + i, y + cy_ * s + j, 12) > 0.82) c = Cx.Mul(c, 0.9);
                            if (flash > 0) c = Cx.Mix(c, Cx.Rgb(255, 255, 255), flash * 0.85);
                            Blend(x + cx_ * s + i, y + cy_ * s + j, c, a);
                        }
                }
        }

        public int LogoScale() { int s = (int)(W * 0.74 / 47.0); return s < 3 ? 3 : s; }

        // intro: it >= 0. Ambiente: it < 0.
        public void Render(double t, double it, double bright)
        {
            bool intro = it >= 0;
            double dt = lastT < 0 ? 0.016 : Math.Min(0.1, t - lastT); lastT = t;
            double g = intro ? Cx.Ease((it - 1.3) / 2.8) : 1.0;
            double sp = intro ? Cx.Ease((it - 2.7) / 2.4) : 1.0;
            double pulse = 0.93 + 0.07 * Math.Sin(t * 1.4);
            int gi = (int)(g * pulse * 256);

            // ceu + brilho
            for (int i = 0; i < frame.Length; i++)
            {
                int s0 = sky[i]; int a = (glowAmt[i] * gi) >> 8;
                if (a == 0) { frame[i] = s0; continue; }
                int r = Cx.Cl(((s0 >> 16) & 255) + (255 * a >> 8)), gg = Cx.Cl(((s0 >> 8) & 255) + (170 * a >> 8)), b = Cx.Cl((s0 & 255) + (58 * a >> 8));
                frame[i] = unchecked((int)0xFF000000) | (r << 16) | (gg << 8) | b;
            }
            // arvore
            double tb = g * pulse;
            for (int i = 0; i < tn; i++)
            {
                double f = tb;
                if (tph[i] >= 0) f *= 0.82 + 0.28 * Math.Sin(t * 2.2 + tph[i]);
                int y = ty[i]; if (y > hy + 6) continue;
                if (f < 0.02) continue;
                int idx = y * W + tx[i];
                frame[idx] = Cx.Mix(frame[idx], Cx.Mul(tc[i], Math.Min(1.25, f)), Math.Min(1, f * 1.6));
            }
            // colinas e neblina (parallax)
            double lit = 0.15 + 0.7 * g;
            DrawHills(t * 0.5, hy + 2, 5, 0.8, Cx.Mix(Cx.Rgb(4, 3, 2), Cx.Rgb(34, 22, 9), g), Cx.Rgb(120, 84, 30), lit * 0.5);
            if (!Low) DrawMist(t, hy - 4, 16, 5, 0.30 * g, Cx.Rgb(150, 105, 40));
            DrawHills(t * 1.4 + 90, hy + 15, 6, 1.3, Cx.Mix(Cx.Rgb(3, 2, 1), Cx.Rgb(20, 13, 6), g), Cx.Rgb(80, 54, 20), lit * 0.35);
            if (!Low) DrawMist(-t, hy + 14, 12, 9, 0.22 * g, Cx.Rgb(110, 76, 28));

            // penhasco em primeiro plano
            int cliffEnd = (int)(W * 0.34), slopeEnd = (int)(W * 0.50);
            int fgc = Cx.Hex("#060402"), fgc2 = Cx.Hex("#0B0805");
            for (int x = 0; x < W; x++)
            {
                int top;
                if (x < cliffEnd) top = pl + (int)(Cx.Hash(x / 3, 0, 9) * 3);
                else if (x < slopeEnd) top = pl + (int)((H * 0.30) * Cx.Smooth((double)(x - cliffEnd) / (slopeEnd - cliffEnd)));
                else top = (int)(H * 0.905 + 2 * Math.Sin(x * 0.08) + Cx.Hash(x, 0, 10) * 2 + (Cx.Hash(x, 1, 11) > 0.72 ? -3 : 0));
                for (int y = top; y < H; y++) frame[y * W + x] = Cx.Hash(x, y, 13) > 0.9 ? fgc2 : fgc;
                if (top >= 0 && top < H) frame[top * W + x] = Cx.Mix(fgc, Cx.Rgb(110, 76, 24), 0.35 * lit);
            }

            // Tarnished + Torrent
            sprX = W * 0.09; sprY = pl - 48 - 1 + 0;
            int ox = (int)sprX, oy = sprY;
            double bob = Math.Sin(t * 1.1) > 0.7 ? 1 : 0;
            if (sp > 0)
            {
                for (int i = 0; i < sn; i++)
                {
                    int col = sc[i]; bool isRim = ((col >> 8) & 255) > 100;
                    int px = ox + sx[i], py = oy + sy[i] + (sy[i] < 24 ? (int)bob : 0);
                    int cc = isRim ? Cx.Mul(col, 0.35 + 0.65 * g) : col;
                    Blend(px, py, cc, sp);
                }
                for (int i = 0; i < cpn; i++)
                {
                    int col = cpc[i]; bool isRim = ((col >> 8) & 255) > 100;
                    double w = cpw[i];
                    int px = ox + cpx[i] + (int)Math.Round(Math.Sin(t * 3.0 - cpx[i] * 0.25) * w * 1.5);
                    int py = oy + cpy[i] + (int)Math.Round(Math.Sin(t * 4.0 - cpx[i] * 0.35) * w * 2.0) + (int)bob;
                    Blend(px, py, isRim ? Cx.Mul(col, 0.35 + 0.65 * g) : col, sp);
                }
                // cauda e crina espectrais (Torrent)
                for (int k = 0; k < 2; k++)
                    for (int i = 0; i < 26; i++)
                    {
                        double u = i / 25.0;
                        double x = 16 - u * 17, y = 29 + u * 12 + Math.Sin(t * 3 + u * 5 + k) * u * 3.5 + k;
                        Blend(ox + (int)x, oy + (int)y + OY, Cx.Mix(Cx.Rgb(255, 232, 150), Cx.Rgb(180, 110, 30), u), sp * (1 - u * 0.5) * g);
                    }
                for (int i = 0; i < 14; i++)
                {
                    double u = i / 13.0; double x = 43 - u * 8 + Math.Sin(t * 6 + i) * 0.7, y = 18 + u * 12 + Math.Cos(t * 5 + i) * 0.7;
                    Blend(ox + (int)x, oy + (int)y + OY, Cx.Mix(Cx.Rgb(255, 240, 170), Cx.Rgb(210, 140, 40), u), sp * g);
                }
                // olho brilhante
                Blend(ox + 53, oy + 22 + OY, Cx.Rgb(255, 230, 140), sp * g);
            }

            // brasas
            int target = Low ? 22 : EmberTarget;
            int normal = 0; foreach (var e0 in embers) if (e0.max > 4) normal++;
            while (normal < target) { embers.Add(NewEmber(false)); normal++; }
            for (int i = embers.Count - 1; i >= 0; i--)
            {
                var e = embers[i]; e.life += dt;
                if (e.life >= e.max || e.y < -3) { embers.RemoveAt(i); continue; }
                bool burst = e.max < 3;
                if (burst) e.vy += 26 * dt;
                e.x += (e.vx + (burst ? 0 : Math.Sin(t * 1.3 + e.ph) * 5)) * dt; e.y += e.vy * dt;
                double f = e.life / e.max;
                int c = EmberColor(f);
                double a = (1 - f * f) * (intro ? Math.Min(1, it / 1.2 + 0.0) : 1);
                if (e.sz == 2) { Blend((int)e.x, (int)e.y, c, a); Blend((int)e.x + 1, (int)e.y, c, a * 0.8); Blend((int)e.x, (int)e.y + 1, c, a * 0.8); Blend((int)e.x + 1, (int)e.y + 1, c, a * 0.6); }
                else Blend((int)e.x, (int)e.y, c, a);
            }

            // logo (so na intro)
            double flashAll = 0;
            if (intro)
            {
                double lt0 = 8 * Beat; int s = LogoScale();
                string all = "MINERING";
                int gw = 5 * s, gap = s;
                int total = all.Length * gw + (all.Length - 1) * gap;
                int x0 = (W - total) / 2, y0 = (int)(H * 0.655);
                double lp = Cx.Smooth((it - lt0 + 0.8) / 1.4);
                if (lp > 0) // escurecer faixa atras do logo
                    for (int y = (int)(H * 0.58); y < (int)(H * 0.93); y++)
                    {
                        double v = Math.Sin(Math.PI * (y - H * 0.58) / (H * 0.35)); v = Math.Pow(Cx.Cl01(v), 0.7) * 0.62 * lp;
                        for (int x = 0; x < W; x++) frame[y * W + x] = Cx.Mul(frame[y * W + x], 1 - v);
                    }
                int xc = x0; int k = 0;
                for (int ci = 0; ci < all.Length; ci++)
                {
                    char ch = all[ci];
                    if (ch == ' ') { xc += gw; continue; }
                    double Tk = lt0 + (k + 1) * 0.5 * Beat;
                    double p = (it - Tk) / 0.40;
                    if (p > 0)
                    {
                        double a = Cx.Ease(p * 1.6);
                        int yo = (int)((1 - Cx.Ease(p)) * -14);
                        double fl = 1 - Cx.Cl01((it - Tk) / 0.30);
                        if (k < 4) DrawGlyph(ch, xc, y0 + yo, s, Cx.Hex("#D4D4D4"), Cx.Hex("#707070"), a, fl);
                        else DrawGlyph(ch, xc, y0 + yo, s, Cx.Hex("#FFF3B8"), Cx.Hex("#B07A1C"), a, fl);
                        if (k > burstIdx) { burstIdx = k; Burst(xc + gw / 2, y0 + 3 * s, 10); if (OnCue != null) OnCue("tick"); }
                    }
                    xc += gw + gap; k++;
                }
                double endT = lt0 + 9 * 0.5 * Beat;
                if (it > endT)
                {
                    if (!logoCue) { logoCue = true; if (OnCue != null) OnCue("logo"); Burst(W / 2.0, y0 + 3 * s, 40); }
                    flashAll = Math.Exp(-(it - endT) * 5);
                    // faiscas correndo pelo logo
                    double sx0 = x0 + ((it * 60) % (total + 40)) - 20;
                    for (int j = -2; j <= 2; j++) Blend((int)sx0 + j, y0 + (int)(3.5 * s) + j, Cx.Rgb(255, 255, 255), 0.5);
                }
                // subtitulo MINECRAFT x ELDEN RING (menor)
                double mt = 13 * Beat; double mp = Cx.Ease((it - mt) / 0.6);
                if (mp > 0)
                {
                    int s2 = Math.Max(1, (int)Math.Round(s / 3.5));
                    string sub = "MINECRAFT X ELDEN RING";
                    int tot2 = 0; foreach (char c2 in sub) tot2 += (c2 == ' ' ? 4 : 6) * s2; tot2 -= s2;
                    int xx = (W - tot2) / 2, yy = y0 + 7 * s + s * 2 + 4 + (int)((1 - mp) * 6);
                    for (int i = 0; i < sub.Length; i++)
                    {
                        char c2 = sub[i];
                        if (c2 == ' ') { xx += 4 * s2; continue; }
                        bool mcPart = i < sub.IndexOf('X');
                        if (mcPart) DrawGlyph(c2, xx, yy, s2, Cx.Hex("#D4D4D4"), Cx.Hex("#707070"), mp, 0);
                        else DrawGlyph(c2, xx, yy, s2, Cx.Hex("#F0D283"), Cx.Hex("#A87420"), mp, 0);
                        xx += 6 * s2;
                    }
                }
            }

            // vinheta + brilho global
            double bb = bright * (1 + 0.45 * flashAll);
            int bi = (int)(bb * 256);
            var px32 = frame;
            for (int i = 0; i < px32.Length; i++)
            {
                int c = px32[i]; int v = (vig[i] * bi) >> 8;
                int r = (((c >> 16) & 255) * v) >> 8, gg = (((c >> 8) & 255) * v) >> 8, b = ((c & 255) * v) >> 8;
                if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
                px32[i] = (r << 16) | (gg << 8) | b;
            }
            Bmp.WritePixels(new Int32Rect(0, 0, W, H), frame, W * 4, 0);
        }
    }

    // ---------- sons 8-bit sintetizados ----------
    class Sfx
    {
        public bool Muted; public double Vol = 0.6;
        SoundPlayer cur; DateTime lastHover = DateTime.MinValue;
        static readonly Dictionary<string, double[][]> Defs = new Dictionary<string, double[][]> {
            { "hover", new[] { new[] { 1318.0, 20, 0.25, 0 } } },
            { "click", new[] { new[] { 880.0, 34, 0.5, 1320 } } },
            { "tab", new[] { new[] { 660.0, 34, 0.5, 0 }, new[] { 990.0, 50, 0.5, 0 } } },
            { "tick", new[] { new[] { 1568.0, 28, 0.25, 2093 } } },
            { "logo", new[] { new[] { 392.0, 110, 0.5, 0 }, new[] { 523.0, 110, 0.5, 0 }, new[] { 784.0, 110, 0.5, 0 }, new[] { 1047.0, 420, 0.25, 0 } } },
            { "confirm", new[] { new[] { 523.0, 70, 0.5, 0 }, new[] { 659.0, 70, 0.5, 0 }, new[] { 784.0, 70, 0.5, 0 }, new[] { 1047.0, 260, 0.25, 0 } } },
            { "error", new[] { new[] { 196.0, 110, 0.5, 147 }, new[] { 147.0, 170, 0.5, 120 } } },
            { "volume", new[] { new[] { 988.0, 40, 0.5, 0 }, new[] { 1319.0, 60, 0.5, 0 } } }
        };

        static byte[] Wav(double[][] notes, double amp)
        {
            const int sr = 22050; var s = new List<short>(); double ph = 0;
            foreach (var n in notes)
            {
                int cnt = (int)(sr * n[1] / 1000.0); double f0 = n[0], f1 = n[3] > 0 ? n[3] : n[0];
                for (int i = 0; i < cnt; i++)
                {
                    double u = (double)i / cnt, f = f0 + (f1 - f0) * u;
                    ph += f / sr; ph -= Math.Floor(ph);
                    double w = ph < n[2] ? 1 : -1;
                    double env = Math.Min(1, i / 60.0) * Math.Pow(1 - u, 0.8);
                    s.Add((short)(w * env * amp * 32767));
                }
            }
            var ms = new MemoryStream(); var bw = new BinaryWriter(ms);
            int dl = s.Count * 2;
            bw.Write(new[] { 'R', 'I', 'F', 'F' }); bw.Write(36 + dl); bw.Write(new[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
            bw.Write(16); bw.Write((short)1); bw.Write((short)1); bw.Write(sr); bw.Write(sr * 2); bw.Write((short)2); bw.Write((short)16);
            bw.Write(new[] { 'd', 'a', 't', 'a' }); bw.Write(dl);
            foreach (var v in s) bw.Write(v);
            bw.Flush(); return ms.ToArray();
        }

        public void Play(string name)
        {
            if (Muted || Vol <= 0.001) return;
            if (name == "hover") { if ((DateTime.UtcNow - lastHover).TotalMilliseconds < 90) return; lastHover = DateTime.UtcNow; }
            double[][] d; if (!Defs.TryGetValue(name, out d)) return;
            try
            {
                var p = new SoundPlayer(new MemoryStream(Wav(d, 0.16 * Vol + (name == "hover" ? -0.03 * Vol : 0))));
                p.Play(); cur = p;
            }
            catch { }
        }
    }

    // ---------- musica de fundo em loop com fade-in ----------
    class Music
    {
        MediaPlayer p; DateTime t0; Action<string> log; bool started;
        public bool Opened; public string Error; public double Vol = 0.6; public bool Muted;
        public double FadeIn = 3.5; bool failed;
        public void Start(string path, Action<string> logger)
        {
            log = logger;
            if (!File.Exists(path)) { Error = "arquivo nao encontrado: " + path; log("Musica: " + Error); failed = true; return; }
            try
            {
                p = new MediaPlayer(); p.Volume = 0;
                p.MediaOpened += (s, e) => { Opened = true; log("Musica: arquivo aberto sem erro (" + (p.NaturalDuration.HasTimeSpan ? p.NaturalDuration.TimeSpan.TotalSeconds.ToString("0.0") : "?") + " s)."); };
                p.MediaFailed += (s, e) => { Error = e.ErrorException == null ? "falha" : e.ErrorException.Message; failed = true; log("Musica: FALHA ao abrir - " + Error); };
                p.MediaEnded += (s, e) => { try { p.Position = TimeSpan.Zero; p.Play(); } catch { } };
                p.Open(new Uri(path, UriKind.Absolute)); t0 = DateTime.UtcNow; p.Play(); started = true;
            }
            catch (Exception ex) { Error = ex.Message; failed = true; log("Musica: erro - " + ex.Message); }
        }
        public void Update()
        {
            if (!started || failed) return;
            double f = Math.Min(1, (DateTime.UtcNow - t0).TotalSeconds / FadeIn);
            try { p.Volume = Muted ? 0 : Vol * f * f; } catch { }
        }
        public double Position { get { try { return started ? p.Position.TotalSeconds : 0; } catch { return 0; } } }
        public bool Playing { get { return started && !failed && Opened; } }
    }
}
