# SPDX-License-Identifier: GPL-3.0-or-later
"""Check Mesa patch anchors against vendored, unmodified pinned source (no SDK/network).

These are patch-generation tests, not D3D12 runtime validation or a Mesa build.
"""

import hashlib
import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures/mesa-15acdd7"
DRIVER = Path("src/gallium/drivers/d3d12")
SPEC = importlib.util.spec_from_file_location(
    "patch_mesa_uwp", ROOT / "tools/nxbox/patch_mesa_uwp.py"
)
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


class MesaPsoPatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="nxbox-mesa-pso-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.driver = self.root / DRIVER
        self.driver.mkdir(parents=True)
        for source in FIXTURES.glob("*.cpp"):
            shutil.copyfile(source, self.driver / source.name)

    def patched(self):
        PATCH.patch_pso(self.root)
        return (self.driver / "d3d12_pipeline_state.cpp").read_text()

    def test_pinned_fixture_checksums(self):
        expected = {
            "d3d12_pipeline_state.cpp": "8fd9def7d83810d94fd887a8b4a4335b70e15f8f5d7b79603dcc0e077f8f3f47",
            "d3d12_draw.cpp": "4b4df349be4eabf20ce06f342b4bc55f1b8041e4bbcb83b35302e599cd51659f",
        }
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest(), digest)

    def test_original_fast_path_and_both_creation_apis(self):
        source = self.patched()
        create = source.split("   auto create = [&]() -> HRESULT {", 1)[1].split("   };", 1)[0]
        self.assertIn("sizeof(pso_desc), &pso_desc", create)
        self.assertIn("CreatePipelineState(&stream", create)
        self.assertIn("v0desc = pso_desc.GraphicsDescV0()", create)
        self.assertIn("CreateGraphicsPipelineState(&v0desc", create)
        self.assertLess(
            source.index("if (SUCCEEDED(hr))\n      return ret;"),
            source.index("const auto original = pso_desc;"),
        )
        # The compute path must remain byte-for-byte identical.
        marker = "static ID3D12PipelineState *\ncreate_compute_pipeline_state"
        original = (FIXTURES / "d3d12_pipeline_state.cpp").read_text()
        self.assertEqual(source.split(marker)[1], original.split(marker)[1])

    def test_retry_gating_terminal_failure_and_reporting(self):
        source = self.patched()
        self.assertIn("level <= 5 && hr == E_INVALIDARG && SUCCEEDED(removed)", source)
        self.assertIn("hr = results[level - 1] = create();", source)
        self.assertIn("tried |= 1u << (level - 1);", source)
        self.assertIn(
            "nxbox_report_pso_fallback(id, 0, tried, results, removed);\n   return NULL;", source
        )
        self.assertEqual(source.count("removed = screen->dev->GetDeviceRemovedReason();"), 2)
        self.assertIn("counts[level]++;", source)
        self.assertIn("std::lock_guard<std::mutex>", source)

    def test_success_keeps_original_key_and_effective_attachments(self):
        source = self.patched()
        self.assertIn("data->key = ctx->gfx_pipeline_state;", source)
        self.assertIn("data->pso = create_gfx_pipeline_state(ctx, data);", source)
        self.assertIn("cache_entry->fallback_level = level;", source)
        self.assertIn("cache_entry->num_render_targets = render_targets.NumRenderTargets;", source)
        self.assertIn("&data->key, data", source)
        self.assertIn("if (!data->pso) {\n         FREE(data);\n         return NULL;", source)
        self.assertIn("return ((struct d3d12_gfx_pso_entry *)(entry->data))->pso;", source)
        draw = (self.driver / "d3d12_draw.cpp").read_text()
        self.assertIn("D3D12_DIRTY_FRAMEBUFFER | D3D12_DIRTY_GFX_PSO", draw)
        self.assertIn("d3d12_nxbox_set_render_targets(ctx, ctx->fb.nr_cbufs", draw)
        self.assertIn("count = MIN2(count, data->num_render_targets);", source)
        self.assertIn("if (!data->has_dsv)\n            depth = NULL;", source)

    def test_ladder_order_and_original_dsv_only_last_resort(self):
        source = self.patched()
        ladder = source.split("nxbox_simplify_pso(", 1)[1].split("\nstatic ID3D12PipelineState", 1)[
            0
        ]
        for level, expression in enumerate(
            [
                "rt.LogicOpEnable = false;",
                "rt.BlendEnable = false;",
                "blend.AlphaToCoverageEnable = false;",
                "nxbox_ps_color_count(ps)",
                "desc = original;",
            ],
            1,
        ):
            case = ladder.split(f"case {level}:", 1)[1].split("break;", 1)[0]
            self.assertIn(expression, case)
        self.assertIn("blend.IndependentBlendEnable = true;", ladder)
        for field in ["ForcedSampleCount", "DepthBias", "DepthBiasClamp", "SlopeScaledDepthBias"]:
            self.assertIn(f"rast.{field} = 0;", ladder)
        self.assertIn("targets.RTFormats[i] = DXGI_FORMAT_UNKNOWN;", ladder)
        self.assertIn("MIN2(targets.NumRenderTargets, nxbox_ps_color_count(ps))", ladder)
        self.assertRegex(
            ladder, r"case 5:\s+desc = original;\s+desc.DSVFormat = DXGI_FORMAT_UNKNOWN;\s+break;"
        )
        # No shader substitutions, attachment format reinterpretation, or shared state writes.
        self.assertNotIn("state->", ladder)
        self.assertNotIn("desc.PS =", ladder)

    def test_color_slots_and_masks(self):
        source = self.patched()
        count = source.split("nxbox_ps_color_count(nir_shader *ps)", 1)[1].split("static UINT8", 1)[
            0
        ]
        self.assertIn("if (!ps)\n      return 0;", count)
        self.assertIn("BITFIELD64_BIT(FRAG_RESULT_COLOR)", count)
        self.assertIn("BITFIELD64_BIT(FRAG_RESULT_DATA0 + i)", count)
        self.assertIn("count = i + 1;", count)
        self.assertNotIn("util_bitcount", count)
        self.assertNotIn("FRAG_RESULT_DEPTH", count)
        mask = source.split("nxbox_rtv_write_mask(DXGI_FORMAT format)", 1)[1].split("/* Report", 1)[
            0
        ]
        self.assertIn("case DXGI_FORMAT_UNKNOWN: return 0;", mask)
        self.assertIn("case DXGI_FORMAT_A8_UNORM: return D3D12_COLOR_WRITE_ENABLE_ALPHA;", mask)
        red = mask.split("case DXGI_FORMAT_R8_UNORM:", 1)[1].split("return", 1)[1].split(";", 1)[0]
        self.assertEqual(red.strip(), "D3D12_COLOR_WRITE_ENABLE_RED")

    def test_reports_include_requested_fields_and_frontend_subscriptions(self):
        source = self.patched()
        report = source.split("nxbox_report_pso_desc(", 1)[1].split(
            "nxbox_report_pso_fallback(", 1
        )[0]
        for field in [
            "BlendEnable",
            "LogicOpEnable",
            "SrcBlend",
            "DestBlend",
            "BlendOp",
            "SrcBlendAlpha",
            "DestBlendAlpha",
            "BlendOpAlpha",
            "LogicOp",
            "RenderTargetWriteMask",
            "AlphaToCoverageEnable",
            "FillMode",
            "CullMode",
            "FrontCounterClockwise",
            "DepthBias",
            "DepthClipEnable",
            "MultisampleEnable",
            "AntialiasedLineEnable",
            "ForcedSampleCount",
            "ConservativeRaster",
            "SampleMask",
            "outputs_written",
            "SemanticName",
            "SemanticIndex",
            "e.Format",
            "glsl_get_bit_size",
            "glsl_get_vector_elements",
            "cast_to_uint",
            "cast_to_int",
        ]:
            self.assertIn(field, report)
        self.assertIn("MIN2(d.InputLayout.NumElements, 4u)", report)
        self.assertIn("char text[512];", report)
        self.assertIn("sizeof(text)", report)
        self.assertIn("CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT", report)
        frontend = (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        for name in ["NXBOX_D3D12_PSO_FAIL", "NXBOX_D3D12_PSO_FAIL2", "NXBOX_D3D12_PSO_FALLBACK"]:
            self.assertIn(f'SetEnvironmentVariableA("{name}"', source)
            self.assertIn(f'"{name}"', frontend)
        self.assertIn("std::array<std::string, 9> last", frontend)
        self.assertIn("std::array<const char*, 9> names", frontend)

    def test_draw_patches_compose_in_production_order(self):
        self.patched()
        PATCH.patch_draw(self.root)
        PATCH.patch_null_pso(self.root)
        source = (self.driver / "d3d12_draw.cpp").read_text()
        self.assertIn("if (!ctx->current_gfx_pso) {", source)
        self.assertIn("if (!ctx->current_compute_pso)", source)
        self.assertIn("d3d12_nxbox_set_render_targets(ctx, ctx->fb.nr_cbufs", source)

    def test_reapplying_rejects_without_writing(self):
        self.patched()
        before = {p: p.read_bytes() for p in self.driver.glob("*.cpp")}
        with self.assertRaisesRegex(RuntimeError, "anchor mismatch"):
            PATCH.patch_pso(self.root)
        for path, contents in before.items():
            self.assertEqual(path.read_bytes(), contents)

    def test_missing_or_duplicate_anchor_rejects_without_partial_writes(self):
        anchors = {
            "d3d12_pipeline_state.cpp": [
                "static ID3D12PipelineState *\ncreate_gfx_pipeline_state(struct d3d12_context *ctx)\n",
                "   ID3D12PipelineState *ret;\n\n   if (screen->opts14.",
                "struct d3d12_gfx_pso_entry {\n",
                "      data->pso = create_gfx_pipeline_state(ctx);",
                "void\nd3d12_gfx_pipeline_state_cache_init(struct d3d12_context *ctx)\n",
            ],
            "d3d12_draw.cpp": [
                '#include "d3d12_context.h"\n',
                "   if (ctx->cmdlist_dirty & D3D12_DIRTY_FRAMEBUFFER) {",
                "      ctx->cmdlist->OMSetRenderTargets(ctx->fb.nr_cbufs, render_targets, false, depth_desc);",
            ],
        }
        for name, needles in anchors.items():
            for needle in needles:
                for duplicate in (False, True):
                    with self.subTest(name=name, needle=needle, duplicate=duplicate):
                        for source in FIXTURES.glob("*.cpp"):
                            shutil.copyfile(source, self.driver / source.name)
                        path = self.driver / name
                        original = path.read_text()
                        # Duplicate whole files to exercise exact-one checks for every anchor.
                        modified = (
                            original * 2
                            if duplicate
                            else original.replace(needle, "/* changed */", 1)
                        )
                        self.assertNotEqual(original, modified)
                        path.write_text(modified)
                        before = {p: p.read_bytes() for p in self.driver.glob("*.cpp")}
                        with self.assertRaisesRegex(RuntimeError, "anchor mismatch"):
                            PATCH.patch_pso(self.root)
                        for path, contents in before.items():
                            self.assertEqual(path.read_bytes(), contents)


if __name__ == "__main__":
    unittest.main()
