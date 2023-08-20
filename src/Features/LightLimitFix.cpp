#include "LightLimitFix.h"

#include "State.h"
#include "Util.h"
#include <detours/Detours.h>

constexpr std::uint32_t CLUSTER_SIZE_X = 16;
constexpr std::uint32_t CLUSTER_SIZE_Y = 8;
constexpr std::uint32_t CLUSTER_SIZE_Z = 24;
constexpr std::uint32_t CLUSTER_MAX_LIGHTS = 128;

constexpr std::uint32_t CLUSTER_COUNT = CLUSTER_SIZE_X * CLUSTER_SIZE_Y * CLUSTER_SIZE_Z;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LightLimitFix::Settings,
	EnableContactShadows,
	ExtendFirstPersonShadows,
	EnableParticleLights,
	EnableParticleLightsCulling,
	EnableParticleLightsFade,
	EnableParticleLightsDetection,
	ParticleLightsBrightness,
	ParticleLightsRadius,
	ParticleLightsRadiusBillboards,
	EnableParticleLightsOptimization,
	ParticleLightsOptimisationClusterRadius)

void LightLimitFix::DrawSettings()
{
	if (ImGui::TreeNodeEx("Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Contact Shadows", &settings.EnableContactShadows);
		ImGui::Checkbox("Extend First-Person Shadows", &settings.ExtendFirstPersonShadows);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Particle Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Particle Lights", &settings.EnableParticleLights);
		ImGui::Checkbox("Enable Culling", &settings.EnableParticleLightsCulling);
		ImGui::Checkbox("Enable Fade", &settings.EnableParticleLightsFade);
		ImGui::Checkbox("Enable Detection", &settings.EnableParticleLightsDetection);

		ImGui::SliderFloat("Brightness", &settings.ParticleLightsBrightness, 0.0, 10.0);
		ImGui::SliderFloat("Radius", &settings.ParticleLightsRadius, 0.0, 1000.0);
		ImGui::SliderFloat("Billboard Radius", &settings.ParticleLightsRadiusBillboards, 0.0, 1.0);

		ImGui::Checkbox("Enable Optimization", &settings.EnableParticleLightsOptimization);
		ImGui::SliderInt("Optimisation Cluster Radius", (int*)&settings.ParticleLightsOptimisationClusterRadius, 1, 128);

		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Clustered Light Count : {}", lightCount).c_str());
		ImGui::Text(std::format("Particle Lights Detection Count : {}", particleLightsDetectionHits).c_str());

		ImGui::TreePop();
	}
}

void LightLimitFix::SetupResources()
{
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(LightData);
		sbDesc.ByteWidth = sizeof(LightData) * 1024;
		strictLights = eastl::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 15;
		strictLights->CreateSRV(srvDesc);
	}

	clusterBuildingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", {}, "cs_5_0");
	clusterCullingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", {}, "cs_5_0");

	perFrameLightCulling = new ConstantBuffer(ConstantBufferDesc<PerFrameLightCulling>());

	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DEFAULT;
	sbDesc.CPUAccessFlags = 0;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.Flags = 0;

	std::uint32_t numElements = CLUSTER_COUNT;

	sbDesc.StructureByteStride = sizeof(ClusterAABB);
	sbDesc.ByteWidth = sizeof(ClusterAABB) * numElements;
	clusters = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	clusters->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	clusters->CreateUAV(uavDesc);

	numElements = 1;
	sbDesc.StructureByteStride = sizeof(uint32_t);
	sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
	lightCounter = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	lightCounter->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	lightCounter->CreateUAV(uavDesc);

	numElements = CLUSTER_COUNT * CLUSTER_MAX_LIGHTS;
	sbDesc.StructureByteStride = sizeof(uint32_t);
	sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
	lightList = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	lightList->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	lightList->CreateUAV(uavDesc);

	numElements = CLUSTER_COUNT;
	sbDesc.StructureByteStride = sizeof(LightGrid);
	sbDesc.ByteWidth = sizeof(LightGrid) * numElements;
	lightGrid = eastl::make_unique<Buffer>(sbDesc);
	srvDesc.Buffer.NumElements = numElements;
	lightGrid->CreateSRV(srvDesc);
	uavDesc.Buffer.NumElements = numElements;
	lightGrid->CreateUAV(uavDesc);
}

void LightLimitFix::Reset()
{
	rendered = false;
	for (auto& particleLight : particleLights) {
		if (const auto particleSystem = netimmerse_cast<RE::NiParticleSystem*>(particleLight.first)) {
			if (auto particleData = particleSystem->particleData.get()) {
				particleData->DecRefCount();
			}
		}
		particleLight.first->DecRefCount();
	}
	particleLights.clear();
	std::swap(particleLights, queuedParticleLights);
}

void LightLimitFix::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];
	Feature::Load(o_json);
}

void LightLimitFix::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void LightLimitFix::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass*)
{
	strictLightsCount = 0;
	strictLightsData.clear();
}

void LightLimitFix::BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* Pass, uint32_t NumLights, uint32_t ShadowLightCount)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	NumLights = (uint8_t)std::min(7, Pass->numLights - 1);

	for (uint8_t i = 0; i < NumLights; i++) {
		auto bsLight = Pass->sceneLights[i + 1];
		auto niLight = bsLight->light.get();

		LightData light{};

		float dimmer = niLight->GetLightRuntimeData().fade * bsLight->lodDimmer;
		dimmer = 1;
		light.color.x = niLight->GetLightRuntimeData().diffuse.red * dimmer;
		light.color.y = niLight->GetLightRuntimeData().diffuse.green * dimmer;
		light.color.z = niLight->GetLightRuntimeData().diffuse.blue * dimmer;
		light.radius = niLight->GetLightRuntimeData().radius.x;

		auto worldPos = niLight->world.translate - state->posAdjust.getEye();
		light.positionWS.x = worldPos.x;
		light.positionWS.y = worldPos.y;
		light.positionWS.z = worldPos.z;
		light.positionVS = DirectX::SimpleMath::Vector3::Transform(light.positionWS, state->cameraData.getEye().viewMat);

		//light.shadow = false;
		//light.mask = -1;

		if (i < ShadowLightCount) {
			//light.shadow = true;
			//light.mask = (float)static_cast<RE::BSShadowLight*>(bsLight)->maskSelect;
		}

		light.shadowMode = 0;

		strictLightsData.push_back(light);
		strictLightsCount++;
	}
}

float LightLimitFix::CalculateLuminance(CachedParticleLight& light, RE::NiPoint3& point)
{
	// See BSLight::CalculateLuminance_14131D3D0
	// Performs lighting on the CPU which is identical to GPU code

	auto lightDirection = light.position - point;
	float lightDist = lightDirection.Length();
	float intensityFactor = std::clamp(lightDist / light.radius, 0.0f, 1.0f);
	float intensityMultiplier = 1 - intensityFactor * intensityFactor;

	float luminance = (light.color.red + light.color.blue + light.color.green) / 3.0f;
	return luminance * intensityMultiplier;
}

void LightLimitFix::AddParticleLightLuminance(RE::NiPoint3& targetPosition, int& numHits, float& lightLevel)
{
	std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
	particleLightsDetectionHits = 0;
	if (settings.EnableParticleLightsDetection) {
		for (auto& light : cachedParticleLights) {
			auto luminance = CalculateLuminance(light, targetPosition);
			lightLevel += luminance;
			if (luminance > 0.0)
				particleLightsDetectionHits++;
		}
	}
	numHits += particleLightsDetectionHits;
}

void LightLimitFix::BSLightingShader_SetupGeometry_After(RE::BSRenderPass*)
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(strictLights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(LightData) * strictLightsCount;
	memcpy_s(mapped.pData, bytes, strictLightsData.data(), bytes);
	context->Unmap(strictLights->resource.get(), 0);
}

void LightLimitFix::BSEffectShader_SetupGeometry_Before(RE::BSRenderPass* a_pass)
{
	if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
		if (auto material = static_cast<RE::BSEffectShaderMaterial*>(shaderProperty->material)) {
			if (material->sourceTexturePath.size() > 1) {
				std::string textureName = material->sourceTexturePath.c_str();
				auto lastSeparatorPos = textureName.find_last_of("\\/");
				if (lastSeparatorPos != std::string::npos) {
					std::string filename = textureName.substr(lastSeparatorPos + 1);
#pragma warning(push)
#pragma warning(disable: 4244)
					std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
#pragma warning(pop)
					std::set<std::string> cullingList{
						"fxglowenb.dds", "glowsoft01_enbl.dds", "black.dds", "enblightglow.dds"
					};
					bool shouldCull = cullingList.contains(filename);
					if (shouldCull) {
						logger::info("cull");
					}
				}
			}
		}
	}
}

void LightLimitFix::Bind()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

	if (RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS || accumulator->activeShadowSceneNode != RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
		PerPass perPassData{};
		perPassData.EnableGlobalLights = false;

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &perPassData, bytes);
		context->Unmap(perPass->resource.get(), 0);
	} else {
		if (!rendered) {
			UpdateLights();
			rendered = true;
		}

		{
			ID3D11ShaderResourceView* views[4]{};
			views[0] = lights->srv.get();
			views[1] = lightList->srv.get();
			views[2] = lightGrid->srv.get();
			views[3] = RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
			context->PSSetShaderResources(17, ARRAYSIZE(views), views);
		}

		{
			PerPass perPassData{};
			perPassData.CameraData.x = accumulator->kCamera->viewFrustum.fFar;
			perPassData.CameraData.y = accumulator->kCamera->viewFrustum.fNear;
			perPassData.CameraData.z = accumulator->kCamera->viewFrustum.fFar - accumulator->kCamera->viewFrustum.fNear;
			perPassData.CameraData.w = accumulator->kCamera->viewFrustum.fFar * accumulator->kCamera->viewFrustum.fNear;

			auto viewport = RE::BSGraphics::State::GetSingleton();
			float resolutionX = viewport->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
			float resolutionY = viewport->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

			perPassData.LightsNear = lightsNear;
			perPassData.LightsFar = lightsFar;

			perPassData.BufferDim = { resolutionX, resolutionY };
			perPassData.FrameCount = viewport->uiFrameCount;
			perPassData.EnableGlobalLights = true;

			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(PerPass);
			memcpy_s(mapped.pData, bytes, &perPassData, bytes);
			context->Unmap(perPass->resource.get(), 0);
		}
	}

	{
		ID3D11ShaderResourceView* views[2]{};
		views[0] = perPass->srv.get();
		views[1] = strictLights ? strictLights->srv.get() : nullptr;
		context->PSSetShaderResources(32, ARRAYSIZE(views), views);
	}
}

bool LightLimitFix::IsValidLight(RE::BSLight* a_light)
{
	using func_t = decltype(&LightLimitFix::IsValidLight);
	static REL::Relocation<func_t> func{ REL::RelocationID(98902, 105550) };
	return a_light && a_light->unk05C != 255 && !a_light->light->GetFlags().any(RE::NiAVObject::Flag::kHidden);
}

bool LightLimitFix::IsGlobalLight(RE::BSLight* a_light)
{
	return !(a_light->portalStrict || !a_light->portalGraph || skyrim_cast<RE::BSShadowLight*>(a_light));
}

struct VertexColor
{
	std::uint8_t data[4];
};

bool LightLimitFix::CheckParticleLights(RE::BSRenderPass* a_pass, uint32_t a_technique)
{
	// See https://www.nexusmods.com/skyrimspecialedition/articles/1391
	if (settings.EnableParticleLights) {
		//	{
		//			if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
		//				if (auto material = static_cast<RE::BSEffectShaderMaterial*>(shaderProperty->material)) {
		//					std::string textureName = material->sourceTexturePath.c_str();
		//					auto lastSeparatorPos = textureName.find_last_of("\\/");
		//					if (lastSeparatorPos != std::string::npos) {
		//						std::string filename = textureName.substr(lastSeparatorPos + 1);
		//#pragma warning(push)
		//#pragma warning(disable: 4244)
		//						std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
		//#pragma warning(pop)
		//						std::set<std::string> cullingList{
		//							"fxglowenb.dds", "glowsoft01_enbl.dds", "black.dds", "enblightglow.dds"
		//						};
		//						bool shouldCull = cullingList.contains(filename);
		//						if (shouldCull) {
		//							logger::info("tech {:X}", a_technique);
		//						}
		//					}
		//				}
		//			}
		//		}
		//
		if (a_technique == 0x4004146F || a_technique == 0x4004046F || a_technique == 0x4000046F) {
			if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
				if (auto material = static_cast<RE::BSEffectShaderMaterial*>(shaderProperty->material)) {
					if (!material->sourceTexturePath.empty() && material->sourceTexturePath.size() > 1) {
						std::string textureName = material->sourceTexturePath.c_str();
						auto lastSeparatorPos = textureName.find_last_of("\\/");
						if (lastSeparatorPos != std::string::npos) {
							std::string filename = textureName.substr(lastSeparatorPos + 1);
#pragma warning(push)
#pragma warning(disable: 4244)
							std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
#pragma warning(pop)

							std::set<std::string> lightsList{
								"candleglow01.dds", "glowslightflash.dds", "fxglowenb.dds", "glowsoft01_enbl.dds", "black.dds", "enblightglow.dds"
							};

							if (lightsList.contains(filename)) {
								a_pass->geometry->IncRefCount();
								if (const auto particleSystem = netimmerse_cast<RE::NiParticleSystem*>(a_pass->geometry)) {
									if (auto particleData = particleSystem->particleData.get()) {
										particleData->IncRefCount();
									}
								}

								RE::NiColor color;
								color.red = material->baseColor.red * material->baseColorScale * material->baseColor.alpha * shaderProperty->alpha * settings.ParticleLightsBrightness;
								color.green = material->baseColor.green * material->baseColorScale * material->baseColor.alpha * shaderProperty->alpha * settings.ParticleLightsBrightness;
								color.blue = material->baseColor.blue * material->baseColorScale * material->baseColor.alpha * shaderProperty->alpha * settings.ParticleLightsBrightness;

								if (auto emittance = shaderProperty->unk88) {
									color.red *= emittance->red;
									color.green *= emittance->green;
									color.blue *= emittance->blue;
								}

								if (auto rendererData = a_pass->geometry->GetGeometryRuntimeData().rendererData) {
									if (rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS)) {
										if (auto triShape = a_pass->geometry->AsTriShape()) {
											uint32_t vertexSize = rendererData->vertexDesc.GetSize();
											uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR);
											RE::NiColorA vertexColor{};
											for (int v = 0; v < triShape->GetTrishapeRuntimeData().vertexCount; v++) {
												if (VertexColor* vertex = reinterpret_cast<VertexColor*>(&rendererData->rawVertexData[vertexSize * v + offset])) {
													RE::NiColorA niColor{ (float)vertex->data[0] / 255.0f, (float)vertex->data[1] / 255.0f, (float)vertex->data[2] / 255.0f, (float)vertex->data[3] / 255.0f };
													if (niColor.alpha > vertexColor.alpha)
														vertexColor = niColor;
												}
											}
											color.red *= vertexColor.red * vertexColor.alpha;
											color.green *= vertexColor.green * vertexColor.alpha;
											color.blue *= vertexColor.blue * vertexColor.alpha;
										}
									}
								}

								queuedParticleLights.insert({ a_pass->geometry, color });

								std::set<std::string> cullingList{
									"fxglowenb.dds", "glowsoft01_enbl.dds", "black.dds", "enblightglow.dds"
								};
								bool shouldCull = cullingList.contains(filename);
								return settings.EnableParticleLightsCulling && shouldCull;
							}
						}
					}
				}
			}
		}
	}
	return false;
}

void LightLimitFix::BSEffectShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (auto shaderProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty)) {
		if (auto material = static_cast<RE::BSEffectShaderMaterial*>(shaderProperty->material)) {
			logger::info("{}", material->greyscaleTexturePath.c_str());
		}
	}
}

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void LightLimitFix::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Lighting:
		{
			Bind();
		}
		break;
	case RE::BSShader::Type::Grass:
		{
			const auto technique = descriptor & 0b1111;
			if (technique != static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
				Bind();
			}
		}
		break;
	}
}

void LightLimitFix::UpdateLights()
{
	std::uint32_t currentLightCount = 0;  // Max number of lights is 4294967295

	auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	RE::NiLight* refLight = nullptr;
	RE::NiLight* magicLight = nullptr;
	RE::BSLight* firstPersonLight = nullptr;
	RE::BSLight* thirdPersonLight = nullptr;

	if (settings.ExtendFirstPersonShadows) {
		if (auto playerCamera = RE::PlayerCamera::GetSingleton()) {
			if (playerCamera->IsInFirstPerson()) {
				if (auto player = RE::PlayerCharacter::GetSingleton()) {
					firstPersonLight = player->GetPlayerRuntimeData().firstPersonLight.get();
					thirdPersonLight = player->GetPlayerRuntimeData().thirdPersonLight.get();
					if (auto light = player->extraList.GetByType<RE::ExtraLight>()) {
						refLight = light->lightData->light.get();
					}
					if (auto light = player->extraList.GetByType<RE::ExtraMagicLight>()) {
						magicLight = light->lightData->light.get();
					}
				}
			}
		}
	}

	eastl::vector<LightData> lightsData{};

	for (auto& e : shadowSceneNode->GetRuntimeData().activePointLights) {
		if (auto bsLight = e.get()) {
			if (auto niLight = bsLight->light.get()) {
				if (IsValidLight(bsLight) && IsGlobalLight(bsLight)) {
					LightData light{};

					float dimmer = niLight->GetLightRuntimeData().fade * bsLight->lodDimmer;
					light.color.x = niLight->GetLightRuntimeData().diffuse.red * dimmer;
					light.color.y = niLight->GetLightRuntimeData().diffuse.green * dimmer;
					light.color.z = niLight->GetLightRuntimeData().diffuse.blue * dimmer;
					light.radius = niLight->GetLightRuntimeData().radius.x;

					auto worldPos = niLight->world.translate - state->posAdjust.getEye();
					light.positionWS.x = worldPos.x;
					light.positionWS.y = worldPos.y;
					light.positionWS.z = worldPos.z;
					light.positionVS = DirectX::SimpleMath::Vector3::Transform(light.positionWS, state->cameraData.getEye().viewMat);

					light.shadowMode = bsLight == firstPersonLight || bsLight == thirdPersonLight || niLight == refLight || niLight == magicLight;

					lightsData.push_back(light);
					currentLightCount++;
				}
			}
		}
	}

	static float& lightFadeStart = (*(float*)RELOCATION_ID(527668, 414582).address());
	static float& lightFadeEnd = (*(float*)RELOCATION_ID(527669, 414583).address());

	{
		std::lock_guard<std::shared_mutex> lk{ cachedParticleLightsMutex };
		cachedParticleLights.clear();

		for (const auto& particleLight : particleLights) {
			if (const auto particleSystem = netimmerse_cast<RE::NiParticleSystem*>(particleLight.first)) {
				auto particleData = particleSystem->particleData.get();
				LightData light{};
				uint32_t clusteredLights = 0;
				auto numVertices = particleData->GetActiveVertexCount();
				for (std::uint32_t p = 0; p < numVertices; p++) {
					light.color.x += particleLight.second.red * particleData->color[p].red * particleData->color[p].alpha;
					light.color.y += particleLight.second.green * particleData->color[p].green * particleData->color[p].alpha;
					light.color.z += particleLight.second.blue * particleData->color[p].blue * particleData->color[p].alpha;

					float radius = particleData->sizes[p] * settings.ParticleLightsRadius;

					RE::NiPoint3 positionWS = particleData->positions[p] + (particleSystem->isWorldspace ? RE::NiPoint3{} : (particleLight.first->worldBound.center)) - state->posAdjust.getEye();

					if (clusteredLights) {
						float radiusDiff = abs((light.radius / (float)clusteredLights) - radius);
						radiusDiff *= radiusDiff;

						auto averagePosition = light.positionWS / (float)clusteredLights;
						float positionDiff = positionWS.GetDistance({ averagePosition.x, averagePosition.y, averagePosition.z });

						if ((radiusDiff + positionDiff) > settings.ParticleLightsOptimisationClusterRadius || !settings.EnableParticleLightsOptimization) {
							light.radius /= (float)clusteredLights;
							light.positionWS /= (float)clusteredLights;

							float distance = (light.positionWS.x * light.positionWS.x) + (light.positionWS.y * light.positionWS.y) + (light.positionWS.z * light.positionWS.z) - (light.radius * light.radius);

							float dimmer = 0.0f;
							if (!settings.EnableParticleLightsFade || distance < lightFadeStart || lightFadeEnd == 0.0f) {
								dimmer = 1.0f;
							} else if (distance <= lightFadeEnd) {
								dimmer = 1.0f - ((distance - lightFadeStart) / (lightFadeEnd - lightFadeStart));
							} else {
								dimmer = 0.0f;
							}

							if (dimmer != 0) {
								if (light.color.x > 0 && light.color.y > 0 && light.color.z > 0 && light.radius > 0) {
									CachedParticleLight cachedParticleLight{};
									cachedParticleLight.color = { light.color.x, light.color.y, light.color.z };
									cachedParticleLight.position = { light.positionWS.x + state->posAdjust.getEye().x, light.positionWS.y + state->posAdjust.getEye().y, light.positionWS.z + state->posAdjust.getEye().z };
									cachedParticleLight.radius = light.radius;
									cachedParticleLights.push_back(cachedParticleLight);

									light.color.x *= dimmer * 2.0f;
									light.color.y *= dimmer * 2.0f;
									light.color.z *= dimmer * 2.0f;

									light.positionVS = DirectX::SimpleMath::Vector3::Transform(light.positionWS, state->cameraData.getEye().viewMat);

									light.shadowMode = 2 * settings.EnableContactShadows;

									lightsData.push_back(light);
									currentLightCount++;
								}
							}

							clusteredLights = 0;
							light.color = { 0, 0, 0 };
							light.radius = 0;
							light.positionWS = { 0, 0, 0 };
						}
					}

					clusteredLights++;
					light.radius += radius;
					light.positionWS.x += positionWS.x;
					light.positionWS.y += positionWS.y;
					light.positionWS.z += positionWS.z;
				}

				if (clusteredLights) {
					light.radius /= (float)clusteredLights;
					light.positionWS /= (float)clusteredLights;

					float distance = (light.positionWS.x * light.positionWS.x) + (light.positionWS.y * light.positionWS.y) + (light.positionWS.z * light.positionWS.z) - (light.radius * light.radius);

					float dimmer = 0.0f;
					if (!settings.EnableParticleLightsFade || distance < lightFadeStart || lightFadeEnd == 0.0f) {
						dimmer = 1.0f;
					} else if (distance <= lightFadeEnd) {
						dimmer = 1.0f - ((distance - lightFadeStart) / (lightFadeEnd - lightFadeStart));
					} else {
						dimmer = 0.0f;
					}

					if (dimmer != 0) {
						if (light.color.x > 0 && light.color.y > 0 && light.color.z > 0 && light.radius > 0) {
							CachedParticleLight cachedParticleLight{};
							cachedParticleLight.color = { light.color.x, light.color.y, light.color.z };
							cachedParticleLight.position = { light.positionWS.x + state->posAdjust.getEye().x, light.positionWS.y + state->posAdjust.getEye().y, light.positionWS.z + state->posAdjust.getEye().z };
							cachedParticleLight.radius = light.radius;
							cachedParticleLights.push_back(cachedParticleLight);

							light.color.x *= dimmer * 2.0f;
							light.color.y *= dimmer * 2.0f;
							light.color.z *= dimmer * 2.0f;

							light.positionVS = DirectX::SimpleMath::Vector3::Transform(light.positionWS, state->cameraData.getEye().viewMat);

							light.shadowMode = 2 * settings.EnableContactShadows;

							lightsData.push_back(light);
							currentLightCount++;
						}
					}
				}
			} else {
				LightData light{};

				light.color.x = particleLight.second.red;
				light.color.y = particleLight.second.green;
				light.color.z = particleLight.second.blue;

				float radius = particleLight.first->modelBound.radius * particleLight.first->world.scale;

				light.radius = radius * settings.ParticleLightsRadiusBillboards;

				RE::NiPoint3 positionWS = particleLight.first->worldBound.center - state->posAdjust.getEye();
				light.positionWS.x = positionWS.x;
				light.positionWS.y = positionWS.y;
				light.positionWS.z = positionWS.z;

				float distance = (light.positionWS.x * light.positionWS.x) + (light.positionWS.y * light.positionWS.y) + (light.positionWS.z * light.positionWS.z) - (light.radius * light.radius);

				float dimmer = 0.0f;
				if (!settings.EnableParticleLightsFade || distance < lightFadeStart || lightFadeEnd == 0.0f) {
					dimmer = 1.0f;
				} else if (distance <= lightFadeEnd) {
					dimmer = 1.0f - ((distance - lightFadeStart) / (lightFadeEnd - lightFadeStart));
				} else {
					dimmer = 0.0f;
				}

				if (dimmer != 0) {
					if (light.color.x > 0 && light.color.y > 0 && light.color.z > 0 && light.radius > 0) {
						CachedParticleLight cachedParticleLight{};
						cachedParticleLight.color = { light.color.x, light.color.y, light.color.z };
						cachedParticleLight.position = { light.positionWS.x + state->posAdjust.getEye().x, light.positionWS.y + state->posAdjust.getEye().y, light.positionWS.z + state->posAdjust.getEye().z };
						cachedParticleLight.radius = light.radius;
						cachedParticleLights.push_back(cachedParticleLight);

						light.color.x *= dimmer;
						light.color.y *= dimmer;
						light.color.z *= dimmer;

						light.positionVS = DirectX::SimpleMath::Vector3::Transform(light.positionWS, state->cameraData.getEye().viewMat);

						light.shadowMode = 2 * settings.EnableContactShadows;

						lightsData.push_back(light);
						currentLightCount++;
					}
				}
			}
		}
	}

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	{
		if (!currentLightCount) {
			LightData data{};
			lightsData.push_back(data);
			currentLightCount = 1;
		}

		bool lightCountChanged = currentLightCount != lightCount;

		if (!lights || lightCountChanged) {
			lightCount = currentLightCount;

			D3D11_BUFFER_DESC sbDesc{};
			sbDesc.Usage = D3D11_USAGE_DYNAMIC;
			sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			sbDesc.StructureByteStride = sizeof(LightData);
			sbDesc.ByteWidth = sizeof(LightData) * lightCount;
			lights = eastl::make_unique<Buffer>(sbDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = lightCount;
			lights->CreateSRV(srvDesc);
		}

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(lights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(LightData) * lightCount;
		memcpy_s(mapped.pData, bytes, lightsData.data(), bytes);
		context->Unmap(lights->resource.get(), 0);
	}

	{
		auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();

		lightsNear = std::max(0.0f, accumulator->kCamera->viewFrustum.fNear);
		lightsFar = std::min(16384.0f, accumulator->kCamera->viewFrustum.fFar);

		float fov = atan(1.0f / static_cast<float4x4>(state->cameraData.getEye().projMatrixUnjittered).m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

		static float _near = 0.0f, _far = 0.0f, _fov = 0.0f, _lightsNear = 0.0f, _lightsFar = 0.0f;
		if (fabs(_near - accumulator->kCamera->viewFrustum.fNear) > 1e-4 || fabs(_far - accumulator->kCamera->viewFrustum.fFar) > 1e-4 || fabs(_fov - fov) > 1e-4 || fabs(_lightsNear - lightsNear) > 1e-4 || fabs(_lightsFar - lightsFar) > 1e-4) {
			PerFrameLightCulling perFrameData{};
			perFrameData.InvProjMatrix = DirectX::XMMatrixInverse(nullptr, state->cameraData.getEye().projMatrixUnjittered);
			perFrameData.LightsNear = lightsNear;
			perFrameData.LightsFar = lightsFar;

			perFrameLightCulling->Update(perFrameData);

			ID3D11Buffer* perframe_cb = perFrameLightCulling->CB();
			context->CSSetConstantBuffers(0, 1, &perframe_cb);

			ID3D11UnorderedAccessView* clusters_uav = clusters->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &clusters_uav, nullptr);

			context->CSSetShader(clusterBuildingCS, nullptr, 0);
			context->Dispatch(CLUSTER_SIZE_X, CLUSTER_SIZE_Y, CLUSTER_SIZE_Z);
			context->CSSetShader(nullptr, nullptr, 0);

			ID3D11UnorderedAccessView* null_uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);

			_near = accumulator->kCamera->viewFrustum.fNear;
			_far = accumulator->kCamera->viewFrustum.fFar;
			_fov = fov;
			_lightsNear = lightsNear;
			_lightsFar = lightsFar;
		}
	}

	{
		ID3D11ShaderResourceView* srvs[] = { clusters->srv.get(), lights->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		ID3D11UnorderedAccessView* uavs[] = { lightCounter->uav.get(), lightList->uav.get(), lightGrid->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(clusterCullingCS, nullptr, 0);
		context->Dispatch(CLUSTER_SIZE_X / 16, CLUSTER_SIZE_Y / 8, CLUSTER_SIZE_Z / 8);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	ID3D11ShaderResourceView* null_srvs[2] = { nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(null_srvs), null_srvs);
	ID3D11UnorderedAccessView* null_uavs[3] = { nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(null_uavs), null_uavs, nullptr);
}