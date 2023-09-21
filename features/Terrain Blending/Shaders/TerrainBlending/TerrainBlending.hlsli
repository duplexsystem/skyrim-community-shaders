

Texture2D<float4> SkinnedMaskTexture : register(t35);
Texture2D<float4> TerrainMaskTexture : register(t36);
Texture2D<float4> DiffuseTexture : register(t37);

float2 ShiftTerrain(float blendFactor, float2 coords, float3 viewDir, float3x3 tbn)
{
    float3 viewDirTS = mul(viewDir, tbn).xyz;
    return viewDirTS.xy * ((1.0 - blendFactor) * 0.02) + coords.xy;
}
