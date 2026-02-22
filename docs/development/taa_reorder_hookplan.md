# TAA Reordering Hook Plan — SkyrimVR Hybrid DLSS

## Objective

Reorder TAA to execute **before** DLSS rather than after, enabling a hybrid VR upscaling strategy: DLSS upscales the center of each eye's view while the periphery uses cheap bicubic upscaling with TAA applied to reduce aliasing artifacts.

Image Base: `0x7FF7FA650000`

---

## Architecture Overview

The post-processing pipeline follows this call chain:

```
DLSS dispatch (pre-post-processing)
  ↓
sub_7FF7FB974E80 (Post-Processing Preamble)
  ├── Sets up projection/camera constants, G-buffer SRV bindings
  ├── Configures stereo VR paths
  ├── Calls sub_7FF7FB922300 (pre-post-processing flush)
  └── Calls sub_7FF7FB921C50 (Post-Processing Pass Dispatcher)
        ├── Phase 1: Main pass loop (indices 0..N from pass array)
        ├── Phase 2: TAA block (gated by byte [manager+0x239])  ← CURRENT TAA LOCATION
        ├── Phase 3: Secondary pass loop (indices 3-7)
        └── Phase 4: Final resolve pass (technique 0x0E)
```

**Goal**: Move TAA to fire before DLSS, then suppress the vanilla TAA call.

```
TAA dispatch (NEW — early hook)    ← DESIRED TAA LOCATION
  ↓
DLSS dispatch (pre-post-processing)
  ↓
sub_7FF7FB974E80 (Post-Processing Preamble)
  └── sub_7FF7FB921C50 (Dispatcher)
        ├── Phase 1: Main pass loop
        ├── Phase 2: TAA block — SUPPRESSED, skipped
        ├── Phase 3: Secondary pass loop
        └── Phase 4: Final resolve
```

---

## Confirmed Runtime Values

All values confirmed via runtime breakpoints (consistent across multiple frames):

| Value | Decimal | Hex | Source |
|-------|---------|-----|--------|
| Source technique ID (srcTechId) | 41 | `0x29` | `esi` at TAA executor call |
| Destination technique ID (TAA) | 75 | `0x4B` | Hardcoded in dispatcher |
| Initial frame orchestrator value | 114 | `0x72` | `r14d` before preamble call |
| TLS render phase ID | 33 | `0x21` | Hardcoded in all TAA-related blocks |
| TAA sampler mode | 8 | `0x08` | Hardcoded in TAA block |

The source technique ID transforms from `0x72` → `0x29` through the dispatcher's pass loop swap logic. At `0x7FF7FB921DFA` a default of `r12d = 0x29` is loaded, and swaps cause it to land in `esi`. For the early hook, use `0x29` directly.

---

## Part 1: Suppressing Vanilla TAA

### How the Flag Gets Set

Two functions (and ONLY two — confirmed via byte search `C6 8? 39 02 00 00 01`) write `[singleton+0x239] = 1`:

#### Setter A: sub_7FF7FAC18EE0 — "Force TAA" (RVA `0x005C8EE0`)

Unconditional. No checks. Called via vtable/function pointer (no direct CODE XREF).

```asm
mov  rax, cs:qword_7FF7FDA734C0     ; load renderer singleton
xor  ecx, ecx
mov  cs:dword_7FF7FD64D768, ecx     ; clear TAA state flag = 0
mov  byte ptr [rax+239h], 1         ; ← THE WRITE TO SUPPRESS
mov  [rax+240h], rcx                ; frame counter = 0
ret
```

#### Setter B: sub_7FF7FAC18F10 — "TAA State Machine" (RVA `0x005C8F10`)

Conditional. Called from frame orchestration at `sub_7FF7FAC08FA1`. Can both enable AND disable TAA:

```
If dword_7FF7FD64D768 != 0:
  Call sub_7FF7FB922E70(singleton)     ; readiness check
  If returns false:
    dword_7FF7FD64D768 = 0
    [singleton+0x239] = 1              ; ← ENABLE (at 0x7FF7FAC18F61)
    [singleton+0x240] = 0

If dword_7FF7FD64D76C == 0:
  Call sub_7FF7FB922E50(singleton)     ; alternate check
  If returns true:
    dword_7FF7FD64D76C = 1
    [singleton+0x239] = 0              ; ← DISABLE (at 0x7FF7FAC18F8D)
    [singleton+0x240] = 0
```

### Suppression Strategy

Hook both setters to prevent `[singleton+0x239]` from ever being set to 1. The dispatcher's TAA block checks this flag at `0x7FF7FB921F23` and will naturally skip if it's 0.

#### Hook Implementation for Setter A (Force TAA)

```cpp
// Function pointer types
using ForceTAA_t = void(*)();

// Original function pointer (populated by hook framework)
ForceTAA_t Original_ForceTAA = nullptr;

// RVA: 0x005C8EE0
void Hooked_ForceTAA() {
    auto singleton = *reinterpret_cast<uintptr_t*>(imageBase + 0x034234C0);

    // Preserve companion state (other code may depend on these)
    *reinterpret_cast<uint32_t*>(imageBase + 0x02FFD768) = 0;  // TAA state flag
    *reinterpret_cast<uint64_t*>(singleton + 0x240) = 0;        // frame counter

    // DO NOT set [singleton+0x239] = 1
    // Instead, set our own flag so the early hook knows TAA was requested
    g_TAARequestedThisFrame = true;
}
```

#### Hook Implementation for Setter B (State Machine)

```cpp
using TAAStateMachine_t = void(*)();
TAAStateMachine_t Original_TAAStateMachine = nullptr;

// RVA: 0x005C8F10
void Hooked_TAAStateMachine() {
    // Call original — let it do all its state machine logic
    Original_TAAStateMachine();

    auto singleton = *reinterpret_cast<uintptr_t*>(imageBase + 0x034234C0);
    uint8_t flagValue = *reinterpret_cast<uint8_t*>(singleton + 0x239);

    if (flagValue == 1) {
        // Original just enabled TAA — suppress it, redirect to our flag
        *reinterpret_cast<uint8_t*>(singleton + 0x239) = 0;
        g_TAARequestedThisFrame = true;
    }
    // If flagValue == 0, the state machine disabled TAA — respect that decision
    // g_TAARequestedThisFrame stays false, so ExecuteTAAEarly() will skip
}
```

#### Verification

After implementing suppression, you should see:
- No TAA applied (aliased edges visible)
- No crashes or rendering glitches
- The rest of the post-processing pipeline runs normally

---

## Part 2: Early TAA Injection

### Hook Location

Hook immediately **before** the DLSS dispatch point (pre-post-processing). The exact address depends on your DLSS integration's hook location.

### Shared State

```cpp
// Flag set by suppression hooks, consumed by early TAA hook
static bool g_TAARequestedThisFrame = false;
```

### Function Pointer Typedefs

```cpp
// All addresses are imageBase + RVA
using BindTechnique_t    = void(*)(void* techState, int techId, int param1, int param2);
using SetTAAConstants_t  = void(*)(void* stateObj, float xmm1, float xmm2,
                                   float xmm3, float jitter);
                           // All 4 params are single floats. xmm1-3 zeroed for TAA.
                           // jitter is the 5th param passed on stack at [rsp+28h].
                           // Confirmed by examining sub_7FF7FB40C2A0 prologue:
                           //   reads xmm1/xmm2/xmm3 as movss (single float)
                           //   reads [rsp+arg_20] as movss (single float)
using CommitState_t      = void(*)(void* stateObj, int param);
using CleanupState_t     = void(*)(void* stateObj, int a, int b, int c);
using ExecutePass_t      = void(*)(void* manager, void* passObj, int srcTech, int dstTech,
                                   void* extraData, uint8_t flag);

// Resolved at init from imageBase + RVA
BindTechnique_t    BindTechnique;    // imageBase + 0x00DC7C20
SetTAAConstants_t  SetTAAConstants;  // imageBase + 0x00DBC2A0
CommitState_t      CommitState;      // imageBase + 0x00DBBE60
CleanupState_t     CleanupState;     // imageBase + 0x00DBB780
ExecutePass_t      ExecutePass;      // imageBase + 0x012D2540
```

### Pointer Resolution

```cpp
// Global data pointers — resolve once at initialization
uintptr_t* g_pRendererSingleton;   // imageBase + 0x034234C0
uintptr_t* g_pSecondarySingleton;  // imageBase + 0x03485B38
void*      g_pTechniqueState;      // imageBase + 0x03185600
void*      g_pStateObject;         // imageBase + 0x03181700
uint32_t*  g_pSamplerMode;        // imageBase + 0x03180E68
uint32_t*  g_pDirtyFlags;         // imageBase + 0x03180DB0
uint32_t*  g_pVRModeFlag;         // imageBase + 0x03186D28
uint32_t*  g_pGlobalFlagA;        // imageBase + 0x01ED63C4
uint32_t*  g_pTAAStateFlag;       // imageBase + 0x02FFD768
uint32_t*  g_pTAA_DD0;            // imageBase + 0x03180DD0
uint32_t*  g_pTAA_DF8;            // imageBase + 0x03180DF8
uint32_t*  g_pTAA_E00;            // imageBase + 0x03180E00
uint32_t*  g_pFrameCounter;       // imageBase + 0x03186C5C
uint32_t   g_TlsIndex;            // read from imageBase + 0x036F2F58

void InitTAAPointers(uintptr_t imageBase) {
    // Function pointers
    BindTechnique   = (BindTechnique_t)  (imageBase + 0x00DC7C20);
    SetTAAConstants = (SetTAAConstants_t)(imageBase + 0x00DBC2A0);
    CommitState     = (CommitState_t)    (imageBase + 0x00DBBE60);
    CleanupState    = (CleanupState_t)   (imageBase + 0x00DBB780);
    ExecutePass     = (ExecutePass_t)    (imageBase + 0x012D2540);

    // TLS Index (read the value, not the pointer)
    g_TlsIndex = *reinterpret_cast<uint32_t*>(imageBase + 0x036F2F58);

    // Global data
    g_pRendererSingleton  = (uintptr_t*)(imageBase + 0x034234C0);
    g_pSecondarySingleton = (uintptr_t*)(imageBase + 0x03485B38);
    g_pTechniqueState     = (void*)     (imageBase + 0x03185600);
    g_pStateObject        = (void*)     (imageBase + 0x03181700);
    g_pSamplerMode        = (uint32_t*) (imageBase + 0x03180E68);
    g_pDirtyFlags         = (uint32_t*) (imageBase + 0x03180DB0);
    g_pVRModeFlag         = (uint32_t*) (imageBase + 0x03186D28);
    g_pGlobalFlagA        = (uint32_t*) (imageBase + 0x01ED63C4);
    g_pTAAStateFlag       = (uint32_t*) (imageBase + 0x02FFD768);
    g_pTAA_DD0            = (uint32_t*) (imageBase + 0x03180DD0);
    g_pTAA_DF8            = (uint32_t*) (imageBase + 0x03180DF8);
    g_pTAA_E00            = (uint32_t*) (imageBase + 0x03180E00);
    g_pFrameCounter       = (uint32_t*) (imageBase + 0x03186C5C);
}
```

### Complete Early TAA Hook

```cpp
// Constants (all confirmed via runtime capture)
constexpr int TAA_SRC_TECHNIQUE  = 0x29;  // source technique ID (decimal 41)
constexpr int TAA_DST_TECHNIQUE  = 0x4B;  // TAA technique ID (decimal 75)
constexpr int TAA_SAMPLER_MODE   = 8;
constexpr int TAA_TLS_PHASE      = 0x21;
constexpr int TAA_TLS_OFFSET     = 0x768;

void ExecuteTAAEarly() {
    // ─── Gate: was TAA requested this frame? ───
    if (!g_TAARequestedThisFrame) return;
    g_TAARequestedThisFrame = false;

    uintptr_t singleton = *g_pRendererSingleton;
    uintptr_t manager   = singleton;  // manager IS the singleton (qword_7FF7FDA734C0)

    // ─── TLS Phase ID ───
    // Save current TLS phase and set to 0x21
    uintptr_t tlsArray = __readgsqword(0x58);
    uintptr_t tlsSlot  = *(uintptr_t*)(tlsArray + g_TlsIndex * 8) + TAA_TLS_OFFSET;
    uint32_t  savedTLS = *(uint32_t*)tlsSlot;
    *(uint32_t*)tlsSlot = TAA_TLS_PHASE;

    // ─── Step 1: Set companion state ───
    *g_pTAAStateFlag = 0;                                    // dword_7FF7FD64D768
    *(uint64_t*)(manager + 0x240) = 0;                       // frame counter

    // ─── Step 2: Save & set global flag ───
    uint32_t savedGlobalA = *g_pGlobalFlagA;
    *g_pGlobalFlagA = 1;

    // ─── Step 3: Bind TAA shader (technique 0x4B) ───
    BindTechnique(g_pTechniqueState, TAA_DST_TECHNIQUE, 0, 0);

    // ─── Step 4: Set sampler state ───
    if (*g_pSamplerMode != TAA_SAMPLER_MODE) {
        *g_pSamplerMode = TAA_SAMPLER_MODE;
        *g_pDirtyFlags |= 0x80;
    }

    // ─── Step 5: Set TAA constants (jitter/projection) ───
    //
    // sub_7FF7FB40C2A0 confirmed as taking 5 single-float parameters:
    //   rcx  = state object
    //   xmm1 = float (zeroed for TAA) → written to [stateObj+0x2EE8]
    //   xmm2 = float (zeroed for TAA) → written to [stateObj+0x2EEC]
    //   xmm3 = float (zeroed for TAA) → written to [stateObj+0x2EF0]
    //   [rsp+28h] = float (jitter)    → written to [stateObj+0x2EF4]
    //
    // The function also saves the PREVIOUS values from those offsets into
    // globals at dword_7FF7FD7CE7E8..F4 (prior frame data for temporal reprojection).
    //
    // The preamble loads xmm6 with movups from [viewData+0x1F0] (16 bytes),
    // but only the low float is passed via movss to the stack parameter.
    //
    uintptr_t secondarySingleton = *g_pSecondarySingleton;
    uintptr_t viewData = *(uintptr_t*)(secondarySingleton + 0x150);
    float jitterValue = *(float*)(viewData + 0x1F0);  // single float, NOT 16 bytes

    SetTAAConstants(g_pStateObject, 0.0f, 0.0f, 0.0f, jitterValue);
    CommitState(g_pStateObject, 0);

    // ─── Step 6: Clear pass dirty flag ───
    uintptr_t passArray = *(uintptr_t*)(manager + 0x28);
    uintptr_t taaPass   = *(uintptr_t*)(passArray + 0x110);
    if (*g_pVRModeFlag == 0 && taaPass != 0) {
        *(uint8_t*)(taaPass + 0x88) = 0;
    }

    // ─── Step 7: Render target ping-pong (history buffer swap) ───
    //
    // The TAA pass needs the history buffer swapped in as the active RT.
    // [singleton+0x48] = default RT (saved/restored)
    // [singleton+0x50] = current active RT (swapped with history)
    // [singleton+0x58] = byte flag: 1 = using alternate RT
    // [manager+0x60]   = TAA history buffer
    //
    uintptr_t savedRT = *(uintptr_t*)(singleton + 0x48);
    if (savedRT) {
        _InterlockedIncrement((volatile long*)(savedRT + 8));  // AddRef
    }

    *(uint8_t*)(singleton + 0x58) = 1;  // flag: using alternate RT

    // Swap [singleton+0x50] ↔ [manager+0x60]
    uintptr_t currentRT = *(uintptr_t*)(singleton + 0x50);
    uintptr_t historyRT = *(uintptr_t*)(manager + 0x60);

    if (currentRT != historyRT) {
        if (historyRT) {
            _InterlockedIncrement((volatile long*)(historyRT + 8));  // AddRef new
        }
        *(uintptr_t*)(singleton + 0x50) = historyRT;
        if (currentRT) {
            long prev = _InterlockedExchangeAdd((volatile long*)(currentRT + 8), -1);
            if (prev == 1) {
                // Release via vtable destructor: [currentRT]->vtable[1](currentRT)
                auto vtable = *(uintptr_t*)currentRT;
                auto destructor = *(void(**)(uintptr_t))(vtable + 8);
                destructor(currentRT);
            }
        }
    }

    // ─── Step 8: Execute TAA pass ───
    ExecutePass(
        (void*)manager,               // rcx: post-proc manager
        (void*)taaPass,                // rdx: TAA pass object
        TAA_SRC_TECHNIQUE,             // r8d: 0x29 (confirmed)
        TAA_DST_TECHNIQUE,             // r9d: 0x4B (confirmed)
        nullptr,                       // stack arg_20: no extra data
        0                              // stack arg_28: standard technique lookup
    );

    // ─── Step 9: Restore render target ───
    *(uint8_t*)(singleton + 0x58) = 0;  // clear alternate RT flag

    // Swap [singleton+0x50] back to savedRT
    currentRT = *(uintptr_t*)(singleton + 0x50);
    if (currentRT != savedRT) {
        if (savedRT) {
            _InterlockedIncrement((volatile long*)(savedRT + 8));
        }
        *(uintptr_t*)(singleton + 0x50) = savedRT;
        if (currentRT) {
            long prev = _InterlockedExchangeAdd((volatile long*)(currentRT + 8), -1);
            if (prev == 1) {
                auto vtable = *(uintptr_t*)currentRT;
                auto destructor = *(void(**)(uintptr_t))(vtable + 8);
                destructor(currentRT);
            }
        }
    }

    // ─── Step 10: Update global TAA state ───
    *g_pTAA_DD0    = TAA_SRC_TECHNIQUE;  // 0x29
    *g_pTAA_DF8    = 0xFFFFFFFF;         // -1
    *g_pTAA_E00    = 3;
    *g_pDirtyFlags |= 1;

    // ─── Step 11: Cleanup ───
    CleanupState(g_pStateObject, 0, 0, 0);
    *(uint32_t*)(manager + 0x240) = *g_pFrameCounter;  // store frame counter
    *g_pGlobalFlagA = savedGlobalA;                     // restore saved global

    // Release savedRT
    if (savedRT) {
        long prev = _InterlockedExchangeAdd((volatile long*)(savedRT + 8), -1);
        if (prev == 1) {
            auto vtable = *(uintptr_t*)savedRT;
            auto destructor = *(void(**)(uintptr_t))(vtable + 8);
            destructor(savedRT);
        }
    }

    // ─── Restore TLS ───
    *(uint32_t*)tlsSlot = savedTLS;
}
```

---

## Part 3: Integration with DLSS Hook

The complete per-frame flow in your hook:

```cpp
// Called from your DLSS hook, immediately BEFORE DLSS dispatch
void PreDLSSHook() {
    // Run TAA first (if requested by the game this frame)
    ExecuteTAAEarly();

    // Then DLSS runs:
    //   - Center region: DLSS upscale (receives TAA'd input)
    //   - Periphery: bicubic upscale (also receives TAA'd input)
}
```

The setter hooks fire during frame orchestration (before DLSS), setting
`g_TAARequestedThisFrame = true`. When your pre-DLSS hook fires,
`ExecuteTAAEarly()` sees the flag, runs TAA, and clears it. Later, the
dispatcher's Phase 2 checks `[singleton+0x239]` which is 0, and skips
the vanilla TAA entirely.

---

## Complete RVA Reference

Image Base: `0x7FF7FA650000`

### TAA Flag Setters

| What | IDA Address | RVA | Notes |
|------|-------------|-----|-------|
| Force TAA — entry | `0x7FF7FAC18EE0` | `0x005C8EE0` | **Hook this** |
| Force TAA — flag write | `0x7FF7FAC18EEF` | `0x005C8EEF` | 7 bytes: `C6 80 39 02 00 00 01` |
| State machine — entry | `0x7FF7FAC18F10` | `0x005C8F10` | **Hook this** |
| State machine — flag enable | `0x7FF7FAC18F61` | `0x005C8F61` | 7 bytes: `C6 80 39 02 00 00 01` |
| State machine — flag disable | `0x7FF7FAC18F8D` | `0x005C8F8D` | **Leave intact** (TAA off path) |
| State machine — caller | `0x7FF7FAC08FA1` | `0x005B8FA1` | Frame orchestration |

### TAA Block in Dispatcher

| What | IDA Address | RVA | Notes |
|------|-------------|-----|-------|
| TAA block — 0x239 check | `0x7FF7FB921F23` | `0x12D1F23` | `cmp byte ptr [rbp+239h], 0` |
| TAA block — jz skip | `0x7FF7FB921F2A` | `0x12D1F2A` | Fallback: patch to unconditional jmp |
| TAA block — flag clear | `0x7FF7FB921F30` | `0x12D1F30` | `mov byte ptr [rbp+239h], 0` |
| TAA block — executor call | `0x7FF7FB92205B` | `0x12D205B` | `call sub_7FF7FB922540` |
| TAA block — end | `0x7FF7FB922136` | `0x12D2136` | After RT release |

### Pipeline Functions

| Function | IDA Address | RVA |
|----------|-------------|-----|
| Post-proc preamble | `0x7FF7FB974E80` | `0x1324E80` |
| Pass dispatcher | `0x7FF7FB921C50` | `0x12D1C50` |
| Pass executor | `0x7FF7FB922540` | `0x12D2540` |
| Bind technique | `0x7FF7FB417C20` | `0x00DC7C20` |
| Set TAA constants | `0x7FF7FB40C2A0` | `0x00DBC2A0` |
| Commit state | `0x7FF7FB40BE60` | `0x00DBBE60` |
| State cleanup | `0x7FF7FB40B780` | `0x00DBB780` |
| Resource binder | `0x7FF7FB98ECB0` | `0x0133ECB0` |
| Technique lookup | `0x7FF7FB98F880` | `0x0133F880` |
| Technique lookup (alt) | `0x7FF7FB98F8D0` | `0x0133F8D0` |
| TAA readiness check A | `0x7FF7FB922E70` | `0x012D2E70` |
| TAA readiness check B | `0x7FF7FB922E50` | `0x012D2E50` |
| Force TAA setter | `0x7FF7FAC18EE0` | `0x005C8EE0` |
| TAA state machine | `0x7FF7FAC18F10` | `0x005C8F10` |

### Global Data

| Symbol | IDA Address | RVA | Used As |
|--------|-------------|-----|---------|
| Renderer singleton ptr | `0x7FF7FDA734C0` | `0x034234C0` | Manager object; `rbp` in dispatcher |
| Secondary singleton ptr | `0x7FF7FDAD5B38` | `0x03485B38` | Camera/view data; jitter at `[+0x150]+0x1F0` |
| Technique state | `0x7FF7FD7D5600` | `0x03185600` | `rcx` for BindTechnique |
| Render state object | `0x7FF7FD7D1700` | `0x03181700` | `rcx` for SetTAAConstants, CommitState, CleanupState |
| Sampler mode | `0x7FF7FD7D0E68` | `0x03180E68` | Set to `8` for TAA |
| Dirty flags | `0x7FF7FD7D0DB0` | `0x03180DB0` | `|= 0x80` for sampler, `|= 1` for RT state |
| TAA RT tracking A | `0x7FF7FD7D0DD0` | `0x03180DD0` | Set to `0x29` (srcTechId) after TAA |
| TAA RT tracking B | `0x7FF7FD7D0DF8` | `0x03180DF8` | Set to `-1` after TAA |
| TAA RT tracking C | `0x7FF7FD7D0E00` | `0x03180E00` | Set to `3` after TAA |
| Frame counter | `0x7FF7FD7D6C5C` | `0x03186C5C` | Read into `[manager+0x240]` after TAA |
| VR mode flag | `0x7FF7FD7D6D28` | `0x03186D28` | Checked before clearing pass dirty flag |
| Global flag A | `0x7FF7FC5263C4` | `0x01ED63C4` | Saved/restored around TAA; set to 1 during |
| TAA state flag | `0x7FF7FD64D768` | `0x02FFD768` | Cleared by setters; checked by state machine |
| TAA state counter | `0x7FF7FD64D76C` | `0x02FFD76C` | State machine secondary flag |
| TLS Index | `0x7FF7FDD42F58` | `0x036F2F58` | Read value at init into g_TlsIndex |
| Condition check vtable | `0x7FF7FC5D3200` | `0x01F83200` | Indirect ptr used by state machine |
| TLS Index | `0x7FF7FDD42F58` | `0x036F2F58` | Thread Local Storage index; read value at init |

---

## Implementation Order

### Step 1 — Hook Both Setters (Suppression)

Hook `sub_7FF7FAC18EE0` and `sub_7FF7FAC18F10` as described in Part 1. Both hooks redirect the TAA request to `g_TAARequestedThisFrame` instead of writing `[singleton+0x239]`.

**Verify**: Run the game. You should see aliased edges (no TAA) and no crashes. The rest of post-processing should work normally.

### Step 2 — Implement Early TAA Hook

Add `ExecuteTAAEarly()` call immediately before your DLSS dispatch.

**Verify**: TAA should now be visible again, but running before DLSS. Check for:
- Correct anti-aliasing on edges
- No double-TAA artifacts (confirms suppression is working)
- Correct jitter/temporal stability

### Step 3 — Validate Render Targets

Ensure the TAA output flows correctly to both paths:
- Center → DLSS upscale
- Periphery → bicubic upscale

### Step 4 — Test VR Stereo

The dispatcher has VR-specific conditional paths. Verify both eyes receive TAA correctly.

### Step 5 — Edge Case: Dynamic TAA Toggle

The state machine's disable path (`[singleton+0x239] = 0` at `0x7FF7FAC18F8D`) should still work. When the state machine clears the flag, it won't set `g_TAARequestedThisFrame`, so `ExecuteTAAEarly()` will skip. Leave this path intact.

---

## Cautions & Notes

### Jitter Data Availability

The early hook fetches the jitter float directly from `[secondarySingleton+0x150]+0x1F0`. This data must be valid at the time of your hook. If DLSS fires very early in the frame before camera data is updated, this pointer chain may not yet be populated. Add a safety check:

```cpp
uintptr_t secondarySingleton = *g_pSecondarySingleton;
if (!secondarySingleton) return;  // too early
uintptr_t viewData = *(uintptr_t*)(secondarySingleton + 0x150);
if (!viewData) return;  // camera data not ready
float jitterValue = *(float*)(viewData + 0x1F0);
```

### SetTAAConstants Calling Convention (CONFIRMED)

All parameters are single floats. Verified from `sub_7FF7FB40C2A0` prologue:

```
Signature: void SetTAAConstants(void* stateObj, float xmm1, float xmm2, float xmm3, float jitter)

Function behavior:
  1. SAVES current [stateObj+0x2EE8..0x2EF4] → globals at dword_7FF7FD7CE7E8..F4
     (previous frame values for temporal reprojection)
  2. WRITES new values:
     xmm1           → [stateObj+0x2EE8]
     xmm2           → [stateObj+0x2EEC]
     xmm3 (via xmm5)→ [stateObj+0x2EF0]
     [rsp+28h]       → [stateObj+0x2EF4]   ← jitter float
```

The caller passes the jitter as `movss [rsp+var], xmm6` — only the low float of xmm6, not the full 16-byte vector. The early hook reads this as `*(float*)(viewData + 0x1F0)`.

### Render Target Refcounting

The ping-pong code uses `lock inc` / `lock xadd` for thread-safe refcount management. Getting this wrong causes use-after-free crashes. The implementation mirrors the original assembly exactly. Test thoroughly.

### TLS Index (CONFIRMED)

TLS Index global located at `0x7FF7FDD42F58` (RVA `0x036F2F58`). Resolved in `InitTAAPointers()`:
```cpp
g_TlsIndex = *reinterpret_cast<uint32_t*>(imageBase + 0x036F2F58);
```
