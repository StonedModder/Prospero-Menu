// SPDX-License-Identifier: GPL-3.0-or-later
export function startupNotification(result, menuReady, ip) {
  if (result?.completed && menuReady) return 'ProsperoMenu loaded\nHold L3 + R3 to open the menu';
  const detail = result?.error?.message || 'The menu has not confirmed it is ready.';
  if(menuReady)return `ProsperoMenu menu ready\nHold L3 + R3 to open the menu\n${detail}\nSee Activity for the affected feature.`;
  return `ProsperoMenu menu did not start\n${detail}\nOpen http://${ip}:8780 → Activity → Review startup`;
}
