(() => {
  const slides = [...document.querySelectorAll('.slide')];
  const progress = document.querySelector('.progress');
  const currentEl = document.querySelector('[data-current]');
  const totalEl = document.querySelector('[data-total]');
  let current = 0;
  let touchStartX = 0;

  const clamp = (value) => Math.max(0, Math.min(slides.length - 1, value));

  function show(index, updateHash = true) {
    current = clamp(index);
    slides.forEach((slide, i) => {
      const active = i === current;
      slide.classList.toggle('active', active);
      slide.setAttribute('aria-hidden', String(!active));
    });
    currentEl.textContent = String(current + 1).padStart(2, '0');
    totalEl.textContent = String(slides.length).padStart(2, '0');
    progress.style.width = `${((current + 1) / slides.length) * 100}%`;
    if (updateHash) history.replaceState(null, '', `#${current + 1}`);
    document.title = `${slides[current].dataset.title || '体素采集'} · ${current + 1}/${slides.length}`;
  }

  function next() { show(current + 1); }
  function previous() { show(current - 1); }

  document.querySelector('[data-next]').addEventListener('click', next);
  document.querySelector('[data-prev]').addEventListener('click', previous);
  document.querySelector('[data-fullscreen]').addEventListener('click', async () => {
    if (!document.fullscreenElement) await document.documentElement.requestFullscreen?.();
    else await document.exitFullscreen?.();
  });

  document.addEventListener('keydown', (event) => {
    if (['ArrowRight', 'PageDown', 'Enter', ' '].includes(event.key)) {
      event.preventDefault();
      next();
    }
    if (['ArrowLeft', 'PageUp', 'Backspace'].includes(event.key)) {
      event.preventDefault();
      previous();
    }
    if (event.key === 'Home') show(0);
    if (event.key === 'End') show(slides.length - 1);
    if (event.key.toLowerCase() === 'f') document.querySelector('[data-fullscreen]').click();
    if (event.key.toLowerCase() === 'p') window.print();
  });

  document.addEventListener('touchstart', (event) => {
    touchStartX = event.changedTouches[0].screenX;
  }, { passive: true });
  document.addEventListener('touchend', (event) => {
    const delta = event.changedTouches[0].screenX - touchStartX;
    if (Math.abs(delta) > 55) delta < 0 ? next() : previous();
  }, { passive: true });

  window.addEventListener('hashchange', () => {
    const index = Number(location.hash.slice(1)) - 1;
    if (Number.isFinite(index)) show(index, false);
  });

  document.documentElement.classList.add('deck-initializing');
  const initial = Number(location.hash.slice(1)) - 1;
  show(Number.isFinite(initial) && initial >= 0 ? initial : 0, false);
  requestAnimationFrame(() => {
    requestAnimationFrame(() => document.documentElement.classList.remove('deck-initializing'));
  });
})();
