# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify DRED patch generation against pinned Mesa, without an SDK or GPU.

These tests validate composition and diagnostic contracts, not C++ compilation
or the Xbox runtime's support for DRED.
"""

import hashlib
import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures/mesa-15acdd7"
SPEC = importlib.util.spec_from_file_location(
    "mesa_dred_patch", ROOT / "tools/nxbox/patch_mesa_uwp.py"
)
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class MesaDredPatchTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="nxbox-mesa-dred-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.driver = self.root / "src/gallium/drivers/d3d12"
        self.driver.mkdir(parents=True)
        self.reset()

    def reset(self, pso=True):
        for path in self.driver.iterdir():
            path.unlink()
        for source in FIXTURES.glob("*.cpp"):
            shutil.copyfile(source, self.driver / source.name)
        if pso:
            PATCH.patch_pso(self.root)
        PATCH.patch_shader_model(self.root)
        PATCH.patch_format_cast_report(self.root)
        PATCH.patch_draw(self.root)
        PATCH.patch_null_pso(self.root)
        PATCH.patch_root_signature_report(self.root)
        PATCH.patch_batch(self.root)

    def read(self, name):
        return (self.driver / name).read_text()

    def snapshot(self):
        return {p.name: p.read_bytes() for p in self.driver.iterdir()}

    def test_pinned_fixture_checksums(self):
        expected = {
            "d3d12_screen.cpp": "84cc2e5e666c1ec4db320923c2e937ae6909f71ec6854870c95b273336ce045f",
            "d3d12_batch.cpp": "95e4c09773d9f7237158e4e53ffc5e5882f1950662839a8ed38a0a1710e2835e",
            "d3d12_context.cpp": "b3722ec76d797dcd36dd07a68aa460c28c7399456af97c315bf9f402aaf15ffb",
            "d3d12_root_signature.cpp": "e2e6dc8cfd741c78351cbfed59fbda2109f602de669a781bdc9b18631561dd3a",
        }
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest(), digest)

    def test_enable_before_device_with_matching_factory_and_legacy_fallback(self):
        PATCH.patch_dred(self.root)
        screen = self.read("d3d12_screen.cpp")
        helper = self.read("nxbox_dred.h")
        self.assertEqual(helper, (ROOT / "tools/nxbox/mesa_dred.h").read_text())
        self.assertEqual(screen.count("#define NXBOX_DRED_IMPLEMENTATION"), 1)
        self.assertLess(
            screen.index("nxbox_dred_enable(screen->d3d12_mod, factory)"),
            screen.index("screen->dev = create_device(screen->d3d12_mod, adapter, factory)"),
        )
        for text in [
            'util_dl_get_proc_address(module, "D3D12GetDebugInterface")',
            "factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData",
            "ID3D12DeviceRemovedExtendedDataSettings1 *settings1",
            "ID3D12DeviceRemovedExtendedDataSettings *settings",
            "SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON)",
            "SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON)",
            "SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON)",
            "ERROR_PROC_NOT_FOUND",
            "unavailable stage=%s hr=0x%08lx",
            "contexts=unavailable",
            "settings1->Release()",
            "settings->Release()",
        ]:
            self.assertIn(text, helper)
        self.assertNotIn("EnableDebugLayer", helper)

    def test_single_capture_across_batch_pso_and_root_signature(self):
        PATCH.patch_dred(self.root)
        pso = self.read("d3d12_pipeline_state.cpp")
        for tag in ["gfx-pso", "gfx-pso-retry", "compute-pso"]:
            self.assertIn(f'"{tag}"', pso)
        root = self.read("d3d12_root_signature.cpp")
        for tag in ["rootsig", "rootsig-serialize"]:
            self.assertIn(f'"{tag}"', root)
        helper = self.read("nxbox_dred.h")
        capture = helper.split("void\nnxbox_dred_capture", 1)[1]
        self.assertLess(
            capture.index("if (SUCCEEDED(removed))"), capture.index("lock(nxbox_dred_mutex)")
        )
        self.assertLess(capture.index("if (state.captured)"), capture.index("QueryInterface"))
        self.assertLess(capture.index("state.captured = true"), capture.index("QueryInterface"))
        self.assertIn("nxbox_dred_states.erase(dev)", helper)
        screen = self.read("d3d12_screen.cpp")
        self.assertLess(
            screen.index("nxbox_dred_forget(screen->dev)"), screen.index("screen->dev->Release()")
        )

    def test_capture_fallback_outputs_and_bounds(self):
        PATCH.patch_dred(self.root)
        helper = self.read("nxbox_dred.h")
        for text in [
            "GetAutoBreadcrumbsOutput1(&output)",
            "GetPageFaultAllocationOutput1(&fault)",
            "GetAutoBreadcrumbsOutput(&output)",
            "GetPageFaultAllocationOutput(&fault)",
            "dred1->Release()",
            "dred->Release()",
            "char text[501]",
            "vsnprintf",
            "visited < 4096",
            "shown < 2",
            "i < 2",
            "i < 65536",
            "if (!node->pLastBreadcrumbValue)",
            "last >= node->BreadcrumbCount",
            "!node->pCommandHistory",
            "i % 65536",
            "i < oldest",
            'out.add("overwritten")',
            "UINT i = last - back",
            "PageFaultVA",
            "pHeadExistingAllocationNode",
            "pHeadRecentFreedAllocationNode",
            "ObjectNameA",
            "ObjectNameW",
            "AllocationType",
            "pBreadcrumbContexts",
            "BreadcrumbIndex",
            "pContextString",
            "OP(%u)",
            "pCommandListDebugNameA",
            "pCommandQueueDebugNameW",
            "__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__",
            "__ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__",
        ]:
            self.assertIn(text, helper)
        for op in [
            "COPYTEXTUREREGION",
            "RESOURCEBARRIER",
            "EXECUTEINDIRECT",
            "DRAWINSTANCED",
            "DRAWINDEXEDINSTANCED",
            "DISPATCH",
            "CLEARUNORDEREDACCESSVIEW",
        ]:
            self.assertIn(f"NXBOX_DRED_OP({op})", helper)
        frontend = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        for name in ["NXBOX_D3D12_DRED", "NXBOX_D3D12_DRED2"]:
            self.assertIn(f'"{name}"', frontend)
            self.assertIn(f'SetEnvironmentVariableA("{name}"', helper)

    def test_batch_observation_is_not_throttled_and_correlates_submission(self):
        PATCH.patch_dred(self.root)
        batch = self.read("d3d12_batch.cpp")
        report = batch.split("nxbox_report_batch(", 1)[1].split("nxbox_report_reset", 1)[0]
        self.assertLess(report.index("GetDeviceRemovedReason()"), report.index("if (!report"))
        self.assertIn("if (!report && SUCCEEDED(removed))", report)
        self.assertIn("(unsigned)(batch - ctx->batches)", report)
        self.assertIn("batch->submit_id", report)
        self.assertIn("fence_target=%llu fence_ok=%u", report)
        self.assertIn('strcmp(stage, "close") == 0', report)
        self.assertIn('nxbox_report_batch(ctx, batch, nxbox_close_hr, "close", 0)', batch)
        self.assertIn('nxbox_report_batch(ctx, batch, S_OK, "execute", nxbox_fence_target)', batch)
        self.assertLess(
            batch.index("nxbox_dred_submit(screen->dev"), batch.index("->ExecuteCommandLists(")
        )
        self.assertLess(
            batch.index("batch->fence = d3d12_create_fence(screen)"),
            batch.index('nxbox_report_batch(ctx, batch, S_OK, "execute"'),
        )
        self.assertIn("screen->fence_value + 1", batch)
        self.assertIn("ctx->cmdlist->SetName(nxbox_name)", batch)
        self.assertIn("ctx->state_fixup_cmdlist->SetName(nxbox_name)", batch)
        helper = self.read("nxbox_dred.h")
        self.assertIn("submissions[4][96]", helper)
        self.assertIn("recent(newest-first)", helper)
        self.assertIn("nxbox_dred_publish_batch(screen->dev, text)", report)
        self.assertIn("if (!nxbox_dred_states[dev].captured)", helper)

    def test_pso_skip_keeps_both_upstream_graphics_paths_covered(self):
        self.reset(pso=False)
        PATCH.patch_dred(self.root)
        pso = self.read("d3d12_pipeline_state.cpp")
        self.assertEqual(pso.count('"gfx-pso"'), 2)
        self.assertIn('"compute-pso"', pso)

    def test_reapplication_and_existing_helper_reject_without_writes(self):
        PATCH.patch_dred(self.root)
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "DRED anchor mismatch"):
            PATCH.patch_dred(self.root)
        self.assertEqual(before, self.snapshot())
        self.reset()
        (self.driver / "nxbox_dred.h").write_text("existing helper\n")
        before = self.snapshot()
        with self.assertRaisesRegex(RuntimeError, "helper already exists"):
            PATCH.patch_dred(self.root)
        self.assertEqual(before, self.snapshot())

    def test_missing_and_duplicate_anchors_do_not_partially_write(self):
        anchors = {
            "d3d12_screen.cpp": [
                "#include <dxguids/dxguids.h>\n",
                "      screen->dev = create_device(screen->d3d12_mod, adapter, factory);",
                "      screen->dev->Release();\n      screen->dev = nullptr;",
            ],
            "d3d12_batch.cpp": [
                "#include <directx/d3d12sdklayers.h>\n",
                "   nxbox_report_batch(screen, nxbox_close_hr);",
                "   screen->cmdqueue->ExecuteCommandLists(count_to_execute, to_execute);",
                "   batch->fence = d3d12_create_fence(screen);",
                "   batch->submit_id = ++ctx->submit_id;",
            ],
            "d3d12_pipeline_state.cpp": [
                "   HRESULT removed = screen->dev->GetDeviceRemovedReason();",
                "      removed = screen->dev->GetDeviceRemovedReason();",
                '      debug_printf("D3D12: CreateComputePipelineState failed!\\n");',
            ],
            "d3d12_root_signature.cpp": [
                '      nxbox_report_root_signature("create", nxbox_create_hr, NULL, num_params);',
                '         nxbox_report_root_signature("serialize", nxbox_hr, error.Get(), num_params);',
            ],
        }
        for name, needles in anchors.items():
            for needle in needles:
                for duplicate in [False, True]:
                    with self.subTest(name=name, needle=needle, duplicate=duplicate):
                        self.reset()
                        path = self.driver / name
                        original = path.read_text()
                        self.assertEqual(original.count(needle), 1)
                        path.write_text(
                            original + needle if duplicate else original.replace(needle, "changed")
                        )
                        before = self.snapshot()
                        with self.assertRaisesRegex(RuntimeError, "DRED anchor mismatch"):
                            PATCH.patch_dred(self.root)
                        self.assertEqual(before, self.snapshot())


if __name__ == "__main__":
    unittest.main()
