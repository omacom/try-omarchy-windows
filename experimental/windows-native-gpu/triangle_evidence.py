"""Independent integer rasterization oracle for the shader diagnostic.

Pixel centers and the three screen-space vertices are represented in eighths
of a pixel. No pixel center lies exactly on an edge for this 130x73 viewport.
Coverage is exact; only interpolated RGB permits a two-UNORM-step error.
"""
import re

WIDTH, HEIGHT, ROUNDS, TOLERANCE = 130, 73, 2, 2
SHADERS = ('2288', '1720', '3e5ff943b07f0f53', '86df59ba24d3b394')


def checksum(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def triangle_reference(round_):
    if round_ not in (1, 2):
        raise ValueError('The shader diagnostic has exactly two rounds')
    pixels, coverage = bytearray(), []
    ax, ay, bx, by, cx, cy = 130, 511, 520, 73, 910, 511
    area = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            px, py = 8 * x + 4, 8 * y + 4
            a = (by - cy) * (px - cx) + (cx - bx) * (py - cy)
            b = (cy - ay) * (px - cx) + (ax - cx) * (py - cy)
            weights = (a, b, area - a - b)
            if 0 in weights:
                raise ValueError('Ambiguous edge sample in the diagnostic geometry')
            inside = all(w > 0 for w in weights)
            rgba = [0, 0, 255 if round_ == 1 else 0, 255]
            if inside:
                for vertex, weight in enumerate(weights):
                    rgba[(vertex + round_ - 1) % 3] = (255 * weight + area // 2) // area
            pixels.extend(rgba)
            coverage.append(inside)
    return bytes(pixels), coverage


def triangle_readback(log):
    """Return only the actual exported GPU bytes, requiring every row in order."""
    rows = re.findall(r'^GPU_TRIANGLE_PIXEL_ROW round=(\d+) y=(\d+) rgba=([0-9a-f]+)[ \t\r]*$', log, re.MULTILINE)
    if len(rows) != ROUNDS * HEIGHT:
        return None
    frames = []
    for round_ in range(1, ROUNDS + 1):
        frame = bytearray()
        for y, (r, row, rgba) in enumerate(rows[(round_ - 1) * HEIGHT:round_ * HEIGHT]):
            if (r, row) != (str(round_), str(y)) or len(rgba) != WIDTH * 8:
                return None
            frame.extend(bytes.fromhex(rgba))
        frames.append(bytes(frame))
    return frames


def triangle_workload_complete(log, asynchronous_commands_verified=False):
    def matches(pattern):
        return re.findall(r'^' + pattern + r'[ \t\r]*$', log, re.MULTILINE)
    if matches(r'GPU_TRIANGLE_TEST_BEGIN width=(\d+) height=(\d+) format=(\w+) rounds=(\d+) vertices=(\d+) tolerance=(\d+)') != [('130','73','R8G8B8A8_UNORM','2','3','2')]:
        return False
    if matches(r'GPU_TRIANGLE_TEST_COMPLETE verified=true width=(\d+) height=(\d+) rounds=(\d+)') != [('130','73','2')]:
        return False
    if matches(r'GPU_TRIANGLE_SHADERS vertexBytes=(\d+) pixelBytes=(\d+) vertexHash=([0-9a-f]{16}) pixelHash=([0-9a-f]{16})') != [SHADERS]:
        return False
    tail = log.split('GPU_TRIANGLE_TEST_BEGIN', 1)[1]
    if ('GPU_TRIANGLE_TIMEOUT' in tail or 'GPU_TRIANGLE_MISMATCH' in tail or 'gpuTriangleDeviceRemoved=' in tail or
        not (tail.count('nativeCommandSubmitted=true') >= 2 or
             (asynchronous_commands_verified and tail.count('nativeCommandQueued=true') >= 2))):
        return False
    for stage, count in (('DirectQueue',1), ('SerializeRootSignature',1), ('RootSignature',1), ('Pipeline',1),
                         ('RenderTarget',1), ('Readback',1), ('RtvHeap',1), ('Allocator',1), ('CommandList',1),
                         ('Fence',1), ('Close',2), ('Signal',2), ('ReadbackMap',2), ('AllocatorReset',1), ('CommandListReset',1)):
        if matches(r'gpuTriangle' + stage + r'=([0-9a-f]{8})') != ['00000000'] * count:
            return False
    footprints = matches(r'GPU_TRIANGLE_FOOTPRINT width=(\d+) height=(\d+) offset=(\d+) rowPitch=(\d+) rowBytes=(\d+) rows=(\d+) totalBytes=(\d+)')
    if len(footprints) != 1:
        return False
    width, height, offset, pitch, row_bytes, rows, total = map(int, footprints[0])
    if (width, height, offset, row_bytes, rows) != (130, 73, 512, 520, 73) or not 520 <= pitch <= 4096 or pitch % 256 or not offset + 72 * pitch + row_bytes <= total <= 1048576:
        return False
    if matches(r'GPU_TRIANGLE_DRAW round=(\d+) vertices=(\d+) startVertex=(\d+) instances=(\d+) colorRotation=(\d+)') != [('1','3','0','1','0'), ('2','3','0','1','1')]:
        return False
    rounds = matches(r'GPU_TRIANGLE_ROUND round=(\d+) verifiedPixels=(\d+) trianglePixels=(\d+) backgroundPixels=(\d+) maxChannelError=(\d+) referenceHash=([0-9a-f]{16}) observedHash=([0-9a-f]{16}) fenceTarget=(\d+) fenceObserved=(\d+)')
    frames = triangle_readback(log)
    if len(rounds) != ROUNDS or frames is None or frames[0] == frames[1]:
        return False
    for round_, (record, actual) in enumerate(zip(rounds, frames), 1):
        r, verified, triangle, background, error, reference_hash, actual_hash, target, retired = record
        expected, coverage = triangle_reference(round_)
        maximum_error = 0
        for pixel, inside in enumerate(coverage):
            for channel in range(4):
                difference = abs(expected[pixel * 4 + channel] - actual[pixel * 4 + channel])
                maximum_error = max(maximum_error, difference)
                if difference > (TOLERANCE if inside and channel < 3 else 0):
                    return False
        if ((int(r), int(verified), int(triangle), int(background), int(error)) !=
                (round_, WIDTH * HEIGHT, sum(coverage), WIDTH * HEIGHT - sum(coverage), maximum_error) or
            reference_hash != checksum(expected) or actual_hash != checksum(actual) or
            int(target) != round_ or not round_ <= int(retired) < (1 << 64) - 1):
            return False
        draw_position = log.index(f'GPU_TRIANGLE_DRAW round={round_}')
        round_position = log.index(f'GPU_TRIANGLE_ROUND round={round_}')
        first_row = log.index(f'GPU_TRIANGLE_PIXEL_ROW round={round_} y=0 ')
        last_row = log.index(f'GPU_TRIANGLE_PIXEL_ROW round={round_} y=72 ')
        end = log.index('GPU_TRIANGLE_DRAW round=2') if round_ == 1 else log.index('GPU_TRIANGLE_TEST_COMPLETE')
        if not draw_position < round_position < first_row < last_row < end:
            return False
    # Reject unparsed diagnostic lines, malformed payloads, and hidden failures.
    diagnostic = [line for line in log.splitlines() if line.startswith(('GPU_TRIANGLE_', 'gpuTriangle'))]
    return (len(diagnostic) == 172 and diagnostic[0].startswith('GPU_TRIANGLE_TEST_BEGIN ') and
            diagnostic[-1].startswith('GPU_TRIANGLE_TEST_COMPLETE '))  # 18 HRESULTs, 146 rows, 8 workload records.
