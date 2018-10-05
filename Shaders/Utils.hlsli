

float NoiseRandomValue(float2 uv)
{
    return frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453);
}

float NoiseInterpolate(float a, float b, float t)
{
    return (1.0 - t) * a + (t * b);
}

float Noise(float2 uv)
{
    float2 i = floor(uv);
    float2 f = frac(uv);
    f = f * f * (3.0 - 2.0 * f);

    uv = abs(frac(uv) - 0.5);
    float2 c0 = i + float2(0.0, 0.0);
    float2 c1 = i + float2(1.0, 0.0);
    float2 c2 = i + float2(0.0, 1.0);
    float2 c3 = i + float2(1.0, 1.0);
    float  r0 = NoiseRandomValue(c0);
    float  r1 = NoiseRandomValue(c1);
    float  r2 = NoiseRandomValue(c2);
    float  r3 = NoiseRandomValue(c3);

    float bottom = NoiseInterpolate(r0, r1, f.x);
    float top    = NoiseInterpolate(r2, r3, f.x);
    float t      = NoiseInterpolate(bottom, top, f.y);

    return t;
}

float SimpleNoise(float2 uv, float scale)
{
    float noise = 0.0f;
    for (int i = 0; i < 3; i++)
    {
        float freq = pow(2.0, float(i));
        float amp = pow(0.5, float(3 - i));
        noise += Noise(float2(uv.x * scale / freq, uv.y * scale / freq)) * amp;
    }

    return noise;
}