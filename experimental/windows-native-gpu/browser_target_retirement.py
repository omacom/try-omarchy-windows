# SPDX-License-Identifier: MIT
"""Bounded CDP tab retirement; does not certify Windows input or GPU performance.

Protocol: https://github.com/ChromeDevTools/devtools-protocol/blob/master/pdl/domains/Target.pdl
"""
import math
import time


def close_owned_page(call, original, temporary, baseline, *, timeout_seconds=2.0,
                     clock=time.monotonic, sleep=time.sleep):
    baseline=set(baseline)
    if (not isinstance(original,str) or not original or not isinstance(temporary,str) or not temporary or
            original not in baseline or temporary in baseline or
            any(not isinstance(value,str) or not value for value in baseline)):
        raise ValueError('Invalid owned temporary-page identity')
    if (type(timeout_seconds) not in (int,float) or not math.isfinite(timeout_seconds) or
            not 0<timeout_seconds<=2.0):
        raise ValueError('Tab retirement deadline must be at most two seconds')
    started=clock();deadline=started+timeout_seconds

    def remaining():
        seconds=deadline-clock()
        if not math.isfinite(seconds) or seconds<=0:
            raise TimeoutError('The owned temporary tab did not retire before its deadline')
        return min(seconds,timeout_seconds)

    result=call('Target.closeTarget',dict(targetId=temporary),seconds=remaining())
    remaining()
    if not isinstance(result,dict) or result.get('success') is not True:
        raise RuntimeError('The owned temporary tab close was not acknowledged')
    call('Target.activateTarget',dict(targetId=original),seconds=remaining())
    remaining()
    for query in range(1,82):
        result=call('Target.getTargets',seconds=remaining())
        remaining()
        targets=result.get('targetInfos') if isinstance(result,dict) else None
        if not isinstance(targets,list) or len(targets)>256:raise RuntimeError('Malformed or oversized browser target list')
        pages=set()
        for target in targets:
            if not isinstance(target,dict) or not isinstance(target.get('type'),str):
                raise RuntimeError('Malformed browser target identity')
            if target['type']!='page':continue
            identity=target.get('targetId')
            if not isinstance(identity,str) or not identity or identity in pages:
                raise RuntimeError('Malformed or duplicate browser page target')
            pages.add(identity)
        remaining()
        if pages==baseline:
            return dict(queries=query,elapsedSeconds=clock()-started,deadlineSeconds=timeout_seconds,
                        exactBaselineRestored=True,ownedTemporaryAbsent=True,pageTargetCount=len(pages))
        # Only this owned tab may be pending. Do not hide a lost original page,
        # another closing baseline page, a popup, or a replaced target identity.
        if pages!=baseline|{temporary}:
            raise RuntimeError('Browser page targets changed beyond the owned temporary tab: '+
                               repr(dict(missing=sorted(baseline-pages),unexpected=sorted(pages-baseline-{temporary}))))
        sleep(min(.025,remaining()))
    raise TimeoutError('The owned temporary tab exceeded the bounded query count')
