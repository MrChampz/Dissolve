//=======================================================================
// Combines two images.
//=======================================================================

#define gLinearExposure 1.0

Texture2D gHDRBaseMap : register(t0);
Texture2D gHDRBlurMap : register(t1);

SamplerState gSamplerPointWrap        : register(s0);
SamplerState gSamplerPointClamp       : register(s1);
SamplerState gSamplerLinearWrap       : register(s2);
SamplerState gSamplerLinearClamp	  : register(s3);
SamplerState gSamplerAnisotropicWrap  : register(s4);
SamplerState gSamplerAnisotropicClamp : register(s5);

struct PixelIn
{
    float4 PosH     : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

float3 ToneMapReinhard(float3 color)
{
    return color / (1.0f + color);
}

float3 ToneMapACESFilmic(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 LinearToSRGBEst(float3 color)
{
    return pow(abs(color), 1 / 2.2f);
}

float4 PS(PixelIn pin) : SV_Target
{
    float4 base  = gHDRBaseMap.Sample(gSamplerPointClamp, pin.TexCoord);
    float4 bloom = gHDRBlurMap.Sample(gSamplerPointClamp, pin.TexCoord);
    
    float4 hdr = base + bloom;

    // Tone mapping
    float3 sdr = ToneMapACESFilmic(hdr.rgb * gLinearExposure);

    // Gamma correction
    float3 srgb = LinearToSRGBEst(sdr);

    return float4(srgb, hdr.a);
}