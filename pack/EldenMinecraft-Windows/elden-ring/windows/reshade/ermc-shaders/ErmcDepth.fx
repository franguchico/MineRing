// MIT. Raw depth only: projection reconstruction belongs to the bridge.
uniform bool ErmcDepthReady < source = "bufready_depth"; >;
texture ErmcHostDepth : DEPTH;
sampler ErmcDepthSampler { Texture = ErmcHostDepth; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
texture ErmcRawDepth { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = R32F; };
// Exposing the render target as a sampler preserves its public texture binding.
sampler ErmcRawSampler { Texture = ErmcRawDepth; };
void VS(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD) {
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float PS(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    return ErmcDepthReady ? tex2D(ErmcDepthSampler, uv).r : 0.0;
}
technique ErmcDepthExport {
    pass { VertexShader = VS; PixelShader = PS; RenderTarget = ErmcRawDepth; }
}
