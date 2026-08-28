# Handoff — tryomarchy.com landing page (separate session)

You're building the landing page for Try Omarchy for Windows. This file is
self-contained: read it plus README.md and you have everything. The product
lives at https://github.com/tsouth89/try-omarchy-windows (Brandon's personal
account tsouth89 — `gh auth switch --user tsouth89` before any push there,
back to bts-cssi after).

## What the product is (one breath)

A native Windows app that boots the full Omarchy Linux desktop (Hyprland) in a
window. No VMware, no VirtualBox, no dual boot, no WSL — QEMU on the Windows
Hypervisor Platform, GPU-accelerated graphics (virgl + Venus Vulkan), a real
Arch/Omarchy image. Download, launch, you're in Hyprland. Nothing touches your
disk layout.

## Audience and job

Windows users who are Omarchy-curious (mostly from DHH's orbit / X) and want to
feel the desktop before committing to a real install. The page has ONE job:
get them to download and launch. Secondary: make the project look credible
enough that Eduardo/Jorge/cmspam want to collaborate.

## Claims that are TRUE and provable (use freely)

- Full Omarchy 4.0.1 desktop — the real thing, not a demo reel (all 22 themes,
  screensavers, the whole Hyprland experience)
- GPU-accelerated (virgl + Venus Vulkan via WINQ-EMU); smooth YouTube playback
  verified on hardware
- Works on Windows 11 Home AND Pro (WHPX — same platform WSL2 rides on; no
  Hyper-V role needed)
- Boots to the desktop in seconds (6.8s to graphical.target on a mid-range
  Ryzen 5 laptop; don't promise a universal number)
- Setup once, then every launch goes straight to the desktop — no Linux login
  screens, no console text, branded window
- Clipboard works both ways; share a Windows folder into the guest (-Share)
- Your disk is untouched: the whole Linux system is one file you can delete
- Free, MIT, open source; image build is reproducible and the recipe is public
- ~1.4 GB download during setup (the installer itself is tiny)

## Claims NOT to make (yet or ever)

- NOT official / NOT affiliated with Omarchy, Basecamp, DHH, or 37signals.
  Credit them, link omarchy.org, and say "unofficial" somewhere visible. Do not
  reuse the Omarchy logo as OUR logo or imitate omarchy.org's design/branding.
- No signed installer claim until the signed build actually ships (Azure
  Trusted Signing is set up but the cert profile + signed release don't exist
  yet).
- Don't promise Windows 10 (only 11 is tested), don't promise specific FPS.
- Current release is a developer preview driven by PowerShell scripts — the
  page can (and should) go up before the polished app, but the download section
  must be honest about that until the native app ships.

## Page structure that fits (adapt, don't worship)

1. Hero: name, one-liner, a real screenshot or short screen recording of
   Hyprland running in the branded window, download button, "Windows 11 Home &
   Pro · free · open source · your disk stays untouched"
2. 15–30s capture loop (theme switch, Super+Space menu, a screensaver kicking
   in — the flashy bits)
3. Three-up: No install commitment / Real GPU speed / The real Omarchy
4. How it works (short, honest, links to FINDINGS for nerds)
5. FAQ: is this official? (no — credits), is it safe? (one file, delete it),
   Home vs Pro, specs needed, how do I really install Omarchy (link
   omarchy.org — being a good citizen here IS the pitch to upstream)
6. Footer: GitHub, credits (Omarchy/DHH, themartiano, jorge-huxley, cmspam,
   Chainfire, dockur), MIT

## Copy voice (non-negotiable)

Page copy must read like Brandon wrote it: plain sentences, casual-professional,
lean and short. No em dashes or en dashes (hyphens fine), no "neither X nor Y",
no bullet-essay-with-bold-lead-ins on the page itself, no hedging filler, no
marketing slop ("unleash", "seamless", "revolutionary"). Short verbs win.

## Assets — what exists and what's missing

- MISSING: real screenshots/recordings. The only good captures live on the
  Windows laptop (GPU build). Get: hero shot of the desktop in the "Try
  Omarchy" window over a Windows desktop, theme-switch clip, screensaver clip.
  Ask Brandon or grab them during the laptop validation pass (HANDOFF.md).
  QMP screendump does NOT work on the GL path — capture with Windows tooling
  (Win+Shift+S / Xbox Game Bar) or ship -NoGpu shots as placeholders.
- MISSING: a logo/wordmark of our own. Keep it simple; don't derive it from
  Omarchy's logo.
- Release asset URLs are stable: latest is
  https://github.com/tsouth89/try-omarchy-windows/releases/tag/v0.0.2-preview

## Tech and hosting

- Static site, no framework needed. One HTML page + assets is fine; Astro if
  you want components. Must look great in dark mode (the audience lives there).
- Suggested host: Cloudflare Pages or GitHub Pages from a `site/` dir or a
  separate repo — Brandon's call, ask once at the start, then build.
- Domain: tryomarchy.com — CONFIRM Brandon actually owns it before wiring DNS;
  registering it (if needed) and DNS are his manual steps, prepare the exact
  records to paste.
- Basics: OG/Twitter card meta (the announcement thread will carry the link),
  favicon, single page, no trackers or at most a privacy-friendly counter.

## Definition of done

A deployed page Brandon can put in his X bio and pin under the announcement
thread: hero + capture + honest download section pointing at the latest GitHub
release, credits and "unofficial" disclosure present, copy in Brandon's voice,
loads fast, looks right on phone and desktop, dark mode first.
