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

	// ─── Setter A: Pass-through ───
	void ForceTAASetter::thunk()
	{
		func();
	}

	// ─── Setter B: Pass-through ───
	void TAAStateMachine::thunk()
	{
		func();
	}

	// ─── Capture RTV[0] from D3D11 state after a pass completes ───
	// Keeps both the texture (for desc queries) and the RTV (for PS composite).
	static void CaptureRTV0()
	{
		// Release any previously captured resources
		if (g_capturedTAAOutput) {
			g_capturedTAAOutput->Release();
			g_capturedTAAOutput = nullptr;
		}
		if (g_capturedTAARTV) {
			g_capturedTAARTV->Release();
			g_capturedTAARTV = nullptr;
		}

		auto context = globals::d3d::context;
		if (!context)
			return;

		ID3D11RenderTargetView* rtv = nullptr;
		context->OMGetRenderTargets(1, &rtv, nullptr);

		if (rtv) {
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			if (res) {
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&g_capturedTAAOutput))) {
					// Keep the RTV for the pixel shader composite pass
					g_capturedTAARTV = rtv;  // take ownership of the AddRef from OMGetRenderTargets
					rtv = nullptr;           // prevent release below

					if (g_diagCounter == 0) {
						D3D11_TEXTURE2D_DESC desc;
						g_capturedTAAOutput->GetDesc(&desc);
						logger::info("[TAAReorder] Captured TAA output: tex=0x{:X} {}x{} fmt={} arraySize={}",
							(uintptr_t)g_capturedTAAOutput, desc.Width, desc.Height,
							(uint32_t)desc.Format, desc.ArraySize);
					}
				}
				res->Release();
			}
			if (rtv)
				rtv->Release();
		}
	}

	// ─── Capture Phase1 PP output (pre-TAA) from RTV[0] ───
	// Called after each non-TAA pass in FirstFunc; the last capture before TAA
	// is the Phase1 PP output (display-res, no HAM overlay).
	static void CapturePPOutput()
	{
		if (g_capturedPPOutput) {
			g_capturedPPOutput->Release();
			g_capturedPPOutput = nullptr;
		}

		auto context = globals::d3d::context;
		if (!context)
			return;

		ID3D11RenderTargetView* rtv = nullptr;
		context->OMGetRenderTargets(1, &rtv, nullptr);

		if (rtv) {
			ID3D11Resource* res = nullptr;
			rtv->GetResource(&res);
			if (res) {
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&g_capturedPPOutput))) {
					if (g_diagCounter == 0) {
						D3D11_TEXTURE2D_DESC desc;
						g_capturedPPOutput->GetDesc(&desc);
						logger::info("[TAAReorder] Captured PP output: tex=0x{:X} {}x{} fmt={}",
							(uintptr_t)g_capturedPPOutput, desc.Width, desc.Height, (uint32_t)desc.Format);
					}
				}
				res->Release();
			}
			rtv->Release();
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
	// Renders Source SRV → destination RTV using DLSSCompositePS (Load-based, 1:1 pixel copy).
	// The output merger handles format conversion automatically.
	static void DrawFullscreenCopy(ID3D11ShaderResourceView* srcSRV, ID3D11RenderTargetView* dstRTV,
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

	// ─── Helper: draw fullscreen bilinear upscale (render-res → display-res) ───
	// Uses DlssUpscalePS with DynResScale to map display-res output pixels to render-res source.
	static void DrawBilinearUpscale(ID3D11ShaderResourceView* srcSRV, ID3D11RenderTargetView* dstRTV,
		float vpX, float vpY, float vpW, float vpH,
		float dynResW, float dynResH, float eyeOffsetX, float texW, float texH)
	{
		auto& upscaling = globals::features::upscaling;
		auto context = globals::d3d::context;

		// Ensure upscale PS and CB are created
		upscaling.GetDlssUpscalePS();

		// Update constant buffer
		Upscaling::DlssCompositeCB cbData;
		cbData.DynResScale = { dynResW, dynResH };
		cbData.EyeOffset = { eyeOffsetX, 0.0f };
		cbData.SrcTexSize = { texW, texH };
		cbData.pad = { 0, 0 };

		D3D11_MAPPED_SUBRESOURCE mapped{};
		context->Map(upscaling.vrDlssUpscaleCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		memcpy(mapped.pData, &cbData, sizeof(cbData));
		context->Unmap(upscaling.vrDlssUpscaleCB.get(), 0);

		SetupFullscreenState(context, vpX, vpY, vpW, vpH);
		context->PSSetShader(upscaling.GetDlssUpscalePS(), nullptr, 0);

		ID3D11Buffer* cbs[] = { upscaling.vrDlssUpscaleCB.get() };
		context->PSSetConstantBuffers(0, 1, cbs);

		// Set linear sampler (create if needed — reuse vrLinearSampler)
		if (!upscaling.vrLinearSampler) {
			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, upscaling.vrLinearSampler.put()));
		}
		ID3D11SamplerState* samplers[] = { upscaling.vrLinearSampler.get() };
		context->PSSetSamplers(0, 1, samplers);

		ID3D11ShaderResourceView* srvs[] = { srcSRV };
		context->PSSetShaderResources(0, 1, srvs);

		ID3D11RenderTargetView* rtvs[] = { dstRTV };
		context->OMSetRenderTargets(1, rtvs, nullptr);

		context->Draw(3, 0);
	}

	// ─── Ensure DLSS output copy texture exists and matches kMAIN dimensions/format ───
	static void EnsureDlssOutputCopy(ID3D11Texture2D* kMainTex)
	{
		D3D11_TEXTURE2D_DESC mainDesc;
		kMainTex->GetDesc(&mainDesc);

		bool needCreate = !g_dlssOutputCopy;
		if (g_dlssOutputCopy) {
			D3D11_TEXTURE2D_DESC existingDesc;
			g_dlssOutputCopy->GetDesc(&existingDesc);
			if (existingDesc.Width != mainDesc.Width || existingDesc.Height != mainDesc.Height ||
				existingDesc.Format != mainDesc.Format)
				needCreate = true;
		}

		if (needCreate) {
			g_dlssOutputCopy = nullptr;
			g_dlssOutputCopySRV = nullptr;

			D3D11_TEXTURE2D_DESC copyDesc = {};
			copyDesc.Width = mainDesc.Width;
			copyDesc.Height = mainDesc.Height;
			copyDesc.MipLevels = 1;
			copyDesc.ArraySize = 1;
			copyDesc.Format = mainDesc.Format;
			copyDesc.SampleDesc.Count = 1;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			auto hr = globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, g_dlssOutputCopy.put());
			if (FAILED(hr)) {
				logger::error("[TAAReorder] Failed to create DLSS output copy texture ({}x{} fmt={})",
					copyDesc.Width, copyDesc.Height, (uint32_t)copyDesc.Format);
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = copyDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = globals::d3d::device->CreateShaderResourceView(g_dlssOutputCopy.get(), &srvDesc, g_dlssOutputCopySRV.put());
			if (FAILED(hr)) {
				logger::error("[TAAReorder] Failed to create SRV for DLSS output copy");
				g_dlssOutputCopy = nullptr;
				return;
			}

			logger::info("[TAAReorder] Created DLSS output copy texture {}x{} fmt={}",
				copyDesc.Width, copyDesc.Height, (uint32_t)copyDesc.Format);
		}
	}

	// ─── Ensure pre-DRS TAA copy texture matches the TAA output ───
	static void EnsurePreDRSTAACopy(ID3D11Texture2D* taaTex)
	{
		D3D11_TEXTURE2D_DESC taaDesc;
		taaTex->GetDesc(&taaDesc);

		bool needCreate = !g_preDRSTAACopy;
		if (g_preDRSTAACopy) {
			D3D11_TEXTURE2D_DESC existingDesc;
			g_preDRSTAACopy->GetDesc(&existingDesc);
			if (existingDesc.Width != taaDesc.Width || existingDesc.Height != taaDesc.Height ||
				existingDesc.Format != taaDesc.Format)
				needCreate = true;
		}

		if (needCreate) {
			g_preDRSTAACopy = nullptr;
			g_preDRSTAACopySRV = nullptr;

			D3D11_TEXTURE2D_DESC copyDesc = {};
			copyDesc.Width = taaDesc.Width;
			copyDesc.Height = taaDesc.Height;
			copyDesc.MipLevels = 1;
			copyDesc.ArraySize = 1;
			copyDesc.Format = taaDesc.Format;
			copyDesc.SampleDesc.Count = 1;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			auto hr = globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, g_preDRSTAACopy.put());
			if (FAILED(hr)) {
				logger::error("[TAAReorder] Failed to create pre-DRS TAA copy texture");
				return;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = copyDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = globals::d3d::device->CreateShaderResourceView(g_preDRSTAACopy.get(), &srvDesc, g_preDRSTAACopySRV.put());
			if (FAILED(hr)) {
				logger::error("[TAAReorder] Failed to create SRV for pre-DRS TAA copy");
				g_preDRSTAACopy = nullptr;
				return;
			}

			logger::info("[TAAReorder] Created pre-DRS TAA copy {}x{} fmt={}",
				copyDesc.Width, copyDesc.Height, (uint32_t)copyDesc.Format);
		}
	}

	// ─── Post-conductor DLSS composite ───
	// Called AFTER func() returns. Runs DLSS, composites directly onto the TAA RT.
	// By compositing HERE (before the game draws UI), UI will naturally appear
	// on top of both the TAA periphery and the DLSS center.
	//
	// Flow:
	//   1. Blit PP → kMAIN (format conversion for DLSS input)
	//   2. Run DLSS → kMAIN has display-res DLSS output
	//   3. Overwrite TAA RT with clean AFTER-TAA snapshot (removes post-conductor mask)
	//   4. Paste DLSS center per-eye from kMAIN onto TAA RT
	//   5. Restore kMAIN from vrPreTAACopy
	//   6. Game draws UI on TAA RT → both TAA and DLSS areas get UI
	void CompositeAfterConductor()
	{
		if (!g_capturedTAAOutput || !g_capturedTAARTV)
			return;

		auto& upscaling = globals::features::upscaling;
		auto context = globals::d3d::context;
		auto device = globals::d3d::device;
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& mainTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		float vpScale = upscaling.settings.vrDlssViewportScale;

		ID3D11ShaderResourceView* nullSRV[] = { nullptr };

		// Step 1: Blit post-PP content → kMAIN (format conversion via pixel shader).
		if (g_capturedPPOutput) {
			D3D11_TEXTURE2D_DESC ppDesc;
			g_capturedPPOutput->GetDesc(&ppDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = ppDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;

			ID3D11ShaderResourceView* ppSRV = nullptr;
			if (SUCCEEDED(device->CreateShaderResourceView(g_capturedPPOutput, &srvDesc, &ppSRV))) {
				DrawFullscreenCopy(ppSRV, mainTexture.RTV,
					0.0f, 0.0f, (float)ppDesc.Width, (float)ppDesc.Height);
				ppSRV->Release();

				if (g_diagCounter == 0)
					logger::info("[TAAReorder] Blit post-PP → kMAIN via PS ({}x{} fmt={} → kMAIN)",
						ppDesc.Width, ppDesc.Height, (uint32_t)ppDesc.Format);
			} else {
				if (g_diagCounter == 0)
					logger::warn("[TAAReorder] Failed to create SRV for PP output (fmt={})", (uint32_t)ppDesc.Format);
			}

			g_capturedPPOutput->Release();
			g_capturedPPOutput = nullptr;
		}

		context->PSSetShaderResources(0, 1, nullSRV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		// Step 2: Run DLSS (reads from kMAIN at render-res, outputs to kMAIN at display-res).
		upscaling.Upscale();

		if (upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS)
			upscaling.ApplySharpening();

		// Aggressively clear ALL pipeline state after DLSS.
		// DLSS/Streamline may leave textures (including the TAA RT) bound as SRVs,
		// which would cause CopyResource to silently fail on those textures.
		{
			ID3D11ShaderResourceView* nullSRVs[16] = {};
			context->PSSetShaderResources(0, 16, nullSRVs);
			context->VSSetShaderResources(0, 16, nullSRVs);
			context->CSSetShaderResources(0, 16, nullSRVs);
			ID3D11UnorderedAccessView* nullUAVs[8] = {};
			context->CSSetUnorderedAccessViews(0, 8, nullUAVs, nullptr);
			context->OMSetRenderTargets(0, nullptr, nullptr);
		}

		// Step 3: Overwrite TAA RT with clean AFTER-TAA snapshot via DrawFullscreenCopy.
		// Uses rendering (not CopyResource) to ensure RTV binding auto-unbinds any
		// remaining SRV conflicts that could cause silent failure.
		// Both textures are R8G8B8A8_UNORM; the Load shader does a 1:1 pixel copy.
		if (g_preDRSTAACopy && g_preDRSTAACopySRV) {
			D3D11_TEXTURE2D_DESC taaDesc;
			g_capturedTAAOutput->GetDesc(&taaDesc);
			DrawFullscreenCopy(g_preDRSTAACopySRV.get(), g_capturedTAARTV,
				0.0f, 0.0f, (float)taaDesc.Width, (float)taaDesc.Height);
			if (g_diagCounter == 0)
				logger::info("[TAAReorder] Overwrote TAA RT with clean AFTER-TAA snapshot via DrawFullscreenCopy ({}x{})",
					taaDesc.Width, taaDesc.Height);
		} else {
			if (g_diagCounter == 0)
				logger::warn("[TAAReorder] Step 3 SKIPPED: g_preDRSTAACopy={} g_preDRSTAACopySRV={}",
					(bool)g_preDRSTAACopy, (bool)g_preDRSTAACopySRV);
		}

		// Step 4: Paste DLSS center per-eye from kMAIN onto TAA RT.
		// The Load-based shader reads at SV_POSITION.xy, so per-eye center viewports
		// naturally map to the correct per-eye regions of the stereo kMAIN buffer.
		{
			D3D11_TEXTURE2D_DESC taaDesc;
			g_capturedTAAOutput->GetDesc(&taaDesc);

			uint32_t eyeW = taaDesc.Width / 2;
			uint32_t eyeH = taaDesc.Height;
			uint32_t centerW = (uint32_t)(eyeW * vpScale);
			uint32_t centerH = (uint32_t)(eyeH * vpScale);
			uint32_t centerX = (eyeW - centerW) / 2;
			uint32_t centerY = (eyeH - centerH) / 2;

			// Create temporary SRV for kMAIN (DLSS output, R11G11B10_FLOAT)
			D3D11_TEXTURE2D_DESC mainDesc;
			mainTexture.texture->GetDesc(&mainDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = mainDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;

			ID3D11ShaderResourceView* mainSRV = nullptr;
			if (SUCCEEDED(device->CreateShaderResourceView(mainTexture.texture, &srvDesc, &mainSRV))) {
				for (uint32_t i = 0; i < 2; i++) {
					uint32_t eyeOffset = i * eyeW;
					DrawFullscreenCopy(mainSRV, g_capturedTAARTV,
						(float)(eyeOffset + centerX), (float)centerY,
						(float)centerW, (float)centerH);
				}
				mainSRV->Release();

				// Save layout info for diagnostics and HAM skip
				g_savedEyeW = eyeW;
				g_savedEyeH = eyeH;
				g_savedVpScale = vpScale;
				g_savedDynResW = upscaling.resolutionScale.x;
				g_savedDynResH = upscaling.resolutionScale.y;
				g_dlssOutputReady = true;  // Signal for HAM skip

				if (g_diagCounter == 0)
					logger::info("[TAAReorder] Composited DLSS center {}x{} at ({},{}) per eye onto TAA RT (before UI)",
						centerW, centerH, centerX, centerY);
			} else {
				if (g_diagCounter == 0)
					logger::warn("[TAAReorder] Failed to create SRV for kMAIN (DLSS output)");
			}
		}

		// Clean up rendering state
		context->PSSetShaderResources(0, 1, nullSRV);
		context->OMSetRenderTargets(0, nullptr, nullptr);

		// Step 5: Restore kMAIN from vrPreTAACopy
		if (upscaling.vrPreTAACopy)
			context->CopyResource(mainTexture.texture, upscaling.vrPreTAACopy.get());

		// Release captured TAA refs
		g_capturedTAARTV->Release();
		g_capturedTAARTV = nullptr;
		g_capturedTAAOutput->Release();
		g_capturedTAAOutput = nullptr;
	}

	// ─── Diagnostic: dump all bound SRVs for a given shader stage ───
	static void DumpBoundSRVs(ID3D11DeviceContext* context, const char* stage, const char* timing)
	{
		constexpr int MAX_SLOTS = 16;
		ID3D11ShaderResourceView* srvs[MAX_SLOTS] = {};

		if (strcmp(stage, "PS") == 0)
			context->PSGetShaderResources(0, MAX_SLOTS, srvs);
		else if (strcmp(stage, "VS") == 0)
			context->VSGetShaderResources(0, MAX_SLOTS, srvs);

		for (int i = 0; i < MAX_SLOTS; i++) {
			if (srvs[i]) {
				ID3D11Resource* res = nullptr;
				srvs[i]->GetResource(&res);
				if (res) {
					ID3D11Texture2D* tex = nullptr;
					if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
						D3D11_TEXTURE2D_DESC desc;
						tex->GetDesc(&desc);
						logger::info("[TAAReorder] {} {} SRV[{}]: tex=0x{:X} {}x{} fmt={} bind=0x{:X} arraySize={}",
							timing, stage, i, (uintptr_t)tex, desc.Width, desc.Height,
							(uint32_t)desc.Format, (uint32_t)desc.BindFlags, desc.ArraySize);
						tex->Release();
					} else {
						logger::info("[TAAReorder] {} {} SRV[{}]: non-Texture2D resource 0x{:X}",
							timing, stage, i, (uintptr_t)res);
					}
					res->Release();
				}
				srvs[i]->Release();
			}
		}
	}

	// ─── Diagnostic: dump bound RTVs and DSV ───
	static void DumpBoundRTVsAndDSV(ID3D11DeviceContext* context, const char* timing)
	{
		ID3D11RenderTargetView* rtvs[4] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(4, rtvs, &dsv);

		for (int i = 0; i < 4; i++) {
			if (rtvs[i]) {
				ID3D11Resource* res = nullptr;
				rtvs[i]->GetResource(&res);
				if (res) {
					ID3D11Texture2D* tex = nullptr;
					if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
						D3D11_TEXTURE2D_DESC desc;
						tex->GetDesc(&desc);
						logger::info("[TAAReorder] {} RTV[{}]: tex=0x{:X} {}x{} fmt={} bind=0x{:X}",
							timing, i, (uintptr_t)tex, desc.Width, desc.Height,
							(uint32_t)desc.Format, (uint32_t)desc.BindFlags);
						tex->Release();
					}
					res->Release();
				}
				rtvs[i]->Release();
			}
		}

		if (dsv) {
			ID3D11Resource* res = nullptr;
			dsv->GetResource(&res);
			if (res) {
				ID3D11Texture2D* tex = nullptr;
				if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
					D3D11_TEXTURE2D_DESC desc;
					tex->GetDesc(&desc);
					logger::info("[TAAReorder] {} DSV: tex=0x{:X} {}x{} fmt={} bind=0x{:X}",
						timing, (uintptr_t)tex, desc.Width, desc.Height,
						(uint32_t)desc.Format, (uint32_t)desc.BindFlags);
					tex->Release();
				}
				res->Release();
			}
			dsv->Release();
		}
	}

	// ─── Diagnostic: dump viewport state ───
	static void DumpViewport(ID3D11DeviceContext* context, const char* timing)
	{
		D3D11_VIEWPORT vps[4] = {};
		UINT numVPs = 4;
		context->RSGetViewports(&numVPs, vps);
		for (UINT i = 0; i < numVPs; i++) {
			logger::info("[TAAReorder] {} VP[{}]: x={:.0f} y={:.0f} w={:.0f} h={:.0f} minD={:.2f} maxD={:.2f}",
				timing, i, vps[i].TopLeftX, vps[i].TopLeftY, vps[i].Width, vps[i].Height,
				vps[i].MinDepth, vps[i].MaxDepth);
		}
	}

	// ─── Ensure pre-TAA-pass snapshot texture exists and matches ───
	static void EnsurePreTAAPassSnapshot(ID3D11Texture2D* srcTex)
	{
		D3D11_TEXTURE2D_DESC srcDesc;
		srcTex->GetDesc(&srcDesc);

		bool needCreate = !g_preTAAPassSnapshot;
		if (g_preTAAPassSnapshot) {
			D3D11_TEXTURE2D_DESC existingDesc;
			g_preTAAPassSnapshot->GetDesc(&existingDesc);
			if (existingDesc.Width != srcDesc.Width || existingDesc.Height != srcDesc.Height ||
				existingDesc.Format != srcDesc.Format)
				needCreate = true;
		}

		if (needCreate) {
			g_preTAAPassSnapshot = nullptr;
			g_preTAAPassSnapshotSRV = nullptr;

			D3D11_TEXTURE2D_DESC copyDesc = {};
			copyDesc.Width = srcDesc.Width;
			copyDesc.Height = srcDesc.Height;
			copyDesc.MipLevels = 1;
			copyDesc.ArraySize = 1;
			copyDesc.Format = srcDesc.Format;
			copyDesc.SampleDesc.Count = 1;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

			auto hr = globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, g_preTAAPassSnapshot.put());
			if (FAILED(hr))
				return;

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = copyDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			hr = globals::d3d::device->CreateShaderResourceView(g_preTAAPassSnapshot.get(), &srvDesc, g_preTAAPassSnapshotSRV.put());
			if (FAILED(hr)) {
				g_preTAAPassSnapshot = nullptr;
				return;
			}

			logger::info("[TAAReorder] Created pre-TAA-pass snapshot texture {}x{} fmt={}",
				copyDesc.Width, copyDesc.Height, (uint32_t)copyDesc.Format);
		}
	}

	// ─── ExecutePass hook: state machine for conductor interposition ───
	void ExecutePassHook::thunk(void* manager, void* passObj, int srcTech, int dstTech, void* extraData, uint8_t flag)
	{
		switch (g_hookPhase) {
		case HookPhase::FirstFunc:
		{
			// DIAGNOSTIC: For the TAA pass, snapshot the output RT BEFORE the pass runs.
			// This tells us if the mask is already there before TAA processes it.
			if (dstTech == TAA_PASS_DST) {
				auto context = globals::d3d::context;

				// Get the current RTV[0] texture (the TAA output RT)
				ID3D11RenderTargetView* rtv = nullptr;
				context->OMGetRenderTargets(1, &rtv, nullptr);
				if (rtv) {
					ID3D11Resource* res = nullptr;
					rtv->GetResource(&res);
					if (res) {
						ID3D11Texture2D* rtvTex = nullptr;
						if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rtvTex))) {
							EnsurePreTAAPassSnapshot(rtvTex);
							if (g_preTAAPassSnapshot) {
								context->CopyResource(g_preTAAPassSnapshot.get(), rtvTex);
								if (g_diagCounter == 0) {
									D3D11_TEXTURE2D_DESC desc;
									rtvTex->GetDesc(&desc);
									logger::info("[TAAReorder] Saved BEFORE-TAA RT snapshot: tex=0x{:X} {}x{} fmt={}",
										(uintptr_t)rtvTex, desc.Width, desc.Height, (uint32_t)desc.Format);
								}
							}
							rtvTex->Release();
						}
						res->Release();
					}
					rtv->Release();
				}

				// Also log pipeline state
				if (g_diagCounter == 0) {
					logger::info("[TAAReorder] ===== PRE-TAA PASS (dst=0x{:X}) PIPELINE STATE =====", dstTech);
					DumpBoundSRVs(context, "PS", "PRE-TAA");
					DumpBoundRTVsAndDSV(context, "PRE-TAA");
					DumpViewport(context, "PRE-TAA");
				}
			}

			// Execute pass normally (PP passes + TAA all execute).
			func(manager, passObj, srcTech, dstTech, extraData, flag);
			g_phase1PassCount++;

			if (dstTech == TAA_PASS_DST) {
				// DIAGNOSTIC: Dump full pipeline state AFTER the TAA pass
				if (g_diagCounter == 0) {
					auto context = globals::d3d::context;
					logger::info("[TAAReorder] ===== POST-TAA PASS (dst=0x{:X}) PIPELINE STATE =====", dstTech);
					DumpBoundSRVs(context, "PS", "POST-TAA");
					DumpBoundRTVsAndDSV(context, "POST-TAA");
					DumpViewport(context, "POST-TAA");
				}

				// TAA pass just completed. Capture its output RT for later compositing.
				CaptureRTV0();

				// Capture AFTER-TAA snapshot
				if (g_capturedTAAOutput) {
					EnsurePreDRSTAACopy(g_capturedTAAOutput);
					if (g_preDRSTAACopy) {
						auto context = globals::d3d::context;
						context->CopyResource(g_preDRSTAACopy.get(), g_capturedTAAOutput);

						if (g_diagCounter == 0) {
							D3D11_TEXTURE2D_DESC desc;
							g_preDRSTAACopy->GetDesc(&desc);
							logger::info("[TAAReorder] Saved AFTER-TAA RT snapshot {}x{} (inside conductor after TAA pass)",
								desc.Width, desc.Height);
						}
					}
				}

				if (g_diagCounter == 0)
					logger::info("[TAAReorder] TAA pass done (src=0x{:X} dst=0x{:X}), passCount={}, TAA output captured",
						srcTech, dstTech, g_phase1PassCount);
			} else {
				// Non-TAA pass: capture output as PP candidate.
				CapturePPOutput();

				if (g_diagCounter == 0) {
					auto context = globals::d3d::context;
					ID3D11RenderTargetView* rtv = nullptr;
					context->OMGetRenderTargets(1, &rtv, nullptr);
					if (rtv) {
						ID3D11Resource* res = nullptr;
						rtv->GetResource(&res);
						if (res) {
							ID3D11Texture2D* tex = nullptr;
							if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
								D3D11_TEXTURE2D_DESC desc;
								tex->GetDesc(&desc);
								logger::info("[TAAReorder] Pass #{} (src=0x{:X} dst=0x{:X}) output: tex=0x{:X} {}x{} fmt={}",
									g_phase1PassCount, srcTech, dstTech, (uintptr_t)tex,
									desc.Width, desc.Height, (uint32_t)desc.Format);
								tex->Release();
							}
							res->Release();
						}
						rtv->Release();
					}
				}
			}
			return;
		}

		default:
			// Inactive — normal pass-through
			func(manager, passObj, srcTech, dstTech, extraData, flag);
			return;
		}
	}

	// ─── Hidden area mesh render hook: skip late HAM calls ───
	// The HAM renderer draws black triangles to both depth and color.
	// Early calls (before DRS) are fine — DRS properly upscales them.
	// Late calls (after CompositeAfterConductor, when g_dlssOutputReady is true)
	// render at render-res coordinates on the display-res submit texture,
	// creating a mismatched mask at ~66.7% scale. We skip those.
	void HiddenAreaMeshHook::thunk(void* rendererState, uint8_t mode)
	{
		// HAM calls all happen at frame start (before conductor), not after DRS.
		// Confirmed via timing: hookPhase=Inactive, dlssOutputReady=false, 4 calls/frame.
		// The "mask at 66.7%" is the viewport boundary, not the HAM overlay.
		// Keep this hook for potential future use (skip late calls if timing changes).
		if (ShouldReorderTAA() && g_dlssOutputReady) {
			return;  // Skip any late HAM calls (safety net)
		}

		func(rendererState, mode);
	}

	// ─── BSOpenVR::Submit hook: pass-through (compositing done in CompositeAfterConductor) ───
	// Compositing now happens in CompositeAfterConductor (before the game draws UI).
	// The TAA RT already has: clean TAA periphery + DLSS center + game-drawn UI.
	// SubmitHook just passes through and handles diagnostics/cleanup.
	void SubmitHook::thunk(void* thisPtr, void* textureHandle)
	{
		// Diagnostic logging (rate-limited)
		if (g_diagCounter == 0 && textureHandle) {
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto& mainTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
			void* kMainTex = mainTexture.texture;

			D3D11_TEXTURE2D_DESC desc = {};
			auto tex2d = static_cast<ID3D11Texture2D*>(textureHandle);
			if (tex2d)
				tex2d->GetDesc(&desc);

			logger::info("[TAAReorder] Submit: tex=0x{:X} {}x{} fmt={} | kMAIN=0x{:X} match={} | dlssReady={}",
				(uintptr_t)textureHandle, desc.Width, desc.Height, (uint32_t)desc.Format,
				(uintptr_t)kMainTex, (textureHandle == kMainTex), g_dlssOutputReady);
		}

		// Reset flag (compositing already done in CompositeAfterConductor)
		if (g_dlssOutputReady)
			g_dlssOutputReady = false;

		func(thisPtr, textureHandle);
	}


	void Init()
	{
		auto base = REL::Module::get().base();

		g_pRendererSingleton = reinterpret_cast<uintptr_t*>(base + 0x034234C0);

		ForceTAASetter::func = base + 0x005C8EE0;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&ForceTAASetter::func), reinterpret_cast<PVOID>(ForceTAASetter::thunk));
		DetourTransactionCommit();

		TAAStateMachine::func = base + 0x005C8F10;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&TAAStateMachine::func), reinterpret_cast<PVOID>(TAAStateMachine::thunk));
		DetourTransactionCommit();

		ExecutePassHook::func = reinterpret_cast<ExecutePass_t>(base + 0x012D2540);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&ExecutePassHook::func), reinterpret_cast<PVOID>(ExecutePassHook::thunk));
		DetourTransactionCommit();

		HiddenAreaMeshHook::func = reinterpret_cast<HiddenAreaMeshRender_t>(base + 0x00DC2980);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&HiddenAreaMeshHook::func), reinterpret_cast<PVOID>(HiddenAreaMeshHook::thunk));
		DetourTransactionCommit();

		SubmitHook::func = reinterpret_cast<BSOpenVRSubmit_t>(base + 0x00C53920);
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&SubmitHook::func), reinterpret_cast<PVOID>(SubmitHook::thunk));
		DetourTransactionCommit();

		g_initialized = true;

		logger::info("[TAAReorder] Initialized — base=0x{:X}", base);
		logger::info("[TAAReorder] TAA_PASS_DST=0x{:X}", TAA_PASS_DST);
		logger::info("[TAAReorder] HiddenAreaMesh renderer hooked at RVA 0x00DC2980");
		logger::info("[TAAReorder] BSOpenVR::Submit hooked at RVA 0x00C53920");
	}
}
