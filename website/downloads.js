// Keep normal download navigation; the dialog is only an accompanying guide.
(() => {
  const dialog = document.getElementById('appimage-instructions');
  if (!dialog || typeof dialog.showModal !== 'function') return;

  const filenameLabel = document.getElementById('appimage-filename');
  const commands = document.getElementById('appimage-commands');
  const retry = document.getElementById('appimage-retry');
  let opener = null;
  let pointerStartedOutside = false;

  const shellQuote = value => "'" + value.replaceAll("'", "'\\''") + "'";

  document.querySelectorAll('a[data-release-asset="appimage"]').forEach(link => {
    link.setAttribute('aria-haspopup', 'dialog');
    link.addEventListener('click', event => {
      if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      const url = new URL(link.href);
      let filename;
      try {
        filename = decodeURIComponent(url.pathname.split('/').pop());
      } catch {
        return;
      }
      // Refuse unexpected path/control characters in terminal instructions.
      if (!filename.endsWith('.AppImage') || /[\x00-\x1f\x7f/\\]/.test(filename)) return;
      opener = link;
      filenameLabel.textContent = filename;
      commands.textContent = `chmod +x -- ${shellQuote(filename)}\n./${shellQuote(filename)}`;
      retry.href = link.href;
      if (!dialog.open) dialog.showModal();
      document.body.classList.add('appimage-open');
      // Do not preventDefault: the browser still requests the actual asset.
      // The site cannot observe download completion or promise it succeeded.
    });
  });

  dialog.querySelector('.appimage-close').addEventListener('click', () => dialog.close());
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
    if (dialog.open) return;
    document.body.classList.remove('appimage-open');
    if (opener?.isConnected) opener.focus({ preventScroll: true });
    opener = null;
  });
})();
