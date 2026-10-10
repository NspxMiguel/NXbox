# SPDX-License-Identifier: GPL-3.0-or-later
"""Check wait policy, actual generated submit branches, and pinned-source composition."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import test_mesa_render_safety as safety
from test_mesa_full_chain import ROOT, SOURCE


@unittest.skipUnless(SOURCE.is_dir(), "Set NXBOX_MESA_SRC or provide /tmp/mesa-pin")
class MesaPerfWaitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="nxbox-perf-waits-test-")
        cls.root = Path(cls.temp.name)
        mesa = cls.root / "mesa"
        shutil.copytree(SOURCE, mesa, ignore=shutil.ignore_patterns(".git"))
        environment = os.environ.copy()
        environment.pop("NXBOX_MESA_SKIP", None)
        subprocess.run(
            [sys.executable, str(ROOT / "tools/nxbox/patch_mesa_uwp.py"), str(mesa)],
            env=environment,
            check=True,
            capture_output=True,
            text=True,
            timeout=60,
        )
        cls.driver = mesa / "src/gallium/drivers/d3d12"

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_generated_submission_behavior(self):
        batch = (self.driver / "d3d12_batch.cpp").read_text()
        block = batch.split("   const bool nxbox_drain_submit =", 1)[1].split(
            '   nxbox_observe(screen->dev, "execute-return");', 1
        )[0]
        helper = (self.driver / "nxbox_perf_waits.h").read_text().replace("#pragma once", "")
        runner = safety.MesaRenderSafetyTests()
        runner.root = self.root
        # Compile the exact generated post-Execute branch, not a second implementation of it.
        code = (
            r"""
#include <cassert>
#include <cstring>
#include <string>
#include <atomic>
using HRESULT=int;
using UINT64=unsigned long long;
#define FAILED(hr) ((hr)<0)
#define SUCCEEDED(hr) ((hr)>=0)
std::string report;
void SetEnvironmentVariableA(const char *name,const char *value) {
 if(!strcmp(name,"NXBOX_D3D12_PERF_WAITS")) report=value;
}
"""
            + helper
            + r"""
bool sync_enabled=true;
bool nxbox_sync_batch_enabled() { return sync_enabled; }
unsigned waits=0,signals=0,collections=0,captures=0,completions=0;
struct Device { HRESULT removed=0; HRESULT GetDeviceRemovedReason() { return removed; } };
struct Queue { HRESULT result=0; HRESULT Signal(void *,UINT64) { ++signals; return result; } };
struct Screen { Device *dev; Queue *cmdqueue; void *fence=nullptr; UINT64 fence_value=0; };
struct Batch { bool has_errors=false; };
struct Context { void *nxbox_journal=nullptr; };
template <typename T> T &nxbox_api(T *value,const char *) { return *value; }
HRESULT nxbox_sync_wait(Device *dev,void *,UINT64 target) {
 assert(signals && target==signals); ++waits; return dev->GetDeviceRemovedReason();
}
void nxbox_sync_capture(Device *,HRESULT) { ++captures; }
void nxbox_dred_capture(Device *,HRESULT,const char *) { ++captures; }
void nxbox_sync_completed(Device *) { ++completions; }
std::atomic<bool> stopped{false};
std::atomic<bool> &nxbox_sync_stopped() { return stopped; }
#define NXBOX_PROFILE_COLLECT(queue,journal) (++collections)
void submit(Screen *screen,Batch *batch,Context *ctx) {
 (void)ctx;
 const bool nxbox_drain_submit =
"""
            + block
            + r"""
}
int main(int argc,char **argv) {
 assert(argc==7);
 unsetenv("NXBOX_ASYNC_SUBMIT"); unsetenv("NXBOX_SO_NO_WAIT"); unsetenv("NXBOX_GPU_PROFILE");
 if(strcmp(argv[1],"unset")) setenv("NXBOX_ASYNC_SUBMIT",argv[1],1);
 if(strcmp(argv[2],"unset")) setenv("NXBOX_GPU_PROFILE",argv[2],1);
 setenv("NXBOX_SO_NO_WAIT",argv[3],1);
 sync_enabled=atoi(argv[4]);
 const bool drain=atoi(argv[5]),so=atoi(argv[6]);
 assert(nxbox_so_no_wait_enabled()==so);
 Device dev; Queue queue; Screen screen{&dev,&queue}; Batch batch; Context ctx;
 submit(&screen,&batch,&ctx);
 assert(waits==unsigned(drain) && signals==unsigned(drain));
 assert(collections==unsigned(drain) && completions==unsigned(drain));
 assert(!batch.has_errors && !stopped);
 assert(report.find(drain ? "submit_drain=1" : "async_submit=1")!=std::string::npos);
 if(drain) {
  queue.result=-1; submit(&screen,&batch,&ctx);
  assert(stopped && batch.has_errors && waits==1); // Failed Signal must still poison submission.
  stopped=false; batch.has_errors=false; queue.result=0; dev.removed=-2;
  submit(&screen,&batch,&ctx);
  assert(batch.has_errors && captures==2); // Device-loss handling survives gating.
 }
 nxbox_count_perf_wait(NxboxPerfWait::SoEnable);
 nxbox_count_perf_wait(NxboxPerfWait::SoDisable);
 assert(report.find("so_enable=1 so_disable=1")!=std::string::npos);
}
"""
        )
        cases = (
            ("unset", "unset", "1", "1", "0", "1"),
            ("0", "unset", "0", "1", "1", "0"),
            ("1", "1", "1", "1", "1", "1"),
            ("0", "1", "1", "0", "0", "1"),
        )
        for args in cases:
            with self.subTest(args=args):
                try:
                    runner.compile_run(code, args)
                except subprocess.CalledProcessError as error:
                    self.fail(error.stderr or str(error))

    def test_required_waits_and_so_gpu_ordering_survive(self):
        batch = (self.driver / "d3d12_batch.cpp").read_text()
        context = (self.driver / "d3d12_context.cpp").read_text()
        resource = (self.driver / "d3d12_resource.cpp").read_text()
        draw = (self.driver / "d3d12_draw.cpp").read_text()
        self.assertIn("nxbox_batch_wait(screen->dev, batch->fence->cmdqueue_fence,", batch)
        self.assertIn("d3d12_reset_batch(ctx, batch, OS_TIMEOUT_INFINITE)", batch)
        self.assertIn("unfenced = !nxbox_batch_wait", batch)
        self.assertIn("d3d12_foreach_submitted_batch(ctx, old_batch)", context)
        self.assertIn("d3d12_resource_wait_idle(ctx, res, usage & PIPE_MAP_WRITE);", resource)
        self.assertIn("d3d12_flush_cmdlist_and_wait(ctx);", resource)
        self.assertIn("nxbox_query_ready(", (self.driver / "d3d12_query.cpp").read_text())
        enable = context.split("d3d12_enable_fake_so_buffers(", 1)[1].split(
            "bool\nd3d12_disable_fake_so_buffers", 1
        )[0]
        disable = context.split("bool\nd3d12_disable_fake_so_buffers", 1)[1].split(
            "void\nd3d12_flush_cmdlist", 1
        )[0]
        self.assertIn("if (nxbox_so_no_wait_enabled())", enable)
        self.assertIn("else\n         d3d12_resource_wait_idle", enable)
        self.assertIn("else\n      d3d12_flush_cmdlist_and_wait", disable)
        self.assertLess(disable.index("launch_grid"), disable.index("pipe_so_target_reference"))
        self.assertIn("new_cs_ssbos[0].buffer = target->base.buffer;", disable)
        self.assertIn("new_cs_ssbos[1].buffer = fake_target->base.buffer;", disable)
        # The compute launch retains and transitions the SSBOs before dispatch.
        self.assertIn("d3d12_batch_reference_resource(batch, res,", draw)
        self.assertIn("D3D12_RESOURCE_STATE_UNORDERED_ACCESS", draw)
        self.assertIn(
            "NXBOX_D3D12_PERF_WAITS", (ROOT / "src/eden_uwp/game_session.cpp").read_text()
        )


if __name__ == "__main__":
    unittest.main()
