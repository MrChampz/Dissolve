// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
#define NUM_DIR_LIGHTS 3
#endif
#ifndef NUM_POINT_LIGHTS
#define NUM_POINT_LIGHTS 0
#endif
#ifndef NUM_SPOT_LIGHTS
#define NUM_SPOT_LIGHTS 0
#endif

#define gNoiseScale 100.0
#define gEdgeWidth 0.02

// Include utilitary structures and functions.
#include "Utils.hlsli"

// Include structures and functions for lighting.
#include "LightingUtil.hlsli"

Texture2D gDiffuseMap : register(t0);

SamplerState gSamplerPointWrap        : register(s0);
SamplerState gSamplerPointClamp       : register(s1);
SamplerState gSamplerLinearWrap       : register(s2);
SamplerState gSamplerLinearClamp	  : register(s3);
SamplerState gSamplerAnisotropicWrap  : register(s4);
SamplerState gSamplerAnisotropicClamp : register(s5);

cbuffer cbMaterial : register(b1)
{
	float4	 gDiffuseAlbedo;
	float3	 gFresnelR0;
	float	 gRoughness;
	float4x4 gMatTransform;
};

cbuffer cbPass : register(b2)
{
	float4x4 gView;
	float4x4 gProj;
	float4x4 gViewProj;
	float4x4 gInvView;
	float4x4 gInvProj;
	float4x4 gInvViewProj;
	float3   gEyePosW;
	float    cbPerObjectPad1;
	float2   gRenderTargetSize;
	float2   gInvRenderTargetSize;
	float    gNearZ;
	float    gFarz;
	float    gTotalTime;
	float    gDeltaTime;

	float4 gAmbientLight;

	// Allow application to change fog parameters once per frame.
	// For example, we may only use fog for certain times of day.
	float4 gFogColor;
	float  gFogStart;
	float  gFogRange;
	float2 cbPerObjectPad2;

	// Indices [0, NUM_DIR_LIGHTS) are directional lights;
	// indices [NUM_DIR_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHTS) are point lights;
	// indices [NUM_DIR_LIGHTS+NUM_POINT_LIGHTS, NUM_DIR_LIGHTS+NUM_POINT_LIGHT+NUM_SPOT_LIGHTS)
	// are spot lights for a maximum of MaxLights per object.
	Light gLights[MaxLights];
};

struct PixelIn
{
	float4 PosH	    : SV_POSITION;
	float3 PosW     : POSITION;
	float3 NormalW  : NORMAL;
	float2 TexCoord : TEXCOORD;
};

struct PixelOut
{
    float4 Color  : SV_Target0;
    float4 Bright : SV_Target1;
};

PixelOut PS(PixelIn pin)
{
    PixelOut pout = (PixelOut)0;

    // Generate the noise.
    float noise = SimpleNoise(pin.TexCoord, gNoiseScale);

    // Calculate the sine time.
    float sine = 0.5f * sin(gTotalTime) + 0.5f;

    // Clip the fragment if it isn't in the range.
    clip(noise - sine);

    // Generate the emissive edge.
    float edge = gEdgeWidth + sine;
    edge = step(noise, edge);
    
    float4 edgeColor = float4(0.0f, 1.0f, 0.129411765f, 1.0f);
    edgeColor = edgeColor * edge;

    //
    // Lighting calculations.
    //

    float4 diffuseAlbedo = gDiffuseAlbedo;

    // Uncomment to use texture slot 0.
    //diffuseAlbedo *= gDiffuseMap.Sample(gSamplerAnisotropicWrap, pin.TexCoord);

	// Interpolating normal can unnormalize it, so renormalize it.
	pin.NormalW = normalize(pin.NormalW);

	// Vector from point being lit to eye.
	float3 toEyeW = gEyePosW - pin.PosW;
	float  distToEye = length(toEyeW);
	toEyeW /= distToEye; // normalize

    // Emissive lighting.
    float4 emissive = edgeColor;

	// Indirect lighting.
	float4 ambient = gAmbientLight * diffuseAlbedo;

	// Direct lighting.
	const float shininess = 1.0f - gRoughness;
	Material mat = { diffuseAlbedo, gFresnelR0, shininess };
	float3 shadowFactor = 1.0f;
	float4 directLight = ComputeLighting(gLights, mat, pin.PosW, pin.NormalW, toEyeW, shadowFactor);

    pout.Color = ambient + directLight + emissive;

	// Common convention to take alpha from diffuse material.
    pout.Color.a = diffuseAlbedo.a;

    // Output bright colors to the bloom render target.
    pout.Bright = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float brightness = (pout.Color.r * 0.2126f) + (pout.Color.g * 0.7152f) + (pout.Color.b * 0.0722f);
    if (brightness > 1.0f)
    {
        pout.Bright = float4(pout.Color.rgb, 1.0f);
    }

    return pout;
}