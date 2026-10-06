# Xbox graphics PSO audit and quarantine

Host-only audit of Mesa `15acdd7ea2`, BotW `pipe6-6a7affd1e` and MK8D
`pipe6-3dff688b0`. No console validation. This change avoids selected creation
calls; it cannot guarantee that an unknown PSO or an adapter-wide fault will
never remove the device. Skipped draws can cause missing graphics.

## Reproduce the container inspection

```sh
python3 tools/nxbox/inspect_dxil.py tests/port/fixtures/botw-pipe6-dxil.json
```

The dependency-free parser checks container/part bounds, DXIL program headers,
ISG1/OSG1 signatures, PSV0 runtime information and resource bindings, SFI0,
HASH, and RDAT tables (including resource records). A small LLVM bitstream
reader extracts type declarations, symbols, and Mesa's version metadata. It
is not a DXIL validator, hash verifier, full disassembler, or reachability
analysis. Unknown data remains explicit. A binary DXBC container is also accepted.

| BotW property | VS (2492 bytes) | PS (2192 bytes) |
| --- | --- | --- |
| Shader model / metadata DXIL / validator | 6.4 / 1.4 / 1.8 | 6.4 / 1.4 / 1.8 |
| PSV resources | CBV `b1, space0`, kind cbuffer, flags 0 | SRV `t0, space0`, kind Texture2DArray, flags 0 |
| Resource operations | `createHandle`, `cbufferLoadLegacy.f32` | `createHandle`, `textureLoad.f32` |
| Input semantics | float32 TEXCOORD0/1, mask F, read mask 1 | float32 TEXCOORD0–3, mask F, read masks 1/0/0/0 |
| Output semantics | TEXCOORD0–3 and SV_Position (system type 1) | SV_Target0 (system type 64) |
| Samplers / UAV / ROV | none | none |
| Half or int16 type declarations / signature min precision | none / 0 | none / 0 |
| Derivative or sample intrinsic declarations | none | none |
| Feature flags / ViewID / special wave requirement | SFI0=0 / 0 / none | SFI0=0 / 0 / none |

PSV runtime info is 48 bytes with 24-byte resource records. There is no
RDAT, HASH, or embedded RTS0 root signature in either container. The DXBC
header contains a digest; absence of a HASH part does **not** mean unsigned
DXIL. The PS metadata string `sampler` names its texture resource, not a
sampler binding. A texture load needs no sampler or implicit derivatives.

The DXIL program header says 1.0 while `dx.version` metadata says 1.4. This
is exactly how this Mesa revision writes it (`dxil_container_add_module`
hardcodes `1 << 8`); it is not evidence of capture corruption. Mesa chooses
the shader minor version from the device cap and validator version, caps the
validator at 1.8, and writes the 1.8 metadata observed here. Compatibility
between that emitted container and Xbox's driver remains a candidate, not a
proven cause. Zero SFI0 does not establish driver acceptance.

## Root signatures and PSO fields

Pinned `d3d12_root_signature.cpp` uses version 1.1, **no static samplers**, and
stage-specific visibility. It supplies IA permission for graphics and stream
output permission only when required. Bindings are finite counts derived from
the compiled shader; there is no intentional unbounded range. The two SSBO
ranges alias descriptor offsets in **different register spaces**, 0 and 2;
that is not an overlapping shader-register declaration and BotW uses neither.

Xbox UWP follows the Windows path, not `_GAMING_XBOX`: CBV/SRV ranges use
`DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS`; sampler/UAV ranges use NONE.
There is no `DESCRIPTORS_VOLATILE`. NONE still makes static-descriptor promises
under root signature 1.1. Do not blindly remove flags or change binding
semantics based on a PSO failure. [Microsoft's flag contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-version-1-1)
explains these lifetime promises.

BotW requires VS b1 and PS t0. Mesa implements state variables through root
constants and also allocates a sampler table alongside an SRV table, even
when the shader has no sampler declaration. Actual root parameter counts and
constant sizes cannot be recovered from the logged root pointer: no serialized
root signature was captured. MK8D's resource requirements are still unknown.
The source uses shader-visible sampler heaps of 1024 (below 2048), view heaps
of 8096, and non-visible sampler pools of 64. These heap sizes are distinct
from per-stage table limits. The observed BotW bindings fit even Tier 1's
14 CBVs, 128 SRVs and 16 samplers; the Xbox tier itself was not captured.
[Microsoft's hardware tier table](https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-support)
provides those limits. Mesa already queries the tier for shader caps.

Both PSOs have one UNORM RTV (28), triangle topology (3), samples 1/0, node
mask 0, flags 0, no cached blob, no depth attachment, and no HS/DS/GS.
Blend factors 2/1 and operation 1 mean ONE/ZERO/ADD; `op=4` is the disabled
logic operation NOOP, **not** blend MIN. Fill 3 is SOLID. These fields are
ordinary legal state. Rasterization differs: BotW uses cull NONE/CCW false;
MK8D uses BACK/CCW true. IA differs too: BotW has two float4 attributes,
whereas MK8D has one **float3** (format 6) TEXCOORD0. Its complete DXIL is absent.

The common suspicious fields are zero comparison/stencil enums under disabled
depth/stencil. They are now canonicalized before the first creation call to
ALWAYS/KEEP. Active state is preserved. Existing input normalization already
applies to all shaders, not only BotW, and matches emitted signatures; it
must not invent attributes or narrow a float4 based on a read mask.

## Defense and canary decision

`NXBOX_PSO_GUARD` defaults ON. Setting it to `0` disables the shape heuristic:
VS 2048–2560 bytes, PS 2048–2304 bytes, one RTV 28, triangle topology,
samples 1/0, no HS/DS/GS, and one or two per-vertex TEXCOORD0[/1] float3/float4
inputs matching the emitted VS signature. Size bounds are inclusive. This is
an empirical quarantine, not a validator. Valid draws can match it. The exact
BotW and MK8D hash pairs remain blocked even with the heuristic disabled.
`NXBOX_PSO_FIX=0` disables ignored-state/IA normalization, not quarantine.

No default-on throwaway device was added: safe fault isolation is not
established for this Xbox UWP driver. [D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice)
returns the existing per-adapter device. Mesa also uses DeviceFactory, but
explicitly permits returning existing/incompatible devices. Modern
[independent-device APIs](https://microsoft.github.io/DirectX-Specs/d3d/IndependentDevices.html)
can request independence and fail if the driver lacks support. The logged
`FactoryConfiguration` proves configuration access, not independent-device
support or isolation from this removal. A future canary would need a separate
factory without reuse flags, verified COM identity, a root signature recreated
from its serialized blob, and evidence that removal stays local. It must never
reuse the main device's root object. `NXBOX_PSO_CANARY` is not implemented or
silently advertised as protection. This revision uses the requested heuristic
fallback. A trivial replacement PSO cannot repair an already removed device;
the existing null-PSO path skips the draw instead.

## Why MK8D DXIL disappeared, and the next log

In the existing source, `PSO_FAIL_FIRST` and DXIL capture share a routine. The
MK8D log has the former but exits through `GAME_FAIL` before the next five-second
poll, and that terminal path omitted DXIL. This explains the missing log data
without assuming CreateGraphicsPipelineState returned success. There was also
a separate success-after-removal hole: only creation failures reached the
first-capture routine.

Capture now runs for pre-existing loss, failed creation, and success followed
by removal, **before** publishing DEVICE_LOST. Successful API results observed
on a removed device release their returned PSO and become terminal failures.
The frontend drains the manifest and every chunk on terminal loss too. A known
pair supersedes the first heuristic capture; a real failure supersedes both.
No failing-device creation or retry is attempted. The first rejected unknown
PSO can still remove a healthy device; host inspection cannot eliminate that risk.

```sh
rg 'D3D12_PSO_(FIX|FAIL_FIRST|DXIL)|D3D12_DRED|D3D12_FIRST_FAILURE|GAME_(PRESENT|FAIL)' next-diag.txt
```

Expect `quarantined graphics signature; draw skipped`, `hr=00000001` (S_FALSE,
intentional skip), and DXIL manifest `complete=1`, followed by VS/PS chunks.
For MK8D the hashes should be `a9d00cddd91828bc` / `789a1e9e39699088`. Continued
`GAME_PRESENT` without a new removal is the runtime check still needed.
`D3D12_PSO_DXIL_ERROR` means the capture must not be treated as complete.

Host validation: all 76 Python tests in `tests/port` passed, including the
full patch chain against `/tmp/mesa-pin` with LF and CRLF. All four portable
C++ test executables (`demand_commit`, `update_version`, `protocol_uri`,
`cheats_parse`) compiled and passed. The Windows-only sparse-memory integration
test cannot run on this Mac. No Xbox build, GPU execution, or console test was performed.
