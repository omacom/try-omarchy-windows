"""Keep boot VGA out of an active native compositor session's renderer factory.

Apply after the native-render, native-input and persistent-session adapters.
The exact pinned backend factory is changed; seat/input sources are untouched.
"""
import argparse
import hashlib
from pathlib import Path

ORIGINAL = '74d4cba8989336a259026d8c96694bf406d7d56aec820e35b9c95ce291ff9e80'
SELECTED = '37fd76357fa283b5872488d6a03300f1fa48ccfab3c75f38085193c406213b0d'
HEADLESS = {
    'a36e55b192c1695a222e9dc13baaeabd6c20b1d1be77cdd56c70b4d5a74e42fe',
    'ef35875ad513ac4b77166d2dfc4149d45fb25387989627f57ae439d17a665e36',
}
BEFORE = '    for (auto const& b : backends) {\n'
AFTER = '''    // WDDM_SESSION_BACKEND_SELECTION: boot VGA is never a desktop renderer.
    bool nativeSession = false;
    if (NativeRender::requested()) {
        using SessionMode = int (*)(uint32_t*, uint32_t*);
        const auto mode = reinterpret_cast<SessionMode>(dlsym(RTLD_DEFAULT, "wddm_bridge_compositor_session_mode"));
        uint32_t width = 0, height = 0;
        const int selected = mode ? mode(&width, &height) : 0;
        if (selected < 0 || selected > 1 ||
            (selected == 1 && !((width == 1920 && height == 1080) || (width == 3840 && height == 2160)))) {
            backend->log(AQ_LOG_CRITICAL, "Native compositor session selection failed");
            return nullptr;
        }
        nativeSession = selected == 1;
        if (nativeSession) {
            const auto headless = std::find_if(backends.begin(), backends.end(), [](const auto& option) { return option.backendType == AQ_BACKEND_HEADLESS; });
            if (std::count_if(backends.begin(), backends.end(), [](const auto& option) { return option.backendType == AQ_BACKEND_HEADLESS; }) != 1 ||
                headless->backendRequestMode != AQ_BACKEND_REQUEST_MANDATORY) {
                backend->log(AQ_LOG_CRITICAL, "Native compositor session requires one mandatory output backend");
                return nullptr;
            }
        }
    }

    for (auto const& b : backends) {
        if (nativeSession && b.backendType != AQ_BACKEND_HEADLESS)
            continue;
'''


def patch_text(text):
    if 'WDDM_SESSION_BACKEND_SELECTION' in text:
        if text.count(AFTER) != 1:
            raise ValueError('Incomplete or altered native session selection')
        return text
    if text.count(BEFORE) != 1:
        raise ValueError('Aquamarine backend factory boundary changed')
    return text.replace(BEFORE, AFTER, 1)


def apply(root):
    root = Path(root)
    paths = [root/'VERSION', root/'src/backend/Headless.cpp', root/'src/backend/Backend.cpp']
    if root.is_symlink() or any(path.is_symlink() or not path.resolve().is_relative_to(root.resolve()) for path in paths):
        raise ValueError('Aquamarine selection source escaped checkout')
    version, headless, source = paths
    if version.read_bytes().strip() != b'0.15.1' or hashlib.sha256(headless.read_bytes()).hexdigest() not in HEADLESS:
        raise ValueError('Selection requires pinned Aquamarine 0.15.1 and complete persistent-session adapter')
    original = source.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    if digest == SELECTED:
        if patch_text(original.decode()).encode() != original:
            raise ValueError('Invalid complete native session selection')
        return
    if digest != ORIGINAL:
        raise ValueError('Aquamarine backend digest mismatch; no files changed')
    selected = patch_text(original.decode()).encode()
    if hashlib.sha256(selected).hexdigest() != SELECTED:
        raise ValueError('Aquamarine selection digest mismatch; no files changed')
    source.write_bytes(selected)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    apply(parser.parse_args().source)
