"""Add opt-in event-loop-owned session output to patched Aquamarine 0.15.1.

Apply after aquamarine-0.15.1-native-render.patch. Diagnostic behavior is retained.
No worker, hyprctl process, forced repaint, config replacement or input change.
"""
import argparse
import hashlib
from pathlib import Path

OBSERVER = '''    // Private diagnostic observer only. This does not add Windows presentation.
    if (NativeRender::requested() && (state->state().committed & COutputState::AQ_OUTPUT_STATE_BUFFER) && state->state().buffer) {
        using Observer = int (*)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
        const auto observer = reinterpret_cast<Observer>(dlsym(RTLD_DEFAULT, "wddm_bridge_compositor_commit"));
        if (observer && backend->backend->primaryAllocator && NativeRender::supported(backend->backend->primaryAllocator->drmFD())) {
            const auto attrs = state->state().buffer->dmabuf();
            if (attrs.success && attrs.planes == 1)
                observer(attrs.fds[0], attrs.size.x, attrs.size.y, attrs.format, attrs.strides[0], attrs.modifier);
        }
    }
'''
SESSION = '''    // WDDM_SESSION_OUTPUT: persistent native output; the compositor owns damage.
    using SessionMode = int (*)(uint32_t*, uint32_t*);
    const auto sessionMode = reinterpret_cast<SessionMode>(dlsym(RTLD_DEFAULT, "wddm_bridge_compositor_session_mode"));
    uint32_t sessionWidth = 0, sessionHeight = 0;
    const int session = sessionMode ? sessionMode(&sessionWidth, &sessionHeight) : 0;
    if (session < 0)
        return false;
    if (session && name == "WDDM-1" && (state->state().committed & COutputState::AQ_OUTPUT_STATE_BUFFER) && state->state().buffer) {
        using Observer = int (*)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t);
        const auto observer = reinterpret_cast<Observer>(dlsym(RTLD_DEFAULT, "wddm_bridge_compositor_commit"));
        if (!NativeRender::requested() || !observer || !backend->backend->primaryAllocator ||
            !NativeRender::supported(backend->backend->primaryAllocator->drmFD()))
            return false;
        const auto attrs = state->state().buffer->dmabuf();
        if (!attrs.success || attrs.planes != 1 || attrs.size.x != sessionWidth || attrs.size.y != sessionHeight ||
            observer(attrs.fds[0], attrs.size.x, attrs.size.y, attrs.format, attrs.strides[0], attrs.modifier))
            return false;
    }
    // Preserve the finite diagnostic observer only when no session is selected.
    if (!session) {
''' + ''.join('    '+line+'\n' for line in OBSERVER.rstrip('\n').split('\n')) + '''    }
'''
READY = '''void Aquamarine::CHeadlessBackend::onReady() {
    ;
}'''
SESSION_READY = '''void Aquamarine::CHeadlessBackend::onReady() {
    // WDDM_SESSION_READY: allocator and compositor listeners must exist first.
    using SessionMode = int (*)(uint32_t*, uint32_t*);
    const auto mode = reinterpret_cast<SessionMode>(dlsym(RTLD_DEFAULT, "wddm_bridge_compositor_session_mode"));
    uint32_t width = 0, height = 0;
    if (!NativeRender::requested() || !mode || mode(&width, &height) != 1)
        return;
    backend->addIdleEvent(makeShared<std::function<void(void)>>([weak = self]() {
        const auto headless = weak.lock();
        if (headless && !headless->createOutput("WDDM-1"))
            headless->backend->log(AQ_LOG_ERROR, "WDDM session output creation failed");
    }));
}'''
MODE = '    output->modes.emplace_back(SP<SOutputMode>(new SOutputMode(Vector2D{1920, 1080}, 60000, true)));'
SESSION_MODE = '''    // WDDM_SESSION_MODE: fixed native size; normal Hyprland config is retained.
    Vector2D nativeSize{1920, 1080};
    using SessionMode = int (*)(uint32_t*, uint32_t*);
    const auto mode = reinterpret_cast<SessionMode>(dlsym(RTLD_DEFAULT, "wddm_bridge_compositor_session_mode"));
    uint32_t width = 0, height = 0;
    if (outputName == "WDDM-1" && mode && mode(&width, &height) == 1)
        nativeSize = Vector2D{static_cast<double>(width), static_cast<double>(height)};
    output->modes.emplace_back(SP<SOutputMode>(new SOutputMode(nativeSize, 60000, true)));'''
PAIRS = ((OBSERVER,SESSION),(READY,SESSION_READY),(MODE,SESSION_MODE))

def patch_text(text):
    if 'WDDM_SESSION_OUTPUT' in text:
        if any(text.count(after)!=1 for _,after in PAIRS):
            raise ValueError('Incomplete or altered session adapter')
        return text
    for before,after in PAIRS:
        if text.count(before)!=1:
            raise ValueError('Aquamarine session source boundary changed')
        text=text.replace(before,after,1)
    return text

def apply(root):
    root=Path(root)
    if (root/'VERSION').read_text().strip()!='0.15.1':
        raise ValueError('Session adapter requires Aquamarine 0.15.1')
    source=root/'src/backend/Headless.cpp'
    pins={
        'cac6f66bca5333c2f66a2db2b11c9660e0800871bed3789ad0109120093996a4':
            'a36e55b192c1695a222e9dc13baaeabd6c20b1d1be77cdd56c70b4d5a74e42fe',
        '2e078a4923d9ffd076a52b50cce67f4bda2c2ff458d6ec55fc3d2ec85a373625':
            'ef35875ad513ac4b77166d2dfc4149d45fb25387989627f57ae439d17a665e36',
    }
    if source.is_symlink() or not source.resolve().is_relative_to(root.resolve()):
        raise ValueError('Aquamarine source escaped checkout')
    original=source.read_bytes()
    digest=hashlib.sha256(original).hexdigest()
    if digest in pins.values():
        if patch_text(original.decode()).encode()!=original:
            raise ValueError('Invalid complete adapter')
        return
    if digest not in pins:
        raise ValueError('Aquamarine source digest mismatch; no files changed')
    selected=patch_text(original.decode()).encode()
    if hashlib.sha256(selected).hexdigest()!=pins[digest]:
        raise ValueError('Aquamarine selected digest mismatch; no files changed')
    source.write_bytes(selected)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    apply(parser.parse_args().source)
