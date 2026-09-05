package main

// Guest sizing rules. They are pure so the platform-independent tests cover
// them; main.go feeds them the live host numbers.

const (
	// A guest below two vCPUs makes Hyprland stutter on every input; above
	// eight the scheduling overhead under WHPX outweighs what a desktop uses.
	minimumAutoGuestCPUs = 2
	maximumAutoGuestCPUs = 8
	// Settings and -cpus accept an explicit count in this range.
	minimumGuestCPUs = 1
	maximumGuestCPUs = 64
	// Automatic RAM: a third of the machine, clamped so a small laptop still
	// gets a working guest and a workstation does not commit half its memory
	// to a trial. GPU mode adds room for the guest side of blob resources.
	minimumAutoGuestMemMiB = 4096
	maximumAutoGuestMemMiB = 8192
	gpuGuestMemExtraMiB    = 2048
	maximumAutoGPUMemMiB   = 12288
	// Windows keeps this much of what is available; the guest takes the rest.
	hostMemReserveMiB = 2048
	guestMemFloorMiB  = 1024
)

// pickGuestCPUs leaves two logical processors for Windows and the launcher,
// gives small machines two vCPUs anyway (the host shares them), and caps the
// count where more stops helping a desktop.
func pickGuestCPUs(hostLogical int) int {
	if hostLogical <= 0 {
		return minimumAutoGuestCPUs
	}
	n := hostLogical - 2
	if n < minimumAutoGuestCPUs {
		n = minimumAutoGuestCPUs
		if hostLogical < n {
			n = hostLogical
		}
	}
	if n > maximumAutoGuestCPUs {
		n = maximumAutoGuestCPUs
	}
	return n
}

// pickGuestMemMiB sizes the guest to the machine instead of demanding a
// fixed 4-6 GB (which dies with "cannot set up guest memory" on a busy 8 GB
// PC). totalMiB and availMiB are the host's physical and available memory;
// zero means the query failed and only the ideal applies.
func pickGuestMemMiB(gpu bool, totalMiB, availMiB int) int {
	want := minimumAutoGuestMemMiB
	if totalMiB > 0 {
		want = totalMiB / 3
		if want < minimumAutoGuestMemMiB {
			want = minimumAutoGuestMemMiB
		}
		if want > maximumAutoGuestMemMiB {
			want = maximumAutoGuestMemMiB
		}
	}
	if gpu {
		want += gpuGuestMemExtraMiB
		if want > maximumAutoGPUMemMiB {
			want = maximumAutoGPUMemMiB
		}
	}
	if availMiB == 0 {
		return want
	}
	m := availMiB - hostMemReserveMiB
	if m > want {
		m = want
	}
	if m < guestMemFloorMiB {
		m = guestMemFloorMiB
	}
	return m
}

// gpuHostMem sizes the host-side memory virtio-gpu can map for blob
// resources. It scales with the guest and, on machines with plenty of RAM,
// grows so large displays with many windows do not run out.
func gpuHostMem(memMiB, hostTotalMiB int) string {
	switch {
	case memMiB < 3072:
		return "1G"
	case memMiB < 4096:
		return "2G"
	case memMiB >= 8192 && hostTotalMiB >= 32768:
		return "8G"
	}
	return "4G"
}
