# Mesa patch fixtures

Unmodified files from `aerisarn/mesa-uwp`, commit
`15acdd7ea2b9dcdd62f26fe86b88280d79efc46b`, under
`src/gallium/drivers/d3d12/`. Their Microsoft MIT license notices are retained.
These copies allow patch-generation tests to run without network access or a Windows SDK.
`test_mesa_pso.py` and `test_mesa_dred.py` check their SHA-256 digests before
exercising the patches. DRED fixtures additionally cover screen/device creation,
batch submission, context teardown and root-signature failures.

Source: https://github.com/aerisarn/mesa-uwp/tree/15acdd7ea2b9dcdd62f26fe86b88280d79efc46b/src/gallium/drivers/d3d12
