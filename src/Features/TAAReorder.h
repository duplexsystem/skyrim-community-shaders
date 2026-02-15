#pragma once

// TAA Reordering for VR DLSS Viewport Scaling (PureDark-aligned approach)
//
// Flow:
//   1. Single func() call with TAA enabled
//   2. Conductor runs PP passes → TAA/DRS combined pass (dst=0x72)
//      - TAA pass does BOTH temporal AA AND DRS upscale to display-res
//   3. ExecutePassHook captures TAA output (display-res) + PP output
//      - g_preDRSTAACopy = clean display-res TAA snapshot (before post-TAA mask)
//   4. Conductor finishes; something after TAA draws a "mask" at render-res coords
//   5. After func() returns, CompositeAfterConductor():
//      a. Post-PP content → kMAIN (for matching PP level)
//      b. DLSS processes center sub-region → writes to kMAIN
//      c. DLSS output saved to g_dlssOutputCopy (not pasted yet!)
//      d. kMAIN restored from vrPreTAACopy
//   6. BSOpenVR::Submit called with TAA RT (NOT kMAIN) — SubmitHook fires:
//      a. 1:1 copy g_preDRSTAACopy → submit texture (clean periphery, no mask)
//      b. Paste DLSS center from g_dlssOutputCopy on top
//      c. Call original Submit (periphery = clean TAA, center = DLSS)
//
// Key finding: The TAA pass (dst=0x72) IS the DRS — it outputs display-res
// content directly. Something after it draws a mask at render-res coordinates.
// We overwrite the submit texture with the clean pre-mask TAA snapshot.
//
// All RVAs are VR-specific (SkyrimVR.exe).

#include <Windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <winrt/base.h>

struct Upscaling;

namespace TAAReorder
{
	// ─── Confirmed technique IDs (from runtime capture) ───
	// TAA is embedded in the dst=0x72 pass (not a separate TAA-specific pass).
	// When taaEnabled=true, the shader variant for dst=0x72 includes temporal blending.
	inline constexpr int TAA_PASS_DST = 0x72;  // The pass containing TAA (decimal 114)

	// ─── Function pointer types ───
	using ExecutePass_t = void (*)(void* manager, void* passObj, int srcTech, int dstTech, void* extraData, uint8_t flag);
	using BSOpenVRSubmit_t = void (*)(void* thisPtr, void* textureHandle);

	// ─── Resolved global data pointers ───
	inline uintptr_t* g_pRendererSingleton = nullptr;
	inline bool g_initialized = false;

	// ─── Diagnostics (rate-limited logging) ───
	inline int g_diagCounter = 0;
	inline constexpr int DIAG_INTERVAL = 300;


	// ─── ExecutePass hook (conductor interposition) ───
	// RVA: 0x012D2540 — called by the conductor for each render pass
	struct ExecutePassHook
	{
		static void thunk(void* manager, void* passObj, int srcTech, int dstTech, void* extraData, uint8_t flag);
		static inline ExecutePass_t func = nullptr;
	};

	// ─── State machine for conductor interception ───
	enum class HookPhase
	{
		Inactive,   // Normal pass-through
		FirstFunc,  // PP + TAA execute; captures TAA/PP output refs (no compositing)
	};

	inline HookPhase g_hookPhase = HookPhase::Inactive;
	inline int g_phase1PassCount = 0;  // Passes executed in func()

	// ─── Captured TAA output ───
	// After the dst=0x72 ExecutePass completes, we grab the RTV[0] texture + RTV.
	// The RTV is kept for the pixel shader composite (format conversion).
	// AddRef'd when captured, Released after compositing.
	inline ID3D11Texture2D* g_capturedTAAOutput = nullptr;
	inline ID3D11RenderTargetView* g_capturedTAARTV = nullptr;

	// ─── Captured post-PP output (pre-TAA) ───
	// After the last non-TAA pass, we grab the RTV[0] texture.
	// This is the post-PP content that DLSS uses as input (matches TAA's PP level).
	// AddRef'd when captured, Released after use.
	inline ID3D11Texture2D* g_capturedPPOutput = nullptr;

	// ─── Saved DLSS output (for deferred paste in SubmitHook) ───
	// After DLSS runs in CompositeAfterConductor, the DLSS output is copied here.
	// SubmitHook then pastes the center into the TAA RT after the game's DRS upscale.
	// This avoids double-magnification: game DRS stretches TAA periphery, then we
	// overlay the DLSS center at full display-res quality.
	inline winrt::com_ptr<ID3D11Texture2D> g_dlssOutputCopy;
	inline winrt::com_ptr<ID3D11ShaderResourceView> g_dlssOutputCopySRV;
	inline bool g_dlssOutputReady = false;

	// Saved layout info for SubmitHook to know where to paste
	inline float g_savedVpScale = 1.0f;
	inline float g_savedDynResW = 1.0f;  // render-res / display-res width ratio
	inline float g_savedDynResH = 1.0f;  // render-res / display-res height ratio
	inline uint32_t g_savedEyeW = 0;     // per-eye width at display-res
	inline uint32_t g_savedEyeH = 0;     // per-eye height at display-res

	// ─── TAA pass snapshots (for mask investigation) ───
	// g_preDRSTAACopy: captured RIGHT AFTER ExecutePass returns for dst=0x72
	// g_preTAAPassSnapshot: captured RIGHT BEFORE ExecutePass runs for dst=0x72
	// Comparing these two reveals whether the mask is created BY the TAA pass
	// or was already present in the RT before the pass ran.
	inline winrt::com_ptr<ID3D11Texture2D> g_preDRSTAACopy;
	inline winrt::com_ptr<ID3D11ShaderResourceView> g_preDRSTAACopySRV;
	inline winrt::com_ptr<ID3D11Texture2D> g_preTAAPassSnapshot;
	inline winrt::com_ptr<ID3D11ShaderResourceView> g_preTAAPassSnapshotSRV;
	inline bool g_showPreTAASnapshot = true;  // true=show BEFORE TAA, false=show AFTER TAA

	// ─── Setter hook: Setter A (Force TAA) ───
	// RVA: 0x005C8EE0 — unconditional TAA enable (pass-through)
	struct ForceTAASetter
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// ─── Setter hook: Setter B (TAA State Machine) ───
	// RVA: 0x005C8F10 — conditional TAA enable/disable (pass-through)
	struct TAAStateMachine
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// ─── Hidden area mesh render hook ───
	// RVA: 0x00DC2980 — sub_7FF7FB412980, the actual HAM renderer.
	// Checks bUseHiddenAreaMesh internally, sets up D3D pipeline (depth-stencil,
	// blend, vertex buffer), and draws HAM geometry as black triangles.
	// Writes to BOTH depth and color buffers.
	// Called from multiple sites; "late" calls (after DRS upscale) render at
	// render-res coordinates on the display-res submit texture = wrong scale.
	// We skip late calls (when g_dlssOutputReady is true) to prevent the
	// mismatched HAM overlay. The early HAM is properly DRS-upscaled.
	using HiddenAreaMeshRender_t = void (*)(void* rendererState, uint8_t mode);
	struct HiddenAreaMeshHook
	{
		static void thunk(void* rendererState, uint8_t mode);
		static inline HiddenAreaMeshRender_t func = nullptr;
	};

	// ─── BSOpenVR::Submit hook (VR frame submission interception) ───
	// RVA: 0x00C53920 — BSOpenVR::Submit, vtable[3]. Called for each frame.
	// Submits texture to IVRCompositor with hardcoded full half-texture bounds.
	// textureHandle is the raw ID3D11Texture2D* to be displayed.
	struct SubmitHook
	{
		static void thunk(void* thisPtr, void* textureHandle);
		static inline BSOpenVRSubmit_t func = nullptr;
	};

	// Check if TAA reordering should be active based on current settings
	bool ShouldReorderTAA();

	// Run DLSS and composite center into TAA output. Called after func() returns
	// (same timing as PureDark's BSImagespaceShader_Hook_VR — after conductor finishes).
	void CompositeAfterConductor();

	// Initialize all pointers and install hooks. Call once from PostPostLoad (VR only).
	void Init();
}
