#include "Common.hlsl"
// Copyright (C) 2019-2022 Alessio Tamburini (aletamburini78@gmail.com) AKA Alenet
// All rights reserved.

// The current maintainer is Llde https://github.com/llde

// Contributor(s):
//  - Timeslip
//  - scanti
//  - ShadeMe
//  - Ethatron
//  - GBR
//  - mcfurston
//  - noonemusteverknow
//  - And all others that helped fixing bugs, betatesting or improving shaders

// The source code is under public license.

// The redistribution and use of the binaries, with or without modification, are permitted and publishing binaries is allowed, but the following conditions MUST be met:
//  - the current license MUST be included in the redistribution of the binaries and it cannot be modified
//  - the distributed binaries MUST report the word "UNOFFICIAL" (or a branch name) in the product description
//  - the distributer MUST state that the official channels cannot be used for unofficial versions.
//  - the source MUST be provided (included or separated)

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER (AUTHOR) 'AS IT IS', WITHOUT WARRANTY OF ANY KIND.
// THIS SOFTWARE IS DISTRIBUTED FOR FREE AND ANY EXPRESS OR IMPLIED MONEY REQUEST IS DISCLAIMED BY THE AUTHOR. YOU CANNOT EARN MONEY (DIRECTLY OR INDIRECTLY) WITH ITS DISTRIBUTION.

// HAVE FUN!

Texture2D<float> OcclusionTexture : register(t1);

#define cKernelSize 12

static const float BlurWeights[cKernelSize] = {
	0.057424882f,
	0.058107773f,
	0.061460144f,
	0.071020611f,
	0.088092873f,
	0.106530916f,
	0.106530916f,
	0.088092873f,
	0.071020611f,
	0.061460144f,
	0.058107773f,
	0.057424882f
};

static const float2 BlurOffsets[cKernelSize] = {
	float2(-6.0f, -6.0f),
	float2(-5.0f, -5.0f),
	float2(-4.0f, -4.0f),
	float2(-3.0f, -3.0f),
	float2(-2.0f, -2.0f),
	float2(-1.0f, -1.0f),
	float2(1.0f, 1.0f),
	float2(2.0f, 2.0f),
	float2(3.0f, 3.0f),
	float2(4.0f, 4.0f),
	float2(5.0f, 5.0f),
	float2(6.0f, 6.0f)
};

float FilterCS(uint3 DTid, float2 TexCoord, float startDepth, uint a_eyeIndex = 0)
{
#if defined(HORIZONTAL)
	float2 OffsetMask = float2(1.0f, 0.0f);
#elif defined(VERTICAL)
	float2 OffsetMask = float2(0.0f, 1.0f);
#else
#	error "Must define an axis!"
#endif

	float WeightSum = 0.114725602f;
	float color1 = OcclusionTexture.SampleLevel(LinearSampler, TexCoord * 2, 0).r * WeightSum;

	float depth1 = InverseProjectUVZ(TexCoord, startDepth, a_eyeIndex).z;

	float depthDrop = depth1 * BlurDropoff;

	[unroll] for (int i = 0; i < cKernelSize; i++)
	{
#if defined(HORIZONTAL)
		float2 uv = TexCoord + (BlurOffsets[i] * OffsetMask * RcpBufferDim) * BlurRadius;
#elif defined(VERTICAL)
		float2 uv = TexCoord + (BlurOffsets[i] * OffsetMask * RcpBufferDim / 2) * BlurRadius;
#endif
		float4 color2 = OcclusionTexture.SampleLevel(LinearSampler, uv * 2, 0).r;
		float depth2 = InverseProjectUV(uv * 2, a_eyeIndex).z;

		// Depth-awareness
		float awareness = saturate(depthDrop - abs(depth1 - depth2));

		color1 += BlurWeights[i] * color2 * awareness;
		WeightSum += BlurWeights[i] * awareness;
	}
	color1 /= WeightSum;
	return color1;
}

[numthreads(32, 32, 1)] void main(uint3 DTid  // [pixels]
								  : SV_DispatchThreadID) {
	float2 TexCoord = (DTid.xy + 0.5) * RcpBufferDim;  // convert to [0,1]

#ifdef VR
	uint eyeIndex = (DTid.x * RcpBufferDim.x >= 0.5) ? 1 : 0;
#else
	uint eyeIndex = 0;
#endif  // VR

	float startDepth = GetDepth(TexCoord * 2);
	if (startDepth >= 1)
		return;

	OcclusionRW[DTid.xy] = FilterCS(DTid, TexCoord, startDepth, eyeIndex);
}