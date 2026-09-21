package main

// Ordinary desktop shortcuts open the launcher. Commands specifying runtime
// options keep their existing unattended behavior; -launcher opts them in.
func shouldOpenLauncher(flags map[string]bool, launcher, start bool) bool {
	if start {
		return false
	}
	for name := range flags {
		switch name {
		case "dir", "portable", "launcher":
		default:
			if !launcher {
				return false
			}
		}
	}
	return true
}
