// Supersampling resolve (D3D9, shader model 3): exact area average of the high-resolution frame
// into the native back buffer, in linear light so bright/dark edges keep their weight (gowfov's
// downscale.hlsl). Supports up to 8 source pixels per destination pixel per axis (factor 64).

float4 scaleSize : register(c0);    // source pixels per destination pixel (x, y), source size (w, h)
float4 pixelOffset : register(c2);  // D3D9 half-pixel offset in clip space (-1/w, 1/h)

sampler2D Src : register(s0);

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOut o;
    o.pos = float4(pos.xy + pixelOffset.xy, 0, 1);
    o.uv = uv;
    return o;
}

float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }
float3 ToGamma(float3 c) { return pow(max(c, 0), 1 / 2.2); }

float4 PSMain(float2 vpos : VPOS) : COLOR
{
    float2 scale = scaleSize.xy, size = scaleSize.zw;
    // Destination pixel covers source span [p0, p1).
    float2 p0 = vpos * scale;
    float2 p1 = p0 + scale;
    float2 i0 = floor(p0);
    float3 acc = 0;
    float wsum = 0;
    // A span of up to 8 source pixels touches at most 9 of them.
    [loop] for (int y = 0; y < 9; ++y) {
        float ty = i0.y + y;
        float wy = max(0, min(p1.y, ty + 1) - max(p0.y, ty));
        if (wy <= 0) break;
        [loop] for (int x = 0; x < 9; ++x) {
            float tx = i0.x + x;
            float wx = max(0, min(p1.x, tx + 1) - max(p0.x, tx));
            if (wx <= 0) break;
            float w = wx * wy;
            float2 t = min(float2(tx, ty), size - 1) + 0.5;
            acc += ToLinear(tex2Dlod(Src, float4(t / size, 0, 0)).rgb) * w;
            wsum += w;
        }
    }
    return float4(ToGamma(acc / wsum), 1);
}

// Integer scales (2-8 source pixels per destination pixel per axis): the sampler decodes sRGB and
// filters bilinearly (D3DSAMP_SRGBTEXTURE, LINEAR), so one read between texels returns the
// linear-light average of up to 2x2 of them: a quarter of PSMain's reads, the same average.
float3 ToSrgb(float3 c)
{
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

float4 PSFast(float2 vpos : VPOS) : COLOR
{
    float s = scaleSize.x;  // the same integer on both axes
    float2 size = scaleSize.zw;
    float2 p0 = floor(vpos) * s;
    float3 acc = 0;
    float wsum = 0;
    [loop] for (float y = 0; y < s; y += 2) {
        float hy = min(2, s - y);  // rows in this read (2, or 1 at the end of an odd span)
        [loop] for (float x = 0; x < s; x += 2) {
            float hx = min(2, s - x);
            float2 t = p0 + float2(x, y) + float2(hx, hy) * 0.5;  // between 2 texels, or one's centre
            float w = hx * hy;
            acc += tex2Dlod(Src, float4(t / size, 0, 0)).rgb * w;
            wsum += w;
        }
    }
    return float4(ToSrgb(acc / wsum), 1);
}
