# SkyrimVR Rendering Pipeline Analysis

**Image Base:** `0x7FF7FA650000`

---

## Table of Contents

1. Key Function Reference
2. Key Global Variables
3. Frame Master Pipeline (sub_7FF7FAC09330)
4. Frame Render Orchestrator (sub_7FF7FB974E80)
5. Post-Processing Conductor (sub_7FF7FB921C50)
6. RT Activation / SetRenderTarget (sub_7FF7FB97C530)
7. Command Buffer Submit (sub_7FF7FB9A01F0)
8. Render Target Pool Init (sub_7FF7FB975D10)
9. RT Registration Functions
10. Render Target Slot Map
11. Hook Point Recommendations

---

## 1. Key Function Reference

| Function | IDA Address | RVA | Purpose |
|----------|-------------|-----|---------|
| `sub_7FF7FAC09330` | `0x7FF7FAC09330` | `0x5B9330` | Frame Master — top-level frame orchestration |
| `sub_7FF7FB974E80` | `0x7FF7FB974E80` | `0x1324E80` | Frame Render Orchestrator — RT setup + scene + post-process |
| `sub_7FF7FB921C50` | `0x7FF7FB921C50` | `0x12D1C50` | Post-Processing Conductor — TAA + DRS + all post passes |
| `sub_7FF7FB922540` | `0x7FF7FB922540` | `0x12D2540` | Individual post-processing pass executor |
| `sub_7FF7FB97C530` | `0x7FF7FB97C530` | `0x132C530` | RT Activation — applies DRS scaling per-target |
| `sub_7FF7FB9A01F0` | `0x7FF7FB9A01F0` | `0x13501F0` | Deferred context command buffer submit |
| `sub_7FF7FB975D10` | `0x7FF7FB975D10` | `0x1325D10` | Render Target Pool Init — creates all RTs |
| `sub_7FF7FB417980` | `0x7FF7FB417980` | `0xDC7980` | Color RT registration |
| `sub_7FF7FB4179D0` | `0x7FF7FB4179D0` | `0xDC79D0` | Depth/Stencil RT registration |
| `sub_7FF7FB417B30` | `0x7FF7FB417B30` | `0xDC7B30` | SetRenderTarget (viewport/RT bind) |
| `sub_7FF7FB417B50` | `0x7FF7FB417B50` | `0xDC7B50` | Set stencil reference value |
| `sub_7FF7FB417C20` | `0x7FF7FB417C20` | `0xDC7C20` | Set depth/stencil target |
| `sub_7FF7FB40C2A0` | `0x7FF7FB40C2A0` | `0xDBC2A0` | Clear render targets |
| `sub_7FF7FB40BE60` | `0x7FF7FB40BE60` | `0xDBBE60` | Bind render state |
| `sub_7FF7FB40B780` | `0x7FF7FB40B780` | `0xDBB780` | Flush/commit render state |
| `sub_7FF7FB40C310` | `0x7FF7FB40C310` | `0xDBC310` | Finalize RT state |
| `sub_7FF7FB40C370` | `0x7FF7FB40C370` | `0xDBC370` | Copy/resolve RT |
| `sub_7FF7FB40C860` | `0x7FF7FB40C860` | `0xDBC860` | RT logging/validation (tail-call target) |
| `sub_7FF7FB417CB0` | `0x7FF7FB417CB0` | `0xDC7CB0` | Get stencil ref for RT |
| `sub_7FF7FB9C4650` | `0x7FF7FB9C4650` | `0x1374650` | Update shader constants for active RT |
| `sub_7FF7FAC98960` | `0x7FF7FAC98960` | `0x648960` | Scaleform Advance (UI logic tick) |
| `sub_7FF7FB56C5F0` | `0x7FF7FB56C5F0` | `0xF1C5F0` | Scaleform Display (UI render) |
| `sub_7FF7FB973EF0` | `0x7FF7FB973EF0` | `0x1323EF0` | Present / final flip |
| `sub_7FF7FB922300` | `0x7FF7FB922300` | `0x12D2300` | Scene draw (geometry + lighting) |
| `sub_7FF7FB922EB0` | `0x7FF7FB922EB0` | `0x12D2EB0` | Dynamic resolution resize |
| `sub_7FF7FB980A40` | `0x7FF7FB980A40` | `0x1330A40` | Resolve/tonemap setup |
| `sub_7FF7FB971D80` | `0x7FF7FB971D80` | `0x1321D80` | Shadow maps / pre-lighting |
| `sub_7FF7FB972130` | `0x7FF7FB972130` | `0x1322130` | Additional pre-pass |
| `sub_7FF7FB973250` | `0x7FF7FB973250` | `0x1323250` | Lighting resolve |
| `sub_7FF7FB973680` | `0x7FF7FB973680` | `0x1323680` | Environment / sky |
| `sub_7FF7FB973F80` | `0x7FF7FB973F80` | `0x1323F80` | Atmospheric effects |
| `sub_7FF7FB973B10` | `0x7FF7FB973B10` | `0x1323B10` | Final lighting pass |
| `sub_7FF7FB973F90` | `0x7FF7FB973F90` | `0x1323F90` | Particle finalize (alt path) |
| `sub_7FF7FB95B350` | `0x7FF7FB95B350` | `0x130B350` | Particle system check |
| `sub_7FF7FB95B650` | `0x7FF7FB95B650` | `0x130B650` | Particle system pass |
| `sub_7FF7FB9744E0` | `0x7FF7FB9744E0` | `0x13244E0` | Water/reflections or final effects |
| `sub_7FF7FB977660` | `0x7FF7FB977660` | `0x1327660` | Get depth stencil for current pass |
| `sub_7FF7FB97F420` | `0x7FF7FB97F420` | `0x132F420` | Conditional pre-render pass |
| `sub_7FF7FB978F20` | `0x7FF7FB978F20` | `0x1328F20` | VR stereo shadow/lighting pre-pass |
| `sub_7FF7FB97B3F0` | `0x7FF7FB97B3F0` | `0x132B3F0` | VR stereo second-eye pre-pass |
| `sub_7FF7FB96B240` | `0x7FF7FB96B240` | `0x131B240` | VR stereo rendering |
| `sub_7FF7FB41F900` | `0x7FF7FB41F900` | `0xDCF900` | Buffer swap |
| `sub_7FF7FB41F4F0` | `0x7FF7FB41F4F0` | `0xDCF4F0` | Buffer swap (variant) |
| `sub_7FF7FB9481D0` | `0x7FF7FB9481D0` | `0x12F81D0` | Finalize scene lists |
| `sub_7FF7FB9653A0` | `0x7FF7FB9653A0` | `0x13053A0` | Depth resolve setup |
| `sub_7FF7FB965450` | `0x7FF7FB965450` | `0x1305450` | Depth resolve cleanup |
| `sub_7FF7FB2C6C20` | `0x7FF7FB2C6C20` | `0xC76C20` | Job system: dispatch task |
| `sub_7FF7FB2C6D60` | `0x7FF7FB2C6D60` | `0xC76D60` | Job system: sync |
| `sub_7FF7FB2C6D80` | `0x7FF7FB2C6D80` | `0xC76D80` | Job system: barrier |
| `sub_7FF7FB2CD260` | `0x7FF7FB2CD260` | `0xC7D260` | Job system: sync by ID |
| `sub_7FF7FB9902F0` | `0x7FF7FB9902F0` | `0x13502F0` | Get critical section for context |
| `sub_7FF7FB9901E0` | `0x7FF7FB9901E0` | `0x13501E0` | Lookup/create pipeline state (0x5C006077) |
| `sub_7FF7FB999680` | `0x7FF7FB999680` | `0x1349680` | Execute command list |
| `sub_7FF7FB990280` | `0x7FF7FB990280` | `0x1350280` | Command list cleanup |
| `sub_7FF7FB9999B0` | `0x7FF7FB9999B0` | `0x13499B0` | Signal completion |

---

## 2. Key Global Variables

| Variable | IDA Address | RVA | Purpose |
|----------|-------------|-----|---------|
| `dword_7FF7FD7D5600` | `0x7FF7FD7D5600` | `0x3185600` | **RT Manager Object** (rcx for all RT calls) |
| `unk_7FF7FD7D1700` | `0x7FF7FD7D1700` | `0x3181700` | Render state object |
| `qword_7FF7FDA734C0` | `0x7FF7FDA734C0` | `0x34234C0` | Global render context |
| `qword_7FF7FD63B9B0` | `0x7FF7FD63B9B0` | `0x2FEB9B0` | D3D device / swapchain |
| `qword_7FF7FD63B9F0` | `0x7FF7FD63B9F0` | `0x2FEB9F0` | D3D device (secondary ref) |
| `qword_7FF7FDAD5B38` | `0x7FF7FDAD5B38` | `0x3485B38` | Frame/view data root |
| `qword_7FF7FDAD59D0` | `0x7FF7FDAD59D0` | `0x34859D0` | Camera/exposure data |
| `qword_7FF7FDAD5D00` | `0x7FF7FDAD5D00` | `0x3485D00` | Per-frame constant buffer |
| `qword_7FF7FDAD5AF0` | `0x7FF7FDAD5AF0` | `0x3485AF0` | VR multi-view context array |
| `qword_7FF7FDA72FD8` | `0x7FF7FDA72FD8` | `0x3422FD8` | Current frame view pointer |
| `qword_7FF7FD63BA40` | `0x7FF7FD63BA40` | `0x2FEBA40` | Job system handle |
| `off_7FF7FC5D3200` | `0x7FF7FC5D3200` | `0x2F83200` | Scaleform/UI manager |
| `off_7FF7FC5D5100` | `0x7FF7FC5D5100` | `0x2F85100` | UI manager (secondary) |
| `dword_7FF7FD7D6C34` | `0x7FF7FD7D6C34` | `0x3186C34` | Render width |
| `dword_7FF7FD7D6C38` | `0x7FF7FD7D6C38` | `0x3186C38` | Render height |
| `dword_7FF7FD7D6D14` | `0x7FF7FD7D6D14` | `0x3186D14` | DRS scale X |
| `dword_7FF7FD7D6D18` | `0x7FF7FD7D6D18` | `0x3186D18` | DRS scale Y |
| `dword_7FF7FD7D6D28` | `0x7FF7FD7D6D28` | `0x3186D28` | DRS mode (0=off) |
| `dword_7FF7FD7D0DB0` | `0x7FF7FD7D0DB0` | `0x3180DB0` | Render state dirty flags |
| `dword_7FF7FD7D0DB4` | `0x7FF7FD7D0DB4` | `0x3180DB4` | RT change dirty flag |
| `dword_7FF7FD7D0DB8` | `0x7FF7FD7D0DB8` | `0x3180DB8` | Stencil change dirty flag |
| `dword_7FF7FD7D0EB4` | `0x7FF7FD7D0EB4` | `0x3180EB4` | Depth stencil state tracking |
| `dword_7FF7FD7D0EF8` | `0x7FF7FD7D0EF8` | `0x3180EF8` | Current depth stencil view ptr |
| `dword_7FF7FD7D0E48` | `0x7FF7FD7D0E48` | `0x3180E48` | Stencil ref value |
| `dword_7FF7FD7D0E4C` | `0x7FF7FD7D0E4C` | `0x3180E4C` | Stencil ref mask |
| `dword_7FF7FD7D0E50` | `0x7FF7FD7D0E50` | `0x3180E50` | Depth write enable state |
| `dword_7FF7FD7D0E60` | `0x7FF7FD7D0E60` | `0x3180E60` | Depth stencil state ID |
| `dword_7FF7FDAD5BB8` | `0x7FF7FDAD5BB8` | `0x3485BB8` | Hierarchical-Z mip count |
| `dword_7FF7FC5246F8` | `0x7FF7FC5246F8` | `0x2BF46F8` | Dynamic resolution divisor (r14) |
| `dword_7FF7FC5263C4` | `0x7FF7FC5263C4` | `0x2BF63C4` | TAA state save/restore |
| `TlsIndex` | (varies) | — | Thread-local storage index |
| `byte_7FF7FDAD5BD0` | `0x7FF7FDAD5BD0` | `0x3485BD0` | DRS override enable |
| `dword_7FF7FC525A88` | `0x7FF7FC525A88` | `0x2BF5A88` | DRS override scale value |
| `byte_7FF7FC525A70` | `0x7FF7FC525A70` | `0x2BF5A70` | DRS override feature flag |
| `byte_7FF7FDAD5362` | `0x7FF7FDAD5362` | `0x3485362` | VR stereo rendering enabled |
| `byte_7FF7FDA72FC2` | `0x7FF7FDA72FC2` | `0x3422FC2` | Main render enabled |
| `byte_7FF7FC523CDC` | `0x7FF7FC523CDC` | `0x2BF3CDC` | Secondary render check |
| `byte_7FF7FDA73E18` | `0x7FF7FDA73E18` | `0x3423E18` | Motion vectors enabled |
| `byte_7FF7FD7D1708` | `0x7FF7FD7D1708` | `0x3181708` | Frame rendering flag |

### Feature Flags (RT Pool Init)

| Flag | IDA Address | RVA | Controls |
|------|-------------|-----|----------|
| `byte_7FF7FC525D80` | `0x7FF7FC525D80` | `0x2BF5D80` | Dynamic resolution scaling RTs (slots 0x44–0x4A) |
| `byte_7FF7FC525DE0` | `0x7FF7FC525DE0` | `0x2BF5DE0` | Half-res DRS variants |
| `byte_7FF7FC524740` | `0x7FF7FC524740` | `0x2BF4740` | General VR/feature flag (multi-use) |
| `byte_7FF7FC525F78` | `0x7FF7FC525F78` | `0x2BF5F78` | Temporal upscaling RTs (slots 0x56–0x57) |
| `byte_7FF7FC5263D8` | `0x7FF7FC5263D8` | `0x2BF63D8` | SSR cascade RTs (slots 0x4E–0x50) |
| `byte_7FF7FDAD5BE5` | `0x7FF7FDAD5BE5` | `0x3485BE5` | Half-res targets (0x2F/0x3F/0x41/0x43) |
| `byte_7FF7FC5249A4` | `0x7FF7FC5249A4` | `0x2BF49A4` | Alternate render path select |
| `byte_7FF7FC516FD8` | `0x7FF7FC516FD8` | `0x2BE6FD8` | Stencil ref management enable |
| `byte_7FF7FC4C01E0` | `0x7FF7FC4C01E0` | `0x2B901E0` | Depth mip chain conditional |
| `byte_7FF7FC5246E0` | `0x7FF7FC5246E0` | `0x2BF46E0` | RT format variant selector |
| `flt_7FF7FBC376AC` | `0x7FF7FBC376AC` | `0x15E76AC` | TAA aspect ratio threshold |

---

## 3. Frame Master Pipeline (sub_7FF7FAC09330)

**IDA:** `0x7FF7FAC09330` | **RVA:** `0x5B9330`

This is the top-level per-frame rendering function. Complete execution order:

### Phase 1 — Frame Setup
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 1 | `0xFAC0936D` | `0x5B936D` | `byte_7FF7FD7D1708 = 1` (frame begin) |
| 2 | `0xFAC093AF` | `0x5B93AF` | `sub_7FF7FB922EB0` — DRS resize (conditional) |
| 3 | `0xFAC093C0` | `0x5B93C0` | Swapchain vtable `+0x378` — query state |
| 4 | `0xFAC09420` | `0x5B9420` | VR multi-view context setup loop |

### Phase 2 — Scene Build
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 5 | `0xFAC094A7` | `0x5B94A7` | `"DrawWorld_BuildSceneLists"` job dispatch |
| 6 | `0xFAC094CA` | `0x5B94CA` | `sub_7FF7FAC09C00` — scene culling/sorting |
| 7 | `0xFAC094E5` | `0x5B94E5` | `sub_7FF7FB9481D0` — finalize scene lists |

### Phase 3 — Pre-Render
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 8 | `0xFAC0950E` | `0x5B950E` | `sub_7FF7FB971D80` — shadow maps / pre-lighting |
| 9 | `0xFAC0954C` | `0x5B954C` | `sub_7FF7FB41F900` — buffer swap (×2) |
| 10 | `0xFAC09573` | `0x5B9573` | `sub_7FF7FB41F4F0` — buffer swap |
| 11 | `0xFAC09578` | `0x5B9578` | `sub_7FF7FB972130` — additional pre-pass |
| 12 | `0xFAC095C1` | `0x5B95C1` | `sub_7FF7FB96B240` — VR stereo (conditional) |

### Phase 4 — Lighting & Environment
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 13 | `0xFAC0961E` | `0x5B961E` | `sub_7FF7FB973250` — lighting resolve |
| 14 | `0xFAC09625` | `0x5B9625` | `sub_7FF7FB973680` — environment / sky |
| 15 | `0xFAC0962A` | `0x5B962A` | `sub_7FF7FB973F80` — atmospheric effects |

### Phase 5 — Job Dispatch
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 16 | `0xFAC096EE` | `0x5B96EE` | `"Clear Lists"` + `"ClearListJobFunc"` jobs |

### Phase 6 — Camera/View Update
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 17 | `0xFAC097A6` | `0x5B97A6` | Copy camera positions, view matrices to globals |
| 18 | `0xFAC098BC` | `0x5B98BC` | `sub_7FF7FB971D10` — finalize view |

### Phase 7 — G-Buffer Clear & RT Bind
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 19 | `0xFAC09924` | `0x5B9924` | `sub_7FF7FB40C2A0` — clear RTs |
| 20 | `0xFAC09943` | `0x5B9943` | SetRenderTarget slot 0, depth=auto, stencil=3 |
| 21 | `0xFAC09976` | `0x5B9976` | SetRenderTarget slot 0, depth=1, stencil=3 |
| 22 | `0xFAC099A0` | `0x5B99A0` | SetRenderTarget slot 1, depth=7, stencil=3 |
| 23 | `0xFAC099C2` | `0x5B99C2` | SetRenderTarget slot 2, depth=auto, stencil=3 |
| 24 | `0xFAC099DD` | `0x5B99DD` | `sub_7FF7FB40C2A0` — second clear |
| 25 | `0xFAC09A20` | `0x5B9A20` | (Conditional) SetRenderTarget slot 0, depth=0x70 — motion vectors |
| 26 | `0xFAC09A70` | `0x5B9A70` | (Conditional) SetRenderTarget slot 3, depth=0x6D |

### Phase 8 — Pre-Scene Final
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 27 | `0xFAC09AC1` | `0x5B9AC1` | `sub_7FF7FB973B10` — final lighting |
| 28 | `0xFAC09ACD` | `0x5B9ACD` | Job system barrier sync |

### Phase 9 — Effects
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 29 | `0xFAC09AF3` | `0x5B9AF3` | `sub_7FF7FB95B350` — particle system check |
| 30 | `0xFAC09B0F` | `0x5B9B0F` | `sub_7FF7FB95B650` — particle pass 6 |
| 31 | `0xFAC09B20` | `0x5B9B20` | `sub_7FF7FB95B650` — particle pass 5 |
| 32 | `0xFAC09B25` | `0x5B9B25` | `sub_7FF7FAB8BEA0` — effect finalize |
| 33 | `0xFAC09B2A` | `0x5B9B2A` | `sub_7FF7FB9744E0` — water/reflections |

### Phase 10 — UI Advance + 3D Render + Post-Process
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 34 | `0xFAC09B3B` | `0x5B9B3B` | **`sub_7FF7FAC98960` — Scaleform Advance (UI logic tick)** |
| 35 | `0xFAC09B45` | `0x5B9B45` | `sub_7FF7FB2CD260(7)` — sync barrier |
| 36 | `0xFAC09B73` | `0x5B9B73` | `sub_7FF7FB97F420` — conditional pre-render (VR) |
| 37 | `0xFAC09B7B` | `0x5B9B7B` | **`sub_7FF7FB974E80` — 3D RENDER + POST-PROCESS + TAA + DRS** |

### Phase 11 — UI Render + Present
| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 38 | `0xFAC09B92` | `0x5B9B92` | Swapchain vtable `+0x90` — GetBackBufferSize |
| 39 | `0xFAC09BC4` | `0x5B9BC4` | **`sub_7FF7FB56C5F0` — Scaleform Display (UI RENDER)** — edx=0x72 (RT slot) |
| 40 | `0xFAC09BC9` | `0x5B9BC9` | **`sub_7FF7FB973EF0` — PRESENT / final flip** |

---

## 4. Frame Render Orchestrator (sub_7FF7FB974E80)

**IDA:** `0x7FF7FB974E80` | **RVA:** `0x1324E80`

Called from Frame Master at step 37. Handles the 3D rendering pipeline.

| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 1 | `0xFB974EA6` | `0x1324EA6` | `byte_7FF7FD7D1708 = 0` |
| 2 | `0xFB974EDD` | `0x1324EDD` | Load view/camera data, save xmm6 |
| 3 | `0xFB974F07` | `0x1324F07` | Copy exposure/lighting params |
| 4 | `0xFB974F51` | `0x1324F51` | SetRenderTarget slot 0 (G-buffer) |
| 5 | `0xFB974F70` | `0x1324F70` | SetRenderTarget slot 1 (G-buffer) |
| 6 | `0xFB974F8F` | `0x1324F8F` | SetRenderTarget slot 2 (G-buffer) |
| 7 | `0xFB974FAD` | `0x1324FAD` | SetRenderTarget slot 3 (G-buffer) |
| 8 | `0xFB974FC8` | `0x1324FC8` | Set stencil ref = -1 (disabled) |
| 9 | `0xFB974FD6` | `0x1324FD6` | `sub_7FF7FB40BE60` — bind render state |
| 10 | `0xFB974FF8` | `0x1324FF8` | (Conditional) VR stereo pre-passes with slots 0x2B/0x2C/0x2D |
| 11 | `0xFB97506A` | `0x132506A` | **`sub_7FF7FB922300` — Scene draw** |
| 12 | `0xFB975086` | `0x1325086` | **`sub_7FF7FB921C50` — Post-processing conductor** |
| 13 | `0xFB97508B` | `0x132508B` | Restore view matrix (xmm6) |

---

## 5. Post-Processing Conductor (sub_7FF7FB921C50)

**IDA:** `0x7FF7FB921C50` | **RVA:** `0x12D1C50`

Called from Frame Render Orchestrator at step 12.

### Parameters
- `rcx` = render pipeline object (`rbp`)
- `edx` = pass index
- `r8d` = target slot
- `r9b` = arg_20 (depth resolve flag)

### Execution Order

| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 1 | `0xFB921CC0` | `0x12D1CC0` | **Pre-pass loop** (0 to [rbp+40h]): calls `sub_7FF7FB922540` per pass |
| 2 | `0xFB921E3D` | `0x12D1E3D` | **Main scene pass** (path A: `byte_7FF7FC5249A4` set) via `+0xF0` |
| 2a | `0xFB921E80` | `0x12D1E80` | **Main scene pass** (path B: normal) via `+0x110` |
| 3 | `0xFB921F0C` | `0x12D1F0C` | `sub_7FF7FB980A40` — resolve/tonemap on `[rbp+218h]` |
| 4 | `0xFB921F5C` | `0x12D1F5C` | **TAA pass** — `sub_7FF7FB417C20` sets depth stencil=8 |
| 4a | `0xFB921F94` | `0x12D1F94` | `sub_7FF7FB40C2A0` — clear for TAA |
| 4b | `0xFB921FA2` | `0x12D1FA2` | `sub_7FF7FB40BE60` — bind for TAA |
| 4c | `0xFB92205B` | `0x12D205B` | **`sub_7FF7FB922540` with r9d=0x4B` — TAA render** |
| 4d | `0xFB9220A5` | `0x12D20A5` | `sub_7FF7FB40C310` — finalize TAA |
| 4e | `0xFB922109` | `0x12D2109` | `sub_7FF7FB40B780` — flush state |
| 5 | `0xFB922152` | `0x12D2152` | **Post-TAA pass loop** (passes 3–7) |
| 6 | `0xFB922225` | `0x12D2225` | **`sub_7FF7FB97C530` — DRS/RT activation** |
| 7 | `0xFB922273` | `0x12D2273` | **Depth resolve** — `sub_7FF7FB922540` with r8d=0x0E, via `+0x118` |

---

## 6. RT Activation / SetRenderTarget (sub_7FF7FB97C530)

**IDA:** `0x7FF7FB97C530` | **RVA:** `0x132C530`

Activates a render target slot with dynamic resolution scaling.

### Parameters
- `rcx` = render state object (enable at `+8`, scale at `+30h`)
- `edx` = RT slot index
- `r8d` = pass-through (returned as eax)

### Key Flow
1. Early-out if `byte [rcx+8] == 0` (system disabled)
2. Read scale from `[rcx+30h]`, clamp to [0.0, 1.0]
3. Override path: if `byte_7FF7FDAD5BD0` && `byte_7FF7FC525A70` → use `dword_7FF7FC525A88` instead
4. Read RT dimensions from `dword_7FF7FD7D5600 + slot*40`
5. Apply scale, compute viewport ratios
6. `sub_7FF7FB417B30` — bind the scaled RT
7. `sub_7FF7FB40B780` — flush render state
8. `sub_7FF7FB9C4650` — update shader constants
9. `sub_7FF7FB9A01F0` — submit command buffer (with edx=0 → default context)
10. Stencil ref management (conditional on `byte_7FF7FC516FD8`)

### Internal Call Sequence

| Step | IDA Address | RVA | Call/Action |
|------|-------------|-----|-------------|
| 1 | `0xFB97C75E` | `0x132C75E` | `sub_7FF7FB417B30` — SetRenderTarget (viewport/bind) |
| 2 | `0xFB97C811` | `0x132C811` | `sub_7FF7FB40B780` — flush render state |
| 3 | `0xFB97C81D` | `0x132C81D` | `sub_7FF7FB9C4650` — update shader constants |
| 4 | `0xFB97C827` | `0x132C827` | `sub_7FF7FB9A01F0` — command buffer submit |

---

## 7. Command Buffer Submit (sub_7FF7FB9A01F0)

**IDA:** `0x7FF7FB9A01F0` | **RVA:** `0x13501F0`

Wraps command list execution in a critical section.

1. Calls vtable `[rax+50h]` — pre-submit flush
2. `EnterCriticalSection`
3. If rdx=0: fallback to `qword_7FF7FDA734C0 + 0x48`
4. `sub_7FF7FB9901E0` — lookup state block `0x5C006077`
5. `sub_7FF7FB999680` — execute command list
6. `sub_7FF7FB990280` — cleanup
7. `LeaveCriticalSection`
8. Calls vtable `[rax+58h]` — post-submit
9. Tail-call `sub_7FF7FB9999B0` — signal completion

---

## 8. Render Target Pool Init (sub_7FF7FB975D10)

**IDA:** `0x7FF7FB975D10` | **RVA:** `0x1325D10`

Creates all render targets at startup/resize. Uses two registration paths:

- **Color RTs:** `sub_7FF7FB417980` (RVA `0xDC7980`)
- **Depth/Stencil:** `sub_7FF7FB4179D0` (RVA `0xDC79D0`) — stores at manager `+ 0x1388 + slot*0x1C`

### RT Descriptor Structure (0x1C bytes)

| Offset | Type | Field |
|--------|------|-------|
| `+0x00` | uint32 | Width |
| `+0x04` | uint32 | Height |
| `+0x08` | uint32 | Format (DXGI_FORMAT enum) |
| `+0x0C` | uint32 | Flags / ArraySize |
| `+0x10` | int32 | MipLevels (-1 = full chain) |
| `+0x14` | uint32 | Misc flags |
| `+0x18` | uint16 | Bind flags / sample count |

---

## 9. RT Registration Functions

### sub_7FF7FB417980 — Color RT Registration

**IDA:** `0x7FF7FB417980` | **RVA:** `0xDC7980`

Registers a color render target descriptor into the RT manager.

- `rcx` = RT manager (`dword_7FF7FD7D5600`)
- `edx` = slot index
- `r8` = pointer to 0x1C-byte descriptor

### sub_7FF7FB4179D0 — Depth/Stencil RT Registration

**IDA:** `0x7FF7FB4179D0` | **RVA:** `0xDC79D0`

Registers a depth/stencil target descriptor. Stores at `[rcx + 0x1388 + slot*0x1C]`. Logs via `"Depth/stencil target"` string then tail-calls `sub_7FF7FB40C860`.

- `rcx` = RT manager (`dword_7FF7FD7D5600`)
- `edx` = slot index
- `r8` = pointer to 0x1C-byte descriptor

**Note:** Depth/stencil targets go through a completely separate registration and storage path from color RTs. If upscaling hooks only intercept `sub_7FF7FB417980`, depth/stencil targets will be missed entirely.

---

## 10. Render Target Slot Map

### Color RT Slots (via sub_7FF7FB417980)

| Slot | Res | Format | Likely Purpose |
|------|-----|--------|----------------|
| 0x01 | Full | 0x1C (R8G8B8A8) | Main color / G-buffer base |
| 0x02, 0x03 | Full | 0x1C | G-buffer albedo/normals |
| 0x04, 0x05 | Full | 0x1C | Additional G-buffer channels |
| 0x06 | Full/r14 | 0x1C | Downsampled variant |
| 0x07 | Full | 0x22 (R16G16B16A16_FLOAT) | HDR accumulation |
| 0x0D | Full | 0x1C | Additional full-res |
| 0x0E–0x14 | Various | 0x1C/0x1A | Post-processing chain |
| 0x0F | 512×512 | 0x1C | Cubemap face / env probe |
| 0x12, 0x10, 0x11 | Half | 0x1C | Half-res effects (SSAO etc.) |
| 0x18+ (loop) | Quarter→1×1 | from config | Bloom / luminance downsample |
| 0x28 | Full | 0x29 (R32G32_FLOAT) | Dynamic resolution variant |
| 0x2F, 0x3F, 0x41, 0x43 | Half | 0x1C | Conditional on `byte_7FF7FDAD5BE5` |
| 0x32+ (mip loop) | Mip chain | 0x29 + 0x100 | Hierarchical-Z / luminance |
| 0x44, 0x46, 0x48, 0x4A | Full | 0x1C | DRS targets (conditional `byte_7FF7FC525D80`) |
| 0x45, 0x47, 0x49 | Half | 0x1A | DRS half-res (conditional) |
| **0x4B** | **Aspect-dep** | **0x1C** | **TAA history buffer** |
| 0x4C | Half W × Full H | 0x1C + 0x1000000 | VR asymmetric target |
| 0x4E–0x50 | Half, quarter | 0x1A (R11G11B10_FLOAT) | SSR tiers (conditional) |
| 0x51–0x54 | Full | 0x1C | Deferred lighting outputs |
| 0x55 | Full | 0x31 | High-precision variant |
| 0x56, 0x57 | Full | 0x1C | Temporal upscaling (conditional) |
| 0x58–0x5A | Full | 0x18 | Lower precision targets |
| 0x5B–0x5D | Quarter | 0x1A | Bloom subpasses |
| 0x5E–0x61 | W/16 × H/4 | 0x1A | Luminance tier 1 |
| 0x62–0x65 | W/32 × H/4 | 0x1A | Luminance tier 2 |
| 0x66–0x68 | W/16 × H/16 | 0x1A | Luminance tier 3 |
| 0x69–0x6C | H × H | 0x1C | Square (cubemap faces?) |
| 0x6D–0x6F | Full/r14 | 0x0A (R16G16_FLOAT) | Velocity / motion vectors |
| 0x70, 0x71 | Full | 0x31 (R32G32B32A32_FLOAT) | High-precision buffers |
| **0x72** | **Full** | **0x1C + flags** | **Writeback target (UI render target)** |
| 0x73–0x75 | Fixed globals | 0x1C | Shadow map cascades |
| 0x76–0x7B | Fixed globals | 0x1C | Shadow maps (loop) |

### Depth/Stencil Slots (via sub_7FF7FB4179D0)

| Slot | Res | Notes |
|------|-----|-------|
| **0** | **Full** | **Main depth — must scale with upscaling** |
| **1** | **Full** | **Second eye / copy — must scale** |
| 2, 0x11, 0x12 | Shadow dims | Shadow cascade depths |
| 3, 0x13, 0x14 | Shadow dims/8 | Small shadow depths |
| 4 | Shadow dims | Different format (=8) |
| 5 | Conditional | Mip loop depth (gated by feature flags) |
| 6 | 200×200 | Utility depth |
| **7** | **Full + 0x100** | **Full-res with extra bind flags — must scale** |
| 9 | H × H | Square depth (cubemap?) |
| 0x0A | 200×200 | Utility |
| 0x0B, 0x0C | Fixed globals | Shadow-related |
| 0x0D | Fixed globals | Shadow-related |
| 0x0E | Quarter | Downsampled depth |
| 0x10 | Fixed globals | Shadow-related |

---

## 11. Hook Point Recommendations

### Primary Hook: Post-TAA+DRS, Pre-UI Render

**IDA:** `0x7FF7FAC09B80` | **RVA:** `0x5B9B80`

Located in the Frame Master function (`sub_7FF7FAC09330`), immediately after `call sub_7FF7FB974E80` returns at `0xFAC09B7B`.

**State at this point:**
- TAA has resolved into slot 0x4B
- DRS scaling has been applied
- All 3D post-processing is complete
- The scene is fully composited
- UI has NOT been drawn (Scaleform Display happens at `0xFAC09BC4`)
- Back buffer dimensions are queried next (swapchain vtable `+0x90`)
- You are at full back-buffer resolution context

**What follows:**
```
0xFAC09B80  ← HOOK HERE
0xFAC09B92  Swapchain GetBackBufferSize
0xFAC09BC4  sub_7FF7FB56C5F0 — Scaleform Display (UI render to slot 0x72)
0xFAC09BC9  sub_7FF7FB973EF0 — Present / final flip
```

**Use case:** Insert DLSS upscale pass here — take TAA output (slot 0x4B) and upscale to final resolution before UI composites at native resolution on top.

### Alternative Hook: Inside Frame Render Orchestrator

**IDA:** `0x7FF7FB97508B` | **RVA:** `0x132508B`

Inside `sub_7FF7FB974E80`, right after the post-processing conductor returns. Narrower scope but fully within the render thread context.

### Depth/Stencil Upscaling Hook

**IDA:** `0x7FF7FB4179D0` | **RVA:** `0xDC79D0`

Hook the depth/stencil registration function at entry. Check `edx` for slots 0, 1, 7 and scale the descriptor dimensions in `[r8]` to match your upscaled resolution. Leave shadow/utility depth slots (2–6, 9, 0x0A–0x14) at native size.

**Why depth/stencil may not be upscaling:** Depth/stencil targets are registered via `sub_7FF7FB4179D0` which stores at offset `+0x1388` in the RT manager — a completely separate path from color RTs registered via `sub_7FF7FB417980`. Any upscaling hook that only intercepts color RT registration will miss depth/stencil entirely.

### DRS Override Injection Point

**IDA:** `0x7FF7FB97C5B9` | **RVA:** `0x132C5B9`

Inside the RT Activation function (`sub_7FF7FB97C530`). The override path gated by `byte_7FF7FDAD5BD0` and `byte_7FF7FC525A70` substitutes `dword_7FF7FC525A88` as the resolution scale. Setting these flags and the scale value could force a custom resolution through the existing DRS pipeline.

---

*Analysis based on SkyrimVR executable with image base `0x7FF7FA650000`.*
