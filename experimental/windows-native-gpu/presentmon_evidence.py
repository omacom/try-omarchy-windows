"""Offline ETW corroboration, separate from the strict DXGI statistics result.

Accept only a uniquely matching PID, swapchain and actual Present-call QPC
interval. A successful Present call or GPU fence is not display evidence.
PresentMon v1 metrics omit the first row when no previous Present is available;
missing rows remain unverified. No frame rate is inferred from diagnostic gaps.
"""
import csv
import io
import math


def correlate_presentmon(text, receipts):
    if not isinstance(text, str) or not isinstance(receipts, list) or len(receipts) != 2:
        raise ValueError('Require exactly two Present receipts and a CSV string')
    required = ('Application', 'ProcessID', 'SwapChainAddress', 'Runtime', 'Dropped',
                'PresentMode', 'msUntilDisplayed', 'QPCTime')
    reader = csv.DictReader(io.StringIO(text))
    if (not reader.fieldnames or len(set(reader.fieldnames)) != len(reader.fieldnames) or
            not set(required).issubset(reader.fieldnames)):
        raise ValueError('Missing or duplicate ETW columns')
    previous = 0
    for i, r in enumerate(receipts, 1):
        for key in ('round','presentProcessId','presentSwapChain','presentCallQpcBefore',
                    'presentCallQpcAfter','qpcFrequency','presentHresult','lastPresentCount',
                    'presentQueueFenceTarget','presentQueueFenceObserved'):
            if type(r.get(key)) is not int:
                raise ValueError('Invalid Present receipt types')
        if (r['round'] != i or r['presentHresult'] != 0 or r['lastPresentCount'] != i or
                r['presentQueueFenceTarget'] != i or not i <= r['presentQueueFenceObserved'] < (1 << 64) - 1 or
                r.get('windowVisible') is not True or r.get('cpuUpload') is not False or
                r.get('backBufferState') != 'PRESENT' or not 0 < r['presentProcessId'] < 1 << 32 or
                not 0 < r['presentSwapChain'] < 1 << 64 or not 0 < r['qpcFrequency'] < 1 << 63 or
                not previous < r['presentCallQpcBefore'] <= r['presentCallQpcAfter'] < 1 << 63):
            raise ValueError('Present did not complete, or QPC intervals overlap')
        previous = r['presentCallQpcAfter']
    if any(receipts[0][k] != receipts[1][k] for k in ('presentProcessId','presentSwapChain','qpcFrequency')):
        raise ValueError('Producer identity changed between frames')
    matched = {}
    for row in reader:
        if None in row or any(row.get(k) is None for k in required):
            raise ValueError('Malformed ETW row')
        try:
            pid = int(row['ProcessID'])
            chain = int(row['SwapChainAddress'], 16)
            qpc = int(row['QPCTime'])
            displayed_ms = float(row['msUntilDisplayed'])
        except (ValueError, TypeError) as error:
            raise ValueError('Invalid ETW values') from error
        candidates = [r for r in receipts if r['presentProcessId'] == pid and r['presentSwapChain'] == chain and
                      r['presentCallQpcBefore'] <= qpc <= r['presentCallQpcAfter']]
        if len(candidates) != 1 or candidates[0]['round'] in matched:
            raise ValueError('Unmatched or duplicate ETW Present')
        if (row['Runtime'] != 'DXGI' or row['Dropped'] not in ('0','1') or
                not math.isfinite(displayed_ms) or displayed_ms < 0):
            raise ValueError('Invalid ETW display state')
        r = candidates[0]
        displayed = (row['Dropped'] == '0' and displayed_ms > 0 and row['PresentMode'] in
                     ('Composed: Flip', 'Hardware: Independent Flip', 'Hardware Composed: Independent Flip'))
        matched[r['round']] = dict(round=r['round'], etwDisplayed=displayed, presentQpc=qpc,
                                   msUntilDisplayed=displayed_ms, presentMode=row['PresentMode'])
    frames = [matched.get(i, dict(round=i, etwDisplayed=False, reason='missing-etw-row')) for i in (1,2)]
    return dict(etwFramesDisplayedVerified=all(r['etwDisplayed'] for r in frames),
                matchedRows=len(matched), frames=frames, nativePerformanceMeasured=False)
