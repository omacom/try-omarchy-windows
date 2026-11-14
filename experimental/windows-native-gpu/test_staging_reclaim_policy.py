"""Synthetic ledger controls only; actual GPU acceptance uses the complete auditor."""
import copy
import unittest
from verify_reclaimed_staging_input_response import reclaimed_staging_admission


def trace():
    rows = []
    size, limit = 33423360, 1073741824
    def check(stage, additional=0):
        rows.append(dict(nativePresentationMemoryGuard=True, stage=stage, usageBytes=200000000,
                         additionalBytes=additional, limitBytes=limit, accepted=True))
    def allocate(slot, generation, count, live, phase):
        check('staging-before', size)
        check('staging-after')
        rows.append(dict(nativeCompositorStagingAllocated=True, slot=slot, generation=generation,
                         bytes=size, capacity=3, onDemand=True, preparedTextures=count,
                         preparedBytes=count * size, liveTextureBytes=live * size, phase=phase))
    check('device-before')
    check('device-after')
    allocate(0, 1, 1, 1, 'initialization')
    check('readback-before', 4194304)
    check('readback-after')
    check('swapchain-before', 2 * size)
    check('swapchain-after')
    rows.append(dict(nativeCompositorRingOpened=True, slots=3, sourceCacheLimit=3, onDemandStaging=True,
                     initialStagingTextures=1, ordinaryCpuReadback=False))
    for frame in range(1, 13):
        check('frame-before')
        if frame == 2:
            allocate(1, 1, 2, 2, 'serialized-submit')
        if frame == 3:
            allocate(1, 2, 3, 2, 'serialized-submit')
        if frame == 1:
            check('import-before')
            check('import-after')
        check('frame-admitted')
        rows.append(dict(nativeCompositorRingInput=True, frame=frame, slot=0 if frame == 1 else 1))
        if frame == 2:
            rows.append(dict(nativeCompositorStagingTrimmed=True, slot=1, generation=1, bytes=size,
                             reusedAfter=2, inputCopyFenceObserved=2, outputCopyFenceObserved=2,
                             slotOccupied=False, capacity=3, liveTextureBytes=size,
                             serializedResidency=True, commandsReset=True))
            rows.append(dict(nativeResidencyStagingPressure=True, usageBefore=limit - 33554432,
                             usageAfter=limit - 33554432 - size, targetBytes=33554432, releasedBytes=size,
                             reserveBytes=67108864, limitBytes=limit, accepted=True, beforeNativeResidency=True))
    rows.append(dict(nativeCompositorRingStopped=True, stagingTexturesCreated=3, stagingTexturesReleased=3,
                     stagingTextureBytes=3 * size, stagingTexturesTrimmed=1, liveStagingTextureBytes=0, liveGpuResources=0))
    return rows


class StagingReclaimLedger(unittest.TestCase):
    def test_reclaim_and_reallocation_keep_capacity_and_accounting(self):
        result = reclaimed_staging_admission(trace(), 0)
        self.assertEqual((result['capacity'], result['createdTextures'], result['trimmedTextures'], result['reallocatedTextures']), (3, 3, 1, 1))
        self.assertTrue(result['safeReclamationObserved'])
        self.assertFalse(result['physicalGpuQuotaVerified'])

    def test_busy_pending_device_lost_and_wrong_owner_rejected(self):
        for key, value in (('slot', 0), ('generation', 2), ('generation', True), ('slotOccupied', True),
                           ('inputCopyFenceObserved', 1), ('outputCopyFenceObserved', 1),
                           ('inputCopyFenceObserved', 2**64-1), ('commandsReset', False), ('serializedResidency', False)):
            rows = trace()
            next(row for row in rows if 'nativeCompositorStagingTrimmed' in row)[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                reclaimed_staging_admission(rows, 0)

    def test_pressure_cannot_bypass_budget_or_lose_release(self):
        for key, value in (('usageBefore', 1073741825), ('usageAfter', 1073741825), ('targetBytes', 1),
                           ('releasedBytes', 0), ('reserveBytes', 1), ('limitBytes', 2147483648),
                           ('beforeNativeResidency', False), ('accepted', False)):
            rows = trace()
            next(row for row in rows if 'nativeResidencyStagingPressure' in row)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                reclaimed_staging_admission(rows, 0)

    def test_stale_reallocation_and_unprepared_inputs_rejected(self):
        rows = trace()
        [row for row in rows if 'nativeCompositorStagingAllocated' in row][-1]['generation'] = 1
        with self.assertRaises(ValueError):
            reclaimed_staging_admission(rows, 0)
        rows = trace()
        next(row for row in rows if row.get('nativeCompositorRingInput') and row['frame'] == 3)['slot'] = 2
        with self.assertRaises(ValueError):
            reclaimed_staging_admission(rows, 0)

    def test_missing_admission_and_post_teardown_work_rejected(self):
        rows = trace()
        del rows[next(i for i, row in enumerate(rows) if row.get('stage') == 'staging-after')]
        with self.assertRaises(ValueError):
            reclaimed_staging_admission(rows, 0)
        rows = trace()
        rows.append(copy.deepcopy(next(row for row in rows if 'nativeResidencyStagingPressure' in row)))
        with self.assertRaises(ValueError):
            reclaimed_staging_admission(rows, 0)

    def test_incomplete_release_and_changed_byte_ledger_rejected(self):
        for key, value in (('stagingTexturesReleased', 2), ('stagingTextureBytes', 1),
                           ('stagingTexturesTrimmed', 0), ('liveStagingTextureBytes', 33423360)):
            rows = trace()
            rows[-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                reclaimed_staging_admission(rows, 0)


if __name__ == '__main__':
    unittest.main()
