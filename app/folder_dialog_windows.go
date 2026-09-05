//go:build windows

package main

var procCoTaskMemFree = ole32.NewProc("CoTaskMemFree")

func browseForFolder(owner uintptr, prompt string) (string, bool) {
	selected, ok, err := chooseRecoveryPath(owner, prompt, "", false, true)
	if err != nil {
		errorBox("Windows could not open the folder picker.\n\n" + err.Error())
	}
	return selected, ok
}
