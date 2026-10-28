// Shuffle bags avoid repeats within a round and at round boundaries.
(() => {
  const headline = document.querySelector('.hero-words');
  if (!headline) return;

  const words = [...headline.querySelectorAll('.hero-word')];
  if (words.length < 2) return;

  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
  let remaining = [];
  let previous = null;
  let timer = null;

  function nextWord() {
    if (remaining.length === 0) {
      remaining = [...words];
      for (let i = remaining.length - 1; i > 0; i--) {
        const j = Math.floor(Math.random() * (i + 1));
        [remaining[i], remaining[j]] = [remaining[j], remaining[i]];
      }
      const last = remaining.length - 1;
      if (remaining[last] === previous) {
        [remaining[0], remaining[last]] = [remaining[last], remaining[0]];
      }
    }

    const current = remaining.pop();
    words.forEach(word => word.classList.toggle('is-current', word === current));
    previous = current;
  }

  function updateMotion() {
    window.clearInterval(timer);
    timer = null;
    headline.classList.remove('is-randomized');
    words.forEach(word => word.classList.remove('is-current'));
    if (reducedMotion.matches) return;

    headline.classList.add('is-randomized');
    nextWord();
    if (!document.hidden) timer = window.setInterval(nextWord, 2000);
  }

  reducedMotion.addEventListener('change', updateMotion);
  document.addEventListener('visibilitychange', updateMotion);
  updateMotion();
})();
