// Lens warp (D3D9, shader model 3): maps each output pixel to a view ray through the chosen lens,
// then samples the engine's wide rectilinear render along that ray. Math follows
// shaunlebron/flex-fov, as in gowfov's lens.hlsl.
// Ray convention: x right, y up, z forward. Screen x spans [-1, 1], y spans [-1/aspect, 1/aspect].

float4 params : register(c0);   // halfFov (radians), tanHalfRender (true horizontal), aspect, -
float4 params2 : register(c1);  // mode (2 Panini, 3 stereographic, 4 fisheye), circle (radians; 0 none), -, -
float4 pixelOffset : register(c2);  // D3D9 half-pixel offset in clip space (-1/w, 1/h)
float4 hole : register(c3);         // source uv rect (x0, y0, x1, y1) read from PreUI instead; empty = none

sampler2D Scene : register(s0);
sampler2D PreUI : register(s1);     // Pip-Boy frames: the frame before the UI (no cursor)

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    VSOut o;
    o.pos = float4(pos.xy + pixelOffset.xy, 0, 1);
    o.uv = uv;
    return o;
}

float3 RayPanini(float2 s, float halfFov)
{
    // Panini with d = 1: x = S sin(lon), y = S tan(lat), S = 2 / (1 + cos(lon)).
    float edge = 2 * sin(halfFov) / (1 + cos(halfFov));
    float2 p = s * edge;
    float k = p.x * p.x / 4;
    float clon = (1 - k) / (1 + k);
    float S = 1 + k;
    float lon = atan2(p.x, S * clon);
    float lat = atan2(p.y, S);
    return float3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
}

float3 RayStereographic(float2 s, float halfFov)
{
    // r = tan(theta / 2)
    float2 p = s * tan(halfFov * 0.5);
    float r = length(p);
    if (r < 1e-6) return float3(0, 0, 1);
    float theta = 2 * atan(r);
    return float3(p / r * sin(theta), cos(theta));
}

float3 RayFisheye(float2 s, float halfFov)
{
    // equidistant: theta = r (corners past 90 degrees point backwards and are left black)
    float2 p = s * halfFov;
    float r = length(p);
    if (r < 1e-6) return float3(0, 0, 1);
    return float3(p / r * sin(r), cos(r));
}

float4 PSMain(float2 uv : TEXCOORD0) : COLOR
{
    float halfFov = params.x, tanHalfRender = params.y, aspect = params.z;
    float mode = params2.x;
    float2 s = float2(uv.x * 2 - 1, (1 - uv.y * 2) / aspect);
    float3 ray;
    if (mode > 3.5) ray = RayFisheye(s, halfFov);
    else if (mode > 2.5) ray = RayStereographic(s, halfFov);
    else ray = RayPanini(s, halfFov);

    if (ray.z <= 1e-4) return float4(0, 0, 0, 1);
    // Fisheye full circle: nothing beyond the circle's angle (the render's corners would show past it).
    if (params2.y > 0 && length(s) * halfFov > params2.y) return float4(0, 0, 0, 1);
    float2 r = ray.xy / ray.z / float2(tanHalfRender, tanHalfRender / aspect);
    if (abs(r.x) > 1.001 || abs(r.y) > 1.001) return float4(0, 0, 0, 1);
    float2 src = float2(0.5 + 0.5 * r.x, 0.5 - 0.5 * r.y);
    // The cursor's area comes from the frame without the UI: the cursor is drawn unwarped later.
    if (src.x >= hole.x && src.y >= hole.y && src.x < hole.z && src.y < hole.w) return tex2Dlod(PreUI, float4(src, 0, 0));
    return tex2Dlod(Scene, float4(src, 0, 0));
}

// The cursor, unwarped: the pixels of the frame with the UI that differ from the frame before
// the UI, copied 1:1.
float4 PSCursor(float2 uv : TEXCOORD0) : COLOR
{
    float4 a = tex2Dlod(Scene, float4(uv, 0, 0));
    float4 b = tex2Dlod(PreUI, float4(uv, 0, 0));
    float3 d = abs(a.rgb - b.rgb);
    clip(max(d.r, max(d.g, d.b)) - 0.03);
    return float4(a.rgb, 1);
}
