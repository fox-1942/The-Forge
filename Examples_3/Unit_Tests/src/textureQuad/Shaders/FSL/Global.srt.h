#pragma once

// limit push_constant decl to particle shader for metal

// for low end iOS devices, do not use Argument buffers
BEGIN_SRT_NO_AB(SrtData)
    BEGIN_SRT_SET(Persistent)
		DECL_TEXTURE(Persistent, Tex2D(float4), Texture)
        DECL_SAMPLER(Persistent, SamplerState, uSampler0)
    END_SRT_SET(Persistent)

	BEGIN_SRT_SET(PerFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(UniformData), UniformData)
    END_SRT_SET(PerFrame)
END_SRT(SrtData)