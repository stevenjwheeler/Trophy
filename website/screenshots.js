// Progressive enhancement: original PNG links still work without JavaScript.
(() => {
  const dialog = document.getElementById('screenshot-inspector');
  if (!dialog || typeof dialog.showModal !== 'function') return;

  const image = document.getElementById('screenshot-full');
  const status = document.getElementById('screenshot-status');
  let opener = null;
  let pointerStartedOutside = false;

  document.querySelectorAll('a[data-screenshot]').forEach(link => {
    link.setAttribute('aria-haspopup', 'dialog');
    const hint = document.createElement('span');
    hint.className = 'screenshot-hint';
    hint.setAttribute('aria-hidden', 'true');
    hint.textContent = 'Enlarge ⤢';
    link.append(hint);
    link.addEventListener('click', event => {
      if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      const thumbnail = link.querySelector('img');
      if (!thumbnail) return;
      event.preventDefault();
      opener = link;
      dialog.setAttribute('aria-label', link.dataset.screenshot);
      image.alt = thumbnail.alt;
      image.hidden = false;
      status.hidden = true;
      image.src = link.href;
      dialog.showModal();
      document.body.classList.add('screenshot-open');
    });
  });

  image.addEventListener('error', () => {
    if (!dialog.open) return;
    image.hidden = true;
    status.textContent = 'The screenshot could not be loaded. Press Escape or click outside to close.';
    status.hidden = false;
  });
  function outside(event) {
    const box = dialog.getBoundingClientRect();
    return event.clientX < box.left || event.clientX > box.right || event.clientY < box.top || event.clientY > box.bottom;
  }
  dialog.addEventListener('pointerdown', event => { pointerStartedOutside = outside(event); });
  dialog.addEventListener('click', event => {
    if (event.target === dialog && pointerStartedOutside && outside(event)) dialog.close();
    pointerStartedOutside = false;
  });
  dialog.addEventListener('close', () => {
    // A new screenshot may already be open when a queued close event arrives.
    if (dialog.open) return;
    document.body.classList.remove('screenshot-open');
    image.removeAttribute('src');
    image.alt = '';
    if (opener?.isConnected) opener.focus({ preventScroll: true });
    opener = null;
  });
})();
