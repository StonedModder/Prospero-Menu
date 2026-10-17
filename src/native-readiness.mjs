// SPDX-License-Identifier: GPL-3.0-or-later
import { invariant } from './errors.mjs';

// Read-only contention is not a failed dispatch. Give brief native workers a
// chance to finish, then validate a fresh census; never substitute cached data.
export async function waitForNativeProbe(native, { delay, check = () => {}, attempts = 20 } = {}) {
  for (let attempt = 0; ; attempt++) {
    check();
    const probe = native.systemProbe();
    if (!probe.busy) return probe;
    invariant(attempt < attempts, 'NATIVE_BUSY', 'Another console operation is still running. Wait for it to finish, then try again.');
    await delay(50);
  }
}
