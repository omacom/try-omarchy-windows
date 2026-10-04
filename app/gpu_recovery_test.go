package main

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestGPUSilenceAllowsHeavyLoadAndResets(t *testing.T) {
	var s qmpSilence
	for i := 1; i < qmpHangMisses; i++ {
		if s.probe() {
			t.Fatalf("hang on probe %d before the conservative threshold", i)
		}
	}
	s.answered()
	for i := 1; i < qmpHangMisses; i++ {
		if s.probe() {
			t.Fatalf("previous silence carried into probe %d", i)
		}
	}
	if !s.probe() {
		t.Fatal("continuous silence did not trigger recovery")
	}
	if (qmpHangMisses-1)*5 < 90 {
		t.Fatal("threshold allows less than 90 seconds between unanswered probes")
	}
}

func TestGPUFreezeCountingAndSuggestion(t *testing.T) {
	for _, mode := range []string{renderAuto, renderGPU, renderCPU} {
		t.Run(mode, func(t *testing.T) {
			var p gpuFreezes
			for _, observation := range []struct{ gpu, ready bool }{{true, false}, {false, true}, {false, false}} {
				next, suggest := p.observe(mode, observation.gpu, observation.ready, "runtime", "drivers")
				if next != p || suggest {
					t.Fatalf("non post-ready GPU failure counted: %+v", next)
				}
			}
			p, suggest := p.observe(mode, true, true, "runtime", "drivers")
			want := 1
			if mode == renderCPU {
				want = 0
			}
			if p.Count != want || suggest {
				t.Fatalf("first freeze: %+v suggest=%v", p, suggest)
			}
			p, suggest = p.observe(mode, true, true, "runtime", "drivers")
			if suggest != (mode == renderAuto) {
				t.Fatalf("second freeze suggestion in %s = %v", mode, suggest)
			}
			for _, identity := range []struct{ runtime, drivers string }{{"new-runtime", "drivers"}, {"runtime", "new-drivers"}} {
				next, suggest := p.observe(mode, true, true, identity.runtime, identity.drivers)
				if next.Count != want || suggest {
					t.Fatalf("changed stack retained freeze count: %+v", next)
				}
			}
			for _, identity := range []struct{ runtime, drivers string }{{"", "drivers"}, {"runtime", ""}} {
				next, suggest := p.observe(mode, true, true, identity.runtime, identity.drivers)
				if next != p || suggest {
					t.Fatalf("unknown stack counted: %+v", next)
				}
			}
		})
	}
}

func TestGPUFreezeHistorySurvivesLauncherRestart(t *testing.T) {
	dir := t.TempDir()
	for i := 1; i <= 3; i++ {
		suggest, err := recordGPUFreeze(dir, renderAuto, true, true, "runtime", "drivers")
		if err != nil {
			t.Fatal(err)
		}
		if suggest != (i >= 2) {
			t.Fatalf("freeze %d suggestion=%v", i, suggest)
		}
	}
	p, err := loadGPUFreezes(dir)
	if err != nil || p.Count != 2 {
		t.Fatalf("history=%+v err=%v", p, err)
	}
	suggest, err := recordGPUFreeze(dir, renderAuto, true, true, "runtime", "new-drivers")
	if err != nil || suggest {
		t.Fatalf("driver update suggestion=%v err=%v", suggest, err)
	}
}

func TestGPURecoveryOnlyExplicitAlwaysPersists(t *testing.T) {
	for _, mode := range []string{renderAuto, renderGPU, renderCPU} {
		for _, action := range []int{0, gpuRecoveryCPUOnce, gpuRecoveryGPU, gpuRecoveryClose, gpuRecoveryCPUAlways, 99} {
			dir := t.TempDir()
			path := settingsPath(dir)
			original := settings{Render: mode, MemoryMiB: 8192, Share: "keep this", CPUs: 4}
			if err := saveSettings(path, original); err != nil {
				t.Fatal(err)
			}
			before, _ := os.ReadFile(path)
			decision := decideGPURecovery(action)
			if err := saveGPURecoveryChoice(dir, decision); err != nil {
				t.Fatal(err)
			}
			after, _ := os.ReadFile(path)
			if action != gpuRecoveryCPUAlways && string(before) != string(after) {
				t.Fatalf("mode=%s action=%d changed Settings", mode, action)
			}
			s, err := loadSettings(path)
			if err != nil {
				t.Fatal(err)
			}
			if s.MemoryMiB != 8192 || s.Share != "keep this" || s.CPUs != 4 {
				t.Fatal("unrelated settings changed")
			}
			if action == gpuRecoveryCPUAlways && s.Render != renderCPU {
				t.Fatal("Always choice not saved")
			}
			if decision.restart != (action == gpuRecoveryCPUOnce || action == gpuRecoveryGPU || action == gpuRecoveryCPUAlways) {
				t.Fatalf("wrong restart decision: %+v", decision)
			}
			if decision.gpu != (action == gpuRecoveryGPU) || decision.temporaryCPU != (action == gpuRecoveryCPUOnce) {
				t.Fatalf("wrong renderer decision: %+v", decision)
			}
			if shouldRecordRenderResult("runtime", mode, false, decision.temporaryCPU) && action == gpuRecoveryCPUOnce {
				t.Fatal("one-time CPU launch would overwrite render probe")
			}
		}
	}
}

func TestGPUOnceDoesNotMakeAutoRememberCPU(t *testing.T) {
	dir := t.TempDir()
	p := renderProbe{Result: renderGPU, RuntimeID: "runtime", DisplayDriver: "drivers", RecordedAt: time.Now()}
	if err := saveRenderProbe(dir, p); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(filepath.Join(dir, renderProbeFilename))
	decision := decideGPURecovery(gpuRecoveryCPUOnce)
	if err := saveGPURecoveryChoice(dir, decision); err != nil {
		t.Fatal(err)
	}
	if shouldRecordRenderResult("runtime", renderAuto, false, decision.temporaryCPU) {
		t.Fatal("one-time CPU result must be skipped")
	}
	after, _ := os.ReadFile(filepath.Join(dir, renderProbeFilename))
	if string(before) != string(after) {
		t.Fatal("GPU probe changed")
	}
	saved, err := loadRenderProbe(dir)
	if err != nil {
		t.Fatal(err)
	}
	gpu, _ := startWithGPU(renderAuto, saved, "runtime", "drivers", time.Now())
	if !gpu {
		t.Fatal("next independent Auto launch must still try GPU")
	}
}
