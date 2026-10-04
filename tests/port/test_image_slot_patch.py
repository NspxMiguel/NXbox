"""The d3d12 image slot patch indexes the emulation formats by slot (i + start_slot)."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("patch_mesa_uwp", ROOT / "tools/nxbox/patch_mesa_uwp.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)

SOURCE = """\
      ctx->image_view_emulation_formats[shader][i] = PIPE_FORMAT_NONE;
      if (i < count && images && images[i].resource) {
            ctx->image_view_emulation_formats[shader][i] =
               get_shader_image_emulation_format(images[i].resource->format);
      }
"""


class ImageSlotPatchTest(unittest.TestCase):
    def test_both_writes_use_the_slot_index(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "src/gallium/drivers/d3d12/d3d12_context.cpp"
            target.parent.mkdir(parents=True)
            target.write_text(SOURCE)
            PATCH.patch_image_slot(Path(tmp))
            patched = target.read_text()
        self.assertEqual(patched.count("[shader][i + start_slot]"), 2)
        self.assertNotIn("[shader][i] =", patched)

    def test_rejects_unexpected_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "src/gallium/drivers/d3d12/d3d12_context.cpp"
            target.parent.mkdir(parents=True)
            target.write_text("int unrelated;\n")
            with self.assertRaises(RuntimeError):
                PATCH.patch_image_slot(Path(tmp))


if __name__ == "__main__":
    unittest.main()
