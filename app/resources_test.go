package main

import "testing"

func TestPickGuestCPUsLeavesTwoForWindowsWithinBounds(t *testing.T) {
	for host, want := range map[int]int{0: 2, 1: 1, 2: 2, 4: 2, 6: 4, 8: 6, 10: 8, 12: 8, 32: 8} {
		if got := pickGuestCPUs(host); got != want {
			t.Errorf("host %d: got %d vCPUs, want %d", host, got, want)
		}
	}
}

func TestPickGuestMemScalesWithTheMachine(t *testing.T) {
	cases := []struct {
		name         string
		gpu          bool
		total, avail int
		want         int
	}{
		{"unknown host", false, 0, 0, 4096},
		{"unknown host gpu", true, 0, 0, 6144},
		{"8 GB laptop busy", false, 8192, 5000, 2952},
		{"8 GB laptop idle", false, 8192, 7000, 4096},
		{"16 GB laptop", false, 16384, 12000, 5461},
		{"16 GB laptop gpu", true, 16384, 12000, 7509},
		{"32 GB desktop", false, 32768, 28000, 8192},
		{"32 GB desktop gpu", true, 32768, 28000, 10240},
		{"64 GB workstation gpu", true, 65536, 60000, 10240},
		{"starved", false, 8192, 1500, 1024},
	}
	for _, c := range cases {
		if got := pickGuestMemMiB(c.gpu, c.total, c.avail); got != c.want {
			t.Errorf("%s: got %d MiB, want %d", c.name, got, c.want)
		}
	}
}

func TestGPUHostMemFollowsGuestAndHost(t *testing.T) {
	for _, c := range []struct {
		mem, total int
		want       string
	}{{2048, 8192, "1G"}, {3072, 8192, "2G"}, {4096, 16384, "4G"}, {8192, 16384, "4G"}, {8192, 32768, "8G"}, {10240, 65536, "8G"}} {
		if got := gpuHostMem(c.mem, c.total); got != c.want {
			t.Errorf("mem %d total %d: got %s, want %s", c.mem, c.total, got, c.want)
		}
	}
}
