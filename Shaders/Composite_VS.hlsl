//=======================================================================
// Combines two images.
//=======================================================================

struct VertexOut
{
    float4 PosH     : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

VertexOut VS(uint vId : SV_VertexID)
{
    VertexOut vout;

    // We use the 'big triangle' optimization so you only Draw 3 verticies instead of 4.
    vout.TexCoord = float2((vId << 1) & 2, vId & 2);

    // Map [0, 1]^2 to NDC space.
    vout.PosH = float4(2.0f * vout.TexCoord.x - 1.0f, 1.0f - 2.0f * vout.TexCoord.y, 0.0f, 1.0f);

    return vout;
}