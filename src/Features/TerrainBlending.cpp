#include "TerrainBlending.h"

void TerrainBlending::DrawSettings()
{
	ImGui::SliderInt("Optimisation Distance", &optimisationDistance, 1, 4096);
}

enum class LightingShaderTechniques
{
	None = 0,
	Envmap = 1,
	Glowmap = 2,
	Parallax = 3,
	Facegen = 4,
	FacegenRGBTint = 5,
	Hair = 6,
	ParallaxOcc = 7,
	MTLand = 8,
	LODLand = 9,
	Snow = 10,  // unused
	MultilayerParallax = 11,
	TreeAnim = 12,
	LODObjects = 13,
	MultiIndexSparkle = 14,
	LODObjectHD = 15,
	Eye = 16,
	Cloud = 17,  // unused
	LODLandNoise = 18,
	MTLandLODBlend = 19,
	Outline = 20,
};
uint32_t GetTechnique(uint32_t descriptor)
{
	return 0x3F & (descriptor >> 24);
}

void TerrainBlending::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	if (!loaded)
		return;

					if (objectDistance <= optimisationDistance) {

	if (shader->shaderType.any(RE::BSShader::Type::Lighting)) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
		auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

		auto reflections = (!REL::Module::IsVR() ?
								   RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget :
								   RE::BSGraphics::RendererShadowState::GetSingleton()->GetVRRuntimeData().cubeMapRenderTarget) == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;

		if (!(reflections || accumulator->GetRuntimeData().activeShadowSceneNode != RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0])) {
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto mainTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

			if (!terrainMaskTexture) {

				D3D11_TEXTURE2D_DESC texDesc{};
				mainTexture.texture->GetDesc(&texDesc);

				terrainMaskTexture = new Texture2D(texDesc);
				skinnedMaskTexture = new Texture2D(texDesc);

				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
				mainTexture.SRV->GetDesc(&srvDesc);
				terrainMaskTexture->CreateSRV(srvDesc);
				skinnedMaskTexture->CreateSRV(srvDesc);

				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				mainTexture.RTV->GetDesc(&rtvDesc);
				terrainMaskTexture->CreateRTV(rtvDesc);
				skinnedMaskTexture->CreateRTV(rtvDesc);
			}

			const auto technique = static_cast<LightingShaderTechniques>(GetTechnique(descriptor));
		
			ID3D11RenderTargetView* views[6];
			ID3D11DepthStencilView* depthStencil;

			if (technique == LightingShaderTechniques::MTLand || technique == LightingShaderTechniques::MTLandLODBlend || technique == LightingShaderTechniques::Snow) {
				context->OMGetRenderTargets(4, views, &depthStencil);
				views[3] = terrainMaskTexture->rtv.get();
				context->OMSetRenderTargets(4, views, depthStencil);
			} else {
				context->OMGetRenderTargets(4, views, &depthStencil);
				views[3] = !staticReference ? skinnedMaskTexture->rtv.get() : nullptr;		
				context->OMSetRenderTargets(4, views, depthStencil);
			}

			ID3D11ShaderResourceView* views2[3] = { skinnedMaskTexture->srv.get(), terrainMaskTexture->srv.get(), mainTexture.SRV};

			context->PSSetShaderResources(35, 3, views2);

			if (technique == LightingShaderTechniques::MTLand || technique == LightingShaderTechniques::MTLandLODBlend || technique == LightingShaderTechniques::Snow) {
					ID3D11BlendState* blendState;
					FLOAT blendFactor[4];
					UINT sampleMask;

					context->OMGetBlendState(&blendState, blendFactor, &sampleMask);

					if (!mappedBlendStates.contains(blendState)) {
						if (!modifiedBlendStates.contains(blendState)) {
							D3D11_BLEND_DESC blendDesc;
							blendState->GetDesc(&blendDesc);
							//blendDesc.AlphaToCoverageEnable = FALSE;
							blendDesc.IndependentBlendEnable = TRUE;

							blendDesc.RenderTarget[0].BlendEnable = TRUE;
							blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
							blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
							blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
							blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
							blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
							blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
							blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

							for (int i = 1; i < 8; i++)
								blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];

							blendDesc.RenderTarget[3].BlendEnable = false;
							blendDesc.RenderTarget[4].BlendEnable = false;
							blendDesc.RenderTarget[5].BlendEnable = false;

							auto device = renderer->GetRuntimeData().forwarder;
							ID3D11BlendState* modifiedBlendState;
							DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &modifiedBlendState));
							mappedBlendStates.insert(modifiedBlendState);
							modifiedBlendStates.insert({ blendState, modifiedBlendState });
						}
						blendState = modifiedBlendStates[blendState];
						context->OMSetBlendState(blendState, blendFactor, sampleMask);
					}

					ID3D11DepthStencilState* depthStencilState;
					UINT stencilRef;
					context->OMGetDepthStencilState(&depthStencilState, &stencilRef);

					if (!mappedDepthStencilStates.contains(depthStencilState)) {
						depthStencilStateBackup = depthStencilState;
						stencilRefBackup = stencilRef;
						if (!modifiedDepthStencilStates.contains(depthStencilState)) {
							D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
							depthStencilState->GetDesc(&depthStencilDesc);
							depthStencilDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
							auto device = renderer->GetRuntimeData().forwarder;
							ID3D11DepthStencilState* modifiedDepthStencilState;
							DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, &modifiedDepthStencilState));
							mappedDepthStencilStates.insert(modifiedDepthStencilState);
							modifiedDepthStencilStates.insert({ depthStencilState, modifiedDepthStencilState });
						}
						depthStencilState = modifiedDepthStencilStates[depthStencilState];
						context->OMSetDepthStencilState(depthStencilState, stencilRef);
					}

					auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
					state->GetRuntimeData().stateUpdateFlags |= RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE;
					state->GetRuntimeData().stateUpdateFlags |= RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND;
				}
			}
		}
	}
}

void TerrainBlending::SetupResources()
{

}

void TerrainBlending::Reset()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	if (skinnedMaskTexture) {
		FLOAT clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(skinnedMaskTexture->rtv.get(), clear);
		context->ClearRenderTargetView(terrainMaskTexture->rtv.get(), clear);
	}
}

void TerrainBlending::Load(json& o_json)
{
	Feature::Load(o_json);
}

void TerrainBlending::Save(json&)
{
}

void TerrainBlending::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* Pass)
{
	staticReference = true;
	if (auto ref = Pass->geometry->GetUserData())
	{
		staticReference = false;
		if (auto base = ref->GetBaseObject())
		{
			if (base->As <RE::TESObjectSTAT>())
			{
				staticReference = true;
			}
		}
	}

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto eyePosition = state->GetRuntimeData().posAdjust.getEye(0);
	auto objectPosition = Pass->geometry->world.translate;
	objectPosition = objectPosition - eyePosition;
	objectDistance = objectPosition.Length();
}
