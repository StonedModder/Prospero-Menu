// SPDX-License-Identifier: GPL-3.0-or-later
// Keep rendering while a deduplicated action validates files or starts services.
// 202 carries a fresh frame but leaves the native action pending; 200 acknowledges
// its completion (including a rejection displayed in the frame's notice).
export async function syncDeck(deck, input) {
  if (!input) return { status: 200, frame: deck.frame() };
  let settled = false, timer;
  const result = deck.action(input).then(() => { settled = true; }, () => { settled = true; });
  try {
    await Promise.race([result, new Promise(resolve => { timer = setTimeout(resolve, 25); })]);
    return { status: settled ? 200 : 202, frame: deck.frame() };
  } finally { clearTimeout(timer); }
}
