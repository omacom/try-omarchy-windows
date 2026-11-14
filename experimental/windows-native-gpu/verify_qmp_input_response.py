"""Audit the real application's post-input confirmation in complete NVIDIA backbuffers."""
import re

from verify_hyprland_lifecycle_capture import require
from verify_uwsm_native_input_devices import validate_gpu_input_discovery
from verify_qmp_input_delivery import delivery_metadata, snapshot

GUEST_SOURCES = {
    'hyprland_startup_probe.c': '6326473cbb9d73337ad58435fd9c717050e15c019248ab4a8b1302384c48cbf3',
    'gpu-terminal-scene.sh': 'ee614b91eb045c7e2dc34457118ccbbe8d90898285ff69e2818753423897a9ff',
}


def confirmation_pixels(raw, client):
    require(len(raw) == 3840 * 2160 * 4, 'Wrong complete backbuffer extent')
    at, size = client.get('at'), client.get('size')
    require(isinstance(at, list) and isinstance(size, list) and len(at) == len(size) == 2 and
            all(type(value) is int for value in at + size) and size[0] > 32 and size[1] > 32 and
            at[0] >= 0 and at[1] >= 0 and at[0] + size[0] <= 3840 and at[1] + size[1] <= 2160,
            'Invalid authenticated application bounds')
    x0, y0 = at[0] + 16, at[1] + 16
    x1, y1 = at[0] + size[0] - 16, at[1] + size[1] - 16
    count = 0
    rows = []
    left, top, right, bottom = 3840, 2160, -1, -1
    # Independent true-color background oracle. The actual PTY reader prints
    # this only after receiving and checking the complete token. Borders,
    # notification overlays and wallpaper lie outside this interior region.
    for y in range(y0, y1):
        start, end = (y * 3840 + x0) * 4, (y * 3840 + x1) * 4
        matches = [x0 + x for x, (b, g, r) in enumerate(zip(raw[start:end:4], raw[start + 1:end:4], raw[start + 2:end:4]))
                   if abs(r - 32) <= 6 and abs(g - 160) <= 6 and abs(b - 96) <= 6]
        if matches:
            count += len(matches)
            left, right = min(left, matches[0]), max(right, matches[-1])
            top, bottom = min(top, y), max(bottom, y)
            if len(matches) >= 150:
                rows.append(y)
    return dict(pixels=count, bounds=[left, top, right + 1, bottom + 1] if count else None, denseRows=len(rows))


def response_metadata(guest, frames):
    barriers = re.findall(r'^OMARCHY_NATIVE_INPUT_RESPONSE_BARRIER receiptCommit=(\d+) observedCommit=(\d+) queuedFrames=(\d+) gpuProof=false$', guest, re.M)
    require(len(barriers) == 1, 'Missing/duplicate post-input compositor barrier')
    receipt, observed, queued = map(int, barriers[0])
    require(receipt > 0 and observed >= receipt + 3 and queued == 2, 'The final handoffs did not wait for the application response')
    require(guest.index('OMARCHY_NATIVE_INPUT_PTY_HEX ') < guest.index('OMARCHY_NATIVE_INPUT_RESPONSE_BARRIER ') <
            guest.index('HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=3 '), 'Post-input GPU handoffs precede the PTY/compositor barrier')
    clients = snapshot(guest, 'OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX')
    require(isinstance(clients, list) and len(clients) == 1, 'Missing unique GPU application for the response')
    before = confirmation_pixels(frames['compositor-copy-1.raw'], clients[0])
    after = confirmation_pixels(frames['compositor-copy-2.raw'], clients[0])
    require(before['pixels'] == 0 and after['pixels'] >= 3000 and after['denseRows'] >= 10 and
            300 <= after['bounds'][2] - after['bounds'][0] <= 1000 and
            12 <= after['bounds'][3] - after['bounds'][1] <= 64,
            'The final GPU backbuffer does not show the actual PTY confirmation band')
    return dict(diagnosticResponseRenderedVerified=True, firstFrameConfirmationPixels=0,
                finalFrameConfirmationPixels=after['pixels'], confirmationBounds=after['bounds'],
                postReceiptCompositorCommits=observed - receipt, heldDiagnosticHandoffs=True,
                exactGlyphOcrVerified=False, continuousDisplayVerified=False)


def validate_qmp_input_response(report, guest, records, frames, source, wallpaper_reference, owner_output):
    capture = validate_gpu_input_discovery(report, guest, records, frames, source, wallpaper_reference)
    delivery = delivery_metadata(guest, records, source, owner_output, guest_sources=GUEST_SOURCES)
    response = response_metadata(guest, frames)
    return dict(capture=capture, inputDelivery=delivery, inputResponse=response,
                gpuCaptureWithQmpInputResponseVerified=True, hostWindowIntegrationVerified=False,
                normalLauncherVerified=False, fullDesktopAccepted=False, allFramesDisplayedVerified=False,
                nearNativePerformanceVerified=False)
