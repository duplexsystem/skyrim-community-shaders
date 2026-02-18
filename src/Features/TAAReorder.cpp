#include "TAAReorder.h"

#include "Globals.h"
#include "Upscaling.h"
#include <d3d11.h>

namespace TAAReorder
{
	bool ShouldReorderTAA()
	{
		if (!g_initialized)
			return false;
		auto& upscaling = globals::features::upscaling;
		return globals::game::isVR &&
		       upscaling.settings.vrPeripheryTAA &&
		       upscaling.settings.vrDlssViewportScale < 1.0f &&
		       upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS;
	}

	// ─── Setter A: Force TAA (pass-through) ───
	void ForceTAASetter::thunk()
	{
		func();
	}

	// ─── Setter B: TAA State Machine (pass-through) ───
	void TAAStateMachine::thunk()
	{
		func();
	}

	// ─── EnsurePostPPCopy: create/resize staging texture matching source ───
	void EnsurePostPPCopy(ID3D11Texture2D* sourceTex)
	{
		D3D11_TEXTURE2D_DESC srcDesc;
		sourceTex->GetDesc(&srcDesc);

		if (g_postPPCopy) {
			D3D11_TEXTURE2D_DESC existingDesc;
			g_postPPCopy->GetDesc(&existingDesc);
			if (existingDesc.Width == srcDesc.Width && existingDesc.Height == srcDesc.Height &&
				existingDesc.Format == srcDesc.Format)
				return;
		}

		D3D11_TEXTURE2D_DESC desc = srcDesc;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = 0;
		g_postPPCopy = nullptr;
		g_postPPCopySRV = nullptr;
		globals::d3d::device->CreateTexture2D(&desc, nullptr, g_postPPCopy.put());

		if (g_postPPCopy) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			globals::d3d::device->CreateShaderResourceView(g_postPPCopy.get(), &srvDesc, g_postPPCopySRV.put());
			Util::SetResourceName(g_postPPCopy.get(), "TAAReorder_PostPPCopy");
		}
	}

	// ─── Helper: set up common fullscreen rendering state ───
	static void SetupFullscreenState(ID3D11DeviceContext* context, float vpX, float vpY, float vpW, float vpH)
	{
		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = vpX;
		viewport.TopLeftY = vpY;
		viewport.Width = vpW;
		viewport.Height = vpH;
		viewport.MaxDepth = 1.0f;

		auto& upscaling = globals::features::upscaling;
		context->RSSetViewports(1, &viewport);
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(upscaling.GetUpscaleVS(), nullptr, 0);
		context->RSSetState(upscaling.upscaleRasterizerState.get());
		context->OMSetBlendState(upscaling.upscaleBlendState.get(), nullptr, 0xffffffff);
	}

	// ─── Helper: draw fullscreen triangle (point-sample format-converting copy) ───
	void DrawFullscreenCopy(ID3D11ShaderResourceView* srcSRV, ID3D11RenderTargetView* dstRTV,
		float vpX, float vpY, float vpW, float vpH)
	{
		auto& upscaling = globals::features::upscaling;
		auto context = globals::d3d::context;

		SetupFullscreenState(context, vpX, vpY, vpW, vpH);
		context->PSSetShader(upscaling.GetDlssCompositePS(), nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { srcSRV };
		context->PSSetShaderResources(0, 1, srvs);

		ID3D11RenderTargetView* rtvs[] = { dstRTV };
		context->OMSetRenderTargets(1, rtvs, nullptr);

		context->Draw(3, 0);
	}

	// ─── ExecutePass hook: capture Phase 2A output, detect Phase 5 ───
	void ExecutePassHook::thunk(void* manager, void* passObj, int srcTech, int dstTech, void* extraData, uint8_t flag)
	{
		bool isPeripheryTAA = ShouldReorderTAA();
		bool shouldLog = (g_diagCounter == 0);

		// Compute pass index for Phase 2A / Phase 5 detection
		int passIndex = -1;
		if (isPeripheryTAA || shouldLog) {
			uintptr_t managerAddr = (uintptr_t)manager;
			uintptr_t passArrayBase = *(uintptr_t*)(managerAddr + 0x28);
			if (passArrayBase) {
				for (int i = 0; i < 40; i++) {
					if (*(uintptr_t*)(passArrayBase + i * 8) == (uintptr_t)passObj) {
						passIndex = i;
						break;
					}
				}
			}
		}

		if (shouldLog)
			logger::info("[TAAReorder] ExecutePass: src=0x{:X} dst=0x{:X} flag={} passIdx={}",
				srcTech, dstTech, flag, passIndex);

		// Execute the original pass
		func(manager, passObj, srcTech, dstTech, extraData, flag);

		// After Phase 2A: copy output RT to g_postPPCopy for DLSS to process
		if (isPeripheryTAA && passIndex == 30 && dstTech == 0x29) {
			ID3D11RenderTargetView* postRTV = nullptr;
			globals::d3d::context->OMGetRenderTargets(1, &postRTV, nullptr);
			if (postRTV) {
				ID3D11Resource* res = nullptr;
				postRTV->GetResource(&res);
				if (res) {
					ID3D11Texture2D* postTex = nullptr;
					res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&postTex);
					if (postTex) {
						EnsurePostPPCopy(postTex);
						globals::d3d::context->CopyResource(g_postPPCopy.get(), postTex);
						g_postPPReady = true;
						if (shouldLog) {
							D3D11_TEXTURE2D_DESC desc;
							postTex->GetDesc(&desc);
							logger::info("[TAAReorder] Phase 2A output: {}x{} fmt={} → copied to g_postPPCopy",
								desc.Width, desc.Height, (uint32_t)desc.Format);
						}
						postTex->Release();
					}
					res->Release();
				}
				postRTV->Release();
			}
		}

		// Detect Phase 5 completion
		if (isPeripheryTAA && passIndex == 35) {
			g_phase5Complete = true;
			if (shouldLog)
				logger::info("[TAAReorder] Phase 5 complete (passIdx=35)");
		}
	}

	// ─── BSImagespaceShader hook: DLSS eval + paste after pipeline completes ───
	// Wraps call at 0x132C827 (write_thunk_call). func() encompasses the
	// conductor (Phase 2A) but NOT Phase 5 (TAA+DRS) — Phase 5 runs after us.
	// We evaluate DLSS on the captured Phase 2A output and paste the center
	// via CopySubresourceRegion. HAM artifacts are suppressed by blocking HAM
	// rendering in HiddenAreaMeshHook when peripheryTAA is active.
	void BSImagespaceShaderHook::thunk(void* a_this, uint64_t a_param)
	{
		func(a_this, a_param);

		if (!ShouldReorderTAA())
			return;

		bool shouldLog = (g_diagCounter == 0);
		auto context = globals::d3d::context;
		auto& upscaling = globals::features::upscaling;

		// Get submit texture from bound RT after pipeline stage completes
		ID3D11RenderTargetView* submitRTV = nullptr;
		context->OMGetRenderTargets(1, &submitRTV, nullptr);
		ID3D11Texture2D* submitTex = nullptr;
		if (submitRTV) {
			ID3D11Resource* res = nullptr;
			submitRTV->GetResource(&res);
			if (res) {
				res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&submitTex);
				res->Release();
			}
			submitRTV->Release();
		}

		if (shouldLog) {
			if (submitTex) {
				D3D11_TEXTURE2D_DESC desc;
				submitTex->GetDesc(&desc);
				logger::info("[TAAReorder] BSImagespaceShaderHook: submitTex=0x{:X} {}x{} fmt={} bind=0x{:X} postPPReady={} phase5={}",
					(uintptr_t)submitTex, desc.Width, desc.Height, (uint32_t)desc.Format,
					desc.BindFlags, g_postPPReady, g_phase5Complete);
			} else {
				logger::info("[TAAReorder] BSImagespaceShaderHook: no submitTex bound");
			}
		}

		// Step 1: Evaluate DLSS on the captured post-PP intermediate
		if (g_postPPReady && g_postPPCopy) {
			if (shouldLog)
				logger::info("[TAAReorder] BSImagespaceShaderHook: evaluating DLSS on g_postPPCopy...");

			upscaling.Upscale(g_postPPCopy.get());
			g_dlssReady = true;

			if (shouldLog)
				logger::info("[TAAReorder] BSImagespaceShaderHook: DLSS evaluation complete");
		} else if (shouldLog) {
			logger::info("[TAAReorder] BSImagespaceShaderHook: skip DLSS (postPPReady={} postPPCopy={})",
				g_postPPReady, (void*)g_postPPCopy.get());
		}

		// Step 2: Paste DLSS center from g_postPPCopy onto submit texture per-eye
		if (g_dlssReady && submitTex && g_postPPCopy) {
			auto screenSize = globals::state->screenSize;
			uint32_t eyeW = (uint32_t)(screenSize.x / 2);
			uint32_t eyeH = (uint32_t)screenSize.y;
			float vpScale = upscaling.settings.vrDlssViewportScale;
			uint32_t centerW = (uint32_t)(eyeW * vpScale);
			uint32_t centerH = (uint32_t)(eyeH * vpScale);
			uint32_t centerX = (eyeW - centerW) / 2;
			uint32_t centerY = (eyeH - centerH) / 2;

			for (uint32_t i = 0; i < 2; i++) {
				uint32_t eyeOffset = i * eyeW;
				D3D11_BOX srcBox = {
					eyeOffset + centerX, centerY, 0,
					eyeOffset + centerX + centerW, centerY + centerH, 1
				};
				context->CopySubresourceRegion(submitTex, 0,
					eyeOffset + centerX, centerY, 0,
					g_postPPCopy.get(), 0, &srcBox);
			}

			g_dlssPasteComplete = true;

			if (shouldLog)
				logger::info("[TAAReorder] BSImagespaceShaderHook: pasted DLSS center {}x{} at ({},{}) per-eye onto submit",
					centerW, centerH, centerX, centerY);
		} else if (shouldLog) {
			logger::info("[TAAReorder] BSImagespaceShaderHook: skip paste (dlssReady={} submitTex={} postPPCopy={})",
				g_dlssReady, (void*)submitTex, (void*)g_postPPCopy.get());
		}

		if (submitTex)
			submitTex->Release();
	}

	// ─── Depth/stencil registration hook: diagnostic logging ───
	// Tracks dimensions per slot and logs whenever they change.
	// data[0]=width, data[1]=height based on initial analysis.
	void DepthStencilRegHook::thunk(void* manager, uint32_t slot, void* desc)
	{
		if (desc && slot < 32) {
			auto* data = reinterpret_cast<uint32_t*>(desc);
			static uint32_t lastWidth[32] = {};
			static uint32_t lastHeight[32] = {};
			static uint32_t callCount[32] = {};

			callCount[slot]++;
			bool dimsChanged = (data[0] != lastWidth[slot] || data[1] != lastHeight[slot]);
			if (dimsChanged) {
				logger::info("[TAAReorder] DepthStencilReg: slot={} {}x{} → {}x{} (call #{}) data[2..7]= {} {} {} {} {} {}",
					slot, lastWidth[slot], lastHeight[slot], data[0], data[1], callCount[slot],
					data[2], data[3], data[4], data[5], data[6], data[7]);
				lastWidth[slot] = data[0];
				lastHeight[slot] = data[1];
			}
		}

		func(manager, slot, desc);
	}

	// ─── Hidden area mesh render hook: suppress stencil overlay when peripheryTAA active ───
	// HAM has two parts: (1) culling = performance optimization, (2) stencil overlay = cosmetic.
	// The stencil overlay writes at render-res coordinates. After DRS upscaling to display-res,
	// the stencil boundary misaligns with color, creating "frozen frame" artifacts.
	// We suppress the stencil overlay (mode TBD) but keep culling for performance.
	// SteamVR's own HAM handles final lens masking at submit time.
	void HiddenAreaMeshHook::thunk(void* rendererState, uint8_t mode)
	{
		if (g_diagCounter == 0)
			logger::info("[TAAReorder] HiddenAreaMeshHook: mode={} peripheryTAA={}", mode, ShouldReorderTAA());

		// HAM fully enabled for RenderDoc capture to diagnose culling vs stencil overlay
		func(rendererState, mode);
	}

	// ─── BSOpenVR::Submit hook: diagnostic logging ───
	void SubmitHook::thunk(void* thisPtr, void* textureHandle)
	{
		if (g_diagCounter == 0 && textureHandle) {
			auto tex2d = static_cast<ID3D11Texture2D*>(textureHandle);
			D3D11_TEXTURE2D_DESC desc = {};
			tex2d->GetDesc(&desc);
			auto base = REL::Module::get().base();
			auto retAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
			logger::info("[TAAReorder] Submit: tex=0x{:X} {}x{} fmt={} dlssPasted={} callerRVA=0x{:X}",
				(uintptr_t)textureHandle, desc.Width, desc.Height, (uint32_t)desc.Format,
				g_dlssPasteComplete, retAddr - base);
		}

		func(thisPtr, textureHandle);
	}

	// ─── Post-processing conductor call hook: pass-through (tracking only) ───
	// Inner conductor call at 0x1325086 inside BSImagespaceShader::Render.
	// Only tracks g_insideConductor state. DLSS logic is in BSImagespaceShaderHook.
	void ConductorCallHook::thunk(void* a1, void* a2, void* a3, void* a4)
	{
		g_insideConductor = true;
		func(a1, a2, a3, a4);
		g_insideConductor = false;
	}

	void InitEarly()
	{
		auto base = REL::Module::get().base();

		// ─── Hook: DepthStencilRegistration (RVA 0x00DC79D0) ───
		// Must be installed before renderer initialization (which registers depth/stencil targets).
		// Called from Upscaling::Load(), before D3D device creation.
		DepthStencilRegHook::func = reinterpret_cast<RegisterDepthStencil_t>(base + 0x00DC79D0);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&DepthStencilRegHook::func), reinterpret_cast<PVOID>(DepthStencilRegHook::thunk));
		DetourTransactionCommit();

		logger::info("[TAAReorder] InitEarly: DepthStencil registration hooked at RVA 0x00DC79D0");
	}

	void Init()
	{
		auto base = REL::Module::get().base();

		// ─── Core pointers ───
		g_pRendererSingleton = reinterpret_cast<uintptr_t*>(base + 0x034234C0);

		// ─── Hook: ForceTAASetter (RVA 0x005C8EE0) ───
		ForceTAASetter::func = base + 0x005C8EE0;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&ForceTAASetter::func), reinterpret_cast<PVOID>(ForceTAASetter::thunk));
		DetourTransactionCommit();

		// ─── Hook: TAAStateMachine (RVA 0x005C8F10) ───
		TAAStateMachine::func = base + 0x005C8F10;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&TAAStateMachine::func), reinterpret_cast<PVOID>(TAAStateMachine::thunk));
		DetourTransactionCommit();

		// ─── Hook: ExecutePass (RVA 0x012D2540) ───
		ExecutePassHook::func = reinterpret_cast<ExecutePass_t>(base + 0x012D2540);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&ExecutePassHook::func), reinterpret_cast<PVOID>(ExecutePassHook::thunk));
		DetourTransactionCommit();

		// ─── Hook: HiddenAreaMesh (RVA 0x00DC2980) ───
		HiddenAreaMeshHook::func = reinterpret_cast<HiddenAreaMeshRender_t>(base + 0x00DC2980);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&HiddenAreaMeshHook::func), reinterpret_cast<PVOID>(HiddenAreaMeshHook::thunk));
		DetourTransactionCommit();

		// ─── Hook: BSOpenVR::Submit (RVA 0x00C53920) ───
		SubmitHook::func = reinterpret_cast<BSOpenVRSubmit_t>(base + 0x00C53920);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&SubmitHook::func), reinterpret_cast<PVOID>(SubmitHook::thunk));
		DetourTransactionCommit();

		// ─── Hook: BSImagespaceShader via write_thunk_call at RVA 0x132C827 ───
		// Wraps BSImagespaceShader::Render from the Orchestrator level.
		// func() encompasses conductor (Phase 2A) + Phase 5 (TAA+DRS) + Submit.
		// After func(): DLSS eval + paste. Matches PureDark's BSImagespaceShader_Hook_VR.
		stl::write_thunk_call<BSImagespaceShaderHook>(base + 0x132C827);

		// ─── Hook: Inner conductor call via write_thunk_call at RVA 0x1325086 ───
		// Pass-through, only tracks g_insideConductor state.
		stl::write_thunk_call<ConductorCallHook>(base + 0x1325086);

		g_initialized = true;

		logger::info("[TAAReorder] Initialized — base=0x{:X}", base);
		logger::info("[TAAReorder] Post-pipeline DLSS mode (periphery TAA)");
		logger::info("[TAAReorder] BSImagespaceShader hooked via write_thunk_call at RVA 0x132C827 (DLSS eval + paste)");
		logger::info("[TAAReorder] Inner conductor hooked via write_thunk_call at RVA 0x1325086 (tracking only)");
		logger::info("[TAAReorder] HiddenAreaMesh renderer hooked at RVA 0x00DC2980");
		logger::info("[TAAReorder] BSOpenVR::Submit hooked at RVA 0x00C53920");
	}
}
