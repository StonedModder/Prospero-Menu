// SPDX-License-Identifier: GPL-3.0-or-later
// Only kernel-resident startup work may expire with a verified console boot.
// This must never be applied to package installs, saves or filesystem jobs.
export function expiredStartup(native, job, receipt) {
  const previousBoot = receipt?.boot || job.args?.boot;
  if (typeof previousBoot !== 'string' || !previousBoot || previousBoot.length > 80) return null;
  const current = native.systemProbe();
  if (current.busy || typeof current.boot !== 'string' || !current.boot || current.boot === previousBoot) return null;
  return { expired: true, request: job.id, previousBoot, currentBoot: current.boot };
}
export function sameExpiry(a, b) {
  return a?.expired === true && b?.expired === true && a.request === b.request &&
    a.previousBoot === b.previousBoot && a.currentBoot === b.currentBoot;
}
