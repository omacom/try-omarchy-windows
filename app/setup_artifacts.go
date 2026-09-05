package main

const runtimeZip = "winq-emu-alpha10-portable.zip"

var (
	downloadedGuestArtifacts = []string{"guest-manifest.json", "build-spec.json", "vmlinuz-linux", "initramfs-linux.img"}
	installedGuestArtifacts  = []string{"build-spec.json", "vmlinuz-linux", "initramfs-linux.img", "rootfs.ext4"}
)
