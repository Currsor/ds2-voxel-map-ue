using System;

// 精确复现 shader VoxelMap.usf 的 ray-box + DDA(LOD2)，对照暴力遍历，找漏判体素的情形。
// 坐标约定同 shader：体素 (x, y=高度, z)，block 本地 [0,4]。
class Program
{
    const float VoxelSize = 10.0f;

    // 阶梯地形：x=0,1 -> H=2 (Y 0..2 占用), x=2,3 -> H=1 (Y 0..1 占用)
    static bool TestBit(int lx, int ly, int lz)
    {
        int H = (lx < 2) ? 2 : 1;
        return ly <= H;
    }

    // 精确复现 shader DDA（LOD2），返回命中的本地坐标 (x,y,z) 或 null
    static int[] DDA(float ox, float oy, float oz, float dx, float dy, float dz)
    {
        float vx = dx / VoxelSize, vy = dy / VoxelSize, vz = dz / VoxelSize;
        // SafeVoxelDir + 1e-8
        float svx = vx + 1e-8f, svy = vy + 1e-8f, svz = vz + 1e-8f;
        float ivx = 1.0f / svx, ivy = 1.0f / svy, ivz = 1.0f / svz;
        float t0x = (0.0f - ox) * ivx, t1x = (4.0f - ox) * ivx;
        float t0y = (0.0f - oy) * ivy, t1y = (4.0f - oy) * ivy;
        float t0z = (0.0f - oz) * ivz, t1z = (4.0f - oz) * ivz;
        float tminx = Math.Min(t0x, t1x), tmaxx = Math.Max(t0x, t1x);
        float tminy = Math.Min(t0y, t1y), tmaxy = Math.Max(t0y, t1y);
        float tminz = Math.Min(t0z, t1z), tmaxz = Math.Max(t0z, t1z);
        float tenter = Math.Max(Math.Max(tminx, tminy), tminz);
        float texit = Math.Min(Math.Min(tmaxx, tmaxy), tmaxz);
        if (tenter > texit || texit < 0.0f) return null;
        if (tenter < 0.0f) tenter = 0.0f;

        float ex = ox + vx * tenter, ey = oy + vy * tenter, ez = oz + vz * tenter;
        float lx = ex, ly = ey, lz = ez;
        int cx = Clamp((int)Math.Floor(lx), 0, 3);
        int cy = Clamp((int)Math.Floor(ly), 0, 3);
        int cz = Clamp((int)Math.Floor(lz), 0, 3);

        int sx = (vx >= 0.0f) ? 1 : -1;
        int sy = (vy >= 0.0f) ? 1 : -1;
        int sz = (vz >= 0.0f) ? 1 : -1;
        float tdx = Math.Abs(1.0f / vx), tdy = Math.Abs(1.0f / vy), tdz = Math.Abs(1.0f / vz);
        float nbx = cx + ((sx > 0) ? 1 : 0);
        float nby = cy + ((sy > 0) ? 1 : 0);
        float nbz = cz + ((sz > 0) ? 1 : 0);
        float tmx = (nbx - lx) / vx, tmy = (nby - ly) / vy, tmz = (nbz - lz) / vz;
        float tcur = tenter;

        for (int i = 0; i < 64; i++)
        {
            if (TestBit(cx, cy, cz)) return new int[] { cx, cy, cz };
            if (tmx < tmy && tmx < tmz)
            {
                tcur = tmx; cx += sx;
                if (cx < 0 || cx > 3) break;
                tmx += tdx;
            }
            else if (tmy < tmz)
            {
                tcur = tmy; cy += sy;
                if (cy < 0 || cy > 3) break;
                tmy += tdy;
            }
            else
            {
                tcur = tmz; cz += sz;
                if (cz < 0 || cz > 3) break;
                tmz += tdz;
            }
        }
        return null;
    }

    // 暴力遍历：小步长前进，检测第一个占用体素
    static int[] Brute(float ox, float oy, float oz, float dx, float dy, float dz)
    {
        float len = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz);
        float ux = dx / len, uy = dy / len, uz = dz / len;
        for (float s = 0.0f; s < 400.0f; s += 0.02f)
        {
            float px = ox + ux * s, py = oy + uy * s, pz = oz + uz * s;
            int cx = (int)Math.Floor(px), cy = (int)Math.Floor(py), cz = (int)Math.Floor(pz);
            if (cx < 0 || cx > 3 || cy < 0 || cy > 3 || cz < 0 || cz > 3) continue;
            if (TestBit(cx, cy, cz)) return new int[] { cx, cy, cz };
        }
        return null;
    }

    static int Clamp(int v, int lo, int hi) => v < lo ? lo : (v > hi ? hi : v);

    static string Key(int[] a) => a == null ? "null" : $"{a[0]},{a[1]},{a[2]}";

    static void Main()
    {
        int misses = 0, total = 0;
        // 从上方各种角度/位置射向 block，覆盖阶梯边缘
        for (int i = 0; i <= 400; i++)
        {
            // 起点 x 在 [-1,5] 扫描，y 在 4..5.5，z 固定 2；方向向下微斜
            float ox = -1.0f + (i % 100) * 0.06f;
            float oz = 1.0f + (i / 100) * 0.8f;
            float oy = 4.5f;
            float ang = ((i % 7) - 3) * 0.10f; // -0.3..0.3 rad
            float dx = (float)Math.Sin(ang), dy = -1.0f, dz = 0.0f;
            int[] d = DDA(ox, oy, oz, dx, dy, dz);
            int[] b = Brute(ox, oy, oz, dx, dy, dz);
            total++;
            if (Key(d) != Key(b))
            {
                if (misses < 30)
                    Console.WriteLine($"MISMATCH o=({ox:F3},{oy},{oz:F3}) ang={ang:F3} DDA={Key(d)} BRUTE={Key(b)}");
                misses++;
            }
        }
        Console.WriteLine($"扫描 {total} 条射线, 不一致 {misses} 条");
    }
}
