"""Read-only structural prerequisites for the input-free, single-output 4K fixture.

Candidates still require visual inspection. This module cannot accept a healthy
desktop, host driver stability, physical input or daily use. It opens no devices,
starts no processes and sends no input.
"""
import json

MAX_METADATA_BYTES = 4 * 1024 * 1024


def _reject_constant(value):
    raise ValueError('Non-finite JSON constant: ' + value)


def captured_scene_tables(text):
    marker = 'NORMAL_FRAME_CAPTURED\n'
    if (type(text) is not str or len(text) > MAX_METADATA_BYTES or
            len(text.encode('utf-8')) > MAX_METADATA_BYTES or text.count(marker) != 1):
        raise ValueError('Capture metadata marker/bounds')
    remaining = text.split(marker, 1)[1]
    tables = []
    decoder = json.JSONDecoder(parse_constant=_reject_constant)
    try:
        for _ in range(3):
            remaining = remaining.lstrip()
            value, count = decoder.raw_decode(remaining)
            tables.append(value)
            remaining = remaining[count:]
    except RecursionError as error:
        raise ValueError('Capture metadata nesting limit') from error
    if remaining.strip():
        raise ValueError('Unexpected capture metadata suffix')
    return tables


def assess_scene_metadata(monitors, clients, layers, terminal_pid, shell_ok):
    if type(terminal_pid) is not int or terminal_pid <= 0 or type(shell_ok) is not bool:
        raise ValueError('Scene identity types')
    if not isinstance(monitors, list) or len(monitors) != 1 or not isinstance(monitors[0], dict):
        raise ValueError('One output required')
    monitor = monitors[0]
    name = monitor.get('name')
    if (type(monitor.get('width')) is not int or monitor['width'] != 3840 or
            type(monitor.get('height')) is not int or monitor['height'] != 2160 or
            not isinstance(name, str) or not name.strip() or len(name) > 128):
        raise ValueError('One named 4K output required')
    scale = monitor.get('scale', 1)
    if type(scale) not in (int, float) or not 0 < scale <= 8:
        raise ValueError('Invalid output scale')
    if (not isinstance(clients, list) or len(clients) > 128 or
            any(not isinstance(client, dict) for client in clients)):
        raise ValueError('Client metadata bounds')
    if not isinstance(layers, dict) or not isinstance(layers.get(name), dict):
        raise ValueError('Output layer metadata required')
    levels = layers[name].get('levels')
    if not isinstance(levels, dict) or set(levels) != {'0', '1', '2', '3'}:
        raise ValueError('Layer levels required')
    for level in levels.values():
        if (not isinstance(level, list) or len(level) > 128 or
                any(not isinstance(layer, dict) for layer in level)):
            raise ValueError('Layer metadata bounds')
    width, height = 3840 / scale, 2160 / scale

    def geometry(layer):
        namespace = layer.get('namespace')
        w, h = layer.get('w'), layer.get('h')
        if (not isinstance(namespace, str) or not namespace.strip() or len(namespace) > 256 or
                type(w) not in (int, float) or type(h) not in (int, float) or
                not 0 < w <= 131072 or not 0 < h <= 131072):
            return None
        return w, h

    backgrounds = sum(bool(size and size[0] >= width / 2 and size[1] >= height / 2)
                      for layer in levels['0'] if (size := geometry(layer)))
    panels = sum(bool(size and size[0] >= width / 2 and size[1] <= height / 4)
                 for level in ('1', '2', '3') for layer in levels[level]
                 if (size := geometry(layer)))
    terminal = any(client.get('mapped') is True and type(client.get('pid')) is int and
                   client['pid'] == terminal_pid and client.get('class') == 'foot'
                   for client in clients)
    # Input-free startup expects shell layers, not shell toplevels. A crash
    # reporter used exactly this class with the uninformative title "quickshell".
    # Other workloads opening legitimate shell dialogs need a separate policy.
    unexpected = sum(client.get('mapped') is True and client.get('class') == 'org.quickshell'
                     for client in clients)
    return dict(prerequisitesMet=bool(terminal and shell_ok and backgrounds and panels and not unexpected),
                mappedFoot=terminal, shellPing=shell_ok, backgroundLayerCandidates=backgrounds,
                panelLayerCandidates=panels, unexpectedQuickshellTopLevels=unexpected,
                visualInspectionRequired=True, healthyDesktopAccepted=False,
                hostDriverStabilityAccepted=False, dailyUseAccepted=False)
