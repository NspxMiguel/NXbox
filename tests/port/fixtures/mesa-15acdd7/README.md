# Mesa patch fixtures

Unmodified files from `aerisarn/mesa-uwp`, commit
`15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`, under
`src/gallium/drivers/d3d12/`. Their Microsoft MIT license notices are retained.
These copies allow patch-generation tests to run without network access or a Windows SDK.
`test_mesa_pso.py` and `test_mesa_dred.py` check their SHA-256 digests before
exercising the patches. DRED fixtures additionally cover screen/device creation,
batch submission, context teardown and root-signature failures.

Source: https://github.com/aerisarn/mesa-uwp/tree/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/src/gallium/drivers/d3d12

Render-safety fixtures add the unmodified `d3d12_blit.cpp` and `d3d12_query.cpp` from the
same pin; `test_mesa_render_safety.py` checks their SHA-256 digests. The adjacent
`../botw-pipe6-dxil.json` is separate runtime evidence, decoded from the complete
`D3D12_PSO_DXIL_VS_0` / `PS_0` fields in the owner's
`pipe6-6a7affd1e/botw-diag.txt`, not Mesa source. It retains the captured bytecode so
host tests exercise the production signature reader and pair quarantine, including
negative tests that mutate each byte without changing the shader sizes.
